#pragma once

#include "feed/itch/messages.hpp"
#include "gateway/common/order_state.hpp"
#include "gateway/common/order_types.hpp"

#include <cstdint>
#include <cstdio>
#include <optional>
#include <span>
#include <vector>

namespace ttt::risk {

struct SymbolPos {
  itch::Symbol symbol;
  int64_t position = 0; // signed filled shares
  int64_t open_buy = 0, open_sell = 0;
  int64_t cash = 0;
  int64_t pnl(gw::Price mark) const { return cash + position * int64_t(mark); }
};

// Per-symbol positions and open exposure, built from ExecEvents.
// Each event carries open_qty after it was applied; the keeper stores each
// order's last open_qty and applies the difference
class PositionKeeper {
public:
  PositionKeeper(const gw::OrderState& orders, std::size_t max_orders, std::size_t max_symbols)
      : orders_(orders), max_symbols_(max_symbols), open_(max_orders, 0), slot_of_(max_orders, 0) {
        syms_.reserve(max_symbols);
      }

  // config time, before trading. Returns the slot or nullopt if full
  std::optional<uint16_t> add_symbol(itch::Symbol s);
  std::optional<uint16_t> slot_of(itch::Symbol s) const;
  uint16_t slot_of_order(gw::OrderId id) const { return slot_of_[id]; }

  // hot path: called by RiskGate right after the gateway accepted a send
  void on_sent(gw::OrderId id, uint16_t slot, gw::Side side, gw::Qty qty) {
    slot_of_[id] = slot;
    open_[id] = qty;
    exposure(syms_[slot], side) += qty;
  }

  // hot path: called by RiskGate right after gateway accepted replace.
  // replace up counts as exposure while in flight, so a new same-side order
  // cant slip under position limit before Replaced arrives.
  void on_replace_sent(gw::OrderId id, gw::Qty qty) {
    if (qty <= open_[id]) return;
    exposure(syms_[slot_of_[id]], orders_.find(id)->req.side) += int64_t(qty) - int64_t(open_[id]);
    open_[id] = qty;
  }

  // exec path, once per ExecEvent
  void on_exec(gw::ExecEvent& e) {
    const gw::Order* o = orders_.find(e.id);
    if (!o) return;
    SymbolPos& p = syms_[slot_of_[e.id]];
    const bool buy = o->req.side == gw::Side::Buy;

    // whatever the exchange says is still working, signed so a replace up works too
    exposure(p, o->req.side) += int64_t(e.open_qty) - int64_t(open_[e.id]);
    open_[e.id] = e.open_qty;

    if (e.type == gw::ExecType::Executed) {
      const int64_t q = buy ? int64_t(e.last_qty) : -int64_t(e.last_qty);
      p.position += q;
      p.cash -= q * int64_t(e.last_px);
    }
  }

  // hot reads used by the risk check and strategy
  const SymbolPos& at(uint16_t slot) const { return syms_[slot]; }
  std::span<const SymbolPos> all() const { return syms_; }

  // true once nothing is working on any symbol
  bool no_exposure() const;

  void print(std::FILE* out) const;

private:
  int64_t& exposure(SymbolPos& p, gw::Side s) {
    return s == gw::Side::Buy ? p.open_buy : p.open_sell;
  }
  const gw::OrderState& orders_;
  std::size_t max_symbols_;
  std::vector<SymbolPos> syms_; // index=slot; reserved
  std::vector<gw::Qty> open_; // index=OrderId; last open_qty seen
  std::vector<uint16_t> slot_of_; // index=OrderId
};

}
