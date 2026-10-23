// Gridline — J-35: reference engine of the stereo feedback delay (gl.delay
// DlyModel = 3; behaviour study). Also the chorus
// core of the module rack (packs/dynamics).
//
// 26 normalised controls (the reference's own order) drive: input wet,
// delay type (fake stereo / true stereo / ping-pong / off), tempo sync,
// keep pitch (two read heads cross-faded over 100 ms, else a tape-style
// glide), time, R/L offset (tap offset, input pre-delay or the ping-pong input
// balance), time smoothing, output stereo spread, a sine LFO (rate, delay
// depth, filter-cutoff depth), one diffusion all-pass per channel and head in
// the loop, distortion (envelope limiter or quadratic-knee saturator),
// feedback, a state-variable feedback filter, rate / bit reduction, output
// wet, a one-pole tone filter and dry. Audio is float; times are double.
// Buffers are sized in init(); process never allocates.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace gl::d3ref {

enum Param {
  PInWet = 0, PType, PSync, PKeepPitch, PTime, POffset, PSmooth, PSpread, PModRate, PModTime, PModCut, PDifLevel, PDifSpread,
  PDistType, PFeedback, PFilter, PCutoff, PRes, PThresh, PHard, PSym, PRate, PBits, POutWet, PTone, PDry, PCount
};
// Integer ranges of the stored controls (n = (v − lo) / (hi − lo)).
inline constexpr int kLo[PCount] = {0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -3000, 0};
inline constexpr int kHi[PCount] = {6000, 3, 1, 1, 768, 6000, 1000, 6000, 6000, 6000, 6000, 6000, 6000, 1, 6000, 3, 6000, 6000, 6000, 6000, 6000, 60000, 6000, 6000, 3000, 6000};
inline double toNorm(int k, double v) { return std::clamp((v - kLo[k]) / static_cast<double>(kHi[k] - kLo[k]), 0.0, 1.0); }
inline double fromNorm(int k, double n) { return kLo[k] + std::clamp(n, 0.0, 1.0) * (kHi[k] - kLo[k]); }
// Smoothed controls (others apply at once).
inline bool smoothed(int k) { return k == 0 || k == 7 || k == 10 || k == 11 || k == 14 || (k >= 16 && k <= 25); }
inline int choice(float n, int count) { return static_cast<int>(n * static_cast<float>(count - 1) + 0.5f); }

inline constexpr float kLpTab[5] = {270, 480, 1300, 7000, 22000};
inline constexpr float kHpTab[5] = {20, 90, 400, 1300, 4200};
inline constexpr float kRateTab[7] = {8.872f, 3.808f, 1.833f, 0.878f, 0.398f, 0.15f, 0.05f};
inline float lerpTab(const float* t, float x) {
  const int i = static_cast<int>(x);
  const float f = x - static_cast<float>(i);
  return t[i] * (1 - f) + t[i + 1] * f;
}
// Free (unsynced) delay time in seconds.
inline double freeTimeSec(double n) {
  const double x = 0.94 * n + 0.06;
  return (x * x * x - 0.000216) * 1.00021604667 * 0.999 + 0.001;
}
// Distortion threshold law: dB and make-up gain.
inline float threshDb(float n) { return -30.f * (0.95f * n); }
inline float makeUp(float n) {
  const float p = 0.95f * n, p2 = p * p;
  const float d = (((1.f + 6.8819f * p2) - 3.1787f * p2) - 7.5177f * p2 * p) + 2.9843f * p2 * p2;
  return std::pow(1.f / d, 0.707f);
}
// Saturator knee (J-68: shared with the editor's transfer display): linear up
// to hard * top, then a quadratic knee reaching top, per polarity (symmetry
// shifts the two tops).
struct Knee {
  float lp = 1, kp = 1e-11f, ln = 1, kn = 1e-11f;
};
inline Knee kneeFor(float thrN, float hard, float sym) {
  const float T = std::pow(10.f, threshDb(thrN) / 20.f);
  const float A = (1 + sym) * T, B = (1 - sym) * T;
  Knee k;
  k.lp = hard * A, k.kp = std::max(1e-11f, A - k.lp);
  k.ln = hard * B, k.kn = std::max(1e-11f, B - k.ln);
  return k;
}
inline float kneeApply(const Knee& c, float x) {
  const bool neg = x < 0;
  const float v = neg ? -x : x, l = neg ? c.ln : c.lp, k = neg ? c.kn : c.kp;
  float y;
  if (v <= l) y = v;
  else {
    const float d = std::min(v - l, 2 * k);
    y = l + d * (1 - d * 0.25f / k);
  }
  return neg ? -y : y;
}

