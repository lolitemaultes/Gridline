// Gridline — "dynamics" pack, J-35: the module rack reference (gl.amprack
// RkModel = 1; study analysis/demos/specs/j35_distructor.md).
//
// Up to four modules in series, then an optional soft clip. A module is a
// distortion (18 algorithms), a filter (22 types), a chorus (the stereo delay
// reference, fully wet) or a speaker (J-42: FL Studio's impulse files read in
// place from the installed program, cabinet_ir.h; a procedural cabinet when
// they are not available). Each module: input gain → processor → dry / wet →
// output gain, distortion and filter at 2^os × the rate through cascaded IIR
// half-band resamplers (the dry path too), then (distortion) an optional DC
// remover. Settings are the stored integers (ModuleState). Sizes are fixed at
// configure(); process never allocates.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <random>
#include <vector>
#include "../../engine/Delay3Ref.h"
#include "cabinet_ir.h"

namespace gl::packs::dynamics::drack {

inline constexpr int kModules = 4, kParams = 49;
inline constexpr double kPi = 3.141592653589793;
enum Kind { KDist = 0, KFilter, KChorus, KSpeaker };

struct ModuleState {
  int kind = 0, algo = 0, os = 0, removeDc = 0, disabled = 0, ftype = 0, fopt = 1, ir = 0, chorusMode = 0;
  int p[kParams] = {};
  bool operator==(const ModuleState& o) const { return std::memcmp(this, &o, sizeof *this) == 0; }
};
struct RackState {
  int count = 0;
  bool softClip = true;
  ModuleState m[kModules];
};
// Raw parameter defaults of a fresh module (j26 §3).
inline void moduleDefaults(ModuleState& m) {
  static const int d[kParams] = {0, 5000, 0, 10000, 0, 0, 500, 5000, 5000, 10000, 500, 5000, 5000, 10000, 2000, 0, 128, 10, 128, 128, 0, 100,
                                 128, 500, 0, 1000, 0, 950, 2000, 5000, 2000, 5000, 8400, 0, 0, 0, 5000, 10000, 5000, 10000, 0, 0, 0,
                                 108, 2880, 1500, 1995, 0, 0};
  std::memcpy(m.p, d, sizeof d);
}
// Level law of the fast-dist / soft clipper knobs: v / 128 → 1 at 128, else (11^c − 1)/10.
inline float level128(int v) {
  const float c = static_cast<float>(v) / 128.f;
  return c == 1.f ? 1.f : (std::pow(11.f, c) - 1.f) / 10.f;
}
// Which kinds / algorithms / types the reference is reproduced for.
// `haveIr`: the speaker's impulse file is readable (J-42).
inline bool moduleExact(const ModuleState& m, bool haveIr = false) {
  if (m.kind == KSpeaker) return m.disabled != 0 || haveIr;
  if (m.kind == KFilter) return m.ftype < 8 || m.ftype > 11;
  return true;
}

// ---------------------------------------------------------------- building blocks
struct Ramp {  // exponential per-sample parameter ramp (15 ms to 1 %)
  float tgt[2] = {0, 0}, cur[2] = {0, 0}, c = 0;
  void set(float l, float r) { tgt[0] = l, tgt[1] = r; }
  void tick() {
    for (int k = 0; k < 2; ++k) {
      cur[k] = tgt[k] + (cur[k] - tgt[k]) * c;
      if (std::fabs(cur[k] - tgt[k]) < 5.9604645e-08f) cur[k] = tgt[k];
    }
  }
};

struct SoftClip {
  float T = 1, K = 1, A = 1;
  void set(float t, float ceil) {
    T = t, K = ceil - t;
    if (K <= 0) K = 1.f / 65536.f;
    A = 1.f / K;
  }
  float run(float x) const {
    const float ax = std::fabs(x);
    if (ax <= T) return x;
    const float y = T + K * (1.f - std::exp(-A * (ax - T)));
    return x < 0 ? -y : y;
  }
};

struct DcRemover {
  double m[2] = {0, 0}, v[2] = {0, 0}, a = 0;
  void set(double fsHost) { a = 2.2835e-5 * 44100.0 / fsHost; }
  float run(int c, float x) {
    v[c] += a * (x - m[c]);
    m[c] += v[c];
    v[c] *= 0.96;
    return static_cast<float>(x - m[c]);
  }
};

// One 2× stage of the polyphase IIR half-band (three first-order sections per branch, at the low rate).
struct HbStage {
  static constexpr double kP[3] = {0.147377113601047, 0.4824685427697, 0.883005025769373};
  static constexpr double kQ[3] = {0.03915159773446, 0.302646848328493, 0.674615918546964};
  double px[2][3] = {}, py[2][3] = {}, qx[2][3] = {}, qy[2][3] = {};
  static double branch(const double* a, double* x1, double* y1, double x) {
    for (int i = 0; i < 3; ++i) {
      const double y = a[i] * (x - y1[i]) + x1[i];
      x1[i] = x, y1[i] = y, x = y;
    }
    return x;
  }
  void up(int c, double x, double& first, double& second) {
    first = branch(kQ, qx[c], qy[c], x);
    second = branch(kP, px[c], py[c], x);
  }
  double down(int c, double a, double b) { return 0.5 * (branch(kP, px[c], py[c], a) + branch(kQ, qx[c], qy[c], b)); }
};

struct Svf {  // linear trapezoidal SVF (float), modes as the reference
  float ic1[2] = {0, 0}, ic2[2] = {0, 0}, a1 = 1, a2 = 0, a3 = 0, m0 = 0, m1 = 0, m2 = 1;
  void design(int mode, double f, double k, double fs, double gainDb = 0) {
    double g = std::tan(kPi * f / fs);
    const double A = std::pow(10.0, gainDb / 40.0);
    double M0 = 0, M1 = 0, M2 = 1, kk = k;
    switch (mode) {
      case 0: M0 = 0, M1 = 0, M2 = 1; break;
      case 1: M0 = 1, M1 = -k, M2 = -1; break;
      case 2: M0 = 0, M1 = 1, M2 = 0; break;
      case 3: M0 = 1, M1 = -k, M2 = 0; break;
      case 4: M0 = 1, M1 = -k, M2 = -2; break;
      case 6: g = g / std::sqrt(A), M0 = 1, M1 = k * (A - 1), M2 = A * A - 1; break;
      case 7: g = g * std::sqrt(A), M0 = A * A, M1 = k * (1 - A) * A, M2 = 1 - A * A; break;
      case 8: M0 = 1, M1 = -2 * k, M2 = 0; break;
      default: break;
    }
    const double b1 = 1.0 / (1.0 + g * (g + kk));
    a1 = static_cast<float>(b1), a2 = static_cast<float>(g * b1), a3 = static_cast<float>(g * g * b1);
    m0 = static_cast<float>(M0), m1 = static_cast<float>(M1), m2 = static_cast<float>(M2);
  }
  float run(int c, float x) {
    const float v3 = x - ic2[c], v1 = a1 * ic1[c] + a2 * v3, v2 = ic2[c] + a2 * ic1[c] + a3 * v3;
    ic1[c] = 2 * v1 - ic1[c], ic2[c] = 2 * v2 - ic2[c];
    return m0 * x + m1 * v1 + m2 * v2;
  }
  void clear() { ic1[0] = ic1[1] = ic2[0] = ic2[1] = 0; }
};

struct OnePole {  // bilinear one-pole (float)
  float b0 = 1, b1 = 0, a1 = 0, x1[2] = {0, 0}, y1[2] = {0, 0};
  void lp(double f, double fs) {
    const double g = std::tan(kPi * f / fs);
    b0 = b1 = static_cast<float>(g / (1 + g)), a1 = static_cast<float>((1 - g) / (1 + g));
  }
  void hp(double f, double fs) {
    const double g = std::tan(kPi * f / fs);
    b0 = static_cast<float>(1 / (1 + g)), b1 = -b0, a1 = static_cast<float>((1 - g) / (1 + g));
  }
  float run(int c, float x) {
    const float y = b0 * x + b1 * x1[c] + a1 * y1[c];
    x1[c] = x, y1[c] = y;
    return y;
  }
  void clear() { x1[0] = x1[1] = y1[0] = y1[1] = 0; }
};

struct Biquad {  // RBJ, direct form I, double state, float I/O
  double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, x1[2] = {}, x2[2] = {}, y1[2] = {}, y2[2] = {};
  void lowpass(double f, double fs, double q) {
    const double w = 2 * kPi * f / fs, al = std::sin(w) / (2 * q), cs = std::cos(w), a0 = 1 + al;
    b1 = (1 - cs) / a0, b0 = b2 = b1 / 2, a1 = -2 * cs / a0, a2 = (1 - al) / a0;
  }
  void bandSkirt(double f, double fs, double q) {  // constant skirt gain (peak = Q)
    const double w = 2 * kPi * f / fs, al = std::sin(w) / (2 * q), cs = std::cos(w), a0 = 1 + al;
    b0 = q * al / a0, b1 = 0, b2 = -b0, a1 = -2 * cs / a0, a2 = (1 - al) / a0;
  }
  float run(int c, float xf) {
    const double x = xf, y = b0 * x + b1 * x1[c] + b2 * x2[c] - a1 * y1[c] - a2 * y2[c];
    x2[c] = x1[c], x1[c] = x, y2[c] = y1[c], y1[c] = y;
    return static_cast<float>(y);
  }
  void clear() {
    for (int c = 0; c < 2; ++c) x1[c] = x2[c] = y1[c] = y2[c] = 0;
  }
};

// ---------------------------------------------------------------- fast dist tables (computed, int16)
inline const std::vector<int16_t>& fastTables() {
  static const std::vector<int16_t> t = [] {
    std::vector<int16_t> v(2 * 10 * 8192);
    for (int type = 0; type < 2; ++type)
      for (int d = 1; d <= 10; ++d)
        for (int k = 0; k < 8192; ++k) {
          const double a = k / 8191.0;
          double D = a > 0 ? 20 * std::log10(a) : (type == 0 ? -10000.0 : -1000.0);
          if (type == 0) {
            if (D >= -25) D = (D + 25) * (11 - d) / 10 - 25;
            if (D <= -1.6 * (d - 1)) D += 1.6 * (d - 1);
          } else {
            D = -std::pow(-D, 0.8 + 0.02 * (10 - d));
          }
          v[static_cast<size_t>((type * 10 + d - 1) * 8192 + k)] = static_cast<int16_t>(std::nearbyint(32767 * std::pow(10.0, D / 20)));
        }
    return v;
  }();
  return t;
}
// Blood overdrive quarter-sine table.
inline const std::vector<float>& bodTable() {
  static const std::vector<float> t = [] {
    std::vector<float> v(65536);
    for (int i = 0; i < 65536; ++i) {
      const int k = i - 32768;
      v[static_cast<size_t>(i)] = static_cast<float>(k >= 0 ? std::sin(kPi * k / 65536.0) : -std::sin(kPi * (-k - 1) / 65536.0));
    }
    return v;
  }();
  return t;
}

// ---------------------------------------------------------------- one module
class Module {
 public:
  Module() {
    comb_.assign(1u << 16, 0.f);
    combMask_ = (1u << 16) - 1;
  }
  void configure(const ModuleState& s, double fsHost, bool first) {
    const bool rate = fsHost != fsHost_ || first;
    const int osMax = s.kind == KDist ? 4 : s.kind == KFilter ? 2 : 0;
    const int os = std::clamp(s.os, 0, osMax);
    const bool kindChange = first || s.kind != st_.kind || s.algo != st_.algo || s.ftype != st_.ftype || os != os_;
    st_ = s, os_ = os, fsHost_ = fsHost;
    fs_ = fsHost * (1 << os);
    if (rate) dc_.set(fsHost);
    const float rc = static_cast<float>(std::exp(std::log(0.01) / (0.015 * fs_)));
    for (Ramp* r : {&asym_, &xover_, &drive_, &dry_, &wet_}) r->c = rc;
    if (kindChange) clearStates();
    if (s.kind == KDist) setupDist();
    else if (s.kind == KFilter) setupFilter();
    else if (s.kind == KChorus) setupChorus(rate);
    else setupSpeaker();
  }
  // In place, one host frame.
  void tick(float& L, float& R) {
    const ModuleState& s = st_;
    if (s.disabled) return;
    const float gin = static_cast<float>(s.p[36] / 5000.0), w = static_cast<float>(s.p[37] / 10000.0), gout = static_cast<float>(s.p[38] / 5000.0);
    if (s.kind == KChorus || s.kind == KSpeaker) {
      float a[2] = {L * gin, R * gin}, f[2] = {a[0], a[1]};
      if (s.kind == KChorus) chorus(f);
      else speaker(f);
      L = (a[0] * (1 - w) + f[0] * w) * gout;
      R = (a[1] * (1 - w) + f[1] * w) * gout;
      return;
    }
    const int F = 1 << os_;
    float bl[16], br[16];
    if (os_ == 0) {
      bl[0] = L, br[0] = R;
    } else {
      upsample(L, R, bl, br);
    }
    for (int k = 0; k < F; ++k) {
      float a[2] = {bl[k] * gin, br[k] * gin}, f[2] = {a[0], a[1]};
      if (s.kind == KDist) distort(f);
      else filter(f);
      bl[k] = (a[0] * (1 - w) + f[0] * w) * gout;
      br[k] = (a[1] * (1 - w) + f[1] * w) * gout;
    }
    if (os_ == 0) L = bl[0], R = br[0];
    else downsample(bl, br, L, R);
    if (s.kind == KDist && s.removeDc) L = dc_.run(0, L), R = dc_.run(1, R);
  }

