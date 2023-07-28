#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/queue.h>
#include <netinet/in.h>
#include <setjmp.h>
#include <stdarg.h>
#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdbool.h>
#include <unistd.h>

#include <rte_common.h>
#include <rte_log.h>
#include <rte_malloc.h>
#include <rte_memory.h>
#include <rte_memcpy.h>
#include <rte_eal.h>
#include <rte_launch.h>
#include <rte_atomic.h>
#include <rte_cycles.h>
#include <rte_prefetch.h>
#include <rte_lcore.h>
#include <rte_per_lcore.h>
#include <rte_branch_prediction.h>
#include <rte_interrupts.h>
#include <rte_random.h>
#include <rte_debug.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_mempool.h>
#include <rte_mbuf.h>
#include <rte_string_fns.h>

//////////////////////////////////////
/*--------------头文件区-------------*/
#include <rte_ip.h>
#include <rte_udp.h>
#include <rte_tcp.h>
#include <rte_ring.h>

#include <sys/wait.h>
#include <time.h>

static volatile bool force_quit;
static volatile bool recv_ready = false;
static int queue_num = 1;
#define RTE_LOGTYPE_L2FWD RTE_LOGTYPE_USER1
#define MAX_QUEUE 16
#define MAX_PKT_BURST 32
#define MAX_TX_BURST 64
#define BURST_TX_DRAIN_US 10 /* TX drain every ~100us */
#define MEMPOOL_CACHE_SIZE 256

/*
 * Configurable number of RX/TX ring descriptors
 */
#define RTE_TEST_RX_DESC_DEFAULT 1024
#define RTE_TEST_TX_DESC_DEFAULT 1024 / 2
static uint16_t nb_rxd = RTE_TEST_RX_DESC_DEFAULT;
static uint16_t nb_txd = RTE_TEST_TX_DESC_DEFAULT;
static int RX_PORT = -1, TX_PORT = -1;
/* ethernet addresses of ports */
static struct rte_ether_addr l2fwd_ports_eth_addr[RTE_MAX_ETHPORTS];
/*mbuf_pool*/
struct rte_mempool *pktmbuf_pool[RTE_MAX_ETHPORTS];

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

#define MAX_TIMER_PERIOD 86400 /* 1 day max */
/* A tsc-based timer responsible for triggering statistics printout */
static uint64_t timer_period = 10; /* default period is 10 seconds */

////////////////////////////////////////////////
/* Check the link status of all ports in up to 9s, and print them finally */
static void
check_all_ports_link_status(uint32_t *port_array, uint32_t len)
{
#define CHECK_INTERVAL 100 /* 100ms */
#define MAX_CHECK_TIME 90  /* 9s (90 * 100ms) in total */
	uint16_t portid;
	uint8_t count, all_ports_up, print_flag = 0;
	struct rte_eth_link link;
	int ret;
	char link_status_text[RTE_ETH_LINK_MAX_STR_LEN];

	RTE_LOG(INFO, USER1, "Checking link status\n");
	fflush(stdout);
	for (count = 0; count <= MAX_CHECK_TIME; count++)
	{
		uint32_t *_port_array = port_array;
		uint32_t _len = len;
		if (force_quit)
			return;
		all_ports_up = 1;
		void checking(int portid)
		{
			if (force_quit)
				return;
			// if ((port_mask & (1 << portid)) == 0)
			// 	continue;
			memset(&link, 0, sizeof(link));
			ret = rte_eth_link_get_nowait(portid, &link);
			if (ret < 0)
			{
				all_ports_up = 0;
				if (print_flag == 1)
					RTE_LOG(INFO, USER1, "Port %u link get failed: %s\n",
							portid, rte_strerror(-ret));
				return;
			}
			/* print link status if flag set */
			if (print_flag == 1)
			{
				rte_eth_link_to_str(link_status_text,
									sizeof(link_status_text), &link);
				RTE_LOG(INFO, USER1, "Port %d %s\n", portid,
						link_status_text);
				return;
			}
			/* clear all_ports_up flag if any link down */
			if (link.link_status == ETH_LINK_DOWN)
			{
				all_ports_up = 0;
				return;
			}
		}

		while (_len-- > 0)
		{
			checking(*(_port_array++));
		}

		/* after finally printing all link status, get out */
		if (print_flag == 1)
			break;

		if (all_ports_up == 0)
		{
			RTE_LOG(INFO, USER1, ".");
			fflush(stdout);
			rte_delay_ms(CHECK_INTERVAL);
		}
		/* set the print_flag if all ports up or timeout */
		if (all_ports_up == 1 || count == (MAX_CHECK_TIME - 1))
		{
			print_flag = 1;
			RTE_LOG(INFO, USER1, "done\n");
		}
	}
}

