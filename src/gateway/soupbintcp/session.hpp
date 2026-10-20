#pragma once

#include "core/async_logger.hpp"
#include "gateway/soupbintcp/framing.hpp"
#include "gateway/soupbintcp/wire.hpp"
#include "net/tcp_client.hpp"
#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
namespace ttt::soup {

struct SoupConfig {
  const char* host = "127.0.0.1";
  uint16_t port = 40001;
  Username user{};
  Password password{};
  // send R if nothing is sent for this long
  uint64_t heartbeat_ns = 1'000'000'000;
  // drop the link if nothing received for this long
  uint64_t timeout_ns = 15'000'000'000;
  uint64_t backoff_min_ns = 50'000'000;
  uint64_t backoff_max_ns = 2'000'000'000;
};

enum class SoupState : uint8_t { Disconnected, LoginSent, LoggedIn, Rejected, Ended };

struct SoupStats {
  uint64_t connects = 0;
  uint64_t disconnects = 0;
  uint64_t sequenced = 0;
  uint64_t heartbeats_rx = 0;
  uint64_t heartbeats_tx = 0;
};

template <class H>
concept SoupHandler = requires(H h, std::span<const std::byte> msg, uint64_t seq) {
  // single S payload; next_seq advances only after this returns
  h.on_sequenced(msg, seq);
  // after login accepted, before any replayed S
  h.on_logged_in();
  h.on_disconnected();
};

// Client side SoupBinTCP session - login, heartbeats, reconnect with backoff,
// and counting seqno of S packets
class Session {
public:
  Session(const SoupConfig& cfg, core::LogChannel& logger) : cfg_(cfg), logger_(logger) {
    // blank = "current session" on first login
    session_.fill(' ');
  }

  // One non-blocking pass - connect when due, read, dispatch, timers, flush
  template <SoupHandler H> void poll(H& h, uint64_t now_ns) {
    now_ns_ = now_ns;
    if (terminal()) return;
    if (!sock_) {
      if (now_ns >= next_attempt_ns_) try_connect();
      return;
    }

    const auto n = sock_->recv(rx_.tail());
    if (n < 0) return drop(h, "peer closed");
    if (n > 0) {
      rx_.commit(static_cast<std::size_t>(n));
      last_rx_ns_ = now_ns;
    }
    const bool ok =
        rx_.drain([&](char type, std::span<const std::byte> p) { return on_packet(h, type, p); });
    if (!ok) return drop(h, "bad framing");
    if (terminal()) return close(h);

    if (now_ns - last_rx_ns_ > cfg_.timeout_ns) return drop(h, "heartbeat timeout");
    if (state_ == SoupState::LoggedIn && now_ns - last_tx_ns_ > cfg_.heartbeat_ns) {
      put(kClientHeartbeat, 0, [](std::byte*) {});
      ++stats_.heartbeats_tx;
    }
    if (!tx_.flush(*sock_)) return drop(h, "send failed");
  }

  // encodes the app message into the send buffer and flushes.
  // returns false if not logged in.
  // only the client can send U unsequenced data.
  template <class Enc> bool send_unsequenced(std::size_t len, Enc&& enc) {
    if (state_ != SoupState::LoggedIn) return false;
    if (!put(kUnsequenced, len, enc)) return false;
    // a broken socket is noticed by next poll's recv
    tx_.flush(*sock_);
    return true;
  }

  SoupState state() const { return state_; }
  bool logged_in() const { return state_ == SoupState::LoggedIn; }
  uint64_t next_seq() const { return next_seq_; }
  const SessionId& session() const { return session_; }
  const SoupStats& stats() const { return stats_; }

private:
  bool terminal() const { return state_ == SoupState::Rejected || state_ == SoupState::Ended; }

  template <class H> bool on_packet(H& h, char type, std::span<const std::byte> p) {
    switch (type) {
      case kSequenced:
        if (state_ != SoupState::LoggedIn) return true;
        h.on_sequenced(p, next_seq_);
        // advance only after message is applied
        // on reconnect, we ask for next_seq_ so each execution applied once
        ++next_seq_;
        ++stats_.sequenced;
        return true;
      case kLoginAccepted: {
        if (p.size() < kLoginAcceptedLen) return true;
        std::memcpy(session_.data(), p.data(), 10);
        const uint64_t seq = read_num(p.data() + 10, 20);
        // server starting past what we asked for = messages never seen
        if (seq != next_seq_) {
          logger_.log(core::LogLevel::Error, "server starts at seq {}, wanted {}", seq, next_seq_);
        }
        next_seq_ = seq;
        state_ = SoupState::LoggedIn;
        backoff_ns_ = cfg_.backoff_min_ns;
        logger_.log(core::LogLevel::Info, "logged in, next seq {}", seq);
        h.on_logged_in();
        return true;
      }
      case kLoginRejected:
        logger_.log(core::LogLevel::Error, "login rejected: {}",
                    p.empty() ? '?' : static_cast<char>(p[0]));
        state_ = SoupState::Rejected;
        return false;
      case kEndOfSession:
        logger_.log(core::LogLevel::Info, "end of session at seq {}", next_seq_);
        state_ = SoupState::Ended;
        return false;
      case kServerHeartbeat: ++stats_.heartbeats_rx; return true;
      default:
        // debug, server U
        return true;
    }
  }

  void try_connect();
  void schedule_retry() {
    next_attempt_ns_ = now_ns_ + backoff_ns_;
    backoff_ns_ = std::min(backoff_ns_ * 2, cfg_.backoff_max_ns);
  }

  template <class Enc> bool put(char type, std::size_t len, Enc&& enc) {
    if (!put_packet(tx_, type, len, enc)) return false;
    last_tx_ns_ = now_ns_;
    return true;
  }

  // unsent bytes are dropped. they were unsequenced and gateway resends
  // anything still pending after the next login
  template <class H> void drop(H& h, core::Lit why) {
    logger_.log(core::LogLevel::Warn, "disconnected: {}, next seq {}", why, next_seq_);
    sock_.reset();
    rx_.reset();
    tx_.reset();
    state_ = SoupState::Disconnected;
    ++stats_.disconnects;
    schedule_retry();
    h.on_disconnected();
  }

  // Rejected / Ended - no reconnect
  template <class H> void close(H& h) {
    sock_.reset();
    h.on_disconnected();
  }

  SoupConfig cfg_;
  core::LogChannel& logger_;
  std::optional<net::TcpClient> sock_;
  RecvBuffer rx_;
  SendBuffer tx_;
  SoupState state_ = SoupState::Disconnected;
  SessionId session_{};
  // next sequenced msg expect = 1 + last one applied
  uint64_t next_seq_ = 1;
  uint64_t now_ns_ = 0, last_rx_ns_ = 0, last_tx_ns_ = 0;
  uint64_t next_attempt_ns_ = 0;
  uint64_t backoff_ns_ = cfg_.backoff_min_ns;
  SoupStats stats_;
};

} // namespace ttt::soup
