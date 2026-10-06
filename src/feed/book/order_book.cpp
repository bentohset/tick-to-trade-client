#include "feed/book/order_book.hpp"
#include "feed/book/types.hpp"
#include <cassert>
#include <cstddef>

namespace ttt::book {

namespace {

// bids_ and asks_ are diff map types, so per-side logic is written once
// as templates over the map type

template <class Map> void add_to(Map& side, Price px, Qty qty) {
  auto& level = side[px]; // create empty level if price is new
  level.qty += qty;
  ++level.orders;
}

template <class Map> void reduce_in(Map& side, Price px, Qty qty, bool removes_order) {
  const auto it = side.find(px);
  // BookManager only reduces orders it knows at the price they rest at
  // a missing level or invalid quantity means out of sync
  assert(it != side.end() && "reduce at a price with no level");
  if (it == side.end()) return;

  auto& level = it->second;
  assert(level.qty >= qty && "level would go negative");
  level.qty -= qty;
  if (removes_order) --level.orders;

  if (level.orders == 0) {
    assert(level.qty == 0 && "empty level still has shares");
    side.erase(it);
  }
}

template <class Map> std::optional<Level> best_of(const Map& side) {
  if (side.empty()) return std::nullopt;
  const auto& [px, level] = *side.begin(); // begin() is best price on both sides
  return Level{px, level.qty, level.orders};
}

template <class Map> std::vector<Level> top_of(const Map& side, std::size_t n) {
  std::vector<Level> out;
  out.reserve(std::min(n, side.size()));
  for (const auto& [px, level] : side) {
    if (out.size() == n) break;
    out.push_back(Level{px, level.qty, level.orders});
  }
  return out;
}

} // namespace

void OrderBook::add(Side side, Price px, Qty qty) {
  if (side == Side::Buy) {
    add_to(bids_, px, qty);
  } else {
    add_to(asks_, px, qty);
  }
}

void OrderBook::reduce(Side side, Price px, Qty qty, bool removes_order) {
  if (side == Side::Buy) {
    reduce_in(bids_, px, qty, removes_order);
  } else {
    reduce_in(asks_, px, qty, removes_order);
  }
}

std::optional<Level> OrderBook::best_bid() const { return best_of(bids_); }
std::optional<Level> OrderBook::best_ask() const { return best_of(asks_); }

std::vector<Level> OrderBook::depth(Side side, std::size_t n) const {
  return side == Side::Buy ? top_of(bids_, n) : top_of(asks_, n);
}

bool OrderBook::crossed() const {
  // Equal prices does not count; only bid strictly above ask
  return !bids_.empty() && !asks_.empty() && bids_.begin()->first > asks_.begin()->first;
}

} // namespace ttt::book