static void
signal_handler(int signum)
{
	if (signum == SIGINT || signum == SIGTERM)
	{
		RTE_LOG(INFO, USER1, "\n\nSignal %d received, preparing to exit...\n",
				signum);
		force_quit = true;
		// rte_exit(EXIT_FAILURE, "CRT+C Bye!!!\n");
	}
}

/*-------------------------------------------------------------------------------------------------------*/
static int Initialise_Port(int portid, int rx_num, int tx_num, int nb_lcores)
{
	struct rte_eth_rxconf rxq_conf;
	struct rte_eth_txconf txq_conf;
	// struct rte_eth_conf local_port_conf = port_conf;
	struct rte_eth_dev_info dev_info;

	/* init port */
	printf("Initializing port %u... ", portid);
	fflush(stdout);

	/////////////////////////////////////////
	unsigned int nb_mbufs = RTE_MAX(nb_rxd * nb_lcores + nb_txd * nb_lcores + MAX_PKT_BURST +
										nb_lcores * MEMPOOL_CACHE_SIZE,
									8192U);
	/* create the mbuf pool */
	char name[100];
	sprintf(name, "mbuf_pool-%d", portid);
	pktmbuf_pool[portid] = rte_pktmbuf_pool_create(name, nb_mbufs,
												   MEMPOOL_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE,
												   rte_socket_id());
	if (pktmbuf_pool[portid] == NULL)
		rte_exit(EXIT_FAILURE, "Cannot init mbuf pool\n");

	//////////////////////////////////////

	int ret = rte_eth_dev_info_get(portid, &dev_info);
	if (ret != 0)
		rte_exit(EXIT_FAILURE,
				 "Error during getting device (port %u) info: %s\n",
				 portid, strerror(-ret));

	//	if (dev_info.tx_offload_capa & DEV_TX_OFFLOAD_MBUF_FAST_FREE)
	port_conf.txmode.offloads |=
		DEV_TX_OFFLOAD_IPV4_CKSUM | DEV_TX_OFFLOAD_UDP_CKSUM | DEV_TX_OFFLOAD_MBUF_FAST_FREE;

	port_conf.rx_adv_conf.rss_conf.rss_hf |= dev_info.flow_type_rss_offloads;
	ret = rte_eth_dev_configure(portid, rx_num, tx_num, &port_conf); // &local_port_conf);

	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Cannot configure device: err=%d, port=%u\n",
				 ret, portid);

	ret = rte_eth_dev_adjust_nb_rx_tx_desc(portid, &nb_rxd,
										   &nb_txd);

	if (ret < 0)
		rte_exit(EXIT_FAILURE,
				 "Cannot adjust number of descriptors: err=%d, port=%u\n",
				 ret, portid);

	ret = rte_eth_macaddr_get(portid,
							  &l2fwd_ports_eth_addr[portid]);
	if (ret < 0)
		rte_exit(EXIT_FAILURE,
				 "Cannot get MAC address: err=%d, port=%u\n",
				 ret, portid);

	/* init one RX queue */
	fflush(stdout);
	rxq_conf = dev_info.default_rxconf;
	rxq_conf.offloads = port_conf.rxmode.offloads;
	for (int r = 0; r < rx_num; r++)
	{
		ret = rte_eth_rx_queue_setup(portid, r, nb_rxd,
									 rte_eth_dev_socket_id(portid),
									 &rxq_conf,
									 pktmbuf_pool[portid]);
		if (ret < 0)
			rte_exit(EXIT_FAILURE, "rte_eth_rx_queue_setup:err=%d, port=%u\n",
					 ret, portid);
	}
	/* init one TX queue on each port */
	fflush(stdout);
	txq_conf = dev_info.default_txconf;
	txq_conf.offloads = port_conf.txmode.offloads;
	for (int t = 0; t < tx_num; t++)
	{
		ret = rte_eth_tx_queue_setup(portid, t, nb_txd,
									 rte_eth_dev_socket_id(portid),
									 &txq_conf);
		if (ret < 0)
			rte_exit(EXIT_FAILURE, "rte_eth_tx_queue_setup:err=%d, port=%u\n",
					 ret, portid);
	}

	ret = rte_eth_dev_set_ptypes(portid, RTE_PTYPE_UNKNOWN, NULL,
								 0);
	if (ret < 0)
		printf("Port %u, Failed to disable Ptype parsing\n",
			   portid);
	/* Start device */
	ret = rte_eth_dev_start(portid);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "rte_eth_dev_start:err=%d, port=%u\n",
				 ret, portid);

	printf("done: \n");

	ret = rte_eth_promiscuous_enable(portid);
	if (ret != 0)
		rte_exit(EXIT_FAILURE,
				 "rte_eth_promiscuous_enable:err=%s, port=%u\n",
				 rte_strerror(-ret), portid);

	printf("Port %u, MAC address: %02X:%02X:%02X:%02X:%02X:%02X\n\n",
		   portid,
		   l2fwd_ports_eth_addr[portid].addr_bytes[0],
		   l2fwd_ports_eth_addr[portid].addr_bytes[1],
		   l2fwd_ports_eth_addr[portid].addr_bytes[2],
		   l2fwd_ports_eth_addr[portid].addr_bytes[3],
		   l2fwd_ports_eth_addr[portid].addr_bytes[4],
		   l2fwd_ports_eth_addr[portid].addr_bytes[5]);

	return 1;
}

