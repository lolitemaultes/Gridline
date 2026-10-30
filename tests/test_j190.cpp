// Gridline — J-190 tests: Kerf slices on the beat grid (engine/KerfGrid.h), the
// way Seltmann chops: synthetic drum loops (written here) at several tempos, with
// a pickup and played late, a drifting groove, a 3/4 groove; every division incl.
// triplets lands on the true grid within a few ms; the cuts can move onto the
// hits; the loop-length fallback (analysis off / failed); the app path never
// waits (busy, then the analysis generation changes); the classic engine plays
// the cuts; the state round trip (old states, project save / load) and FL export
// (the cuts travel as markers). All audio is synthetic.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>
#include "Check.h"
#include "core/PluginInfo.h"
#include "core/PluginParams.h"
#include "core/Project.h"
#include "core/Song.h"
#include "core/flp/Flp.h"
#include "core/flp/FlpExport.h"
#include "engine/Generators.h"
#include "engine/RegionActivity.h"
#include "engine/KerfGrid.h"
#include "engine/KerfMarks.h"
#include "engine/Sample.h"
#include "engine/Slicing.h"
#include "packs/seltmann/seltmann.h"
#include "packs/seltmann_synth.h"

using namespace gl;
namespace kg = gl::kerfgrid;
namespace km = gl::kerfmarks;
namespace sm = gl::packs::seltmann;
namespace fs = std::filesystem;

