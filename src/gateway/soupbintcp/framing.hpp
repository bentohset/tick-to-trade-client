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
// The buffer always starts at index 0. Everything before end_ is data waiting to be parsed;
// Everything after end_ to size() is free space.
class RecvBuffer {
public:
  // returns the free region, kenerl copies bytes directly to this
  std::span<std::byte> tail() { return {buf_.data() + end_, buf_.size() - end_}; }
  // after recv() returns n bytes, mark them as part of the unparsed data
  void commit(std::size_t n) { end_ += n; }
  void reset() { end_ = 0; }

  // F(type, payload) returns boolean denoting keep_going, called once per complete packet.
  // Any partial packet is moved to the front.
  // returns false if framing error
  template <class F> bool drain(F&& f) {
    std::size_t pos = 0; // read cursor
    bool ok = true;
    // loop while there are at least 2 unparsed bytes, enough to read a length
    while (end_ - pos >= 2) {
      // type + payload
      const uint16_t len = be::load<uint16_t>(buf_.data() + pos);
      if (len == 0) {
        ok = false;
        break;
      }
      // if whole packet received (2 length bytes + len bytes) denotes
      // partial, wait for more bytes
      if (end_ - pos < 2u + len) break;
      const auto type = static_cast<char>(buf_[pos + 2]);
      const std::span<const std::byte> payload(buf_.data() + pos + kHeaderSize, len - 1u);
      pos += 2u + len;
      // hand the packet to the session
      if (!f(type, payload)) break;
    }
    // slide whatever wasn't consumed to the front of the buffer
    // usually this will move 0 bytes because everything was consumed
    // TODO: probably can optimize this sliding
    std::memmove(buf_.data(), buf_.data() + pos, end_ - pos);
    end_ -= pos;
    return ok;
  }

private:
  // fixed array so never allocates
  // 128KiB as largest legal packet is 2+65535 bytes. so after drain, at most one
  // partial packet is left at the front, leaving enough room for the rest
  std::array<std::byte, 1 << 17> buf_;
  // bytes [0, end_) are received but not consumed yet
  std::size_t end_ = 0;
};

// Handles bytes the kernel has not taken. Packets encoded straight in here
// buf_: [ already sent | waiting for the kernel | free space ]
//       0            head_                     end_       size()
// [head_, end_) holds bytes encoded but not yet accepted by send()
// when everythin gis sent, both head_ and end_ goes back to 0
class SendBuffer {
public:
  // caller should call reserve before commit
  // returns a pointer where the caller can write n bytes
  std::byte* reserve(std::size_t n) {
    // check if there is enough room for n bytes after end_
    // if not, make room by sliding unsent bytes to the front to reclaim already-sent
    if (buf_.size() - end_ < n) compact();
    // still no room after compact means 64KiB of unsent data piled up
    // peer not reading or link is dead
    return buf_.size() - end_ < n ? nullptr : buf_.data() + end_;
  }

  // reserve doesnt move end, commit does
  void commit(std::size_t n) { end_ += n; }
  void reset() { head_ = end_ = 0; }
  bool empty() const { return head_ == end_; }

  bool flush(net::TcpClient& s) {
    // loop while there are unsent bytes
    while (head_ < end_) {
      // offer all unsent bytes to kernel - may be fewer than offered
      const auto n = s.send({buf_.data() + head_, end_ - head_});
      if (n < 0) return false; // connection broken
      if (n == 0) break;       // kernel buffer is full
      head_ += static_cast<std::size_t>(n);
    }
    // rewind to start if everything went out to empty the buffer
    if (head_ == end_) head_ = end_ = 0;
    return true;
  }

private:
  // reclaims sent region in front of head_
  void compact() {
    std::memmove(buf_.data(), buf_.data() + head_, end_ - head_);
    end_ -= head_;
    head_ = 0;
  }
  // 64 KiB
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
