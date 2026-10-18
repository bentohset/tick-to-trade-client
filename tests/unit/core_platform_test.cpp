// clock.hpp and cpu_affinity.hpp: small platform wrappers

#include "core/clock.hpp"
#include "core/cpu_affinity.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <thread>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

namespace ttt::core {
namespace {

TEST(Clock, TicksNeverGoBackwards) {
  uint64_t prev = read_ticks();
  for (int i = 0; i < 100'000; ++i) {
    const uint64_t now = read_ticks();
    ASSERT_GE(now, prev);
    prev = now;
  }
}

TEST(Clock, TicksToNsOfZeroIsZero) { EXPECT_EQ(ticks_to_ns(0), 0u); }

TEST(Clock, TicksToNsMatchesWallTime) {
  // Loose bounds: a sleep can overshoot on a loaded CI runner, never undershoot
  const uint64_t t0 = read_ticks();
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  const uint64_t ns = ticks_to_ns(read_ticks() - t0);
  EXPECT_GE(ns, 19'000'000u);
  EXPECT_LT(ns, 500'000'000u);
}

#if defined(__linux__)

TEST(CpuAffinity, PinsToAllowedCpu) {
  std::thread t([] {
    cpu_set_t allowed;
    ASSERT_EQ(sched_getaffinity(0, sizeof(allowed), &allowed), 0);
    int cpu = 0;
    while (!CPU_ISSET(cpu, &allowed)) ++cpu; // first CPU this process may use

    ASSERT_TRUE(pin_this_thread(cpu));
    cpu_set_t now;
    ASSERT_EQ(pthread_getaffinity_np(pthread_self(), sizeof(now), &now), 0);
    EXPECT_EQ(CPU_COUNT(&now), 1);
    EXPECT_TRUE(CPU_ISSET(cpu, &now));
    EXPECT_EQ(sched_getcpu(), cpu);
  });
  t.join();
}

TEST(CpuAffinity, RejectsCpuOutsideTheMachine) {
  std::thread t([] { EXPECT_FALSE(pin_this_thread(CPU_SETSIZE - 1)); });
  t.join();
}

TEST(CpuAffinity, SetsThreadName) {
  std::thread t([] {
    set_thread_name("ttt-test");
    char name[16];
    ASSERT_EQ(pthread_getname_np(pthread_self(), name, sizeof(name)), 0);
    EXPECT_STREQ(name, "ttt-test");
  });
  t.join();
}

#else

TEST(CpuAffinity, PinningUnsupportedOffLinux) { EXPECT_FALSE(pin_this_thread(0)); }

#endif

} // namespace
} // namespace ttt::core