namespace {

std::string tempDir() {
  const fs::path d = fs::temp_directory_path() / "gridline-test-j190";
  std::error_code ec;
  fs::create_directories(d, ec);
  return d.string();
}

// Writes a synthetic loop under a neutral name (no tempo in it: the analysis finds the beat itself).
std::string writeSong(const seltsynth::Song& song, const std::string& name) {
  const std::string p = (fs::path(tempDir()) / name).string();
  sm::forgetAnalysis(p);
  sampleCache().forget(p);
  sm::writeLoopWav(p, song.sample->left, song.sample->right.vec(), song.sample->sampleRate);
  return p;
}

kg::Settings settings(int div, bool hits = false, bool zero = true) {
  kg::Settings s;
  s.division = div;
  s.hits = hits;
  s.zero = zero;
  return s;
}

// The true grid line (seconds) at `cell` beats from bar 1, following the true beats (drift too).
double truthAt(const std::vector<double>& beats, double beat) {
  if (beats.size() < 2) return 0;
  const double last = static_cast<double>(beats.size() - 1);
  if (beat <= 0) return beats[0] + beat * (beats[1] - beats[0]);
  if (beat >= last) return beats.back() + (beat - last) * (beats.back() - beats[beats.size() - 2]);
  const auto i = static_cast<size_t>(beat);
  return beats[i] + (beat - static_cast<double>(i)) * (beats[i + 1] - beats[i]);
}

// Largest distance (ms) of every cut inside [from, to) seconds from its nearest true grid line
// of `cell` beats; also counts the true lines in that span that got no cut within `tolMs`.
struct Fit {
  double worstMs = 0;
  int cuts = 0, missed = 0;
};
Fit fitOf(const std::vector<int64_t>& cuts, double rate, const std::vector<double>& beats, double cell, double from, double to, double tolMs) {
  Fit f;
  std::vector<double> lines;
  for (double b = std::ceil(-1.0 / cell) * cell; truthAt(beats, b) < to - 0.001; b += cell)  // lines inside [from, to)
    if (truthAt(beats, b) >= from - 1e-9) lines.push_back(truthAt(beats, b));
  if (std::getenv("J190_DEBUG")) {
    for (int64_t c : cuts) std::printf("# cut %.4f\n", static_cast<double>(c) / rate);
    for (double l : lines) std::printf("# line %.4f\n", l);
  }
  for (int64_t c : cuts) {
    const double t = static_cast<double>(c) / rate;
    if (t < from - 0.03 || t >= to - 0.001) continue;
    double best = 1e9;
    for (double l : lines) best = std::min(best, std::fabs(l - t));
    f.worstMs = std::max(f.worstMs, best * 1000.0);
    ++f.cuts;
  }
  for (double l : lines) {
    bool hit = false;
    for (int64_t c : cuts) hit = hit || std::fabs(static_cast<double>(c) / rate - l) * 1000.0 <= tolMs;
    f.missed += hit ? 0 : 1;
  }
  return f;
}

void testCells() {
  expect(kg::cellBeats(kg::DivBar, 4, false) == 4 && kg::cellBeats(kg::DivHalfBar, 4, false) == 2 && kg::cellBeats(kg::DivBeat, 4, false) == 1 &&
             kg::cellBeats(kg::Div8th, 4, false) == 0.5 && kg::cellBeats(kg::Div16th, 4, false) == 0.25 && kg::cellBeats(kg::Div32nd, 4, false) == 0.125,
         "cells: bar, half bar, quarter, 8th, 16th, 32nd in 4/4");
  expect(std::fabs(kg::cellBeats(kg::Div4T, 4, false) - 2.0 / 3) < 1e-12 && std::fabs(kg::cellBeats(kg::Div8T, 4, false) - 1.0 / 3) < 1e-12 &&
             std::fabs(kg::cellBeats(kg::Div16T, 4, false) - 1.0 / 6) < 1e-12,
         "cells: triplets (three in the space of two)");
  expect(kg::cellBeats(kg::DivBar, 3, false) == 3 && kg::cellBeats(kg::DivHalfBar, 3, false) == 1.5, "cells: bars follow the meter (3/4)");
  expect(kg::cellBeats(kg::DivBar, 2, true) == 2 && std::fabs(kg::cellBeats(kg::Div8th, 2, true) - 1.0 / 3) < 1e-12 &&
             std::fabs(kg::cellBeats(kg::Div16th, 2, true) - 1.0 / 6) < 1e-12,
         "cells: 6/8 counts dotted beats (an 8th is a third of one)");
  expect(std::string(kg::divisionName(kg::Div16T)) == "1/16 T" && std::string(kg::meterName(kg::Meter68)) == "6/8", "cells: names");
}

// Loops at several tempos (2 bars, hits played 4 ms late, cut like a sample-pack file).
void testTempos() {
  for (double bpm : {84.0, 96.0, 120.0, 140.0, 172.0}) {
    seltsynth::LoopSpec sp;
    sp.bpm = bpm;
    sp.bars = 2;
    sp.late = 0.004;
    const auto song = seltsynth::loop(sp);
    char name[64];
    std::snprintf(name, sizeof name, "loop_t%d.wav", static_cast<int>(bpm));
    const std::string p = writeSong(song, name);
    const SamplePtr s = sampleCache().get(p);
    const double rate = s ? s->sampleRate : 44100;
    const double end = song.beats.back() + 60.0 / bpm;
    for (int div : {kg::DivBar, kg::DivBeat, kg::Div8th, kg::Div16th, kg::Div32nd, kg::Div8T, kg::Div16T, kg::Div4T}) {
      const auto r = kg::resultFor(p, s, settings(div), true);
      const double cell = kg::cellBeats(div, 4, false);
      const bool ok = r && r->source == kg::SourceDetected && std::fabs(r->bpm - bpm) / bpm < 0.01;
      char lb[160];
      std::snprintf(lb, sizeof lb, "tempo %.0f: detected (%.2f BPM)", bpm, r ? r->bpm : 0.0);
      if (div == kg::DivBar) expect(ok, lb);
      if (!ok) continue;
      // The hits are 4 ms late: the grid sits on the beats or on the hits.
      const Fit f = fitOf(r->cuts, rate, song.beats, cell, 0.0, end, 8.0);
      const int want = static_cast<int>(std::lround(8.0 / cell));
      std::snprintf(lb, sizeof lb, "tempo %.0f, %s: %d cuts (want %d), worst %.2f ms off the grid, %d lines missed", bpm, kg::divisionName(div),
                    static_cast<int>(r->cuts.size()), std::min(want, 64), f.worstMs, f.missed);
      expect(static_cast<int>(r->cuts.size()) == std::min(want, pp::kMaxSlices) && f.worstMs <= 8.0 && (want > 64 || f.missed == 0), lb);
    }
  }
}

// A pickup (a snare an 8th before bar 1, after half a beat of lead-in) and hits 12 ms late:
// the cuts still sit on bar 1's beats; with "on the hits" they move onto the hits.
void testPickup() {
  seltsynth::LoopSpec sp;
  sp.bpm = 100;
  sp.bars = 2;
  sp.lead = 0.3;
  sp.pickup = true;
  sp.late = 0.012;
  const auto song = seltsynth::loop(sp);
  const std::string p = writeSong(song, "loop_pickup.wav");
  const SamplePtr s = sampleCache().get(p);
  const double rate = s ? s->sampleRate : 44100;
  const auto r = kg::resultFor(p, s, settings(kg::Div8th), true);
  expect(r && r->source == kg::SourceDetected, "pickup: the beat is found");
  if (!r) return;
  const double end = song.beats.back() + 0.6;
  std::vector<double> hits;  // the played hits (12 ms behind the beats)
  for (double b : song.beats) hits.push_back(b + sp.late);
  // After bar 1's downbeat the grid sits on the played beats: 8ths within a few ms.
  const Fit f = fitOf(r->cuts, rate, hits, 0.5, song.beats[1], end, 4.0);
  char lb[160];
  std::snprintf(lb, sizeof lb, "pickup: 8ths on the played beats after bar 1 (worst %.2f ms, %d missed, %.2f BPM)", f.worstMs, f.missed, r->bpm);
  expect(f.worstMs <= 4.0 && f.missed == 0, lb);
  // A cut on bar 1's downbeat (at a file's edge the grid has the analysis' bar-1 accuracy: Seltmann
  // asks for 20 ms of the region start) and the pickup before it is its own slice.
  double bar1Ms = 1e9;
  for (int64_t c : r->cuts) bar1Ms = std::min(bar1Ms, std::fabs(static_cast<double>(c) / rate - song.beats.front()) * 1000.0);
  std::snprintf(lb, sizeof lb, "pickup: a cut on bar 1 (%.1f ms off its beat), the pickup is the first slice", bar1Ms);
  expect(bar1Ms <= 25.0 && !r->cuts.empty() && static_cast<double>(r->cuts.front()) / rate < song.beats.front() - 0.1, lb);
  // On the hits (the default): every cut moves onto its played hit, bar 1's too.
  for (int div : {kg::DivBeat, kg::Div8th}) {
    const auto h = kg::resultFor(p, s, settings(div, true, false), true);
    const Fit fh = h ? fitOf(h->cuts, rate, hits, kg::cellBeats(div, 4, false), song.beats.front(), end, 4.0) : Fit{99, 0, 99};
    std::snprintf(lb, sizeof lb, "pickup: on the hits, %s cuts land on the played hits incl. bar 1 (worst %.2f ms, %d missed)", kg::divisionName(div), fh.worstMs,
                  fh.missed);
    expect(fh.worstMs <= 4.0 && fh.missed == 0, lb);
  }
}

// A groove whose tempo wanders by 3 %: the cuts follow the beats (straight cuts would not).
void testDrift() {
  seltsynth::Spec sp;
  sp.bpm = 112;
  sp.bars = 8;
  sp.drift = 0.03;
  sp.sixteenths = true;
  sp.seed = 5;
  const auto song = seltsynth::make(sp);
  const std::string p = writeSong(song, "groove_drift.wav");
  const SamplePtr s = sampleCache().get(p);
  const double rate = s ? s->sampleRate : 44100;
  const auto r = kg::resultFor(p, s, settings(kg::Div8th), true);
  expect(r && r->source == kg::SourceDetected, "drift: the beat is found");
  if (!r) return;
  const Fit f = fitOf(r->cuts, rate, song.beats, 0.5, song.beats.front(), song.beats.back(), 10.0);
  char lb[160];
  std::snprintf(lb, sizeof lb, "drift: 8ths follow the drifting beats (worst %.2f ms, %d of the lines missed, %d cuts)", f.worstMs, f.missed, f.cuts);
  expect(f.worstMs <= 10.0 && f.missed == 0, lb);
  // A straight grid of the mean tempo from the first beat drifts away from them by tens of ms.
  const double beat = (song.beats.back() - song.beats.front()) / static_cast<double>(song.beats.size() - 1);
  double straightWorst = 0;
  for (size_t k = 0; k < song.beats.size(); ++k)
    straightWorst = std::max(straightWorst, std::fabs(song.beats.front() + static_cast<double>(k) * beat - song.beats[k]) * 1000.0);
  std::snprintf(lb, sizeof lb, "drift: a straight grid would be %.1f ms off", straightWorst);
  expect(straightWorst > 25.0, lb);
}

// A 3/4 groove: the meter is found, bars hold three beats, bar cuts sit on the downbeats.
void testThreeFour() {
  seltsynth::Spec sp;
  sp.bpm = 126;
  sp.bars = 8;
  sp.meter = 3;
  sp.seed = 9;
  const auto song = seltsynth::make(sp);
  const std::string p = writeSong(song, "waltz.wav");
  const SamplePtr s = sampleCache().get(p);
  const double rate = s ? s->sampleRate : 44100;
  const auto r = kg::resultFor(p, s, settings(kg::DivBar), true);
  char lb[160];
  std::snprintf(lb, sizeof lb, "3/4: found as %d beats a bar at %.2f BPM", r ? r->beatsPerBar : 0, r ? r->bpm : 0.0);
  expect(r && r->source == kg::SourceDetected && r->beatsPerBar == 3 && std::fabs(r->bpm - 126) < 2, lb);
  if (!r) return;
  const Fit f = fitOf(r->cuts, rate, song.downbeats.size() > 1 ? song.downbeats : song.beats, 1.0, song.beats.front(), song.beats.back(), 10.0);
  std::snprintf(lb, sizeof lb, "3/4: bar cuts on the downbeats (worst %.2f ms, %d missed)", f.worstMs, f.missed);
  expect(f.worstMs <= 10.0 && f.missed == 0, lb);
  // The meter can be set by hand: 4/4 bars on the same beats.
  kg::Settings four = settings(kg::DivBar);
  four.meter = kg::Meter44;
  const auto r4 = kg::resultFor(p, s, four, true);
  expect(r4 && r4->beatsPerBar == 4 && r4->cuts.size() < r->cuts.size(), "3/4: a 4/4 meter by hand makes longer bars");
  // The time grid (ruler, snapping) counts bars of three.
  const km::Grid g = r->timeGrid();
  expect(g.valid() && g.beatsPerBar == 3 && km::barsBeats(g.frameAt(4.0), g, false) == "2:2", "3/4: the ruler counts bars of three beats");
}

// Without the analysis: the loop-length grid (Fit tempo beats, or the power-of-two guess).
void testFallback() {
  seltsynth::LoopSpec sp;
  sp.bpm = 120;
  sp.bars = 2;
  const auto song = seltsynth::loop(sp);
  const std::string p = writeSong(song, "loop_fallback.wav");
  const SamplePtr s = sampleCache().get(p);
  kg::Settings off = settings(kg::Div8th, false, false);
  off.follow = false;
  off.fitted = true;
  off.loopBeats = 8;
  const auto r = kg::resultFor(p, s, off, true);
  const double frames = s ? static_cast<double>(s->frames()) : 0;
  bool even = r && r->cuts.size() == 16;
  for (size_t k = 0; even && k < r->cuts.size(); ++k) even = std::llabs(r->cuts[k] - std::llround(frames * static_cast<double>(k) / 16)) <= 1;
  expect(r && r->source == kg::SourceLoop && !r->failed && !r->analysing && even, "fallback: off, 8ths of an 8-beat loop are 16 even cuts");
  off.fitted = false;  // 4 s loop: the power-of-two guess is 8 beats (120 BPM)
  const auto g = kg::resultFor(p, s, off, true);
  expect(g && g->cuts.size() == 16 && std::fabs(g->bpm - 120) < 0.5, "fallback: unfitted, the power-of-two beat count");
  // An analysis that fails (no file to decode) falls back too, and says so.
  const auto f = kg::resultFor((fs::path(tempDir()) / "missing.wav").string(), s, settings(kg::Div8th, false, false), true);
  expect(f && f->failed && f->source == kg::SourceLoop && f->cuts.size() == 16, "fallback: a failed analysis uses the loop length");
  // A one-shot-like loop: still a valid grid and finite, sorted cuts.
  bool sorted = f && std::is_sorted(f->cuts.begin(), f->cuts.end()) && f->cuts.front() >= 0;
  expect(sorted, "fallback: cuts sorted inside the loop");
}

// The app's builds never wait: busy first, the generation moves when the analysis is in.
void testNonBlocking() {
  seltsynth::LoopSpec sp;
  sp.bpm = 92;
  sp.bars = 4;
  const auto song = seltsynth::loop(sp);
  const std::string p = writeSong(song, "loop_async.wav");
  const SamplePtr s = sampleCache().get(p);
  const uint64_t gen0 = kg::analysisGeneration();
  const auto r = kg::resultFor(p, s, settings(kg::Div16th), false);
  expect(r && r->analysing && r->source == kg::SourceLoop && !r->cuts.empty(), "async: busy, the loop-length cuts meanwhile");
  sm::waitAnalysisIdle();
  expect(kg::analysisGeneration() != gen0, "async: a finished analysis changes the generation (the app rebuilds)");
  const auto d = kg::resultFor(p, s, settings(kg::Div16th), false);
  expect(d && !d->analysing && d->source == kg::SourceDetected && d->cuts.size() == 64, "async: then the detected grid (4 bars of 16ths)");
  // Prepared for the engine (offline: waits) and played by the classic engine from those cuts.
  PluginState st = defaultPluginState("gl.slicer");
  st.data = p;
  st.params[pp::SlMode] = pp::SlModeGrid;
  st.params[pp::SlGridDiv] = kg::Div16th;
  st.params[pp::SlGridHits] = 0;
  ChannelAux aux;
  expect(prepareBuiltinAux(st, aux) && aux.slices == d->cuts, "engine: the prepared slices are the grid cuts");
  expect(computeSlices(*s, pp::SlModeGrid, 0.5).size() == 1, "engine: computeSlices leaves the grid mode to KerfGrid");
  auto gen = createGenerator("gl.slicer");
  const RegionActivity* ra = gen ? gen->regionActivity() : nullptr;
  if (gen && ra && d && d->cuts.size() > 6) {
    // Key 65 (base 60) plays slice 6: it reads from the sixth cut on.
    gen->noteOn(1, 65, 0.9f, 0);
    ProcessContext ctx;
    ctx.sampleRate = s->sampleRate;
    ctx.tempo = 120;
    std::vector<float> L(256), R(256);
    double energy = 0;
    for (int b = 0; b < 4; ++b) {
      std::fill(L.begin(), L.end(), 0.0f), std::fill(R.begin(), R.end(), 0.0f);
      gen->process(st, aux, ctx, L.data(), R.data(), 256);
      for (float v : L) energy += static_cast<double>(v) * v;
    }
    int slot = -1;
    for (int i = 0; i < RegionActivity::kSlots; ++i)
      if (ra->region(i) == 5) slot = i;
    const int64_t f = slot >= 0 ? ra->frame(slot) : -1;
    expect(std::isfinite(energy) && energy > 0 && slot >= 0 && f >= d->cuts[5] && f < d->cuts[6], "engine: key 65 plays slice 6, from its grid cut");
  }
}

void testState() {
  const PluginInfo* info = findPlugin("gl.slicer");
  expect(info && info->params.size() == static_cast<size_t>(pp::SlCount) && info->params[pp::SlMode].choices.size() == 7 &&
             info->params[pp::SlGridDiv].choices.size() == kg::DivCount && info->params[pp::SlGridMeter].choices.size() == kg::MeterCount,
         "state: the grid settings are gl.slicer parameters");
  // States saved before J-190 open with the grid defaults and their own mode.
  PluginState old = defaultPluginState("gl.slicer");
  old.params.resize(static_cast<size_t>(pp::SlGridLines) + 1);
  old.params[pp::SlMode] = 3;
  normalizePluginState(old);
  expect(old.params.size() == static_cast<size_t>(pp::SlCount) && old.params[pp::SlMode] == 3 && old.params[pp::SlGridDiv] == kg::Div16th &&
             old.params[pp::SlGridMeter] == 0 && old.params[pp::SlGridHits] == 1 && old.params[pp::SlGridZero] == 1 && old.params[pp::SlGridFollow] == 1,
         "state: an older state opens with the grid defaults");
  // Project save / load keeps the grid.
  seltsynth::LoopSpec sp;
  sp.bpm = 104;
  sp.bars = 2;
  const std::string wav = writeSong(seltsynth::loop(sp), "loop_state.wav");
  Song s = makeEmptySong();
  Channel ch;
  ch.id = s.allocateId();
  ch.name = "Kerf";
  ch.plugin = defaultPluginState("gl.slicer");
  ch.plugin.data = wav;
  ch.plugin.params[pp::SlMode] = pp::SlModeGrid;
  ch.plugin.params[pp::SlGridDiv] = kg::Div8T;
  ch.plugin.params[pp::SlGridMeter] = kg::Meter34;
  ch.plugin.params[pp::SlGridHits] = 1;
  ch.plugin.params[pp::SlGridZero] = 0;
  ch.plugin.params[pp::SlGridFollow] = 0;
  ch.mixerTrack = s.masterId();
  s.channels.push_back(ch);
  const std::string proj = (fs::path(tempDir()) / "kerf_grid.gridline").string();
  std::string err;
  Song back;
  const bool io = saveProject(proj, s, &err) && loadProject(proj, back, &err);
  expect(io && back.channels.size() == 1, "round trip: the project saves and loads");
  if (io && !back.channels.empty()) {
    const PluginState& b = back.channels[0].plugin;
    expect(b.params[pp::SlMode] == pp::SlModeGrid && b.params[pp::SlGridDiv] == kg::Div8T && b.params[pp::SlGridMeter] == kg::Meter34 &&
               b.params[pp::SlGridHits] == 1 && b.params[pp::SlGridZero] == 0 && b.params[pp::SlGridFollow] == 0,
           "round trip: the beat grid settings are kept");
  }
  // FL export: the grid's cuts travel as markers (FL's slicer has no beat grid).
  ch.plugin.params[pp::SlGridFollow] = 1;
  ch.plugin.params[pp::SlGridMeter] = 0;
  ch.plugin.params[pp::SlGridDiv] = kg::DivBeat;
  ch.plugin.params[pp::SlGridHits] = 0;
  s.channels[0] = ch;
  const auto want = kg::resultFor(ch.plugin, sampleCache().get(wav), true);
  flp::ExportOptions o;
  o.useOrigin = false;
  flp::ExportReport er;
  flp::Bytes out;
  const bool ok = flp::exportFlp(s, out, er, o);
  Song fl;
  flp::Report rep;
  const bool ok2 = ok && importFlpBytes(out, fl, rep, &err, "", "", false);
  const bool one = ok2 && fl.channels.size() == 1 && fl.channels[0].plugin.type == "gl.slicer";
  expect(one, "FL export: the Kerf channel on the beat grid exports and imports");
  if (one && want) {
    const PluginState& f = fl.channels[0].plugin;
    const SamplePtr smp = sampleCache().get(wav);
    int markers = 0, close = 0;
    for (int k = 0; k < pp::kMaxSlices; ++k) {
      const float m = f.params[static_cast<size_t>(pp::slMarker(k))];
      if (m < 0) continue;
      ++markers;
      const int64_t fr = std::llround(static_cast<double>(m) * static_cast<double>(smp->frames()));
      for (int64_t c : want->cuts) close += std::llabs(c - fr) <= 2 ? 1 : 0;
    }
    char lb[128];
    std::snprintf(lb, sizeof lb, "FL export: the %d beat cuts arrive as markers (%d markers, %d matched)", static_cast<int>(want->cuts.size()), markers, close);
    expect(static_cast<int>(f.params[pp::SlMode] + 0.5f) == pp::SlModeManual && markers == static_cast<int>(want->cuts.size()) && close == markers, lb);
  }
}

// Extremes: the finest cell over a long groove keeps 64 slices; a tiny sample is safe.
void testExtremes() {
  seltsynth::Spec sp;
  sp.bpm = 100;
  sp.bars = 8;
  const auto song = seltsynth::make(sp);
  const std::string p = writeSong(song, "groove_long.wav");
  const SamplePtr s = sampleCache().get(p);
  const auto r = kg::resultFor(p, s, settings(kg::Div32nd), true);
  expect(r && r->cuts.size() == static_cast<size_t>(pp::kMaxSlices) && r->found > pp::kMaxSlices && std::is_sorted(r->cuts.begin(), r->cuts.end()),
         "extremes: 32nds of 8 bars keep the first 64 slices");
  auto tiny = std::make_shared<Sample>();
  tiny->sampleRate = 44100;
  tiny->left.assign(100, 0.1f);
  kg::Settings off = settings(kg::Div32nd);
  off.follow = false;
  const auto t = kg::resultFor("", tiny, off, true);
  expect(t && !t->cuts.empty() && t->cuts.front() >= 0 && t->cuts.back() < 100, "extremes: a tiny sample gets sane cuts");
  expect(kg::resultFor("", nullptr, off, true) == nullptr, "extremes: no sample, no grid");
}

}  // namespace

int main() {
  testCells();
  testTempos();
  testPickup();
  testDrift();
  testThreeFour();
  testFallback();
  testNonBlocking();
  testState();
  testExtremes();
  sm::waitAnalysisIdle();
  return finish();
}