struct Ring {
  std::vector<float> buf;
  uint32_t mask = 0, w = 0;
  void alloc(uint32_t size) {
    buf.assign(size, 0.f);
    mask = size - 1, w = 0;
  }
  void write(float x) {
    buf[w] = x;
    w = (w + 1) & mask;
  }
  float read(float d) const {
    const uint32_t i = static_cast<uint32_t>(std::max(0.f, d));
    const float f = d - static_cast<float>(i);
    return (1 - f) * buf[(w - i) & mask] + f * buf[(w - i - 1) & mask];
  }
  void clear() { std::fill(buf.begin(), buf.end(), 0.f), w = 0; }
};

struct OnePole {
  float b0 = 1, b1 = 0, a1 = 0, x1 = 0, y1 = 0;
  void lp(float f, float sr) {
    const float t = std::tan(f * 3.1415927f / sr);
    b0 = b1 = t / (1 + t), a1 = (1 - t) / (1 + t);
  }
  void hp(float f, float sr) {
    const float t = std::tan(f * 3.1415927f / sr);
    b0 = 1 / (1 + t), b1 = -b0, a1 = (1 - t) / (1 + t);
  }
  void bypass() { b0 = 1, b1 = a1 = 0; }
  float run(float x) {
    const float y = b0 * x + b1 * x1 + a1 * y1;
    x1 = x, y1 = y;
    return y;
  }
};

struct Svf {
  float a1 = 1, a2 = 0, a3 = 0, m0 = 0, m1 = 0, m2 = 1, s1 = 0, s2 = 0;
  void design(int mode, float f, float Q, float sr) {
    const float w = std::min(f, 0.49f * sr) * (3.1415f / sr);
    const float g = std::tan(w), k = 1.f / Q;
    a1 = 1.f / (1.f + g * (g + k)), a2 = g * a1, a3 = g * a2;
    if (mode == 0) m0 = 0, m1 = 0, m2 = 1;
    else if (mode == 1) m0 = 1, m1 = -k, m2 = -1;
    else m0 = 0, m1 = k, m2 = 0;
  }
  void copyCoefs(const Svf& o) { a1 = o.a1, a2 = o.a2, a3 = o.a3, m0 = o.m0, m1 = o.m1, m2 = o.m2; }
  float run(float x) {
    const float v3 = x - s2, v1 = a1 * s1 + a2 * v3, v2 = s2 + a2 * s1 + a3 * v3;
    s1 = 2 * v1 - s1, s2 = 2 * v2 - s2;
    return m0 * x + m1 * v1 + m2 * v2;
  }
};

class Engine {
 public:
  void init(double sr) {
    sr_ = sr;
    srf_ = static_cast<float>(sr);
    const uint32_t big = sr <= 61440 ? (1u << 20) : sr <= 126976 ? (1u << 21) : (1u << 22);
    loopL_.alloc(big), loopR_.alloc(big), preL_.alloc(big / 4), preR_.alloc(big / 4);
    for (auto& a : ap_) a.alloc(32768);
    halfSr_ = 0.5f * srf_;
    rrScale_ = srf_ / 44100.f;
    for (auto& d : dc_) d.hp(10.f, 44100.f);
    xfLen_ = static_cast<int>(0.1 * sr);
    xfStep_ = 1.f / static_cast<float>(std::max(1, xfLen_));
    depthCoef_ = static_cast<float>(1.0 - std::pow(0.01, 1.0 / (0.1 * sr)));
    smoothC_ = static_cast<float>(1.0 - std::pow(0.01, 1.0 / (0.005 * sr)));
    primed_ = false;
    fullReset();
  }
  bool ready() const { return sr_ > 0; }
  // Lines, filters and crushers cleared (LFO, smoothers and envelopes kept, as the reference).
  void reset() { fullReset(); }