 private:
  void clearStates() {
    for (auto& h : up_) h = HbStage{};
    for (auto& h : dn_) h = HbStage{};
    std::memset(bod_, 0, sizeof bod_);
    bodG_ = bodB_ = bodF_ = bodPg_ = 0;
    for (auto& f : post_) f.clear();
    for (auto& f : svf_) f.clear();
    for (auto& f : vow_) f.clear();
    one_.clear(), phHp_.clear();
    std::fill(comb_.begin(), comb_.end(), 0.f), combW_ = 0;
    for (auto& z : phZ_) z = 0;
    held_[0] = held_[1] = cnt_[0] = cnt_[1] = 0;
    for (auto& f : spk_) f.clear();
    smoothPrimed_ = false;
  }
  void upsample(float L, float R, float* bl, float* br) {
    bl[0] = L, br[0] = R;
    int len = 1;
    for (int st = os_ - 1; st >= 0; --st) {
      float tl[16], tr[16];
      for (int i = 0; i < len; ++i) {  // in time order
        double a, b;
        up_[st].up(0, bl[i], a, b), tl[2 * i] = static_cast<float>(a), tl[2 * i + 1] = static_cast<float>(b);
        up_[st].up(1, br[i], a, b), tr[2 * i] = static_cast<float>(a), tr[2 * i + 1] = static_cast<float>(b);
      }
      len *= 2;
      for (int i = 0; i < len; ++i) bl[i] = tl[i], br[i] = tr[i];
    }
  }
  void downsample(float* bl, float* br, float& L, float& R) {
    int len = 1 << os_;
    for (int st = 0; st < os_; ++st) {
      for (int i = 0; i < len / 2; ++i) {
        bl[i] = static_cast<float>(dn_[st].down(0, bl[2 * i], bl[2 * i + 1]));
        br[i] = static_cast<float>(dn_[st].down(1, br[2 * i], br[2 * i + 1]));
      }
      len /= 2;
    }
    L = bl[0], R = br[0];
  }

