#pragma once

#include "core/async_logger.hpp"
#include "core/clock.hpp"
#include "feed/book/book_manager.hpp"
#include "feed/book/order_book.hpp"
#include "feed/book/types.hpp"
#include "feed/book_update.hpp"
#include "feed/feed_io.hpp"
#include "feed/itch/messages.hpp"
#include "feed/itch/parser.hpp"
#include "feed/itch/symbol_directory.hpp"
#include "feed/moldudp64/recovery.hpp"
#include "feed/moldudp64/session.hpp"
#include "feed/moldudp64/types.hpp"
#include "feed/moldudp64/wire.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>

namespace ttt::feed {

// External states
enum class FeedHealth : uint8_t { Live, Recovering, Stale, Ended, Failed };

struct FeedConfig {
  mold::SessionId session{};
  uint64_t start_seq = 1;
  std::size_t buffer_capacity = 4096;
  uint64_t recovery_timeout_ns = 20'000'000;
  uint32_t max_retries = 10;
  // considered stale if no packet or heartbeat for this long
  uint64_t stale_after_ns = 2'000'000'000;
};

struct FeedStats {
  uint64_t packets = 0;
  uint64_t messages = 0;
  uint64_t bad_length = 0;
  uint64_t updates = 0;
  uint64_t rerequests = 0;
  uint64_t first_rx_tsc = 0; // 0 until the first packet
  uint64_t last_rx_tsc = 0;
};

// Sink is where updates on subscriptions go (the consumer) 
// and Io is where packets arrive from
template <BookUpdateSink Sink, FeedIo Io = UdpFeedIo>
class FeedHandler {
public:
  FeedHandler(const FeedConfig& cfg, Io& io, Sink& sink, core::LogChannel& logger)
    : cfg_(cfg), io_(io), sink_(sink), logger_(logger),
      session_(cfg.start_seq, cfg.buffer_capacity),
      recovery_(cfg.recovery_timeout_ns, cfg.max_retries),
      sub_slot_(65536, kNoSub) {}

  // Denote which symbols should produce BookUpdates to the consumer
  void subscribe(std::string_view symbol) {
    subs_.push_back({itch::Symbol::from(symbol).key(), {}});
  }

  // Single non-blocking pass
  // false if the feed has ended or failed
  bool poll() {
    // whether this pass has received anything
    bool got = false;
    // try the multicast socket without waiting
    if (const auto n = io_.recv_feed(buf_); n > 0) {
      // if packet arrives, timestamp it and process
      on_packet({buf_.data(), n}, core::read_ticks());
      got = true;
    }
    // check re-request socket if gapped; when feed is live, nothing expected
    if (session_.state() == mold::FeedState::Recovering) {
      if (const auto n = io_.recv_rerequest(buf_); n > 0) {
        on_packet({buf_.data(), n}, core::read_ticks());
        got = true;
      }
    }
    // timers run when loop is idle or recovering
    // while recovering, timers run on every pass so retry timeouts can be checked
    if (!got || session_.state() == mold::FeedState::Recovering) {
      on_timer(core::read_ticks());
    }
    return running();
  }

  // public for testing
  // handles one packet
  void on_packet(std::span<const std::byte> pkt, uint64_t rx_tsc) {
    // counts the packet and record arrival time
    ++stats_.packets;
    if (!stats_.first_rx_tsc) stats_.first_rx_tsc = rx_tsc;
    stats_.last_rx_tsc = rx_tsc;
    // store arrival time in member because callback needs it to stamp BookUpdates
    // TODO: flaw in latency measurement
    // packets released from PacketBuffer after a gap get this rx_tsc, not their own
    // so latency for a given packet may consist of re-requested packets
    // fix: each packet needs to keep its own arrival time
    rx_tsc_ = rx_tsc;

    // take snapshot of health before processing
    const auto before = health();
    stale_ = false;

    // hands packet to Session
    session_.on_packet(pkt, [this](uint64_t seq, std::span<const std::byte> msg) {
      seq_ = seq; // store seq for BookUpdate
      ++stats_.messages;
      // parse the ITCH message and forwards to on_message
      Dispatch d{*this};
      if (itch::parse(msg, d) == itch::ParseResult::BadLength) ++stats_.bad_length;
    });
    log_transition(before, health());
  }

  // during idle or recovering
  void on_timer(uint64_t now_tsc) {
    const auto before = health();
    if (session_.state() == mold::FeedState::Recovering) {
      // check if re-request is possible for recovery
      if (const auto req = recovery_.poll(session_, core::ticks_to_ns(now_tsc))) {
        send_rerequest(*req);
      }
    }
    // considered stale only once the feed has started; not considered before the first packet as we just wait
    if (stats_.last_rx_tsc && !stale_ && core::ticks_to_ns(now_tsc - stats_.last_rx_tsc) > cfg_.stale_after_ns) {
      stale_ = true;
    }
    log_transition(before, health());
  }

