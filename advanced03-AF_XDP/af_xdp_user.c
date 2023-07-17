/*
This is a general AF_XDP userspace program that can make use of any number of sockets,
corresponding to any number of NIC rx queues, as well as any number of threads to poll
on these sockets.  Completely reusable code for any use case.  Simply update the handle_packet()
function accordingly
*/


#include <assert.h>
#include <errno.h>
#include <getopt.h>
#include <locale.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/resource.h>

#include <bpf/bpf.h>
#include <xdp/xsk.h>
#include <xdp/libxdp.h>

#include <arpa/inet.h>
#include <net/if.h>
#include <linux/if_link.h>
#include <linux/if_ether.h>
#include <linux/ip.h>
#include <linux/icmpv6.h>
#include <linux/udp.h>

#include "../common/common_params.h"
#include "../common/common_user_bpf_xdp.h"
#include "../common/common_libbpf.h"

#define NUM_FRAMES         4096
#define FRAME_SIZE         XSK_UMEM__DEFAULT_FRAME_SIZE
#define RX_BATCH_SIZE      64
#define TX_BATCH_SIZE	   5
#define INVALID_UMEM_FRAME UINT64_MAX
#define NUM_SOCKETS		   1
#define NUM_THREADS		   1
#define TIMEOUT_NSEC	   500000000

#define MAX_PACKET_LEN	XSK_UMEM__DEFAULT_FRAME_SIZE
#define SRC_MAC	"9c:dc:71:5d:41:f1"
#define DST_MAC	"9c:dc:71:5d:01:81"
#define SRC_IP	"192.168.6.1"
#define DST_IP	"192.168.6.2"
#define SRC_PORT	8889
#define DST_PORT	8889

size_t num_packets = 0;
size_t num_ready = 0;
size_t num_tx_packets = 0;
struct timespec timeout_start = {0, 0};

static struct xdp_program *prog;
int xsk_map_fd;
bool custom_xsk = false;
struct config cfg = {
	.ifindex   = -1,
};

struct threadArgs {
	struct xsk_socket_info** xskis;
	int* batch_ar;
};

struct xsk_umem_info {
	struct xsk_ring_prod fq;
	struct xsk_ring_cons cq;
	struct xsk_umem *umem;
	void *buffer;
};
struct stats_record {
	uint64_t timestamp;
	uint64_t rx_packets;
	uint64_t rx_bytes;
	uint64_t tx_packets;
	uint64_t tx_bytes;
};
struct xsk_socket_info {
	struct xsk_ring_cons rx;
	struct xsk_ring_prod tx;
	struct xsk_umem_info *umem;
	struct xsk_socket *xsk;

	uint64_t umem_frame_addr[NUM_FRAMES];
	uint32_t umem_frame_free;

	uint32_t outstanding_tx;

	struct stats_record stats;
	struct stats_record prev_stats;
};

static inline __u32 xsk_ring_prod__free(struct xsk_ring_prod *r)
{
	r->cached_cons = *r->consumer + r->size;
	return r->cached_cons - r->cached_prod;
}

static const char *__doc__ = "AF_XDP kernel bypass example\n";

static const struct option_wrapper long_options[] = {

	{{"help",	 no_argument,		NULL, 'h' },
	 "Show help", false},

	{{"dev",	 required_argument,	NULL, 'd' },
	 "Operate on device <ifname>", "<ifname>", true},

	{{"skb-mode",	 no_argument,		NULL, 'S' },
	 "Install XDP program in SKB (AKA generic) mode"},

	{{"native-mode", no_argument,		NULL, 'N' },
	 "Install XDP program in native mode"},

	{{"auto-mode",	 no_argument,		NULL, 'A' },
	 "Auto-detect SKB or native mode"},

	{{"force",	 no_argument,		NULL, 'F' },
	 "Force install, replacing existing program on interface"},

	{{"copy",        no_argument,		NULL, 'c' },
	 "Force copy mode"},

	{{"zero-copy",	 no_argument,		NULL, 'z' },
	 "Force zero-copy mode"},

	{{"queue",	 required_argument,	NULL, 'Q' },
	 "Configure interface receive queue for AF_XDP, default=0"},

	{{"poll-mode",	 no_argument,		NULL, 'p' },
	 "Use the poll() API waiting for packets to arrive"},

	{{"quiet",	 no_argument,		NULL, 'q' },
	 "Quiet mode (no output)"},

	{{"filename",    required_argument,	NULL,  1  },
	 "Load program from <file>", "<file>"},

	{{"progname",	 required_argument,	NULL,  2  },
	 "Load program from function <name> in the ELF file", "<name>"},

	{{0, 0, NULL,  0 }, NULL, false}
};

