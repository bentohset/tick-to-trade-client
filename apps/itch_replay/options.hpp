#pragma once

#include "apps/common/constants.hpp"
#include "core/format.hpp"
#include "feed/moldudp64/wire.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string_view>
#include <vector>

namespace itch_replay {

struct Options {
  std::string_view file;
  const char* group = apps::defaults::kGroup;
  uint16_t port = apps::defaults::kPort;
  const char* iface = apps::defaults::kIface;
  uint16_t rerequest_port = apps::defaults::kRerequestPort;
  ttt::mold::SessionId session = apps::defaults::kSession;
  // packets/s (0 = unpaced)
  uint64_t rate = 200'000;
  // stop at this ITCH timestamp
  std::optional<uint64_t> until_ns;
  uint64_t seed = 1;
  double drop_rate = 0;
  // drop the packet holding each seq (first send only)
  std::vector<uint64_t> drop_seqs;
  // keep answering re-requests after End-Of-Session
  int linger_s = 5;
};

inline void usage(const char* prog) {
  std::fprintf(
      stderr,
      "usage: %s <itch-file> [--group ADDR] [--port N] [--iface ADDR]\n"
      "       [--rerequest-port N] [--session ID] [--rate PPS] [--until HH:MM[:SS]]\n"
      "       [--seed N] [--drop-rate P] [--drop-seq N,...] [--linger SECONDS]\n"
      "  <itch-file>       Nasdaq ITCH 5.0 file to replay\n"
      "  --group           multicast group to send to (default 239.1.1.1)\n"
      "  --port            multicast port (default 30001)\n"
      "  --iface           local interface address for multicast IO (default 127.0.0.1)\n"
      "  --rerequest-port  port this process listens on for re-requests (default 30002)\n"
      "  --session         10-char session id, space padded (default TTT0000001)\n"
      "  --rate            packets/s, spin-paced; 0 = unpaced (default 200000)\n"
      "  --until           stop before the first message at/after this ITCH timestamp\n"
      "  --seed            PRNG seed for --drop-rate (default 1)\n"
      "  --drop-rate       probability [0,1] of dropping each packet, first send only (default 0)\n"
      "  --drop-seq        comma-separated seqs to drop, first send only, e.g. 100,205,9001\n"
      "  --linger          seconds to keep answering re-requests after End-Of-Session (default "
      "5)\n",
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
    } else if (arg == "--rerequest-port" && has_value) {
      if (!parse_u64(argv[++i], v)) return std::nullopt;
      o.rerequest_port = static_cast<uint16_t>(v);
    } else if (arg == "--session" && has_value) {
      const std::string_view s = argv[++i];
      o.session.fill(' '); // space-padded, same convention as itch::Symbol::from
      std::memcpy(o.session.data(), s.data(), std::min(s.size(), o.session.size()));
    } else if (arg == "--rate" && has_value) {
      if (!parse_u64(argv[++i], o.rate)) return std::nullopt;
    } else if (arg == "--until" && has_value) {
      o.until_ns = ttt::core::parse_time_of_day(argv[++i]); // reuse book_dump's parser
      if (!o.until_ns) return std::nullopt;
    } else if (arg == "--drop-rate" && has_value) {
      const char* val = argv[++i];
      char* end = nullptr;
      const double rate = std::strtod(val, &end);
      if (end == val || *end != '\0' || rate < 0 || rate > 1) return std::nullopt;
      o.drop_rate = rate;
    } else if (arg == "--drop-seq" && has_value) {
      std::string_view list = argv[++i];
      while (!list.empty()) {
        const auto comma = list.find(',');
        if (!parse_u64(list.substr(0, comma), v)) return std::nullopt;
        o.drop_seqs.push_back(v);
        if (comma == std::string_view::npos) break;
        list = list.substr(comma + 1);
      }
    } else if (arg == "--linger" && has_value) {
      if (!parse_u64(argv[++i], v)) return std::nullopt;
      o.linger_s = static_cast<int>(v);
    } else if (!arg.starts_with("-") && o.file.empty()) {
      o.file = arg;
    } else {
      return std::nullopt;
    }
  }
  if (o.file.empty()) return std::nullopt;
  return o;
}

} // namespace itch_replay
