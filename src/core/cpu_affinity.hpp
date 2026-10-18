#pragma once

namespace ttt::core {

/**
 * Only applicable to linux machines.
 * Usage:
 *  std::jthread feed([&] {
 *    set_thread_name("xxxx");
 *    pin_this_thread(x);
 *    run_loop(...);
 *  })
 */

inline bool pin_this_thread(int cpu) noexcept {
#if defined(__linux__)
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
#else
  // macOS dev env, dont set threads
  (void)cpu;
  return false;
#endif
}

inline void set_thread_name(const char* name) noexcept {
#if defined(__linux__)
  // max 15 chars, else ERANGE (name unchangeed)
  pthread_setname_np(pthread_self(), name);
#else
  (void)name;
#endif
}

} // namespace ttt::core
