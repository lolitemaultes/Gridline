// Gridline: pitch-synchronous voice engine (J-140, rebuilt in J-144). See Psola.h.
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
// Grain shape away from unison: the rise before the excitation and the decay
// after it, in periods (full shape from 5 semitones on; 1, 1 is the classic
// two-period Hann window). A shorter rise keeps less of the previous period
// (its harmonic comb); much shorter than half a period it starts to cut the
// voice's own waveform and the spectral envelope suffers. Measured on real
// voices, half a period is the balance.
constexpr double kRise = 0.5, kDecay = 1.0, kShapeOctaves = 5.0 / 12.0;
constexpr double kMaxReadRate = 8.0;  // read rates the input history is sized for
}  // namespace

void Psola::configure(int channels, double sampleRate) {
  channels_ = std::clamp(channels, 1, 2);
  sr_ = sampleRate > 0 ? sampleRate : 44100.0;
  tMax_ = static_cast<int>(std::ceil(sr_ / 50.0));
  hn_ = std::max(32, static_cast<int>(std::lround(0.010 * sr_)));
  search_ = std::max(16, static_cast<int>(std::lround(0.008 * sr_)));
  ramp_ = std::max(16, static_cast<int>(std::lround(0.008 * sr_)));
  horizon_ = 2 * tMax_ + kStep;
  track_.configure(sr_, 0);
  lead_ = track_.latency() + tMax_ + hn_ + search_ + 64;
  cap_ = pow2AtLeast(lead_ + static_cast<int64_t>(kMaxReadRate * (horizon_ + kStep)) + 4 * tMax_ + 2 * (hn_ + search_) + 1024);
  mask_ = cap_ - 1;
  track_.configure(sr_, cap_);
  for (auto& r : ring_) r.assign(static_cast<size_t>(cap_), 0.0f);
  const int64_t acc = pow2AtLeast(horizon_ + 2 * tMax_ + 2 * kStep + 2 * hn_ + 64);
  accMask_ = acc - 1;
  for (auto& a : accV_) a.assign(static_cast<size_t>(acc), 0.0f);
  for (auto& a : accN_) a.assign(static_cast<size_t>(acc), 0.0f);
  const SincTable& t = table(Quality::Realtime);
  kern_.assign(static_cast<size_t>(2 * static_cast<int>(std::ceil(t.zeroCrossings / cutoffOf(1.0, t))) + 4), 0.0f);
  for (auto& o : outBuf_) o.assign(static_cast<size_t>(kStep), 0.0f);
  reset();
}

void Psola::reset() {
  track_.reset();
  for (auto& r : ring_) std::fill(r.begin(), r.end(), 0.0f);
  for (auto& a : accV_) std::fill(a.begin(), a.end(), 0.0f);
  for (auto& a : accN_) std::fill(a.begin(), a.end(), 0.0f);
  written_ = 0, ana_ = 0, outPos_ = 0;
  vNext_ = 0, nRegion_ = 0, nPrev_ = 0, gv_ = -1;
  fresh_ = true;
  lastIdx_ = -1;
}

