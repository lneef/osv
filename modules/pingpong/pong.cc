#include <arpa/inet.h>
#include <bypass/mem.hh>
#include <bypass/time.hh>
#include <cstdint>
#include <cstdlib>

#include <api/bypass/mem.hh>
#include <api/bypass/dev.hh>
#include <bypass/defs.hh>
#include <endian.h>
#include <features.h>
#include <memory>
#include <getopt.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include "net.hh"
static constexpr uint32_t pbuf_sz = 2048;

#define SWAP(val1, val2) \
    do{             \
    auto temp = val1;  \
    val1 = val2; \
    val2 = temp; \
    }while(0);

using pool_ptr = std::unique_ptr<rte_pktmbuf_pool, decltype(std::addressof(rte_pktmbuf_pool::rte_pktmbuf_pool_delete))>;

struct port_config{
    pool_ptr recv_pool;
    rte_eth_dev *dev;
    uint64_t rt;
    uint16_t burst_size;

    port_config():
        recv_pool(rte_pktmbuf_pool::rte_pktmbuf_pool_create("recv", pbuf_sz, 0), &rte_pktmbuf_pool::rte_pktmbuf_pool_delete),
        rt(300), burst_size(1){}
};

static rte_eth_conf conf = {
    .rxmode = {.offloads = RTE_ETH_RX_OFFLOAD_CHECKSUM},
    .txmode = {.offloads = RTE_ETH_TX_OFFLOAD_UDP_CKSUM | RTE_ETH_TX_OFFLOAD_IPV4_CKSUM}
};


static void configure_port(port_config& pconf){
    uint32_t nb_desc = 1024;
    rte_eth_dev_info info{};
    struct rte_eth_rxconf rxconf{};
    struct rte_eth_txconf txconf{};
    pconf.dev = eth_os::get_eth_for_port(0);
    pconf.dev->get_dev_info(&info);
    printf("UDP: %u, IP %u\n", info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_IPV4_CKSUM, info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_UDP_CKSUM);
    pconf.dev->dev_configure(1, 1, &conf);
    rxconf.offloads |= RTE_ETH_RX_OFFLOAD_CHECKSUM;
    txconf.offloads |= RTE_ETH_TX_OFFLOAD_UDP_CKSUM | RTE_ETH_TX_OFFLOAD_IPV4_CKSUM;
    pconf.dev->rx_queue_setup(0, nb_desc, 0, &rxconf, pconf.recv_pool.get());
    pconf.dev->tx_queue_setup(0, nb_desc, 0, &txconf);
    pconf.dev->start();
}

static void close_port(port_config& pconf){
    pconf.dev->stop();
}


static int receive_packets(rte_mbuf* pkt){
    if(!verify_packet(pkt))
            return -1;
    eth_header* eth = reinterpret_cast<eth_header*>(pkt->buf);
    ipv4_header* ipv4 = reinterpret_cast<ipv4_header*>(eth + 1);
    udp_header* udp = reinterpret_cast<udp_header*>(ipv4 + 1);
    std::swap(eth->dst, eth->src);
    SWAP(ipv4->dst_addr, ipv4->src_addr);
    SWAP(udp->dst_port, udp->src_port);
    udp->dgram_cksum = 0;
    ipv4->hdr_checksum = 0;
    ipv4->time_to_live = TTL;
    pkt->l2_len = sizeof(*eth);
    pkt->l3_len = sizeof(*ipv4);
    pkt->l4_len = sizeof(*udp);
    pkt->ol_flags = RTE_MBUF_F_TX_UDP_CKSUM | RTE_MBUF_F_TX_IPV4 | RTE_MBUF_F_TX_IP_CKSUM;
    udp->dgram_cksum = phdr_cksum(ipv4, udp);
    for(auto i = 0; i < pkt->data_len; ++i){
        printf("%02x", pkt->buf[i]);
    }
    printf("\n");
    return 0;
}

static void do_pong(port_config &pconf){
    uint16_t nb_rx = 0, burst_size = pconf.burst_size;
    uint16_t nb_tx = burst_size;
    uint16_t nb_rm = 0;
    std::vector<rte_mbuf*> pkts(burst_size, nullptr);
    std::vector<rte_mbuf*> rpkts(burst_size, nullptr);
    auto cycles = rte_get_timer_cycles();
    auto end = cycles + pconf.rt * 1e9;
    for(; rte_get_timer_cycles() < end; ){
        nb_rx = pconf.dev->rx_burst(0, rpkts.data(), burst_size - nb_rm); 
        for(uint16_t i = 0; i < nb_rx ; ++i){
            pkts[nb_rm] = rpkts[i];
            if(!receive_packets(pkts[nb_rm]))
                ++nb_rm;
        }
        nb_tx = pconf.dev->tx_burst(0, pkts.data(), nb_rm);
        for(uint16_t j = 0, i = nb_tx; i < nb_rm; ++i, ++j)
            pkts[j] = pkts[i];
        nb_rm = nb_rm - nb_tx;
    }
}


int main(int argc, char* argv[]){
    port_config pconf;
int opt, option_index;
    static const struct option long_options[] = {
      {"bs", required_argument, 0, 0},
      {"rt", required_argument, 0, 0},
      {0, 0, 0, 0}};
  while ((opt = getopt_long(argc, argv, "", long_options, &option_index)) !=
         -1) {
    if (opt == '?')
      continue;
    switch (option_index) {
    case 0:
      pconf.burst_size = atoi(optarg);
      break;
    case 1:
      pconf.rt = atoi(optarg);
      break;
    default:
      break;
    }
  }
  configure_port(pconf);
  do_pong(pconf);
  close_port(pconf);
    return 0;

}
