// Order book throughput: full replay of an ITCH file through BookManager.
//
// Usage:
//   bench_order_book <itch-file> [--benchmark_* flags]
//   e.g. build-release/bench/bench_order_book data/itch/12302019.NASDAQ_ITCH50
//          --benchmark_repetitions=5 --benchmark_display_aggregates_only=true
//          --benchmark_out=docs/design/order-book-results/v1/v1-gha-x86-bench.json
//          --benchmark_out_format=json
//
// One iteration = one pass over the whole file through a fresh BookManager.
// Construction and destruction of the BookManager aren't timed. The per_msg
// counter includes parsing (~2-5 ns/msg, see bench_itch_parse); the rest is the book.
// The book_errors counter must be 0: otherwise the replay itself is wrong.
//
// Warm-up: one untimed pass runs before the first repetition. Without it the
// first repetition was consistently ~20% slower than the rest (one-time costs
// such as growing the heap), which skewed the median of a few repetitions.

#include "core/mapped_file.hpp"
#include "feed/book/book_manager.hpp"
#include "feed/itch/framing.hpp"
#include "feed/itch/parser.hpp"

#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <span>

namespace itch = ttt::itch;

namespace {

std::span<const std::byte> g_data; // the mapped file, set in main()

void replay_book(benchmark::State& state) {
  uint64_t messages = 0;
  uint64_t errors = 0;
  for (auto _ : state) {
    state.PauseTiming();
    auto books = std::make_unique<ttt::book::BookManager>();
    state.ResumeTiming();

    itch::FrameReader frames(g_data);
    messages = 0;
    for (auto msg = frames.next(); !msg.empty(); msg = frames.next()) {
      messages += itch::parse(msg, *books) == itch::ParseResult::Ok;
    }
    benchmark::DoNotOptimize(books->live_orders());

    state.PauseTiming();
    errors = books->errors().total();
    books.reset();
    state.ResumeTiming();
  }

  const auto passes = static_cast<int64_t>(state.iterations());
  state.SetItemsProcessed(passes * static_cast<int64_t>(messages));
  state.SetBytesProcessed(passes * static_cast<int64_t>(g_data.size()));
  state.counters["per_msg"] = benchmark::Counter(
      static_cast<double>(messages),
      benchmark::Counter::kIsIterationInvariantRate | benchmark::Counter::kInvert);
  state.counters["book_errors"] = static_cast<double>(errors);
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

    // One pass takes over a minute, so these mean "one iteration" each:
    //  - MinWarmUpTime: a single untimed warm-up pass, once, before the first
    //    repetition. Google Benchmark only honours a per-benchmark warm-up time
    //    when MinTime is also set on the benchmark.
    //  - MinTime: one timed pass per repetition.
    benchmark::RegisterBenchmark("replay_book_v1", replay_book)
        ->Unit(benchmark::kMillisecond)
        ->MinTime(1.0)
        ->MinWarmUpTime(1.0);
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
  } catch (const std::exception& e) {
    std::fprintf(stderr, "error: %s\n", e.what());
    return 1;
  }
}
