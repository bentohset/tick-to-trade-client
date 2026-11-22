#include "feed/feed_handler.hpp"

#include "core/async_logger.hpp"
#include "core/clock.hpp"
#include "feed/moldudp64/wire.hpp"
#include "utils/itch_builder.hpp"
#include "utils/mold_builder.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <deque>
#include <span>
#include <vector>

namespace ttt::feed {
namespace {

using test::mold_session;

// In-memory network: tests queue packets to "arrive" and inspect what was sent
struct FakeIo {
  std::deque<std::vector<std::byte>> feed;        // multicast
  std::deque<std::vector<std::byte>> retransmits; // answers to re-requests
  std::vector<std::vector<std::byte>> requests;   // re-requests the handler sent

  std::size_t recv_feed(std::span<std::byte> b) { return pop(feed, b); }
  std::size_t recv_rerequest(std::span<std::byte> b) { return pop(retransmits, b); }
  void send_rerequest(std::span<const std::byte> r) { requests.emplace_back(r.begin(), r.end()); }

private:
  static std::size_t pop(std::deque<std::vector<std::byte>>& q, std::span<std::byte> b) {
    if (q.empty()) return 0;
    const auto& p = q.front();
    std::copy(p.begin(), p.end(), b.begin());
    const std::size_t n = p.size();
    q.pop_front();
    return n;
  }
};

struct RecordingSink {
  std::vector<BookUpdate> updates;
  void on_update(const BookUpdate& u) { updates.push_back(u); }
};

// MoldUDP64 packet carrying real ITCH messages: header, then [u16 BE length][message]
std::vector<std::byte> packet(uint64_t seq, const std::vector<test::Msg>& msgs) {
  std::vector<std::byte> out(mold::kHeaderSize);
  mold::write_header(out.data(), mold_session(), seq, static_cast<uint16_t>(msgs.size()));
  for (const auto& m : msgs) {
    const auto b = m.bytes();
    out.push_back(static_cast<std::byte>(b.size() >> 8));
    out.push_back(static_cast<std::byte>(b.size() & 0xFF));
    out.insert(out.end(), b.begin(), b.end());
  }
  return out;
}

// Tick value for a time in ns, so tests can drive on_timer without waiting
uint64_t tsc_at_ns(uint64_t ns) {
  return static_cast<uint64_t>(static_cast<double>(ns) / core::ns_per_tick());
}

constexpr uint16_t kAapl = 1;
constexpr uint16_t kMsft = 2;
constexpr uint64_t kMs = 1'000'000;

class FeedHandlerTest : public ::testing::Test {
protected:
  FeedConfig cfg() const {
    FeedConfig c;
    c.session = mold_session();
    c.buffer_capacity = 64;
    c.recovery_timeout_ns = 1 * kMs;
    c.max_retries = 2;
    c.stale_after_ns = 5 * kMs;
    return c;
  }

