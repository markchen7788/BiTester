/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2017 Mellanox Technologies, Ltd
 */
struct rte_flow *
generate_lat_flow(uint16_t port_id, uint16_t rx_q, uint8_t tos, struct rte_flow_error *error, uint16_t SRC_PORT, uint16_t DST_PORT)
{
    struct rte_flow_attr attr;
    struct rte_flow_item pattern[4];
    struct rte_flow_action action[2];
    struct rte_flow *flow = NULL;
    struct rte_flow_action_queue queue = {.index = rx_q};
    struct rte_flow_item_ipv4 ipv4_spec;
    struct rte_flow_item_ipv4 ipv4_mask;
    struct rte_flow_item_udp udp_spec;
    struct rte_flow_item_udp udp_mask = {
        .hdr.dst_port = RTE_BE16(0xffff),
        .hdr.src_port = RTE_BE16(0xffff),
    };

    int res;
    memset(pattern, 0, 4 * sizeof(struct rte_flow_item));
    memset(action, 0, sizeof(action));
    memset(&attr, 0, sizeof(struct rte_flow_attr));
    memset(&ipv4_spec, 0, sizeof(struct rte_flow_item_ipv4));
    memset(&ipv4_mask, 0, sizeof(struct rte_flow_item_ipv4));
    attr.ingress = 1;
    action[0].type = RTE_FLOW_ACTION_TYPE_QUEUE;
    action[0].conf = &queue;
    action[1].type = RTE_FLOW_ACTION_TYPE_END;

    pattern[0].type = RTE_FLOW_ITEM_TYPE_ETH;
    pattern[1].type = RTE_FLOW_ITEM_TYPE_IPV4;
    ipv4_spec.hdr.type_of_service = tos;
    ipv4_mask.hdr.type_of_service = 0xff;
    pattern[1].spec = &ipv4_spec;
    pattern[1].mask = &ipv4_mask;
    pattern[2].type = RTE_FLOW_ITEM_TYPE_UDP;
    /*-------------------------------*/

    // udp_spec.hdr.dst_port = RTE_BE16(DST_PORT);
    // udp_spec.hdr.src_port = RTE_BE16(SRC_PORT);
    // pattern[2].spec = &udp_spec;
    // pattern[2].last = NULL;
    // pattern[2].mask = &udp_mask;

    /*-------------------------------*/
    pattern[3].type = RTE_FLOW_ITEM_TYPE_END;

    res = rte_flow_validate(port_id, &attr, pattern, action, error);

    if (!res)
        flow = rte_flow_create(port_id, &attr, pattern, action, error);

    return flow;
}
struct rte_flow *
generate_drop_other_flow(uint16_t port_id, uint16_t rx_q, struct rte_flow_error *error)
{
    struct rte_flow_attr attr;
    struct rte_flow_item pattern[2];
    struct rte_flow_action action[2];
    struct rte_flow *flow = NULL;

    int res;
    memset(pattern, 0, sizeof(pattern));
    memset(action, 0, sizeof(action));
    memset(&attr, 0, sizeof(struct rte_flow_attr));
    attr.ingress = 1;
    action[0].type = RTE_FLOW_ACTION_TYPE_DROP;
    action[1].type = RTE_FLOW_ACTION_TYPE_END;

    pattern[0].type = RTE_FLOW_ITEM_TYPE_ETH;
    pattern[1].type = RTE_FLOW_ITEM_TYPE_END;

    res = rte_flow_validate(port_id, &attr, pattern, action, error);

    if (!res)
        flow = rte_flow_create(port_id, &attr, pattern, action, error);

    return flow;
}
static void addRteFlow()
{
    //////////////////////////////////////////////////////flow_table
    struct rte_flow_error error;
    struct rte_flow *flow;
    int ff = rte_flow_flush(RX_PORT, &error);
    flow = generate_lat_flow(RX_PORT, 0, 0x20, &error, 7788, 7788);
    if (!flow)
    {
        rte_exit(EXIT_FAILURE, "error in creating flow,Flow can't be created %d message: %s\n", error.type, error.message ? error.message : "(no stated reason)");
    }
    flow = generate_drop_other_flow(RX_PORT, 0, &error);
    if (!flow)
    {
        rte_exit(EXIT_FAILURE, "error in creating flow,Flow can't be created %d message: %s\n", error.type, error.message ? error.message : "(no stated reason)");
    }
}
