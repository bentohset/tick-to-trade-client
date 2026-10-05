#pragma once

#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace ttt::be {

template <std::unsigned_integral T> [[nodiscard]] inline T load(const std::byte* p) noexcept {
  T v;
  // use memcpy and byteswap, not reinterpret_cast to a packed struct.
  // casting is UB as messages sit at arbitrary offsets.
  // compiler turns memcpy plus byteswap into a single ldr+rev(ARM) or movbe(x86)
  std::memcpy(&v, p, sizeof v);
  if constexpr (std::endian::native == std::endian::little) v = std::byteswap(v);
  return v;
}

// ITCH timestamps = 6 bytes
[[nodiscard]] inline uint64_t load48(const std::byte* p) noexcept {
  return (uint64_t{load<uint16_t>(p)} << 32) | load<uint32_t>(p + 2);
}

} // namespace ttt::be
