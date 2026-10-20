#pragma once

#include "feed/itch/messages.hpp"

#include <cstdint>

namespace ttt::gw {

using OrderId = uint32_t;
using Price = uint32_t;
using Qty = uint32_t;

enum class Side : uint8_t { Buy, Sell, SellShort };
enum class Tif : uint8_t { Day, Ioc };

struct NewOrder {
  itch::Symbol symbol;
  Price price;
  Qty qty;
  Side side;
  Tif tif;
};

enum class OrderStatus : uint8_t {
  PendingNew,
  Live,
  PendingCancel,
  PendingReplace,
  Filled,
  Cancelled,
  Rejected
};

inline bool is_terminal(OrderStatus s) { return s >= OrderStatus::Filled; }
inline bool is_pending(OrderStatus s) {
  return s == OrderStatus::PendingNew || s == OrderStatus::PendingCancel ||
         s == OrderStatus::PendingReplace;
}

enum class ExecType : uint8_t {
  Accepted,
  Rejected,
  Executed,
  Canceled,
  Replaced,
  ReplaceRejected,
  CancelRejected
};

// Protocol-neutral event the gateway emits after applying a message to OrderState
struct ExecEvent {
  uint64_t rx_tsc;
  uint64_t match; // Executed: exchange match number (same as ITCH E/C)
  OrderId id;
  ExecType type;
  OrderStatus status; // the status after this event
  char reason;
  Qty last_qty; // Executed: num shares filled; Canceled: num shares removed
  Price last_px;
  Qty open_qty;
  Qty cum_qty;
};

template <class S>
concept ExecSink = requires(S s, const ExecEvent& e) { s.on_exec(e); };

} // namespace ttt::gw
