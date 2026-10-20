// Gridline — "synths" pack, J-33 group "hex": wave spectra, band-limited wave
// tables and the filter coefficient table of Hex Synth.
//
// Waves 1..5 are simple harmonic recipes. Waves 6..32 are the reference
// instrument's own harmonic spectra: they are read at run time from its
// installed plugin file (located by searching it for the triangle spectrum the
// code below computes itself) and never copied into Gridline. Without the
// reference installed, formula spectra of the same families stand in.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include "../../core/flp/Flp.h"
#include "hex_rt.h"

namespace gl::packs::synths::hex {

namespace {

constexpr int kRefWaves = 27;           // selectors 6..32
constexpr size_t kRefBlock = 2 * 128;   // floats per wave (amplitudes, phases)

// Spectra 6..32 found in the reference plugin, or empty.
const std::vector<float>& referenceSpectra() {
  static const std::vector<float> data = [] {
    std::vector<float> out;
    namespace fs = std::filesystem;
    const std::string fl = flp::defaultFactoryData();
    if (fl.empty()) return out;
    std::error_code ec;
    const fs::path dir = fs::path(fl) / "Plugins" / "Fruity" / "Generators" / "Toxic Biohazard";
    if (!fs::is_directory(dir, ec)) return out;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end && out.empty(); it.increment(ec)) {
      const std::string ext = it->path().extension().string();
      if (ext != ".dylib" && ext != ".dll") continue;
      std::FILE* f = std::fopen(it->path().string().c_str(), "rb");
      if (!f) continue;
      std::vector<uint8_t> bytes;
      uint8_t buf[65536];
      size_t n;
      while ((n = std::fread(buf, 1, sizeof buf, f)) > 0 && bytes.size() < (64u << 20)) bytes.insert(bytes.end(), buf, buf + n);
      std::fclose(f);
      // Anchor: the triangle wave (odd harmonics 8 / (π² h²), even ones zero).
      const double t1 = 8.0 / (M_PI * M_PI);
      auto fl32 = [&](size_t at) {
        float v;
        std::memcpy(&v, bytes.data() + at, 4);
        return v;
      };
      const size_t need = kRefWaves * kRefBlock * 4;
      for (size_t at = 0; at + need <= bytes.size(); at += 4) {
        if (std::fabs(fl32(at) - t1) > 2e-3) continue;
        bool ok = true;
        for (int h = 1; h <= 9 && ok; ++h) {
          const float a = fl32(at + 4 * static_cast<size_t>(h - 1));
          ok = (h % 2 == 0) ? a == 0.f : std::fabs(a - t1 / (h * h)) < 2e-3;
        }
        // Saw (selector 12, six waves on): roughly 0.51 / h.
        const size_t saw = at + 6 * kRefBlock * 4;
        ok = ok && std::fabs(fl32(saw) - 0.509f) < 0.01f && std::fabs(fl32(saw + 4) - 0.2547f) < 0.01f;
        if (!ok) continue;
        out.resize(kRefWaves * kRefBlock);
        std::memcpy(out.data(), bytes.data() + at, need);
        for (float& v : out)
          if (!std::isfinite(v)) v = 0.f;
        break;
      }
    }
    return out;
  }();
  return data;
}

// Formula stand-ins for selectors 6..32 (families: triangle, square, pulse,
// saw, tonal, noise, gate).
Spectrum formulaSpectrum(int w) {
  Spectrum s;
  auto tri = [&](double scale) {
    for (int h = 1; h <= kHarmonics; h += 2) {
      s.amp[h - 1] = static_cast<float>(scale * 8.0 / (M_PI * M_PI * h * h));
      s.phase[h - 1] = (h % 4 == 3) ? 0.5f : 0.f;
    }
  };
  auto square = [&](double scale) {
    for (int h = 1; h <= kHarmonics; h += 2) s.amp[h - 1] = static_cast<float>(scale * 3.0 / (M_PI * h));
  };
  auto pulse = [&](double duty) {
    for (int h = 1; h <= kHarmonics; ++h) s.amp[h - 1] = static_cast<float>(std::fabs(std::sin(M_PI * h * duty)) * 1.6 / (M_PI * h));
  };
  auto saw = [&]() {
    for (int h = 1; h <= kHarmonics; ++h) {
      s.amp[h - 1] = static_cast<float>(0.509 / h);
      s.phase[h - 1] = static_cast<float>(h / 256.0);
    }
  };
  if (w == 6 || w == 7) tri(1.0);
  else if (w == 8 || w == 9) square(1.0);
  else if (w == 10) pulse(0.25);
  else if (w == 11) pulse(0.125);
  else if (w >= 20 && w <= 23) {  // noise: flat spectrum, scrambled phases
    uint32_t r = 0x9E3779B9u * static_cast<uint32_t>(w);
    for (int h = 1; h <= kHarmonics; ++h) {
      r = r * 1664525u + 1013904223u;
      s.amp[h - 1] = 0.12f;
      s.phase[h - 1] = static_cast<float>(r >> 8) / 16777216.0f;
    }
  } else if (w >= 24) pulse(0.5);
  else saw();
  return s;
}

}  // namespace

bool referenceSpectraAvailable() { return !referenceSpectra().empty(); }

Spectrum builtinSpectrum(int w, bool* exact) {
  Spectrum s;
  if (exact) *exact = true;
  switch (w) {
    case 1: s.amp[0] = 1.f; return s;
    case 2: s.amp[0] = s.amp[1] = 0.5f; return s;
    case 3: s.amp[0] = s.amp[1] = s.amp[3] = 0.4f; return s;
    case 4: s.amp[0] = 0.5f, s.amp[1] = 0.25f, s.amp[3] = 0.125f; return s;
    case 5: s.amp[0] = 0.8f, s.amp[1] = 0.256f, s.amp[7] = 0.008f; return s;
    default: break;
  }
  if (w < 6 || w > 32) {
    s.amp[0] = 1.f;
    return s;
  }
  const auto& ref = referenceSpectra();
  if (ref.empty()) {
    if (exact) *exact = false;
    return formulaSpectrum(w);
  }
  const float* b = ref.data() + static_cast<size_t>(w - 6) * kRefBlock;
  std::memcpy(s.amp, b, sizeof s.amp);
  std::memcpy(s.phase, b + kHarmonics, sizeof s.phase);
  return s;
}

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
