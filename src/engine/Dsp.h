// Gridline — DSP building blocks (header-only, realtime-safe, deterministic).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace gl::dsp {

inline constexpr double kPi = 3.14159265358979323846;
inline constexpr double kTwoPi = 2.0 * kPi;

inline double dbToGain(double db) { return std::pow(10.0, db / 20.0); }
inline double gainToDb(double g) { return g > 1e-9 ? 20.0 * std::log10(g) : -180.0; }
inline double noteToHz(double note) { return 440.0 * std::pow(2.0, (note - 69.0) / 12.0); }

// Fader taper: `unity` maps to 0 dB, quadratic below/above.
inline double faderGain(double v, double unity) {
  double x = v / unity;
  return x * x;
}

// Balance pan law: centre is unity on both sides.
inline void panGains(double pan, double& l, double& r) {
  l = pan > 0 ? 1.0 - pan : 1.0;
  r = pan < 0 ? 1.0 + pan : 1.0;
}

// Deterministic white noise (xorshift32).
struct Noise {
  uint32_t s = 0x12345678u;
  void seed(uint32_t v) { s = v ? v : 0x12345678u; }
  float next() {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return static_cast<float>(static_cast<int32_t>(s)) * (1.0f / 2147483648.0f);
  }
};

// PolyBLEP residual for band-limited saw/square.
inline double polyBlep(double t, double dt) {
  if (t < dt) {
    t /= dt;
    return t + t - t * t - 1.0;
  }
  if (t > 1.0 - dt) {
    t = (t - 1.0) / dt;
    return t * t + t + t + 1.0;
  }
  return 0.0;
}

enum class Shape { Sine = 0, Triangle, Saw, Square, Noise, RoundSaw, Pulse25 };

struct Osc {
  double phase = 0.0;  // 0..1
  double triState = 0.0;
  // One sample at frequency `hz` (per-sample increment dt = hz / sr).
  double next(Shape shape, double dt, Noise& noise) {
    double out = 0.0;
    switch (shape) {
      case Shape::Sine: out = std::sin(kTwoPi * phase); break;
      case Shape::Saw: out = 2.0 * phase - 1.0 - polyBlep(phase, dt); break;
      case Shape::Square: {
        out = phase < 0.5 ? 1.0 : -1.0;
        out += polyBlep(phase, dt);
        out -= polyBlep(std::fmod(phase + 0.5, 1.0), dt);
        break;
      }
      case Shape::Triangle: {
        // Integrated band-limited square (leaky).
        double sq = phase < 0.5 ? 1.0 : -1.0;
        sq += polyBlep(phase, dt);
        sq -= polyBlep(std::fmod(phase + 0.5, 1.0), dt);
        triState = dt * 4.0 * sq + (1.0 - dt * 0.05) * triState;
        out = triState;
        break;
      }
      case Shape::Noise: out = noise.next(); break;
      case Shape::RoundSaw: {
        // Capacitor-charge ramp up to p≈0.46, flat top, then low for the last sixth.
        constexpr double kRise = 0.4638671875, kFall = 5.0 / 6.0, kK = 22.834;  // fast charge, flat top, low last sixth
        if (phase < kRise)
          out = -1.0 + 2.0 * (1.0 - std::exp(-kK * phase / kRise)) / (1.0 - std::exp(-kK));
        else
          out = phase < kFall ? 1.0 : -1.0;
        out -= polyBlep(std::fmod(phase + (1.0 - kFall), 1.0), dt);
        break;
      }
      case Shape::Pulse25: {
        out = phase < 0.25 ? 1.0 : -1.0;
        out += polyBlep(phase, dt);
        out -= polyBlep(std::fmod(phase + 0.75, 1.0), dt);
        out += 0.5;  // remove DC of a 25% pulse
        break;
      }
    }
    phase += dt;
    if (phase >= 1.0) phase -= std::floor(phase);
    return out;
  }
};

