// Gridline — "synths" pack, J-33 group "hex": Gridline's own wave set of Hex
// Synth (selectors 1..32), generated from formulas in this file.
//
// Selectors keep their meaning across imports: each index holds a wave of the
// same family as the instrument it was decoded from (classic shapes, tonal
// shapes, noise, formant / sync), but every spectrum here is Gridline's own
// design. Nothing is read from disk. The set is built once, lazily, on the
// first call (prepareAux, UI thread): a few milliseconds of arithmetic.
//
// Families:
//   1..5    sine recipes          (sine, sine + octave, sine stack, sine fall, organ)
//   6..12   classic shapes        (triangle, rectified sine, square, round square,
//                                  25 % pulse, narrow pulse, saw)
//   13..19  tonal shapes          (soft-clipped and rectified sines, octave leads,
//                                  a sparse bell partial set, a reed formant)
//   20..23  bit noise             (3-bit sample-and-hold cycles of 7..60 steps)
//   24..32  formant / sync        (glottal-slope sources through resonances at
//                                  harmonic 8 (4 for 24) and their odd multiples;
//                                  hard-synced sine and square slaves at ratio ≈ 8)
// Every wave is peak-normalised (full-band cycle peak kPeak) and stored as 128
// harmonics; the wave table builder band-limits it per pitch.
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <functional>
#include <vector>
#include "hex_rt.h"

