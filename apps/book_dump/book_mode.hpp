#pragma once

#include "apps/common/book_print.hpp"
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
    if (!apps::print_check(run.books.errors(), run.books.live_orders(), applied, read_whole_file))
      status = 1;
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
    apps::print_book(o.symbol, *locate, *book, o.depth);
  }
  return status;
}

} // namespace book_dump
