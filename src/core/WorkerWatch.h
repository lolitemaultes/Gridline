// Gridline - K-133: a render worker's sign of life. The worker beats whenever it moves on (each
// progress step); the interface thread looks once a frame. A beat that stops for longer than the
// limit is a worker that hangs (a plugin that never returns from its processing, a file system that
// does not answer): it is reported once, and reported as moving again when the beats come back.
#pragma once
#include <atomic>
#include <cstdint>
#include <cstdlib>

namespace gl {

class WorkerBeat {
 public:
  void beat() noexcept { n_.fetch_add(1, std::memory_order_relaxed); }
  uint64_t count() const noexcept { return n_.load(std::memory_order_relaxed); }

 private:
  std::atomic<uint64_t> n_{0};
};

// The interface thread's view of one worker. Not thread safe: one reader.
class StallWatch {
 public:
  enum class Event { None, Stalled, Moving };

  // How long a worker may stay silent: 90 s (a slow plugin, a long file write still move within
  // that), GRIDLINE_DEV_WORKER_STALL_MS in tests.
  static int64_t defaultLimitMs() {
    if (const char* e = std::getenv("GRIDLINE_DEV_WORKER_STALL_MS")) {
      const long v = std::atol(e);
      if (v > 0) return v;
    }
    return 90000;
  }

  explicit StallWatch(int64_t limitMs = defaultLimitMs()) : limitMs_(limitMs) {}

  // A new job starts.
  void start(int64_t nowMs, uint64_t beat) {
    last_ = beat;
    since_ = nowMs;
    stalled_ = false;
  }
  // Once a frame while the job runs. Stalled when the beat has not changed for the limit (once),
  // Moving when it changes again after that.
  Event update(int64_t nowMs, uint64_t beat) {
    if (beat != last_) {
      last_ = beat;
      since_ = nowMs;
      if (stalled_) {
        stalled_ = false;
        return Event::Moving;
      }
      return Event::None;
    }
    if (!stalled_ && nowMs - since_ >= limitMs_) {
      stalled_ = true;
      return Event::Stalled;
    }
    return Event::None;
  }
  bool stalled() const { return stalled_; }
  // How long the worker has been silent.
  int64_t silentMs(int64_t nowMs) const { return nowMs - since_; }
  int64_t limitMs() const { return limitMs_; }

 private:
  int64_t limitMs_;
  uint64_t last_ = 0;
  int64_t since_ = 0;
  bool stalled_ = false;
};

}  // namespace gl
