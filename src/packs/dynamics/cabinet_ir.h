// Gridline — J-42: impulse-response cabinets for the module rack's speaker
// (gl.amprack RkModel 1). The impulse files are FL Studio's own data: they are
// read in place from the installed program (the directory travels in the state
// as a "cabdir=" line) and never shipped. The convolution is zero-latency: a
// 64-tap direct head plus 64-sample uniformly partitioned overlap-save blocks
// for the rest, per channel (a stereo file convolves L with L and R with R).
#pragma once
#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <memory>
#include <string>
#include <vector>
#include "../../engine/Generators.h"
#include "maximus_ref.h"

namespace gl::packs::dynamics::cab {

inline constexpr int kHead = 64, kB = 64, kN = 128;
inline constexpr int kTable = 59;

// Speaker menu index → file stem (after "IMP Cabinet ") and output gain; empty = menu header.
struct Entry {
  const char* stem;
  double gain;
};
inline const Entry& entry(int i) {
  static const Entry t[kTable] = {
      {"", 0},
      {"Stereo Blues", 0.026},     {"Stereo Guitaro", 0.1},     {"Stereo Metal", 0.08},      {"Stereo Muffled", 0.058},
      {"Stereo Wide", 0.086},      {"Stereo Combo 01", 0.15},   {"Stereo Combo 02", 0.136},  {"Stereo Combo 03", 0.128},
      {"Stereo Combo 04", 0.1},    {"Stereo Combo 05", 0.16},   {"Stereo Combo 06", 0.128},  {"Stereo Combo 07", 0.12},
      {"Stereo Combo 08", 0.1},    {"Stereo Combo 09", 0.13},   {"Stereo Combo 10", 0.12},   {"Stereo Combo 11", 0.1},
      {"Stereo Combo 12", 0.12},   {"Stereo Combo 13", 0.102},  {"Stereo Combo 14", 0.1},    {"", 0},
      {"Mono Closest", 0.158},     {"Mono Closer", 0.13},       {"Mono Close", 0.13},        {"Mono Medium", 0.11},
      {"Mono Far", 0.096},         {"Mono Farther", 0.088},     {"", 0},
      {"Model AB", 0.026},         {"Model CD", 0.13},          {"Model EF", 0.12},          {"Model GH", 0.2},
      {"Model IJ", 0.07},          {"Model KL", 0.172},         {"Model MN", 0.068},         {"Model OP", 0.025},
      {"Model QR", 0.056},         {"Model ST", 0.024},         {"", 0},
      {"Model A", 0.104},          {"Model B", 0.12},           {"Model C", 0.102},          {"Model D", 0.138},
      {"Model E", 0.12},           {"Model F", 0.08},           {"Model G", 0.12},           {"Model H", 0.136},
      {"Model I", 0.108},          {"Model J", 0.148},          {"Model K", 0.124},          {"Model L", 0.14},
      {"Model M", 0.08},           {"Model N", 0.11},           {"Model O", 0.056},          {"Model P", 0.052},
      {"Model Q", 0.076},          {"Model R", 0.064},          {"Model S", 0.092},          {"Model T", 0.064},
  };
  return t[std::clamp(i, 0, kTable - 1)];
}
inline bool valid(int i) { return i > 0 && i < kTable && entry(i).stem[0] != 0; }
inline std::string fileFor(const std::string& dir, int i) { return dir + "/IMP Cabinet " + entry(i).stem + ".wv"; }
// "cabdir=<dir>" line of the state data ("" when absent).
inline std::string dirOf(const std::string& data) {
  const size_t at = data.find("cabdir=");
  if (at == std::string::npos) return {};
  const size_t e = data.find('\n', at);
  return data.substr(at + 7, e == std::string::npos ? std::string::npos : e - at - 7);
}

// Pedal board cabinet n (1-based): the n-th of the sorted "*.wav" names in `dir` ("" when absent).
std::string boardFile(const std::string& dir, int n);  // dynamics_dsp.cpp
// Pedal board impulse at `rate`: N = trunc(len·rate/fileRate) taps, tap i read linearly at
// i·(len − 1)/N, × 0.2.
inline std::vector<float> boardResample(const std::vector<float>& h, double fileRate, double rate) {
  const size_t len = h.size();
  const size_t N = static_cast<size_t>(static_cast<double>(len) * rate / fileRate);
  std::vector<float> out(N);
  for (size_t i = 0; i < N; ++i) {
    const double p = static_cast<double>(i) * static_cast<double>(len - 1) / static_cast<double>(N);
    const size_t a = static_cast<size_t>(p);
    const double f = p - static_cast<double>(a), x0 = h[a], x1 = a + 1 < len ? h[a + 1] : 0.0;
    out[i] = static_cast<float>((x0 + (x1 - x0) * f) * 0.2);
  }
  return out;
}

// Prepared impulse (immutable, built on the UI thread).
struct Kernel {
  double rate = 0, gain = 0.1;
  int len = 0, parts = 0;
  float head[2][kHead] = {};
  std::vector<std::complex<double>> spec[2];  // parts × kN
};
// One kernel per rack module (null = no impulse: the procedural cabinet).
struct Aux final : AuxData {
  double rate = 0;
  std::shared_ptr<const Kernel> k[4];
};
inline std::shared_ptr<Kernel> build(const std::vector<float>* ch, double rate, double gain) {
  auto k = std::make_shared<Kernel>();
  k->rate = rate, k->gain = gain > 0 ? gain : 0.1;
  k->len = static_cast<int>(std::max(ch[0].size(), ch[1].size()));
  k->parts = std::max(0, (k->len - kHead + kB - 1) / kB);
  maxref::FftD fft;
  fft.init(kN);
  for (int c = 0; c < 2; ++c) {
    const std::vector<float>& h = ch[c];
    auto at = [&](int i) { return i < static_cast<int>(h.size()) ? static_cast<double>(h[static_cast<size_t>(i)]) : 0.0; };
    for (int i = 0; i < kHead; ++i) k->head[c][i] = static_cast<float>(at(i));
    k->spec[c].assign(static_cast<size_t>(k->parts) * kN, {});
    for (int j = 0; j < k->parts; ++j) {
      std::complex<double>* s = k->spec[c].data() + static_cast<size_t>(j) * kN;
      for (int i = 0; i < kB; ++i) s[i] = at(kHead + j * kB + i);
      fft.run(s, false);
    }
  }
  return k;
}

// Per-module runtime state (allocated on the UI thread for `cap` partitions).
struct Run {
  int cap = 0, pos = 0, slot = 0, hp = 0;
  maxref::FftD fft;
  std::vector<std::complex<double>> fdl[2], work;
  float in[2][kN] = {}, out[2][kB] = {}, hist[2][2 * kHead] = {};
  explicit Run(int parts) : cap(std::max(parts, 1)) {
    fft.init(kN);
    for (auto& f : fdl) f.assign(static_cast<size_t>(cap) * kN, {});
    work.assign(kN, {});
  }
  void clear() {
    for (auto& f : fdl) std::fill(f.begin(), f.end(), std::complex<double>{});
    std::fill(&in[0][0], &in[0][0] + 2 * kN, 0.f), std::fill(&out[0][0], &out[0][0] + 2 * kB, 0.f);
    std::fill(&hist[0][0], &hist[0][0] + 4 * kHead, 0.f);
    pos = slot = hp = 0;
  }
  // One stereo frame in place.
  void tick(const Kernel& k, float* x) {
    float y[2];
    for (int c = 0; c < 2; ++c) {
      hist[c][hp] = hist[c][hp + kHead] = x[c];
      const float* h = &hist[c][hp + 1];  // oldest .. newest
      float acc = 0;
      for (int t = 0; t < kHead; ++t) acc += k.head[c][kHead - 1 - t] * h[t];
      y[c] = acc + out[c][pos];
      in[c][kB + pos] = x[c];
    }
    hp = (hp + 1) % kHead;
    if (++pos == kB) pos = 0, block(k);
    x[0] = y[0], x[1] = y[1];
  }

