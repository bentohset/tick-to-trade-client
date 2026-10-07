#pragma once

#include <cstdint>

namespace ttt::book {

using Price = uint32_t;
using Qty = uint32_t;
using OrderRef = uint64_t;
enum class Side : uint8_t { Buy, Sell };

inline Side to_side(char c) { return c == 'B' ? Side::Buy : Side::Sell; }

// what a snapshot / BookUpdate reports
struct Level {
  Price price;
  uint64_t qty;    // sum of shares at this price
  uint32_t orders; // number of orders at this price
};

// Data inconsistencies seen while applying ITCH messages to book
struct Errors {
  uint64_t unknown_ref = 0;           // E/C/X/D/U for a non-existent order
  uint64_t duplicate_ref = 0;         // A/F/U adds a ref thats already live
  uint64_t over_reduce = 0;           // E/C/X removes more shares than order has left
  uint64_t unknown_locate = 0;        // order message for locate with no 'R' message
  uint64_t crossed_while_trading = 0; // bid > ask while state 'T', outside a reopening cross
  uint64_t pool_full = 0;             // add dropped

  uint64_t total() const {
    return unknown_ref + duplicate_ref + over_reduce + unknown_locate + crossed_while_trading +
           pool_full;
  }
};

} // namespace ttt::book
