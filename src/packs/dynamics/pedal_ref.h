// Gridline — "dynamics" pack, J-35: the pedal board reference (gl.amprack
// RkModel = 2; behaviour study).
//
// 30 Hz high-pass → input gain → five boxes (distortion, chorus, flanger,
// phaser, reverb, delay, noise gate, equalizer, compressor, modulator,
// tube drive) → cabinet → eight-band EQ → output gain. The cabinet of the
// reference convolves FL Studio's impulse files, which Gridline does not ship:
// it is modelled procedurally (approximate). Values are the plugin's 50
// stored floats. Buffers are sized when the rate is first seen.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>
#include "../../engine/FullReverbRef.h"
#include "cabinet_ir.h"

namespace gl::packs::dynamics::pedal {

inline constexpr int kValues = 50, kBoxes = 5;
inline constexpr double kPi = 3.141592653589793;
enum Type { TEmpty = 0, TDist, TChorus, TFlanger, TPhaser, TReverb, TDelay, TGate, TEq, TComp, TMod, TTube, TCount };
inline int boxType(float v) { return std::clamp(static_cast<int>(std::trunc(v)), -1, 11); }
inline double levelGain(double x) { return std::pow(10.0, 2.4 * x - 1.2); }
// Everything but a cabinet (FL Studio's impulse files) is reproduced.
// `haveIr`: the selected cabinet's impulse file is readable (J-42).
inline bool stateExact(const float* v, bool haveIr = false) { return static_cast<int>(std::trunc(v[11])) <= 0 || haveIr; }

inline float flush(float x) { return std::fabs(x) < 1e-9f ? 0.f : x; }

// ---------------------------------------------------------------- filter primitives (double coefficients)
struct Biquad {
  double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
  double x1[2] = {}, x2[2] = {}, y1[2] = {}, y2[2] = {};
  void norm(double B0, double B1, double B2, double A0, double A1, double A2) {
    auto f = [](double v) { return std::fabs(v) < 1e-9 ? 0.0 : v; };
    b0 = f(B0 / A0), b1 = f(B1 / A0), b2 = f(B2 / A0), a1 = f(A1 / A0), a2 = f(A2 / A0);
  }
  // Shared designer: A = 10^(dB/20) in formulas meant for 10^(dB/40) (gain doubled, as the reference).
  void design(int kind, double f, double bw, double db, double sr) {  // kind 0 peak, 1 low shelf, 2 high shelf
    const double w = std::clamp(2 * kPi * f / sr, 0.0, 3.0), s = std::sin(w), c = std::cos(w);
    const double al = s * std::sinh(std::log(2.0) / 2 * bw * w / s), G = std::pow(10.0, db / 20);
    if (kind == 0) {
      norm(1 + al * G, -2 * c, 1 - al * G, 1 + al / G, -2 * c, 1 - al / G);
    } else {
      const double A = G, sq = 2 * std::sqrt(A) * al;
      if (kind == 1)
        norm(A * ((A + 1) - (A - 1) * c + sq), 2 * A * ((A - 1) - (A + 1) * c), A * ((A + 1) - (A - 1) * c - sq), (A + 1) + (A - 1) * c + sq,
             -2 * ((A - 1) + (A + 1) * c), (A + 1) + (A - 1) * c - sq);
      else
        norm(A * ((A + 1) + (A - 1) * c + sq), -2 * A * ((A - 1) + (A + 1) * c), A * ((A + 1) + (A - 1) * c - sq), (A + 1) - (A - 1) * c + sq,
             2 * ((A - 1) - (A + 1) * c), (A + 1) - (A - 1) * c - sq);
    }
  }
  void hp2(double f, double q, double sr) {
    const double w = 2 * kPi * f / sr, s = std::sin(w), c = std::cos(w), k0 = (1 + c) / (2 + s / q);
    b0 = k0, b1 = -2 * k0, b2 = k0, a1 = -2 * q * c / (q + s), a2 = (q - s) / (q + s);
  }
  void lp2(double f, double q, double sr) {
    const double w = 2 * kPi * f / sr, s = std::sin(w), c = std::cos(w), k0 = (1 - c) / (2 + s / q);
    b0 = k0, b1 = 2 * k0, b2 = k0, a1 = -2 * q * c / (q + s), a2 = (q - s) / (q + s);
  }
  void bp(double f, double q, double sr) {
    const double w = 2 * kPi * f / sr, s = std::sin(w), c = std::cos(w), al = s / (2 * q);
    b0 = (s / 2) / (1 + al), b1 = 0, b2 = -b0, a1 = -2 * c / (1 + al), a2 = (1 - al) / (1 + al);
  }
  float run(int ch, float xf) {
    const double x = flush(xf);
    double y = b0 * x + b1 * x1[ch] + b2 * x2[ch] - a1 * y1[ch] - a2 * y2[ch];
    if (std::fabs(y) < 1e-9) y = 0;
    x2[ch] = x1[ch], x1[ch] = x, y2[ch] = y1[ch], y1[ch] = y;
    return static_cast<float>(y);
  }
  void clear() {
    for (int c = 0; c < 2; ++c) x1[c] = x2[c] = y1[c] = y2[c] = 0;
  }
};

// ---------------------------------------------------------------- variable-speed tape line
struct Tape {
  std::vector<float> buf;
  uint32_t mask = 0, w = 0;
  double N = 4, vMin = 0.25, vT = 0.25, vS = 0.25, fb = 0.0078125, wet = 0, g = 0, depth = 0, rate = 0, ph = 0, sr = 44100;
  void build(int n, double rateSr) {
    N = std::max(4, n), sr = rateSr;
    uint32_t sz = 1;
    while (sz < static_cast<uint32_t>(N) * 2 + 8) sz <<= 1;
    buf.assign(sz, 0.f), mask = sz - 1;
    vMin = 1.0 / N, vT = 1.0 / (1 + N / 2);
    reset();
  }
  void setDelay(double a) {
    a = std::clamp(a, 0.0, 0.99);
    vT = 1.0 / (2 + a * (N - 2));
  }
  void reset() {
    ph = 0, vS = vT, g = 0, w = 0;
    std::fill(buf.begin(), buf.end(), 0.f);
  }
  float tick(float x) {
    vS += (vT - vS) * 0.005;
    ph += rate / sr;
    ph -= std::floor(ph);
    const double tri = ph < 0.5 ? 2 * ph : 2 - 2 * ph, S = tri * tri * (3 - 2 * tri);
    const double speed = std::clamp(vS * std::max(1.0, 1 + depth * S), vMin, 0.2);
    const double d = std::min(1.0 / speed, static_cast<double>(mask - 2));
    const uint32_t di = static_cast<uint32_t>(d);
    const double fr = d - di;
    const float a = buf[(w - di) & mask], b = buf[(w - di - 1) & mask];
    const float y = flush(static_cast<float>(a + (b - a) * fr));
    buf[w & mask] = flush(static_cast<float>(x + fb * y));
    ++w;
    g += (wet - g) * 0.01;
    return static_cast<float>(y * g);
  }
};

// ---------------------------------------------------------------- one pedal
class Pedal {
 public:
  void setup(int type, double sr) {
    type_ = type, sr_ = sr;
    const int B = static_cast<int>(std::lround(0.02 * sr));
    switch (type) {
      case TChorus:
        tape_[0].build(static_cast<int>(1.1 * B), sr), tape_[1].build(static_cast<int>(0.854 * B), sr), tape_[2].build(B, sr);
        break;
      case TFlanger:
        tape_[0].build(static_cast<int>(0.005 * sr), sr), tape_[1].build(static_cast<int>(0.005 * sr), sr);
        for (int i = 0; i < 2; ++i) tape_[i].fb = -0.1, tape_[i].depth = 1;
        break;
      case TDelay:
        tape_[0].build(static_cast<int>(sr), sr), tape_[1].build(static_cast<int>(sr), sr);
        for (int i = 0; i < 2; ++i) tape_[i].rate = 1, tape_[i].depth = 0.01;
        break;
      case TMod:
        tape_[0].build(static_cast<int>(0.01 * sr), sr);
        tape_[0].wet = 1, tape_[0].fb = 0, tape_[0].depth = 1;
        break;
      case TReverb:
        if (!rev_.ready() || rev_.rate() != static_cast<int>(sr)) rev_.init(fullrev::Variant::Multi, static_cast<int>(sr), static_cast<int>(sr) / 4);
        break;
      case TDist: {
        const int n1 = std::max(1, static_cast<int>(0.001 * 8 * sr)), n2 = std::max(1, static_cast<int>(0.0002 * 8 * sr));
        comb1_.assign(static_cast<size_t>(n1), 0.f), comb2_.assign(static_cast<size_t>(n2), 0.f);
        break;
      }
      default: break;
    }
    reset();
  }
  void reset() {
    for (auto& t : tape_)
      if (!t.buf.empty()) t.reset();
    for (auto& b : bq_) b.clear();
    std::fill(comb1_.begin(), comb1_.end(), 0.f), std::fill(comb2_.begin(), comb2_.end(), 0.f);
    i1_ = i2_ = 0;
    for (auto& h : hist_) h[0] = h[1] = h[2] = h[3] = 0;
    phTheta_ = 0, phBaseS_ = phBaseT_, phMixS_ = phMixT_, phPrimed_ = true;
    for (auto& c : phS_)
      for (auto& v : c) v = 0;
    phY_[0] = phY_[1] = 0;
    gate_[0] = gate_[1] = 0, cnt_[0] = cnt_[1] = 0;
    env_[0] = env_[1] = 0;
    modPh_ = 0, modA_ = modAT_, modP_ = modPT_;
    dcd_ = 0;
    if (type_ == TReverb && rev_.ready()) {
      rev_.resume();
      rn_[fullrev::MpHighCut] = 1, rn_[fullrev::MpDiffusion] = 1;
      rev_.setMulti(rn_);
    }
  }
  void setParams(const float* k) {
    for (int i = 0; i < 5; ++i) k_[i] = k[i];
    const double sr = sr_;
    switch (type_) {
      case TDist:
        bq_[0].bp(200, 0.99, sr), bq_[1].bp(1000, 0.99, sr);
        bq_[2].lp2(500, 4, sr);
        bq_[3].hp2(85, 1 + 30 * k[2], sr);
        bq_[4].design(0, 2500, 3, 20 * k[3] - 10, sr);
        {
          const int l2max = static_cast<int>(comb2_.size());
          l2_ = std::clamp(static_cast<int>(std::floor(k[1] * l2max)), 0, l2max - 1);
        }
        break;
      case TChorus: {
        const double r = 0.1 + 5 * k[1];
        const double rates[3] = {0.896 * r, r, 0.689 * r};
        for (int i = 0; i < 3; ++i) tape_[i].rate = rates[i], tape_[i].wet = -k[2] * k[2], tape_[i].depth = k[0] * k[0] * k[0];
        break;
      }
      case TFlanger:
        for (int i = 0; i < 2; ++i) {
          tape_[i].setDelay(std::clamp(static_cast<double>(k[0]) * k[0], 0.01, 1.0));
          tape_[i].rate = 0.01 + 10.0 * k[1] * k[1] * k[1];
          tape_[i].wet = -0.1 - 0.9 * k[2];
        }
        break;
      case TPhaser:
        phBaseT_ = std::max(static_cast<double>(k[1]) * k[1], 1e-5), phMixT_ = std::max(static_cast<double>(k[3]), 1e-5);
        if (phPrimed_) phBaseS_ = phBaseT_, phMixS_ = phMixT_, phPrimed_ = false;
        break;
      case TReverb:
        for (int i = 0; i < fullrev::MpCount; ++i) rn_[i] = fullrev::kMultiDefaults[i];
        rn_[fullrev::MpHighCut] = 1, rn_[fullrev::MpDiffusion] = 1;
        rn_[fullrev::MpDecay] = 0.6f * k[0] * k[0] * k[0];
        rn_[fullrev::MpDamp] = std::fabs(1 - k[1]);
        rn_[fullrev::MpWet] = 0.001f + 0.999f * k[2];
        rev_.setMulti(rn_);
        break;
      case TDelay:
        for (int i = 0; i < 2; ++i) {
          tape_[i].setDelay(std::clamp(static_cast<double>(k[0]), 0.1, 1.0));
          tape_[i].fb = -0.95 * k[1] * k[1];
          tape_[i].wet = -(0.1 + 0.9 * k[2] * k[2]);
        }
        break;
      case TEq: {
        auto lbl = [](double x) {
          const double g = 2 * x - 1;
          return 10 * g * std::fabs(g);
        };
        bq_[0].design(1, 300, 0.985, lbl(k[0]), sr), bq_[1].design(0, 700, 0.985, lbl(k[1]), sr);
        bq_[2].design(0, 1800, 0.985, lbl(k[2]), sr), bq_[3].design(2, 7000, 0.985, lbl(k[3]), sr);
        break;
      }
      case TMod:
        tape_[0].setDelay(std::clamp(static_cast<double>(k[0]) * k[0], 0.01, 1.0));
        tape_[0].rate = 30.0 * k[3] * k[3];
        modAT_ = k[1], modPT_ = k[2];
        break;
      case TTube:
        bq_[0].design(1, 200, 2.5, 28 * k[0] - 28, sr), bq_[1].design(2, 7000, 2, 28 * k[1] - 28, sr);
        bq_[2].lp2(4000, 0.5, sr);
        break;
      default: break;
    }
  }
  void tick(float& L, float& R) {
    switch (type_) {
      case TDist: dist(L, R); break;
      case TChorus: {
        const float m = L + R, a = tape_[0].tick(m), b = tape_[1].tick(m), c = tape_[2].tick(m);
        L += a + b, R += c + b;
        break;
      }
      case TFlanger: L += tape_[0].tick(L), R += tape_[1].tick(R); break;
      case TPhaser: phaser(L, R); break;
      case TReverb: {
        float oL = 0, oR = 0;
        rev_.tickMulti(L, R, oL, oR);
        L = oL, R = oR;
        break;
      }
      case TDelay: L += tape_[0].tick(L), R += tape_[1].tick(R); break;
      case TGate: gate(L, R); break;
      case TEq:
        for (int i = 0; i < 4; ++i) L = bq_[i].run(0, L), R = bq_[i].run(1, R);
        break;
      case TComp: comp(L, R); break;
      case TMod: modulator(L, R); break;
      case TTube: tube(L, R); break;
      default: break;
    }
  }

