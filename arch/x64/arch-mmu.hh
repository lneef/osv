/*
 * Copyright (C) 2013 Cloudius Systems, Ltd.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#ifndef ARCH_MMU_HH_
#define ARCH_MMU_HH_
#include <cassert>
#include <cstdint>
namespace mmu {
extern uint8_t phys_bits, virt_bits;
extern uint64_t pat_val_msr;
constexpr uint8_t rsvd_bits_used = 1;
constexpr uint8_t max_phys_bits = 52 - rsvd_bits_used;

enum mattr {
  normal = 0,
  wc = 1,
  uc = 2,
};

enum pat : uint64_t {
  PAT_UC = 0x0,       // strong uncacheable
  PAT_WC = 0x1,       // write combining
  PAT_WT = 0x4,       // write through
  PAT_WP = 0x5,       // write protected
  PAT_WB = 0x6,       // write back
  PAT_UC_MINUS = 0x7, // weak uncacheable
};
constexpr mattr mattr_default = mattr::normal;

static constexpr uint64_t PAGE_PWT = (1ull << 3);
static constexpr uint64_t PAGE_PCD = (1ull << 4);
static constexpr uint64_t PAGE_PAT = (1ull << 7);
static constexpr uint64_t PAGE_PAT_LARGE = (1ull << 12);

static constexpr uint64_t PAT_LOW =
    (mmu::pat::PAT_UC << 24) | (mmu::pat::PAT_UC_MINUS << 16) |
    (mmu::pat::PAT_WC << 8) | mmu::pat::PAT_WB;

inline uint64_t init_pat() {
  auto pat_val_msr = PAT_LOW;
  pat_val_msr |= (mmu::pat::PAT_WT << 56) | (mmu::pat::PAT_UC_MINUS << 48) |
                 (mmu::pat::PAT_WP << 40) | (mmu::pat::PAT_WB << 32);
  return pat_val_msr;
}

// Page bits for each mattr after init_pat() has reprogrammed IA32_PAT.
// PAT slot layout: 0=WB, 1=WC, 2=UC-, 3=UC. The PAT bit is 0 for all of
// these, so only PWT/PCD are encoded here; the page-size-dependent
// PAGE_PAT/PAGE_PAT_LARGE bit must be OR'd in by the caller if needed.
static constexpr uint64_t mattr_page_bits[] = {
    [mattr::normal] = 0,                    // PAT slot 0: WB
    [mattr::wc]     = PAGE_PWT,             // PAT slot 1: WC
    [mattr::uc]     = PAGE_PWT | PAGE_PCD,  // PAT slot 3: UC
};

constexpr uint64_t pte_addr_mask(bool large) {
  return ((1ull << max_phys_bits) - 1) & ~(0xfffull) &
         ~(uint64_t(large) << page_size_shift);
}

template <int N> class pt_element : public pt_element_common<N> {
public:
  constexpr pt_element() noexcept : pt_element_common<N>(0) {}
  explicit pt_element(u64 x) noexcept : pt_element_common<N>(x) {}

  void set_pwt(bool v) { pt_element_common<N>::set_bit(3, v); }
  void set_pcd(bool v) { pt_element_common<N>::set_bit(4, v); }

  void set_pat(bool v) {
    pt_element_common<N>::set_bit(
        pt_level_traits<N>::large_capable::value ? 12 : 7, v);
  }

  pt_element &operator|=(u64 bits) {
    pt_element_common<N>::x |= bits;
    return *this;
  }
  pt_element operator|(u64 bits) const {
    pt_element r(*this);
    r |= bits;
    return r;
  }
};

/* common interface implementation */

