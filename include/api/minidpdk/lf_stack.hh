// SPDX-License-Identifier: BSD-3-Clause
// Copyright(c) 2019 Intel Corporation (original DPDK C implementation)
//
// Standalone C++ port of the DPDK lock-free stack (librte_stack, LF variant).

#ifndef LF_STACK_HH
#define LF_STACK_HH

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>

#include <minidpdk/util.hh>

static inline constexpr std::size_t rte_cache_line_roundup(std::size_t n) {
  return ((n + RTE_CACHE_LINE_SIZE - 1) / RTE_CACHE_LINE_SIZE) *
         RTE_CACHE_LINE_SIZE;
}

#define RTE_STACK_F_LF 0x0001

struct rte_stack_lf_elem {
  void *data;
  rte_stack_lf_elem *next;
};

struct alignas(16) rte_stack_lf_head {
  rte_stack_lf_elem *top;
  uint64_t cnt;
};

struct rte_stack_lf_list {
  std::atomic<rte_stack_lf_head> head;
  std::atomic<uint64_t> len;

  void push_elems(rte_stack_lf_elem *first, rte_stack_lf_elem *last,
                  unsigned int num) {
    rte_stack_lf_head old_head = head.load(std::memory_order_relaxed);
    rte_stack_lf_head new_head;

    do {
      new_head.top = first;
      new_head.cnt = old_head.cnt + 1;

      last->next = old_head.top;

    } while (!head.compare_exchange_weak(old_head, new_head,
                                         std::memory_order_release,
                                         std::memory_order_relaxed));
    len.fetch_add(num, std::memory_order_release);
  }

  rte_stack_lf_elem *pop_elems(unsigned int num, void **obj_table,
                               rte_stack_lf_elem **last) {
    rte_stack_lf_head old_head;

    uint64_t cur_len = len.load(std::memory_order_relaxed);

    while (true) {
      if (unlikely(cur_len < num))
        return nullptr;

      if (len.compare_exchange_weak(cur_len, cur_len - num,
                                    std::memory_order_acquire,
                                    std::memory_order_relaxed))
        break;
    }

    old_head = head.load(std::memory_order_relaxed);
    bool success = false;
    do {
      rte_stack_lf_head new_head;
      rte_stack_lf_elem *tmp;
      unsigned int i;

      std::atomic_thread_fence(std::memory_order_acquire);
      rte_prefetch0(old_head.top);
      tmp = old_head.top;

      for (i = 0; i < num && tmp != nullptr; i++) {
        rte_prefetch0(tmp->next);
        if (obj_table)
          obj_table[i] = tmp->data;
        if (last)
          *last = tmp;
        tmp = tmp->next;
      }

      if (i != num) {
        old_head = head.load(std::memory_order_relaxed);
        continue;
      }

      new_head.top = tmp;
      new_head.cnt = old_head.cnt + 1;

      success = head.compare_exchange_strong(old_head, new_head,
                                             std::memory_order_relaxed,
                                             std::memory_order_relaxed);
    } while (!success);

    return old_head.top;
  }
};

struct rte_stack_lf {
  alignas(RTE_CACHE_LINE_SIZE) rte_stack_lf_list used;
  alignas(RTE_CACHE_LINE_SIZE) rte_stack_lf_list free;
  alignas(RTE_CACHE_LINE_SIZE) rte_stack_lf_elem elems[];

  unsigned int push(void *const *obj_table, unsigned int n) {
    rte_stack_lf_elem *tmp, *first, *last = nullptr;

    if (unlikely(n == 0))
      return 0;

    first = free.pop_elems(n, nullptr, &last);
    if (unlikely(first == nullptr))
      return 0;

    unsigned int i;
    for (tmp = first, i = 0; i < n; i++, tmp = tmp->next)
      tmp->data = obj_table[n - i - 1];

    used.push_elems(first, last, n);

    return n;
  }

  unsigned int pop(void **obj_table, unsigned int n) {
    rte_stack_lf_elem *first, *last = nullptr;
    if (unlikely(n == 0))
      return 0;
    first = used.pop_elems(n, obj_table, &last);
    if (unlikely(first == nullptr))
      return 0;
    free.push_elems(first, last, n);
    return n;
  }

  unsigned int count() const {
    return static_cast<unsigned int>(used.len.load(std::memory_order_relaxed));
  }
};

struct rte_stack {
  uint32_t capacity;
  uint32_t flags;
  alignas(RTE_CACHE_LINE_SIZE) rte_stack_lf stack_lf;

  unsigned int push(void *const *obj_table, unsigned int n) {
    return stack_lf.push(obj_table, n);
  }

  unsigned int pop(void **obj_table, unsigned int n) {
    return stack_lf.pop(obj_table, n);
  }

  unsigned int count() const { return stack_lf.count(); }

  unsigned int free_count() const { return capacity - count(); }
};

static inline std::size_t rte_stack_lf_get_memsize(unsigned int count) {
  size_t sz = sizeof(rte_stack);
  sz += rte_cache_line_roundup(count * sizeof(rte_stack_lf_elem));
  sz += 2 * RTE_CACHE_LINE_SIZE;

  return sz;
}

static inline void rte_stack_lf_init(rte_stack *s, unsigned int count) {
  rte_stack_lf_elem *elems = s->stack_lf.elems;

  for (unsigned int i = 0; i < count; i++)
    s->stack_lf.free.push_elems(&elems[i], &elems[i], 1);
}

static inline rte_stack *rte_stack_create(unsigned int count,
                                          uint32_t flags = RTE_STACK_F_LF) {
  void *mem = nullptr;
  size_t sz = rte_stack_lf_get_memsize(count);

  if (posix_memalign(&mem, alignof(rte_stack), sz) != 0)
    return nullptr;
  rte_stack *s = new (mem) rte_stack;

  rte_stack_lf_head empty_head{nullptr, 0};
  s->stack_lf.used.head.store(empty_head, std::memory_order_relaxed);
  s->stack_lf.used.len.store(0, std::memory_order_relaxed);
  s->stack_lf.free.head.store(empty_head, std::memory_order_relaxed);
  s->stack_lf.free.len.store(0, std::memory_order_relaxed);
  s->capacity = count;
  s->flags = flags;
  rte_stack_lf_init(s, count);
  return s;
}

static inline void rte_stack_free(rte_stack *s) {
  if (s != nullptr)
    s->~rte_stack();
  std::free(s);
}

static inline unsigned int rte_stack_push(rte_stack *s, void *const *obj_table,
                                          unsigned int n) {
  return s->push(obj_table, n);
}

static inline unsigned int rte_stack_pop(rte_stack *s, void **obj_table,
                                         unsigned int n) {
  return s->pop(obj_table, n);
}

static inline unsigned int rte_stack_count(rte_stack *s) { return s->count(); }

static inline unsigned int rte_stack_free_count(rte_stack *s) {
  return s->free_count();
}

#endif
