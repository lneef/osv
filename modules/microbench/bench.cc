#include <algorithm>
#include <arpa/inet.h>
#include <cassert>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <minidpdk/mem.hh>
#include <minidpdk/net.hh>
#include <minidpdk/time.hh>
#include <minidpdk/util.hh>

#include "net.hh"
#include <algorithm>
#include <api/minidpdk/dev.hh>
#include <api/minidpdk/mem.hh>
#include <cstring>
#include <endian.h>
#include <features.h>
#include <getopt.h>
#include <iostream>
#include <memory>
#include <minidpdk/defs.hh>
#include <minidpdk/lcore.hh>
#include <ostream>
#include <osv/sched.hh>
#include <sched.h>
#include <string>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

#define SWAP(val1, val2)                                                       \
  do {                                                                         \
    auto temp = val1;                                                          \
    val1 = val2;                                                               \
    val2 = temp;                                                               \
  } while (0);

using pkt_t = rte_mbuf;
struct payload {
  uint64_t ticks;
};

static volatile int terminate = 0;
static void handler(int sig) {
  (void)sig;
  terminate = 1;
}

template <typename T> static __inline T pun(rte_mbuf *pbuf) {
  char *data = rte_pktmbuf_mtod(pbuf, char *) + sizeof(rte_ipv4_hdr) +
               sizeof(rte_udp_hdr) + sizeof(rte_ether_hdr);
  T ret_data;
  std::memcpy(&ret_data, data, sizeof(T));
  return ret_data;
}

template <typename T> static __inline void move_data(rte_mbuf *pbuf, T &data) {
  char *data_ptr = rte_pktmbuf_mtod(pbuf, char *) + sizeof(rte_ipv4_hdr) +
                   sizeof(rte_udp_hdr) + sizeof(rte_ether_hdr);
  memcpy(data_ptr, &data, sizeof(T));
}

template <typename T> static __inline void prefetch(rte_mbuf *pbuf, T &data) {
  rte_prefetch0_write(rte_pktmbuf_mtod(pbuf, char *) + sizeof(rte_ipv4_hdr) +
                      sizeof(rte_udp_hdr) + sizeof(rte_ether_hdr));
}

using pool_ptr = std::unique_ptr<rte_pktmbuf_pool, decltype(&rte_mempool_free)>;

enum class opmode { PING, PONG, RECEIVE };

// Configuration parsed from the command line / shared across all lcores.
struct benchmark_config {
  app_config app;
  uint64_t rt = 30;
  uint16_t burst_size = 1;
  uint16_t nb_cores = 1;
  opmode role = opmode::PONG;
};

// Offload capabilities of the underlying device.
struct capabilities {
  bool ip_cksum_tx = false, ip_cksum_rx = false;
  bool l4_cksum_tx = false, l4_cksum_rx = false;
};

// Per-lcore / per-queue state. With one queue per lcore, each block owns a
// single rx/tx queue and its own mempool plus running counters.
struct thread_block {
  uint16_t rx_queue = 0;
  uint16_t tx_queue = 0;
  pool_ptr pool;
  uint64_t ticks = 0, pkts = 0, faulty = 0;
  thread_block() : pool(nullptr, &rte_mempool_free) {}
};

// Per-port device state shared by all lcores driving the port.
struct port_info {
  uint16_t port_id = 0;
  rte_eth_dev *dev = nullptr;
  capabilities caps;
  rte_ether_addr addr{};
  std::vector<thread_block> thread_blocks;

  thread_block &local() {
    return thread_blocks[rte_lcore_index(rte_lcore_id())];
  }
};

// Bundle handed to each lcore entry point.
struct lcore_adapter {
  port_info &info;
  benchmark_config &config;
};

