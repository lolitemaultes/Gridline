// Gridline — Octa Filter reference filter cores (J-42, OfModel = 1).
// Spec: behaviour study notes (§3, §4, §8) + J-42 notes.
// Per unit and control tick a Design is computed from the unit's cutoff /
// resonance / gain values; per sample the stage kernels run with linearly
// ramped coefficients. Everything is fixed-size (no allocation).
#pragma once
#include <algorithm>
#include <cmath>
#include "creative.h"

namespace gl::packs::creative::ref {

inline constexpr double kPi = 3.14159265358979323846;
inline constexpr double kLn2 = 0.69314718055994530942;

// Stage parameters (per stage; meaning depends on the type).
struct StageP {
  // SVF: f, q, gl, gb, gh. Lime: a, d. Mango: g, fb, G.
  double v[5] = {0, 1, 0, 0, 0};
  // Biquad (a0-normalised).
  double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
};

enum Kind { KSvf, KLime, KBiquad, KMango, KOff };
inline Kind kindOf(int type) {
  switch (type) {
    case FtSvf: return KSvf;
    case FtLp2:
    case FtHp2: return KLime;
    case FtLadder: return KMango;
    default: return type >= 0 && type < FilterTypeCount ? KBiquad : KOff;
  }
}

struct Design {
  int type = 0, n = 0;  // n = stages (0 = off)
  Kind kind = KOff;
  double drive = 0;     // Mango D
  StageP st[3];
};

// Stages and stagger of a mode (0 off, 1..3 single..triple, 4 alt dual, 5 alt triple).
inline int stagesOf(int mode) { return mode <= 0 ? 0 : mode <= 3 ? mode : mode - 2; }

// Normalised cutoff of a type (c ≤ 0 → 0).
inline double cutoff(int type, double c) {
  if (c <= 0) return 0;
  switch (type) {
    case FtSvf: return 0.0159 * (std::pow(51.0, c) - 1.0);
    case FtLp2:
    case FtHp2: return std::min(1.6, 0.01324 * (std::pow(76.0, c) - 1.0));
    case FtLpRbj: return 0.000664667 * (std::pow(1500.0, c) - 1.0);
    case FtHpRbj: return std::min(0.6001, 0.00039993 * (std::pow(1500.0, c) - 1.0));
    case FtLadder: return std::min(3.0, 0.01495 * (std::pow(201.0, c) - 1.0));
    default: return std::min(1.0001, 0.00039993 * (std::pow(1500.0, c) - 1.0));
  }
}

inline void rbj(StageP& s, int mode, double w, double alpha, double A) {
  const double cw = std::cos(w);
  double b0, b1, b2, a0, a1, a2;
  switch (mode) {
    case 0: b0 = (1 - cw) / 2, b1 = 1 - cw, b2 = (1 - cw) / 2, a0 = 1 + alpha, a1 = -2 * cw, a2 = 1 - alpha; break;   // LP
    case 1: b0 = alpha, b1 = 0, b2 = -alpha, a0 = 1 + alpha, a1 = -2 * cw, a2 = 1 - alpha; break;                        // BP
    case 2: b0 = 1, b1 = -2 * cw, b2 = 1, a0 = 1 + alpha, a1 = -2 * cw, a2 = 1 - alpha; break;                           // notch
    case 3: b0 = (1 + cw) / 2, b1 = -(1 + cw), b2 = (1 + cw) / 2, a0 = 1 + alpha, a1 = -2 * cw, a2 = 1 - alpha; break;  // HP
    case 4: b0 = 1 - alpha, b1 = -2 * cw, b2 = 1 + alpha, a0 = 1 + alpha, a1 = -2 * cw, a2 = 1 - alpha; break;          // all pass
    case 5: b0 = 1 + alpha * A, b1 = -2 * cw, b2 = 1 - alpha * A, a0 = 1 + alpha / A, a1 = -2 * cw, a2 = 1 - alpha / A; break;  // peak
    case 6: {  // low shelf (alpha = 2√A·α of the cookbook)
      b0 = A * ((A + 1) - (A - 1) * cw + alpha), b1 = 2 * A * ((A - 1) - (A + 1) * cw), b2 = A * ((A + 1) - (A - 1) * cw - alpha);
      a0 = (A + 1) + (A - 1) * cw + alpha, a1 = -2 * ((A - 1) + (A + 1) * cw), a2 = (A + 1) + (A - 1) * cw - alpha;
      break;
    }
    default: {  // high shelf
      b0 = A * ((A + 1) + (A - 1) * cw + alpha), b1 = -2 * A * ((A - 1) + (A + 1) * cw), b2 = A * ((A + 1) + (A - 1) * cw - alpha);
      a0 = (A + 1) - (A - 1) * cw + alpha, a1 = 2 * ((A - 1) - (A + 1) * cw), a2 = (A + 1) - (A - 1) * cw - alpha;
      break;
    }
  }
  s.b0 = b0 / a0, s.b1 = b1 / a0, s.b2 = b2 / a0, s.a1 = a1 / a0, s.a2 = a2 / a0;
}

// Inputs of a unit's design for one tick.
struct DesignIn {
  int type = 0, mode = 1;
  bool alt = false;
  double c = 0.5, r = 0.5;          // cutoff / resonance (knob + articulation, 0..1 scale)
  double lowRaw = 128;              // LOW knob raw (0..128) × articulation (shelves / peak / Mango)
  double gLow = 1, gBand = 0, gHigh = 0;  // SVF gains (volume curve × articulation)
  double K = 1;                     // 44100 / internal rate
};

inline Design design(const DesignIn& in) {
  Design d;
  d.type = in.type;
  d.kind = kindOf(in.type);
  d.n = stagesOf(in.mode);
  if (d.kind == KOff || d.n == 0) {
    d.n = 0;
    return d;
  }
  const int n = d.n;
  const bool stag = in.mode >= 4;
  const double e = in.type == FtLadder ? 1.0 / (1.0 + 0.25 * (n - 1)) : 1.0 / n;
  double r = in.r;
  if (in.alt && in.type != FtSvf && in.type != FtLadder) r = 0.5 * (in.c + 1.0) * r;
  double fc = cutoff(in.type, in.c);
  const double K = in.K;
  auto ep = [&](double x) { return n > 1 ? std::pow(x, e) : x; };
  for (int k = n - 1; k >= 0; --k) {
    StageP& s = d.st[k];
    switch (in.type) {
      case FtSvf: {
        const double a = (fc + 0.005) * K;
        s.v[0] = in.c <= 0 ? 0.0 : a <= 0.625 ? 2.0 * std::sin(a) : 1.17019;
        double q = r <= 0 ? 1.0 : r >= 1 ? 0.2 : 1.0 - 0.9 * r;
        if (in.alt) q = q * q;
        s.v[1] = k == 0 ? q : 1.0;
        auto g = [&](double x) { return x <= 0 ? 0.0 : ep(x); };
        s.v[2] = g(in.gLow), s.v[3] = g(in.gBand), s.v[4] = g(in.gHigh);
        break;
      }
      case FtLp2:
      case FtHp2: {
        double Q = r <= 0 ? 1.0 : r >= 1 ? 50.0 : 1.0 + 0.049 * (std::pow(1001.0, r) - 1.0);
        Q = ep(Q);
        const double x = (fc + 0.007) * K * kPi / 2;
        const double cv = x > 0.4 * kPi ? 0.30901699437494745 : std::cos(x);
        s.v[0] = 2.0 - 2.0 * cv;
        s.v[1] = std::max(0.1, 1.0 - std::sqrt(2.0) * std::sqrt(1.0 - cv) / Q);
        break;
      }
      case FtLpRbj:
      case FtHpRbj:
      case FtAllPass: {
        const double off = in.type == FtLpRbj ? 0.003 : 0.0001;
        const double w = std::clamp(2 * kPi * K * (fc + off), 0.0, 2.8495);
        double Q = r <= 0 ? 0.1 : r >= 1 ? 50.0 : 0.1 + 0.02495 * (std::pow(2000.0, r) - 1.0);
        Q = ep(Q);
        rbj(s, in.type == FtLpRbj ? 0 : in.type == FtHpRbj ? 3 : 4, w, std::sin(w) * (0.5 / Q), 1);
        break;
      }
      case FtBp:
      case FtNotch: {
        const double w = std::clamp(2 * kPi * K * (fc + 0.0001), 0.0, 2.8495);
        const double B = r <= 0 ? 0.01 : r >= 1 ? 12.0 : 0.01 + 0.1199 * (std::pow(101.0, r) - 1.0);
        const double wB = (in.type == FtBp ? n : 1.0 / n) * kLn2 / 2;
        const double sw = std::sin(w);
        const double alpha = sw > 0 ? sw * std::sinh(B * wB * w / sw) : 0.0;
        rbj(s, in.type == FtBp ? 1 : 2, w, alpha, 1);
        break;
      }
      case FtLowShelf:
      case FtHighShelf:
      case FtPeak: {
        const double w = std::clamp(2 * kPi * K * (fc + 0.0001), 0.0, 2.8495);
        const double A = ep(0.125 * std::pow(8.0, in.lowRaw / 64.0));
        const double sv = 1.0 - r, sw = std::sin(w);
        if (in.type == FtPeak) {
          const double Q = sv <= 0 ? 0.1 : sv >= 1 ? 100.0 : 0.1 + 0.04995 * (std::pow(2000.0, sv) - 1.0);
          rbj(s, 5, w, sw / (2 * Q), A);
        } else {
          const double S = sv <= 0 ? 0.1 : sv >= 1 ? 1.0 : 0.1 + 0.45 * (std::pow(3.0, sv) - 1.0);
          const double beta = sw * std::sqrt(A) * std::sqrt(std::max(0.0, (A + 1.0 / A) * (1.0 / S - 1.0) + 2.0));
          rbj(s, in.type == FtLowShelf ? 6 : 7, w, beta, A);
        }
        break;
      }
      case FtLadder: {
        const double D = 0.12 * (std::pow(251.0, in.lowRaw / 128.0) - 1.0);
        d.drive = D;
        double kk = 0;
        if (r > 0) {
          const double k0 = 1.9 + 0.6 * std::min(D, 1.0);
          kk = r >= 1 ? k0 : r * k0;
        }
        if (n > 1) kk = std::pow(kk + 1.0, e) - 1.0;
        const double x = std::min(0.65, (fc + 0.01) * K / 2);
        const double g = 1.0 - std::exp(-kPi * x * (0.9988 - 0.649 * x + 0.4955 * x * x + 1.873 * x * x * x));
        const double comp = 0.9968 + 1.8409 * x - 3.9364 * x * x;
        double fb = -kk * comp;
        double G = (1.0 - fb) / 2;
        if (D >= 0.001) {
          const double t = std::tanh(4.0 / D);
          if (in.alt) fb *= t;
          G = 0.5 * (1.0 - fb) / t;
        }
        s.v[0] = g, s.v[1] = fb, s.v[2] = G;
        break;
      }
      default: break;
    }
    if (stag) fc = std::pow(fc, in.mode == 4 ? 0.5 : 0.75);
  }
  return d;
}

// Rational saturator ≈ tanh(u/2).
inline float sat(float u) {
  const float a = std::fabs(u);
  return u * (a * a + 3 * a + 6) / (a * a * a + 3 * a * a + 6 * a + 12);
}

// Per-channel state of one stage.
struct StageS {
  double s0 = 0, s1 = 0, s2 = 0, s3 = 0;  // SVF low/band, Lime p/v, biquad x1 x2 y1 y2
  float m[5] = {0, 0, 0, 0, 0};           // Mango s0..s3, y_prev
};

// One sample through stage k of a unit's design; p = ramped stage parameters.
inline double runStage(const Design& d, const StageP& p, StageS& z, double x) {
  switch (d.kind) {
    case KSvf: {
      const double f = p.v[0], q = p.v[1];
      z.s0 += f * z.s1;
      const double high = x - z.s0 - q * z.s1;
      z.s1 += f * high;
      return p.v[2] * z.s0 + p.v[3] * z.s1 + p.v[4] * high;
    }
    case KLime: {
      z.s1 += p.v[0] * (x - z.s0);
      z.s0 += z.s1;
      const double out = d.type == FtLp2 ? z.s0 : x - z.s0;
      z.s1 *= p.v[1];
      return out;
    }
    case KBiquad: {
      const double y = p.b0 * x + p.b1 * z.s0 + p.b2 * z.s1 - p.a1 * z.s2 - p.a2 * z.s3;
      z.s1 = z.s0, z.s0 = x, z.s3 = z.s2, z.s2 = y;
      return y;
    }
    case KMango: {
      const float g = static_cast<float>(p.v[0]), fb = static_cast<float>(p.v[1]), G = static_cast<float>(p.v[2]);
      const float xin = static_cast<float>(x);
      const float s3old = z.m[3];
      if (d.drive < 0.001) {
        const float u = xin + fb * z.m[4];
        z.m[0] += g * (u - z.m[0]);
        z.m[1] += g * (z.m[0] - z.m[1]);
        z.m[2] += g * (z.m[1] - z.m[2]);
        z.m[3] += g * (z.m[2] - z.m[3]);
      } else {
        const float D = static_cast<float>(d.drive), dl = 2 * D, gD = g / D;
        float in = xin + fb * z.m[4];
        for (int i = 0; i < 4; ++i) {
          z.m[i] += gD * (sat(dl * in) - sat(dl * z.m[i]));
          in = z.m[i];
        }
      }
      const float y = s3old + z.m[3];
      z.m[4] = y;
      return G * y;
    }
    default: return x;
  }
}

// ------------------------------------------------------------- 2x resampler
// Polyphase IIR half-band (two branches of three first-order all-passes,
// y = x1 + c (x - y1), double state). Up: (B, A); down: (A(even) + B(odd)) / 2.
struct Halfband {
  static constexpr double kA[3] = {0.1473771136010466, 0.48246854276970014, 0.8830050257693731};
  static constexpr double kB[3] = {0.039151597734460045, 0.3026468483284934, 0.6746159185469639};
  double ax[3] = {}, ay[3] = {}, bx[3] = {}, by[3] = {};
  static double branch(const double* c, double* x1, double* y1, double x) {
    for (int i = 0; i < 3; ++i) {
      const double y = x1[i] + c[i] * (x - y1[i]);
      x1[i] = x, y1[i] = y, x = y;
    }
    return x;
  }
  void up(double x, double& first, double& second) {
    first = branch(kB, bx, by, x);
    second = branch(kA, ax, ay, x);
  }
  double down(double even, double odd) {
    const double a = branch(kA, ax, ay, even);
    return 0.5 * (branch(kB, bx, by, odd) + a);
  }
};

}  // namespace gl::packs::creative::ref
