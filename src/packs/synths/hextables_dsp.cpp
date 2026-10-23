// Gridline — "synths" pack, J-33 group "hex": band-limited wave tables and the
// filter coefficient table of Hex Synth. The built-in wave spectra are
// Gridline's own (hexwaves_dsp.cpp).
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include "hex_rt.h"

namespace gl::packs::synths::hex {

TablePtr waveTable(const Spectrum& s) {
  // Content hash (FNV-1a over the float bits).
  uint64_t h = 1469598103934665603ull;
  auto mix = [&](const float* p, size_t n) {
    const auto* b = reinterpret_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n * 4; ++i) h = (h ^ b[i]) * 1099511628211ull;
  };
  mix(s.amp, kHarmonics);
  mix(s.phase, kHarmonics);
  static std::mutex mu;
  static std::map<uint64_t, std::weak_ptr<const WaveTable>> cache;
  {
    std::lock_guard<std::mutex> lk(mu);
    auto it = cache.find(h);
    if (it != cache.end())
      if (auto t = it->second.lock()) return t;
  }
  auto t = std::make_shared<WaveTable>();
  // Cumulative sums: after harmonic k (0-based) the cycle holds harmonics
  // 1..k + 1, which is level 127 − k.
  float acc[256] = {};
  for (int k = 0; k < kHarmonics; ++k) {
    const double inc = static_cast<double>(k + 1) / 256.0;
    const double ph = s.phase[k], a = s.amp[k];
    if (a != 0.0)
      for (int n = 0; n < 256; ++n)
        acc[n] = static_cast<float>(static_cast<double>(acc[n]) + std::sin((n * inc + ph) * 6.28318530718) * a);
    float* row = t->t[kHarmonics - 1 - k];
    row[0] = acc[255];
    std::memcpy(row + 1, acc, sizeof acc);
    row[257] = acc[0];
    row[258] = acc[1];
  }
  std::lock_guard<std::mutex> lk(mu);
  cache[h] = t;
  return t;
}

const FilterCoef* filterTable() {
  static const std::vector<FilterCoef> table = [] {
    std::vector<FilterCoef> v(65536);
    for (int i = 0; i < 65536; ++i) {
      const double u = i / 65535.0;
      const double fc = std::exp2(5.0 + 9.0 * u);
      const double g = 1.6326530612244898e-4 * fc - 3.290809899167528e-9 * fc * fc;
      const double term = 0.95 * std::exp(1.386249 - 0.6931245 * g);
      const double r = 1.0 - u;
      const double q = u > 0.7 ? 0.08 * ((10.0 * u - 7.0) / 3.0) * ((10.0 * u - 7.0) / 3.0) : 0.0;
      v[static_cast<size_t>(i)].halfG = static_cast<float>(g * 0.5);
      v[static_cast<size_t>(i)].oneMinusG = static_cast<float>(1.0 - g);
      v[static_cast<size_t>(i)].feedback = static_cast<float>((2.0 * std::sqrt(r) * r + q + term) * 0.98);
    }
    return v;
  }();
  return table.data();
}

std::shared_ptr<const HexAux> buildHexAux(const std::string& data, const std::vector<float>& params) {
  HexData d;
  parseHexData(data, d);
  auto a = std::make_shared<HexAux>();
  for (int s = 0; s < kSlots; ++s) {
    const size_t pi = static_cast<size_t>(HxWave + s);
    const int w = pi < params.size() ? std::clamp(static_cast<int>(std::lround(params[pi])), 0, kWaves - 1) : 1;
    a->wave[s] = w;
    Spectrum sp;
    if (w == 0) {
      std::memcpy(sp.amp, d.amp[s], sizeof sp.amp);
      std::memcpy(sp.phase, d.phase[s], sizeof sp.phase);
    } else {
      sp = builtinSpectrum(w);
    }
    a->slot[s] = waveTable(sp);
  }
  return a;
}

}  // namespace gl::packs::synths::hex
