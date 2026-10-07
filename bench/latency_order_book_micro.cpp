// Per-component latency on the real day: order map operations and order book
// add / reduce / remove, each timed individually.
//
// Usage:
//   latency_order_book_micro <itch-file> [--label NAME] [--max-messages N] [--verify] > out.json
//   e.g. build-release/bench/latency_order_book_micro data/itch/12302019.NASDAQ_ITCH50
//          --label v2-gha-x86 > docs/design/order-book-results/v2/v2-gha-x86-latency-micro.json
//
// BookManager calls the map, pool and books internally, so there is nowhere to put
// a timer between those calls from outside. Instead this tool does BookManager's
// work itself (the Parts handler below): same OrderPool, OrderMap and OrderBooks,
// fed the real messages, with a timer around each component call. All components
// share the cache as in the real replay.
//
// Rows:
//   map_find     OrderMap::find    (lookup by order ref: E/C/X and U's original)
//   map_insert   OrderMap::insert  (A/F and U's replacement)
//   map_erase    OrderMap::erase   (D, full fills/cancels, U's original)
//   book_add     OrderBook::add    (insert into a price level)
//   book_reduce  OrderBook::reduce (partial execute/cancel)
//   book_remove  OrderBook::remove (remove from a price level)
// The whole-message cost ("market event") is latency_order_book's "book" row.
//
// Caveats:
// - Parts duplicates BookManager's control flow (not its trading-state or crossed
//   checks, which aren't components being measured). --verify replays the same
//   messages through BookManager afterwards and compares every book's full depth
//   and the live order count; run it whenever BookManager changes.
// - Each timer pair costs ~25-30 ns on GitHub runners ("timer" in the JSON) and is
//   included in every value; nothing is subtracted. Timers also stop consecutive
//   work overlapping, so components don't add up to bench_order_book's ns/msg.

#include "core/clock.hpp"
#include "core/latency_histogram.hpp"
#include "core/mapped_file.hpp"
#include "feed/book/book_manager.hpp"
#include "feed/book/order_book.hpp"
#include "feed/book/order_map.hpp"
#include "feed/book/order_pool.hpp"
#include "feed/itch/framing.hpp"
#include "feed/itch/parser.hpp"

#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <sys/utsname.h>

namespace itch = ttt::itch;
using namespace ttt::book;
using ttt::core::LatencyHistogram;

