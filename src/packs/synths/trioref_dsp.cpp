// Gridline — Trio Synth "Reference" oscillators (see trioref.h). Arithmetic
// order and float / double precision follow the reference's measured
// behaviour; keep them when editing.
#include "trioref.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <mutex>
#include "../../core/PluginParams.h"

namespace gl::packs::synths::trioref {

namespace {

constexpr double kTwoPi = 6.283185307179586476925;
constexpr double kInv32 = 2.3283064365386963e-10;  // 2^-32
constexpr double kBaseHz32 = 2247346493527.166;     // 2^32 · 523.2511 Hz
constexpr double kCentLn = 0.0005776226504666211;   // ln 2 / 1200

float prm(const PluginState& s, int i, float d = 0.f) { return i >= 0 && static_cast<size_t>(i) < s.params.size() ? s.params[static_cast<size_t>(i)] : d; }

// Rounded saw: fast exponential charge to the top, flat, then low for the last sixth.
float roundSaw(float x) {
  float v = -1.0f;
  if (static_cast<double>(x) < 0.83333333333333337) {
    v = 1.0f;
    if (x < 0.463867188f) {
      const double p = std::pow(0.997, (static_cast<double>(x) / 0.4638671875) * 7600.0);
      v = static_cast<float>((1.0 - p) + (1.0 - p) + -1.0);
    }
  }
  return v;
}

// In-place radix-2 FFT (double).
void fft(std::vector<std::complex<double>>& a, bool inverse) {
  const size_t n = a.size();
  for (size_t i = 1, j = 0; i < n; ++i) {
    size_t bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) std::swap(a[i], a[j]);
  }
  for (size_t len = 2; len <= n; len <<= 1) {
    const double ang = kTwoPi / static_cast<double>(len) * (inverse ? 1.0 : -1.0);
    for (size_t i = 0; i < n; i += len)
      for (size_t j = 0; j < len / 2; ++j) {
        const std::complex<double> w = std::polar(1.0, ang * static_cast<double>(j));
        const std::complex<double> u = a[i + j], v = a[i + j + len / 2] * w;
        a[i + j] = u + v, a[i + j + len / 2] = u - v;
      }
  }
}

// The high-quality oscillator's one-cycle source waves (2048 samples).
void baseWave(int shape, float* w) {
  for (int i = 0; i < 2048; ++i) {
    const float x = static_cast<float>(static_cast<double>(i) * 0.00048828125);
    float v = 0;
    switch (shape) {
      case 0:  // triangle
        if (0.25f <= x && x < 0.75f) v = 1.0f - (x + -0.25f) * 4.0f;
        else if (0.75f <= x) v = (x + -0.75f) * 4.0f + -1.0f;
        else v = x * 4.0f;
        break;
      case 1: v = x < 0.5f ? 1.0f : -1.0f; break;
      case 2: {
        const float t = static_cast<float>(static_cast<double>(i) * -0.00048828125);
        v = -(t + t + 1.0f);
        break;
      }
      default: v = roundSaw(x); break;
    }
    w[i] = v;
  }
}

void buildMips(Tables& t) {
  constexpr int kSrc = 2048, kOver = 16, kN = Tables::kMipLen;
  for (int s = 0; s < 4; ++s) {
    float w[kSrc];
    baseWave(s, w);
    std::vector<std::complex<double>> spec(kN);
    double pos = 0;
    for (int j = 0; j < kN; ++j) {  // 16× linear-interpolated source
      const int i0 = static_cast<int>(pos);
      const int i1 = i0 + 1 >= kSrc ? 0 : i0 + 1;
      const float fr = static_cast<float>(pos) - static_cast<float>(static_cast<int>(pos));
      spec[static_cast<size_t>(j)] = static_cast<double>((1.0f - fr) * w[i0] + fr * w[i1]);
      pos += 1.0 / kOver;
    }
    fft(spec, false);
    double hf = kSrc * 0.5;
    const double ratio = std::exp2(0.25);
    std::vector<std::complex<double>> b(kN);
    for (int lev = 0; lev < Tables::kLevels; ++lev) {
      const int h = std::max(1, static_cast<int>(hf));
      hf /= ratio;
      std::fill(b.begin(), b.end(), std::complex<double>(0.0));
      for (int k = 0; k <= h; ++k) b[static_cast<size_t>(k)] = spec[static_cast<size_t>(k)];
      for (int k = 1; k <= h; ++k) b[static_cast<size_t>(kN - k)] = spec[static_cast<size_t>(kN - k)];
      fft(b, true);
      std::vector<float>& tab = t.mip[s][lev];
      tab.assign(static_cast<size_t>(kN) + 2, 0.0f);
      for (int j = 0; j < kN; ++j) tab[static_cast<size_t>(j)] = static_cast<float>(b[static_cast<size_t>(j)].real() / kN);
      tab[kN] = tab[0], tab[kN + 1] = tab[1];
    }
  }
}

void buildClassic(Tables& t) {
  constexpr int N = Tables::kClassic;
  uint32_t seed = 0x2545F491u;  // Gridline's own noise cycle
  for (auto& c : t.classic) c.assign(static_cast<size_t>(N) + 2, 0.0f);
  for (int i = 0; i < N; ++i) {
    const double x = static_cast<double>(i) / N;
    t.classic[RSine][static_cast<size_t>(i)] = static_cast<float>(std::sin(kTwoPi * x));
    t.classic[RTri][static_cast<size_t>(i)] = static_cast<float>(x < 0.25 ? 4 * x : x < 0.75 ? 2 - 4 * x : 4 * x - 4);
    t.classic[RSquare][static_cast<size_t>(i)] = x < 0.5 ? 1.0f : -1.0f;
    t.classic[RSaw][static_cast<size_t>(i)] = static_cast<float>(2 * x - 1);
    t.classic[RRound][static_cast<size_t>(i)] = roundSaw(static_cast<float>(x));
    seed = seed * 1664525u + 1013904223u;
    t.classic[RNoise][static_cast<size_t>(i)] = static_cast<float>(static_cast<double>(seed) * kInv32 * 2.0 - 1.0);
    t.classic[RPulse][static_cast<size_t>(i)] = x < 0.25 ? 1.0f : -1.0f;
  }
}

}  // namespace

