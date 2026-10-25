// Gridline — "plasma" pack: FFT, Plasma's own factory wavetables (generated
// from formulas: no sampled content), band-limited mip maps, wavetable file
// import (single-cycle and multi-frame WAV), generated noise sources and
// impulse responses, SFZ multisample parsing, and the waveshapers shared by
// the warp menu and the Distortion module.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <sstream>
#include "plasma_dsp.h"

namespace gl::packs::plasma {

// ------------------------------------------------------------------ FFT
void Fft::init(int n) {
  n_ = n;
  tw_.resize(static_cast<size_t>(n / 2));
  for (int i = 0; i < n / 2; ++i) tw_[static_cast<size_t>(i)] = std::polar(1.0f, static_cast<float>(-kPi2 * i / n));
  rev_.resize(static_cast<size_t>(n));
  int bits = 0;
  while ((1 << bits) < n) ++bits;
  for (int i = 0; i < n; ++i) {
    int r = 0;
    for (int b = 0; b < bits; ++b)
      if (i & (1 << b)) r |= 1 << (bits - 1 - b);
    rev_[static_cast<size_t>(i)] = r;
  }
}
void Fft::run(cpx* x, bool inv) const {
  const int n = n_;
  for (int i = 0; i < n; ++i) {
    const int r = rev_[static_cast<size_t>(i)];
    if (r > i) std::swap(x[i], x[r]);
  }
  for (int len = 2; len <= n; len <<= 1) {
    const int half = len >> 1, step = n / len;
    for (int i = 0; i < n; i += len)
      for (int j = 0; j < half; ++j) {
        cpx w = tw_[static_cast<size_t>(j * step)];
        if (inv) w = std::conj(w);
        const cpx u = x[i + j], v = x[i + j + half] * w;
        x[i + j] = u + v;
        x[i + j + half] = u - v;
      }
  }
  if (inv) {
    const float s = 1.0f / static_cast<float>(n);
    for (int i = 0; i < n; ++i) x[i] *= s;
  }
}
void Fft::forward(cpx* x) const { run(x, false); }
void Fft::inverse(cpx* x) const { run(x, true); }
const Fft& fft2048() {
  static const Fft f = [] {
    Fft x;
    x.init(2048);
    return x;
  }();
  return f;
}

// ------------------------------------------------------------------ mip maps
std::shared_ptr<const MipTable> buildMip(const std::shared_ptr<const Table>& t) {
  if (!t || t->frames <= 0) return nullptr;
  auto m = std::make_shared<MipTable>();
  m->frames = t->frames;
  m->raw = t;
  m->name = t->name;
  std::array<Fft, MipTable::kLevels> ffts;
  for (int l = 0; l < MipTable::kLevels; ++l) {
    const int H = MipTable::kHarm >> l;
    const int len = std::clamp(4 * H, 64, 2048);
    m->len[static_cast<size_t>(l)] = len;
    m->data[static_cast<size_t>(l)].assign(static_cast<size_t>(t->frames) * static_cast<size_t>(len + 1), 0.f);
    ffts[static_cast<size_t>(l)].init(len);
  }
  m->mag.assign(static_cast<size_t>(t->frames) * MipTable::kHarm, 0.f);
  m->phs.assign(static_cast<size_t>(t->frames) * MipTable::kHarm, 0.f);
  const Fft& big = fft2048();
  std::vector<cpx> spec(kFrameSize), buf(kFrameSize);
  for (int f = 0; f < t->frames; ++f) {
    const float* src = t->frame(f);
    for (int i = 0; i < kFrameSize; ++i) spec[static_cast<size_t>(i)] = cpx(src[i], 0.f);
    big.forward(spec.data());
    for (int h = 1; h <= MipTable::kHarm; ++h) {
      const cpx c = h < kFrameSize / 2 ? spec[static_cast<size_t>(h)] : cpx(0, 0);
      m->mag[static_cast<size_t>(f) * MipTable::kHarm + static_cast<size_t>(h - 1)] = std::abs(c) * (2.0f / kFrameSize);
      m->phs[static_cast<size_t>(f) * MipTable::kHarm + static_cast<size_t>(h - 1)] = std::arg(c);
    }
    for (int l = 0; l < MipTable::kLevels; ++l) {
      const int H = MipTable::kHarm >> l;
      const int len = m->len[static_cast<size_t>(l)];
      const int top = std::min(H, len / 2 - 1);
      std::fill(buf.begin(), buf.begin() + len, cpx(0, 0));
      const float scale = static_cast<float>(len) / kFrameSize;
      for (int h = 1; h <= top && h < kFrameSize / 2; ++h) {
        buf[static_cast<size_t>(h)] = spec[static_cast<size_t>(h)] * scale;
        buf[static_cast<size_t>(len - h)] = std::conj(spec[static_cast<size_t>(h)]) * scale;
      }
      ffts[static_cast<size_t>(l)].inverse(buf.data());
      float* dst = m->data[static_cast<size_t>(l)].data() + static_cast<size_t>(f) * static_cast<size_t>(len + 1);
      for (int i = 0; i < len; ++i) dst[i] = buf[static_cast<size_t>(i)].real();
      dst[len] = dst[0];
    }
  }
  return m;
}

// ------------------------------------------------------------------ factory tables (formulas)
namespace {
using FrameFn = void (*)(int f, int frames, float* out);  // out: kFrameSize samples

double ph(int i) { return static_cast<double>(i) / kFrameSize; }
// additive frame: amplitude per harmonic
void additive(float* out, int count, const std::function<double(int)>& amp, const std::function<double(int)>& phase = nullptr) {
  // x[n] = sum a_h sin(2 pi h n / N + p_h), built with one inverse FFT
  std::vector<cpx> X(kFrameSize, cpx(0, 0));
  for (int h = 1; h <= count && h < kFrameSize / 2; ++h) {
    double a = amp(h);
    if (std::fabs(a) < 1e-9) continue;
    double p = (phase ? phase(h) : 0.0) - kPi2 * 0.25;
    if (a < 0) a = -a, p += kPi2 * 0.5;  // std::polar needs a non-negative magnitude
    const cpx c = std::polar(static_cast<float>(a * kFrameSize * 0.5), static_cast<float>(p));
    X[static_cast<size_t>(h)] = c;
    X[static_cast<size_t>(kFrameSize - h)] = std::conj(c);
  }
  fft2048().inverse(X.data());
  for (int i = 0; i < kFrameSize; ++i) out[i] = X[static_cast<size_t>(i)].real();
}
double sawAmp(int h) { return 1.0 / h; }
double sqAmp(int h) { return (h & 1) ? 1.0 / h : 0.0; }
double triAmp(int h) { return (h & 1) ? ((((h - 1) / 2) & 1) ? -1.0 : 1.0) / (static_cast<double>(h) * h) : 0.0; }
float lerpf(float a, float b, float t) { return a + (b - a) * t; }

void basicShapes(int f, int frames, float* out) {
  // saw -> square -> triangle -> sine (crossfades of band-limited shapes)
  static std::vector<float> shapes;
  static std::once_flag once;
  std::call_once(once, [] {
    shapes.assign(4 * kFrameSize, 0.f);
    additive(shapes.data(), 1023, sawAmp);
    additive(shapes.data() + kFrameSize, 1023, sqAmp);
    additive(shapes.data() + 2 * kFrameSize, 1023, triAmp);
    additive(shapes.data() + 3 * kFrameSize, 1, [](int) { return 1.0; });
  });
  const double pos = static_cast<double>(f) / std::max(1, frames - 1) * 3.0;
  const int a = std::min(2, static_cast<int>(pos));
  const float t = static_cast<float>(pos - a);
  for (int i = 0; i < kFrameSize; ++i)
    out[i] = lerpf(shapes[static_cast<size_t>(a * kFrameSize + i)], shapes[static_cast<size_t>((a + 1) * kFrameSize + i)], t);
}
void sawStack(int f, int frames, float* out) {
  // saw whose upper harmonics gain a moving comb (brightness + hollow sweep)
  const double t = static_cast<double>(f) / std::max(1, frames - 1);
  additive(out, 1023, [t](int h) { return (1.0 / h) * (0.55 + 0.45 * std::cos(kPi2 * h * (0.02 + 0.2 * t))) * std::pow(h, -0.3 * (1 - t)); });
}
void squarePwm(int f, int frames, float* out) {
  const double w = 0.5 - 0.47 * static_cast<double>(f) / std::max(1, frames - 1);
  additive(out, 1023, [w](int h) { return 2.0 / (kPi2 * 0.5 * h) * std::sin(kPi2 * 0.5 * h * w); }, [](int) { return kPi2 * 0.25; });
}
void sineHarmonics(int f, int frames, float* out) {
  const int n = 1 + f * 63 / std::max(1, frames - 1);
  additive(out, n, [n](int h) { return h <= n ? 1.0 / std::sqrt(static_cast<double>(h)) : 0.0; });
}
void analogMorph(int f, int frames, float* out) {
  const double t = static_cast<double>(f) / std::max(1, frames - 1);
  // soft saw -> round square -> rounded triangle; gentle analog rolloff
  additive(out, 1023, [t](int h) {
    const double roll = 1.0 / (1.0 + std::pow(h / (8.0 + 40.0 * (1 - t)), 2.0));
    const double saw = 1.0 / h, sq = (h & 1) ? 1.0 / h : 0.0, tri = triAmp(h);
    const double a = t < 0.5 ? saw + (sq - saw) * (t * 2) : sq + (tri - sq) * ((t - 0.5) * 2);
    return a * roll;
  }, [](int h) { return 0.07 * h; });
}
double formantGain(double hz, const double* fr, const double* bw) {
  double g = 0;
  for (int k = 0; k < 3; ++k) {
    const double d = (hz - fr[k]) / bw[k];
    g += std::exp(-d * d) * (k == 0 ? 1.0 : k == 1 ? 0.7 : 0.45);
  }
  return g;
}
void vocalFormants(int f, int frames, float* out) {
  static const double V[5][3] = {{730, 1090, 2440}, {530, 1840, 2480}, {270, 2290, 3010}, {570, 840, 2410}, {300, 870, 2240}};
  const double pos = static_cast<double>(f) / std::max(1, frames - 1) * 4.0;
  const int a = std::min(3, static_cast<int>(pos));
  const double t = pos - a;
  double fr[3], bw[3] = {90, 120, 170};
  for (int k = 0; k < 3; ++k) fr[k] = V[a][k] + (V[a + 1][k] - V[a][k]) * t;
  const double f0 = 110.0;  // formants placed for a 110 Hz reference
  additive(out, 1023, [&](int h) { return formantGain(h * f0, fr, bw) * 0.25 + 0.02 / h; });
}
void glassBells(int f, int frames, float* out) {
  const double t = static_cast<double>(f) / std::max(1, frames - 1);
  static const int partials[] = {1, 3, 4, 7, 9, 12, 17, 23, 31, 42};
  additive(out, 64, [t](int h) {
    for (int k = 0; k < 10; ++k)
      if (partials[k] == h) return std::pow(0.72 + 0.2 * t, k) * (k == 0 ? 1.0 : 0.8);
    return 0.0;
  });
}
void organDrawbars(int f, int frames, float* out) {
  static const int harm[9] = {1, 2, 3, 4, 6, 8, 10, 12, 16};
  static const float regs[4][9] = {{8, 8, 8, 0, 0, 0, 0, 0, 0}, {8, 8, 6, 8, 0, 4, 0, 0, 0}, {8, 6, 8, 6, 4, 6, 3, 4, 2}, {6, 8, 8, 8, 8, 8, 8, 8, 8}};
  const double pos = static_cast<double>(f) / std::max(1, frames - 1) * 3.0;
  const int a = std::min(2, static_cast<int>(pos));
  const double t = pos - a;
  additive(out, 16, [&](int h) {
    for (int k = 0; k < 9; ++k)
      if (harm[k] == h) return (regs[a][k] + (regs[a + 1][k] - regs[a][k]) * t) / 8.0;
    return 0.0;
  });
}
void digitalGrowl(int f, int frames, float* out) {
  const double t = static_cast<double>(f) / std::max(1, frames - 1);
  const double idx = 0.2 + 7.0 * t * t;
  for (int i = 0; i < kFrameSize; ++i) {
    const double p = kPi2 * ph(i);
    double v = std::sin(p + idx * std::sin(2 * p) + 0.5 * idx * std::sin(3 * p + 0.3));
    v = std::tanh(v * (1 + 3 * t)) / std::tanh(1 + 3 * t);
    out[i] = static_cast<float>(v);
  }
}
void electricKeys(int f, int frames, float* out) {
  const double t = static_cast<double>(f) / std::max(1, frames - 1);
  const double idx = 3.0 * (1 - t);
  for (int i = 0; i < kFrameSize; ++i) {
    const double p = kPi2 * ph(i);
    out[i] = static_cast<float>(0.8 * std::sin(p + idx * std::sin(p)) + 0.2 * std::sin(14 * p) * (1 - t));
  }
}
void formantSweep(int f, int frames, float* out) {
  const double c = 2.0 + 60.0 * static_cast<double>(f) / std::max(1, frames - 1);
  additive(out, 1023, [c](int h) {
    const double d = (h - c) / (0.12 * c + 1.5);
    return (1.0 / h) * 0.25 + std::exp(-d * d) / std::sqrt(static_cast<double>(h));
  });
}
void syncSweep(int f, int frames, float* out) {
  const double r = 1.0 + 7.0 * static_cast<double>(f) / std::max(1, frames - 1);
  for (int i = 0; i < kFrameSize; ++i) {
    const double p = ph(i) * r;
    out[i] = static_cast<float>(1.0 - 2.0 * (p - std::floor(p))) * static_cast<float>(1.0 - ph(i) * 0.3);
  }
}
void waveFold(int f, int frames, float* out) {
  const double g = 1.0 + 9.0 * static_cast<double>(f) / std::max(1, frames - 1);
  for (int i = 0; i < kFrameSize; ++i) {
    double x = std::sin(kPi2 * ph(i)) * g;
    x = std::asin(std::sin(x * kPi2 * 0.25)) / (kPi2 * 0.25);
    out[i] = static_cast<float>(x);
  }
}
void chebyshev(int f, int frames, float* out) {
  const double pos = 1.0 + 11.0 * static_cast<double>(f) / std::max(1, frames - 1);
  const int a = static_cast<int>(pos);
  const double t = pos - a;
  for (int i = 0; i < kFrameSize; ++i) {
    const double x = std::cos(kPi2 * ph(i));
    out[i] = static_cast<float>(std::cos(a * std::acos(x)) * (1 - t) + std::cos((a + 1) * std::acos(x)) * t);
  }
}
void pulseTrain(int f, int frames, float* out) {
  const int n = 4 + f * 250 / std::max(1, frames - 1);
  additive(out, n, [](int) { return 1.0; }, [](int) { return kPi2 * 0.25; });
}
void hollowPipes(int f, int frames, float* out) {
  const double t = static_cast<double>(f) / std::max(1, frames - 1);
  additive(out, 255, [t](int h) { return (h & 1) ? std::pow(h, -1.2) * (1 + std::sin(h * (0.3 + t))) : 0.04 * t / h; });
}
void metallic(int f, int frames, float* out) {
  static const int primes[] = {1, 2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37, 41, 43, 47, 53, 59, 61, 67};
  const double t = static_cast<double>(f) / std::max(1, frames - 1);
  additive(out, 67, [t](int h) {
    for (int k = 0; k < 20; ++k)
      if (primes[k] == h) return std::pow(0.85, k * (1.5 - t)) * ((k & 1) ? 0.7 : 1.0);
    return 0.0;
  });
}
void bitSteps(int f, int frames, float* out) {
  const int steps = std::max(2, 32 - f * 30 / std::max(1, frames - 1));
  for (int i = 0; i < kFrameSize; ++i) {
    const double x = std::sin(kPi2 * ph(i));
    out[i] = static_cast<float>(std::round(x * steps * 0.5) / (steps * 0.5));
  }
}
void spectralNoise(int f, int, float* out) {
  uint32_t s = 0x2545F491u + static_cast<uint32_t>(f) * 977u;
  std::vector<double> amp(512), phs(512);
  for (int h = 0; h < 512; ++h) {
    amp[static_cast<size_t>(h)] = rngUni(s) / std::sqrt(h + 1.0);
    phs[static_cast<size_t>(h)] = rngUni(s) * kPi2;
  }
  additive(out, 512, [&](int h) { return amp[static_cast<size_t>(h - 1)]; }, [&](int h) { return phs[static_cast<size_t>(h - 1)]; });
}
void additiveBright(int f, int frames, float* out) {
  const double t = static_cast<double>(f) / std::max(1, frames - 1);
  const double tilt = 2.5 - 2.0 * t;
  additive(out, 1023, [tilt](int h) { return std::pow(h, -tilt); });
}
void squareSaw(int f, int frames, float* out) {
  const double t = static_cast<double>(f) / std::max(1, frames - 1);
  additive(out, 1023, [t](int h) { return ((h & 1) ? 1.0 : t) / h; });
}
void resoSweep(int f, int frames, float* out) {
  // saw through a resonant low-pass: the resonance walks up the spectrum
  const double c = 1.5 + 80.0 * std::pow(static_cast<double>(f) / std::max(1, frames - 1), 2.0);
  additive(out, 1023, [c](int h) {
    const double x = h / c;
    return (1.0 / h) / std::sqrt((1 - x * x) * (1 - x * x) + 0.02 * x * x);
  });
}

struct FactoryDef {
  const char* name;
  int frames;
  FrameFn fn;
};
const FactoryDef kFactory[] = {
    {"Basic Shapes", 64, basicShapes},   {"Saw Stack", 32, sawStack},           {"Square PWM", 32, squarePwm},
    {"Analog Morph", 32, analogMorph},   {"Sine Harmonics", 32, sineHarmonics}, {"Vocal Formants", 32, vocalFormants},
    {"Glass Bells", 16, glassBells},     {"Organ Drawbars", 16, organDrawbars}, {"Digital Growl", 32, digitalGrowl},
    {"Electric Keys", 16, electricKeys}, {"Formant Sweep", 32, formantSweep},   {"Sync Sweep", 32, syncSweep},
    {"Wave Fold", 32, waveFold},         {"Chebyshev", 32, chebyshev},          {"Pulse Train", 16, pulseTrain},
    {"Hollow Pipes", 16, hollowPipes},   {"Metallic", 16, metallic},            {"Bit Steps", 16, bitSteps},
    {"Spectral Noise", 16, spectralNoise}, {"Additive Tilt", 16, additiveBright}, {"Square to Saw", 16, squareSaw},
    {"Resonance Sweep", 32, resoSweep},
};
constexpr int kFactoryCount = static_cast<int>(sizeof(kFactory) / sizeof(kFactory[0]));

// Factory tables: every frame peaks at full scale (even level while scanning).
void normalizeTable(Table& t) {
  for (int f = 0; f < t.frames; ++f) {
    float* p = t.data.data() + static_cast<size_t>(f) * kFrameSize;
    float peak = 0;
    for (int i = 0; i < kFrameSize; ++i) peak = std::max(peak, std::fabs(p[i]));
    if (peak > 1e-9f)
      for (int i = 0; i < kFrameSize; ++i) p[i] /= peak;
  }
}
}  // namespace

const std::vector<std::string>& factoryTableNames() {
  static const std::vector<std::string> v = [] {
    std::vector<std::string> n;
    for (const auto& d : kFactory) n.emplace_back(d.name);
    return n;
  }();
  return v;
}

std::shared_ptr<const Table> factoryTable(int index) {
  static std::array<std::shared_ptr<const Table>, kFactoryCount> cache;
  static std::mutex mu;
  if (index < 0 || index >= kFactoryCount) index = 0;
  std::lock_guard<std::mutex> lk(mu);
  auto& c = cache[static_cast<size_t>(index)];
  if (!c) {
    auto t = std::make_shared<Table>();
    const FactoryDef& d = kFactory[index];
    t->name = d.name;
    t->frames = d.frames;
    t->data.assign(static_cast<size_t>(d.frames) * kFrameSize, 0.f);
    for (int f = 0; f < d.frames; ++f) d.fn(f, d.frames, t->data.data() + static_cast<size_t>(f) * kFrameSize);
    // remove DC per frame
    for (int f = 0; f < d.frames; ++f) {
      float* p = t->data.data() + static_cast<size_t>(f) * kFrameSize;
      double m = 0;
      for (int i = 0; i < kFrameSize; ++i) m += p[i];
      m /= kFrameSize;
      for (int i = 0; i < kFrameSize; ++i) p[i] -= static_cast<float>(m);
    }
    normalizeTable(*t);
    c = t;
  }
  return c;
}

const std::vector<std::shared_ptr<const MipTable>>& factoryMips() {
  static const std::vector<std::shared_ptr<const MipTable>> v = [] {
    std::vector<std::shared_ptr<const MipTable>> out;
    for (int i = 0; i < kFactoryCount; ++i) out.push_back(buildMip(factoryTable(i)));
    return out;
  }();
  return v;
}

std::shared_ptr<const Table> tableFromFrames(const std::vector<std::vector<float>>& frames, const std::string& name) {
  if (frames.empty()) return nullptr;
  auto t = std::make_shared<Table>();
  t->name = name;
  t->frames = static_cast<int>(std::min<size_t>(frames.size(), 256));
  t->data.assign(static_cast<size_t>(t->frames) * kFrameSize, 0.f);
  for (int f = 0; f < t->frames; ++f) {
    const auto& src = frames[static_cast<size_t>(f)];
    float* dst = t->data.data() + static_cast<size_t>(f) * kFrameSize;
    if (src.size() == static_cast<size_t>(kFrameSize)) {
      std::copy(src.begin(), src.end(), dst);
    } else if (!src.empty()) {  // resample one cycle (linear, wrap)
      const double r = static_cast<double>(src.size()) / kFrameSize;
      for (int i = 0; i < kFrameSize; ++i) {
        const double x = i * r;
        const size_t a = static_cast<size_t>(x) % src.size(), b = (a + 1) % src.size();
        dst[i] = static_cast<float>(src[a] + (src[b] - src[a]) * (x - std::floor(x)));
      }
    }
  }
  return t;
}

namespace {
// Cycle length from a wavetable WAV's 'clm ' chunk ("<!>2048 ..."), else 0.
int clmCycle(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return 0;
  char hdr[12];
  if (!f.read(hdr, 12) || std::memcmp(hdr, "RIFF", 4) != 0 || std::memcmp(hdr + 8, "WAVE", 4) != 0) return 0;
  for (int guard = 0; guard < 64; ++guard) {
    char id[4];
    uint8_t sz[4];
    if (!f.read(id, 4) || !f.read(reinterpret_cast<char*>(sz), 4)) return 0;
    const uint32_t n = static_cast<uint32_t>(sz[0]) | (static_cast<uint32_t>(sz[1]) << 8) | (static_cast<uint32_t>(sz[2]) << 16) |
                       (static_cast<uint32_t>(sz[3]) << 24);
    if (std::memcmp(id, "clm ", 4) == 0 && n < 4096) {
      std::string s(n, '\0');
      if (!f.read(s.data(), n)) return 0;
      const size_t p = s.find("<!>");
      if (p == std::string::npos) return 0;
      int v = 0;
      for (size_t i = p + 3; i < s.size() && s[i] >= '0' && s[i] <= '9' && v < 100000; ++i) v = v * 10 + (s[i] - '0');
      return v;
    }
    f.seekg(static_cast<std::streamoff>(n + (n & 1)), std::ios::cur);
    if (!f) return 0;
  }
  return 0;
}
}  // namespace

std::shared_ptr<const Table> loadTableFile(const std::string& path, std::string* err, int channel) {
  SamplePtr s = loadSample(path, err);
  if (!s || s->frames() == 0) return nullptr;
  const std::vector<float>& src = channel == 1 && !s->mono() ? s->right.vec() : s->left;
  const size_t n = src.size();
  int cyc = clmCycle(path);
  if (cyc <= 0) {
    if (n <= 4096 && n % kFrameSize != 0) cyc = static_cast<int>(n);  // one single cycle
    else cyc = kFrameSize;
  }
  const size_t frames = std::clamp<size_t>(n / static_cast<size_t>(cyc), 1, 256);
  std::vector<std::vector<float>> fr(frames);
  for (size_t f = 0; f < frames; ++f) {
    const size_t a = f * static_cast<size_t>(cyc);
    const size_t b = std::min(n, a + static_cast<size_t>(cyc));
    fr[f].assign(src.begin() + static_cast<std::ptrdiff_t>(a), src.begin() + static_cast<std::ptrdiff_t>(b));
    fr[f].resize(static_cast<size_t>(cyc), 0.f);
  }
  std::string name = path;
  const size_t sl = name.find_last_of("/\\");
  if (sl != std::string::npos) name = name.substr(sl + 1);
  const size_t dot = name.find_last_of('.');
  if (dot != std::string::npos) name = name.substr(0, dot);
  auto t = tableFromFrames(fr, name);
  return t;
}

// ------------------------------------------------------------------ noises (generated)
namespace {
void onePoleLp(std::vector<float>& v, double a) {
  double z = 0;
  for (float& x : v) x = static_cast<float>(z = z + (x - z) * a);
}
void normalize(std::vector<float>& v, float peak = 0.9f) {
  float m = 0;
  for (float x : v) m = std::max(m, std::fabs(x));
  if (m > 1e-9f)
    for (float& x : v) x *= peak / m;
}
}  // namespace

const std::vector<NoiseSrc>& factoryNoises() {
  static const std::vector<NoiseSrc> v = [] {
    const int sr = 44100, n = sr * 2;
    std::vector<NoiseSrc> out;
    auto add = [&](const char* name, bool loop, auto fn) {
      NoiseSrc s;
      s.name = name;
      s.loop = loop;
      s.L.assign(static_cast<size_t>(n), 0.f);
      uint32_t r = 0x1234567u + static_cast<uint32_t>(out.size()) * 7919u;
      fn(s.L, r);
      normalize(s.L);
      // loop seam: short crossfade of the tail into the head
      if (loop) {
        const int x = 2048;
        for (int i = 0; i < x; ++i) {
          const float w = static_cast<float>(i) / x;
          s.L[static_cast<size_t>(n - x + i)] = s.L[static_cast<size_t>(n - x + i)] * (1 - w) + s.L[static_cast<size_t>(i)] * w;
        }
      }
      out.push_back(std::move(s));
    };
    add("White", true, [](std::vector<float>& v, uint32_t& r) { for (float& x : v) x = rngBi(r); });
    add("Pink", true, [](std::vector<float>& v, uint32_t& r) {
      double b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0;
      for (float& x : v) {
        const double w = rngBi(r);
        b0 = 0.99886 * b0 + w * 0.0555179; b1 = 0.99332 * b1 + w * 0.0750759; b2 = 0.96900 * b2 + w * 0.1538520;
        b3 = 0.86650 * b3 + w * 0.3104856; b4 = 0.55000 * b4 + w * 0.5329522; b5 = -0.7616 * b5 - w * 0.0168980;
        x = static_cast<float>(b0 + b1 + b2 + b3 + b4 + b5 + b6 + w * 0.5362);
        b6 = w * 0.115926;
      }
    });
    add("Brown", true, [](std::vector<float>& v, uint32_t& r) {
      double z = 0;
      for (float& x : v) {
        z = (z + 0.02 * rngBi(r)) * 0.998;
        x = static_cast<float>(z);
      }
    });
    add("Crackle", true, [](std::vector<float>& v, uint32_t& r) {
      for (size_t i = 0; i < v.size(); ++i)
        if (rngUni(r) < 0.0015f) {
          const float a = rngBi(r);
          for (size_t k = 0; k < 40 && i + k < v.size(); ++k) v[i + k] += a * std::exp(-static_cast<float>(k) * 0.15f) * ((k & 1) ? -1.f : 1.f);
        }
    });
    add("Geiger", false, [](std::vector<float>& v, uint32_t& r) {
      for (size_t i = 0; i < v.size(); ++i)
        if (rngUni(r) < 0.0004f)
          for (size_t k = 0; k < 120 && i + k < v.size(); ++k) v[i + k] += std::sin(static_cast<float>(k) * 0.9f) * std::exp(-static_cast<float>(k) * 0.05f);
    });
    add("Vinyl", true, [](std::vector<float>& v, uint32_t& r) {
      double z = 0;
      for (size_t i = 0; i < v.size(); ++i) {
        z += (rngBi(r) * 0.15 - z) * 0.2;
        v[i] = static_cast<float>(z);
        if (rngUni(r) < 0.0008f) v[i] += rngBi(r) * 0.9f;
      }
    });
    add("Hiss Air", true, [](std::vector<float>& v, uint32_t& r) {
      double z = 0;
      for (float& x : v) {
        const double w = rngBi(r);
        x = static_cast<float>(w - z);
        z = w * 0.9;
      }
    });
    add("Rumble", true, [](std::vector<float>& v, uint32_t& r) {
      for (float& x : v) x = rngBi(r);
      onePoleLp(v, 0.004);
      onePoleLp(v, 0.004);
    });
    add("Digital Grit", true, [](std::vector<float>& v, uint32_t& r) {
      float hold = 0;
      for (size_t i = 0; i < v.size(); ++i) {
        if (i % 37 == 0) hold = std::round(rngBi(r) * 4) / 4;
        v[i] = hold;
      }
    });
    add("Bit Rain", true, [](std::vector<float>& v, uint32_t& r) {
      for (size_t i = 0; i < v.size(); ++i) v[i] = ((rngNext(r) >> 13) & 1) ? 0.4f : -0.4f;
      onePoleLp(v, 0.3);
    });
    add("Breath", false, [](std::vector<float>& v, uint32_t& r) {
      const size_t n2 = v.size();
      double z = 0;
      for (size_t i = 0; i < n2; ++i) {
        z += (rngBi(r) - z) * 0.25;
        const double t = static_cast<double>(i) / n2;
        v[i] = static_cast<float>(z * std::sin(kPi2 * 0.5 * t));
      }
    });
    add("Wind", true, [](std::vector<float>& v, uint32_t& r) {
      double z1 = 0, z2 = 0;
      for (size_t i = 0; i < v.size(); ++i) {
        const double c = 0.02 + 0.015 * std::sin(kPi2 * i / 30000.0);
        z1 += (rngBi(r) - z1) * c;
        z2 += (z1 - z2) * c;
        v[i] = static_cast<float>(z1 - z2);
      }
    });
    add("Metal Ring", false, [](std::vector<float>& v, uint32_t&) {
      const double fr[] = {523.0, 1213.0, 1871.0, 2712.0, 3967.0};
      for (size_t i = 0; i < v.size(); ++i) {
        const double t = static_cast<double>(i) / 44100.0;
        double s = 0;
        for (int k = 0; k < 5; ++k) s += std::sin(kPi2 * fr[k] * t) * std::exp(-t * (2.0 + k));
        v[i] = static_cast<float>(s);
      }
    });
    add("Tape Hum", true, [](std::vector<float>& v, uint32_t& r) {
      for (size_t i = 0; i < v.size(); ++i) {
        const double t = static_cast<double>(i) / 44100.0;
        v[i] = static_cast<float>(0.6 * std::sin(kPi2 * 50 * t) + 0.2 * std::sin(kPi2 * 150 * t) + 0.15 * rngBi(r));
      }
    });
    add("Electric Hum", true, [](std::vector<float>& v, uint32_t& r) {
      for (size_t i = 0; i < v.size(); ++i) {
        const double t = static_cast<double>(i) / 44100.0;
        const double s = std::sin(kPi2 * 60 * t);
        v[i] = static_cast<float>(std::tanh(4 * s) * 0.5 + 0.05 * rngBi(r));
      }
    });
    add("Rain", true, [](std::vector<float>& v, uint32_t& r) {
      for (float& x : v) x = rngBi(r) * 0.2f;
      for (size_t i = 0; i < v.size(); ++i)
        if (rngUni(r) < 0.002f)
          for (size_t k = 0; k < 200 && i + k < v.size(); ++k) v[i + k] += rngBi(r) * std::exp(-static_cast<float>(k) * 0.03f);
    });
    add("Click Train", true, [](std::vector<float>& v, uint32_t&) {
      for (size_t i = 0; i < v.size(); i += 2205) v[i] = 1.f;
    });
    add("Bandpass Swirl", true, [](std::vector<float>& v, uint32_t& r) {
      double lp = 0, bp = 0;
      for (size_t i = 0; i < v.size(); ++i) {
        const double f = 0.02 + 0.08 * (0.5 + 0.5 * std::sin(kPi2 * i / 22050.0));
        const double hp = rngBi(r) - lp - 0.2 * bp;
        bp += f * hp;
        lp += f * bp;
        v[i] = static_cast<float>(bp);
      }
    });
    add("Shaker Loop", true, [](std::vector<float>& v, uint32_t& r) {
      const size_t beat = 44100 / 8;
      for (size_t i = 0; i < v.size(); ++i) {
        const double t = static_cast<double>(i % beat) / beat;
        v[i] = static_cast<float>(rngBi(r) * std::exp(-t * 12) * ((i / beat) % 2 ? 0.6 : 1.0));
      }
    });
    return out;
  }();
  return v;
}

// ------------------------------------------------------------------ impulses (generated)
const Impulse& factoryImpulse(int index) {
  constexpr int kCount = 11;
  static std::array<Impulse, kCount> cache;
  static std::array<std::once_flag, kCount> once;
  index = std::clamp(index, 0, kCount - 1);
  std::call_once(once[static_cast<size_t>(index)], [index] {
    Impulse& ir = cache[static_cast<size_t>(index)];
    const double sr = 48000;
    uint32_t r = 0xBADC0DEu + static_cast<uint32_t>(index) * 31u;
    auto tail = [&](double secs, double decay, double bright, double pre) {
      const size_t n = static_cast<size_t>(secs * sr);
      ir.L.assign(n, 0.f);
      ir.R.assign(n, 0.f);
      double zl = 0, zr = 0;
      const size_t p0 = static_cast<size_t>(pre * sr);
      for (size_t i = p0; i < n; ++i) {
        const double t = static_cast<double>(i - p0) / sr;
        const double env = std::exp(-t * 6.9 / decay);
        const double a = bright * std::exp(-t * 2.0) + 0.05;
        zl += (rngBi(r) - zl) * a;
        zr += (rngBi(r) - zr) * a;
        ir.L[i] = static_cast<float>(zl * env);
        ir.R[i] = static_cast<float>(zr * env);
      }
      if (n > 0) ir.L[0] += 0.6f, ir.R[0] += 0.6f;
    };
    switch (index) {
      case 0: tail(0.45, 0.35, 0.8, 0.002); break;   // small room
      case 1: tail(0.9, 0.7, 0.7, 0.006); break;     // medium room
      case 2: tail(2.8, 2.4, 0.55, 0.018); break;    // large hall
      case 3: tail(2.0, 1.6, 0.95, 0.0); break;      // plate
      case 4: {                                      // spring: dispersive chirps
        tail(1.6, 1.3, 0.6, 0.0);
        for (size_t i = 0; i < ir.L.size(); ++i) {
          const double t = static_cast<double>(i) / sr;
          const double c = std::sin(kPi2 * (300 + 1500 * std::fmod(t * 9, 1.0)) * t) * std::exp(-t * 3);
          ir.L[i] = static_cast<float>(ir.L[i] * 0.5 + c * 0.3);
          ir.R[i] = static_cast<float>(ir.R[i] * 0.5 + c * 0.28);
        }
        break;
      }
      case 5:
      case 6: {  // speaker cabinets: short resonant responses
        const size_t n = static_cast<size_t>(0.03 * sr);
        ir.L.assign(n, 0.f);
        const double f1 = index == 5 ? 110 : 85, f2 = index == 5 ? 2400 : 1800;
        for (size_t i = 0; i < n; ++i) {
          const double t = static_cast<double>(i) / sr;
          ir.L[i] = static_cast<float>(std::exp(-t * 400) * (std::sin(kPi2 * f1 * t) * 0.5 + std::sin(kPi2 * f2 * t) * 0.6 +
                                                               std::sin(kPi2 * 4200 * t) * 0.2 * std::exp(-t * 900)));
        }
        ir.L[0] += 0.4f;
        ir.R = ir.L;
        break;
      }
      case 7:
      case 8: {  // telephone / radio: band-limited impulse
        const size_t n = 1024;
        ir.L.assign(n, 0.f);
        const double lo = index == 7 ? 300 : 120, hi = index == 7 ? 3400 : 5000;
        for (size_t i = 0; i < n; ++i) {
          const double t = (static_cast<double>(i) - n / 2.0) / sr;
          const double sincHi = t == 0 ? 2 * hi : std::sin(kPi2 * hi * t) / (kPi2 / 2 * t);
          const double sincLo = t == 0 ? 2 * lo : std::sin(kPi2 * lo * t) / (kPi2 / 2 * t);
          const double w = 0.5 - 0.5 * std::cos(kPi2 * i / (n - 1));
          ir.L[i] = static_cast<float>((sincHi - sincLo) / sr * w);
        }
        ir.R = ir.L;
        break;
      }
      case 9: {  // reverse swell
        tail(1.8, 1.5, 0.6, 0.0);
        std::reverse(ir.L.begin(), ir.L.end());
        std::reverse(ir.R.begin(), ir.R.end());
        break;
      }
      default: tail(4.5, 4.0, 0.5, 0.03); break;  // cathedral
    }
    // normalize energy
    double e = 0;
    for (size_t i = 0; i < ir.L.size(); ++i) e += ir.L[i] * ir.L[i] + (i < ir.R.size() ? ir.R[i] * ir.R[i] : 0);
    const float g = e > 1e-12 ? static_cast<float>(1.0 / std::sqrt(e * 0.5)) * 0.5f : 1.f;
    for (float& x : ir.L) x *= g;
    for (float& x : ir.R) x *= g;
    ir.rate = sr;
  });
  return cache[static_cast<size_t>(index)];
}

// ------------------------------------------------------------------ SFZ
namespace {
int noteNumber(const std::string& v) {
  if (v.empty()) return -1;
  if ((v[0] >= '0' && v[0] <= '9') || v[0] == '-') return std::atoi(v.c_str());
  static const int base[7] = {9, 11, 0, 2, 4, 5, 7};  // a b c d e f g
  const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(v[0])));
  if (c < 'a' || c > 'g') return -1;
  int n = base[c - 'a'];
  size_t i = 1;
  if (i < v.size() && v[i] == '#') ++n, ++i;
  else if (i < v.size() && v[i] == 'b') --n, ++i;
  const int oct = std::atoi(v.c_str() + i);
  return std::clamp(n + (oct + 1) * 12, 0, 127);
}
}  // namespace

