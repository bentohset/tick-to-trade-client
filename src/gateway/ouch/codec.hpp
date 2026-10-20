#pragma once

#include "feed/itch/messages.hpp"
#include "gateway/common/order_types.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>

namespace ttt::ouch {

using Token = std::array<char, 14>;
using Firm = std::array<char, 4>;
using TokenPrefix = std::array<char, 2>;

// Token = 2-char prefix + 9-digit OrderId + 3-digit replace generation.
// OrderId comes back out of the token with no lookup table; the generation
// signals a reject of the replacement token apart from a reject of the original
inline Token make_token(TokenPrefix prefix, uint32_t id, uint16_t gen) {
  Token t;
  t[0] = prefix[0];
  t[1] = prefix[1];
  for (int i = 10; i >= 2; --i, id /= 10) t[i] = static_cast<char>('0' + id % 10);
  for (int i = 13; i >= 11; --i, gen /= 10) t[i] = static_cast<char>('0' + gen % 10);
  return t;
}

struct TokenParts {
  uint32_t id;
  uint16_t gen;
};

inline std::optional<TokenParts> parse_token(const Token& t, TokenPrefix prefix) {
  if (t[0] != prefix[0] || t[1] != prefix[1]) return std::nullopt; // not ours
  TokenParts r{0, 0};
  for (int i = 2; i < 14; ++i) {
    if (t[i] < '0' || t[i] > '9') return std::nullopt;
    if (i <= 10)
      r.id = r.id * 10 + static_cast<uint32_t>(t[i] - '0');
    else
      r.gen = static_cast<uint16_t>(r.gen * 10 + (t[i] - '0'));
  }
  return r;
}

inline Token load_token(const std::byte* p) {
  Token t;
  std::memcpy(t.data(), p, 14);
  return t;
}

inline itch::Symbol load_stock(const std::byte* p) {
  itch::Symbol s;
  std::memcpy(s.raw.data(), p, 8);
  return s;
}

// Each message is a view: a pointer plus accessors that read fields at fixed offsets

// ---- client -> exchange (decoded by mock_exchange) ----

struct EnterOrder {
  static constexpr char kType = 'O';
  static constexpr std::size_t kSize = 49;
  const std::byte* p;
  Token token() const { return load_token(p + 1); }
  char side() const { return static_cast<char>(p[15]); }
  uint32_t shares() const { return be::load<uint32_t>(p + 16); }
  itch::Symbol stock() const { return load_stock(p + 20); }
  uint32_t price() const { return be::load<uint32_t>(p + 28); }
  uint32_t tif() const { return be::load<uint32_t>(p + 32); }
};

struct ReplaceOrder {
  static constexpr char kType = 'U';
  static constexpr std::size_t kSize = 47;
  const std::byte* p;
  Token existing() const { return load_token(p + 1); }
  Token replacement() const { return load_token(p + 15); }
  uint32_t shares() const { return be::load<uint32_t>(p + 29); }
  uint32_t price() const { return be::load<uint32_t>(p + 33); }
  uint32_t tif() const { return be::load<uint32_t>(p + 37); }
};

struct CancelOrder {
  static constexpr char kType = 'X';
  static constexpr std::size_t kSize = 19;
  const std::byte* p;
  Token token() const { return load_token(p + 1); }
  uint32_t shares() const { return be::load<uint32_t>(p + 15); } // new remaining, 0 = all
};

// ---- exchange -> client ----

// every outbound message: type(1) timestamp(8), then a token for all but SystemEvent
struct OutHeader {
  const std::byte* p;
  uint64_t timestamp_ns() const { return be::load<uint64_t>(p + 1); }
  Token token() const { return load_token(p + 9); }
};

// 'S'
struct SystemEvent : OutHeader {
  static constexpr char kType = 'S';
  static constexpr std::size_t kSize = 10;
  char event() const { return static_cast<char>(p[9]); }
};

// 'A'
struct Accepted : OutHeader {
  static constexpr char kType = 'A';
  static constexpr std::size_t kSize = 66;
  char side() const { return static_cast<char>(p[23]); }
  uint32_t shares() const { return be::load<uint32_t>(p + 24); }
  itch::Symbol stock() const { return load_stock(p + 28); }
  uint32_t price() const { return be::load<uint32_t>(p + 36); }
  uint64_t order_ref() const { return be::load<uint64_t>(p + 49); }
  char order_state() const { return static_cast<char>(p[64]); } // 'L' live / 'D' dead
};

// 'U', token() is the replacement token
struct Replaced : OutHeader {
  static constexpr char kType = 'U';
  static constexpr std::size_t kSize = 80;
  uint32_t shares() const { return be::load<uint32_t>(p + 24); }
  uint32_t price() const { return be::load<uint32_t>(p + 36); }
  uint64_t order_ref() const { return be::load<uint64_t>(p + 49); }
  char order_state() const { return static_cast<char>(p[64]); }
  Token previous_token() const { return load_token(p + 65); }
};

// 'C'
struct Canceled : OutHeader {
  static constexpr char kType = 'C';
  static constexpr std::size_t kSize = 28;
  uint32_t decrement() const { return be::load<uint32_t>(p + 23); }
  char reason() const { return static_cast<char>(p[27]); }
};

// 'E'
struct Executed : OutHeader {
  static constexpr char kType = 'E';
  static constexpr std::size_t kSize = 40;
  uint32_t shares() const { return be::load<uint32_t>(p + 23); }
  uint32_t price() const { return be::load<uint32_t>(p + 27); }
  char liquidity() const { return static_cast<char>(p[31]); }
  uint64_t match_number() const { return be::load<uint64_t>(p + 32); }
};

// 'B'
struct BrokenTrade : OutHeader {
  static constexpr char kType = 'B';
  static constexpr std::size_t kSize = 32;
  uint64_t match_number() const { return be::load<uint64_t>(p + 23); }
  char reason() const { return static_cast<char>(p[31]); }
};

// 'J'
struct Rejected : OutHeader {
  static constexpr char kType = 'J';
  static constexpr std::size_t kSize = 24;
  char reason() const { return static_cast<char>(p[23]); }
};

// 'I'
struct CancelReject : OutHeader {
  static constexpr char kType = 'I';
  static constexpr std::size_t kSize = 23;
};

enum class ParseResult : uint8_t { Ok, Unknown, BadLength };

template <class Msg, class H> ParseResult deliver(std::span<const std::byte> m, H& h) {
  if (m.size() < Msg::kSize) return ParseResult::BadLength;
  // handlers implement only the messages they care about
  // on() must be public so the check can pass
  if constexpr (requires { h.on(std::declval<const Msg&>()); }) h.on(Msg{m.data()});
  return ParseResult::Ok;
}

// exchange -> client
template <class H> ParseResult parse_outbound(std::span<const std::byte> m, H& h) {
  if (m.empty()) return ParseResult::BadLength;
  switch (static_cast<char>(m[0])) {
    case Accepted::kType: return deliver<Accepted>(m, h);
    case Executed::kType: return deliver<Executed>(m, h);
    case Canceled::kType: return deliver<Canceled>(m, h);
    case Replaced::kType: return deliver<Replaced>(m, h);
    case Rejected::kType: return deliver<Rejected>(m, h);
    case CancelReject::kType: return deliver<CancelReject>(m, h);
    case BrokenTrade::kType: return deliver<BrokenTrade>(m, h);
    case SystemEvent::kType: return deliver<SystemEvent>(m, h);
    default: return ParseResult::Unknown; // P, D, T, M not handled yet
  }
}

// client -> exchange
template <class H> ParseResult parse_inbound(std::span<const std::byte> m, H& h) {
  if (m.empty()) return ParseResult::BadLength;
  switch (static_cast<char>(m[0])) {
    case EnterOrder::kType: return deliver<EnterOrder>(m, h);
    case ReplaceOrder::kType: return deliver<ReplaceOrder>(m, h);
    case CancelOrder::kType: return deliver<CancelOrder>(m, h);
    default: return ParseResult::Unknown;
  }
}

// ---- client-side encoders ----

inline char side_code(gw::Side s) {
  return s == gw::Side::Buy ? 'B' : s == gw::Side::Sell ? 'S' : 'T';
}
inline uint32_t tif_secs(gw::Tif t) { return t == gw::Tif::Ioc ? 0 : 99999; }

inline void encode_enter(std::byte* p, const Token& t, const gw::NewOrder& n, const Firm& firm) {
  p[0] = std::byte{'O'};
  std::memcpy(p + 1, t.data(), 14);
  p[15] = static_cast<std::byte>(side_code(n.side));
  be::store(p + 16, n.qty);
  std::memcpy(p + 20, n.symbol.raw.data(), 8);
  be::store(p + 28, n.price);
  be::store(p + 32, tif_secs(n.tif));
  std::memcpy(p + 36, firm.data(), 4);
  p[40] = std::byte{'Y'};         // displayed
  p[41] = std::byte{'P'};         // principal
  p[42] = std::byte{'N'};         // not ISO
  be::store(p + 43, uint32_t{0}); // no min qty
  p[47] = std::byte{'N'};         // continuous market
  p[48] = std::byte{' '};         // customer type
}

inline void encode_cancel(std::byte* p, const Token& t, uint32_t remaining) {
  p[0] = std::byte{'X'};
  std::memcpy(p + 1, t.data(), 14);
  be::store(p + 15, remaining);
}

inline void encode_replace(std::byte* p, const Token& old_tok, const Token& new_tok, uint32_t qty,
                           uint32_t px, uint32_t tif) {
  p[0] = std::byte{'U'};
  std::memcpy(p + 1, old_tok.data(), 14);
  std::memcpy(p + 15, new_tok.data(), 14);
  be::store(p + 29, qty);
  be::store(p + 33, px);
  be::store(p + 37, tif);
  p[41] = std::byte{'Y'};
  p[42] = std::byte{'N'};
  be::store(p + 43, uint32_t{0});
}

} // namespace ttt::ouch
