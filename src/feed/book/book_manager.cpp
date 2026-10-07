#include "feed/book/book_manager.hpp"
#include "feed/book/types.hpp"
#include "feed/itch/messages.hpp"

namespace ttt::book {

namespace {
// Peak live orders on a full day is expected to be ~2M; reserve so map never rehashes mid-day
constexpr std::size_t kExpectedLiveOrders = 4'000'000;
} // namespace

BookManager::BookManager() : pool_(kMaxOrders), map_(kMapSlots) {}

void BookManager::on(const itch::SystemEvent& m) {
  if (m.event_code() == 'Q') market_open_ = true;
  if (m.event_code() == 'M') market_open_ = false;
}

void BookManager::on(const itch::StockDirectory& m) {
  const uint16_t locate = m.stock_locate();
  if (locate >= books_.size()) {
    // resize is ok here because 'R' messages usually come before trading
    books_.resize(locate + 1, OrderBook(&pool_));
    trading_state_.resize(locate + 1, 0);
    reopening_.resize(locate + 1, 0);
  }
  // announce first, state unknown
  if (trading_state_[locate] == 0) trading_state_[locate] = ' ';
}

void BookManager::on(const itch::StockTradingAction& m) {
  const uint16_t locate = m.stock_locate();
  if (locate >= trading_state_.size() || trading_state_[locate] == 0) {
    ++errors_.unknown_locate;
    return;
  }
  const char state = m.trading_state();
  // Halted -> trading: a reopening cross follows and uncrosses the book
  if (state == 'T' && trading_state_[locate] != 'T') reopening_[locate] = 1;
  trading_state_[locate] = state;
}

// --- order messages -----------------------------------------------------

void BookManager::on(const itch::AddOrder& m) {
  check_crossed(add(m.order_ref(), m.stock_locate(), to_side(m.side()), m.price(), m.shares()));
}

void BookManager::on(const itch::AddOrderMpid& m) {
  // mpid attribute does not affect book
  on(static_cast<const itch::AddOrder&>(m));
}

void BookManager::on(const itch::OrderExecuted& m) {
  check_crossed(reduce(m.order_ref(), m.executed_shares()));
}

void BookManager::on(const itch::OrderExecutedWithPrice& m) {
  // Reduces the order at its resting price; execution price does not matter to book
  check_crossed(reduce(m.order_ref(), m.executed_shares()));
}

void BookManager::on(const itch::OrderCancel& m) {
  check_crossed(reduce(m.order_ref(), m.cancelled_shares()));
}

void BookManager::on(const itch::OrderDelete& m) { check_crossed(remove(m.order_ref())); }

void BookManager::on(const itch::OrderReplace& m) {
  const uint32_t old_idx = map_.find(m.original_order_ref());
  if (old_idx == kNil) {
    ++errors_.unknown_ref;
    return;
  }
  // 'U' has no side: the replcaement inherits side and symbol from original
  // then goes to back of the queue at new price
  const uint16_t locate = pool_[old_idx].locate;
  const Side side = pool_[old_idx].side;
  remove(m.original_order_ref());
  check_crossed(add(m.new_order_ref(), locate, side, m.price(), m.shares()));
}

// --- helpers --------------------------------------------------------

const OrderBook* BookManager::book(uint16_t locate) const {
  if (locate >= books_.size() || trading_state_[locate] == 0) return nullptr;
  return &books_[locate];
}

uint16_t BookManager::add(OrderRef ref, uint16_t locate, Side side, Price px, Qty qty) {
  if (locate >= books_.size() || trading_state_[locate] == 0) {
    ++errors_.unknown_locate;
    return kNoLocate;
  }
  const uint32_t idx = pool_.alloc();
  if (idx == kNil) {
    ++errors_.pool_full;
    return kNoLocate;
  }
  if (!map_.insert(ref, idx)) {
    pool_.free(idx);
    ++errors_.duplicate_ref;
    return kNoLocate;
  }
  Order& o = pool_[idx];
  o.ref = ref;
  o.locate = locate;
  o.price = px;
  o.qty = qty;
  o.side = side;
  books_[locate].add(idx);
  return locate;
}

uint16_t BookManager::reduce(OrderRef ref, Qty qty) {
  const uint32_t idx = map_.find(ref);
  if (idx == kNil) {
    ++errors_.unknown_ref;
    return kNoLocate;
  }
  Order& o = pool_[idx];
  // clamp if over_reduce
  if (qty > o.qty) {
    ++errors_.over_reduce;
    qty = o.qty;
  }
  if (qty == o.qty) return remove(ref);
  books_[o.locate].reduce(idx, qty);
  return o.locate;
}

uint16_t BookManager::remove(OrderRef ref) {
  const uint32_t idx = map_.erase(ref);
  if (idx == kNil) {
    ++errors_.unknown_ref;
    return kNoLocate;
  }
  const uint16_t locate = pool_[idx].locate;
  books_[locate].remove(idx);
  pool_.free(idx);
  return locate;
}

// During market hours a trading symbol's book must no be crossed (bid > ask).
// Exception: right after a halt ends, the reopening cross is still executing,
// allow it until book first becomes uncrossed
void BookManager::check_crossed(uint16_t locate) {
  if (locate == kNoLocate || !market_open_ || trading_state_[locate] != 'T') return;
  if (!books_[locate].crossed()) {
    reopening_[locate] = 0;
    return;
  }
  if (!reopening_[locate]) ++errors_.crossed_while_trading;
}

} // namespace ttt::book
