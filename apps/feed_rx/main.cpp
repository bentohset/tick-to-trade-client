#include "apps/common/book_print.hpp"
#include "core/async_logger.hpp"
#include "core/clock.hpp"
#include "core/cpu_affinity.hpp"
#include "core/format.hpp"
#include "core/latency_histogram.hpp"
#include "core/spsc_queue.hpp"
#include "feed/book/order_book.hpp"
#include "feed/book_update.hpp"
#include "feed/feed_handler.hpp"
#include "feed/feed_io.hpp"
#include "net/udp_socket.hpp"
#include "options.hpp"

#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <thread>

namespace {

using ttt::feed::BookUpdate;
using ttt::feed::FeedHealth;

// Set by Ctrl-C / SIGTERM. Loops stop, threads join and results still print.
std::atomic<bool> g_stop{false};
extern "C" void on_signal(int) { g_stop.store(true, std::memory_order_relaxed); }

// Stand-in for the strategy: measures how long after its packet arrived each update
// is seen. Same type in both modes, so single vs pipeline differ only by the queue hop.
struct UpdateConsumer {
  ttt::core::LatencyHistogram latency;
  void on_update(const BookUpdate& u) {
    latency.record(ttt::core::ticks_to_ns(ttt::core::read_ticks() - u.rx_tsc));
  }
};

// Pipeline producer side: never blocks the feed thread
struct QueueSink {
  ttt::core::SpscQueue<BookUpdate>& q;
  uint64_t full = 0;
  void on_update(const BookUpdate& u) {
    // TODO: coalesce per symbol instead of dropping, so the consumer's view can't go stale
    if (!q.try_push(u)) ++full;
  }
};

// What a run reports besides the handler's own state
struct RunInfo {
  bool interrupted = false;
  uint64_t queue_full = 0; // pipeline only
};

void pin(int cpu, const char* who) {
  if (cpu >= 0 && !ttt::core::pin_this_thread(cpu)) {
    std::fprintf(stderr, "warning: could not pin %s thread to cpu %d\n", who, cpu);
  }
}

ttt::feed::FeedConfig feed_config(const feed_rx::Options& o) {
  ttt::feed::FeedConfig c;
  c.session = o.session;
  c.start_seq = 1;
  c.buffer_capacity = o.buffer_capacity;
  c.recovery_timeout_ns = o.recovery_timeout_ns;
  c.max_retries = o.max_retries;
  c.stale_after_ns = o.stale_ms * 1'000'000;
  return c;
}

// true if stopped by a signal rather than the feed ending or failing
template <class Handler> bool run_loop(Handler& feed) {
  while (!g_stop.load(std::memory_order_relaxed) && feed.poll()) {}
  return g_stop.load(std::memory_order_relaxed);
}

const char* health_name(FeedHealth h) {
  switch (h) {
    case FeedHealth::Live: return "Live";
    case FeedHealth::Recovering: return "Recovering";
    case FeedHealth::Stale: return "Stale";
    case FeedHealth::Ended: return "Ended";
    case FeedHealth::Failed: return "Failed";
  }
  return "?";
}

template <class Handler>
bool print_results(const feed_rx::Options& o, const Handler& feed, const UpdateConsumer& consumer,
                   const RunInfo& info) {
  const auto& session = feed.session();
  const auto& st = session.stats();
  const auto& fs = feed.stats();
  const auto u64 = [](uint64_t v) { return static_cast<unsigned long long>(v); };
  // first to last packet: idle time before the replay starts isn't counted
  const double elapsed_ns =
      fs.first_rx_tsc
          ? static_cast<double>(ttt::core::ticks_to_ns(fs.last_rx_tsc - fs.first_rx_tsc))
          : 0.0;
  const bool ended = feed.health() == FeedHealth::Ended;

  std::printf("%-22s %s\n", "mode", o.mode == feed_rx::Mode::Pipeline ? "pipeline" : "single");
  std::printf("%-22s %s%s\n", "feed state", health_name(feed.health()),
              info.interrupted ? " (interrupted)" : "");
  std::printf("%-22s %llu\n", "gaps recovered", u64(st.gaps));
  std::printf("%-22s %llu\n", "rerequests", u64(fs.rerequests));
  std::printf("%-22s %llu\n", "buffer full", u64(st.buffer_full));
  std::printf("%-22s %zu / %zu\n", "peak buffered", session.peak_buffered(),
              session.buffer_capacity());
  std::printf("%-22s %llu\n", "duplicates", u64(st.duplicates));
  std::printf("%-22s %llu\n", "bad length", u64(fs.bad_length));
  std::printf("%-22s ", "time taken");
  ttt::core::print_duration(stdout, elapsed_ns);
  std::printf("\n%-22s ", "time per message");
  ttt::core::print_duration(stdout,
                            fs.messages ? elapsed_ns / static_cast<double>(fs.messages) : 0.0);
  std::printf("\n");

  if (!o.symbol.empty()) {
    const auto& lat = consumer.latency;
    std::printf("%-22s %llu\n", "book updates", u64(fs.updates));
    if (o.mode == feed_rx::Mode::Pipeline) {
      std::printf("%-22s %llu\n", "queue full", u64(info.queue_full));
    }
    // Updates from packets held during a gap include the wait for the fill
    std::printf("%-22s p50 %llu  p99 %llu  p99.9 %llu  max %llu ns (%s, %llu samples)\n",
                "rx -> consumer", u64(lat.percentile(0.5)), u64(lat.percentile(0.99)),
                u64(lat.percentile(0.999)), u64(lat.max()), ttt::core::tick_source(),
                u64(lat.count()));
  }

  const bool ok = apps::print_check(feed.books().errors(), feed.books().live_orders(), fs.messages,
                                    ended) &&
                  fs.bad_length == 0 && ended && info.queue_full == 0;

  if (!o.symbol.empty()) {
    const auto locate = feed.directory().locate_of(o.symbol);
    const ttt::book::OrderBook* book = locate ? feed.books().book(*locate) : nullptr;
    if (!book) {
      std::fprintf(stderr, "error: symbol %.*s not found\n", static_cast<int>(o.symbol.size()),
                   o.symbol.data());
      return false;
    }
    std::printf("\n");
    apps::print_book(o.symbol, *locate, *book, o.depth);
  }
  return ok;
}

// --mode single: this thread runs the feed and calls the consumer inline
template <class Report>
RunInfo run_single(const feed_rx::Options& o, ttt::feed::UdpFeedIo& io,
                   ttt::core::LogChannel& log, UpdateConsumer& consumer, Report&& report) {
  ttt::feed::FeedHandler<UpdateConsumer> feed(feed_config(o), io, consumer, log);
  if (!o.symbol.empty()) feed.subscribe(o.symbol);
  pin(o.cpu_feed, "feed");

  RunInfo info;
  info.interrupted = run_loop(feed);
  report(feed, info);
  return info;
}

// --mode pipeline: T1 runs the feed and pushes updates; T2 drains them into the consumer
template <class Report>
RunInfo run_pipeline(const feed_rx::Options& o, ttt::feed::UdpFeedIo& io,
                     ttt::core::LogChannel& log, UpdateConsumer& consumer, Report&& report) {
  ttt::core::SpscQueue<BookUpdate> q(o.queue_capacity);
  QueueSink sink{q};
  ttt::feed::FeedHandler<QueueSink> feed(feed_config(o), io, sink, log);
  feed.subscribe(o.symbol); // parse_args guarantees one in pipeline mode

  std::atomic<bool> feed_done{false};
  RunInfo info;

  std::thread consumer_thread([&] {
    ttt::core::set_thread_name("ttt-consumer");
    pin(o.cpu_consumer, "consumer");
    for (;;) {
      if (BookUpdate* u = q.front()) {
        consumer.on_update(*u);
        q.pop();
      } else if (feed_done.load(std::memory_order_acquire)) {
        // re-check: T1 may have pushed between the empty front() and seeing feed_done
        if (!q.front()) break;
      }
    }
  });

  std::thread feed_thread([&] {
    ttt::core::set_thread_name("ttt-feed");
    pin(o.cpu_feed, "feed");
    info.interrupted = run_loop(feed);
    feed_done.store(true, std::memory_order_release);
  });

  feed_thread.join();
  consumer_thread.join();
  // both threads joined: the handler and consumer are safe to read from here
  info.queue_full = sink.full;
  report(feed, info);
  return info;
}

} // namespace

