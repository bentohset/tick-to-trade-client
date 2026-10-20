#pragma once

#include "net/helpers.hpp"

#include <cstdint>
#include <netinet/in.h>

namespace ttt::net {

struct Endpoint {
  sockaddr_in addr{};
  static Endpoint from(const char* ip, uint16_t port) {
    Endpoint e;
    e.addr.sin_family = AF_INET;
    e.addr.sin_port = htons(port);
    e.addr.sin_addr = parse_ip(ip);
    return e;
  }
};

} // namespace ttt::net
