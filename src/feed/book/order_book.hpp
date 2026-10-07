#pragma once

#include "feed/book/order_pool.hpp"
#include "feed/book/price_level.hpp"
#include "feed/book/types.hpp"

#include <optional>
#include <vector>

namespace ttt::book {

class OrderBook {
public:
  explicit OrderBook(OrderPool* pool);

  void add(uint32_t idx);

  // exec / cancel / delete
  void reduce(uint32_t idx, Qty by);
  void remove(uint32_t idx);

  std::optional<Level> best_bid() const;
  std::optional<Level> best_ask() const;
  // top n levels, best first
  std::vector<Level> depth(Side side, std::size_t n) const;
  // best_bid > best_ask
  bool crossed() const;

private:
  // Sorted worse to better, best price is at the back
  std::vector<PriceLevel> bids_; // asc, back() = highest bid
  std::vector<PriceLevel> asks_; // desc, back() = lowest ask
  OrderPool* pool_;
};

} // namespace ttt::book
