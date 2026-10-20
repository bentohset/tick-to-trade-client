#pragma once

#include "core/async_logger.hpp"
#include "gateway/common/order_state.hpp"
#include "gateway/common/order_types.hpp"
#include "gateway/ouch/codec.hpp"
#include "gateway/soupbintcp/session.hpp"

#include <cstddef>
#include <optional>

namespace ttt::ouch {

struct OuchConfig {
  soup::SoupConfig soup;
  // tokens must be unique for the day
  TokenPrefix token_prefix{'T', '1'};
  // MPID written into eveyr order
  Firm firm{'T', 'T', 'T', 'C'};
  // capacity reserved in OrderState
  std::size_t max_orders = 1 << 16;
};

struct GatewayStats {
  uint64_t resent = 0;
  uint64_t unknown_token = 0;
  uint64_t bad_message = 0;
  uint64_t broken_trades = 0;
};

// Sits between the trading loop and the soupbintcp session and handles the
// encoding/decoding of OUCH messages
class OuchGateway {
public:
  OuchGateway(const OuchConfig& cfg, core::LogChannel& logger)
      : cfg_(cfg), logger_(logger), soup_(cfg.soup, logger), orders_(cfg.max_orders) {}

  std::optional<gw::OrderId> send_new(const gw::NewOrder& n) {
    if (!connected()) return std::nullopt;
    // add the order to table in PendingNew state.
    const auto id = orders_.create(n);
    // if created successfully, encode OUCH O message and send it
    if (id) send_enter(*id, *orders_.find(*id));
    return id;
  }

  // remaining = shares left working, 0 = cancel all
  bool send_cancel(gw::OrderId id, gw::Qty remaining = 0) {
    // check if OrderState allows cancel
    // begin_cancel moves order to PendingCancel and store target_qty=remaining
    if (!connected() || !orders_.begin_cancel(id, remaining)) return false;
    // send OUCH X message
    send_x(id, *orders_.find(id));
    return true;
  }

  bool send_replace(gw::OrderId id, gw::Qty qty, gw::Price px) {
    // begin_replace checks i Order is Live and moves to PendingReplace
    // stores new qty and price as targets
    if (!connected() || !orders_.begin_replace(id, qty, px)) return false;
    // send OUCH U message
    send_u(id, *orders_.find(id));
    return true;
  }

  // drains the socket, applies every sequenced message, emits ExecEvents
  // S is the sink that receives events (ie. strategy or position keeper)
  template <gw::ExecSink S> void poll(S& sink) {
    const uint64_t now = core::read_ticks();
    // build callback object the session calls into. lives on stack for this call
    Handler<S> h{*this, sink, now};
    // start control in the session - reconnects/reads/splits packets/sequences/heartbeats
    soup_.poll(h, core::ticks_to_ns(now));
  }

  bool connected() const { return soup_.logged_in(); }
  const gw::OrderState& orders() const { return orders_; }
  const soup::Session& session() const { return soup_; }
  const GatewayStats& stats() const { return stats_; }
  // current token of an order, for reconciliation against the exchange
  Token token(gw::OrderId id) const {
    return make_token(cfg_.token_prefix, id, orders_.find(id)->gen);
  }

private:
  // soup::Session callbacks - wraps all objects needed for session
  template <class S> struct Handler {
    OuchGateway& self;
    S& sink;
    uint64_t rx_tsc;

    // m is one OUCH message from the exchange taken from one S packet
    void on_sequenced(std::span<const std::byte> m, uint64_t /*seq*/) {
      // per-message-type handler
      Dispatch<S> d{self, sink, rx_tsc};
      // looks at the type byte, wrap the bytes in matching view and call d.on
      if (parse_outbound(m, d) == ParseResult::BadLength) ++self.stats_.bad_message;
      // restart persistence: save seq here, after the apply
    }

    // After login accepted, resend anything that was in-flight when link dropped
    void on_logged_in() { self.resend_pending(); }
    void on_disconnected() {} // nothing to undo: in-flight requests stay pending
  };

