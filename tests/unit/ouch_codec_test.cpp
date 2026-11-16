#include "gateway/ouch/codec.hpp"

#include "apps/mock_exchange/ouch_encode.hpp"

#include <gtest/gtest.h>

#include <array>
#include <span>
#include <string_view>

namespace ttt::ouch {
namespace {

constexpr TokenPrefix kPrefix{'T', '1'};

std::string_view sv(const Token& t) { return {t.data(), t.size()}; }

// --- tokens ---------------------------------------------------------------------------

TEST(OuchToken, LayoutIsPrefixIdGeneration) {
  EXPECT_EQ(sv(make_token(kPrefix, 42, 3)), "T1000000042003");
  EXPECT_EQ(sv(make_token(kPrefix, 0, 0)), "T1000000000000");
  EXPECT_EQ(sv(make_token(kPrefix, 999'999'999, 999)), "T1999999999999");
}

TEST(OuchToken, RoundTrip) {
  for (const uint32_t id : {0u, 1u, 12345u, 999'999'999u}) {
    for (const uint16_t gen : {uint16_t{0}, uint16_t{1}, uint16_t{999}}) {
      const auto r = parse_token(make_token(kPrefix, id, gen), kPrefix);
      ASSERT_TRUE(r);
      EXPECT_EQ(r->id, id);
      EXPECT_EQ(r->gen, gen);
    }
  }
}

TEST(OuchToken, ForeignPrefixIsNotOurs) {
  EXPECT_FALSE(parse_token(make_token({'X', '9'}, 1, 0), kPrefix));
}

TEST(OuchToken, NonDigitIsRejected) {
  Token t = make_token(kPrefix, 1, 0);
  t[5] = ' ';
  EXPECT_FALSE(parse_token(t, kPrefix));
}

// --- client -> exchange ---------------------------------------------------------------

struct InboundRecorder {
  int enters = 0, cancels = 0, replaces = 0;
  std::array<std::byte, 64> last{};
  void on(const EnterOrder&) { ++enters; }
  void on(const CancelOrder&) { ++cancels; }
  void on(const ReplaceOrder&) { ++replaces; }
};

TEST(OuchCodec, EnterOrderRoundTrip) {
  std::array<std::byte, EnterOrder::kSize> buf{};
  const gw::NewOrder n{itch::Symbol::from("MSFT"), 1'234'500, 300, gw::Side::SellShort,
                       gw::Tif::Ioc};
  const Token t = make_token(kPrefix, 7, 0);
  encode_enter(buf.data(), t, n, {'T', 'T', 'T', 'C'});

  const EnterOrder m{buf.data()};
  EXPECT_EQ(static_cast<char>(buf[0]), 'O');
  EXPECT_EQ(m.token(), t);
  EXPECT_EQ(m.side(), 'T');
  EXPECT_EQ(m.shares(), 300u);
  EXPECT_EQ(m.stock().view(), "MSFT");
  EXPECT_EQ(m.price(), 1'234'500u);
  EXPECT_EQ(m.tif(), 0u); // IOC

  InboundRecorder r;
  EXPECT_EQ(parse_inbound(buf, r), ParseResult::Ok);
  EXPECT_EQ(r.enters, 1);
}

TEST(OuchCodec, SideAndTifCodes) {
  EXPECT_EQ(side_code(gw::Side::Buy), 'B');
  EXPECT_EQ(side_code(gw::Side::Sell), 'S');
  EXPECT_EQ(side_code(gw::Side::SellShort), 'T');
  EXPECT_EQ(tif_secs(gw::Tif::Ioc), 0u);
  EXPECT_EQ(tif_secs(gw::Tif::Day), 99999u);
}

TEST(OuchCodec, CancelOrderRoundTrip) {
  std::array<std::byte, CancelOrder::kSize> buf{};
  const Token t = make_token(kPrefix, 7, 2);
  encode_cancel(buf.data(), t, 150);

  const CancelOrder m{buf.data()};
  EXPECT_EQ(m.token(), t);
  EXPECT_EQ(m.shares(), 150u);

  InboundRecorder r;
  EXPECT_EQ(parse_inbound(buf, r), ParseResult::Ok);
  EXPECT_EQ(r.cancels, 1);
}

TEST(OuchCodec, ReplaceOrderRoundTrip) {
  std::array<std::byte, ReplaceOrder::kSize> buf{};
  const Token old_t = make_token(kPrefix, 7, 0);
  const Token new_t = make_token(kPrefix, 7, 1);
  encode_replace(buf.data(), old_t, new_t, 500, 999'900, 99999);

  const ReplaceOrder m{buf.data()};
  EXPECT_EQ(m.existing(), old_t);
  EXPECT_EQ(m.replacement(), new_t);
  EXPECT_EQ(m.shares(), 500u);
  EXPECT_EQ(m.price(), 999'900u);
  EXPECT_EQ(m.tif(), 99999u);

  InboundRecorder r;
  EXPECT_EQ(parse_inbound(buf, r), ParseResult::Ok);
  EXPECT_EQ(r.replaces, 1);
}

// --- exchange -> client ---------------------------------------------------------------

mock_exchange::ExOrder ex_order() {
  mock_exchange::ExOrder o;
  o.stock = itch::Symbol::from("AAPL");
  o.side = 'B';
  o.price = 1'500'000;
  o.tif = 99999;
  o.open = 800;
  return o;
}

TEST(OuchCodec, AcceptedRoundTrip) {
  std::array<std::byte, Accepted::kSize> buf{};
  const Token t = make_token(kPrefix, 3, 0);
  mock_exchange::encode_accepted(buf.data(), 123456789, t, ex_order(), 800, 55, 'L');

  const Accepted m{buf.data()};
  EXPECT_EQ(m.timestamp_ns(), 123456789u);
  EXPECT_EQ(m.token(), t);
  EXPECT_EQ(m.side(), 'B');
  EXPECT_EQ(m.shares(), 800u);
  EXPECT_EQ(m.stock().view(), "AAPL");
  EXPECT_EQ(m.price(), 1'500'000u);
  EXPECT_EQ(m.order_ref(), 55u);
  EXPECT_EQ(m.order_state(), 'L');
}

TEST(OuchCodec, ReplacedRoundTrip) {
  std::array<std::byte, Replaced::kSize> buf{};
  const Token prev = make_token(kPrefix, 3, 0);
  const Token t = make_token(kPrefix, 3, 1);
  mock_exchange::encode_replaced(buf.data(), 1, t, ex_order(), 56, 'L', prev);

  const Replaced m{buf.data()};
  EXPECT_EQ(m.token(), t);
  EXPECT_EQ(m.previous_token(), prev);
  EXPECT_EQ(m.shares(), 800u);
  EXPECT_EQ(m.price(), 1'500'000u);
  EXPECT_EQ(m.order_ref(), 56u);
  EXPECT_EQ(m.order_state(), 'L');
}

TEST(OuchCodec, ExecutedRoundTrip) {
  std::array<std::byte, Executed::kSize> buf{};
  const Token t = make_token(kPrefix, 3, 0);
  mock_exchange::encode_executed(buf.data(), 1, t, 100, 1'499'900, 'A', 0x0102030405060708);

  const Executed m{buf.data()};
  EXPECT_EQ(m.token(), t);
  EXPECT_EQ(m.shares(), 100u);
  EXPECT_EQ(m.price(), 1'499'900u);
  EXPECT_EQ(m.liquidity(), 'A');
  EXPECT_EQ(m.match_number(), 0x0102030405060708u);
}

TEST(OuchCodec, CanceledAndRejectedRoundTrip) {
  const Token t = make_token(kPrefix, 3, 0);

  std::array<std::byte, Canceled::kSize> c{};
  mock_exchange::encode_canceled(c.data(), 1, t, 250, 'U');
  EXPECT_EQ(Canceled{c.data()}.decrement(), 250u);
  EXPECT_EQ(Canceled{c.data()}.reason(), 'U');

  std::array<std::byte, Rejected::kSize> j{};
  mock_exchange::encode_rejected(j.data(), 1, t, 'O');
  EXPECT_EQ(Rejected{j.data()}.token(), t);
  EXPECT_EQ(Rejected{j.data()}.reason(), 'O');
}

// --- dispatch -------------------------------------------------------------------------

struct OutboundRecorder {
  std::string types;
  void on(const Accepted&) { types += 'A'; }
  void on(const Executed&) { types += 'E'; }
  void on(const Canceled&) { types += 'C'; }
  // Replaced, Rejected, etc. deliberately not handled
};

TEST(OuchCodec, ParseOutboundDispatchesByType) {
  const Token t = make_token(kPrefix, 1, 0);
  OutboundRecorder r;

  std::array<std::byte, Accepted::kSize> a{};
  mock_exchange::encode_accepted(a.data(), 1, t, ex_order(), 1, 1, 'L');
  std::array<std::byte, Executed::kSize> e{};
  mock_exchange::encode_executed(e.data(), 1, t, 1, 1, 'A', 1);
  std::array<std::byte, Canceled::kSize> c{};
  mock_exchange::encode_canceled(c.data(), 1, t, 1, 'U');

  EXPECT_EQ(parse_outbound(a, r), ParseResult::Ok);
  EXPECT_EQ(parse_outbound(e, r), ParseResult::Ok);
  EXPECT_EQ(parse_outbound(c, r), ParseResult::Ok);
  EXPECT_EQ(r.types, "AEC");
}

TEST(OuchCodec, UnhandledMessageIsOkButNotDelivered) {
  std::array<std::byte, Rejected::kSize> j{};
  mock_exchange::encode_rejected(j.data(), 1, make_token(kPrefix, 1, 0), 'O');
  OutboundRecorder r;
  EXPECT_EQ(parse_outbound(j, r), ParseResult::Ok);
  EXPECT_TRUE(r.types.empty());
}

TEST(OuchCodec, ShortMessageIsBadLength) {
  std::array<std::byte, Executed::kSize> e{};
  mock_exchange::encode_executed(e.data(), 1, make_token(kPrefix, 1, 0), 1, 1, 'A', 1);
  OutboundRecorder r;
  EXPECT_EQ(parse_outbound(std::span(e).first(Executed::kSize - 1), r), ParseResult::BadLength);
  EXPECT_EQ(parse_outbound(std::span<const std::byte>{}, r), ParseResult::BadLength);
  EXPECT_TRUE(r.types.empty());
}

TEST(OuchCodec, UnknownTypeIsReported) {
  std::array<std::byte, 64> m{};
  m[0] = std::byte{'Q'};
  OutboundRecorder r;
  EXPECT_EQ(parse_outbound(m, r), ParseResult::Unknown);
  InboundRecorder in;
  EXPECT_EQ(parse_inbound(m, in), ParseResult::Unknown);
}

} // namespace
} // namespace ttt::ouch