  // ---------------------------------------------------------- distortion
  void setupDist() {
    const int* p = st_.p;
    const int algo = std::clamp(st_.algo, 0, 17);
    switch (algo) {
      case 0: break;
      case 1: {
        kPre_ = level128(p[16]);
        const float post = level128(p[18]);
        kWet_ = level128(p[19]) * post;
        kDry_ = (post - kWet_) * kPre_ * post;
        fType_ = std::clamp(p[20], 0, 1), fThr_ = std::clamp(p[17], 1, 10);
        break;
      }
      case 2:
        sc_.set(static_cast<float>(p[21]) / 128.f, 1.f);
        scPost_ = level128(p[22]);
        break;
      case 14:
      case 15:
        asym_.set(static_cast<float>(p[algo == 14 ? 8 : 12] / 5000.0 - 1), static_cast<float>(p[algo == 14 ? 8 : 12] / 5000.0 - 1));
        xover_.set(static_cast<float>(p[algo == 14 ? 7 : 11] / 5000.0 - 1), static_cast<float>(p[algo == 14 ? 7 : 11] / 5000.0 - 1));
        break;
      case 16:
        if (p[33] == 1) noiseBp_.design(2, 20.0 * std::pow(2.0, p[32] / 1000.0), 0.505, fs_);
        break;
      default:
        if (algo >= 3 && algo <= 13) setupShape(algo - 2);
        break;
    }
  }
  void setupShape(int shape) {
    const int* p = st_.p;
    const double pre = p[23], asym = p[24];
    double drive = 0, div = 1, d;
    switch (shape) {
      case 4: d = std::pow(2.0, 0.01 * pre - 2), drive = kPi * d, div = kPi * std::pow(std::tanh(4 * d), 3); break;
      case 5: d = std::pow(2.0, 0.025 * pre - 5), drive = d, div = std::log(d + 1) * (1 + 0.001 * pre); break;
      case 6: d = std::pow(2.0, 0.005 * pre - 3), drive = kPi * d, div = kPi * std::tanh(d); break;
      case 7: {
        const double e = kPi * std::pow(2.0, 0.002 * pre - 1);
        drive = e * e, div = e;
        break;
      }
      case 9: drive = kPi * std::pow(2.0, 7 - 0.007 * pre); break;
      case 10: drive = std::pow(2.0, 9 - 0.007 * pre), div = drive; break;
      case 11: {
        const double K = 1024 * std::pow(2.0, 0.02 * asym);
        drive = K, div = K;
        bjQ_ = static_cast<float>(std::pow(2.0, 10 - 0.007 * pre) / std::log(K + 1));
        bjInvQ_ = 1.f / bjQ_;
        break;
      }
      default: d = std::pow(2.0, 0.01 * pre - 4), drive = kPi * d, div = kPi * std::tanh(d); break;  // 1, 2, 3, 8
    }
    const double a = 0.002 * asym;
    double kAsym = std::pow(2.0, 0.004 * asym);
    if (shape == 9) div = drive * kAsym;
    const double W = 0.001 * p[25], Wp = W == 1 ? 1.0 : (std::pow(3.0, W) - 1) / 2;
    const double dry = 0.001 * p[26];
    const double wet = Wp * (1 - std::fabs(dry)) / div;
    shape_ = shape;
    drive_.set(static_cast<float>(drive), static_cast<float>(drive));
    wet_.set(static_cast<float>(wet), static_cast<float>(wet));
    dry_.set(static_cast<float>(dry), static_cast<float>(dry));
    if (shape == 9) {
      asym_.set(static_cast<float>(kAsym), static_cast<float>(kAsym));
    } else if (shape != 11) {
      const double v = shape >= 10 ? a / drive : shape == 8 ? a * a * drive : 0.75 * a * a;
      if (a > 0) asym_.set(static_cast<float>(v), static_cast<float>(v));
      else asym_.set(static_cast<float>(v), static_cast<float>(-v));
    }
    asymAdd_ = shape != 8 && shape != 9 && shape != 11;
    // Post filter: two RBJ low-passes (1100 = off).
    postOn_ = p[27] < 1100;
    if (postOn_) {
      double f = 16.3516 * std::pow(2.0, p[27] / 100.0);
      f = std::min(f, os_ > 0 ? 0.4999 * fs_ : 0.4999 * fsHost_ - 45);
      for (auto& b : post_) b.lowpass(f, fs_, 1 / std::sqrt(2.0));
    }
  }
  float shapeFn(int c, float x) const {
    const float s = std::signbit(x) ? -1.f : 1.f;
    switch (shape_) {
      case 1: return std::tanh(x);
      case 2: return std::exp(std::tanh(x)) - 1.f;
      case 3: return std::tanh(std::exp(x) - 1.f);
      case 4: {
        const float t = std::tanh(x);
        return t * t * t;
      }
      case 5: return s * std::log(1.f + std::fabs(x));
      case 6: return std::cbrt(std::tanh(x * x * x));
      case 7: return s * std::sin(std::sqrt(std::fabs(x)));
      case 8: {
        const float o = asym_.cur[c];
        return std::tanh(x) * std::tanh(x + o) * std::tanh(x + 2 * o);
      }
      case 9: return std::sin(x) + x * asym_.cur[c];
      case 10: return std::nearbyint(x);
      default: return s * std::expm1(std::nearbyint(bjQ_ * std::log(1.f + std::fabs(x))) * bjInvQ_);
    }
  }
  void distort(float* x) {
    const int algo = std::clamp(st_.algo, 0, 17);
    const int* p = st_.p;
    switch (algo) {
      case 0: bloodOverdrive(x); return;
      case 1: {
        const auto& T = fastTables();
        const size_t base = static_cast<size_t>((fType_ * 10 + fThr_ - 1) * 8192);
        for (int c = 0; c < 2; ++c) {
          int i = static_cast<int>(std::nearbyint(x[c] * kPre_ * 8191.0));
          i = std::clamp(i, -8191, 8191);
          const float t = (i < 0 ? -1.f : 1.f) * static_cast<float>(T[base + static_cast<size_t>(std::abs(i))]);
          x[c] = x[c] * kDry_ + t * kWet_ / 32768.f;
        }
        return;
      }
      case 2:
        for (int c = 0; c < 2; ++c) {
          x[c] = sc_.run(x[c]);
          if (scPost_ != 1.f) x[c] *= scPost_;
        }
        return;
      case 14:
      case 15: {
        asym_.tick(), xover_.tick();
        for (int c = 0; c < 2; ++c) {
          double v = static_cast<double>(x[c]) + asym_.cur[c];
          const double sg = v > 0 ? 1 : v < 0 ? -1 : 0;
          v += sg * xover_.cur[c];
          x[c] = static_cast<float>(algo == 14 ? fold(v) : aperture(v));
        }
        return;
      }
      case 16: abrasive(x); return;
      case 17: crusher(x); return;
      default: {
        drive_.tick(), wet_.tick(), dry_.tick(), asym_.tick();
        for (int c = 0; c < 2; ++c) {
          float v = x[c];
          float D = v * dry_.cur[c];
          if (asymAdd_) v += asym_.cur[c];
          v *= drive_.cur[c];
          D += shapeFn(c, v) * wet_.cur[c];
          if (postOn_) D = post_[1].run(c, post_[0].run(c, D));
          x[c] = D;
        }
        (void)p;
        return;
      }
    }
  }
  double fold(double x) const {
    const double g = 1 + st_.p[6] / 500.0, T = 0.1 + 0.9 * st_.p[9] / 10000.0;
    if (!(T > 0 && T <= 1) || g == 0) return 0;
    const double v = g * x;
    double u = std::fabs(v) / (4 * T);
    u -= std::trunc(u);
    const double t = u < 0.25 ? 4 * u : u < 0.75 ? 1 - 4 * (u - 0.25) : 4 * (u - 0.75) - 1;
    return (v < 0 ? -1 : v > 0 ? 1 : 0) * T * t;
  }
  double aperture(double x) const {
    const double g = 1 + st_.p[10] / 500.0, T = 0.5 + 0.5 * st_.p[13] / 10000.0, W = 2 * st_.p[14] / 10000.0 - 1;
    if (!(T > 0) || g == 0) return 0;
    double v = g * x;
    if (st_.p[15] == 0) {
      if (std::fabs(v) > T) v = std::fmod(v, T) - T - W;
      return v;
    }
    int k = 0;
    while (std::fabs(v) > T && k < 8) {
      const double sg = v > 0 ? 1 : v < 0 ? -1 : 0;
      v = sg * (std::fabs(v) - (T + W));
      ++k;
    }
    if (std::fabs(v) > T) v = (v > 0 ? 1 : v < 0 ? -1 : 0) * T;
    return v;
  }
  void abrasive(float* x) {
    const int* p = st_.p;
    const float g = static_cast<float>(1 + p[28] / 5000.0), G = static_cast<float>(p[29] / 10000.0), w = static_cast<float>(p[30] / 10000.0),
                post = static_cast<float>(1 + p[31] / 5000.0);
    float eta = 0;
    if (w != 0) {
      eta = static_cast<float>(2.0 * (static_cast<double>(mt_()) / 4294967296.0) - 1.0);
      if (p[33] == 1) eta = noiseBp_.run(0, eta);
    }
    for (int c = 0; c < 2; ++c) {
      const float raw = x[c], v = g * raw;
      float y;
      if (w != 0) {
        const float gate = std::fabs(raw) > G ? (raw > 0 ? 1.f : raw < 0 ? -1.f : 0.f) : 0.f;
        y = (w * v * eta * gate + (1 - w) * v) * post;
      } else {
        y = v * post;
      }
      x[c] = (y < 0 ? -1.f : 1.f) * std::min(std::fabs(y), 1.f);
    }
  }
  void crusher(float* x) {
    const float r = static_cast<float>(std::pow(st_.p[34] / 10000.0, 3)), b = static_cast<float>(std::pow(st_.p[35] / 10000.0, 2));
    for (int c = 0; c < 2; ++c) {
      float y = x[c];
      if (r > 0) {
        const float u = 127 * r;
        const float P = std::max(1.f, (1 + 0.7435f * u - 0.0171f * u * u + 0.0002f * u * u * u) * static_cast<float>(fs_ / 44100.0));
        cnt_[c] += 1;
        if (cnt_[c] >= P) cnt_[c] = std::fmod(cnt_[c], P), held_[c] = y;
        y = held_[c];
      }
      if (b > 0) {
        const float q = std::exp2(2.f + 14.f * (1.f - std::pow(b, 0.25f)));
        y = std::trunc(y * q) / q;
      }
      x[c] = y;
    }
  }
  void bloodOverdrive(float* x) {
    const auto& tab = bodTable();
    auto shp = [&](float v) {
      const int i = static_cast<int>(std::clamp(std::trunc(32767.0f * v), -32768.0f, 32767.0f));
      return tab[static_cast<size_t>(i + 32768)];
    };
    const int* p = st_.p;
    const float pa = static_cast<float>(p[2] / 10000.0), col = static_cast<float>(p[1] / 10000.0), pgN = static_cast<float>(p[3] / 10000.0);
    const bool boost = p[5] >= 1;  // J-92: the x100 boost switch is stored 0 / 1 (was compared as 0..10000: never on)
    const float gT = 1.0f + (boost ? 2500.0f : 25.0f) * pa * pa * pa, bT = 1.0f - static_cast<float>(p[0] / 10000.0);
    const float fT = col * col * col, pgT = pgN * pgN;
    const float a = 0.2f + 0.7f * static_cast<float>(p[4] / 10000.0), ia = 1.0f - a;
    bodG_ = 0.99975f * bodG_ + 0.00025f * gT;
    bodB_ = 0.99975f * bodB_ + 0.00025f * bT;
    bodF_ = 0.99975f * bodF_ + 0.00025f * fT;
    bodPg_ = 0.99975f * bodPg_ + 0.00025f * pgT;
    for (int c = 0; c < 2; ++c) {
      float* st = bod_[c];
      st[0] += bodF_ * st[1];
      const float hp = x[c] - st[0] - st[1];
      st[1] += bodF_ * hp;
      const float sIn = ((hp + st[0]) * bodB_ + st[1]) * bodG_;
      st[2] += 0.25f * st[3];
      const float h2 = sIn - st[2] - st[3];
      st[3] += 0.25f * h2;
      float v = (shp(h2) + shp(st[3]) + shp(st[2])) * bodPg_;
      v = std::clamp(v, -0.99995f, 0.99995f);
      st[4] = a * st[4] + ia * v;
      st[5] = a * st[5] + ia * st[4];
      x[c] = st[5];
    }
  }

