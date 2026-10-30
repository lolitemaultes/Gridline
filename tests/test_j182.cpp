// Gridline — J-182 tests: Kerf's time and marker settings (engine/KerfMarks.h):
// snap sizes (grid, triplets, zero crossings, grid then zero crossing,
// clamping), nudging by one snap step, the bars:beats ruler and position
// text, the classic engine's region names kept after the sample path, and the
// state round trips: project save / load keeps snap, ruler and names, older
// (shorter) states open with today's behaviour, FL export writes the path alone.
// All audio is synthetic.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <string>
#include <vector>
#include "Check.h"
#include "core/AudioBuffer.h"
#include "core/PluginInfo.h"
#include "core/PluginParams.h"
#include "core/Project.h"
#include "core/Song.h"
#include "core/WavWriter.h"
#include "core/flp/Flp.h"
#include "core/flp/FlpExport.h"
#include "engine/Generators.h"
#include "engine/KerfMarks.h"
#include "engine/Sample.h"
#include "engine/Slicing.h"

using namespace gl;
namespace km = gl::kerfmarks;
namespace fs = std::filesystem;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kRate = 44100;
constexpr double kFpb = 27562.5;  // 96 BPM at 44.1 kHz: a beat is not a whole number of frames
constexpr int kBeats = 8;

// A loop of kBeats beats: a 110 Hz sine shifted so no zero crossing sits on the beat grid,
// and a click on every beat.
Sample loopSample() {
  Sample s;
  s.sampleRate = kRate;
  const size_t n = static_cast<size_t>(kFpb * kBeats);
  s.left.resize(n);
  for (size_t i = 0; i < n; ++i) s.left[i] = static_cast<float>(0.5 * std::sin(2 * kPi * 110.0 * (static_cast<double>(i) + 37.0) / kRate));
  s.setMono();
  return s;
}

float monoAt(const Sample& s, int64_t i) { return (s.left[static_cast<size_t>(i)] + s.right[static_cast<size_t>(i)]) * 0.5f; }
bool isCrossing(const Sample& s, int64_t f) {
  if (f <= 0 || f >= static_cast<int64_t>(s.frames())) return false;
  const float a = monoAt(s, f - 1), b = monoAt(s, f);
  return (a <= 0 && b > 0) || (a >= 0 && b < 0) || b == 0;
}
// Distance of a frame from the nearest line of a grid of `step` frames.
double offGrid(int64_t f, double step) {
  const double k = std::round(static_cast<double>(f) / step);
  return std::fabs(static_cast<double>(f) - k * step);
}