 private:
  static float shape(float u, double D) {
    const double a = std::fabs(u);
    const double y = (1 - (1 - a) * std::pow(10.0, -D * a));
    return static_cast<float>(u < 0 ? -y : y);
  }
  static float catmull(const float* p, float t) {
    const float t2 = t * t, t3 = t2 * t;
    return 0.5f * ((2 * p[1]) + (-p[0] + p[2]) * t + (2 * p[0] - 5 * p[1] + 4 * p[2] - p[3]) * t2 + (-p[0] + 3 * p[1] - 3 * p[2] + p[3]) * t3);
  }
  void dist(float& L, float& R) {
    const double D = 1 + 99.0 * k_[0] * k_[0];
    const float m = L + R;
    const float a = bq_[0].run(0, m) * 0.05f, b = bq_[1].run(1, m) * 5.f;
    float* ha = hist_[0];
    float* hb = hist_[1];
    ha[0] = ha[1], ha[1] = ha[2], ha[2] = ha[3], ha[3] = a;
    hb[0] = hb[1], hb[1] = hb[2], hb[2] = hb[3], hb[3] = b;
    float o = 0;
    for (int s = 0; s < 8; ++s) {
      const float t = static_cast<float>(s) / 8.f;
      const float ua = catmull(ha, t), ub = catmull(hb, t);
      const float r1 = comb1_[i1_];
      comb1_[i1_] = shape(ub, D) - 0.25f * r1;
      i1_ = (i1_ + 1) % comb1_.size();
      o = shape(ua, D) + 0.7f * r1;
      if (l2_ > 0) {
        const size_t len = static_cast<size_t>(l2_);
        const float r2 = comb2_[i2_ % len];
        comb2_[i2_ % len] = o;
        i2_ = (i2_ + 1) % len;
        o -= 0.3f * r2;
      }
      o = bq_[2].run(0, o);
    }
    o *= static_cast<float>(1.0 / (1 + 8.0 * k_[0]));
    o = bq_[3].run(0, o);
    o = bq_[4].run(0, o);
    L = R = o;
  }
  void phaser(float& L, float& R) {
    phBaseS_ += (phBaseT_ - phBaseS_) * 0.001;
    phMixS_ += (phMixT_ - phMixS_) * 0.001;
    phTheta_ += 44.1 * k_[0] * k_[0] / sr_;
    if (phTheta_ >= 2 * kPi) phTheta_ -= 2 * kPi;
    const double lo = 50 / sr_, hi = 20000 / sr_;
    const double xn = std::min(0.499, lo + (hi - lo) * (phBaseS_ + static_cast<double>(k_[2]) * k_[2] * k_[2] * (1 + std::cos(phTheta_)) / 2));
    const float c = static_cast<float>((1 - xn) / (1 + xn));
    float* io[2] = {&L, &R};
    for (int ch = 0; ch < 2; ++ch) {
      float u = *io[ch] - 1e-6f + 0.45488f * phY_[ch];
      for (float& s : phS_[ch]) {
        const float y = s - c * u;
        s = u + c * y;
        u = y;
      }
      phY_[ch] = u;
      *io[ch] += static_cast<float>(phMixS_) * u;
    }
  }
  void gate(float& L, float& R) {
    const double T = std::pow(10.0, 3.2 * k_[2] - 3.2);
    const double cA = 8.82 / ((2.0 * k_[0] * k_[0] + 0.05) * sr_), cR = 8.82 / ((2.0 * k_[1] * k_[1] + 0.05) * sr_);
    const int hold = static_cast<int>(0.1 * sr_);
    float* io[2] = {&L, &R};
    for (int c = 0; c < 2; ++c) {
      if (std::fabs(*io[c]) > T) cnt_[c] = 0, gate_[c] += (1 - gate_[c]) * cA;
      else if (++cnt_[c] <= hold) gate_[c] += (1 - gate_[c]) * cA;
      else gate_[c] -= gate_[c] * cR;
      gate_[c] = std::max(gate_[c], 1e-7);
      *io[c] = static_cast<float>(gate_[c] * *io[c]);
    }
  }
  void comp(float& L, float& R) {
    const double T = std::pow(10.0, 3.2 * k_[0] - 3.2), c = 1.0 / (31 - 30.0 * k_[1]);
    const double cA = 8.82 / ((2.0 * k_[2] * k_[2] + 0.1) * sr_), cR = 8.82 / ((4.8 * k_[3] * k_[3] + 0.2) * sr_);
    float* io[2] = {&L, &R};
    for (int ch = 0; ch < 2; ++ch) {
      const double a = std::fabs(*io[ch]);
      if (a > T) env_[ch] += (T / (T + (a - T) * c) - env_[ch]) * cA;
      else env_[ch] += (1 - env_[ch]) * cR;
      *io[ch] = static_cast<float>(env_[ch] * *io[ch]);
    }
  }
  void modulator(float& L, float& R) {
    modA_ += (modAT_ - modA_) * 0.001, modP_ += (modPT_ - modP_) * 0.001;
    const double C = std::cos(2 * kPi * modPh_);
    modPh_ += 30.0 * k_[3] * k_[3] / sr_;
    modPh_ -= std::floor(modPh_);
    const float v = tape_[0].tick(L + R);
    const double a = (1 - modA_ * C * C * C) * v;
    L = static_cast<float>((1 - modP_ * C) * a), R = static_cast<float>((1 + modP_ * C) * a);
  }
  void tube(float& L, float& R) {
    const double D = 10.0 * k_[2] * k_[2] * k_[2] + 0.01;
    float u = static_cast<float>((L + R) * D);
    u = bq_[0].run(0, u);
    u = bq_[2].run(0, u);
    const double a = std::fabs(u), s = (u < 0 ? -1 : 1) * (1 - (1 - a) * std::pow(10.0, -8 * a));
    dcd_ += (s - dcd_) * 0.01;
    float v = static_cast<float>(s - dcd_);
    v = bq_[1].run(0, v);
    const double M = 1.0 / (1 + (D / 2 - 1) * std::pow(10.0, -4 * D));
    L = R = static_cast<float>(v * 0.5 * M);
  }

