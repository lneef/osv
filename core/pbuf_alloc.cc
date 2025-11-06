#include <api/bypass/pktbuf.hh>
#include <cerrno>
#include <cstdint>
#include <bsd/porting/netport.h>
#include <cstdlib>
#include <osv/export.h>
#include <new>

/*
 * Uses just malloc for now 
 */


static int	mb_ctor_buf(void * mem, int, void *arg, int){
    pkt_buf* pbuf = static_cast<pkt_buf*>(mem);
    pbuf_pool* pool = static_cast<pbuf_pool*>(arg);
    *pbuf  = {};
    pbuf->pool = pool;
    pbuf->buf_len = pool->get_data_size();
    pbuf->next = nullptr;
    return 0;
}


pbuf_pool::pbuf_pool(const char* name, uint32_t size, uint32_t elems, uint32_t flags): data_size(size){
    (void)name;
    (void)elems;
    (void)flags;
}


pbuf_pool::~pbuf_pool(){
}


pbuf_pool* pbuf_pool::bpuf_pool_create(const char *name, uint32_t size, uint32_t flags = 0){
    pbuf_pool* pool = static_cast<pbuf_pool*>(malloc(sizeof(pbuf_pool)));
    new (pool) pbuf_pool(name, size, 0, flags);
    return pool;
}


void pbuf_pool::pbuf_pool_delete(pbuf_pool *pb_pool){
    pb_pool->~pbuf_pool();
    free(pb_pool);
}


int pbuf_pool::alloc_bulk(struct pkt_buf** pkts, uint16_t nb){
    uint16_t i = 0;
    for(; i < nb; ++i){
        pkts[i] = static_cast<pkt_buf*>(malloc(sizeof(pkt_buf) + data_size));
        if(!pkts[i])
            break;
    }
    if(i < nb){
        free_bulk(pkts, i);
        return -ENOMEM;
    }
    for(i = 0; i < nb; ++i)
        mb_ctor_buf(pkts[i], 0, this, 0);
    return 0;

}


void pbuf_pool::free_bulk(struct pkt_buf** pkts, uint16_t nb){
    for(uint16_t i = 0; i < nb; ++i){
        pkt_buf* pbuf = pkts[i];
        for(; pbuf;){
            pkt_buf* del = pbuf;
            pbuf = pbuf->next;
            free(del);
        }
    }
}
