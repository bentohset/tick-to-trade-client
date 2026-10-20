#pragma once

#include "net/fd.hpp"

#include <system_error>
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace ttt::net {

[[noreturn]] inline void fail(const char* what) {
  throw std::system_error(errno, std::generic_category(), what);
}

inline void set_nonblocking(int fd) {
  if (fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK) < 0) fail("fcntl");
}

inline Fd make_socket(int type, bool nonblocking) {
  Fd fd(socket(AF_INET, type, 0));
  if (fd.get() < 0) fail("socket");
  if (nonblocking) set_nonblocking(fd.get());
  return fd;
}

inline in_addr parse_ip(const char* ip) {
  in_addr a{};
  if (inet_pton(AF_INET, ip, &a) != 1) {
    throw std::invalid_argument(ip);
  }
  return a;
}

} // namespace ttt::net
