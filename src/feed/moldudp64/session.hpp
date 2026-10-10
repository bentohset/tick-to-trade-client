#pragma once

#include "feed/itch/framing.hpp"
#include "feed/moldudp64/packet_buffer.hpp"
#include "feed/moldudp64/types.hpp"
#include "feed/moldudp64/wire.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>

namespace ttt::mold {

// The receiver-side state machine for moldudp64 protocol
// Given a stream of moldudp64 packets arriving, manage the session state,
// covering duplicates, gaps and handing it to the caller strictly in sequence
class Session {
public:
  Session(uint64_t start_seq, std::size_t buffer_capacity)
      : expected_(start_seq), buf_(buffer_capacity) {}

  // Dispatcher and handles state of packets
  // Sink template: void(uint64_t seq, std::span<const std::byte>msg)
  template <class Sink> void on_packet(std::span<const std::byte> pkt, Sink&& sink) {
    // decode header
    const auto h = read_header(pkt);
    if (!h) return; // header parse fail

    if (!have_session_) {
      // this is the first packet of the session
      session_ = h->session;
      have_session_ = true;
    } else if (h->session != session_) {
      // session id mismatch
      ++stats_.wrong_session;
      return;
    }

    // denotes the end of the session
    if (h->count == kEndOfSession) {
      end_seq_ = h->seq; // total count of messages sent
      update_state();
      return;
    }

    // heartbeat packet sent during idle, with the next expected seq
    if (h->count == kHeartbeat) {
      ++stats_.heartbeats;
      known_end_ = std::max(known_end_, h->seq);
      update_state();
      return;
    }

    // ordinary data packet
    const uint64_t last = h->seq + h->count;
    known_end_ = std::max(known_end_, last);

    if (last <= expected_) {
      // everything in this packet is here or before what has been delivered
      ++stats_.duplicates;
      return;
    }
    // gap detected, hand it to packetbuffer to hold
    if (h->seq > expected_) {
      if (!buf_.put(h->seq, pkt)) {
        // buffer has no more room
        ++stats_.buffer_full;
        state_ = FeedState::Failed;
        return;
      }
      update_state();
      return;
    }

    // this packet overlaps with expected (either starts at expected_ or slightly before)
    apply(h->seq, h->count, pkt.subspan(kHeaderSize), sink);
    // after advancing expected_, check if any buffered gap can be unblocked
    buf_.pop_ready(expected_, [&](uint64_t, std::span<const std::byte> buffered) {
      const auto bh = read_header(buffered);
      // mutates expected_
      apply(bh->seq, bh->count, buffered.subspan(kHeaderSize), sink);
    });
    update_state();
  }

  FeedState state() const noexcept { return state_; }
  uint64_t expected() const noexcept { return expected_; }
  const Stats& stats() const noexcept { return stats_; }
  std::size_t peak_buffered() const noexcept { return buf_.peak_used(); }
  std::size_t buffer_capacity() const noexcept { return buf_.capacity(); }

  // For recovery. Always requests up to known_end_, even if part of that range is
  // already buffered -- the ring no longer tracks the lowest buffered seq cheaply,
  // so this occasionally asks for a bit more than strictly necessary. Session::put's
  // dedup (and the receiving Session's own duplicate/overlap handling) absorbs that
  // for free; it's not worth an O(capacity) scan to avoid a slightly wider request.
  std::optional<Gap> gap() const {
    if (state_ != FeedState::Recovering) return std::nullopt;
    return Gap{expected_, known_end_};
  }

private:
  // deliver one packet message to caller
  template <class Sink>
  void apply(uint64_t seq, uint16_t count, std::span<const std::byte> blocks, Sink&& sink) {
    // determine how many leading blocks to discard
    // normally skip == 0 if seq == expected_
    const uint64_t skip = expected_ - seq;
    if (skip) ++stats_.overlaps;

    // moldudp64 packet payload and ITCH file are same framing, reuse framereader
    // [2-byte big-endian len][message bytes] block frames
    itch::FrameReader r(blocks);
    uint16_t i = 0;
    // walk the packet blocks, stopping when
    // header's declared block count is met or no more blocks
    for (auto b = r.next(); !b.empty() && i < count; b = r.next(), ++i) {
      // advance past skip without delivering
      if (i < skip) continue;
      sink(expected_, b);
      ++expected_;
      ++stats_.messages;
    }
  }

  void update_state() {
    if (state_ == FeedState::Failed) return;
    // end-of-session
    if (end_seq_ && expected_ >= *end_seq_) {
      state_ = FeedState::Ended;
      return;
    }
    // check if recovering
    const bool was_live = state_ == FeedState::Live;
    state_ = (expected_ < known_end_) ? FeedState::Recovering : FeedState::Live;
    if (was_live && state_ == FeedState::Recovering) ++stats_.gaps;
  }

  bool have_session_ = false;
  SessionId session_{};
  // the next seqno we expect
  uint64_t expected_;
  // thhighest seq we have seen
  uint64_t known_end_ = 0;
  // set during End-of-Session packet received
  std::optional<uint64_t> end_seq_;
  FeedState state_ = FeedState::Live;
  Stats stats_;
  // buffer for packets that arrive ahead of expected_
  PacketBuffer buf_;
};

} // namespace ttt::mold
