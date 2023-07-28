#include "stack.c"
static void
LAT_worker(int RX_PORT, int TX_PORT, int queue_id, int payloadLength, int burst, double Mpps, int printGap, int mode, struct Flows *flows)
{
    uint64_t next_tx = rte_rdtsc(), cur_tsc = 0, lastPrint = rte_rdtsc(), psn = 0, nb_tx = 0, nb_rx = 0, cpu_cycles = 0;
    uint64_t Latency = 0, TX = 0, RX = 0;
    const uint64_t TxDuration = rte_get_tsc_hz() / 1000000.0 / Mpps;
    struct rte_mbuf *TX_MBUFS[MAX_TX_BURST];
    struct rte_mbuf *pkts_burst[MAX_PKT_BURST];

    struct Flows thisFlow;
    memcpy(&thisFlow, flows, sizeof(struct Flows));
    uint64_t cps_gap = rte_get_tsc_hz() / thisFlow.cps, cps_timer = rte_rdtsc() + cps_gap;
    thisFlow.flow_num = 1;

    while (!force_quit)
    {
        if (mode >= 0)
        {
            cur_tsc = rte_rdtsc();

            if (cur_tsc > cps_timer && thisFlow.flow_num < flows->flow_num)
            {
                // RTE_LOG(INFO, USER1, "Flow_num=%d\n", thisFlow.flow_num + 1);
                thisFlow.flow_num = thisFlow.flow_num + 1;
                cps_timer += cps_gap;
            }

            if (cur_tsc > next_tx)
            {
                for (int i = 0; i < burst; i++)
                {
                    thisFlow.src_ip = (psn % thisFlow.flow_num) / 65536 + flows->src_ip;
                    thisFlow.src_port = (psn % thisFlow.flow_num + flows->src_port) % 65536;
                    TX_MBUFS[i] = generateLatPerfMBuf(psn, payloadLength, &thisFlow, pktmbuf_pool[TX_PORT]);
                    psn++;
                }
                for (int i = 0; i < burst; i++)
                {
                    setLat(TX_MBUFS[i]);
                }
                nb_tx = rte_eth_tx_burst(TX_PORT, queue_id, TX_MBUFS, burst);
                TX += nb_tx;
                if (unlikely(nb_tx < burst))
                {
                    do
                    {
                        rte_pktmbuf_free(TX_MBUFS[nb_tx]);
                    } while (++nb_tx < burst);
                }
                next_tx += TxDuration;
                cpu_cycles += rte_rdtsc() - cur_tsc;
            }
        }

        if (mode <= 0)
        {
            nb_rx = rte_eth_rx_burst(RX_PORT, queue_id, pkts_burst, MAX_PKT_BURST); // 接收回传探测包
            if (nb_rx)
            {
                uint64_t tmp_cycle = rte_rdtsc();
                for (int i = 0; i < nb_rx; i++)
                {
                    struct rte_mbuf *m = pkts_burst[i];
                    rte_prefetch0(rte_pktmbuf_mtod(m, void *));
                    uint64_t tmp = getLat(m);
                    if (likely(tmp > 0))
                    {
                        Latency += tmp;
                        RX++;
                    }
                    rte_pktmbuf_free(m);
                }
                cpu_cycles += rte_rdtsc() - tmp_cycle;
            }
        }
        uint64_t _cur_tsc = rte_rdtsc();
        if (_cur_tsc - lastPrint > printGap * rte_get_tsc_hz())
        {

            double cpuHz = rte_get_tsc_hz() / 1000000;
            double avg_cycles = (double)Latency / RX;
            double lat = avg_cycles / cpuHz;
            double interval = (_cur_tsc - lastPrint) / cpuHz;
            double TXPPS = TX / interval;
            double RXPPS = RX / interval;
            double util = cpu_cycles * 1.0 / ((_cur_tsc - lastPrint) * 1.0);
            RTE_LOG(INFO, USER1, "[Latency Test] Core %d,TX %.2lf Mpps, RX: %.2lf Mpps, CPU Util %.2lf, AVG LAT:%.2lfus\n", rte_lcore_id(), TXPPS, RXPPS, util, lat);
            lastPrint = _cur_tsc;
            TX = 0;
            RX = 0;
            cpu_cycles = 0;
            Latency = 0;
        }
    }
}

struct Statistics
{
    uint64_t ipackets;
    uint64_t opackets;
    uint64_t dpackets; // dropped packets
    uint64_t bytes;
    uint64_t cycles;
    uint64_t cycles_per_pkt;
    uint64_t flowNum;
} __rte_cache_aligned;

