#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>

#include <osv/mmu.hh>
#include <osv/mutex.h>
#include <osv/pagealloc.hh>

namespace minidpdk {

struct alignas(64) page_header {
  page_header *next;
  page_header *prev;
  uintptr_t iova;
  size_t free;

  static void list_remove(page_header *s) {
    s->prev->next = s->next;
    s->next->prev = s->prev;
  }

  page_header() : next(nullptr), prev(nullptr), free(0) {}
};

static_assert(sizeof(page_header) % 64 == 0, "");
class page_store {
public:
  static constexpr size_t page_size = mmu::huge_page_size;

  struct page_list {
    page_header head, tail;
    page_list() : head(), tail() {
      head.next = &tail;
      tail.prev = &head;
    }

    void list_push(page_header *s) {
      s->next = head.next;
      s->prev = &head;
      head.next->prev = s;
      head.next = s;
    }

    bool empty() const { return head.next == &tail; }

    page_header *front() { return head.next; }
  };

  page_store() : regions() {}

  page_header *alloc_region() {
    SCOPE_LOCK(lock_);
    auto *region = memory::alloc_huge_page(page_size);
    assert(region != nullptr);
    auto *s = new (region) page_header();
    s->iova = mmu::virt_to_phys(s);
    s->free = page_size - sizeof(page_header);
    regions.list_push(s);
    return s;
  }

  ~page_store() {
    auto *s = regions.head.next;
    while (s != &regions.tail) {
      auto *next = s->next;
      memory::free_huge_page(s, page_size);
      s = next;
    }
  }

  static std::shared_ptr<page_store> instance() {
    static std::shared_ptr<page_store> inst = std::make_shared<page_store>();
    return inst;
  }

private:
  page_list regions;
  mutex lock_;
};

} // namespace minidpdk
