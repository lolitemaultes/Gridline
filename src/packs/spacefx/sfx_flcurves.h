// Gridline — "spacefx" pack: control curves of the J-1 reference models
// (Bucket Chorus, Retro Phaser, Multi Delay). Header-only: shared by the
// runtimes, the editors and the FL importers. n = a control's normalised
// position 0..1 unless noted.
#pragma once
#include <algorithm>
#include <cmath>

namespace gl::packs::spacefx::flc {

inline double clamp01(double v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }
inline double lin01(double v, double lo, double hi) { return clamp01((v - lo) / (hi - lo)); }

// ------------------------------------------------------------ Bucket Chorus
inline double bcTimeMs(double n) { return 9.8 * clamp01(n) + 0.2; }       // 0.2 .. 10 ms
inline double bcSpeedHz(double n) { n = clamp01(n); return 10.0 * n * n; }  // 0 = frozen LFO
inline double bcHighPassHz(double n) { n = clamp01(n); return 160.0 * n * n + 10.0; }
inline double bcFeedback(double n) { return 0.63 * clamp01(n); }
inline double bcGainDb(double n) { return 12.0 * clamp01(n); }
// Speed control → synced LFO period in ticks of 1/192 beat (1 .. 6144).
inline double bcSyncTicks(double n) {
  n = clamp01(n);
  return std::floor((std::pow(2.0, 7.0 * (1.0 - n)) - 1.0) * (128.0 / 127.0) * 48.0 * 6143.0 / 6144.0 + 1.1);
}
// Hiss control → level (linear full scale); 0 = off.
inline double bcHissLevel(double n) { return n <= 0 ? 0.0 : std::pow(10.0, (50.0 * clamp01(n) - 90.0) / 20.0); }
// Fixed modes I, II, I+II: rate (Hz), wave (0 sine, 1 triangle), delay range (ms), L/R phase (cycles).
struct BcFixed {
  double rate;
  int wave;
  double t1, t2, lr;
};
inline BcFixed bcFixedMode(int mode) {
  switch (mode) {
    case 1: return {0.6, 1, 1.08, 5.23, 0.5};
    case 2: return {8.0, 0, 2.85, 3.37, 0.028611};
    default: return {0.4, 1, 1.08, 5.23, 0.5};
  }
}
// Reference LFO shapes (0..1): sine 0.5 + 0.5 sin, triangle starting at 0.5 rising.
inline double refSine(double ph) { return 0.5 + 0.5 * std::sin(6.283185307179586 * (ph - std::floor(ph))); }
inline double refTri(double ph) {
  ph -= std::floor(ph);
  return ph < 0.25 ? 2 * ph + 0.5 : (ph < 0.75 ? 1.5 - 2 * ph : 2 * ph - 1.5);
}

// ------------------------------------------------------------ Retro Phaser
inline double vpFreqHz(double n) { return 1.25 * std::pow(2.0, 14.0 * clamp01(n)); }  // 1.25 Hz .. 20.48 kHz
inline double vpFreqToN(double hz) { return clamp01(std::log2(std::max(1e-9, hz / 1.25)) / 14.0); }
inline double vpRateRangeHz(double n) { n = clamp01(n); return n * n * n * n * 149.975 + 0.025; }
inline double vpSyncTicks(double n) { return std::floor(std::pow(2.0, 7.0 - 7.0 * clamp01(n)) * 48.37008 - 47.27008); }
inline double vpTensionK(double n) { return 0.1 * (std::exp(8.755737 * clamp01(n)) - 1.0); }
// Tension bend of an LFO value x (0..1).
inline double vpBend(double x, double t) {
  if (t < 1e-9) return x;
  return 1.0 + (1.0 - std::pow(1.0 + t, 1.0 - x)) / t;
}
inline double vpEchoLpHz(double n) { return 320.0 * std::pow(2.0, 6.0 * clamp01(n)); }
inline double vpEchoHpHz(double n) { return 20.0 * std::pow(2.0, 10.0 * clamp01(n)); }
inline double vpHpHz(double n) { n = clamp01(n); return 195.0 * n * n * n + 5.0; }
inline double vpNoiseAmp(double n) { return n <= 0 ? 0.0 : 0.35 * std::pow(10.0, (50.0 * clamp01(n) - 90.0) / 20.0); }
// Speed-dependent sweep: exponent position (0..1 of the min..max octave span)
// for a bent LFO value y, speed position s, depth scale S and pull-down P.
inline double vpSweepPos(double y, double s, double S, double P) {
  double span = (1.0 - s) * (1.0 - S) + S;
  double off = (1.0 - span) - s * s * s * s * P;
  return off + y * span;
}

// ------------------------------------------------------------ Multi Delay (Delay Bank)
// Level law: sign(x) · (11^|x| − 1) / 10 (1 at |x| = 1), x = control / 128.
inline double mdGain(double x) {
  double g = (std::pow(11.0, std::fabs(x)) - 1.0) / 10.0;
  return x < 0 ? -g : g;
}
// Filter type order of the reference model (see MdFilterType).
// Cutoff control c (0..1) → Hz at the reference scale (J-35: 44100 Hz per unit, spec j35_delaybank.md §1).
inline double mdCutHz(int type, double c) {
  c = clamp01(c);
  if (type == 1) {  // low pass
    double f = c > 0 ? (std::exp(c * 7.313886831633462) - 1.0) * 0.0006646666666666667 : 0.0;
    return 44100.0 * (f + 0.003);
  }
  double f = c > 0 ? (std::exp(c * 7.313886831633462) - 1.0) * 0.0003999333333333333 : 0.0;
  f = std::min(f, type == 2 ? 0.6001 : 1.0001);  // high pass is capped lower
  return 44100.0 * (f + 0.0001);
}
// J-35: the reference's 2×2 pan matrix for a pan p (−1..1) and gain g:
// constant-power sin / cos scaled by √2 (0 dB at the centre), the far side
// folded into the near one. m = {LL, RR, RL, LR}: outL = LL·L + RL·R,
// outR = RR·R + LR·L. sep ≠ 0 (feedback matrix only) adds the separation
// cross term: s > 0 → (5^s − 1)/4, s < 0 → s (widening).
inline void mdPanMatrix(double p, double g, double sep, bool fbMatrix, double* m) {
  if (p == 0 && (!fbMatrix || sep == 0)) {
    m[0] = m[1] = g, m[2] = m[3] = 0;
    return;
  }
  if (g == 0) {
    m[0] = m[1] = m[2] = m[3] = 0;
    return;
  }
  const double th = (1.0 + p) * 0.7853981633974483, k = g * static_cast<double>(1.4142135f);
  const double L = std::cos(th) * k, R = std::sin(th) * k;
  double cl = 0, cr = 0;
  if (fbMatrix) {
    if (p > 0) cr = 1.0 - L / g;
    else cl = 1.0 - R / g;
    if (sep != 0) {
      const double t = sep > 0 ? static_cast<double>(static_cast<float>(std::exp(sep * 1.6094379124341003) - 1.0)) * 0.25 : sep;
      cl = std::min(cl + t, 1.0), cr = std::min(cr + t, 1.0);
    }
  } else {
    if (p > 0) cr = 1.0 - L / g;
    else cl = 1.0 - R / g;
  }
  m[2] = L * (cl / 2), m[0] = L * (1 - cl / 2);
  m[3] = R * (cr / 2), m[1] = R * (1 - cr / 2);
}
// Resonance control r (0..1) → Q (LP / HP), bandwidth in octaves (BP / notch),
// Q (peaking, from 1 − r) or shelf slope (shelves, from 1 − r).
inline double mdQ(double r) { return r <= 0 ? 0.1 : (r >= 1 ? 50.0 : (std::exp(r * 7.601402334583733) - 1.0) * 0.02495 + 0.1); }
inline double mdBw(double r) { return r <= 0 ? 0.01 : (r >= 1 ? 12.0 : (std::exp(r * 4.61512051684126) - 1.0) * 0.1199 + 0.01); }
inline double mdPeakQ(double r) {
  r = 1.0 - r;
  return r <= 0 ? 0.1 : (r >= 1 ? 100.0 : (std::exp(r * 7.601402334583733) - 1.0) * 0.04995 + 0.1);
}
inline double mdShelfS(double r) {
  r = 1.0 - r;
  return r <= 0 ? 0.1 : (r >= 1 ? 1.0 : (std::exp(r * 1.0986122886681098) - 1.0) * 0.45 + 0.1);
}
// Filter gain control (0..256, 128 = unity) → shelf / peak amplitude A (0.1 .. 2).
inline double mdFilterA(double v) { return (std::exp(v / 128.0 * 0.10536056334154098) - 1.0) * 8.1 + 0.1; }
// Stereo time offset control (−512..512) → ms (± 500, positive = right later).
inline double mdOffsetMs(double v) {
  v = std::clamp(v, -512.0, 512.0);
  double ms = (std::pow(1001.0, std::fabs(v) / 512.0) - 1.0) * 0.5;
  return v < 0 ? -ms : ms;
}
// One biquad stage (a0 = 1) of a reference unit filter.
struct Bq {
  double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
};
// Designs one of `stages` identical stages: type (MdFilterType order), cutoff
// Hz, resonance control r (0..1), shelf / peak amplitude A. Cascades take the
// n-th root of Q (LP / HP) and of A, and scale the band-pass / notch width.
inline Bq mdDesign(int type, double hz, double r, double A, int stages, double sr) {
  Bq q;
  if (type <= 0 || type > 7) return q;
  const int n = std::clamp(stages, 1, 3);
  double w = std::clamp(6.283185307179586 * hz / sr, 0.0, 2.849517146113191);
  w = std::max(w, 1e-6);
  const double sn = std::sin(w), cs = std::cos(w);
  double a0 = 1, b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
  if (type == 1 || type == 2) {
    double Q = mdQ(r);
    if (n > 1) Q = std::pow(Q, 1.0 / n);
    double al = sn * 0.5 / Q;
    a0 = 1 + al, a1 = -2 * cs, a2 = 1 - al;
    if (type == 1) b0 = b2 = (1 - cs) * 0.5, b1 = 1 - cs;
    else b0 = b2 = (1 + cs) * 0.5, b1 = -(1 + cs);
  } else if (type == 3 || type == 4) {
    double bw = mdBw(r) * (type == 3 ? n : 1.0 / n);
    double al = sn * std::sinh(bw * w / sn);
    a0 = 1 + al, a1 = -2 * cs, a2 = 1 - al;
    if (type == 3) b0 = al, b1 = 0, b2 = -al;
    else b0 = 1, b1 = -2 * cs, b2 = 1;
  } else if (type == 6) {
    double a = n > 1 ? std::pow(A, 1.0 / n) : A;
    double al = sn / (2.0 * mdPeakQ(r));
    a0 = 1 + al / a, a1 = -2 * cs, a2 = 1 - al / a;
    b0 = 1 + al * a, b1 = -2 * cs, b2 = 1 - al * a;
  } else {  // shelves (RBJ, slope S)
    double a = n > 1 ? std::pow(A, 1.0 / n) : A;
    double S = mdShelfS(r);
    double t = sn * std::sqrt(std::max(0.0, (1.0 / S - 1.0) * (1.0 / a + a) + 2.0)) * std::sqrt(a);  // 2·√A·α
    if (type == 5) {
      b0 = a * ((a + 1) - (a - 1) * cs + t), b1 = 2 * a * ((a - 1) - (a + 1) * cs), b2 = a * ((a + 1) - (a - 1) * cs - t);
      a0 = (a + 1) + (a - 1) * cs + t, a1 = -2 * ((a - 1) + (a + 1) * cs), a2 = (a + 1) + (a - 1) * cs - t;
    } else {
      b0 = a * ((a + 1) + (a - 1) * cs + t), b1 = -2 * a * ((a - 1) + (a + 1) * cs), b2 = a * ((a + 1) + (a - 1) * cs - t);
      a0 = (a + 1) - (a - 1) * cs + t, a1 = 2 * ((a - 1) - (a + 1) * cs), a2 = (a + 1) - (a - 1) * cs - t;
    }
  }
  q.b0 = b0 / a0, q.b1 = b1 / a0, q.b2 = b2 / a0, q.a1 = a1 / a0, q.a2 = a2 / a0;
  return q;
}
// Magnitude (dB) of `stages` cascaded copies of a stage at hz.
inline double bqMagDb(const Bq& q, int stages, double hz, double sr) {
  double w = 6.283185307179586 * hz / sr, c1 = std::cos(w), s1 = std::sin(w), c2 = std::cos(2 * w), s2 = std::sin(2 * w);
  double nr = q.b0 + q.b1 * c1 + q.b2 * c2, ni = -(q.b1 * s1 + q.b2 * s2);
  double dr = 1 + q.a1 * c1 + q.a2 * c2, di = -(q.a1 * s1 + q.a2 * s2);
  double m = std::sqrt((nr * nr + ni * ni) / std::max(1e-30, dr * dr + di * di));
  return 20.0 * std::log10(std::max(1e-12, m)) * std::clamp(stages, 1, 3);
}

// Grain window: Hann over `len` samples (rise to the middle, fall after),
// bent by shape s (−1..1): k = 10000^|s| − 1, rise y = ((1+k)^h − 1)/k.
inline double mdGrainWindow(int i, int len, double s) {
  int last = len - 1, half = last / 2;
  if (last < 1 || half < 1) return 1.0;
  double a = i < half ? i * 3.141592653589793 / half : (i - half) * 3.141592653589793 / std::max(1, last - half) + 3.141592653589793;
  double h = 0.5 - 0.5 * std::cos(a);
  double k = std::pow(10000.0, std::fabs(s)) - 1.0;
  if (k < 1e-9) return h;
  if (s > 0) return 1.0 - (std::pow(1.0 + k, 1.0 - h) - 1.0) / k;
  return (std::pow(1.0 + k, h) - 1.0) / k;
}

}  // namespace gl::packs::spacefx::flc
