#pragma once

#include "core/endian.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>

namespace ttt::mold {

inline constexpr std::size_t kHeaderSize = 20;
inline constexpr std::size_t kMaxDatagram = 2048;
inline constexpr std::size_t kMaxPayload = 1400;
inline constexpr uint16_t kHeartbeat = 0;
inline constexpr uint16_t kEndOfSession = 0xFFFF;
inline constexpr uint16_t kMaxRequestCount = 0xFFFF;

using SessionId = std::array<char, 10>;

struct Header {
  SessionId session;
  uint64_t seq;
  uint16_t count;
};

inline std::optional<Header> read_header(std::span<const std::byte> pkt) noexcept {
  if (pkt.size() < kHeaderSize) return std::nullopt;
  Header h;
  std::memcpy(h.session.data(), pkt.data(), 10);
  h.seq = be::load<uint64_t>(pkt.data() + 10);
  h.count = be::load<uint16_t>(pkt.data() + 18);
  return h;
}

inline void write_header(std::byte* p, SessionId session, uint64_t seq, uint16_t count) noexcept {
  std::memcpy(p, session.data(), 10);
  be::store(p + 10, seq);
  be::store(p + 18, count);
}

} // namespace ttt::mold
