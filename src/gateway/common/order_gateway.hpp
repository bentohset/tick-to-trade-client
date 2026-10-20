#pragma once

#include "gateway/common/order_state.hpp"
#include "gateway/common/order_types.hpp"

#include <concepts>
#include <optional>

namespace ttt::gw {

// only used to check concept
struct NullExecSink {
  void on_exec(const ExecEvent&) {}
};

// the contract which all gateways have to define
// gateway protocol picked once at compile time
template <class G>
concept OrderGateway =
    requires(G g, const G& cg, const NewOrder& n, OrderId id, Qty q, Price p, NullExecSink& s) {
      { g.send_new(n) } -> std::same_as<std::optional<OrderId>>;
      { g.send_cancel(id, q) } -> std::same_as<bool>;
      { g.send_replace(id, q, p) } -> std::same_as<bool>;
      { g.poll(s) };
      { cg.connected() } -> std::same_as<bool>;
      { cg.orders() } -> std::same_as<const OrderState&>;
    };

} // namespace ttt::gw