  // ---------------------------------------------------------- filter
  void setupFilter() {
    fx_ = static_cast<float>(st_.p[39] / 10000.0), fr_ = static_cast<float>(st_.p[40] / 10000.0);
    smoothC_ = static_cast<float>(1.0 - std::pow(0.1, 1.0 / (0.01 * fs_)));
    if (!smoothPrimed_) sx_ = fx_, sr_ = fr_, smoothPrimed_ = true;
    designFilter(fx_, fr_);
  }
  void designFilter(float x, float r) {
    const int type = std::clamp(st_.ftype, 0, 22);
    const double fc = 20.0 * std::pow(2.0, 10.0 * x);
    const double kq = 1 - 0.99 * r;
    switch (type) {
      case 0: one_.lp(fc, fs_); break;
      case 1: one_.hp(fc, fs_); break;
      case 2: case 8: svf_[0].design(0, fc, kq, fs_); break;
      case 3: case 10: svf_[0].design(1, fc, kq, fs_); break;
      case 4: case 9: svf_[0].design(2, fc, kq, fs_); svf_[0].m1 = 1; break;
      case 5: case 11: svf_[0].design(0, fc, 2 * std::sqrt(kq), fs_), svf_[1].design(0, fc, 2 * std::sqrt(kq), fs_); break;
      case 6: svf_[0].design(1, fc, 2 * std::sqrt(kq), fs_), svf_[1].design(1, fc, 2 * std::sqrt(kq), fs_); break;
      case 7: svf_[0].design(3, fc, 1 - 0.792 * r, fs_); break;
      case 12: svf_[0].design(4, fc, kq, fs_); break;
      case 13: case 14: combD_ = std::min(1 + 0.05 * x * fs_, 32700.0); break;
      case 15: {
        static const double fA[8] = {250, 700, 2500, 3500, 4200, 8500, 10000, 12000};
        static const double fB[8] = {700, 1250, 2500, 3500, 4200, 8500, 10000, 12000};
        static const double fC[8] = {250, 2500, 2700, 3500, 4200, 8500, 10000, 12000};
        const double pv = 3.0 * x;
        int i = static_cast<int>(pv);
        const double ph = pv - i;
        if (i >= 3) i = 0;
        const double* t0 = i == 0 ? fA : i == 1 ? fB : fC;
        const double* t1 = i == 0 ? fB : i == 1 ? fC : fA;
        vowI_ = i;
        const double shift = std::pow(2.0, 1.7 * (r - 0.5));
        for (int k = 0; k < 8; ++k) vow_[k].bandSkirt(std::min(((1 - ph) * t0[k] + ph * t1[k]) * shift, 0.49 * fs_), fs_, 12);
        break;
      }
      case 16: case 17: case 18: {
        static const double qt[5] = {3, 1.6, 0.832, 0.5, 0.35};
        const double qx = 3.9999 * r;
        const int qi = static_cast<int>(qx);
        const double Q = qt[qi] * (1 - (qx - qi)) + qt[std::min(qi + 1, 4)] * (qx - qi);
        phK_ = 1.0 / Q;
        phPos_ = x;
        break;
      }
      case 19: svf_[0].design(6, fc, 1, fs_, 40 * r - 20); break;
      case 20: svf_[0].design(7, fc, 1, fs_, 40 * r - 20); break;
      case 21: svf_[0].design(8, fc, kq, fs_); break;
      default: break;
    }
  }
  void filter(float* x) {
    const int type = std::clamp(st_.ftype, 0, 22);
    if (st_.fopt) {  // per-sample smoothing of cutoff and resonance, coefficients every sample
      sx_ += (fx_ - sx_) * smoothC_;
      if (std::fabs(fx_ - sx_) < 1e-4f) sx_ = fx_;
      sr_ += (fr_ - sr_) * smoothC_;
      if (std::fabs(fr_ - sr_) < 1e-4f) sr_ = fr_;
      designFilter(sx_, sr_);
    }
    const float rr = st_.fopt ? sr_ : fr_;
    for (int c = 0; c < 2; ++c) {
      float v = x[c];
      switch (type) {
        case 0: case 1: v = one_.run(c, v); break;
        case 5: case 6: case 11: v = svf_[1].run(c, svf_[0].run(c, v)); break;
        case 13: {  // comb +: feed-forward
          comb_[(combW_ + static_cast<uint32_t>(c)) & combMask_] = v;
          v = v + 0.95f * rr * combRead(c, 2);
          break;
        }
        case 14: {  // comb −: feedback
          const float y = v + 0.95f * rr * combRead(c, 0);
          comb_[(combW_ + static_cast<uint32_t>(c)) & combMask_] = y;
          v = y;
          break;
        }
        case 15: {
          float sum = 0;
          for (auto& b : vow_) sum += b.run(c, v);
          v = 0.5f * (1 - 0.125f * static_cast<float>(vowI_)) * sum;
          break;
        }
        case 16: case 17: case 18: v = phaser(c, v); break;
        case 22: break;
        default: v = svf_[0].run(c, v); break;
      }
      x[c] = v;
    }
    if (type == 13 || type == 14) combW_ += 2;
    if (type >= 16 && type <= 18) {
      phPhase_ += 1.0 / (576.29 * fs_);
      phPhase_ -= std::floor(phPhase_);
    }
  }
  // Interleaved comb ring: channel c at even/odd slots.
  // ahead = 2 once this sample is written (feed-forward), 0 before (feedback).
  float combRead(int c, uint32_t ahead) const {
    const double D = combD_;
    const uint32_t di = static_cast<uint32_t>(D), w = combW_ + ahead;
    const float ph = static_cast<float>(D - di);
    const float a = comb_[(w - 2 * di + static_cast<uint32_t>(c)) & combMask_], b = comb_[(w - 2 * (di + 1) + static_cast<uint32_t>(c)) & combMask_];
    return (1 - ph) * a + ph * b;
  }
  float phaser(int c, float x) {
    const int type = st_.ftype;
    // mode tables: outTap, fb, depth, fbTap, hp
    static const int outTap[3] = {3, 2, 5}, fbTap[3] = {4, 3, 5};
    static const double fb[3] = {-0.728, 0.728, -0.728}, depth[3] = {0, 0.2125, 0}, hpN[3] = {0.19, 0.19, 0};
    const int m = type - 16;
    const double ph = phPhase_, tri = ph < 0.5 ? 2 * ph : 2 - 2 * ph;
    const double mm = phPos_ + (1 - phPos_) * tri * depth[m];
    const double fc = std::min(28.5 * std::pow(2.0, 10.020833 * 0.925 * mm), 22000.0);
    Svf ap;
    ap.design(8, std::min(fc, 0.49 * fs_), phK_, fs_);
    if (c == 0) phHp_.hp(10 + 990 * hpN[m] * hpN[m] * hpN[m], fs_);
    const int N = std::max(4, fbTap[m] + 1);
    float u = x + static_cast<float>(fb[m]) * phZ_[c];
    float wet = 0;
    for (int s = 0; s < N; ++s) {
      ap.ic1[0] = phS_[c][s][0], ap.ic2[0] = phS_[c][s][1];
      u = ap.run(0, u);
      phS_[c][s][0] = ap.ic1[0], phS_[c][s][1] = ap.ic2[0];
      if (s == outTap[m]) wet = u;
      if (s == fbTap[m]) phZ_[c] = u;
    }
    return phHp_.run(c, wet);  // mix 1 in every mode
  }

