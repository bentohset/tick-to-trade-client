#pragma once

#include "feed/moldudp64/session.hpp"
#include "feed/moldudp64/types.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>

namespace ttt::mold {

// Decides when to ask the server to re-send gapped messages.
// Purely decision-making, the caller should re-request the packets
class Recovery {
public:
  Recovery(uint64_t timeout_ns, uint32_t max_retries)
      : timeout_ns_(timeout_ns), max_retries_(max_retries) {}

  // called on every loop iteration while feed is recovering
  // returns a range to re-request now
  std::optional<Gap> poll(const Session& s, uint64_t now_ns) noexcept {
    // check for an active gap
    const auto gap = s.gap();
    if (!gap) {
      active_ = false;
      return std::nullopt;
    }
    // check if theres already an active request that already covers the gap.
    // if the reported gap's start is still below our end_, server may still be sending
    if (active_ && gap->first < end_) {
      // expected_ has moved forward since request was sent, there is progress
      // re-requested packets are arriving
      if (gap->first != first_) {
        // update the state only
        first_ = gap->first;
        sent_ns_ = now_ns;
        retries_ = 0;
        return std::nullopt;
      }
      // no progress and timeout has not passed, keep waiting
      if (now_ns - sent_ns_ < timeout_ns_) return std::nullopt;

      // no progress and timeout passed, retry
      if (++retries_ > max_retries_) {
        timed_out_ = true;
        return std::nullopt;
      }
    } else {
      // new gap or previous request fully answered
      retries_ = 0;
    }
    // create new gap request, capped to at most kMaxRequestCount messages wide
    // (the wire request-count field is 16 bits) -- not an absolute end sequence cap
    const Gap req{gap->first, std::min<uint64_t>(gap->end, gap->first + kMaxRequestCount)};
    active_ = true;
    first_ = req.first;
    end_ = req.end;
    sent_ns_ = now_ns;
    return req;
  }

  bool timed_out() const noexcept { return timed_out_; }

private:
  uint64_t timeout_ns_;
  uint32_t max_retries_;
  bool active_{false};
  uint64_t first_{0};
  uint64_t end_{0};
  uint64_t sent_ns_{0};
  uint32_t retries_{0};
  bool timed_out_{false};
};

} // namespace ttt::mold
