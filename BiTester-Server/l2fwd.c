#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/queue.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <setjmp.h>
#include <stdarg.h>
#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdbool.h>

#include <rte_common.h>
#include <rte_log.h>
#include <rte_malloc.h>
#include <rte_memory.h>
#include <rte_memcpy.h>
#include <rte_memzone.h>
#include <rte_eal.h>
#include <rte_launch.h>
#include <rte_atomic.h>
#include <rte_cycles.h>
#include <rte_prefetch.h>
#include <rte_lcore.h>
#include <rte_per_lcore.h>
#include <rte_branch_prediction.h>
#include <rte_interrupts.h>
#include <rte_pci.h>
#include <rte_random.h>
#include <rte_debug.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_mempool.h>
#include <rte_mbuf.h>
#include <rte_ip.h>

#define NB_MBUF 203456
#define BURST_SIZE 32
#define BUF_SIZE 2048
#define MAX_QUEUES 16
#define MAX_ETHPORTS 16
#define MIN_PKT_SIZE 64
#define MAX_PKT_SIZE 1518
#define MEMPOOL_CACHE_SIZE 256
#define MAX_MBUFS_PER_PORT 16384
#define MBUF_SIZE (BUF_SIZE + RTE_PKTMBUF_HEADROOM)
#define TIMEVAL_TO_MSEC(t) ((t.tv_sec * 1000) + (t.tv_usec / 1000))
#define OFF_MF 0x2000
#define OFF_MASK 0x1fff
#define MAX_SEGS_BUFFER_SPLIT 8

/*
 * Configurable number of RX/TX ring descriptors
 */
#define RTE_TEST_RX_DESC_DEFAULT 128
#define RTE_TEST_TX_DESC_DEFAULT 128

/* device info */
static struct rte_eth_dev_info dev_info[MAX_ETHPORTS];

/* port configure */
static struct rte_eth_conf port_conf = {
	.rxmode = {
		.mq_mode = ETH_MQ_RX_RSS,
		.max_rx_pkt_len = RTE_ETHER_MAX_LEN,
		.split_hdr_size = 0,
	},
	.rx_adv_conf = {
		.rss_conf = {.rss_key = NULL, .rss_hf = 0},
	},
	.txmode = {
		.mq_mode = ETH_MQ_TX_NONE,
		.offloads = DEV_TX_OFFLOAD_MBUF_FAST_FREE | DEV_TX_OFFLOAD_IPV4_CKSUM | DEV_TX_OFFLOAD_UDP_CKSUM,
	},
};

static bool rx_only = false;
static int exec_time = 1000;
static volatile bool reverse = false;
static uint32_t interval = 1;
static volatile bool force_quit;
static struct rte_mempool *pktmbuf_pool[8];
static int dropped[MAX_ETHPORTS][MAX_QUEUES];
static int app_cores, rx_port, tx_port, queue_num;
static bool promiscuous_mode = false; // 如果交换机配置了stp协议，实现hairpin的时候一定swap mac地址，不然端口会被stp协议阻塞；
static uint16_t nb_rxd = RTE_TEST_RX_DESC_DEFAULT;
static uint16_t nb_txd = RTE_TEST_TX_DESC_DEFAULT;
static struct rte_eth_dev_tx_buffer *tx_buffer[MAX_ETHPORTS][MAX_QUEUES];

static void check_all_ports_link_status(uint8_t port_num, uint32_t port_mask)
{
#define CHECK_INTERVAL 100 /* 100ms */
#define MAX_CHECK_TIME 90  /* 9s (90 * 100ms) in total */

	uint8_t portid, count, all_ports_up, print_flag = 0;
	struct rte_eth_link link;

	printf("\nChecking link status");
	fflush(stdout);
	for (count = 0; count <= MAX_CHECK_TIME; count++)
	{
		all_ports_up = 1;
		for (portid = 0; portid < port_num; portid++)
		{
			if ((port_mask & (1 << portid)) == 0)
				continue;
			memset(&link, 0, sizeof(link));
			rte_eth_link_get_nowait(portid, &link);
			/* print link status if flag set */
			if (print_flag == 1)
			{
				if (link.link_status)
					printf("Port %d Link Up - speed %u "
						   "Mbps - %s\n",
						   (uint8_t)portid,
						   (unsigned)link.link_speed,
						   (link.link_duplex == ETH_LINK_FULL_DUPLEX) ? ("full-duplex") : ("half-duplex\n"));
				else
					printf("Port %d Link Down\n",
						   (uint8_t)portid);
				continue;
			}
			/* clear all_ports_up flag if any link down */
			if (link.link_status == 0)
			{
				all_ports_up = 0;
				break;
			}
		}
		/* after finally printing all link status, get out */
		if (print_flag == 1)
			break;

		if (all_ports_up == 0)
		{
			printf(".");
			fflush(stdout);
			rte_delay_ms(CHECK_INTERVAL);
		}

		/* set the print_flag if all ports up or timeout */
		if (all_ports_up == 1 || count == (MAX_CHECK_TIME - 1))
		{
			print_flag = 1;
			printf("done\n");
		}
	}
}

