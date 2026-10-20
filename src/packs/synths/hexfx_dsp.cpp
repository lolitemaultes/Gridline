// Gridline — "synths" pack, J-33 group "hex": Hex Synth post-voice chain.
//
// Laws (the reference instrument's, x = stored 0..1 position):
//   drive d = 20 x² (smoothed 0.01 / sample): y = 1 + (s − 1)·10^(−d s) for
//     s ≥ 0, −1 + (s + 1)·10^(d s) below, × (0.3 + 0.7 (1 − d/16)^8); a DC
//     blocker m += (y − m)·0.003 always runs.
//   delay  6 clocked lines (L 2 s / 0.63 / 0.41, R 2 s / 0.76 / 0.54 of it),
//          time t = x² + 0.01 (or tempo synced), feedback −0.99 x, rate 20 x²
//          (×1.1 on every other line), depth x⁴, mix x², blur-scaled taps.
//   chorus 3 lines (~22 / 17 / 20 ms), rate 5 x + 0.1 (×0.896 / 1 / 0.689),
//          depth x³, mix −x², mono input.
//   flanger 2 lines of 12 ms, delay x², feedback ±0.99 x, rate 10 x² (×1.1),
//          depth x², mix ±x.
//   phaser 6 first-order all-passes, sweep 50 Hz … 20 kHz, LFO 3.5 x² Hz,
//          depth x³ / 2, feedback 0.98 ∛x, mix x.
//   lo-fi  sample & hold every trunc(40 x) + 1 samples of a 1024 x⁴ + 4 level
//          quantiser, low-passed at 30·2^(9.3 x) Hz, added to the dry signal.
//   reverb plate engine (hexreverb_dsp.cpp).
//   EQ     8 bands at 62.5 Hz · 2^k, one octave wide, low shelf / peaks / high
//          shelf, gain 18 s|s| dB (s = 2x − 1) applied as 10^(g/20) in the
//          cookbook formulas (twice the nominal dB, like the reference).
#include <algorithm>
#include <cmath>
#include "hex_fx.h"

