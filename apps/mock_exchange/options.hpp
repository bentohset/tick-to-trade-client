#pragma once

#include "apps/mock_exchange/exchange.hpp"

#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string_view>
#include <system_error>

namespace mock_exchange {

struct Options {
  ExchangeConfig cfg;
};

inline void usage(const char* prog) {
  std::fprintf(
      stderr,
      "usage: %s [--port N] [--session ID] [--user NAME] [--password PW]\n"
      "       [--fill-qty N] [--fill-every-us N] [--kill-after N] [--heartbeat-ms N]\n"
      "  --port           TCP port to listen on (default 40001)\n"
      "  --session        10-char SoupBinTCP session id, space padded (default OUCH000001)\n"
      "  --user           login username, up to 6 chars (default ttt)\n"
      "  --password       login password, up to 10 chars (default pw)\n"
      "  --fill-qty       shares per execution (default 100)\n"
      "  --fill-every-us  one execution per live order per interval (default 1000)\n"
      "  --kill-after     drop the client after N sequenced messages, once; 0 = never (default 0)\n"
      "  --heartbeat-ms   server heartbeat interval when idle (default 1000)\n"
      "signals: SIGUSR1 drops the current client connection, SIGINT/SIGTERM exit\n",
      prog);
}

inline std::optional<Options> parse_args(int argc, char** argv) {
  Options o;
  auto& c = o.cfg;
  const auto parse_u64 = [](std::string_view v, uint64_t& out) {
    const auto [end, ec] = std::from_chars(v.data(), v.data() + v.size(), out);
    return ec == std::errc{} && end == v.data() + v.size();
  };
  // space-padded alpha field, same convention as itch::Symbol::from.
  // false if the value doesn't fit, rather than silently truncating a credential
  const auto pad = [](std::string_view v, auto& field) {
    if (v.size() > field.size()) return false;
    field.fill(' ');
    std::memcpy(field.data(), v.data(), v.size());
    return true;
  };

  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    const bool has_value = i + 1 < argc;
    uint64_t v;
    if (arg == "--port" && has_value) {
      if (!parse_u64(argv[++i], v) || v > UINT16_MAX) return std::nullopt;
      c.port = static_cast<uint16_t>(v);
    } else if (arg == "--session" && has_value) {
      if (!pad(argv[++i], c.session)) return std::nullopt;
    } else if (arg == "--user" && has_value) {
      if (!pad(argv[++i], c.user)) return std::nullopt;
    } else if (arg == "--password" && has_value) {
      if (!pad(argv[++i], c.password)) return std::nullopt;
    } else if (arg == "--fill-qty" && has_value) {
      if (!parse_u64(argv[++i], v) || v == 0 || v > UINT32_MAX) return std::nullopt;
      c.fill_qty = static_cast<uint32_t>(v);
    } else if (arg == "--fill-every-us" && has_value) {
      if (!parse_u64(argv[++i], v)) return std::nullopt;
      c.fill_every_ns = v * 1'000;
    } else if (arg == "--kill-after" && has_value) {
      if (!parse_u64(argv[++i], c.kill_after)) return std::nullopt;
    } else if (arg == "--heartbeat-ms" && has_value) {
      if (!parse_u64(argv[++i], v) || v == 0) return std::nullopt;
      c.heartbeat_ns = v * 1'000'000;
    } else {
      return std::nullopt;
    }
  }
  return o;
}

} // namespace mock_exchange