void testSnapSizes() {
  const Sample s = loopSample();
  const int64_t n = static_cast<int64_t>(s.frames());
  expect(km::stepBeats(km::SnapBar) == 4 && km::stepBeats(km::SnapHalfBar) == 2 && km::stepBeats(km::SnapBeat) == 1 &&
             km::stepBeats(km::SnapHalfBeat) == 0.5 && std::fabs(km::stepBeats(km::SnapThirdBeat) - 1.0 / 3) < 1e-12 &&
             km::stepBeats(km::SnapQuarterBeat) == 0.25 && std::fabs(km::stepBeats(km::SnapSixthBeat) - 1.0 / 6) < 1e-12 &&
             km::stepBeats(km::SnapEighthBeat) == 0.125,
         "snap: each size is its fraction of a beat (bar = 4 beats)");
  expect(!km::isGrid(km::SnapOff) && !km::isGrid(km::SnapZero) && km::isGrid(km::SnapBar) && km::isGrid(km::SnapEighthBeat), "snap: off and zero crossings have no grid");
  // Every grid size: any frame lands within half a frame of the nearest line of its step, and on the nearest one.
  bool onGrid = true, nearest = true;
  for (int snap = km::SnapBar; snap <= km::SnapEighthBeat; ++snap) {
    const double step = km::stepBeats(snap) * kFpb;
    for (int64_t f = 0; f < n; f += 1777) {
      const int64_t g = km::snapFrame(&s, f, snap, false, kFpb);
      onGrid = onGrid && (offGrid(g, step) <= 0.5 + 1e-9 || g == n - 1);  // (the end line clamps to the last frame)
      nearest = nearest && (std::fabs(static_cast<double>(g - f)) <= step * 0.5 + 1 || g == n - 1);
      if (!onGrid || !nearest) std::printf("  snap %d: %lld -> %lld\n", snap, static_cast<long long>(f), static_cast<long long>(g));
    }
  }
  expect(onGrid, "snap: every grid size lands on its grid (within half a frame of the exact line)");
  expect(nearest, "snap: on the nearest line");
  // Triplets: 1/3 beat lines at 9187.5 frames; one third of the first beat rounds to 9188.
  expect(km::snapFrame(&s, 9000, km::SnapThirdBeat, false, kFpb) == 9188, "snap: triplet (1/3 beat) grid");
  expect(km::snapFrame(&s, 4400, km::SnapSixthBeat, false, kFpb) == 4594, "snap: 1/6 beat grid (triplet sixteenths)");
  expect(km::snapFrame(&s, 13000, km::SnapQuarterBeat, false, kFpb) == 13781, "snap: 1/4 beat grid");
  expect(km::snapFrame(&s, 100000, km::SnapBar, false, kFpb) == 110250, "snap: bar grid");
  // Clamping: before the start, past the end; a line past the end clamps to the last frame.
  expect(km::snapFrame(&s, -5000, km::SnapBeat, false, kFpb) == 0, "snap: before the start: frame 0");
  expect(km::snapFrame(&s, n + 9000, km::SnapOff, false, kFpb) == n - 1, "snap off: past the end: the last frame");
  expect(km::snapFrame(&s, n - 10, km::SnapBar, false, kFpb) == n - 1, "snap: the end line clamps to the last frame");
  expect(km::snapFrame(&s, 12345, km::SnapOff, false, kFpb) == 12345, "snap off: the frame itself");
  expect(km::snapFrame(nullptr, 12345, km::SnapBeat, false, kFpb) == 12345, "snap: no sample: unchanged");
  // Zero crossings, and grid then zero crossing.
  const int64_t z = km::snapFrame(&s, 30000, km::SnapZero, false, kFpb);
  expect(isCrossing(s, z) && std::llabs(z - 30000) <= static_cast<int64_t>(kRate * 0.005), "snap: zero crossings (within 5 ms)");
  const int64_t gz = km::snapFrame(&s, 26000, km::SnapBeat, true, kFpb);
  expect(isCrossing(s, gz) && std::fabs(static_cast<double>(gz) - kFpb) <= kRate * 0.005 && gz != 27563, "snap: grid, then the nearest zero crossing");
  // The grid line itself is not a crossing in this loop (the sine is shifted): the nudge to a crossing is real.
  expect(!isCrossing(s, 27563), "fixture: no crossing on the beat line");
}

void testNudge() {
  const Sample s = loopSample();
  const int64_t n = static_cast<int64_t>(s.frames());
  expect(km::nudgeFrame(&s, 27563, 1, km::SnapBeat, false, kFpb) == 55125, "nudge: from a line to the next line");
  expect(km::nudgeFrame(&s, 27563, -1, km::SnapBeat, false, kFpb) == 0, "nudge: from a line to the one before");
  expect(km::nudgeFrame(&s, 30000, 1, km::SnapBeat, false, kFpb) == 55125, "nudge: between lines: the next line");
  expect(km::nudgeFrame(&s, 30000, -1, km::SnapBeat, false, kFpb) == 27563, "nudge: between lines: the line before");
  expect(km::nudgeFrame(&s, 9188, 1, km::SnapThirdBeat, false, kFpb) == 18375, "nudge: triplet steps");
  expect(km::nudgeFrame(&s, n - 100, 1, km::SnapBar, false, kFpb) == n - 1, "nudge: clamped at the end");
  expect(km::nudgeFrame(&s, 10, -1, km::SnapBar, false, kFpb) == 0, "nudge: clamped at the start");
  expect(km::nudgeFrame(&s, 1000, 1, km::SnapOff, false, kFpb) == 1044, "nudge without snap: 1 ms");
  const int64_t z = km::nudgeFrame(&s, 30000, 1, km::SnapZero, false, kFpb);
  expect(z > 30000 && isCrossing(s, z) && z < 30000 + 300, "nudge: zero crossings: the next crossing");
  const int64_t zb = km::nudgeFrame(&s, z, -1, km::SnapZero, false, kFpb);
  expect(zb < z && isCrossing(s, zb), "nudge: zero crossings: the one before");
  const int64_t gz = km::nudgeFrame(&s, 30000, 1, km::SnapBeat, true, kFpb);
  expect(gz > 30000 && isCrossing(s, gz) && std::fabs(static_cast<double>(gz) - 2 * kFpb) <= kRate * 0.005, "nudge: grid then zero crossing moves on");
  // Repeated nudges walk the grid both ways and come back.
  int64_t f = 0;
  for (int i = 0; i < 12; ++i) f = km::nudgeFrame(&s, f, 1, km::SnapThirdBeat, false, kFpb);
  bool walk = std::llabs(f - static_cast<int64_t>(std::llround(4 * kFpb))) <= 1;
  for (int i = 0; i < 12; ++i) f = km::nudgeFrame(&s, f, -1, km::SnapThirdBeat, false, kFpb);
  expect(walk && f == 0, "nudge: twelve triplet steps make a bar, and back");
}

