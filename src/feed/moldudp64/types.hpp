#pragma once
#include <cstdint>

namespace ttt::mold {

enum class FeedState : uint8_t { Live, Recovering, Ended, Failed };

struct Stats {
  uint64_t messages = 0, heartbeats = 0;
  uint64_t duplicates = 0, overlaps = 0, gaps = 0, wrong_session = 0;
  uint64_t buffer_full = 0; // PacketBuffer ran out of slots mid-gap -> FeedState::Failed
};

struct Gap {
  uint64_t first, end;
}; // [first, end)

} // namespace ttt::mold
