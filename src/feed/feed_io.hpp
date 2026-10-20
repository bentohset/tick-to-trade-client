#pragma once

#include "feed/book/types.hpp"

#include <cstdint>

namespace ttt::feed {

// Message from FeedHandler to upstream consumer denoting
// "this symbol's best bid and best ask are now these".
struct BookUpdate {
  uint64_t rx_tsc; // when the packet arrived
  uint64_t seq;    // MoldUDP64 sequence of the message that caused it
  uint64_t bid_qty, ask_qty;
  book::Price bid_px, ask_px;
  uint16_t locate; // which symbol
};

// rule for anything that can receive book updates
template <class S>
concept BookUpdateSink = requires(S s, const BookUpdate& u) { s.on_update(u); };

} // namespace ttt::feed