static int dpdk_init(int argc, char **argv, int nb_ports, unsigned nb_lcores)
{
	/* init EAL */
	int ret = rte_eal_init(argc, argv);

	force_quit = false;
	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);

	/* convert to number of cycles */
	timer_period *= rte_get_timer_hz();

	if (nb_ports > rte_eth_dev_count_avail())
		rte_exit(EXIT_FAILURE, "No Enough ports - bye\n");

	/////////////////////////////////////////////initialise port
	uint32_t ports[RTE_MAX_ETHPORTS];
	for (int i = 0; i < nb_ports; i++)
	{
		if (Initialise_Port(i, nb_lcores, nb_lcores, nb_lcores) <= 0)
		{
			rte_exit(EXIT_FAILURE,
					 "PORT UNAVAILABLE.\n");
		}
		ports[i] = i;
	}
	check_all_ports_link_status(ports, nb_ports);
	queue_num = nb_lcores;
	return 0;
}

static int dpdk_destroy(int nb_ports)
{
	int ret = 0;
	void closePorts(int portid)
	{
		RTE_LOG(INFO, USER1, "Closing port %d...\n", portid);
		ret = rte_eth_dev_stop(portid);
		if (ret != 0)
			RTE_LOG(INFO, USER1, "rte_eth_dev_stop: err=%d, port=%d\n",
					ret, portid);
		rte_eth_dev_close(portid);
		RTE_LOG(INFO, USER1, " Done\n");
	}
	for (int i = 0; i < nb_ports; i++)
	{
		closePorts(i);
	}
	/* clean up the EAL */
	rte_eal_cleanup();
	RTE_LOG(INFO, USER1, "Bye...\n");
}
