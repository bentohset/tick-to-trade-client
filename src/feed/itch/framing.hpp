#pragma once

#include "core/endian.hpp"

#include <cstddef>
#include <span>

namespace ttt::itch {

class FrameReader {
public:
  explicit FrameReader(std::span<const std::byte> buf) : buf_(buf) {}

  std::span<const std::byte> next() noexcept {
    if (buf_.size() - pos_ < 2) return {};
    const auto len = be::load<uint16_t>(buf_.data() + pos_);
    if (len == 0 || buf_.size() - pos_ - 2 < len) {
      truncated_ = pos_ != buf_.size();
      return {};
    }
    auto msg = buf_.subspan(pos_ + 2, len);
    pos_ += 2 + len;
    return msg;
  }

  bool truncated() const noexcept { return truncated_; }
  std::size_t offset() const noexcept { return pos_; }

private:
  std::span<const std::byte> buf_;
  std::size_t pos_ = 0;
  bool truncated_ = false;
};

} // namespace ttt::itch