  // ---------------------------------------------------------- chorus (stereo delay reference, fully wet)
  void setupChorus(bool rate) {
    if (rate || !d3_.ready()) d3_.init(fsHost_);
    static const float base[26] = {1, 0.33f, 0, 1, 0.0678f, 0.84f, 0.5f, 1, 0.5f, 0.5f, 0, 0, 0, 1, 0.16f, 0, 1, 0, 0, 0, 0, 1, 1, 1, 0.5f, 0};
    for (int k = 0; k < 26; ++k) d3n_[k] = base[k];
    const int* p = st_.p;
    d3n_[d3ref::PTime] = static_cast<float>(p[43] / 768.0);
    d3n_[d3ref::PFeedback] = static_cast<float>(p[44] / 6000.0);
    d3n_[d3ref::PModRate] = static_cast<float>(p[45] / 6000.0);
    d3n_[d3ref::PModTime] = static_cast<float>(p[46] / 6000.0);
    d3n_[d3ref::PDifLevel] = d3n_[d3ref::PDifSpread] = static_cast<float>(p[47] / 6000.0);
    d3n_[d3ref::PTone] = static_cast<float>((p[48] + 3000) / 6000.0);
    const int mode = std::clamp(st_.chorusMode, 0, 2);
    d3n_[d3ref::PType] = mode == 0 ? 0.33f : mode == 1 ? 0.f : 0.66f;
    d3n_[d3ref::POffset] = mode == 0 ? 0.84f : 0.5f;
    for (float& v : d3n_) v = std::clamp(v, 0.f, 1.f);
  }
  void chorus(float* x) { d3_.process(d3n_, 120.0, &x[0], &x[1], 1); }