struct Statistics stats[MAX_QUEUE] = {0}, preStats[MAX_QUEUE] = {0}, curStats[MAX_QUEUE] = {0};
static uint64_t upper_bound = 0, lower_bound = 1000000000;
static double speed = 0.05; // Latency: 0.05为sockperf测试速率，单位Mpps Throughput: bps
static bool converge = true;
static void covergeSpeed(double tx, double rx, int queue_num)
{
    uint64_t tmp = 0;
    if (tx - rx < 2 && tx - rx > -2)
    {
        lower_bound = stats[0].cycles_per_pkt;
        if (lower_bound - upper_bound < 10)
        {
            upper_bound *= 0.98;
        }
    }
    else
    {
        upper_bound = stats[0].cycles_per_pkt;
        if (lower_bound - upper_bound < 10)
        {
            lower_bound *= 1.05;
        }
    }
    tmp = (upper_bound + lower_bound) / 2;
    for (int i = 0; i < queue_num; i++)
    {
        stats[i].cycles_per_pkt = tmp;
    }
    RTE_LOG(INFO, USER1, "[Converged Speed] upper_bound:%lu,lower_bound:%lu,cur:%lu\n", upper_bound, lower_bound, tmp);
}

static void covergeSpeedByCPU(double cpuUtil, int queue_num)
{
    uint64_t tmp = 0;
    if (cpuUtil < queue_num * 0.95)
    {
        lower_bound = stats[0].cycles_per_pkt;
        if (lower_bound - upper_bound < 10)
        {
            upper_bound *= 0.98;
        }
    }
    else
    {
        upper_bound = stats[0].cycles_per_pkt;
        if (lower_bound - upper_bound < 10)
        {
            lower_bound *= 1.05;
        }
    }
    tmp = (upper_bound + lower_bound) / 2;
    for (int i = 0; i < queue_num; i++)
    {
        stats[i].cycles_per_pkt = tmp;
    }
    RTE_LOG(INFO, USER1, "[Converged Speed] upper_bound:%lu,lower_bound:%lu,cur:%lu\n", upper_bound, lower_bound, tmp);
}
static void showTotal(int sleep_s, int queue_num)
{
    uint64_t cur = 0, prev = 0;
    while (!force_quit)
    {
        uint64_t opackets = 0, bytes = 0, ipackets = 0, _opackets = 0, _bytes = 0, _ipackets = 0;
        double cpuUtil = 0, _cpuUtil = 0;
        cur = rte_rdtsc();
        memcpy(curStats, stats, sizeof(stats));
        RTE_LOG(INFO, USER1, "******************************Thoughput Test************************************\n");
        double diff_ts = (double)(cur - prev) / (double)rte_get_tsc_hz();
        for (int i = 0; i < queue_num; i++)
        {
            _opackets = curStats[i].opackets - preStats[i].opackets;
            opackets += _opackets;

            _ipackets = curStats[i].ipackets - preStats[i].ipackets;
            ipackets += _ipackets;

            _bytes = curStats[i].bytes - preStats[i].bytes;
            bytes += _bytes;

            _cpuUtil = (curStats[i].cycles - preStats[i].cycles) / (double)(cur - prev);
            cpuUtil += _cpuUtil;
            RTE_LOG(INFO, USER1, "CPU %d : FlowNum %lu, TX %.2f Gbps, TX %.2f Mpps, RX %.2f Mpps, Util %.2f\n",
                    i,
		    curStats[i].flowNum,
                    _bytes * 8.0 / diff_ts / 1000000000,
                    _opackets * 1.0 / diff_ts / 1000000,
                    _ipackets * 1.0 / diff_ts / 1000000,
                    _cpuUtil);
        }
        RTE_LOG(INFO, USER1, "[Total] TX %.2f Gbps, TX %.2f Mpps, RX %.2f Mpps, CPU %.2f\n",
                bytes * 8.0 / diff_ts / 1000000000,
                opackets * 1.0 / diff_ts / 1000000,
                ipackets * 1.0 / diff_ts / 1000000,
                cpuUtil);

        // double tx_speed = opackets * 1.0 / diff_ts / 1000000, rx_speed = ipackets * 1.0 / diff_ts / 1000000;
        // covergeSpeed(tx_speed, rx_speed, queue_num);
        if (converge)
            covergeSpeedByCPU(cpuUtil, queue_num);

        prev = cur;
        memcpy(&preStats, &curStats, sizeof(stats));
        sleep(sleep_s);
    }
}
static uint64_t calRate(double rate, double pktlen) // Mbps
{
    double cpuHZ = rte_get_tsc_hz(), unit = 1000000.0; // 000;
    double res = (cpuHZ / unit * 8.0 * pktlen) / rate;
    uint64_t cycles_per_pkt = res;
    return cycles_per_pkt;
}

