#ifndef BYPASS_MEM_H
#define BYPASS_MEM_H




#include "osv/mmu-defs.hh"
#include "osv/pagealloc.hh"
#include "osv/virt_to_phys.hh"
#include <osv/types.h>
#include <array>
#include <cassert>
#include <vector>
#include <cstdint>

#define RTE_MBUF_F_RX_VLAN (1ULL << 0)

#define RTE_MBUF_F_RX_RSS_HASH (1ULL << 1)

#define RTE_MBUF_F_RX_FDIR (1ULL << 2)

#define RTE_MBUF_F_RX_OUTER_IP_CKSUM_BAD (1ULL << 5)

#define RTE_MBUF_F_RX_VLAN_STRIPPED (1ULL << 6)

#define RTE_MBUF_F_RX_IP_CKSUM_MASK ((1ULL << 4) | (1ULL << 7))

#define RTE_MBUF_F_RX_IP_CKSUM_UNKNOWN 0
#define RTE_MBUF_F_RX_IP_CKSUM_BAD (1ULL << 4)
#define RTE_MBUF_F_RX_IP_CKSUM_GOOD (1ULL << 7)
#define RTE_MBUF_F_RX_IP_CKSUM_NONE ((1ULL << 4) | (1ULL << 7))

#define RTE_MBUF_F_RX_L4_CKSUM_MASK ((1ULL << 3) | (1ULL << 8))

#define RTE_MBUF_F_RX_L4_CKSUM_UNKNOWN 0
#define RTE_MBUF_F_RX_L4_CKSUM_BAD (1ULL << 3)
#define RTE_MBUF_F_RX_L4_CKSUM_GOOD (1ULL << 8)
#define RTE_MBUF_F_RX_L4_CKSUM_NONE ((1ULL << 3) | (1ULL << 8))

#define RTE_MBUF_F_RX_IEEE1588_PTP (1ULL << 9)

#define RTE_MBUF_F_RX_IEEE1588_TMST (1ULL << 10)

#define RTE_MBUF_F_RX_FDIR_ID (1ULL << 13)

#define RTE_MBUF_F_RX_FDIR_FLX (1ULL << 14)

#define RTE_MBUF_F_RX_QINQ_STRIPPED (1ULL << 15)

#define RTE_MBUF_F_RX_LRO (1ULL << 16)

/* There is no flag defined at offset 17. It is free for any future use. */

#define RTE_MBUF_F_RX_SEC_OFFLOAD (1ULL << 18)

#define RTE_MBUF_F_RX_SEC_OFFLOAD_FAILED (1ULL << 19)

#define RTE_MBUF_F_RX_QINQ (1ULL << 20)

#define RTE_MBUF_F_RX_OUTER_L4_CKSUM_MASK ((1ULL << 21) | (1ULL << 22))

#define RTE_MBUF_F_RX_OUTER_L4_CKSUM_UNKNOWN 0
#define RTE_MBUF_F_RX_OUTER_L4_CKSUM_BAD (1ULL << 21)
#define RTE_MBUF_F_RX_OUTER_L4_CKSUM_GOOD (1ULL << 22)
#define RTE_MBUF_F_RX_OUTER_L4_CKSUM_INVALID ((1ULL << 21) | (1ULL << 22))

/* add new RX flags here, don't forget to update RTE_MBUF_F_FIRST_FREE */

#define RTE_MBUF_F_FIRST_FREE (1ULL << 23)
#define RTE_MBUF_F_LAST_FREE (1ULL << 40)

/* add new TX flags here, don't forget to update RTE_MBUF_F_LAST_FREE  */

#define RTE_MBUF_F_TX_OUTER_UDP_CKSUM (1ULL << 41)

#define RTE_MBUF_F_TX_UDP_SEG (1ULL << 42)

#define RTE_MBUF_F_TX_SEC_OFFLOAD (1ULL << 43)

#define RTE_MBUF_F_TX_MACSEC (1ULL << 44)

#define RTE_MBUF_F_TX_TUNNEL_VXLAN (0x1ULL << 45)
#define RTE_MBUF_F_TX_TUNNEL_GRE (0x2ULL << 45)
#define RTE_MBUF_F_TX_TUNNEL_IPIP (0x3ULL << 45)
#define RTE_MBUF_F_TX_TUNNEL_GENEVE (0x4ULL << 45)
#define RTE_MBUF_F_TX_TUNNEL_MPLSINUDP (0x5ULL << 45)
#define RTE_MBUF_F_TX_TUNNEL_VXLAN_GPE (0x6ULL << 45)
#define RTE_MBUF_F_TX_TUNNEL_GTP (0x7ULL << 45)
#define RTE_MBUF_F_TX_TUNNEL_ESP (0x8ULL << 45)
#define RTE_MBUF_F_TX_TUNNEL_IP (0xDULL << 45)
#define RTE_MBUF_F_TX_TUNNEL_UDP (0xEULL << 45)
/* add new TX TUNNEL type here */
#define RTE_MBUF_F_TX_TUNNEL_MASK (0xFULL << 45)

