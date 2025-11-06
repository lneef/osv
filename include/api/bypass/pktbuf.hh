#ifndef PKTBUF_H
#define PKTBUF_H

#include <cstdint>

class pbuf_pool;

enum pbuf_offloads : uint64_t{
    PBUF_OFFLOAD_NONE = 0,
    PBUF_OFFLOAD_IPV4_CKSUM = 1,
    PBUF_OFFLOAD_UDP_CKSUM = 2,
    PBUF_CKSUM_L3_BAD = 4,
    PBUF_CKSUM_L3_OK = 8,
    PBUF_CKSUM_L4_BAD = 16,
    PBUF_CKSUM_L4_OK = 32,
    PBUF_CKSUM_L4_UNKONW = 64,
};

enum packet_type : uint16_t {
    PACKET_NONE = 0,
    PACKET_IPV4 = 1,
    PACKET_UDP = 4
};

struct pkt_buf{
    uint16_t l2_len, l3_len, l4_len, nb_segs, packet_type;
    uint32_t pkt_len, data_len, buf_len;
    uint64_t olflags;
    pbuf_pool* pool;
    pkt_buf* next;
    alignas(uint64_t) char buf[];

    void copy_data(uint32_t size, uint8_t* target);

    void append(struct pkt_buf* pbuf);
};

class pbuf_pool{
    public:
        pbuf_pool(const char* name, uint32_t size, uint32_t elems, uint32_t flags);

        ~pbuf_pool();

        pbuf_pool(const pbuf_pool&) = delete;
        pbuf_pool(pbuf_pool&&) noexcept = delete;

        static pbuf_pool* bpuf_pool_create(const char* name, uint32_t size, uint32_t flags);
        static void pbuf_pool_delete(pbuf_pool* pb_pool);


        int alloc_bulk(struct pkt_buf** pkts, uint16_t nb);
        void free_bulk(struct pkt_buf** pkts, uint16_t nb);

        uint32_t get_data_size() const {return data_size;}

    private:
       uint32_t data_size;
};


#endif