static void
worker(struct Statistics *counter, int RX_PORT, int TX_PORT, int queue_id, int dataLength, int burst, double rate, struct Flows *flows)
{
    uint64_t cur_tsc = 0, count = 0;
    int nb_tx, nb_rx;
    const uint64_t drain_tsc = (rte_get_tsc_hz() + US_PER_S - 1) / US_PER_S *
                               BURST_TX_DRAIN_US;
    struct rte_mbuf *TX_MBUFS[MAX_TX_BURST];
    int _dataLength;
    if (dataLength < 18)
        _dataLength = 84;
    else
        _dataLength = dataLength + 8 + 20 + 18 + 20;
    counter->cycles_per_pkt = calRate(rate, _dataLength * burst);
    uint64_t next_pkt_tmp = rte_rdtsc() + counter->cycles_per_pkt; // 开启限速计时器
    int lcore = rte_lcore_id();
    RTE_LOG(INFO, USER1, "Lcore %d start send packets......\n", lcore);

    struct Flows thisFlow;
    memcpy(&thisFlow, flows, sizeof(struct Flows));
    uint64_t cps_gap = rte_get_tsc_hz() / thisFlow.cps, cps_timer = rte_rdtsc() + cps_gap;
    thisFlow.flow_num = 1;

    while (!force_quit)
    {
        cur_tsc = rte_rdtsc();

        if (cur_tsc > cps_timer && thisFlow.flow_num < flows->flow_num)
        {
            // RTE_LOG(INFO, USER1, "Flow_num=%d\n", thisFlow.flow_num + 1);
            thisFlow.flow_num = thisFlow.flow_num + 1;
	    counter->flowNum=thisFlow.flow_num;
            cps_timer += cps_gap;
        }

        if (cur_tsc > next_pkt_tmp)
        {
            for (int i = 0; i < burst; i++)
            {
                thisFlow.src_ip = (count % thisFlow.flow_num) / 65536 + flows->src_ip;
                thisFlow.src_port = (count % thisFlow.flow_num + flows->src_port) % 65536;
                TX_MBUFS[i] = generateUDPMBuf(dataLength, &thisFlow, pktmbuf_pool[TX_PORT]);
                // generateVlanUDPMBuf(dataLength, count%10);
                count++;
                counter->bytes += (TX_MBUFS[i]->pkt_len + 24 < 84) ? 84 : TX_MBUFS[i]->pkt_len + 24;
            }
            nb_tx = rte_eth_tx_burst(TX_PORT, queue_id, TX_MBUFS, burst);
            if (unlikely(nb_tx < burst))
            {
                while (nb_tx < burst)
                {
                    nb_tx += rte_eth_tx_burst(TX_PORT, queue_id,
                                              &TX_MBUFS[nb_tx], burst - nb_tx);
                }
            }
            counter->opackets += burst;
            next_pkt_tmp += counter->cycles_per_pkt;
            counter->cycles += rte_rdtsc() - cur_tsc;
        }

        cur_tsc = rte_rdtsc();
        nb_rx = rte_eth_rx_burst(RX_PORT, queue_id, TX_MBUFS, burst);
        if (nb_rx)
        {
            for (int i = 0; i < nb_rx; i++)
            {
                // int pkt_len = TX_MBUFS[i]->pkt_len;
                // pkt_len = +24;
                // if (pkt_len < 84)
                // {
                //     pkt_len = 84;
                // }
                counter->ipackets++;
                rte_pktmbuf_free(TX_MBUFS[i]);
            }
            counter->cycles += rte_rdtsc() - cur_tsc;
        }
    }
}
static struct Flows flows = {0};
static int
launch_one_lcore(__rte_unused void *dummy)
{
    unsigned *cores = (unsigned *)dummy;
    if (flows.mode == 0)
        worker(&stats[*cores], RX_PORT, TX_PORT, *cores, flows.payload, 32, speed, &flows);
    if (flows.mode == 1)
        LAT_worker(RX_PORT, TX_PORT, *cores, flows.payload, 1, speed, 1, 0, &flows);
    if (flows.mode == 2)
    {
        if ((*cores) % 2)
            LAT_worker(RX_PORT, TX_PORT, (*cores) / 2, flows.payload, 1, speed, 1, 1, &flows);
        else
            LAT_worker(RX_PORT, TX_PORT, (*cores) / 2, flows.payload, 1, speed, 1, -1, &flows);
    }
    return 0;
}

static int start()
{
    unsigned lcore_id;
    unsigned cores = 0, core_num = 0;
    if (flows.mode == 2)
        core_num = queue_num * 2;
    else
        core_num = queue_num;
    RTE_LCORE_FOREACH_WORKER(lcore_id)
    {
        int ret = rte_eal_remote_launch(launch_one_lcore, &cores, lcore_id);
        if (ret == 0)
        {
            cores++;
            if (cores == core_num)
                break;
        }
    }
    if (flows.mode == 0)
        showTotal(1, queue_num);
    getchar();
    force_quit = true;
}
