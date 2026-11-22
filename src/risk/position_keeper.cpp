#include "risk/position_keeper.hpp"

#include "feed/itch/messages.hpp"

#include <cinttypes> // PRId64
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>

namespace ttt::risk {

std::optional<uint16_t> PositionKeeper::add_symbol(itch::Symbol s) {
  if (const auto existing = slot_of(s)) return existing;
  if (syms_.size() == max_symbols_) return std::nullopt;
  syms_.push_back({.symbol = s});
  return static_cast<uint16_t>(syms_.size() - 1);
}

std::optional<uint16_t> PositionKeeper::slot_of(itch::Symbol s) const {
  for (uint16_t i = 0; i < syms_.size(); ++i) {
    if (syms_[i].symbol.key() == s.key()) return i;
  }
  return std::nullopt;
}

bool PositionKeeper::no_exposure() const {
  for (const auto& p : syms_) {
    if (p.open_buy || p.open_sell) return false;
  }
  return true;
}

void PositionKeeper::print(std::FILE* out) const {
  for (const auto& p : syms_) {
    char sym[9]{};
    std::memcpy(sym, p.symbol.raw.data(), 8);
    std::fprintf(out, " %s %+" PRId64 "\n", sym, p.position);
  }
}

}
