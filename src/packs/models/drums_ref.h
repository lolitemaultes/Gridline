// Gridline — "models" pack, J-33: Membrane Pad reference engine (PdEngine = 1),
// the reference drum instrument's model as analysed (behaviour only):
//   internal rate 38 kHz, resampled linearly to the host rate (pitch knob
//   ±1 octave as tape speed; the key does not transpose);
//   mallet: decaying DC step mixed with LCG noise (seedable for repeatable
//   noise), driving a bank of 512 comb lines (delays 512 … 1 samples, one-pole
//   damped, negative feedback), of which a window chosen by size / shape is
//   heard with triangle-power gains from tension / phase / material;
//   self-oscillating 4-stage ladder (pitch glide), LOW high pass, HIGH shelf,
//   MID peak with a rising modulation envelope (+ repeats, recomputed every 6
//   samples), hard clip, hold gate, lo-fi.
// Velocity: v = vel^0.5; a route's value is reached at full velocity.
// One-shot: note off only frees the voice's note slot; a reused voice keeps
// ringing (its membrane is not cleared). Audio thread: no allocation after
// allocate().
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>
#include "drums.h"

namespace gl::packs::models::dr {

class RefMembraneVoice {
 public:
  static constexpr int kLines = 512;
  static constexpr double kRate = 38000.0;
  bool on = false;    // rendering (ringing)
  bool held = false;  // note slot taken
  uint32_t id = 0;
  uint64_t age = 0;
  bool released = false;

  void allocate(uint32_t seed) {
    buf_.assign(static_cast<size_t>(kLines) * (kLines + 1) / 2, 0.f);
    size_t at = 0;
    for (int j = 0; j < kLines; ++j) off_[j] = at, at += static_cast<size_t>(kLines - j);
    lcg_ = seed;
    reset();
  }

  // Applies the velocity routes (v = velocity^0.5) to the model values.
  static void routes(const float* in, float* out, float velocity) {
    const double v = std::sqrt(std::clamp(static_cast<double>(velocity), 0.0, 1.0));
    std::copy(in, in + MdCount, out);
    for (int t = MdMalletAmp; t <= MdPitch; ++t) {
      double val = in[t];
      for (int r = 0; r < 4; ++r) {
        const int s = static_cast<int>(std::trunc(in[MdVelSrc1 + r]));
        if (s != 0 && s + 1 == t) val = in[MdVelAmt1 + r];
      }
      out[t] = static_cast<float>(in[t] + (val - in[t]) * v);
    }
  }

  // Note on with the routed model values `x`; `semis`: pitch deviation (fine / slides).
  void strike(const float* x, double semis, double hostRate) {
    on = held = true;
    released = false;
    step_ = kRate / hostRate * std::exp2(2.0 * x[MdPitch] - 1.0) * std::exp2(std::clamp(semis, -12.0, 12.0) / 12.0);
    // Mallet.
    env_ = std::pow(10.0, 4.0 * x[MdMalletAmp] - 4.0);
    if (x[MdMalletRetrig] != 0.f) lcg_ = static_cast<uint32_t>(std::trunc(static_cast<double>(x[MdMalletNoiseRp]) * 4294967295.0));
    const double md = 1.0 - x[MdMalletDecay];
    dm_ = md * md * md * md * 0.05 + 0.0001;
    noise_ = x[MdMalletNoise];
    // Membrane.
    fb_ = -std::pow(static_cast<double>(x[MdMemDecay]), 0.3);
    clp_ = static_cast<double>(x[MdMemCutoff]) * x[MdMemCutoff];
    start_ = std::min(511, static_cast<int>(std::trunc(std::clamp(1.0 - x[MdSize], 0.0, 1.0) * 512.0)));
    const int count = std::clamp(static_cast<int>(std::trunc(128.0 * x[MdShape])), 1, 128);
    end_ = std::min(511, start_ + count);
    g0_ = std::min(127, start_ >> 2);
    gN_ = (end_ + 3) / 4 - g0_;
    {
      const double inc = 0.5 * std::pow(static_cast<double>(x[MdTension]), 4.0);
      double ph = x[MdPhase];
      const double m = 2.0 * x[MdMaterial] - 1.0, m4 = m * m * m * m;
      const double e = m >= 0 ? 1.0 + 20.0 * m4 : 1.0 / (1.0 + 20.0 * m4);
      for (int j = kLines - 1; j >= 0; --j) {
        double t = 2.0 * ph - 1.0;
        if (t > 0.5) t = 1.0 - t;
        if (t < -0.5) t = -1.0 - t;
        t *= 2.0;
        gain_[j] = (j >= start_ && j <= end_) ? static_cast<float>((t < 0 ? -1.0 : 1.0) * std::pow(std::fabs(t), e)) : 0.f;
        ph += inc;
        if (ph > 1.0) ph -= 1.0;
      }
    }
    // SOF.
    sofOn_ = x[MdSofLevel] != 0.f;
    sg0_ = std::min(1.0, std::exp2(4.65 * x[MdSofFreq] + 5.93) * 6.0 / 38000.0);
    kappa_ = 333.333 * sg0_;
    envS_ = 1.0;
    const double sd = x[MdSofDecay] / 2.0;
    dS_ = sd * sd * sd * sd * 0.01 + 1e-5;
    lambda_ = 0.5 * std::pow(static_cast<double>(x[MdSofLevel]), 8.0);
    // LOW.
    lowOn_ = x[MdLowFreq] != 0.f;
    if (lowOn_) {
      const double f = std::exp2(11.0 * x[MdLowFreq] + 2.0), Q = 1.0 + 8.0 * x[MdLowReso];
      const double w = 2.0 * 3.14159265358979323846 * f / kRate, s = std::sin(w), c = std::cos(w);
      lb_ = (1.0 + c) / (2.0 + s / Q), la1_ = -2.0 * Q * c / (Q + s), la2_ = (Q - s) / (Q + s);
    }
    // HIGH.
    const double hg = 36.0 * x[MdHighLevel] - 18.0;
    highOn_ = std::fabs(hg) > 0.01;
    if (highOn_) shelf(std::exp2(4.1 * x[MdHighFreq] + 8.0), hg, 2.0 * (1.0 - x[MdHighRes]) + 0.5);
    // MID.
    midOn_ = x[MdMidLevel] != 0.5f || x[MdMidLevelMod] != 0.5f;
    mv_ = 1.0;
    const double ma = 1.0 - x[MdMidAttack];
    mr_ = ma * ma * ma * ma * 0.01 + 1e-5;
    repeats_ = std::clamp(static_cast<int>(std::trunc(3.0 * x[MdRetrig])), 0, 3);
    mcount_ = 0;
    mG0_ = 32.0 * x[MdMidLevel] - 16.0, mGm_ = 48.0 * x[MdMidLevelMod] - 24.0;
    mFm_ = 32.0 * x[MdMidFreqMod] - 16.0, mF_ = std::exp2(9.0 * x[MdMidFreq] + 5.0);
    mBw_ = 4.0 * (1.0 - x[MdMidRes]) + 0.5;
    // Output.
    lofiQ_ = x[MdLofi] >= 0.01f ? std::trunc(std::pow(1.0 - x[MdLofi], 5.0) * 65536.0 + 4.0) : 0.0;
    holdT_ = 3.0 * std::pow(static_cast<double>(x[MdHold]), 3.0) + 0.001;
    t_ = 0;
  }