 private:
  void block(const Kernel& k) {
    slot = (slot + 1) % cap;
    const int parts = std::min(k.parts, cap);
    for (int c = 0; c < 2; ++c) {
      std::complex<double>* X = fdl[c].data() + static_cast<size_t>(slot) * kN;
      for (int i = 0; i < kN; ++i) X[i] = in[c][i];
      fft.run(X, false);
      std::fill(work.begin(), work.end(), std::complex<double>{});
      for (int j = 0; j < parts; ++j) {
        const std::complex<double>* Xj = fdl[c].data() + static_cast<size_t>((slot - j + cap) % cap) * kN;
        const std::complex<double>* H = k.spec[c].data() + static_cast<size_t>(j) * kN;
        for (int i = 0; i < kN; ++i) work[static_cast<size_t>(i)] += Xj[i] * H[i];
      }
      fft.run(work.data(), true);
      for (int i = 0; i < kB; ++i) out[c][i] = static_cast<float>(work[static_cast<size_t>(kB + i)].real() / kN);
      std::copy(in[c] + kB, in[c] + kN, in[c]);
    }
  }
};
// Partitions a kernel needs at `sr` (longest shipped impulse: 6363 samples at 44.1 kHz).
inline int partsFor(double sr) { return static_cast<int>(std::ceil((6400.0 * sr / 44100.0 - kHead) / kB)) + 1; }

}  // namespace gl::packs::dynamics::cab