static bool global_exit;

static struct xsk_umem_info *configure_xsk_umem(void *buffer, uint64_t size)
{
	struct xsk_umem_info *umem;
	int ret;

	umem = calloc(1, sizeof(*umem));
	if (!umem)
		return NULL;

	ret = xsk_umem__create(&umem->umem, buffer, size, &umem->fq, &umem->cq,
			       NULL);
	if (ret) {
		errno = -ret;
		return NULL;
	}

	umem->buffer = buffer;
	return umem;
}

static uint64_t xsk_alloc_umem_frame(struct xsk_socket_info *xsk)
{
	uint64_t frame;
	if (xsk->umem_frame_free == 0)
		return INVALID_UMEM_FRAME;

	frame = xsk->umem_frame_addr[--xsk->umem_frame_free];
	xsk->umem_frame_addr[xsk->umem_frame_free] = INVALID_UMEM_FRAME;
	return frame;
}

static void xsk_free_umem_frame(struct xsk_socket_info *xsk, uint64_t frame)
{
	assert(xsk->umem_frame_free < NUM_FRAMES);

	xsk->umem_frame_addr[xsk->umem_frame_free++] = frame;
}

static uint64_t xsk_umem_free_frames(struct xsk_socket_info *xsk)
{
	return xsk->umem_frame_free;
}

static struct xsk_socket_info *xsk_configure_socket(struct config *cfg,
						    struct xsk_umem_info *umem, int queue)
{
	struct xsk_socket_config xsk_cfg;
	struct xsk_socket_info *xsk_info;
	uint32_t idx;
	int i;
	int ret;
	uint32_t prog_id;

	xsk_info = calloc(1, sizeof(*xsk_info));
	if (!xsk_info)
		return NULL;

	xsk_info->umem = umem;
	xsk_cfg.rx_size = XSK_RING_CONS__DEFAULT_NUM_DESCS;
	xsk_cfg.tx_size = XSK_RING_PROD__DEFAULT_NUM_DESCS;
	xsk_cfg.xdp_flags = cfg->xdp_flags;
	xsk_cfg.bind_flags = cfg->xsk_bind_flags;
	xsk_cfg.libbpf_flags = (custom_xsk) ? XSK_LIBBPF_FLAGS__INHIBIT_PROG_LOAD: 0;
	ret = xsk_socket__create_shared(&xsk_info->xsk, cfg->ifname,
				 queue, umem->umem, &xsk_info->rx,
				 &xsk_info->tx, &umem->fq, &umem->cq, &xsk_cfg);
	if (ret)
		goto error_exit;

	if (custom_xsk) {
		ret = xsk_socket__update_xskmap(xsk_info->xsk, xsk_map_fd);
		if (ret)
			goto error_exit;
	} else {
		/* Getting the program ID must be after the xdp_socket__create() call */
		if (bpf_xdp_query_id(cfg->ifindex, cfg->xdp_flags, &prog_id))
			goto error_exit;
	}

	/* Initialize umem frame allocation */
	for (i = 0; i < NUM_FRAMES; i++)
		xsk_info->umem_frame_addr[i] = i * FRAME_SIZE;

	xsk_info->umem_frame_free = NUM_FRAMES;

	/* Stuff the receive path with buffers, we assume we have enough */
	ret = xsk_ring_prod__reserve(&xsk_info->umem->fq,
					XSK_RING_PROD__DEFAULT_NUM_DESCS,
					&idx);

	if (ret != XSK_RING_PROD__DEFAULT_NUM_DESCS)
		goto error_exit;

	for (i = 0; i < XSK_RING_PROD__DEFAULT_NUM_DESCS; i ++)
		*xsk_ring_prod__fill_addr(&xsk_info->umem->fq, idx++) =
			xsk_alloc_umem_frame(xsk_info);

	xsk_ring_prod__submit(&xsk_info->umem->fq,
			    XSK_RING_PROD__DEFAULT_NUM_DESCS);

	return xsk_info;

error_exit:
	errno = -ret;
	return NULL;
}

