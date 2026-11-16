#include "gateway/soupbintcp/framing.hpp"
#include "gateway/soupbintcp/wire.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ttt::soup {
namespace {

// one SoupBinTCP packet: length(2) type(1) payload
std::vector<std::byte> packet(char type, std::string_view payload) {
  std::vector<std::byte> v(kHeaderSize + payload.size());
  be::store(v.data(), static_cast<uint16_t>(payload.size() + 1));
  v[2] = static_cast<std::byte>(type);
  std::memcpy(v.data() + kHeaderSize, payload.data(), payload.size());
  return v;
}

void feed(RecvBuffer& rx, std::span<const std::byte> bytes) {
  auto tail = rx.tail();
  ASSERT_GE(tail.size(), bytes.size());
  std::memcpy(tail.data(), bytes.data(), bytes.size());
  rx.commit(bytes.size());
}

std::string as_str(std::span<const std::byte> p) {
  return {reinterpret_cast<const char*>(p.data()), p.size()};
}

struct Collected {
  std::string types;
  std::vector<std::string> payloads;
};

// drains and records every packet; keep_going = true
bool drain_all(RecvBuffer& rx, Collected& c) {
  return rx.drain([&](char t, std::span<const std::byte> p) {
    c.types += t;
    c.payloads.push_back(as_str(p));
    return true;
  });
}

// --- numeric fields -------------------------------------------------------------------

TEST(SoupWire, WriteNumPadsLeftWithSpaces) {
  std::array<std::byte, 6> b{};
  write_num(b.data(), b.size(), 42);
  EXPECT_EQ(as_str(b), "    42");
  write_num(b.data(), b.size(), 0);
  EXPECT_EQ(as_str(b), "     0");
}

TEST(SoupWire, ReadNumSkipsPadding) {
  std::array<std::byte, 20> b{};
  for (const uint64_t v : {0ull, 1ull, 123456789ull, 18'446'744'073'709'551'615ull}) {
    write_num(b.data(), b.size(), v);
    EXPECT_EQ(read_num(b.data(), b.size()), v);
  }
}

TEST(SoupWire, LoginRequestLayout) {
  std::array<std::byte, kLoginRequestLen> b{};
  const Username u{'t', 't', 't', ' ', ' ', ' '};
  const Password pw{'p', 'w', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '};
  SessionId s;
  s.fill(' ');
  encode_login(b.data(), u, pw, s, 17);

  EXPECT_EQ(as_str(std::span(b).subspan(0, 6)), "ttt   ");
  EXPECT_EQ(as_str(std::span(b).subspan(6, 10)), "pw        ");
  EXPECT_EQ(as_str(std::span(b).subspan(16, 10)), "          ");
  EXPECT_EQ(read_num(b.data() + 26, 20), 17u);
}

TEST(SoupWire, LoginAcceptedLayout) {
  std::array<std::byte, kLoginAcceptedLen> b{};
  const SessionId s{'O', 'U', 'C', 'H', '0', '0', '0', '0', '0', '1'};
  encode_login_accepted(b.data(), s, 9001);
  EXPECT_EQ(as_str(std::span(b).subspan(0, 10)), "OUCH000001");
  EXPECT_EQ(read_num(b.data() + 10, 20), 9001u);
}

// --- RecvBuffer -----------------------------------------------------------------------

TEST(SoupRecvBuffer, DrainsSeveralWholePackets) {
  auto rx = std::make_unique<RecvBuffer>();
  feed(*rx, packet('S', "hello"));
  feed(*rx, packet('H', ""));
  feed(*rx, packet('S', "world"));

  Collected c;
  EXPECT_TRUE(drain_all(*rx, c));
  EXPECT_EQ(c.types, "SHS");
  EXPECT_EQ(c.payloads, (std::vector<std::string>{"hello", "", "world"}));
}

TEST(SoupRecvBuffer, PacketSplitAcrossReads) {
  auto rx = std::make_unique<RecvBuffer>();
  const auto p = packet('S', "abcdefgh");
  Collected c;

  feed(*rx, std::span(p).first(5));
  EXPECT_TRUE(drain_all(*rx, c));
  EXPECT_TRUE(c.types.empty()); // partial: kept for next time

  feed(*rx, std::span(p).subspan(5));
  EXPECT_TRUE(drain_all(*rx, c));
  EXPECT_EQ(c.types, "S");
  EXPECT_EQ(c.payloads.at(0), "abcdefgh");
}

TEST(SoupRecvBuffer, LengthFieldSplitAcrossReads) {
  auto rx = std::make_unique<RecvBuffer>();
  const auto p = packet('S', "xy");
  Collected c;

  feed(*rx, std::span(p).first(1)); // half of the length
  EXPECT_TRUE(drain_all(*rx, c));
  EXPECT_TRUE(c.types.empty());

  feed(*rx, std::span(p).subspan(1));
  EXPECT_TRUE(drain_all(*rx, c));
  EXPECT_EQ(c.payloads, (std::vector<std::string>{"xy"}));
}

TEST(SoupRecvBuffer, WholePacketThenPartialKeepsLeftover) {
  auto rx = std::make_unique<RecvBuffer>();
  const auto a = packet('S', "first");
  const auto b = packet('S', "second");
  Collected c;

  feed(*rx, a);
  feed(*rx, std::span(b).first(4));
  EXPECT_TRUE(drain_all(*rx, c));
  EXPECT_EQ(c.payloads, (std::vector<std::string>{"first"}));

  feed(*rx, std::span(b).subspan(4));
  EXPECT_TRUE(drain_all(*rx, c));
  EXPECT_EQ(c.payloads, (std::vector<std::string>{"first", "second"}));
}

TEST(SoupRecvBuffer, ZeroLengthIsFramingError) {
  auto rx = std::make_unique<RecvBuffer>();
  const std::array<std::byte, 3> bad{std::byte{0}, std::byte{0}, std::byte{'S'}};
  feed(*rx, bad);
  Collected c;
  EXPECT_FALSE(drain_all(*rx, c));
  EXPECT_TRUE(c.types.empty());
}

TEST(SoupRecvBuffer, StopKeepsRemainingPackets) {
  auto rx = std::make_unique<RecvBuffer>();
  feed(*rx, packet('S', "1"));
  feed(*rx, packet('Z', ""));
  feed(*rx, packet('S', "2"));

  std::string seen;
  EXPECT_TRUE(rx->drain([&](char t, std::span<const std::byte>) {
    seen += t;
    return t != 'Z'; // e.g. End of Session
  }));
  EXPECT_EQ(seen, "SZ");

  Collected c;
  EXPECT_TRUE(drain_all(*rx, c));
  EXPECT_EQ(c.payloads, (std::vector<std::string>{"2"}));
}

TEST(SoupRecvBuffer, ResetDiscardsPartial) {
  auto rx = std::make_unique<RecvBuffer>();
  const auto p = packet('S', "abc");
  feed(*rx, std::span(p).first(3));
  rx->reset();
  feed(*rx, packet('H', ""));
  Collected c;
  EXPECT_TRUE(drain_all(*rx, c));
  EXPECT_EQ(c.types, "H");
}

// --- SendBuffer -----------------------------------------------------------------------

TEST(SoupSendBuffer, PutPacketWritesHeaderAndPayload) {
  auto tx = std::make_unique<SendBuffer>();
  EXPECT_TRUE(tx->empty());
  ASSERT_TRUE(put_packet(*tx, kUnsequenced, 3, [](std::byte* p) { std::memcpy(p, "abc", 3); }));
  EXPECT_FALSE(tx->empty());

  // the bytes land where the next reserve would have started
  tx->reset();
  std::byte* p = tx->reserve(1);
  ASSERT_NE(p, nullptr);
  ASSERT_TRUE(put_packet(*tx, kUnsequenced, 3, [](std::byte* q) { std::memcpy(q, "abc", 3); }));
  EXPECT_EQ(be::load<uint16_t>(p), 4u); // type + payload
  EXPECT_EQ(static_cast<char>(p[2]), 'U');
  EXPECT_EQ(std::memcmp(p + 3, "abc", 3), 0);
}

TEST(SoupSendBuffer, ReserveFailsWhenFull) {
  auto tx = std::make_unique<SendBuffer>();
  ASSERT_NE(tx->reserve(1 << 16), nullptr);
  tx->commit(1 << 16);
  EXPECT_EQ(tx->reserve(1), nullptr); // nothing sent, nothing to compact
  EXPECT_FALSE(put_packet(*tx, kClientHeartbeat, 0, [](std::byte*) {}));

  tx->reset();
  EXPECT_TRUE(tx->empty());
  EXPECT_NE(tx->reserve(1), nullptr);
}

} // namespace
} // namespace ttt::soup
