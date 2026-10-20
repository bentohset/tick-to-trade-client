#pragma once

#include <utility>
#include <unistd.h>
namespace ttt::net {

// RAII wrapper for a single file descriptor
class Fd {
public:
  Fd() = default;
  explicit Fd(int fd) : fd_(fd) {}
  Fd(Fd&& o) noexcept : fd_(std::exchange(o.fd_, -1)) {}
  Fd& operator=(Fd&& o) noexcept {
    if (this != &o) {
      reset();
      fd_ = std::exchange(o.fd_, -1);
    }
    return *this;
  }
  ~Fd() { reset(); }

  int get() const { return fd_; }
  void reset() {
    if (fd_ >= 0) close(std::exchange(fd_, -1));
  }

private:
  int fd_{-1};
};

} // namespace ttt::net
