// OuchGateway against an in-process MockExchange over loopback TCP.
// Both sides are polled from this one thread, so no locking is needed and the
// interleaving is driven by the test.

#include "apps/mock_exchange/exchange.hpp"
#include "core/async_logger.hpp"
#include "core/clock.hpp"
#include "gateway/common/order_gateway.hpp"
#include "gateway/ouch/ouch_gateway.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <set>
#include <vector>

namespace ttt::ouch {
namespace {

static_assert(gw::OrderGateway<OuchGateway>);

using mock_exchange::ExchangeConfig;
using mock_exchange::MockExchange;

uint64_t now_ns() { return core::ticks_to_ns(core::read_ticks()); }

struct Sink {
  std::vector<gw::ExecEvent> events;
  void on_exec(const gw::ExecEvent& e) { events.push_back(e); }

  std::size_t count(gw::ExecType t) const {
    return static_cast<std::size_t>(
        std::count_if(events.begin(), events.end(), [&](const auto& e) { return e.type == t; }));
  }
};

class GatewaySession : public ::testing::Test {
protected:
  // each test gets its own port so ctest -j can run them side by side
  void start(uint16_t port, ExchangeConfig ex_cfg = {}, bool good_password = true) {
    ex_cfg.port = port;
    ex_ = std::make_unique<MockExchange>(ex_cfg);

    OuchConfig cfg;
    cfg.soup.port = port;
    cfg.soup.user = ex_cfg.user;
    cfg.soup.password = ex_cfg.password;
    if (!good_password) cfg.soup.password[0] = 'X';
    cfg.soup.backoff_min_ns = 1'000'000; // reconnect fast
    cfg.soup.backoff_max_ns = 10'000'000;
    cfg.max_orders = 64;
    gw_ = std::make_unique<OuchGateway>(cfg, log_);
  }

  void poll_once() {
    gw_->poll(sink_);
    ex_->poll(now_ns());
  }

  template <class Pred>
  bool run_until(Pred done, std::chrono::milliseconds timeout = std::chrono::seconds(3)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!done()) {
      if (std::chrono::steady_clock::now() > deadline) return false;
      poll_once();
    }
    return true;
  }

  bool connect() {
    return run_until([&] { return gw_->connected(); });
  }

  gw::OrderStatus status(gw::OrderId id) const { return gw_->orders().find(id)->status; }

  static gw::NewOrder buy(gw::Qty qty, gw::Tif tif = gw::Tif::Day) {
    return {itch::Symbol::from("AAPL"), 1'500'000, qty, gw::Side::Buy, tif};
  }

