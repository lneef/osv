#include <cstdint>
#include <cstring>
#include <api/bypass/pktbuf.hh>
#include <osv/export.h>
#include <algorithm>


void pkt_buf::copy_data(uint32_t size, uint8_t *target){
    uint32_t target_offset = 0, to_copy = 0;
    pkt_buf *cur = this;
    while(target_offset < size){
        if(!cur)
            break;
        to_copy = std::min(cur->data_len, size);
        std::memcpy(cur->buf, target + target_offset, to_copy);
        target_offset += to_copy;
        size -= to_copy;
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