static void complete_tx(struct xsk_socket_info *xsk)
{
	unsigned int completed;
	uint32_t idx_cq;

	if (!xsk->outstanding_tx) {
		//printf("No outstanding\n");
		return;
	}

	sendto(xsk_socket__fd(xsk->xsk), NULL, 0, MSG_DONTWAIT, NULL, 0);

	/* Collect/free completed TX buffers */
	completed = xsk_ring_cons__peek(&xsk->umem->cq,
					XSK_RING_CONS__DEFAULT_NUM_DESCS,
					&idx_cq);

	if (completed > 0) {
		for (int i = 0; i < completed; i++)
			xsk_free_umem_frame(xsk,
					    *xsk_ring_cons__comp_addr(&xsk->umem->cq,
								      idx_cq++));

		xsk_ring_cons__release(&xsk->umem->cq, completed);
		xsk->outstanding_tx -= completed < xsk->outstanding_tx ?
			completed : xsk->outstanding_tx;
	}
	else {
		//printf("No completed\n");
	}
}

static inline __sum16 csum16_add(__sum16 csum, __be16 addend)
{
	uint16_t res = (uint16_t)csum;

	res += (__u16)addend;
	return (__sum16)(res + (res < (__u16)addend));
}

static inline __sum16 csum16_sub(__sum16 csum, __be16 addend)
{
	return csum16_add(csum, ~addend);
}

static inline void csum_replace2(__sum16 *sum, __be16 old, __be16 new)
{
	*sum = ~csum16_add(csum16_sub(~(*sum), old), new);
}

static bool process_packet(struct xsk_socket_info *xsk,
			   uint64_t addr, uint32_t len, int* nbatched)
{
	uint8_t *pkt = xsk_umem__get_data(xsk->umem->buffer, addr);

	++num_packets;

	int ret;
	uint32_t tx_idx = 0;
	uint8_t tmp_mac[ETH_ALEN];
	struct in_addr tmp_ip;
	struct ethhdr *eth = (struct ethhdr *) pkt;
	struct iphdr *iph = (struct iphdr *) (eth + 1);
	struct udphdr *udph = NULL;
		
	if (ntohs(eth->h_proto) != ETH_P_IP)
			return false;

	// Swap source and destination MAC
	memcpy(tmp_mac, eth->h_dest, ETH_ALEN);
	memcpy(eth->h_dest, eth->h_source, ETH_ALEN);
	memcpy(eth->h_source, tmp_mac, ETH_ALEN);

	// Swap source and destination IP
	memcpy(&tmp_ip, &iph->saddr, sizeof(tmp_ip));
	memcpy(&iph->saddr, &iph->daddr, sizeof(tmp_ip));
	memcpy(&iph->daddr, &tmp_ip, sizeof(tmp_ip));

	// Swap source and destination port
	unsigned char* ip_data = (unsigned char*)iph + (iph->ihl * 4);
	udph = (struct udphdr*)ip_data;
	//printf("src: %d\n", udph->source);
	//printf("dst: %d\n", udph->dest);
	uint16_t tmp = udph->source;
	udph->source = udph->dest;
	udph->dest = tmp;
		

	/* Here we sent the packet out of the receive port. Note that
	 * we allocate one entry and schedule it. Your design would be
	 * faster if you do batch processing/transmission */

	ret = xsk_ring_prod__reserve(&xsk->tx, 1, &tx_idx);
	if (ret != 1) {
		printf("no more transmit slots\n");
		/* No more transmit slots, drop the packet */
		return false;
	}

	xsk_ring_prod__tx_desc(&xsk->tx, tx_idx)->addr = addr;
	xsk_ring_prod__tx_desc(&xsk->tx, tx_idx)->len = len;

	(*nbatched) += 1;
	if (*nbatched >= TX_BATCH_SIZE) {
		xsk_ring_prod__submit(&xsk->tx, (*nbatched));
		xsk->outstanding_tx += (*nbatched);
		(*nbatched) = 0;
	}

	xsk->stats.tx_bytes += len;
	xsk->stats.tx_packets++;
	return true;
}

