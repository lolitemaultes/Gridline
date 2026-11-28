// Gridline — K-432 (GL-8006CE18): safe std::thread teardown helper.
//
// Destroying a joinable std::thread or calling join() from the thread itself
// inside a noexcept destructor invokes std::terminate() with no active
// exception. Use safeJoin() in destructors and shutdown paths so unexpected
// exit() (for example from a third-party plugin) never cascades into SIGABRT.
#pragma once
#include <atomic>
#include <chrono>
#include <thread>

namespace gl::threads {

// Joins `t` when joinable, or detaches it if called from `t` itself (preventing
// resource_deadlock_would_occur -> std::terminate() inside noexcept destructors).
inline void safeJoin(std::thread& t) noexcept {
  if (!t.joinable()) return;
  if (t.get_id() == std::this_thread::get_id()) {
    t.detach();
    return;
  }
  try {
    t.join();
  } catch (...) {
    if (t.joinable()) t.detach();
  }
}

// For static workers torn down by an unexpected exit(): waits up to `ms` for `running` to turn
// false, then joins; a worker still busy (for example waiting on a UI-thread task that will never
// be served now) is detached instead of blocking the exit forever. Never serves UI-thread work
// itself, since exit() may run on any thread.
inline void joinWithin(std::thread& t, const std::atomic<bool>& running, int ms = 2000) noexcept {
  if (!t.joinable()) return;
  if (t.get_id() == std::this_thread::get_id()) {
    t.detach();
    return;
  }
  const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  while (running.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < until)
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  if (running.load(std::memory_order_acquire)) t.detach();
  else safeJoin(t);
}

}  // namespace gl::threads
