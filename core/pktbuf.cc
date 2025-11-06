#include <cstdint>
#include <cstring>
#include <api/bypass/pktbuf.hh>
#include <osv/export.h>


void pkt_buf::copy_data(uint32_t size, uint8_t *target){
    uint32_t target_offset = 0;
    pkt_buf *cur = this;
    while(target_offset < size){
        if(!cur)
            break;
        std::memcpy(cur->buf, target + target_offset, size);
        target_offset += size;
        cur = cur->next;
    }
}


void pkt_buf::append(pkt_buf *other){
    pkt_buf** buf = &next;
    for(; *buf; buf = &(*buf)->next) 
        ;
    *buf = other;
    pkt_len += other->pkt_len;
    ++nb_segs;
}
