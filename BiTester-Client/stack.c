struct LatPerf_hdr
{
    /* data */
    uint64_t TXCycles;
    uint64_t psn;
    uint32_t length;
}; // 16B

static void
fill_ethernet_header(struct rte_ether_hdr *hdr, struct rte_ether_addr *src_mac, struct rte_ether_addr *dst_mac) // 14byte
{
    rte_ether_addr_copy(dst_mac, &hdr->d_addr);
    rte_ether_addr_copy(src_mac, &hdr->s_addr);
    hdr->ether_type = rte_cpu_to_be_16(0x0800);
}

static void
fill_ethernetAndVlan_header(struct rte_ether_hdr *hdr, struct rte_vlan_hdr *vlan, struct rte_ether_addr *src_mac, struct rte_ether_addr *dst_mac, uint16_t vlan_tci) // 14byte
{
    rte_ether_addr_copy(dst_mac, &hdr->d_addr);
    rte_ether_addr_copy(src_mac, &hdr->s_addr);
    hdr->ether_type = rte_cpu_to_be_16(0x8100); // vlan
    vlan->vlan_tci = rte_cpu_to_be_16(vlan_tci);
    vlan->eth_proto = rte_cpu_to_be_16(0x0800); // IP
}

void setVlan(struct rte_mbuf *m, uint16_t vlan_tci)
{
    struct rte_vlan_hdr *vlan = rte_pktmbuf_mtod_offset(m, void *, sizeof(struct rte_ether_hdr));
    vlan->vlan_tci = rte_cpu_to_be_16(vlan_tci);
}

static void
fill_ipv4_header(struct rte_ipv4_hdr *hdr, uint32_t src_ip, uint32_t dst_ip, int tos, int length) // 20byte
{
    hdr->version_ihl = 0x45;                      // ipv4, length 5 (*4)
    hdr->type_of_service = tos;                   // No Diffserv
    hdr->total_length = rte_cpu_to_be_16(length); // ip+udp+payload 20
    hdr->packet_id = 0;                           // rte_cpu_to_be_16(seqnum); // set random
    hdr->fragment_offset = rte_cpu_to_be_16(0);
    hdr->time_to_live = 64;
    hdr->next_proto_id = 17; // udp
    hdr->src_addr = rte_cpu_to_be_32(src_ip);
    hdr->dst_addr = rte_cpu_to_be_32(dst_ip);

    hdr->hdr_checksum = 0; // 一定提请提前设置为0,不然内核协议栈不能解析
    // hdr->hdr_checksum = rte_ipv4_cksum(hdr);
}
static void
fill_udp_header(struct rte_ipv4_hdr *hdr1, struct rte_udp_hdr *hdr, int sport, int dport, int length)
{                                            // 8byte
    hdr->src_port = rte_cpu_to_be_16(sport); // 7788+seqnum%3);
    hdr->dst_port = rte_cpu_to_be_16(dport);
    hdr->dgram_len = rte_cpu_to_be_16(length);
    hdr->dgram_cksum = 0;
    // hdr->dgram_cksum = rte_ipv4_udptcp_cksum(hdr1, hdr);
}

static void fill_latPerf_hdr(struct LatPerf_hdr *hdr, uint64_t psn, int length)
{
    hdr->psn = psn;
    // hdr->TxCylces = rte_rdtsc();
    hdr->length = length;
}

static void
fill_payload(char *buf, int length)
{
    for (int i = 0; i < length; i++)
    {
        buf[i] = i % 256;
    }
}

uint64_t getLat(struct rte_mbuf *m)
{
    struct rte_ether_hdr *ether_h;
    struct rte_ipv4_hdr *ipv4_h;
    ether_h = rte_pktmbuf_mtod_offset(m, void *, 0);
    uint16_t IPV4 = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);
    if (likely(ether_h->ether_type == IPV4))
    {
        ipv4_h = rte_pktmbuf_mtod_offset(m, void *, sizeof(struct rte_ether_hdr));
        if (likely(ipv4_h->type_of_service == 0x20))
        {
            struct LatPerf_hdr *lat_hdr = rte_pktmbuf_mtod_offset(m, void *, sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr) + sizeof(struct rte_udp_hdr));
            return rte_rdtsc() - lat_hdr->TXCycles;
        }
    }
    return 0;
}
void setLat(struct rte_mbuf *m)
{
    struct LatPerf_hdr *lat_hdr = rte_pktmbuf_mtod_offset(m, void *, sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr) + sizeof(struct rte_udp_hdr));
    lat_hdr->TXCycles = rte_rdtsc();
}

