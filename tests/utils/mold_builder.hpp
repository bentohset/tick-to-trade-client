#pragma once

// Builds MoldUDP64 packets for tests. Blocks carry 1 dummy byte each — Session only
// cares about framing (lengths, counts), never message content.

#include "feed/moldudp64/wire.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace ttt::test {

inline mold::SessionId mold_session(const char* id = "TEST000001") {
  mold::SessionId s;
  for (std::size_t i = 0; i < s.size(); ++i) s[i] = id[i];
  return s;
}

// Builds a packet's bytes directly: header count is whatever is passed in, independent
// of how many blocks actually get appended — lets a test build a packet that lies
// about its block count (TruncatedPacketBecomesGap).
class PacketBuilder {
public:
  PacketBuilder(mold::SessionId session, uint64_t seq, uint16_t declared_count) {
    bytes_.resize(mold::kHeaderSize);
    mold::write_header(bytes_.data(), session, seq, declared_count);
  }

  PacketBuilder& block(std::byte tag = std::byte{0xAB}) {
    bytes_.push_back(std::byte{0});
    bytes_.push_back(std::byte{1}); // length = 1, big-endian
    bytes_.push_back(tag);
    return *this;
  }

  std::vector<std::byte> bytes() const { return bytes_; }

private:
  std::vector<std::byte> bytes_;
};

// A well-formed data packet: `count` one-byte blocks, header count matches exactly.
inline std::vector<std::byte> packet(mold::SessionId session, uint64_t seq, uint16_t count) {
  PacketBuilder b(session, seq, count);
  for (uint16_t i = 0; i < count; ++i) b.block(static_cast<std::byte>((seq + i) & 0xFF));
  return b.bytes();
}

inline std::vector<std::byte> heartbeat(mold::SessionId session, uint64_t next_seq) {
  std::vector<std::byte> bytes(mold::kHeaderSize);
  mold::write_header(bytes.data(), session, next_seq, mold::kHeartbeat);
  return bytes;
}

inline std::vector<std::byte> end_of_session(mold::SessionId session, uint64_t end_seq) {
  std::vector<std::byte> bytes(mold::kHeaderSize);
  mold::write_header(bytes.data(), session, end_seq, mold::kEndOfSession);
  return bytes;
}

// Recorder sink: void(uint64_t seq, std::span<const std::byte>) matching Session's Sink.
struct Recorder {
  std::vector<uint64_t> seqs;
  void operator()(uint64_t seq, std::span<const std::byte>) { seqs.push_back(seq); }
};

} // namespace ttt::test