std::shared_ptr<const Tables> tables(bool hq) {
  static std::mutex mu;
  static std::shared_ptr<Tables> classicOnly, full;
  std::lock_guard<std::mutex> lock(mu);
  if (hq) {
    if (!full) {
      auto t = std::make_shared<Tables>();
      buildClassic(*t);
      buildMips(*t);
      t->hq = true;
      full = t;
    }
    return full;
  }
  if (full) return full;
  if (!classicOnly) {
    auto t = std::make_shared<Tables>();
    buildClassic(*t);
    classicOnly = t;
  }
  return classicOnly;
}

Setup setup(const PluginState& s, double sampleRate) {
  using namespace pp;
  Setup r;
  r.sr = sampleRate;
  r.hq = prm(s, SynHQ, 1) >= 0.5f;
  r.base = static_cast<float>(kBaseHz32 / sampleRate);
  const bool am = static_cast<int>(prm(s, SynOsc3Mode) + 0.5f) == SynModeAM;
  r.amOsc = am ? 2 : -1;
  const double m2 = std::clamp(static_cast<double>(prm(s, SynOsc2Level)), 0.0, 1.0);
  const double m3 = std::clamp(static_cast<double>(prm(s, SynOsc3Level)), 0.0, 1.0);
  const double l1 = std::clamp(static_cast<double>(prm(s, SynOsc1Level, 1)), 0.0, 1.0);
  double lv[3];
  if (am) lv[0] = l1 * (1 - m2), lv[1] = m2, lv[2] = m3;
  else lv[0] = l1 * (1 - m2) * (1 - m3), lv[1] = m2 * (1 - m3), lv[2] = m3;
  const int shapeP[3] = {SynOsc1Shape, SynOsc2Shape, SynOsc3Shape};
  const int coarseP[3] = {SynOsc1Coarse, SynOsc2Coarse, SynOsc3Coarse};
  const int fineP[3] = {SynOsc1Fine, SynOsc2Fine, SynOsc3Fine};
  r.stereo = false;
  for (int k = 0; k < 3; ++k) {
    // Trio shape order (sine, triangle, saw, square, noise, rounded saw, pulse, custom) → reference order.
    static const int kMap[8] = {RSine, RTri, RSaw, RSquare, RNoise, RRound, RPulse, RCustom};
    r.shape[k] = kMap[std::clamp(static_cast<int>(prm(s, shapeP[k]) + 0.5f), 0, 7)];
    r.cents[k] = static_cast<int>(std::lround(prm(s, coarseP[k]))) * 100 + static_cast<int>(std::lround(prm(s, fineP[k])));
    const float sign = prm(s, SynOsc1Invert + k) >= 0.5f ? -1.0f : 1.0f;
    r.level[k] = static_cast<float>(lv[k]) * sign;
    const int pan = static_cast<int>(std::lround(std::clamp(prm(s, SynOsc1Pan + k), -1.0f, 1.0f) * 64.0f));
    const double ang = static_cast<double>(pan + 64) / 128.0 * (kTwoPi / 4.0);
    r.L[k] = static_cast<float>(static_cast<double>(r.level[k]) * std::cos(ang));
    r.R[k] = static_cast<float>(static_cast<double>(r.level[k]) * std::sin(ang));
    const int ph = static_cast<int>(std::lround(std::clamp(prm(s, SynOsc1Phase + k), -0.5f, 0.5f) * 128.0f));
    r.phaseR[k] = static_cast<uint32_t>(ph * 0x1FFFFFF);
    r.detune[k] = static_cast<int>(std::lround(prm(s, SynOsc1Detune + k)));
    if (pan != 0 || ph != 0 || r.detune[k] != 0 || r.shape[k] == RNoise) r.stereo = true;
    if (r.shape[k] == RCustom) r.custom = true;
  }
  return r;
}