#define RTE_MBUF_F_TX_QINQ (1ULL << 49)

#define RTE_MBUF_F_TX_TCP_SEG (1ULL << 50)

#define RTE_MBUF_F_TX_IEEE1588_TMST (1ULL << 51)

/*
 * Bits 52+53 used for L4 packet type with checksum enabled: 00: Reserved,
 * 01: TCP checksum, 10: SCTP checksum, 11: UDP checksum. To use hardware
 * L4 checksum offload, the user needs to:
 *  - fill l2_len and l3_len in mbuf
 *  - set the flags RTE_MBUF_F_TX_TCP_CKSUM, RTE_MBUF_F_TX_SCTP_CKSUM or
 *    RTE_MBUF_F_TX_UDP_CKSUM
 *  - set the flag RTE_MBUF_F_TX_IPV4 or RTE_MBUF_F_TX_IPV6
 */

#define RTE_MBUF_F_TX_L4_NO_CKSUM (0ULL << 52)

#define RTE_MBUF_F_TX_TCP_CKSUM (1ULL << 52)

#define RTE_MBUF_F_TX_SCTP_CKSUM (2ULL << 52)

#define RTE_MBUF_F_TX_UDP_CKSUM (3ULL << 52)

#define RTE_MBUF_F_TX_L4_MASK (3ULL << 52)

#define RTE_MBUF_F_TX_IP_CKSUM (1ULL << 54)

#define RTE_MBUF_F_TX_IPV4 (1ULL << 55)

#define RTE_MBUF_F_TX_IPV6 (1ULL << 56)

#define RTE_MBUF_F_TX_VLAN (1ULL << 57)

#define RTE_MBUF_F_TX_OUTER_IP_CKSUM (1ULL << 58)

#define RTE_MBUF_F_TX_OUTER_IPV4 (1ULL << 59)

#define RTE_MBUF_F_TX_OUTER_IPV6 (1ULL << 60)

#define RTE_MBUF_F_TX_OFFLOAD_MASK                                             \
  (RTE_MBUF_F_TX_OUTER_IPV6 | RTE_MBUF_F_TX_OUTER_IPV4 |                       \
   RTE_MBUF_F_TX_OUTER_IP_CKSUM | RTE_MBUF_F_TX_VLAN | RTE_MBUF_F_TX_IPV6 |    \
   RTE_MBUF_F_TX_IPV4 | RTE_MBUF_F_TX_IP_CKSUM | RTE_MBUF_F_TX_L4_MASK |       \
   RTE_MBUF_F_TX_IEEE1588_TMST | RTE_MBUF_F_TX_TCP_SEG | RTE_MBUF_F_TX_QINQ |  \
   RTE_MBUF_F_TX_TUNNEL_MASK | RTE_MBUF_F_TX_MACSEC |                          \
   RTE_MBUF_F_TX_SEC_OFFLOAD | RTE_MBUF_F_TX_UDP_SEG |                         \
   RTE_MBUF_F_TX_OUTER_UDP_CKSUM)

#define RTE_MBUF_F_EXTERNAL (1ULL << 61)

#define RTE_MBUF_F_INDIRECT (1ULL << 62)
#define RTE_MBUF_PRIV_ALIGN 8

#define RTE_MBUF_DEFAULT_DATAROOM 2048
#define RTE_MBUF_DEFAULT_BUF_SIZE                                              \
  (RTE_MBUF_DEFAULT_DATAROOM + RTE_PKTMBUF_HEADROOM)

class rte_pktmbuf_pool;
struct rte_mbuf;

template <typename T, T alignment> static constexpr T align(T val) {
  return (val + alignment - 1) & ~(alignment - 1);
}

void rte_pktmbuf_free(rte_mbuf *mbuf);
void rte_mbuf_raw_free(rte_mbuf *mbuf);
void rte_pktmbuf_free_bulk(rte_mbuf **pkts, uint16_t size);
void rte_pktmbuf_read(rte_mbuf *, uint32_t, uint32_t, uint8_t *);

struct rte_mbuf {
  uint16_t l2_len, l3_len, l4_len, nb_segs, packet_type;
  uint16_t pkt_len, data_len, buf_len;
  uint64_t ol_flags;
  uintptr_t iova;
  struct {
    uint64_t rss;
  } hash;
  rte_pktmbuf_pool *pool;
  rte_mbuf *next;
  char *buf;
};