bool parseSfz(const std::string& path, std::vector<Zone>& zones, std::string* err) {
  std::ifstream f(path);
  if (!f) {
    if (err) *err = "cannot open " + path;
    return false;
  }
  std::stringstream ss;
  ss << f.rdbuf();
  std::string text = ss.str();
  // strip comments
  for (size_t p = text.find("//"); p != std::string::npos; p = text.find("//", p)) {
    const size_t e = text.find('\n', p);
    text.erase(p, e == std::string::npos ? std::string::npos : e - p);
  }
  std::string dir = path;
  const size_t sl = dir.find_last_of("/\\");
  dir = sl == std::string::npos ? "" : dir.substr(0, sl + 1);
  std::map<std::string, std::string> global, group, region, control;
  int section = 0;  // 0 none, 1 control, 2 global, 3 group, 4 region
  auto flush = [&]() {
    if (section != 4) return;
    std::map<std::string, std::string> o = global;
    for (auto& [k, v] : group) o[k] = v;
    for (auto& [k, v] : region) o[k] = v;
    if (!o.count("sample") || zones.size() >= 1024) return;
    std::string sp = o["sample"];
    std::replace(sp.begin(), sp.end(), '\\', '/');
    const std::string full = (sp.size() && sp[0] == '/') ? sp : dir + control["default_path"] + sp;
    Zone z;
    z.s = loadSample(full);
    if (!z.s) return;
    auto num = [&](const char* k, double d) { return o.count(k) ? std::atof(o[k].c_str()) : d; };
    auto key = [&](const char* k, int d) {
      if (!o.count(k)) return d;
      const int n = noteNumber(o[k]);
      return n < 0 ? d : n;
    };
    const int k = key("key", -1);
    z.loKey = key("lokey", k >= 0 ? k : 0);
    z.hiKey = key("hikey", k >= 0 ? k : 127);
    z.center = key("pitch_keycenter", k >= 0 ? k : 60);
    z.loVel = static_cast<int>(num("lovel", 0));
    z.hiVel = static_cast<int>(num("hivel", 127));
    z.tune = num("tune", 0) + num("transpose", 0) * 100;
    z.gainDb = num("volume", 0);
    const std::string lm = o.count("loop_mode") ? o["loop_mode"] : "";
    z.loopMode = lm == "loop_continuous" ? 1 : lm == "loop_sustain" ? 2 : 0;
    z.loopStart = static_cast<int64_t>(num("loop_start", num("loopstart", 0)));
    z.loopEnd = static_cast<int64_t>(num("loop_end", num("loopend", static_cast<double>(z.s->frames()) - 1)));
    z.start = static_cast<int64_t>(num("offset", 0));
    z.end = static_cast<int64_t>(num("end", -1));
    zones.push_back(z);
  };
  size_t i = 0;
  while (i < text.size()) {
    if (std::isspace(static_cast<unsigned char>(text[i]))) {
      ++i;
      continue;
    }
    if (text[i] == '<') {
      const size_t e = text.find('>', i);
      if (e == std::string::npos) break;
      const std::string h = text.substr(i + 1, e - i - 1);
      flush();
      region.clear();
      if (h == "control") section = 1;
      else if (h == "global") section = 2, global.clear();
      else if (h == "group" || h == "master") section = 3, group.clear();
      else if (h == "region") section = 4;
      else section = 0;
      i = e + 1;
      continue;
    }
    // opcode=value (value may contain spaces for sample=)
    const size_t eq = text.find('=', i);
    if (eq == std::string::npos) break;
    const std::string k = text.substr(i, eq - i);
    size_t ve = eq + 1;
    if (k == "sample" || k == "default_path") {
      // up to the next opcode ("name=") or header
      size_t next = text.find_first_of("<\n\r", ve);
      ve = next == std::string::npos ? text.size() : next;
      size_t eq2 = text.find('=', eq + 1);
      if (eq2 != std::string::npos && eq2 < ve) {
        size_t sp = text.rfind(' ', eq2);
        if (sp != std::string::npos && sp > eq) ve = sp;
      }
    } else {
      while (ve < text.size() && !std::isspace(static_cast<unsigned char>(text[ve])) && text[ve] != '<') ++ve;
    }
    std::string v = text.substr(eq + 1, ve - eq - 1);
    while (!v.empty() && std::isspace(static_cast<unsigned char>(v.back()))) v.pop_back();
    auto& tgt = section == 1 ? control : section == 2 ? global : section == 3 ? group : region;
    tgt[k] = v;
    i = ve;
  }
  flush();
  if (zones.empty() && err) *err = "no playable regions";
  return !zones.empty();
}

