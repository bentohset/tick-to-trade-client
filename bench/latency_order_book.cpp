// Order book latency distribution: times every message's book apply individually.
//
// Usage:
//   latency_order_book <itch-file> [--label NAME] [--max-messages N] > result.json
//   e.g. build-release/bench/latency_order_book data/itch/12302019.NASDAQ_ITCH50 \
//          --label v1-gha-x86 > docs/design/results/v1-gha-x86-latency.json
//
// For each message: read_ticks(); parse + apply to BookManager; read_ticks().
// The difference goes into a histogram per message type. Output (stdout) is JSON
// with count, mean, p50/p90/p99/p99.9 and max per type, plus:
//   "book": A F E C X D U combined (the messages that change the book)
//   "all":  every message
// A short human-readable table goes to stderr.
//
// Times include parsing (~2-5 ns) and the timer's own cost ("timer" in the JSON:
// distribution of an empty read/read pair). Nothing is subtracted. On Apple Silicon
// the counter ticks every 41.67 ns, so values are multiples of that.

#include "core/clock.hpp"
#include "core/latency_histogram.hpp"
#include "core/mapped_file.hpp"
#include "feed/book/book_manager.hpp"
#include "feed/itch/framing.hpp"
#include "feed/itch/parser.hpp"

#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <sys/utsname.h>

namespace itch = ttt::itch;
using ttt::core::LatencyHistogram;

namespace {

constexpr std::string_view kBookTypes = "AFECXDU";

struct Args {
  const char* file = nullptr;
  std::string_view label = "unlabeled";
  uint64_t max_messages = UINT64_MAX;
};

bool parse_args(int argc, char** argv, Args& a) {
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--label" && i + 1 < argc) {
      a.label = argv[++i];
    } else if (arg == "--max-messages" && i + 1 < argc) {
      const std::string_view v = argv[++i];
      const auto [end, ec] = std::from_chars(v.data(), v.data() + v.size(), a.max_messages);
      if (ec != std::errc{} || end != v.data() + v.size()) return false;
    } else if (!arg.starts_with("-") && a.file == nullptr) {
      a.file = argv[i];
    } else {
      return false;
    }
  }
  return a.file != nullptr;
}

uint64_t to_ns(uint64_t ticks, double ns_per_tick) {
  return static_cast<uint64_t>(std::llround(static_cast<double>(ticks) * ns_per_tick));
}

// Distribution of an empty read/read pair: the floor under every measurement
LatencyHistogram measure_timer(double ns_per_tick) {
  LatencyHistogram h;
  for (int i = 0; i < 10'000'000; ++i) {
    const uint64_t t0 = ttt::core::read_ticks();
    const uint64_t t1 = ttt::core::read_ticks();
    h.record(to_ns(t1 - t0, ns_per_tick));
  }
  return h;
}

void prefault(std::span<const std::byte> data) {
  constexpr std::size_t kStride = 4096;
  volatile uint64_t sum = 0;
  for (std::size_t i = 0; i < data.size(); i += kStride) sum = sum + static_cast<uint8_t>(data[i]);
}

void json_stats(const LatencyHistogram& h) {
  std::printf("{\"count\": %llu, \"mean\": %.1f, \"min\": %llu, \"p50\": %llu, \"p90\": %llu, "
              "\"p99\": %llu, \"p999\": %llu, \"max\": %llu, \"over_65us\": %llu}",
              static_cast<unsigned long long>(h.count()), h.mean(),
              static_cast<unsigned long long>(h.min()),
              static_cast<unsigned long long>(h.percentile(0.50)),
              static_cast<unsigned long long>(h.percentile(0.90)),
              static_cast<unsigned long long>(h.percentile(0.99)),
              static_cast<unsigned long long>(h.percentile(0.999)),
              static_cast<unsigned long long>(h.max()),
              static_cast<unsigned long long>(h.overflow()));
}

void table_row(std::string_view name, const LatencyHistogram& h) {
  std::fprintf(stderr, "  %-6.*s %12llu %8.1f %7llu %7llu %7llu %8llu %10llu\n",
               static_cast<int>(name.size()), name.data(),
               static_cast<unsigned long long>(h.count()), h.mean(),
               static_cast<unsigned long long>(h.percentile(0.50)),
               static_cast<unsigned long long>(h.percentile(0.90)),
               static_cast<unsigned long long>(h.percentile(0.99)),
               static_cast<unsigned long long>(h.percentile(0.999)),
               static_cast<unsigned long long>(h.max()));
}

