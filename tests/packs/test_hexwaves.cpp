// Gridline — J-102 tests: Gridline's own wave set of Hex Synth (gl.hexsynth)
// and Kick Sculptor's synthesised noise layers (gl.kicksculpt).
//
// The run points GRIDLINE_FL_DATA at a decoy FL folder (a fake Hex Synth
// reference library carrying the spectra anchor the old reader searched for,
// and fake kick noise WAVs). Nothing of it may reach the sound: the wave set
// keeps its own fingerprints, the noise layers are Gridline's, and rendering
// opens no file.
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#if defined(_WIN32)
#include <process.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif
#include "../../src/core/PluginInfo.h"
#include "../../src/engine/Sample.h"
#include "../../src/packs/Packs.h"
#include "../../src/packs/models/drums.h"
#include "../../src/packs/synths/hex.h"
#include "../../src/packs/synths/hex_rt.h"
#include "../Check.h"

using namespace gl;
namespace hx = gl::packs::synths::hex;
namespace fs = std::filesystem;

namespace {

void expect(bool ok, const std::string& name) { expect(ok, name.c_str()); }

int pid() {
#if defined(_WIN32)
  return _getpid();
#else
  return static_cast<int>(::getpid());
#endif
}

void setEnv(const char* k, const std::string& v) {
#if defined(_WIN32)
  _putenv_s(k, v.c_str());
#else
  ::setenv(k, v.c_str(), 1);
#endif
}

// Open file descriptors (POSIX); -1 where not counted.
int openFiles() {
#if defined(_WIN32)
  return -1;
#else
  int n = 0;
  for (int fd = 0; fd < 1024; ++fd) n += ::fcntl(fd, F_GETFD) != -1;
  return n;
#endif
}

// ------------------------------------------------------------------ decoy
fs::path makeDecoy() {
  std::error_code ec;
  const fs::path root = fs::temp_directory_path(ec) / ("gridline-j102-decoy-" + std::to_string(pid()));
  const fs::path tb = root / "Plugins/Fruity/Generators/Toxic Biohazard";
  const fs::path bd = root / "Plugins/Fruity/Generators/BassDrum/Noise Samples";
  fs::create_directories(tb, ec);
  fs::create_directories(bd, ec);
  // 27 blocks of 128 amplitudes + 128 phases: block 0 a triangle, block 6
  // a saw start (the old reader's anchor), everything else a loud constant.
  std::vector<float> blocks(27 * 256, 0.3f);
  for (int h = 1; h <= 128; ++h) blocks[static_cast<size_t>(h - 1)] = h % 2 ? static_cast<float>(8.0 / (M_PI * M_PI * h * h)) : 0.f;
  blocks[6 * 256] = 0.509f, blocks[6 * 256 + 1] = 0.2547f;
  std::vector<char> lib(4096, 0);
  lib.insert(lib.end(), reinterpret_cast<const char*>(blocks.data()), reinterpret_cast<const char*>(blocks.data() + blocks.size()));
  std::ofstream(tb / "Decoy_x64.dylib", std::ios::binary).write(lib.data(), static_cast<std::streamsize>(lib.size()));
  // Kick noise files: 16-bit mono WAVs of a constant (DC).
  for (int k = 0; k < gl::packs::models::dr::kickNoiseCount(); ++k) {
    const uint32_t n = 4410, bytes = n * 2;
    std::ofstream f(bd / (std::string(gl::packs::models::dr::kickNoiseName(k)) + ".wav"), std::ios::binary);
    auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    f.write("RIFF", 4), u32(36 + bytes), f.write("WAVEfmt ", 8), u32(16), u16(1), u16(1), u32(44100), u32(88200), u16(2), u16(16);
    f.write("data", 4), u32(bytes);
    for (uint32_t i = 0; i < n; ++i) u16(20000);
  }
  return root;
}

// ------------------------------------------------------------- rendering
PluginState hexWith(int wave) {
  PluginState s;
  s.type = "gl.hexsynth";
  normalizePluginState(s);
  s.params[static_cast<size_t>(hx::HxWave)] = static_cast<float>(wave);
  return s;
}

std::vector<float> render(const PluginState& s, double pitch, int frames, double sr = 44100.0) {
  std::vector<float> L(static_cast<size_t>(frames)), R(static_cast<size_t>(frames));
  auto g = packs::createGenerator(s.type);
  if (!g) return L;
  ChannelAux aux;
  packs::prepareAux(s, aux);
  ProcessContext ctx;
  ctx.sampleRate = sr;
  ctx.tempo = 120;
  g->noteOn(1, pitch, 0.9f, 0);
  for (int pos = 0; pos < frames; pos += 256) g->process(s, aux, ctx, L.data() + pos, R.data() + pos, std::min(256, frames - pos));
  return L;
}

// Share of the signal's power more than a few bins away from every harmonic
// of f0 (Hann window, plain DFT).
double inharmonicShare(const std::vector<float>& x, size_t from, size_t n, double f0, double sr) {
  std::vector<double> w(n);
  for (size_t i = 0; i < n; ++i) w[i] = x[from + i] * (0.5 - 0.5 * std::cos(2 * M_PI * static_cast<double>(i) / static_cast<double>(n)));
  double total = 0, off = 0;
  for (size_t k = 1; k < n / 2; ++k) {
    std::complex<double> acc = 0;
    const double step = -2 * M_PI * static_cast<double>(k) / static_cast<double>(n);
    std::complex<double> rot(std::cos(step), std::sin(step)), ph = 1;
    for (size_t i = 0; i < n; ++i, ph *= rot) acc += w[i] * ph;
    const double p = std::norm(acc), f = static_cast<double>(k) * sr / static_cast<double>(n);
    total += p;
    const double h = f / f0, binHz = sr / static_cast<double>(n);
    if (std::fabs(h - std::round(h)) * f0 > 4 * binHz || std::round(h) < 1) off += p;
  }
  return total > 0 ? off / total : 1.0;
}

struct Desc {
  double centroid, oddShare;
  int top;           // strongest harmonic
  int topAbove2;     // strongest harmonic from 3 up
};
Desc describe(const hx::Spectrum& s) {
  double e = 0, c = 0, odd = 0;
  int top = 1, top3 = 3;
  for (int h = 1; h <= hx::kHarmonics; ++h) {
    const double a = s.amp[h - 1], p = a * a;
    e += p, c += h * p, odd += h % 2 ? p : 0;
    if (a > s.amp[top - 1]) top = h;
    if (h >= 3 && a > s.amp[top3 - 1]) top3 = h;
  }
  return {e > 0 ? c / e : 0, e > 0 ? odd / e : 0, top, top3};
}

double cyclePeak(const hx::Spectrum& s) {
  double pk = 0;
  for (int n = 0; n < 512; ++n) {
    double v = 0;
    for (int k = 0; k < hx::kHarmonics; ++k) v += s.amp[k] * std::sin(2 * M_PI * ((k + 1) * n / 512.0 + s.phase[k]));
    pk = std::max(pk, std::fabs(v));
  }
  return pk;
}

}  // namespace

