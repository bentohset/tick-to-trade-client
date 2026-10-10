#include "apps/common/book_print.hpp"
#include "core/clock.hpp"
#include "feed/book/book_manager.hpp"
#include "feed/book/order_book.hpp"
#include "feed/itch/parser.hpp"
#include "feed/itch/symbol_directory.hpp"
#include "feed/moldudp64/recovery.hpp"
#include "feed/moldudp64/session.hpp"
#include "feed/moldudp64/types.hpp"
#include "feed/moldudp64/wire.hpp"
#include "net/udp_socket.hpp"
#include "options.hpp"
#include <cstdint>
#include <span>
namespace {

// Both SymbolDirectory and BookManager derive from itch::NullHandler, so one
// on() forwards to each — same pattern as book_dump's detail::BookRun.
struct BookRun : ttt::itch::NullHandler {
  ttt::itch::SymbolDirectory dir;
  ttt::book::BookManager books;
  uint64_t bad_length = 0;

  template <class Msg> void on(const Msg& m) {
    dir.on(m);
    books.on(m);
  }
};

} // namespace

int main(int argc, char** argv) {
  const auto opts = feed_rx::parse_args(argc, argv);
  if (!opts) {
    feed_rx::usage(argv[0]);
    return 2;
  }

  auto rx =
      ttt::net::UdpSocket::multicast_rx(opts->group, opts->port, opts->iface, opts->rcvbuf_bytes);
  auto rr = ttt::net::UdpSocket::unicast(opts->iface, 0);
  const auto rerequest_to = ttt::net::Endpoint::from(opts->rerequest_host, opts->rerequest_port);

  ttt::mold::Session session(1, opts->buffer_capacity);
  ttt::mold::Recovery recovery(opts->recovery_timeout_ns, opts->max_retries);

  BookRun run;
  uint64_t applied = 0;
  const auto sink = [&](uint64_t, std::span<const std::byte> msg) {
    if (ttt::itch::parse(msg, run) == ttt::itch::ParseResult::BadLength) ++run.bad_length;
    ++applied;
  };

  std::array<std::byte, ttt::mold::kMaxDatagram> buf;
  while (session.state() == ttt::mold::FeedState::Live ||
         session.state() == ttt::mold::FeedState::Recovering) {
    if (const auto n = rx.recv(buf); n > 0) {
      session.on_packet({buf.data(), n}, sink);
    }

    if (session.state() == ttt::mold::FeedState::Recovering) {
      ttt::net::Endpoint from;
      if (const auto n = rr.recv(buf, &from); n > 0) {
        session.on_packet({buf.data(), n}, sink);
      }

      const uint64_t now_ns = static_cast<uint64_t>(static_cast<double>(ttt::core::read_ticks()) *
                                                    ttt::core::ns_per_tick());
      if (const auto req = recovery.poll(session, now_ns)) {
        std::fprintf(stderr, "sending rerequest: [%llu, %llu) to %s:%u\n",
                     static_cast<unsigned long long>(req->first),
                     static_cast<unsigned long long>(req->end), opts->rerequest_host,
                     opts->rerequest_port);
        std::array<std::byte, ttt::mold::kHeaderSize> hdr;
        ttt::mold::write_header(hdr.data(), opts->session, req->first,
                                static_cast<uint16_t>(req->end - req->first));
        rr.send_to(hdr, rerequest_to);
      }
      if (recovery.timed_out()) {
        std::fprintf(stderr, "error: recovery timed out, stuck at seq %llu\n",
                     static_cast<unsigned long long>(session.expected()));
        return 1;
      }
    }
  }

  const auto& st = session.stats();
  std::printf("%-22s %s\n", "feed state",
              session.state() == ttt::mold::FeedState::Ended ? "Ended" : "Failed");
  std::printf("%-22s %llu\n", "gaps recovered", static_cast<unsigned long long>(st.gaps));
  std::printf("%-22s %llu\n", "buffer full", static_cast<unsigned long long>(st.buffer_full));
  std::printf("%-22s %zu / %zu\n", "peak buffered", session.peak_buffered(),
              session.buffer_capacity());
  std::printf("%-22s %llu\n", "duplicates", static_cast<unsigned long long>(st.duplicates));
  std::printf("%-22s %llu\n", "bad length", static_cast<unsigned long long>(run.bad_length));

  const bool ok = apps::print_check(run.books.errors(), run.books.live_orders(), applied,
                                    session.state() == ttt::mold::FeedState::Ended) &&
                  run.bad_length == 0 && session.state() == ttt::mold::FeedState::Ended;

  if (!opts->symbol.empty()) {
    const auto locate = run.dir.locate_of(opts->symbol);
    const ttt::book::OrderBook* book = locate ? run.books.book(*locate) : nullptr;
    if (!book) {
      std::fprintf(stderr, "error: symbol %.*s not found\n", static_cast<int>(opts->symbol.size()),
                   opts->symbol.data());
      return 1;
    }
    std::printf("\n");
    apps::print_book(opts->symbol, *locate, *book, opts->depth);
  }

  return ok ? 0 : 1;
}
