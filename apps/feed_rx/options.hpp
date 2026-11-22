#pragma once

#include "apps/common/constants.hpp"
#include "core/async_logger.hpp"
#include "feed/moldudp64/wire.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string_view>

namespace feed_rx {

enum class Mode : uint8_t { Single, Pipeline };

struct Options {
  // network and session
  const char* group = apps::defaults::kGroup;
  uint16_t port = apps::defaults::kPort;
  const char* iface = apps::defaults::kIface;
  const char* rerequest_host =
      apps::defaults::kIface; // itch_replay's rr_ socket binds to own iface
  uint16_t rerequest_port = apps::defaults::kRerequestPort;
  ttt::mold::SessionId session = apps::defaults::kSession;
  int rcvbuf_bytes = 1 << 20;
  std::size_t buffer_capacity = 4096; // PacketBuffer slots
  uint64_t recovery_timeout_ns = 20'000'000;
  uint32_t max_retries = 10;

  // threading
  Mode mode = Mode::Single;
  std::size_t queue_capacity = 4096; // pipeline only
  // pin cpu for components, -1 == no pin
  int cpu_feed = -1;
  int cpu_consumer = -1;
  int cpu_logger = -1;

  // feed health and logging
  uint64_t stale_ms = 2000;
  ttt::core::LogLevel log_level = ttt::core::LogLevel::Info;
  const char* log_file = nullptr; // == stderr

