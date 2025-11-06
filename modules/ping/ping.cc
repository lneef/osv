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
#include <vector>

static constexpr uint32_t pbuf_sz = 2048;

using pool_ptr = std::unique_ptr<pbuf_pool, decltype(std::addressof(pbuf_pool::pbuf_pool_delete))>;

struct payload{
    uint64_t ticks;
};

template<typename T>
static __inline T pun(pkt_buf *pbuf){
    char *data = pbuf->buf + sizeof(ipv4_header) + sizeof(udp_header) + sizeof(eth_header); 
    T ret_data;
    std::memcpy(&ret_data, data, sizeof(T));
    return ret_data;
}

template<typename T>
static __inline void move_data(pkt_buf *pbuf, T& data){
    char *data_ptr = pbuf->buf + sizeof(ipv4_header) + sizeof(udp_header) + sizeof(eth_header);
    memcpy(data_ptr, &data, sizeof(T));
}

struct port_config{
    pool_ptr send_pool, recv_pool;
    app_config app;
    eth_dev *dev;
    uint64_t rt;
    uint64_t ticks;
    uint64_t pkts;

    port_config():send_pool(pbuf_pool::bpuf_pool_create("send", pbuf_sz, 0), &pbuf_pool::pbuf_pool_delete),
        recv_pool(pbuf_pool::bpuf_pool_create("recv", pbuf_sz, 0), &pbuf_pool::pbuf_pool_delete) {}
};

static void configure_port(port_config& pconf){
    uint32_t nb_desc = 1024;
    pconf.dev = eth_dev::get_eth_for_port(0);
    pconf.dev->setup_device(1, 1);
    pconf.dev->setup_rx_queue(0, nb_desc, 0, PBUF_OFFLOAD_UDP_CKSUM | PBUF_OFFLOAD_IPV4_CKSUM, pconf.send_pool.get());
    pconf.dev->setup_tx_queue(0, nb_desc, 0, PBUF_OFFLOAD_UDP_CKSUM | PBUF_OFFLOAD_IPV4_CKSUM);
    pconf.dev->get_mac_addr(pconf.app.src.addr.data());
    pconf.dev->eth_start();
}

static void close_port(port_config& pconf){
    pconf.dev->eth_stop();
}

static void init_packets(const std::vector<pkt_buf*>& pkts){
    payload payload;
    payload = {static_cast<uint64_t>(get_ns())};
    for(auto* pkt: pkts)
        move_data(pkt, payload);
 
}
static uint16_t receive_packets(port_config& pconf, std::vector<pkt_buf*>& pkts, uint16_t nb_rx){
    uint16_t total = 0;
    auto ticks = get_ns();
    for(uint16_t i = 0; i < nb_rx; ++i){
        if(!verify_packet(pkts[i]))
            continue;;
        auto pticks = pun<payload>(pkts[i]);
        pconf.ticks += ticks - pticks.ticks;
        ++pconf.pkts;
        ++total;
        pconf.recv_pool->free_bulk(pkts.data(), nb_rx);
    }
    return total;
}

static void do_ping(port_config &pconf){
    uint16_t nb_rx = 0, burst_size = 1, total = 0;
    uint16_t nb_tx = burst_size;
    std::vector<pkt_buf*> pkts(burst_size, nullptr);
    std::vector<pkt_buf*> rpkts(burst_size, nullptr);
    auto cycles_per_it = 1e9;  
    auto cycles = get_ns();
    auto end = cycles + pconf.rt * 1e9;
    for(; get_ns() < end; ){
        if(nb_tx){
            if(pconf.send_pool->alloc_bulk(pkts.data(), nb_tx))
                continue;
            for(uint16_t i = 0; i < nb_tx; ++i)
                create_packet(pconf.app, pkts[i]);
        }
        init_packets(pkts);
        nb_tx = pconf.dev->submit_pkts_burst(0, pkts.data(), burst_size);
        if(!nb_tx)
            continue;
        cycles = cycles_per_it;
        total = 0;
        do{
            nb_rx = pconf.dev->retrieve_pkts_burst(0, rpkts.data(), burst_size);
            if(nb_rx)
                total += receive_packets(pconf, rpkts, nb_rx);
        }while(total < nb_tx && get_ns() < cycles);

    }
}

int main(int argc, char* argv[]){
    port_config pconf{};
    pconf.app.data_len = sizeof(payload);
int opt, option_index;
    static const struct option long_options[] = {
      {"l4port", required_argument, 0, 0},
      {"dip", required_argument, 0, 0},
      {"sip", required_argument, 0, 0},
      {"dmac", required_argument, 0, 0},
      {"rt", required_argument, 0, 0},
      {0, 0, 0, 0}};
  while ((opt = getopt_long(argc, argv, "", long_options, &option_index)) !=
         -1) {
    if (opt == '?')
      continue;
    switch (option_index) {
    case 0:
      pconf.app.l4port = htobe16(atoi(optarg));
      break;
    case 1:
      pconf.app.dip = inet_addr(optarg);
      break;
    case 2:
      pconf.app.sip = inet_addr(optarg);
      break;
    case 3:
      pconf.app.dst.parse_string(optarg);
      break;
    case 4:
      pconf.rt = atol(optarg);
      break;
    default:
      break;
    }
  }
    configure_port(pconf);
    do_ping(pconf);
    printf("Average latency: %.2f\n", static_cast<double>(pconf.ticks) / 1e3 / pconf.pkts);
    close_port(pconf);
    return 0;

}
