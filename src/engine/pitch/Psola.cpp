// Gridline: TD-PSOLA engine (J-140). See Psola.h.
#include "Psola.h"
#include "Kernel.h"

namespace gl::pitch {

namespace {
constexpr double kPi = 3.14159265358979323846;
int64_t pow2AtLeast(int64_t v) {
  int64_t p = 1;
  while (p < v) p <<= 1;
  return p;
}
}  // namespace

void Psola::configure(int channels, double sampleRate) {
  channels_ = std::clamp(channels, 1, 2);
  sr_ = sampleRate > 0 ? sampleRate : 44100.0;
  tMin_ = std::max(8, static_cast<int>(sr_ / 1000.0));
  tMax_ = static_cast<int>(std::ceil(sr_ / 40.0));
  unv_ = std::max(16, static_cast<int>(std::lround(0.010 * sr_)));
  yinW_ = 4 * std::max(32, static_cast<int>(std::lround(0.0232 * sr_ / 4.0)));
  frameHop_ = std::max(16, static_cast<int>(std::lround(0.0058 * sr_)));
  // Lookahead: pitch analysis (half a YIN window plus the longest lag), the
  // mark search (a period and a half), a grain (a period) and slack.
  seek_ = yinW_ / 2 + 3 * tMax_ + frameHop_ + 64;
  cap_ = static_cast<int>(pow2AtLeast(static_cast<int64_t>(seek_) + 4 * tMax_ + 4096));
  mask_ = cap_ - 1;
  for (auto& r : ring_) r.assign(static_cast<size_t>(cap_), 0.0f);
  dec_.assign(static_cast<size_t>(cap_ / 4), 0.0f);
  decMask_ = cap_ / 4 - 1;
  yinD_.assign(static_cast<size_t>(tMax_ / 4 + 4), 0.0);
  const int64_t frames = pow2AtLeast(2 * cap_ / frameHop_ + 4);
  framePeriod_.assign(static_cast<size_t>(frames), 0.0);
  frameMask_ = frames - 1;
  const int64_t mk = pow2AtLeast(2 * cap_ / tMin_ + 4);
  marks_.assign(static_cast<size_t>(mk), Mark{});
  markMask_ = mk - 1;
  const int64_t acc = pow2AtLeast(2 * static_cast<int64_t>(tMax_) * 8 + 2 * kStep + 64);
  for (auto& a : acc_) a.assign(static_cast<size_t>(acc), 0.0f);
  wacc_.assign(static_cast<size_t>(acc), 0.0f);
  accMask_ = acc - 1;
  for (auto& o : outBuf_) o.assign(static_cast<size_t>(kStep), 0.0f);
  (void)table(Quality::Realtime);
  reset();
}

void Psola::reset() {
  for (auto& r : ring_) std::fill(r.begin(), r.end(), 0.0f);
  for (auto& a : acc_) std::fill(a.begin(), a.end(), 0.0f);
  std::fill(wacc_.begin(), wacc_.end(), 0.0f);
  std::fill(dec_.begin(), dec_.end(), 0.0f);
  written_ = 0, decDone_ = 0, framesDone_ = 0, marksDone_ = 0;
  ana_ = 0, outPos_ = 0, nextOut_ = 0;
}

float Psola::monoAt(int64_t i) const {
  if (i < 0 || i >= written_ || i <= written_ - cap_) return 0.0f;
  const auto k = static_cast<size_t>(i & mask_);
  return channels_ == 2 ? 0.5f * (ring_[0][k] + ring_[1][k]) : ring_[0][k];
}

float Psola::decAt(int64_t i) const {
  if (i < 0 || i >= decDone_ || i <= decDone_ - (decMask_ + 1)) return 0.0f;
  return dec_[static_cast<size_t>(i & decMask_)];
}

// YIN on the decimated mix, window centred at `centre` (input frames).
bool Psola::yinAt(int64_t centre, double& period) const {
  const int W = yinW_ / 4;
  const int tauMax = tMax_ / 4 + 1, tauMin = std::max(2, tMin_ / 4);
  const int64_t a = (centre - yinW_ / 2) / 4;
  double e = 0;
  for (int i = 0; i < W; ++i) {
    const double v = decAt(a + i);
    e += v * v;
  }
  if (e / W < 1e-7) return false;  // below about -70 dBFS: unvoiced
  auto& d = const_cast<std::vector<double>&>(yinD_);
  double run = 0;
  int best = -1;
  double bestV = 1e9;
  double prev = 1, prev2 = 1;
  for (int tau = 1; tau <= tauMax; ++tau) {
    double s = 0;
    for (int i = 0; i < W; ++i) {
      const double df = static_cast<double>(decAt(a + i)) - decAt(a + i + tau);
      s += df * df;
    }
    run += s;
    const double dn = s * tau / std::max(run, 1e-30);
    d[static_cast<size_t>(tau)] = dn;
    (void)prev2;
    if (tau >= tauMin && best < 0 && dn < 0.15) best = tau;  // first dip under the threshold
    if (best >= 0 && tau == best + 1 && dn < d[static_cast<size_t>(best)]) best = tau;  // follow it down
    if (tau >= tauMin && dn < bestV) bestV = dn;
    prev2 = prev, prev = dn;
  }
  if (best < 0) return false;
  // Parabolic refinement.
  double t = best;
  if (best > 1 && best < tauMax) {
    const double ym = d[static_cast<size_t>(best - 1)], y0 = d[static_cast<size_t>(best)], yp = d[static_cast<size_t>(best + 1)];
    const double den = ym - 2 * y0 + yp;
    if (std::fabs(den) > 1e-12) t += std::clamp(0.5 * (ym - yp) / den, -0.5, 0.5);
  }
  period = std::clamp(4.0 * t, static_cast<double>(tMin_), static_cast<double>(tMax_));
  return true;
}

void Psola::analyse() {
  // Decimated mix (4x, a 4-frame mean: enough for a pitch tracker under 1 kHz).
  while ((decDone_ + 1) * 4 <= written_) {
    const int64_t b = decDone_ * 4;
    dec_[static_cast<size_t>(decDone_ & decMask_)] = 0.25f * (monoAt(b) + monoAt(b + 1) + monoAt(b + 2) + monoAt(b + 3));
    ++decDone_;
  }
  // Pitch frames.
  for (;;) {
    const int64_t c = framesDone_ * frameHop_;
    if ((c + yinW_ / 2 + tMax_ + 8) / 4 >= decDone_) break;
    double t = 0;
    framePeriod_[static_cast<size_t>(framesDone_ & frameMask_)] = yinAt(c, t) ? t : 0.0;
    ++framesDone_;
  }
  addMarks();
}

void Psola::addMarks() {
  auto periodAt = [&](int64_t pos, bool& ok) {
    const int64_t k = (pos + frameHop_ / 2) / frameHop_;
    ok = k >= 0 && k < framesDone_;
    if (!ok) return 0.0;
    const int64_t lo = std::max<int64_t>(0, framesDone_ - frameMask_);
    return framePeriod_[static_cast<size_t>(std::max(k, lo) & frameMask_)];
  };
  for (;;) {
    Mark next;
    if (marksDone_ == 0) {
      bool ok = false;
      const double t = periodAt(0, ok);
      if (!ok) return;
      next = Mark{0, t > 0 ? t : static_cast<double>(unv_), t > 0};
    } else {
      const Mark& last = marks_[static_cast<size_t>((marksDone_ - 1) & markMask_)];
      const auto step = static_cast<int64_t>(std::lround(last.period));
      const int64_t p = last.pos + std::max<int64_t>(1, step);
      bool ok = false;
      const double tp = periodAt(p, ok);
      if (!ok) return;
      const auto reachT = static_cast<int64_t>(std::max(tp, last.period));
      if (p + reachT + reachT / 6 + 4 >= written_) return;  // the search needs the period after p
      if (tp <= 0) {
        next = Mark{p, static_cast<double>(unv_), false};
      } else if (!last.voiced) {
        // Voicing starts: the mark goes on the strongest peak of the first period.
        int64_t best = p;
        float bv = -1;
        for (int64_t i = p; i < p + static_cast<int64_t>(tp); ++i)
          if (const float v = std::fabs(monoAt(i)); v > bv) bv = v, best = i;
        next = Mark{best, tp, true};
      } else {
        // Next period: the offset whose waveform best repeats the last period.
        const int h = static_cast<int>(std::min(tp, last.period) * 0.5);
        const int range = std::max(2, static_cast<int>(tp / 6));
        const int stride = h > 300 ? 3 : (h > 120 ? 2 : 1);
        auto score = [&](int64_t q, int st) {
          double ab = 0, bb = 0;
          for (int i = -h; i < h; i += st) {
            const double x = monoAt(last.pos + i), y = monoAt(q + i);
            ab += x * y, bb += y * y;
          }
          return ab / std::sqrt(bb + 1e-12);
        };
        int64_t best = p;
        double bs = -1e30;
        for (int64_t d = -range; d <= range; ++d) {
          const int64_t q = last.pos + static_cast<int64_t>(std::lround(tp)) + d;
          if (q <= last.pos + tMin_ / 2) continue;
          if (const double v = score(q, stride); v > bs) bs = v, best = q;
        }
        // Sub-sample position (parabola through the peak and its neighbours):
        // whole-sample marks would round every period, a few cents off and
        // jittery on high notes.
        double frac = 0;
        {
          const double ym = score(best - 1, 1), y0 = score(best, 1), yp = score(best + 1, 1);
          const double den = ym - 2 * y0 + yp;
          if (den < -1e-12) frac = std::clamp(0.5 * (ym - yp) / den, -0.5, 0.5);
        }
        // The waveform at the last mark's whole sample repeats at best + frac:
        // that is one period. The mark itself sits last.frac further on.
        const double per = static_cast<double>(best - last.pos) + frac;
        const double at = static_cast<double>(best) + frac + last.frac;
        next = Mark{static_cast<int64_t>(std::lround(at)), std::clamp(per, static_cast<double>(tMin_), static_cast<double>(tMax_)), true};
        next.frac = at - static_cast<double>(next.pos);
      }
      if (next.voiced) next.tonal = 0.5f * (tonalAt(next.pos, next.period) + (last.voiced ? last.tonal : 0.0f));
    }
    marks_[static_cast<size_t>(marksDone_ & markMask_)] = next;
    ++marksDone_;
  }
}

// How close the period at `pos` is to a pure tone: a sine half a period later
// is its own negative (correlation -1); a voice or any rich tone is far from
// that (a sawtooth: -0.5; a sung /i/ with its first formant on the
// fundamental: about -0.99). Only a purer tone (under -0.995) counts as one.
float Psola::tonalAt(int64_t pos, double period) const {
  const int h = std::max(2, static_cast<int>(period * 0.5));
  const int stride = h > 200 ? 2 : 1;
  double ab = 0, aa = 0, bb = 0;
  for (int i = -h; i < h; i += stride) {
    const double a = monoAt(pos + i), b = monoAt(pos + i + h);
    ab += a * b, aa += a * a, bb += b * b;
  }
  if (aa < 1e-12 || bb < 1e-12) return 0.0f;
  const double r = ab / std::sqrt(aa * bb);
  // Repeating within half the period: the tracker found a multiple of a
  // period too short for it (a tone over 1 kHz, where formants are moot).
  if (r > 0.8) return 1.0f;
  return static_cast<float>(std::clamp((-r - 0.995) / 0.004, 0.0, 1.0));
}

const Psola::Mark* Psola::markNear(double pos) const {
  if (marksDone_ == 0) return nullptr;
  int64_t lo = std::max<int64_t>(0, marksDone_ - markMask_), hi = marksDone_ - 1;
  auto at = [&](int64_t i) -> const Mark& { return marks_[static_cast<size_t>(i & markMask_)]; };
  if (pos <= static_cast<double>(at(lo).pos)) return &at(lo);
  if (pos >= static_cast<double>(at(hi).pos)) return &at(hi);
  while (hi - lo > 1) {
    const int64_t mid = (lo + hi) / 2;
    if (static_cast<double>(at(mid).pos) <= pos)
      lo = mid;
    else
      hi = mid;
  }
  return pos - static_cast<double>(at(lo).pos) <= static_cast<double>(at(hi).pos) - pos ? &at(lo) : &at(hi);
}

void Psola::addGrain(double centreOut, double centreIn, double halfOut, double f, bool exact) {
  const auto i0 = std::max<int64_t>(outPos_, static_cast<int64_t>(std::ceil(centreOut - halfOut + 1e-9)));
  const auto i1 = static_cast<int64_t>(std::floor(centreOut + halfOut - 1e-9));
  const SincTable& t = table(Quality::Realtime);
  const int64_t oldest = written_ - cap_ + 1;
  for (int64_t i = i0; i <= i1; ++i) {
    const double u = (static_cast<double>(i) - centreOut) / halfOut;
    const auto w = static_cast<float>(0.5 + 0.5 * std::cos(kPi * u));
    const double src = centreIn + (static_cast<double>(i) - centreOut) * f;
    const auto k = static_cast<size_t>(i & accMask_);
    for (int c = 0; c < channels_; ++c) {
      const float* rb = ring_[static_cast<size_t>(c)].data();
      float v;
      if (exact) {
        const auto j = static_cast<int64_t>(std::llround(src));
        v = j >= oldest && j >= 0 && j < written_ ? rb[static_cast<size_t>(j & mask_)] : 0.0f;
      } else {
        readWith<1>(
            [rb, this, oldest](int64_t j, int) { return j >= oldest && j >= 0 && j < written_ ? rb[static_cast<size_t>(j & mask_)] : 0.0f; },
            src, f, t, &v);
      }
      acc_[static_cast<size_t>(c)][k] += w * v;
    }
    wacc_[k] += w;
  }
}

void Psola::render(int m, double rate) {
  for (;;) {
    const double s = nextOut_;
    const double a = ana_ + (s - static_cast<double>(outPos_)) * rate;
    const Mark* mk = markNear(a);
    const bool voiced = mk && mk->voiced && std::fabs(static_cast<double>(mk->pos) - a) <= mk->period;
    // Grains are one period apart (voiced) or 10 ms apart (unvoiced). A voiced
    // grain spans two input periods, so its output half length is a period
    // over the formant factor (classic PSOLA: lowering the pitch leaves the
    // quiet stretch between two pulses, which is what makes it lower). Unvoiced
    // grains have a fixed output length (half overlap).
    //
    // A near pure tone (a whistle, a flute, a sine bass) has no envelope to
    // keep: its formant factor goes to one, so it is simply transposed.
    const double spacing = voiced ? mk->period : static_cast<double>(unv_);
    const double f = voiced ? 1.0 + (formant_ - 1.0) * (1.0 - mk->tonal) : formant_;
    const double halfOut = voiced ? std::max(spacing / f, spacing * 0.5) : spacing;
    const double centreIn = voiced ? static_cast<double>(mk->pos) + mk->frac : std::round(a);
    if (s - halfOut >= static_cast<double>(outPos_ + m)) break;
    // Whole-sample reads when nothing is resampled (same rate, same fraction).
    const double fd = (centreIn - s) - std::round(centreIn - s);
    addGrain(s, centreIn, halfOut, f, std::fabs(f - 1.0) < 1e-9 && std::fabs(fd) < 1e-6);
    nextOut_ += voiced ? std::max(1.0, spacing) : std::max(1.0, std::round(spacing));
  }
  for (int j = 0; j < m; ++j) {
    const auto k = static_cast<size_t>((outPos_ + j) & accMask_);
    // Window normalisation: where grains of different lengths meet the
    // weights do not sum to one; dividing by the sum keeps the level (with
    // a floor, so the edges of a lone grain are not blown up).
    const float g = 1.0f / std::max(0.25f, wacc_[k]);
    for (int c = 0; c < channels_; ++c) {
      outBuf_[static_cast<size_t>(c)][static_cast<size_t>(j)] = acc_[static_cast<size_t>(c)][k] * g;
      acc_[static_cast<size_t>(c)][k] = 0.0f;
    }
    wacc_[k] = 0.0f;
  }
  outPos_ += m;
}

}  // namespace gl::pitch
