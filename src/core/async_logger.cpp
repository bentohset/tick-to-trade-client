#include "core/async_logger.hpp"

#include "core/clock.hpp"
#include "core/cpu_affinity.hpp"
#include "core/format.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>

namespace ttt::core {

LogChannel& AsyncLogger::channel(const char* name, std::size_t capacity) {
  return *channels_.emplace_back(std::make_unique<LogChannel>(name, capacity));
}

void AsyncLogger::start(int cpu) {
  // anchor tsc to wall-clock time once
  // each record thenconverts with ticks_to_ns
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  tsc0_ = read_ticks();
  tod0_ns_ = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count()) %
             (86'4000 * kNsPerSec);
  setvbuf(out_, nullptr, _IOFBF, 1 << 20);
  thread_ = std::thread([this, cpu] {
    if (cpu >= 0) pin_this_thread(cpu);
    run();
  });
}

void AsyncLogger::run() {
  while (!stop_.load(std::memory_order_acquire)) {
    bool any = false;
    // bounded batch
    for (int i = 0; i < 256 && drain_once(); ++i) any = true;
    if (!any) {
      report_drops();
      std::fflush(out_);
      std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
  }
  while (drain_once()) {}
  report_drops();
  std::fflush(out_);
}

bool AsyncLogger::drain_once() {
  LogChannel* best = nullptr;
  LogRecord* rec = nullptr;
  for (auto& ch : channels_) {
    if (LogRecord* r = ch->q_.front(); r && (!rec || r->tsc < rec->tsc)) {
      best = ch.get();
      rec = r;
    }
  }
  if (!rec) return false;
  write(*rec, *best);
  best->q_.pop();
  return true;
}

void AsyncLogger::write(const LogRecord& r, const LogChannel& ch) {
  static constexpr const char* kLevel[] = {"DBG", "INF", "WRN", "ERR"};
  print_time_of_day(out_, (tod0_ns_ + ticks_to_ns(r.tsc - tsc0_)) % (86'400 * kNsPerSec));
  buf_.clear();
  r.decode(r, buf_);
  std::fprintf(out_, " %s %-5s %.*s\n", kLevel[int(r.level)], ch.name_, int(buf_.size()),
               buf_.data());
}

void AsyncLogger::report_drops() {
  for (auto& ch : channels_) {
    const auto d = ch->dropped_.load(std::memory_order_relaxed);
    if (d != ch->dropped_reported_) {
      std::fprintf(out_, "WRN logger: %s dropped %llu records\n", ch->name_,
                   (unsigned long long)(d - ch->dropped_reported_));
      ch->dropped_reported_ = d;
    }
  }
}

void AsyncLogger::stop() {
  if (thread_.joinable()) {
    stop_.store(true, std::memory_order_release);
    thread_.join();
  }
}

} // namespace ttt::core
