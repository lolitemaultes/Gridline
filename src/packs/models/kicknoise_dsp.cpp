// Gridline — "models" pack, J-22 group "drums": Kick Sculptor's eight noise
// layers, synthesised (no sample files). Each is a short mono noise hit of its
// own family — swept hiss, metal, clap, crunch, low bits — built once, lazily,
// on the UI thread (prepareAux) in a few milliseconds and shared by every
// instance.
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <mutex>
#include "../../engine/Sample.h"
#include "drums.h"

namespace gl::packs::models {

namespace dr {

namespace {

constexpr double kSr = 44100.0;
constexpr double kPi = 3.141592653589793;

struct Rng {
  uint32_t s;
  double next() {  // white, −1..1
    s ^= s << 13, s ^= s >> 17, s ^= s << 5;
    return static_cast<double>(s) / 2147483648.0 - 1.0;
  }
};

// Topology-preserving state-variable filter (cutoff may change per sample).
struct Svf {
  double ic1 = 0, ic2 = 0;
  // Returns {low, band, high}.
  std::array<double, 3> run(double x, double fc, double q) {
    const double g = std::tan(kPi * std::clamp(fc, 10.0, kSr * 0.45) / kSr), k = 1.0 / q;
    const double a1 = 1.0 / (1.0 + g * (g + k)), a2 = g * a1, a3 = g * a2;
    const double v3 = x - ic2, v1 = a1 * ic1 + a2 * v3, v2 = ic2 + a2 * ic1 + a3 * v3;
    ic1 = 2 * v1 - ic1, ic2 = 2 * v2 - ic2;
    return {v2, v1, x - k * v1 - v2};
  }
};

double sq(double phase) { return phase - std::floor(phase) < 0.5 ? 1.0 : -1.0; }

// Sum of square oscillators at inharmonic frequencies (a metal cluster).
struct Cluster {
  std::array<double, 6> f, ph{};
  double next() {
    double v = 0;
    for (size_t i = 0; i < f.size(); ++i) {
      v += sq(ph[i]);
      ph[i] += f[i] / kSr;
    }
    return v / static_cast<double>(f.size());
  }
};

std::vector<float> render(int k) {
  static const double kLen[] = {1.2, 1.2, 0.8, 1.2, 1.2, 1.2, 0.84, 0.44};
  const size_t n = static_cast<size_t>(kLen[k] * kSr);
  std::vector<float> out(n);
  Rng r{0x2545F491u + 977u * static_cast<uint32_t>(k)};
  Svf a, b;
  double hold = 0;
  Cluster bright{{2950, 4110, 5330, 6720, 8240, 10130}, {}}, ring{{3170, 4790, 6010, 7430, 9020, 11350}, {}};
  for (size_t i = 0; i < n; ++i) {
    const double t = static_cast<double>(i) / kSr, w = r.next();
    double v = 0;
    switch (k) {
      case 0:  // swept hiss: low-pass falling from bright to dark
        v = a.run(w, 400 + 9000 * std::exp(-t / 0.12), 0.9)[0] * std::exp(-t / 0.35);
        break;
      case 1:  // darker sweep with a little resonance
        v = a.run(w, 300 + 4500 * std::exp(-t / 0.1), 1.6)[0] * std::exp(-t / 0.28);
        break;
      case 2:  // bright metal: high noise and a square cluster
        v = a.run(0.5 * w + 0.7 * bright.next(), 6000, 0.7)[2] * std::exp(-t / 0.1);
        break;
      case 3: {  // hard clap: three quick bursts and a band-passed tail
        double env = std::exp(-t / 0.17);
        for (double at : {0.0, 0.011, 0.022})
          if (t >= at) env += 1.2 * std::exp(-(t - at) / 0.006);
        v = (a.run(w, 2200, 1.2)[1] * 1.6 + 0.15 * w) * env;
        break;
      }
      case 4: {  // crunch: decimated, 4-bit noise
        if (i % 8 == 0) hold = std::round(a.run(w, 3000, 0.7)[0] * 8) / 8;
        v = b.run(hold, 6000, 0.7)[0] * std::exp(-t / 0.3);
        break;
      }
      case 5: {  // low bits: random ±1 held at 1.5 kHz
        if (i % 29 == 0) hold = w < 0 ? -1.0 : 1.0;
        v = a.run(hold, 2000, 0.7)[0] * std::exp(-t / 0.2);
        break;
      }
      case 6:  // ring metal: a higher cluster, high-passed
        v = a.run(ring.next() + 0.2 * w, 7000, 0.8)[2] * std::exp(-t / 0.08);
        break;
      default: {  // soft clap: bright band, soft-edged bursts
        double env = std::exp(-t / 0.06);
        for (double at : {0.0, 0.008, 0.017})
          if (t >= at) env += 0.8 * (1 - std::exp(-(t - at) / 0.002)) * std::exp(-(t - at) / 0.01);
        v = a.run(w, 7500, 0.9)[1] * 1.5 * env;
        break;
      }
    }
    out[i] = static_cast<float>(v);
  }
  // Level: RMS of the first eighth 0.25, peak at most 0.99.
  const size_t head = std::max<size_t>(1, n / 8);
  double e = 0, pk = 0;
  for (size_t i = 0; i < head; ++i) e += static_cast<double>(out[i]) * out[i];
  for (float x : out) pk = std::max(pk, std::fabs(static_cast<double>(x)));
  double g = e > 0 ? 0.25 / std::sqrt(e / static_cast<double>(head)) : 1.0;
  if (pk * g > 0.99) g = 0.99 / pk;
  for (float& x : out) x = static_cast<float>(x * g);
  return out;
}

}  // namespace

SamplePtr kickNoiseSample(int k) {
  static std::array<SamplePtr, 8> set;
  static std::once_flag once;
  std::call_once(once, [] {
    for (int i = 0; i < 8; ++i) {
      auto s = std::make_shared<Sample>();
      s->path = std::string("gridline:kick-noise/") + kickNoiseLabel(i);
      s->sampleRate = kSr;
      s->left = render(i);
      s->setMono();
      set[static_cast<size_t>(i)] = s;
    }
  });
  return k >= 0 && k < 8 ? set[static_cast<size_t>(k)] : nullptr;
}

}  // namespace dr

}  // namespace gl::packs::models
