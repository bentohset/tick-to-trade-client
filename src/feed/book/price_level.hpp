#pragma once

#include "feed/book/order_pool.hpp"
#include "feed/book/types.hpp"
namespace ttt::book {

// One price on one side: totals plus orders in time priority
// a doubly linked list with Order::prev/next
struct PriceLevel {
  Price price = 0;
  uint32_t count = 0;
  uint64_t qty = 0;
  uint32_t head = kNil; // pool indices pointing to oldest order
  uint32_t tail = kNil; // newest order

  // add a new order to back of queue
  void push_back(uint32_t idx, OrderPool& pool) {
    Order& o = pool[idx]; // new order reference
    o.prev = tail;
    o.next = kNil;
    if (tail != kNil) {
      // level already has orders, the old last order point to this order
      pool[tail].next = idx;
    } else {
      // level is empty, this order is the oldest
      head = idx;
    }
    tail = idx; // this order is newest
    qty += o.qty;
    ++count;
  }

  // remove an order from idx in the queue
  void unlink(uint32_t idx, OrderPool& pool) {
    Order& o = pool[idx]; // the order being removed
    if (o.prev != kNil) {
      // if order has a predecessor, point it to the next one
      pool[o.prev].next = o.next;
    } else {
      // this order is the oldest, head move to its successor
      head = o.next;
    }
    if (o.next != kNil) {
      // order has successor, order's prev skips back to this order's predecessor
      pool[o.next].prev = o.prev;
    } else {
      // order is the newest, tail move to its predecessor
      tail = o.prev;
    }
    qty -= o.qty;
    --count;
  }

  bool empty() const { return count == 0; }
};

} // namespace ttt::book