  // no logger thread: records are queued and dropped when full
  core::LogChannel log_{"test", 1024};
  std::unique_ptr<MockExchange> ex_;
  std::unique_ptr<OuchGateway> gw_;
  Sink sink_;
};

TEST_F(GatewaySession, SendFailsWhileDisconnected) {
  start(41101);
  EXPECT_FALSE(gw_->connected());
  EXPECT_FALSE(gw_->send_new(buy(100)));
  EXPECT_EQ(gw_->orders().size(), 0u);
}

TEST_F(GatewaySession, OrderIsAcceptedAndFilled) {
  ExchangeConfig ex;
  ex.fill_qty = 100;
  ex.fill_every_ns = 1'000'000;
  start(41102, ex);
  ASSERT_TRUE(connect());

  const auto id = gw_->send_new(buy(300));
  ASSERT_TRUE(id);
  ASSERT_TRUE(run_until([&] { return status(*id) == gw::OrderStatus::Filled; }));

  ASSERT_EQ(sink_.events.size(), 4u);
  EXPECT_EQ(sink_.events[0].type, gw::ExecType::Accepted);
  for (std::size_t i = 1; i < 4; ++i) {
    EXPECT_EQ(sink_.events[i].type, gw::ExecType::Executed);
    EXPECT_EQ(sink_.events[i].last_qty, 100u);
    EXPECT_EQ(sink_.events[i].cum_qty, 100u * i);
  }
  EXPECT_EQ(gw_->orders().find(*id)->cum_qty, 300u);
  EXPECT_EQ(ex_->position(itch::Symbol::from("AAPL")), 300);
  EXPECT_EQ(ex_->executions(), 3u);
}

TEST_F(GatewaySession, IocIsAcceptedDead) {
  start(41103);
  ASSERT_TRUE(connect());

  const auto id = gw_->send_new(buy(100, gw::Tif::Ioc));
  ASSERT_TRUE(id);
  ASSERT_TRUE(run_until([&] { return status(*id) == gw::OrderStatus::Cancelled; }));
  EXPECT_EQ(sink_.count(gw::ExecType::Accepted), 1u);
  EXPECT_EQ(gw_->orders().find(*id)->open_qty, 0u);
  EXPECT_EQ(ex_->executions(), 0u);
}

TEST_F(GatewaySession, PartialThenFullCancel) {
  ExchangeConfig ex;
  ex.fill_every_ns = 60'000'000'000; // never fills during the test
  start(41104, ex);
  ASSERT_TRUE(connect());

  const auto id = gw_->send_new(buy(1000));
  ASSERT_TRUE(id);
  ASSERT_TRUE(run_until([&] { return status(*id) == gw::OrderStatus::Live; }));

  ASSERT_TRUE(gw_->send_cancel(*id, 400));
  EXPECT_EQ(status(*id), gw::OrderStatus::PendingCancel);
  ASSERT_TRUE(run_until([&] { return status(*id) == gw::OrderStatus::Live; }));
  EXPECT_EQ(gw_->orders().find(*id)->open_qty, 400u);
  EXPECT_EQ(ex_->order(gw_->token(*id))->open, 400u);

  ASSERT_TRUE(gw_->send_cancel(*id));
  ASSERT_TRUE(run_until([&] { return status(*id) == gw::OrderStatus::Cancelled; }));
  EXPECT_FALSE(ex_->order(gw_->token(*id))->live);
  EXPECT_EQ(sink_.count(gw::ExecType::Canceled), 2u);
}

TEST_F(GatewaySession, ReplaceMovesToNewToken) {
  ExchangeConfig ex;
  ex.fill_every_ns = 60'000'000'000;
  start(41105, ex);
  ASSERT_TRUE(connect());

  const auto id = gw_->send_new(buy(1000));
  ASSERT_TRUE(id);
  ASSERT_TRUE(run_until([&] { return status(*id) == gw::OrderStatus::Live; }));
  const Token old_tok = gw_->token(*id);

  ASSERT_TRUE(gw_->send_replace(*id, 500, 1'490'000));
  ASSERT_TRUE(run_until([&] { return sink_.count(gw::ExecType::Replaced) == 1; }));

  const gw::Order* o = gw_->orders().find(*id);
  EXPECT_EQ(o->status, gw::OrderStatus::Live);
  EXPECT_EQ(o->gen, 1u);
  EXPECT_EQ(o->price, 1'490'000u);
  EXPECT_EQ(o->open_qty, 500u);

  const auto* ex_new = ex_->order(gw_->token(*id));
  ASSERT_NE(ex_new, nullptr);
  EXPECT_TRUE(ex_new->live);
  EXPECT_EQ(ex_new->open, 500u);
  EXPECT_EQ(ex_new->price, 1'490'000u);
  EXPECT_FALSE(ex_->order(old_tok)->live);
}

TEST_F(GatewaySession, BadPasswordIsRejectedForGood) {
  start(41106, {}, /*good_password=*/false);
  ASSERT_TRUE(run_until([&] { return gw_->session().state() == soup::SoupState::Rejected; }));
  EXPECT_FALSE(gw_->connected());

  // terminal: no reconnect attempts afterwards
  const auto connects = gw_->session().stats().connects;
  for (int i = 0; i < 1000; ++i) poll_once();
  EXPECT_EQ(gw_->session().stats().connects, connects);
}

// kill the link mid-session, reconnect, and receive every
// missed execution exactly once with the right final order state.
TEST_F(GatewaySession, ReconnectReplaysMissedExecutionsExactlyOnce) {
  ExchangeConfig ex;
  ex.fill_qty = 100;
  ex.fill_every_ns = 2'000'000;
  ex.kill_after = 3; // Accepted + 2 fills, then the exchange drops us
  start(41107, ex);
  ASSERT_TRUE(connect());

  const auto id = gw_->send_new(buy(1000));
  ASSERT_TRUE(id);
  ASSERT_TRUE(run_until([&] { return status(*id) == gw::OrderStatus::Filled; }));

  const auto& st = gw_->session().stats();
  EXPECT_EQ(st.disconnects, 1u);
  EXPECT_EQ(st.connects, 2u);

  // every execution seen once: match numbers are 1..N with no gaps or repeats
  std::vector<uint64_t> matches;
  for (const auto& e : sink_.events) {
    if (e.type == gw::ExecType::Executed) matches.push_back(e.match);
  }
  ASSERT_EQ(matches.size(), 10u);
  EXPECT_EQ(std::set<uint64_t>(matches.begin(), matches.end()).size(), matches.size());
  EXPECT_EQ(*std::min_element(matches.begin(), matches.end()), 1u);
  EXPECT_EQ(*std::max_element(matches.begin(), matches.end()), 10u);

  // reconciles with the exchange's records
  EXPECT_EQ(gw_->orders().find(*id)->cum_qty, 1000u);
  EXPECT_EQ(ex_->executions(), 10u);
  EXPECT_EQ(ex_->position(itch::Symbol::from("AAPL")), 1000);
  EXPECT_EQ(gw_->session().next_seq(), ex_->sequenced() + 1);
  EXPECT_EQ(gw_->orders().ignored(), 0u);
}

// An order sent right before the link drops is resent after login. Whether the
// exchange saw the first copy or not, it ends up with exactly one order.
TEST_F(GatewaySession, PendingNewIsResentButNotDuplicated) {
  ExchangeConfig ex;
  ex.fill_qty = 100;
  ex.fill_every_ns = 1'000'000;
  start(41108, ex);
  ASSERT_TRUE(connect());

  const auto id = gw_->send_new(buy(200));
  ASSERT_TRUE(id);
  ex_->kill(); // drops before anything is sent back
  ex_->poll(now_ns());

  ASSERT_TRUE(run_until([&] { return status(*id) == gw::OrderStatus::Filled; }));
  EXPECT_EQ(gw_->stats().resent, 1u);
  EXPECT_EQ(gw_->session().stats().disconnects, 1u);
  EXPECT_EQ(sink_.count(gw::ExecType::Accepted), 1u);
  EXPECT_EQ(sink_.count(gw::ExecType::Executed), 2u);
  EXPECT_EQ(ex_->executions(), 2u);
  EXPECT_EQ(ex_->position(itch::Symbol::from("AAPL")), 200);
}

} // namespace
} // namespace ttt::ouch
