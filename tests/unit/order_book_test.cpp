#include "feed/book/order_book.hpp"

#include <gtest/gtest.h>

#include <cstdint>

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

// --- empty book -----------------------------------------------------------------------

TEST(OrderBook, EmptyBookHasNoLevels) {
  OrderBook book;
  EXPECT_FALSE(book.best_bid().has_value());
  EXPECT_FALSE(book.best_ask().has_value());
  EXPECT_TRUE(book.depth(Side::Buy, 10).empty());
  EXPECT_TRUE(book.depth(Side::Sell, 10).empty());
  EXPECT_FALSE(book.crossed());
}

// --- add ------------------------------------------------------------------------------

TEST(OrderBook, AddBidSetsBestBidOnly) {
  OrderBook book;
  book.add(Side::Buy, px(100.00), 300);
  expect_level(book.best_bid(), px(100.00), 300, 1);
  EXPECT_FALSE(book.best_ask().has_value());
}

TEST(OrderBook, AddAskSetsBestAskOnly) {
  OrderBook book;
  book.add(Side::Sell, px(100.05), 200);
  expect_level(book.best_ask(), px(100.05), 200, 1);
  EXPECT_FALSE(book.best_bid().has_value());
}

TEST(OrderBook, OrdersAtSamePriceAggregateIntoOneLevel) {
  OrderBook book;
  book.add(Side::Buy, px(100.00), 100);
  book.add(Side::Buy, px(100.00), 250);
  book.add(Side::Buy, px(100.00), 50);
  expect_level(book.best_bid(), px(100.00), 400, 3);
  EXPECT_EQ(book.depth(Side::Buy, 10).size(), 1u);
}

TEST(OrderBook, SamePriceOnBothSidesAreSeparateLevels) {
  OrderBook book;
  book.add(Side::Buy, px(100.00), 100);
  book.add(Side::Sell, px(100.00), 200);
  expect_level(book.best_bid(), px(100.00), 100, 1);
  expect_level(book.best_ask(), px(100.00), 200, 1);
}