  // n: the 26 normalised controls; bpm: host tempo. In-place stereo.
  void process(const float* n, double bpm, float* L, float* R, int count) {
    if (!primed_) {  // first call: every control applied as given (the host sends them all at once)
      for (int k = 0; k < PCount; ++k) cur_[k] = target_[k] = std::clamp(n[k], 0.f, 1.f), apply(k);
      bpm_ = bpm;
      primed_ = true;
      retime_ = true;
    }
    for (int k = 0; k < PCount; ++k) {
      const float v = std::clamp(n[k], 0.f, 1.f);
      if (v == target_[k]) continue;
      target_[k] = v;
      if (!smoothed(k)) cur_[k] = v, apply(k);
    }
    if (bpm != bpm_) bpm_ = bpm, retime_ = true;
    for (int i = 0; i < count; ++i) {
      if ((tick_++ & 7) == 0)
        for (int k = 0; k < PCount; ++k)
          if (smoothed(k) && cur_[k] != target_[k]) {
            float c = cur_[k] + (target_[k] - cur_[k]) * smoothC_;
            if (std::fabs(c - target_[k]) < 1e-4f) c = target_[k];
            cur_[k] = c;
            apply(k);
          }
      float l = L[i], r = R[i];
      if (std::fabs(l) < 1e-13f) l = 0;
      if (std::fabs(r) < 1e-13f) r = 0;
      sample(l, r);
      L[i] = l, R[i] = r;
    }
  }

 private:
  // ---- parameter setters
  void apply(int k) {
    const float n = cur_[k];
    switch (k) {
      case PInWet: inWet_ = n; break;
      case PType: {
        const int t = std::clamp(choice(n, 4), 0, 3);
        if (t != type_) type_ = t, fullReset();
        retime_ = true;
        break;
      }
      case PSync: sync_ = n >= 0.5f, retime_ = true; break;
      case PKeepPitch: {
        const bool kp = n >= 0.5f;
        if (!kp && keepPitch_) retime_ = true;
        keepPitch_ = kp;
        break;
      }
      case PTime: timeN_ = n, retime_ = true; break;
      case POffset: offN_ = n, retime_ = true; break;
      case PSmooth: glide_ = n == 0 ? 1.0 : 1.0 - std::pow(0.01, 1.0 / (static_cast<double>(n) * sr_)); break;
      case PSpread: sA_ = 0.5f + 0.5f * n, sB_ = 1 - sA_; break;
      case PModRate: {
        const float P = lerpTab(kRateTab, (0.99f * n) * 5.9994f);
        lfoInc_ = 1.f / (P * srf_);
        rateDepth_ = std::sqrt(P / 8.872f);
        break;
      }
      case PModTime: modAmt_ = 0.1f * n * n; break;
      case PModCut: modCut_ = 0.5f * n; break;
      case PDifLevel: apK_ = std::clamp(n, 0.f, 1.f); break;
      case PDifSpread: difSpread_ = n, retime_ = true; break;
      case PDistType: distType_ = std::clamp(choice(n, 2), 0, 1); break;
      case PFeedback: fb_ = 1.25f * n; break;
      case PFilter: filter_ = std::clamp(choice(n, 4), 0, 3); break;
      case PCutoff: cut_ = n; break;
      case PRes: res_ = n; break;
      case PThresh:
      case PHard:
      case PSym: {
        const float tn = k == PThresh ? n : cur_[PThresh];
        thrDb_ = threshDb(tn);
        M_ = makeUp(tn);
        limLevel_ = std::pow(10.f, thrDb_ / 20.f) * M_;
        knee_ = kneeFor(tn, cur_[PHard], cur_[PSym]);
        break;
      }
      case PRate: {
        const float p = 1 - n;
        rr_ = p * p * p;
        break;
      }
      case PBits: {
        const float p = 1 - n;
        bq_ = p * p;
        scale_ = std::exp2(2.f + 14.f * (1.f - std::sqrt(p)));
        break;
      }
      case POutWet: wet_ = n; break;
      case PTone:
        for (auto& t : tone_) {
          if (n < 0.5f) t.lp(lerpTab(kLpTab, 2 * n * 3.9999f), srf_);
          else if (n == 0.5f) t.bypass();
          else t.hp(lerpTab(kHpTab, 2 * (n - 0.5f) * 3.9999f), srf_);
        }
        break;
      case PDry: dry_ = n; break;
      default: break;
    }
  }

