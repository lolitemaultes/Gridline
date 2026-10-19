// Gridline — Additive: the partial-gain model shared by the runtime and the
// editor (spectra and colours, timbre EQ, mask, harmonic-domain filter with
// resonance shapes, phaser combs, pluck, harmonizer) and the drawn-data text.
// The shape helpers are also used by the two-part resynthesis synth.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include "models.h"

namespace gl::packs::models {

namespace {

constexpr double kPi = 3.14159265358979323846;

float prm(const PluginState& s, int i, float d) {
  return i >= 0 && static_cast<size_t>(i) < s.params.size() ? s.params[static_cast<size_t>(i)] : d;
}
int prmi(const PluginState& s, int i, int d) { return static_cast<int>(std::lround(prm(s, i, static_cast<float>(d)))); }

// Deterministic hash → 0..1.
float hash01(uint32_t a, uint32_t b) {
  uint32_t h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u) * 0x85EBCA77u;
  h ^= h >> 15;
  h *= 0x2C1B3C6Du;
  h ^= h >> 12;
  h *= 0x297A2D39u;
  h ^= h >> 15;
  return static_cast<float>(h & 0xFFFFFF) / static_cast<float>(0xFFFFFF);
}


}  // namespace

// ------------------------------------------------------------------ spectra
float spectrumLevel(int sp, int k, int seed, const AdditiveData* custom) {
  const float fk = static_cast<float>(k);
  switch (sp) {
    case SpSaw: return 1.0f / fk;
    case SpSquare: return (k & 1) ? 1.0f / fk : 0.0f;
    case SpTriangle: return (k & 1) ? 1.0f / (fk * fk) : 0.0f;
    case SpSine: return k == 1 ? 1.0f : 0.0f;
    case SpPulse: return 1.4142f * std::fabs(std::sin(3.14159265f * fk * 0.2f)) / fk;
    case SpOrgan: {
      static const float bars[16] = {1, 0.85f, 0.7f, 0.55f, 0.06f, 0.4f, 0.03f, 0.32f, 0.02f, 0.18f, 0.01f, 0.12f, 0.01f, 0.02f, 0.01f, 0.08f};
      return k <= 16 ? bars[k - 1] : 0.0f;
    }
    case SpVowel: {
      float f1 = std::exp(-0.5f * ((fk - 4.0f) / 1.3f) * ((fk - 4.0f) / 1.3f));
      float f2 = std::exp(-0.5f * ((fk - 11.0f) / 2.0f) * ((fk - 11.0f) / 2.0f));
      return (0.25f / std::sqrt(fk) + 0.9f * f1 + 0.45f * f2) * (k == 1 ? 1.6f : 1.0f);
    }
    case SpGlass: return (k % 3 == 1) ? 1.0f / std::sqrt(fk) : 0.12f / fk;
    case SpChoir: return (1.0f / fk) * (1.0f + 2.5f * std::exp(-0.5f * ((fk - 7.0f) / 2.5f) * ((fk - 7.0f) / 2.5f)));
    case SpRandom: return k == 1 ? 1.0f : hash01(static_cast<uint32_t>(seed), static_cast<uint32_t>(k)) / std::sqrt(fk);
    // Saw-family colours (h = k - 1).
    case SpBright: return 1.41421f / (fk + 1.0f);
    case SpBrighter: return 2.0f / (fk + 3.0f);
    case SpDephased:
    case SpDephased2: return 1.0f / fk;
    case SpSqSaw: return (k & 1) ? 1.1f / fk : 1.1f / (fk * (fk - 1.0f));
    case SpSqSaw2: return (k & 1) ? 1.1f / fk : 1.1f / (fk * std::pow(fk - 1.0f, 0.2f));
    case SpNotched: return (std::pow(7.0f, 1.0f / fk) - 1.0f) * 7.0f / 24.0f;
    case SpDouble: return 0.5f / fk + ((k % 2 == 0) ? 0.5f / (1.0f + (fk - 1.0f) / 2.0f) : 0.0f);
    case SpTriple:
      return 1.0f / (3.0f * fk) + ((k % 2 == 0) ? 1.0f / (3.0f * (1.0f + (fk - 1.0f) / 2.0f)) : 0.0f) +
             (((k - 1) % 4 == 3) ? 1.0f / (3.0f * (1.0f + (fk - 1.0f) / 4.0f)) : 0.0f);
    case SpTriple2:
      return 0.5f / fk + ((k % 2 == 0) ? 0.25f / (1.0f + (fk - 1.0f) / 2.0f) : 0.0f) +
             (((k - 1) % 4 == 3) ? 0.125f / (1.0f + (fk - 1.0f) / 4.0f) : 0.0f);
    case SpCustom:
      if (custom && !custom->level.empty()) {
        return k <= static_cast<int>(custom->level.size()) ? std::fabs(custom->level[static_cast<size_t>(k - 1)]) : 0.0f;
      }
      return 1.0f / fk;  // no table yet: a saw
    default: return 0.0f;
  }
}