namespace {

struct Args {
  const char* file = nullptr;
  std::string_view label = "unlabeled";
  uint64_t max_messages = UINT64_MAX;
  bool verify = false;
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
    } else if (arg == "--verify") {
      a.verify = true;
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

// --- BookManager's steps, with a timer around each component call -----------------

struct Parts : itch::NullHandler {
  using NullHandler::on;

  explicit Parts(double ns_per_tick)
      : pool(BookManager::kMaxOrders), map(BookManager::kMapSlots), ns_per_tick_(ns_per_tick) {}

  OrderPool pool;
  OrderMap map;
  std::vector<OrderBook> books; // index = stock locate

  LatencyHistogram map_find, map_insert, map_erase, book_add, book_reduce, book_remove;
  uint64_t skipped = 0; // messages Parts couldn't apply; must be 0 on a clean feed

  void on(const itch::StockDirectory& m) {
    if (m.stock_locate() >= books.size()) books.resize(m.stock_locate() + 1, OrderBook(&pool));
  }

  void on(const itch::AddOrder& m) {
    add(m.order_ref(), m.stock_locate(), to_side(m.side()), m.price(), m.shares());
  }
  void on(const itch::AddOrderMpid& m) { on(static_cast<const itch::AddOrder&>(m)); }
  void on(const itch::OrderExecuted& m) { reduce(m.order_ref(), m.executed_shares()); }
  void on(const itch::OrderExecutedWithPrice& m) { reduce(m.order_ref(), m.executed_shares()); }
  void on(const itch::OrderCancel& m) { reduce(m.order_ref(), m.cancelled_shares()); }
  void on(const itch::OrderDelete& m) { remove(m.order_ref()); }

  void on(const itch::OrderReplace& m) {
    const uint32_t old = timed(map_find, [&] { return map.find(m.original_order_ref()); });
    if (old == kNil) {
      ++skipped;
      return;
    }
    const uint16_t locate = pool[old].locate;
    const Side side = pool[old].side;
    remove(m.original_order_ref());
    add(m.new_order_ref(), locate, side, m.price(), m.shares());
  }

private:
  // Explicit return type: used by the handlers above, before this definition
  template <class F> std::invoke_result_t<F&> timed(LatencyHistogram& h, F&& f) {
    const uint64_t t0 = ttt::core::read_ticks();
    auto result = f();
    const uint64_t t1 = ttt::core::read_ticks();
    h.record(to_ns(t1 - t0, ns_per_tick_));
    return result;
  }

  void add(OrderRef ref, uint16_t locate, Side side, Price px, Qty qty) {
    if (locate >= books.size()) {
      ++skipped;
      return;
    }
    const uint32_t idx = pool.alloc();
    if (idx == kNil) {
      ++skipped;
      return;
    }
    if (!timed(map_insert, [&] { return map.insert(ref, idx); })) {
      pool.free(idx);
      ++skipped;
      return;
    }
    pool[idx] = Order{.ref = ref,
                      .price = px,
                      .qty = qty,
                      .prev = kNil,
                      .next = kNil,
                      .locate = locate,
                      .side = side};
    timed(book_add, [&] {
      books[locate].add(idx);
      return 0;
    });
  }

  void reduce(OrderRef ref, Qty qty) {
    const uint32_t idx = timed(map_find, [&] { return map.find(ref); });
    if (idx == kNil) {
      ++skipped;
      return;
    }
    if (qty >= pool[idx].qty) { // full fill/cancel (an over-reduce is clamped, as in BookManager)
      if (qty > pool[idx].qty) ++skipped;
      remove(ref);
      return;
    }
    timed(book_reduce, [&] {
      books[pool[idx].locate].reduce(idx, qty);
      return 0;
    });
  }

  void remove(OrderRef ref) {
    const uint32_t idx = timed(map_erase, [&] { return map.erase(ref); });
    if (idx == kNil) {
      ++skipped;
      return;
    }
    timed(book_remove, [&] {
      books[pool[idx].locate].remove(idx); // before free(): needs price, side and links
      return 0;
    });
    pool.free(idx);
  }

  double ns_per_tick_;
};

// --- --verify: same messages through BookManager, compare every book ----------------

bool same_levels(const std::vector<Level>& a, const std::vector<Level>& b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (a[i].price != b[i].price || a[i].qty != b[i].qty || a[i].orders != b[i].orders)
      return false;
  }
  return true;
}

bool verify(std::span<const std::byte> data, uint64_t messages, const Parts& parts) {
  auto mgr = std::make_unique<BookManager>();
  itch::FrameReader frames(data);
  uint64_t n = 0;
  for (auto msg = frames.next(); !msg.empty() && n < messages; msg = frames.next(), ++n) {
    itch::parse(msg, *mgr);
  }

  bool ok = true;
  if (mgr->live_orders() != parts.pool.live()) {
    std::fprintf(stderr, "verify: live orders differ: BookManager %zu, parts %u\n",
                 mgr->live_orders(), parts.pool.live());
    ok = false;
  }
  constexpr std::size_t kAllLevels = SIZE_MAX;
  std::size_t books_checked = 0;
  for (std::size_t locate = 0; locate < parts.books.size(); ++locate) {
    const OrderBook* expected = mgr->book(static_cast<uint16_t>(locate));
    if (expected == nullptr) continue; // locate never announced
    ++books_checked;
    for (const Side side : {Side::Buy, Side::Sell}) {
      if (!same_levels(expected->depth(side, kAllLevels),
                       parts.books[locate].depth(side, kAllLevels))) {
        std::fprintf(stderr, "verify: locate %zu %s side differs\n", locate,
                     side == Side::Buy ? "bid" : "ask");
        ok = false;
      }
    }
  }
  std::fprintf(stderr, "verify: %s (%zu books, %u live orders, BookManager errors %llu)\n",
               ok ? "OK" : "FAILED", books_checked, parts.pool.live(),
               static_cast<unsigned long long>(mgr->errors().total()));
  return ok;
}

// --- output -------------------------------------------------------------------------

void json_stats(const LatencyHistogram& h) {
  std::printf(
      "{\"count\": %llu, \"mean\": %.1f, \"min\": %llu, \"p50\": %llu, \"p90\": %llu, "
      "\"p99\": %llu, \"p999\": %llu, \"max\": %llu, \"over_65us\": %llu}",
      static_cast<unsigned long long>(h.count()), h.mean(),
      static_cast<unsigned long long>(h.min()), static_cast<unsigned long long>(h.percentile(0.50)),
      static_cast<unsigned long long>(h.percentile(0.90)),
      static_cast<unsigned long long>(h.percentile(0.99)),
      static_cast<unsigned long long>(h.percentile(0.999)),
      static_cast<unsigned long long>(h.max()), static_cast<unsigned long long>(h.overflow()));
}

void table_row(std::string_view name, const LatencyHistogram& h) {
  std::fprintf(stderr, "  %-12.*s %12llu %8.1f %7llu %7llu %7llu %8llu %10llu\n",
               static_cast<int>(name.size()), name.data(),
               static_cast<unsigned long long>(h.count()), h.mean(),
               static_cast<unsigned long long>(h.percentile(0.50)),
               static_cast<unsigned long long>(h.percentile(0.90)),
               static_cast<unsigned long long>(h.percentile(0.99)),
               static_cast<unsigned long long>(h.percentile(0.999)),
               static_cast<unsigned long long>(h.max()));
}

std::string_view file_name_of(std::string_view path) {
  const auto slash = path.find_last_of('/');
  return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

} // namespace

int main(int argc, char** argv) {
  Args args;
  if (!parse_args(argc, argv, args)) {
    std::fprintf(stderr,
                 "usage: %s <itch-file> [--label NAME] [--max-messages N] [--verify] > out.json\n",
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

    auto parts = std::make_unique<Parts>(ns_per_tick);
    itch::FrameReader frames(file.bytes());
    uint64_t messages = 0;
    for (auto msg = frames.next(); !msg.empty() && messages < args.max_messages;
         msg = frames.next()) {
      if (itch::parse(msg, *parts) == itch::ParseResult::BadLength) {
        std::fprintf(stderr, "error: bad length at offset %zu\n", frames.offset() - msg.size() - 2);
        return 1;
      }
      ++messages;
    }

    bool verified = true;
    if (args.verify) verified = verify(file.bytes(), messages, *parts);

    const std::pair<std::string_view, const LatencyHistogram*> rows[] = {
        {"map_find", &parts->map_find},       {"map_insert", &parts->map_insert},
        {"map_erase", &parts->map_erase},     {"book_add", &parts->book_add},
        {"book_reduce", &parts->book_reduce}, {"book_remove", &parts->book_remove},
    };

    utsname host{};
    uname(&host);

    // --- JSON (stdout) ---
    std::printf("{\n");
    std::printf("  \"label\": \"%.*s\",\n", static_cast<int>(args.label.size()), args.label.data());
    const auto file_name = file_name_of(args.file);
    std::printf("  \"file\": \"%.*s\",\n", static_cast<int>(file_name.size()), file_name.data());
    std::printf("  \"machine\": \"%s %s\",\n", host.sysname, host.machine);
    std::printf("  \"messages\": %llu,\n", static_cast<unsigned long long>(messages));
    std::printf("  \"skipped\": %llu,\n", static_cast<unsigned long long>(parts->skipped));
    std::printf("  \"live_orders_at_end\": %u,\n", parts->pool.live());
    std::printf("  \"verified\": %s,\n", args.verify ? (verified ? "true" : "false") : "null");
    std::printf("  \"timer\": {\"source\": \"%s\", \"ns_per_tick\": %.4f, \"empty_pair\": ",
                ttt::core::tick_source(), ns_per_tick);
    json_stats(timer);
    std::printf("},\n  \"components\": {");
    bool first = true;
    for (const auto& [name, h] : rows) {
      std::printf("%s\n    \"%.*s\": ", first ? "" : ",", static_cast<int>(name.size()),
                  name.data());
      json_stats(*h);
      first = false;
    }
    std::printf("\n  }\n}\n");

    // --- summary (stderr) ---
    std::fprintf(stderr, "%s: %llu messages, skipped %llu, timer %s (%.2f ns/tick)\n", args.file,
                 static_cast<unsigned long long>(messages),
                 static_cast<unsigned long long>(parts->skipped), ttt::core::tick_source(),
                 ns_per_tick);
    std::fprintf(stderr, "  %-12s %12s %8s %7s %7s %7s %8s %10s   (ns)\n", "component", "count",
                 "mean", "p50", "p90", "p99", "p99.9", "max");
    table_row("timer", timer);
    for (const auto& [name, h] : rows) table_row(name, *h);

    return verified ? 0 : 1;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "error: %s\n", e.what());
    return 1;
  }
}
