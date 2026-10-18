#include "core/async_logger.hpp"

#include "core/clock.hpp"

#include <gtest/gtest.h>

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace ttt::core {
namespace {

// --- compile-time rules ---------------------------------------------------------------

static_assert(sizeof(LogRecord) == 64);
static_assert(detail::Loggable<int>);
static_assert(detail::Loggable<double>);
static_assert(detail::Loggable<Lit>);
static_assert(detail::Loggable<FixedStr<8>>);
static_assert(!detail::Loggable<const char*>); // must go through Lit or FixedStr
static_assert(!detail::Loggable<char[4]>);     // a string literal passed directly

// --- pack / decode, no threads -------------------------------------------------------

// Builds a record the way LogChannel::log does and formats it the way T3 does
template <class... Args>
std::string roundtrip(std::format_string<Args...> fmt, const Args&... args) {
  LogRecord r{};
  r.fmt = fmt.get().data();
  r.fmt_len = uint16_t(fmt.get().size());
  detail::pack(r.args, args...);
  std::string out;
  detail::decode<Args...>(r, out);
  return out;
}

TEST(LogDecode, NoArgs) { EXPECT_EQ(roundtrip("feed live"), "feed live"); }

TEST(LogDecode, Integers) {
  EXPECT_EQ(roundtrip("rerequest [{}, {})", uint64_t{100}, uint64_t{164}), "rerequest [100, 164)");
  EXPECT_EQ(roundtrip("{} {} {}", int8_t{-1}, uint16_t{65535}, int64_t{-9'000'000'000}),
            "-1 65535 -9000000000");
}

TEST(LogDecode, MixedSizesPackedWithoutPadding) {
  // 1 + 8 + 2 + 8 bytes: misaligned offsets after the first argument
  EXPECT_EQ(roundtrip("{} {} {} {}", 'x', 2.5, uint16_t{7}, uint64_t{1} << 40),
            "x 2.5 7 1099511627776");
}

TEST(LogDecode, FormatSpecsApply) {
  EXPECT_EQ(roundtrip("{:>5}|{:.2f}|{:#x}", 42, 3.14159, 255u), "   42|3.14|0xff");
}

TEST(LogDecode, Lit) { EXPECT_EQ(roundtrip("reject: {}", Lit{"price band"}), "reject: price band"); }

TEST(LogDecode, FixedStr) {
  EXPECT_EQ(roundtrip("sym {}", FixedStr<8>{"AAPL"}), "sym AAPL");
  EXPECT_EQ(roundtrip("[{:>6}]", FixedStr<8>{"AAPL"}), "[  AAPL]");
}

TEST(LogDecode, FixedStrTruncatesToCapacity) {
  EXPECT_EQ(roundtrip("{}", FixedStr<4>{"ABCDEFG"}), "ABCD");
}

TEST(LogDecode, FixedStrCopiesItsSource) {
  std::string src = "MSFT";
  const FixedStr<8> f{src};
  src = "XXXX"; // the source changing after capture must not change the log line
  EXPECT_EQ(roundtrip("{}", f), "MSFT");
}

// --- AsyncLogger end to end ----------------------------------------------------------

// Logger output goes to a temp file; read it back after stop()
class AsyncLoggerTest : public ::testing::Test {
protected:
  void SetUp() override {
    file_ = std::tmpfile();
    ASSERT_NE(file_, nullptr);
  }
  void TearDown() override { std::fclose(file_); }

  std::vector<std::string> lines() {
    std::rewind(file_);
    std::vector<std::string> out;
    char buf[512];
    while (std::fgets(buf, sizeof(buf), file_)) {
      std::string s(buf);
      if (!s.empty() && s.back() == '\n') s.pop_back();
      out.push_back(std::move(s));
    }
    return out;
  }

  // "HH:MM:SS.nnnnnnnnn INF feed  text" -> "INF feed  text"
  static std::string_view body(std::string_view line) {
    constexpr std::size_t kStamp = 18; // HH:MM:SS.nnnnnnnnn
    return line.size() > kStamp + 1 ? line.substr(kStamp + 1) : std::string_view{};
  }

  std::FILE* file_ = nullptr;
};

TEST_F(AsyncLoggerTest, WritesTimestampLevelChannelAndText) {
  {
    AsyncLogger logger(file_);
    auto& feed = logger.channel("feed");
    logger.start();
    feed.log(LogLevel::Warn, "gap at {}", uint64_t{42});
    logger.stop();
  }
  const auto out = lines();
  ASSERT_EQ(out.size(), 1u);

  const std::string& l = out[0];
  ASSERT_GE(l.size(), 19u);
  for (std::size_t i : {0, 1, 3, 4, 6, 7}) EXPECT_TRUE(std::isdigit(l[i])) << l;
  EXPECT_EQ(l[2], ':');
  EXPECT_EQ(l[5], ':');
  EXPECT_EQ(l[8], '.');
  EXPECT_EQ(body(l), "WRN feed  gap at 42");
}

TEST_F(AsyncLoggerTest, FiltersBelowMinLevel) {
  {
    AsyncLogger logger(file_);
    auto& feed = logger.channel("feed");
    feed.set_min_level(LogLevel::Warn);
    logger.start();
    feed.log(LogLevel::Debug, "debug");
    feed.log(LogLevel::Info, "info");
    feed.log(LogLevel::Warn, "warn");
    feed.log(LogLevel::Error, "error");
    logger.stop();
  }
  const auto out = lines();
  ASSERT_EQ(out.size(), 2u);
  EXPECT_EQ(body(out[0]), "WRN feed  warn");
  EXPECT_EQ(body(out[1]), "ERR feed  error");
}

TEST_F(AsyncLoggerTest, StopDrainsEverythingQueued) {
  constexpr int kLines = 3000;
  {
    AsyncLogger logger(file_);
    auto& feed = logger.channel("feed", 4096); // large enough that nothing drops
    logger.start();
    for (int i = 0; i < kLines; ++i) feed.log(LogLevel::Info, "{}", i);
    logger.stop();
  }
  const auto out = lines();
  ASSERT_EQ(out.size(), std::size_t(kLines));
  for (int i = 0; i < kLines; ++i) EXPECT_EQ(body(out[i]), "INF feed  " + std::to_string(i));
}

TEST_F(AsyncLoggerTest, FullChannelDropsAndReportsCount) {
  {
    AsyncLogger logger(file_);
    auto& feed = logger.channel("feed", 4);
    // Logger not started yet, so nothing drains: 4 fit, 6 are dropped
    for (int i = 0; i < 10; ++i) feed.log(LogLevel::Info, "{}", i);
    logger.start();
    logger.stop();
  }
  const auto out = lines();
  ASSERT_EQ(out.size(), 5u);
  for (int i = 0; i < 4; ++i) EXPECT_EQ(body(out[i]), "INF feed  " + std::to_string(i));
  EXPECT_EQ(out[4], "WRN logger: feed dropped 6 records");
}

TEST_F(AsyncLoggerTest, MergesChannelsByTimestamp) {
  // Wait for the tick counter to move so every record has a distinct tsc.
  // Needed on Apple Silicon, where it only advances every ~41 ns.
  const auto next_tick = [] {
    const uint64_t t = read_ticks();
    while (read_ticks() == t) {}
  };
  {
    AsyncLogger logger(file_);
    auto& a = logger.channel("a");
    auto& b = logger.channel("b");
    // Queued before start(), so the order in the file can only come from the merge
    a.log(LogLevel::Info, "1");
    next_tick();
    b.log(LogLevel::Info, "2");
    next_tick();
    b.log(LogLevel::Info, "3");
    next_tick();
    a.log(LogLevel::Info, "4");
    logger.start();
    logger.stop();
  }
  const auto out = lines();
  ASSERT_EQ(out.size(), 4u);
  EXPECT_EQ(body(out[0]), "INF a     1");
  EXPECT_EQ(body(out[1]), "INF b     2");
  EXPECT_EQ(body(out[2]), "INF b     3");
  EXPECT_EQ(body(out[3]), "INF a     4");
}

TEST_F(AsyncLoggerTest, ConcurrentProducersKeepPerChannelOrder) {
  constexpr int kPerThread = 2000;
  {
    AsyncLogger logger(file_);
    auto& t1 = logger.channel("t1", 4096); // > kPerThread: no drops even if T3 lags
    auto& t2 = logger.channel("t2", 4096);
    logger.start();
    std::thread p1([&] {
      for (int i = 0; i < kPerThread; ++i) t1.log(LogLevel::Info, "{}", i);
    });
    std::thread p2([&] {
      for (int i = 0; i < kPerThread; ++i) t2.log(LogLevel::Info, "{}", i);
    });
    p1.join();
    p2.join();
    logger.stop(); // producers stopped first, so the final drain sees everything
  }

  int next1 = 0;
  int next2 = 0;
  for (const auto& l : lines()) {
    const auto b = body(l);
    if (b.starts_with("INF t1    ")) {
      EXPECT_EQ(b.substr(10), std::to_string(next1++));
    } else if (b.starts_with("INF t2    ")) {
      EXPECT_EQ(b.substr(10), std::to_string(next2++));
    } else {
      ADD_FAILURE() << "unexpected line: " << l;
    }
  }
  EXPECT_EQ(next1, kPerThread);
  EXPECT_EQ(next2, kPerThread);
}

TEST_F(AsyncLoggerTest, StopWithoutStartIsSafe) {
  AsyncLogger logger(file_);
  logger.channel("feed");
  logger.stop(); // destructor calls stop() again
  EXPECT_TRUE(lines().empty());
}

} // namespace
} // namespace ttt::core