  // One host-rate output sample (mono).
  float next() {
    const float y = static_cast<float>(a_ + frac_ * (b_ - a_));
    frac_ += step_;
    while (frac_ >= 1.0 && on) {
      frac_ -= 1.0;
      a_ = b_;
      b_ = tick();
    }
    if (!on) frac_ = 0;
    return y;
  }

 private:
  void reset() {
    std::fill(buf_.begin(), buf_.end(), 0.f);
    std::fill(z_, z_ + kLines, 0.0);
    std::fill(idx_, idx_ + kLines, 0);
    s1_ = s2_ = s3_ = s4_ = 0;
    lx1_ = lx2_ = ly1_ = ly2_ = hx1_ = hx2_ = hy1_ = hy2_ = mx1_ = mx2_ = my1_ = my2_ = 0;
    gate_ = 1.0;
    sounded_ = false;
    quiet_ = 0;
    a_ = b_ = 0;
    frac_ = 0;
    on = false;
  }

  // Cookbook coefficients with A = 10^(G/20) and bandwidth in octaves.
  void coef(double f, double g, double bw, bool peak, double* b, double* a) {
    const double w0 = std::clamp(2.0 * 3.14159265358979323846 * f / kRate, 0.0, 3.0);
    const double sw = std::sin(w0), cw = std::cos(w0);
    const double al = sw * std::sinh(0.34657359027997264 * bw * (sw != 0 ? w0 / sw : 1.0));
    const double A = std::pow(10.0, g / 20.0);
    double b0, b1, b2, a0, a1, a2;
    if (peak) {
      b0 = 1 + al * A, b1 = -2 * cw, b2 = 1 - al * A, a0 = 1 + al / A, a1 = -2 * cw, a2 = 1 - al / A;
    } else {
      const double sa = 2.0 * std::sqrt(A) * al;
      b0 = A * ((A + 1) + (A - 1) * cw + sa), b1 = -2 * A * ((A - 1) + (A + 1) * cw), b2 = A * ((A + 1) + (A - 1) * cw - sa);
      a0 = (A + 1) - (A - 1) * cw + sa, a1 = 2 * ((A - 1) - (A + 1) * cw), a2 = (A + 1) - (A - 1) * cw - sa;
    }
    auto z = [](double v) { return std::fabs(v) < 1e-9 ? 0.0 : v; };
    b[0] = z(b0 / a0), b[1] = z(b1 / a0), b[2] = z(b2 / a0), a[0] = z(a1 / a0), a[1] = z(a2 / a0);
  }
  void shelf(double f, double g, double bw) { coef(f, g, bw, false, hb_, ha_); }