TEST(OrderBook, LevelQuantityCanExceed32Bits) {
  // Qty is 32-bit per order, but a level's total is 64-bit
  OrderBook book;
  book.add(Side::Buy, px(1.00), 3'000'000'000u);
  book.add(Side::Buy, px(1.00), 3'000'000'000u);
  expect_level(book.best_bid(), px(1.00), 6'000'000'000ull, 2);
}

// --- ordering / depth -----------------------------------------------------------------

TEST(OrderBook, BidsAreHighestFirst) {
  OrderBook book;
  book.add(Side::Buy, px(99.98), 100);
  book.add(Side::Buy, px(100.00), 200);
  book.add(Side::Buy, px(99.99), 300);

  const auto levels = book.depth(Side::Buy, 10);
  ASSERT_EQ(levels.size(), 3u);
  expect_level(levels[0], px(100.00), 200, 1);
  expect_level(levels[1], px(99.99), 300, 1);
  expect_level(levels[2], px(99.98), 100, 1);
  expect_level(book.best_bid(), px(100.00), 200, 1);
}

TEST(OrderBook, AsksAreLowestFirst) {
  OrderBook book;
  book.add(Side::Sell, px(100.03), 100);
  book.add(Side::Sell, px(100.01), 200);
  book.add(Side::Sell, px(100.02), 300);

  const auto levels = book.depth(Side::Sell, 10);
  ASSERT_EQ(levels.size(), 3u);
  expect_level(levels[0], px(100.01), 200, 1);
  expect_level(levels[1], px(100.02), 300, 1);
  expect_level(levels[2], px(100.03), 100, 1);
  expect_level(book.best_ask(), px(100.01), 200, 1);
}

TEST(OrderBook, DepthIsLimitedToN) {
  OrderBook book;
  for (int i = 0; i < 5; ++i) book.add(Side::Buy, px(100.00) - static_cast<Price>(i * 100), 100);

  const auto top2 = book.depth(Side::Buy, 2);
  ASSERT_EQ(top2.size(), 2u);
  EXPECT_EQ(top2[0].price, px(100.00));
  EXPECT_EQ(top2[1].price, px(99.99));

  EXPECT_TRUE(book.depth(Side::Buy, 0).empty());
  EXPECT_EQ(book.depth(Side::Buy, 100).size(), 5u); // n larger than the book
}

TEST(OrderBook, DepthReturnsACopy) {
  OrderBook book;
  book.add(Side::Buy, px(100.00), 100);
  const auto before = book.depth(Side::Buy, 10);

  book.add(Side::Buy, px(100.00), 900);

  ASSERT_EQ(before.size(), 1u);
  EXPECT_EQ(before[0].qty, 100u); // unaffected by later changes
}

// --- reduce ---------------------------------------------------------------------------

TEST(OrderBook, PartialReduceKeepsOrderCount) {
  // Partial execute / cancel: shares leave, the order stays
  OrderBook book;
  book.add(Side::Buy, px(100.00), 500);
  book.reduce(Side::Buy, px(100.00), 200, /*removes_order=*/false);
  expect_level(book.best_bid(), px(100.00), 300, 1);
}

TEST(OrderBook, RemovingOneOfSeveralOrdersKeepsLevel) {
  OrderBook book;
  book.add(Side::Sell, px(100.05), 100);
  book.add(Side::Sell, px(100.05), 300);
  book.reduce(Side::Sell, px(100.05), 100, /*removes_order=*/true);
  expect_level(book.best_ask(), px(100.05), 300, 1);
}

TEST(OrderBook, RemovingLastOrderErasesLevel) {
  OrderBook book;
  book.add(Side::Buy, px(100.00), 100);
  book.reduce(Side::Buy, px(100.00), 100, /*removes_order=*/true);
  EXPECT_FALSE(book.best_bid().has_value());
  EXPECT_TRUE(book.depth(Side::Buy, 10).empty());
}

TEST(OrderBook, BestMovesToNextLevelWhenTopEmpties) {
  OrderBook book;
  book.add(Side::Buy, px(100.00), 100);
  book.add(Side::Buy, px(99.99), 200);
  book.add(Side::Sell, px(100.01), 300);
  book.add(Side::Sell, px(100.02), 400);

  book.reduce(Side::Buy, px(100.00), 100, true);
  book.reduce(Side::Sell, px(100.01), 300, true);

  expect_level(book.best_bid(), px(99.99), 200, 1);
  expect_level(book.best_ask(), px(100.02), 400, 1);
}

TEST(OrderBook, ReduceOnlyAffectsItsSideAndPrice) {
  OrderBook book;
  book.add(Side::Buy, px(100.00), 100);
  book.add(Side::Buy, px(99.99), 100);
  book.add(Side::Sell, px(100.00), 100); // same price, other side

  book.reduce(Side::Buy, px(100.00), 40, false);

  expect_level(book.best_bid(), px(100.00), 60, 1);
  expect_level(book.depth(Side::Buy, 2)[1], px(99.99), 100, 1);
  expect_level(book.best_ask(), px(100.00), 100, 1);
}

TEST(OrderBook, ExecuteThenCancelRestOfOrder) {
  // Typical lifecycle of one order: partial fill ('E'), then cancel the rest ('X' / 'D')
  OrderBook book;
  book.add(Side::Sell, px(50.00), 1000);
  book.reduce(Side::Sell, px(50.00), 300, false); // executed 300
  book.reduce(Side::Sell, px(50.00), 700, true);  // remaining 700 cancelled
  EXPECT_FALSE(book.best_ask().has_value());
}

// --- crossed --------------------------------------------------------------------------

TEST(OrderBook, NormalSpreadIsNotCrossed) {
  OrderBook book;
  book.add(Side::Buy, px(100.00), 100);
  book.add(Side::Sell, px(100.01), 100);
  EXPECT_FALSE(book.crossed());
}

TEST(OrderBook, LockedBookIsNotCrossed) {
  // bid == ask is "locked", which is allowed
  OrderBook book;
  book.add(Side::Buy, px(100.00), 100);
  book.add(Side::Sell, px(100.00), 100);
  EXPECT_FALSE(book.crossed());
}

TEST(OrderBook, BidAboveAskIsCrossed) {
  OrderBook book;
  book.add(Side::Buy, px(100.02), 100);
  book.add(Side::Sell, px(100.01), 100);
  EXPECT_TRUE(book.crossed());
}

TEST(OrderBook, OneSidedBookIsNotCrossed) {
  OrderBook book;
  book.add(Side::Buy, px(100.00), 100);
  EXPECT_FALSE(book.crossed());
}

TEST(OrderBook, UncrossesWhenCrossingLevelIsRemoved) {
  OrderBook book;
  book.add(Side::Buy, px(100.00), 100);
  book.add(Side::Buy, px(100.05), 100);
  book.add(Side::Sell, px(100.02), 100);
  ASSERT_TRUE(book.crossed());

  book.reduce(Side::Buy, px(100.05), 100, true);
  EXPECT_FALSE(book.crossed());
}

// --- programming errors (asserts are compiled out in Release) -------------------------

#ifndef NDEBUG
TEST(OrderBookDeathTest, ReduceAtUnknownPriceAsserts) {
  OrderBook book;
  book.add(Side::Buy, px(100.00), 100);
  EXPECT_DEATH(book.reduce(Side::Buy, px(99.00), 100, true), "no level");
}

TEST(OrderBookDeathTest, ReduceMoreThanLevelHasAsserts) {
  OrderBook book;
  book.add(Side::Buy, px(100.00), 100);
  EXPECT_DEATH(book.reduce(Side::Buy, px(100.00), 101, false), "negative");
}
#endif

} // namespace
} // namespace ttt::book
