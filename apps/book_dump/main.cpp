#include "core/mapped_file.hpp"
#include "feed/itch/framing.hpp"
#include "feed/itch/parser.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
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

using Clock = std::chrono::steady_clock;

uint64_t elapsed_ns(Clock::time_point a, Clock::time_point b) {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count());
}

// Average cost of an empty now()/now() pair. A message parses in a few ns, about
// the same as reading the clock, so this is subtracted from the per-type averages.
double timer_overhead_ns() {
  constexpr int kSamples = 10'000'000;
  uint64_t total = 0;
  for (int i = 0; i < kSamples; ++i) {
    const auto a = Clock::now();
    const auto b = Clock::now();
    total += elapsed_ns(a, b);
  }
  return static_cast<double>(total) / kSamples;
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
    // Total parse time per message type, timed around each parse() call.
    // A single reading is mostly clock noise; the average over many messages is what's printed.
    std::array<uint64_t, 256> ns_by_type{};
    const double overhead_ns = timer_overhead_ns();
    const auto t0 = Clock::now();
    for (auto msg = frames.next(); !msg.empty(); msg = frames.next()) {
      const auto start = Clock::now();
      const auto result = itch::parse(msg, stats);
      ns_by_type[static_cast<unsigned char>(msg[0])] += elapsed_ns(start, Clock::now());
      switch (result) {
        case ttt::itch::ParseResult::Ok: ++n; break;
        case ttt::itch::ParseResult::Unknown: ++unknown; break;
        case ttt::itch::ParseResult::BadLength:
          std::fprintf(stderr, "error: bad length %zu for type '%c' at offset %zu\n", msg.size(),
                       static_cast<char>(msg[0]), frames.offset() - msg.size() - 2);
          return 1;
      }
    }

    const double secs = std::chrono::duration<double>(Clock::now() - t0).count();
    std::printf("%-16s %s\n", "file", argv[1]);
    std::printf("  %-4s %16s %12s\n", "type", "count", "avg ns/msg");
    // Below this many samples the average is dominated by clock ticks and cold caches
    constexpr uint64_t kMinSamples = 10'000;
    for (int t = 0; t < 256; ++t) {
      const uint64_t count = stats.by_type[t];
      if (count == 0) continue;
      if (count < kMinSamples) {
        std::printf("  %-4c %16llu %12s\n", static_cast<char>(t),
                    static_cast<unsigned long long>(count), "-");
        continue;
      }
      const double avg = static_cast<double>(ns_by_type[t]) / static_cast<double>(count);
      std::printf("  %-4c %16llu %12.1f\n", static_cast<char>(t),
                  static_cast<unsigned long long>(count), avg - overhead_ns);
    }
    std::printf("%-16s %.1f ns (subtracted from avg ns/msg)\n", "timer overhead", overhead_ns);
    std::printf("%-16s %llu\n", "messages", static_cast<unsigned long long>(n));
    std::printf("%-16s %llu\n", "unknown types", static_cast<unsigned long long>(unknown));
    std::printf("%-16s %s\n", "truncated tail", frames.truncated() ? "yes" : "no");
    std::printf("%-16s %llu\n", "ts regressions",
                static_cast<unsigned long long>(stats.ts_regressions));
    print_time("first timestamp", stats.first_ts);
    print_time("last timestamp", stats.last_ts);
    std::printf("%-16s %.3f s  (%.1f M msgs/s, %.2f GB/s; includes timer overhead)\n", "parse time",
                secs, static_cast<double>(n) / secs / 1e6,
                static_cast<double>(f.bytes().size()) / secs / 1e9);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "error: %s\n", e.what());
    return 1;
  }
}
