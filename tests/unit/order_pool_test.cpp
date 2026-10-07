#include "feed/book/order_pool.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <set>

namespace ttt::book {
namespace {

TEST(OrderPool, StartsEmpty) {
  OrderPool pool(8);
  EXPECT_EQ(pool.live(), 0u);
}

TEST(OrderPool, AllocHandsOutDistinctSlotsUntilFull) {
  constexpr uint32_t kCapacity = 100;
  OrderPool pool(kCapacity);

  std::set<uint32_t> seen;
  for (uint32_t i = 0; i < kCapacity; ++i) {
    const uint32_t idx = pool.alloc();
    ASSERT_NE(idx, kNil);
    ASSERT_LT(idx, kCapacity);
    EXPECT_TRUE(seen.insert(idx).second) << "slot " << idx << " handed out twice";
  }
  EXPECT_EQ(pool.live(), kCapacity);
  EXPECT_EQ(pool.alloc(), kNil); // full
  EXPECT_EQ(pool.live(), kCapacity);
}

TEST(OrderPool, FreeIsLifo) {
  // The most recently freed slot comes back first: it's still in cache
  OrderPool pool(8);
  const uint32_t a = pool.alloc();
  const uint32_t b = pool.alloc();
  pool.free(a);
  pool.free(b);
  EXPECT_EQ(pool.alloc(), b);
  EXPECT_EQ(pool.alloc(), a);
}

TEST(OrderPool, FreeingMakesRoomWhenFull) {
  OrderPool pool(2);
  const uint32_t a = pool.alloc();
  pool.alloc();
  ASSERT_EQ(pool.alloc(), kNil);

  pool.free(a);
  EXPECT_EQ(pool.live(), 1u);
  EXPECT_EQ(pool.alloc(), a);
  EXPECT_EQ(pool.alloc(), kNil);
}

TEST(OrderPool, SlotsKeepTheirContents) {
  OrderPool pool(4);
  const uint32_t a = pool.alloc();
  const uint32_t b = pool.alloc();
  pool[a].ref = 111;
  pool[a].price = 1'000'000;
  pool[b].ref = 222;

  EXPECT_EQ(pool[a].ref, 111u);
  EXPECT_EQ(pool[a].price, 1'000'000u);
  EXPECT_EQ(pool[b].ref, 222u);
}

TEST(OrderPool, ChurnNeverLeaksSlots) {
  // Alternating alloc/free in many patterns keeps live() exact and never runs dry
  OrderPool pool(16);
  for (int round = 0; round < 1000; ++round) {
    uint32_t held[16];
    const int n = round % 16 + 1;
    for (int i = 0; i < n; ++i) ASSERT_NE(held[i] = pool.alloc(), kNil);
    EXPECT_EQ(pool.live(), static_cast<uint32_t>(n));
    for (int i = n - 1; i >= 0; i -= 2) pool.free(held[i]);
    for (int i = n - 2; i >= 0; i -= 2) pool.free(held[i]);
    EXPECT_EQ(pool.live(), 0u);
  }
}

} // namespace
} // namespace ttt::book
