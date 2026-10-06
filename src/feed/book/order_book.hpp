#pragma once

#include "feed/book/types.hpp"

#include <functional>
#include <map>
#include <optional>
#include <vector>

namespace ttt::book {

// TODO: optimize away from naive std::map
class OrderBook {
public:
  // new order at px
  void add(Side side, Price px, Qty qty);

  // exec / cancel / delete
  void reduce(Side side, Price px, Qty, bool removes_order);

  std::optional<Level> best_bid() const;
  std::optional<Level> best_ask() const;
  // top n levels, best first
  std::vector<Level> depth(Side side, std::size_t n) const;
  // best_bid > best_ask
  bool crossed() const;

private:
  struct Agg {
    uint64_t qty = 0;
    uint32_t orders = 0;
  };
  std::map<Price, Agg, std::greater<>> bids_; // begin() = highest bid
  std::map<Price, Agg> asks_;                 // begin() = lowest ask
};

} // namespace ttt::book
