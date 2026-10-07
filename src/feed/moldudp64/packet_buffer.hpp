#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace ttt::mold {

// A fixed-capacity buffer for packets that arrive after a gap until the fill lands.
// When session detects a gap (seq > expected_), the out-of-order packets wait in this
// packet buffer until expected_ catches up to them.
// Does a linear scan during Recovery, never on Live fast path.
class PacketBuffer {
public:
  explicit PacketBuffer(std::size_t capacity) : slots_(capacity) {}

  // Store a packet
  bool put(uint64_t seq, std::span<const std::byte> pkt) {
    // check duplicates
    for (auto& s : slots_) {
      // TODO: probably can optimize away
      if (s.used && s.seq == seq) return true;
    }
    // finds a free slot with used=false, taking the first one
    for (auto& s : slots_) {
      if (!s.used) {
        s.used = true;
        s.seq = seq;
        s.data.assign(pkt.begin(), pkt.end());
        return true;
      }
    }
    // buffer is full, caller should detect failure
    return false;
  }

  // session to check if any buffered packets are deliverable
  // upto = expected (pass by reference so loop sees live value after each delivery)
  // returns the lowest-seq slot with seq <= upto to f, draining as it walks.
  template <class F> void pop_ready(const uint64_t& upto, F&& f) {
    for (;;) {
      // find the best candidate, the lowest seq packet buffered
      auto best = slots_.end();
      for (auto it = slots_.begin(); it != slots_.end(); ++it) {
        if (it->used && it->seq <= upto && (best == slots_.end() || it->seq < best->seq)) {
          best = it;
        }
      }
      // no candidate found, no buffered packets are deliverable
      if (best == slots_.end()) return;
      // hand the packet to the callback
      f(best->seq, std::span<const std::byte>(best->data));
      // free the slot
      best->used = false;

      // keep iterating until no candidates left
    }
  }

  // returns the smallest seq among all slots
  std::optional<uint64_t> lowest_seq() const {
    std::optional<uint64_t> lo;
    for (auto& s : slots_) {
      if (s.used && (!lo || s.seq < *lo)) {
        lo = s.seq;
      }
    }
    return lo;
  }

private:
  // a fixed-size array of slots (either empty or holding a buffered packet seqno)
  struct Slot {
    bool used = false; // empty
    uint64_t seq = 0;
    // copy of raw bytes (rx buffer will get reused for next read)
    std::vector<std::byte> data;
  };
  std::vector<Slot> slots_;
};

} // namespace ttt::mold
