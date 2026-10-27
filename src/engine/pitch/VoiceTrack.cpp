// Gridline: streaming voice analysis (J-144). See VoiceTrack.h.
#include "VoiceTrack.h"
#include <cmath>

namespace gl::pitch {

namespace {
constexpr double kPi = 3.14159265358979323846;
int64_t pow2AtLeast(int64_t v) {
  int64_t p = 1;
  while (p < v) p <<= 1;
  return p;
}
// Viterbi costs. A frame's candidate costs its CMNDF value (0 a perfect
// repetition, 1 none), plus a little per octave of period above the shortest
// good candidate (a periodic signal also repeats after two periods: without
// it, the doubled period would tie). Unvoiced costs a flat 0.35, so a frame is
// voiced when its best dip is clearly under that. Moving between candidates
// costs per octave of change (an octave jump costs 1.2: a few frames of much
// better fit are needed to make one), switching voicing on or off 0.3.
constexpr float kUnvoiced = 0.35f;
constexpr float kOctaveCost = 1.2f;
constexpr float kSwitchCost = 0.3f;
constexpr float kLongerCost = 0.15f;
constexpr float kMaxDip = 0.6f;  // dips above this are not candidates
}  // namespace

void VoiceTrack::configure(double sampleRate, int64_t capacity) {
  sr_ = sampleRate > 0 ? sampleRate : 44100.0;
  D_ = std::max(1, static_cast<int>(std::lround(sr_ / 11025.0)));
  tMin_ = std::max(8, static_cast<int>(sr_ / 1100.0));
  tMax_ = static_cast<int>(std::ceil(sr_ / 50.0));
  hop_ = std::max(16, static_cast<int>(std::lround(0.0058 * sr_)));
  const double dsr = sr_ / D_;
  W_ = std::max(32, static_cast<int>(std::lround(0.025 * dsr)));
  tauMax_ = static_cast<int>(std::ceil(tMax_ / static_cast<double>(D_))) + 2;
  tauMin_ = std::max(2, static_cast<int>(tMin_ / D_));
  exLen_ = std::max(4, static_cast<int>(std::lround(0.0006 * sr_)));
  // Decimation low-pass: Kaiser-windowed sinc, cutoff at 0.42 of the decimated rate.
  firHalf_ = 4 * D_;
  fir_.assign(static_cast<size_t>(2 * firHalf_ + 1), 0.0f);
  {
    const double fc = 0.42 / D_;  // cycles per input frame
    const double beta = 6.0;
    auto i0 = [](double x) {
      double s = 1, t = 1;
      for (int k = 1; k < 30; ++k) t *= (x / (2 * k)) * (x / (2 * k)), s += t;
      return s;
    };
    double sum = 0;
    for (int k = -firHalf_; k <= firHalf_; ++k) {
      const double u = static_cast<double>(k) / (firHalf_ + 1);
      const double w = i0(beta * std::sqrt(std::max(0.0, 1 - u * u))) / i0(beta);
      const double s = k == 0 ? 2 * fc : std::sin(2 * kPi * fc * k) / (kPi * k);
      fir_[static_cast<size_t>(k + firHalf_)] = static_cast<float>(s * w);
      sum += s * w;
    }
    for (auto& v : fir_) v = static_cast<float>(v / sum);
  }
  // Lookahead until marks are final: frame analysis (half a window, the
  // longest lag, the filter), the decision lag, then the mark search (a
  // period and a quarter plus the correlation half window) and the excitation smoother.
  latency_ = W_ * D_ / 2 + tauMax_ * D_ + firHalf_ + D_ + (kLag + 2) * hop_ + 2 * tMax_ + exLen_ + 16;
  cap_ = pow2AtLeast(std::max<int64_t>(capacity, latency_ + 4 * tMax_) + 1024);
  mask_ = cap_ - 1;
  mono_.assign(static_cast<size_t>(cap_), 0.0f);
  ex_.assign(static_cast<size_t>(cap_), 0.0f);
  exHist_.assign(static_cast<size_t>(exLen_), 0.0f);
  decCap_ = pow2AtLeast(cap_ / D_ + 64);
  decMask_ = decCap_ - 1;
  dec_.assign(static_cast<size_t>(decCap_), 0.0f);
  dif_.assign(static_cast<size_t>(tauMax_ + 2), 0.0);
  const int64_t nf = pow2AtLeast(2 * kLag + 8);
  frames_.assign(static_cast<size_t>(nf), Frame{});
  frameMask_ = nf - 1;
  const int64_t nd = pow2AtLeast(cap_ / hop_ + 8);
  decPeriod_.assign(static_cast<size_t>(nd), 0.0f);
  decidedMask_ = nd - 1;
  const int64_t nm = pow2AtLeast(cap_ / tMin_ + 16);
  marks_.assign(static_cast<size_t>(nm), Mark{});
  markMask_ = nm - 1;
  reset();
}

void VoiceTrack::reset() {
  std::fill(mono_.begin(), mono_.end(), 0.0f);
  std::fill(ex_.begin(), ex_.end(), 0.0f);
  std::fill(exHist_.begin(), exHist_.end(), 0.0f);
  std::fill(dec_.begin(), dec_.end(), 0.0f);
  std::fill(decPeriod_.begin(), decPeriod_.end(), 0.0f);
  exSum_ = 0, last_ = 0;
  written_ = 0, nextStep_ = D_;
  decDone_ = 0, framesDone_ = 0, decided_ = 0, marksDone_ = 0;
  state_ = 0, scan_ = 0, runTonal_ = 0, runEnd_ = 0, tails_ = 0;
}

void VoiceTrack::step() {
  nextStep_ = written_ + D_;
  decimate();
  for (;;) {
    const int64_t c = framesDone_ * hop_;
    const int64_t a = (c - static_cast<int64_t>(W_) * D_ / 2) / D_;
    if (a + W_ + tauMax_ + 1 >= decDone_) break;
    analyseFrame();
  }
  makeMarks();
}

void VoiceTrack::decimate() {
  while (decDone_ * D_ + firHalf_ < written_) {
    const int64_t c = decDone_ * D_;
    double s = 0;
    for (int k = -firHalf_; k <= firHalf_; ++k) s += static_cast<double>(fir_[static_cast<size_t>(k + firHalf_)]) * monoAt(c + k);
    dec_[static_cast<size_t>(decDone_ & decMask_)] = static_cast<float>(s);
    ++decDone_;
  }
}

void VoiceTrack::analyseFrame() {
  Frame& f = frames_[static_cast<size_t>(framesDone_ & frameMask_)];
  f = Frame{};
  const int64_t c = framesDone_ * hop_;
  const int64_t a = (c - static_cast<int64_t>(W_) * D_ / 2) / D_;
  double e = 0;
  for (int i = 0; i < W_; ++i) {
    const double v = decAt(a + i);
    e += v * v;
  }
  if (e / W_ > 1e-7) {  // above about -70 dBFS
    // CMNDF: d(tau) * tau / sum of d(1 .. tau).
    double run = 0;
    for (int tau = 1; tau <= tauMax_; ++tau) {
      double s = 0;
      for (int i = 0; i < W_; ++i) {
        const double df = static_cast<double>(decAt(a + i)) - decAt(a + i + tau);
        s += df * df;
      }
      run += s;
      dif_[static_cast<size_t>(tau)] = run > 1e-20 ? s * tau / run : 1.0;
    }
    dif_[0] = 1.0;
    dif_[static_cast<size_t>(tauMax_ + 1)] = 1.0;
    // Local minima under kMaxDip, best first.
    Cand all[16];
    int na = 0;
    for (int tau = tauMin_; tau <= tauMax_ && na < 16; ++tau) {
      const double y0 = dif_[static_cast<size_t>(tau)];
      if (y0 >= kMaxDip || y0 > dif_[static_cast<size_t>(tau - 1)] || y0 > dif_[static_cast<size_t>(tau + 1)]) continue;
      const double ym = dif_[static_cast<size_t>(tau - 1)], yp = dif_[static_cast<size_t>(tau + 1)];
      const double den = ym - 2 * y0 + yp;
      double t = tau, y = y0;
      if (den > 1e-12) {
        const double o = std::clamp(0.5 * (ym - yp) / den, -0.5, 0.5);
        t += o;
        y = y0 - 0.25 * (ym - yp) * o;
      }
      all[na++] = Cand{static_cast<float>(std::clamp(t * D_, static_cast<double>(tMin_), static_cast<double>(tMax_))), static_cast<float>(std::max(0.0, y))};
    }
    std::sort(all, all + na, [](const Cand& x, const Cand& y) { return x.d < y.d; });
    f.n = std::min(na, kCands);
    for (int i = 0; i < f.n; ++i) f.c[i] = all[i];
  }
  viterbi();
  ++framesDone_;
}

void VoiceTrack::viterbi() {
  const int64_t t = framesDone_;
  Frame& f = frames_[static_cast<size_t>(t & frameMask_)];
  // Observation costs.
  float obs[kCands + 1];
  float shortest = 1e9f;
  float best = 1e9f;
  for (int i = 0; i < f.n; ++i) best = std::min(best, f.c[i].d);
  for (int i = 0; i < f.n; ++i)
    if (f.c[i].d < best + 0.15f) shortest = std::min(shortest, f.c[i].tau);
  for (int i = 0; i < f.n; ++i) obs[i] = f.c[i].d + kLongerCost * static_cast<float>(std::log2(f.c[i].tau / shortest));
  obs[kCands] = kUnvoiced;
  if (t == 0) {
    for (int i = 0; i < f.n; ++i) f.cost[i] = obs[i], f.back[i] = static_cast<int8_t>(kCands);
    f.cost[kCands] = obs[kCands], f.back[kCands] = static_cast<int8_t>(kCands);
  } else {
    const Frame& p = frames_[static_cast<size_t>((t - 1) & frameMask_)];
    auto relax = [&](int j, float tauJ) {
      float bc = p.cost[kCands] + (tauJ > 0 ? kSwitchCost : 0.0f);
      int bi = kCands;
      for (int i = 0; i < p.n; ++i) {
        const float tr = tauJ > 0 ? kOctaveCost * std::fabs(static_cast<float>(std::log2(tauJ / p.c[i].tau))) : kSwitchCost;
        if (p.cost[i] + tr < bc) bc = p.cost[i] + tr, bi = i;
      }
      f.cost[j] = bc + obs[j];
      f.back[j] = static_cast<int8_t>(bi);
    };
    for (int j = 0; j < f.n; ++j) relax(j, f.c[j].tau);
    relax(kCands, 0.0f);
    float mn = f.cost[kCands];
    for (int j = 0; j < f.n; ++j) mn = std::min(mn, f.cost[j]);
    for (int j = 0; j < f.n; ++j) f.cost[j] -= mn;
    f.cost[kCands] -= mn;
  }
  // Fixed-lag decision: backtrack from the best state of this frame.
  if (t - kLag < decided_) return;
  int s = kCands;
  float bc = f.cost[kCands];
  for (int j = 0; j < f.n; ++j)
    if (f.cost[j] < bc) bc = f.cost[j], s = j;
  for (int64_t k = t; k > decided_; --k) s = frames_[static_cast<size_t>(k & frameMask_)].back[s];
  const Frame& d = frames_[static_cast<size_t>(decided_ & frameMask_)];
  decPeriod_[static_cast<size_t>(decided_ & decidedMask_)] = s < d.n ? d.c[s].tau : 0.0f;
  ++decided_;
}

bool VoiceTrack::frameVoiced(int64_t k) const { return k >= 0 && k < decided_ && k > decided_ - decidedMask_ && decPeriod_[static_cast<size_t>(k & decidedMask_)] > 0; }

double VoiceTrack::framePeriod(int64_t k) const { return frameVoiced(k) ? decPeriod_[static_cast<size_t>(k & decidedMask_)] : 0.0; }

bool VoiceTrack::voicedAt(double pos) const { return frameVoiced(static_cast<int64_t>(std::floor(pos / hop_ + 0.5))); }

double VoiceTrack::recentPeriod(int frames) const {
  const int64_t back = std::max<int64_t>(1, frames / hop_);
  for (int64_t k = decided_ - 1; k >= 0 && k >= decided_ - back; --k)
    if (const double p = framePeriod(k); p > 0) return p;
  return 0.0;
}

double VoiceTrack::periodAt(double pos) const {
  const double u = pos / hop_;
  const auto k0 = static_cast<int64_t>(std::floor(u));
  const double a = framePeriod(k0), b = framePeriod(k0 + 1);
  if (a > 0 && b > 0) return a + (b - a) * (u - static_cast<double>(k0));
  if (a > 0 || b > 0) return a > 0 ? a : b;
  for (int64_t d = 1; d <= 3; ++d) {
    if (const double v = framePeriod(k0 - d); v > 0) return v;
    if (const double v = framePeriod(k0 + 1 + d); v > 0) return v;
  }
  return 0.0;
}

// Normalised cross-correlation of the windows [a - h, a + h) and [b - h, b + h).
double VoiceTrack::ncc(int64_t a, int64_t b, int h, int stride) const {
  double ab = 0, aa = 0, bb = 0;
  for (int i = -h; i < h; i += stride) {
    const double x = monoAt(a + i), y = monoAt(b + i);
    ab += x * y, aa += x * x, bb += y * y;
  }
  return aa > 1e-20 && bb > 1e-20 ? ab / std::sqrt(aa * bb) : 0.0;
}

// How close the period at pos is to a pure tone: a sine half a period later
// is its own negative (correlation -1); a voice is far from that. A period
// that repeats within its first half is two periods of something: the half
// period is judged instead (a tone above the tracker's range is still a tone,
// a voice tracked an octave low is still a voice).
float VoiceTrack::tonalAt(int64_t pos, double period) const {
  for (int depth = 0; depth < 4; ++depth) {
    const double half = period * 0.5;
    if (half < 2.0) return 1.0f;
    const auto hi = static_cast<int>(half);
    const double fr = half - hi;
    const int h = std::max(2, hi);
    const int stride = h > 200 ? 2 : 1;
    double ab = 0, aa = 0, bb = 0;
    for (int i = -h; i < h; i += stride) {
      const double a = monoAt(pos + i);
      const double b = (1.0 - fr) * monoAt(pos + i + hi) + fr * monoAt(pos + i + hi + 1);
      ab += a * b, aa += a * a, bb += b * b;
    }
    if (aa < 1e-12 || bb < 1e-12) return 0.0f;
    const double r = ab / std::sqrt(aa * bb);
    if (r <= 0.8) return static_cast<float>(std::clamp((-r - 0.995) / 0.004, 0.0, 1.0));
    period = half;
  }
  return 0.0f;
}

void VoiceTrack::emit(const Mark& m) {
  marks_[static_cast<size_t>(marksDone_ & markMask_)] = m;
  ++marksDone_;
}

int64_t VoiceTrack::markNear(double pos) const {
  if (marksDone_ == 0) return -1;
  int64_t lo = firstMark(), hi = marksDone_ - 1;
  if (pos <= mark(lo).pos) return lo;
  if (pos >= mark(hi).pos) return hi;
  while (hi - lo > 1) {
    const int64_t mid = (lo + hi) / 2;
    if (mark(mid).pos <= pos)
      lo = mid;
    else
      hi = mid;
  }
  return pos - mark(lo).pos <= mark(hi).pos - pos ? lo : hi;
}

void VoiceTrack::makeMarks() {
  const int64_t avail = written_ - exLen_ / 2 - 2;  // frames whose excitation value is final
  const double decidedEnd = static_cast<double>(decided_) * hop_ - hop_ / 2.0;
  for (int guard = 0; guard < 4096; ++guard) {
    const bool have = marksDone_ > 0;
    const Mark last = have ? mark(marksDone_ - 1) : Mark{};
    if (state_ == 0) {
      // Look for the next voiced run.
      while (scan_ < decided_ && !frameVoiced(scan_)) ++scan_;
      if (scan_ >= decided_) return;
      const double P = framePeriod(scan_);
      double s0 = static_cast<double>(scan_) * hop_ - hop_ / 2.0;
      if (have) s0 = std::max(s0, last.pos + 0.5 * last.period);
      if (s0 + P + 2 >= static_cast<double>(avail)) return;
      // The run's first mark: the strongest excitation in its first period.
      int64_t q = static_cast<int64_t>(std::ceil(s0));
      float bv = -1;
      for (auto i = static_cast<int64_t>(std::ceil(s0)); i < static_cast<int64_t>(s0 + P); ++i)
        if (const float v = exAt(i); v > bv) bv = v, q = i;
      runTonal_ = tonalAt(q, P);
      // Lead marks (faded in by the engine) before it.
      for (int k = 2; k >= 1; --k) {
        const double lp = static_cast<double>(q) - k * P;
        if (lp >= 0 && (!have || lp > mark(marksDone_ - 1).pos + 0.5 * P)) emit(Mark{lp, P, 0.0f, runTonal_});
      }
      emit(Mark{static_cast<double>(q), P, 1.0f, runTonal_});
      state_ = 1;
      continue;
    }
    if (state_ == 2) {
      // Tail marks after a run: they let the engine fade the run out.
      const double P = last.period;
      const double tp = last.pos + P;
      if (tp + P >= static_cast<double>(avail)) return;
      if (tails_ >= 6 || (tails_ >= 1 && tp > runEnd_ + 0.006 * sr_) || tp > decidedEnd) {
        state_ = 0;
        scan_ = std::max(scan_, static_cast<int64_t>(std::floor(tp / hop_ + 0.5)));
        continue;
      }
      emit(Mark{tp, P, 0.0f, runTonal_});
      ++tails_;
      continue;
    }
    // Inside a run: the next period.
    double P = periodAt(last.pos);
    if (P <= 0) P = last.period;
    const double pred = last.pos + P;
    if (pred + 0.5 * P >= decidedEnd) return;
    if (pred + 1.3 * P + 2 >= static_cast<double>(avail)) return;
    if (!voicedAt(pred)) {
      state_ = 2, tails_ = 0, runEnd_ = pred;
      continue;
    }
    const auto L0 = static_cast<int64_t>(std::floor(last.pos + 0.5));
    const double off = last.pos - static_cast<double>(L0);
    const int h = std::max(4, static_cast<int>(std::min(P, last.period) * 0.5));
    const int range = std::max(2, static_cast<int>(P * 0.25));
    const int stride = h > 300 ? 3 : (h > 120 ? 2 : 1);
    const auto qc = static_cast<int64_t>(std::floor(pred - off + 0.5));
    float exMax = 1e-30f;
    for (int64_t i = qc - range - h; i <= qc + range + h; ++i) exMax = std::max(exMax, exAt(i));
    int64_t best = qc;
    double bs = -1e30, bn = 0;
    for (int64_t q = qc - range; q <= qc + range; ++q) {
      if (static_cast<double>(q) + off <= last.pos + tMin_ * 0.5) continue;
      const double c = ncc(L0, q, h, stride);
      const double dq = (static_cast<double>(q) + off - pred) / (0.25 * P);
      const double sc = c + 0.3 * exAt(q) / exMax - 0.25 * dq * dq;
      if (sc > bs) bs = sc, best = q, bn = c;
    }
    double pos = 0;
    if (bn < 0.2) {
      // The shape broke (a transition): re-anchor on the excitation.
      int64_t qa = qc;
      float bv = -1;
      for (int64_t i = qc - range; i <= qc + range; ++i)
        if (const float v = exAt(i); v > bv) bv = v, qa = i;
      pos = static_cast<double>(qa);
    } else {
      double frac = 0;
      const double ym = ncc(L0, best - 1, h, 1), y0 = ncc(L0, best, h, 1), yp = ncc(L0, best + 1, h, 1);
      const double den = ym - 2 * y0 + yp;
      if (den < -1e-12) frac = std::clamp(0.5 * (ym - yp) / den, -0.5, 0.5);
      pos = static_cast<double>(best) + frac + off;
    }
    double per = pos - last.pos;
    if (per < 0.7 * P || per > 1.4 * P) pos = pred, per = P;
    const float raw = tonalAt(static_cast<int64_t>(std::floor(pos + 0.5)), per);
    runTonal_ = 0.85f * runTonal_ + 0.15f * raw;
    emit(Mark{pos, std::clamp(per, static_cast<double>(tMin_), static_cast<double>(tMax_)), 1.0f, runTonal_});
  }
}

}  // namespace gl::pitch
