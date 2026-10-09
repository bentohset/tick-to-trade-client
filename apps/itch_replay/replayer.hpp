#pragma once

#include "core/endian.hpp"
#include "feed/itch/framing.hpp"
#include "feed/itch/messages.hpp"
#include "feed/moldudp64/wire.hpp"
#include "net/udp_socket.hpp"
#include "options.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <random>
#include <span>
#include <vector>

namespace itch_replay {

// The engine for replaying a mmapp ITCH file.
// It slices the file into packets and sends them over multicast, advancing a seq counter
// It optionally drops packets on purpose to test Recovery, and answers re-request packets on
// a separate unicast socket.
// Paces itself to a target rate and sends heartbeats and End-of-Session so the receiver
// can detect a tail gap and know when the day is over.
class Replayer {
public:
  Replayer(std::span<const std::byte> file, const Options& o)
      : file_(file),
        opts_(o),
        group_(ttt::net::Endpoint::from(o.group, o.port)),
        tx_(ttt::net::UdpSocket::multicast_tx(o.iface, true)),
        rr_(ttt::net::UdpSocket::unicast(o.iface, o.rerequest_port)),
        rng_(o.seed) {}

  void run() {
    uint64_t off = 0;
    // walks the file
    while (off < file_.size()) {
      // get the next batch of ITCH messages at offset=off and capped at kMaxRequestCount
      const Slice s = cut(off, ttt::mold::kMaxRequestCount);
      // file ran out
      if (s.count == 0) break;

      // record the packet starting seqno and file offset
      index_.push_back({next_seq_, off});
      // decide if should be dropped or sent
      if (!should_drop(next_seq_, s.count)) {
        send(next_seq_, off, s, group_, tx_);
      }
      // advance counters to maintain gap detection
      next_seq_ += s.count;
      off = s.end;
      // between every packet sent, give a pending re-request chance to answer
      serve_rerequests();
      pace();
    }

    // end of the file, send 1 heartbeat
    send_control(ttt::mold::kHeartbeat);

    // keep running for linger_s seconds and announce end-of-session every second
    const auto linger_until =
        std::chrono::steady_clock::now() + std::chrono::seconds(opts_.linger_s);
    auto next_eos = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() < linger_until) {
      if (std::chrono::steady_clock::now() >= next_eos) {
        send_control(ttt::mold::kEndOfSession);
        next_eos += std::chrono::seconds(1);
      }
      // continue to answer re-requests
      serve_rerequests();
    }
  }

private:
  struct PacketIdx {
    uint64_t first_seq;
    uint64_t offset;
  };
  struct Slice {
    uint64_t end;
    uint16_t count;
  };

  // slice one packet worth of blocks at most kMaxPayload and max_count blocks
  Slice cut(uint64_t off, uint64_t max_count) const {
    // start framereader at offset=off
    ttt::itch::FrameReader r(file_.subspan(off));
    uint16_t n = 0; // counts how many blocks get included
    std::size_t end_offset = 0;
    while (n < max_count) {
      auto blk = r.next();
      // EOF or truncated tail
      if (blk.empty()) break;
      // --until reached
      if (opts_.until_ns && ttt::itch::Header{blk.data()}.timestamp_ns() >= *opts_.until_ns) break;
      // does not fit in packet
      if (r.offset() > ttt::mold::kMaxPayload) break;

      end_offset = r.offset();
      ++n;
    }
    return {off + end_offset, n};
  }

  // send one packet to the wire
  void send(uint64_t seq, uint64_t off, Slice s, const ttt::net::Endpoint& to,
            ttt::net::UdpSocket& sock) {
    std::array<std::byte, ttt::mold::kHeaderSize> hdr;
    ttt::mold::write_header(hdr.data(), opts_.session, seq, s.count);
    sock.send_to(hdr, file_.subspan(off, s.end - off), to);
  }

  // send for heartbeat (count = 0) and end-of-session (count = 0xFFFF)
  void send_control(uint16_t count) {
    std::array<std::byte, ttt::mold::kHeaderSize> hdr;
    ttt::mold::write_header(hdr.data(), opts_.session, next_seq_, count);
    tx_.send_to(hdr, group_);
  }

  // simulate loss
  bool should_drop(uint64_t seq, uint16_t count) {
    const bool drop =
        std::find(opts_.drop_seqs.begin(), opts_.drop_seqs.end(), seq) != opts_.drop_seqs.end() ||
        std::bernoulli_distribution(opts_.drop_rate)(rng_);
    if (drop) dropped_ += count;
    return drop;
  }

  // answer gap fill request
  void serve_rerequests() {
    std::array<std::byte, ttt::mold::kHeaderSize> hdr;
    ttt::net::Endpoint from;
    // non-blocking poll of rr socket
    const auto n = rr_.recv(hdr, &from);
    if (n == 0) return; // nothing arrived
    const auto h = ttt::mold::read_header(hdr);
    if (!h || h->session != opts_.session || h->seq >= next_seq_) return;

    // find which originally-sent packet covers the requested seqno
    // gets the first indexed packet with first_seq > h->seq
    auto it = std::upper_bound(index_.begin(), index_.end(), h->seq,
                               [](uint64_t seq, const PacketIdx& p) { return seq < p.first_seq; });
    if (it != index_.begin()) --it; // avoid iterating past start of vector
    uint64_t seq = it->first_seq, off = it->offset;
    const uint64_t want_end = h->seq + h->count;
    // we know the seqno from start to end
    while (seq < want_end && seq < next_seq_) {
      // get the Slice of data and send each packet
      const Slice s = cut(off, std::min<uint64_t>(want_end - seq, ttt::mold::kMaxRequestCount));
      if (s.count == 0) break;
      send(seq, off, s, from, rr_);
      ++served_;
      seq += s.count;
      off = s.end;
    }
  }

  // throttle to a target rate
  void pace() {
    if (opts_.rate == 0) return;
    if (next_send_ == std::chrono::steady_clock::time_point{}) {
      next_send_ = std::chrono::steady_clock::now();
    }
    next_send_ += std::chrono::nanoseconds(1'000'000'000 / opts_.rate);
    while (std::chrono::steady_clock::now() < next_send_) {} // spin
  }

  std::span<const std::byte> file_;
  const Options& opts_;
  ttt::net::Endpoint group_;
  ttt::net::UdpSocket tx_, rr_;
  std::mt19937_64 rng_;
  // store sent packet seqno and file offset so we can fill re-requests
  std::vector<PacketIdx> index_;

  uint64_t next_seq_ = 1, dropped_ = 0, served_ = 0;
  std::chrono::steady_clock::time_point next_send_{};
};

} // namespace itch_replay
