#ifndef BYPASS_NET_H
#define BYPASS_NET_H

#include <array>
#include <cstdio>
#include <cstdint>


#define RTE_ETHER_ADDR_LEN 6
struct rte_ether_addr {
    std::array<unsigned char, RTE_ETHER_ADDR_LEN> addr;
    void parse_string(const char* mac){
        sscanf(mac, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx", &addr[0], &addr[1], &addr[2], &addr[3], &addr[4], &addr[5]);
    }

     static rte_ether_addr broadcast;   
};

struct [[gnu::packed]] rte_eth_header{
    rte_ether_addr dst;
    rte_ether_addr src;
    uint16_t ether_type;
};
#endif // !BYPASS_NET_H