static uint8_t RSS_DEFAULT_KEY[] = {
    0xbe, 0xac, 0x01, 0xfa, 0x6a, 0x42, 0xb7, 0x3b, 0x80, 0x30,
    0xf2, 0x0c, 0x77, 0xcb, 0x2d, 0xa3, 0xae, 0x7b, 0x30, 0xb4,
    0xd0, 0xca, 0x2b, 0xcb, 0x43, 0xa3, 0x8f, 0xb0, 0x41, 0x67,
    0x25, 0x3d, 0x25, 0x5b, 0x0e, 0xc2, 0x6d, 0x5a, 0x56, 0xda};
static constexpr unsigned RSS_KEY_LEN = 40;

static void setup_reta(port_info &info, uint32_t nrx, uint32_t reta_size) {
  if (reta_size == 0)
    return;
  auto groups = (reta_size + RTE_ETH_RETA_GROUP_SIZE - 1) /
                RTE_ETH_RETA_GROUP_SIZE;
  std::vector<rte_eth_rss_reta_entry64> reta(groups);
  for (auto i = 0u; i < reta_size; ++i) {
    uint32_t reta_id = i / RTE_ETH_RETA_GROUP_SIZE;
    uint32_t reta_pos = i % RTE_ETH_RETA_GROUP_SIZE;
    reta[reta_id].mask = UINT64_MAX;
    reta[reta_id].reta[reta_pos] = static_cast<uint16_t>(i % nrx);
  }
  if (rte_eth_dev_rss_reta_update(info.port_id, reta.data(), reta_size))
    std::cout << "reta update failed" << std::endl;
}

static int configure_port(port_info &info, benchmark_config &config) {
  static constexpr uint16_t kDefaultDescNum = 1024;
  uint16_t nb_cores = config.nb_cores;
  rte_eth_dev_info dinfo{};
  rte_eth_rxconf rxconf{};
  rte_eth_txconf txconf{};
  rte_eth_conf conf{};

  info.dev = eth_os::get_eth_for_port(info.port_id);
  if (!info.dev) {
    std::cout << "no dev" << std::endl;
    return ENODEV;
  }
  info.dev->get_dev_info(&dinfo);

  uint16_t rx_desc = std::min<uint16_t>(kDefaultDescNum, dinfo.rx_desc_lim.nb_max);
  uint16_t tx_desc = std::min<uint16_t>(kDefaultDescNum, dinfo.tx_desc_lim.nb_max);
  rte_eth_dev_adjust_nb_rx_tx_desc(info.port_id, &rx_desc, &tx_desc);

  if (dinfo.tx_offload_capa & RTE_ETH_TX_OFFLOAD_IPV4_CKSUM)
    conf.txmode.offloads |= RTE_ETH_TX_OFFLOAD_IPV4_CKSUM;
  if (dinfo.tx_offload_capa & RTE_ETH_TX_OFFLOAD_UDP_CKSUM)
    conf.txmode.offloads |= RTE_ETH_TX_OFFLOAD_UDP_CKSUM;
  if (dinfo.rx_offload_capa & RTE_ETH_RX_OFFLOAD_IPV4_CKSUM)
    conf.rxmode.offloads |= RTE_ETH_RX_OFFLOAD_IPV4_CKSUM;
  if (dinfo.rx_offload_capa & RTE_ETH_RX_OFFLOAD_UDP_CKSUM)
    conf.rxmode.offloads |= RTE_ETH_RX_OFFLOAD_UDP_CKSUM;

  info.caps.ip_cksum_tx = dinfo.tx_offload_capa & RTE_ETH_TX_OFFLOAD_IPV4_CKSUM;
  info.caps.l4_cksum_tx = dinfo.tx_offload_capa & RTE_ETH_TX_OFFLOAD_UDP_CKSUM;
  info.caps.ip_cksum_rx = dinfo.rx_offload_capa & RTE_ETH_RX_OFFLOAD_IPV4_CKSUM;
  info.caps.l4_cksum_rx = dinfo.rx_offload_capa & RTE_ETH_RX_OFFLOAD_UDP_CKSUM;

  bool rss = nb_cores > 1;
  auto &rssconf = conf.rx_adv_conf.rss_conf;
  if (rss) {
    conf.rxmode.mq_mode = RTE_ETH_MQ_RX_RSS;
    rssconf.algorithm = RTE_ETH_HASH_FUNCTION_DEFAULT;
    if (dinfo.hash_key_size == RSS_KEY_LEN) {
      rssconf.rss_key = RSS_DEFAULT_KEY;
      rssconf.rss_key_len = RSS_KEY_LEN;
    }
    rssconf.rss_hf =
        (RTE_ETH_RSS_NONFRAG_IPV4_UDP & dinfo.flow_type_rss_offloads) |
        (RTE_ETH_RSS_NONFRAG_IPV4_TCP & dinfo.flow_type_rss_offloads);
  } else {
    rssconf.rss_key = nullptr;
    rssconf.rss_hf = 0;
  }

  if (rte_eth_dev_configure(info.port_id, nb_cores, nb_cores, &conf)) {
    std::cout << "dev configure failed" << std::endl;
    return 1;
  }

  rxconf.offloads = conf.rxmode.offloads;
  rxconf.rx_free_thresh = config.burst_size;
  txconf.offloads = conf.txmode.offloads;

  info.thread_blocks.resize(nb_cores);
  for (uint16_t i = 0; i < nb_cores; ++i) {
    auto &tb = info.thread_blocks[i];
    std::string name = "pool-" + std::to_string(i);
    tb.pool = pool_ptr(rte_pktmbuf_pool_create(name.c_str(), 4095, 0, 0, 0, 0),
                       &rte_mempool_free);
    if (!tb.pool) {
      std::cout << "pool create failed" << std::endl;
      return 1;
    }
    tb.rx_queue = i;
    tb.tx_queue = i;
    if (rte_eth_rx_queue_setup(info.port_id, i, rx_desc, 0, &rxconf,
                               tb.pool.get())) {
      std::cout << "rx queue setup failed" << std::endl;
      return 1;
    }
    if (rte_eth_tx_queue_setup(info.port_id, i, tx_desc, 0, &txconf)) {
      std::cout << "tx queue setup failed" << std::endl;
      return 1;
    }
  }

  rte_eth_macaddr_get(info.port_id, &info.addr);
  config.app.src = info.addr;

  if (rte_eth_dev_start(info.port_id)) {
    std::cout << "Starting dev failed" << std::endl;
    return 1;
  }
  if (rss)
    setup_reta(info, nb_cores, dinfo.reta_size);
  return 0;
}

