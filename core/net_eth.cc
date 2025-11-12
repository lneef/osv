#include <cstdint>
#include <api/bypass/dev.hh>
#include <osv/export.h>
#include <osv/clock.hh>
eth_os ports;


void eth_os::register_port(uint16_t port, rte_eth_dev *dev){ 
    auto& pinfo = ports.ports_info[port];
    pinfo.configured = 1;
    pinfo.dev = dev;
}

/*
rte_eth_dev* rte_eth_dev::get_eth_for_port(uint16_t port){
    if(port >= MAX_ETH_PORTS || !ports.ports_info[port].configured)
        return nullptr;
    else
        return ports.ports_info[port].dev;
}
*/

extern "C" OSV_MODULE_API 
uint64_t get_ns(){
    return osv::clock::uptime::now().time_since_epoch().count();
}
