// Gridline — "spacefx" pack: realtime building blocks (included by spacefx_dsp.cpp only).
// Everything here is allocation-free after init(); init() runs in constructors.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>
#include "../../engine/Dsp.h"
#include "../../engine/Effects.h"
#include "spacefx.h"

namespace gl::packs::spacefx::detail {

using dsp::kPi;
using dsp::kTwoPi;
inline constexpr double kMaxRate = 192000.0;

inline float pv(const PluginState& s, int k, float def = 0.f) {
  return k >= 0 && static_cast<size_t>(k) < s.params.size() ? s.params[static_cast<size_t>(k)] : def;
}
inline int pi(const PluginState& s, int k, int def = 0) { return static_cast<int>(std::lround(pv(s, k, static_cast<float>(def)))); }
inline bool pb(const PluginState& s, int k) { return pv(s, k) >= 0.5f; }
inline float flush(float x) { return std::fabs(x) < 1e-20f ? 0.f : x; }
inline double flushd(double x) { return std::fabs(x) < 1e-20 ? 0.0 : x; }
inline double dbg(double db) { return std::pow(10.0, db / 20.0); }
inline double clampd(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }
// Default value of a parameter from the catalogue (for short/old states).
inline float pdef(const PluginState& s, int k) {
  const PluginInfo* info = findPlugin(s.type);
  return info && static_cast<size_t>(k) < info->params.size() ? static_cast<float>(info->params[static_cast<size_t>(k)].def) : 0.f;
}
inline float pvd(const PluginState& s, int k) {
  return static_cast<size_t>(k) < s.params.size() ? s.params[static_cast<size_t>(k)] : pdef(s, k);
}

// Circular delay buffer, power-of-two size. Reads are relative to the next
// write: read(d) == x[n - d] while x[n] has not been written yet (d >= 1).
struct Ring {
  std::vector<float> b;
  size_t mask = 0, w = 0;
  void init(size_t minSize) {
    size_t n = 16;
    while (n < minSize) n <<= 1;
    b.assign(n, 0.f);
    mask = n - 1;
    w = 0;
  }
  void clear() {
    std::fill(b.begin(), b.end(), 0.f);
    w = 0;
  }
  size_t size() const { return b.size(); }
  void push(float x) {
    b[w & mask] = x;
    ++w;
  }
  float at(size_t d) const { return b[(w - d) & mask]; }  // d >= 1
  // Fractional read (cubic Hermite), d clamped to [1, size-3].
  float read(double d) const {
    d = clampd(d, 1.0, static_cast<double>(b.size() - 3));
    size_t i = static_cast<size_t>(d);
    float f = static_cast<float>(d - static_cast<double>(i));
    float xm1 = at(i > 1 ? i - 1 : 1), x0 = at(i), x1 = at(i + 1), x2 = at(i + 2);
    float c1 = 0.5f * (x1 - xm1), c2 = xm1 - 2.5f * x0 + 2.f * x1 - 0.5f * x2, c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
    return ((c3 * f + c2) * f + c1) * f + x0;
  }
  float readLinear(double d) const {
    d = clampd(d, 1.0, static_cast<double>(b.size() - 2));
    size_t i = static_cast<size_t>(d);
    float f = static_cast<float>(d - static_cast<double>(i));
    return at(i) + (at(i + 1) - at(i)) * f;
  }
};

// Schroeder allpass on a Ring (delay in samples, gain g).
struct Allpass {
  Ring r;
  void init(size_t n) { r.init(n); }
  void clear() { r.clear(); }
  float process(float x, double d, float g) {
    float z = r.read(d);
    float v = x + g * z;
    r.push(flush(v));
    return z - g * v;
  }
};

// One-pole lowpass/highpass pair (cheap tone shaping).
struct OnePole {
  float z = 0, a = 0;
  void setHz(double sr, double hz) { a = static_cast<float>(std::exp(-kTwoPi * clampd(hz, 1.0, sr * 0.49) / sr)); }
  float lp(float x) { return z = flush(x + (z - x) * a); }
  float hp(float x) { return x - lp(x); }
  void clear() { z = 0; }
};

// TPT state-variable filter with a mode (0 off, 1 LP, 2 HP, 3 BP).
struct Svf {
  double ic1 = 0, ic2 = 0, a1 = 0, a2 = 0, a3 = 0, k = 1;
  void set(double sr, double hz, double res) {
    double g = std::tan(kPi * clampd(hz, 10.0, sr * 0.45) / sr);
    k = 2.0 - 1.9 * clampd(res, 0.0, 1.0);
    a1 = 1.0 / (1.0 + g * (g + k));
    a2 = g * a1;
    a3 = g * a2;
  }
  float process(float x, int mode) {
    if (mode <= 0) return x;
    double v3 = x - ic2, v1 = a1 * ic1 + a2 * v3, v2 = ic2 + a2 * ic1 + a3 * v3;
    ic1 = flushd(2 * v1 - ic1);
    ic2 = flushd(2 * v2 - ic2);
    if (mode == 1) return static_cast<float>(v2);
    if (mode == 2) return static_cast<float>(x - k * v1 - v2);
    return static_cast<float>(v1 * k);
  }
  void clear() { ic1 = ic2 = 0; }
};

// Smoothed parameter (one-pole toward a target, per sample).
struct Smooth {
  double v = 0, a = 0.999;
  bool primed = false;
  void setTime(double sr, double sec) { a = std::exp(-1.0 / std::max(1.0, sec * sr)); }
  double next(double target) {
    if (!primed) {
      v = target;
      primed = true;
    }
    return v = target + (v - target) * a;
  }
};

// Balance pan gains (centre = unity both sides).
inline void balance(double pan, double& l, double& r) {
  l = pan > 0 ? 1.0 - pan : 1.0;
  r = pan < 0 ? 1.0 + pan : 1.0;
}

// ------------------------------------------------------------------- FFT
// In-place iterative radix-2 complex FFT on interleaved floats (re, im).
struct Fft {
  int n = 0;
  std::vector<int> rev;
  std::vector<float> tw;  // n/2 twiddles (cos, -sin)
  void init(int size) {
    n = size;
    rev.assign(static_cast<size_t>(n), 0);
    int bits = 0;
    while ((1 << bits) < n) ++bits;
    for (int i = 0; i < n; ++i) {
      int r = 0;
      for (int b = 0; b < bits; ++b)
        if (i & (1 << b)) r |= 1 << (bits - 1 - b);
      rev[static_cast<size_t>(i)] = r;
    }
    tw.assign(static_cast<size_t>(n), 0.f);
    for (int i = 0; i < n / 2; ++i) {
      double a = -kTwoPi * i / n;
      tw[static_cast<size_t>(2 * i)] = static_cast<float>(std::cos(a));
      tw[static_cast<size_t>(2 * i + 1)] = static_cast<float>(std::sin(a));
    }
  }
  // Forward transform (no scaling). inverse=true uses conjugate twiddles (no 1/n).
  void run(float* a, bool inverse) const {
    for (int i = 0; i < n; ++i) {
      int j = rev[static_cast<size_t>(i)];
      if (i < j) {
        std::swap(a[2 * i], a[2 * j]);
        std::swap(a[2 * i + 1], a[2 * j + 1]);
      }
    }
    const float sgn = inverse ? -1.f : 1.f;
    for (int len = 2; len <= n; len <<= 1) {
      int half = len >> 1, step = n / len;
      for (int i = 0; i < n; i += len) {
        for (int j = 0; j < half; ++j) {
          float wr = tw[static_cast<size_t>(2 * j * step)], wi = sgn * tw[static_cast<size_t>(2 * j * step + 1)];
          float* u = a + 2 * (i + j);
          float* v = a + 2 * (i + j + half);
          float tr = v[0] * wr - v[1] * wi, ti = v[0] * wi + v[1] * wr;
          v[0] = u[0] - tr;
          v[1] = u[1] - ti;
          u[0] += tr;
          u[1] += ti;
        }
      }
    }
  }
};

// LFO shapes used by the modulation effects (phase 0..1 → -1..1).
inline double lfoSine(double ph) { return std::sin(kTwoPi * ph); }
inline double lfoTri(double ph) {
  ph -= std::floor(ph);
  return ph < 0.5 ? 4 * ph - 1 : 3 - 4 * ph;
}

// Smooth random walk (for "movement"): interpolated random targets.
struct Wander {
  dsp::Noise rng;
  double a = 0, b = 0, ph = 1;
  double next(double inc) {
    ph += inc;
    if (ph >= 1) {
      ph -= std::floor(ph);
      a = b;
      b = rng.next();
    }
    double t = ph * ph * (3 - 2 * ph);
    return a + (b - a) * t;
  }
};

}  // namespace gl::packs::spacefx::detail
