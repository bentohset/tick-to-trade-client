#include "feed/book/order_book.hpp"
#include "feed/book/order_pool.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <numeric>
#include <optional>
#include <random>
#include <vector>

namespace ttt::book {
namespace {

// Prices are ITCH Price(4): 4 implied decimals, so 1000000 = $100.0000
constexpr Price px(double dollars) { return static_cast<Price>(dollars * 10'000 + 0.5); }

void expect_level(const std::optional<Level>& level, Price price, uint64_t qty, uint32_t orders) {
  ASSERT_TRUE(level.has_value());
  EXPECT_EQ(level->price, price);
  EXPECT_EQ(level->qty, qty);
  EXPECT_EQ(level->orders, orders);
}

void expect_level(const Level& level, Price price, uint64_t qty, uint32_t orders) {
  expect_level(std::optional<Level>{level}, price, qty, orders);
}

// Owns a small pool and one book. add() does what BookManager does: allocate a
// slot, fill in the order, hand the index to the book.
class OrderBookTest : public ::testing::Test {
protected:
  uint32_t add(Side side, Price price, Qty qty) {
    const uint32_t idx = pool.alloc();
    EXPECT_NE(idx, kNil);
    Order& o = pool[idx];
    o.ref = next_ref_++;
    o.price = price;
    o.qty = qty;
    o.locate = 1;
    o.side = side;
    book.add(idx);
    return idx;
  }

  // As BookManager does: unlink from the book, then free the slot
  void remove(uint32_t idx) {
    book.remove(idx);
    pool.free(idx);
  }

  OrderPool pool{4096};
  OrderBook book{&pool};

private:
  OrderRef next_ref_ = 1;
};

// --- empty book -----------------------------------------------------------------------

TEST_F(OrderBookTest, EmptyBookHasNoLevels) {
  EXPECT_FALSE(book.best_bid().has_value());
  EXPECT_FALSE(book.best_ask().has_value());
  EXPECT_TRUE(book.depth(Side::Buy, 10).empty());
  EXPECT_TRUE(book.depth(Side::Sell, 10).empty());
  EXPECT_FALSE(book.crossed());
}

// --- add ------------------------------------------------------------------------------

TEST_F(OrderBookTest, AddBidSetsBestBidOnly) {
  add(Side::Buy, px(100.00), 300);
  expect_level(book.best_bid(), px(100.00), 300, 1);
  EXPECT_FALSE(book.best_ask().has_value());
}

TEST_F(OrderBookTest, AddAskSetsBestAskOnly) {
  add(Side::Sell, px(100.05), 200);
  expect_level(book.best_ask(), px(100.05), 200, 1);
  EXPECT_FALSE(book.best_bid().has_value());
}

TEST_F(OrderBookTest, OrdersAtSamePriceAggregateIntoOneLevel) {
  add(Side::Buy, px(100.00), 100);
  add(Side::Buy, px(100.00), 250);
  add(Side::Buy, px(100.00), 50);
  expect_level(book.best_bid(), px(100.00), 400, 3);
  EXPECT_EQ(book.depth(Side::Buy, 10).size(), 1u);
}

TEST_F(OrderBookTest, SamePriceOnBothSidesAreSeparateLevels) {
  add(Side::Buy, px(100.00), 100);
  add(Side::Sell, px(100.00), 200);
  expect_level(book.best_bid(), px(100.00), 100, 1);
  expect_level(book.best_ask(), px(100.00), 200, 1);
}

TEST_F(OrderBookTest, LevelQuantityCanExceed32Bits) {
  // Qty is 32-bit per order, but a level's total is 64-bit
  add(Side::Buy, px(1.00), 3'000'000'000u);
  add(Side::Buy, px(1.00), 3'000'000'000u);
  expect_level(book.best_bid(), px(1.00), 6'000'000'000ull, 2);
}

// --- ordering / depth -----------------------------------------------------------------

TEST_F(OrderBookTest, BidsAreHighestFirst) {
  add(Side::Buy, px(99.98), 100);
  add(Side::Buy, px(100.00), 200);
  add(Side::Buy, px(99.99), 300);

  const auto levels = book.depth(Side::Buy, 10);
  ASSERT_EQ(levels.size(), 3u);
  expect_level(levels[0], px(100.00), 200, 1);
  expect_level(levels[1], px(99.99), 300, 1);
  expect_level(levels[2], px(99.98), 100, 1);
  expect_level(book.best_bid(), px(100.00), 200, 1);
}

TEST_F(OrderBookTest, AsksAreLowestFirst) {
  add(Side::Sell, px(100.03), 100);
  add(Side::Sell, px(100.01), 200);
  add(Side::Sell, px(100.02), 300);

  const auto levels = book.depth(Side::Sell, 10);
  ASSERT_EQ(levels.size(), 3u);
  expect_level(levels[0], px(100.01), 200, 1);
  expect_level(levels[1], px(100.02), 300, 1);
  expect_level(levels[2], px(100.03), 100, 1);
  expect_level(book.best_ask(), px(100.01), 200, 1);
}

TEST_F(OrderBookTest, NewWorstAndNewBestPricesGoToTheEnds) {
  add(Side::Buy, px(100.00), 100);
  add(Side::Buy, px(99.00), 100);  // worse than everything: front of the vector
  add(Side::Buy, px(101.00), 100); // better than everything: back of the vector

  const auto levels = book.depth(Side::Buy, 10);
  ASSERT_EQ(levels.size(), 3u);
  EXPECT_EQ(levels[0].price, px(101.00));
  EXPECT_EQ(levels[1].price, px(100.00));
  EXPECT_EQ(levels[2].price, px(99.00));
}

TEST_F(OrderBookTest, DepthIsLimitedToN) {
  for (int i = 0; i < 5; ++i) add(Side::Buy, px(100.00) - static_cast<Price>(i * 100), 100);

  const auto top2 = book.depth(Side::Buy, 2);
  ASSERT_EQ(top2.size(), 2u);
  EXPECT_EQ(top2[0].price, px(100.00));
  EXPECT_EQ(top2[1].price, px(99.99));

  EXPECT_TRUE(book.depth(Side::Buy, 0).empty());
  EXPECT_EQ(book.depth(Side::Buy, 100).size(), 5u); // n larger than the book
}

TEST_F(OrderBookTest, DepthReturnsACopy) {
  add(Side::Buy, px(100.00), 100);
  const auto before = book.depth(Side::Buy, 10);

  add(Side::Buy, px(100.00), 900);

  ASSERT_EQ(before.size(), 1u);
  EXPECT_EQ(before[0].qty, 100u); // unaffected by later changes
}

// --- deep books: levels beyond the linear scan use binary search ----------------------

TEST_F(OrderBookTest, ManyLevelsInRandomOrderStaySorted) {
  // 200 bid and 200 ask prices, added in shuffled order
  std::vector<Price> prices(200);
  std::iota(prices.begin(), prices.end(), Price{1});
  std::mt19937 rng(42);
  std::shuffle(prices.begin(), prices.end(), rng);
  for (const Price p : prices) {
    add(Side::Buy, p * 100, 10);
    add(Side::Sell, 1'000'000 + p * 100, 10);
  }

  const auto bids = book.depth(Side::Buy, 1000);
  const auto asks = book.depth(Side::Sell, 1000);
  ASSERT_EQ(bids.size(), 200u);
  ASSERT_EQ(asks.size(), 200u);
  for (std::size_t i = 1; i < bids.size(); ++i) EXPECT_GT(bids[i - 1].price, bids[i].price);
  for (std::size_t i = 1; i < asks.size(); ++i) EXPECT_LT(asks[i - 1].price, asks[i].price);
  EXPECT_EQ(bids.front().price, 200u * 100);
  EXPECT_EQ(asks.front().price, 1'000'000u + 100);
}

TEST_F(OrderBookTest, DeepLevelsAreFoundForAddReduceAndRemove) {
  // Deep enough that the bottom levels are only reachable by binary search
  std::vector<uint32_t> idx;
  for (Price p = 1; p <= 100; ++p) idx.push_back(add(Side::Buy, p * 100, 10));

  const uint32_t deep = idx[2]; // price 300: the 98th level from the top
  add(Side::Buy, 300, 5);       // joins the existing deep level
  book.reduce(deep, 4);         // partial reduce on a deep level
  auto levels = book.depth(Side::Buy, 1000);
  expect_level(levels[97], 300, 11, 2);

  remove(deep);
  levels = book.depth(Side::Buy, 1000);
  expect_level(levels[97], 300, 5, 1);
  EXPECT_EQ(levels.size(), 100u);
}

// --- reduce / remove ------------------------------------------------------------------

TEST_F(OrderBookTest, PartialReduceKeepsOrderAndUpdatesBothQuantities) {
  // Partial execute / cancel: shares leave, the order stays
  const uint32_t idx = add(Side::Buy, px(100.00), 500);
  book.reduce(idx, 200);
  expect_level(book.best_bid(), px(100.00), 300, 1);
  EXPECT_EQ(pool[idx].qty, 300u);
}

TEST_F(OrderBookTest, RemovingOneOfSeveralOrdersKeepsLevel) {
  const uint32_t first = add(Side::Sell, px(100.05), 100);
  add(Side::Sell, px(100.05), 300);
  remove(first);
  expect_level(book.best_ask(), px(100.05), 300, 1);
}

TEST_F(OrderBookTest, RemovingLastOrderErasesLevel) {
  const uint32_t idx = add(Side::Buy, px(100.00), 100);
  remove(idx);
  EXPECT_FALSE(book.best_bid().has_value());
  EXPECT_TRUE(book.depth(Side::Buy, 10).empty());
}

TEST_F(OrderBookTest, RemovedOrderUsesItsRemainingQuantity) {
  const uint32_t idx = add(Side::Buy, px(100.00), 500);
  add(Side::Buy, px(100.00), 100);
  book.reduce(idx, 450); // 50 left
  remove(idx);           // takes the remaining 50, not the original 500
  expect_level(book.best_bid(), px(100.00), 100, 1);
}

TEST_F(OrderBookTest, BestMovesToNextLevelWhenTopEmpties) {
  const uint32_t top_bid = add(Side::Buy, px(100.00), 100);
  add(Side::Buy, px(99.99), 200);
  const uint32_t top_ask = add(Side::Sell, px(100.01), 300);
  add(Side::Sell, px(100.02), 400);

  remove(top_bid);
  remove(top_ask);

  expect_level(book.best_bid(), px(99.99), 200, 1);
  expect_level(book.best_ask(), px(100.02), 400, 1);
}

TEST_F(OrderBookTest, ReduceOnlyAffectsItsSideAndPrice) {
  const uint32_t idx = add(Side::Buy, px(100.00), 100);
  add(Side::Buy, px(99.99), 100);
  add(Side::Sell, px(100.00), 100); // same price, other side

  book.reduce(idx, 40);

  expect_level(book.best_bid(), px(100.00), 60, 1);
  expect_level(book.depth(Side::Buy, 2)[1], px(99.99), 100, 1);
  expect_level(book.best_ask(), px(100.00), 100, 1);
}

TEST_F(OrderBookTest, ExecuteThenCancelRestOfOrder) {
  // One order's lifecycle: partial fill ('E'), then cancel the rest ('D')
  const uint32_t idx = add(Side::Sell, px(50.00), 1000);
  book.reduce(idx, 300);
  remove(idx);
  EXPECT_FALSE(book.best_ask().has_value());
}

TEST_F(OrderBookTest, ReAddingAFreedSlotWorks) {
  // The pool is LIFO: a replace frees a slot and the new order reuses it
  const uint32_t idx = add(Side::Buy, px(100.00), 100);
  remove(idx);
  const uint32_t again = add(Side::Sell, px(101.00), 200);
  EXPECT_EQ(again, idx);
  EXPECT_FALSE(book.best_bid().has_value());
  expect_level(book.best_ask(), px(101.00), 200, 1);
}

// --- crossed --------------------------------------------------------------------------

TEST_F(OrderBookTest, NormalSpreadIsNotCrossed) {
  add(Side::Buy, px(100.00), 100);
  add(Side::Sell, px(100.01), 100);
  EXPECT_FALSE(book.crossed());
}

TEST_F(OrderBookTest, LockedBookIsNotCrossed) {
  // bid == ask is "locked", which is allowed
  add(Side::Buy, px(100.00), 100);
  add(Side::Sell, px(100.00), 100);
  EXPECT_FALSE(book.crossed());
}

TEST_F(OrderBookTest, BidAboveAskIsCrossed) {
  add(Side::Buy, px(100.02), 100);
  add(Side::Sell, px(100.01), 100);
  EXPECT_TRUE(book.crossed());
}

TEST_F(OrderBookTest, OneSidedBookIsNotCrossed) {
  add(Side::Buy, px(100.00), 100);
  EXPECT_FALSE(book.crossed());
}

TEST_F(OrderBookTest, UncrossesWhenCrossingLevelIsRemoved) {
  add(Side::Buy, px(100.00), 100);
  const uint32_t crossing = add(Side::Buy, px(100.05), 100);
  add(Side::Sell, px(100.02), 100);
  ASSERT_TRUE(book.crossed());

  remove(crossing);
  EXPECT_FALSE(book.crossed());
}

// --- programming errors (asserts are compiled out in Release) -------------------------

#ifndef NDEBUG
using OrderBookDeathTest = OrderBookTest;

TEST_F(OrderBookDeathTest, RemoveOrderNotInBookAsserts) {
  add(Side::Buy, px(100.00), 100);
  // A pool slot filled in but never added to the book: no level at its price
  const uint32_t idx = pool.alloc();
  pool[idx].price = px(99.00);
  pool[idx].qty = 100;
  pool[idx].side = Side::Buy;
  EXPECT_DEATH(book.remove(idx), "no level");
}

TEST_F(OrderBookDeathTest, ReduceThatWouldEmptyTheOrderAsserts) {
  // Full reductions must go through remove()
  const uint32_t idx = add(Side::Buy, px(100.00), 100);
  EXPECT_DEATH(book.reduce(idx, 100), "reduce");
}
#endif

} // namespace
} // namespace ttt::book
