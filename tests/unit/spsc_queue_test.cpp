#include "core/spsc_queue.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <thread>

namespace ttt::core {
namespace {

TEST(SpscQueue, CapacityRoundsUpToPowerOfTwo) {
  EXPECT_EQ(SpscQueue<int>(1).capacity(), 1u);
  EXPECT_EQ(SpscQueue<int>(4).capacity(), 4u);
  EXPECT_EQ(SpscQueue<int>(5).capacity(), 8u);
  EXPECT_EQ(SpscQueue<int>(1000).capacity(), 1024u);
}

TEST(SpscQueue, StartsEmpty) {
  SpscQueue<int> q(4);
  int out = -1;
  EXPECT_EQ(q.front(), nullptr);
  EXPECT_FALSE(q.try_pop(out));
  EXPECT_EQ(out, -1); // untouched on failure
}

TEST(SpscQueue, PushFailsWhenFull) {
  SpscQueue<int> q(4);
  for (int i = 0; i < 4; ++i) EXPECT_TRUE(q.try_push(i));
  EXPECT_FALSE(q.try_push(99));

  int out;
  ASSERT_TRUE(q.try_pop(out));
  EXPECT_EQ(out, 0);
  EXPECT_TRUE(q.try_push(4)); // one slot freed
  EXPECT_FALSE(q.try_push(5));
}

TEST(SpscQueue, PopsInFifoOrder) {
  SpscQueue<int> q(8);
  for (int i = 0; i < 5; ++i) ASSERT_TRUE(q.try_push(i));
  for (int i = 0; i < 5; ++i) {
    int out;
    ASSERT_TRUE(q.try_pop(out));
    EXPECT_EQ(out, i);
  }
  EXPECT_EQ(q.front(), nullptr);
}

TEST(SpscQueue, WrapsAroundManyTimes) {
  // Counters run far past the array size; slot index is counter & mask
  SpscQueue<uint64_t> q(4);
  uint64_t next_push = 0;
  uint64_t next_pop = 0;
  for (int round = 0; round < 100; ++round) {
    while (q.try_push(next_push)) ++next_push; // fill
    EXPECT_EQ(next_push - next_pop, 4u);
    for (int i = 0; i < 3; ++i) { // drain most of it, so head/tail land at different offsets
      uint64_t out;
      ASSERT_TRUE(q.try_pop(out));
      EXPECT_EQ(out, next_pop++);
    }
  }
}

TEST(SpscQueue, FrontPeeksWithoutRemoving) {
  SpscQueue<int> q(4);
  ASSERT_TRUE(q.try_push(7));
  ASSERT_TRUE(q.try_push(8));

  int* p = q.front();
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(*p, 7);
  EXPECT_EQ(q.front(), p); // still there
  q.pop();

  ASSERT_NE(q.front(), nullptr);
  EXPECT_EQ(*q.front(), 8);
  q.pop();
  EXPECT_EQ(q.front(), nullptr);
}

TEST(SpscQueue, CopiesStructPayload) {
  struct Item {
    uint64_t a;
    uint32_t b;
    char c[4];
  };
  SpscQueue<Item> q(2);
  ASSERT_TRUE(q.try_push({1, 2, {'a', 'b', 'c', 'd'}}));
  Item out{};
  ASSERT_TRUE(q.try_pop(out));
  EXPECT_EQ(out.a, 1u);
  EXPECT_EQ(out.b, 2u);
  EXPECT_EQ(out.c[3], 'd');
}

// Producer and consumer on separate threads. A small capacity forces many
// full/empty transitions, so the cached-index reload paths run constantly.
// Most useful under TSan: a wrong memory_order shows up as a race report.
TEST(SpscQueue, TwoThreadsSeeEveryItemInOrder) {
  constexpr uint64_t kItems = 1'000'000;
  SpscQueue<uint64_t> q(64);

  std::thread producer([&] {
    for (uint64_t i = 0; i < kItems; ++i) {
      while (!q.try_push(i)) {}
    }
  });

  uint64_t expected = 0;
  uint64_t out_of_order = 0;
  while (expected < kItems) {
    if (const uint64_t* p = q.front()) {
      if (*p != expected) ++out_of_order;
      q.pop();
      ++expected;
    }
  }
  producer.join();

  EXPECT_EQ(out_of_order, 0u);
  EXPECT_EQ(q.front(), nullptr);
}

} // namespace
} // namespace ttt::core
