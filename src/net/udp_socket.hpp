#pragma once

#include <cstdint>
#include <span>
#include <utility>
#include <netinet/in.h>
#include <unistd.h>

namespace ttt::net {

struct Endpoint {
  sockaddr_in addr{};
  static Endpoint from(const char* ip, uint16_t port);
};

class UdpSocket {
public:
  // udp multicast receiver client
  static UdpSocket multicast_rx(const char* group, uint16_t port, const char* iface_ip, int rcvbuf);
  // udp multicast transmitter server
  static UdpSocket multicast_tx(const char* iface_ip, bool loopback);
  // 1-to-1 udp connection
  static UdpSocket unicast(const char* ip, uint16_t port);

  UdpSocket(UdpSocket&& o) noexcept : fd_(std::exchange(o.fd_, -1)) {}
  UdpSocket& operator=(UdpSocket&& o) noexcept {
    if (this != &o) {
      if (fd_ > 0) close(fd_);
      fd_ = std::exchange(o.fd_, -1);
    }
    return *this;
  }
  ~UdpSocket() {
    if (fd_ >= 0) close(fd_);
  }

  // Non-blocking sockets: returns datagram size or 0 if nothing is waiting
  std::size_t recv(std::span<std::byte> buf, Endpoint* from = nullptr);
  void send_to(std::span<const std::byte> data, const Endpoint& to) { send_to(data, {}, to); }
  // Combines head and body into one datagram (sendmsg) - header from stack, body from mmap
  void send_to(std::span<const std::byte> head, std::span<const std::byte> body,
               const Endpoint& to);

private:
  explicit UdpSocket(int fd) : fd_(fd) {};
  int fd_{-1};
};

} // namespace ttt::net
