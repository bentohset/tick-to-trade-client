#pragma once

// Cheap timestamps for latency measurement.
//
// read_ticks() returns a raw hardware counter; convert differences with ns_per_tick().
//   x86-64: TSC via rdtsc (sub-ns resolution). Assumes an invariant TSC, true on
//           any x86 CPU from the last ~15 years.
//   arm64:  the generic timer (cntvct_el0). Readable from user space and cheaper
//           than steady_clock, but coarse on Apple Silicon: it advances every
//           41.67 ns (24 MHz). On M4 cntfrq_el0 reports 1 GHz and the counter
//           steps by ~41.67 at a time, so ns_per_tick() is 1.0 but values are
//           still multiples of ~41.67 ns.
//   other:  steady_clock in ns.

#include <chrono>
#include <cstdint>
#include <thread>

#if defined(__x86_64__)
#include <x86intrin.h>
#endif

namespace ttt::core {

inline uint64_t read_ticks() noexcept {
#if defined(__x86_64__)
  _mm_lfence(); // don't start the read before earlier instructions finish
  const uint64_t t = __rdtsc();
  _mm_lfence(); // ...or let later instructions start before it
  return t;
#elif defined(__aarch64__)
  uint64_t t;
  asm volatile("isb\n\tmrs %0, cntvct_el0" : "=r"(t) : : "memory");
  return t;
#else
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count());
#endif
}

inline const char* tick_source() noexcept {
#if defined(__x86_64__)
  return "rdtsc";
#elif defined(__aarch64__)
  return "cntvct_el0";
#else
  return "steady_clock";
#endif
}

namespace detail {
inline double measure_ns_per_tick() {
#if defined(__x86_64__)
  // TSC frequency isn't exposed portably: calibrate against steady_clock
  using Clock = std::chrono::steady_clock;
  const auto t0 = Clock::now();
  const uint64_t c0 = read_ticks();
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  const uint64_t c1 = read_ticks();
  const auto t1 = Clock::now();
  const double ns = std::chrono::duration<double, std::nano>(t1 - t0).count();
  return ns / static_cast<double>(c1 - c0);
#elif defined(__aarch64__)
  uint64_t hz;
  asm volatile("mrs %0, cntfrq_el0" : "=r"(hz));
  return 1e9 / static_cast<double>(hz);
#else
  return 1.0;
#endif
}
} // namespace detail

// Measured once per process (x86 calibration takes ~200 ms on first call)
inline double ns_per_tick() {
  static const double value = detail::measure_ns_per_tick();
  return value;
}

} // namespace ttt::core
