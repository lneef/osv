#include <cstdint>
#include <api/bypass/net_eth.hh>
#include <osv/export.h>
#include <osv/clock.hh>
eth_os ports;


void eth_os::register_port(uint16_t port, eth_dev *dev){ 
    auto& pinfo = ports.ports_info[port];
    pinfo.configured = 1;
    pinfo.dev = dev;
}


eth_dev* eth_dev::get_eth_for_port(uint16_t port){
    if(port >= MAX_ETH_PORTS || !ports.ports_info[port].configured)
        return nullptr;
    else
        return ports.ports_info[port].dev;
}

extern "C" OSV_MODULE_API 
uint64_t get_ns(){
    return osv::clock::uptime::now().time_since_epoch().count();
}
