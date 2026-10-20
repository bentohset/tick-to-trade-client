#pragma once

#include "net/endpoint.hpp"
#include "net/fd.hpp"
#include <cstdint>
#include <span>
#include <utility>
#include <netinet/in.h>
#include <unistd.h>

namespace ttt::net {

class UdpSocket {
public:
  // udp multicast receiver client
  static UdpSocket multicast_rx(const char* group, uint16_t port, const char* iface_ip, int rcvbuf);
  // udp multicast transmitter server
  static UdpSocket multicast_tx(const char* iface_ip, bool loopback);
  // 1-to-1 udp connection
  static UdpSocket unicast(const char* ip, uint16_t port);

  // Non-blocking sockets: returns datagram size or 0 if nothing is waiting
  std::size_t recv(std::span<std::byte> buf, Endpoint* from = nullptr);
  void send_to(std::span<const std::byte> data, const Endpoint& to) { send_to(data, {}, to); }
  // Combines head and body into one datagram (sendmsg) - header from stack, body from mmap
  void send_to(std::span<const std::byte> head, std::span<const std::byte> body,
               const Endpoint& to);

private:
  explicit UdpSocket(Fd fd) : fd_(std::move(fd)) {};
  Fd fd_;
};

} // namespace ttt::net