namespace gl::packs::synths::hex {

namespace {

inline float flush(float v) { return (v < 1e-9f && v > -1e-9f) ? 0.f : v; }

// Raw field index inside FxSlot::raw_ for parameter p (238 + 2 f + slot).
constexpr int field(int p) { return (p - HxFlDelay) / 2; }

}  // namespace

// ------------------------------------------------------------------ BbdLine
void BbdLine::reserve(size_t n) {
  for (auto& c : cyc_) {
    c.s.assign(std::max<size_t>(4, n) + 256 + kPre, 0.f);
    c.p.assign(std::max<size_t>(4, n) + 256 + kPre, 0.0);
  }
}

void BbdLine::configure(size_t n, double startPos) {
  const size_t cap = cyc_[0].s.size() > 256 + kPre ? cyc_[0].s.size() - 256 - kPre : 4;
  n_ = std::min(std::max<size_t>(4, n), cap);
  start_ = startPos;
  const double N = static_cast<double>(n_);
  sMin_ = 1.0 / N;
  s0_ = 1.0 / ((sMin_ + 0.5) * N);
  fb_ = 0, m_ = 1, inc_ = 0, depth_ = 0;
  reset();
}

void BbdLine::reset() {
  for (auto& c : cyc_) {
    const size_t used = std::min(c.s.size(), n_ + 256 + kPre);
    std::fill(c.s.begin(), c.s.begin() + static_cast<std::ptrdiff_t>(used), 0.f);
    std::fill(c.p.begin(), c.p.begin() + static_cast<std::ptrdiff_t>(used), 0.0);
    c.count = c.cursor = 0;
  }
  write_ = 0;
  pos_ = start_;
  phi_ = 0;
  sSmooth_ = s0_;
  mSmooth_ = 0;
  lpS_ = 0;
}

void BbdLine::setTime(double t) {
  t = std::clamp(t, 0.0, 0.99);
  const double N = static_cast<double>(n_);
  const double two = 2.0 / N;
  s0_ = 1.0 / (((1.0 - two) * t + two) * N);
}

float BbdLine::tick(float in) {
  const double tri = phi_ < 0.5 ? 2.0 * phi_ : 2.0 - 2.0 * phi_;
  {
    const double x = phi_ + inc_;
    phi_ = x < 1.0 ? x : x - std::floor(x);
  }
  // Read the previous cycle at the current position.
  Cycle& rd = cyc_[1 - write_];
  float y = 0.f;
  if (rd.count != 0) {
    for (int k = rd.cursor; k <= rd.count; ++k) {
      const double pk = rd.p[static_cast<size_t>(k + kPre)];
      if (pos_ <= pk) {
        const float p1 = static_cast<float>(pk), p0 = static_cast<float>(rd.p[static_cast<size_t>(k + kPre - 1)]);
        const float s0 = rd.s[static_cast<size_t>(k + kPre - 1)];
        const double fr = p0 < p1 ? (pos_ - p0) / static_cast<double>(p1 - p0) : 0.0;
        y = static_cast<float>((rd.s[static_cast<size_t>(k + kPre)] - s0) * fr + s0);
        break;
      }
      rd.cursor = k + 1;
    }
  }
  y = flush(y);
  const double speed = std::max(1.0, tri * tri * depth_ * (3.0 - 2.0 * tri) + 1.0);
  const float w = fb_ * (lpOn_ ? (lpS_ = flush(lpS_ + (y - lpS_) * 0.5f)) : y) + flush(in);
  double step = std::min(speed * sSmooth_, 0.2);
  step = std::max(step, sMin_);
  pos_ += step;
  Cycle* wr = &cyc_[write_];
  if (pos_ >= 1.0) {
    Cycle& old = cyc_[1 - write_];  // the cycle being read becomes the next write cycle
    // The finished cycle gets its pre-history from the end of the older one.
    for (int j = 1; j <= 3; ++j) {
      const int src = old.count - j + kPre;
      const int dst = kPre - j;
      wr->s[static_cast<size_t>(dst)] = src >= 0 ? old.s[static_cast<size_t>(src)] : 0.f;
      wr->p[static_cast<size_t>(dst)] = (src >= 0 ? old.p[static_cast<size_t>(src)] : 0.0) - 1.0;
    }
    if (static_cast<size_t>(wr->count + kPre) < wr->s.size()) {
      wr->s[static_cast<size_t>(wr->count + kPre)] = w;
      wr->p[static_cast<size_t>(wr->count + kPre)] = pos_;
    }
    wr->cursor = 0;
    pos_ -= 1.0;
    write_ = 1 - write_;
    wr = &cyc_[write_];
    wr->count = 0;
  }
  if (static_cast<size_t>(wr->count + kPre) < wr->s.size()) {
    wr->s[static_cast<size_t>(wr->count + kPre)] = w;
    wr->p[static_cast<size_t>(wr->count + kPre)] = pos_;
    ++wr->count;
  }
  const float out = y * mSmooth_;
  mSmooth_ = flush(mSmooth_ + (m_ - mSmooth_) * 0.01f);
  sSmooth_ += (s0_ - sSmooth_) * 0.005;
  return out;
}

// ---------------------------------------------------------------- FxSlot
void FxSlot::Lp::set(float hz, float q, float sr) {
  if (hz == f) return;
  f = hz;
  const double w = static_cast<double>(hz) * 6.283185307179586 / sr;
  const float s = std::sin(static_cast<float>(w)), c = std::cos(static_cast<float>(w));
  b = flush((1.f - c) / (s / q + 2.f));
  a1 = flush((c * q * -2.f) / (s + q));
  a2 = flush((q - s) / (s + q));
}

float FxSlot::Lp::tick(float x) {
  x = flush(x);
  const float y = (x1 + x1 + x + x2) * b - (y1 * a1 + y2 * a2);
  x2 = x1, x1 = x, y2 = y1, y1 = flush(y);
  return y1;
}

void FxSlot::configure(double sr) {
  sr_ = sr;
  reverb_.configure(sr);
  // Room for the longest line of each position (the delay's), so changing the
  // effect type never allocates.
  const double t = std::trunc(sr * 2.0);
  const double room[6] = {t, std::round(t * 0.63), std::round(t * 0.41), t, std::round(t * 0.76), std::round(t * 0.54)};
  for (int k = 0; k < 6; ++k) line_[k].reserve(static_cast<size_t>(std::max(room[k], sr * 0.03)));
  const int kind = type_;
  type_ = -1;
  setType(kind < 0 ? FxDelay : kind);
}

void FxSlot::setType(int type) {
  if (type < 0 || type >= FxKinds || type == type_) return;
  type_ = type;
  built_ = false;
  if (type == FxFlanger) {
    const size_t n = static_cast<size_t>(sr_ * 0.012);
    line_[0].configure(n, 0), line_[1].configure(n, 0);
    built_ = true;
  } else if (type == FxChorus) {
    const double t2 = std::trunc(sr_ * 0.02);
    line_[0].configure(static_cast<size_t>(std::round(t2 * 1.1)), 0);
    line_[1].configure(static_cast<size_t>(std::round(t2 * 0.854)), 0.467);
    line_[2].configure(static_cast<size_t>(sr_ * 0.02), 0.778);
    built_ = true;
  } else if (type == FxDelay) {
    const double t = std::trunc(sr_ * 2.0);
    line_[0].configure(static_cast<size_t>(t), 0);
    line_[1].configure(static_cast<size_t>(std::round(t * 0.63)), 0.27);
    line_[2].configure(static_cast<size_t>(std::round(t * 0.41)), 0.4);
    line_[3].configure(static_cast<size_t>(t), 0.3);
    line_[4].configure(static_cast<size_t>(std::round(t * 0.76)), 0.62);
    line_[5].configure(static_cast<size_t>(std::round(t * 0.54)), 0.87);
    built_ = true;
  } else {
    built_ = true;  // reverb, phaser, lo-fi
  }
  reset();
  applySetup();
}

void FxSlot::setEnabled(bool on) {
  if (bypass_ && on) wet_ = 1.f;
  bypass_ = !on;
}

void FxSlot::setParams(const float* p, int slot) {
  bool changed = false;
  for (int f = 0; f < 28; ++f) {
    const float v = std::max(0.f, p[HxFlDelay + 2 * f + slot]);
    changed = changed || v != raw_[f];
    raw_[f] = v;
  }
  const int sync = static_cast<int>(p[HxFxSync + slot]);
  changed = changed || sync != sync_;
  sync_ = sync;
  if (changed) applySetup();
}

void FxSlot::reset() {
  fade_ = 0;
  wet_ = bypass_ ? 0.f : 1.f;
  for (auto& l : line_) l.reset();
  std::fill(std::begin(apL_), std::end(apL_), 0.f);
  std::fill(std::begin(apR_), std::end(apR_), 0.f);
  fbL_ = fbR_ = phPhase_ = 0;
  phF_ = phM_ = -1.f;
  reverb_.reset();
  lpL_.x1 = lpL_.x2 = lpL_.y1 = lpL_.y2 = 0;
  lpR_.x1 = lpR_.x2 = lpR_.y1 = lpR_.y2 = 0;
  hold_ = 0;
}

void FxSlot::applySetup() {
  if (!built_) return;
  auto r = [&](int p) { return static_cast<double>(raw_[field(p)]); };
  switch (type_) {
    case FxFlanger: {
      const double t = r(HxFlDelay) * r(HxFlDelay);
      line_[0].setTime(t), line_[1].setTime(t);
      float fb = static_cast<float>(r(HxFlFeedback) * 0.99);
      if (fb < 1e-4f) fb = 0.f;
      else if (r(HxFlInvFeedback) != 0) fb = -fb;
      line_[0].setFeedback(fb), line_[1].setFeedback(fb);
      const double rate = r(HxFlRate) * r(HxFlRate) * 10.0;
      line_[0].setRate(rate, sr_), line_[1].setRate(rate * 1.1, sr_);
      const double dp = r(HxFlDepth) * r(HxFlDepth);
      line_[0].setDepth(dp), line_[1].setDepth(dp);
      float mix = static_cast<float>(r(HxFlMix));
      if (mix >= 1e-4f) {
        if (r(HxFlInvMix) != 0) mix = -mix;
      } else {
        mix = 1e-4f;
      }
      line_[0].setMix(mix), line_[1].setMix(mix);
      break;
    }
    case FxChorus: {
      const double rate = r(HxChRate) * 5.0 + 0.1;
      line_[0].setRate(rate * 0.896, sr_), line_[1].setRate(rate, sr_), line_[2].setRate(rate * 0.689, sr_);
      const float mix = static_cast<float>(-r(HxChMix) * r(HxChMix));
      const double dp = r(HxChDepth) * r(HxChDepth) * r(HxChDepth);
      for (int k = 0; k < 3; ++k) line_[k].setMix(mix), line_[k].setDepth(dp), line_[k].setFeedback(0);
      break;
    }
    case FxDelay: {
      const double rate = r(HxDlRate) * r(HxDlRate) * 20.0;
      const double t = r(HxDlTime) * r(HxDlTime) + 0.01;
      const double d2 = r(HxDlDepth) * r(HxDlDepth);
      const float fb = static_cast<float>(r(HxDlFeedback) * -0.99);
      const float mix = static_cast<float>(r(HxDlMix) * r(HxDlMix));
      const float blur = static_cast<float>(r(HxDlBlur));
      const float bm = blur * static_cast<float>(r(HxDlMix));
      const float mixes[3] = {mix, mix * blur, bm * bm};
      for (int k = 0; k < 6; ++k) {
        line_[k].setRate(k % 2 == 0 ? rate : rate * 1.1, sr_);
        line_[k].setTime(t);
        line_[k].setFeedback(fb);
        line_[k].setDepth(d2 * d2);
        line_[k].setMix(mixes[k % 3]);
      }
      break;
    }
    case FxReverb: {
      const double hc = r(HxRvHighCut);
      reverb_.setHighCut(hc * hc * hc);
      const double dc = r(HxRvDecay);
      reverb_.setDecay(0.6 * dc * dc * dc);
      reverb_.setDamping(std::fabs(1.0 - r(HxRvDamp)));
      reverb_.setWet(std::min(1.0, r(HxRvMix)) * 0.999 + 0.001);
      break;
    }
    case FxLoFi: {
      const double hz = std::min(30.0 * std::exp2(r(HxLfFilter) * 9.3), sr_ * 0.47619047619047616);
      lpL_.set(static_cast<float>(hz), 1.f, static_cast<float>(sr_));
      lpR_.set(static_cast<float>(hz), 1.f, static_cast<float>(sr_));
      break;
    }
    default: break;
  }
}

void FxSlot::process(float* L, float* R, int n, double bpm) {
  if (!built_) return;
  if (bypass_) {
    if (wet_ == 0.f) return;
    if (wet_ < 1e-5f) {
      reset();
      wet_ = 0.f;
      return;
    }
  }
  auto r = [&](int p) { return static_cast<double>(raw_[field(p)]); };
  if (type_ == FxDelay && sync_ != 0) {
    static const double kDiv[10] = {1, 15, 30, 45, 60, 120, 180, 240, 480, 960};
    const double t = static_cast<float>(0.5 / (bpm / kDiv[std::clamp(sync_, 1, 9)]));
    for (int k = 0; k < 6; ++k) line_[k].setTime(t);
  }
  const bool decay = bypass_;
  switch (type_) {
    case FxDelay:
      for (int i = 0; i < n; ++i) {
        float a = L[i];
        a = line_[0].tick(fade_ * a) * wet_ + a;
        a = line_[1].tick(fade_ * a) * wet_ + a;
        L[i] = line_[2].tick(fade_ * a) * wet_ + a;
        float b = R[i];
        b = line_[3].tick(fade_ * b) * wet_ + b;
        b = line_[4].tick(fade_ * b) * wet_ + b;
        R[i] = line_[5].tick(fade_ * b) * wet_ + b;
        fade_ = static_cast<float>(static_cast<double>(fade_) * 0.9950000001117587 + 0.004999999888241291);
        if (decay) wet_ *= 0.995f;
      }
      break;
    case FxChorus:
      for (int i = 0; i < n; ++i) {
        const float u = (L[i] + R[i]) * fade_;
        fade_ = fade_ * 0.995f + 0.005f;
        const float c1 = line_[0].tick(u), c3 = line_[2].tick(u), c2 = line_[1].tick(u);
        L[i] += (c1 + c2) * wet_;
        R[i] += (c3 + c2) * wet_;
        if (decay) wet_ *= 0.995f;
      }
      break;
    case FxFlanger:
      for (int i = 0; i < n; ++i) {
        L[i] += line_[0].tick(fade_ * L[i]) * wet_;
        R[i] += line_[1].tick(fade_ * R[i]) * wet_;
        fade_ = static_cast<float>(static_cast<double>(fade_) * 0.9950000001117587 + 0.004999999888241291);
        if (decay) wet_ *= 0.995f;
      }
      break;
    case FxPhaser: {
      const double inv = 1.0 / sr_;
      const float fb = std::cbrt(static_cast<float>(r(HxPhFeedback))) * 0.98f;
      const float dpt = static_cast<float>(r(HxPhDepth));
      const float da = dpt * dpt * 0.5f * dpt;
      const float rate = static_cast<float>(r(HxPhRate));
      const double dphi = static_cast<double>(static_cast<float>(static_cast<double>(rate * rate) * 44100.0 * inv)) * 0.0005;
      const float fq = static_cast<float>(r(HxPhFreq));
      const float ft = std::max(fq * fq, 0.f), mt = std::max(static_cast<float>(r(HxPhMix)), 1e-5f);
      if (phM_ < 0.f) phM_ = mt;
      if (phF_ < 0.f) phF_ = ft;
      const float lo = static_cast<float>(inv * 50.0), span = static_cast<float>(inv * 20000.0) - lo;
      for (int i = 0; i < n; ++i) {
        const float c = std::cos(phPhase_);
        float nph = static_cast<float>(static_cast<double>(phPhase_) + dphi);
        if (nph > 6.2831855f) nph -= 6.2831855f;
        phPhase_ = nph;
        const float f = fade_;
        fade_ = f * 0.995f + 0.005f;
        float w = ((c + 1.f) * da + phF_) * span + lo;
        if (w >= 0.499f) w = 0.499f;
        const float a = (1.f - w) / (w + 1.f);
        auto chain = [&](float* st, float u) {
          for (int k = 0; k < 6; ++k) {
            const float y = st[k] - u * a;
            st[k] = y * a + u;
            u = y;
          }
          return u;
        };
        const float yl = chain(apL_, fbL_ * fb - 1e-6f + L[i] * f);
        const float yr = chain(apR_, fbR_ * fb - 1e-6f + f * R[i]);
        fbL_ = yl, fbR_ = yr;
        const float m = phM_;
        phF_ += (ft - phF_) * 0.001f;
        phM_ += (mt - phM_) * 0.001f;
        L[i] += yl * m * wet_;
        R[i] += yr * m * wet_;
        if (decay) wet_ *= 0.995f;
      }
      break;
    }
    case FxReverb:
      if (!bypass_) wet_ = 5e-6f;
      fade_ = 1.f;
      if (bypass_) return;
      reverb_.process(L, R, n);
      break;
    case FxLoFi: {
      if (!bypass_) wet_ = 5e-6f;
      fade_ = 1.f;
      if (bypass_) return;
      const int hold = static_cast<int>(static_cast<float>(r(HxLfRate)) * 40.0f);
      for (int i = 0; i < n; ++i) {
        if (hold_ == 0) {
          const float b = static_cast<float>(r(HxLfBits));
          const float b2 = b * b;
          const float q = std::trunc(b2 * b2 * 1024.f + 4.f);
          heldL_ = (1.f / q) * std::trunc(q * L[i]);
          heldR_ = (1.f / q) * std::trunc(q * R[i]);
        }
        hold_ = hold <= hold_ ? 0 : hold_ + 1;
        const float yl = lpL_.tick(heldL_), yr = lpR_.tick(heldR_);
        L[i] += yl;
        R[i] += yr;
      }
      break;
    }
    default: break;
  }
}

// ------------------------------------------------------------- PostChain
float PostChain::Biquad::tick(float x) {
  x = flush(x);
  const float y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
  x2 = x1, x1 = x, y2 = y1, y1 = flush(y);
  return y1;
}

void PostChain::configure(double sr) {
  sr_ = sr;
  for (auto& s : slot) s.configure(sr);
  for (int b = 0; b < kEqBands; ++b) eqDesigned_[b] = 1e9f, design(b);
  reset();
}

void PostChain::reset() {
  drive_ = 0, dcL_ = dcR_ = 0;
  for (auto& s : slot) s.reset();
  for (int b = 0; b < kEqBands; ++b) {
    eqL_[b].x1 = eqL_[b].x2 = eqL_[b].y1 = eqL_[b].y2 = 0;
    eqR_[b].x1 = eqR_[b].x2 = eqR_[b].y1 = eqR_[b].y2 = 0;
  }
  silent_ = 0;
  asleep_ = false;
}

void PostChain::setEq(bool on, const float* gainDb) {
  if (on != eqOn_) {
    eqOn_ = on;
    for (int b = 0; b < kEqBands; ++b) {
      eqL_[b].x1 = eqL_[b].x2 = eqL_[b].y1 = eqL_[b].y2 = 0;
      eqR_[b].x1 = eqR_[b].x2 = eqR_[b].y1 = eqR_[b].y2 = 0;
    }
  }
  for (int b = 0; b < kEqBands; ++b) {
    eqDb_[b] = gainDb[b];
    if (eqDb_[b] != eqDesigned_[b]) design(b);
  }
}

void PostChain::design(int b) {
  eqDesigned_[b] = eqDb_[b];
  const float A = std::pow(10.f, eqDb_[b] * 0.05f);  // the reference's (doubled) gain
  const float f = static_cast<float>(62.5 * std::exp2(b));
  float w0 = f * 6.283185f / static_cast<float>(sr_);
  w0 = std::clamp(w0, 0.f, 3.f);
  const float s = std::sin(w0), c = std::cos(w0);
  const double sh = std::sinh((static_cast<double>(w0) * 1.0 * 0.3465736) / static_cast<double>(s));
  float b0, b1, b2, a0, a1, a2;
  if (b == 0 || b == kEqBands - 1) {
    const float twoSqrtAalpha = std::sqrt(A) * static_cast<float>(sh + sh) * s;
    if (b == 0) {  // low shelf
      b0 = A * ((A + 1) - (A - 1) * c + twoSqrtAalpha);
      b1 = 2 * A * ((A - 1) - (A + 1) * c);
      b2 = A * ((A + 1) - (A - 1) * c - twoSqrtAalpha);
      a0 = (A + 1) + (A - 1) * c + twoSqrtAalpha;
      a1 = -2 * ((A - 1) + (A + 1) * c);
      a2 = (A + 1) + (A - 1) * c - twoSqrtAalpha;
    } else {  // high shelf
      b0 = A * ((A + 1) + (A - 1) * c + twoSqrtAalpha);
      b1 = -2 * A * ((A - 1) + (A + 1) * c);
      b2 = A * ((A + 1) + (A - 1) * c - twoSqrtAalpha);
      a0 = (A + 1) - (A - 1) * c + twoSqrtAalpha;
      a1 = 2 * ((A - 1) - (A + 1) * c);
      a2 = (A + 1) - (A - 1) * c - twoSqrtAalpha;
    }
  } else {  // peaking
    const float alpha = static_cast<float>(sh + sh) * 0.5f * s;
    b0 = 1 + alpha * A, b1 = -2 * c, b2 = 1 - alpha * A;
    a0 = 1 + alpha / A, a1 = -2 * c, a2 = 1 - alpha / A;
  }
  const float inv = 1.f / a0;
  for (Biquad* q : {&eqL_[b], &eqR_[b]}) {
    q->b0 = flush(b0 * inv), q->b1 = flush(b1 * inv), q->b2 = flush(b2 * inv);
    q->a1 = flush(a1 * inv), q->a2 = flush(a2 * inv);
  }
}

void PostChain::process(float* L, float* R, int n, double bpm) {
  // Drive + DC blocker (always).
  bool loud = false;
  for (int i = 0; i < n; ++i) {
    const float sl = L[i], sr = R[i];
    loud = loud || sl > 1e-5f || sl < -1e-5f || sr > 1e-5f || sr < -1e-5f;
    drive_ = drive_ + (driveTarget_ - drive_) * 0.01f;
    float yl = sl, yr = sr;
    if (drive_ < 0.001f && driveTarget_ < 0.001f) {
      drive_ = 0;
    } else {
      const float k = drive_ * -0.0625f + 1.f;
      float k2 = k * k;
      k2 = k2 * k2;
      const double mk = static_cast<double>(k2 * k2 * 0.7f + 0.3f);
      auto shape = [&](float s) {
        double y = s >= 0.f ? std::pow(10.0, static_cast<double>(-s * drive_)) * (static_cast<double>(s) - 1.0) + 1.0
                            : std::pow(10.0, static_cast<double>(s * drive_)) * (static_cast<double>(s) + 1.0) - 1.0;
        return static_cast<float>(y * mk);
      };
      yl = shape(sl), yr = shape(sr);
    }
    dcL_ = dcL_ + (yl - dcL_) * 0.003f;
    dcR_ = dcR_ + (yr - dcR_) * 0.003f;
    dcL_ = flush(dcL_), dcR_ = flush(dcR_);
    L[i] = yl - dcL_;
    R[i] = yr - dcR_;
  }
  if (loud) asleep_ = false, silent_ = 0;
  if (!(asleep_ && !loud)) {
    slot[0].process(L, R, n, bpm);
    slot[1].process(L, R, n, bpm);
    if (eqOn_)
      for (int b = 0; b < kEqBands; ++b)
        for (int i = 0; i < n; ++i) L[i] = eqL_[b].tick(L[i]), R[i] = eqR_[b].tick(R[i]);
  }
  bool quiet = true;
  for (int i = 0; i < n && quiet; ++i) quiet = L[i] <= 1e-5f && L[i] >= -1e-5f && R[i] <= 1e-5f && R[i] >= -1e-5f;
  silent_ = quiet ? silent_ + n : 0;
  if (silent_ > static_cast<int64_t>(2.1 * sr_) && !loud && !asleep_) {
    slot[0].reset(), slot[1].reset();
    for (int b = 0; b < kEqBands; ++b) {
      eqL_[b].x1 = eqL_[b].x2 = eqL_[b].y1 = eqL_[b].y2 = 0;
      eqR_[b].x1 = eqR_[b].x2 = eqR_[b].y1 = eqR_[b].y2 = 0;
    }
    asleep_ = true;
  }
}

}  // namespace gl::packs::synths::hex