// Linear-attack / exponential decay-release ADSR, per sample.
struct Adsr {
  enum Stage { Idle, Attack, Decay, Sustain, Release };
  Stage stage = Idle;
  double level = 0.0;
  double atkInc = 0.0, decCoef = 0.0, relCoef = 0.0, sustain = 1.0;

  void set(double sr, double attack, double decay, double sus, double release) {
    atkInc = 1.0 / std::max(1.0, attack * sr);
    decCoef = std::exp(-1.0 / std::max(1.0, decay * sr * 0.25));
    relCoef = std::exp(-1.0 / std::max(1.0, release * sr * 0.25));
    sustain = sus;
  }
  void gate(bool on) {
    if (on)
      stage = Attack;  // restarts from the current level (no click when retriggered)
    else if (stage != Idle)
      stage = Release;
  }
  bool active() const { return stage != Idle; }
  double next() {
    switch (stage) {
      case Idle: return 0.0;
      case Attack:
        level += atkInc;
        if (level >= 1.0) {
          level = 1.0;
          stage = Decay;
        }
        break;
      case Decay:
        level = sustain + (level - sustain) * decCoef;
        if (level - sustain < 1e-4) {
          level = sustain;
          stage = Sustain;
        }
        break;
      case Sustain: level = sustain; break;
      case Release:
        level *= relCoef;
        if (level < 1e-5) {
          level = 0.0;
          stage = Idle;
        }
        break;
    }
    return level;
  }
};

// Topology-preserving state-variable filter (Zavalishin), stereo-capable per instance.
struct Svf {
  double ic1 = 0, ic2 = 0;
  double g = 0, k = 1, a1 = 0, a2 = 0, a3 = 0;
  void set(double sr, double hz, double resonance01) {
    hz = std::clamp(hz, 10.0, sr * 0.49);
    g = std::tan(kPi * hz / sr);
    k = 2.0 - 1.98 * std::clamp(resonance01, 0.0, 1.0);
    a1 = 1.0 / (1.0 + g * (g + k));
    a2 = g * a1;
    a3 = g * a2;
  }
  // Returns lowpass; band/high available via out params.
  double process(double v0, double* band = nullptr, double* high = nullptr) {
    double v3 = v0 - ic2;
    double v1 = a1 * ic1 + a2 * v3;
    double v2 = ic2 + a2 * ic1 + a3 * v3;
    ic1 = 2.0 * v1 - ic1;
    ic2 = 2.0 * v2 - ic2;
    if (band) *band = v1;
    if (high) *high = v0 - k * v1 - v2;
    return v2;
  }
  void reset() { ic1 = ic2 = 0; }
};