static struct rte_mbuf *
generateLatPerfMBuf(uint64_t seq, int DATA_LENGTH, struct Flows *flow, struct rte_mempool *l2fwd_pktmbuf_pool)
{
    struct rte_mbuf *m;

    struct rte_ether_hdr *ether_h;
    struct rte_ipv4_hdr *ipv4_h;
    struct rte_udp_hdr *udp_h;
    struct LatPerf_hdr *lat_hdr;
    char *pay_h; // data payload
    //////////////////////////////////////////////////////
    m = rte_pktmbuf_alloc(l2fwd_pktmbuf_pool);
    ether_h = (struct rte_ether_hdr *)rte_pktmbuf_append(m, sizeof(struct rte_ether_hdr));
    ipv4_h = (struct rte_ipv4_hdr *)rte_pktmbuf_append(m, sizeof(struct rte_ipv4_hdr));
    udp_h = (struct rte_udp_hdr *)rte_pktmbuf_append(m, sizeof(struct rte_udp_hdr));
    lat_hdr = (struct LatPerf_hdr *)rte_pktmbuf_append(m, sizeof(struct LatPerf_hdr));
    pay_h = (char *)rte_pktmbuf_append(m, DATA_LENGTH);

    m->l2_len = sizeof(struct rte_ether_hdr);
    m->l3_len = sizeof(struct rte_ipv4_hdr);
    m->ol_flags |= PKT_TX_IPV4 | PKT_TX_IP_CKSUM | PKT_TX_UDP_CKSUM; // offload checksum

    fill_payload(pay_h, DATA_LENGTH);
    fill_latPerf_hdr(lat_hdr, seq, DATA_LENGTH + sizeof(struct LatPerf_hdr));
    fill_udp_header(ipv4_h, udp_h, flow->src_port, flow->dst_port, DATA_LENGTH + sizeof(struct LatPerf_hdr) + 8);
    fill_ipv4_header(ipv4_h, flow->src_ip, flow->dst_ip, 0x20, DATA_LENGTH + sizeof(struct LatPerf_hdr) + 8 + 20);
    fill_ethernet_header(ether_h, &(flow->src_addr), &(flow->dst_addr));
    return m;
}

static struct rte_mbuf *
generateUDPMBuf(int DATA_LENGTH, struct Flows *flow, struct rte_mempool *l2fwd_pktmbuf_pool)
{
    struct rte_mbuf *m;

    struct rte_ether_hdr *ether_h;
    struct rte_ipv4_hdr *ipv4_h;
    struct rte_udp_hdr *udp_h;
    char *pay_h; // data payload
    //////////////////////////////////////////////////////
    m = rte_pktmbuf_alloc(l2fwd_pktmbuf_pool);
    ether_h = (struct rte_ether_hdr *)rte_pktmbuf_append(m, sizeof(struct rte_ether_hdr));
    ipv4_h = (struct rte_ipv4_hdr *)rte_pktmbuf_append(m, sizeof(struct rte_ipv4_hdr));
    udp_h = (struct rte_udp_hdr *)rte_pktmbuf_append(m, sizeof(struct rte_udp_hdr));
    pay_h = (char *)rte_pktmbuf_append(m, DATA_LENGTH);

    m->l2_len = sizeof(struct rte_ether_hdr);
    m->l3_len = sizeof(struct rte_ipv4_hdr);
    m->ol_flags |= PKT_TX_IPV4 | PKT_TX_IP_CKSUM | PKT_TX_UDP_CKSUM; // offload checksum

    //    fill_payload(pay_h, DATA_LENGTH);
    fill_udp_header(ipv4_h, udp_h, flow->src_port, flow->dst_port, DATA_LENGTH + 8);
    fill_ipv4_header(ipv4_h, flow->src_ip, flow->dst_ip, 0x00, DATA_LENGTH + 8 + 20);
    fill_ethernet_header(ether_h, &(flow->src_addr), &(flow->dst_addr));
    return m;
}

static struct rte_mbuf *
generateVlanUDPMBuf(int DATA_LENGTH, int flows, struct rte_mempool *l2fwd_pktmbuf_pool)
{
    struct rte_mbuf *m;

    struct rte_ether_hdr *ether_h;
    struct rte_vlan_hdr *vlan_h;
    struct rte_ipv4_hdr *ipv4_h;
    struct rte_udp_hdr *udp_h;
    char *pay_h;                                                                                                              // data payload
    struct rte_ether_addr src_mac = {{0x08, 0xc0, 0xeb, 0xbf, 0xef, 0x9b}}, dst_mac = {{0x08, 0xc0, 0xeb, 0xbf, 0xef, 0x83}}; // 08:c0:eb:bf:ef:83
    uint32_t src_ip = RTE_IPV4(192, 168, 203, 2), dst_ip = RTE_IPV4(192, 168, 203, 1);
    int sport = (7788 + flows) % 65536, dport = 7788;
    //////////////////////////////////////////////////////
    m = rte_pktmbuf_alloc(l2fwd_pktmbuf_pool);
    ether_h = (struct rte_ether_hdr *)rte_pktmbuf_append(m, sizeof(struct rte_ether_hdr));
    vlan_h = (struct rte_vlan_hdr *)rte_pktmbuf_append(m, sizeof(struct rte_vlan_hdr));
    ipv4_h = (struct rte_ipv4_hdr *)rte_pktmbuf_append(m, sizeof(struct rte_ipv4_hdr));
    udp_h = (struct rte_udp_hdr *)rte_pktmbuf_append(m, sizeof(struct rte_udp_hdr));
    pay_h = (char *)rte_pktmbuf_append(m, DATA_LENGTH);

    m->l2_len = sizeof(struct rte_ether_hdr);
    m->l3_len = sizeof(struct rte_ipv4_hdr);
    m->ol_flags |= PKT_TX_IPV4 | PKT_TX_IP_CKSUM | PKT_TX_UDP_CKSUM; // offload checksum

    fill_payload(pay_h, DATA_LENGTH);
    fill_udp_header(ipv4_h, udp_h, sport, dport, DATA_LENGTH + 8);
    fill_ipv4_header(ipv4_h, src_ip, dst_ip, 0x00, DATA_LENGTH + 8 + 20);
    fill_ethernetAndVlan_header(ether_h, vlan_h, &src_mac, &dst_mac, 0x0002);
    return m;
}