void testRulerText() {
  expect(km::loopBeats(5.0, false, 4) == 8, "ruler: a 5 s loop is 8 beats (96 BPM, between 80 and 160)");
  expect(km::loopBeats(5.0, true, 6) == 6, "ruler: a fitted loop keeps its beats");
  expect(std::fabs(km::framesPerBeat(kFpb * kBeats, kBeats) - kFpb) < 1e-9, "ruler: frames per beat");
  expect(km::barsBeats(0, kFpb, true) == "1:1:000", "bars:beats: the start is 1:1:000");
  expect(km::barsBeats(4 * kFpb, kFpb, true) == "2:1:000", "bars:beats: a bar later is 2:1:000");
  expect(km::barsBeats(1.5 * kFpb, kFpb, true) == "1:2:048", "bars:beats: half a beat is 48 ticks");
  expect(km::barsBeats(6 * kFpb, kFpb, false) == "2:3", "bars:beats without ticks");
  expect(km::barsBeats(2 * kFpb - 0.2, kFpb, true) == "1:3:000", "bars:beats: a hair before a beat rounds to it");
  double f = -1;
  expect(km::parsePosition("2:3:48", km::UnitBars, kRate, kFpb, f) && std::fabs(f - 6.5 * kFpb) < 1e-6, "parse: bars:beats:ticks");
  expect(km::parsePosition("2.3.048", km::UnitBars, kRate, kFpb, f) && std::fabs(f - 6.5 * kFpb) < 1e-6, "parse: dots work as separators");
  expect(km::parsePosition(" 3 ", km::UnitBars, kRate, kFpb, f) && std::fabs(f - 8 * kFpb) < 1e-6, "parse: a bar alone");
  expect(km::parsePosition("1:4", km::UnitBars, kRate, kFpb, f) && std::fabs(f - 3 * kFpb) < 1e-6, "parse: bar and beat");
  bool bad = true;
  for (const char* t : {"0:1", "1:5", "1:1:96", "abc", "1:2:3:4", "", "1::2", "-1"}) bad = bad && !km::parsePosition(t, km::UnitBars, kRate, kFpb, f);
  expect(bad, "parse: not positions (bar 0, beat 5 of 4, tick 96, words, too many parts, empty)");
  expect(km::parsePosition("1.25", km::UnitSeconds, kRate, kFpb, f) && std::fabs(f - 1.25 * kRate) < 1e-6, "parse: seconds");
  expect(km::parsePosition("1,5 s", km::UnitSeconds, kRate, kFpb, f) && std::fabs(f - 1.5 * kRate) < 1e-6, "parse: seconds with a comma and the unit");
  expect(!km::parsePosition("-2", km::UnitSeconds, kRate, kFpb, f) && !km::parsePosition("x", km::UnitSeconds, kRate, kFpb, f), "parse: bad seconds");
  expect(km::parsePosition("22050", km::UnitSamples, kRate, kFpb, f) && f == 22050, "parse: samples");
  expect(!km::parsePosition("-5", km::UnitSamples, kRate, kFpb, f) && !km::parsePosition("1.5", km::UnitSamples, kRate, kFpb, f), "parse: bad samples");
  // Format and parse agree in every unit.
  bool back = true;
  for (double at : {0.0, 9188.0, 110250.0, 123456.0})
    for (int u = 0; u < km::UnitCount; ++u) {
      double g = -1;
      back = back && km::parsePosition(km::formatPosition(at, u, kRate, kFpb), u, kRate, kFpb, g) && std::fabs(g - at) <= (u == km::UnitBars ? kFpb / 96 : u == km::UnitSeconds ? 23 : 0);
    }
  expect(back, "position text: format then parse comes back (to the unit's resolution)");
  // Ruler steps: labels at least minPx apart, and the smallest step that does it.
  bool steps = true;
  for (double ppb : {0.5, 3.0, 12.0, 47.0, 90.0, 400.0, 2000.0}) {
    const double st = km::rulerStepBeats(ppb, 46);
    steps = steps && (st * ppb >= 46 || st == 256) && (st == 0.125 || st / 2 * ppb < 46 || st == 2 || st == 4);
  }
  expect(steps, "ruler: bars:beats steps keep labels apart");
  expect(km::rulerStepSeconds(100, 46) == 0.5 && km::rulerStepSeconds(1000, 46) == 0.05, "ruler: seconds steps");
}