// ------------------------------------------------------------------ waveshapers
int warpDistMode(int warp) {
  static const int map[14] = {0, 1, 2, 3, 4, 5, 6, 7, 9, 10, 13, 14, 15, 17};
  const int i = warp - 19;
  return i >= 0 && i < 14 ? map[i] : 0;
}

float shapeSample(int mode, float x, float drive, float bias, const float* curveA, const float* curveB, float& state) {
  const float g = std::pow(10.f, drive * 1.8f);  // 0 .. +36 dB
  const float v = x * g;
  auto curve = [](const float* c, float t) {  // t -1..1 -> c (0..1 over 257 points) -> -1..1
    if (!c) return t;
    const float u = std::clamp((t + 1.f) * 0.5f, 0.f, 1.f) * 256.f;
    const int i = std::min(255, static_cast<int>(u));
    const float fr = u - static_cast<float>(i);
    return (c[i] + (c[i + 1] - c[i]) * fr) * 2.f - 1.f;
  };
  switch (mode) {
    case 0: {  // tube: asymmetric soft clip
      const float b = 0.25f + bias * 0.5f;
      return (fastTanh(v + b) - fastTanh(b)) / (1.f + 0.3f * b);
    }
    case 1: return v / (1.f + std::fabs(v));
    case 2: return std::clamp(v, -1.f, 1.f);
    case 3: return v >= 0 ? 1.f - std::exp(-v) : -(1.f - std::exp(v * 0.6f)) * 0.8f;
    case 4: return v >= 0 ? fastTanh(v * 1.5f) : fastTanh(v * 0.3f) * 0.35f;
    case 5: {  // linear fold
      float t = v * 0.25f + 0.25f;
      t -= std::floor(t);
      return t < 0.5f ? t * 4.f - 1.f : 3.f - t * 4.f;
    }
    case 6: return std::sin(v * 1.5707963f);
    case 7: {
      const float a = std::min(1.f, v * v);
      return v >= 0 ? a : -a;
    }
    case 8: {  // downsample (amplitude crush in waveshaper context)
      const float steps = 2.f + (1.f - drive) * 62.f;
      return std::round(x * steps) / steps;
    }
    case 9: return fastTanh(v + bias) - fastTanh(bias);
    case 10: {
      const float r = std::fabs(v);
      return std::clamp(r * 2.f - 1.f, -1.f, 1.f) * (0.5f + 0.5f * drive) + x * (0.5f - 0.5f * drive);
    }
    case 11: return curve(curveA, std::clamp(v, -1.f, 1.f));
    case 12: return v >= 0 ? curve(curveA, std::clamp(v, 0.f, 1.f)) : curve(curveB ? curveB : curveA, std::clamp(v, -1.f, 0.f));
    case 13: return std::sin(v);
    case 14: {
      const float a = fastTanh(v * 1.5f);
      return std::clamp(a * 1.2f, -0.9f, 0.9f) / 0.9f;
    }
    case 15: {  // tape: soft saturation with a little memory
      state += (v - state) * 0.35f;
      return fastTanh(0.7f * v + 0.3f * state);
    }
    case 16: {
      const float c = std::clamp(v, -1.f, 1.f);
      return 1.5f * c - 0.5f * c * c * c;
    }
    case 17: {
      const float c = std::clamp(v, -1.5f, 1.5f);
      return (c - c * c * c / 6.75f) / (1.5f - 3.375f / 6.75f);
    }
    default: return x;
  }
}

}  // namespace gl::packs::plasma
