// Gridline - K-123 (0.7.7): capture while armed. See ArmedRing.h.
#include "ArmedRing.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace gl {

namespace {
// Frames at the old end of the ring the reader leaves alone: the writer may be overwriting them.
constexpr size_t kSafety = 8192;
constexpr size_t kMinFrames = size_t{1} << 16;
}  // namespace

size_t ArmedRing::capacityFor(size_t frames) { return std::max(frames, kMinFrames); }

ArmedRing::ArmedRing(size_t frames)
    : cap_(capacityFor(frames)), L_(cap_, 0.f), R_(cap_, 0.f), marks_(kMarks) {}

void ArmedRing::write(const float* L, const float* R, const double* ticks, int n) {
  if (n <= 0) return;
  const size_t w = pos_.load(std::memory_order_relaxed);
  const auto un = static_cast<size_t>(n);
  {  // the frames, in at most two pieces
    const size_t k = w % cap_;
    const size_t first = std::min(un, cap_ - k);
    std::memcpy(L_.data() + k, L, sizeof(float) * first);
    std::memcpy(R_.data() + k, R, sizeof(float) * first);
    if (first < un) {
      std::memcpy(L_.data(), L + first, sizeof(float) * (un - first));
      std::memcpy(R_.data(), R + first, sizeof(float) * (un - first));
    }
  }
  // The positions, as marks: a new mark where the positions stop following the current one.
  size_t head = markHead_.load(std::memory_order_relaxed);
  auto push = [&](const Mark& m) {
    marks_[head % kMarks] = m;
    ++head;
    markHead_.store(head, std::memory_order_release);
  };
  for (int i = 0; i < n; ++i) {
    const double t = ticks[i];
    const size_t f = w + static_cast<size_t>(i);
    if (t < 0) {
      if (expect_ >= 0) push({f, -1.0, 0.0, true});
      expect_ = last_ = -1;
      continue;
    }
    if (expect_ >= 0 && std::fabs(t - expect_) <= 1e-3) {
      expect_ += step_;
      last_ = t;
      continue;
    }
    // A jump (the transport started, looped or moved), or the spacing changed (a tempo change).
    const bool jump = last_ < 0 || t < last_ - 1e-6 || t > last_ + 4.0;
    const double step = i + 1 < n && ticks[i + 1] >= 0 ? ticks[i + 1] - t : step_;
    step_ = step > 0 ? step : step_;
    push({f, t, step_, jump});
    expect_ = t + step_;
    last_ = t;
  }
  pos_.store(w + un, std::memory_order_release);
}

size_t ArmedRing::oldest() const {
  const size_t w = written();
  const size_t keep = cap_ - kSafety;
  return w > keep ? w - keep : 0;
}

size_t ArmedRing::read(size_t from, size_t n, float* L, float* R) const {
  if (n == 0 || from < oldest() || from + n > written()) return 0;
  for (size_t i = 0; i < n; ++i) {
    const size_t k = (from + i) % cap_;
    L[i] = L_[k];
    R[i] = R_[k];
  }
  return from >= oldest() ? n : 0;  // overwritten while copying: nothing
}

void ArmedRing::markRange(size_t& lo, size_t& hi) const {
  hi = markHead_.load(std::memory_order_acquire);
  const size_t keep = kMarks - kMarkMargin;
  lo = hi > keep ? hi - keep : 0;
}

bool ArmedRing::markFor(size_t frame, size_t lo, size_t hi, size_t& k) const {
  if (hi <= lo || marks_[lo % kMarks].frame > frame) return false;
  size_t a = lo, b = hi;  // the last mark with .frame <= frame lies in [a, b)
  while (b - a > 1) {
    const size_t m = a + (b - a) / 2;
    if (marks_[m % kMarks].frame <= frame) {
      a = m;
    } else {
      b = m;
    }
  }
  k = a;
  return true;
}

bool ArmedRing::tickAt(size_t frame, double& tick) const {
  if (frame < oldest() || frame >= written()) return false;
  size_t lo = 0, hi = 0, k = 0;
  markRange(lo, hi);
  if (!markFor(frame, lo, hi, k)) return false;
  const Mark& m = marks_[k % kMarks];
  if (m.tick < 0) return false;
  tick = m.tick + static_cast<double>(frame - m.frame) * m.step;
  return true;
}

bool ArmedRing::runOf(size_t frame, size_t& first, size_t& end) const {
  if (frame < oldest() || frame >= written()) return false;
  size_t lo = 0, hi = 0, k = 0;
  markRange(lo, hi);
  if (!markFor(frame, lo, hi, k) || marks_[k % kMarks].tick < 0) return false;
  size_t s = k;
  while (s > lo && !marks_[s % kMarks].jump) --s;
  first = std::max(marks_[s % kMarks].frame, oldest());
  end = written();
  for (size_t j = k + 1; j < hi; ++j) {
    if (marks_[j % kMarks].jump) {
      end = marks_[j % kMarks].frame;
      break;
    }
  }
  return end > first;
}

bool ArmedRing::lastRun(size_t minFrames, size_t& first, size_t& end) const {
  size_t lo = 0, hi = 0;
  markRange(lo, hi);
  const size_t old = oldest();
  size_t stop = written();
  for (size_t j = hi; j > lo; --j) {
    const Mark& m = marks_[(j - 1) % kMarks];
    if (!m.jump) continue;
    if (m.tick >= 0 && stop > std::max(m.frame, old) && stop - std::max(m.frame, old) >= std::max<size_t>(1, minFrames)) {
      first = std::max(m.frame, old);
      end = stop;
      return true;
    }
    stop = m.frame;
    if (stop <= old) break;
  }
  return false;
}

}  // namespace gl