void start(Voice& v, const Setup& s) {
  for (int k = 0; k < 3; ++k) v.ph[0][k] = 0, v.ph[1][k] = s.phaseR[k];
}

namespace {

uint32_t increment(const Setup& s, float cents) {
  return static_cast<uint32_t>(static_cast<int64_t>(std::nearbyint(std::exp(static_cast<double>(cents) * kCentLn) * static_cast<double>(s.base))));
}

// One oscillator into one side (stride 2). op: 0 add, 1 write, 2 multiply (AM).
uint32_t classicOsc(const float* tab, float* b, int n, uint32_t ph, uint32_t inc, float lvl, int op) {
  for (int i = 0; i < n; ++i) {
    float& x = b[2 * i];
    const float t = tab[ph >> 18];
    if (op == 2) x = x - t * lvl * x;
    else if (op == 1) x = t * lvl;
    else x = t * lvl + x;
    ph += inc;
  }
  return ph;
}

uint32_t hqOsc(const Tables& t, int shape, float* b, int n, uint32_t ph, uint32_t inc, float lvl, int op, double sr, uint32_t& seed) {
  if (op == 1)
    for (int i = 0; i < n; ++i) b[2 * i] = 0.0f;
  const bool am = op == 2;
  if (shape == RSine) {
    for (int i = 0; i < n; ++i) {
      const float v = std::sin(static_cast<float>(static_cast<double>(ph) * kInv32) * 6.28318548f);
      const float cur = b[2 * i];
      b[2 * i] = v * (am ? -cur : 1.0f) * lvl + cur;
      ph += inc;
    }
    return ph;
  }
  if (shape == RNoise) {
    for (int i = 0; i < n; ++i) {
      seed = seed * 0xbb38435u + 0x3619636bu;
      const float cur = b[2 * i];
      b[2 * i] = static_cast<float>(static_cast<int32_t>(seed)) * 4.65661287e-10f * (am ? -cur : 1.0f) * lvl + cur;
    }
    return ph + inc * static_cast<uint32_t>(n);
  }
  const double incF = static_cast<double>(inc) * kInv32;
  const double oct = std::log10(incF / ((1.0 / sr) * 8.176)) / 0.3010299956639812;
  const float lev = static_cast<float>(oct * 4.0 + -5.0);
  float pick = static_cast<float>(Tables::kLevels) + -2.0f;
  if (lev <= pick) pick = lev;
  if (lev < 0.0f) pick = 0.0f;
  const float* tab = t.mip[shape - 1][static_cast<int>(pick)].data();
  const bool table = 0.0005 <= incF;
  for (int i = 0; i < n; ++i) {
    const float x = static_cast<float>(static_cast<double>(ph) * kInv32);
    float v;
    if (table) {
      const float idx = x * 2048.0f * 16.0f;
      const int ii = static_cast<int>(idx);
      const float fr = idx - static_cast<float>(ii);
      v = (1.0f - fr) * tab[ii] + fr * tab[ii + 1];
    } else {
      switch (shape) {
        case RTri: v = x < 0.25f ? x * 4.0f : (x < 0.75f ? (x + -0.25f) * -4.0f + 1.0f : (x + -0.75f) * 4.0f + -1.0f); break;
        case RSquare: v = x < 0.5f ? 1.0f : -1.0f; break;
        case RSaw: v = -(1.0f - (x + x)); break;
        default: v = roundSaw(x); break;
      }
    }
    const float cur = b[2 * i];
    b[2 * i] = v * (am ? -cur : 1.0f) * lvl + cur;
    ph += inc;
  }
  return ph;
}

}  // namespace

