// SPDX-License-Identifier: BSD-3-Clause
// Copyright(c) 2019 Intel Corporation (original DPDK C implementation)
//
// Standalone C++ port of the DPDK lock-free stack (librte_stack, LF variant),
// adapted for minidpdk/OSv.

#pragma once

#include <minidpdk/util.hh>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>

#if !defined(__x86_64__)
#error "minidpdk lf_stack requires x86-64"
#endif


static inline constexpr std::size_t
rte_cache_line_roundup(std::size_t n)
{
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
	std::uint64_t cnt;
};

// Relaxed 128-bit CAS: emits `lock cmpxchg16b` inline, clobbering only the
// touched location (via the "+m" operand) instead of all memory. Dropping the
// blanket "memory" clobber lets the compiler keep unrelated state (e.g. the
// retry loop's old_head) in registers across the CAS. C++ memory-order
// semantics are layered on top in lf_head_cas via __atomic_signal_fence; on
// x86 the `lock` prefix is always a full hardware barrier, so acquire/release
// only constrain compiler reordering.
static inline bool
lf_head_cas_relaxed(rte_stack_lf_head *dst, rte_stack_lf_head *expected,
		    rte_stack_lf_head desired)
{
	bool ok;
	std::uint64_t exp_lo = reinterpret_cast<std::uint64_t>(expected->top);
	std::uint64_t exp_hi = expected->cnt;

	asm volatile("lock cmpxchg16b %[dst]\n\t"
		     "sete %[ok]"
		     : [ok] "=q"(ok), [dst] "+m"(*dst),
		       "+a"(exp_lo), "+d"(exp_hi)
		     : "b"(reinterpret_cast<std::uint64_t>(desired.top)),
		       "c"(desired.cnt)
		     : "cc");

	expected->top = reinterpret_cast<rte_stack_lf_elem *>(exp_lo);
	expected->cnt = exp_hi;
	return ok;
}

static inline bool
lf_head_cas(rte_stack_lf_head *dst, rte_stack_lf_head *expected,
	    rte_stack_lf_head desired, int success, int failure)
{
	// release side: prior stores must not sink past the CAS.
	if (success == __ATOMIC_RELEASE || success == __ATOMIC_ACQ_REL ||
	    success == __ATOMIC_SEQ_CST)
		__atomic_signal_fence(__ATOMIC_RELEASE);

	bool ok = lf_head_cas_relaxed(dst, expected, desired);

	// acquire side: later loads must not hoist before the CAS.
	int order = ok ? success : failure;
	if (order == __ATOMIC_ACQUIRE || order == __ATOMIC_ACQ_REL ||
	    order == __ATOMIC_SEQ_CST)
		__atomic_signal_fence(__ATOMIC_ACQUIRE);

	return ok;
}

static inline rte_stack_lf_head
lf_head_load(rte_stack_lf_head *src, int order)
{
	rte_stack_lf_head expected{nullptr, 0};
	rte_stack_lf_head desired{nullptr, 0};

	lf_head_cas(src, &expected, desired, order, order);
	return expected;
}

static inline void
lf_head_store(rte_stack_lf_head *dst, rte_stack_lf_head desired, int order)
{
	rte_stack_lf_head expected = lf_head_load(dst, __ATOMIC_RELAXED);

	while (!lf_head_cas(dst, &expected, desired, order, __ATOMIC_RELAXED))
		;
}

struct lf_atomic_head {
	alignas(16) rte_stack_lf_head v;

	rte_stack_lf_head load(std::memory_order order = std::memory_order_seq_cst)
	{
		return lf_head_load(&v, static_cast<int>(order));
	}

	void store(rte_stack_lf_head desired,
		   std::memory_order order = std::memory_order_seq_cst)
	{
		lf_head_store(&v, desired, static_cast<int>(order));
	}

	bool compare_exchange_weak(rte_stack_lf_head &expected,
				   rte_stack_lf_head desired,
				   std::memory_order success,
				   std::memory_order failure)
	{
		return lf_head_cas(&v, &expected, desired,
				   static_cast<int>(success),
				   static_cast<int>(failure));
	}

	bool compare_exchange_strong(rte_stack_lf_head &expected,
				     rte_stack_lf_head desired,
				     std::memory_order success,
				     std::memory_order failure)
	{
		return lf_head_cas(&v, &expected, desired,
				   static_cast<int>(success),
				   static_cast<int>(failure));
	}
};

struct rte_stack_lf_list {
	lf_atomic_head head;
	std::atomic<std::uint64_t> len;

	void push_elems(rte_stack_lf_elem *first, rte_stack_lf_elem *last,
			unsigned int num)
	{
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
				     rte_stack_lf_elem **last)
	{
		rte_stack_lf_head old_head;

		std::uint64_t cur_len = len.load(std::memory_order_relaxed);

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

			success = head.compare_exchange_strong(
					old_head, new_head,
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

	unsigned int push(void *const *obj_table, unsigned int n)
	{
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

	unsigned int pop(void **obj_table, unsigned int n)
	{
		rte_stack_lf_elem *first, *last = nullptr;

		if (unlikely(n == 0))
			return 0;

		first = used.pop_elems(n, obj_table, &last);
		if (unlikely(first == nullptr))
			return 0;

		free.push_elems(first, last, n);

		return n;
	}

	unsigned int count() const
	{
		return (unsigned int)used.len.load(std::memory_order_relaxed);
	}
};

struct lf_stack {
	std::uint32_t capacity;
	std::uint32_t flags;
	alignas(RTE_CACHE_LINE_SIZE) rte_stack_lf stack_lf;

	static std::size_t memsize(unsigned int count)
	{
		std::size_t sz = sizeof(lf_stack);
		sz += rte_cache_line_roundup(count * sizeof(rte_stack_lf_elem));
		sz += 2 * RTE_CACHE_LINE_SIZE;
		return sz;
	}

	static lf_stack *create(std::size_t size,
				std::uint32_t flags = RTE_STACK_F_LF)
	{
		unsigned int count = static_cast<unsigned int>(size);
		void *mem = nullptr;

		if (posix_memalign(&mem, alignof(lf_stack), memsize(count)) != 0)
			return nullptr;
		lf_stack *s = new (mem) lf_stack;

		rte_stack_lf_head empty_head{nullptr, 0};
		s->stack_lf.used.head.store(empty_head, std::memory_order_relaxed);
		s->stack_lf.used.len.store(0, std::memory_order_relaxed);
		s->stack_lf.free.head.store(empty_head, std::memory_order_relaxed);
		s->stack_lf.free.len.store(0, std::memory_order_relaxed);
		s->capacity = count;
		s->flags = flags;

		rte_stack_lf_elem *elems = s->stack_lf.elems;
		for (unsigned int i = 0; i < count; i++)
			s->stack_lf.free.push_elems(&elems[i], &elems[i], 1);

		return s;
	}

	static void destroy(lf_stack *s)
	{
		if (s != nullptr)
			s->~lf_stack();
		std::free(s);
	}

	unsigned int push(void *const *obj_table, unsigned int n)
	{
		return stack_lf.push(obj_table, n);
	}

	unsigned int pop(void **obj_table, unsigned int n)
	{
		return stack_lf.pop(obj_table, n);
	}

	unsigned size() const
	{
		return stack_lf.count();
	}

	unsigned free_space() const
	{
		return capacity - stack_lf.count();
	}
};