/*
 * Tx buffer error callback
 */
static void flush_tx_error_callback(struct rte_mbuf **unsent, uint16_t count, void *userdata)
{
	int i;
	int *dropped = (int *)userdata;
	*dropped += count;
	/* free the mbufs which failed from transmit */
	for (i = 0; i < count; i++)
		rte_pktmbuf_free(unsent[i]);
}

static void nic_init(int nb_ports)
{
	int portid, queueid, ret;

	/* set up mempool for each port */
	for (portid = 0; portid < nb_ports; portid++)
	{
		uint32_t nb_mbuf = NB_MBUF;
		char name[RTE_MEMPOOL_NAMESIZE];
		sprintf(name, "mbuf_pool-%d", portid);
		/* create the mbuf pool */
		pktmbuf_pool[portid] =
			rte_mempool_create(name, nb_mbuf,
							   MBUF_SIZE, MEMPOOL_CACHE_SIZE,
							   sizeof(struct rte_pktmbuf_pool_private),
							   rte_pktmbuf_pool_init, NULL,
							   rte_pktmbuf_init, NULL,
							   rte_socket_id(), 0);

		if (pktmbuf_pool[portid] == NULL)
			rte_exit(EXIT_FAILURE, "Cannot init mbuf pool, errno: %d\n",
					 rte_errno);

		/* check port capabilities */
		rte_eth_dev_info_get(portid, &dev_info[portid]);

		/* init port */
		printf("Initializing port %u... ", (unsigned)portid);

		// port_conf.intr_conf.lsc = 1;
		// port_conf.intr_conf.rmv = 1;
		port_conf.rx_adv_conf.rss_conf.rss_hf |= dev_info[portid].flow_type_rss_offloads;
		ret = rte_eth_dev_configure(portid, queue_num, queue_num, &port_conf);

		if (ret < 0)
			rte_exit(EXIT_FAILURE, "Cannot configure device: err=%d, port=%u\n",
					 ret, (unsigned)portid);

		for (queueid = 0; queueid < queue_num; queueid++)
		{
			/* setup rx queue */
			struct rte_eth_rxconf rxconf;
			memcpy(&rxconf, &dev_info[portid].default_rxconf, sizeof(struct rte_eth_rxconf));
			ret = rte_eth_rx_queue_setup(portid, queueid, nb_rxd,
										 rte_eth_dev_socket_id(portid), &rxconf,
										 pktmbuf_pool[portid]);
			if (ret < 0)
				rte_exit(EXIT_FAILURE,
						 "rte_eth_rx_queue_setup:err=%d, port=%u, queueid: %d\n",
						 ret, (unsigned)portid, queueid);
		}

		for (queueid = 0; queueid < queue_num; queueid++)
		{
			/* setup tx queue */
			struct rte_eth_txconf txconf;
			memcpy(&txconf, &dev_info[portid].default_txconf, sizeof(struct rte_eth_txconf));
			ret = rte_eth_tx_queue_setup(portid, queueid, nb_txd,
										 rte_eth_dev_socket_id(portid), &txconf);
			if (ret < 0)
				rte_exit(EXIT_FAILURE,
						 "rte_eth_tx_queue_setup:err=%d, port=%u, queueid: %d\n",
						 ret, (unsigned)portid, queueid);
		}

		/* Initialize TX buffers */
		for (queueid = 0; queueid < queue_num; queueid++)
		{
			tx_buffer[portid][queueid] = rte_zmalloc_socket("tx_buffer",
															RTE_ETH_TX_BUFFER_SIZE(BURST_SIZE), 0,
															rte_eth_dev_socket_id(portid));
			if (tx_buffer[portid][queueid] == NULL)
				rte_exit(EXIT_FAILURE, "Cannot allocate buffer for tx on port %u queue %d\n", portid, queueid);
			rte_eth_tx_buffer_init(tx_buffer[portid][queueid], BURST_SIZE);
			ret = rte_eth_tx_buffer_set_err_callback(tx_buffer[portid][queueid],
													 flush_tx_error_callback,
													 &dropped[portid][queueid]);
			if (ret < 0)
				rte_exit(EXIT_FAILURE,
						 "Cannot set error callback for tx buffer on port %u queue %d\n", portid, queueid);
		}

		/* Start device */
		ret = rte_eth_dev_start(portid);
		if (ret < 0)
			rte_exit(EXIT_FAILURE, "rte_eth_dev_start:err=%d, port=%u\n",
					 ret, (unsigned)portid);

		printf("done: \n");
		if (promiscuous_mode)
			rte_eth_promiscuous_enable(portid);
	}

	check_all_ports_link_status(nb_ports, 0xFFFFFFFF);
}

