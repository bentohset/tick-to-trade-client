#include "feed/book/book_manager.hpp"
#include "feed/itch/parser.hpp"
#include "utils/itch_builder.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>

namespace ttt::book {
namespace {

using namespace ttt::test; // ITCH message builders

// --- fixture --------------------------------------------------------------------------

constexpr uint16_t kAapl = 1;
constexpr uint16_t kMsft = 2;

constexpr Price px(double dollars) { return static_cast<Price>(dollars * 10'000 + 0.5); }

class BookManagerTest : public ::testing::Test {
protected:
  void SetUp() override {
    feed(stock_directory(kAapl, "AAPL"));
    feed(trading_action(kAapl, 'T'));
  }

  void feed(const Msg& m) { ASSERT_EQ(itch::parse(m.bytes(), bm), itch::ParseResult::Ok); }

  void open_market() { feed(system_event('Q')); }

  const OrderBook& book(uint16_t locate = kAapl) {
    const OrderBook* b = bm.book(locate);
    EXPECT_NE(b, nullptr);
    return *b;
  }

  void expect_no_errors() { EXPECT_EQ(bm.errors().total(), 0u); }

  BookManager bm;
};

void expect_level(const std::optional<Level>& level, Price price, uint64_t qty, uint32_t orders) {
  ASSERT_TRUE(level.has_value());
  EXPECT_EQ(level->price, price);
  EXPECT_EQ(level->qty, qty);
  EXPECT_EQ(level->orders, orders);
}

// --- symbols --------------------------------------------------------------------------

TEST_F(BookManagerTest, AnnouncedSymbolHasEmptyBook) {
  EXPECT_FALSE(book().best_bid().has_value());
  EXPECT_FALSE(book().best_ask().has_value());
  EXPECT_EQ(bm.live_orders(), 0u);
}

TEST_F(BookManagerTest, UnannouncedSymbolHasNoBook) {
  EXPECT_EQ(bm.book(kMsft), nullptr);  // never announced
  EXPECT_EQ(bm.book(0), nullptr);      // locate 0 is never valid
  EXPECT_EQ(bm.book(60'000), nullptr); // beyond anything announced
}

TEST_F(BookManagerTest, AddForUnannouncedSymbolIsCounted) {
  feed(add(kMsft, 1, 'B', 100, px(100.00)));
  EXPECT_EQ(bm.errors().unknown_locate, 1u);
  EXPECT_EQ(bm.live_orders(), 0u);
}

TEST_F(BookManagerTest, TradingActionForUnannouncedSymbolIsCounted) {
  feed(trading_action(kMsft, 'T'));
  EXPECT_EQ(bm.errors().unknown_locate, 1u);
}

// --- add ------------------------------------------------------------------------------

TEST_F(BookManagerTest, AddPutsOrdersOnTheRightSide) {
  feed(add(kAapl, 1, 'B', 100, px(100.00)));
  feed(add(kAapl, 2, 'S', 200, px(100.01)));

  expect_level(book().best_bid(), px(100.00), 100, 1);
  expect_level(book().best_ask(), px(100.01), 200, 1);
  EXPECT_EQ(bm.live_orders(), 2u);
  expect_no_errors();
}

TEST_F(BookManagerTest, AddWithMpidBehavesLikeAdd) {
  feed(add_mpid(kAapl, 1, 'B', 300, px(99.50)));
  expect_level(book().best_bid(), px(99.50), 300, 1);
  EXPECT_EQ(bm.live_orders(), 1u);
  expect_no_errors();
}

TEST_F(BookManagerTest, DuplicateRefIsCountedAndOriginalKept) {
  feed(add(kAapl, 7, 'B', 100, px(100.00)));
  feed(add(kAapl, 7, 'S', 500, px(101.00)));

  EXPECT_EQ(bm.errors().duplicate_ref, 1u);
  expect_level(book().best_bid(), px(100.00), 100, 1);
  EXPECT_FALSE(book().best_ask().has_value());
  EXPECT_EQ(bm.live_orders(), 1u);
}

TEST_F(BookManagerTest, SymbolsHaveSeparateBooks) {
  feed(stock_directory(kMsft, "MSFT"));
  feed(trading_action(kMsft, 'T'));
  feed(add(kAapl, 1, 'B', 100, px(100.00)));
  feed(add(kMsft, 2, 'B', 200, px(200.00)));

  // 'D' carries only the ref; the manager must find MSFT's book from it
  feed(del(kMsft, 2));

  expect_level(book(kAapl).best_bid(), px(100.00), 100, 1);
  EXPECT_FALSE(book(kMsft).best_bid().has_value());
  expect_no_errors();
}

// --- execute / cancel / delete --------------------------------------------------------

TEST_F(BookManagerTest, PartialExecuteReducesOrder) {
  feed(add(kAapl, 1, 'S', 500, px(100.00)));
  feed(executed(kAapl, 1, 200));

  expect_level(book().best_ask(), px(100.00), 300, 1);
  EXPECT_EQ(bm.live_orders(), 1u);
  expect_no_errors();
}

TEST_F(BookManagerTest, FullExecuteRemovesOrder) {
  feed(add(kAapl, 1, 'S', 500, px(100.00)));
  feed(executed(kAapl, 1, 500));

  EXPECT_FALSE(book().best_ask().has_value());
  EXPECT_EQ(bm.live_orders(), 0u);
  expect_no_errors();
}

TEST_F(BookManagerTest, ExecuteWithPriceReducesAtRestingPrice) {
  feed(add(kAapl, 1, 'B', 500, px(100.00)));
  feed(add(kAapl, 2, 'B', 100, px(99.00)));
  feed(executed_with_price(kAapl, 1, 200, px(99.00))); // executed at 99, rests at 100

  expect_level(book().best_bid(), px(100.00), 300, 1);
  EXPECT_EQ(book().depth(Side::Buy, 2)[1].qty, 100u); // 99.00 level untouched
  expect_no_errors();
}

TEST_F(BookManagerTest, PartialCancelThenDeleteRest) {
  feed(add(kAapl, 1, 'B', 1000, px(100.00)));
  feed(cancel(kAapl, 1, 400));
  expect_level(book().best_bid(), px(100.00), 600, 1);

  feed(del(kAapl, 1));
  EXPECT_FALSE(book().best_bid().has_value());
  EXPECT_EQ(bm.live_orders(), 0u);
  expect_no_errors();
}

TEST_F(BookManagerTest, DeleteOneOfTwoOrdersAtLevel) {
  feed(add(kAapl, 1, 'B', 100, px(100.00)));
  feed(add(kAapl, 2, 'B', 300, px(100.00)));
  feed(del(kAapl, 1));

  expect_level(book().best_bid(), px(100.00), 300, 1);
  expect_no_errors();
}

TEST_F(BookManagerTest, UnknownRefIsCountedForEveryMessageType) {
  feed(executed(kAapl, 99, 100));
  feed(executed_with_price(kAapl, 99, 100, px(1.00)));
  feed(cancel(kAapl, 99, 100));
  feed(del(kAapl, 99));
  feed(replace(kAapl, 99, 100, 100, px(1.00)));

  EXPECT_EQ(bm.errors().unknown_ref, 5u);
  EXPECT_EQ(bm.live_orders(), 0u); // the replace must not create order 100
}

TEST_F(BookManagerTest, OverReduceIsClampedAndCounted) {
  feed(add(kAapl, 1, 'B', 100, px(100.00)));
  feed(executed(kAapl, 1, 150));

  EXPECT_EQ(bm.errors().over_reduce, 1u);
  EXPECT_FALSE(book().best_bid().has_value()); // clamped to 100: order fully removed
  EXPECT_EQ(bm.live_orders(), 0u);
}

TEST_F(BookManagerTest, OrderIsGoneAfterRemoval) {
  feed(add(kAapl, 1, 'B', 100, px(100.00)));
  feed(del(kAapl, 1));
  feed(del(kAapl, 1)); // second delete of the same ref

  EXPECT_EQ(bm.errors().unknown_ref, 1u);
}

// --- replace --------------------------------------------------------------------------

TEST_F(BookManagerTest, ReplaceMovesOrderToNewPriceAndSize) {
  feed(add(kAapl, 1, 'B', 100, px(100.00)));
  feed(replace(kAapl, 1, 2, 250, px(100.05)));

  expect_level(book().best_bid(), px(100.05), 250, 1);
  EXPECT_EQ(book().depth(Side::Buy, 10).size(), 1u); // old level gone
  EXPECT_EQ(bm.live_orders(), 1u);
  expect_no_errors();
}

TEST_F(BookManagerTest, ReplaceInheritsSide) {
  // 'U' carries no side: a replaced sell must stay a sell
  feed(add(kAapl, 1, 'S', 100, px(101.00)));
  feed(replace(kAapl, 1, 2, 100, px(100.50)));

  expect_level(book().best_ask(), px(100.50), 100, 1);
  EXPECT_FALSE(book().best_bid().has_value());
  expect_no_errors();
}

TEST_F(BookManagerTest, ReplacedOrderIsKnownByNewRefOnly) {
  feed(add(kAapl, 1, 'B', 100, px(100.00)));
  feed(replace(kAapl, 1, 2, 100, px(100.00)));

  feed(executed(kAapl, 1, 10)); // old ref: gone
  EXPECT_EQ(bm.errors().unknown_ref, 1u);

  feed(executed(kAapl, 2, 10)); // new ref: live
  expect_level(book().best_bid(), px(100.00), 90, 1);
}

TEST_F(BookManagerTest, ReplaceAtSamePriceKeepsOneOrderAtLevel) {
  feed(add(kAapl, 1, 'B', 100, px(100.00)));
  feed(add(kAapl, 2, 'B', 300, px(100.00)));
  feed(replace(kAapl, 1, 3, 50, px(100.00)));

  expect_level(book().best_bid(), px(100.00), 350, 2);
  expect_no_errors();
}

TEST_F(BookManagerTest, ReplaceToExistingRefIsDuplicate) {
  feed(add(kAapl, 1, 'B', 100, px(100.00)));
  feed(add(kAapl, 2, 'B', 200, px(99.00)));
  feed(replace(kAapl, 1, 2, 100, px(100.00)));

  EXPECT_EQ(bm.errors().duplicate_ref, 1u);
  expect_level(book().best_bid(), px(99.00), 200, 1); // original 1 removed, 2 untouched
}

// --- crossed-book check ---------------------------------------------------------------

TEST_F(BookManagerTest, CrossedWhileTradingIsCounted) {
  open_market();
  feed(add(kAapl, 1, 'S', 100, px(100.00)));
  feed(add(kAapl, 2, 'B', 100, px(100.05)));
  EXPECT_EQ(bm.errors().crossed_while_trading, 1u);
}

TEST_F(BookManagerTest, LockedBookIsNotAnError) {
  open_market();
  feed(add(kAapl, 1, 'S', 100, px(100.00)));
  feed(add(kAapl, 2, 'B', 100, px(100.00)));
  expect_no_errors();
}

TEST_F(BookManagerTest, CrossedBeforeMarketOpenIsNotChecked) {
  feed(add(kAapl, 1, 'S', 100, px(100.00)));
  feed(add(kAapl, 2, 'B', 100, px(100.05)));
  expect_no_errors();
}

TEST_F(BookManagerTest, CrossedAfterMarketCloseIsNotChecked) {
  open_market();
  feed(system_event('M'));
  feed(add(kAapl, 1, 'S', 100, px(100.00)));
  feed(add(kAapl, 2, 'B', 100, px(100.05)));
  expect_no_errors();
}

TEST_F(BookManagerTest, CrossedWhileHaltedIsNotChecked) {
  open_market();
  feed(add(kAapl, 1, 'S', 100, px(100.00)));
  feed(trading_action(kAapl, 'H'));
  feed(add(kAapl, 2, 'B', 100, px(100.05)));
  expect_no_errors();
}

TEST_F(BookManagerTest, ReopeningCrossMayStayCrossedUntilItUncrosses) {
  open_market();
  feed(add(kAapl, 1, 'S', 100, px(100.00)));
  feed(trading_action(kAapl, 'H'));
  feed(add(kAapl, 2, 'B', 100, px(100.05))); // crossed during the halt
  feed(trading_action(kAapl, 'T'));          // resume: reopening cross begins

  // Reopening cross executes; messages while still crossed are allowed,
  // including adds and replaces mixed into the burst
  feed(add(kAapl, 3, 'B', 50, px(100.02)));
  feed(executed_with_price(kAapl, 1, 60, px(100.02)));
  EXPECT_EQ(bm.errors().crossed_while_trading, 0u);

  feed(executed_with_price(kAapl, 1, 40, px(100.02))); // ask gone: uncrossed
  EXPECT_EQ(bm.errors().crossed_while_trading, 0u);

  // After it has uncrossed once, crossing again is a real error
  feed(add(kAapl, 4, 'S', 100, px(100.01)));
  EXPECT_EQ(bm.errors().crossed_while_trading, 1u);
}

// --- lifecycle ------------------------------------------------------------------------

TEST_F(BookManagerTest, BookIsEmptyAfterEveryOrderIsRemoved) {
  // Mirrors the full-day check: every order added is eventually removed
  feed(add(kAapl, 1, 'B', 100, px(100.00)));
  feed(add(kAapl, 2, 'S', 200, px(100.02)));
  feed(add_mpid(kAapl, 3, 'B', 300, px(99.99)));
  feed(executed(kAapl, 1, 100));
  feed(cancel(kAapl, 2, 50));
  feed(replace(kAapl, 3, 4, 300, px(99.98)));
  feed(del(kAapl, 2));
  feed(executed_with_price(kAapl, 4, 300, px(99.98)));

  EXPECT_EQ(bm.live_orders(), 0u);
  EXPECT_FALSE(book().best_bid().has_value());
  EXPECT_FALSE(book().best_ask().has_value());
  expect_no_errors();
}

} // namespace
} // namespace ttt::book
