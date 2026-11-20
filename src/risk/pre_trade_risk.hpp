#pragma once

#include "gateway/common/order_gateway.hpp"
#include "gateway/common/order_state.hpp"
#include "gateway/common/order_types.hpp"
#include "risk/kill_switch.hpp"
#include "risk/position_keeper.hpp"
#include <array>
#include <cstdint>
#include <expected>
namespace ttt::risk {

struct RiskLimits {
  gw::Qty max_order_qty = 1'000;
  uint64_t max_notional = 1'000'000'000; // $1M, Price(4) * shares
  int64_t max_position = 5'000; // |position + same-side open + new| per symbol
  uint32_t band_bps = 50; // price must be within +-0.5% of reference mid
  uint32_t max_orders_per_sec = 200; // new orders + replaces
  int64_t max_loss = 10'000'000; // $10k; engine tripes kill switch past it
};

enum class Reject : uint8_t { None, Killed, Qty, Notional, Position, PriceBand, Rate, NoRef, NotLive, Gateway, Count_ };

template<gw::OrderGateway G>
class RiskGate {
public:
  RiskGate(G& gw, PositionKeeper& pk, const KillSwitch& ks, const RiskLimits& l)
    : gw_(gw), pk_(pk), ks_(ks), lim_(l) , cost_ns_(l.max_orders_per_sec ? kSecond / l.max_orders_per_sec : 0) {}

  std::expected<gw::OrderId, Reject> new_order(uint16_t slot, gw::Side side, gw::Price px, gw::Qty qty, gw::Tif tif, gw::Price ref_mid, uint64_t now_ns) {
    if (auto r = check(slot, side, px, qty, ref_mid, now_ns); r != Reject::None) return reject(r);
    const auto id = gw_.send_new({pk_.at(slot).symbol, px, qty, side, tif});
    if (!id) return reject(Reject::Gateway);
    pk_.on_sent(*id, slot, side, qty);
    return *id;
  }

  std::optional<Reject> replace(gw::OrderId id, gw::Qty qty, gw::Price px, gw::Price ref_mid, uint64_t now_ns) {
    const gw::Order* o = gw_.orders().find(id);
    // fail early with a reason; the gateway returns false
    if (!o || o->status != gw::OrderStatus::Live) return reject(Reject::NotLive);

    // only the increase adds exposure; shrinking a quote never breaches position
    const gw::Qty added = qty > o->open_qty ? qty - o->open_qty : 0;
    const uint16_t slot = pk_.slot_of_order(id);
    const Reject r = check(slot, o->req.side, px, qty, added, ref_mid, now_ns);
    if (r != Reject::None) return reject(r);

    if (!gw_.send_replace(id, qty, px)) return reject(Reject::Gateway);
    pk_.on_replace_sent(id, qty);
    return {};
  }

  // cancelled always allowed, even after kill switch trip
  bool cancel(gw::OrderId id) { return gw_.send_cancel(id, 0); }

  const PositionKeeper& positions() const { return pk_; }
  const gw::OrderState& orders() const { return gw_.orders(); }
  uint64_t rejects(Reject r) const { return rejects_[size_t(r)]; }

private:
  static constexpr uint64_t kSecond = 1'000'000'000;

  Reject check(uint16_t slot, gw::Side side, gw::Price px, gw::Qty qty, gw::Price mid, uint64_t now) {
    if (ks_.tripped()) return Reject::Killed;
    if (qty == 0 || qty > lim_.max_order_qty) return Reject::Qty;
    if (uint64_t(px) * qty > lim_.max_notional) return Reject::Notional;
    if (mid == 0) return Reject::NoRef;
    const uint64_t dist = px > mid ? px - mid : mid - px;
    if (dist * 10'000 > uint64_t(lim_.band_bps) * mid) return Reject::PriceBand;
    // worst case: every working order on this side fills
    const auto& p = pk_.at(slot);
    const int64_t worst = side == gw::Side::Buy ? p.position + p.open_buy + qty
                                                : -(p.position - p.open_sell - int64_t(qty));
    if (worst > lim_.max_position) return Reject::Position;
    if (!take_token(now)) return Reject::Rate;
    return Reject::None;
  }

  // token bucket: refills max_orders_per_sec tokens per second for rate limiting
  bool take_token(uint64_t now_ns) {
    if (cost_ns_ == 0) return true;
    budget_ns_ = std::min(kSecond, budget_ns_ + (now_ns - last_refill_ns_));
    last_refill_ns_ = now_ns;
    if (budget_ns_ < cost_ns_) return false;
    budget_ns_ -= cost_ns_;
    return true;
  }

  std::unexpected<Reject> reject(Reject r) {
    ++rejects_[size_t(r)];
    return std::unexpected(r);
  }

  G& gw_;
  PositionKeeper& pk_;
  const KillSwitch& ks_;
  RiskLimits lim_;
  uint64_t cost_ns_;
  uint64_t budget_ns_ = 0;
  uint64_t last_refill_ns_ = 0;
  std::array<uint64_t, static_cast<std::size_t>(Reject::Count_)> rejects_{};
};

}