void signal_handler(int signum)
{
	if (signum == SIGINT || signum == SIGTERM)
	{
		printf("\n\nSignal %d received, preparing to exit...\n", signum);
		force_quit = true;
	}
}

#define BURST_TX_DRAIN_US 100 /* TX drain every ~100us */

#define MAX_QUEUE 16
struct Stats
{
	uint64_t rx_pkts;
	uint64_t tx_pkts;
	uint64_t rx_byts;
	uint64_t cycles;
};
struct Stats stats[MAX_QUEUE] = {0}, preStats[MAX_QUEUE] = {0}, curStats[MAX_QUEUE] = {0};

static uint64_t _prev = 0, _cur = 0;
void showTotal()
{
	uint64_t rx_pkts = 0, tx_pkts = 0, rx_byts = 0, cycles = 0;
	_cur = rte_rdtsc();
	memcpy(curStats, stats, sizeof(stats));
	for (int i = 0; i < queue_num; i++)
	{
		rx_pkts += curStats[i].rx_pkts - preStats[i].rx_pkts;
		tx_pkts += curStats[i].tx_pkts - preStats[i].tx_pkts;
		rx_byts += curStats[i].rx_byts - preStats[i].rx_byts;
		cycles += curStats[i].cycles - preStats[i].cycles;
	}
	memcpy(preStats, curStats, sizeof(stats));
	double diff_ts = (double)(_cur - _prev) / (double)rte_get_tsc_hz();
	RTE_LOG(INFO, USER1, "**********[Total] RX %.2f Mpps, %.2f Gbps, TX %.2f Mpps, CPU %.2f****************\n",
			rx_pkts * 1.0 / diff_ts / 1000000,
			rx_byts * 8.0 / diff_ts / 1000000000,
			tx_pkts * 1.0 / diff_ts / 1000000,
			cycles * 1.0 / (double)(_cur - _prev));
	_prev = _cur;
}

#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_udp.h>
void print_7tuple(struct rte_mbuf *m)
{
	struct rte_ether_hdr *ether_h;
	struct rte_ipv4_hdr *ipv4_h;
	struct rte_udp_hdr *udp_h;
	ether_h = rte_pktmbuf_mtod_offset(m, void *, 0);
	printf("|%02X:%02X:%02X:%02X:%02X:%02X->%02X:%02X:%02X:%02X:%02X:%02X|",
		   ether_h->d_addr.addr_bytes[0],
		   ether_h->d_addr.addr_bytes[1],
		   ether_h->d_addr.addr_bytes[2],
		   ether_h->d_addr.addr_bytes[3],
		   ether_h->d_addr.addr_bytes[4],
		   ether_h->d_addr.addr_bytes[5],
		   ether_h->s_addr.addr_bytes[0],
		   ether_h->s_addr.addr_bytes[1],
		   ether_h->s_addr.addr_bytes[2],
		   ether_h->s_addr.addr_bytes[3],
		   ether_h->s_addr.addr_bytes[4],
		   ether_h->s_addr.addr_bytes[5]);

	void print_ipv4_addr(const rte_be32_t dip, const rte_be32_t sip)
	{
		printf("%d.%d.%d.%d->%d.%d.%d.%d|",
			   (dip & 0xff000000) >> 24,
			   (dip & 0x00ff0000) >> 16,
			   (dip & 0x0000ff00) >> 8,
			   (dip & 0x000000ff),
			   (sip & 0xff000000) >> 24,
			   (sip & 0x00ff0000) >> 16,
			   (sip & 0x0000ff00) >> 8,
			   (sip & 0x000000ff));
	}
	uint16_t IPV4 = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);
	// printf("%X\n",rte_be_to_cpu_16(ether_h->ether_type));
	if (likely(ether_h->ether_type == IPV4))
	{
		ipv4_h = rte_pktmbuf_mtod_offset(m, void *, sizeof(struct rte_ether_hdr));
		print_ipv4_addr(rte_be_to_cpu_32(ipv4_h->src_addr), rte_be_to_cpu_32(ipv4_h->dst_addr));
		if (likely(ipv4_h->next_proto_id == 17))
		{
			udp_h = rte_pktmbuf_mtod_offset(m, void *, sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr));
			printf("UDP,%d->%d|", rte_be_to_cpu_16(udp_h->src_port), rte_be_to_cpu_16(udp_h->dst_port));
		}
	}
	printf("\n");
}

