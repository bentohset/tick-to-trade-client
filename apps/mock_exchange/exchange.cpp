#include "apps/mock_exchange/exchange.hpp"

#include <algorithm>
#include <cstring>
#include <utility>

namespace mock_exchange {

void MockExchange::poll(uint64_t now) {
  now_ = now;
  if (!client_) {
    if (auto c = listener_.accept()) {
      client_ = std::move(c);
      rx_.reset();
      tx_.reset();
      logged_in_ = false;
      last_rx_ = last_tx_ = now;
    }
  }
  if (client_) {
    const auto n = client_->recv(rx_.tail());
    if (n < 0) drop_ = true;
    else if (n > 0) {
      rx_.commit(static_cast<std::size_t>(n));
      last_rx_ = now;
    }
    if (!rx_.drain([&](char t, std::span<const std::byte> p) { return on_packet(t, p); })) {
      drop_ = true;
    }
    // client went silent: no data and no R heartbeats
    if (now - last_rx_ > cfg_.timeout_ns) drop_ = true;
  }
  fill_due();
  if (client_ && logged_in_ && !drop_) pump();
  if (client_ && logged_in_ && now - last_tx_ > cfg_.heartbeat_ns) {
    soup::put_packet(tx_, soup::kServerHeartbeat, 0, [](std::byte*) {});
    last_tx_ = now;
  }
  if (client_ && !tx_.flush(*client_)) drop_ = true;
  if (drop_) drop_client();
}

bool MockExchange::on_packet(char type, std::span<const std::byte> p) {
  switch (type) {
  case soup::kLoginRequest:
    if (!logged_in_) on_login(p); // a second login on the same connection is ignored
    break;
  case soup::kUnsequenced:
    if (logged_in_) {
      Inbound in{*this};
      ouch::parse_inbound(p, in);
    }
    break;
  case soup::kLogoutRequest: drop_ = true; break;
  default: break; // R heartbeat: last_rx_ already updated
  }
  return !drop_;
}

void MockExchange::on_login(std::span<const std::byte> p) {
  if (p.size() < soup::kLoginRequestLen) {
    drop_ = true;
    return;
  }
  const bool auth = std::memcmp(p.data(), cfg_.user.data(), 6) == 0 &&
                    std::memcmp(p.data() + 6, cfg_.password.data(), 10) == 0;
  if (!auth) return reject_login('A'); // not authorized

  // all spaces = "current session"; anything else must be ours
  const auto* s = p.data() + 16;
  const bool blank = std::all_of(s, s + 10, [](std::byte b) { return b == std::byte{' '}; });
  if (!blank && std::memcmp(s, cfg_.session.data(), 10) != 0) return reject_login('S');

  uint64_t seq = soup::read_num(p.data() + 26, 20);
  // 0 = start from now; past the end = clamp to the end
  if (seq == 0 || seq > log_.size() + 1) seq = log_.size() + 1;
  cursor_ = seq;
  soup::put_packet(tx_, soup::kLoginAccepted, soup::kLoginAcceptedLen,
                   [&](std::byte* q) { soup::encode_login_accepted(q, cfg_.session, seq); });
  last_tx_ = now_;
  logged_in_ = true;
}

void MockExchange::reject_login(char reason) {
  soup::put_packet(tx_, soup::kLoginRejected, 1,
                   [&](std::byte* q) { q[0] = static_cast<std::byte>(reason); });
  drop_ = true; // drop_client() flushes the reject before closing
}

void MockExchange::on_enter(const ouch::EnterOrder& m) {
  const auto tok = m.token();
  if (orders_.contains(tok)) return; // duplicate token: silently ignored -> resend is safe
  const bool ioc = m.tif() == 0;
  ExOrder& o = orders_[tok];
  o = {m.stock(), m.side(), m.price(), m.tif(), m.shares(), 0, !ioc, now_ + cfg_.fill_every_ns};
  // IOC is accepted dead: exercises the "Accepted with state D" path
  emit(ouch::Accepted::kSize, [&](std::byte* p) {
    encode_accepted(p, now_, tok, o, m.shares(), next_ref_++, ioc ? 'D' : 'L');
  });
  if (ioc) o.open = 0;
}

void MockExchange::on_cancel(const ouch::CancelOrder& m) {
  const auto it = orders_.find(m.token());
  if (it == orders_.end() || !it->second.live) return; // unknown or dead: ignore
  ExOrder& o = it->second;
  if (m.shares() >= o.open) return; // already at/below target: a resent X is a no-op
  const uint32_t dec = o.open - m.shares();
  o.open = m.shares();
  o.live = o.open > 0;
  emit(ouch::Canceled::kSize, [&](std::byte* p) { encode_canceled(p, now_, it->first, dec, 'U'); });
}

void MockExchange::on_replace(const ouch::ReplaceOrder& m) {
  const auto repl = m.replacement();
  if (orders_.contains(repl)) return; // duplicate replacement token
  const auto it = orders_.find(m.existing());
  if (it == orders_.end() || !it->second.live) {
    orders_[repl] = ExOrder{}; // remember it (dead) so a resend isn't rejected twice
    emit(ouch::Rejected::kSize, [&](std::byte* p) { encode_rejected(p, now_, repl, 'O'); });
    return;
  }
  ExOrder n = it->second;
  it->second.live = false;
  n.open = m.shares();
  n.price = m.price();
  n.tif = m.tif();
  n.executed = 0; // executions are tracked per token
  n.live = n.open > 0;
  orders_[repl] = n;
  emit(ouch::Replaced::kSize, [&](std::byte* p) {
    encode_replaced(p, now_, repl, n, next_ref_++, n.live ? 'L' : 'D', m.existing());
  });
}

void MockExchange::fill_due() {
  for (auto& [tok, o] : orders_) {
    if (!o.live || now_ < o.next_fill_ns) continue;
    const uint32_t q = std::min(cfg_.fill_qty, o.open);
    o.open -= q;
    o.executed += q;
    o.live = o.open > 0;
    o.next_fill_ns = now_ + cfg_.fill_every_ns;
    position_[o.stock.key()] += o.side == 'B' ? int64_t{q} : -int64_t{q};
    emit(ouch::Executed::kSize,
         [&](std::byte* p) { encode_executed(p, now_, tok, q, o.price, 'A', next_match_++); });
  }
}

void MockExchange::pump() {
  while (cursor_ <= log_.size()) {
    const auto& m = log_[cursor_ - 1];
    const bool ok = soup::put_packet(tx_, soup::kSequenced, m.size(),
                                     [&](std::byte* p) { std::memcpy(p, m.data(), m.size()); });
    if (!ok) return; // tx full, continue next poll (big replays go out in pieces)
    ++cursor_;
    last_tx_ = now_;
    if (cfg_.kill_after && !killed_ && ++sent_ == cfg_.kill_after) {
      // what's queued is flushed, then the connection closes. Everything after
      // this point in log_ reaches the client only through the replay
      killed_ = true;
      drop_ = true;
      return;
    }
  }
}

void MockExchange::drop_client() {
  if (client_) tx_.flush(*client_); // best effort, e.g. a Login Rejected
  client_.reset();
  logged_in_ = false;
  drop_ = false;
}

} // namespace mock_exchange