  // ---- timing
  void retimeHead(int h) {
    double s;
    if (sync_) {
      const int ticks = static_cast<int>(timeN_ * 767.0f + 0.5f) + 1;
      const float beats = static_cast<float>(ticks) / 192.f;
      s = beats * 60.0 / std::max(1.0, bpm_);
    } else {
      s = freeTimeSec(timeN_);
    }
    const double T = std::trunc(s * sr_);
    const double D = std::trunc(std::max(1.0, (590.0 * difSpread_ + 65.0) * (sr_ / 44100.0) * std::min(1.0, T / (0.015 * sr_))));
    const double o = offN_, a = std::fabs(o - 0.5) * 2;
    Head& H = tgt_[h];
    H.dif = D;
    if (type_ == 0) {
      const double base = std::trunc(std::max(1.0, T - D)), other = std::trunc(base + (T - T * (1 - a / 2)));
      H.tapA = base, H.tapB = other;
      if (o < 0.5) std::swap(H.tapA, H.tapB);
    } else if (type_ <= 2) {
      H.loop = std::trunc(std::max(1.0, T - D));
      const double pre = std::trunc(std::max(1.0, T * a * a / 4));
      H.tapA = o > 0.5 ? pre : 1, H.tapB = o > 0.5 ? 1 : pre;
    }
    const double cap = static_cast<double>(loopL_.mask) - 2;
    H.tapA = std::min(H.tapA, cap), H.tapB = std::min(H.tapB, cap), H.loop = std::min(H.loop, cap);
    H.dif = std::min(H.dif, 32760.0);
    if (type_ != 0) H.tapA = std::min(H.tapA, static_cast<double>(preL_.mask) - 2), H.tapB = std::min(H.tapB, static_cast<double>(preL_.mask) - 2);
  }
  void timing() {
    if (!keepPitch_) {
      g_[0] = 1, g_[1] = 0, active_ = 0, fading_ = false;
      if (retime_) retime_ = false, retimeHead(0);
      auto mv = [&](double& c, double t) { c = std::fabs(t - c) < 0.001 ? t : c + (t - c) * glide_; };
      mv(head_[0].dif, tgt_[0].dif), mv(head_[0].tapA, tgt_[0].tapA), mv(head_[0].tapB, tgt_[0].tapB), mv(head_[0].loop, tgt_[0].loop);
    } else if (!fading_ && retime_) {
      retime_ = false;
      fading_ = true, xfN_ = 0;
      const int nw = 1 - active_;
      retimeHead(nw);
      head_[nw] = tgt_[nw];
    }
  }
  void fadeStep() {
    if (!keepPitch_ || !fading_) return;
    const float g = static_cast<float>(xfN_) * xfStep_;
    g_[active_] = 1 - g, g_[1 - active_] = g;
    if (++xfN_ >= xfLen_) {
      fading_ = false;
      active_ = 1 - active_;
      g_[active_] = 1, g_[1 - active_] = 0;
    }
  }

