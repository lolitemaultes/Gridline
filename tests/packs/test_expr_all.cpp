// Gridline - per-note pressure and timbre (MPE) on every built-in instrument.
//
// For each instrument in the catalogue (and each of its models) a held C4 is rendered plain, with
// its pressure lane at full, and with its timbre lane at the top and at the bottom. With the
// channel's expression left to the instrument, full pressure must change the sound (most
// instruments: the level up to 6 dB) and timbre up must sound different from timbre down (most:
// brighter); a centred timbre lane and zero pressure must leave the sound as it was; every render is
// finite and the same at block sizes 64 and 512. Instruments that make no sound on a plain held
// note (controllers, sequencers waiting for their own pattern) are listed and skipped; hits that have
// died away before the held part are measured over their start.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>
#include "../Check.h"
#include "../../src/core/PluginInfo.h"
#include "../../src/core/PluginParams.h"
#include "../../src/core/Project.h"
#include "../../src/core/WavWriter.h"
#include "../../src/engine/Render.h"

using namespace gl;

namespace {

constexpr int kSr = 44100;

Song baseSong() {
  Song s = makeEmptySong();
  s.tempo = 120;
  s.patterns[0].bars = 1;
  Clip c;
  c.id = s.allocateId();
  c.pattern = s.patterns[0].id;
  s.playlist.push_back(c);
  return s;
}

AudioBuffer render(const Song& s, int block) {
  RenderOptions o;
  o.blockSize = block;
  return renderSong(s, o);
}

bool finite(const AudioBuffer& b) {
  for (float v : b.data)
    if (!std::isfinite(v)) return false;
  return true;
}

// The mono mix's RMS and brightness (RMS of the first difference over RMS) over [t0, t1) s.
void measure(const AudioBuffer& b, double t0, double t1, double& level, double& bright) {
  const size_t a = static_cast<size_t>(t0 * kSr), e = std::min(b.frames, static_cast<size_t>(t1 * kSr));
  double s = 0, d = 0, prev = 0;
  for (size_t i = a; i < e; ++i) {
    const double x = 0.5 * (b.at(0, i) + b.at(1, i));
    s += x * x;
    if (i > a) d += (x - prev) * (x - prev);
    prev = x;
  }
  level = e > a ? std::sqrt(s / static_cast<double>(e - a)) : 0.0;
  bright = s > 0 ? std::sqrt(d / s) : 0.0;
}

// How different two renders are over [t0, t1) s, relative to the first's level.
double difference(const AudioBuffer& x, const AudioBuffer& y, double t0, double t1) {
  const size_t a = static_cast<size_t>(t0 * kSr), e = std::min({x.frames, y.frames, static_cast<size_t>(t1 * kSr)});
  double d = 0, r = 0;
  for (size_t i = a; i < e; ++i)
    for (int c = 0; c < 2; ++c) d += std::fabs(x.at(c, i) - y.at(c, i)), r += std::fabs(x.at(c, i));
  return r > 0 ? d / r : 0.0;
}

std::string sineWav() {
  AudioBuffer b(2, static_cast<size_t>(kSr * 3));
  for (size_t i = 0; i < b.frames; ++i) {
    const float v = static_cast<float>(0.5 * std::sin(2 * 3.14159265358979323846 * 261.6256 * static_cast<double>(i) / kSr));
    b.at(0, i) = v, b.at(1, i) = v;
  }
  const std::string path = (std::filesystem::temp_directory_path() / "gridline_test_expr_c4.wav").u8string();
  writeWav(path, b, kSr, 16);
  return path;
}

struct Case {
  std::string type;
  int modelParam = -1;
  double model = 0;
  std::string label() const { return modelParam < 0 ? type : type + " model " + std::to_string(static_cast<int>(model)); }
};

}  // namespace