float spectrumPhase(int sp, int k, const AdditiveData* custom) {
  switch (sp) {
    case SpDephased: return k % 2 == 0 ? 0.25f : 0.0f;
    case SpDephased2: return (k - 1) % 4 == 1 ? 0.5f : 0.0f;
    case SpSqSaw:
    case SpSqSaw2: return k % 2 == 0 ? 0.25f : 0.0f;
    case SpNotched: return k % 2 == 0 ? 0.75f : 0.0f;
    case SpCustom:
      if (custom && k <= static_cast<int>(custom->phase.size())) {
        const float p = custom->phase[static_cast<size_t>(k - 1)];
        return p - std::floor(p);
      }
      return 0.0f;
    default: return 0.0f;
  }
}

// ------------------------------------------------------------------ shapes
namespace {
// Steep slope: -72 dB over the table, forced to 0 at its end.
double steep(double t) { return std::pow(10.0, -72.0 * t / 20.0) * (1.0 - std::pow(t, 16.0)); }
// Flat top with raised-cosine² edges over the outer thirds.
double flatTop(double t) {
  if (t < 1.0 / 3) return std::pow(std::sin(0.5 * kPi * 3 * t), 2.0);
  if (t > 2.0 / 3) return std::pow(std::sin(0.5 * kPi * 3 * (1 - t)), 2.0);
  return 1.0;
}
struct Tab {
  double len, centre;
};
Tab filterTab(int shape) {
  switch (shape) {
    case AfLowPass: return {2048, 0};
    case AfBandPass: case AfBandStop: return {4096, 0.5};
    case AfHighPass: return {2048, 1};
    case AfSoftLp: return {1024, 0};
    case AfSoftBp: case AfSoftBs: return {2048, 0.5};
    case AfSoftHp: return {1024, 1};
    case AfHill: return {682, 0};
    case AfHybrid: return {1365, 0};
    case AfFin: case AfComb: case AfBandComb: return {4096, 0.5};
    case AfFlatBp: return {2048, 0.5};
    default: return {1024, 0};
  }
}
}  // namespace

float filterShapeT(int shape, double t) {
  t = std::clamp(t, 0.0, 1.0);
  const double u = 1.0 - t;
  switch (shape) {
    case AfOff: return 1.0f;
    case AfLowPass: return static_cast<float>(steep(t));
    case AfBandPass: return static_cast<float>(steep(std::fabs(2 * t - 1)));
    case AfBandStop: return static_cast<float>(1.0 - steep(std::fabs(2 * t - 1)));
    case AfHighPass: return static_cast<float>(steep(u));
    case AfSoftLp: return static_cast<float>(std::pow(1.0 - (std::cos(kPi * u) + 1) / 2, 2.0));
    case AfSoftBp: return static_cast<float>(std::pow(1.0 - (std::cos(2 * kPi * u) + 1) / 2, 2.0));
    case AfSoftBs: return static_cast<float>(std::pow((std::cos(2 * kPi * u) + 1) / 2, 2.0));
    case AfSoftHp: return static_cast<float>(std::pow((std::cos(kPi * u) + 1) / 2, 2.0));
    case AfHill: return static_cast<float>(-std::cos(kPi * (1 - t / 2)));
    case AfSlope: return static_cast<float>(0.25 * u + 0.00075 * (std::pow(1001.0, u) - 1));
    case AfHybrid: return static_cast<float>(steep(0.5 - 0.5 * std::cos(kPi * t)));
    case AfSnake: return static_cast<float>((1 - (std::cos(3 * kPi * u) + 1) / 2) * (1 - (std::cos(kPi * u) + 1) / 2));
    case AfFin: return static_cast<float>(t < 0.5 ? std::pow(1 - (std::cos(2 * kPi * u) + 1) / 4, 2.0) : steep(2 * t - 1));
    case AfLeaky: {
      const double v = 1 - (std::cos(kPi * u) + 1) / 2;
      return static_cast<float>(std::pow(10.0, (18 * v - 18) / 20));
    }
    case AfComb: return static_cast<float>(std::pow((std::cos(8 * kPi * u) + 1) / 2, 2.0));
    case AfBandComb: {
      const double b = 1 - (std::cos(2 * kPi * u) + 1) / 2;
      return static_cast<float>(0.125 * b + b * b * std::pow((std::cos(12 * kPi * u) + 1) / 2, 2.0));
    }
    case AfFlatBp: return static_cast<float>(flatTop(t));
    default: return static_cast<float>(steep(t));
  }
}

