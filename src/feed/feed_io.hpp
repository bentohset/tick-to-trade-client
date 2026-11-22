#pragma once

#include "net/udp_socket.hpp"

#include <concepts>
#include <cstddef>
#include <span>

namespace ttt::feed {

// network operations that all feed handlers need
template <class T>
concept FeedIo = requires(T io, std::span<std::byte> buf, std::span<const std::byte> req) {
  { io.recv_feed(buf) } -> std::same_as<std::size_t>;
  { io.recv_rerequest(buf) } -> std::same_as<std::size_t>;
  io.send_rerequest(req);
};

struct UdpFeedIo {
  net::UdpSocket rx, rr;
  net::Endpoint rerequest_to;

  std::size_t recv_feed(std::span<std::byte> b) { return rx.recv(b); }
  std::size_t recv_rerequest(std::span<std::byte> b) { return rr.recv(b); }
  void send_rerequest(std::span<const std::byte> req) { rr.send_to(req, rerequest_to); }
};

} // namespace ttt::feed