  // ---- per-sample blocks
  float allpass(int idx, float L, float x) {
    Ring& a = ap_[idx];
    const float g = 0.499f * apK_, d = a.read(L);
    a.write(x - g * d);
    return g * x + (1 - g * g) * d;
  }
  float limiter(int c, float x) {
    const float a = std::fabs(x);
    env_[c] += (a - env_[c]) * (a >= env_[c] ? 0.00520766f : 0.000522017f);
    return x / std::max(1.f, env_[c]);
  }
  float knee(float x) const { return kneeApply(knee_, x); }
  float dist(int c, float x) { return distType_ == 0 ? limiter(c, x / limLevel_) * limLevel_ : knee(x) * M_; }
  float crush(int c, float x) {
    float y = x;
    if (rr_ > 0) {
      const float yy = 127.f * rr_;
      const float F = std::max(1.f, (1 + 0.7435f * yy - 0.0171f * yy * yy + 0.0002f * yy * yy * yy) * rrScale_);
      cnt_[c] += 1;
      if (cnt_[c] >= F) {
        if (cnt_[c] >= 0 && F > 0) {
          const float q = cnt_[c] / F;
          cnt_[c] = (q - std::trunc(q)) * cnt_[c];
        }
        held_[c] = y;
      }
      y = held_[c];
    }
    if (bq_ > 0) y = std::trunc(y * scale_) / scale_;
    return y;
  }
  void filterUpdate(float lfo) {
    if ((ftick_++ & 3) != 0 || filter_ == 3) return;
    const float pos = std::clamp(lfo * modCut_ + cut_, 0.f, 1.f), r = std::clamp(res_, 0.f, 1.f) * 0.5f;
    if (filter_ == 2) svf_[0].design(2, lerpTab(kLpTab, pos * 3.6f), 0.107f + 4.6f * r * r, srf_);
    else svf_[0].design(filter_, lerpTab(filter_ == 0 ? kLpTab : kHpTab, pos * 3.999f), 0.707f + 4 * r * r, srf_);
    svf_[1].copyCoefs(svf_[0]);
  }
  void sample(float& L, float& R) {
    timing();
    const float lfo = 0.5f * (std::sin(6.2831855f * phase_) + 1.f);
    phase_ += lfoInc_;
    phase_ -= std::trunc(phase_);
    const bool filt = filter_ <= 2;
    float oL, oR;
    if (type_ == 3) {
      filterUpdate(lfo);
      oL = tone_[0].run(dist(0, crush(0, filt ? svf_[0].run(L) : L)));
      oR = tone_[1].run(dist(1, crush(1, filt ? svf_[1].run(R) : R)));
    } else {
      depth_ += (halfSr_ * modAmt_ * rateDepth_ - depth_) * depthCoef_;
      const float m = (lfo + 0.5f) * depth_;
      filterUpdate(lfo);
      if (type_ == 0) {
        const float in = 0.5f * (L + R) * inWet_;
        float a = 0, b = 0;
        for (int h = 0; h < 2; ++h) {
          a += g_[h] * loopL_.read(static_cast<float>(head_[h].tapA) + m);
          b += g_[h] * loopL_.read(static_cast<float>(head_[h].tapB) + m);
        }
        const float src = offN_ < 0.5f ? b : a;
        float x = dc_[0].run(src * fb_) + in;
        x = crush(0, x);
        x = dist(0, x);
        if (filt) x = svf_[0].run(x);
        float w = 0;
        for (int h = 0; h < 2; ++h) w += g_[h] * allpass(h, static_cast<float>(head_[h].dif), x);
        loopL_.write(w);
        oL = tone_[0].run(a), oR = tone_[1].run(b);
      } else {
        float yL = 0, yR = 0;
        for (int h = 0; h < 2; ++h) {
          yL += g_[h] * loopL_.read(static_cast<float>(head_[h].loop) + m);
          yR += g_[h] * loopR_.read(static_cast<float>(head_[h].loop) + m);
        }
        float xL, xR;
        if (type_ == 1) {
          preL_.write(L), preR_.write(R);
          float pL = 0, pR = 0;
          for (int h = 0; h < 2; ++h) pL += g_[h] * preL_.read(static_cast<float>(head_[h].tapA)), pR += g_[h] * preR_.read(static_cast<float>(head_[h].tapB));
          xL = dc_[0].run(yL * fb_) + pL * inWet_;
          xR = dc_[1].run(yR * fb_) + pR * inWet_;
        } else {
          const float x = 2 * offN_ - 1;
          float iL, iR;
          if (x < 0) iL = L * (1 + x / 2) + R * (-x / 2), iR = R * (1 + x);
          else iL = L * (1 - x), iR = R * (1 - x / 2) + L * (x / 2);
          xL = dc_[0].run(yL * fb_) + iL * inWet_;
          xR = dc_[1].run(yR * fb_) + iR * inWet_;
        }
        if (filt) xL = svf_[0].run(xL), xR = svf_[1].run(xR);
        xL = dist(0, crush(0, xL));
        xR = dist(1, crush(1, xR));
        float wl = 0, wr = 0;
        for (int h = 0; h < 2; ++h) wl += g_[h] * allpass(h, static_cast<float>(head_[h].dif), xL), wr += g_[h] * allpass(2 + h, static_cast<float>(head_[h].dif), xR);
        if (type_ == 1) loopL_.write(wl), loopR_.write(wr);
        else loopR_.write(wl), loopL_.write(wr);
        oL = tone_[0].run(yL), oR = tone_[1].run(yR);
      }
    }
    float wL = oL * sA_ + oR * sB_, wR = oL * sB_ + oR * sA_;
    if (type_ == 2 && distType_ == 1) wL = limiter(0, wL), wR = limiter(1, wR);
    L = L * dry_ + wL * wet_;
    R = R * dry_ + wR * wet_;
    fadeStep();
  }