void traverse(struct rte_mbuf *m)
{
	struct rte_ether_hdr *ether_h;
	struct rte_ipv4_hdr *ipv4_h;
	struct rte_udp_hdr *udp_h;

	ether_h = rte_pktmbuf_mtod_offset(m, void *, 0);

	// reverse mac
	struct rte_ether_hdr tmp;
	rte_ether_addr_copy(&ether_h->d_addr, &tmp);
	rte_ether_addr_copy(&ether_h->s_addr,
						&ether_h->d_addr);
	rte_ether_addr_copy(&tmp, &ether_h->s_addr);

	uint16_t IPV4 = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);
	if (likely(ether_h->ether_type == IPV4))
	{
		ipv4_h = rte_pktmbuf_mtod_offset(m, void *, sizeof(struct rte_ether_hdr));
		uint32_t tmp_ip = ipv4_h->src_addr;
		ipv4_h->src_addr = ipv4_h->dst_addr;
		ipv4_h->dst_addr = tmp_ip;
		ipv4_h->hdr_checksum = 0;
		if (likely(ipv4_h->next_proto_id == 17))
		{
			udp_h = rte_pktmbuf_mtod_offset(m, void *, sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr));
			uint16_t tmp_port = udp_h->src_port;
			udp_h->src_port = udp_h->dst_port;
			udp_h->dst_port = tmp_port;
			udp_h->dgram_cksum = 0;
		}
	}

	m->l2_len = sizeof(struct rte_ether_hdr);
	m->l3_len = sizeof(struct rte_ipv4_hdr);
	m->ol_flags |= PKT_TX_IPV4 | PKT_TX_IP_CKSUM | PKT_TX_UDP_CKSUM; // offload checksum
}

