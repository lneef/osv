#include <api/bypass/dev.hh>
#include <osv/export.h>
#include <osv/clock.hh>


eth_os eth_os::instance;


void eth_os::register_port(rte_eth_dev *dev){ 
    instance.ifs.push_back(dev);
}

rte_eth_dev* eth_os::get_eth_for_port(uint16_t port){
    if(instance.ifs.size() <= port)
        return nullptr;
    return instance.ifs[port];
}