  void fullReset() {
    loopL_.clear(), loopR_.clear(), preL_.clear(), preR_.clear();
    for (auto& a : ap_) a.clear();
    for (auto& s : svf_) s.s1 = s.s2 = 0;
    for (auto& d : dc_) d.x1 = d.y1 = 0;
    cnt_[0] = cnt_[1] = held_[0] = held_[1] = 0;
    for (int h = 0; h < 2; ++h) head_[h] = tgt_[h];
    if (sr_ > 0) retimeHead(0), retimeHead(1), head_[0] = tgt_[0], head_[1] = tgt_[1];
  }

  struct Head {
    double dif = 10, tapA = 10, tapB = 10, loop = 10;
  };
  double sr_ = 0, bpm_ = 120, glide_ = 1;
  float srf_ = 44100, halfSr_ = 22050, rrScale_ = 1, xfStep_ = 0.001f, depthCoef_ = 0, smoothC_ = 0;
  int xfLen_ = 1000, xfN_ = 0, active_ = 0;
  bool fading_ = false, primed_ = false, retime_ = true;
  uint64_t tick_ = 0, ftick_ = 0;
  float cur_[PCount] = {}, target_[PCount] = {};
  // applied state
  int type_ = 0, distType_ = 0, filter_ = 0;
  bool sync_ = false, keepPitch_ = false;
  float inWet_ = 0.5f, timeN_ = 0.5f, offN_ = 0.5f, sA_ = 1, sB_ = 0, lfoInc_ = 0, rateDepth_ = 1, modAmt_ = 0, modCut_ = 0, apK_ = 0;
  float difSpread_ = 0, fb_ = 0.99f, cut_ = 0.5f, res_ = 0, thrDb_ = 0, M_ = 1, limLevel_ = 1;
  Knee knee_;
  float rr_ = 0, bq_ = 0, scale_ = 65536, wet_ = 1, dry_ = 0.5f;
  float phase_ = 0, depth_ = 0;
  float env_[2] = {0, 0}, cnt_[2] = {0, 0}, held_[2] = {0, 0};
  float g_[2] = {1, 0};
  Head head_[2], tgt_[2];
  Ring loopL_, loopR_, preL_, preR_, ap_[4];
  Svf svf_[2];
  OnePole dc_[2], tone_[2];
};

}  // namespace gl::d3ref
