#pragma once

#include "feed/book/order_book.hpp"
#include "feed/book/order_map.hpp"
#include "feed/book/order_pool.hpp"
#include "feed/book/types.hpp"
#include "feed/itch/messages.hpp"
#include "feed/itch/parser.hpp"
#include <unordered_map>
#include <vector>

namespace ttt::book {

class BookManager : public itch::NullHandler {
public:
  using NullHandler::on; // only non-template overloads are added

  // Sized from day average 2M live orders at peak
  static constexpr uint32_t kMaxOrders = 4'000'000;              // pool 128MB
  static constexpr std::size_t kMapSlots = std::size_t{1} << 23; // 8M slots, 128MB, load <= 0.24

  BookManager(); // reserves the order map

  void on(const itch::SystemEvent& m); // market open/close for crossed check
  void on(const itch::StockDirectory& m);
  void on(const itch::StockTradingAction& m);
  void on(const itch::AddOrder& m);
  void on(const itch::AddOrderMpid& m);
  void on(const itch::OrderExecuted& m);
  void on(const itch::OrderExecutedWithPrice& m);
  void on(const itch::OrderCancel& m);
  void on(const itch::OrderDelete& m);
  void on(const itch::OrderReplace& m);

  const OrderBook* book(uint16_t locate) const;

  std::size_t live_orders() const { return pool_.live(); }
  const Errors& errors() const { return errors_; }

private:
  static constexpr uint16_t kNoLocate = 0; // ITCH locates start at 1

  uint16_t add(OrderRef ref, uint16_t locate, Side side, Price px, Qty qty);
  uint16_t reduce(OrderRef ref, Qty qty); // qty < order.qty -> srhink; == -> remove
  uint16_t remove(OrderRef ref);
  void check_crossed(uint16_t locate);

  OrderPool pool_;
  OrderMap map_;
  std::vector<OrderBook> books_;    // index = stock locate
  std::vector<char> trading_state_; // index = stock locate
  // index = stock locate; 1 = resumed after a halt, not yet uncrossed
  std::vector<uint8_t> reopening_;

  bool market_open_{false};
  Errors errors_;
};

} // namespace ttt::book
