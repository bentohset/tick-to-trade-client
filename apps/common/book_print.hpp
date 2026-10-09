#pragma once

#include "core/format.hpp"
#include "feed/book/order_book.hpp"
#include "feed/book/types.hpp"

#include <algorithm>
#include <cstdio>
#include <string_view>

namespace apps {

inline void print_side(const ttt::book::Level& level) {
  ttt::core::print_price(stdout, level.price);
  std::printf(" %10llu (%4u)", static_cast<unsigned long long>(level.qty), level.orders);
}

inline void print_book(std::string_view symbol, uint16_t locate, const ttt::book::OrderBook& book,
                       std::size_t depth) {
  std::printf("%.*s (locate %u)\n", static_cast<int>(symbol.size()), symbol.data(), locate);
  const auto bids = book.depth(ttt::book::Side::Buy, depth);
  const auto asks = book.depth(ttt::book::Side::Sell, depth);
  if (bids.empty() && asks.empty()) {
    std::printf("  (empty book)\n");
    return;
  }

  constexpr int kWidth = 28;
  std::printf("  %-*s | %s\n", kWidth, "BID   qty (orders)", "ASK   qty (orders)");
  for (std::size_t i = 0; i < std::max(bids.size(), asks.size()); ++i) {
    std::printf("  ");
    if (i < bids.size())
      print_side(bids[i]);
    else
      std::printf("%*s", kWidth, "");
    std::printf(" | ");
    if (i < asks.size()) print_side(asks[i]);
    std::printf("\n");
  }
}

// Returns true if every error counter is 0 and, when told the replay covered the
// whole day, no orders are left live. Mirrors book_dump's print_check exactly.
inline bool print_check(const ttt::book::Errors& e, uint64_t live_orders, uint64_t applied,
                        bool expect_fully_drained) {
  const auto row = [](const char* name, uint64_t v) {
    std::printf("%-22s %llu\n", name, static_cast<unsigned long long>(v));
  };
  row("messages applied", applied);
  row("unknown ref", e.unknown_ref);
  row("duplicate ref", e.duplicate_ref);
  row("over reduce", e.over_reduce);
  row("unknown locate", e.unknown_locate);
  row("crossed while trading", e.crossed_while_trading);
  row("live orders", live_orders);

  bool ok = e.total() == 0;
  if (expect_fully_drained && live_orders != 0) ok = false;
  std::printf("%-22s %s\n", "result", ok ? "OK" : "FAILED");
  return ok;
}

} // namespace apps