  // ---------------------------------------------------------- speaker (procedural cabinet, approximate)
  void setupSpeaker() {
    // Body / presence / high-cut voicing from the impulse family (j35 §6.1), Gridline's own.
    const int ir = st_.ir;
    const bool dark = ir == 1 || ir == 4 || ir == 28 || ir == 35 || ir == 37 || ir >= 51;
    const double hc = dark ? 1500.0 : ir == 3 ? 9000.0 : 5500.0;
    spk_[0].lowpass(std::min(hc, 0.45 * fsHost_), fsHost_, 0.7071);
    spk_[1].lowpass(std::min(hc * 1.2, 0.45 * fsHost_), fsHost_, 0.6);
    spkHp_.hp(90.0, fsHost_);
    auto sep = [](int p, float* m) {
      const double s = p * 0.001, sq = s > 0 ? (std::pow(5.0, s) - 1) / 4 : s, c = 0.5 * std::min(sq, 1.0);
      m[0] = m[1] = static_cast<float>(1 - c), m[2] = m[3] = static_cast<float>(c);
    };
    sep(st_.p[41], mIn_), sep(st_.p[42], mOut_);
  }
  void speaker(float* x) {
    float a = x[0] * mIn_[0] + x[1] * mIn_[2], b = x[1] * mIn_[1] + x[0] * mIn_[3];
    if (cabK_ && cabRun_) {  // J-42: the impulse file; output matrix at 44100/fs, then the table gain
      float w[2] = {a, b};
      cabRun_->tick(*cabK_, w);
      const float v = static_cast<float>(44100.0 / fsHost_), g = static_cast<float>(cabK_->gain);
      x[0] = (w[0] * mOut_[0] * v + w[1] * mOut_[2] * v) * g;
      x[1] = (w[1] * mOut_[1] * v + w[0] * mOut_[3] * v) * g;
      return;
    }
    float v[2] = {a, b};
    for (int c = 0; c < 2; ++c) v[c] = spk_[1].run(c, spk_[0].run(c, spkHp_.run(c, v[c])));
    x[0] = v[0] * mOut_[0] + v[1] * mOut_[2];
    x[1] = v[1] * mOut_[1] + v[0] * mOut_[3];
  }