  FakeIo io;
  RecordingSink sink;
  core::LogChannel log{"feed", 1024}; // never drained: records just accumulate or drop
  FeedHandler<RecordingSink, FakeIo> feed{cfg(), io, sink, log};
};

TEST_F(FeedHandlerTest, SubscribedSymbolEmitsOnlyWhenTopOfBookChanges) {
  feed.subscribe("AAPL");
  feed.on_packet(packet(1, {test::stock_directory(kAapl, "AAPL"),
                            test::stock_directory(kMsft, "MSFT"),
                            test::add(kAapl, 1, 'B', 100, 1'000'000),          // seq 3: new bid
                            test::add(kAapl, 2, 'S', 200, 1'010'000),          // seq 4: new ask
                            test::add(kAapl, 3, 'B', 50, 990'000),             // seq 5: deeper bid
                            test::add(kMsft, 4, 'B', 10, 500'000, "MSFT"),     // seq 6: not subscribed
                            test::add(kAapl, 5, 'B', 30, 1'000'000)}),         // seq 7: bid qty 130
                 42);

  ASSERT_EQ(sink.updates.size(), 3u);

  const auto& a = sink.updates[0];
  EXPECT_EQ(a.seq, 3u);
  EXPECT_EQ(a.rx_tsc, 42u);
  EXPECT_EQ(a.locate, kAapl);
  EXPECT_EQ(a.bid_px, 1'000'000u);
  EXPECT_EQ(a.bid_qty, 100u);
  EXPECT_EQ(a.ask_qty, 0u); // empty side

  const auto& b = sink.updates[1];
  EXPECT_EQ(b.seq, 4u);
  EXPECT_EQ(b.ask_px, 1'010'000u);
  EXPECT_EQ(b.ask_qty, 200u);

  const auto& c = sink.updates[2];
  EXPECT_EQ(c.seq, 7u);
  EXPECT_EQ(c.bid_px, 1'000'000u);
  EXPECT_EQ(c.bid_qty, 130u);

  EXPECT_EQ(feed.stats().updates, 3u);
  EXPECT_EQ(feed.stats().messages, 7u);
}

TEST_F(FeedHandlerTest, WithoutSubscriptionsBooksAreStillBuilt) {
  feed.on_packet(packet(1, {test::stock_directory(kAapl, "AAPL"),
                            test::add(kAapl, 1, 'B', 100, 1'000'000)}),
                 1);

  EXPECT_TRUE(sink.updates.empty());
  const book::OrderBook* b = feed.books().book(kAapl);
  ASSERT_NE(b, nullptr);
  ASSERT_TRUE(b->best_bid().has_value());
  EXPECT_EQ(b->best_bid()->qty, 100u);
  EXPECT_EQ(feed.directory().locate_of("AAPL"), kAapl);
}

TEST_F(FeedHandlerTest, CountsBadLengthMessages) {
  feed.on_packet(packet(1, {test::Msg('A', kAapl).u64(1)}), 1); // add order cut short
  EXPECT_EQ(feed.stats().bad_length, 1u);
}

TEST_F(FeedHandlerTest, GapSendsRerequestAndFillReturnsToLive) {
  feed.subscribe("AAPL");
  io.feed.push_back(packet(1, {test::stock_directory(kAapl, "AAPL")}));
  // seq 2 is lost
  io.feed.push_back(packet(3, {test::add(kAapl, 2, 'S', 200, 1'010'000)}));

  EXPECT_TRUE(feed.poll()); // seq 1
  EXPECT_EQ(feed.health(), FeedHealth::Live);
  EXPECT_TRUE(feed.poll()); // seq 3: gap, re-request sent in the same pass
  EXPECT_EQ(feed.health(), FeedHealth::Recovering);

  ASSERT_EQ(io.requests.size(), 1u);
  const auto req = mold::read_header(io.requests[0]);
  ASSERT_TRUE(req.has_value());
  EXPECT_EQ(req->seq, 2u);   // first missing
  EXPECT_EQ(req->count, 2u); // [2, 4): asks up to the highest seq seen
  EXPECT_EQ(req->session, mold_session());

  io.retransmits.push_back(packet(2, {test::add(kAapl, 1, 'B', 100, 1'000'000)}));
  EXPECT_TRUE(feed.poll()); // fill: applies 2, then releases buffered 3
  EXPECT_EQ(feed.health(), FeedHealth::Live);

  ASSERT_EQ(sink.updates.size(), 2u);
  EXPECT_EQ(sink.updates[0].seq, 2u);
  EXPECT_EQ(sink.updates[1].seq, 3u);
  EXPECT_EQ(feed.stats().rerequests, 1u);
  EXPECT_EQ(feed.session().stats().gaps, 1u);
}

TEST_F(FeedHandlerTest, RecoveredUpdatesCarryTheirOwnArrivalTime) {
  feed.subscribe("AAPL");
  feed.on_packet(packet(1, {test::stock_directory(kAapl, "AAPL")}), 100);
  feed.on_packet(packet(3, {test::add(kAapl, 2, 'S', 200, 1'010'000)}), 200); // buffered
  feed.on_packet(packet(2, {test::add(kAapl, 1, 'B', 100, 1'000'000)}), 900); // fill

  ASSERT_EQ(sink.updates.size(), 2u);
  EXPECT_EQ(sink.updates[0].rx_tsc, 900u); // seq 2 arrived with the fill
  EXPECT_EQ(sink.updates[1].rx_tsc, 200u); // seq 3 arrived earlier and waited in the buffer
}

TEST_F(FeedHandlerTest, RecoveryGivesUpAfterMaxRetries) {
  feed.on_packet(packet(1, {test::stock_directory(kAapl, "AAPL")}), tsc_at_ns(0));
  feed.on_packet(packet(3, {test::stock_directory(kMsft, "MSFT")}), tsc_at_ns(0)); // gap

  feed.on_timer(tsc_at_ns(0));      // first request
  feed.on_timer(tsc_at_ns(kMs / 2)); // inside the 1 ms timeout: wait
  EXPECT_EQ(io.requests.size(), 1u);

  feed.on_timer(tsc_at_ns(2 * kMs)); // retry 1
  feed.on_timer(tsc_at_ns(4 * kMs)); // retry 2
  EXPECT_EQ(io.requests.size(), 3u);
  EXPECT_TRUE(feed.running());

  feed.on_timer(tsc_at_ns(6 * kMs)); // retries exhausted
  EXPECT_EQ(io.requests.size(), 3u);
  EXPECT_EQ(feed.health(), FeedHealth::Failed);
  EXPECT_FALSE(feed.running());
}

TEST_F(FeedHandlerTest, NotStaleBeforeTheFirstPacket) {
  feed.on_timer(tsc_at_ns(1000 * kMs));
  EXPECT_EQ(feed.health(), FeedHealth::Live);
}

TEST_F(FeedHandlerTest, StaleAfterSilenceAndLiveOnTheNextPacket) {
  feed.on_packet(packet(1, {test::stock_directory(kAapl, "AAPL")}), tsc_at_ns(1 * kMs));

  feed.on_timer(tsc_at_ns(4 * kMs)); // 3 ms of silence: under the 5 ms threshold
  EXPECT_EQ(feed.health(), FeedHealth::Live);

  feed.on_timer(tsc_at_ns(7 * kMs)); // 6 ms
  EXPECT_EQ(feed.health(), FeedHealth::Stale);
  EXPECT_TRUE(feed.running()); // keep listening: the feed may come back

  feed.on_packet(test::heartbeat(mold_session(), 2), tsc_at_ns(8 * kMs));
  EXPECT_EQ(feed.health(), FeedHealth::Live);
}

TEST_F(FeedHandlerTest, EndOfSessionStopsThePoll) {
  feed.on_packet(packet(1, {test::stock_directory(kAapl, "AAPL")}), 1);
  feed.on_packet(test::end_of_session(mold_session(), 2), 2);

  EXPECT_EQ(feed.health(), FeedHealth::Ended);
  EXPECT_FALSE(feed.running());
  EXPECT_FALSE(feed.poll());
}

} // namespace
} // namespace ttt::feed
