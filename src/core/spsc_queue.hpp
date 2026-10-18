#pragma once

#include <atomic>
#include <bit>
#include <cstddef>
#include <memory>
#include <type_traits>
namespace ttt::core {

// macOS prefetches 128 B pairs
#if defined(__aarch64__)
inline constexpr std::size_t kCacheLine = 128;
#else
inline constexpr std::size_t kCacheLine = 64;
#endif

// Bounded single-producer single-consumer ring. Capacity rounded up to power of 2.
// Queue is a fixed array of slots and 2 counters (tail_ - producer, head_ - consumer)
// which only go up.
// The array position for counter value i is i & mask_, so counters wrap around the array.
// Empty: tail == head; Full: tail-head == capacity; NumItems: tail - head;
// Each side keeps cached copy of other side's index so it only touches the shared line
// when cache is full or empty.
template <class T> class SpscQueue {
  static_assert(std::is_trivially_copyable_v<T>);

  // TODO: possible optimizations:
  // 1. sequence flag inside each slot (FastForward) - consumer spins on slots_[head].seq == head +
  // 1 instead of
  //    on tail_ and producer writes payload and does release store to seq. Data and "ready" signal
  //    arrive in one line so its one transfer per message isntead of 2. But slot grows by 8 bytes.
  // 2. Consumer batching. Pop everything available, then publish head_ once. To handle bursts.
public:
  explicit SpscQueue(std::size_t capacity)
      : mask_(std::bit_ceil(capacity) - 1), slots_(std::make_unique<T[]>(mask_ + 1)) {}

  // Producer writes the item to tail&mask_ and increment tail_
  bool try_push(const T& v) noexcept {
    const auto tail = tail_.load(std::memory_order_relaxed);
    // looks full
    if (tail - head_cache_ > mask_) {
      head_cache_ = head_.load(std::memory_order_acquire);
      // actually full
      if (tail - head_cache_ > mask_) return false;
    }
    slots_[tail & mask_] = v;
    // publish the slot
    tail_.store(tail + 1, std::memory_order_release);
    return true;
  }

  // zero-copy peek + pop.
  T* front() noexcept {
    const auto head = head_.load(std::memory_order_relaxed);
    if (head == tail_cache_) {
      tail_cache_ = tail_.load(std::memory_order_acquire);
      if (head == tail_cache_) return nullptr;
    }
    return &slots_[head & mask_];
  }

  void pop() noexcept { head_.store(head_.load() + 1, std::memory_order_release); }

  // Consumer reads the slot at head_ and increment head_
  bool try_pop(T& out) noexcept {
    if (T* p = front()) {
      out = *p;
      pop();
      return true;
    }
    return false;
  }

  std::size_t capacity() const noexcept { return mask_ + 1; }

private:
  const std::size_t mask_;
  std::unique_ptr<T[]> slots_;
  // put tail_ and head_ on separate cache lines by aligning
  alignas(kCacheLine) std::atomic<uint64_t> tail_{0};
  // cached copies as loading other thread counter is expensive (pulls
  // cache line from other core). trust it until it says full or empty then
  // does reload of the real value.
  uint64_t head_cache_{0};
  alignas(kCacheLine) std::atomic<uint64_t> head_{0};
  uint64_t tail_cache_{0};
};

} // namespace ttt::core