static void handle_receive_packets(struct xsk_socket_info *xsk, int* nbatched)
{
	unsigned int rcvd, stock_frames, i;
	uint32_t idx_rx = 0, idx_fq = 0;
	int ret;

	// Check if there is something to consume at all
	
	rcvd = xsk_ring_cons__peek(&xsk->rx, RX_BATCH_SIZE, &idx_rx);
	if (!rcvd)
		return;

	++num_ready;
	/* Stuff the ring with as much frames as possible */
	stock_frames = xsk_prod_nb_free(&xsk->umem->fq,
					xsk_umem_free_frames(xsk));

	if (stock_frames > 0) {

		ret = xsk_ring_prod__reserve(&xsk->umem->fq, stock_frames,
					     &idx_fq);
		/* This should not happen, but just in case */
		while (ret != stock_frames)
			ret = xsk_ring_prod__reserve(&xsk->umem->fq, rcvd,
						     &idx_fq);

		for (i = 0; i < stock_frames; i++)
			*xsk_ring_prod__fill_addr(&xsk->umem->fq, idx_fq++) =
				xsk_alloc_umem_frame(xsk);

		xsk_ring_prod__submit(&xsk->umem->fq, stock_frames);
	}

	/* Process received packets */
	for (i = 0; i < rcvd; i++) {
		uint64_t addr = xsk_ring_cons__rx_desc(&xsk->rx, idx_rx)->addr;
		uint32_t len = xsk_ring_cons__rx_desc(&xsk->rx, idx_rx++)->len;

		if (!process_packet(xsk, addr, len, nbatched)) {
			printf("Couldn't send!\n");
			xsk_free_umem_frame(xsk, addr);
		}
		xsk->stats.rx_bytes += len;
	}

	xsk_ring_cons__release(&xsk->rx, rcvd);
	xsk->stats.rx_packets += rcvd;

	/* Do we need to wake up the kernel for transmission */
	complete_tx(xsk);

	// Reset timeout start
	clock_gettime(CLOCK_MONOTONIC, &timeout_start);
  }

//static void rx_and_process(struct config* cfg,
//						   struct xsk_socket_info **xsk_sockets)

static void rx_and_process(void* args)
{
	struct threadArgs* th_args = (struct threadArgs*)args;
	struct xsk_socket_info **xsk_sockets = th_args->xskis;
	int* batch_ar = th_args->batch_ar;

	struct timespec timeout_end;
	struct timespec timeout_elapsed;

	struct pollfd fds[1];
	int ret = 1;
	int nfds = NUM_SOCKETS;

	memset(fds, 0, sizeof(fds));
	for (int sockidx = 0; sockidx < NUM_SOCKETS; ++sockidx) {
		fds[sockidx].fd = xsk_socket__fd(xsk_sockets[sockidx]->xsk);
		fds[sockidx].events = POLLIN;
	}


	while (!global_exit) {
		if (cfg.xsk_poll_mode) {
			ret = poll(fds, nfds, -1);
			handle_receive_packets(xsk_sockets[0], &batch_ar[0]);
		}
		else {
			for (int sockidx = 0; sockidx < NUM_SOCKETS; ++sockidx) {
				handle_receive_packets(xsk_sockets[sockidx], &batch_ar[sockidx]);
			}
		}
		
		// Check timeout
		bool timeout_valid = false;
		for (int i = 0; i < NUM_SOCKETS; ++i) {
			if (batch_ar[i] > 0)  {
				timeout_valid = true;
				break;
			}
		}
		if (timeout_valid) {
			clock_gettime(CLOCK_MONOTONIC, &timeout_end);
			timeout_elapsed.tv_sec = timeout_end.tv_sec - timeout_start.tv_sec;
			if (timeout_end.tv_nsec >= timeout_start.tv_nsec) {
				timeout_elapsed.tv_nsec = timeout_end.tv_nsec - timeout_start.tv_nsec;
			} else {
				timeout_elapsed.tv_sec--;
				timeout_elapsed.tv_nsec = 1000000000 + timeout_end.tv_nsec - timeout_start.tv_nsec;
			}

			if (timeout_elapsed.tv_nsec >= TIMEOUT_NSEC) {
				printf("timeout\n");

				for (int idx = 0; idx < NUM_SOCKETS; ++idx) {
					int num_batched = batch_ar[idx];
					if (num_batched > 0) {
						struct xsk_socket_info* xsk = xsk_sockets[idx];
						xsk_ring_prod__submit(&xsk->tx, num_batched);
						xsk->outstanding_tx += num_batched;
						batch_ar[idx] = 0;
						complete_tx(xsk);
					}
				}
			}
		}
	}	
}

