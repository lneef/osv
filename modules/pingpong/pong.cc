#include "net.hh"
#include "osv/clock.hh"
#include <arpa/inet.h>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <api/bypass/net_eth.hh>
#include <api/bypass/net_bypass.h>
#include <api/bypass/pktbuf.hh>
#include <endian.h>
#include <features.h>
#include <memory>
#include <getopt.h>
#include <unistd.h>
#include <utility>
#include <vector>

static constexpr uint32_t pbuf_sz = 2048;

#define SWAP(val1, val2) \
    do{             \
    auto temp = val1;  \
    val1 = val2; \
    val2 = temp; \
    }while(0);

using pool_ptr = std::unique_ptr<pbuf_pool, decltype(std::addressof(pbuf_pool::pbuf_pool_delete))>;

struct port_config{
    pool_ptr recv_pool;
    eth_dev *dev;
    uint64_t rt;
    uint16_t burst_size;

    port_config():
        recv_pool(pbuf_pool::bpuf_pool_create("recv", pbuf_sz, 0), &pbuf_pool::pbuf_pool_delete),
        rt(300), burst_size(1){}
};

static void configure_port(port_config& pconf){
    uint32_t nb_desc = 1024;
    pconf.dev = eth_dev::get_eth_for_port(0);
    pconf.dev->setup_device(1, 1);
    pconf.dev->setup_rx_queue(0, nb_desc, 0, PBUF_OFFLOAD_UDP_CKSUM | PBUF_OFFLOAD_IPV4_CKSUM, pconf.recv_pool.get());
    pconf.dev->setup_tx_queue(0, nb_desc, 0, PBUF_OFFLOAD_UDP_CKSUM | PBUF_OFFLOAD_IPV4_CKSUM);
    pconf.dev->eth_start();
}

static void close_port(port_config& pconf){
    pconf.dev->eth_stop();
}


static int receive_packets(pkt_buf* pkt){
    if(pkt->olflags & (PBUF_CKSUM_L4_BAD | PBUF_CKSUM_L3_BAD))
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
    pkt->olflags = PBUF_OFFLOAD_UDP_CKSUM | PBUF_OFFLOAD_IPV4_CKSUM;
    udp->dgram_cksum = phdr_cksum(ipv4, udp);
    return 0;
}

static void do_pong(port_config &pconf){
    uint16_t nb_rx = 0, burst_size = pconf.burst_size;
    uint16_t nb_tx = burst_size;
    uint16_t nb_rm = 0;
    std::vector<pkt_buf*> pkts(burst_size, nullptr);
    std::vector<pkt_buf*> rpkts(burst_size, nullptr);
    auto cycles = get_ns();
    auto end = cycles + pconf.rt * 1e9;
    for(; get_ns() < end; ){
        nb_rx = pconf.dev->retrieve_pkts_burst(0, rpkts.data(), burst_size - nb_rm); 
        for(uint16_t i = 0; i < nb_rx ; ++i, ++nb_rm){
            pkts[nb_rm] = rpkts[i];
            if(receive_packets(pkts[nb_rm]))
                --nb_rm;
        }
        nb_tx = pconf.dev->submit_pkts_burst(0, pkts.data(), nb_rm);
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