  double tick() {
    // Mallet.
    const double u = lcg_ * 2.3283064365386963e-10;
    lcg_ = lcg_ * 0x0BB38435u + 0x3619636Bu;
    env_ += (1e-7 - env_) * dm_;
    const double exc = env_ * (noise_ * (u - 0.5) + (1.0 - noise_));
    // Membrane.
    double m = 0;
    const int j0 = 4 * g0_, j1 = std::min(kLines, 4 * (g0_ + std::max(0, gN_)));
    for (int j = j0; j < j1; ++j) {
      float* b = buf_.data() + off_[j];
      int& i = idx_[j];
      const double y = b[i];
      m += gain_[j] * y;
      z_[j] += (y - z_[j]) * clp_;
      b[i] = static_cast<float>(z_[j] * fb_ + exc);
      if (++i >= kLines - j) i = 0;
    }
    // SOF.
    double y = m;
    if (sofOn_) {
      const double g = sg0_ / (1.0 + kappa_ - kappa_ * envS_);
      double v = m - 100000.0 * s4_;
      v = v / (1.0 + 0.005 * std::fabs(v));
      const double old4 = s4_;
      s1_ += (v - s1_) * g, s2_ += (s1_ - s2_) * g, s3_ += (s2_ - s3_) * g, s4_ += (s3_ - s4_) * g;
      y = m + lambda_ * old4;
      envS_ -= envS_ * dS_;
    }
    if (lowOn_) {
      const double o = lb_ * (y - 2.0 * lx1_ + lx2_) - la1_ * ly1_ - la2_ * ly2_;
      lx2_ = lx1_, lx1_ = y, ly2_ = ly1_, ly1_ = o, y = o;
    }
    if (highOn_) {
      const double o = hb_[0] * y + hb_[1] * hx1_ + hb_[2] * hx2_ - ha_[0] * hy1_ - ha_[1] * hy2_;
      hx2_ = hx1_, hx1_ = y, hy2_ = hy1_, hy1_ = o, y = o;
    }
    if (midOn_) {
      if (mcount_ == 0) {
        const double mm = 1.0 - mv_;
        const double G = mG0_ + mGm_ * mm;
        double f = mFm_ >= 0 ? mF_ * (1.0 + mFm_ * mm) : mF_ / (1.0 - mFm_ * mm);
        f = std::clamp(f, 32.0, 12000.0);
        coef(f, G, mBw_, true, mb_, ma_);
      }
      mcount_ = (mcount_ + 1) % 6;
      mv_ -= mv_ * mr_;
      if (repeats_ > 0 && mv_ < 0.01) mv_ = 1.0, --repeats_;
      const double o = mb_[0] * y + mb_[1] * mx1_ + mb_[2] * mx2_ - ma_[0] * my1_ - ma_[1] * my2_;
      mx2_ = mx1_, mx1_ = y, my2_ = my1_, my1_ = o, y = o;
    }
    y = std::clamp(y, -1.0, 1.0);
    gate_ = t_ < holdT_ ? gate_ + (1.0 - gate_) * 0.1 : 0.999 * gate_;
    t_ += 1.0 / kRate;
    y *= gate_;
    if (lofiQ_ > 0) y = std::trunc(y * lofiQ_) / lofiQ_;
    // Voice end.
    if (std::fabs(y) > 1e-4) sounded_ = true, quiet_ = 0;
    else ++quiet_;
    if ((sounded_ && quiet_ > 0.025 * kRate) || quiet_ > 0.5 * kRate || !std::isfinite(y)) {
      reset();
      return 0.0;
    }
    return y;
  }

  std::vector<float> buf_;
  size_t off_[kLines] = {};
  int idx_[kLines] = {};
  double z_[kLines] = {};
  float gain_[kLines] = {};
  int start_ = 0, end_ = 0, g0_ = 0, gN_ = 0;
  uint32_t lcg_ = 1;
  double env_ = 0, dm_ = 0, noise_ = 0, fb_ = 0, clp_ = 0;
  bool sofOn_ = false, lowOn_ = false, highOn_ = false, midOn_ = false;
  double sg0_ = 0, kappa_ = 0, envS_ = 1, dS_ = 0, lambda_ = 0, s1_ = 0, s2_ = 0, s3_ = 0, s4_ = 0;
  double lb_ = 0, la1_ = 0, la2_ = 0, lx1_ = 0, lx2_ = 0, ly1_ = 0, ly2_ = 0;
  double hb_[3] = {1, 0, 0}, ha_[2] = {0, 0}, hx1_ = 0, hx2_ = 0, hy1_ = 0, hy2_ = 0;
  double mb_[3] = {1, 0, 0}, ma_[2] = {0, 0}, mx1_ = 0, mx2_ = 0, my1_ = 0, my2_ = 0;
  double mv_ = 1, mr_ = 0, mG0_ = 0, mGm_ = 0, mFm_ = 0, mF_ = 1000, mBw_ = 1;
  int mcount_ = 0, repeats_ = 0;
  double lofiQ_ = 0, holdT_ = 0, t_ = 0, gate_ = 1;
  bool sounded_ = false;
  int64_t quiet_ = 0;
  double a_ = 0, b_ = 0, frac_ = 0, step_ = 1;
};

}  // namespace gl::packs::models::dr
