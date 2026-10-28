// Gridline — "platter" pack tests (J-160): Platter's output level through the
// whole engine (channel, mixer, master) against the same file played by the
// Sampler and as an audio clip. A track on one deck at Platter's defaults (fader
// up, crossfader in the centre, trim / EQ / master at 0 dB) comes out at the
// file's own level, like an audio clip; key lock (a track at another tempo than
// the project) keeps that level too; deck B matches deck A; the crossfader
// leaves the deck on its own side at full level. The mixer's dials (EQ, trim,
// master) are at 0 dB in their centre, like a DJ mixer's centre detents.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
#include "../Check.h"
#include "core/AudioBuffer.h"
#include "core/PluginInfo.h"
#include "core/Song.h"
#include "core/WavWriter.h"
#include "engine/Render.h"
#include "engine/Sample.h"
#include "packs/platter/platter.h"
#include "packs/seltmann/seltmann.h"

using namespace gl;
namespace pl = gl::packs::platter;
namespace fs = std::filesystem;

namespace {

constexpr int kSr = 44100;
constexpr double kPi = 3.14159265358979323846;

// Music-like material (partials and a click on every beat at `bpm`), 12 s.
std::string writeTrack(const std::string& name, double bpm, bool dense = false) {
  const auto n = static_cast<size_t>(12 * kSr);
  AudioBuffer b(2, n);
  for (size_t i = 0; i < n; ++i)
    for (int c = 0; c < 2; ++c) {
      const double t = static_cast<double>(i) / kSr;
      const double ph = std::fmod(t, 60.0 / bpm);
      double x = 0.25 * std::sin(2 * kPi * 220 * t + c) + 0.12 * std::sin(2 * kPi * 331 * t) + 0.08 * std::sin(2 * kPi * 1250 * t + 0.5 * c);
      x += 0.4 * std::exp(-ph * 60.0) * std::sin(2 * kPi * 90 * ph);
      if (dense) {  // a full mix: many partials and noise
        x *= 0.5;
        for (int k = 1; k <= 48; ++k) x += 0.03 * std::sin(2 * kPi * (37.0 * k + 13.0 * k * k / 7.0) * t + 0.7 * k * k + c * k);
        uint32_t h = static_cast<uint32_t>(i) * 2654435761u ^ (static_cast<uint32_t>(c) + 1u) * 0x9E3779B9u;
        h ^= h >> 15, h *= 0x2C1B3C6Du, h ^= h >> 12, h *= 0x297A2D39u, h ^= h >> 15;
        x += 0.3 * (static_cast<double>(h & 0xFFFF) / 32768.0 - 1.0);
      }
      b.data[static_cast<size_t>(c) * n + i] = static_cast<float>(x);
    }
  const fs::path d = fs::temp_directory_path() / "gl_platter_level_test";
  std::error_code ec;
  fs::create_directories(d, ec);
  const std::string path = (d / name).string();
  writeWav(path, b, kSr, 24);
  return path;
}

struct Level {
  double rmsDb = -200, peakDb = -200;
};
Level level(const AudioBuffer& b, double t0, double t1) {
  const auto a = static_cast<size_t>(t0 * kSr);
  const size_t e = std::min(b.frames, static_cast<size_t>(t1 * kSr));
  double acc = 0, pk = 0;
  for (size_t i = a; i < e; ++i)
    for (size_t c = 0; c < 2; ++c) {
      const double v = b.at(c, i);
      acc += v * v;
      pk = std::max(pk, std::fabs(v));
    }
  Level l;
  if (e > a && acc > 0) l.rmsDb = 10 * std::log10(acc / static_cast<double>(2 * (e - a)));
  if (pk > 0) l.peakDb = 20 * std::log10(pk);
  return l;
}
Level fileLevel(const std::string& path, double t0, double t1) {
  SamplePtr s = sampleCache().get(path);
  AudioBuffer b(2, s->frames());
  for (size_t i = 0; i < s->frames(); ++i) b.at(0, i) = s->left[i], b.at(1, i) = s->right[i];
  return level(b, t0, t1);
}

// 120 BPM song, one 4-bar pattern placed once.
Song baseSong(double tempo = 120) {
  Song s = makeEmptySong();
  s.tempo = tempo;
  s.patterns[0].bars = 4;
  Clip c;
  c.id = s.allocateId();
  c.pattern = s.patterns[0].id;
  s.playlist.push_back(c);
  return s;
}
ObjectId addChannel(Song& s, const std::string& type, const std::string& data) {
  Channel c;
  c.id = s.allocateId();
  c.name = type;
  c.plugin = defaultPluginState(type);
  c.plugin.data = data;
  c.mixerTrack = s.masterId();
  s.channels.push_back(c);
  return c.id;
}
Note note(Tick start, Tick len, int pitch) {
  Note n;
  n.start = start;
  n.length = len;
  n.pitch = pitch;
  n.velocity = 100;
  return n;
}
AudioBuffer render(const Song& s) {
  RenderOptions o;
  if (const char* r = std::getenv("GL_LEVEL_SR")) o.sampleRate = std::atoi(r);
  return renderSong(s, o);
}

// A Platter channel with `path` on deck `deck` (flat grid at `bpm`), a held hot cue key (plays from the cue).
Song platterSong(const std::string& path, double bpm, int deck, double tempo, const std::vector<std::pair<int, float>>& params = {}) {
  Song s = baseSong(tempo);
  pl::Model m;
  SamplePtr src = sampleCache().get(path);
  pl::DeckModel dm = pl::deckModel(*src, gl::packs::seltmann::flatGrid(*src, bpm), true);
  dm.cue = 0;
  pl::loadTrack(m, deck, path, dm);
  m.asked = true;
  const ObjectId ch = addChannel(s, pl::kType, pl::formatData(m.trackOf(0), m));
  for (const auto& [p, v] : params) s.channels.back().plugin.params[static_cast<size_t>(p)] = v;
  s.patterns[0].notes[ch] = {note(0, s.ppq * 14, deck == 0 ? pl::kHotKeyFirst : pl::kHotKeyFirstB)};
  return s;
}

double gDiff(const Level& a, const Level& b) { return a.rmsDb - b.rmsDb; }

void report(const char* what, const Level& l, const Level& ref) {
  std::printf("# %-44s rms %7.2f dB  peak %7.2f dB  (%+.2f dB vs the file)\n", what, l.rmsDb, l.peakDb, gDiff(l, ref));
}

}  // namespace

