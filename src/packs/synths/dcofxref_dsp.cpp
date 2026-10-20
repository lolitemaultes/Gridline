// Gridline — "synths" pack, J-33: DCO Synth reference-model master effects
// (see dcofx_ref.h for the block descriptions and laws).
#include <algorithm>
#include <cmath>
#include "dcofx_ref.h"

namespace gl::packs::synths::dcofx {

namespace {

constexpr double kPi = 3.14159265358979323846;

// Per-line ranges: delay ms, rate Hz, depth %, move rate Hz (min, max each).
constexpr float kEns[8][8] = {
    {2.f, 6.f, 1.5f, 2.47f, 20.f, 80.f, 0.33f, 3.56f},     {3.5f, 8.25f, 1.57f, 3.41f, 20.f, 80.f, 0.29f, 2.66f},
    {6.5f, 10.f, 0.38f, 3.64f, 20.f, 80.f, 0.26f, 3.78f},  {5.f, 12.8f, 1.29f, 3.68f, 20.f, 80.f, 0.14f, 2.98f},
    {7.1f, 16.6f, 3.41f, 5.03f, 20.f, 80.f, 0.44f, 5.06f}, {9.1f, 15.1f, 2.47f, 5.37f, 20.f, 80.f, 0.66f, 8.08f},
    {8.3f, 16.8f, 3.01f, 6.02f, 20.f, 80.f, 0.93f, 6.02f}, {9.6f, 17.7f, 3.3f, 6.5f, 20.f, 80.f, 0.41f, 10.22f},
};
constexpr int kEnsLen = 65536;

// Parabolic sine, −1..1 for a phase in [0, 1).
double psin(double p) {
  const double u = 2.0 * p;
  const double a = std::min(u, 1.0), b = std::max(u, 1.0) - 1.0;
  return 4.0 * ((a - a * a) - (b - b * b));
}
double fracT(double x) { return x - std::trunc(x); }

void rbj(double sr, double hz, double q, bool high, double& b0, double& b1, double& b2, double& a1, double& a2) {
  const double w = 2.0 * kPi * std::clamp(hz, 5.0, 0.49 * sr) / sr;
  const double al = std::sin(w) / (2.0 * q), c = std::cos(w), a0 = 1.0 + al;
  if (high) b0 = (1.0 + c) / 2.0, b1 = -(1.0 + c);
  else b0 = (1.0 - c) / 2.0, b1 = 1.0 - c;
  b2 = b0;
  b0 /= a0, b1 /= a0, b2 /= a0;
  a1 = -2.0 * c / a0, a2 = (1.0 - al) / a0;
}

constexpr double kLpTab[5] = {270, 480, 1300, 7000, 22000};
constexpr double kHpTab[5] = {20, 90, 400, 1300, 4200};
double interp5(const double* t, double p) {
  p = std::clamp(p, 0.0, 3.9999);
  const int i = static_cast<int>(p);
  return t[i] + (t[i + 1] - t[i]) * (p - i);
}

// [7/6] Padé tanh, input clamped to ±5.
double pade(double u) {
  u = std::clamp(u, -5.0, 5.0);
  const double u2 = u * u, u4 = u2 * u2, u6 = u4 * u2;
  return u * (u6 + 378.0 * u4 + 17325.0 * u2 + 135135.0) / (28.0 * u6 + 3150.0 * u4 + 62370.0 * u2 + 135135.0);
}
double logCosh(double v) {
  const double a = std::fabs(v);
  return a + std::log1p(std::exp(-2.0 * a)) - 0.69314718055994531;
}

}  // namespace

// ------------------------------------------------------------------ ensemble
void RefEnsemble::configure(double sr) {
  sr_ = sr > 0 ? sr : 44100.0;
  for (auto& l : lines_) l.l.assign(kEnsLen, 0.f), l.r.assign(kEnsLen, 0.f);
  hpFadeLen_ = std::max(1, static_cast<int>(sr_ / 100.0));
  lastLp_ = lastHp_ = -1;
  clear();
}

void RefEnsemble::clear() {
  uint32_t seed = 0x1234567u;  // the reference starts its phases at random; fixed here
  auto rnd = [&]() {
    seed = seed * 1664525u + 1013904223u;
    return (seed >> 8) * (1.0 / 16777216.0);
  };
  for (auto& l : lines_) {
    std::fill(l.l.begin(), l.l.end(), 0.f);
    std::fill(l.r.begin(), l.r.end(), 0.f);
    l.yl = l.yr = 0;
    l.ph = rnd(), l.mph = rnd();
  }
  w_ = 0;
  for (int c = 0; c < 2; ++c) lp_.z1[c] = lp_.z2[c] = hp_.z1[c] = hp_.z2[c] = 0;
  hpFade_ = hpOn_ ? hpFadeLen_ : 0;
}

void RefEnsemble::set(double delay, double fb, double rate, double depth, double move, double moveRate, bool eight,
                      double lp, double hp, double mix, double phase) {
  for (int k = 0; k < 8; ++k) {
    const float* t = kEns[k];
    Line& l = lines_[static_cast<size_t>(k)];
    l.D = (t[0] + (t[1] - t[0]) * delay) * 0.001 * sr_;
    l.inc = (t[2] + (t[3] - t[2]) * rate) / sr_;
    const double dp = (t[4] + (t[5] - t[4]) * depth) * 0.01;
    l.dep = 0.5 * dp * dp;
    l.minc = (t[6] + (t[7] - t[6]) * moveRate) / sr_;
  }
  fb_ = 0.7 * fb;
  mv_ = 0.499 * move;
  off_ = 0.25 * phase * phase * phase;
  eight_ = eight;
  if (lp != lastLp_) {
    lastLp_ = lp;
    rbj(sr_, 714.0 * std::exp2(4.9454 * lp), 0.5, false, lp_.b0, lp_.b1, lp_.b2, lp_.a1, lp_.a2);
  }
  if (hp != lastHp_) {
    lastHp_ = hp;
    rbj(sr_, 12.0 * std::exp2(6.505 * hp), 0.5, true, hp_.b0, hp_.b1, hp_.b2, hp_.a1, hp_.a2);
  }
  hpOn_ = hp > 0;
  const double f = mix < 0.5 ? 2.0 * mix * mix : 1.0 - 2.0 * (1.0 - mix) * (1.0 - mix);
  dry_ = std::sqrt(std::max(0.0, 1.0 - f)), wet_ = std::sqrt(std::max(0.0, f));
}

void RefEnsemble::process(float* L, float* R, int n) {
  const int count = eight_ ? 8 : 4;
  const double lineGain = eight_ ? 0.281170487 : 0.562340975;
  auto herm = [&](const std::vector<float>& b, double d) {
    const int i = static_cast<int>(d);
    const double f = d - i;
    const int m = kEnsLen - 1;
    const double y0 = b[static_cast<size_t>((w_ - i) & m)], y1 = b[static_cast<size_t>((w_ - i - 1) & m)];
    const double y2 = b[static_cast<size_t>((w_ - i - 2) & m)], y3 = b[static_cast<size_t>((w_ - i - 3) & m)];
    const double c1 = (y2 - y0) * 0.5, c2 = y0 - 2.5 * y1 + 2.0 * y2 - 0.5 * y3, c3 = 1.5 * (y1 - y2) + 0.5 * (y3 - y0);
    return ((c3 * f + c2) * f + c1) * f + y1;
  };
  for (int s = 0; s < n; ++s) {
    const double xl = L[s], xr = R[s];
    double wl = 0, wr = 0;
    for (int k = 0; k < count; ++k) {
      Line& l = lines_[static_cast<size_t>(k)];
      const double pan = 0.5 + mv_ * psin(l.mph);
      l.l[static_cast<size_t>(w_)] = static_cast<float>(1.41442716 * std::sqrt(1.0 - pan) * xl + fb_ * l.yl);
      l.r[static_cast<size_t>(w_)] = static_cast<float>(1.41442716 * std::sqrt(pan) * xr + fb_ * l.yr);
      const double dL = std::clamp(l.D + l.dep * l.D * psin(l.ph), 0.0, kEnsLen - 8.0);
      const double dR = std::clamp(l.D + l.dep * l.D * psin(fracT(l.ph + off_)), 0.0, kEnsLen - 8.0);
      l.yl = static_cast<float>(herm(l.l, dL));
      l.yr = static_cast<float>(herm(l.r, dR));
      wl += l.yl, wr += l.yr;
      l.ph = fracT(l.ph + l.inc);
      l.mph = fracT(l.mph + l.minc);
    }
    w_ = (w_ + 1) & (kEnsLen - 1);
    wl = lp_.run(wl * lineGain, 0), wr = lp_.run(wr * lineGain, 1);
    const double hl = hp_.run(wl, 0), hr = hp_.run(wr, 1);
    if (hpOn_ && hpFade_ < hpFadeLen_) ++hpFade_;
    else if (!hpOn_ && hpFade_ > 0) --hpFade_;
    const double k = static_cast<double>(hpFade_) / hpFadeLen_;
    wl = (1.0 - k) * wl + k * hl, wr = (1.0 - k) * wr + k * hr;
    L[s] = static_cast<float>(dry_ * xl + wet_ * wl);
    R[s] = static_cast<float>(dry_ * xr + wet_ * wr);
  }
}

// --------------------------------------------------------------------- delay
void RefDelay::configure(double sr) {
  sr_ = sr > 0 ? sr : 44100.0;
  main_.assign(kLen, 0.f);
  for (auto& b : blur_) b.assign(kLen, 0.f);
  hpG_ = std::tan(kPi * 10.0 / 44100.0);  // designed for 44.1 kHz whatever the rate
  lastCut_ = -1, lastType_ = -1, lastTone_ = -1, bpReady_ = false;
  bb0_ = bb1_ = bb2_ = ba1_ = ba2_ = 0;
  clear();
}

void RefDelay::clear() {
  std::fill(main_.begin(), main_.end(), 0.f);
  for (auto& b : blur_) std::fill(b.begin(), b.end(), 0.f);
  pre_.fill(0.f);
  pi_ = w_ = 0;
  hpS_ = env_ = low_ = band_ = bz1_ = bz2_ = 0;
  toneS_[0] = toneS_[1] = 0;
  first_ = true, fade_ = 0, poll_ = 0, active_ = 0;
}

void RefDelay::set(double input, double seconds, double rightTap, double fb, double fbCut, int fbType, double stereo,
                   double blur, double tone) {
  in_ = input, T_ = seconds, r_ = rightTap, fb_ = fb, cut_ = std::clamp(fbCut, 0.0, 1.0);
  type_ = std::clamp(fbType, 0, 3);
  s1_ = 0.5 + 0.5 * stereo, s2_ = 0.5 - 0.5 * stereo;
  bl_ = blur, tone_ = tone;
  updateFilters();
}

void RefDelay::updateFilters() {
  if (cut_ != lastCut_ || type_ != lastType_) {
    lastCut_ = cut_, lastType_ = type_;
    if (type_ <= 1) {
      const double f = interp5(type_ == 0 ? kLpTab : kHpTab, 3.999 * cut_);
      svfF_ = 2.0 * std::sin(kPi * std::min(0.25, f / (2.0 * sr_)));
      svfQ_ = std::min(2.0, 2.0 / svfF_ - svfF_ / 2.0);
    } else if (type_ == 2) {
      const double f = std::min(interp5(kLpTab, 3.999 * 0.82 * cut_), 0.49 * sr_);
      const double w = 2.0 * kPi * f / sr_, al = std::sin(w) / (2.0 * 0.04), a0 = 1.0 + al;
      bb0_ = al / a0, bb1_ = 0, bb2_ = -al / a0, ba1_ = -2.0 * std::cos(w) / a0, ba2_ = (1.0 - al) / a0;
      bpReady_ = true;
    }
  }
  if (tone_ != lastTone_) {
    lastTone_ = tone_;
    toneHp_ = tone_ >= 0.5;
    const double f = toneHp_ ? interp5(kHpTab, (2.0 * tone_ - 1.0) * 3.9999) : interp5(kLpTab, 2.0 * tone_ * 3.9999);
    toneG_ = std::tan(kPi * std::min(f, 0.49 * sr_) / sr_);
  }
}

float RefDelay::rd(const std::vector<float>& b, double d) const {
  d = std::clamp(d, 1.0, kLen - 4.0);
  const int i = static_cast<int>(d);
  const double f = d - i;
  const float a = b[static_cast<size_t>((w_ - i) & (kLen - 1))], c = b[static_cast<size_t>((w_ - i - 1) & (kLen - 1))];
  return static_cast<float>(a + (c - a) * f);
}

void RefDelay::process(float* L, float* R, int n) {
  const int pollLen = std::max(1, static_cast<int>(sr_ / 500.0));
  for (int s = 0; s < n; ++s) {
    // Time changes: load the idle head and crossfade over 1000 samples.
    if (--poll_ <= 0) {
      poll_ = pollLen;
      auto load = [&](int h) {
        hT_[h] = T_, hR_[h] = r_;
        dl_[h] = T_ * sr_ + 1.0;
        dr_[h] = (1.0 - r_ / 2.0) * (T_ * sr_ + 0.0001) + 1.0;
      };
      if (first_) {
        load(0), load(1);
        g_[0] = 1, g_[1] = 0, active_ = 0, first_ = false;
      } else if (fade_ == 0 && (T_ != hT_[active_] || r_ != hR_[active_])) {
        load(1 - active_);
        fade_ = 1000;
      }
    }
    if (fade_ > 0) {
      --fade_;
      const double k = (1000 - fade_) / 1000.0;
      g_[1 - active_] = k, g_[active_] = 1.0 - k;
      if (fade_ == 0) {
        active_ = 1 - active_;
        g_[active_] = 1, g_[1 - active_] = 0;
      }
    }
    const double xl = L[s], xr = R[s];
    const double m = 0.5 * (xl + xr) * in_;
    double wl = rd(main_, dl_[0]) * g_[0] + rd(main_, dl_[1]) * g_[1];
    double wr = rd(main_, dr_[0]) * g_[0] + rd(main_, dr_[1]) * g_[1];
    // Feedback path.
    const double hx = fb_ / 1.00296 * wl;
    const double v = (hx - hpS_) * hpG_ / (1.0 + hpG_), lp = v + hpS_;
    hpS_ = lp + v;
    double f = (hx - lp) + m;
    if (type_ <= 1) {
      double acc = 0;
      for (int k = 0; k < 2; ++k) {
        low_ += svfF_ * band_;
        const double high = f - low_ - svfQ_ * band_;
        band_ += svfF_ * high;
        acc += type_ == 0 ? low_ : high;
      }
      f = acc * 0.5;
    } else if (type_ == 2 && bpReady_) {  // type 3 ("off"): no filter
      const double y = bb0_ * f + bz1_;
      bz1_ = bb1_ * f - ba1_ * y + bz2_;
      bz2_ = bb2_ * f - ba2_ * y;
      f = y;
    }
    const double u = 0.1 * f;
    const double c = u < -3 ? -1.0 : (u > 3 ? 1.0 : u * (u * u + 27.0) / (9.0 * u * u + 27.0));
    f = 10.0296 * c;
    const double af = std::fabs(f);
    env_ += (af - env_) * (af >= env_ ? 0.0052077 : 0.000521);
    f /= std::max(1.0, env_);
    const float delayed = pre_[static_cast<size_t>(pi_)];
    pre_[static_cast<size_t>(pi_)] = static_cast<float>(f);
    pi_ = (pi_ + 1) % 65;
    // Blur.
    wl += bl_ * (rd(blur_[0], 0.63 * dl_[0]) * g_[0] + rd(blur_[0], 0.63 * dl_[1]) * g_[1]);
    wr += bl_ * (rd(blur_[1], 0.76 * dl_[0]) * g_[0] + rd(blur_[1], 0.76 * dl_[1]) * g_[1]);
    blur_[0][static_cast<size_t>(w_)] = static_cast<float>(-wl * fb_);
    blur_[1][static_cast<size_t>(w_)] = static_cast<float>(-wr * fb_);
    const double b2 = bl_ * bl_;
    wl += b2 * (rd(blur_[2], 0.41 * dl_[0]) * g_[0] + rd(blur_[2], 0.41 * dl_[1]) * g_[1]);
    wr += b2 * (rd(blur_[3], 0.54 * dl_[0]) * g_[0] + rd(blur_[3], 0.54 * dl_[1]) * g_[1]);
    blur_[2][static_cast<size_t>(w_)] = static_cast<float>(-wl * fb_);
    blur_[3][static_cast<size_t>(w_)] = static_cast<float>(-wr * fb_);
    main_[static_cast<size_t>(w_)] = delayed;
    w_ = (w_ + 1) & (kLen - 1);
    // Tone.
    double t[2] = {wl, wr};
    for (int ch = 0; ch < 2; ++ch) {
      const double vv = (t[ch] - toneS_[ch]) * toneG_ / (1.0 + toneG_), y = vv + toneS_[ch];
      toneS_[ch] = y + vv;
      t[ch] = toneHp_ ? t[ch] - y : y;
    }
    L[s] = static_cast<float>(xl + s1_ * t[0] + s2_ * t[1]);
    R[s] = static_cast<float>(xr + s2_ * t[0] + s1_ * t[1]);
  }
}

// ---------------------------------------------------------------- saturation
void RefSaturation::configure(double sr) {
  k_ = 1.0 - std::pow(0.1, 1.0 / (0.05 * (sr > 0 ? sr : 44100.0)));
  clear();
}
void RefSaturation::clear() {
  prev_[0] = prev_[1] = 0;
  started_ = false;
}
void RefSaturation::set(double amount, bool hq) {
  target_ = std::tan(1.55 * std::cbrt(std::clamp(amount, 0.0, 1.0)));
  hq_ = hq;
  if (!started_) g_ = target_, started_ = true;
}
void RefSaturation::process(float* L, float* R, int n) {
  float* io[2] = {L, R};
  for (int s = 0; s < n; ++s) {
    g_ += (target_ - g_) * k_;
    if (g_ < 1e-4) continue;  // the limit (x) — the reference would divide 0 by 0 here
    const double norm = 1.0 / (3.0 * pade(g_));
    for (int c = 0; c < 2; ++c) {
      const double v = 3.0 * g_ * io[c][s];
      double sh;
      if (hq_) {
        const double d = v - prev_[c];
        sh = std::fabs(d) < 0.001 ? std::tanh(0.5 * (v + prev_[c])) : (logCosh(v) - logCosh(prev_[c])) / d;
        prev_[c] = v;
      } else {
        sh = pade(v);
      }
      io[c][s] = static_cast<float>(sh * norm);
    }
  }
}

}  // namespace gl::packs::synths::dcofx