// One grain into the voiced accumulator: output centre s, input centre c,
// left / right half lengths (output frames), read rate f.
void Psola::addGrain(double s, double c, double lo, double ro, double f) {
  const auto i0 = std::max<int64_t>(outPos_, static_cast<int64_t>(std::ceil(s - lo + 1e-9)));
  const auto i1 = static_cast<int64_t>(std::floor(s + ro - 1e-9));
  if (i1 < i0) return;
  const SincTable& t = table(Quality::Realtime);
  auto weight = [&](int64_t i) {
    const double u = static_cast<double>(i) - s;
    return static_cast<float>(u < 0 ? 0.5 + 0.5 * std::cos(kPi * u / lo) : 0.5 + 0.5 * std::cos(kPi * u / ro));
  };
  if (std::fabs(f - 1.0) < 1e-9) {
    // Read rate 1: every sample of the grain has the same fractional offset,
    // so one fractional-delay kernel serves the whole grain.
    const double d = c - s;
    auto base = static_cast<int64_t>(std::floor(d));
    double frac = d - static_cast<double>(base);
    if (frac > 1.0 - 1e-7) frac = 0, ++base;
    if (frac < 1e-7) {
      for (int64_t i = i0; i <= i1; ++i) {
        const float w = weight(i);
        const auto k = static_cast<size_t>(i & accMask_);
        for (int ch = 0; ch < channels_; ++ch) accV_[static_cast<size_t>(ch)][k] += w * at(ch, i + base);
      }
      return;
    }
    const double fc = cutoffOf(1.0, t);
    const int half = static_cast<int>(kern_.size() / 2) - 1;
    double sum = 0;
    for (int k = -half + 1; k <= half; ++k) {
      const double x = (frac - k) * fc;
      const float v = std::fabs(x) < t.zeroCrossings ? t.at(x) : 0.0f;
      kern_[static_cast<size_t>(k + half - 1)] = v;
      sum += v;
    }
    const auto g = static_cast<float>(sum > 1e-9 ? 1.0 / sum : 0.0);
    const int taps = 2 * half;
    for (int64_t i = i0; i <= i1; ++i) {
      const float w = weight(i) * g;
      const auto k = static_cast<size_t>(i & accMask_);
      const int64_t j0 = i + base - half + 1;
      for (int ch = 0; ch < channels_; ++ch) {
        float v = 0;
        if (j0 >= 0 && j0 > written_ - cap_ && j0 + taps <= written_) {
          const float* rb = ring_[static_cast<size_t>(ch)].data();
          for (int q = 0; q < taps; ++q) v += kern_[static_cast<size_t>(q)] * rb[static_cast<size_t>((j0 + q) & mask_)];
        } else {
          for (int q = 0; q < taps; ++q) v += kern_[static_cast<size_t>(q)] * at(ch, j0 + q);
        }
        accV_[static_cast<size_t>(ch)][k] += w * v;
      }
    }
    return;
  }
  // Resampled grain (a formant shift, a pure tone transposed): band-limited reads.
  for (int64_t i = i0; i <= i1; ++i) {
    const float w = weight(i);
    const auto k = static_cast<size_t>(i & accMask_);
    const double src = c + (static_cast<double>(i) - s) * f;
    for (int ch = 0; ch < channels_; ++ch) {
      float v;
      readWith<1>([this, ch](int64_t j, int) { return at(ch, j); }, src, f, t, &v);
      accV_[static_cast<size_t>(ch)][k] += w * v;
    }
  }
}