int main(int argc, char** argv) {
  const auto opts = feed_rx::parse_args(argc, argv);
  if (!opts) {
    feed_rx::usage(argv[0]);
    return 2;
  }
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  std::FILE* log_out = opts->log_file ? std::fopen(opts->log_file, "w") : stderr;
  if (!log_out) {
    std::perror(opts->log_file);
    return 2;
  }
  ttt::core::AsyncLogger logger(log_out);
  auto& feed_log = logger.channel("feed");
  feed_log.set_min_level(opts->log_level);
  logger.start(opts->cpu_logger);

  ttt::feed::UdpFeedIo io{
      ttt::net::UdpSocket::multicast_rx(opts->group, opts->port, opts->iface, opts->rcvbuf_bytes),
      ttt::net::UdpSocket::unicast(opts->iface, 0),
      ttt::net::Endpoint::from(opts->rerequest_host, opts->rerequest_port)};
  // logged once here: the host comes from argv, so it can't go through the async logger
  std::fprintf(stderr, "rerequests go to %s:%u\n", opts->rerequest_host, opts->rerequest_port);

  // The handler's type depends on the mode, so results are printed from inside the
  // run function while it still exists
  UpdateConsumer consumer;
  bool ok = false;
  const auto report = [&](const auto& feed, const RunInfo& info) {
    logger.stop(); // flush log lines first so they don't interleave with the results
    ok = print_results(*opts, feed, consumer, info);
  };

  const RunInfo info = opts->mode == feed_rx::Mode::Pipeline
                           ? run_pipeline(*opts, io, feed_log, consumer, report)
                           : run_single(*opts, io, feed_log, consumer, report);

  if (log_out != stderr) std::fclose(log_out);
  return ok && !info.interrupted ? 0 : 1;
}
