#include <bypass/dev.hh>
#include <cstdint>

void rte_eth_dev::dev_configure(uint16_t nb_rx, uint16_t nb_tx, rte_eth_conf *conf){
    data.nb_rx_queues = nb_rx;
    data.nb_tx_queues = nb_tx;
    data.dev_conf.rxmode = conf->rxmode;
    data.dev_conf.txmode = conf->txmode;
    data.dev_conf.rx_adv_conf = conf->rx_adv_conf;
}