void Psola::voicedGrain(double s, double a, double rate) {
  const int64_t idx = track_.markNear(a);
  const double idle = std::max(16.0, 0.0025 * sr_);
  if (idx < 0 || std::fabs(track_.mark(idx).pos - a) > 1.5 * track_.mark(idx).period) {
    vNext_ += idle;
    fresh_ = true;
    return;
  }
  const VoiceTrack::Mark& mk = track_.mark(idx);
  const double P = mk.period;
  // The period after this mark (the spacing to the next one), when known.
  double Pn = P;
  if (idx + 1 < track_.markCount()) {
    const VoiceTrack::Mark& nx = track_.mark(idx + 1);
    if (std::fabs(nx.pos - mk.pos - nx.period) < 0.5 && nx.period > 0) Pn = nx.period;
  }
  // A new chain of marks (the tracker re-started between this mark and the
  // last one used) starts afresh too.
  if (!fresh_ && idx > lastIdx_) {
    for (int64_t j = std::max(lastIdx_ + 1, track_.firstMark() + 1); j <= idx && !fresh_; ++j)
      if (std::fabs(track_.mark(j).pos - track_.mark(j - 1).pos - track_.mark(j).period) > 0.5) fresh_ = true;
  }
  lastIdx_ = std::max(lastIdx_, idx);
  const double tau = mk.tonal;
  // Pure tones are transposed (their grains read at the pitch ratio), voices
  // keep their formants (grains read at the formant factor).
  const double f = (1.0 - tau) * formant_ + tau * pitch_;
  const double amt = std::clamp(std::fabs(std::log2(pitch_)) / kShapeOctaves, 0.0, 1.0);
  double rise = 1.0 - (1.0 - kRise) * amt, decay = 1.0 - (1.0 - kDecay) * amt;
  // Pitching up, the decay ends with the new period: grains overlap less, so
  // a waveform does not pile up on shifted copies of itself (peaks).
  if (pitch_ > 1.0) decay *= std::pow(pitch_, -amt);
  rise += (1.0 - rise) * tau, decay += (1.0 - decay) * tau;
  // Near unison the halves reach the neighbouring marks (they sum to one).
  const double lo = std::min(rise * P / f, static_cast<double>(horizon_ - 1));
  const double ro = std::min(decay * Pn / f, 2.0 * tMax_);
  if (fresh_) {
    // A run's first grain sits where its mark plays (at unison the grains then
    // land exactly on the input's own periods: the voice comes out unchanged),
    // as long as the whole grain is still ahead of the output (at the very
    // start nothing has played: the part before frame 0 is simply not there).
    const double s2 = static_cast<double>(outPos_) + (mk.pos - ana_) / std::max(1e-6, rate);
    const double late = std::min(1.5 * P / pitch_, static_cast<double>(tMax_));
    const bool ahead = s2 - lo >= static_cast<double>(outPos_) || (outPos_ == 0 && s2 >= 0);
    if (ahead && s2 >= s - P / pitch_ && s2 < s + late) s = s2;
    fresh_ = false;
  }
  addGrain(s, mk.pos, lo, ro, f);
  vNext_ = s + Pn / pitch_;
}

// Best input centre for the next noise grain near `want`: the one whose
// lead-in matches the previous grain's continuation (`cont`) best.
double Psola::noiseSearch(double want, double cont) const {
  const double f = formant_;
  const auto w0 = static_cast<int64_t>(std::floor(want + 0.5));
  if (std::fabs(cont - want) <= search_ * 0.5) return cont;  // carry on: no seam
  // Similarity of [c - hn*f, c) with [cont - hn*f, cont) (mono, coarse then fine).
  const int n = hn_;
  const int stride = std::max(1, n / 96);
  auto score = [&](int64_t c) {
    double ab = 0, aa = 0, bb = 0;
    for (int t = 0; t < n; t += stride) {
      const double x = track_.monoAt(static_cast<int64_t>(std::floor(cont + 0.5)) - n + t);
      const double y = track_.monoAt(c - n + t);
      ab += x * y, aa += x * x, bb += y * y;
    }
    return aa > 1e-20 && bb > 1e-20 ? ab / std::sqrt(aa * bb) : 0.0;
  };
  (void)f;
  int64_t best = w0;
  double bs = -2;
  for (int64_t c = w0 - search_; c <= w0 + search_; c += 4)
    if (const double v = score(c); v > bs) bs = v, best = c;
  const int64_t b0 = best;
  for (int64_t c = b0 - 3; c <= b0 + 3; ++c)
    if (const double v = score(c); v > bs) bs = v, best = c;
  return static_cast<double>(best);
}

