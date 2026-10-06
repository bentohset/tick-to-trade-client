#pragma once

#include "core/format.hpp"
#include <charconv>
#include <optional>
#include <string_view>
#include <system_error>

namespace book_dump {

struct Options {
  std::string_view file;
  bool check = false;            // --check
  std::string_view symbol;       // --symbol; empty = dont print a book
  std::optional<uint64_t> at_ns; // --at; apply only messages before this time
  std::size_t depth = 10;        // --depth

  bool book_mode() const { return check || !symbol.empty(); }
};

inline void usage(const char* prog) {
  std::fprintf(stderr,
               "usage: %s <itch-file> [--check] [--symbol SYM] [--at HH:MM[:SS[.fff]]] "
               "[--depth N]\n"
               "  (no flags)  message counts and file sanity checks\n"
               "  --check     replay through the order book; exit 1 on any error\n"
               "  --symbol    print SYM's book at the end of the file, or at --at\n"
               "  --at        stop before the first message at/after this time\n"
               "  --depth     levels per side to print (default 10)\n",
               prog);
}

inline std::optional<Options> parse_args(int argc, char** argv) {
  Options o;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    const bool has_value = i + 1 < argc;
    if (arg == "--check") {
      o.check = true;
    } else if (arg == "--symbol" && has_value) {
      o.symbol = argv[++i];
    } else if (arg == "--at" && has_value) {
      o.at_ns = ttt::core::parse_time_of_day(argv[++i]);
      if (!o.at_ns) return std::nullopt;
    } else if (arg == "--depth" && has_value) {
      const std::string_view v = argv[++i];
      const auto [end, ec] = std::from_chars(v.data(), v.data() + v.size(), o.depth);
      if (ec != std::errc{} || end != v.data() + v.size()) return std::nullopt;
    } else if (!arg.starts_with("-") && o.file.empty()) {
      o.file = arg;
    } else {
      return std::nullopt;
    }
  }
  if (o.file.empty()) return std::nullopt;
  if (o.at_ns && !o.book_mode()) return std::nullopt; // --at needs --check or --symbol
  return o;
}

} // namespace book_dump