int main() {
  const std::string t120 = writeTrack("level120.wav", 120);
  const std::string t100 = writeTrack("level100.wav", 100);
  const double t0 = 1.0, t1 = 5.0;  // measured window (seconds of output)
  const Level file = fileLevel(t120, t0, t1);
  report("the file itself", file, file);

  // Sampler: the same file on a note at its root key
  Song smp = baseSong();
  const ObjectId sc = addChannel(smp, "gl.sampler", t120);
  smp.patterns[0].notes[sc] = {note(0, smp.ppq * 14, 60)};
  const Level lSampler = level(render(smp), t0, t1);
  report("Sampler (note C5, defaults)", lSampler, file);

  // Audio clip of the file
  Song clip = baseSong();
  clip.playlist.clear();
  const ObjectId ac = addChannel(clip, "gl.sampler", t120);
  Clip c;
  c.id = clip.allocateId();
  c.kind = ClipKind::Audio;
  c.source = ac;
  c.track = 0;
  c.start = 0;
  c.length = clip.barTicks() * 4;
  clip.playlist.push_back(c);
  const Level lClip = level(render(clip), t0, t1);
  report("audio clip", lClip, file);

  // Platter at its defaults
  const Level lA = level(render(platterSong(t120, 120, 0, 120)), t0, t1);
  report("Platter deck A (defaults)", lA, file);
  const Level lB = level(render(platterSong(t120, 120, 1, 120)), t0, t1);
  report("Platter deck B (defaults)", lB, file);
  const Level lKey = level(render(platterSong(t100, 100, 0, 120)), t0, t1);
  const Level file100 = fileLevel(t100, t0, t1);
  report("Platter deck A, 100 BPM track synced to 120", lKey, file100);
  const Level lNoKey = level(render(platterSong(t100, 100, 0, 120, {{pl::PKeyLock, 0.f}})), t0, t1);
  report("  same, key lock off", lNoKey, file100);
  const std::string d100 = writeTrack("dense100.wav", 100, true);
  const Level fileD = fileLevel(d100, t0, t1);
  const Level lDense = level(render(platterSong(d100, 100, 0, 120)), t0, t1);
  report("dense 100 BPM track, key lock to 120", lDense, fileD);
  const Level lDense2 = level(render(platterSong(d100, 100, 0, 120, {{pl::PKeyLock, 0.f}})), t0, t1);
  report("dense 100 BPM track, no key lock", lDense2, fileD);
  const Level lDense3 = level(render(platterSong(d100, 100, 0, 75, {{pl::PKeyLock, 0.f}})), t0, t1);
  report("dense 100 BPM track at 75, no key lock", lDense3, fileD);
  const Level lXa = level(render(platterSong(t120, 120, 0, 120, {{pl::PXfader, 0.f}})), t0, t1);
  report("Platter deck A, crossfader at A", lXa, file);
  const Level lXb = level(render(platterSong(t120, 120, 0, 120, {{pl::PXfader, 1.f}})), t0, t1);
  report("Platter deck A, crossfader at B", lXb, file);
  const Level lSmooth = level(render(platterSong(t120, 120, 0, 120, {{pl::PXfCurve, 0.f}})), t0, t1);
  report("Platter deck A, smoothest crossfader curve", lSmooth, file);

  // Platter at its defaults plays the file at its own level (unity), like an audio clip
  expect(std::fabs(gDiff(lA, file)) < 0.1, "deck A at defaults: the file's own level (unity, within 0.1 dB)");
  expect(std::fabs(lA.peakDb - file.peakDb) < 0.1, "deck A at defaults: the file's own peak");
  expect(std::fabs(gDiff(lA, lClip)) < 0.1, "deck A at defaults: the level of the same file as an audio clip");
  expect(std::fabs(gDiff(lB, file)) < 0.1, "deck B at defaults: unity");
  expect(std::fabs(gDiff(lKey, file100)) < 0.3, "key lock (a 100 BPM track synced to 120): unity");
  expect(std::fabs(gDiff(lNoKey, file100)) < 0.3, "no key lock at another tempo: unity");
  expect(std::fabs(gDiff(lDense, fileD)) < 1.5, "a dense track through key lock keeps its level");
  expect(std::fabs(gDiff(lXa, lA)) < 0.05, "crossfader at the deck's side: full level");
  expect(lXb.rmsDb < -120, "crossfader at the other side: silent");
  expect(std::fabs(gDiff(lSmooth, lA) + 3.0) < 0.2, "smoothest curve in the centre: constant power (-3 dB)");

  // J-160: the mixer's dials sit at 0 dB in their centre (12 o'clock), as on a DJ mixer
  const PluginInfo* info = findPlugin(pl::kType);
  expect(info != nullptr, "Platter in the catalogue");
  if (info) {
    for (int d = 0; d < pl::kDecks; ++d)
      for (int b : {pl::DEqHigh, pl::DEqMid, pl::DEqLow, pl::DTrim}) {
        const ParamDesc& pd = info->params[static_cast<size_t>(pl::deckParam(d, b))];
        expect(std::fabs(pd.fromNormalized(0.5)) < 1e-9 && std::fabs(pd.toNormalized(pd.def) - 0.5) < 1e-9,
               (pd.name + ": the centre is 0 dB, the default").c_str());
      }
    const ParamDesc& eq = info->params[pl::PEqHigh];
    expect(eq.fromNormalized(0.0) == pl::kEqKillDb && eq.fromNormalized(1.0) == 6.0, "EQ: kill at the bottom, +6 dB at the top");
    expect(eq.fromNormalized(0.25) < -12 && eq.fromNormalized(0.75) > 2.9 && eq.fromNormalized(0.75) < 3.1, "EQ: cuts below, boosts above");
    const ParamDesc& ms = info->params[pl::PMaster];
    expect(std::fabs(ms.fromNormalized(0.5)) < 1e-9 && std::fabs(ms.toNormalized(0.0) - 0.5) < 1e-9, "master: 0 dB at the centre");
    expect(ms.fromNormalized(0.0) == pl::kMasterOffDb && ms.fromNormalized(1.0) == 6.0, "master: off at the bottom, +6 dB at the top");
    // every dial turned to its centre (where a DJ sets a neutral mixer) plays at unity; with the
    // linear dials before J-160 that was -10 dB on each EQ band and -17 dB on the master
    std::vector<std::pair<int, float>> noon;
    for (int k : {pl::PEqHigh, pl::PEqMid, pl::PEqLow, pl::PTrim, pl::PMaster})
      noon.push_back({k, static_cast<float>(info->params[static_cast<size_t>(k)].fromNormalized(0.5))});
    const Level lNoon = level(render(platterSong(t120, 120, 0, 120, noon)), t0, t1);
    report("Platter deck A, every dial at 12 o'clock", lNoon, file);
    std::vector<std::pair<int, float>> oldNoon;
    for (int k : {pl::PEqHigh, pl::PEqMid, pl::PEqLow, pl::PTrim, pl::PMaster}) {
      const ParamDesc& pd = info->params[static_cast<size_t>(k)];
      oldNoon.push_back({k, static_cast<float>(pd.min + 0.5 * (pd.max - pd.min))});
    }
    report("  (before J-160: the same dials at 12 o'clock)", level(render(platterSong(t120, 120, 0, 120, oldNoon)), t0, t1), file);
    expect(std::fabs(gDiff(lNoon, file)) < 0.1, "every dial at its centre: unity");
    for (double n = 0; n <= 1.0; n += 0.05)
      expect(std::fabs(eq.toNormalized(eq.fromNormalized(n)) - n) < 1e-9 && std::fabs(ms.toNormalized(ms.fromNormalized(n)) - n) < 1e-9,
             "dials: the taper round-trips");
  }
  return finish();
}