void render(Voice& v, const Setup& s, const Tables& t, float cents, float* buf, int n, uint32_t& noiseSeed) {
  for (int i = 0; i < 2 * n; ++i) buf[i] = 0.0f;
  const bool hq = s.hq && t.hq;
  for (int sd = 0; sd < (s.stereo ? 2 : 1); ++sd) {
    bool first = true;
    for (int k = 0; k < 3; ++k) {
      const float x = sd == 0 ? static_cast<float>(s.cents[k]) + cents
                              : static_cast<float>(s.detune[k]) + static_cast<float>(s.cents[k]) + cents;
      const uint32_t inc = increment(s, x);
      if (s.level[k] == 0.0f) {
        v.ph[sd][k] += inc * static_cast<uint32_t>(n);
        continue;
      }
      const float lvl = sd == 0 ? s.L[k] : s.R[k];
      const int op = k == s.amOsc ? 2 : (first ? 1 : 0);
      int shape = s.shape[k];
      if (shape == RCustom) shape = RSaw;
      if (hq && shape != RPulse)
        v.ph[sd][k] = hqOsc(t, shape, buf + sd, n, v.ph[sd][k], inc, lvl, op, s.sr, noiseSeed);
      else
        v.ph[sd][k] = classicOsc(t.classic[shape].data(), buf + sd, n, v.ph[sd][k], inc, lvl, op);
      if (op != 2) first = false;
    }
  }
  if (!s.stereo)
    for (int i = 0; i < n; ++i) buf[2 * i + 1] = buf[2 * i];
}

float declick(int k, double sampleRate) {
  const int n = declickLength(sampleRate);
  if (k >= n) return 0.0f;
  return static_cast<float>(0.5 * (1.0 + std::cos(kTwoPi * 0.5 * static_cast<double>(k + 1) / static_cast<double>(n + 1))));
}

}  // namespace gl::packs::synths::trioref