#define NANOSEC_PER_SEC 1000000000 /* 10^9 */
static uint64_t gettime(void)
{
	struct timespec t;
	int res;

	res = clock_gettime(CLOCK_MONOTONIC, &t);
	if (res < 0) {
		fprintf(stderr, "Error with gettimeofday! (%i)\n", res);
		exit(EXIT_FAIL);
	}
	return (uint64_t) t.tv_sec * NANOSEC_PER_SEC + t.tv_nsec;
}

static double calc_period(struct stats_record *r, struct stats_record *p)
{
	double period_ = 0;
	__u64 period = 0;

	period = r->timestamp - p->timestamp;
	if (period > 0)
		period_ = ((double) period / NANOSEC_PER_SEC);

	return period_;
}

static void stats_print(struct stats_record *stats_rec,
			struct stats_record *stats_prev)
{
	uint64_t packets, bytes;
	double period;
	double pps; /* packets per sec */
	double bps; /* bits per sec */

	char *fmt = "%-12s %'11lld pkts (%'10.0f pps)"
		" %'11lld Kbytes (%'6.0f Mbits/s)"
		" period:%f\n";

	period = calc_period(stats_rec, stats_prev);
	if (period == 0)
		period = 1;

	packets = stats_rec->rx_packets - stats_prev->rx_packets;
	pps     = packets / period;

	bytes   = stats_rec->rx_bytes   - stats_prev->rx_bytes;
	bps     = (bytes * 8) / period / 1000000;

	printf(fmt, "AF_XDP RX:", stats_rec->rx_packets, pps,
	       stats_rec->rx_bytes / 1000 , bps,
	       period);

	packets = stats_rec->tx_packets - stats_prev->tx_packets;
	pps     = packets / period;

	bytes   = stats_rec->tx_bytes   - stats_prev->tx_bytes;
	bps     = (bytes * 8) / period / 1000000;

	printf(fmt, "       TX:", stats_rec->tx_packets, pps,
	       stats_rec->tx_bytes / 1000 , bps,
	       period);

	printf("\n");
}

static void *stats_poll(void *arg)
{
	unsigned int interval = 2;
	struct xsk_socket_info *xsk = arg;
	static struct stats_record previous_stats = { 0 };

	previous_stats.timestamp = gettime();

	/* Trick to pretty printf with thousands separators use %' */
	setlocale(LC_NUMERIC, "en_US");

	while (!global_exit) {
		sleep(interval);
		xsk->stats.timestamp = gettime();
		stats_print(&xsk->stats, &previous_stats);
		previous_stats = xsk->stats;
	}
	return NULL;
}

static void exit_application(int signal)
{
	printf("received %d packets\n", num_packets);
	printf("socket ready %d times\n", num_ready);
	int err;

	cfg.unload_all = true;
	err = do_unload(&cfg);
	if (err) {
		fprintf(stderr, "Couldn't detach XDP program on iface '%s' : (%d)\n",
			cfg.ifname, err);
	}

	signal = signal;
	global_exit = true;
}

