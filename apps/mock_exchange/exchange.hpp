#pragma once

#include "apps/common/constants.hpp"
#include "apps/mock_exchange/ouch_encode.hpp"
#include "gateway/ouch/codec.hpp"
#include "gateway/soupbintcp/framing.hpp"
#include "gateway/soupbintcp/wire.hpp"
#include "net/tcp_client.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <vector>

namespace mock_exchange {

namespace itch = ttt::itch;
namespace net = ttt::net;
namespace ouch = ttt::ouch;
namespace soup = ttt::soup;

struct ExchangeConfig {
  uint16_t port = apps::defaults::kGatewayPort;
  soup::SessionId session{'O', 'U', 'C', 'H', '0', '0', '0', '0', '0', '1'};
  soup::Username user{'t', 't', 't', ' ', ' ', ' '};
  soup::Password password{'p', 'w', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '};
  uint32_t fill_qty = 100;               // shares per execution
  uint64_t fill_every_ns = 1'000'000;    // one execution per live order per interval
  uint64_t kill_after = 0;               // drop the client after this many S packets (0 = never), once
  uint64_t heartbeat_ns = 1'000'000'000; // send H if nothing else was sent for this long
  uint64_t timeout_ns = 15'000'000'000;  // drop the client if nothing was received for this long
};

// Minimal SoupBinTCP/OUCH server. A class so gateway_session_test can run it
// in a thread in the same process
class MockExchange {
public:
  explicit MockExchange(const ExchangeConfig& cfg)
    : cfg_(cfg), listener_(net::TcpListener::listen("127.0.0.1", cfg.port)) {}

  // accept, read, fill, send, heartbeat. Fills continue while no client is
  // connected: those are exactly the executions the replay has to recover
  void poll(uint64_t now_ns);
  void kill() { drop_ = true; } // tests / SIGUSR1

  // records for reconciliation
  const ExOrder* order(const ouch::Token& t) const {
    const auto it = orders_.find(t);
    return it == orders_.end() ? nullptr : &it->second;
  }
  int64_t position(itch::Symbol s) const {
    const auto it = position_.find(s.key());
    return it == position_.end() ? 0 : it->second;
  }
  const std::map<uint64_t, int64_t>& positions() const { return position_; }
  uint64_t executions() const { return next_match_ - 1; }
  uint64_t sequenced() const { return log_.size(); }

private:
  // parse_inbound needs a public on(); keep the handlers out of the class's public API
  struct Inbound {
    MockExchange& self;
    void on(const ouch::EnterOrder& m) { self.on_enter(m); }
    void on(const ouch::CancelOrder& m) { self.on_cancel(m); }
    void on(const ouch::ReplaceOrder& m) { self.on_replace(m); }
  };

  bool on_packet(char type, std::span<const std::byte> p); // false = stop draining
  void on_login(std::span<const std::byte> p);
  void reject_login(char reason);
  void on_enter(const ouch::EnterOrder& m);
  void on_cancel(const ouch::CancelOrder& m);
  void on_replace(const ouch::ReplaceOrder& m);
  void fill_due();
  void pump(); // log_[cursor_ - 1 ..] -> S packets, as far as tx_ has room
  void drop_client();

  // append to the sequenced log; pump() sends it, live or as replay
  template <class Enc> void emit(std::size_t len, Enc&& enc) {
    auto& m = log_.emplace_back(len);
    enc(m.data());
  }

  ExchangeConfig cfg_;
  net::TcpListener listener_;
  std::optional<net::TcpClient> client_;
  soup::RecvBuffer rx_;
  soup::SendBuffer tx_;
  bool logged_in_ = false;
  bool drop_ = false;
  bool killed_ = false;
  uint64_t now_ = 0, last_rx_ = 0, last_tx_ = 0;
  std::vector<std::vector<std::byte>> log_; // seq n is log_[n-1]
  uint64_t cursor_ = 1;                     // next seq to send to the client
  uint64_t sent_ = 0;                       // S packets sent, for kill_after
  std::map<ouch::Token, ExOrder> orders_;   // every token ever seen, live or dead
  std::map<uint64_t, int64_t> position_;    // symbol key -> client's signed shares
  uint64_t next_ref_ = 1, next_match_ = 1;
};

} // namespace mock_exchange
