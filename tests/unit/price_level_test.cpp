#include "feed/book/price_level.hpp"
#include "feed/book/order_pool.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace ttt::book {
namespace {

// The level's queue isn't visible through OrderBook, so its FIFO behaviour is
// tested here directly.
class PriceLevelTest : public ::testing::Test {
protected:
  uint32_t push(Qty qty) {
    const uint32_t idx = pool.alloc();
    pool[idx].qty = qty;
    level.push_back(idx, pool);
    return idx;
  }

  // Walks head -> tail and checks every back link on the way
  std::vector<uint32_t> queue() {
    std::vector<uint32_t> out;
    uint32_t prev = kNil;
    for (uint32_t i = level.head; i != kNil; i = pool[i].next) {
      EXPECT_EQ(pool[i].prev, prev) << "broken prev link at " << i;
      out.push_back(i);
      prev = i;
    }
    EXPECT_EQ(level.tail, prev) << "tail isn't the last order";
    return out;
  }

  OrderPool pool{64};
  PriceLevel level{.price = 1'000'000};
};

TEST_F(PriceLevelTest, StartsEmpty) {
  EXPECT_TRUE(level.empty());
  EXPECT_EQ(level.head, kNil);
  EXPECT_EQ(level.tail, kNil);
  EXPECT_EQ(level.qty, 0u);
  EXPECT_TRUE(queue().empty());
}

TEST_F(PriceLevelTest, PushBackKeepsArrivalOrder) {
  const uint32_t a = push(100);
  const uint32_t b = push(200);
  const uint32_t c = push(300);

  EXPECT_EQ(queue(), (std::vector<uint32_t>{a, b, c}));
  EXPECT_EQ(level.head, a);
  EXPECT_EQ(level.tail, c);
  EXPECT_EQ(level.count, 3u);
  EXPECT_EQ(level.qty, 600u);
}

TEST_F(PriceLevelTest, UnlinkMiddle) {
  const uint32_t a = push(100);
  const uint32_t b = push(200);
  const uint32_t c = push(300);

  level.unlink(b, pool);

  EXPECT_EQ(queue(), (std::vector<uint32_t>{a, c}));
  EXPECT_EQ(level.count, 2u);
  EXPECT_EQ(level.qty, 400u);
}

TEST_F(PriceLevelTest, UnlinkHead) {
  const uint32_t a = push(100);
  const uint32_t b = push(200);
  const uint32_t c = push(300);

  level.unlink(a, pool);

  EXPECT_EQ(queue(), (std::vector<uint32_t>{b, c}));
  EXPECT_EQ(level.head, b);
}

TEST_F(PriceLevelTest, UnlinkTail) {
  const uint32_t a = push(100);
  const uint32_t b = push(200);
  const uint32_t c = push(300);

  level.unlink(c, pool);

  EXPECT_EQ(queue(), (std::vector<uint32_t>{a, b}));
  EXPECT_EQ(level.tail, b);
}

TEST_F(PriceLevelTest, UnlinkOnlyOrderEmptiesLevel) {
  const uint32_t a = push(100);

  level.unlink(a, pool);

  EXPECT_TRUE(level.empty());
  EXPECT_EQ(level.head, kNil);
  EXPECT_EQ(level.tail, kNil);
  EXPECT_EQ(level.qty, 0u);
}

TEST_F(PriceLevelTest, UnlinkEverythingInAnyOrder) {
  const uint32_t a = push(1);
  const uint32_t b = push(2);
  const uint32_t c = push(3);
  const uint32_t d = push(4);

  level.unlink(c, pool);
  EXPECT_EQ(queue(), (std::vector<uint32_t>{a, b, d}));
  level.unlink(a, pool);
  EXPECT_EQ(queue(), (std::vector<uint32_t>{b, d}));
  level.unlink(d, pool);
  EXPECT_EQ(queue(), (std::vector<uint32_t>{b}));
  level.unlink(b, pool);
  EXPECT_TRUE(level.empty());
  EXPECT_EQ(level.qty, 0u);
}

TEST_F(PriceLevelTest, ReAddedOrderGoesToTheBack) {
  // A replace at the same price: unlink, then push_back -> loses priority
  const uint32_t a = push(100);
  const uint32_t b = push(200);

  level.unlink(a, pool);
  level.push_back(a, pool);

  EXPECT_EQ(queue(), (std::vector<uint32_t>{b, a}));
  EXPECT_EQ(level.qty, 300u);
}

TEST_F(PriceLevelTest, UnlinkUsesTheOrdersCurrentQuantity) {
  // A partial fill reduces order and level qty together (OrderBook::reduce);
  // unlink then removes only what's left
  const uint32_t a = push(500);
  push(100);
  pool[a].qty -= 450;
  level.qty -= 450;

  level.unlink(a, pool);

  EXPECT_EQ(level.qty, 100u);
}

} // namespace
} // namespace ttt::book
