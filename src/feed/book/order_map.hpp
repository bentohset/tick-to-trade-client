#pragma once

#include "feed/book/order_pool.hpp"
#include "feed/book/types.hpp"
#include <bit>
#include <cstddef>
#include <cstdint>
#include <vector>
namespace ttt::book {

// ref -> pool index. Linear probing with backward-shift deletion
// no tombstones: the workload deletes as often as it inserts
// Ref 0 marks an empty slot.
// Capacity must be power of 2 and large than the pool so a probe always reaches an empty slot
class OrderMap {
public:
  explicit OrderMap(std::size_t capacity_pow2)
      : slots_(capacity_pow2),
        mask_(capacity_pow2 - 1),
        shift_(64 - static_cast<unsigned>(std::countr_zero(capacity_pow2))) {}

  // returns pool index, or kNil if absent
  uint32_t find(OrderRef ref) const {
    for (std::size_t i = home(ref);; i = (i + 1) & mask_) {
      const Slot& s = slots_[i];
      if (s.ref == ref) return s.index;
      if (s.ref == 0) return kNil;
    }
  }

  // returns false if ref is present
  bool insert(OrderRef ref, uint32_t index) {
    for (std::size_t i = home(ref);; i = (i + 1) & mask_) {
      Slot& s = slots_[i];
      if (s.ref == ref) return false;
      if (s.ref == 0) {
        s = Slot{ref, index};
        return true;
      }
    }
  }

  // returns removed pool index, or kNil if absent
  uint32_t erase(OrderRef ref) {
    std::size_t hole = home(ref);
    while (slots_[hole].ref != ref) {
      if (slots_[hole].ref == 0) return kNil;
      hole = (hole + 1) & mask_;
    }
    const uint32_t removed = slots_[hole].index;
    // walk the rest of this probe and pull each entry back to shift everything backwards
    // into the hole if that keeps it reachable from its home slot.
    // (the hole lies between entry's home and where it sits now)
    for (std::size_t j = (hole + 1) & mask_; slots_[j].ref != 0; j = (j + 1) & mask_) {
      const std::size_t h = home(slots_[j].ref);
      if (((j - h) & mask_) >= ((j - hole) & mask_)) {
        slots_[hole] = slots_[j];
        hole = j;
      }
    }
    slots_[hole].ref = 0;
    return removed;
  }

private:
  struct Slot {
    OrderRef ref = 0;
    uint32_t index = 0;
  }; // 16 bytes: 4 per cache line

  // Fibonacci hashing: multiply by 2^64/phi, keep the top bits
  std::size_t home(OrderRef ref) const {
    return static_cast<std::size_t>((ref * 0x9E3779B97F4A7C15ull) >> shift_);
  }

  std::vector<Slot> slots_;
  std::size_t mask_;
  unsigned shift_;
};

} // namespace ttt::book