  int type_ = 0;
  double sr_ = 44100;
  float k_[5] = {};
  Tape tape_[3];
  Biquad bq_[5];
  std::vector<float> comb1_, comb2_;
  size_t i1_ = 0, i2_ = 0;
  int l2_ = 0;
  float hist_[2][4] = {};
  double phTheta_ = 0, phBaseT_ = 1e-5, phMixT_ = 1e-5, phBaseS_ = 1e-5, phMixS_ = 1e-5;
  bool phPrimed_ = true;
  float phS_[2][6] = {}, phY_[2] = {};
  double gate_[2] = {}, env_[2] = {};
  int cnt_[2] = {};
  double modPh_ = 0, modA_ = 0, modP_ = 0, modAT_ = 0, modPT_ = 0, dcd_ = 0;
  fullrev::Engine rev_;
  float rn_[fullrev::MpCount] = {};
};

// ---------------------------------------------------------------- the board
class Board {
 public:
  void process(const float* v, double sr, float* L, float* R, int n) {
    if (sr != sr_) {
      sr_ = sr;
      for (int b = 0; b < kBoxes; ++b) type_[b] = -99;
      inHp_.hp2(30, 1, sr), inHp_.clear();
      for (auto& e : eqDb_) e = -999;
      primed_ = false;
    }
    // Boxes: type changes rebuild the pedal; enable writes schedule a reset.
    for (int b = 0; b < kBoxes; ++b) {
      const int t = std::max(0, boxType(v[45 + b]));
      if (t != type_[b]) {
        type_[b] = t;
        if (t > 0) ped_[b].setup(t, sr), ped_[b].setParams(v + 13 + 5 * b);
        for (int k = 0; k < 5; ++k) bk_[b][k] = v[13 + 5 * b + k];
        en_[b] = v[39 + b];
      }
      bool changed = false;
      for (int k = 0; k < 5; ++k) changed = changed || bk_[b][k] != v[13 + 5 * b + k];
      if (changed && t > 0) {
        for (int k = 0; k < 5; ++k) bk_[b][k] = v[13 + 5 * b + k];
        ped_[b].setParams(bk_[b]);
      }
      if (v[39 + b] != en_[b]) {
        en_[b] = v[39 + b];
        if (t > 0) ped_[b].reset();
      }
    }
    // EQ (designer per band; gain law 18 g|g| label dB).
    for (int k = 0; k < 8; ++k) {
      const double g = 2.0 * v[3 + k] - 1, db = 18 * g * std::fabs(g);
      if (db != eqDb_[k]) eqDb_[k] = db, eq_[k].design(k == 0 ? 1 : k == 7 ? 2 : 0, 125.0 * std::pow(2.0, k), 1.0, db, sr);
    }
    const bool eqOn = v[2] != 0;
    if (eqOn != eqOn_) {
      eqOn_ = eqOn;
      for (auto& e : eq_) e.clear();
    }
    const int cab = static_cast<int>(std::trunc(v[11]));
    if (cab != cab_) setCab(cab);
    const double gInT = levelGain(v[0]), gOutT = levelGain(v[1]);
    if (!primed_) gIn_ = gInT, gOut_ = gOutT, primed_ = true;
    for (int i = 0; i < n; ++i) {
      gIn_ += (gInT - gIn_) * 0.001;
      gOut_ += (gOutT - gOut_) * 0.001;
      float l = static_cast<float>(inHp_.run(0, L[i]) * gIn_), r = static_cast<float>(inHp_.run(1, R[i]) * gIn_);
      for (int b = 0; b < kBoxes; ++b)
        if (type_[b] > 0 && en_[b] != 0) ped_[b].tick(l, r);
      if (cab_ > 0 && cabK && cabRun) {  // J-42: FL Studio's impulse file (direct convolution, × 0.2 folded in)
        float w[2] = {l, r};
        cabRun->tick(*cabK, w);
        l = w[0], r = w[1];
      } else if (cab_ > 0) {
        l = cabinet(0, l), r = cabinet(1, r);
      }
      if (eqOn_)
        for (auto& e : eq_) l = e.run(0, l), r = e.run(1, r);
      L[i] = static_cast<float>(l * gOut_), R[i] = static_cast<float>(r * gOut_);
    }
  }
  void reset() { sr_ = 0; }
  const cab::Kernel* cabK = nullptr;  // J-42: impulse kernel / convolver state (null = procedural)
  cab::Run* cabRun = nullptr;

