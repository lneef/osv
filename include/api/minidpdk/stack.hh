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

  unsigned int push(void *const *obj_table, unsigned int n);

  unsigned free_space() const{
      return capacity - head;
  }

  unsigned size() const{
      return head;
  }

  // Defined out-of-line in core/mem.cc (see push() above for rationale).
  unsigned int pop(void **obj_table, unsigned int n);
};
