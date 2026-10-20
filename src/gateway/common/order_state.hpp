#pragma once

#include "gateway/common/order_types.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace ttt::gw {

struct Order {
  // as first sent
  NewOrder req;
  // current price (changes on replace)
  Price price = 0;
  // shares still working at the exchange
  Qty open_qty = 0;
  Qty cum_qty = 0;
  // PendingCancel: remaining asked for; PendingReplace: new qty
  Qty target_qty = 0;
  // PendingReplace: new price
  Price target_px = 0;
  uint64_t exch_ref = 0;
  // replace generation (token in OUCH)
  uint16_t gen = 0;
  OrderStatus status = OrderStatus::PendingNew;
};

// Owner of own-order state. Shared by all gateway protocols
class OrderState {
public:
  // TODO: consider first-touch page faults
  explicit OrderState(std::size_t capacity) { orders_.reserve(capacity); }

  // TODO: consider inlining all of these functions or enabling -flto on release

  // local actions, called before sending
  std::optional<OrderId> create(const NewOrder& n);
  bool begin_cancel(OrderId id, Qty remaining);
  bool begin_replace(OrderId id, Qty qty, Price px);

  // exchange events
  std::optional<ExecEvent> on_accepted(OrderId id, bool live, Qty open, uint64_t ref);
  std::optional<ExecEvent> on_rejected(OrderId id, uint16_t gen, char reason);
  std::optional<ExecEvent> on_executed(OrderId id, Qty qty, Price px, uint64_t match);
  std::optional<ExecEvent> on_canceled(OrderId id, Qty decrement, char reason);
  std::optional<ExecEvent> on_replaced(OrderId id, uint16_t gen, bool live, Qty open, Price px);
  std::optional<ExecEvent> on_cancel_rejected(OrderId id);

  const Order* find(OrderId id) const { return id < orders_.size() ? &orders_[id] : nullptr; }
  std::size_t size() const { return orders_.size(); }
  uint64_t ignored() const { return ignored_; }

  // every order with a request in flight
  // used to resend after reconnect
  template <class F> void for_each_pending(F&& f) const {
    for (OrderId id = 0; id < orders_.size(); ++id) {
      if (is_pending(orders_[id].status)) f(id, orders_[id]);
    }
  }

private:
  Order* get(OrderId id) { return id < orders_.size() ? &orders_[id] : nullptr; }
  std::nullopt_t ignore() {
    ++ignored_;
    return std::nullopt;
  }
  ExecEvent event(OrderId id, const Order& o, ExecType t, char reason = ' ') const {
    return {0, 0, id, t, o.status, reason, 0, 0, o.open_qty, o.cum_qty};
  }

  std::vector<Order> orders_; // index = OrderId
  uint64_t ignored_ = 0;
};

} // namespace ttt::gw
