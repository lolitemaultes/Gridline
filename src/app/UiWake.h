// Gridline - K-142 (0.7.8): waking the interface's loop from another thread. While nothing is on
// screen the loop draws nothing and sleeps until an event comes; work another thread queues for
// the interface thread (a later launch's files, a MIDI controller's message, a collaborator's edit,
// an AI assistant or OSC request) calls poke() after queueing it, and the loop takes a turn at once.
//
// Cheap when the interface is awake: poke() marks the work pending (one atomic store) and wakes
// nothing, since the loop is about to take its next turn anyway. Only while the loop sleeps
// (beginWait .. endWait) does a poke call the waker the app set (an SDL event of its own, and on
// macOS a signal to the run loop the wait sleeps in), once per sleep. The two sides meet Dekker
// style (sequentially consistent flags): a poke that comes as the loop goes to sleep is either
// seen by the loop before it sleeps or wakes it.
//
// From a real-time thread (the audio callback, markRealtimeThread) poke() only marks the work
// pending, lock-free and with no system call: the sleeping loop finds it at its next look (while
// nothing is shown that can be seconds later, IdlePace.h kHiddenBeat): work that needs the
// interface at once goes through a thread that can wake it.
//
// No SDL here: the core library's services (OscServer, the collaboration hub, the AI assistant's
// HTTP server) reach poke() through hooks the app sets.
#pragma once
#include <atomic>

namespace gl::uiwake {

using Waker = void (*)();

namespace detail {
inline std::atomic<bool>& pending() {
  static std::atomic<bool> f{false};
  return f;
}
inline std::atomic<bool>& asleep() {
  static std::atomic<bool> f{false};
  return f;
}
inline std::atomic<bool>& wokenThisSleep() {
  static std::atomic<bool> f{false};
  return f;
}
inline std::atomic<Waker>& waker() {
  static std::atomic<Waker> w{nullptr};
  return w;
}
inline std::atomic<unsigned long long>& wakes() {
  static std::atomic<unsigned long long> n{0};
  return n;
}
inline bool& realtimeThread() {
  static thread_local bool rt = false;
  return rt;
}
}  // namespace detail

// The app, once: how a sleeping loop is woken (any thread but a real-time one calls it).
inline void setWaker(Waker w) { detail::waker().store(w); }
// The audio callback's thread (and any other real-time thread), once: pokes from it never wake.
inline void markRealtimeThread() { detail::realtimeThread() = true; }

// Work for the interface thread was queued: take a turn now.
inline void poke() {
  detail::pending().store(true);  // (sequentially consistent: before the look at `asleep`)
  if (!detail::asleep().load()) return;
  if (detail::realtimeThread()) return;              // lock-free: found at the loop's next look
  if (detail::wokenThisSleep().exchange(true)) return;  // this sleep is already being woken
  detail::wakes().fetch_add(1, std::memory_order_relaxed);
  if (const Waker w = detail::waker().load()) w();
}

// The interface thread, around each sleep of its wait. beginWait: false when work is already
// pending (do not sleep; takePending says so too).
inline bool beginWait() {
  detail::wokenThisSleep().store(false);
  detail::asleep().store(true);  // (sequentially consistent: before the look at `pending`)
  if (detail::pending().load()) {
    detail::asleep().store(false);
    return false;
  }
  return true;
}
inline void endWait() { detail::asleep().store(false); }
// The interface thread: true (once) when work was poked since the last call; its turn takes it.
inline bool takePending() {
  if (!detail::pending().load(std::memory_order_relaxed)) return false;
  return detail::pending().exchange(false);
}
// Sleeps the waker was called for (tests, the CPU report).
inline unsigned long long wakes() { return detail::wakes().load(std::memory_order_relaxed); }

}  // namespace gl::uiwake
