#include "gateway/common/order_state.hpp"

#include <gtest/gtest.h>

#include <vector>

namespace ttt::gw {
namespace {

NewOrder order(Qty qty = 1000, Price px = 1'000'000, Tif tif = Tif::Day) {
  return {itch::Symbol::from("AAPL"), px, qty, Side::Buy, tif};
}

// creates an order and accepts it live with all shares open
OrderId live_order(OrderState& s, Qty qty = 1000) {
  const auto id = s.create(order(qty));
  EXPECT_TRUE(id);
  EXPECT_TRUE(s.on_accepted(*id, true, qty, 77));
  return *id;
}

// --- create ---------------------------------------------------------------------------

TEST(OrderState, CreateAssignsSequentialIdsInPendingNew) {
  OrderState s(4);
  EXPECT_EQ(s.create(order()), 0u);
  EXPECT_EQ(s.create(order(500)), 1u);

  const Order* o = s.find(1);
  ASSERT_NE(o, nullptr);
  EXPECT_EQ(o->status, OrderStatus::PendingNew);
  EXPECT_EQ(o->open_qty, 500u);
  EXPECT_EQ(o->cum_qty, 0u);
  EXPECT_EQ(o->gen, 0u);
  EXPECT_EQ(s.size(), 2u);
}

TEST(OrderState, CreateFailsAtCapacity) {
  OrderState s(2);
  EXPECT_TRUE(s.create(order()));
  EXPECT_TRUE(s.create(order()));
  EXPECT_FALSE(s.create(order())); // never reallocates on the hot path
  EXPECT_EQ(s.size(), 2u);
}

TEST(OrderState, FindUnknownIdIsNull) {
  OrderState s(2);
  EXPECT_EQ(s.find(0), nullptr);
}

// --- accept / reject ------------------------------------------------------------------

TEST(OrderState, AcceptedLiveGoesLive) {
  OrderState s(4);
  const auto id = *s.create(order());
  const auto ev = s.on_accepted(id, true, 1000, 42);

  ASSERT_TRUE(ev);
  EXPECT_EQ(ev->type, ExecType::Accepted);
  EXPECT_EQ(ev->status, OrderStatus::Live);
  EXPECT_EQ(ev->open_qty, 1000u);
  EXPECT_EQ(s.find(id)->exch_ref, 42u);
}

TEST(OrderState, AcceptedDeadIsCancelledWithNoOpenQty) {
  // IOC that didn't trade: accepted with state D
  OrderState s(4);
  const auto id = *s.create(order(1000, 1'000'000, Tif::Ioc));
  const auto ev = s.on_accepted(id, false, 1000, 1);

  ASSERT_TRUE(ev);
  EXPECT_EQ(ev->status, OrderStatus::Cancelled);
  EXPECT_EQ(ev->open_qty, 0u);
}

TEST(OrderState, DuplicateAcceptedIsIgnored) {
  OrderState s(4);
  const auto id = live_order(s);
  EXPECT_FALSE(s.on_accepted(id, true, 1000, 42));
  EXPECT_EQ(s.ignored(), 1u);
}

TEST(OrderState, EventForUnknownIdIsIgnored) {
  OrderState s(4);
  EXPECT_FALSE(s.on_accepted(3, true, 100, 1));
  EXPECT_FALSE(s.on_executed(3, 100, 1, 1));
  EXPECT_EQ(s.ignored(), 2u);
}

TEST(OrderState, RejectedNewOrder) {
  OrderState s(4);
  const auto id = *s.create(order());
  const auto ev = s.on_rejected(id, 0, 'S');

  ASSERT_TRUE(ev);
  EXPECT_EQ(ev->type, ExecType::Rejected);
  EXPECT_EQ(ev->status, OrderStatus::Rejected);
  EXPECT_EQ(ev->reason, 'S');
  EXPECT_EQ(ev->open_qty, 0u);
}

TEST(OrderState, RejectAfterLiveIsIgnored) {
  OrderState s(4);
  const auto id = live_order(s);
  EXPECT_FALSE(s.on_rejected(id, 0, 'S'));
  EXPECT_EQ(s.find(id)->status, OrderStatus::Live);
}

// --- executions -----------------------------------------------------------------------

TEST(OrderState, PartialThenFullFill) {
  OrderState s(4);
  const auto id = live_order(s, 300);

  auto ev = s.on_executed(id, 100, 1'000'100, 9);
  ASSERT_TRUE(ev);
  EXPECT_EQ(ev->type, ExecType::Executed);
  EXPECT_EQ(ev->status, OrderStatus::Live);
  EXPECT_EQ(ev->last_qty, 100u);
  EXPECT_EQ(ev->last_px, 1'000'100u);
  EXPECT_EQ(ev->match, 9u);
  EXPECT_EQ(ev->open_qty, 200u);
  EXPECT_EQ(ev->cum_qty, 100u);

  ev = s.on_executed(id, 200, 1'000'000, 10);
  ASSERT_TRUE(ev);
  EXPECT_EQ(ev->status, OrderStatus::Filled);
  EXPECT_EQ(ev->open_qty, 0u);
  EXPECT_EQ(ev->cum_qty, 300u);
}

TEST(OrderState, ExecutionBeforeAcceptIsIgnored) {
  OrderState s(4);
  const auto id = *s.create(order());
  EXPECT_FALSE(s.on_executed(id, 100, 1, 1));
  EXPECT_EQ(s.find(id)->cum_qty, 0u);
}

TEST(OrderState, OverfillIsClampedToOpenQty) {
  OrderState s(4);
  const auto id = live_order(s, 100);
  const auto ev = s.on_executed(id, 500, 1, 1);
  ASSERT_TRUE(ev);
  EXPECT_EQ(ev->last_qty, 100u);
  EXPECT_EQ(ev->cum_qty, 100u);
  EXPECT_EQ(ev->status, OrderStatus::Filled);
}

TEST(OrderState, EventsAfterTerminalAreIgnored) {
  OrderState s(4);
  const auto id = live_order(s, 100);
  ASSERT_TRUE(s.on_executed(id, 100, 1, 1));
  EXPECT_FALSE(s.on_executed(id, 100, 1, 2));
  EXPECT_FALSE(s.on_canceled(id, 100, 'U'));
  EXPECT_EQ(s.find(id)->cum_qty, 100u);
}

// --- cancel ---------------------------------------------------------------------------

TEST(OrderState, BeginCancelRequiresLiveAndSmallerRemaining) {
  OrderState s(4);
  const auto pending = *s.create(order());
  EXPECT_FALSE(s.begin_cancel(pending, 0)); // not acked yet

  const auto id = live_order(s, 1000);
  EXPECT_FALSE(s.begin_cancel(id, 1000)); // not a reduction
  EXPECT_TRUE(s.begin_cancel(id, 0));
  EXPECT_EQ(s.find(id)->status, OrderStatus::PendingCancel);
  EXPECT_FALSE(s.begin_cancel(id, 0)); // already pending
}

TEST(OrderState, FullCancel) {
  OrderState s(4);
  const auto id = live_order(s, 1000);
  ASSERT_TRUE(s.begin_cancel(id, 0));

  const auto ev = s.on_canceled(id, 1000, 'U');
  ASSERT_TRUE(ev);
  EXPECT_EQ(ev->type, ExecType::Canceled);
  EXPECT_EQ(ev->status, OrderStatus::Cancelled);
  EXPECT_EQ(ev->last_qty, 1000u);
  EXPECT_EQ(ev->open_qty, 0u);
}

TEST(OrderState, PartialCancelReturnsToLive) {
  OrderState s(4);
  const auto id = live_order(s, 1000);
  ASSERT_TRUE(s.begin_cancel(id, 400));

  const auto ev = s.on_canceled(id, 600, 'U');
  ASSERT_TRUE(ev);
  EXPECT_EQ(ev->status, OrderStatus::Live);
  EXPECT_EQ(ev->open_qty, 400u);
}

TEST(OrderState, UnsolicitedPartialCancelKeepsStatus) {
  OrderState s(4);
  const auto id = live_order(s, 1000);
  ASSERT_TRUE(s.begin_replace(id, 800, 5));

  const auto ev = s.on_canceled(id, 100, 'Q');
  ASSERT_TRUE(ev);
  EXPECT_EQ(ev->status, OrderStatus::PendingReplace);
  EXPECT_EQ(ev->open_qty, 900u);
}

TEST(OrderState, CancelRejectedReturnsToLive) {
  OrderState s(4);
  const auto id = live_order(s);
  ASSERT_TRUE(s.begin_cancel(id, 0));

  const auto ev = s.on_cancel_rejected(id);
  ASSERT_TRUE(ev);
  EXPECT_EQ(ev->type, ExecType::CancelRejected);
  EXPECT_EQ(ev->status, OrderStatus::Live);
  EXPECT_FALSE(s.on_cancel_rejected(id)); // no cancel pending anymore
}

TEST(OrderState, FullFillWinsRaceWithPendingCancel) {
  OrderState s(4);
  const auto id = live_order(s, 100);
  ASSERT_TRUE(s.begin_cancel(id, 0));

  const auto ev = s.on_executed(id, 100, 1, 1);
  ASSERT_TRUE(ev);
  EXPECT_EQ(ev->status, OrderStatus::Filled);
  EXPECT_FALSE(s.on_canceled(id, 100, 'U')); // too late
}

TEST(OrderState, PartialFillDuringPendingCancelStaysPending) {
  OrderState s(4);
  const auto id = live_order(s, 1000);
  ASSERT_TRUE(s.begin_cancel(id, 0));

  auto ev = s.on_executed(id, 100, 1, 1);
  ASSERT_TRUE(ev);
  EXPECT_EQ(ev->status, OrderStatus::PendingCancel);

  ev = s.on_canceled(id, 900, 'U');
  ASSERT_TRUE(ev);
  EXPECT_EQ(ev->status, OrderStatus::Cancelled);
  EXPECT_EQ(ev->cum_qty, 100u);
}

// --- replace --------------------------------------------------------------------------

TEST(OrderState, ReplaceBumpsGenerationAndPrice) {
  OrderState s(4);
  const auto id = live_order(s, 1000);
  ASSERT_TRUE(s.begin_replace(id, 500, 2'000'000));
  EXPECT_EQ(s.find(id)->status, OrderStatus::PendingReplace);
  EXPECT_FALSE(s.begin_replace(id, 400, 1)); // one replace in flight at a time

  const auto ev = s.on_replaced(id, 1, true, 500, 2'000'000);
  ASSERT_TRUE(ev);
  EXPECT_EQ(ev->type, ExecType::Replaced);
  EXPECT_EQ(ev->status, OrderStatus::Live);
  EXPECT_EQ(ev->open_qty, 500u);

  const Order* o = s.find(id);
  EXPECT_EQ(o->gen, 1u);
  EXPECT_EQ(o->price, 2'000'000u);
  EXPECT_EQ(o->req.price, 1'000'000u); // original request kept
}

TEST(OrderState, ReplacedWithWrongGenerationIsIgnored) {
  OrderState s(4);
  const auto id = live_order(s);
  ASSERT_TRUE(s.begin_replace(id, 500, 5));
  EXPECT_FALSE(s.on_replaced(id, 2, true, 500, 5));
  EXPECT_FALSE(s.on_replaced(id, 0, true, 500, 5));
  EXPECT_EQ(s.find(id)->status, OrderStatus::PendingReplace);
}

TEST(OrderState, ReplacedDeadIsCancelled) {
  OrderState s(4);
  const auto id = live_order(s);
  ASSERT_TRUE(s.begin_replace(id, 0, 5));
  const auto ev = s.on_replaced(id, 1, false, 0, 5);
  ASSERT_TRUE(ev);
  EXPECT_EQ(ev->status, OrderStatus::Cancelled);
}

TEST(OrderState, ReplaceRejectedKeepsOriginalLive) {
  OrderState s(4);
  const auto id = live_order(s, 1000);
  ASSERT_TRUE(s.begin_replace(id, 500, 5));

  const auto ev = s.on_rejected(id, 1, 'O'); // reject of the replacement token
  ASSERT_TRUE(ev);
  EXPECT_EQ(ev->type, ExecType::ReplaceRejected);
  EXPECT_EQ(ev->status, OrderStatus::Live);
  EXPECT_EQ(ev->open_qty, 1000u);
  EXPECT_EQ(s.find(id)->gen, 0u);
  EXPECT_EQ(s.find(id)->price, 1'000'000u);
}

// --- pending iteration ----------------------------------------------------------------

TEST(OrderState, ForEachPendingVisitsOnlyInFlightRequests) {
  OrderState s(8);
  const auto pnew = *s.create(order());
  const auto live = live_order(s);
  const auto pcancel = live_order(s);
  const auto preplace = live_order(s);
  const auto filled = live_order(s, 100);
  ASSERT_TRUE(s.begin_cancel(pcancel, 0));
  ASSERT_TRUE(s.begin_replace(preplace, 10, 10));
  ASSERT_TRUE(s.on_executed(filled, 100, 1, 1));

  std::vector<OrderId> seen;
  s.for_each_pending([&](OrderId id, const Order&) { seen.push_back(id); });
  EXPECT_EQ(seen, (std::vector<OrderId>{pnew, pcancel, preplace}));
  (void)live;
}

} // namespace
} // namespace ttt::gw
