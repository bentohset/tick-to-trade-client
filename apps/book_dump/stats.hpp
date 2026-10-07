#pragma once

#include "core/format.hpp"
#include "core/mapped_file.hpp"
#include "feed/itch/framing.hpp"
#include "feed/itch/parser.hpp"
namespace book_dump {

namespace detail {

struct Stats : ttt::itch::NullHandler {
  std::array<uint64_t, 256> by_type{};
  uint64_t first_ts = 0, last_ts = 0, ts_regressions = 0;

  template <class Msg> void on(const Msg& m) { record(m); }

  void on_other(char /*type*/, std::span<const std::byte> msg) {
    record(ttt::itch::Header{msg.data()});
  }

private:
  void record(const ttt::itch::Header& h) {
    ++by_type[static_cast<unsigned char>(h.type())];
    const uint64_t ts = h.timestamp_ns();
    if (first_ts == 0) first_ts = ts;
    if (ts < last_ts) ++ts_regressions;
    last_ts = ts;
  }
};

void print_labeled_time(const char* label, uint64_t ns) {
  std::printf("%-16s ", label);
  ttt::core::print_time_of_day(stdout, ns);
  std::printf("\n");
}

} // namespace detail

inline int run_stats(const char* path, const ttt::core::MappedFile& file) {
  namespace itch = ttt::itch;
  itch::FrameReader frames(file.bytes());
  detail::Stats stats;
  uint64_t n = 0;
  uint64_t unknown = 0;

  for (auto msg = frames.next(); !msg.empty(); msg = frames.next()) {
    switch (itch::parse(msg, stats)) {
      case itch::ParseResult::Ok: ++n; break;
      case itch::ParseResult::Unknown: ++unknown; break;
      case itch::ParseResult::BadLength:
        std::fprintf(stderr, "error: bad length %zu for type '%c' at offset %zu\n", msg.size(),
                     static_cast<char>(msg[0]), frames.offset() - msg.size() - 2);
        return 1;
    }
  }

  std::printf("%-16s %s\n", "file", path);
  for (int t = 0; t < 256; ++t) {
    if (stats.by_type[t] != 0) {
      std::printf("  %c %20llu\n", static_cast<char>(t),
                  static_cast<unsigned long long>(stats.by_type[t]));
    }
  }
  std::printf("%-16s %llu\n", "messages", static_cast<unsigned long long>(n));
  std::printf("%-16s %llu\n", "unknown types", static_cast<unsigned long long>(unknown));
  std::printf("%-16s %s\n", "truncated tail", frames.truncated() ? "yes" : "no");
  std::printf("%-16s %llu\n", "ts regressions",
              static_cast<unsigned long long>(stats.ts_regressions));
  detail::print_labeled_time("first timestamp", stats.first_ts);
  detail::print_labeled_time("last timestamp", stats.last_ts);
  return 0;
}

} // namespace book_dump