void testNames() {
  const std::string path = "/loops/Break 96.wav";
  expect(!km::hasNames(path) && km::pathOf(path) == path && km::namesOf(path).empty(), "names: a path alone has none");
  expect(km::withNames(path, {}) == path, "names: none: the data stays the path alone");
  const std::string d = km::withNames(path, {{1, "Kick"}, {7, " Snare\nroll "}, {3, ""}});
  expect(km::hasNames(d) && km::pathOf(d) == path, "names: kept after the path");
  const auto nm = km::namesOf(d);
  expect(nm.size() == 2 && nm.at(1) == "Kick" && nm.at(7) == "Snare roll", "names: read back (one line, trimmed, empty dropped)");
  expect(!km::hasNames(path + "\nslicex 1 393219\n") && !km::hasNames(path + "\nkerf-namesX"), "names: region-model data is not names");
  std::string longName(100, 'a');
  longName[62] = '\xC3', longName[63] = '\xA9', longName[64] = '\xC3', longName[65] = '\xA9';
  const std::string c = km::cleanName(longName);
  expect(c.size() <= 64 && c.size() >= 62 && (static_cast<unsigned char>(c.back()) & 0xC0) != 0xC0, "names: at most 64 bytes, never cut inside a character");
}

std::string tempDir() {
  const fs::path d = fs::temp_directory_path() / "gridline-test-j182";
  std::error_code ec;
  fs::create_directories(d, ec);
  return d.string();
}

std::string writeLoop() {
  const Sample s = loopSample();
  AudioBuffer b(2, s.frames());
  for (size_t i = 0; i < s.frames(); ++i) {
    // Clicks on the beats so the transient slicer finds them.
    const double ph = std::fmod(static_cast<double>(i), kFpb);
    const float v = ph < 2000 ? static_cast<float>(0.8 * std::exp(-ph / 300.0) * std::sin(2 * kPi * 180 * ph / kRate)) : 0.0f;
    b.data[i] = b.data[s.frames() + i] = v;
  }
  const std::string p = (fs::path(tempDir()) / "loop.wav").string();
  writeWav(p, b, static_cast<int>(kRate));
  return p;
}

