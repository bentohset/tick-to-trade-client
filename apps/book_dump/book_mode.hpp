#pragma once

#include "core/format.hpp"
#include "core/mapped_file.hpp"
#include "feed/book/book_manager.hpp"
#include "feed/book/order_book.hpp"
#include "feed/itch/framing.hpp"
#include "feed/itch/parser.hpp"
#include "feed/itch/symbol_directory.hpp"
#include "options.hpp"
namespace book_dump {

namespace detail {

// Feeds every message to symbol dir and books. Both derive from NullHandler so each has
// on() for every type.
struct BookRun : ttt::itch::NullHandler {
  ttt::itch::SymbolDirectory dir;
  ttt::book::BookManager books;

  template <class Msg> void on(const Msg& m) {
    dir.on(m);
    books.on(m);
  }
};

inline void print_side(const ttt::book::Level& level) {
  ttt::core::print_price(stdout, level.price);
  std::printf(" %10llu (%4u)", static_cast<unsigned long long>(level.qty), level.orders);
}

inline void print_book(std::string_view symbol, uint16_t locate, const ttt::book::OrderBook& book,
                       std::size_t depth, std::optional<uint64_t> at_ns) {
  std::printf("%.*s (locate %u) ", static_cast<int>(symbol.size()), symbol.data(), locate);
  if (at_ns) {
    std::printf("as of ");
    ttt::core::print_time_of_day(stdout, *at_ns);
    std::printf("\n");
  } else {
    std::printf("at end of file\n");
  }

  const auto bids = book.depth(ttt::book::Side::Buy, depth);
  const auto asks = book.depth(ttt::book::Side::Sell, depth);
  if (bids.empty() && asks.empty()) {
    std::printf("  (empty book)\n");
    return;
  }

  // Column width matches print_side() for prices up to 99999.9999
  constexpr int kWidth = 28;
  std::printf("  %-*s | %s\n", kWidth, "BID   qty (orders)", "ASK   qty (orders)");
  for (std::size_t i = 0; i < std::max(bids.size(), asks.size()); ++i) {
    std::printf("  ");
    if (i < bids.size()) {
      print_side(bids[i]);
    } else {
      std::printf("%*s", kWidth, "");
    }
    std::printf(" | ");
    if (i < asks.size()) print_side(asks[i]);
    std::printf("\n");
  }
}

// Returns true if everything is clean
inline bool print_check(const ttt::book::BookManager& books, uint64_t applied,
                        bool read_whole_file) {
  const auto& e = books.errors();
  const auto row = [](const char* name, uint64_t v) {
    std::printf("%-22s %llu\n", name, static_cast<unsigned long long>(v));
  };
  row("messages applied", applied);
  row("unknown ref", e.unknown_ref);
  row("duplicate ref", e.duplicate_ref);
  row("over reduce", e.over_reduce);
  row("unknown locate", e.unknown_locate);
  row("crossed while trading", e.crossed_while_trading);
  row("live orders", books.live_orders());

  bool ok = e.total() == 0;
  // Every order is removed by end of day; only meaningful if we read to the end
  if (read_whole_file && books.live_orders() != 0) ok = false;
  std::printf("%-22s %s\n", "result", ok ? "OK" : "FAILED");
  return ok;
}

} // namespace detail

inline int run_book(const Options& o, const ttt::core::MappedFile& file) {
  namespace itch = ttt::itch;
  itch::FrameReader frames(file.bytes());
  detail::BookRun run;
  uint64_t applied = 0;
  bool stopped_at_time = false;

  for (auto msg = frames.next(); !msg.empty(); msg = frames.next()) {
    // --at: the book as of a time (every msg before it)
    if (o.at_ns && itch::Header{msg.data()}.timestamp_ns() >= *o.at_ns) {
      stopped_at_time = true;
      break;
    }
    if (itch::parse(msg, run) == itch::ParseResult::BadLength) {
      std::fprintf(stderr, "error: bad length %zu for type '%c' at offset %zu\n", msg.size(),
                   static_cast<char>(msg[0]), frames.offset() - msg.size() - 2);
      return 1;
    }
    ++applied;
  }

  int status = 0;
  if (o.check) {
    const bool read_whole_file = !stopped_at_time && !frames.truncated();
    if (!detail::print_check(run.books, applied, read_whole_file)) status = 1;
  }

  if (!o.symbol.empty()) {
    if (o.check) std::printf("\n");
    const auto locate = run.dir.locate_of(o.symbol);
    const ttt::book::OrderBook* book = locate ? run.books.book(*locate) : nullptr;
    if (!book) {
      std::fprintf(stderr, "error: symbol %.*s not found (no 'R' message before this point)\n",
                   static_cast<int>(o.symbol.size()), o.symbol.data());
      return 1;
    }
    detail::print_book(o.symbol, *locate, *book, o.depth, o.at_ns);
  }
  return status;
}

} // namespace book_dump