static void init_packets(const std::vector<rte_mbuf *> &pkts) {
  payload payload{static_cast<uint64_t>(rte_get_timer_cycles())};
  for (auto *pkt : pkts) {
    move_data(pkt, payload);
  }
}

static void close_port(port_info &info) { info.dev->stop(); }

static uint16_t receive_packets_ping(thread_block &tb,
                                     std::vector<rte_mbuf *> &pkts,
                                     uint16_t nb_rx) {
  uint16_t total = 0;
  auto ticks = rte_get_timer_cycles();
  for (uint16_t i = 0; i < nb_rx; ++i) {
    if (!verify_packet(pkts[i])) {
      tb.faulty++;
      continue;
    }

    auto pticks = pun<payload>(pkts[i]);
    auto diff = ticks - pticks.ticks;
    tb.ticks += diff;
    ++tb.pkts;
    ++total;
  }
  rte_pktmbuf_free_bulk(pkts.data(), nb_rx);
  return total;
}

static int receive_packets_pong(const rte_ether_addr &src, rte_mbuf *pkt) {
  if (!verify_packet(pkt))
    return -1;
  rte_ether_hdr *eth = rte_pktmbuf_mtod(pkt, rte_ether_hdr *);
  rte_ipv4_hdr *ipv4 = reinterpret_cast<rte_ipv4_hdr *>(eth + 1);
  rte_udp_hdr *udp = reinterpret_cast<rte_udp_hdr *>(ipv4 + 1);
  eth->dst_addr = eth->src_addr;
  eth->src_addr = src;
  SWAP(ipv4->dst_addr, ipv4->src_addr);
  SWAP(udp->dst_port, udp->src_port);
  udp->dgram_cksum = 0;
  ipv4->hdr_checksum = 0;
  ipv4->time_to_live = TTL;
  pkt->l2_len = sizeof(*eth);
  pkt->l3_len = sizeof(*ipv4);
  pkt->l4_len = sizeof(*udp);
  pkt->ol_flags =
      RTE_MBUF_F_TX_UDP_CKSUM | RTE_MBUF_F_TX_IPV4 | RTE_MBUF_F_TX_IP_CKSUM;
  return 0;
}

