#pragma once

#include <atomic>
#include <cstdint>

namespace ttt::risk {

enum class KillReason : uint8_t { None, Manual, Signal, Shutdown, Loss, FeedDown, GatewayDown };

// Latches: once tripped, it stays tripped until process restarts
class KillSwitch {
public:
  void trip(KillReason r) noexcept {
    auto none = static_cast<uint8_t>(KillReason::None);
    reason_.compare_exchange_strong(none, static_cast<uint8_t>(r), std::memory_order_relaxed);
  }
  bool tripped() const noexcept { return reason_.load(std::memory_order_relaxed) != 0; }
  KillReason reason() const noexcept {
    return KillReason(reason_.load(std::memory_order_relaxed));
  }

private:
  std::atomic<uint8_t> reason_{0};
};

}
