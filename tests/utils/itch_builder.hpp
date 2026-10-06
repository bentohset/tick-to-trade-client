#pragma once

// Builds ITCH 5.0 messages byte by byte for tests.
// Fields are written big-endian at the spec offsets, so a test reads like the
// message it sends: add(locate, ref, 'B', 100, px) rather than a hex dump.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace ttt::test {

class Msg {
public:
  Msg(char type, uint16_t locate) {
    ch(type).u16(locate).u16(0 /*tracking*/).u48(34'200'000'000'000 /*09:30:00*/);
  }
  Msg& ch(char c) { return put(static_cast<uint64_t>(static_cast<unsigned char>(c)), 1); }
  Msg& u16(uint16_t v) { return put(v, 2); }
  Msg& u32(uint32_t v) { return put(v, 4); }
  Msg& u48(uint64_t v) { return put(v, 6); }
  Msg& u64(uint64_t v) { return put(v, 8); }
  Msg& alpha(std::string_view s, std::size_t width) { // space-padded ASCII
    for (std::size_t i = 0; i < width; ++i) ch(i < s.size() ? s[i] : ' ');
    return *this;
  }
  std::span<const std::byte> bytes() const { return bytes_; }

private:
  Msg& put(uint64_t v, int n) {
    for (int i = n - 1; i >= 0; --i) bytes_.push_back(static_cast<std::byte>(v >> (8 * i)));
    return *this;
  }
  std::vector<std::byte> bytes_;
};

// --- one builder per message type -----------------------------------------------------

inline Msg system_event(char code) { return std::move(Msg('S', 0).ch(code)); }

inline Msg stock_directory(uint16_t locate, std::string_view symbol) {
  return std::move(Msg('R', locate)
                       .alpha(symbol, 8)
                       .ch('Q')  // market category
                       .ch('N')  // financial status
                       .u32(100) // round lot size
                       .ch('N')  // round lots only
                       .ch('C')  // issue classification
                       .alpha("Z", 2)
                       .ch('P')   // authenticity
                       .ch('N')   // short sale threshold
                       .ch(' ')   // IPO flag
                       .ch('1')   // LULD tier
                       .ch('N')   // ETP flag
                       .u32(0)    // ETP leverage
                       .ch('N')); // inverse
}

inline Msg trading_action(uint16_t locate, char state, std::string_view symbol = "AAPL") {
  return std::move(Msg('H', locate).alpha(symbol, 8).ch(state).ch(' ').alpha("", 4));
}

inline Msg add(uint16_t locate, uint64_t ref, char side, uint32_t shares, uint32_t price,
               std::string_view symbol = "AAPL") {
  return std::move(Msg('A', locate).u64(ref).ch(side).u32(shares).alpha(symbol, 8).u32(price));
}

inline Msg add_mpid(uint16_t locate, uint64_t ref, char side, uint32_t shares, uint32_t price,
                    std::string_view symbol = "AAPL", std::string_view mpid = "GSCO") {
  return std::move(
      Msg('F', locate).u64(ref).ch(side).u32(shares).alpha(symbol, 8).u32(price).alpha(mpid, 4));
}

inline Msg executed(uint16_t locate, uint64_t ref, uint32_t shares, uint64_t match = 1) {
  return std::move(Msg('E', locate).u64(ref).u32(shares).u64(match));
}

inline Msg executed_with_price(uint16_t locate, uint64_t ref, uint32_t shares, uint32_t exec_price,
                               uint64_t match = 1) {
  return std::move(Msg('C', locate).u64(ref).u32(shares).u64(match).ch('Y').u32(exec_price));
}

inline Msg cancel(uint16_t locate, uint64_t ref, uint32_t shares) {
  return std::move(Msg('X', locate).u64(ref).u32(shares));
}

inline Msg del(uint16_t locate, uint64_t ref) { return std::move(Msg('D', locate).u64(ref)); }

inline Msg replace(uint16_t locate, uint64_t orig, uint64_t new_ref, uint32_t shares,
                   uint32_t price) {
  return std::move(Msg('U', locate).u64(orig).u64(new_ref).u32(shares).u32(price));
}

} // namespace ttt::test