int main() {
  const fs::path decoy = makeDecoy();
  setEnv("GRIDLINE_FL_DATA", decoy.string());

  // ------------------------------------------------- count and index mapping
  const PluginInfo* info = findPlugin("gl.hexsynth");
  expect(info && info->params.size() == static_cast<size_t>(hx::HxCount), "hex: registered with 340 parameters");
  if (!info) return finish();
  const ParamDesc& wp = info->params[static_cast<size_t>(hx::HxWave)];
  expect(hx::kWaves == 33 && wp.choices.size() == static_cast<size_t>(hx::kWaves), "hex: 33 wave selectors (custom + 32), one name each");
  for (int s = 0; s < hx::kSlots; ++s)
    expect(info->params[static_cast<size_t>(hx::HxWave + s)].choices == wp.choices,
           "hex: wave slot " + std::to_string(s) + " offers the same set");

  std::vector<hx::Spectrum> sp(static_cast<size_t>(hx::kWaves));
  std::vector<Desc> d(static_cast<size_t>(hx::kWaves));
  for (int w = 1; w < hx::kWaves; ++w) sp[static_cast<size_t>(w)] = hx::builtinSpectrum(w), d[static_cast<size_t>(w)] = describe(sp[static_cast<size_t>(w)]);
  auto D = [&](int w) { return d[static_cast<size_t>(w)]; };

  // Every built-in wave is non-trivial, finite, peak-normalised and distinct.
  bool finite = true, normal = true, distinct = true;
  for (int w = 6; w < hx::kWaves; ++w) {
    const auto& s = sp[static_cast<size_t>(w)];
    for (int h = 0; h < hx::kHarmonics; ++h) finite = finite && std::isfinite(s.amp[h]) && std::isfinite(s.phase[h]) && s.amp[h] >= 0;
    const double pk = cyclePeak(s);
    if (std::fabs(pk - 0.9) > 0.02) std::printf("#   wave %d peak %.3f\n", w, pk), normal = false;
    for (int v = 6; v < w; ++v) {
      double diff = 0;
      for (int h = 0; h < hx::kHarmonics; ++h) diff += std::fabs(s.amp[h] - sp[static_cast<size_t>(v)].amp[h]);
      if (diff < 0.05) std::printf("#   waves %d and %d alike\n", v, w), distinct = false;
    }
  }
  expect(finite, "hex: wave spectra finite, amplitudes non-negative");
  expect(normal, "hex: waves 6..32 peak-normalised (0.9)");
  expect(distinct, "hex: waves 6..32 all distinct");
  for (int w = 1; w < hx::kWaves; ++w)
    std::printf("#   wave %2d %-14s centroid %6.2f odd %.2f top %2d top>2 %2d\n", w, wp.choices[static_cast<size_t>(w)].c_str(),
                D(w).centroid, D(w).oddShare, D(w).top, D(w).topAbove2);

  // Families per index (the meaning imported selectors rely on).
  expect(hx::waveFamily(0) == hx::WaveFamily::Custom && hx::waveFamily(5) == hx::WaveFamily::Sine &&
             hx::waveFamily(6) == hx::WaveFamily::Classic && hx::waveFamily(12) == hx::WaveFamily::Classic &&
             hx::waveFamily(13) == hx::WaveFamily::Tonal && hx::waveFamily(19) == hx::WaveFamily::Tonal &&
             hx::waveFamily(20) == hx::WaveFamily::Noise && hx::waveFamily(23) == hx::WaveFamily::Noise &&
             hx::waveFamily(24) == hx::WaveFamily::Formant && hx::waveFamily(32) == hx::WaveFamily::Formant,
         "hex: family ranges 1-5 / 6-12 / 13-19 / 20-23 / 24-32");
  expect(D(6).oddShare > 0.999 && D(6).centroid < 1.1, "hex: 6 triangle (odd, 1/h²)");
  expect(D(7).oddShare < 0.001 && D(7).top == 2, "hex: 7 even-only (rectified sine)");
  expect(D(8).oddShare > 0.999 && D(8).centroid > 2 && D(8).centroid < 3.2, "hex: 8 square (odd, 1/h)");
  expect(D(9).oddShare > 0.999 && D(9).centroid < D(8).centroid, "hex: 9 rounder square");
  expect(sp[10].amp[3] < 1e-6f && sp[10].amp[7] < 1e-6f && sp[10].amp[1] > 0.3f * sp[10].amp[0], "hex: 10 pulse 25 % (holes at 4, 8)");
  expect(D(11).centroid > D(10).centroid * 1.5, "hex: 11 narrower pulse is brighter");
  expect(std::fabs(sp[12].amp[1] / sp[12].amp[0] - 0.5f) < 1e-3f && std::fabs(sp[12].amp[9] / sp[12].amp[0] - 0.1f) < 1e-3f,
         "hex: 12 saw (1/h)");
  bool tonal = true;
  for (int w = 13; w <= 19; ++w) tonal = tonal && D(w).centroid > 1.0 && D(w).centroid < 2.8;
  expect(tonal, "hex: 13-19 tonal shapes (centroid 1..2.8)");
  expect(D(13).oddShare > 0.999 && D(14).top == 1 && D(16).top == 2 && D(17).top == 2, "hex: 13 odd soft clip, 14 half sine, 16/17 octave-led");
  expect(sp[18].amp[1] == 0.f && sp[18].amp[2] > 0.3f * sp[18].amp[0] && sp[18].amp[7] > 0.f, "hex: 18 sparse bell partials (1, 3, 4, 5, 8 …)");
  expect(D(19).topAbove2 == 4 && sp[19].amp[1] == 0.f, "hex: 19 reed formant at 4");
  expect(D(20).centroid > 2.5 && D(20).centroid < D(21).centroid && D(21).centroid < D(22).centroid && D(22).centroid < D(23).centroid &&
             D(23).centroid > 15,
         "hex: 20-23 noise, brighter with the index");
  expect(D(24).topAbove2 == 4, "hex: 24 formant at harmonic 4");
  bool f8 = true;
  for (int w = 25; w <= 32; ++w) {
    f8 = f8 && std::abs(D(w).topAbove2 - 8) <= 1 && D(w).centroid > 5 && D(w).centroid < 20;
    if (!(std::abs(D(w).topAbove2 - 8) <= 1)) std::printf("#   wave %d resonance at %d\n", w, D(w).topAbove2);
  }
  expect(f8, "hex: 25-32 formant / sync resonance near harmonic 8");

  // The same selector always gives the same wave (cached, stable).
  {
    const hx::Spectrum again = hx::builtinSpectrum(26);
    expect(std::equal(again.amp, again.amp + hx::kHarmonics, sp[26].amp) && std::equal(again.phase, again.phase + hx::kHarmonics, sp[26].phase),
           "hex: wave spectra stable across calls");
  }

  // ------------------------------------------- full timbre set without FL
  {
    bool all = true;
    std::vector<double> rms;
    for (int w = 1; w < hx::kWaves; ++w) {
      const std::vector<float> x = render(hexWith(w), 48, 22050);
      double e = 0;
      bool ok = true;
      for (size_t i = 2000; i < x.size(); ++i) ok = ok && std::isfinite(x[i]), e += static_cast<double>(x[i]) * x[i];
      e = std::sqrt(e / static_cast<double>(x.size() - 2000));
      if (!ok || e < 0.01) std::printf("#   wave %d rms %.4f\n", w, e), all = false;
      rms.push_back(e);
    }
    expect(all, "hex: every wave selector sounds with the decoy FL folder");
    expect(*std::max_element(rms.begin(), rms.end()) < 6 * *std::min_element(rms.begin(), rms.end()), "hex: wave levels within 16 dB of each other");
  }

  // ------------------------------------------------------- band limiting
  {
    double worst = 0;
    int worstWave = 0, worstNote = 0;
    for (int note : {96, 108, 115})
      for (int w : {8, 11, 12, 23, 26, 30, 32}) {
        const std::vector<float> x = render(hexWith(w), note, 16384);
        const double f0 = 440.0 * std::pow(2.0, (note - 69) / 12.0);
        const double s = inharmonicShare(x, 4096, 8192, f0, 44100.0);
        if (s > worst) worst = s, worstWave = w, worstNote = note;
      }
    std::printf("#   worst inharmonic share %.2e (wave %d, note %d)\n", worst, worstWave, worstNote);
    expect(worst < 1e-4, "hex: no aliasing at high notes (inharmonic power < -40 dB)");
  }

  // --------------------------------------------------- no file I/O in render
  {
    PluginState s = hexWith(27);
    auto g = packs::createGenerator(s.type);
    ChannelAux aux;
    packs::prepareAux(s, aux);
    ProcessContext ctx;
    ctx.sampleRate = 48000;
    std::vector<float> L(512), R(512);
    const int before = openFiles();
    g->noteOn(1, 60, 0.8f, 0);
    for (int i = 0; i < 40; ++i) g->process(s, aux, ctx, L.data(), R.data(), 512);
    expect(openFiles() == before, "hex: rendering opens no file");
  }

  // ------------------------------------------------- Kick Sculptor noises
  {
    namespace dr = gl::packs::models::dr;
    expect(dr::kickNoiseCount() == 8, "kick: eight noise layers");
    bool own = true, sound = true;
    double cen[8] = {};
    for (int k = 0; k < dr::kickNoiseCount(); ++k) {
      SamplePtr p = dr::kickNoiseSample(k);
      own = own && p && p->path.rfind("gridline:", 0) == 0 && p->mono();
      if (!p) continue;
      double e = 0, de = 0, pk = 0, dc = 0;
      for (size_t i = 1; i < p->frames(); ++i) {
        const double v = p->left[i], dv = v - p->left[i - 1];
        e += v * v, de += dv * dv, pk = std::max(pk, std::fabs(v)), dc += v;
      }
      cen[k] = e > 0 ? std::sqrt(de / e) : 0;  // first-difference brightness
      sound = sound && e > 1 && pk < 1.0 && std::fabs(dc / static_cast<double>(p->frames())) < 0.02 && p->seconds() > 0.3 && p->seconds() < 1.5;
      std::printf("#   noise %d %-12s %.2f s brightness %.3f peak %.2f\n", k, dr::kickNoiseLabel(k), p->seconds(), cen[k], pk);
    }
    expect(own, "kick: noise layers are Gridline's own (synthesised, not the decoy files)");
    expect(sound, "kick: noise layers audible, bounded, DC-free");
    expect(cen[2] > cen[1] && cen[6] > cen[5] && cen[7] > cen[3] && cen[0] > cen[5], "kick: metal / soft clap brighter than sweeps / low bits");
    expect(dr::kickNoiseSample(3) == dr::kickNoiseSample(3), "kick: noise layers built once and shared");
    PluginState s;
    s.type = "gl.kicksculpt";
    normalizePluginState(s);
    ChannelAux aux;
    packs::prepareAux(s, aux);
    bool all = aux.samples.size() == 8;
    for (const auto& p : aux.samples) all = all && p && p->path.rfind("gridline:", 0) == 0;
    expect(all, "kick: prepareAux hands every layer to the instrument");
  }

  std::error_code ec;
  fs::remove_all(decoy, ec);
  return finish();
}
