#ifndef ENA_IF_H_
#define ENA_IF_H_

#include <bypass/dev.hh>
#include <bypass/mem.hh>
#include <cstdint>

class ena_eth_dev : public rte_eth_dev{
public:
    int mtu_set(uint16_t mtu) override;
    int start() override;
    int stop() override;
    int tx_queue_setup(uint16_t qid,
			      uint16_t nb_desc, unsigned int socket_id,
			      const struct rte_eth_txconf *tx_conf) override;
    int rx_queue_setup(uint16_t qid,
			      uint16_t nb_desc, unsigned int socket_id,
			      const struct rte_eth_rxconf *rx_conf,
            rte_mempool *mp) override;
    uint16_t tx_burst(uint16_t qid, rte_mbuf** pkts, uint16_t nb_pkts) override;
    uint16_t rx_burst(uint16_t qid, rte_mbuf** pkts, uint16_t nb_pkts) override;
    uint64_t tx_queue_offloads() override;
    uint64_t rx_queue_offloads() override;
    uint64_t tx_port_offloads() override;
    uint64_t rx_port_offloads() override;

};
#endif // !ENA_IF_H_
