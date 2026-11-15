// Gridline: which song tick the performer heard at a given moment (recording from MIDI and the
// typing keyboard). The audio thread logs, for every stretch of transport it renders, when it
// rendered it (steady-clock seconds), the tick it started at and how fast ticks went. What comes
// out of the speakers at time t was rendered at t - latency, so the tick heard at t is found in
// that log. Loop wraps, jumps and count-ins are separate stretches, so the answer is right across
// them. Single writer (audio thread), any number of readers; no locks, no allocation.
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>

namespace gl {

class TransportClock {
 public:
  static constexpr int kSpans = 2048;

  // Audio thread: `seconds` of transport rendered at `wall`, from `tick`, at `ticksPerSecond`.
  // `run` changes whenever playback starts (spans of an earlier run are never used for a later one).
  void push(double wall, double tick, double ticksPerSecond, double seconds, uint32_t run) {
    const uint64_t n = count_.load(std::memory_order_relaxed);
    Span& s = spans_[static_cast<size_t>(n % kSpans)];
    s.seq.store(2 * n + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    s.wall.store(wall, std::memory_order_relaxed);
    s.tick.store(tick, std::memory_order_relaxed);
    s.tps.store(ticksPerSecond, std::memory_order_relaxed);
    s.seconds.store(seconds, std::memory_order_relaxed);
    s.run.store(run, std::memory_order_relaxed);
    s.seq.store(2 * n + 2, std::memory_order_release);
    count_.store(n + 1, std::memory_order_release);
  }

  // The tick heard at `wall` when the output lags the rendering by `latency` seconds, in playback
  // `run`. False when nothing of that run was rendered yet. Before the run's first span: its first tick.
  bool tickAt(double wall, double latency, uint32_t run, double* out) const {
    const uint64_t n = count_.load(std::memory_order_acquire);
    if (n == 0) return false;
    const double target = wall - latency;
    const uint64_t lo = n > static_cast<uint64_t>(kSpans) - 1 ? n - (kSpans - 1) : 0;  // (one slot may be rewritten)
    bool haveAny = false;
    double firstTick = 0;
    for (uint64_t k = n; k-- > lo;) {
      double w, t, tps, sec;
      uint32_t r;
      if (!read(k, &w, &t, &tps, &sec, &r)) continue;
      if (r != run) break;  // another playback (an earlier one, or this one is not rendered yet)
      haveAny = true;
      firstTick = t;
      if (w <= target) {
        *out = t + std::clamp(target - w, 0.0, sec) * tps;
        return true;
      }
    }
    if (!haveAny) return false;
    *out = firstTick;  // heard before the run's first rendered sample: where it started
    return true;
  }

  void clear() { count_.store(0, std::memory_order_release); }

 private:
  struct Span {
    std::atomic<uint64_t> seq{0};
    std::atomic<double> wall{0}, tick{0}, tps{0}, seconds{0};
    std::atomic<uint32_t> run{0};
  };
  bool read(uint64_t k, double* w, double* t, double* tps, double* sec, uint32_t* run) const {
    const Span& s = spans_[static_cast<size_t>(k % kSpans)];
    const uint64_t a = s.seq.load(std::memory_order_acquire);
    if (a != 2 * k + 2) return false;
    *w = s.wall.load(std::memory_order_relaxed);
    *t = s.tick.load(std::memory_order_relaxed);
    *tps = s.tps.load(std::memory_order_relaxed);
    *sec = s.seconds.load(std::memory_order_relaxed);
    *run = s.run.load(std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_acquire);
    return s.seq.load(std::memory_order_relaxed) == a;
  }
  std::array<Span, kSpans> spans_{};
  std::atomic<uint64_t> count_{0};
};

}  // namespace gl