  ModuleState st_;
  int os_ = 0;
  double fsHost_ = 0, fs_ = 44100;
  HbStage up_[4], dn_[4];
  DcRemover dc_;
  Ramp asym_, xover_, drive_, dry_, wet_;
  // distortion
  float kPre_ = 1, kWet_ = 1, kDry_ = 0, scPost_ = 1, bjQ_ = 1, bjInvQ_ = 1;
  int fType_ = 0, fThr_ = 10, shape_ = 1;
  bool asymAdd_ = true, postOn_ = false;
  SoftClip sc_;
  Biquad post_[2];
  float bod_[2][6] = {}, bodG_ = 0, bodB_ = 0, bodF_ = 0, bodPg_ = 0;
  std::mt19937 mt_{19650218u};
  Svf noiseBp_;
  float held_[2] = {0, 0}, cnt_[2] = {0, 0};
  // filter
  float fx_ = 1, fr_ = 0, sx_ = 1, sr_ = 0, smoothC_ = 0;
  bool smoothPrimed_ = false;
  Svf svf_[2];
  OnePole one_, phHp_;
  Biquad vow_[8];
  int vowI_ = 0;
  std::vector<float> comb_;
  uint32_t combMask_ = 0, combW_ = 0;
  double combD_ = 1, phK_ = 1, phPos_ = 0, phPhase_ = 0;
  float phZ_[2] = {0, 0}, phS_[2][6][2] = {};
  // chorus
  d3ref::Engine d3_;
  float d3n_[26] = {};
  // speaker
 public:
  const cab::Kernel* cabK_ = nullptr;
  cab::Run* cabRun_ = nullptr;

