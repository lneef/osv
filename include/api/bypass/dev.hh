#ifndef NET_ETH_DEF_H
#define NET_ETH_DEF_H
#include <api/bypass/rss.hh>
#include <bypass/net.hh>
#include <cstdint>
#include <atomic>
#include "api/bypass/mem.hh"

#define MAX_ETH_PORTS 16

struct rte_eth_dev;
struct eth_os{
    struct {
        int configured;
        rte_eth_dev* dev;
    }ports_info[MAX_ETH_PORTS];

    void register_port(uint16_t port, rte_eth_dev* dev);
};

extern eth_os ports;
enum queue_state{
    RTE_ETH_QUEUE_STATE_STOPPED = 0,
    RTE_ETH_QUEUE_STATE_STARTED
};
#define RTE_ETH_MQ_RX_RSS_FLAG  RTE_BIT32(0) 
#define RTE_ETH_MQ_RX_DCB_FLAG  RTE_BIT32(1) 
#define RTE_ETH_MQ_RX_VMDQ_FLAG RTE_BIT32(2) 
enum rte_eth_rx_mq_mode {
    RTE_ETH_MQ_RX_NONE = 0,
 
    RTE_ETH_MQ_RX_RSS = RTE_ETH_MQ_RX_RSS_FLAG,
    RTE_ETH_MQ_RX_DCB = RTE_ETH_MQ_RX_DCB_FLAG,
    RTE_ETH_MQ_RX_DCB_RSS = RTE_ETH_MQ_RX_RSS_FLAG | RTE_ETH_MQ_RX_DCB_FLAG,
 
    RTE_ETH_MQ_RX_VMDQ_ONLY = RTE_ETH_MQ_RX_VMDQ_FLAG,
    RTE_ETH_MQ_RX_VMDQ_RSS = RTE_ETH_MQ_RX_RSS_FLAG | RTE_ETH_MQ_RX_VMDQ_FLAG,
    RTE_ETH_MQ_RX_VMDQ_DCB = RTE_ETH_MQ_RX_VMDQ_FLAG | RTE_ETH_MQ_RX_DCB_FLAG,
    RTE_ETH_MQ_RX_VMDQ_DCB_RSS = RTE_ETH_MQ_RX_RSS_FLAG | RTE_ETH_MQ_RX_DCB_FLAG |
                 RTE_ETH_MQ_RX_VMDQ_FLAG,
};
 
enum rte_eth_tx_mq_mode {
    RTE_ETH_MQ_TX_NONE    = 0,  
    RTE_ETH_MQ_TX_DCB,          
    RTE_ETH_MQ_TX_VMDQ_DCB,     
    RTE_ETH_MQ_TX_VMDQ_ONLY,    
};
 
struct rte_eth_stats {
    std::atomic<uint64_t> ipackets;  
    std::atomic<uint64_t> opackets;  
    std::atomic<uint64_t>  ibytes;    
    std::atomic<uint64_t> obytes;    
    std::atomic<uint64_t>  imissed;
    std::atomic<uint64_t> ierrors;   
    std::atomic<uint64_t> oerrors;   
    std::atomic<uint64_t> rx_nombuf; 
};

struct rte_eth_txconf{
    uint64_t offloads;
    uint64_t tx_free_thresh;
};

struct rte_eth_rxconf{
        enum rte_eth_tx_mq_mode mq_mode;
        uint64_t offloads;
        uint64_t rx_free_thresh;
};
struct rte_dev_conf{
    rte_eth_rxconf rxmode;
    rte_eth_txconf txmode;

    struct{
        rte_eth_rss_conf rss_conf; 
    }rx_adv_conf;
};

struct rte_eth_dev_data {
    uint16_t nb_rx_queues, nb_tx_queues;
    uint16_t port_id;
    rte_dev_conf dev_conf;
    void *data;
    int dev_started = 0;
    std::array<enum queue_state, 1024> rx_queue_state;
    std::array<enum queue_state, 1024> tx_queue_state;
    std::array<void*, 1024> tx_queues;
    std::array<void*, 1024> rx_queues;
    struct{
        std::array<uint8_t, RTE_ETHER_ADDR_LEN> mac;
    }mac_addr;

    template<typename T>
        T* get() {return static_cast<T*>(data);}
};

struct rte_eth_dev{
    rte_eth_dev_data data;
    template<typename T>
        T* get(){ return static_cast<T*>(data.get<T>()); }

    virtual int mtu_set(uint16_t mtu) = 0;
    virtual int start() = 0;
    virtual int stop() = 0;
    virtual int tx_queue_setup(uint16_t qid,
			      uint16_t nb_desc, unsigned int socket_id,
			      const struct rte_eth_txconf *tx_conf) = 0;
    virtual int rx_queue_setup(uint16_t qid,
			      uint16_t nb_desc, unsigned int socket_id,
			      const struct rte_eth_rxconf *rx_conf,
            rte_mempool *mp) = 0;
    virtual uint16_t tx_burst(uint16_t qid, rte_mbuf** pkts, uint16_t nb_pkts) = 0;
    virtual uint16_t rx_burst(uint16_t qid, rte_mbuf** pkts, uint16_t nb_pkts) = 0;
    virtual uint64_t tx_queue_offloads() = 0;
    virtual uint64_t rx_queue_offloads() = 0;
    virtual uint64_t tx_port_offloads() = 0;
    virtual uint64_t rx_port_offloads() = 0;
};

struct eth_dev_info{
    uint64_t tx_offload_capa, rx_offload_capa;
};

#endif