int baseline_main_loop(int q_id)
{

	int i, core, nb_rx = 0, time_cnt = 0;
	struct rte_mbuf *pkts_burst[BURST_SIZE];
	uint64_t cur_tv, prev_tv, begin_tv;
	uint64_t prev_tsc = 0, diff_tsc, cur_tsc, cpuUtil = 0;
	uint64_t recv_pkts = 0, recv_bytes = 0, send_pkts = 0, total_recv = 0;
	const uint64_t drain_tsc = (rte_get_tsc_hz() + US_PER_S - 1) / US_PER_S * BURST_TX_DRAIN_US;
	double diff_ts;
	core = rte_lcore_id();

	// timer
	cur_tv = rte_rdtsc();
	prev_tv = cur_tv;
	begin_tv = cur_tv;

	while (!force_quit)
	{

		/* Drains TX queue in its main loop. */
		cur_tsc = rte_rdtsc();
		/*
		 * TX burst queue drain
		 */
		diff_tsc = cur_tsc - prev_tsc;
		if (unlikely(diff_tsc > drain_tsc))
		{
			send_pkts += rte_eth_tx_buffer_flush(tx_port, q_id, tx_buffer[tx_port][q_id]);
			prev_tsc = cur_tsc;
		}

		nb_rx = rte_eth_rx_burst(rx_port, q_id, pkts_burst, BURST_SIZE);
		if (nb_rx)
		{
			for (i = 0; i < nb_rx; i++)
			{
				int pkt_len = pkts_burst[i]->pkt_len;
				pkt_len = +24;
				if (pkt_len < 84)
				{
					pkt_len = 84;
				}
				recv_bytes += pkt_len;
				stats[q_id].rx_byts += pkt_len;

				if (rx_only)
					rte_pktmbuf_free(pkts_burst[i]);
				else
				{
					// print_7tuple(pkts_burst[i]);
					if (reverse)
					{
						// print_7tuple(pkts_burst[i]);
						traverse(pkts_burst[i]);
						// print_7tuple(pkts_burst[i]);
					}
					// print_7tuple(pkts_burst[i]);

					int sent = rte_eth_tx_buffer(tx_port, q_id, tx_buffer[tx_port][q_id], pkts_burst[i]);
					send_pkts += sent;
					stats[q_id].tx_pkts += sent;
				}
			}
			cpuUtil += rte_rdtsc() - cur_tsc;
			stats[q_id].cycles += rte_rdtsc() - cur_tsc;
		}
		stats[q_id].rx_pkts += nb_rx;
		recv_pkts += nb_rx;
		total_recv += nb_rx;

		cur_tv = rte_rdtsc();
		diff_ts = (double)(cur_tv - prev_tv) / (double)rte_get_tsc_hz();
		if (diff_ts > interval)
		{
			// printf("diff_ts = (cur_tv - prev_tv) / rte_get_tsc_hz()=%lu/%lu=%lf\n", cur_tv - prev_tv, rte_get_tsc_hz(), diff_ts);
			RTE_LOG(INFO, USER1, "[CPU %d] %d RX %.2f Mpps, %.2f Gbps, TX %.2f Mpps, Dropped %.2f, Util %.2f\n",
					core, time_cnt++,
					recv_pkts * 1.0 / diff_ts / 1000000,
					recv_bytes * 8.0 / diff_ts / 1000000000,
					send_pkts * 1.0 / diff_ts / 1000000,
					dropped[tx_port][q_id] * 1.0 / diff_ts / 1000,
					cpuUtil * 1.0 / (double)(cur_tv - prev_tv));
			prev_tv = cur_tv;
			recv_pkts = 0;
			recv_bytes = 0;
			send_pkts = 0;
			cpuUtil = 0;
			dropped[tx_port][q_id] = 0;

			if (core == 1)
				showTotal();
		}
	}

	cur_tv = rte_rdtsc();
	diff_ts = (double)(cur_tv - begin_tv) / (double)rte_get_tsc_hz();
	RTE_LOG(INFO, USER2, "[CPU %d] %d RX %.2f Mpps\n", core, time_cnt, total_recv * 1.0 / diff_ts / 1000000);

	return 0;
}

