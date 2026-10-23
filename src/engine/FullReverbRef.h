// Gridline — J-35: "full" reference reverb (behaviour study).
//
// A figure-of-eight plate: input high cut / low cut → four input all-passes →
// pre-delay → two cross-coupled tank halves (bass shelf on a crossover
// all-pass, damping one-pole, all-pass, delay, loop gain, all-pass, delay,
// loop gain) → seven weighted output taps per side, plus 7 / 8 early
// reflection taps on their own line. Line lengths follow the room size.
//
// Two variants share the network:
//  * Multi    — the multi-effect / pedal engine: 18 normalised controls with
//               integer laws (levels in 0.1 dB), stereo input averaged, the
//               second and fourth input all-passes feed forward with their own
//               coefficient, early reflections tapped after the second
//               all-pass, output added to the caller's buffers with a width
//               matrix.
//  * Rack     — (J-42) the mobile module rack's reverb: the Multi network at the
//               host rate with a modulated delay in each tank half, reflections
//               from the first tap per side only, and two outputs (late + weighted
//               reflections through the width matrix; raw reflections), no dry.
//  * Studio   — the standalone reverb: mono input, all input all-passes on one
//               coefficient, reflections tapped after the first all-pass, a
//               modulated delay in front of each tank's first all-pass; the
//               caller mixes wet / reflection outputs.
// Arithmetic is float, grouped as the reference; buffers are sized in init()
// (never in process).
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace gl::fullrev {

enum class Variant { Multi, Studio, Rack };

// ---------------------------------------------------------------- tables
inline constexpr int kInConst[4] = {142, 107, 379, 277};
inline constexpr int kLeftConst[4] = {672, 4453, 1800, 3720};   // AP_L1, D_L1, AP_L2, D_L2
inline constexpr int kRightConst[4] = {908, 4217, 2656, 3163};  // AP_R1, D_R1, AP_R2, D_R2
inline constexpr int kTapA[7] = {266, 2974, 1913, 2496, 1690, 187, 1566};
inline constexpr int kTapB[7] = {353, 3627, 1728, 3173, 1811, 335, 621};
inline constexpr int kErL[7] = {1102, 2904, 3784, 4873, 5878, 7470, 9062};
inline constexpr int kErR[8] = {837, 1423, 2865, 3938, 5278, 5978, 7532, 9263};

// Room scale S for room 1..100 at integer rate sri.
inline int64_t roomScale(int room, int sri) { return (static_cast<int64_t>(room) + 15) * sri / 115; }
inline int lineLen(int c, int64_t S) {
  const int64_t n = static_cast<int64_t>(c) * S / 20000;
  return n <= 0 ? 1 : static_cast<int>(n);
}

// ---------------------------------------------------------------- multi-variant control laws
// Normalised control n → engine integer (floor(a·n + b) in float).
inline int lawInt(float n, float a, float b) { return static_cast<int>(std::floor(a * n + b)); }
inline int levelDb10(float n) { return lawInt(n, 421.f, -360.5f); }  // 0.1 dB; < -360 = -inf
inline int lowCutHz(float n) { return lawInt(n, 2981.f, 19.5f); }    // < 20 = off
inline int cut100(float n) { return lawInt(n, 216.f, 5.5f); }        // ×100 Hz; > 220 = off
inline int predelayMs(float n) { return lawInt(n, 250.f, 0.5f); }
inline int roomOf(float n) { return lawInt(n, 99.f, 1.5f); }
inline int diffusionPct(float n) { return lawInt(n, 100.f, 0.5f); }
inline int bass10(float n) { return lawInt(n, 28.f, 2.5f); }
inline int crossHz(float n) { return lawInt(n, 1975.f, 25.5f); }
inline int decay10(float n) { return lawInt(n, 199.f, 1.5f); }
inline int widthPct(float n) { return lawInt(n, 200.f, 0.5f); }

// The 18 engine controls (normalised) and the default program.
enum MultiParam {
  MpInput = 0, MpLowCut, MpHighCut, MpPredelay, MpRoom, MpDiffusion, MpBass, MpCross, MpDecay, MpDamp,
  MpDry, MpEr, MpWet, MpWidth, MpMuteIn, MpMuteDry, MpMuteEr, MpMuteWet, MpCount
};
inline constexpr float kMultiDefaults[MpCount] = {0.8574822f, 0.01878564f, 0.16203703f, 0, 0.4949495f, 1, 0.2857143f, 0.24050634f,
                                                  0.07035176f, 0.16203703f, 0.8574822f, 0.7743468f, 0.5724466f, 0.5f, 0, 0, 0, 0};

// Studio-variant settings (engine units).
struct StudioSettings {
  int lowCutHz = 75;      // 0 = off
  int highCut = 40;       // ×100 Hz, 0 = off
  int predelay = 0;       // samples
  int room = 50;          // 1..100
  int diffusion = 100;    // %
  float decaySamples = 0;  // T60 in samples
  int damp = 40;          // ×100 Hz, 0 = off
  float bass = 1;         // multiplier
  float cross = 500;      // Hz
  float modSpeed = 0.5f, modDepth = 0.5f;  // 0..1
};

class Engine {
 public:
  // Allocates every line for the integer rate `sri` (room-100 capacities).
  // predelayCap: pre-delay capacity in samples.
  void init(Variant v, int sri, int predelayCap) {
    v_ = v, sri_ = sri, sr_ = static_cast<float>(sri);
    const int64_t S = sri;
    for (int k = 0; k < 4; ++k) in_[k].alloc(lineLen(kInConst[k], S));
    for (int k = 0; k < 4; ++k) l_[k].alloc(lineLen(kLeftConst[k], S)), r_[k].alloc(lineLen(kRightConst[k], S));
    pd_.alloc(std::max(1, predelayCap));
    const double x = sri * 0.31632653;
    const int64_t xi = v != Variant::Studio ? static_cast<int64_t>(x) : static_cast<int64_t>(std::lround(static_cast<float>(sri) * 0.31632653f));
    int bits = 0;
    while ((int64_t{1} << bits) <= xi) ++bits;
    er_.assign(static_cast<size_t>(1) << (bits + 1), 0.f);
    erMask_ = static_cast<uint32_t>(er_.size() - 1);
    for (auto& m : mod_) m.assign(4096, 0.f);
    lastRoom_ = 0;
    dirty_ = true;
    resume();
  }
  bool ready() const { return sri_ > 0; }
  int rate() const { return sri_; }

  // Back to silence: Multi fills the lines with 1e-30 (the reference's resume);
  // Studio zeroes them. Filter states zeroed, the wet smoother snaps again.
  void resume() {
    const float fill = v_ != Variant::Studio ? 1e-30f : 0.f;
    for (auto* L : lines()) L->fill(fill);
    pd_.fill(fill);
    std::fill(er_.begin(), er_.end(), fill);
    erPos_ = 0;
    lp_ = apA_ = apX_ = 0;
    xsL_ = xpL_ = dL_ = xsR_ = xpR_ = dR_ = 0;
    s_ = -1000;
    for (auto& m : mod_) std::fill(m.begin(), m.end(), 0.f);
    modW_[0] = modW_[1] = 0;
    rd_ = 0;
  }

  // ---- Rack variant (J-42): engine integers in the rack's ranges.
  struct RackSettings {
    int lowCut = 75, highCut = 40, predelayMs = 0, room = 50, diffusion = 100, bass = 10, cross = 500, decay = 15, damp = 40;
    float modDepth = 0, modSpeed = 0;
    bool operator!=(const RackSettings& o) const {
      return lowCut != o.lowCut || highCut != o.highCut || predelayMs != o.predelayMs || room != o.room || diffusion != o.diffusion || bass != o.bass ||
             cross != o.cross || decay != o.decay || damp != o.damp;
    }
  };
  void setRack(const RackSettings& r) {
    if (r != rk_ || rkFirst_) rk_ = r, rkFirst_ = false, dirty_ = true;
    rk_.modDepth = r.modDepth, rk_.modSpeed = r.modSpeed;
    if (dirty_) recomputeRack();
    rfs_ = 0.1f + 9.9f * (rk_.modSpeed * rk_.modSpeed * rk_.modSpeed);
    rdT_ = rk_.modDepth * 0.0045351475f * static_cast<float>(sri_);
  }
  // One stereo input sample → A (late + weighted reflections, width matrix) and B (raw reflections).
  void tickRack(float inL, float inR, float& AL, float& AR, float& BL, float& BR) {
    const float lfo = std::sin(6.2831855f * rphi_);
    rphi_ += rfs_ / static_cast<float>(sri_);
    rphi_ -= std::trunc(rphi_);
    rd_ = std::fabs(rdT_ - rd_) < 0.01f ? rdT_ : (rdT_ - rd_) * rcDepth_ + rd_ + 1e-30f;
    const float iL = inL * inGain_, iR = inR * inGain_;
    const float mono = ((iL + iR) * 0.5f) + 1e-30f;
    lp_ = ((mono - lp_) * cHC_) + lp_;
    const float a = ((lp_ - apA_) * cLC_) + apX_;
    apA_ = a, apX_ = lp_;
    const float x = cLC_ != 0 ? (((lp_ - a) * 0.5f) + 1e-30f) : lp_;
    const float EL = gL_[0] * er_[(erPos_ + static_cast<uint32_t>(erL_[0])) & erMask_];
    const float ERR = gR_[0] * er_[(erPos_ + static_cast<uint32_t>(erR_[0])) & erMask_];
    const float v1 = in_[0].ap(x, cA_, cA_);
    const float v2 = in_[1].ap(v1, cB_, cA_);
    erWrite(v2);
    const float v3 = in_[2].ap(v2, cA_, cA_);
    const float v4 = in_[3].ap(v3, cB_, cA_);
    const float pre = pd_.delay(v4) + 1e-20f;
    tankRack(pre, rd_ * (lfo * 0.5f + 0.5f));
    float TL, TR;
    taps(TL, TR, false);
    s_ = s_ < -99 ? wet_ : s_;
    s_ = ((wet_ - s_) * 0.001f) + s_;
    const float oL = s_ * TL + er_g_ * EL, oR = s_ * TR + er_g_ * ERR;
    const float sum = oL + oR, dif = (oL - oR) * wf_;
    AL = sum + dif, AR = sum - dif, BL = EL, BR = ERR;
  }

  // ---- Multi variant
  void setMulti(const float* n) {
    int v[MpCount];
    v[MpInput] = levelDb10(n[MpInput]), v[MpLowCut] = lowCutHz(n[MpLowCut]), v[MpHighCut] = cut100(n[MpHighCut]);
    v[MpPredelay] = predelayMs(n[MpPredelay]), v[MpRoom] = roomOf(n[MpRoom]), v[MpDiffusion] = diffusionPct(n[MpDiffusion]);
    v[MpBass] = bass10(n[MpBass]), v[MpCross] = crossHz(n[MpCross]), v[MpDecay] = decay10(n[MpDecay]), v[MpDamp] = cut100(n[MpDamp]);
    v[MpDry] = levelDb10(n[MpDry]), v[MpEr] = levelDb10(n[MpEr]), v[MpWet] = levelDb10(n[MpWet]), v[MpWidth] = widthPct(n[MpWidth]);
    for (int k = MpMuteIn; k <= MpMuteWet; ++k) v[k] = lawInt(n[k], 1.f, 0.5f);
    for (int k = 0; k < MpCount; ++k)
      if (v[k] != mv_[k]) mv_[k] = v[k], dirty_ = true;
  }
  // Adds the reverb of (inL, inR) to (oL, oR).
  void tickMulti(float inL, float inR, float& oL, float& oR) {
    if (dirty_) recomputeMulti();
    const float iL = inL * inGain_, iR = inR * inGain_;
    const float mono = ((iR + iL) * 0.5f) + 1e-30f;
    lp_ = ((mono - lp_) * cHC_) + lp_;
    const float a = ((lp_ - apA_) * cLC_) + apX_;
    apA_ = a, apX_ = lp_;
    const float x = cLC_ != 0 ? (((lp_ - a) * 0.5f) + 1e-30f) : lp_;
    float EL = 0, ERR = 0;
    if (er_g_ != 0) {
      float b[7], c[8];
      for (int i = 0; i < 7; ++i) b[i] = er_[(erPos_ + static_cast<uint32_t>(erL_[i])) & erMask_];
      for (int i = 0; i < 8; ++i) c[i] = er_[(erPos_ + static_cast<uint32_t>(erR_[i])) & erMask_];
      EL = gL_[6] * b[6] + (((gL_[0] * b[0] + gL_[1] * b[1]) - gL_[2] * b[2]) + (gL_[5] * b[5] + (gL_[4] * b[4] + gL_[3] * b[3])));
      ERR = ((gR_[2] * c[2] + gR_[0] * c[0]) + (gR_[6] * c[6] + (gR_[5] * c[5] + gR_[3] * c[3]))) - ((gR_[7] * c[7] + gR_[1] * c[1]) + gR_[4] * c[4]);
    }
    const float v1 = in_[0].ap(x, cA_, cA_);
    const float v2 = in_[1].ap(v1, cB_, cA_);
    erWrite(v2);
    const float v3 = in_[2].ap(v2, cA_, cA_);
    const float v4 = in_[3].ap(v3, cB_, cA_);
    const float pre = pd_.delay(v4) + 1e-20f;
    tank(pre, false, 0);
    float TL, TR;
    taps(TL, TR, false);
    s_ = s_ < -99 ? wet_ : s_;
    s_ = ((wet_ - s_) * 0.001f) + s_;
    const float o1 = er_g_ * EL + s_ * TL, o2 = er_g_ * ERR + s_ * TR;
    const float sum = o1 + o2, dif = (o1 - o2) * wf_;
    oL += (dry_ * iL + sum) + dif;
    oR += dry_ * iR + (sum - dif);
  }

  // ---- Studio variant
  void setStudio(const StudioSettings& s) {
    if (s.predelay != st_.predelay) {  // the reference re-allocates: everything restarts silent
      st_.predelay = s.predelay;
      pd_.N = std::clamp(s.predelay, 1, pd_.cap);
      for (auto* L : lines()) L->p = 0;
      pd_.p = 0;
      resume();
      lastRoom_ = 0;
      dirty_ = true;
    }
    if (s.lowCutHz != st_.lowCutHz || s.highCut != st_.highCut || s.room != st_.room || s.diffusion != st_.diffusion ||
        s.decaySamples != st_.decaySamples || s.damp != st_.damp || s.bass != st_.bass || s.cross != st_.cross)
      dirty_ = true;
    st_ = s;
    if (dirty_) recomputeStudio();
    // block-start modulation constants
    const float inv = 1.f / sr_;
    dTarget_ = (sr_ * 0.0045351474f) * st_.modDepth;
    cDepth_ = 1.f - std::exp2(-66.43856f * inv);
    inc_ = ((((st_.modSpeed * st_.modSpeed) * 9.9f) * st_.modSpeed) + 0.1f) * inv;
  }
  // One mono sample → wet (TL, TR) and reflections (EL, ER).
  void tickStudio(float in, float& TL, float& TR, float& EL, float& ER) {
    const float lfo = static_cast<float>(std::sin(6.283185307179586 * phi_));
    phi_ = phi_ + inc_;
    phi_ = phi_ - std::trunc(phi_);
    d_ = std::fabs(dTarget_ - d_) < 0.01f ? dTarget_ : (d_ + 1e-30f) + ((dTarget_ - d_) * cDepth_);
    const float x0 = in + 1e-30f;
    lp_ = ((x0 - lp_) * cHC_) + lp_;
    const float a = ((lp_ - apA_) * cLC_) + apX_;
    apA_ = a, apX_ = lp_;
    const float x = cLC_ != 0 ? (((lp_ - a) * 0.5f) + 1e-30f) : lp_;
    float b[7], c[8];
    for (int i = 0; i < 7; ++i) b[i] = er_[(erPos_ + static_cast<uint32_t>(erL_[i])) & erMask_];
    for (int i = 0; i < 8; ++i) c[i] = er_[(erPos_ + static_cast<uint32_t>(erR_[i])) & erMask_];
    EL = gL_[6] * b[6] + ((gL_[5] * b[5] + (gL_[4] * b[4] + gL_[3] * b[3])) + ((gL_[1] * b[1] + gL_[0] * b[0]) - gL_[2] * b[2]));
    ER = (gR_[6] * c[6] + ((gR_[5] * c[5] + gR_[3] * c[3]) + (gR_[2] * c[2] + gR_[0] * c[0]))) - (gR_[7] * c[7] + (gR_[4] * c[4] + gR_[1] * c[1]));
    const float v1 = in_[0].ap(x, cA_, cA_);
    const float v2 = in_[1].ap(v1, cA_, cA_);
    erWrite(v1);
    const float v3 = in_[2].ap(v2, cA_, cA_);
    const float v4 = in_[3].ap(v3, cA_, cA_);
    const float mod = d_ * ((lfo * 0.5f) + 0.5f);
    const float pre = pd_.delay(v4) + 1e-20f;
    tank(pre, true, mod);
    taps(TL, TR, true);
  }

 private:
  struct Line {
    std::vector<float> buf;
    int N = 1, p = 0, cap = 1;
    void alloc(int n) {
      cap = std::max(1, n);
      buf.assign(static_cast<size_t>(cap), 0.f);
      N = cap, p = 0;
    }
    void fill(float v) { std::fill(buf.begin(), buf.end(), v); }
    void zeroUsed() { std::fill(buf.begin(), buf.begin() + N, 0.f); }
    void adv() {
      if (p <= 0) p = N;
      --p;
    }
    void setLen(int n) {
      N = std::clamp(n, 1, cap);
      p %= N;
    }
    float ap(float x, float cff, float cfb) {
      const float v = buf[static_cast<size_t>(p)] - cff * x;
      buf[static_cast<size_t>(p)] = (cfb * v) + x;
      adv();
      return v;
    }
    float delay(float x) {  // read, then write
      const float y = buf[static_cast<size_t>(p)];
      buf[static_cast<size_t>(p)] = x;
      adv();
      return y;
    }
    float tap(int T) const {
      int i = p + T;
      if (i >= N) i -= N;
      if (i >= N) i %= N;
      return buf[static_cast<size_t>(i)];
    }
  };
  std::array<Line*, 12> lines() {
    return {&in_[0], &in_[1], &in_[2], &in_[3], &l_[0], &l_[1], &l_[2], &l_[3], &r_[0], &r_[1], &r_[2], &r_[3]};
  }
  void erWrite(float v) {
    er_[erPos_] = v;
    if (erPos_ == 0) erPos_ = erMask_ + 1;
    --erPos_;
  }
  // One read through a 4096-sample modulation line (linear interpolation).
  float modLine(int k, float x, float f) {
    std::vector<float>& m = mod_[k];
    uint32_t& w = modW_[k];
    m[w] = x;
    w = (w + 1) & 4095;
    const uint32_t i = static_cast<uint32_t>(f);
    const float fr = f - static_cast<float>(i);
    const float a = m[(w - 1 - i) & 4095], b = m[(w - 2 - i) & 4095];
    return ((b - a) * fr) + a;
  }
  void tank(float pre, bool modulated, float mod) {
    // left half
    const float xl = g_ * r_[3].buf[static_cast<size_t>(r_[3].p)];
    const float xa = ((xl - xsL_) * cX_) + xpL_;
    xsL_ = xa, xpL_ = xl;
    const float yl = ((((xl + pre) + ((xa + xl) * kBass_)) - dL_) * cD_) + dL_;
    dL_ = yl;
    const float zl = modulated ? modLine(0, yl, mod) : yl;
    const float u = l_[0].ap(zl, cT1_, cT1_);
    const float t = l_[1].delay(u);
    const float u2 = l_[2].ap(t * g_, cT2_, cT2_);
    l_[3].buf[static_cast<size_t>(l_[3].p)] = u2;
    l_[3].adv();
    const float xr = g_ * l_[3].buf[static_cast<size_t>(l_[3].p)];
    // right half
    const float xb = ((xr - xsR_) * cX_) + xpR_;
    xsR_ = xb, xpR_ = xr;
    const float yr = ((((xr + pre) + ((xb + xr) * kBass_)) - dR_) * cD_) + dR_;
    dR_ = yr;
    const float zr = modulated ? modLine(1, yr, mod) : yr;
    const float w = r_[0].ap(zr, cT1_, cT1_);
    const float t2 = r_[1].delay(w);
    const float w2 = r_[2].ap(t2 * g_, cT2_, cT2_);
    r_[3].buf[static_cast<size_t>(r_[3].p)] = w2;
    r_[3].adv();
  }
  // Tank with the rack's grouping and a modulated delay after each damping one-pole (depth 0: none).
  void tankRack(float pre, float mod) {
    const float xl = g_ * r_[3].buf[static_cast<size_t>(r_[3].p)];
    const float xa = ((xl - xsL_) * cX_) + xpL_;
    xsL_ = xa, xpL_ = xl;
    const float yl = (((((xa + xl) * kBass_ + xl) + pre) - dL_) * cD_) + dL_;
    dL_ = yl;
    const float zl = modLine(0, yl, mod);
    const float u = l_[0].ap(zl, cT1_, cT1_);
    const float t = l_[1].delay(u);
    const float u2 = l_[2].ap(t * g_, cT2_, cT2_);
    l_[3].buf[static_cast<size_t>(l_[3].p)] = u2;
    l_[3].adv();
    const float xr = g_ * l_[3].buf[static_cast<size_t>(l_[3].p)];
    const float xb = ((xr - xsR_) * cX_) + xpR_;
    xsR_ = xb, xpR_ = xr;
    const float yr = (((((xb + xr) * kBass_ + xr) + pre) - dR_) * cD_) + dR_;
    dR_ = yr;
    const float zr = modLine(1, yr, mod);
    const float w = r_[0].ap(zr, cT1_, cT1_);
    const float t2 = r_[1].delay(w);
    const float w2 = r_[2].ap(t2 * g_, cT2_, cT2_);
    r_[3].buf[static_cast<size_t>(r_[3].p)] = w2;
    r_[3].adv();
  }
  void recomputeRack() {
    const float sr = sr_;
    const RackSettings& r = rk_;
    inGain_ = 1.f;  // the module runs input / dry / reflections / reverb at 0 dB
    if (r.lowCut <= 19) cLC_ = 0;
    else {
      const float f = static_cast<float>(r.lowCut) >= 0.5f * sr ? 0.5f * sr : static_cast<float>(r.lowCut);
      const float t = std::tan(static_cast<float>(static_cast<double>(f) * 3.141592653589793 / static_cast<double>(sr)));
      cLC_ = (t - 1) / (t + 1);
    }
    auto cut = [&](int v) {
      if (v >= 221) return 1.f;
      const float cs = std::cos(static_cast<float>(static_cast<double>(2 * 100 * v) * 3.141592653589793 / static_cast<double>(sr)));
      return (std::sqrt((2 - cs) * (2 - cs) - 1) - (2 - cs)) + 1;
    };
    cHC_ = cut(r.highCut);
    cD_ = cut(r.damp);
    const int64_t pv = static_cast<int64_t>(r.predelayMs) * sri_;
    pd_.setLen(pv < 1000 ? 1 : static_cast<int>(pv / 1000));
    const int room = std::clamp(r.room, 1, 100);
    layout(room);
    const float E = static_cast<float>(static_cast<int>((static_cast<int64_t>(room) + 3) * sri_ / 103));
    erL_[0] = static_cast<int>((static_cast<float>(kErL[0]) * 1.5f * E) / 44100.f);
    erR_[0] = static_cast<int>((static_cast<float>(kErR[0]) * 1.5f * E) / 44100.f);
    const float M = static_cast<float>(std::min(erL_[0], erR_[0]));
    for (float& g : gL_) g = 0;
    for (float& g : gR_) g = 0;
    gL_[0] = (0.5f * M) / static_cast<float>(std::max(1, erL_[0]));
    gR_[0] = (0.36f * M) / static_cast<float>(std::max(1, erR_[0]));
    if (room != lastRoom_) {
      lastRoom_ = room;
      for (auto* L : lines()) L->zeroUsed();
      std::fill(er_.begin(), er_.end(), 0.f);
    }
    const float d = static_cast<float>(r.diffusion) / 100.f;
    if (d == 0) {
      cT1_ = cT2_ = cA_ = cB_ = 0;
    } else {
      auto c = [&](int sum, float k) { return std::exp(((static_cast<float>(sum) * 0.5f / sr) * -6.9077554f) / (d * k)); };
      cT1_ = c(l_[0].N + r_[0].N, 0.35f), cT2_ = c(l_[2].N + r_[2].N, 0.51f);
      cA_ = c(in_[0].N + in_[1].N, 0.07f), cB_ = c(in_[2].N + in_[3].N, 0.11f);
    }
    const float X = (static_cast<float>(sumN()) / sr) * -6.9077554f;
    const float dec = static_cast<float>(std::max(1, r.decay)) / 10.f;
    g_ = std::pow(std::exp(X / dec), 0.25f);
    const float gb = std::pow(std::exp(X / ((static_cast<float>(r.bass) * dec) / 10.f)), 0.25f);
    const float ratio = g_ > 0 ? gb / g_ : 1.f;
    kBass_ = (ratio - 1) * 0.5f;
    const float m = std::min(ratio, 1.f);
    const float t = std::tan(static_cast<float>(static_cast<double>(r.cross) * 3.141592653589793 / static_cast<double>(sr)));
    cX_ = (t - m) / (m + t);
    dry_ = 1.f, er_g_ = 0.5f, wet_ = 0.3f, wf_ = 1.f;
    rcDepth_ = 1.f - std::pow(0.01f, 1.f / (0.1f * static_cast<float>(sri_)));
    weights();
    dirty_ = false;
  }
  void taps(float& TL, float& TR, bool studio) {
    const float a0 = r_[1].tap(tA_[0]), a1 = r_[1].tap(tA_[1]), a2 = r_[2].tap(tA_[2]), a3 = r_[3].tap(tA_[3]), a4 = l_[1].tap(tA_[4]),
                a5 = l_[2].tap(tA_[5]), a6 = l_[3].tap(tA_[6]);
    const float b0 = l_[1].tap(tB_[0]), b1 = l_[1].tap(tB_[1]), b2 = l_[2].tap(tB_[2]), b3 = l_[3].tap(tB_[3]), b4 = r_[1].tap(tB_[4]),
                b5 = r_[2].tap(tB_[5]), b6 = r_[3].tap(tB_[6]);
    if (!studio) {
      TL = a6 * Wh_ + (a5 + ((a0 + a4 * W0_) - ((a3 * W2_ + a1 * W1_) + a2 * W3_)));
      TR = (b6 * Wh_ + (b5 + (b0 + b1 * W1_))) - ((b2 * W3_ + b3 * W2_) + b4 * W0_);
    } else {
      TL = (((a4 * W0_) + (a0 - ((a2 * W3_ + a1 * W1_) + a3 * W2_))) + a5) + a6 * Wh_;
      TR = (((b1 * W1_ + b0) + b5) + b6 * Wh_) - (b4 * W0_ + (b3 * W2_ + b2 * W3_));
    }
  }
  // Line lengths, taps and index wrap for room `room`.
  void layout(int room) {
    const int64_t S = roomScale(room, sri_);
    for (int k = 0; k < 4; ++k) in_[k].setLen(lineLen(kInConst[k], S));
    for (int k = 0; k < 4; ++k) l_[k].setLen(lineLen(kLeftConst[k], S)), r_[k].setLen(lineLen(kRightConst[k], S));
    for (int k = 0; k < 7; ++k) tA_[k] = static_cast<int>(static_cast<int64_t>(kTapA[k]) * S / 20000), tB_[k] = static_cast<int>(static_cast<int64_t>(kTapB[k]) * S / 20000);
  }
  int sumN() const { return l_[1].N + l_[3].N + r_[1].N + r_[3].N; }
  void weights() {
    W0_ = ((1 - g_) * 0.6f) + g_, W1_ = ((1 - g_) * 0.25f) + g_, W2_ = ((1 - g_) * 0.2f) + g_, W3_ = ((1 - g_) * 0.7f) + g_;
    Wh_ = ((1 - g_) * 0.5f) + g_;
  }
  static float onePoleCoef(float v, float sr) {  // float cosine form (multi variant)
    const float cs = std::cos((v * 628.31854f) / sr);
    return (cs + (-1.f)) + std::sqrt(((2 - cs) * (2 - cs)) + (-1.f));
  }
  void recomputeMulti() {
    const int* v = mv_;
    const float sr = sr_;
    inGain_ = (v[MpInput] >= -360 && !v[MpMuteIn]) ? std::pow(10.f, static_cast<float>(v[MpInput]) * 0.005f) : 0.f;
    if (v[MpLowCut] < 20) cLC_ = 0;
    else {
      const float f = std::min(0.5f * sr, static_cast<float>(v[MpLowCut]));
      const float t = std::tan((f * 3.1415927f) / sr);
      cLC_ = (t + (-1.f)) / (t + 1.f);
    }
    cHC_ = v[MpHighCut] > 220 ? 1.f : onePoleCoef(static_cast<float>(v[MpHighCut]), sr);
    const int P = static_cast<int>(static_cast<int64_t>(v[MpPredelay]) * sri_ / 1000);
    pd_.setLen(P < 2 ? 1 : P);
    const int room = std::clamp(v[MpRoom], 1, 100);
    layout(room);
    const int E = static_cast<int>((static_cast<int64_t>(room) + 3) * sri_ / 103);
    const float e = static_cast<float>(E) * 3.4013603e-5f;
    for (int i = 0; i < 7; ++i) erL_[i] = static_cast<int>(static_cast<float>(kErL[i]) * e);
    for (int i = 0; i < 8; ++i) erR_[i] = static_cast<int>(static_cast<float>(kErR[i]) * e);
    const float M = static_cast<float>(std::min(erL_[0], erR_[0]));
    for (int i = 0; i < 7; ++i) gL_[i] = (0.5f * M) / static_cast<float>(std::max(1, erL_[i]));
    for (int i = 0; i < 8; ++i) gR_[i] = (0.36f * M) / static_cast<float>(std::max(1, erR_[i]));
    if (room != lastRoom_) {
      lastRoom_ = room;
      for (auto* L : lines()) L->zeroUsed();
      std::fill(er_.begin(), er_.end(), 0.f);
    }
    const float dF = static_cast<float>(v[MpDiffusion]);
    if (0.01f * dF == 0) {
      cT1_ = cT2_ = cA_ = cB_ = 0;
    } else {
      auto c = [&](int sum, float k) { return std::exp((static_cast<float>(sum) * -3.4538777f) / ((dF * sr) * k)); };
      cT1_ = c(l_[0].N + r_[0].N, 0.0035f), cT2_ = c(l_[2].N + r_[2].N, 0.0051f);
      cA_ = c(in_[0].N + in_[1].N, 0.0007f), cB_ = c(in_[2].N + in_[3].N, 0.0011f);
    }
    const float inv = 1.f / sr;
    const float X = (static_cast<float>(sumN()) * -6.9077554f) * inv;
    const float decay = static_cast<float>(std::max(1, v[MpDecay]));
    const float e1 = std::exp(X / (0.1f * decay));
    const float s = std::sqrt(e1);
    g_ = std::fabs(s) < 1.1754944e-38f ? 0.f : std::sqrt(s);
    const float e2 = std::exp(X / ((decay * 0.0100000007f) * static_cast<float>(std::max(1, v[MpBass]))));
    const float q = std::sqrt(std::sqrt(e2));
    const float ratio = g_ > 0 ? q / g_ : 1.f;
    kBass_ = (0.5f * ratio) + (-0.5f);
    const float m = std::min(ratio, 1.f);
    const float t = std::tan((static_cast<float>(v[MpCross]) * 3.1415927f) * inv);
    cX_ = (t - m) / (m + t);
    cD_ = v[MpDamp] > 220 ? 1.f : onePoleCoef(static_cast<float>(v[MpDamp]), sr);
    auto lvl = [](int d, bool mute) { return (d >= -360 && !mute) ? std::pow(10.f, static_cast<float>(d) * 0.005f) : 0.f; };
    dry_ = lvl(v[MpDry], v[MpMuteDry]);
    er_g_ = lvl(v[MpEr], v[MpMuteEr]) * 0.5f;
    wet_ = lvl(v[MpWet], v[MpMuteWet]) * 0.3f;
    wf_ = static_cast<float>(v[MpWidth]) * 0.01f;
    weights();
    dirty_ = false;
  }
  void recomputeStudio() {
    const double sr = sri_;
    const float inv = static_cast<float>(1.0 / sr);
    const float srf = static_cast<float>(sr);
    if (st_.lowCutHz == 0) cLC_ = 0;
    else {
      const float f = std::min(static_cast<float>(st_.lowCutHz), 0.5f * srf);
      const float t = static_cast<float>(std::tan(static_cast<double>(f) * 3.141592653589793 * static_cast<double>(inv)));
      cLC_ = (t - 1) / (t + 1);
    }
    auto cut = [&](int hc) {
      if (hc == 0) return 1.f;
      const float h = std::min(static_cast<float>(hc), 0.5f * srf);
      const float k = static_cast<float>(2.0 - std::cos(static_cast<double>(200.f * h) * 3.141592653589793 * static_cast<double>(inv)));
      return 1 - (k - std::sqrt(k * k - 1));
    };
    cHC_ = cut(st_.highCut);
    cD_ = cut(st_.damp);
    const int room = std::clamp(st_.room, 1, 100);
    layout(room);
    if (room != lastRoom_) {
      lastRoom_ = room;
      for (auto* L : lines()) L->zeroUsed();
      std::fill(er_.begin(), er_.end(), 0.f);
    }
    const float d = static_cast<float>(st_.diffusion * 0.01);
    const float k = 0.5f * inv;
    const float s0 = static_cast<float>(in_[0].N + in_[1].N) * k, s2 = static_cast<float>(l_[0].N + r_[0].N) * k,
                s3 = static_cast<float>(l_[2].N + r_[2].N) * k;
    const double ln001 = std::log(0.001);
    if (d == 0) {
      cA_ = cB_ = cT1_ = cT2_ = 0;
    } else {
      cA_ = static_cast<float>(std::exp(static_cast<double>(s0) * ln001 / (static_cast<double>(d) * 0.07)));
      cB_ = cA_;
      cT1_ = static_cast<float>(std::exp(static_cast<double>(s2) * ln001 / (static_cast<double>(d) * 0.35)));
      cT2_ = static_cast<float>(std::exp(static_cast<double>(s3) * ln001 / (static_cast<double>(d) * 0.51)));
    }
    const double sum = static_cast<double>(static_cast<float>(sumN()));
    const double ds = std::max(1.0, static_cast<double>(st_.decaySamples));
    g_ = static_cast<float>(std::pow(std::exp(sum * ln001 / ds), 0.25));
    const float gb = static_cast<float>(std::pow(std::exp(sum * ln001 / (ds * static_cast<double>(st_.bass))), 0.25));
    const float ratio = g_ > 0 ? gb / g_ : 1.f;
    kBass_ = (ratio - 1) * 0.5f;
    const float m = std::min(ratio, 1.f);
    const float t = static_cast<float>(std::tan(static_cast<double>(st_.cross) * 3.141592653589793 * static_cast<double>(inv)));
    cX_ = (t - m) / (t + m);
    const float e = static_cast<float>((room + 3) * sri_) * 3.302292e-7f;
    for (int i = 0; i < 7; ++i) erL_[i] = static_cast<int>(std::nearbyint(static_cast<float>(kErL[i]) * e));
    for (int i = 0; i < 8; ++i) erR_[i] = static_cast<int>(std::nearbyint(static_cast<float>(kErR[i]) * e));
    const float M = static_cast<float>(std::min(erR_[0], erL_[0]));
    for (int i = 0; i < 7; ++i) gL_[i] = (0.5f * M) / static_cast<float>(std::max(1, erL_[i]));
    for (int i = 0; i < 8; ++i) gR_[i] = static_cast<float>(static_cast<double>(M) * 0.36 / static_cast<double>(std::max(1, erR_[i])));
    weights();
    dirty_ = false;
  }

  Variant v_ = Variant::Multi;
  int sri_ = 0;
  float sr_ = 44100;
  Line in_[4], l_[4], r_[4], pd_;
  std::vector<float> er_;
  uint32_t erMask_ = 0, erPos_ = 0;
  std::vector<float> mod_[2];
  uint32_t modW_[2] = {0, 0};
  int tA_[7] = {}, tB_[7] = {}, erL_[7] = {}, erR_[8] = {};
  float gL_[7] = {}, gR_[8] = {};
  int mv_[MpCount] = {-99999, -99999, -99999, -99999, -99999, -99999, -99999, -99999, -99999,
                      -99999, -99999, -99999, -99999, -99999, -99999, -99999, -99999, -99999};
  StudioSettings st_{75, 40, -1};
  int lastRoom_ = 0;
  bool dirty_ = true;
  float inGain_ = 1, cLC_ = 0, cHC_ = 1, cA_ = 0, cB_ = 0, cT1_ = 0, cT2_ = 0, g_ = 0, kBass_ = 0, cX_ = 0, cD_ = 1;
  float dry_ = 0, er_g_ = 0, wet_ = 0, wf_ = 1, s_ = -1000;
  float W0_ = 0, W1_ = 0, W2_ = 0, W3_ = 0, Wh_ = 0;
  float lp_ = 0, apA_ = 0, apX_ = 0, xsL_ = 0, xpL_ = 0, dL_ = 0, xsR_ = 0, xpR_ = 0, dR_ = 0;
  double phi_ = 0;
  float d_ = 0, dTarget_ = 0, cDepth_ = 0, inc_ = 0;
  RackSettings rk_;
  bool rkFirst_ = true;
  float rphi_ = 0, rfs_ = 0.1f, rd_ = 0, rdT_ = 0, rcDepth_ = 0;
};

// ---------------------------------------------------------------- standalone reverb (Studio variant + mix)
// Knob values (the standalone plugin's integer controls).
struct StudioKnobs {
  int lowCut = 75, highCut = 40, predelay = 0, room = 50, diffusion = 100, decay = 15, damp = 40, bass = 100, cross = 500;
  int sep = 0, dry = 128, er = 128, wet = 128, modSpeed = 684, modDepth = 0;
  bool tempoPredelay = false, midSide = false;
};
// Level curve: x = 1 → 1, else (11^x − 1) / 10 (x = knob / 128 for dry, / 256 for ER and wet).
inline float levelCurve(double x) { return x == 1 ? 1.f : static_cast<float>((std::exp(x * std::log(11.0)) - 1) * 0.1); }
// Pre-delay knob (1/384 s, or 1/192-beat ticks in tempo mode) → samples.
inline int predelaySamples(int k, int sri, bool tempo, double bpm) {
  const float x = static_cast<float>(static_cast<int64_t>(sri) * k);
  return static_cast<int>(std::nearbyint(tempo ? static_cast<float>(static_cast<double>(x) * 0.3125 / std::max(10.0, bpm)) : x / 384.0f));
}
inline StudioSettings studioSettings(const StudioKnobs& k, int sri, double bpm) {
  StudioSettings s;
  s.lowCutHz = k.lowCut <= 19 ? 0 : k.lowCut;
  s.highCut = k.highCut >= 221 ? 0 : k.highCut;
  s.predelay = predelaySamples(k.predelay, sri, k.tempoPredelay, bpm);
  s.room = std::clamp(k.room, 1, 100);
  s.diffusion = std::clamp(k.diffusion, 0, 100);
  s.decaySamples = static_cast<float>(static_cast<int64_t>(sri) * std::max(1, k.decay)) * 0.1f;
  s.damp = k.damp >= 221 ? 0 : k.damp;
  s.bass = static_cast<float>(k.bass) * 0.01f;
  s.cross = static_cast<float>(k.cross);
  s.modSpeed = static_cast<float>(k.modSpeed / 2048.0);
  s.modDepth = static_cast<float>(k.modDepth / 2048.0);
  return s;
}

class StudioReverb {
 public:
  static constexpr double kMaxPredelaySec = 4.0;
  void init(double sr) {
    sri_ = static_cast<int>(sr);
    eng_.init(Variant::Studio, sri_, static_cast<int>(kMaxPredelaySec * sri_) + 2);
    primed_ = false;
  }
  bool ready() const { return eng_.ready(); }
  void reset() {
    eng_.resume();
    primed_ = false;
  }
  void process(const StudioKnobs& k, double bpm, float* L, float* R, int n) {
    eng_.setStudio(studioSettings(k, sri_, bpm));
    // Output matrices (pan centred) with the separation law.
    const float dryT = levelCurve(std::clamp(k.dry, 0, 160) / 128.0);
    float mt[2][4];
    const float lv[2] = {levelCurve(std::clamp(k.er, 0, 160) / 256.0), levelCurve(std::clamp(k.wet, 0, 160) / 256.0)};
    const float sp = static_cast<float>(std::clamp(k.sep, -64, 64)) * 0.015625f;
    const float sq = sp > 0 ? static_cast<float>((std::pow(5.0, static_cast<double>(sp)) - 1) * 0.25) : sp;
    const float a = std::min(sq, 1.f) * 0.5f;
    for (int m = 0; m < 2; ++m) {
      const float v = lv[m];
      mt[m][0] = (1 - a) * v, mt[m][2] = a * v, mt[m][1] = (1 - a) * v, mt[m][3] = a * v;
      if (k.midSide) mt[m][1] = -mt[m][1], mt[m][3] = -mt[m][3];
    }
    if (!primed_) {
      dry_ = dryT;
      for (int m = 0; m < 2; ++m)
        for (int j = 0; j < 4; ++j) cur_[m][j] = mt[m][j];
      primed_ = true;
    }
    auto ramp = [](float& c, float t) { c = std::fabs(t - c) <= kStep ? t : c + (t > c ? kStep : -kStep); };
    for (int i = 0; i < n; ++i) {
      ramp(dry_, dryT);
      for (int m = 0; m < 2; ++m)
        for (int j = 0; j < 4; ++j) ramp(cur_[m][j], mt[m][j]);
      const float inL = L[i], inR = R[i];
      const float mono = k.midSide ? inL - inR : inL + inR;
      float TL, TR, EL, ER;
      eng_.tickStudio(mono, TL, TR, EL, ER);
      float oL = dry_ * inL, oR = dry_ * inR;
      const float src[2][2] = {{EL, ER}, {TL, TR}};
      for (int m = 0; m < 2; ++m) {
        oL = ((src[m][0] * cur_[m][0]) + (src[m][1] * cur_[m][2])) + oL;
        oR = ((src[m][1] * cur_[m][1]) + (src[m][0] * cur_[m][3])) + oR;
      }
      L[i] = oL, R[i] = oR;
    }
  }

 private:
  static constexpr float kStep = 1.f / 2048;  // per-sample level ramp (the reference's slope is unverified)
  Engine eng_;
  int sri_ = 0;
  bool primed_ = false;
  float dry_ = 1, cur_[2][4] = {};
};

}  // namespace gl::fullrev
