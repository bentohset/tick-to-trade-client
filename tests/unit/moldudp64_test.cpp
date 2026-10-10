#include "feed/moldudp64/recovery.hpp"
#include "feed/moldudp64/session.hpp"

#include "utils/mold_builder.hpp"

#include <gtest/gtest.h>

namespace ttt::mold {
namespace {

using test::Recorder;
using test::mold_session;

// --- Session --------------------------------------------------------------------------

TEST(MoldSession, InOrderPacketsDeliverEverything) {
  Session s(1, 16);
  Recorder r;
  s.on_packet(test::packet(mold_session(), 1, 3), r);
  s.on_packet(test::packet(mold_session(), 4, 2), r);

  EXPECT_EQ(r.seqs, (std::vector<uint64_t>{1, 2, 3, 4, 5}));
  EXPECT_EQ(s.expected(), 6u);
  EXPECT_EQ(s.state(), FeedState::Live);
}

TEST(MoldSession, GapBuffersThenDrainsInOrder) {
  Session s(1, 16);
  Recorder r;

  s.on_packet(test::packet(mold_session(), 1, 2), r); // delivers 1,2; expected = 3
  s.on_packet(test::packet(mold_session(), 5, 2), r); // seq 5,6 arrive early: gap
  EXPECT_EQ(s.state(), FeedState::Recovering);
  EXPECT_EQ(r.seqs, (std::vector<uint64_t>{1, 2})); // nothing from the buffered packet yet

  s.on_packet(test::packet(mold_session(), 3, 2), r); // fills the hole: 3,4
  // filling the hole should also drain the buffered 5,6 in the same call
  EXPECT_EQ(r.seqs, (std::vector<uint64_t>{1, 2, 3, 4, 5, 6}));
  EXPECT_EQ(s.expected(), 7u);
  EXPECT_EQ(s.state(), FeedState::Live);
}

TEST(MoldSession, PartialOverlapSkipsPrefix) {
  Session s(1, 16);
  Recorder r;

  s.on_packet(test::packet(mold_session(), 1, 3), r); // delivers 1,2,3; expected = 4
  // a resend covering seq [2,5): only the seq-4 slot is actually new
  s.on_packet(test::packet(mold_session(), 2, 3), r);

  EXPECT_EQ(r.seqs, (std::vector<uint64_t>{1, 2, 3, 4}));
  EXPECT_EQ(s.expected(), 5u);
  EXPECT_EQ(s.stats().overlaps, 1u);
}

TEST(MoldSession, DuplicatePacketDropped) {
  Session s(1, 16);
  Recorder r;

  s.on_packet(test::packet(mold_session(), 1, 3), r); // delivers 1,2,3; expected = 4
  s.on_packet(test::packet(mold_session(), 1, 3), r); // exact resend: fully stale

  EXPECT_EQ(r.seqs, (std::vector<uint64_t>{1, 2, 3})); // nothing new delivered
  EXPECT_EQ(s.stats().duplicates, 1u);
  EXPECT_EQ(s.expected(), 4u);
}

TEST(MoldSession, SamePacketBufferedTwiceStoredOnce) {
  Session s(1, 16);
  Recorder r;

  s.on_packet(test::packet(mold_session(), 1, 1), r); // delivers 1; expected = 2
  s.on_packet(test::packet(mold_session(), 5, 1), r); // gap: buffer seq 5
  s.on_packet(test::packet(mold_session(), 5, 1), r); // same packet again: dedup, not a 2nd slot

  s.on_packet(test::packet(mold_session(), 2, 3), r); // fills 2,3,4; drains buffered 5

  // seq 5 must appear exactly once, proving the duplicate put() didn't double-store it
  EXPECT_EQ(r.seqs, (std::vector<uint64_t>{1, 2, 3, 4, 5}));
  EXPECT_EQ(s.expected(), 6u);
}

TEST(MoldSession, HeartbeatRevealsTailGap) {
  Session s(1, 16);
  Recorder r;

  s.on_packet(test::packet(mold_session(), 1, 2), r); // delivers 1,2; expected = 3
  s.on_packet(test::heartbeat(mold_session(), 5), r); // sender claims it's reached seq 5

  EXPECT_EQ(s.state(), FeedState::Recovering);
  EXPECT_EQ(s.stats().heartbeats, 1u);
  ASSERT_TRUE(s.gap().has_value());
  EXPECT_EQ(s.gap()->first, 3u);
  EXPECT_EQ(s.gap()->end, 5u); // nothing buffered, so the end is known_end_ itself
}

TEST(MoldSession, EndOfSessionWhileRecoveringStillEnds) {
  Session s(1, 16);
  Recorder r;

  s.on_packet(test::packet(mold_session(), 1, 1), r);          // delivers 1; expected = 2
  s.on_packet(test::packet(mold_session(), 5, 1), r);          // gap: Recovering
  s.on_packet(test::end_of_session(mold_session(), 6), r);     // EOS arrives mid-gap

  EXPECT_EQ(s.state(), FeedState::Recovering); // too early to end: still behind
  EXPECT_TRUE(s.gap().has_value());            // still has an outstanding gap

  s.on_packet(test::packet(mold_session(), 2, 3), r); // fills 2,3,4; drains buffered 5

  EXPECT_EQ(r.seqs, (std::vector<uint64_t>{1, 2, 3, 4, 5}));
  EXPECT_EQ(s.state(), FeedState::Ended); // caught up only now, after EOS already arrived
}

TEST(MoldSession, WrongSessionIdIgnored) {
  Session s(1, 16);
  Recorder r;

  s.on_packet(test::packet(mold_session("TEST000001"), 1, 1), r); // adopts this session
  s.on_packet(test::packet(mold_session("OTHER00001"), 2, 1), r); // different session id

  EXPECT_EQ(r.seqs, (std::vector<uint64_t>{1})); // the mismatched packet changed nothing
  EXPECT_EQ(s.expected(), 2u);
  EXPECT_EQ(s.stats().wrong_session, 1u);
}

TEST(MoldSession, TruncatedPacketBecomesGap) {
  Session s(1, 16);
  Recorder r;

  // header claims 3 blocks but only 1 is actually present: the sender's framing is broken
  test::PacketBuilder b(mold_session(), 1, 3);
  b.block();
  s.on_packet(b.bytes(), r);

  EXPECT_EQ(r.seqs, (std::vector<uint64_t>{1})); // only the one real block got delivered
  EXPECT_EQ(s.expected(), 2u);
  EXPECT_EQ(s.state(), FeedState::Recovering); // the claimed-but-missing 2 messages are a gap
  ASSERT_TRUE(s.gap().has_value());
  EXPECT_EQ(s.gap()->first, 2u);
  EXPECT_EQ(s.gap()->end, 4u); // known_end_ = seq(1) + declared count(3)
}

TEST(MoldSession, BufferFullFails) {
  Session s(1, 1); // capacity 1: the second buffered packet has nowhere to go
  Recorder r;

  s.on_packet(test::packet(mold_session(), 10, 1), r); // gap: buffers seq 10, fills the one slot
  EXPECT_EQ(s.state(), FeedState::Recovering);

  s.on_packet(test::packet(mold_session(), 20, 1), r); // a second, different gapped packet
  EXPECT_EQ(s.state(), FeedState::Failed);
}

// --- Recovery --------------------------------------------------------------------------

TEST(MoldRecovery, WaitsThenRetriesThenTimesOut) {
  Session s(1, 16);
  Recorder r;
  s.on_packet(test::packet(mold_session(), 5, 2), r); // gap from the very start: Recovering

  Recovery rec(/*timeout_ns=*/1000, /*max_retries=*/2);

  // first poll: nothing in flight yet, sends the initial request
  auto req = rec.poll(s, 0);
  ASSERT_TRUE(req.has_value());
  EXPECT_EQ(req->first, 1u);
  EXPECT_FALSE(rec.timed_out());

  // well before the timeout: waits, no resend
  EXPECT_FALSE(rec.poll(s, 500).has_value());
  EXPECT_FALSE(rec.timed_out());

  // timeout elapsed with no progress: retry #1
  EXPECT_TRUE(rec.poll(s, 1000).has_value());
  EXPECT_FALSE(rec.timed_out());

  // timeout elapsed again: retry #2 (== max_retries, not yet over the limit)
  EXPECT_TRUE(rec.poll(s, 2000).has_value());
  EXPECT_FALSE(rec.timed_out());

  // a third timeout with still no progress exceeds max_retries
  rec.poll(s, 3000);
  EXPECT_TRUE(rec.timed_out());
}

TEST(MoldRecovery, ProgressOnPartialRetransmitExtendsDeadline) {
  Session s(1, 16);
  Recorder r;
  s.on_packet(test::packet(mold_session(), 5, 2), r); // gap: expected = 1, buffered seq 5

  Recovery rec(/*timeout_ns=*/1000, /*max_retries=*/5);

  // first request: [1, 7) -- the ring no longer narrows the end to the buffered seq,
  // it always asks up to known_end_ (seq 5, count 2 -> known_end_ = 7)
  auto req = rec.poll(s, 0);
  ASSERT_TRUE(req.has_value());
  EXPECT_EQ(req->first, 1u);
  EXPECT_EQ(req->end, 7u);

  // part of the gap arrives: expected_ advances from 1 to 3
  s.on_packet(test::packet(mold_session(), 1, 2), r);

  // progress is visible (gap->first moved from 1 to 3): deadline extends, no resend
  EXPECT_FALSE(rec.poll(s, 500).has_value());

  // without the extension this would already be due (500 + old timeout == 1500 > 1000);
  // confirm it's still not firing just before the *extended* deadline
  EXPECT_FALSE(rec.poll(s, 1400).has_value());

  // now past the extended deadline (500 + 1000): resends
  EXPECT_TRUE(rec.poll(s, 1500).has_value());
}

} // namespace
} // namespace ttt::mold
