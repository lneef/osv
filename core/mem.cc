#include <bypass/mem.hh>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <new>
#include <bsd/porting/netport.h>
#include <cerrno>

void rte_pktmbuf_free(rte_mbuf* mbuf){
    mbuf->pool->free_bulk(&mbuf, 1);
}
void rte_mbuf_raw_free(rte_mbuf* mbuf){
    mbuf->pool->free_bulk(&mbuf, 1);
}

void rte_pktmbuf_free_bulk(rte_mbuf** pkts, uint16_t size){
    if(!size)
        return;
    pkts[0]->pool->free_bulk(pkts, size);
}


void rte_pktmbuf_read(rte_mbuf *m, uint32_t off,
	uint32_t len, uint8_t *buf)
{
    uint32_t buf_off = 0;
    for(auto* seg = m; len > 0 && seg;  seg = seg->next){
        auto copy_len = seg->buf_len - off;
        if(copy_len > seg->buf_len)
            copy_len = seg->buf_len;
        memcpy(reinterpret_cast<char*>(buf) + buf_off, seg->buf + off, copy_len);
        off = 0;
		    buf_off += copy_len;
        len -= copy_len;
    }
}


static constexpr uint32_t cl_size = 64;

static int mb_ctor_buf(void * mem, int, void *arg, int){
    rte_mbuf* mbuf = static_cast<rte_mbuf*>(mem);
    rte_pktmbuf_pool* pool = static_cast<rte_pktmbuf_pool*>(arg);
    *mbuf  = {};
    mbuf->pool = pool;
    mbuf->buf_len = pool->get_data_size();
    mbuf->next = nullptr;
    return 0;
}

rte_pktmbuf_pool::rte_pktmbuf_pool(const char* name, uint32_t size, uint32_t elems, uint32_t flags): data_size(size){
    (void)name;
    (void)elems;
    (void)flags;
}

void rte_pktmbuf_pool::prefill(){
    auto& head = pool.cpu_cache.head;
    auto& ring = pool.cpu_cache.ring;
    for(; head < cache_size; ++head){
        ring[head] = static_cast<rte_mbuf*>(aligned_alloc(cl_size, data_size + sizeof(rte_mbuf)));
        mb_ctor_buf(ring[head], 0, this, 0);
    }
    static_assert(cache_size > 1024, "too small");
}

rte_pktmbuf_pool::~rte_pktmbuf_pool() = default;


rte_pktmbuf_pool* rte_pktmbuf_pool::rte_pktmbuf_pool_create(const char *name, uint32_t size, uint32_t flags = 0){
    rte_pktmbuf_pool* pool = static_cast<rte_pktmbuf_pool*>(malloc(sizeof(rte_pktmbuf_pool)));
    new (pool) rte_pktmbuf_pool(name, size, 0, flags);
    return pool;
}


void rte_pktmbuf_pool::rte_pktmbuf_pool_delete(rte_pktmbuf_pool *pb_pool){
    pb_pool->~rte_pktmbuf_pool();
    free(pb_pool);
}

int rte_pktmbuf_pool::alloc_from_cache(rte_mbuf **pkts, uint16_t nb){
    uint16_t i = pool.cpu_cache.get(pkts, nb);
    if(i < nb){
        free_bulk(pkts,  nb);
        return ENOMEM;
    }
    return 0;
}

int rte_pktmbuf_pool::alloc_bulk(struct rte_mbuf** pkts, uint16_t nb){
    uint16_t i = pool.cpu_cache.get(pkts, nb);
    malloc_stat += nb - i;
    for(; i < nb; ++i){
        pkts[i] = static_cast<rte_mbuf*>(aligned_alloc(cl_size, data_size + sizeof(rte_mbuf)));
        if(!pkts[i])
            break;
    }
    if(i < nb){
        free_bulk(pkts, i);
        return ENOMEM;
    }
    for(i = 0; i < nb; ++i)
        mb_ctor_buf(pkts[i], 0, this, 0);
    return 0;

}

/* add freeing chains of buffer */
void rte_pktmbuf_pool::free_bulk(struct rte_mbuf** pkts, uint16_t nb){
    uint32_t i = pool.cpu_cache.add(pkts, nb);
    for(; i < nb; ++i)
        free(pkts[i]);
}
