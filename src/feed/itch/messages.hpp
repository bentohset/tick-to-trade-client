#pragma once

#include "core/endian.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace ttt::itch {

using Price4 = uint32_t;

// Each message type is a view - a ptr and accessors that read fields at offsets
// Nothing is decoded until an accessor is called
// After inclining, the compiler drops loads for fields a handler never reads, giving cheap
// zero-copy

struct Symbol { // 8 ASCII bytes
  std::array<char, 8> raw;
  std::string_view view() const {
    std::string_view s(raw.data(), raw.size());
    return s.substr(0, s.find_last_not_of(' ') + 1);
  }

  uint64_t key() const {
    uint64_t k;
    std::memcpy(&k, raw.data(), 8);
    return k;
  }

  static Symbol from(std::string_view name) {
    Symbol s;
    s.raw.fill(' ');
    std::memcpy(s.raw.data(), name.data(), std::min(name.size(), s.raw.size()));
    return s;
  }
};

// Common header , offsets 0..10
struct Header {
  const std::byte* p;
  char type() const { return static_cast<char>(p[0]); }
  uint16_t stock_locate() const { return be::load<uint16_t>(p + 1); }
  uint16_t tracking_number() const { return be::load<uint16_t>(p + 3); }
  uint64_t timestamp_ns() const { return be::load48(p + 5); }
};

// 'A'
struct AddOrder : Header {
  static constexpr char kType = 'A';
  static constexpr std::size_t kSize = 36;
  uint64_t order_ref() const { return be::load<uint64_t>(p + 11); }
  char side() const { return static_cast<char>(p[19]); } // 'B' / 'S'
  uint32_t shares() const { return be::load<uint32_t>(p + 20); }
  Symbol stock() const {
    Symbol s;
    std::memcpy(s.raw.data(), p + 24, 8);
    return s;
  }
  Price4 price() const { return be::load<uint32_t>(p + 32); }
};

// 'F'
struct AddOrderMpid : AddOrder {
  static constexpr char kType = 'F';
  static constexpr std::size_t kSize = 40;
  std::array<char, 4> attribution() const { // MPID of the market participant
    std::array<char, 4> a;
    std::memcpy(a.data(), p + 36, 4);
    return a;
  }
};

// 'E'
struct OrderExecuted : Header {
  static constexpr char kType = 'E';
  static constexpr std::size_t kSize = 31;
  uint64_t order_ref() const { return be::load<uint64_t>(p + 11); }
  uint32_t executed_shares() const { return be::load<uint32_t>(p + 19); }
  uint64_t match_number() const { return be::load<uint64_t>(p + 23); }
};

// 'C': executed at a price different from the order's resting price.
// The book still reduces the order at its own (resting) price.
struct OrderExecutedWithPrice : Header {
  static constexpr char kType = 'C';
  static constexpr std::size_t kSize = 36;
  uint64_t order_ref() const { return be::load<uint64_t>(p + 11); }
  uint32_t executed_shares() const { return be::load<uint32_t>(p + 19); }
  uint64_t match_number() const { return be::load<uint64_t>(p + 23); }
  bool printable() const { return static_cast<char>(p[31]) == 'Y'; }
  Price4 execution_price() const { return be::load<uint32_t>(p + 32); }
};

// 'X'
struct OrderCancel : Header {
  static constexpr char kType = 'X';
  static constexpr std::size_t kSize = 23;
  uint64_t order_ref() const { return be::load<uint64_t>(p + 11); }
  uint32_t cancelled_shares() const { return be::load<uint32_t>(p + 19); }
};

// 'D'
struct OrderDelete : Header {
  static constexpr char kType = 'D';
  static constexpr std::size_t kSize = 19;
  uint64_t order_ref() const { return be::load<uint64_t>(p + 11); }
};

// 'U'
struct OrderReplace : Header {
  static constexpr char kType = 'U';
  static constexpr std::size_t kSize = 35;
  uint64_t original_order_ref() const { return be::load<uint64_t>(p + 11); }
  uint64_t new_order_ref() const { return be::load<uint64_t>(p + 19); }
  uint32_t shares() const { return be::load<uint32_t>(p + 27); }
  Price4 price() const { return be::load<uint32_t>(p + 31); }
};

