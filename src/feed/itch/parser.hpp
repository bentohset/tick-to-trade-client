#pragma once

#include "feed/itch/messages.hpp"
#include <cstddef>
#include <cstdint>
#include <span>

namespace ttt::itch {

enum class ParseResult : uint8_t { Ok, Unknown, BadLength };

// Handlers derive from this and hide only the methods they care about.
// Calls resolve statically on the derived type: no vtable.
struct NullHandler {
  void on(const SystemEvent&) {}
  void on(const StockDirectory&) {}
  void on(const StockTradingAction&) {}
  void on(const AddOrder&) {}
  void on(const AddOrderMpid&) {}
  void on(const OrderExecuted&) {}
  void on(const OrderExecutedWithPrice&) {}
  void on(const OrderCancel&) {}
  void on(const OrderDelete&) {}
  void on(const OrderReplace&) {}
  void on(const Trade&) {}
  void on_other(char /*type*/, std::span<const std::byte>) {} // Y L V W K J h Q B I N O
};

template <class Handler>
[[gnu::always_inline]] inline ParseResult parse(std::span<const std::byte> msg, Handler& h) {
  if (msg.empty()) return ParseResult::BadLength;
  const auto type = static_cast<unsigned char>(msg[0]);
  const auto expected = kSizeByType[type];
  if (expected == 0) return ParseResult::Unknown;
  // framing desync possible
  if (msg.size() != expected) return ParseResult::BadLength;

  const std::byte* p = msg.data();
  switch (type) {
    case 'A': h.on(AddOrder{p}); break;
    case 'F': h.on(AddOrderMpid{p}); break;
    case 'E': h.on(OrderExecuted{p}); break;
    case 'C': h.on(OrderExecutedWithPrice{p}); break;
    case 'X': h.on(OrderCancel{p}); break;
    case 'D': h.on(OrderDelete{p}); break;
    case 'U': h.on(OrderReplace{p}); break;
    case 'P': h.on(Trade{p}); break;
    case 'S': h.on(SystemEvent{p}); break;
    case 'R': h.on(StockDirectory{p}); break;
    case 'H': h.on(StockTradingAction{p}); break;
    default: h.on_other(static_cast<char>(type), msg); break;
  }

  return ParseResult::Ok;
}

} // namespace ttt::itch
