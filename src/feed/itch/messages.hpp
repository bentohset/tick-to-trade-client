#pragma once

#include "core/endian.hpp"

#include <array>
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
  std::array<char, 4> attribution() const;
};

// TODO: fill the rest of the structs
struct OrderExecuted : Header { /* 'E', 31: order_ref, executed_shares, match_number */
};
struct OrderExecutedWithPrice : Header { /* 'C', 36: + printable, execution_price */
};
struct OrderCancel : Header { /* 'X', 23 */
};
struct OrderDelete : Header { /* 'D', 19 */
};
struct OrderReplace : Header { /* 'U', 35: orig_ref, new_ref, shares, price */
};
struct Trade : Header { /* 'P', 44 */
};
struct SystemEvent : Header { /* 'S', 12: event_code */
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

struct StockTradingAction : Header { /* 'H', 25: stock, trading_state */
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

} // namespace ttt::itch