// 'P': execution against a non-displayed order. Never changes the visible book.
struct Trade : Header {
  static constexpr char kType = 'P';
  static constexpr std::size_t kSize = 44;
  uint64_t order_ref() const { return be::load<uint64_t>(p + 11); } // Nasdaq sends 0
  char side() const { return static_cast<char>(p[19]); }            // Nasdaq always sends 'B'
  uint32_t shares() const { return be::load<uint32_t>(p + 20); }
  Symbol stock() const {
    Symbol s;
    std::memcpy(s.raw.data(), p + 24, 8);
    return s;
  }
  Price4 price() const { return be::load<uint32_t>(p + 32); }
  uint64_t match_number() const { return be::load<uint64_t>(p + 36); }
};

// 'S'
struct SystemEvent : Header {
  static constexpr char kType = 'S';
  static constexpr std::size_t kSize = 12;
  // 'O' start of messages, 'S' start of system hours, 'Q' start of market hours,
  // 'M' end of market hours, 'E' end of system hours, 'C' end of messages
  char event_code() const { return static_cast<char>(p[11]); }
};

// 'R'
struct StockDirectory : Header {
  static constexpr char kType = 'R';
  static constexpr std::size_t kSize = 39;
  Symbol stock() const {
    Symbol s;
    std::memcpy(s.raw.data(), p + 11, 8);
    return s;
  }
  char market_category() const {
    return static_cast<char>(p[19]);
  } // Q/G/S = Nasdaq tiers, N = NYSE, ...
  char financial_status() const { return static_cast<char>(p[20]); }
  uint32_t round_lot_size() const { return be::load<uint32_t>(p + 21); }
  bool round_lots_only() const { return static_cast<char>(p[25]) == 'Y'; }
  char issue_classification() const { return static_cast<char>(p[26]); }
  char authenticity() const { return static_cast<char>(p[29]); } // 'P' live, 'T' test symbol
};

// 'H'
struct StockTradingAction : Header {
  static constexpr char kType = 'H';
  static constexpr std::size_t kSize = 25;
  Symbol stock() const {
    Symbol s;
    std::memcpy(s.raw.data(), p + 11, 8);
    return s;
  }
  // 'T' trading, 'H' halted, 'P' paused, 'Q' quotation only
  char trading_state() const { return static_cast<char>(p[19]); }
  // offset 20 is reserved
  std::array<char, 4> reason() const { // e.g. "T1  " news pending; all spaces if none
    std::array<char, 4> r;
    std::memcpy(r.data(), p + 21, 4);
    return r;
  }
};

// Expected size per type byte. 0 = unknown type.
inline constexpr auto kSizeByType = [] {
  std::array<uint8_t, 256> t{};
  t['S'] = 12;
  t['R'] = 39;
  t['H'] = 25;
  t['Y'] = 20;
  t['L'] = 26;
  t['V'] = 35;
  t['W'] = 12;
  t['K'] = 28;
  t['J'] = 35;
  t['h'] = 21;
  t['A'] = 36;
  t['F'] = 40;
  t['E'] = 31;
  t['C'] = 36;
  t['X'] = 23;
  t['D'] = 19;
  t['U'] = 35;
  t['P'] = 44;
  t['Q'] = 40;
  t['B'] = 19;
  t['I'] = 50;
  t['N'] = 20;
  t['O'] = 48;
  return t;
}();

// Every view's kSize must agree with the table the parser checks lengths against
template <class Msg>
constexpr bool kSizeMatches = Msg::kSize == kSizeByType[static_cast<unsigned char>(Msg::kType)];

static_assert(kSizeMatches<AddOrder>);
static_assert(kSizeMatches<AddOrderMpid>);
static_assert(kSizeMatches<OrderExecuted>);
static_assert(kSizeMatches<OrderExecutedWithPrice>);
static_assert(kSizeMatches<OrderCancel>);
static_assert(kSizeMatches<OrderDelete>);
static_assert(kSizeMatches<OrderReplace>);
static_assert(kSizeMatches<Trade>);
static_assert(kSizeMatches<SystemEvent>);
static_assert(kSizeMatches<StockDirectory>);
static_assert(kSizeMatches<StockTradingAction>);

} // namespace ttt::itch