int main() {
  std::vector<Case> cases;
  for (const PluginInfo& p : pluginCatalog()) {
    if (p.kind != PluginKind::Generator || p.hidden) continue;
    if (p.legacyModelParam >= 0 && p.legacyModelParam < static_cast<int>(p.params.size())) {
      const ParamDesc& d = p.params[static_cast<size_t>(p.legacyModelParam)];
      for (int v = static_cast<int>(d.min); v <= static_cast<int>(d.max); ++v) cases.push_back({p.type, p.legacyModelParam, static_cast<double>(v)});
    } else {
      cases.push_back({p.type});
    }
  }
  const std::string wav = sineWav();
  int checked = 0;
  std::string silent, noPressure, noTimbre;
  for (const Case& c : cases) {
    Song s = baseSong();
    const Tick q = s.ppq;
    Channel ch;
    ch.id = s.allocateId();
    ch.name = c.type;
    ch.plugin = defaultPluginState(c.type);
    if (c.modelParam >= 0 && c.modelParam < static_cast<int>(ch.plugin.params.size()))
      ch.plugin.params[static_cast<size_t>(c.modelParam)] = static_cast<float>(c.model);
    ch.mixerTrack = s.masterId();
    s.channels.push_back(ch);
    auto& notes = s.patterns[0].notes[ch.id];
    const Note plainNote(0, 4 * q, 60, 100);
    notes = {plainNote};
    AudioBuffer plain = render(s, 512);
    double l0 = 0, b0 = 0;
    measure(plain, 0.6, 1.4, l0, b0);
    const PluginInfo* info = findPlugin(c.type);
    if (l0 < 1e-4 && info && info->usesSample && s.channels.back().plugin.data.empty()) {  // sample instruments: the sine
      s.channels.back().plugin.data = wav;
      plain = render(s, 512);
      measure(plain, 0.6, 1.4, l0, b0);
    }
    double t0 = 0.6, t1 = 1.4;  // the held part of the note; hits that have died by then: their start
    if (l0 < 1e-4) {
      t0 = 0.02, t1 = 0.3;
      measure(plain, t0, t1, l0, b0);
    }
    if (l0 < 1e-4) {
      silent += " " + c.label();
      continue;
    }
    ++checked;
    auto with = [&](bool pressure, float v, int block) {
      Note n = plainNote;
      if (pressure) n.pressure = {{0, v}};
      else n.timbre = {{0, v}};
      notes = {n};
      return render(s, block);
    };
    const AudioBuffer p1 = with(true, 1.0f, 512), p0 = with(true, 0.0f, 512), tUp = with(false, 0.5f, 512), tDown = with(false, -0.5f, 512);
    const AudioBuffer tMid = with(false, 0.0f, 512), p64 = with(true, 1.0f, 64);
    double lp = 0, bp = 0, lu = 0, bu = 0, ld = 0, bd = 0;
    measure(p1, t0, t1, lp, bp);
    measure(tUp, t0, t1, lu, bu);
    measure(tDown, t0, t1, ld, bd);
    std::printf("# %-34s pressure: level x%.2f, bright x%.2f; timbre up / down: level x%.2f, bright x%.2f\n", c.label().c_str(), lp / l0, bp / b0,
                ld > 0 ? lu / ld : 0.0, bd > 0 ? bu / bd : 0.0);
    const std::string L = c.label();
    expect(finite(p1) && finite(tUp) && finite(tDown) && finite(p64), (L + ": expression renders finite").c_str());
    const bool pressureMoves = difference(plain, p1, t0, t1) > 0.05;
    const bool timbreMoves = difference(tDown, tUp, t0, t1) > 0.05;
    if (!pressureMoves) noPressure += " " + L;
    if (!timbreMoves) noTimbre += " " + L;
    expect(pressureMoves, (L + ": full pressure changes the held note").c_str());
    expect(timbreMoves, (L + ": timbre up and down sound different").c_str());
    expect(difference(plain, p0, t0, t1) < 0.01, (L + ": zero pressure leaves the note as it was").c_str());
    expect(difference(plain, tMid, t0, t1) < 0.01, (L + ": a centred timbre leaves the note as it was").c_str());
    expect(difference(p1, p64, t0, t1) < 0.02, (L + ": the same pressure response at block size 64").c_str());
  }
  std::printf("# %d of %zu instrument models checked; silent on a plain held note:%s\n", checked, cases.size(), silent.c_str());
  if (!noPressure.empty()) std::printf("# no pressure response:%s\n", noPressure.c_str());
  if (!noTimbre.empty()) std::printf("# no timbre response:%s\n", noTimbre.c_str());
  expect(checked >= 55, "every instrument but the controllers sounds on a held note and are checked");
  return finish();
}
