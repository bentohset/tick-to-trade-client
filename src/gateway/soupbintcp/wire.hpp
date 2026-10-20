#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
namespace ttt::soup {

// length(2) + type(1)
inline constexpr std::size_t kHeaderSize = 3;
inline constexpr std::size_t kLoginRequestLen = 46;
inline constexpr std::size_t kLoginAcceptedLen = 30;

// server -> client
inline constexpr char kLoginAccepted = 'A';
inline constexpr char kLoginRejected = 'J';
inline constexpr char kSequenced = 'S';
inline constexpr char kServerHeartbeat = 'H';
inline constexpr char kEndOfSession = 'Z';

// client -> server
inline constexpr char kLoginRequest = 'L';
inline constexpr char kUnsequenced = 'U';
inline constexpr char kClientHeartbeat = 'R';
inline constexpr char kLogoutRequest = 'O';

// both
inline constexpr char kDebug = '+';

using SessionId = std::array<char, 10>;
using Username = std::array<char, 6>;
using Password = std::array<char, 10>;

// numeric ASCII, left-padded with spaces.
// eg. write_num(p, 20, 42) -> "     42"
inline void write_num(std::byte* p, std::size_t width, uint64_t v) noexcept {
  std::memset(p, ' ', width);
  std::size_t i = width;
  do {
    p[--i] = static_cast<std::byte>('0' + v % 10);
    v /= 10;
  } while (v && i);
}

inline uint64_t read_num(const std::byte* p, std::size_t width) noexcept {
  uint64_t v = 0;
  for (std::size_t i = 0; i < width; ++i) {
    const char c = static_cast<char>(p[i]);
    // skip pad spaces
    if (c >= '0' && c <= '9') {
      v = v * 10 + static_cast<uint64_t>(c - '0');
    }
  }
  return v;
}

// Login Request payload: user(6) password(10) session(10) seq(20)
inline void encode_login(std::byte* p, const Username& u, const Password& pw, const SessionId& s,
                         uint64_t seq) noexcept {
  std::memcpy(p, u.data(), 6);
  std::memcpy(p + 6, pw.data(), 10);
  std::memcpy(p + 16, s.data(), 10);
  write_num(p + 26, 20, seq);
}

inline void encode_login_acdcepted(std::byte* p, const SessionId& s, uint64_t seq) noexcept {
  std::memcpy(p, s.data(), 10);
  write_num(p + 10, 20, seq);
}

} // namespace ttt::soup
