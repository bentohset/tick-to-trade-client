#include "gateway/common/order_state.hpp"

#include "gateway/common/order_types.hpp"

#include <algorithm>
#include <optional>

namespace ttt::gw {

std::optional<OrderId> OrderState::create(const NewOrder& n) {
  if (orders_.size() == orders_.capacity()) return std::nullopt;
  orders_.push_back({.req = n, .price = n.price, .open_qty = n.qty});
  return static_cast<OrderId>(orders_.size() - 1);
}

bool OrderState::begin_cancel(OrderId id, Qty remaining) {
  Order* o = get(id);
  if (!o || o->status != OrderStatus::Live || remaining >= o->open_qty) return false;
  o->status = OrderStatus::PendingCancel;
  o->target_qty = remaining;
  return true;
}

bool OrderState::begin_replace(OrderId id, Qty qty, Price px) {
  Order* o = get(id);
  if (!o || o->status != OrderStatus::Live) return false;
  o->status = OrderStatus::PendingReplace;
  o->target_qty = qty;
  o->target_px = px;
  return true;
}

std::optional<ExecEvent> OrderState::on_accepted(OrderId id, bool live, Qty open, uint64_t ref) {
  Order* o = get(id);
  if (!o || o->status != OrderStatus::PendingNew) return ignore();
  o->exch_ref = ref;
  // not live: accepted but already dead (ie. IOC that didnt trade), no cancel
  o->open_qty = live ? open : 0;
  o->status = live ? OrderStatus::Live : OrderStatus::Cancelled;
  return event(id, *o, ExecType::Accepted);
}

std::optional<ExecEvent> OrderState::on_rejected(OrderId id, uint16_t gen, char reason) {
  Order* o = get(id);
  if (!o) return ignore();
  if (o->status == OrderStatus::PendingNew && gen == 0) {
    o->status = OrderStatus::Rejected;
    o->open_qty = 0;
    return event(id, *o, ExecType::Rejected, reason);
  }
  // reject replacement: original stays live under old generation
  if (o->status == OrderStatus::PendingReplace && gen == o->gen + 1) {
    o->status = OrderStatus::Live;
    return event(id, *o, ExecType::ReplaceRejected, reason);
  }
  return ignore();
}

std::optional<ExecEvent> OrderState::on_executed(OrderId id, Qty qty, Price px, uint64_t match) {
  Order* o = get(id);
  // never before Accepted on the wire; valid while Live or pendings
  if (!o || o->status == OrderStatus::PendingNew || is_terminal(o->status)) return ignore();
  qty = std::min(qty, o->open_qty);
  o->open_qty -= qty;
  o->cum_qty += qty;
  // fills can race a pending cancel/replace - full fill wins
  if (o->open_qty == 0) o->status = OrderStatus::Filled;
  auto ev = event(id, *o, ExecType::Executed);
  ev.last_qty = qty;
  ev.last_px = px;
  ev.match = match;
  return ev;
}

std::optional<ExecEvent> OrderState::on_canceled(OrderId id, Qty decrement, char reason) {
  Order* o = get(id);
  if (!o || o->status == OrderStatus::PendingNew || is_terminal(o->status)) return ignore();
  decrement = std::min(decrement, o->open_qty);
  o->open_qty -= decrement;
  if (o->open_qty == 0) {
    o->status = OrderStatus::Cancelled;
  } else if (o->status == OrderStatus::PendingCancel) {
    o->status = OrderStatus::Live;
  }
  // unsolicited partial cancel leaves Live / PendingReplace as is
  auto ev = event(id, *o, ExecType::Canceled, reason);
  ev.last_qty = decrement;
  return ev;
}

std::optional<ExecEvent> OrderState::on_replaced(OrderId id, uint16_t gen, bool live, Qty open,
                                                 Price px) {
  Order* o = get(id);
  if (!o || o->status != OrderStatus::PendingReplace || gen != o->gen + 1) return ignore();
  o->gen = gen;
  o->price = px;
  o->open_qty = live ? open : 0;
  o->status = o->open_qty ? OrderStatus::Live : OrderStatus::Cancelled;
  return event(id, *o, ExecType::Replaced);
}

std::optional<ExecEvent> OrderState::on_cancel_rejected(OrderId id) {
  Order* o = get(id);
  if (!o || o->status != OrderStatus::PendingCancel) return ignore();
  o->status = OrderStatus::Live;
  return event(id, *o, ExecType::CancelRejected);
}

} // namespace ttt::gw