// The next noise region [nRegion_, nRegion_ + hn_): the previous grain's
// tail crossfaded into the next grain's head.
void Psola::noiseRegion(double rate) {
  const double f = formant_;
  const double want = ana_ + (static_cast<double>(nRegion_ + hn_) - static_cast<double>(outPos_)) * rate;
  const double cont = nPrev_ + hn_ * f;
  const double c = noiseSearch(want, cont);
  const SincTable& t = table(Quality::Realtime);
  const bool exact = std::fabs(f - 1.0) < 1e-9 && std::fabs(nPrev_ - std::floor(nPrev_)) < 1e-9 && std::fabs(c - std::floor(c)) < 1e-9;
  auto read = [&](int ch, double pos) {
    if (exact) return at(ch, static_cast<int64_t>(pos));
    float v;
    readWith<1>([this, ch](int64_t j, int) { return at(ch, j); }, pos, f, t, &v);
    return v;
  };
  const bool same = std::fabs(c - cont) < 1e-9;
  // Correlation of the two grains over the region (mono).
  double rho = 1.0;
  if (!same) {
    double ab = 0, aa = 0, bb = 0;
    for (int i = 0; i < hn_; i += 2) {
      const double x = track_.monoAt(static_cast<int64_t>(std::floor(nPrev_ + i * f + 0.5)));
      const double y = track_.monoAt(static_cast<int64_t>(std::floor(c - (hn_ - i) * f + 0.5)));
      ab += x * y, aa += x * x, bb += y * y;
    }
    rho = aa > 1e-20 && bb > 1e-20 ? std::clamp(ab / std::sqrt(aa * bb), 0.0, 1.0) : 1.0;
  }
  for (int i = 0; i < hn_; ++i) {
    const auto k = static_cast<size_t>((nRegion_ + i) & accMask_);
    if (same) {
      for (int ch = 0; ch < channels_; ++ch) accN_[static_cast<size_t>(ch)][k] = read(ch, nPrev_ + i * f);
      continue;
    }
    const double u = (i + 0.5) / hn_;
    const double w2 = 0.5 - 0.5 * std::cos(kPi * u), w1 = 1.0 - w2;
    // Amplitude-complementary for correlated grains, power-complementary for uncorrelated ones.
    const double g = 1.0 / std::sqrt(std::max(1e-6, w1 * w1 + w2 * w2 + 2 * rho * w1 * w2));
    for (int ch = 0; ch < channels_; ++ch)
      accN_[static_cast<size_t>(ch)][k] = static_cast<float>(g * (w1 * read(ch, nPrev_ + i * f) + w2 * read(ch, c - (hn_ - i) * f)));
  }
  nPrev_ = c;
  nRegion_ += hn_;
}

void Psola::render(int m, double rate) {
  // Voiced grains, laid out up to a horizon ahead (whole grains only).
  while (vNext_ < static_cast<double>(outPos_ + m + horizon_ - 1)) {
    const double a = ana_ + (vNext_ - static_cast<double>(outPos_)) * rate;
    voicedGrain(vNext_, a, rate);
  }
  while (nRegion_ < outPos_ + m) noiseRegion(rate);
  const double look = 0.5 * ramp_ * rate;
  const double stepG = 1.0 / ramp_;
  double target = 0.0;
  for (int j = 0; j < m; ++j) {
    const auto k = static_cast<size_t>((outPos_ + j) & accMask_);
    const double a = ana_ + j * rate;
    if ((j & 15) == 0) {
      // Voiced where the track says so and marks are there to make grains
      // from (at the very end of a sound the voicing can outlast the marks).
      const double at = a + look;
      target = 0.0;
      if (track_.voicedAt(at)) {
        const int64_t idx = track_.markNear(at);
        if (idx >= 0 && std::fabs(track_.mark(idx).pos - at) <= 1.5 * track_.mark(idx).period) target = 1.0;
      }
    }
    if (gv_ < 0) gv_ = target;
    gv_ += std::clamp(target - gv_, -stepG, stepG);
    const auto gv = static_cast<float>(gv_);
    for (int c = 0; c < channels_; ++c) {
      float v = gv * accV_[static_cast<size_t>(c)][k] + (1.0f - gv) * accN_[static_cast<size_t>(c)][k];
      if (std::fabs(v) < 1e-30f) v = 0.0f;
      outBuf_[static_cast<size_t>(c)][static_cast<size_t>(j)] = v;
      accV_[static_cast<size_t>(c)][k] = 0.0f;
      accN_[static_cast<size_t>(c)][k] = 0.0f;
    }
  }
  outPos_ += m;
}

}  // namespace gl::pitch
