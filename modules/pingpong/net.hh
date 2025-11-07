#ifndef HEADERS_H
#define HEADERS_H
#include <array>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <api/bypass/pktbuf.hh>
#include <endian.h>
#include <netinet/ip.h>
#include <string>

static constexpr uint16_t ETHER_ADDR_LEN = 6;
static constexpr uint16_t IPV4 = 0x0800;
static constexpr uint16_t VERSION = 4;
static constexpr uint16_t TTL = 64;

static __inline void* PTR_ADD(const void* ptr, size_t x) { return ((void*)((uintptr_t)(ptr) + (x))); }

template<typename T>
static __inline T ALIGN_FLOOR(T val, uint32_t align){
    return static_cast<T>((val) & (~static_cast<T>(align - 1)));
}
struct ether_addr {
    std::array<char, ETHER_ADDR_LEN> addr;
    void parse_string(const char* mac){
        sscanf(mac, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx", &addr[0], &addr[1], &addr[2], &addr[3], &addr[4], &addr[5]);
        auto len = strlen(mac);
        for(auto i = 0u, j = 0u; i < len; i += 3){
            addr[j++] = std::stoul(mac + i, nullptr, 16);
        }
    }
};

struct [[gnu::packed]] eth_header{
    ether_addr dst;
    ether_addr src;
    uint16_t ether_type;
};

struct [[gnu::packed]] ipv4_header{
    uint8_t  version_ihl;       
     uint8_t  type_of_service;   
     uint16_t total_length;    
     uint16_t packet_id;       
     uint16_t fragment_offset; 
     uint8_t  time_to_live;      
     uint8_t  next_proto_id;     
     uint16_t hdr_checksum;    
     uint32_t src_addr;        
     uint32_t dst_addr;
};

struct [[gnu::packed]] udp_header{
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t dgram_len;
    uint16_t dgram_cksum;

};

struct app_config{
    ether_addr src;
    ether_addr dst;
    uint32_t sip;
    uint32_t dip;
    uint32_t l4port;
    uint32_t data_len;
};

/* from dpdk */

 static inline uint32_t
 _raw_cksum(const void *buf, size_t len, uint32_t sum)
 {
     const void *end;
 
     for (end = PTR_ADD(buf, ALIGN_FLOOR(len, sizeof(uint16_t)));
          buf != end; buf = PTR_ADD(buf, sizeof(uint16_t))) {
         uint16_t v;
 
         memcpy(&v, buf, sizeof(uint16_t));
         sum += v;
     }
 
     /* if length is odd, keeping it byte order independent */
     if (len % 2) {
         uint16_t left = 0;
 
         memcpy(&left, end, 1);
         sum += left;
     }
 
     return sum;
 }

static inline uint16_t
 _raw_cksum_reduce(uint32_t sum)
 {
     sum = ((sum & 0xffff0000) >> 16) + (sum & 0xffff);
     sum = ((sum & 0xffff0000) >> 16) + (sum & 0xffff);
     return (uint16_t)sum;
 }

inline uint16_t phdr_cksum(ipv4_header* ipv4, udp_header* udp){
    struct ipv4_psd_header {
         uint32_t src_addr; /* IP address of source host. */
         uint32_t dst_addr; /* IP address of destination host. */
         uint8_t  zero;     /* zero. */
         uint8_t  proto;    /* L4 protocol type. */
         uint16_t len;      /* L4 length. */
     } psd_hdr;
  
     psd_hdr.src_addr = ipv4->src_addr;
     psd_hdr.dst_addr = ipv4->dst_addr;
     psd_hdr.zero = 0;
     psd_hdr.proto = ipv4->next_proto_id;
     psd_hdr.len = udp->dgram_len;
     auto sum = _raw_cksum(&psd_hdr, sizeof(psd_hdr), 0);
     return _raw_cksum_reduce(sum);
}


static void create_packet(const app_config& config, pkt_buf *pkt){
    uint16_t len = config.data_len;
    eth_header *eth = reinterpret_cast<eth_header*>(pkt->buf);
    ipv4_header *ipv4 = reinterpret_cast<ipv4_header*>(eth + 1);
    udp_header *udp = reinterpret_cast<udp_header*>(ipv4 + 1);

    len += sizeof(*udp);
    udp->src_port = config.l4port;
    udp->dst_port = config.l4port;
    udp->dgram_len = htobe16(len);
    pkt->l4_len = sizeof(*udp);

    len += sizeof(*ipv4);
    ipv4->version_ihl = VERSION;
    ipv4->time_to_live = TTL;
    ipv4->next_proto_id = IPPROTO_UDP;
    ipv4->fragment_offset = 0;
    ipv4->packet_id = 0;
    ipv4->total_length = htobe16(len);
    ipv4->type_of_service = 0;
    ipv4->dst_addr = config.dip;
    ipv4->src_addr = config.sip;
    pkt->l3_len = sizeof(*ipv4);

    eth->src = config.src;
    eth->dst = config.dst;
    eth->ether_type = IPV4;
    pkt->l2_len = sizeof(*eth);

    pkt->pkt_len = sizeof(*eth) + len;
    pkt->data_len = sizeof(*eth) + len;

    udp->dgram_cksum = 0;
    ipv4->hdr_checksum = 0;
    udp->dgram_cksum = phdr_cksum(ipv4, udp);
    pkt->nb_segs = 1;
    pkt->olflags = PBUF_OFFLOAD_IPV4_CKSUM | PBUF_OFFLOAD_UDP_CKSUM;
}

static bool verify_packet(const pkt_buf* pkt){
    return (pkt->olflags & PBUF_CKSUM_L4_OK) && (pkt->olflags & PBUF_CKSUM_L3_OK);
}
#endif // !HEADERS_H
