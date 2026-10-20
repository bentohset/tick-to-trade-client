#pragma once

#include "net/helpers.hpp"

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

namespace ttt::net {

inline void set_nonblocking(int fd) {
  if (fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK) < 0) fail("fcntl");
}

inline void set_nodelay(int fd) {
  const int one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
}

inline void set_reuseaddr(int fd) {
  const int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
}

inline void set_rcvbuf(int fd, int bytes) {
  setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bytes, sizeof bytes);
}

} // namespace ttt::net
