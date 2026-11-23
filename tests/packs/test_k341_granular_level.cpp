// Gridline - K-341: Granular's level next to Grain Scanner and the plain Sampler, offline.
// One steady tone sample, one note (C-4, velocity 0.8), each engine at its default settings; the
// RMS of the held part of the note is compared:
//   1. 0.7.7's Granular (the Classic level) was far quieter than Grain Scanner;
//   2. the Balanced level (new instances) plays within 1.5 dB of Grain Scanner;
//   3. and holds within 2 dB at other densities and grain sizes;
//   4. a Granular saved before opens on the Classic level (songs keep their sound).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>
#include "../../src/core/PluginInfo.h"
#include "../../src/core/PluginParams.h"
#include "../../src/engine/Generators.h"
#include "../../src/engine/Sample.h"
#include "../../src/packs/Packs.h"
#include "../Check.h"

using namespace gl;

namespace {

// 2 s of a steady 220 Hz sine at 0.5 (mono, 44.1 kHz): RMS 0.354.
std::string toneSample() {
  static std::string path;
  if (!path.empty()) return path;
  path = (std::filesystem::temp_directory_path() / "gl_k341_tone.wav").string();
  const int sr = 44100, n = sr * 2;
  std::vector<int16_t> pcm(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) pcm[static_cast<size_t>(i)] = static_cast<int16_t>(std::lround(std::sin(2 * 3.14159265358979 * 220.0 * i / sr) * 0.5 * 32767));
  std::FILE* f = std::fopen(path.c_str(), "wb");
  auto w32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
  auto w16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
  std::fwrite("RIFF", 1, 4, f), w32(36 + n * 2), std::fwrite("WAVEfmt ", 1, 8, f), w32(16), w16(1), w16(1), w32(sr), w32(sr * 2), w16(2), w16(16);
  std::fwrite("data", 1, 4, f), w32(static_cast<uint32_t>(n * 2)), std::fwrite(pcm.data(), 2, pcm.size(), f);
  std::fclose(f);
  return path;
}

// RMS (both channels) of 0.4 s .. 1.2 s of a note held 1.5 s.
double level(PluginState st, const char* name) {
  normalizePluginState(st);
  const double sr = 44100;
  const int total = static_cast<int>(1.5 * sr), block = 256;
  std::vector<float> L(static_cast<size_t>(total)), R(static_cast<size_t>(total));
  auto g = createGenerator(st.type);
  if (!g) return -1;
  ChannelAux aux;
  packs::prepareAux(st, aux);
  if (st.type != "gl.grainscan") aux.samples.push_back(loadSample(st.data));  // (as the program build does)
  ProcessContext ctx;
  ctx.sampleRate = sr;
  ctx.tempo = 120;
  for (int pos = 0; pos < total; pos += block) {
    const int n = std::min(block, total - pos);
    if (pos == 0) g->noteOn(1, 60, 0.8f, 0);
    g->process(st, aux, ctx, L.data() + pos, R.data() + pos, n);
  }
  double s = 0;
  const size_t a = static_cast<size_t>(0.4 * sr), b = static_cast<size_t>(1.2 * sr);
  for (size_t i = a; i < b; ++i) s += static_cast<double>(L[i]) * L[i] + static_cast<double>(R[i]) * R[i];
  const double rms = std::sqrt(s / (2.0 * static_cast<double>(b - a)));
  std::printf("# %-28s rms %.4f (%.1f dB)\n", name, rms, 20 * std::log10(std::max(rms, 1e-9)));
  return rms;
}

PluginState fresh(const char* type) {
  PluginState s;
  s.type = type;
  normalizePluginState(s);
  s.data = toneSample();
  return s;
}

double db(double a, double b) { return 20 * std::log10(std::max(a, 1e-9) / std::max(b, 1e-9)); }

}  // namespace

int main() {
  const double sampler = level(fresh("gl.sampler"), "Sampler");
  const double scan = level(fresh("gl.grainscan"), "Grain Scanner (default)");
  const double gran = level(fresh("gl.granular"), "Granular (default, Balanced)");
  PluginState classic = fresh("gl.granular");
  classic.params[pp::GrLevel] = 0;
  const double old = level(classic, "Granular (Classic, 0.7.7)");
  expect(sampler > 0.1 && scan > 0.1 && gran > 0 && old > 0, "every engine sounds");
  char b[200];
  std::snprintf(b, sizeof b, "0.7.7's Granular was far quieter than Grain Scanner (%.1f dB)", db(old, scan));
  expect(db(old, scan) < -4.5, b);
  std::snprintf(b, sizeof b, "Granular now plays within 1.5 dB of Grain Scanner (%.1f dB)", db(gran, scan));
  expect(std::fabs(db(gran, scan)) < 1.5, b);
  // The level holds at other densities and sizes (within 2 dB of the default).
  for (const auto& [dens, size] : {std::pair<float, float>{8, 0.05f}, {60, 0.08f}, {25, 0.25f}, {100, 0.2f}}) {
    PluginState o = fresh("gl.granular");
    o.params[pp::GrDensity] = dens, o.params[pp::GrSize] = size;
    char n[80];
    std::snprintf(n, sizeof n, "Granular %.0f grains/s, %.0f ms", dens, size * 1000);
    const double v = level(o, n);
    std::snprintf(b, sizeof b, "%s: within 2 dB of the default (%.1f dB)", n, db(v, gran));
    expect(std::fabs(db(v, gran)) < 2.0, b);
  }
  // A song saved before keeps the level it had: a state without the Level switch reads Classic.
  PluginState saved;
  saved.type = "gl.granular";
  normalizePluginState(saved);
  saved.params.resize(pp::GrLevel);
  normalizePluginState(saved);
  expect(saved.params.size() > static_cast<size_t>(pp::GrLevel) && saved.params[pp::GrLevel] == 0, "a Granular saved before opens on the Classic level");
  return finish();
}
