#pragma once

#include "feed/itch/messages.hpp"
#include "feed/itch/parser.hpp"
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ttt::itch {

struct StockInfo {
  Symbol symbol{};
  uint32_t round_lot = 0;
  char market_category = ' ';
  bool is_test = false;
  bool valid = false;
};

// The lookup table built from R messages. Maps locate code -> symbol
class SymbolDirectory : public NullHandler {
public:
  using NullHandler::on;
  void on(const StockDirectory& m) {
    const uint16_t locate = m.stock_locate();
    if (locate >= by_locate_.size()) by_locate_.resize(locate + 1);
    StockInfo& info = by_locate_[locate];
    info = {m.stock(), m.round_lot_size(), m.market_category(), m.authenticity() == 'T', true};
    by_symbol_[info.symbol.key()] = locate;
  }

  // locate -> info
  const StockInfo* find(uint16_t locate) const noexcept {
    if (locate >= by_locate_.size() || !by_locate_[locate].valid) return nullptr;
    return &by_locate_[locate];
  }

  // "AAPL" -> locate
  std::optional<uint16_t> locate_of(std::string_view name) const {
    const auto it = by_symbol_.find(Symbol::from(name).key());
    if (it == by_symbol_.end()) return std::nullopt;
    return it->second;
  }

  std::size_t size() const noexcept { return by_symbol_.size(); }

private:
  std::vector<StockInfo> by_locate_;                 // index = locate
  std::unordered_map<uint64_t, uint16_t> by_symbol_; // symbolkey -> locate
};

} // namespace ttt::itch