  FeedHealth health() const {
    if (recovery_.timed_out()) return FeedHealth::Failed;
    switch (session_.state()) {
    case mold::FeedState::Failed: return FeedHealth::Failed;
    case mold::FeedState::Ended: return FeedHealth::Ended;
    case mold::FeedState::Recovering: return FeedHealth::Recovering;
    case mold::FeedState::Live: return stale_ ? FeedHealth::Stale : FeedHealth::Live;
    }
    return FeedHealth::Failed;
  }

  bool running() const {
    const auto h = health();
    return h != FeedHealth::Ended && h != FeedHealth::Failed;
  }

  const mold::Session& session() const { return session_; }
  const itch::SymbolDirectory& directory() const { return dir_; }
  const book::BookManager& books() const { return books_; }
  const FeedStats& stats() const { return stats_; }

private:
  // "not subscribed" marker in sub_slot_
  static constexpr uint16_t kNoSub = UINT16_MAX;

  // top-of-book snapshot used to detect changes
  struct Top {
    book::Price bid_px{}, ask_px{};
    uint64_t bid_qty{0}, ask_qty{0};
    bool operator==(const Top&) const = default;
  };
  struct Sub {
    uint64_t key;
    Top last;
  };

  struct Dispatch {
    FeedHandler& self;
    template <class Msg> void on(const Msg& msg) { self.on_message(msg); }
  };

  // applies a single ITCH message then check for top-of-book changes for subscriptions
  template <class Msg> void on_message(const Msg& m) {
    dir_.on(m);
    books_.on(m);
    if constexpr (std::is_same_v<Msg, itch::StockDirectory>) resolve_subscription(m);
    // for message types that carry stock locate, check if the symbol is subscribed
    if constexpr (requires { m.stock_locate(); }) {
      const uint16_t locate = m.stock_locate();
      if (const uint16_t slot = sub_slot_[locate]; slot != kNoSub) {
        maybe_emit(locate, subs_[slot]);
      }
    }
  }

  // links a subscribed symbol to its locate when StockDirectory message arrives
  void resolve_subscription(const itch::StockDirectory& m) {
    const uint64_t key = m.stock().key();
    // loop is ok because there can only be a handful of subs
    for (uint16_t i = 0; i < subs_.size(); ++i) {
      if (subs_[i].key != key) continue;
      // fill in sub_slot_ with locate so on_message can emit for that locate
      sub_slot_[m.stock_locate()] = i;
      logger_.log(core::LogLevel::Info, "subscribed {} -> locate {}",
                  core::FixedStr<8>{m.stock().view()}, m.stock_locate());
    }
  }

  // compares the top of book with the one last one sent, emit if changed
  void maybe_emit(uint16_t locate, Sub& sub) {
    const book::OrderBook* b = books_.book(locate);
    if (!b) return;
    Top now;
    if (const auto l = b->best_bid()) {
      now.bid_px = l->price, now.bid_qty = l->qty;
    }
    if (const auto l = b->best_ask()) {
      now.ask_px = l->price, now.ask_qty = l->qty;
    }
    // skip if add or cancel since it does not change the top of the book
    if (now == sub.last) return;
    // build the BookUpdate and send to consumer
    sub.last = now;
    ++stats_.updates;
    sink_.on_update({rx_tsc_, seq_, now.bid_qty, now.ask_qty, now.bid_px, now.ask_px, locate});
  }

  // encode and send a re-request
  void send_rerequest(const mold::Gap& req) {
    std::array<std::byte, mold::kHeaderSize> hdr;
    mold::write_header(hdr.data(), cfg_.session, req.first, static_cast<uint16_t>(req.end - req.first));
    io_.send_rerequest(hdr);
    ++stats_.rerequests;
    logger_.log(core::LogLevel::Warn, "rerequest [{}, {})", req.first, req.end);
  }

  void log_transition(FeedHealth from, FeedHealth to) {
    using H = FeedHealth;
    using L = core::LogLevel;
    if (from == to) return;
    const uint64_t seq = session_.expected();
    switch (to) {
    case H::Recovering: logger_.log(L::Warn, "gap: waiting for seq {}", seq); break;
    case H::Stale: logger_.log(L::Warn, "stale: no packets for {} ms", cfg_.stale_after_ns / 1'000'000); break;
    case H::Live: logger_.log(L::Info, "live at seq {}", seq); break;
    case H::Ended: logger_.log(L::Info, "end of session at seq {}", seq); break;
    case H::Failed:
      if (recovery_.timed_out()) logger_.log(L::Error, "recovery timed out at seq {}", seq);
      else logger_.log(L::Error, "packet buffer full at seq {}", seq);
      break;
    }
  }

  FeedConfig cfg_;
  Io& io_;
  Sink& sink_;
  core::LogChannel& logger_;

  mold::Session session_;
  mold::Recovery recovery_;
  itch::SymbolDirectory dir_;
  book::BookManager books_;

  // subscribed symbols and their last top of book
  // only send a bookupdate only when subscribed symbol top of book changes
  std::vector<Sub> subs_;
  // locate -> index into subs_
  std::vector<uint16_t> sub_slot_;
  uint64_t rx_tsc_{0};
  uint64_t seq_{0};
  bool stale_{false};
  FeedStats stats_;
  // receive buffer, reused for every packet
  std::array<std::byte, mold::kMaxDatagram> buf_;
};

}