static int lcore_ping(void *arg) {
  auto &[info, config] = *static_cast<lcore_adapter *>(arg);
  auto &tb = info.local();
  uint16_t nb_rx = 0, burst_size = config.burst_size, total = 0;
  uint16_t nb_tx = burst_size;

  std::vector<rte_mbuf *> pkts(burst_size, nullptr);
  std::vector<rte_mbuf *> rpkts(burst_size, nullptr);
  auto cycles = rte_get_timer_cycles();
  auto end = cycles + config.rt * rte_get_timer_hz();
  for (; cycles < end; cycles = rte_get_timer_cycles()) {
    if (rte_pktmbuf_alloc_bulk(tb.pool.get(), pkts.data(), nb_tx)) {
      std::cerr << "not enough buffers" << std::endl;
      continue;
    }
    for (auto *pkt : pkts)
      create_packet(config.app, pkt);
    init_packets(pkts);
    nb_tx = rte_eth_tx_burst(info.port_id, tb.tx_queue, pkts.data(), burst_size);
    total = 0;
    do {
      nb_rx = rte_eth_rx_burst(info.port_id, tb.rx_queue, rpkts.data(),
                               burst_size);
      if (nb_rx)
        total += receive_packets_ping(tb, rpkts, nb_rx);
    } while (total < nb_tx && rte_get_timer_cycles() < end);
  }
  return 0;
}

static int lcore_pong(void *arg) {
  auto &[info, config] = *static_cast<lcore_adapter *>(arg);
  auto &tb = info.local();
  uint16_t nb_rx = 0, burst_size = config.burst_size;
  uint16_t nb_tx = burst_size;
  uint16_t nb_rm = 0;
  std::vector<rte_mbuf *> pkts(burst_size, nullptr);
  std::vector<rte_mbuf *> rpkts(burst_size, nullptr);
  for (; !terminate;) {
    nb_rx = rte_eth_rx_burst(info.port_id, tb.rx_queue, rpkts.data(),
                             burst_size - nb_rm);
    for (uint16_t i = 0; i < nb_rx; ++i) {
      pkts[nb_rm] = rpkts[i];
      if (!receive_packets_pong(config.app.src, pkts[nb_rm]))
        ++nb_rm;
    }
    nb_tx = rte_eth_tx_burst(info.port_id, tb.tx_queue, pkts.data(), nb_rm);
    for (uint16_t j = 0, i = nb_tx; i < nb_rm; ++i, ++j)
      pkts[j] = pkts[i];
    nb_rm = nb_rm - nb_tx;
    tb.pkts += nb_tx;
  }
  return 0;
}

static int lcore_recv(void *arg) {
  auto &[info, config] = *static_cast<lcore_adapter *>(arg);
  auto &tb = info.local();
  std::vector<pkt_t *> pkts(config.burst_size, nullptr);
  auto begin = rte_get_timer_cycles();
  for (; !terminate;) {
    auto rx = rte_eth_rx_burst(info.port_id, tb.rx_queue, pkts.data(),
                               config.burst_size);
    if (!rx)
      continue;
    tb.pkts += rx;
    rte_pktmbuf_free_bulk(pkts.data(), rx);
  }
  tb.ticks = rte_get_timer_cycles() - begin;

  sched::update_disable_reschedule(false);
  return 0;
}