int main(int argc, char **argv)
{
	int ret;
	void *packet_buffer;
	uint64_t packet_buffer_size;
	DECLARE_LIBBPF_OPTS(bpf_object_open_opts, opts);
	DECLARE_LIBXDP_OPTS(xdp_program_opts, xdp_opts, 0);
	struct rlimit rlim = {RLIM_INFINITY, RLIM_INFINITY};
	struct xsk_umem_info *umems[NUM_SOCKETS];
	struct xsk_socket_info* xsk_sockets[NUM_SOCKETS];
	int batch_ar[NUM_SOCKETS];
	pthread_t stats_poll_thread;
	int err;
	char errmsg[1024];

	/* Global shutdown handler */
	signal(SIGINT, exit_application);

	/* Cmdline options can change progname */
	parse_cmdline_args(argc, argv, long_options, &cfg, __doc__);

	/* Required option */
	if (cfg.ifindex == -1) {
		fprintf(stderr, "ERROR: Required option --dev missing\n\n");
		usage(argv[0], __doc__, long_options, (argc == 1));
		return EXIT_FAIL_OPTION;
	}

	/* Load custom program if configured */
	if (cfg.filename[0] != 0) {
		struct bpf_map *map;

		custom_xsk = true;
		xdp_opts.open_filename = cfg.filename;
		xdp_opts.prog_name = cfg.progname;
		xdp_opts.opts = &opts;

		if (cfg.progname[0] != 0) {
			xdp_opts.open_filename = cfg.filename;
			xdp_opts.prog_name = cfg.progname;
			xdp_opts.opts = &opts;

			prog = xdp_program__create(&xdp_opts);
		} else {
			prog = xdp_program__open_file(cfg.filename,
						  NULL, &opts);
		}
		err = libxdp_get_error(prog);
		if (err) {
			libxdp_strerror(err, errmsg, sizeof(errmsg));
			fprintf(stderr, "ERR: loading program: %s\n", errmsg);
			return err;
		}

		err = xdp_program__attach(prog, cfg.ifindex, cfg.attach_mode, 0);
		if (err) {
			libxdp_strerror(err, errmsg, sizeof(errmsg));
			fprintf(stderr, "Couldn't attach XDP program on iface '%s' : %s (%d)\n",
				cfg.ifname, errmsg, err);
			return err;
		}

		/* We also need to load the xsks_map */
		map = bpf_object__find_map_by_name(xdp_program__bpf_obj(prog), "xsks_map");
		xsk_map_fd = bpf_map__fd(map);
		printf("correct xsk_map_fd: %d\n", xsk_map_fd);
		if (xsk_map_fd < 0) {
			fprintf(stderr, "ERROR: no xsks map found: %s\n",
				strerror(xsk_map_fd));
			exit(EXIT_FAILURE);
		}
	}

	/* Allow unlimited locking of memory, so all memory needed for packet
	 * buffers can be locked.
	 */
	if (setrlimit(RLIMIT_MEMLOCK, &rlim)) {
		fprintf(stderr, "ERROR: setrlimit(RLIMIT_MEMLOCK) \"%s\"\n",
			strerror(errno));
		exit(EXIT_FAILURE);
	}

	/* Allocate memory for NUM_FRAMES of the default XDP frame size */
	packet_buffer_size = NUM_FRAMES * FRAME_SIZE;
	if (posix_memalign(&packet_buffer,
			   getpagesize(), /* PAGE_SIZE aligned */
			   packet_buffer_size)) {
		fprintf(stderr, "ERROR: Can't allocate buffer memory \"%s\"\n",
			strerror(errno));
		exit(EXIT_FAILURE);
	}

	/* Initialize shared packet_buffer for umem usage */
	for (int sockidx = 0; sockidx < NUM_SOCKETS; ++sockidx) {
		umems[sockidx] = configure_xsk_umem(packet_buffer, packet_buffer_size);
		if (umems[sockidx] == NULL) {
			fprintf(stderr, "ERROR: Can't create umem \"%s\"\n",
				strerror(errno));
			exit(EXIT_FAILURE);
		}
	}

	/* Open and configure the AF_XDP (xsk) sockets */
	for (int sockidx = 0; sockidx < NUM_SOCKETS; ++sockidx) {
		xsk_sockets[sockidx] = xsk_configure_socket(&cfg, umems[sockidx], sockidx);
		if (xsk_sockets[sockidx] == NULL) {
			fprintf(stderr, "ERROR: Can't setup AF_XDP socket \"%s\"\n",
				strerror(errno));
			exit(EXIT_FAILURE);
		}

		batch_ar[sockidx] = 0;
	}

	
	/* Receive and count packets than drop them */
	pthread_t threads[NUM_THREADS];

	struct threadArgs* th_args = malloc(sizeof(struct threadArgs));
	th_args->xskis = xsk_sockets;
	th_args->batch_ar = batch_ar;
	for (int th_idx = 0; th_idx < NUM_THREADS; ++th_idx) {
		ret = pthread_create(&threads[th_idx], NULL, rx_and_process, th_args);
	}
	
	// Wait for all threads to finish
	for (int th_idx = 0; th_idx < NUM_THREADS; ++th_idx) {
		pthread_join(threads[th_idx], NULL);
	}

	printf("Threads finished\n");

	/* Cleanup */
	for (int sockidx = 0; sockidx < NUM_SOCKETS; ++sockidx) {
		xsk_socket__delete(xsk_sockets[sockidx]->xsk);
		xsk_umem__delete(umems[sockidx]->umem);
	}

	return EXIT_OK;
}