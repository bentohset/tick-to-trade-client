#pragma once

#include "apps/common/constants.hpp"
#include "feed/moldudp64/wire.hpp"

#include <charconv>
#include <cstdint>
#include <cstdio>
#include <string_view>

namespace feed_rx {

struct Options {
  const char* group = apps::defaults::kGroup;
  uint16_t port = apps::defaults::kPort;
  const char* iface = apps::defaults::kIface;
  const char* rerequest_host = apps::defaults::kIface; // itch_replay's rr_ socket binds to own iface
      uint16_t rerequest_port = apps::defaults::kRerequestPort;
  ttt::mold::SessionId session = apps::defaults::kSession;
  int rcvbuf_bytes = 1 << 20;
  std::size_t buffer_capacity = 4096; // PacketBuffer slots
  uint64_t recovery_timeout_ns = 20'000'000;
  uint32_t max_retries = 10;
  std::string_view symbol; // --symbol: print this book at the end
  std::size_t depth = 10;
};

inline void usage(const char* prog) {
  std::fprintf(
      stderr,
      "usage: %s [--group ADDR] [--port N] [--iface ADDR]\n"
      "       [--rerequest-host ADDR] [--rerequest-port N] [--session ID]\n"
      "       [--symbol SYM] [--depth N] [--timeout-ms N] [--max-retries N]\n"
      "  --group            multicast group to join (default 239.1.1.1)\n"
      "  --port             multicast port (default 30001)\n"
      "  --iface            local interface address (default 127.0.0.1)\n"
      "  --rerequest-host   itch_replay's re-request responder address (default 127.0.0.1)\n"
      "  --rerequest-port   itch_replay's re-request responder port (default 30002)\n"
      "  --session          10-char session id, must match the sender (default TTT0000001)\n"
      "  --symbol           print this symbol's book at the end\n"
      "  --depth            levels per side to print (default 10)\n"
      "  --timeout-ms       recovery retry timeout (default 20)\n"
      "  --max-retries      recovery retries before giving up (default 10)\n",
      prog);
}

inline std::optional<Options> parse_args(int argc, char** argv) {
  Options o;
  const auto parse_u64 = [](std::string_view v, uint64_t& out) {
    const auto [end, ec] = std::from_chars(v.data(), v.data() + v.size(), out);
    return ec == std::errc{} && end == v.data() + v.size();
  };

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
    } else {
      return std::nullopt;
    }
  }
  return o;
}

} // namespace feed_rx