#define rte_pktmbuf_mtod(m, t) reinterpret_cast<t>(m->buf)
#define rte_free free

using rte_mempool = rte_pktmbuf_pool;
struct pageheader {
  pageheader *next;
  uintptr_t phys;
  char page[];
};

template <typename T> struct objheader {
  objheader *next;
  T obj;
};

struct Pool {
  pageheader *memory;
  objheader<rte_mbuf> *objs;
  uint64_t elems, inuse, alloc_size;

  Pool(uint32_t size, uint32_t elems)
      : memory(nullptr), objs(nullptr), elems(elems), inuse(0) {
    alloc_size = align<uint64_t, 64>(size + sizeof(objheader<rte_mbuf>));
    const uint32_t per_page = mmu::huge_page_size - sizeof(pageheader);
    uint32_t offset = per_page;
    for (uint32_t i = 0; i < elems; ++i) {
      if (per_page - offset < alloc_size) {
        auto *page = static_cast<pageheader *>(
            memory::alloc_huge_page(mmu::huge_page_size));
        offset = 0;
        page->next = memory;
        memory = page;
        page->phys = mmu::virt_to_phys(page) + sizeof(pageheader);
      }
      assert(memory && ((memory->phys - sizeof(pageheader)) & (mmu::huge_page_size - 1)) == 0);
      auto *obj =
          reinterpret_cast<objheader<rte_mbuf> *>(memory->page + offset);
      new (&obj->obj) rte_mbuf{};
      obj->obj.iova = memory->phys + offset + sizeof(objheader<rte_mbuf>);
      obj->obj.buf_len = size;
      obj->next = objs;
      objs = obj;
      offset += alloc_size;
    }
  }
  ~Pool() {
    for (auto *it = memory; it;) {
      auto *to_free = it;
      it = it->next;
      memory::free_huge_page(to_free, mmu::huge_page_size);
    }
  }
};
class rte_pktmbuf_pool {
  static constexpr uint32_t cache_size = 256;

public:
  rte_pktmbuf_pool(const char *name, uint32_t size, uint32_t elems,
                   uint32_t flags)
      : pool(size, elems), data_size(elems) {
    (void)name;
    (void)flags;
  }

  ~rte_pktmbuf_pool();

  rte_pktmbuf_pool(const rte_pktmbuf_pool &) = delete;
  rte_pktmbuf_pool(rte_pktmbuf_pool &&) noexcept = delete;

  static rte_pktmbuf_pool *rte_pktmbuf_pool_create(const char *name,
                                                   uint32_t size,
                                                   uint32_t elems,
                                                   uint32_t flags);
  static void rte_pktmbuf_pool_delete(rte_pktmbuf_pool *pb_pool);

  int alloc_bulk(rte_mbuf **pkts, uint16_t nb);
  void free_bulk(rte_mbuf **pkts, uint16_t nb);

  uint64_t get_stat() const { return malloc_stat; }
  uint32_t get_data_size() const { return data_size; }

  template <typename F> void init(F &&fun) {
    for (auto &m : pool.header)
      fun(m);
  }

private:
  struct PoolImpl {
    static constexpr uint32_t cl_size = 64;
    std::vector<rte_mbuf *> header;
    pageheader *pages;
    uint64_t head;
    PoolImpl(uint32_t size, uint32_t elems)
        : header(elems, nullptr), pages(nullptr), head(0) {
      uint64_t offset = mmu::huge_page_size;
      uint64_t alloc_size = align<uint64_t, 64>(sizeof(rte_mbuf) + size); 
      uint32_t i = 0;
      for (auto &m : header) {
        if(mmu::huge_page_size - offset < alloc_size){
            auto *page = memory::alloc_huge_page(mmu::huge_page_size);
            auto *header = static_cast<pageheader*>(page);
            header->next = pages;
            header->phys = mmu::virt_to_phys(page);
            assert(header->phys != 0);
            pages = header;
            offset = sizeof(pageheader);
        }  
        auto *data = reinterpret_cast<char*>(pages);
        m = reinterpret_cast<rte_mbuf*>(data + offset);
        m->buf = data + offset + sizeof(rte_mbuf);
        m->iova = pages->phys + offset + sizeof(rte_mbuf);
        ++i;
        offset += alloc_size;
      }
    }
    rte_mbuf *get() { return header[head++]; }
    void put(rte_mbuf *m) { header[--head] = m; }
    bool can(uint16_t nb) { return (header.size() - head) >= nb; }
    ~PoolImpl() {
        for(auto *it = pages; it; ){
            auto *header = it;
            it = it->next;
            memory::free_huge_page(header, mmu::huge_page_size);
        }
    }
  } pool;
  uint32_t data_size;
  uint64_t malloc_stat = 0;
};

#endif // !BYPASS_MEM_H