std::string_view basename(std::string_view path) {
  const auto slash = path.find_last_of('/');
  return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

} // namespace

int main(int argc, char** argv) {
  Args args;
  if (!parse_args(argc, argv, args)) {
    std::fprintf(stderr, "usage: %s <itch-file> [--label NAME] [--max-messages N] > out.json\n",
                 argv[0]);
    return 2;
  }
#ifndef NDEBUG
  std::fprintf(stderr, "warning: built without optimizations (Debug); numbers are meaningless\n");
#endif

  try {
    const ttt::core::MappedFile file(args.file);
    prefault(file.bytes());

    const double ns_per_tick = ttt::core::ns_per_tick();
    const LatencyHistogram timer = measure_timer(ns_per_tick);

    // One histogram per message type, created on first use (each is 512 KB)
    std::array<std::unique_ptr<LatencyHistogram>, 256> by_type;
    auto books = std::make_unique<ttt::book::BookManager>();
    itch::FrameReader frames(file.bytes());
    uint64_t messages = 0;

    for (auto msg = frames.next(); !msg.empty() && messages < args.max_messages;
         msg = frames.next()) {
      const uint64_t t0 = ttt::core::read_ticks();
      const auto result = itch::parse(msg, *books);
      const uint64_t t1 = ttt::core::read_ticks();

      if (result == itch::ParseResult::BadLength) {
        std::fprintf(stderr, "error: bad length at offset %zu\n", frames.offset() - msg.size() - 2);
        return 1;
      }
      auto& h = by_type[static_cast<unsigned char>(msg[0])];
      if (!h) h = std::make_unique<LatencyHistogram>();
      h->record(to_ns(t1 - t0, ns_per_tick));
      ++messages;
    }

    LatencyHistogram book;
    LatencyHistogram all;
    for (int t = 0; t < 256; ++t) {
      if (!by_type[t]) continue;
      all.merge(*by_type[t]);
      if (kBookTypes.find(static_cast<char>(t)) != std::string_view::npos) book.merge(*by_type[t]);
    }

    utsname host{};
    uname(&host);

    // --- JSON (stdout) ---
    std::printf("{\n");
    std::printf("  \"label\": \"%.*s\",\n", static_cast<int>(args.label.size()), args.label.data());
    const auto file_name = basename(args.file);
    std::printf("  \"file\": \"%.*s\",\n", static_cast<int>(file_name.size()), file_name.data());
    std::printf("  \"machine\": \"%s %s\",\n", host.sysname, host.machine);
    std::printf("  \"messages\": %llu,\n", static_cast<unsigned long long>(messages));
    std::printf("  \"book_errors\": %llu,\n",
                static_cast<unsigned long long>(books->errors().total()));
    std::printf("  \"live_orders_at_end\": %zu,\n", books->live_orders());
    std::printf("  \"timer\": {\"source\": \"%s\", \"ns_per_tick\": %.4f, \"empty_pair\": ",
                ttt::core::tick_source(), ns_per_tick);
    json_stats(timer);
    std::printf("},\n");
    std::printf("  \"book\": ");
    json_stats(book);
    std::printf(",\n  \"all\": ");
    json_stats(all);
    std::printf(",\n  \"types\": {");
    bool first = true;
    for (int t = 0; t < 256; ++t) {
      if (!by_type[t]) continue;
      std::printf("%s\n    \"%c\": ", first ? "" : ",", static_cast<char>(t));
      json_stats(*by_type[t]);
      first = false;
    }
    std::printf("\n  }\n}\n");

    // --- summary (stderr) ---
    std::fprintf(stderr, "%s: %llu messages, book_errors %llu, timer %s (%.2f ns/tick)\n",
                 args.file, static_cast<unsigned long long>(messages),
                 static_cast<unsigned long long>(books->errors().total()),
                 ttt::core::tick_source(), ns_per_tick);
    std::fprintf(stderr, "  %-6s %12s %8s %7s %7s %7s %8s %10s   (ns)\n", "type", "count", "mean",
                 "p50", "p90", "p99", "p99.9", "max");
    table_row("timer", timer);
    for (const char t : kBookTypes) {
      if (by_type[static_cast<unsigned char>(t)]) table_row(std::string(1, t), *by_type[static_cast<unsigned char>(t)]);
    }
    table_row("book", book);
    table_row("all", all);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "error: %s\n", e.what());
    return 1;
  }
}
