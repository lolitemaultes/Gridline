// Gridline — plugin delay compensation (PDC) delay line for one edge of the
// mixer graph (channel → insert, send, sidechain, bus send → return).
// Built on the UI thread with a fixed power-of-two capacity; only the audio
// thread runs it (no allocation). It always records its input, so a delay
// change crossfades between the old and the new read position once enough
// history exists — never a jump, never a gap. A line that must grow is
// replaced by a larger one that adopts the old line's history on its first
// run (see ProgramBuilder::compileGraph).
#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <vector>

namespace gl {

class PdcDelay {
 public:
  static constexpr int kFade = 256;  // crossfade length (samples) when the delay changes

  // `initial`: delay the line starts at (its history is silence).
  PdcDelay(int capacity, int initial) {
    size_t c = 1024;
    while (c < static_cast<size_t>(std::max(1, capacity))) c <<= 1;
    l_.assign(c, 0.f);
    r_.assign(c, 0.f);
    mask_ = c - 1;
    cur_ = next_ = std::clamp(initial, 0, static_cast<int>(mask_));
    filled_ = static_cast<int64_t>(c);  // silence counts as history
  }
  // Largest delay this line can hold.
  int maxDelay() const { return static_cast<int>(mask_); }
  // Current (audio thread) read delay; for tests.
  int current() const { return cur_; }

  // Writes in*level (silence when inL is null) and adds the signal delayed by
  // `target` samples (crossfading from the previous delay) to acc.
  void run(int target, const float* inL, const float* inR, float level, float* accL, float* accR, int n) {
    target = std::clamp(target, 0, static_cast<int>(mask_));
    for (int i = 0; i < n; ++i) {
      const size_t w = static_cast<size_t>(w_) & mask_;
      l_[w] = inL ? inL[i] * level : 0.f;
      r_[w] = inR ? inR[i] * level : 0.f;
      if (filled_ < static_cast<int64_t>(mask_) + 1) ++filled_;
      if (fade_ == 0 && target != cur_ && filled_ > target) {
        next_ = target;
        fade_ = kFade;
      }
      float yl, yr;
      if (fade_ > 0) {
        const float a = static_cast<float>(fade_) / static_cast<float>(kFade);  // weight of the old tap
        const size_t o = static_cast<size_t>(w_ - cur_) & mask_, q = static_cast<size_t>(w_ - next_) & mask_;
        yl = l_[o] * a + l_[q] * (1.f - a);
        yr = r_[o] * a + r_[q] * (1.f - a);
        if (--fade_ == 0) cur_ = next_;
      } else {
        const size_t o = static_cast<size_t>(w_ - cur_) & mask_;
        yl = l_[o];
        yr = r_[o];
      }
      accL[i] += yl;
      accR[i] += yr;
      ++w_;
    }
  }

  // Takes over `old`'s history and read position (audio thread, once, before
  // the first run). `old` stays owned by the Program that hands it over.
  void adopt(const PdcDelay& old) {
    if (adopted_.load(std::memory_order_relaxed)) return;
    w_ = old.w_;
    const int64_t keep = std::min<int64_t>(static_cast<int64_t>(old.mask_) + 1, static_cast<int64_t>(mask_) + 1);
    for (int64_t k = 1; k <= keep; ++k) {
      l_[static_cast<size_t>(w_ - k) & mask_] = old.l_[static_cast<size_t>(w_ - k) & old.mask_];
      r_[static_cast<size_t>(w_ - k) & mask_] = old.r_[static_cast<size_t>(w_ - k) & old.mask_];
    }
    cur_ = old.fade_ > 0 ? old.next_ : old.cur_;
    next_ = cur_;
    fade_ = 0;
    filled_ = std::min<int64_t>(old.filled_, keep);
    adopted_.store(true, std::memory_order_release);
  }
  // True once adopt() ran (the builder then drops the old line).
  bool adopted() const { return adopted_.load(std::memory_order_acquire); }

 private:
  std::vector<float> l_, r_;
  size_t mask_ = 0;
  int64_t w_ = 0;       // samples written
  int64_t filled_ = 0;  // samples of valid history (capped at the capacity)
  int cur_ = 0, next_ = 0, fade_ = 0;
  std::atomic<bool> adopted_{false};
};

// One compensated input of a mixer node: the delay applied and the line that
// applies it (null when the delay is 0 and was never non-zero).
struct PdcTap {
  int delay = 0;
  PdcDelay* line = nullptr;
  PdcDelay* adoptFrom = nullptr;  // previous (smaller) line whose history `line` takes over

  // acc += in·level delayed by `delay` (in == null: silence enters the line).
  void run(const float* inL, const float* inR, float level, float* accL, float* accR, int n) const {
    if (!line) {
      if (inL)
        for (int i = 0; i < n; ++i) {
          accL[i] += inL[i] * level;
          accR[i] += inR[i] * level;
        }
      return;
    }
    if (adoptFrom && !line->adopted()) line->adopt(*adoptFrom);
    line->run(delay, inL, inR, level, accL, accR, n);
  }
};

}  // namespace gl
