#include "feed/book/book_manager.hpp"
#include "feed/book/types.hpp"
#include "feed/itch/messages.hpp"

namespace ttt::book {

namespace {
// Peak live orders on a full day is expected to be ~2M; reserve so map never rehashes mid-day
constexpr std::size_t kExpectedLiveOrders = 4'000'000;
} // namespace

BookManager::BookManager() { orders_.reserve(kExpectedLiveOrders); }

void BookManager::on(const itch::SystemEvent& m) {
  if (m.event_code() == 'Q') market_open_ = true;
  if (m.event_code() == 'M') market_open_ = false;
}

void BookManager::on(const itch::StockDirectory& m) {
  const uint16_t locate = m.stock_locate();
  if (locate >= books_.size()) {
    books_.resize(locate + 1);
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
  const auto it = orders_.find(m.original_order_ref());
  if (it == orders_.end()) {
    ++errors_.unknown_ref;
    return;
  }
  // 'U' has no side: the replcaement inherits side and symbol from original
  // then goes to back of the queue at new price
  const uint16_t locate = it->second.locate;
  const Side side = it->second.side;
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
  const auto [it, inserted] = orders_.try_emplace(ref, Order{locate, side, px, qty});
  if (!inserted) {
    ++errors_.duplicate_ref;
    return kNoLocate;
  }
  books_[locate].add(side, px, qty);
  return locate;
}

uint16_t BookManager::reduce(OrderRef ref, Qty qty) {
  const auto it = orders_.find(ref);
  if (it == orders_.end()) {
    ++errors_.unknown_ref;
    return kNoLocate;
  }
  Order& o = it->second;
  // clamp if over_reduce
  if (qty > o.qty) {
    ++errors_.over_reduce;
    qty = o.qty;
  }
  const uint16_t locate = o.locate;
  const bool removes = qty == o.qty;
  books_[locate].reduce(o.side, o.price, qty, removes);
  if (removes) {
    orders_.erase(it);
  } else {
    o.qty -= qty;
  }
  return locate;
}

uint16_t BookManager::remove(OrderRef ref) {
  const auto it = orders_.find(ref);
  if (it == orders_.end()) {
    ++errors_.unknown_ref;
    return kNoLocate;
  }
  // copy the order before erasing it; 4 small fields is cheap
  const Order o = it->second;
  books_[o.locate].reduce(o.side, o.price, o.qty, true);
  orders_.erase(it);
  return o.locate;
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