void testState() {
  const PluginInfo* info = findPlugin("gl.slicer");
  expect(info && info->params.size() == static_cast<size_t>(pp::SlCount), "state: the editor settings are parameters of gl.slicer");
  if (!info) return;
  // An older state (saved before J-182) opens with snap off and a seconds ruler.
  PluginState old = defaultPluginState("gl.slicer");
  old.params.resize(static_cast<size_t>(pp::SlProcDeclick) + 1);
  old.params[pp::SlMode] = 3, old.params[pp::SlLoopBeats] = 8;
  normalizePluginState(old);
  expect(old.params.size() == static_cast<size_t>(pp::SlCount) && old.params[pp::SlSnap] == 0 && old.params[pp::SlSnapZero] == 0 &&
             old.params[pp::SlRuler] == 0 && old.params[pp::SlMode] == 3 && old.params[pp::SlLoopBeats] == 8,
         "state: an older state opens with snap off and a seconds ruler (its settings kept)");
  PluginState tiny{"gl.slicer", {1.0f}, "x.wav", "", ""};
  normalizePluginState(tiny);
  expect(tiny.params.size() == static_cast<size_t>(pp::SlCount) && tiny.params[pp::SlSnap] == 0 && tiny.params[pp::SlGridLines] == 1, "state: a short vector pads");

  // Project save / load keeps snap, ruler, grid and names.
  const std::string wav = writeLoop();
  Song s = makeEmptySong();
  Channel ch;
  ch.id = s.allocateId();
  ch.name = "Kerf";
  ch.plugin = defaultPluginState("gl.slicer");
  ch.plugin.params[pp::SlMode] = pp::SlModeManual;
  for (int k = 0; k < pp::kMaxSlices; ++k) ch.plugin.params[static_cast<size_t>(pp::slMarker(k))] = k < 4 ? static_cast<float>(k * 2.0 / kBeats) : -1.0f;
  ch.plugin.params[pp::SlSnap] = km::SnapThirdBeat;
  ch.plugin.params[pp::SlSnapZero] = 1;
  ch.plugin.params[pp::SlRuler] = km::RulerBars;
  ch.plugin.params[pp::SlGridLines] = 0;
  ch.plugin.data = km::withNames(wav, {{1, "Kick"}, {2, "Snare"}});
  ch.mixerTrack = s.masterId();
  s.channels.push_back(ch);
  const std::string proj = (fs::path(tempDir()) / "kerf.gridline").string();
  std::string err;
  Song back;
  const bool io = saveProject(proj, s, &err) && loadProject(proj, back, &err);
  expect(io && back.channels.size() == 1, "round trip: the project saves and loads");
  if (io && !back.channels.empty()) {
    const PluginState& b = back.channels[0].plugin;
    expect(b.params[pp::SlSnap] == km::SnapThirdBeat && b.params[pp::SlSnapZero] == 1 && b.params[pp::SlRuler] == km::RulerBars && b.params[pp::SlGridLines] == 0,
           "round trip: snap, then-zero-crossing, ruler and grid lines kept");
    const auto nm = km::namesOf(b.data);
    expect(km::pathOf(b.data) == wav && nm.size() == 2 && nm.at(1) == "Kick" && nm.at(2) == "Snare", "round trip: region names kept with the sample path");
  }
  // The named classic state plays as the classic engine from its sample (not as a region model).
  {
    ChannelAux aux;
    const bool ok = prepareBuiltinAux(ch.plugin, aux);
    expect(ok && aux.samples.size() == 1 && aux.samples[0] && aux.samples[0]->frames() == static_cast<size_t>(kFpb * kBeats) && !aux.custom &&
               !aux.slices.empty(),
           "play: names after the path leave the classic engine reading its sample");
  }
  // FL export: the plugin gets the sample path alone (names and editor settings stay in Gridline).
  {
    flp::ExportOptions o;
    o.useOrigin = false;
    flp::ExportReport er;
    flp::Bytes out;
    const bool ok = flp::exportFlp(s, out, er, o);
    Song fl;
    flp::Report rep;
    const bool ok2 = ok && importFlpBytes(out, fl, rep, &err, "", "", false);
    const bool one = ok2 && fl.channels.size() == 1 && fl.channels[0].plugin.type == "gl.slicer";
    expect(one, "FL export: the named Kerf channel exports and imports");
    if (one) {
      const PluginState& f = fl.channels[0].plugin;
      std::error_code ec;
      expect(!km::hasNames(f.data) && fs::equivalent(fs::path(km::pathOf(f.data)), fs::path(wav), ec), "FL export: the sample path alone reaches FL");
      int markers = 0;
      for (int k = 0; k < pp::kMaxSlices; ++k) markers += f.params[static_cast<size_t>(pp::slMarker(k))] >= 0;
      expect(markers == 4, "FL export: the markers go with it");
    }
  }
}

}  // namespace

int main() {
  testSnapSizes();
  testNudge();
  testRulerText();
  testNames();
  testState();
  return finish();
}