int main(int argc, char *argv[]) {
  struct sigaction sa{};
  sa.sa_handler = handler;
  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGTERM, &sa, NULL);

  port_info info;
  benchmark_config config;
  auto &conf = config.app;
  int opt, option_index;
  static const struct option long_options[] = {
      {"dip", required_argument, 0, 0},   {"sip", required_argument, 0, 0},
      {"dmac", required_argument, 0, 0},  {"rt", required_argument, 0, 0},
      {"mtu", required_argument, 0, 0},   {"mode", required_argument, 0, 0},
      {"bs", required_argument, 0, 0},    {"cores", required_argument, 0, 0},
      {0, 0, 0, 0}};

  config.burst_size = 1;
  config.rt = rte_get_timer_hz();
  while ((opt = getopt_long(argc, argv, "", long_options, &option_index)) !=
         -1) {
    switch (option_index) {
    case 0:
      conf.dip = inet_addr(optarg);
      break;
    case 1:
      conf.sip = inet_addr(optarg);
      break;
    case 2:
      conf.dst.parse_string(optarg);
      break;
    case 3:
      config.rt = atoi(optarg);
      break;
    case 4:
      conf.mtu = atoi(optarg);
      break;
    case 5: {
      std::string m(optarg);
      config.role = m == "PING"      ? opmode::PING
                    : m == "RECEIVE" ? opmode::RECEIVE
                                     : opmode::PONG;
      break;
    }
    case 6:
      config.burst_size = atoi(optarg);
      break;
    case 7:
      config.nb_cores = atoi(optarg);
      break;
    }
  }

  lcore_container::init(config.nb_cores);
  if (configure_port(info, config))
    return -1;

  lcore_adapter adapter{info, config};
  int (*lcore_fn)(void *) = nullptr;
  switch (config.role) {
  case opmode::PING:
    lcore_fn = lcore_ping;
    break;
  case opmode::PONG:
    lcore_fn = lcore_pong;
    break;
  case opmode::RECEIVE:
    lcore_fn = lcore_recv;
    break;
  }

  rte_eth_stats stats;
  info.dev->get_stats(&stats);
  std::cerr << stats.imissed << ", " << stats.ierrors << ", " << stats.ipackets
            << ", " << stats.ibytes << std::endl;

  rte_eal_mp_remote_launch(lcore_fn, &adapter, CALL_MAIN);
  rte_eal_mp_wait_lcore();

  uint64_t total_ticks = 0, total_pkts = 0, total_faulty = 0, max_ticks = 0;
  for (auto &tb : info.thread_blocks) {
    total_ticks += tb.ticks;
    total_pkts += tb.pkts;
    total_faulty += tb.faulty;
    max_ticks = std::max(max_ticks, tb.ticks);
  }

  auto timer_hz = rte_get_timer_hz();
  std::cout << "Packets:" << total_pkts << std::endl;
  std::cout << "Faulty:" << total_faulty << std::endl;
  if (config.role == opmode::PING && total_pkts) {
    std::cout << "Latency:"
              << (static_cast<double>(total_ticks) / (timer_hz / 1e6)) /
                     static_cast<double>(total_pkts)
              << std::endl;
    std::cout << "PPS:" << static_cast<double>(total_pkts) / config.rt
              << std::endl;
  } else if (config.role == opmode::RECEIVE && max_ticks) {
    double seconds = static_cast<double>(max_ticks) / timer_hz;
    std::cout << "PPS:" << static_cast<double>(total_pkts) / seconds
              << std::endl;
  }

  info.dev->get_stats(&stats);
  std::cerr << stats.imissed << ", " << stats.ierrors << ", " << stats.ipackets
            << ", " << stats.ibytes << std::endl;

  std::cerr << "done" << std::endl;
  close_port(info);
}
