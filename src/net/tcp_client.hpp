#pragma once

#include "net/fd.hpp"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <unistd.h>
namespace ttt::net {

// Non-blocking TCP stream with TCP_NODELAY.
class TcpClient {
public:
  // Blocking connect
  static TcpClient connect(const char* ip, uint16_t port);

  // returns >0 for bytes read, 0 if nothing waiting, -1 if peer closed or link broke
  // A dead TCP link is expected (reconnect), so it does not throw
  std::ptrdiff_t recv(std::span<std::byte> buf);
  // returns bytes written (short if the kernel buffer is full), -1 if broken
  std::ptrdiff_t send(std::span<const std::byte> data);

private:
  // let TcpListener use private member TcpClient ctor
  // during TcpListener::accept(), it needs to create a TcpClient
  friend class TcpListener;
  explicit TcpClient(Fd fd) : fd_(std::move(fd)) {}
  Fd fd_;
};

class TcpListener {
public:
  static TcpListener listen(const char* ip, uint16_t port);
  // non-blocking, nullopt if no pending connection
  std::optional<TcpClient> accept();

private:
  explicit TcpListener(Fd fd) : fd_(std::move(fd)) {}
  Fd fd_;
};

} // namespace ttt::net
