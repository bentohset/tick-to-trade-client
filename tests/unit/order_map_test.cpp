#include "feed/book/order_map.hpp"
#include "feed/book/order_pool.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <random>
#include <unordered_map>
#include <vector>

namespace ttt::book {
namespace {

// Tiny tables force long probe runs, collisions and wrap-around past the end of
// the slot array: the cases backward-shift deletion has to get right.
// Refs start at 1 (0 means "empty slot"), and the number of live entries always
// stays below the capacity, as OrderMap requires.

TEST(OrderMap, EmptyMapFindsNothing) {
  OrderMap map(16);
  EXPECT_EQ(map.find(1), kNil);
  EXPECT_EQ(map.find(123456789), kNil);
  EXPECT_EQ(map.erase(1), kNil);
}

TEST(OrderMap, InsertThenFind) {
  OrderMap map(16);
  EXPECT_TRUE(map.insert(42, 7));
  EXPECT_TRUE(map.insert(43, 8));
  EXPECT_EQ(map.find(42), 7u);
  EXPECT_EQ(map.find(43), 8u);
  EXPECT_EQ(map.find(44), kNil);
}

TEST(OrderMap, DuplicateInsertFailsAndKeepsOriginal) {
  OrderMap map(16);
  EXPECT_TRUE(map.insert(42, 7));
  EXPECT_FALSE(map.insert(42, 99));
  EXPECT_EQ(map.find(42), 7u);
}

TEST(OrderMap, EraseReturnsIndexAndRemoves) {
  OrderMap map(16);
  map.insert(42, 7);
  EXPECT_EQ(map.erase(42), 7u);
  EXPECT_EQ(map.find(42), kNil);
  EXPECT_EQ(map.erase(42), kNil); // already gone
}

TEST(OrderMap, ErasedRefCanBeInsertedAgain) {
  OrderMap map(16);
  map.insert(42, 7);
  map.erase(42);
  EXPECT_TRUE(map.insert(42, 9));
  EXPECT_EQ(map.find(42), 9u);
}

TEST(OrderMap, LargeRefs) {
  OrderMap map(16);
  constexpr OrderRef kBig = 0xFFFF'FFFF'FFFF'FFF0ull;
  EXPECT_TRUE(map.insert(kBig, 1));
  EXPECT_TRUE(map.insert(kBig + 1, 2));
  EXPECT_EQ(map.find(kBig), 1u);
  EXPECT_EQ(map.find(kBig + 1), 2u);
}

TEST(OrderMap, NearlyFullTableEraseEachEntryInTurn) {
  // 15 entries in 16 slots: one long probe run that wraps around. Erase each
  // entry in turn and check every other entry is still reachable.
  constexpr std::size_t kSlots = 16;
  for (OrderRef victim = 1; victim < kSlots; ++victim) {
    OrderMap map(kSlots);
    for (OrderRef r = 1; r < kSlots; ++r) ASSERT_TRUE(map.insert(r, static_cast<uint32_t>(r * 10)));

    EXPECT_EQ(map.erase(victim), victim * 10);
    for (OrderRef r = 1; r < kSlots; ++r) {
      if (r == victim) {
        EXPECT_EQ(map.find(r), kNil);
      } else {
        EXPECT_EQ(map.find(r), r * 10) << "ref " << r << " lost after erasing " << victim;
      }
    }
  }
}

TEST(OrderMap, EraseAllInEveryOrderOfASmallSet) {
  // Every permutation of erasing 6 entries from an 8-slot table
  std::vector<OrderRef> refs{3, 11, 19, 27, 35, 43};
  std::sort(refs.begin(), refs.end());
  do {
    OrderMap map(8);
    for (const OrderRef r : refs) ASSERT_TRUE(map.insert(r, static_cast<uint32_t>(r)));
    for (std::size_t i = 0; i < refs.size(); ++i) {
      ASSERT_EQ(map.erase(refs[i]), refs[i]);
      for (std::size_t j = i + 1; j < refs.size(); ++j) ASSERT_EQ(map.find(refs[j]), refs[j]);
    }
  } while (std::next_permutation(refs.begin(), refs.end()));
}

TEST(OrderMap, RandomOperationsMatchUnorderedMap) {
  // Random inserts, erases and finds on a small, busy table, checked against
  // std::unordered_map. Keeps at most 48 live entries in 64 slots (load 0.75).
  constexpr std::size_t kSlots = 64;
  constexpr std::size_t kMaxLive = 48;
  OrderMap map(kSlots);
  std::unordered_map<OrderRef, uint32_t> expected;
  std::mt19937_64 rng(12345);
  std::uniform_int_distribution<OrderRef> ref_dist(1, 200); // small range: many repeats
  std::uniform_int_distribution<int> op_dist(0, 2);

  for (int step = 0; step < 200'000; ++step) {
    const OrderRef ref = ref_dist(rng);
    const auto it = expected.find(ref);
    switch (op_dist(rng)) {
      case 0: { // insert
        if (it == expected.end() && expected.size() >= kMaxLive) break;
        const auto index = static_cast<uint32_t>(step);
        const bool inserted = map.insert(ref, index);
        ASSERT_EQ(inserted, it == expected.end()) << "step " << step;
        if (inserted) expected[ref] = index;
        break;
      }
      case 1: { // erase
        const uint32_t removed = map.erase(ref);
        ASSERT_EQ(removed, it == expected.end() ? kNil : it->second) << "step " << step;
        if (it != expected.end()) expected.erase(it);
        break;
      }
      default: // find
        ASSERT_EQ(map.find(ref), it == expected.end() ? kNil : it->second) << "step " << step;
    }
  }
  // Final full check
  for (OrderRef r = 1; r <= 200; ++r) {
    const auto it = expected.find(r);
    EXPECT_EQ(map.find(r), it == expected.end() ? kNil : it->second);
  }
}

TEST(OrderMap, ManyEntriesInALargeTable) {
  // Realistic shape: sequential-ish refs, half the range used, lots of churn
  OrderMap map(std::size_t{1} << 16);
  for (OrderRef r = 1; r <= 30'000; ++r) ASSERT_TRUE(map.insert(r * 2, static_cast<uint32_t>(r)));
  for (OrderRef r = 1; r <= 30'000; r += 3) ASSERT_EQ(map.erase(r * 2), r);
  for (OrderRef r = 1; r <= 30'000; ++r) {
    EXPECT_EQ(map.find(r * 2), r % 3 == 1 ? kNil : static_cast<uint32_t>(r));
    EXPECT_EQ(map.find(r * 2 + 1), kNil);
  }
}

} // namespace
} // namespace ttt::book
