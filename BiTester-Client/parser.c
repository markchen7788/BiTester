#include <cmdline_rdline.h>
#include <cmdline_parse.h>
#include <cmdline_parse_ipaddr.h>
#include <cmdline_parse_num.h>
#include <cmdline_parse_string.h>
#include <cmdline.h>
struct Flows
{
    struct rte_ether_addr src_addr;
    struct rte_ether_addr dst_addr;
    uint32_t src_ip;
    uint32_t dst_ip;
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t flow_num;
    uint32_t cps;
    uint32_t payload;
    uint32_t mode; // 0: throughput test,1: latency test
} __rte_cache_aligned;

void delEnter(char *p)
{
    if (p[strlen(p) - 1] == '\n')
    {
        p[strlen(p) - 1] = '\0';
    }
}

uint32_t strToIP(char *str)
{
    char *p = strtok(str, ".");
    int ip[4] = {0};
    int i = 0;
    while (p)
    {
        /* code */
        ip[i] = atoi(p);
        if (ip[i] > 255)
        {
            ip[i] = 255;
        }
        p = strtok(NULL, ".");
        i++;
    }
    return RTE_IPV4(ip[0], ip[1], ip[2], ip[3]);
}

static void
parse_eth_dest(const char *optarg, struct rte_ether_addr *dst_mac)
{
    uint8_t *dest, peer_addr[6];

    if (cmdline_parse_etheraddr(NULL, optarg,
                                &peer_addr, sizeof(peer_addr)) < 0)
        rte_exit(EXIT_FAILURE,
                 "Invalid ethernet address: %s\n",
                 optarg);
    dest = (uint8_t *)dst_mac;
    for (int c = 0; c < 6; c++)
        dest[c] = peer_addr[c];
}

void printEth(struct rte_ether_addr *dst_mac)
{
    RTE_LOG(INFO, USER1, "%02X:%02X:%02X:%02X:%02X:%02X",
            dst_mac->addr_bytes[0],
            dst_mac->addr_bytes[1],
            dst_mac->addr_bytes[2],
            dst_mac->addr_bytes[3],
            dst_mac->addr_bytes[4],
            dst_mac->addr_bytes[5]);
}

void parser(struct Flows *flow, char *fileName)
{
    FILE *fp = NULL;
    fp = fopen(fileName, "r");
    size_t bufsize = 100;
    char *buf;
    buf = (char *)malloc(bufsize * sizeof(char));
    while (getline(&buf, &bufsize, fp) != EOF)
    {
        char *p = strtok(buf, " ");
        delEnter(p);

        if (strcmp(p, "src_mac") == 0)
        {
            p = strtok(NULL, " ");
            delEnter(p);
            parse_eth_dest(p, &(flow->src_addr));
        }
        if (strcmp(p, "dst_mac") == 0)
        {
            p = strtok(NULL, " ");
            delEnter(p);
            parse_eth_dest(p, &(flow->dst_addr));
        }
        if (strcmp(p, "src_ip") == 0)
        {
            p = strtok(NULL, " ");
            delEnter(p);
            flow->src_ip = strToIP(p);
        }
        if (strcmp(p, "dst_ip") == 0)
        {
            p = strtok(NULL, " ");
            delEnter(p);
            flow->dst_ip = strToIP(p);
        }

        if (strcmp(p, "src_port") == 0)
        {
            p = strtok(NULL, " ");
            delEnter(p);
            flow->src_port = atoi(p);
        }

        if (strcmp(p, "dst_port") == 0)
        {
            p = strtok(NULL, " ");
            delEnter(p);
            flow->dst_port = atoi(p);
        }
        if (strcmp(p, "flow_num") == 0)
        {
            p = strtok(NULL, " ");
            delEnter(p);
            flow->flow_num = atoi(p);
        }
        if (strcmp(p, "cps") == 0)
        {
            p = strtok(NULL, " ");
            delEnter(p);
            flow->cps = atoi(p);
        }
        if (strcmp(p, "payload") == 0)
        {
            p = strtok(NULL, " ");
            delEnter(p);
            flow->payload = atoi(p);
        }
        if (strcmp(p, "mode") == 0)
        {
            p = strtok(NULL, " ");
            delEnter(p);
            flow->mode = atoi(p);
        }
    }

    fclose(fp);
}