float filterShape(int shape, double oct, double widthOct) {
  if (shape == AfOff) return 1.0f;
  const Tab tb = filterTab(shape);
  const double t = tb.centre + oct * 1024.0 / std::max(0.01, widthOct) / (tb.len - 1);
  return filterShapeT(shape, t);
}

namespace {
struct ResTab {
  double len, scale;
};
ResTab resTab(int type) {
  switch (type) {
    case ArSpring: return {682, 9};
    case ArPedestal: return {341, 10};
    case ArHat: return {512, 18};
    case ArBump: return {2048, 5};
    case ArCone: return {1536, 5};
    case ArWell: return {768, 1};
    case ArHole: return {1024, 10};
    case ArNoise: return {1024, 3};
    default: return {1024, 7};
  }
}
}  // namespace

float resonanceShapeT(int type, double t, uint32_t seed, int k) {
  if (t < 0 || t > 1) return 0.0f;
  const double u = 1.0 - t, d = 2 * t - 1;
  double v;
  switch (type) {
    case ArSpring: {
      const double x = 2 * kPi * d * 2;
      v = (0.5 - 0.5 * std::cos(2 * kPi * t)) * (std::fabs(x) < 1e-9 ? 1.0 : std::sin(x) / x);
      break;
    }
    case ArPedestal: case ArBump: v = flatTop(t); break;
    case ArHat: v = steep(std::fabs(d)); break;
    case ArCone: v = std::pow((1 - std::cos(4 * kPi * u)) / 2, 4.0); break;
    case ArWell: v = -(1 - d * d); break;
    case ArHole: {
      const double x = 2 * kPi * d * 2;
      v = -(std::fabs(x) < 1e-9 ? 1.0 : std::sin(x) / x) * std::pow(0.5 - 0.5 * std::cos(2 * kPi * t), 0.125);
      break;
    }
    case ArNoise: v = hash01(seed * 31u + 7u, static_cast<uint32_t>(k)) * flatTop(t); break;
    default: v = std::pow((1 - std::cos(2 * kPi * u)) / 2, 3.0); break;
  }
  return static_cast<float>(v * resTab(type).scale);
}

float resonanceShape(int type, double oct, double widthOct, uint32_t seed, int k) {
  const ResTab tb = resTab(type);
  const double t = 0.5 + oct * 1024.0 / std::max(0.01, widthOct) / (tb.len - 1);
  return resonanceShapeT(type, t, seed, k);
}

float phaserShape(int shape, double ph) {
  ph -= std::floor(ph);
  const double s = std::sin(2 * kPi * ph);
  const double tri = ph < 0.5 ? 2 * ph : 2 - 2 * ph;
  switch (shape) {
    case ApTriangle: return static_cast<float>(tri * tri * 2);
    case ApEggs: return static_cast<float>(2 * std::sin(kPi * ph));
    case ApDeep: return static_cast<float>(2 * std::pow((s + 1) / 2, 6.0));
    case ApDeeper: return static_cast<float>(2 * steep(1 - tri));
    case ApNotch: return static_cast<float>(2 * steep(1 - std::pow((s + 1) / 2, 4.0)));
    case ApTwins: {
      const double s2 = std::sin(4 * kPi * ph);
      return static_cast<float>(2 * std::pow((s2 + 1) / 2, 4.0));
    }
    case ApCascade: return static_cast<float>((1 - ph) * (1 + std::sin(2 * kPi * (4 * ph + 0.75))));
    case ApBox: {
      const double q = 1 - std::pow(10.0, 6 * (1 - std::fabs(s))) / 1e6;
      return static_cast<float>(std::pow(1 + (s >= 0 ? q : -q), 3.0) / 4);
    }
    case ApFreq: return 1.0f;
    default: return static_cast<float>(1 + s);
  }
}

