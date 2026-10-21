#include "apps/mock_exchange/exchange.hpp"
#include "apps/mock_exchange/options.hpp"
#include "core/clock.hpp"

#include <atomic>
#include <cinttypes>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <exception>

namespace {

// set from signal handlers: only lock-free atomic stores are safe there
std::atomic<bool> g_stop{false};
std::atomic<bool> g_kill{false};

extern "C" void on_stop(int) { g_stop.store(true, std::memory_order_relaxed); }
extern "C" void on_kill(int) { g_kill.store(true, std::memory_order_relaxed); }

void print_records(const mock_exchange::MockExchange& ex) {
  // PRIu64/PRId64: uint64_t is unsigned long on Linux but unsigned long long on macOS
  std::printf("sequenced %" PRIu64 ", executions %" PRIu64 "\n", ex.sequenced(), ex.executions());
  for (const auto& [key, pos] : ex.positions()) {
    char sym[9]{};
    std::memcpy(sym, &key, 8); // key is the 8 symbol bytes, see itch::Symbol::key
    std::printf("  %s %+" PRId64 "\n", sym, pos);
  }
}

} // namespace

int main(int argc, char** argv) {
  const auto opts = mock_exchange::parse_args(argc, argv);
  if (!opts) {
    mock_exchange::usage(argv[0]);
    return 2;
  }

  std::signal(SIGINT, on_stop);
  std::signal(SIGTERM, on_stop);
  std::signal(SIGUSR1, on_kill); // kill -USR1 <pid>: test reconnect by hand

  try {
    mock_exchange::MockExchange ex(opts->cfg);
    std::fprintf(stderr, "mock_exchange listening on 127.0.0.1:%u\n", opts->cfg.port);

    // busy poll, same as the trader side
    while (!g_stop.load(std::memory_order_relaxed)) {
      if (g_kill.exchange(false, std::memory_order_relaxed)) ex.kill();
      ex.poll(ttt::core::ticks_to_ns(ttt::core::read_ticks()));
    }
    // these are what the trader's positions are reconciled against
    print_records(ex);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "error: %s\n", e.what());
    return 1;
  }
  return 0;
}

