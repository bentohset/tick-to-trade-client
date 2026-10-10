#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace ttt::mold {

// A fixed-capacity buffer for packets that arrive after a gap until the fill lands.
// When session detects a gap (seq > expected_), the out-of-order packets wait in this
// packet buffer until expected_ catches up to them.
//
// Direct-mapped ring, indexed by seq % capacity: O(1) put, and O(1) amortized drain
// in pop_ready (each slot is visited once). This replaced an earlier linear-scan
// version -- fine at small capacity, but every put()/pop_ready() call cost O(capacity),
// which made recovery itself the bottleneck once capacity was raised to absorb bigger
// gaps (recovering got slower exactly when it needed to be faster).
//
// Trade-off: capacity must cover the maximum tolerable WIDTH of a gap
// (highest outstanding seq - expected_), not the count of buffered packets. Two
// outstanding seqs that collide on the same slot (seq_a % capacity == seq_b %
// capacity, with seq_a != seq_b) are treated as "no room", same as the buffer being
// genuinely full -- this can happen even with many free slots elsewhere if the gap is
// wide relative to capacity, so size capacity from the worst gap width you want to
// survive, not from "how many packets might be loose at once".
class PacketBuffer {
public:
  explicit PacketBuffer(std::size_t capacity) : slots_(capacity) {}

  // Store a packet. False means no room: either truly full, or a collision with a
  // different seq at the same slot (the gap is wider than capacity).
  bool put(uint64_t seq, std::span<const std::byte> pkt) {
    Slot& s = slots_[seq % slots_.size()];
    if (s.used && s.seq == seq) return true; // already buffered: dedup
    if (s.used) return false;                // occupied by a different seq: no room
    s.used = true;
    s.seq = seq;
    s.data.assign(pkt.begin(), pkt.end());
    ++used_;
    if (used_ > peak_used_) peak_used_ = used_;
    return true;
  }

  // Delivers slots in increasing order starting at `upto` (the caller's expected_,
  // passed by reference so this sees the live value as f's side effects advance it):
  // as long as the slot at the current `upto` holds exactly that seq, hand it to f,
  // free it, and check the next one. Stops at the first miss -- a hole, or nothing
  // buffered there yet.
  template <class F> void pop_ready(const uint64_t& upto, F&& f) {
    for (;;) {
      Slot& s = slots_[upto % slots_.size()];
      if (!s.used || s.seq != upto) return;
      f(s.seq, std::span<const std::byte>(s.data));
      s.used = false;
      --used_;
      // upto has already advanced (f's side effect), loop checks the new value
    }
  }

  // high-water mark of slots used at once, and the fixed capacity -- for sizing the
  // buffer from measurement instead of guessing
  std::size_t peak_used() const noexcept { return peak_used_; }
  std::size_t capacity() const noexcept { return slots_.size(); }

private:
  struct Slot {
    bool used = false;
    uint64_t seq = 0;
    // copy of raw bytes (rx buffer will get reused for next read)
    std::vector<std::byte> data;
  };
  std::vector<Slot> slots_;
  std::size_t used_ = 0;
  std::size_t peak_used_ = 0;
};

} // namespace ttt::mold
