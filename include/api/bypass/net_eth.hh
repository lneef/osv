#ifndef NET_ETH_DEF_H
#define NET_ETH_DEF_H
#include <cstdint>
#include "api/bypass/pktbuf.hh"

#define MAX_ETH_PORTS 16

struct eth_dev;

struct eth_os{
    struct {
        int configured;
        eth_dev* dev;
    }ports_info[MAX_ETH_PORTS];

    void register_port(uint16_t port, eth_dev* dev);
};

extern eth_os ports;

enum offload : uint64_t{
    ETH_OFFLOAD_NONE = 0,
    ETH_OFFLOAD_IPV4_CKSUM = 1,
    ETH_OFFLOAD_TCP_CKSUM = 2,
    ETH_OFFLOAD_ETH_CKSUM = 4,
    ETH_OFFLOAD_RSS_HASH = 8,
};

struct eth_dev_info;

struct eth_dev{
    uint16_t nb_rx_queue, nb_tx_queue;
    uint16_t mtu;
    void* adapter;

    eth_dev(void* adapter): nb_rx_queue(0), nb_tx_queue(0), adapter(adapter) {}

    static eth_dev* get_eth_for_port(uint16_t port);
    virtual int submit_pkts_burst(uint16_t qid, pkt_buf **pkts, uint16_t nb) = 0;
    virtual int retrieve_pkts_burst(uint16_t qid, pkt_buf **pkts, uint16_t nb) = 0;

    virtual int setup_device(uint16_t nb_tx, uint16_t nb_rx) = 0;
    virtual int setup_tx_queue(uint16_t qid, uint16_t nb_desc, unsigned int socket, uint64_t offload) = 0;
    virtual int setup_rx_queue(uint16_t qid, uint16_t nb_desc, unsigned int socket, uint64_t offloads, pbuf_pool *pool) = 0;

    virtual int eth_start() = 0;
    virtual void eth_stop() = 0;

    virtual void get_dev_info(eth_dev_info& dev_info) = 0;

    virtual void get_mac_addr(char* addr) = 0;

};

struct eth_dev_info{
    uint64_t tx_offload_capa, rx_offload_capa;
};

#endif