 private:
  // Procedural cabinet (approximate): high-pass, presence peak, steep low-pass, level.
  void setCab(int cab) {
    cab_ = cab;
    for (auto& f : cabF_) f.clear();
    if (cab <= 0) return;
    const bool longer = cab >= 6, stereoDark = cab >= 13;
    cabF_[0].hp2(longer ? 110 : 60, 1, sr_);
    cabF_[1].design(0, longer ? 3000 : 4000, 1.5, 3, sr_);
    cabF_[2].lp2(stereoDark ? 3000 : 6000, 0.7, sr_);
    cabF_[3].lp2(stereoDark ? 3000 : 6000, 0.7, sr_);
    cabGain_ = std::pow(10.0, (stereoDark ? 8.0 : 7.0) / 20.0) * 0.25;
  }
  float cabinet(int c, float x) {
    for (auto& f : cabF_) x = f.run(c, x);
    return static_cast<float>(x * cabGain_);
  }

  double sr_ = 0, gIn_ = 1, gOut_ = 1, cabGain_ = 1, eqDb_[8] = {};
  bool primed_ = false, eqOn_ = false;
  int type_[kBoxes] = {-99, -99, -99, -99, -99}, cab_ = 0;
  float en_[kBoxes] = {}, bk_[kBoxes][5] = {};
  Pedal ped_[kBoxes];
  Biquad inHp_, eq_[8], cabF_[4];
};

}  // namespace gl::packs::dynamics::pedal
