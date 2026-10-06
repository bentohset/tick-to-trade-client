// ITCH parser throughput: messages/sec and ns/msg over a whole ITCH file.
//
// Usage:
//   bench_itch_parse <itch-file> [--benchmark_* flags]
//   e.g. build-release/bench/bench_itch_parse data/itch/12302019.NASDAQ_ITCH50 \
//          --benchmark_repetitions=5 --benchmark_report_aggregates_only=true
//
// Each benchmark walks the whole file once per iteration. They build on each other,
// so the difference between two rows is the cost of the extra step:
//   frame_only    length-prefix framing (FrameReader) only
//   parse_null    + type lookup, length check and dispatch, with an empty handler
//   parse_header  + reading every message's header (locate, timestamp)
//   parse_book    + reading the fields the order book uses (A/F/E/C/X/D/U)
//
// The file is memory-mapped and prefaulted before timing, so page faults and disk
// reads aren't measured. Run a Release build; Debug numbers are meaningless.

#include "core/mapped_file.hpp"
#include "feed/itch/framing.hpp"
#include "feed/itch/parser.hpp"

#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <span>

namespace itch = ttt::itch;

namespace {

std::span<const std::byte> g_data; // the mapped file, set in main()

// --- handlers -------------------------------------------------------------------------

// Does nothing: measures framing + dispatch only
struct NullSink : itch::NullHandler {};

// Reads the header of every message, which any real consumer does
struct HeaderSink : itch::NullHandler {
  uint64_t acc = 0;

  template <class Msg> void on(const Msg& m) { touch(m); }
  void on_other(char /*type*/, std::span<const std::byte> msg) { touch(itch::Header{msg.data()}); }

private:
  void touch(const itch::Header& h) { acc += h.stock_locate() ^ h.timestamp_ns(); }
};

// Reads the fields the order book needs from the messages that change it
struct BookFieldsSink : itch::NullHandler {
  using NullHandler::on; // non-template overloads only: the rest stay empty
  uint64_t acc = 0;

  void on(const itch::AddOrder& m) {
    acc +=
        m.order_ref() + m.stock_locate() + m.shares() + m.price() + static_cast<uint64_t>(m.side());
  }
  void on(const itch::AddOrderMpid& m) { on(static_cast<const itch::AddOrder&>(m)); }
  void on(const itch::OrderExecuted& m) { acc += m.order_ref() + m.executed_shares(); }
  void on(const itch::OrderExecutedWithPrice& m) { acc += m.order_ref() + m.executed_shares(); }
  void on(const itch::OrderCancel& m) { acc += m.order_ref() + m.cancelled_shares(); }
  void on(const itch::OrderDelete& m) { acc += m.order_ref(); }
  void on(const itch::OrderReplace& m) {
    acc += m.original_order_ref() + m.new_order_ref() + m.shares() + m.price();
  }
};

// --- benchmarks -----------------------------------------------------------------------

void report(benchmark::State& state, uint64_t messages_per_pass) {
  const auto passes = static_cast<int64_t>(state.iterations());
  state.SetItemsProcessed(passes * static_cast<int64_t>(messages_per_pass));
  state.SetBytesProcessed(passes * static_cast<int64_t>(g_data.size()));
  // Inverted rate = seconds per message; printed with an SI prefix, e.g. "2.1n" = 2.1 ns
  state.counters["per_msg"] = benchmark::Counter(
      static_cast<double>(messages_per_pass),
      benchmark::Counter::kIsIterationInvariantRate | benchmark::Counter::kInvert);
}

void frame_only(benchmark::State& state) {
  uint64_t messages = 0;
  for (auto _ : state) {
    itch::FrameReader frames(g_data);
    messages = 0;
    for (auto msg = frames.next(); !msg.empty(); msg = frames.next()) {
      benchmark::DoNotOptimize(msg.data());
      ++messages;
    }
  }
  report(state, messages);
}

template <class Sink> void parse(benchmark::State& state) {
  uint64_t messages = 0;
  for (auto _ : state) {
    Sink sink;
    itch::FrameReader frames(g_data);
    messages = 0;
    for (auto msg = frames.next(); !msg.empty(); msg = frames.next()) {
      // Counting Ok results keeps the type lookup and length check from being optimized out
      messages += itch::parse(msg, sink) == itch::ParseResult::Ok;
    }
    benchmark::DoNotOptimize(sink);
  }
  report(state, messages);
}

// Touch every page once so timing doesn't include page faults or disk reads
void prefault(std::span<const std::byte> data) {
  constexpr std::size_t kStride = 4096;
  uint64_t sum = 0;
  for (std::size_t i = 0; i < data.size(); i += kStride) sum += static_cast<uint8_t>(data[i]);
  benchmark::DoNotOptimize(sum);
}

} // namespace

int main(int argc, char** argv) {
  benchmark::Initialize(&argc, argv); // removes --benchmark_* flags from argv
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s <itch-file> [--benchmark_* flags]\n", argv[0]);
    return 2;
  }
#ifndef NDEBUG
  std::fprintf(stderr, "warning: built without optimizations (Debug); numbers are meaningless\n");
#endif

  try {
    const ttt::core::MappedFile file(argv[1]);
    g_data = file.bytes();
    prefault(g_data);

    for (auto* b : {
             benchmark::RegisterBenchmark("frame_only", frame_only),
             benchmark::RegisterBenchmark("parse_null", parse<NullSink>),
             benchmark::RegisterBenchmark("parse_header", parse<HeaderSink>),
             benchmark::RegisterBenchmark("parse_book", parse<BookFieldsSink>),
         }) {
      b->Unit(benchmark::kMillisecond);
    }

    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
  } catch (const std::exception& e) {
    std::fprintf(stderr, "error: %s\n", e.what());
    return 1;
  }
}
