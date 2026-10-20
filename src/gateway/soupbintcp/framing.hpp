#pragma once

#include "core/endian.hpp"
#include "gateway/soupbintcp/wire.hpp"
#include "net/tcp_client.hpp"

#include <array>
#include <cstddef>
#include <cstring>
#include <span>

namespace ttt::soup {

// Collects received bytes from TCP stream and yields whole packets.
// packets can be split across recv() calls, so leftovers are kept for next one
class RecvBuffer {
public:
  std::span<std::byte> tail() { return {buf_.data() + end_, buf_.size() - end_}; }
  void commit(std::size_t n) { end_ += n; }
  void reset() { end_ = 0; }

  // F(type, payload) returns boolean denoting keep_going, called once per complete packet.
  // Any partial packet is moved to the front
  // returns false if framing error
  template <class F> bool drain(F&& f) {
    std::size_t pos = 0;
    bool ok = true;
    while (end_ - pos >= 2) {
      // type + payload
      const uint16_t len = be::load<uint16_t>(buf_.data() + pos);
      if (len == 0) {
        ok = false;
        break;
      }
      // partial, wait for more bytes
      if (end_ - pos < 2u + len) break;
      const auto type = static_cast<char>(buf_[pos + 2]);
      const std::span<const std::byte> payload(buf_.data() + pos + kHeaderSize, len - 1u);
      pos += 2u + len;
      if (!f(type, payload)) break;
    }
  }

private:
  // >= 2 + 0xFFFF so any legal packet fits
  std::array<std::byte, 1 << 17> buf_;
  std::size_t end_ = 0;
};

// Handles bytes the kernel has not taken. Packets encoded straight in here
class SendBuffer {
public:
  std::byte* reserve(std::size_t n) {
    if (buf_.size() - end_ < n) compact();
    return buf_.size() - end_ < n ? nullptr : buf_.data() + end_;
  }
  void commit(std::size_t n) { end_ += n; }
  void reset() { head_ = end_ = 0; }
  bool empty() const { return head_ == end_; }

  bool flush(net::TcpClient& s) {
    while (head_ < end_) {
      const auto n = s.send({buf_.data() + head_, end_ - head_});
      if (n < 0) return false;
      if (n == 0) break; // kernel buffer is full
      head_ += static_cast<std::size_t>(n);
    }
    if (head_ == end_) head_ = end_ = 0;
    return true;
  }

private:
  void compact() {
    std::memmove(buf_.data(), buf_.data() + head_, end_ - head_);
    end_ -= head_;
    head_ = 0;
  }
  std::array<std::byte, 1 << 16> buf_;
  std::size_t head_ = 0, end_ = 0;
};

// header + enc(payload) put into SendBuffer
// returns false if no room
template <class Enc> bool put_packet(SendBuffer& tx, char type, std::size_t len, Enc&& enc) {
  std::byte* p = tx.reserve(kHeaderSize + len);
  if (!p) return false;
  be::store(p, static_cast<uint16_t>(len + 1));
  p[2] = static_cast<std::byte>(type);
  enc(p + kHeaderSize);
  tx.commit(kHeaderSize + len);
  return true;
}

} // namespace ttt::soup