// ------------------------------------------------------------------ model
void AdditiveModel::read(const PluginState& s) {
  specA = std::clamp(prmi(s, AdSpectrumA, 0), 0, SpCount - 1);
  specB = std::clamp(prmi(s, AdSpectrumB, 1), 0, SpCount - 1);
  morph = prm(s, AdMorph, 0);
  partials = std::clamp(prmi(s, AdPartials, 128), 1, kAdMaxOsc);
  tilt = prm(s, AdTilt, 0);
  oddEven = prm(s, AdOddEven, 0);
  sub = prm(s, AdSub, 0);
  stretch = prm(s, AdStretch, 0);
  scatter = prm(s, AdScatter, 0);
  seed = prmi(s, AdSeed, 1);
  maskMix = prm(s, AdMaskMix, 0);
  fltType = std::clamp(prmi(s, AdFltType, 1), 0, AfCount - 1);
  cutoff = prm(s, AdFltCutoff, 4000);
  width = prm(s, AdFltWidth, 2);
  reso = prm(s, AdFltReso, 0);
  keyTrack = prm(s, AdFltKey, 0.5f);
  phMix = prm(s, AdPhMix, 0);
  phDensity = prm(s, AdPhDensity, 2);
  phOffset = prm(s, AdPhOffset, 0);
  pluck = prm(s, AdPluck, 0);
  pluckTime = prm(s, AdPluckTime, 1);
  for (int i = 0; i < kAdDrawn; ++i) drawn[i] = prm(s, AdDrawnBase + i, 1);
  for (int i = 0; i < kAdMask; ++i) mask[i] = prm(s, AdMaskBase + i, 1);
  for (int i = 0; i < kAdTeq; ++i) teq[i] = prm(s, AdTeqBase + i, 0);
  detune = prm(s, AdDetune, 0);
  protect = prm(s, AdProtect, 0);
  resType = std::clamp(prmi(s, AdResType, 0), 0, ArCount - 1);
  phType = std::clamp(prmi(s, AdPhType, 0), 0, ApCount - 1);
  fltScale = prmi(s, AdFltScale, 0);
  phScale = prmi(s, AdPhScale, 0);
  resOffset = prm(s, AdResOffset, 0);
  resOsc = prm(s, AdResOsc, 0);
  hzType = std::clamp(prmi(s, AdHzType, 0), 0, AhCount - 1);
  hzPos = std::clamp(prmi(s, AdHzPos, 0), 0, 3);
  hzMix = prm(s, AdHzMix, 0);
  hzWidth = prm(s, AdHzWidth, 0.5f);
  hzStrength = prm(s, AdHzStrength, 0);
  pluckMasked = prmi(s, AdPluckMask, 0) != 0;
  pluckAlt = prmi(s, AdPluckAlt, 0) != 0;
  resScale = prmi(s, AdResScale, 0);
}

double AdditiveModel::ratio(int k) const {
  double r = std::pow(static_cast<double>(k), 1.0 + 0.12 * stretch);
  if (k > 1 && scatter > 0) r *= 1.0 + scatter * 0.06 * (hash01(static_cast<uint32_t>(seed) + 977u, static_cast<uint32_t>(k)) * 2.0 - 1.0);
  // Detuning: cents at the 16th partial, growing with log2(k) (0 at the fundamental).
  if (detune != 0 && k > 1) r *= std::pow(2.0, detune / 1200.0 * std::log2(static_cast<double>(k)) / 4.0);
  return r;
}