 private:
  Biquad spk_[2];
  OnePole spkHp_;
  float mIn_[4] = {1, 1, 0, 0}, mOut_[4] = {1, 1, 0, 0};
};

// ---------------------------------------------------------------- the rack
class Rack {
 public:
  // J-42: impulse kernels / convolver states per module (null = procedural cabinet).
  void setCabinets(const cab::Aux* aux, cab::Run* const* runs) {
    for (int k = 0; k < kModules; ++k) {
      const cab::Kernel* kk = aux ? aux->k[k].get() : nullptr;
      if (kk != mod_[k].cabK_ && runs && runs[k]) runs[k]->clear();
      mod_[k].cabK_ = kk, mod_[k].cabRun_ = runs ? runs[k] : nullptr;
    }
  }
  void process(const RackState& s, double fs, float* L, float* R, int n) {
    const bool first = !ready_ || fs != fs_;
    for (int k = 0; k < kModules; ++k)
      if (first || k >= cfgCount_ || !(s.m[k] == cfg_.m[k])) mod_[k].configure(s.m[k], fs, first || k >= cfgCount_);
    cfg_ = s, cfgCount_ = kModules, fs_ = fs, ready_ = true;
    clip_.set(0.78125f, 1.f);
    const int count = std::clamp(s.count, 0, kModules);
    for (int i = 0; i < n; ++i) {
      float l = L[i], r = R[i];
      for (int k = 0; k < count; ++k) mod_[k].tick(l, r);
      if (s.softClip) l = clip_.run(l), r = clip_.run(r);
      L[i] = l, R[i] = r;
    }
  }
  void reset() { ready_ = false; }

 private:
  Module mod_[kModules];
  RackState cfg_;
  int cfgCount_ = 0;
  double fs_ = 0;
  bool ready_ = false;
  SoftClip clip_;
};

}  // namespace gl::packs::dynamics::drack
