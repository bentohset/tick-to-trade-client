#include "net/udp_socket.hpp"

#include <cerrno>
#include <cstddef>
#include <span>
#include <stdexcept>
#include <system_error>
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>

namespace ttt::net {

namespace {

[[noreturn]] void fail(const char* what) {
  throw std::system_error(errno, std::generic_category(), what);
}

int make_socket(bool nonblocking) {
  const int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) fail("socket");
  if (nonblocking && fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK) < 0) {
    close(fd);
    fail("fcntl");
  }
  return fd;
}

in_addr parse_ip(const char* ip) {
  in_addr a{};
  if (inet_pton(AF_INET, ip, &a) != 1) {
    throw std::invalid_argument(ip);
  }
  return a;
}

} // namespace

Endpoint Endpoint::from(const char* ip, uint16_t port) {
  Endpoint e;
  e.addr.sin_family = AF_INET;
  e.addr.sin_port = htons(port);
  e.addr.sin_addr = parse_ip(ip);
  return e;
}

UdpSocket UdpSocket::multicast_rx(const char* group, uint16_t port, const char* iface_ip,
                                  int rcvbuf) {
  UdpSocket s(make_socket(true));
  const int one = 1;
  setsockopt(s.fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  setsockopt(s.fd_, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof rcvbuf);
  const auto local = Endpoint::from(group, port);
  if (bind(s.fd_, reinterpret_cast<const sockaddr*>(&local.addr), sizeof local.addr) < 0) {
    fail("bind");
  }
  ip_mreq mreq{parse_ip(group), parse_ip(iface_ip)};
  if (setsockopt(s.fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof mreq)) {
    fail("IP_ADD_MEMBERSHIP");
  }
  return s;
}

UdpSocket UdpSocket::multicast_tx(const char* iface_ip, bool loopback) {
  UdpSocket s(make_socket(false));
  const in_addr ifc = parse_ip(iface_ip);
  const unsigned char loop = loopback, ttl = 1;
  if (setsockopt(s.fd_, IPPROTO_IP, IP_MULTICAST_IF, &ifc, sizeof ifc) < 0) {
    fail("IP_MULTICAST_IF");
  }
  setsockopt(s.fd_, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof loop);
  setsockopt(s.fd_, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof ttl);
  return s;
}

// Re-request socket. Server has fixed port it listens on.
// Client port 0 lets kernel pick any
UdpSocket UdpSocket::unicast(const char* ip, uint16_t port) {
  UdpSocket s(make_socket(true));
  const auto local = Endpoint::from(ip, port);
  if (bind(s.fd_, reinterpret_cast<const sockaddr*>(&local.addr), sizeof local.addr) < 0) {
    fail("bind");
  }
  return s;
}

std::size_t UdpSocket::recv(std::span<std::byte> buf, Endpoint* from) {
  socklen_t len = sizeof(sockaddr_in);
  const auto n =
      recvfrom(fd_, buf.data(), buf.size(), 0,
               from ? reinterpret_cast<sockaddr*>(&from->addr) : nullptr, from ? &len : nullptr);
  if (n >= 0) return static_cast<std::size_t>(n);
  if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
  fail("recvfrom");
}

void UdpSocket::send_to(std::span<const std::byte> head, std::span<const std::byte> body,
                        const Endpoint& to) {
  iovec iov[2] = {{const_cast<std::byte*>(head.data()), head.size()},
                  {const_cast<std::byte*>(body.data()), body.size()}};

  msghdr msg{};
  msg.msg_name = const_cast<sockaddr_in*>(&to.addr);
  msg.msg_namelen = sizeof to.addr;
  msg.msg_iov = iov;
  msg.msg_iovlen = body.empty() ? 1 : 2;
  // a full socket buffer on non-blocking socket means a lost datagram
  if (sendmsg(fd_, &msg, 0) < 0 && errno != EAGAIN && errno != ENOBUFS) {
    fail("sendmsg");
  }
}

} // namespace ttt::net
