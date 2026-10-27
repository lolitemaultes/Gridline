// Gridline: WSOLA time stretching (J-140). See TdStretch.h.
#include "TdStretch.h"

namespace gl::pitch {

namespace {
constexpr double kPi = 3.14159265358979323846;
int64_t pow2AtLeast(int64_t v) {
  int64_t p = 1;
  while (p < v) p <<= 1;
  return p;
}
}  // namespace

void TdStretch::configure(int channels, double sampleRate, Preset p) {
  channels_ = std::clamp(channels, 1, 2);
  const double sr = sampleRate > 0 ? sampleRate : 44100.0;
  hop_ = std::max(32, static_cast<int>(std::lround(p.grainSec * sr * 0.5)));
  n_ = 2 * hop_;
  tol_ = std::max(8, static_cast<int>(std::lround(p.toleranceSec * sr)));
  // Lookahead: the furthest a grain reads past its nominal start (tolerance + grain),
  // plus one hop (the grain of the next sub-step is placed before its input arrives).
  seek_ = n_ + tol_ + hop_ + 4;
  cap_ = static_cast<int>(pow2AtLeast(static_cast<int64_t>(seek_) + n_ + 2 * tol_ + 2 * hop_ + 64));
  mask_ = cap_ - 1;
  win_.assign(static_cast<size_t>(n_), 0.0f);
  for (int i = 0; i < n_; ++i) {
    const double s = std::sin(kPi * (i + 0.5) / n_);
    win_[static_cast<size_t>(i)] = static_cast<float>(s * s);  // sin² at half overlap sums to one
  }
  for (int c = 0; c < 2; ++c) {
    ring_[c].assign(static_cast<size_t>(cap_), 0.0f);
    acc_[c].assign(static_cast<size_t>(2 * n_), 0.0f);
    outBuf_[c].assign(static_cast<size_t>(hop_), 0.0f);
  }
  reset();
}

void TdStretch::reset() {
  for (int c = 0; c < 2; ++c) {
    std::fill(ring_[c].begin(), ring_[c].end(), 0.0f);
    std::fill(acc_[c].begin(), acc_[c].end(), 0.0f);
  }
  written_ = 0;
  ana_ = 0;
  outPos_ = 0;
  nextGrain_ = 0;
  prevSrc_ = 0;
  first_ = true;
}

float TdStretch::mono(int64_t i) const {
  if (i < 0 || i >= written_ || i < written_ - cap_) return 0.0f;
  const auto k = static_cast<size_t>(i & mask_);
  return channels_ == 2 ? 0.5f * (ring_[0][k] + ring_[1][k]) : ring_[0][k];
}

int64_t TdStretch::search(int64_t nominal, int64_t ref) const {
  const int L = hop_;  // the overlap the new grain shares with the previous one
  const int64_t oldest = written_ - cap_ + 1;
  if (ref < oldest || ref + L > written_) return nominal;
  int64_t lo = std::max(nominal - tol_, oldest), hi = std::min<int64_t>(nominal + tol_, written_ - n_);
  if (hi < lo) return std::clamp<int64_t>(nominal, oldest, std::max<int64_t>(oldest, written_ - n_));
  // Coarse: every 4th lag, every 4th frame; then ±4 frames at every 2nd frame.
  auto score = [&](int64_t s, int stride) {
    double ab = 0, bb = 0;
    for (int i = 0; i < L; i += stride) {
      const double a = mono(ref + i), b = mono(s + i);
      ab += a * b, bb += b * b;
    }
    return ab / std::sqrt(bb + 1e-12);
  };
  int64_t best = std::clamp(nominal, lo, hi);
  double bs = score(best, 4);
  for (int64_t s = lo; s <= hi; s += 4) {
    const double v = score(s, 4);
    if (v > bs) bs = v, best = s;
  }
  const int64_t c = best;
  bs = score(c, 2);
  for (int64_t s = std::max(lo, c - 4); s <= std::min(hi, c + 4); ++s) {
    const double v = score(s, 2);
    if (v > bs) bs = v, best = s;
  }
  return best;
}

void TdStretch::addGrain(int64_t at, int64_t src) {
  const auto size = static_cast<int64_t>(acc_[0].size());
  // Overlap gain: sin² windows sum to one in amplitude, which is right when
  // the two overlapping grains are coherent (tones, aligned by the search) but
  // loses up to 3 dB in the middle of the overlap when they are not (noise,
  // reverb tails): their powers add, not their amplitudes. With rho the
  // normalised correlation of the overlapping parts, the sum's power is
  // w1² + w2² + 2·rho·w1·w2; each overlap frame is scaled back to one.
  double rho = 1.0;
  if (!first_) {
    double ab = 0, aa = 0, bb = 0;
    for (int i = 0; i < hop_; i += 2) {
      const double a = mono(prevSrc_ + hop_ + i), b = mono(src + i);
      ab += a * b, aa += a * a, bb += b * b;
    }
    rho = aa > 1e-12 && bb > 1e-12 ? std::clamp(ab / std::sqrt(aa * bb), 0.0, 1.0) : 1.0;
  }
  for (int c = 0; c < channels_; ++c) {
    float* a = acc_[c].data();
    const float* rb = ring_[c].data();
    for (int i = 0; i < n_; ++i) {
      const int64_t k = src + i;
      const float x = k >= 0 && k < written_ && k >= written_ - cap_ ? rb[static_cast<size_t>(k & mask_)] : 0.0f;
      // The first grain after a reset starts at full level (left half flat):
      // the output begins exactly with the input, no fade-in.
      const float w = first_ && i < hop_ ? 1.0f : win_[static_cast<size_t>(i)];
      a[static_cast<size_t>((at + i) % size)] += w * x;
    }
  }
  if (rho < 0.999) {
    for (int i = 0; i < hop_; ++i) {
      const double w2 = win_[static_cast<size_t>(i)], w1 = 1.0 - w2;
      const auto g = static_cast<float>(1.0 / std::sqrt(w1 * w1 + w2 * w2 + 2.0 * rho * w1 * w2));
      for (int c = 0; c < channels_; ++c) acc_[static_cast<size_t>(c)][static_cast<size_t>((at + i) % size)] *= g;
    }
  }
}

void TdStretch::render(int m, double rate, double anaEnd) {
  (void)anaEnd;
  const auto size = static_cast<int64_t>(acc_[0].size());
  // Grains that start inside this step.
  while (nextGrain_ < outPos_ + m) {
    const double nominalPos = ana_ + static_cast<double>(nextGrain_ - outPos_) * rate;
    const auto nominal = static_cast<int64_t>(std::llround(nominalPos));
    const int64_t src = first_ ? nominal : search(nominal, prevSrc_ + hop_);
    addGrain(nextGrain_, src);
    first_ = false;
    prevSrc_ = src;
    nextGrain_ += hop_;
  }
  for (int c = 0; c < channels_; ++c) {
    float* a = acc_[c].data();
    float* o = outBuf_[c].data();
    for (int j = 0; j < m; ++j) {
      const auto k = static_cast<size_t>((outPos_ + j) % size);
      o[j] = a[k];
      a[k] = 0.0f;
    }
  }
  outPos_ += m;
}

}  // namespace gl::pitch
