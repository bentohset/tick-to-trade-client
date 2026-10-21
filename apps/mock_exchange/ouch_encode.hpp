#pragma once

#include "core/endian.hpp"
#include "feed/itch/messages.hpp"
#include "gateway/ouch/codec.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace mock_exchange {

namespace be = ttt::be;
namespace itch = ttt::itch;
namespace ouch = ttt::ouch;

// What the exchange knows about one token
struct ExOrder {
  itch::Symbol stock{};
  char side = 'B';
  uint32_t price = 0;
  uint32_t tif = 0;
  uint32_t open = 0;
  uint32_t executed = 0;
  bool live = false;
  uint64_t next_fill_ns = 0;
};

// Exchange-side encoders, mirroring the views in gateway/ouch/codec.hpp.
// Also used by ouch_codec_test to build fixtures

inline void put_head(std::byte* p, char type, uint64_t ts, const ouch::Token& t) {
  p[0] = static_cast<std::byte>(type);
  be::store(p + 1, ts);
  std::memcpy(p + 9, t.data(), 14);
}

// offsets 23..64, shared by Accepted and Replaced
inline void put_order_body(std::byte* p, const ExOrder& o, uint32_t shares, uint64_t ref,
                           char state) {
  p[23] = static_cast<std::byte>(o.side);
  be::store(p + 24, shares);
  std::memcpy(p + 28, o.stock.raw.data(), 8);
  be::store(p + 36, o.price);
  be::store(p + 40, o.tif);
  std::memset(p + 44, ' ', 4); // firm
  p[48] = std::byte{'Y'};
  be::store(p + 49, ref);
  p[57] = std::byte{'P'};
  p[58] = std::byte{'N'};
  be::store(p + 59, uint32_t{0});
  p[63] = std::byte{'N'};
  p[64] = static_cast<std::byte>(state);
}

inline void encode_accepted(std::byte* p, uint64_t ts, const ouch::Token& t, const ExOrder& o,
                            uint32_t shares, uint64_t ref, char state) {
  put_head(p, 'A', ts, t);
  put_order_body(p, o, shares, ref, state);
  p[65] = std::byte{' '}; // BBO weight
}

inline void encode_replaced(std::byte* p, uint64_t ts, const ouch::Token& t, const ExOrder& o,
                            uint64_t ref, char state, const ouch::Token& prev) {
  put_head(p, 'U', ts, t);
  put_order_body(p, o, o.open, ref, state);
  std::memcpy(p + 65, prev.data(), 14);
  p[79] = std::byte{' '};
}

inline void encode_executed(std::byte* p, uint64_t ts, const ouch::Token& t, uint32_t qty,
                            uint32_t px, char liq, uint64_t match) {
  put_head(p, 'E', ts, t);
  be::store(p + 23, qty);
  be::store(p + 27, px);
  p[31] = static_cast<std::byte>(liq);
  be::store(p + 32, match);
}

inline void encode_canceled(std::byte* p, uint64_t ts, const ouch::Token& t, uint32_t dec,
                            char reason) {
  put_head(p, 'C', ts, t);
  be::store(p + 23, dec);
  p[27] = static_cast<std::byte>(reason);
}

inline void encode_rejected(std::byte* p, uint64_t ts, const ouch::Token& t, char reason) {
  put_head(p, 'J', ts, t);
  p[23] = static_cast<std::byte>(reason);
}

} // namespace mock_exchange