int baseline_main_loop_burst(int q_id)
{

	int i, core, nb_rx = 0, nb_tx = 0, time_cnt = 0;
	struct rte_mbuf *pkts_burst[BURST_SIZE];
	uint64_t cur_tv, prev_tv, begin_tv;
	uint64_t prev_tsc = 0, diff_tsc, cur_tsc, cpuUtil = 0;
	uint64_t recv_pkts = 0, recv_bytes = 0, send_pkts = 0, total_recv = 0;
	const uint64_t drain_tsc = (rte_get_tsc_hz() + US_PER_S - 1) / US_PER_S * BURST_TX_DRAIN_US;
	double diff_ts;
	core = rte_lcore_id();

	// timer
	cur_tv = rte_rdtsc();
	prev_tv = cur_tv;
	begin_tv = cur_tv;

	while (!force_quit)
	{

		/* Drains TX queue in its main loop. */
		cur_tsc = rte_rdtsc();
		/*
		 * TX burst queue drain
		 */
		diff_tsc = cur_tsc - prev_tsc;
		if (unlikely(diff_tsc > drain_tsc))
		{
			send_pkts += rte_eth_tx_buffer_flush(tx_port, q_id, tx_buffer[tx_port][q_id]);
			prev_tsc = cur_tsc;
		}

		nb_rx = rte_eth_rx_burst(rx_port, q_id, pkts_burst, BURST_SIZE);
		if (nb_rx)
		{
			for (i = 0; i < nb_rx; i++)
			{
				int pkt_len = pkts_burst[i]->pkt_len;
				pkt_len = +24;
				if (pkt_len < 84)
				{
					pkt_len = 84;
				}
				recv_bytes += pkt_len;
				stats[q_id].rx_byts += pkt_len;

				if (rx_only)
					rte_pktmbuf_free(pkts_burst[i]);
				else
				{
					// print_7tuple(pkts_burst[i]);
					if (reverse)
					{
						// print_7tuple(pkts_burst[i]);
						traverse(pkts_burst[i]);
						// print_7tuple(pkts_burst[i]);
					}
					// print_7tuple(pkts_burst[i]);
					// int sent = rte_eth_tx_buffer(tx_port, q_id, tx_buffer[tx_port][q_id], pkts_burst[i]);
					// send_pkts += sent;
					// stats[q_id].tx_pkts += sent;
				}
			}
			cpuUtil += rte_rdtsc() - cur_tsc;
			stats[q_id].cycles += rte_rdtsc() - cur_tsc;
		}
		stats[q_id].rx_pkts += nb_rx;
		recv_pkts += nb_rx;
		total_recv += nb_rx;

		if (!rx_only)
		{
			nb_tx = rte_eth_tx_burst(tx_port, q_id, pkts_burst, nb_rx);
			send_pkts += nb_tx;
			stats[q_id].tx_pkts += nb_tx;
			if (unlikely(nb_tx < nb_rx))
			{
				// fs->fwd_dropped += (nb_rx - nb_tx);
				do
				{
					rte_pktmbuf_free(pkts_burst[nb_tx]);
				} while (++nb_tx < nb_rx);
			}
		}

		cur_tv = rte_rdtsc();
		diff_ts = (double)(cur_tv - prev_tv) / (double)rte_get_tsc_hz();
		if (diff_ts > interval)
		{
			// printf("diff_ts = (cur_tv - prev_tv) / rte_get_tsc_hz()=%lu/%lu=%lf\n", cur_tv - prev_tv, rte_get_tsc_hz(), diff_ts);
			RTE_LOG(INFO, USER1, "[CPU %d] %d RX %.2f Mpps, %.2f Gbps, TX %.2f Mpps, Dropped %.2f, Util %.2f\n",
					core, time_cnt++,
					recv_pkts * 1.0 / diff_ts / 1000000,
					recv_bytes * 8.0 / diff_ts / 1000000000,
					send_pkts * 1.0 / diff_ts / 1000000,
					dropped[tx_port][q_id] * 1.0 / diff_ts / 1000,
					cpuUtil * 1.0 / (double)(cur_tv - prev_tv));
			prev_tv = cur_tv;
			recv_pkts = 0;
			recv_bytes = 0;
			send_pkts = 0;
			cpuUtil = 0;
			dropped[tx_port][q_id] = 0;

			if (core == 1)
				showTotal();
		}
	}

	cur_tv = rte_rdtsc();
	diff_ts = (double)(cur_tv - begin_tv) / (double)rte_get_tsc_hz();
	RTE_LOG(INFO, USER2, "[CPU %d] %d RX %.2f Mpps\n", core, time_cnt, total_recv * 1.0 / diff_ts / 1000000);

	return 0;
}

static int launch_one_lcore(void *arg __rte_unused)
{
	int qid = rte_lcore_id() - 1;

	if (qid < queue_num)
		// baseline_main_loop(qid);
		baseline_main_loop_burst(qid);

	return 0;
}

int main(int argc, char **argv)
{
	int i, ret;
	uint8_t nb_ports, portid;

	/* init EAL */
	ret = rte_eal_init(argc, argv);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Invalid EAL arguments\n");

	argc -= ret;
	argv += ret;
	for (i = 1; i < argc; i++)
	{
		if (strcmp(argv[i], "-n") == 0)
		{
			app_cores = atoi(argv[++i]);
		}
		else if (strcmp(argv[i], "-r") == 0)
		{
			rx_port = atoi(argv[++i]);
		}
		else if (strcmp(argv[i], "-t") == 0)
		{
			tx_port = atoi(argv[++i]);
		}
		else if (strcmp(argv[i], "-q") == 0)
		{
			queue_num = atoi(argv[++i]);
		}
		else if (strcmp(argv[i], "-rxonly") == 0)
		{
			rx_only = true;
		}
		else if (strcmp(argv[i], "-e") == 0)
		{
			exec_time = atoi(argv[++i]);
		}
		else if (strcmp(argv[i], "-reverse") == 0)
		{
			reverse = true; // swap 7 tuples
			promiscuous_mode = true;
			printf("start reverse traffic......\n");
		}
	}

	force_quit = false;
	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);

	nb_ports = rte_eth_dev_count_avail();
	if (nb_ports == 0)
		rte_exit(EXIT_FAILURE, "No Ethernet ports - bye\n");

	// run init processure
	nic_init(nb_ports);

	/* launch per-lcore init on every lcore */
	rte_eal_mp_remote_launch(launch_one_lcore, NULL, SKIP_MAIN);
	sleep(exec_time);
	force_quit = true;

	for (portid = 0; portid < nb_ports; portid++)
	{
		printf("Closing port %d...", portid);
		rte_eth_dev_stop(portid);
		rte_eth_dev_close(portid);
		printf(" Done\n");
	}

	return ret;
}
