#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string_view>
namespace ttt::core {

inline constexpr uint64_t kNsPerSec = 1'000'000'000;

// "HH:MM", "HH:MM:SS" or "HH:MM:SS.fffffffff" (1-9 fraction digits) -> ns since midnight
// Hours and minutes are always 2 digits
inline std::optional<uint64_t> parse_time_of_day(std::string_view s) {
  const auto digit = [&](std::size_t i) { return i < s.size() && s[i] >= '0' && s[i] <= '9'; };
  const auto two = [&](std::size_t i) -> std::optional<unsigned> {
    if (!digit(i) || !digit(i + 1)) return std::nullopt;
    return static_cast<unsigned>((s[i] - '0') * 10 + (s[i + 1] - '0'));
  };

  const auto h = two(0);
  if (!h || s.size() < 5 || s[2] != ':') return std::nullopt;
  const auto m = two(3);
  if (!m) return std::nullopt;

  unsigned sec = 0;
  uint64_t frac_ns = 0;
  std::size_t pos = 5;
  if (pos < s.size()) { // ":SS"
    if (s[pos] != ':') return std::nullopt;
    const auto sv = two(pos + 1);
    if (!sv) return std::nullopt;
    sec = *sv;
    pos += 3;
    if (pos < s.size()) { // ".fff"
      if (s[pos] != '.') return std::nullopt;
      ++pos;
      std::size_t digits = s.size() - pos;
      if (digits == 0 || digits > 9) return std::nullopt;
      for (; pos < s.size(); ++pos) {
        if (!digit(pos)) return std::nullopt;
        frac_ns = frac_ns * 10 + static_cast<uint64_t>(s[pos] - '0');
      }
      for (; digits < 9; ++digits) frac_ns *= 10; // ".5" = 500'000'000 ns
    }
  }
  if (*h > 23 || *m > 59 || sec > 59) return std::nullopt;
  return ((uint64_t{*h} * 60 + *m) * 60 + sec) * kNsPerSec + frac_ns;
}

// ns since midnight -> "HH:MM:SS.nnnnnnnnn"
inline void print_time_of_day(std::FILE* out, uint64_t ns) {
  const uint64_t s = ns / kNsPerSec;
  std::fprintf(out, "%02llu:%02llu:%02llu.%09llu", static_cast<unsigned long long>(s / 3600),
               static_cast<unsigned long long>(s / 60 % 60),
               static_cast<unsigned long long>(s % 60),
               static_cast<unsigned long long>(ns % kNsPerSec));
}

// ITCH/OUCH Price(4), 4 decimals: 2904500 -> 290.4500
inline void print_price(std::FILE* out, uint32_t price) {
  std::fprintf(out, "%u.%04u", price / 10'000, price % 10'000);
}

} // namespace ttt::core
