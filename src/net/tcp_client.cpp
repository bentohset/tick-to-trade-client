#include "net/tcp_client.hpp"
#include "net/endpoint.hpp"
#include "net/fd.hpp"
#include "net/helpers.hpp"
#include "net/socket_opts.hpp"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>

namespace ttt::net {

TcpClient TcpClient::connect(const char* ip, uint16_t port) {
  Fd fd = make_socket(SOCK_STREAM, false);
  const auto to = Endpoint::from(ip, port);
  if (::connect(fd.get(), reinterpret_cast<const sockaddr*>(&to.addr), sizeof to.addr) < 0) {
    fail("connect");
  }
  set_nodelay(fd.get());
  set_nonblocking(fd.get());
  return TcpClient(std::move(fd));
}

std::ptrdiff_t TcpClient::recv(std::span<std::byte> buf) {
  if (buf.empty()) return 0;
  const auto n = ::recv(fd_.get(), buf.data(), buf.size(), 0);
  if (n > 0) return n;
  if (n == 0) return -1; // shutdown by peer
  if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
  return -1;
}

std::ptrdiff_t TcpClient::send(std::span<const std::byte> data) {
  // MSG_NOSIGNAL: EPIPE instead of SIGPIPE killing the process
  const auto n = ::send(fd_.get(), data.data(), data.size(), MSG_NOSIGNAL);
  if (n >= 0) return n;
  if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
  return -1;
}

TcpListener TcpListener::listen(const char* ip, uint16_t port) {
  Fd fd = make_socket(SOCK_STREAM, true);
  set_reuseaddr(fd.get());
  const auto local = Endpoint::from(ip, port);
  if (bind(fd.get(), reinterpret_cast<const sockaddr*>(&local.addr), sizeof local.addr) < 0) {
    fail("bind");
  }
  if (::listen(fd.get(), 1) < 0) fail("listen");
  return TcpListener(std::move(fd));
}

std::optional<TcpClient> TcpListener::accept() {
  Fd fd(::accept(fd_.get(), nullptr, nullptr));
  if (fd.get() < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) return std::nullopt;
    fail("acccept");
  }
  set_nodelay(fd.get());
  set_nonblocking(fd.get());
  return TcpClient(std::move(fd));
}

} // namespace ttt::net