  // output
  std::string_view symbol; // --symbol: print this book at the end
  std::size_t depth = 10;
};

inline void usage(const char* prog) {
  std::fprintf(
      stderr,
      "usage: %s [--group ADDR] [--port N] [--iface ADDR]\n"
      "       [--rerequest-host ADDR] [--rerequest-port N] [--session ID]\n"
      "       [--symbol SYM] [--depth N] [--timeout-ms N] [--max-retries N]\n"
      "       [--buffer-capacity N] [--rcvbuf BYTES]\n"
      "       [--mode single|pipeline] [--queue-capacity N]\n"
      "       [--cpu-feed N] [--cpu-consumer N] [--cpu-logger N]\n"
      "       [--stale-ms N] [--log-level LEVEL] [--log-file PATH]\n"
      "  --group            multicast group to join (default 239.1.1.1)\n"
      "  --port             multicast port (default 30001)\n"
      "  --iface            local interface address (default 127.0.0.1)\n"
      "  --rerequest-host   itch_replay's re-request responder address (default 127.0.0.1)\n"
      "  --rerequest-port   itch_replay's re-request responder port (default 30002)\n"
      "  --session          10-char session id, must match the sender (default TTT0000001)\n"
      "  --symbol           subscribe to this symbol: measure its update latency and print\n"
      "                     its book at the end\n"
      "  --depth            levels per side to print (default 10)\n"
      "  --timeout-ms       recovery retry timeout (default 20)\n"
      "  --max-retries      recovery retries before giving up (default 10)\n"
      "  --buffer-capacity  PacketBuffer slots for out-of-order packets during a gap "
      "(default 4096)\n"
      "  --rcvbuf           multicast socket receive buffer in bytes (default 1048576)\n"
      "  --mode             single: one thread does everything (default)\n"
      "                     pipeline: feed thread -> SPSC queue -> consumer thread;\n"
      "                     needs --symbol\n"
      "  --queue-capacity   SPSC queue slots, pipeline only (default 4096)\n"
      "  --cpu-feed         pin the feed thread to this CPU (Linux only)\n"
      "  --cpu-consumer     pin the consumer thread to this CPU, pipeline only\n"
      "  --cpu-logger       pin the logger thread to this CPU\n"
      "  --stale-ms         mark the feed stale after this much silence (default 2000)\n"
      "  --log-level        debug, info, warn or error (default info)\n"
      "  --log-file         write the log here instead of stderr\n",
      prog);
}

inline std::optional<Options> parse_args(int argc, char** argv) {
  Options o;
  const auto parse_u64 = [](std::string_view v, uint64_t& out) {
    const auto [end, ec] = std::from_chars(v.data(), v.data() + v.size(), out);
    return ec == std::errc{} && end == v.data() + v.size();
  };

  const auto parse_cpu = [&](std::string_view v, int& out) {
    uint64_t n;
    if (!parse_u64(v, n) || n > 4096) return false;
    out = static_cast<int>(n);
    return true;
  };
  // Pipeline-only flags given with --mode single are an error, not silently ignored:
  // a benchmark run shouldn't measure a different setup from the one asked for
  bool pipeline_only_flag = false;

  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    const bool has_value = i + 1 < argc;
    uint64_t v;
    if (arg == "--group" && has_value) {
      o.group = argv[++i];
    } else if (arg == "--port" && has_value) {
      if (!parse_u64(argv[++i], v)) return std::nullopt;
      o.port = static_cast<uint16_t>(v);
    } else if (arg == "--iface" && has_value) {
      o.iface = argv[++i];
    } else if (arg == "--rerequest-host" && has_value) {
      o.rerequest_host = argv[++i];
    } else if (arg == "--rerequest-port" && has_value) {
      if (!parse_u64(argv[++i], v)) return std::nullopt;
      o.rerequest_port = static_cast<uint16_t>(v);
    } else if (arg == "--session" && has_value) {
      const std::string_view s = argv[++i];
      o.session.fill(' ');
      std::memcpy(o.session.data(), s.data(), std::min(s.size(), o.session.size()));
    } else if (arg == "--symbol" && has_value) {
      o.symbol = argv[++i];
    } else if (arg == "--depth" && has_value) {
      if (!parse_u64(argv[++i], v)) return std::nullopt;
      o.depth = static_cast<std::size_t>(v);
    } else if (arg == "--timeout-ms" && has_value) {
      if (!parse_u64(argv[++i], v)) return std::nullopt;
      o.recovery_timeout_ns = v * 1'000'000;
    } else if (arg == "--max-retries" && has_value) {
      if (!parse_u64(argv[++i], v)) return std::nullopt;
      o.max_retries = static_cast<uint32_t>(v);
    } else if (arg == "--buffer-capacity" && has_value) {
      if (!parse_u64(argv[++i], v)) return std::nullopt;
      o.buffer_capacity = static_cast<std::size_t>(v);
    } else if (arg == "--rcvbuf" && has_value) {
      if (!parse_u64(argv[++i], v)) return std::nullopt;
      o.rcvbuf_bytes = static_cast<int>(v);
    } else if (arg == "--mode" && has_value) {
      const std::string_view m = argv[++i];
      if (m == "single") {
        o.mode = Mode::Single;
      } else if (m == "pipeline") {
        o.mode = Mode::Pipeline;
      } else {
        return std::nullopt;
      }
    } else if (arg == "--queue-capacity" && has_value) {
      if (!parse_u64(argv[++i], v) || v == 0) return std::nullopt;
      o.queue_capacity = static_cast<std::size_t>(v);
      pipeline_only_flag = true;
    } else if (arg == "--cpu-feed" && has_value) {
      if (!parse_cpu(argv[++i], o.cpu_feed)) return std::nullopt;
    } else if (arg == "--cpu-consumer" && has_value) {
      if (!parse_cpu(argv[++i], o.cpu_consumer)) return std::nullopt;
      pipeline_only_flag = true;
    } else if (arg == "--cpu-logger" && has_value) {
      if (!parse_cpu(argv[++i], o.cpu_logger)) return std::nullopt;
    } else if (arg == "--stale-ms" && has_value) {
      if (!parse_u64(argv[++i], v)) return std::nullopt;
      o.stale_ms = v;
    } else if (arg == "--log-level" && has_value) {
      const std::string_view l = argv[++i];
      using ttt::core::LogLevel;
      if (l == "debug") {
        o.log_level = LogLevel::Debug;
      } else if (l == "info") {
        o.log_level = LogLevel::Info;
      } else if (l == "warn") {
        o.log_level = LogLevel::Warn;
      } else if (l == "error") {
        o.log_level = LogLevel::Error;
      } else {
        return std::nullopt;
      }
    } else if (arg == "--log-file" && has_value) {
      o.log_file = argv[++i];
    } else {
      return std::nullopt;
    }
  }

  if (o.mode == Mode::Single && pipeline_only_flag) {
    std::fprintf(stderr, "error: --queue-capacity and --cpu-consumer need --mode pipeline\n");
    return std::nullopt;
  }
  // The consumer only receives updates for a subscribed symbol: without one it would
  // measure nothing
  if (o.mode == Mode::Pipeline && o.symbol.empty()) {
    std::fprintf(stderr, "error: --mode pipeline needs --symbol\n");
    return std::nullopt;
  }
  return o;
}

} // namespace feed_rx