float AdditiveModel::timbre(int k, float m) const {
  if (k < 1 || k > partials) return 0.0f;
  float a = spectrumLevel(specA, k, seed, custom) * (k <= kAdDrawn ? drawn[k - 1] : 1.0f);
  float b = spectrumLevel(specB, k, seed, custom);
  m = std::clamp(m, 0.0f, 1.0f);
  float v = a + (b - a) * m;
  if (tilt != 0.0f) v *= std::pow(static_cast<float>(k), tilt / 6.0206f);
  if (k > 1) {
    if (k & 1)
      v *= 1.0f - std::max(0.0f, -oddEven);
    else
      v *= 1.0f - std::max(0.0f, oddEven);
  }
  // Timbre EQ: six raised-cosine bands centred on harmonics 1, 2, 4 … 32
  // (outer bands are shelves); gain per band 0 .. 4 (-inf .. +12 dB).
  bool eq = false;
  for (float g : teq) eq = eq || g != 0.0f;
  if (eq) v *= static_cast<float>(teqGain(std::log2(static_cast<double>(k))));
  if (maskMix > 0) v *= 1.0f + (mask[(k - 1) % kAdMask] - 1.0f) * maskMix;
  return v;
}

double AdditiveModel::teqGain(double oct) const {
  double sum = 0;
  for (int b = 0; b < kAdTeq; ++b) {
    double d = oct - b;
    if ((b == 0 && d < 0) || (b == kAdTeq - 1 && d > 0)) d = 0;
    if (std::fabs(d) >= 1) continue;
    const double w = (std::cos(kPi * d) + 1) / 2;
    const double g = 0.5 * (std::pow(3.0, std::clamp(teq[b] + 1.0, 0.0, 2.0)) - 1.0);
    sum += w * g;
  }
  return sum;
}

float AdditiveModel::phase(int k, float m) const {
  const float pa = spectrumPhase(specA, k, custom), pb = spectrumPhase(specB, k, custom);
  return m < 0.5f ? pa : pb;
}

float AdditiveModel::filter(double hz, double cut, float resoNow, double widthMul, bool withRes) const {
  if (fltType == AfOff) return 1.0f;
  const double w = std::max(0.05, static_cast<double>(width) * widthMul);
  double oct;
  if (fltScale == 1)
    oct = (hz / std::max(cut, 1.0) - 1.0) * 0.9;  // linear-Hz axis
  else
    oct = std::log2(std::max(hz, 1.0) / std::max(cut, 1.0));
  double g = filterShape(fltType, oct, w);
  const double r = resoNow < 0 ? reso : resoNow;
  if (withRes && r > 0) {
    // Resonance centred on cutoff + offset; its own table width (1 octave per 1024).
    const double rcHz = cut * std::pow(2.0, resOffset / 12.0);
    const double ro = resScale == 1 ? (hz / std::max(rcHz, 1.0) - 1.0) * 0.384 * rcHz / 1024.0 : std::log2(std::max(hz, 1.0) / std::max(rcHz, 1.0));
    const double amt = 0.25 * 0.1 * (std::pow(11.0, std::clamp(r, 0.0, 1.0)) - 1.0);
    g += amt * resonanceShape(resType, ro, resWidth, static_cast<uint32_t>(seed), static_cast<int>(hz));
  }
  return static_cast<float>(std::max(0.0, g));
}

float AdditiveModel::phaser(double hz, double off, double densityMul) const {
  if (phMix <= 0 || phType == ApFreq) return 1.0f;
  const double d = phDensity * densityMul;
  double ph;
  if (phScale == 1)
    ph = hz / 1000.0 * d * 0.5 + off;  // linear-Hz comb (evenly spaced notches)
  else if (phScale == 2)
    ph = hz / std::max(1.0, f0ref) * d * 0.25 + off;  // harmonic axis
  else
    ph = std::log2(std::max(hz, 1.0) / 20.0) * d + off;
  return static_cast<float>(phMix * phaserShape(phType, ph) + (1.0 - phMix));
}

float AdditiveModel::pluckGain(double hz, double f0, double t, double timeMul) const {
  if (pluck <= 0) return 1.0f;
  double rel = std::pow(200.0 / std::max(hz, 20.0), 0.8);
  if (pluckAlt) rel = std::pow(std::max(hz, 20.0) / std::max(f0, 20.0), 0.35) * 0.5;  // alternate: upper partials ring longer
  const double tau = pluckTime * timeMul * std::min(4.0, rel);
  return static_cast<float>(1.0 - pluck + pluck * std::exp(-t / std::max(1e-3, tau)));
}

