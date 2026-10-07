#pragma once

#include "feed/book/types.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ttt::book {

inline constexpr uint32_t kNil = UINT32_MAX;

struct Order {
  OrderRef ref;
  Price price;
  Qty qty;
  // FIFO links within price level (pool indices, not pointers)
  uint32_t prev;
  uint32_t next;
  uint16_t locate;
  Side side;
};
static_assert(sizeof(Order) == 32);

// Preallocated orders addressed by index. alloc/free O(1). no system calls.
class OrderPool {
public:
  explicit OrderPool(uint32_t capacity) : slots_(capacity) {
    // value-init touches every page
    for (uint32_t i = 0; i < capacity; ++i) {
      slots_[i].next = i + 1 < capacity ? i + 1 : kNil;
    }
  }

  // allocates an index for a new order.
  // kNil when full - caller counts and drops the add
  uint32_t alloc() {
    const uint32_t i = free_head_;
    if (i == kNil) return kNil;
    free_head_ = slots_[i].next;
    ++live_;
    return i;
  }

  // Frees the next slot
  // LIFO: slot just freed is the next one handed out, so its still in cache.
  // A replace frees the old order then allocates the new one into the same slot
  void free(uint32_t i) {
    slots_[i].next = free_head_;
    free_head_ = i;
    --live_;
  }

  Order& operator[](uint32_t i) { return slots_[i]; }
  const Order& operator[](uint32_t i) const { return slots_[i]; }
  uint32_t live() const { return live_; }

private:
  std::vector<Order> slots_;
  uint32_t free_head_{0};
  uint32_t live_{0};
};

} // namespace ttt::book