template <int N> inline bool pt_element_common<N>::empty() const { return !x; }
template <int N> inline bool pt_element_common<N>::valid() const {
  return x & 1;
}
template <int N> inline bool pt_element_common<N>::writable() const {
  return x & 2;
}
template <int N> inline bool pt_element_common<N>::executable() const {
  return !(x >> 63);
} /* NX */
template <int N> inline bool pt_element_common<N>::dirty() const {
  static_assert(pt_level_traits<N>::leaf_capable::value,
                "only leaf pte can be dirty");
  assert(!pt_level_traits<N>::large_capable::value || large());
  return x & 0x40;
}
template <int N> inline bool pt_element_common<N>::large() const {
  return pt_level_traits<N>::large_capable::value && (x & 0x80);
}
template <int N> inline bool pt_element_common<N>::user() { return x & 4; }
template <int N> inline bool pt_element_common<N>::accessed() {
  return x & 0x20;
}

template <int N> inline bool pt_element_common<N>::sw_bit(unsigned off) const {
  assert(off < 10);
  return (x >> (53 + off)) & 1;
}

template <int N>
inline bool pt_element_common<N>::rsvd_bit(unsigned off) const {
  assert(off < rsvd_bits_used);
  return (x >> (51 - off)) & 1;
}

template <int N> inline phys pt_element_common<N>::addr() const {
  return x & pte_addr_mask(large());
}

template <int N> inline u64 pt_element_common<N>::pfn() const {
  return addr() >> page_size_shift;
}

template <int N> inline phys pt_element_common<N>::next_pt_addr() const {
  assert(!large());
  return addr();
}
template <int N> inline u64 pt_element_common<N>::next_pt_pfn() const {
  assert(!large());
  return pfn();
}

template <int N> inline void pt_element_common<N>::set_valid(bool v) {
  set_bit(0, v);
}
template <int N> inline void pt_element_common<N>::set_writable(bool v) {
  set_bit(1, v);
}
template <int N> inline void pt_element_common<N>::set_executable(bool v) {
  set_bit(63, !v);
} /* NX */
template <int N> inline void pt_element_common<N>::set_dirty(bool v) {
  set_bit(6, v);
}
template <int N> inline void pt_element_common<N>::set_large(bool v) {
  set_bit(7, v);
}
template <int N> inline void pt_element_common<N>::set_user(bool v) {
  set_bit(2, v);
}
template <int N> inline void pt_element_common<N>::set_accessed(bool v) {
  set_bit(5, v);
}

template <int N>
inline void pt_element_common<N>::set_sw_bit(unsigned off, bool v) {
  assert(off < 10);
  set_bit(53 + off, v);
}

template <int N>
inline void pt_element_common<N>::set_rsvd_bit(unsigned off, bool v) {
  assert(off < rsvd_bits_used);
  set_bit(51 - off, v);
}

template <int N>
inline void pt_element_common<N>::set_addr(phys addr, bool large) {
  x = (x & ~pte_addr_mask(large)) | addr;
}

template <int N>
inline void pt_element_common<N>::set_pfn(u64 pfn, bool large) {
  set_addr(pfn << page_size_shift, large);
}

// Currently mem_attr is ignored on x86_64
template <int N>
pt_element<N> make_pte(phys addr, bool leaf, unsigned perm = perm_rwx,
                       mattr mem_attr = mattr_default) {
  assert(pt_level_traits<N>::leaf_capable::value || !leaf);
  bool large = pt_level_traits<N>::large_capable::value && leaf;
  pt_element<N> pte;

  pte.set_valid(perm != 0);
  pte.set_writable(perm & perm_write);
  pte.set_executable(perm & perm_exec);
  pte.set_dirty(true);
  pte.set_large(large);
  pte.set_addr(addr, large);
  pte.set_user(true);
  pte.set_accessed(true);
  if(leaf)
      pte |= mattr_page_bits[static_cast<uint64_t>(mem_attr)];
  return pte;
}

// On Intel x86_64 architecture which comes with strong memory model
// it is not necessary to do anything extra after writes to page
// tables entries to make them visible to page table walker.
inline void synchronize_page_table_modifications() {}

} // namespace mmu
#endif /* ARCH_MMU_HH_ */