  // OUCH message -> OrderState transition -> ExecEvent
  template <class S> struct Dispatch {
    OuchGateway& self;
    S& sink;
    uint64_t rx_tsc;

    void on(const Accepted& m) {
      if (const auto t = self.ours(m.token())) {
        emit(self.orders_.on_accepted(t->id, m.order_state() == 'L', m.shares(), m.order_ref()));
      }
    }
    void on(const Rejected& m) {
      if (const auto t = self.ours(m.token())) {
        emit(self.orders_.on_rejected(t->id, t->gen, m.reason()));
      }
    }
    void on(const Executed& m) {
      if (const auto t = self.ours(m.token())) {
        emit(self.orders_.on_executed(t->id, m.shares(), m.price(), m.match_number()));
      }
    }
    void on(const Canceled& m) {
      if (const auto t = self.ours(m.token())) {
        emit(self.orders_.on_canceled(t->id, m.decrement(), m.reason()));
      }
    }
    void on(const Replaced& m) {
      if (const auto t = self.ours(m.token())) {
        emit(
            self.orders_.on_replaced(t->id, t->gen, m.order_state() == 'L', m.shares(), m.price()));
      }
    }
    void on(const CancelReject& m) {
      if (const auto t = self.ours(m.token())) emit(self.orders_.on_cancel_rejected(t->id));
    }
    void on(const BrokenTrade& m) {
      // correcting the position belongs to position_keeper (step 7)
      ++self.stats_.broken_trades;
      self.logger_.log(core::LogLevel::Error, "broken trade, match {}", m.match_number());
    }

    void emit(std::optional<gw::ExecEvent> ev) {
      if (!ev) return;
      // stamp when this poll's bytes were read and hand it to sink (strategy/position keeper)
      ev->rx_tsc = rx_tsc;
      sink.on_exec(*ev);
    }
  };

  std::optional<TokenParts> ours(const Token& t) {
    // check prefix, decode 9 id digits + 3 gen digits
    const auto r = parse_token(t, cfg_.token_prefix);
    if (!r) ++stats_.unknown_token;
    return r;
  }

  // Called right after Login Accepted, before the replay arrives. Each resend is
  // idempotent at the exchange, so it doesn't matter whether the replay already
  // holds the answer:
  //   O: same token -> duplicate, silently ignored
  //   X: carries target remaining, not a decrement -> no-op if already applied
  //   U: same replacement token -> duplicate, ignored
  void resend_pending() {
    std::size_t n = 0;
    orders_.for_each_pending([&](gw::OrderId id, const gw::Order& o) {
      switch (o.status) {
        case gw::OrderStatus::PendingNew: send_enter(id, o); break;
        case gw::OrderStatus::PendingCancel: send_x(id, o); break;
        case gw::OrderStatus::PendingReplace: send_u(id, o); break;
        default: break;
      }
      ++n;
    });
    stats_.resent += n;
    if (n) logger_.log(core::LogLevel::Info, "resent {} in-flight requests", n);
  }

  void send_enter(gw::OrderId id, const gw::Order& o) {
    const Token t = make_token(cfg_.token_prefix, id, 0);
    soup_.send_unsequenced(EnterOrder::kSize,
                           [&](std::byte* p) { encode_enter(p, t, o.req, cfg_.firm); });
  }

  void send_x(gw::OrderId id, const gw::Order& o) {
    const Token t = make_token(cfg_.token_prefix, id, o.gen);
    soup_.send_unsequenced(CancelOrder::kSize,
                           [&](std::byte* p) { encode_cancel(p, t, o.target_qty); });
  }

  void send_u(gw::OrderId id, const gw::Order& o) {
    const Token old_t = make_token(cfg_.token_prefix, id, o.gen);
    const Token new_t = make_token(cfg_.token_prefix, id, static_cast<uint16_t>(o.gen + 1));
    soup_.send_unsequenced(ReplaceOrder::kSize, [&](std::byte* p) {
      encode_replace(p, old_t, new_t, o.target_qty, o.target_px, tif_secs(o.req.tif));
    });
  }

  OuchConfig cfg_;
  core::LogChannel& logger_;
  soup::Session soup_;
  gw::OrderState orders_;
  GatewayStats stats_;
};

} // namespace ttt::ouch
