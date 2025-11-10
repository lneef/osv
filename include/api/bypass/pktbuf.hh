#ifndef PKTBUF_H
#define PKTBUF_H

#include <array>
#include <cstdint>
#include <cstdlib>

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

/* thread local data structure */
class pbuf_pool{
    public:
        static constexpr uint32_t cache_size = 1024;
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
        /* adapted from uma cache*/
        template<uint32_t size>
       struct cache{
           uint32_t head = 0;
           std::array<pkt_buf*, size> ring;

           uint32_t get(pkt_buf** pkts, uint16_t nb){
               auto from_cache = std::min<uint32_t>(nb, head);
               for(uint32_t i = 0; i < from_cache; ++i)
                   pkts[i] = ring[--head];
               return from_cache;
           }

           uint32_t add(pkt_buf** pkts, uint16_t nb){
               auto to_cache = std::min<uint32_t>(ring.size() - head, nb);
               for(uint32_t i = 0; i < to_cache; ++i)
                   ring[head++] = pkts[i];
               return to_cache;
           }
           ~cache(){
               for(uint32_t i = 0; i < head; ++i)
                   free(ring[i]);
           }
       };

       uint32_t data_size;
       struct{
           cache<cache_size> cpu_cache;
       }pool;
};


#endif
