#pragma once

#include "core/clock.hpp"
#include "core/spsc_queue.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <format>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <type_traits>
#include <vector>
namespace ttt::core {

enum class LogLevel : uint8_t { Debug, Info, Warn, Error };

// Because arguments are captured now on hot thread but formatted later on another thread.
// Copying a const char* only copies the address which may have changed during format time so
// we can't pass raw pointers.

// Wraps a string with static storage so it lives for the whole program
struct Lit {
  const char* s;
  // Only accepts constant expression, so a pointer into a
  // packet buffer or std::string will fail to compile.
  consteval Lit(const char* p) : s(p) {
    if (!p) throw "Lit: null";
  }
};

// Short string copied by value that has fixed size
// eg. stock symbol (8 bytes)
template <std::size_t N> struct FixedStr {
  char data[N]{};
  uint8_t len{0};
  FixedStr() = default;
  explicit FixedStr(std::string_view v) : len(uint8_t(std::min(v.size(), N))) {
    std::memcpy(data, v.data(), len);
  }
};

struct LogRecord;
using DecodeFn = void (*)(const LogRecord&, std::string& out);

// 64bytes - a single cache line
struct LogRecord {
  uint64_t tsc;    // time of event
  DecodeFn decode; // function that knows the argument types
  const char* fmt; // points to the format string literal
  uint16_t fmt_len;
  LogLevel level;
  std::byte args[37];
};
static_assert(sizeof(LogRecord) == 64);
static_assert(std::is_trivially_copyable_v<LogRecord>);

namespace detail {
// packs the arguments into LogRecord args
template <class... Args> void pack(std::byte* dst, const Args&... args) {
  std::size_t off = 0;
  ((std::memcpy(dst + off, &args, sizeof(Args)), off += sizeof(Args)), ...);
}

// reverse of pack() for one value at offset
template <class T> T load(const std::byte* src, std::size_t& off) {
  std::array<std::byte, sizeof(T)> b;
  std::memcpy(b.data(), src + off, sizeof(T));
  off += sizeof(T);
  return std::bit_cast<T>(b);
}

// runs on AsyncLogger
template <class... Args> void decode(const LogRecord& r, std::string& out) {
  std::size_t off = 0;
  // loads every argument back - brace init so order is guaranteed left to right
  std::tuple<Args...> args{load<Args>(r.args, off)...};
  // unpacks the tuple into parameter list
  std::apply(
      [&](const auto&... a) {
        // formats the parameters into out using the format string from record
        std::vformat_to(std::back_inserter(out), std::string_view{r.fmt, r.fmt_len},
                        std::make_format_args(a...));
      },
      args);
}

template <class T>
concept Loggable = std::is_trivially_copyable_v<T> && !std::is_pointer_v<T> && !std::is_array_v<T>;
} // namespace detail

// Producer-side channel with a SpscQueue
class LogChannel {
public:
  LogChannel(const char* name, std::size_t capacity) : name_(name), q_(capacity) {}

  // called by hot thread - must be light
  template <detail::Loggable... Args>
  void log(LogLevel lvl, std::format_string<Args...> fmt, const Args&... args) {
    static_assert((sizeof(Args) + ... + 0) <= sizeof(LogRecord::args), "too many log args");
    if (lvl < min_level_) return;
    LogRecord r;
    r.tsc = read_ticks();
    r.decode = &detail::decode<Args...>;
    r.fmt = fmt.get().data();
    r.fmt_len = uint16_t(fmt.get().size());
    r.level = lvl;
    detail::pack(r.args, args...);
    if (!q_.try_push(r)) {
      dropped_.fetch_add(1, std::memory_order_relaxed);
    }
  }

  // should be set before thread starts
  void set_min_level(LogLevel l) { min_level_ = l; }

private:
  friend class AsyncLogger;
  const char* name_;
  SpscQueue<LogRecord> q_;
  LogLevel min_level_{LogLevel::Info};
  std::atomic<uint64_t> dropped_{0};
  uint64_t dropped_reported_{0};
};

// Consumer-side logger connecting to every Logchannel.
class AsyncLogger {
public:
  explicit AsyncLogger(std::FILE* out) : out_(out) {}
  ~AsyncLogger() { stop(); }

  // called before start()
  LogChannel& channel(const char* name, std::size_t capacity = 4096);

  void start(int cpu = -1);
  // drain everything still queued, then join thread
  void stop();

private:
  void run();
  bool drain_once();
  void write(const LogRecord& r, const LogChannel& ch);
  void report_drops();

  std::FILE* out_;
  std::vector<std::unique_ptr<LogChannel>> channels_;
  uint64_t tsc0_{0}; // anchor for converting tsc to timeofday
  uint64_t tod0_ns_{0};
  // formatting buffer used for every line
  std::string buf_;
  std::atomic<bool> stop_{false}; // set by stop()
  std::thread thread_;
};

} // namespace ttt::core

// lets std::format print the wrapper types Lit and FixedStr
template <> struct std::formatter<ttt::core::Lit> : std::formatter<std::string_view> {
  auto format(ttt::core::Lit l, auto& ctx) const {
    return std::formatter<std::string_view>::format(l.s, ctx);
  }
};

template <std::size_t N>
struct std::formatter<ttt::core::FixedStr<N>> : std::formatter<std::string_view> {
  auto format(const ttt::core::FixedStr<N>& f, auto& ctx) const {
    return std::formatter<std::string_view>::format({f.data, f.len}, ctx);
  }
};
