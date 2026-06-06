#pragma once

#include <minidpdk/util.hh>
#include <osv/preempt-lock.hh>
#include <osv/sched.hh>

struct stack {
  size_t capacity;
  size_t head = 0;
  void *objs[];

  stack(size_t size) : capacity(size), head(0) {}

  static constexpr size_t memsize(size_t size) {
    return sizeof(stack) + size * sizeof(void *);
  }

  static stack *create(size_t size) {
    return new (::operator new(memsize(size))) stack(size);
  }

  static void destroy(stack *s) {
    s->~stack();
    ::operator delete(s);
  }
 
  __attribute__((optimize("no-tree-loop-distribute-patterns")))
  unsigned int push(void *const *obj_table, unsigned int n) {
    WITH_LOCK(preempt_lock) {
      if (unlikely(capacity - head < n))
        return 0;
      for (unsigned i = 0; i < n; ++i)
        objs[head + i] = obj_table[i];
      head += n;
    }
    return n;
  }

  unsigned free_space() const{
      return capacity - head;
  }

  unsigned size() const{
      return head;
  }

  unsigned int pop(void **obj_table, unsigned int n) {
    WITH_LOCK(preempt_lock) {
      if (unlikely(head < n))
        return 0;
      for(unsigned i = 0; i < n; ++i)
          obj_table[n - i - 1] = objs[head - n + i];
      head -= n;
    }
    return n;
  }
};