// RBJ biquad (direct form I).
struct Biquad {
  double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
  double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
  enum Type { LowShelf, HighShelf, Peak, LowPass, HighPass, BandPass, Notch };
  void set(Type t, double sr, double hz, double q, double gainDb) {
    hz = std::clamp(hz, 10.0, sr * 0.49);
    double A = std::pow(10.0, gainDb / 40.0);
    double w = kTwoPi * hz / sr, cw = std::cos(w), sw = std::sin(w);
    double alpha = sw / (2.0 * std::max(q, 0.05));
    double nb0, nb1, nb2, na0, na1, na2;
    switch (t) {
      case Peak:
        nb0 = 1 + alpha * A; nb1 = -2 * cw; nb2 = 1 - alpha * A;
        na0 = 1 + alpha / A; na1 = -2 * cw; na2 = 1 - alpha / A;
        break;
      case LowShelf: {
        double s = 2 * std::sqrt(A) * alpha;
        nb0 = A * ((A + 1) - (A - 1) * cw + s); nb1 = 2 * A * ((A - 1) - (A + 1) * cw); nb2 = A * ((A + 1) - (A - 1) * cw - s);
        na0 = (A + 1) + (A - 1) * cw + s; na1 = -2 * ((A - 1) + (A + 1) * cw); na2 = (A + 1) + (A - 1) * cw - s;
        break;
      }
      case HighShelf: {
        double s = 2 * std::sqrt(A) * alpha;
        nb0 = A * ((A + 1) + (A - 1) * cw + s); nb1 = -2 * A * ((A - 1) + (A + 1) * cw); nb2 = A * ((A + 1) + (A - 1) * cw - s);
        na0 = (A + 1) - (A - 1) * cw + s; na1 = 2 * ((A - 1) - (A + 1) * cw); na2 = (A + 1) - (A - 1) * cw - s;
        break;
      }
      case LowPass:
        nb0 = (1 - cw) / 2; nb1 = 1 - cw; nb2 = (1 - cw) / 2; na0 = 1 + alpha; na1 = -2 * cw; na2 = 1 - alpha;
        break;
      case BandPass:
        nb0 = alpha; nb1 = 0; nb2 = -alpha; na0 = 1 + alpha; na1 = -2 * cw; na2 = 1 - alpha;
        break;
      case Notch:
        nb0 = 1; nb1 = -2 * cw; nb2 = 1; na0 = 1 + alpha; na1 = -2 * cw; na2 = 1 - alpha;
        break;
      case HighPass: default:
        nb0 = (1 + cw) / 2; nb1 = -(1 + cw); nb2 = (1 + cw) / 2; na0 = 1 + alpha; na1 = -2 * cw; na2 = 1 - alpha;
        break;
    }
    b0 = nb0 / na0; b1 = nb1 / na0; b2 = nb2 / na0; a1 = na1 / na0; a2 = na2 / na0;
  }
  // |H(f)| of the current coefficients.
  double magnitude(double hz, double sr) const {
    double w = kTwoPi * hz / sr;
    double c1 = std::cos(w), s1 = std::sin(w), c2 = std::cos(2 * w), s2 = std::sin(2 * w);
    double nr = b0 + b1 * c1 + b2 * c2, ni = -(b1 * s1 + b2 * s2);
    double dr = 1 + a1 * c1 + a2 * c2, di = -(a1 * s1 + a2 * s2);
    return std::sqrt((nr * nr + ni * ni) / std::max(1e-30, dr * dr + di * di));
  }
  void clear() { x1 = x2 = y1 = y2 = 0; }
  double process(double x) {
    double y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
    x2 = x1; x1 = x; y2 = y1; y1 = y;
    if (std::fabs(y1) < 1e-20) y1 = 0;  // flush denormals
    return y;
  }
};

// One-pole lowpass smoother (per sample), coefficient from a time constant.
struct OnePole {
  double z = 0, a = 0;
  void setTime(double sr, double seconds) { a = std::exp(-1.0 / std::max(1.0, seconds * sr)); }
  void setFreq(double sr, double hz) { a = std::exp(-kTwoPi * std::clamp(hz, 1.0, sr * 0.49) / sr); }
  double process(double x) { return z = x + (z - x) * a; }
};

// ---------------------------------------------------------------- 0.5 blocks

// DC blocker (one-zero / one-pole high-pass, ~5 Hz).
struct DcBlock {
  double x1 = 0, y1 = 0, r = 0.9993;
  void setFreq(double sr, double hz = 5.0) { r = std::exp(-kTwoPi * hz / sr); }
  double process(double x) {
    double y = x - x1 + r * y1;
    x1 = x;
    y1 = std::fabs(y) < 1e-20 ? 0 : y;
    return y;
  }
  void reset() { x1 = y1 = 0; }
};

// First-order all-pass: coefficient from a corner frequency.
struct Allpass1 {
  double a = 0, z = 0;
  void setFreq(double sr, double hz) {
    double t = std::tan(kPi * std::clamp(hz, 5.0, sr * 0.49) / sr);
    a = (t - 1) / (t + 1);
  }
  double process(double x) {
    double y = a * x + z;
    z = x - a * y;
    return y;
  }
  void reset() { z = 0; }
};