void AdditiveModel::harmonize(float* lvl, int n, double widthNow, float* tmp) const {
  if (hzMix <= 0 || n < 2) return;
  // Copies of the spectrum moved up the series; copy k fades with
  // r = 1 - k / (d · width + 1); gains normalised by 1 / (1 + Σ gain).
  static const int kA[AhCount] = {-1, 0, 1, -1, 0}, kD[AhCount] = {5, 5, 5, 10, 10};
  const int a = kA[hzType], d = kD[hzType];
  const double strength = 1.331 * std::pow(2.0, 4.0 * std::clamp(static_cast<double>(hzStrength), 0.0, 1.0));
  const double mix2 = static_cast<double>(hzMix) * hzMix;
  for (int i = 0; i < n; ++i) tmp[i] = lvl[i];
  double sum = 0;
  for (int k = 1; k < 64; ++k) {
    const double r = 1.0 - k / (d * std::max(0.0, widthNow) + 1.0);
    if (r <= 0) break;
    const double g = std::min(1.0, r * r * r * strength) * mix2;
    sum += g;
    const int step = hzType <= AhOct2 ? (1 << std::min(k, 10)) : hzType == AhHarm ? k + 1 : 2 * k;
    for (int src = 1; src <= n; ++src) {
      const long t = static_cast<long>(src) * step + a + 1;
      if (t < 1 || t > n) break;
      lvl[t - 1] += static_cast<float>(g * tmp[src - 1]);
    }
  }
  const float norm = static_cast<float>(1.0 / (1.0 + sum));
  for (int i = 0; i < n; ++i) lvl[i] *= norm;
}

float AdditiveModel::protectGain(int k, float g) const {
  (void)k;
  return g;
}

// ------------------------------------------------------------------ data
bool parseAdditiveData(const std::string& text, AdditiveData& out) {
  out = AdditiveData{};
  std::istringstream in(text);
  std::string line;
  if (!std::getline(in, line) || line.rfind("add ", 0) != 0) return false;
  while (std::getline(in, line)) {
    std::istringstream ls(line);
    std::string tag;
    int n = 0;
    if (!(ls >> tag >> n) || n < 0 || n > kAdCustomMax) continue;
    std::vector<float> v;
    float x;
    for (int i = 0; i < n && ls >> x; ++i) v.push_back(std::isfinite(x) ? x : 0.0f);
    if (tag == "c") out.level = std::move(v);
    else if (tag == "p") out.phase = std::move(v);
  }
  return true;
}

std::string serializeAdditiveData(const AdditiveData& d) {
  if (d.level.empty() && d.phase.empty()) return "";
  std::string s = "add 1\n";
  char buf[32];
  auto row = [&](const char* tag, const std::vector<float>& v) {
    if (v.empty()) return;
    s += tag;
    s += " " + std::to_string(std::min<size_t>(v.size(), kAdCustomMax));
    for (size_t i = 0; i < v.size() && i < static_cast<size_t>(kAdCustomMax); ++i) {
      std::snprintf(buf, sizeof buf, " %.6g", static_cast<double>(v[i]));
      s += buf;
    }
    s += "\n";
  };
  row("c", d.level);
  row("p", d.phase);
  return s;
}

AdditiveData analyzeCycle(const float* x, int n, int harmonics) {
  AdditiveData d;
  if (!x || n < 4) return d;
  harmonics = std::clamp(harmonics, 1, std::min(kAdCustomMax, n / 2));
  double peak = 0;
  std::vector<double> lv(static_cast<size_t>(harmonics)), ph(static_cast<size_t>(harmonics));
  for (int k = 1; k <= harmonics; ++k) {
    double re = 0, im = 0;
    for (int i = 0; i < n; ++i) {
      const double a = 2 * kPi * k * i / n;
      re += x[i] * std::cos(a);
      im += x[i] * std::sin(a);
    }
    const double mag = 2.0 * std::sqrt(re * re + im * im) / n;
    lv[static_cast<size_t>(k - 1)] = mag;
    // Phase of a sine partial: x ≈ mag · sin(2πk t + φ) → φ = atan2(re, im).
    double p = std::atan2(re, im) / (2 * kPi);
    ph[static_cast<size_t>(k - 1)] = p - std::floor(p);
    peak = std::max(peak, mag);
  }
  for (int k = 0; k < harmonics; ++k) {
    d.level.push_back(static_cast<float>(peak > 0 ? lv[static_cast<size_t>(k)] / peak : 0.0));
    d.phase.push_back(static_cast<float>(ph[static_cast<size_t>(k)]));
  }
  return d;
}

}  // namespace gl::packs::models
