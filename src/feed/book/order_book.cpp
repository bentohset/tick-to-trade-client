#include "feed/book/order_book.hpp"
#include "feed/book/order_pool.hpp"
#include "feed/book/price_level.hpp"
#include "feed/book/types.hpp"
#include <algorithm>
#include <cassert>
#include <cstddef>

namespace ttt::book {

namespace {

constexpr std::size_t kReserveLevels = 64; // per side
// levels scanned from the top before binary search
constexpr std::size_t kLinearScan = 16;

// comparators
struct BidSide {
  static bool better(Price a, Price b) { return a > b; }
};
struct AskSide {
  static bool better(Price a, Price b) { return a < b; }
};

struct LevelPos {
  std::size_t index; // the level if found
  bool found;
};

// Levels ordered worse to better, search from the back where most activity is.
// returns where a price sits in a side's vector
template <class S> LevelPos find_level(const std::vector<PriceLevel>& levels, Price px) {
  std::size_t i = levels.size();
  // walk down the best level at most kLinearScan steps and stop if the bottom is reached
  for (std::size_t scanned = 0; i > 0 && scanned < kLinearScan; --i, ++scanned) {
    const Price p = levels[i - 1].price;
    // if this level's price not better than px (equal or worse), every level above it was better
    // than px this is the point where px belongs
    if (!S::better(p, px)) {
      return p == px ? LevelPos{i - 1, true} : LevelPos{i, false};
    }
    // level is better than px, the loop moves a step deeper
  }
  if (i == 0) return {0, false};
  // scan stopped without finding the spot, so px is deep in the book; only [0, i) is unchecked
  // binary search over the rest (log2levels steps)
  const auto end = levels.begin() + static_cast<std::ptrdiff_t>(i);
  const auto it = std::lower_bound(
      levels.begin(), end, px, [](const PriceLevel& l, Price p) { return S::better(p, l.price); });
  const auto index = static_cast<std::size_t>(it - levels.begin());
  return {index, it != end && it->price == px};
}

// add an order into its level
template <class S> void add_to(std::vector<PriceLevel>& levels, uint32_t idx, OrderPool& pool) {
  const Price px = pool[idx].price; // the new order price, already set in the pool before
  const auto [i, found] = find_level<S>(levels, px);
  if (!found) {
    // create that level at position i
    // shift is cheap because new prices usually appear near the back
    levels.insert(levels.begin() + static_cast<std::ptrdiff_t>(i), PriceLevel{.price = px});
  }
  // append the order to back of its queue and update totlas
  levels[i].push_back(idx, pool);
}

// take out an order entirely
template <class S>
void remove_from(std::vector<PriceLevel>& levels, uint32_t idx, OrderPool& pool) {
  // bookmanager should call this before freeing the slot, so the order still holds price
  const auto [i, found] = find_level<S>(levels, pool[idx].price);
  assert(found && "remove: no level at order's price");
  if (!found) return;
  // remove the order from the queue in O(1)
  levels[i].unlink(idx, pool);
  // if its the last order, remove the whole level at i and shift everything back (cheap near the
  // back)
  if (levels[i].empty()) levels.erase(levels.begin() + static_cast<std::ptrdiff_t>(i));
}

// partial execute or cancel
template <class S>
void reduce_in(std::vector<PriceLevel>& levels, uint32_t idx, Qty by, OrderPool& pool) {
  Order& o = pool[idx];
  const auto [i, found] = find_level<S>(levels, o.price);
  assert(found && by < o.qty && "reduce: no level or would empty the order");
  if (!found) return;
  levels[i].qty -= by;
  o.qty -= by;
}

// returns a copy of the top level of one side (prevent modification)
std::optional<Level> best_of(const std::vector<PriceLevel>& levels) {
  if (levels.empty()) return std::nullopt;
  // best price is at the back of the vector O(1)
  const PriceLevel& l = levels.back();
  return Level{l.price, l.qty, l.count};
}

} // namespace

OrderBook::OrderBook(OrderPool* pool) : pool_(pool) {
  bids_.reserve(kReserveLevels);
  asks_.reserve(kReserveLevels);
}

void OrderBook::add(uint32_t idx) {
  if ((*pool_)[idx].side == Side::Buy) {
    add_to<BidSide>(bids_, idx, *pool_);
  } else {
    add_to<AskSide>(asks_, idx, *pool_);
  }
}

void OrderBook::remove(uint32_t idx) {
  if ((*pool_)[idx].side == Side::Buy) {
    remove_from<BidSide>(bids_, idx, *pool_);
  } else {
    remove_from<AskSide>(asks_, idx, *pool_);
  }
}

void OrderBook::reduce(uint32_t idx, Qty by) {
  if ((*pool_)[idx].side == Side::Buy) {
    reduce_in<BidSide>(bids_, idx, by, *pool_);
  } else {
    reduce_in<AskSide>(asks_, idx, by, *pool_);
  }
}

std::optional<Level> OrderBook::best_bid() const { return best_of(bids_); }
std::optional<Level> OrderBook::best_ask() const { return best_of(asks_); }

std::vector<Level> OrderBook::depth(Side side, std::size_t n) const {
  const auto& levels = side == Side::Buy ? bids_ : asks_;
  std::vector<Level> out;
  out.reserve(std::min(n, levels.size()));
  for (auto it = levels.rbegin(); it != levels.rend() && out.size() < n; ++it) {
    out.push_back(Level{it->price, it->qty, it->count});
  }
  return out;
}

bool OrderBook::crossed() const {
  // Equal prices does not count; only bid strictly above ask
  return !bids_.empty() && !asks_.empty() && bids_.back().price > asks_.back().price;
}

} // namespace ttt::book
