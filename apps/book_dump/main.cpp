#include "core/mapped_file.hpp"
#include "feed/itch/framing.hpp"
#include "feed/itch/parser.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <span>

namespace itch = ttt::itch;

namespace {

struct Stats : itch::NullHandler {
  std::array<uint64_t, 256> by_type{};
  uint64_t first_ts = 0, last_ts = 0, ts_regressions = 0;

  template <class Msg> void on(const Msg& m) { record(m); }

  void on_other(char /*type*/, std::span<const std::byte> msg) { record(itch::Header{msg.data()}); }

private:
  void record(const itch::Header& h) {
    ++by_type[static_cast<unsigned char>(h.type())];
    const uint64_t ts = h.timestamp_ns();
    if (first_ts == 0) first_ts = ts;
    if (ts < last_ts) ++ts_regressions;
    last_ts = ts;
  }
};

void print_time(const char* label, uint64_t ns) {
  const uint64_t s = ns / 1'000'000'000;
  std::printf("%-16s %02llu:%02llu:%02llu.%09llu\n", label,
              static_cast<unsigned long long>(s / 3600),
              static_cast<unsigned long long>(s / 60 % 60), static_cast<unsigned long long>(s % 60),
              static_cast<unsigned long long>(ns % 1'000'000'000));
}

} // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s <itch-file>\n", argv[0]);
    return 2;
  }
  try {
    ttt::core::MappedFile f(argv[1]);
    itch::FrameReader frames(f.bytes());
    Stats stats;
    uint64_t n = 0, unknown = 0;
    for (auto msg = frames.next(); !msg.empty(); msg = frames.next()) {
      switch (itch::parse(msg, stats)) {
        case ttt::itch::ParseResult::Ok: ++n; break;
        case ttt::itch::ParseResult::Unknown: ++unknown; break;
        case ttt::itch::ParseResult::BadLength:
          std::fprintf(stderr, "error: bad length %zu for type '%c' at offset %zu\n", msg.size(),
                       static_cast<char>(msg[0]), frames.offset() - msg.size() - 2);
          return 1;
      }
    }

    std::printf("%-16s %s\n", "file", argv[1]);
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
    print_time("first timestamp", stats.first_ts);
    print_time("last timestamp", stats.last_ts);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "error: %s\n", e.what());
    return 1;
  }
}