namespace gl::packs::synths::hex {

namespace {

constexpr double kTau = 6.283185307179586;
constexpr double kPeak = 0.9;

using Fn = std::function<double(double)>;

// Harmonics 1..128 of a periodic function x ∈ [0, 1) (sine basis, phase in cycles).
Spectrum fromCycle(const Fn& fn) {
  constexpr int n = 2048;
  std::vector<double> x(n), c(n), s(n);
  for (int i = 0; i < n; ++i) {
    x[static_cast<size_t>(i)] = fn(static_cast<double>(i) / n);
    c[static_cast<size_t>(i)] = std::cos(kTau * i / n);
    s[static_cast<size_t>(i)] = std::sin(kTau * i / n);
  }
  Spectrum sp;
  for (int h = 1; h <= kHarmonics; ++h) {
    double re = 0, im = 0;
    for (int i = 0; i < n; ++i) {
      const size_t k = static_cast<size_t>((static_cast<long>(h) * i) % n);
      re += x[static_cast<size_t>(i)] * c[k];
      im -= x[static_cast<size_t>(i)] * s[k];
    }
    re *= 2.0 / n, im *= 2.0 / n;
    // x = |c| cos(ωt + arg c) = |c| sin(ωt + arg c + π/2)
    sp.amp[h - 1] = static_cast<float>(std::hypot(re, im));
    double ph = (std::atan2(im, re) + kTau / 4) / kTau;
    sp.phase[h - 1] = static_cast<float>(ph - std::floor(ph));
  }
  return sp;
}

// Harmonic series a(h) with phase p(h) (cycles); negative amplitudes flip the phase.
Spectrum series(const std::function<double(int)>& a, const std::function<double(int)>& p = nullptr) {
  Spectrum sp;
  for (int h = 1; h <= kHarmonics; ++h) {
    double v = a(h), ph = p ? p(h) : 0.0;
    if (v < 0) v = -v, ph += 0.5;
    sp.amp[h - 1] = static_cast<float>(v);
    sp.phase[h - 1] = static_cast<float>(ph - std::floor(ph));
  }
  return sp;
}

struct Lcg {
  uint32_t s;
  double next() {
    s = s * 1664525u + 1013904223u;
    return static_cast<double>(s >> 8) / 16777216.0;
  }
};

// One cycle of `steps` held 3-bit random levels (zero mean).
Spectrum bitNoise(int steps, uint32_t seed) {
  Lcg r{seed};
  std::vector<double> v(static_cast<size_t>(steps));
  double mean = 0;
  for (double& x : v) mean += (x = std::round((r.next() * 2 - 1) * 4) / 4);
  mean /= steps;
  for (double& x : v) x -= mean;
  return fromCycle([&](double x) { return v[static_cast<size_t>(std::min(steps - 1, static_cast<int>(x * steps)))]; });
}

// A formant voice: a 1 / h^tilt source shaped by resonances {centre harmonic,
// half width, gain} over a flat floor `base`; listed harmonics are removed.
// Phases spread the energy over the cycle (Schroeder's rule) for a low crest.
struct Res {
  double f, bw, g;
};
Spectrum formant(std::initializer_list<Res> rs, double base, double tilt, std::initializer_list<int> zero = {}) {
  Spectrum sp;
  double e = 0;
  for (int h = 1; h <= kHarmonics; ++h) {
    double g = base;
    for (const Res& r : rs) g += r.g / (1.0 + ((h - r.f) / r.bw) * ((h - r.f) / r.bw));
    if (std::find(zero.begin(), zero.end(), h) != zero.end()) g = 0;
    sp.amp[h - 1] = static_cast<float>(g / std::pow(h, tilt));
    e += static_cast<double>(sp.amp[h - 1]) * sp.amp[h - 1];
  }
  double cum = 0, wsum = 0;  // Σ_{l<h} (h − l) p_l, built incrementally
  for (int h = 1; h <= kHarmonics; ++h) {
    cum += wsum;
    const double ph = 0.25 - cum;
    sp.phase[h - 1] = static_cast<float>(ph - std::floor(ph));
    wsum += static_cast<double>(sp.amp[h - 1]) * sp.amp[h - 1] / e;
  }
  return sp;
}

// Hard sync: a slave at `ratio` restarts every cycle under a falling window;
// `odd` > 0 adds the slave's 3rd and 5th harmonics (a soft square slave).
Spectrum hardSync(double ratio, double window, double odd) {
  return fromCycle([=](double x) {
    const double t = kTau * ratio * x;
    const double v = std::sin(t) + odd * (std::sin(3 * t) / 3 + std::sin(5 * t) / 5);
    return v * std::pow(1.0 - x, window);
  });
}

double cyclePeak(const Spectrum& s) {
  double pk = 0;
  for (int n = 0; n < 512; ++n) {
    double v = 0;
    for (int k = 0; k < kHarmonics; ++k)
      if (s.amp[k] > 0) v += s.amp[k] * std::sin(kTau * ((k + 1) * n / 512.0 + s.phase[k]));
    pk = std::max(pk, std::fabs(v));
  }
  return pk;
}

Spectrum design(int w) {
  auto odd = [](int h) { return h % 2 == 1; };
  switch (w) {
    // ---- classic shapes
    case 6: return series([&](int h) { return odd(h) ? 1.0 / (h * h) : 0.0; }, [](int h) { return h % 4 == 3 ? 0.5 : 0.0; });
    case 7: return series([&](int h) { return odd(h) ? 0.0 : 1.0 / (h * h - 1.0); }, [](int) { return 0.75; });
    case 8: return series([&](int h) { return odd(h) ? 1.0 / h : 0.0; });
    case 9: return series([&](int h) { return odd(h) ? 1.0 / (h * std::sqrt(1.0 + (h / 3.0) * (h / 3.0))) : 0.0; });
    case 10: return series([](int h) { return std::sin(M_PI * h / 4.0) / h; }, [](int) { return 0.25; });
    case 11: return series([](int h) { return std::sin(M_PI * h / 14.0) / h; }, [](int) { return 0.25; });
    case 12: return series([](int h) { return 1.0 / h; });
    // ---- tonal shapes
    case 13: return fromCycle([](double x) { return std::tanh(1.6 * std::sin(kTau * x)); });
    case 14: return fromCycle([](double x) { return std::max(0.0, std::sin(kTau * x)) - 1.0 / M_PI; });
    case 15: return fromCycle([](double x) { return std::tanh(2.6 * std::sin(kTau * x) + 0.9) - std::tanh(0.9); });
    case 16: return series([&](int h) { return h == 1 ? 0.8 : h == 2 ? 1.0 : odd(h) ? 0.6 / (h * h) : 0.0; });
    case 17:
      return fromCycle([](double x) { return std::tanh(1.4 * (std::sin(2 * kTau * x) + 0.75 * std::sin(kTau * x + 0.6))); });
    case 18: {  // sparse bell-like partials, rounded to harmonics
      static const std::array<std::pair<int, double>, 9> p = {
          {{1, 1.0}, {3, 0.55}, {4, 0.5}, {5, 0.16}, {8, 0.12}, {11, 0.05}, {13, 0.04}, {17, 0.02}, {20, 0.015}}};
      return series(
          [&](int h) {
            for (auto [k, a] : p)
              if (k == h) return a;
            return 0.0;
          },
          [](int h) { return h * 0.37; });
    }
    case 19: return formant({{4, 0.9, 2.2}}, 0.25, 1.6, {2, 6});
    // ---- bit noise, brighter with the index
    case 20: return bitNoise(7, 20);
    case 21: return bitNoise(14, 21);
    case 22: return bitNoise(28, 22);
    case 23: return bitNoise(60, 23);
    // ---- formant / sync
    case 24: return formant({{4, 0.8, 3.0}, {12, 1.5, 1.2}, {20, 2.0, 0.4}}, 0.5, 0.6);
    case 25: return formant({{8, 1.0, 4.0}, {24, 2.0, 1.5}, {40, 3.0, 0.6}}, 0.6, 0.6, {2, 6});
    case 26: return formant({{8, 0.8, 5.0}, {24, 2.0, 1.2}, {40, 3.0, 0.6}}, 0.9, 0.5);
    case 27: return formant({{8, 1.0, 3.5}, {16, 2.5, 0.6}, {30, 3.0, 0.5}}, 0.6, 0.6);
    case 28: return formant({{8, 1.0, 3.5}, {13, 2.0, 1.0}, {28, 3.0, 0.5}}, 0.6, 0.6);
    case 29: return formant({{8, 1.2, 2.5}, {20, 2.5, 0.8}, {34, 3.0, 0.4}}, 0.9, 0.6);
    case 30: return hardSync(8.45, 0.3, 0.0);
    case 31: return formant({{1, 0.6, 1.0}, {8, 1.0, 4.0}, {24, 2.0, 2.0}}, 0.6, 0.6, {2, 6});
    case 32: return hardSync(7.7, 0.15, 0.8);
    default: break;
  }
  Spectrum s;
  s.amp[0] = 1.f;
  return s;
}

const std::vector<Spectrum>& waveSet() {
  static const std::vector<Spectrum> set = [] {
    std::vector<Spectrum> v(static_cast<size_t>(kWaves));
    for (int w = 6; w < kWaves; ++w) {
      Spectrum s = design(w);
      const double pk = cyclePeak(s);
      const float g = pk > 0 ? static_cast<float>(kPeak / pk) : 1.f;
      for (float& a : s.amp) a = std::isfinite(a) ? a * g : 0.f;
      for (float& p : s.phase)
        if (!std::isfinite(p)) p = 0.f;
      v[static_cast<size_t>(w)] = s;
    }
    return v;
  }();
  return set;
}

}  // namespace

Spectrum builtinSpectrum(int w) {
  Spectrum s;
  switch (w) {
    case 1: s.amp[0] = 1.f; return s;
    case 2: s.amp[0] = s.amp[1] = 0.5f; return s;
    case 3: s.amp[0] = s.amp[1] = s.amp[3] = 0.4f; return s;
    case 4: s.amp[0] = 0.5f, s.amp[1] = 0.25f, s.amp[3] = 0.125f; return s;
    case 5: s.amp[0] = 0.8f, s.amp[1] = 0.256f, s.amp[7] = 0.008f; return s;
    default: break;
  }
  if (w < 6 || w >= kWaves) {
    s.amp[0] = 1.f;
    return s;
  }
  return waveSet()[static_cast<size_t>(w)];
}

WaveFamily waveFamily(int w) {
  if (w <= 0) return WaveFamily::Custom;
  if (w <= 5) return WaveFamily::Sine;
  if (w <= 12) return WaveFamily::Classic;
  if (w <= 19) return WaveFamily::Tonal;
  if (w <= 23) return WaveFamily::Noise;
  return WaveFamily::Formant;
}

}  // namespace gl::packs::synths::hex