// Fixed-size circular delay (power-of-two capacity, no allocation).
template <int N>
struct FixedDelay {
  static_assert((N & (N - 1)) == 0, "power of two");
  float buf[N] = {};
  int w = 0;
  void push(float x) {
    buf[w] = x;
    w = (w + 1) & (N - 1);
  }
  // Sample written d samples ago (d >= 1).
  float tap(int d) const { return buf[(w - std::clamp(d, 1, N - 1)) & (N - 1)]; }
  float tapFrac(double d) const {
    d = std::clamp(d, 1.0, static_cast<double>(N - 2));
    int i = static_cast<int>(d);
    float fr = static_cast<float>(d - i);
    return tap(i) * (1 - fr) + tap(i + 1) * fr;
  }
  void clear() {
    for (float& v : buf) v = 0;
  }
};

// Halfband FIR (31 taps, Blackman-windowed sinc) used for 2x up/down sampling.
struct Halfband {
  static constexpr int L = 31;
  double h[L];
  Halfband() {
    for (int i = 0; i < L; ++i) {
      int k = i - (L - 1) / 2;
      double sinc = k == 0 ? 0.5 : std::sin(kPi * k * 0.5) / (kPi * k);
      double w = 0.42 - 0.5 * std::cos(kTwoPi * i / (L - 1)) + 0.08 * std::cos(2 * kTwoPi * i / (L - 1));
      h[i] = sinc * w;
    }
    double s = 0;
    for (double v : h) s += v;
    for (double& v : h) v /= s;
  }
};

struct HalfbandState {
  double z[Halfband::L] = {};
  int pos = 0;
  double push(const Halfband& hb, double x) {
    z[pos] = x;
    double acc = 0;
    int j = pos;
    for (int i = 0; i < Halfband::L; ++i) {
      acc += hb.h[i] * z[j];
      j = j == 0 ? Halfband::L - 1 : j - 1;
    }
    pos = (pos + 1) % Halfband::L;
    return acc;
  }
  void reset() {
    for (double& v : z) v = 0;
    pos = 0;
  }
};

// Up to 8x oversampling around a per-sample nonlinearity (cascaded 2x stages).
// Latency (base-rate samples) is reported by latency(stages).
struct Oversampler {
  static const Halfband& hb() {
    static const Halfband h;
    return h;
  }
  HalfbandState up[3], down[3];
  template <typename F>
  double run(double x, int stages, F&& f) { return step(x, 0, std::clamp(stages, 0, 3), f); }
  static int latency(int stages) { return stages <= 0 ? 0 : stages == 1 ? 15 : stages == 2 ? 22 : 26; }
  void reset() {
    for (auto& s : up) s.reset();
    for (auto& s : down) s.reset();
  }

 private:
  template <typename F>
  double step(double x, int level, int stages, F& f) {
    if (level >= stages) return f(x);
    const Halfband& h = hb();
    double a = up[level].push(h, x * 2.0), b = up[level].push(h, 0.0);
    double ya = step(a, level + 1, stages, f), yb = step(b, level + 1, stages, f);
    down[level].push(h, ya);
    return down[level].push(h, yb);
  }
};

// Envelope shape family used by the dynamics plugins (8 shapes): blends an
// exponential approach (shape 0) toward a constant rate in dB (shape 7).
// Returns the new level (dB) moving from `cur` toward `target` for one sample.
inline double curveStep(double cur, double target, double coef, double dbPerSample, int shape) {
  double w = std::clamp(shape, 0, 7) / 7.0;
  double expo = target + (cur - target) * coef;
  double lin = cur < target ? std::min(target, cur + dbPerSample) : std::max(target, cur - dbPerSample);
  return expo * (1 - w) + lin * w;
}

}  // namespace gl::dsp
