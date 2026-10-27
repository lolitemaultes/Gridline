// Gridline — "seltmann" pack tests: Seltmann (gl.seltmann) analysis accuracy
// on synthetic songs (tempo, beats, drift, swing, intros, tempo changes,
// half / double, meter, downbeats, sections), the model text form, chopping,
// layouts, the instrument (keys, pattern, block-size invariance, no
// allocation on the audio thread, click-free micro chops), loop renders and
// seams, project items made by drag-out / send to project, save / load, and
// background analysis of long files. J-107: any material (breaks with late
// hits, leading silence, pickups, tails, odd tempos, one-shots, ambient pads,
// songs): no degenerate slices, loop-length tempo fit, file-name tempo prior,
// automatic chop size, every layout on every kind, drop rules. J-149: regions split
// and deleted (joined) from the sequence, the sequence reordered, old states.
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <new>
#include <string>
#include <thread>
#include <vector>
#include "../Check.h"
#include "core/PluginBrand.h"
#include "core/AudioBuffer.h"
#include "core/BrowserIndex.h"
#include "core/ExternalDrop.h"
#include "core/PluginInfo.h"
#include "core/Project.h"
#include "core/Song.h"
#include "core/WavWriter.h"
#include "engine/Generators.h"
#include "packs/Packs.h"
#include "packs/edison/edison.h"
#include "packs/seltmann/seltmann.h"
#include "seltmann_synth.h"

using namespace gl;
namespace sm = gl::packs::seltmann;
namespace fs = std::filesystem;

namespace {

struct Score {
  double bpm = 0, f = 0, fDown = 0;
  bool bpmOk = false, octaveOk = false;
};

Score score(const seltsynth::Song& song, const sm::Analysis& a) {
  Score s;
  s.bpm = a.bpm;
  s.bpmOk = std::fabs(a.bpm - song.meanBpm) / song.meanBpm < 0.02;
  for (double f : {0.5, 2.0, 1.0})
    if (std::fabs(a.bpm - song.meanBpm * f) / (song.meanBpm * f) < 0.02) s.octaveOk = true;
  // scored over the annotated span (the grid also covers the ring-out after the last beat)
  std::vector<double> est;
  const double lo = song.beats.front() - 0.1, hi = song.beats.back() + 0.1;
  for (double b : a.beats)
    if (b / a.sampleRate >= lo && b / a.sampleRate <= hi) est.push_back(b / a.sampleRate);
  s.f = seltsynth::fmeasure(est, song.beats);
  std::vector<double> down;
  for (size_t k = static_cast<size_t>(a.bar1 % std::max(1, a.beatsPerBar)); k < a.beats.size(); k += static_cast<size_t>(std::max(1, a.beatsPerBar)))
    if (a.beats[k] / a.sampleRate >= lo && a.beats[k] / a.sampleRate <= hi) down.push_back(a.beats[k] / a.sampleRate);
  s.fDown = seltsynth::fmeasure(down, song.downbeats);
  if (std::getenv("SELTMANN_DEBUG")) {
    std::fprintf(stderr, "est %zu ref %zu first est %.3f ref %.3f last est %.3f ref %.3f\n", est.size(), song.beats.size(), est.empty() ? 0 : est[0],
                 song.beats[0], est.empty() ? 0 : est.back(), song.beats.back());
    double sum = 0;
    int c = 0;
    for (double e : est)
      for (double r : song.beats)
        if (std::fabs(e - r) < 0.05) sum += e - r, ++c;
    std::fprintf(stderr, "mean offset %.2f ms\n", c ? sum / c * 1000 : 0.0);
  }
  return s;
}

sm::Analysis analyseSong(const seltsynth::Song& song) {
  sm::Analysis a;
  sm::analyse(*song.sample, a);
  return a;
}

void report(const char* name, const seltsynth::Song& song, const sm::Analysis& a, const Score& s) {
  std::printf("# %-28s true %.2f  est %.2f (conf %.2f)  beatF %.3f  downF %.3f  meter %d%s  key %d%s\n", name, song.meanBpm, s.bpm, a.confidence,
              s.f, s.fDown, a.beatsPerBar, a.compound ? "c" : "", a.key, a.minor ? "m" : "");
}

void testTempo() {
  struct Case {
    const char* name;
    seltsynth::Spec spec;
    double minF;     // beat F-measure
    double minDown;  // downbeat F-measure (-1 = not checked)
    bool exact;      // the tempo itself (else the octave is accepted)
  };
  std::vector<Case> cases;
  auto add = [&](const char* n, seltsynth::Spec s, double f, double d, bool exact) { cases.push_back({n, s, f, d, exact}); };
  {
    seltsynth::Spec s;
    s.bpm = 120;
    add("120 straight", s, 0.95, 0.9, true);
  }
  {
    seltsynth::Spec s;
    s.bpm = 92;
    s.seed = 2;
    add("92 hip-hop", s, 0.95, 0.9, true);
  }
  {
    seltsynth::Spec s;
    s.bpm = 128;
    s.sixteenths = true;
    s.seed = 3;
    add("128 house 16ths", s, 0.95, 0.9, true);
  }
  {
    seltsynth::Spec s;
    s.bpm = 140;
    s.halfTime = true;
    s.seed = 4;
    add("140 half-time", s, 0.9, -1, false);
  }
  {
    seltsynth::Spec s;
    s.bpm = 174;
    s.seed = 5;
    add("174 fast", s, 0.9, -1, false);
  }
  {
    seltsynth::Spec s;
    s.bpm = 72;
    s.seed = 6;
    add("72 slow", s, 0.9, -1, false);
  }
  {
    seltsynth::Spec s;
    s.bpm = 110;
    s.drift = 0.03;
    s.bars = 24;
    s.seed = 7;
    add("110 drift 3%", s, 0.9, 0.8, true);
  }
  {
    seltsynth::Spec s;
    s.bpm = 100;
    s.swing = 0.6;
    s.sixteenths = true;
    s.seed = 8;
    add("100 swing", s, 0.95, 0.85, true);
  }
  {
    seltsynth::Spec s;
    s.bpm = 124;
    s.introBars = 4;
    s.bars = 20;
    s.seed = 9;
    add("124 drumless intro", s, 0.9, 0.85, true);
  }
  {
    seltsynth::Spec s;
    s.bpm = 120;
    s.bpm2 = 128;
    s.changeBar = 10;
    s.bars = 24;
    s.seed = 10;
    add("120 -> 128 change", s, 0.85, -1, false);
  }
  {
    seltsynth::Spec s;
    s.bpm = 150;
    s.meter = 3;
    s.bars = 24;
    s.seed = 11;
    add("150 waltz 3/4", s, 0.9, 0.8, true);
  }
  int exact = 0, octave = 0;
  double fsum = 0;
  for (const auto& c : cases) {
    const auto song = seltsynth::make(c.spec);
    const auto a = analyseSong(song);
    const Score s = score(song, a);
    report(c.name, song, a, s);
    exact += s.bpmOk;
    octave += s.octaveOk;
    fsum += s.f;
    char b[160];
    std::snprintf(b, sizeof b, "%s: tempo %s (%.2f vs %.2f)", c.name, c.exact ? "exact" : "or octave", s.bpm, song.meanBpm);
    expect(c.exact ? s.bpmOk : s.octaveOk, b);
    std::snprintf(b, sizeof b, "%s: beat F-measure %.3f >= %.2f", c.name, s.f, c.minF);
    // beat F is measured against the song's own beat level; an octave pick halves it by design
    expect(s.bpmOk ? s.f >= c.minF : true, b);
    if (c.minDown >= 0) {
      std::snprintf(b, sizeof b, "%s: downbeat F-measure %.3f >= %.2f", c.name, s.fDown, c.minDown);
      expect(s.fDown >= c.minDown, b);
    }
    if (c.spec.meter == 3) expect(a.beatsPerBar == 3, "waltz: 3 beats per bar");
    else expect(a.beatsPerBar == 4, (std::string(c.name) + ": 4 beats per bar").c_str());
  }
  std::printf("# synthetic: %d/%zu exact tempo, %d/%zu tempo or octave, mean beat F %.3f\n", exact, cases.size(), octave, cases.size(),
              fsum / cases.size());
}

void testSections() {
  seltsynth::Spec s;
  s.bpm = 118;
  s.bars = 32;
  s.sections = "ABAC";
  s.sectionBars = 8;
  s.seed = 21;
  const auto song = seltsynth::make(s);
  const auto a = analyseSong(song);
  std::string labels;
  std::vector<int> starts;
  for (const auto& sec : a.sections) {
    labels += sm::slotName(sec.label);
    starts.push_back((sec.firstBeat - a.bar1 % a.beatsPerBar) / a.beatsPerBar);
  }
  std::printf("# sections: %s at bars", labels.c_str());
  for (int b : starts) std::printf(" %d", b);
  std::printf("\n");
  expect(labels == "ABAC", "ABAC song: labels A B A C");
  bool near = starts.size() == 4;
  for (size_t i = 0; near && i < 4; ++i) near = std::abs(starts[i] - static_cast<int>(i) * 8) <= 1;
  expect(near, "ABAC song: boundaries within a bar of 0, 8, 16, 24");
  expect(!a.loops.empty(), "ABAC song: loop spots found");
  expect(a.key >= 0, "ABAC song: a key");
}

// ------------------------------------------------------------------ helpers
std::atomic<long> gAllocs{0};
thread_local bool gWatch = false;

fs::path tmpDir() {
  const fs::path d = fs::temp_directory_path() / "gl_seltmann_test";
  std::error_code ec;
  fs::create_directories(d, ec);
  return d;
}

// A synthetic song written to a WAV file (decoded back through the sample cache).
std::string songFile(const seltsynth::Song& song, const std::string& name) {
  const std::string path = (tmpDir() / name).string();
  sm::writeLoopWav(path, song.sample->left, song.sample->right.vec(), song.sample->sampleRate);
  return path;
}

PluginState stateWith(const std::string& path, const sm::Model* m) {
  PluginState st = defaultPluginState(sm::kType);
  st.data = m ? sm::formatData(path, *m) : path;
  return st;
}

struct Rendered {
  std::vector<float> L, R;
};

// Renders `frames` through a fresh instance in blocks of `block`; notes = (frame, key, length frames).
Rendered play(const PluginState& st, const ChannelAux& aux, int64_t frames, int block, const std::vector<std::array<int64_t, 3>>& notes,
              double tempo = 120, bool playing = false, bool watch = false) {
  auto g = createGenerator(sm::kType);
  Rendered r;
  r.L.assign(static_cast<size_t>(frames), 0.f);
  r.R.assign(static_cast<size_t>(frames), 0.f);
  ProcessContext ctx;
  ctx.sampleRate = 44100;
  ctx.tempo = tempo;
  ctx.ppq = 96;
  ctx.playing = playing;
  uint32_t id = 1;
  std::vector<std::pair<int64_t, uint32_t>> offs;
  offs.reserve(notes.size());
  for (int64_t pos = 0; pos < frames; pos += block) {
    const int n = static_cast<int>(std::min<int64_t>(block, frames - pos));
    ctx.tick = pos / 44100.0 * tempo / 60.0 * ctx.ppq;
    ctx.jumped = pos == 0;
    for (const auto& nt : notes)
      if (nt[0] >= pos && nt[0] < pos + n) {
        g->noteOn(id, static_cast<double>(nt[1]), 1.0f, static_cast<int>(nt[0] - pos));
        offs.push_back({nt[0] + nt[2], id++});
      }
    for (const auto& [at, nid] : offs)
      if (at >= pos && at < pos + n) g->noteOff(nid, static_cast<int>(at - pos));
    if (watch && pos > 0) gWatch = true;
    g->process(st, aux, ctx, r.L.data() + pos, r.R.data() + pos, n);
    gWatch = false;
  }
  return r;
}

double maxDiff(const std::vector<float>& a, const std::vector<float>& b) {
  double m = 0;
  for (size_t i = 0; i < std::min(a.size(), b.size()); ++i) m = std::max(m, static_cast<double>(std::fabs(a[i] - b[i])));
  return m;
}

double maxStep(const std::vector<float>& x) {
  double m = 0;
  for (size_t i = 1; i < x.size(); ++i) m = std::max(m, static_cast<double>(std::fabs(x[i] - x[i - 1])));
  return m;
}

// ------------------------------------------------------------------ model / chop / layouts
void testModel() {
  seltsynth::Spec sp;
  sp.bpm = 120;
  sp.bars = 12;
  const auto song = seltsynth::make(sp);
  sm::Analysis a;
  sm::analyse(*song.sample, a);
  sm::Model m = sm::defaultModel(*song.sample, a);
  expect(!m.slices.empty() && m.regionEnd > m.regionStart, "default model: region and slices");
  const double rb = sm::regionBars(m);
  expect(m.chop.autoSize && rb >= 11.99 && std::fabs(rb - std::round(rb)) < 0.01 && m.regionStart >= static_cast<int64_t>(0.04 * 44100),
         "default model: every whole bar after the leading silence, automatic chop size");
  const std::string data = sm::formatData("/x/song.wav", m);
  std::string path;
  sm::Model r;
  expect(sm::parseData(data, path, r) && path == "/x/song.wav", "text form: path back");
  expect(r.slices == m.slices && r.pattern.steps == m.pattern.steps && r.grid.beats.size() == m.grid.beats.size(),
         "text form: slices, steps, beats round trip");
  bool beatsClose = true;
  for (size_t i = 0; i < r.grid.beats.size(); ++i) beatsClose = beatsClose && std::fabs(r.grid.beats[i] - m.grid.beats[i]) < 0.01;
  expect(beatsClose, "text form: beat frames within 0.01");
  expect(sm::modelToText(r) == sm::modelToText(m), "text form: stable");
  expect(!sm::parseData("/x/song.wav", path, r) && path == "/x/song.wav", "bare path: no model");

  // chop by bars is exact on a straight grid (zero crossings off)
  sm::Model g;
  g.srcFrames = static_cast<int64_t>(song.sample->frames());
  g.srcRate = 44100;
  g.grid = sm::flatGrid(*song.sample, 120);
  g.chop.mode = sm::ChopGrid;
  g.chop.division = sm::Div1Bar;
  g.chop.zeroCross = false;
  sm::setRegionBars(g, 1, 4);
  sm::rechop(g, song.sample.get());
  bool exact = g.slices.size() == 4;
  for (size_t k = 0; exact && k < 4; ++k)
    exact = g.slices[k].start == static_cast<int64_t>((k + 1) * 88200) && g.slices[k].end == static_cast<int64_t>((k + 2) * 88200);
  expect(exact, "chop by bars: slices at exact bar frames");
  g.chop.division = sm::Div16th;
  sm::rechop(g, song.sample.get());
  expect(g.slices.size() == 64 && g.slices[1].start == 88200 + 5513, "chop by 1/16: 64 slices on the grid");
  g.chop.division = sm::Div8T;
  sm::rechop(g, song.sample.get());
  expect(g.slices.size() == 48, "chop by 1/8 triplets: 48 slices");
  g.chop.zeroCross = true;
  g.chop.division = sm::Div1Bar;
  sm::rechop(g, song.sample.get());
  bool nearZero = g.slices.size() == 4;
  for (size_t k = 0; nearZero && k < 4; ++k) nearZero = std::llabs(g.slices[k].start - static_cast<int64_t>((k + 1) * 88200)) <= 67;
  expect(nearZero, "zero-crossing snap stays within 1.5 ms");
  g.chop.mode = sm::ChopEqual;
  g.chop.count = 7;
  sm::rechop(g, song.sample.get());
  expect(g.slices.size() == 7, "equal slices: count");
  g.chop.mode = sm::ChopTransients;
  g.chop.sensitivity = 0.6f;
  sm::rechop(g, song.sample.get());
  expect(g.slices.size() >= 8, "transient chop: the drum hits");
  // manual edits
  g.chop.mode = sm::ChopGrid;
  g.chop.zeroCross = false;
  sm::rechop(g, song.sample.get());
  const int k = sm::addCut(g, 88200 + 44100);
  expect(k == 1 && g.slices.size() == 5 && g.slices[1].start == 88200 + 44100 && g.chop.mode == sm::ChopManual, "manual: add a cut");
  expect(sm::moveCut(g, 1, 88200 + 30000) && g.slices[0].end == 88200 + 30000, "manual: move a cut");
  expect(sm::removeCut(g, 1) && g.slices.size() == 4 && g.slices[0].end == 2 * 88200, "manual: remove a cut");
  // grid corrections
  sm::Analysis gg = sm::flatGrid(*song.sample, 120);
  const size_t nb = gg.beats.size();
  sm::gridDouble(gg);
  expect(std::fabs(gg.bpm - 240) < 1e-6 && gg.beats.size() >= 2 * nb - 1, "grid x2");
  sm::gridHalve(gg);
  expect(std::fabs(gg.bpm - 120) < 1e-6 && std::fabs(gg.beats[1] - 22050) < 1e-6, "grid /2");
  sm::gridSetBar1(gg, 4);
  sm::gridSetBpm(gg, 100, gg.beats[4]);
  expect(std::fabs(gg.beats[static_cast<size_t>(gg.bar1 + 1)] - gg.beats[static_cast<size_t>(gg.bar1)] - 26460) < 1e-6 &&
             std::fabs(gg.beats[static_cast<size_t>(gg.bar1)] - 88200) < 1e-6,
         "grid: typed BPM keeps bar 1");
  sm::gridNudge(gg, 441);
  expect(std::fabs(gg.beats[static_cast<size_t>(gg.bar1)] - 88641) < 1e-6, "grid: nudge");
  sm::gridMoveDownbeat(gg, 90000);
  expect(std::fabs(gg.beats[static_cast<size_t>(gg.bar1)] - 90000) < 1e-6, "grid: drag bar 1");
  sm::gridStretchTo(gg, gg.bar1 + 8, 90000 + 8 * 27000);
  expect(std::fabs(gg.beats[static_cast<size_t>(gg.bar1 + 4)] - (90000 + 4 * 27000)) < 1e-6, "grid: stretch between bar 1 and a marker");
  expect(std::fabs(sm::tapTempo({0, 0.5, 1.0, 1.5, 2.0}) - 120) < 1e-6, "tap tempo");
}

void testPatterns() {
  sm::Pattern p;
  std::string err;
  expect(sm::patternFromText("A B:r:p+7 . 3:x4:g-6 ~ C:l50:o25", p, &err), "DSL: parse");
  expect(p.steps.size() == 5 && p.steps[1].reverse && p.steps[1].pitch == 7 && p.steps[2].src == sm::kRest && !p.steps[3].slot &&
             p.steps[3].src == 2 && p.steps[3].stutter == 4 && p.steps[3].gain == -6 && p.steps[3].ties == 1 && p.steps[4].length == 0.5f &&
             p.steps[4].offset == 0.25f,
         "DSL: fields");
  sm::Pattern q;
  expect(sm::patternFromText(sm::patternToText(p), q) && q.steps == p.steps, "DSL: print / parse round trip");
  expect(!sm::patternFromText("A ?", q, &err) && !err.empty(), "DSL: errors are reported");

  const auto& pr = sm::factoryPresets();
  int perCat[sm::CatCount] = {};
  for (const auto& x : pr) ++perCat[x.category];
  std::printf("# layouts: %zu (Phrase %d, Loop %d, Breaks %d, Micro %d, Fills %d, Creative %d)\n", pr.size(), perCat[0], perCat[1], perCat[2],
              perCat[3], perCat[4], perCat[5]);
  expect(pr.size() >= 40, "40+ factory layouts");
  bool allCats = true;
  for (int c = 0; c < sm::CatUser; ++c) allCats = allCats && perCat[c] >= 6;
  expect(allCats, "six categories with six or more layouts each");
  // every layout applies and makes a playable pattern on a real song
  seltsynth::Spec sp;
  sp.bpm = 120;
  sp.bars = 16;
  const auto song = seltsynth::make(sp);
  sm::Analysis a;
  sm::analyse(*song.sample, a);
  bool all = true;
  for (const auto& x : pr) {
    sm::Model m = sm::defaultModel(*song.sample, a);
    sm::applyPreset(m, song.sample.get(), x, 7);
    if (m.pattern.steps.empty() || m.slices.empty()) all = false, std::printf("# layout %s: empty\n", x.name.c_str());
  }
  expect(all, "every layout applies");
  auto order = [&](const char* name) {
    sm::Model m = sm::defaultModel(*song.sample, a);
    for (const auto& x : pr)
      if (x.name == name) sm::applyPreset(m, song.sample.get(), x, 1);
    std::vector<int> o;
    for (const auto& st : m.pattern.steps) o.push_back(sm::stepSlice(m.pattern, st, static_cast<int>(m.slices.size())));
    return o;
  };
  expect(order("Verse Turn") == std::vector<int>{0, 1, 0, 2}, "Verse Turn plays A B A C");
  expect(order("Triple Lift") == std::vector<int>{0, 0, 0, 1}, "Triple Lift plays A A A B");
  expect(order("Mirror") == std::vector<int>{0, 1, 1, 0}, "Mirror plays A B B A");
  expect(order("Pairs") == std::vector<int>{0, 0, 1, 1}, "Pairs plays A A B B");
  expect(order("Kick Swap") == std::vector<int>{0, 1, 2, 3, 0, 1, 6, 7, 8, 9, 10, 11, 0, 1, 14, 15}, "Kick Swap re-orders the break");
  {
    sm::Model m = sm::defaultModel(*song.sample, a);
    for (const auto& x : pr)
      if (x.name == "Verse Turn") sm::applyPreset(m, song.sample.get(), x, 1);
    m.pattern.slots[2] = 3;  // reassign slot C to slice 4
    expect(sm::stepSlice(m.pattern, m.pattern.steps[3], static_cast<int>(m.slices.size())) == 3, "slot letters can be reassigned");
  }
  // user presets
  {
    sm::Model m = sm::defaultModel(*song.sample, a);
    for (const auto& x : pr)
      if (x.name == "Ghost Notes") sm::applyPreset(m, song.sample.get(), x, 1);
    const std::string dir = (tmpDir() / "presets").string();
    expect(sm::saveUserPreset(dir, "My Ghosts", m, 0.25f, 1), "user preset: save");
    const auto u = sm::loadUserPresets(dir);
    bool found = false;
    for (const auto& x : u)
      if (x.name == "My Ghosts") {
        found = true;
        sm::Model m2 = sm::defaultModel(*song.sample, a);
        sm::applyPreset(m2, song.sample.get(), x, 1);
        expect(m2.pattern.steps == m.pattern.steps && m2.slices.size() == m.slices.size(), "user preset: load gives the same layout");
      }
    expect(found, "user preset: listed");
  }
}

// ------------------------------------------------------------------ J-149 regions and the sequence
std::vector<int> stepOrder(const sm::Model& m) {
  std::vector<int> o;
  for (const auto& st : m.pattern.steps) o.push_back(sm::stepSlice(m.pattern, st, static_cast<int>(m.slices.size())));
  return o;
}
std::vector<int> stepUnits(const sm::Model& m) {
  std::vector<int> o;
  for (const auto& st : m.pattern.steps) o.push_back(1 + st.ties);
  return o;
}
bool tiles(const sm::Model& m) {  // the regions tile the chopped region, no gaps
  if (m.slices.empty() || m.slices.front().start != m.regionStart || m.slices.back().end != m.regionEnd) return false;
  for (size_t i = 1; i < m.slices.size(); ++i)
    if (m.slices[i].start != m.slices[i - 1].end || m.slices[i].end <= m.slices[i].start) return false;
  return true;
}
// Energy of the difference between two renders, as a share of the first one's energy (-1: different lengths).
double renderDiff(const sm::Model& a, const sm::Model& b, SamplePtr src) {
  sm::RenderOpts o;
  o.sampleRate = 44100;
  o.tempo = 120;
  o.sync = sm::SyncOriginal;
  std::vector<float> L0, R0, L1, R1;
  if (!sm::renderPatternLoop(a, src, o, L0, R0) || !sm::renderPatternLoop(b, src, o, L1, R1) || L0.size() != L1.size()) return -1;
  double e0 = 0, ed = 0;
  for (size_t i = 0; i < L0.size(); ++i) e0 += double(L0[i]) * L0[i], ed += double(L0[i] - L1[i]) * (L0[i] - L1[i]);
  return e0 > 0 ? ed / e0 : -1;
}

void testRegions() {
  expect(sm::regionName(0) == "A" && sm::regionName(3) == "D" && sm::regionName(25) == "Z" && sm::regionName(26) == "AA" &&
             sm::regionName(27) == "AB" && sm::regionName(52) == "BA" && sm::regionName(-1) == "-",
         "regions: letters A..Z, then AA, AB ...");
  seltsynth::Spec sp;
  sp.bpm = 120;
  sp.bars = 8;
  sp.seed = 21;
  const auto song = seltsynth::make(sp);
  SamplePtr src = song.sample;
  // A B C D: four one-bar regions on a straight grid (exact frames)
  sm::Model base;
  base.srcFrames = static_cast<int64_t>(src->frames());
  base.srcRate = 44100;
  base.grid = sm::flatGrid(*src, 120);
  base.chop.mode = sm::ChopGrid;
  base.chop.division = sm::Div1Bar;
  base.chop.zeroCross = false;
  sm::setRegionBars(base, 1, 4);
  sm::rechop(base, src.get());
  sm::straightPattern(base);
  const int64_t r0 = base.regionStart, r1 = base.regionEnd;
  expect(base.slices.size() == 4 && sm::isNaturalOrder(base) && stepOrder(base) == std::vector<int>{0, 1, 2, 3},
         "sequence: after the chop it plays A B C D in order");
  {
    sm::Analysis an;
    sm::analyse(*src, an);
    const sm::Model d = sm::defaultModel(*src, an);
    expect(sm::isNaturalOrder(d), "sequence: a dropped file starts in natural order");
  }
  const double beats0 = base.pattern.beats();

  // ---- split B in half: A B C D -> A B C D E (B and C are B's halves)
  {
    sm::Model m = base;
    const sm::Slice b = m.slices[1];
    expect(sm::splitRegion(m, 1, 2), "split: B in half");
    expect(m.slices.size() == 5 && m.slices[1].start == b.start && m.slices[2].end == b.end && m.slices[1].end == m.slices[2].start &&
               m.slices[1].end == b.start + (b.end - b.start) / 2,
           "split: the first half keeps B, the second half is the new C");
    expect(m.slices[3].start == base.slices[2].start && m.slices[4].start == base.slices[3].start, "split: old C, D are now D, E");
    expect(tiles(m) && m.regionStart == r0 && m.regionEnd == r1, "split: the regions still tile the chopped region");
    expect(stepOrder(m) == std::vector<int>{0, 1, 2, 3, 4} && sm::isNaturalOrder(m), "split: the sequence follows (A B C D E)");
    expect(std::fabs(m.pattern.unitBeats - 2.0) < 1e-9 && stepUnits(m) == std::vector<int>{2, 1, 1, 2, 2} &&
               std::fabs(m.pattern.beats() - beats0) < 1e-9,
           "split: halves get half a step, the pattern length stays");
    expect(m.chop.mode == sm::ChopManual && m.pattern.name == sm::kEditedName, "split: the chop is by hand now, the sequence edited");
    const double d = renderDiff(base, m, src);
    std::printf("# split render difference: %.5f of the energy\n", d);
    expect(d >= 0 && d < 0.02, "split: the loop sounds the same (same length, < 2% difference)");
    sm::Model r;
    std::string path;
    expect(sm::parseData(sm::formatData("/x/a.wav", m), path, r) && r.slices == m.slices && r.pattern.steps == m.pattern.steps &&
               std::fabs(r.pattern.unitBeats - m.pattern.unitBeats) < 1e-9 && r.pattern.name == m.pattern.name,
           "split: saved and loaded back");
    // a tempo fix re-chops the hand cuts and keeps the edited sequence
    sm::Model k = m;
    sm::gridNudge(k.grid, 10);
    sm::rechopKeepPattern(k, src.get());
    expect(k.slices.size() == 5 && k.pattern.steps == m.pattern.steps, "split: a re-chop with the same regions keeps the sequence");
  }
  // ---- split into three: the step unit is refined, every other step keeps its length
  {
    sm::Model m = base;
    expect(sm::splitRegion(m, 0, 3) && m.slices.size() == 6 && tiles(m), "split: A in thirds");
    expect(stepOrder(m) == std::vector<int>{0, 1, 2, 3, 4, 5} && stepUnits(m) == std::vector<int>{1, 1, 1, 3, 3, 3} &&
               std::fabs(m.pattern.beats() - beats0) < 1e-9,
           "split: thirds (A B C, then D E F are the old B C D)");
    sm::Model q = m;
    expect(sm::splitRegion(q, 0, 2) && std::fabs(q.pattern.beats() - beats0) < 1e-9 && q.slices.size() == 7 && tiles(q),
           "split again: halves of a third, the pattern length stays");
  }
  // ---- repeated letters: every occurrence becomes the pieces
  {
    sm::Model m = base;
    sm::patternFromText("1 2 1 3", m.pattern);
    m.pattern.name = sm::kEditedName;
    expect(sm::splitRegion(m, 0, 2), "split: A that plays twice");
    expect(stepOrder(m) == std::vector<int>{0, 1, 2, 0, 1, 3} && std::fabs(m.pattern.beats() - beats0) < 1e-9,
           "split: A B A C -> A B C A B D (each A is both halves)");
  }
  // ---- a reversed step plays the pieces backwards
  {
    sm::Model m = base;
    sm::patternFromText("1 2:r 3 4", m.pattern);
    expect(sm::splitRegion(m, 1, 2) && stepOrder(m) == std::vector<int>{0, 2, 1, 3, 4} && m.pattern.steps[1].reverse &&
               m.pattern.steps[2].reverse,
           "split: a reversed B plays C then B, both reversed");
  }
  // ---- slot letters (layouts) resolve to their regions first
  {
    sm::Model m = base;
    sm::patternFromText("A B A C", m.pattern);
    expect(sm::splitRegion(m, 1, 2) && stepOrder(m) == std::vector<int>{0, 1, 2, 0, 3}, "split: layout letters follow the regions");
  }
  // ---- limits
  {
    sm::Model m = base;
    expect(!sm::splitRegion(m, 1, 1) && !sm::splitRegion(m, 1, sm::kMaxSplit + 1) && !sm::splitRegion(m, 9, 2) && m.slices.size() == 4,
           "split: bad counts and missing regions change nothing");
    m.slices[1].end = m.slices[1].start + 40;  // (a tiny region: pieces would be too short)
    m.slices[2].start = m.slices[1].end;
    expect(!sm::splitRegion(m, 1, 2), "split: no pieces shorter than the shortest slice");
  }
  // ---- delete (join) regions
  {
    sm::Model m = base;
    expect(sm::mergeRegion(m, 2), "delete C");
    expect(m.slices.size() == 3 && m.slices[1].start == base.slices[1].start && m.slices[1].end == base.slices[2].end && tiles(m) &&
               m.regionStart == r0 && m.regionEnd == r1,
           "delete C: B and C join (B keeps its letter), the region stays the same");
    expect(stepOrder(m) == std::vector<int>{0, 1, 2} && stepUnits(m) == std::vector<int>{1, 2, 1} && std::fabs(m.pattern.beats() - beats0) < 1e-9,
           "delete C: the sequence is A B C, B as long as both");
    const double d = renderDiff(base, m, src);
    std::printf("# join render difference: %.5f of the energy\n", d);
    expect(d >= 0 && d < 0.02, "delete C: the loop sounds the same");
  }
  {
    sm::Model m = base;
    expect(sm::mergeRegion(m, 0) && m.slices.size() == 3 && m.slices[0].start == r0 && m.slices[0].end == base.slices[1].end && tiles(m) &&
               stepOrder(m) == std::vector<int>{0, 1, 2} && stepUnits(m) == std::vector<int>{2, 1, 1},
           "delete A (the first): it joins B, the joined region is A");
  }
  {
    sm::Model m = base;
    expect(sm::mergeRegion(m, 3) && m.slices.size() == 3 && m.slices[2].start == base.slices[2].start && m.slices[2].end == r1 && tiles(m) &&
               stepOrder(m) == std::vector<int>{0, 1, 2} && stepUnits(m) == std::vector<int>{1, 1, 2},
           "delete D (the last): it joins C, never past the region's end");
  }
  {
    sm::Model m = base;
    sm::patternFromText("3 1", m.pattern);  // C on its own
    expect(sm::mergeRegion(m, 2) && stepOrder(m) == std::vector<int>{1, 0} && std::fabs(m.pattern.steps[0].offset - 0.5f) < 1e-6,
           "delete C: a C on its own plays the joined region from C's half");
    sm::Model one = base;
    while (one.slices.size() > 1) sm::mergeRegion(one, 1);
    expect(one.slices.size() == 1 && one.slices[0].start == r0 && one.slices[0].end == r1 && stepOrder(one) == std::vector<int>{0},
           "delete down to one region: the whole region, one step");
    expect(!sm::mergeRegion(one, 0), "a single region cannot be deleted");
    sm::Model r;
    std::string path;
    expect(sm::splitRegion(one, 0, 4) && sm::parseData(sm::formatData("/x/a.wav", one), path, r) && r.slices.size() == 4 &&
               stepOrder(r) == std::vector<int>{0, 1, 2, 3},
           "one region split in four: A B C D again");
  }
  // ---- reorder
  {
    sm::Model m = base;
    expect(sm::moveStep(m, 0, 4) && stepOrder(m) == std::vector<int>{1, 2, 3, 0} && !sm::isNaturalOrder(m) && m.pattern.name == sm::kEditedName,
           "reorder: A to the end (B C D A)");
    expect(sm::moveStep(m, 3, 1) && stepOrder(m) == std::vector<int>{1, 0, 2, 3}, "reorder: A between B and C");
    expect(!sm::moveStep(m, 1, 1) && !sm::moveStep(m, 1, 2) && !sm::moveStep(m, 9, 0) && !sm::moveStep(m, 0, 9), "reorder: no-op and bad moves");
    sm::Model k = m;
    sm::gridNudge(k.grid, 10);
    sm::rechopKeepPattern(k, src.get());
    expect(stepOrder(k) == std::vector<int>{1, 0, 2, 3}, "reorder: a tempo fix keeps the order");
    expect(sm::splitRegion(m, 0, 2) && stepOrder(m) == std::vector<int>{2, 0, 1, 3, 4}, "reorder then split: B A C D -> C A B D E");
    sm::naturalPattern(m);
    expect(sm::isNaturalOrder(m) && std::fabs(m.pattern.beats() - beats0) < 1e-9 && stepUnits(m) == std::vector<int>{1, 1, 2, 2, 2},
           "reset: the natural order again, steps as long as their regions");
  }
  // ---- old projects: a straight pattern (saved before J-149) loads and re-chops as before
  {
    const std::string old =
        "seltmann 1\nsrc 352800 44100\nanalysed 0\nregion 88200 264600\nchop 0 3 0.5 8 60 1 0 0 0\nslices 2\n"
        "s 88200 176400 0 0 0 0 0 0 1 0\ns 176400 264600 0 0 0 0 0 0 1 0\npattern 4 Straight\nslots 0 1 -1\nsteps 1 2\npreset \n";
    sm::Model m;
    expect(sm::modelFromText(old, m) && m.slices.size() == 2 && stepOrder(m) == std::vector<int>{0, 1} && m.pattern.name == "Straight" &&
               sm::isNaturalOrder(m),
           "old state: loads, A B");
    m.grid = sm::flatGrid(*src, 120);
    m.chop.division = sm::DivHalf;
    sm::rechopKeepPattern(m, src.get());
    expect(m.slices.size() == 4 && sm::isNaturalOrder(m) && m.pattern.name == "Straight", "old state: a new chop size starts the natural order");
  }
}

// ------------------------------------------------------------------ instrument
void testInstrument() {
  seltsynth::Spec sp;
  sp.bpm = 120;
  sp.bars = 12;
  sp.seed = 33;
  const auto song = seltsynth::make(sp);
  const std::string path = songFile(song, "inst.wav");
  SamplePtr src = sampleCache().get(path);
  expect(src && src->frames() == song.sample->frames(), "test file decodes");
  if (!src) return;
  // bare path: prepareAux analyses (short file) and builds a default model
  {
    PluginState st = stateWith(path, nullptr);
    ChannelAux aux;
    expect(packs::prepareAux(st, aux) && aux.custom, "bare path: aux with a default model");
    st.params[sm::SmSync] = sm::SyncOriginal;
    auto r = play(st, aux, 44100, 256, {{0, 60, 30000}});
    expect(maxStep(r.L) > 0.001, "slice key sounds");
  }
  sm::Analysis a = sm::requestAnalysis(path)->result;
  sm::Model m = sm::defaultModel(*src, a);
  // beat slices (the checks below play slices of a beat)
  m.chop = sm::Chop{};
  m.chop.division = sm::DivBeat;
  sm::rechop(m, src.get());
  sm::straightPattern(m);
  PluginState st = stateWith(path, &m);
  st.params[sm::SmSync] = sm::SyncOriginal;
  st.params[sm::SmVelocity] = 0;
  ChannelAux aux;
  packs::prepareAux(st, aux);
  // a slice key plays the slice's own audio (interior, away from the declick fades)
  {
    const auto& sl = m.slices[2];
    const int64_t len = sl.end - sl.start;
    auto r = play(st, aux, len + 2000, 128, {{0, 62, len + 1000}});
    double err = 0;
    for (int64_t i = 400; i < len - 400; ++i)
      err = std::max(err, static_cast<double>(std::fabs(r.L[static_cast<size_t>(i)] - 0.8f * src->left[static_cast<size_t>(sl.start + i)])));
    expect(err < 1e-4, "slice key: the slice audio, sample for sample");
    expect(std::fabs(r.L[static_cast<size_t>(len + 500)]) < 1e-6, "slice key: stops at the slice end");
    expect(std::fabs(r.L[0]) <= std::fabs(0.8 * src->left[static_cast<size_t>(sl.start)]) * 0.01 + 1e-6, "slice key: declicked start");
  }
  // slot letters map
  {
    PluginState s2 = st;
    s2.params[sm::SmKeyMap] = sm::KeySlots;
    sm::Model m2 = m;
    m2.pattern.slots[0] = 5;  // A plays slice 6
    s2.data = sm::formatData(path, m2);
    ChannelAux a2;
    packs::prepareAux(s2, a2);
    const auto& sl = m.slices[5];
    auto r = play(s2, a2, 6000, 128, {{0, 60, 5000}});
    double err = 0;
    for (int64_t i = 400; i < 4000; ++i)
      err = std::max(err, static_cast<double>(std::fabs(r.L[static_cast<size_t>(i)] - 0.8f * src->left[static_cast<size_t>(sl.start + i)])));
    expect(err < 1e-4, "slot letters: A plays its assigned slice");
  }
  // the pattern key plays the steps on time (original tempo, free-running)
  sm::Model mp = m;
  {
    sm::Pattern p = mp.pattern;
    sm::patternFromText("4 3 2 1", p);
    mp.pattern.steps = p.steps;
    mp.pattern.unitBeats = 1;
  }
  PluginState sp2 = stateWith(path, &mp);
  sp2.params[sm::SmSync] = sm::SyncOriginal;
  sp2.params[sm::SmVelocity] = 0;
  sp2.params[sm::SmLock] = 0;
  ChannelAux ap;
  packs::prepareAux(sp2, ap);
  const double beat = 60.0 / mp.grid.bpm * 44100;
  {
    auto r = play(sp2, ap, static_cast<int64_t>(beat * 4), 256, {{0, 48, static_cast<int64_t>(beat * 4)}});
    bool onTime = true;
    for (int k = 0; k < 4; ++k) {
      const auto& sl = mp.slices[static_cast<size_t>(3 - k)];
      const int64_t at = static_cast<int64_t>(std::ceil(k * beat));
      double err = 0;
      for (int64_t i = 500; i < 3000; ++i)
        err = std::max(err, static_cast<double>(std::fabs(r.L[static_cast<size_t>(at + i)] - 0.8f * src->left[static_cast<size_t>(sl.start + i)])));
      if (err > 2e-3) onTime = false, std::printf("# step %d err %.5f\n", k, err);
    }
    expect(onTime, "pattern key: each step plays its slice at its time");
  }
  // block-size invariance (pattern, swing, stutter, pitch, filter)
  {
    sm::Model mb = m;
    sm::Pattern p = mb.pattern;
    sm::patternFromText("1 2:x4 3:p+5 4:r . 6:x2:g-6 7:l50 8", p);
    mb.pattern.steps = p.steps;
    mb.pattern.unitBeats = 0.5;
    PluginState sb = stateWith(path, &mb);
    sb.params[sm::SmSync] = sm::SyncResample;
    sb.params[sm::SmSwing] = 0.4f;
    sb.params[sm::SmCutoff] = 5000;
    ChannelAux ab;
    packs::prepareAux(sb, ab);
    const int64_t n = 44100 * 3;
    const std::vector<std::array<int64_t, 3>> notes = {{0, 48, n}, {1000, 61, 20000}};
    auto r1 = play(sb, ab, n, 64, notes, 100, true);
    auto r2 = play(sb, ab, n, 37, notes, 100, true);
    auto r3 = play(sb, ab, n, 1024, notes, 100, true);
    std::printf("# block diffs %.2g %.2g\n", maxDiff(r1.L, r2.L), maxDiff(r1.L, r3.L));
    expect(maxDiff(r1.L, r2.L) < 1e-5 && maxDiff(r1.L, r3.L) < 1e-5 && maxDiff(r1.R, r3.R) < 1e-5, "block-size invariance (64 / 37 / 1024)");
    expect(maxStep(r1.L) > 0.001, "pattern with options sounds");
    // no allocation on the audio thread
    gAllocs = 0;
    play(sb, ab, n, 256, notes, 100, true, true);
    expect(gAllocs.load() == 0, "no allocation in process()");
    std::printf("# allocations during process: %ld\n", gAllocs.load());
  }
  // stretch: the warp worker renders the region at the project tempo
  {
    PluginState ss = st;
    ss.params[sm::SmSync] = sm::SyncStretch;
    ChannelAux as;
    packs::prepareAux(ss, as);
    const int64_t want = static_cast<int64_t>(std::llround((m.slices[0].end - m.slices[0].start) * 1.2));
    bool ready = false;
    int64_t last = 0;
    for (int i = 0; i < 300 && !ready; ++i) {
      // each run asks the warp service for 100 BPM; the slice lasts 1.2x longer once the render is in
      auto t = play(ss, as, 44100, 256, {{0, 60, 44100}}, 100);
      last = 0;
      for (size_t k = 0; k < t.L.size(); ++k)
        if (std::fabs(t.L[k]) > 1e-6f) last = static_cast<int64_t>(k);
      ready = std::llabs(last - want) < 600;
      if (!ready) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::printf("# stretched slice %lld frames (want %lld)\n", static_cast<long long>(last), static_cast<long long>(want));
    expect(ready, "stretch sync: slices follow the project tempo (time-stretched)");
  }
}

// ------------------------------------------------------------------ renders, project items, files
void testRenders() {
  seltsynth::Spec sp;
  sp.bpm = 120;
  sp.bars = 16;
  sp.seed = 44;
  const auto song = seltsynth::make(sp);
  const std::string path = songFile(song, "render.wav");
  SamplePtr src = sampleCache().get(path);
  if (!src) return;
  sm::Analysis a;
  sm::analyse(*src, a);
  const auto& pr = sm::factoryPresets();
  auto withPreset = [&](const char* name) {
    sm::Model m = sm::defaultModel(*src, a);
    for (const auto& x : pr)
      if (x.name == name) sm::applyPreset(m, src.get(), x, 3);
    return m;
  };
  sm::RenderOpts o;
  o.sampleRate = 44100;
  o.tempo = 100;
  o.sync = sm::SyncResample;
  // pattern loop: exact length, continuous seam
  {
    sm::Model m = withPreset("Verse Turn");
    std::vector<float> L, R;
    expect(sm::renderPatternLoop(m, src, o, L, R), "loop render");
    const int64_t want = static_cast<int64_t>(std::llround(16 * 60.0 / 100 * 44100));
    expect(static_cast<int64_t>(L.size()) == want, "loop render: exactly 4 bars at the project tempo");
    const double seam = std::fabs(L.front() - L.back());
    expect(seam <= maxStep(L) + 1e-6, "loop render: the seam is no bigger than any other step");
    std::printf("# loop seam %.5f, max step %.5f\n", seam, maxStep(L));
    o.sync = sm::SyncStretch;
    std::vector<float> L2, R2;
    expect(sm::renderPatternLoop(m, src, o, L2, R2) && static_cast<int64_t>(L2.size()) == want, "loop render (stretch): exact length");
    expect(maxStep(L2) > 0.001, "loop render (stretch): sounds");
    o.sync = sm::SyncResample;
  }
  // phrase span with a crossfaded seam
  {
    sm::Model m = withPreset("Four Bar Loop");
    std::vector<float> L, R;
    const double b0 = m.grid.bar1 + 4;
    expect(sm::renderSpan(m, src, b0, b0 + 8, o, L, R), "span render");
    expect(static_cast<int64_t>(L.size()) == static_cast<int64_t>(std::llround(8 * 60.0 / 100 * 44100)), "span render: 2 bars long");
    expect(std::fabs(L.front() - L.back()) <= maxStep(L) + 1e-6, "span render: continuous seam");
    sm::RenderOpts os = o;
    os.sync = sm::SyncStretch;
    std::vector<float> L2, R2;
    expect(sm::renderSpan(m, src, b0, b0 + 8, os, L2, R2) && L2.size() == L.size() && std::fabs(L2.front() - L2.back()) <= maxStep(L2) + 1e-6,
           "span render (stretch): length and seam");
  }
  // micro chops: short and click-free
  {
    sm::Model m = withPreset("Grain Cloud");
    bool shortSlices = !m.slices.empty();
    for (const auto& sl : m.slices) shortSlices = shortSlices && (sl.end - sl.start) <= static_cast<int64_t>(0.045 * 44100);
    expect(shortSlices, "micro layout: grains of at most 45 ms");
    sm::RenderOpts oo = o;
    oo.sync = sm::SyncOriginal;
    oo.tempo = a.bpm;
    std::vector<float> L, R;
    sm::renderPatternLoop(m, src, oo, L, R);
    const double srcStep = maxStep(src->left);
    std::printf("# micro max step %.4f (source %.4f)\n", maxStep(L), srcStep);
    expect(maxStep(L) <= srcStep * 1.25 + 1e-3, "micro layout: no clicks (steps no larger than the source's)");
    sm::Model m2 = withPreset("Micro Stutter");
    bool tiny = true;
    for (const auto& sl : m2.slices) tiny = tiny && (sl.end - sl.start) <= static_cast<int64_t>(0.035 * 44100);
    expect(tiny && m2.slices.size() >= 32, "1/64 micro chops");
  }
  // slices render
  {
    sm::Model m = sm::defaultModel(*src, a);
    std::vector<float> L, R;
    o.sync = sm::SyncOriginal;
    expect(sm::renderSlice(m, src, 1, o, L, R) && std::llabs(static_cast<int64_t>(L.size()) - (m.slices[1].end - m.slices[1].start)) < 64, "slice render");
  }
  // loop files + project items (what the drag handles carry and what the drop targets make of them)
  {
    sm::Model m = withPreset("Verse Turn");
    std::vector<float> L, R;
    o.sync = sm::SyncResample;
    sm::renderPatternLoop(m, src, o, L, R);
    const std::string wav = (tmpDir() / "Verse Turn 100 bpm.wav").string();
    expect(sm::writeLoopWav(wav, L, R, 44100), "loop file written");
    SamplePtr back = loadSample(wav);
    expect(back && back->frames() == L.size(), "loop file decodes at full length");
    browse::DropItem audio;
    audio.kind = browse::Kind::Audio;
    expect(browse::dropAction(audio, browse::DropTarget::Playlist) == browse::DropAction::AudioClip, "drag a loop onto the playlist: audio clip");
    expect(browse::dropAction(audio, browse::DropTarget::Rack) == browse::DropAction::NewSamplerChannel, "drag a loop onto the channel rack: sampler channel");
    expect(browse::dropAction(audio, browse::DropTarget::Rack, "gl.sampler", true) == browse::DropAction::ReplaceSample,
           "drag a loop onto a channel row: replaces its sample");
    const std::string mid = (tmpDir() / "Verse Turn.mid").string();
    expect(sm::writePatternMidi(mid, sm::patternNotes(m, 60, 0, 1), 100), "pattern MIDI written");
    browse::DropItem midi;
    midi.kind = browse::Kind::Midi;
    expect(browse::dropAction(midi, browse::DropTarget::PianoRoll) == browse::DropAction::ImportMidiNotes, "drag the pattern MIDI onto the piano roll: notes");
    // send to project
    Song s;
    s.tempo = 100;
    Channel c;
    c.id = s.allocateId();
    c.plugin = stateWith(path, &m);
    s.channels.push_back(c);
    const ObjectId pid = sm::sendPatternToSong(s, c.id, m, c.plugin.params, 2, 0, 0);
    const Pattern* p = nullptr;
    for (const auto& x : s.patterns)
      if (x.id == pid) p = &x;
    const bool hasRow = p && p->notes.count(c.id);
    expect(hasRow && p->notes.at(c.id).size() == 8, "send to project: a pattern with the steps as notes (2 cycles)");
    expect(hasRow && p->notes.at(c.id).size() == 8 && p->notes.at(c.id)[0].pitch == 60 && p->notes.at(c.id)[1].pitch == 61 &&
               p->notes.at(c.id)[2].pitch == 60 && p->notes.at(c.id)[3].pitch == 62,
           "send to project: notes on the slice keys (A B A C)");
    expect(hasRow && p->notes.at(c.id).size() == 8 && p->notes.at(c.id)[4].start == 16 * s.ppq, "send to project: the second cycle starts after 4 bars");
    expect(!s.playlist.empty() && s.playlist.back().pattern == pid && s.playlist.back().length == 32 * s.ppq, "send to project: a playlist clip of 8 bars");
    // notes play the pattern losslessly (mod X carries the pitch, mod Y the reverse)
    sm::Model mm = m;
    sm::Pattern q = mm.pattern;
    sm::patternFromText("A:p+7 B:r", q);
    mm.pattern.steps = q.steps;
    const auto notes = sm::patternNotes(mm, 60, 0, 1);
    expect(notes.size() == 2 && notes[0].modX == 128 + 37 && notes[1].modY == 255, "steps as notes: pitch in mod X, reverse in mod Y");
    // save / load
    const std::string proj = (tmpDir() / "proj.gridline.json").string();
    std::string err;
    expect(saveProject(proj, s, &err), "project saves");
    Song t;
    expect(loadProject(proj, t, &err), "project loads");
    const Channel* tc = t.channel(c.id);
    expect(tc && tc->plugin.data == c.plugin.data && tc->plugin.params == c.plugin.params, "save / load: the chop (grid, slices, pattern) comes back");
  }
}

// ------------------------------------------------------------------ long files off-thread, cache
void testLongFile() {
  seltsynth::Spec sp;
  sp.bpm = 126;
  sp.bars = 160;  // ~5 minutes
  sp.rate = 22050;
  sp.sections = "ABACABCA";
  sp.sectionBars = 20;
  sp.seed = 55;
  const auto song = seltsynth::make(sp);
  const std::string path = (tmpDir() / "long.wav").string();
  {
    AudioBuffer b(2, song.sample->frames());
    for (size_t i = 0; i < song.sample->frames(); ++i) b.at(0, i) = song.sample->left[i], b.at(1, i) = song.sample->right[i];
    writeWav(path, b, 22050, 16);
  }
  std::printf("# long file: %.1f s\n", song.sample->seconds());
  const std::string cache = (tmpDir() / "cache").string();
  std::error_code ec;
  fs::remove_all(cache, ec);
  sm::setAnalysisCacheDir(cache);
  sm::forgetAnalysis(path);
  const auto t0 = std::chrono::steady_clock::now();
  sm::JobPtr job = sm::requestAnalysis(path);
  const double callMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  expect(callMs < 250, "long file: the request returns at once");  // (the analysis itself takes seconds; CI VMs can be slow to start a thread)
  std::printf("# request took %.2f ms\n", callMs);
  float lastP = 0;
  bool monotonic = true, sawMiddle = false;
  while (job->state == 0) {
    const float p = job->progress.load();
    if (p + 1e-6f < lastP) monotonic = false;
    if (p > 0.15f && p < 0.95f) sawMiddle = true;
    lastP = p;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::printf("# long file analysed in %.2f s: %.2f BPM, %zu sections\n", secs, job->result.bpm, job->result.sections.size());
  expect(job->state == 1, "long file: analysed");
  expect(job->worker != std::this_thread::get_id(), "long file: on a worker thread");
  expect(monotonic && sawMiddle, "long file: progress reported");
  expect(std::fabs(job->result.bpm - 126) < 0.5, "long file: tempo");
  expect(job->peaks.mx.size() * sm::kPeakBlock >= song.sample->frames(), "long file: overview peaks");
  sm::dropAnalysisJobs();
  const auto t1 = std::chrono::steady_clock::now();
  sm::JobPtr again = sm::requestAnalysis(path);
  while (again->state == 0) std::this_thread::sleep_for(std::chrono::milliseconds(2));
  const double secs2 = std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count();
  expect(again->fromCache && std::fabs(again->result.bpm - job->result.bpm) < 1e-6, "long file: the second request comes from the disk cache");
  std::printf("# cached reload %.3f s\n", secs2);
  sm::setAnalysisCacheDir("");
  fs::remove(path, ec);
}

// ------------------------------------------------------------------ J-107: any material
// Slices shorter than the chop allows (the "tiny slice A" a drum loop with a late first hit,
// leading silence or a pickup made), and how a layout's slots map onto them.
int degenerate(const sm::Model& m) {
  if (m.slices.size() < 2) return 0;
  int n = 0;
  for (const auto& s : m.slices)
    if (static_cast<double>(s.end - s.start) < sm::degenerateLimit(m)) ++n;
  return n;
}

bool stepsValid(const sm::Model& m) {
  const int n = static_cast<int>(m.slices.size());
  for (const auto& st : m.pattern.steps) {
    if (st.src == sm::kRest) continue;
    if (!st.slot && (st.src < 0 || st.src >= n)) return false;
    if (st.slot) {
      if (st.src < 0 || st.src >= static_cast<int>(m.pattern.slots.size())) return false;
      const int k = m.pattern.slots[static_cast<size_t>(st.src)];
      if (k < 0 || k >= n) return false;
    }
  }
  return !m.pattern.steps.empty();
}

const sm::Preset& presetNamed(const char* name) {
  for (const auto& p : sm::factoryPresets())
    if (p.name == name) return p;
  return sm::factoryPresets().front();
}

void testLoops() {
  struct Case {
    const char* name;
    seltsynth::LoopSpec sp;
  };
  std::vector<Case> cases;
  seltsynth::LoopSpec base;
  base.bpm = 92;
  base.bars = 2;
  cases.push_back({"exact 2-bar break", base});
  {
    auto s = base;
    s.late = 0.012;
    cases.push_back({"hits 12 ms behind the grid", s});
  }
  {
    auto s = base;
    s.lead = 0.35;
    s.pickup = true;
    cases.push_back({"pickup and leading silence", s});
  }
  {
    auto s = base;
    s.lead = 0.08;
    cases.push_back({"80 ms of leading silence", s});
  }
  {
    auto s = base;
    s.tail = 1.3;
    cases.push_back({"ring-out tail", s});
  }
  {
    auto s = base;
    s.bpm = 93.7;
    cases.push_back({"odd tempo 93.7", s});
  }
  {
    auto s = base;
    s.bars = 1;
    s.bpm = 104;
    cases.push_back({"one-bar break", s});
  }
  {
    auto s = base;
    s.bars = 4;
    s.bpm = 170;
    s.sixteenths = false;
    s.seed = 9;
    cases.push_back({"4-bar break at 170", s});
  }
  for (const auto& c : cases) {
    const auto song = seltsynth::loop(c.sp);
    sm::Analysis a;
    sm::analyse(*song.sample, a);
    sm::Model m = sm::defaultModel(*song.sample, a);
    const double sr = song.sample->sampleRate;
    const double cell = sm::divisionBeats(m.chop.division, m.grid.beatsPerBar) * sm::beatFrames(m.grid);
    const double first = m.slices.empty() ? 0.0 : static_cast<double>(m.slices[0].end - m.slices[0].start);
    const double bar1 = (c.sp.lead + c.sp.late) * sr;
    std::printf("# %-28s %.2f BPM (fit %d) %s, %zu slices, chop %s%s, region %.3f..%.3f s (%.2f bars), first slice %.2f of a cell\n", c.name, a.bpm,
                a.loopFit ? 1 : 0, sm::kindName(a.kind), m.slices.size(), sm::divisionName(m.chop.division), m.chop.hitSnap ? " on hits" : "",
                m.regionStart / sr, m.regionEnd / sr, sm::regionBars(m), cell > 0 ? first / cell : 0.0);
    const std::string n = c.name;
    expect(std::fabs(a.bpm - c.sp.bpm) / c.sp.bpm < 0.01, (n + ": tempo within 1 %").c_str());
    expect(degenerate(m) == 0, (n + ": no degenerate slice").c_str());
    expect(m.chop.mode == sm::ChopGrid && first >= 0.9 * cell, (n + ": slice A is a whole cell").c_str());
    expect(std::fabs(static_cast<double>(m.regionStart) - bar1) < 0.02 * sr, (n + ": the region starts on bar 1 (after silence / the pickup)").c_str());
    const double bars = sm::regionBars(m);
    expect(bars >= 0.99 && std::fabs(bars - std::round(bars)) < 0.02, (n + ": the region is whole bars").c_str());
    // A B A C maps onto evenly meaningful slices
    sm::Model v = m;
    sm::applyPreset(v, song.sample.get(), presetNamed("Verse Turn"), 1);
    std::vector<int> order;
    for (const auto& st : v.pattern.steps) order.push_back(sm::stepSlice(v.pattern, st, static_cast<int>(v.slices.size())));
    bool even = v.slices.size() >= 3;
    for (size_t k = 1; even && k < 3; ++k)
      even = std::fabs(static_cast<double>(v.slices[k].end - v.slices[k].start) - static_cast<double>(v.slices[0].end - v.slices[0].start)) <
             0.05 * static_cast<double>(v.slices[0].end - v.slices[0].start);
    expect(order == std::vector<int>({0, 1, 0, 2}) && even, (n + ": Verse Turn plays A B A C on equal slices").c_str());
    expect(degenerate(v) == 0 && stepsValid(v), (n + ": Verse Turn: no degenerate slice, no missing slice").c_str());
  }
  // the user's case before J-107: a loop whose grid sat a few ms after frame 0 made A a sliver.
  // Now the region always starts on a grid line and a fragment under half a cell is never a slice.
  {
    const auto song = seltsynth::loop(base);
    sm::Model g;
    g.srcFrames = static_cast<int64_t>(song.sample->frames());
    g.srcRate = 44100;
    g.grid = sm::flatGrid(*song.sample, 92);
    for (auto& b : g.grid.beats) b += 300;  // the grid 7 ms in
    g.chop.mode = sm::ChopGrid;
    g.chop.division = sm::Div8th;
    g.chop.zeroCross = false;
    g.regionStart = 0;
    g.regionEnd = g.srcFrames;
    sm::rechop(g, song.sample.get());
    const double cell = 0.5 * sm::beatFrames(g.grid);
    expect(g.regionStart == 300 && static_cast<double>(g.slices[0].end - g.slices[0].start) > 0.99 * cell,
           "grid 7 ms after the file start: the region moves onto the grid, no sliver slice");
    expect(degenerate(g) == 0, "minimum slice length: no slice under half a cell");
    // transients 10 ms apart merge
    auto s = std::make_shared<gl::Sample>();
    s->sampleRate = 44100;
    s->left.assign(44100 * 2, 0.f);
    for (double t : {0.3, 0.31, 0.9, 1.5})
      for (int i = 0; i < 2000; ++i) s->left[static_cast<size_t>(t * 44100) + static_cast<size_t>(i)] += 0.8f * std::exp(-i / 300.0f) * ((i / 20) % 2 ? 1.f : -1.f);
    sm::Model h;
    h.srcFrames = static_cast<int64_t>(s->frames());
    h.srcRate = 44100;
    h.grid = sm::flatGrid(*s, 120);
    h.chop.mode = sm::ChopTransients;
    h.chop.sensitivity = 1.0f;
    h.regionStart = 0;
    h.regionEnd = h.srcFrames;
    sm::rechop(h, s.get());
    bool longEnough = true;
    for (size_t k = 0; k + 1 < h.slices.size(); ++k) longEnough = longEnough && h.slices[k].end - h.slices[k].start >= static_cast<int64_t>(0.04 * 44100);
    expect(longEnough && degenerate(h) == 0, "minimum slice length: hits 10 ms apart make one slice");
  }
}

void testNameHints() {
  struct N {
    const char* path;
    double bpm;
    bool strong;
    int key;
    bool minor;
  };
  const N names[] = {
      {"/s/KMRBI_GHHS_80_drum_loop_full.wav", 80, false, -1, false},
      {"/s/Funky Break 92 BPM.wav", 92, true, -1, false},
      {"/s/Funky_Break_92bpm.wav", 92, true, -1, false},
      {"/s/Artist-Song-Master--110.00bpm-Am-[Store].wav", 110, true, 9, true},
      {"/s/Artist-Song-Master--144.00bpm-C-[Store].wav", 144, true, 0, false},
      {"/s/Drumloop 62.wav", 0, false, -1, false},
      {"/s/Track 41.wav", 0, false, -1, false},
      {"/s/Pack - Drop Loop 3 - 150 BPM F Min.wav", 150, true, 5, true},
      {"/s/Pack - Drop Loop 4 - 150 BPM D# Maj.wav", 150, true, 3, false},
      {"/s/MO_MK_140_vocal_loop_humanoid_D#min.wav", 140, false, 3, true},
      {"/s/Drum Loops/Cool 165.5/Loop 1.wav", 165.5, false, -1, false},
      {"/s/Jazz Loop 149.5.wav", 149.5, false, -1, false},
  };
  bool all = true;
  for (const auto& n : names) {
    const sm::AnalyseHints h = sm::analyseHintsFor(n.path);
    const bool ok = std::fabs(h.bpm - n.bpm) < 1e-6 && (n.bpm <= 0 || h.strong == n.strong) && h.key == n.key && (n.key < 0 || h.minor == n.minor);
    if (!ok) std::printf("# name %s -> %.2f %s key %d %s\n", n.path, h.bpm, h.strong ? "strong" : "weak", h.key, h.minor ? "minor" : "major");
    all = all && ok;
  }
  expect(all, "file-name tempo and key: bpm words, bare sample-pack numbers, store names, folders; indexes are not tempos");
  // the prior: a 2-bar loop at 100 BPM read as 200 when the name says so (and as 100 without a name)
  seltsynth::LoopSpec sp;
  sp.bpm = 100;
  sp.bars = 2;
  const auto song = seltsynth::loop(sp);
  sm::Analysis a0, a1;
  sm::analyse(*song.sample, a0);
  sm::AnalyseHints h;
  h.bpm = 200;
  h.strong = true;
  sm::analyse(*song.sample, a1, nullptr, nullptr, h);
  std::printf("# prior: no name %.2f, name 200 -> %.2f (fit %d, %zu beats)\n", a0.bpm, a1.bpm, a1.loopFit ? 1 : 0, a1.beats.size());
  expect(std::fabs(a0.bpm - 100) < 0.5 && std::fabs(a1.bpm - 200) < 0.5 && a1.loopFit && a1.beats.size() == 16,
         "the file name's tempo picks the octave (and the loop is fitted at it)");
  sm::AnalyseHints far;
  far.bpm = 123;  // a bare number that the audio does not support is ignored
  sm::Analysis a2;
  sm::analyse(*song.sample, a2, nullptr, nullptr, far);
  expect(std::fabs(a2.bpm - 100) < 0.5, "a weak name tempo the audio does not support is ignored");
}

void testAutoChop() {
  struct K {
    const char* name;
    std::shared_ptr<gl::Sample> s;
    int kind, mode, division;
  };
  seltsynth::LoopSpec one;
  one.bars = 1;
  one.bpm = 98;
  seltsynth::LoopSpec two;
  two.bars = 2;
  seltsynth::LoopSpec eight;
  eight.bars = 8;
  eight.bpm = 128;
  seltsynth::LoopSpec tonal;
  tonal.bars = 2;
  tonal.bpm = 96;
  tonal.drums = false;
  tonal.chords = true;
  seltsynth::Spec songSpec;
  songSpec.bpm = 120;
  songSpec.bars = 32;
  songSpec.sections = "ABAB";
  const std::vector<K> ks = {
      {"1-bar break", seltsynth::loop(one).sample, sm::KindBreak, sm::ChopGrid, sm::Div16th},
      {"2-bar break", seltsynth::loop(two).sample, sm::KindBreak, sm::ChopGrid, sm::Div8th},
      {"8-bar break", seltsynth::loop(eight).sample, sm::KindBreak, sm::ChopGrid, sm::DivBeat},
      {"2-bar tonal loop", seltsynth::loop(tonal).sample, sm::KindLoop, sm::ChopGrid, sm::DivHalf},
      {"song", seltsynth::make(songSpec).sample, sm::KindSong, sm::ChopGrid, sm::Div1Bar},
      {"one-shot", seltsynth::oneShot(0.6), sm::KindOneShot, sm::ChopTransients, -1},
      {"ambient pad", seltsynth::ambient(12), sm::KindAmbient, sm::ChopEqual, -1},
  };
  for (const auto& k : ks) {
    sm::Analysis a;
    sm::analyse(*k.s, a);
    sm::Model m = sm::defaultModel(*k.s, a);
    std::printf("# auto chop %-16s kind %-8s (%.2f BPM, fit %d, onsets %.1f/s, tonality %.2f, conf %.2f) -> mode %d %s%s, %zu slices\n", k.name,
                sm::kindName(a.kind), a.bpm, a.loopFit ? 1 : 0, a.onsetRate, a.tonality, a.confidence, m.chop.mode, m.chop.mode == sm::ChopGrid ? sm::divisionName(m.chop.division) : "",
                m.chop.hitSnap ? " on hits" : "", m.slices.size());
    const std::string n = k.name;
    expect(a.kind == k.kind, (n + ": material kind").c_str());
    expect(m.chop.autoSize && m.chop.mode == k.mode && (k.division < 0 || m.chop.division == k.division), (n + ": automatic chop size").c_str());
    expect(degenerate(m) == 0 && !m.slices.empty() && stepsValid(m), (n + ": playable, no degenerate slice").c_str());
    if (k.kind == sm::KindBreak) expect(m.chop.hitSnap, (n + ": cuts on the hits").c_str());
    // every layout works on it: no degenerate slices, no steps without a slice
    int bad = 0;
    for (const auto& p : sm::factoryPresets()) {
      sm::Model v = m;
      sm::applyPreset(v, k.s.get(), p, 5);
      if (degenerate(v) != 0 || !stepsValid(v) || v.slices.empty()) {
        if (bad < 3) std::printf("#   layout %s: %zu slices, %d degenerate, steps %s\n", p.name.c_str(), v.slices.size(), degenerate(v), stepsValid(v) ? "ok" : "missing");
        ++bad;
      }
    }
    expect(bad == 0, (n + ": all " + std::to_string(sm::factoryPresets().size()) + " layouts work (no degenerate or missing slices)").c_str());
  }
  // layouts keep their slice count on short material: a 4-bar phrase layout on a 2-bar loop chops in half bars
  {
    const auto s = seltsynth::loop(tonal).sample;
    sm::Analysis a;
    sm::analyse(*s, a);
    sm::Model m = sm::defaultModel(*s, a);
    sm::applyPreset(m, s.get(), presetNamed("Straight Run"), 1);
    expect(m.slices.size() == 4 && m.chop.division == sm::DivHalf && std::fabs(m.pattern.unitBeats - 2) < 1e-9,
           "a 4-bar layout on 2 bars: four half-bar slices, half-bar steps");
    sm::Model b = sm::defaultModel(*s, a);
    sm::applyPreset(b, s.get(), presetNamed("Original Order"), 1);
    expect(b.slices.size() == 16 && b.chop.division == sm::Div8th, "a 2-bar break layout on 2 bars: sixteen eighths");
  }
  // the new instant flips are there and every card has words
  int flips = 0;
  bool hints = true;
  for (const auto& p : sm::factoryPresets()) {
    flips += p.category == sm::CatFlip;
    hints = hints && !p.hint.empty() && !sm::presetSummary(p).empty();
  }
  expect(flips >= 6 && sm::factoryPresets().size() >= 59 && hints, "53 layouts + 6 instant flips, every card says what it does");
  expect(sm::presetSummary(presetNamed("Verse Turn")) == "A B A C", "card summary: A B A C");
}

void testDropRules() {
  expect(xdrop::sampleSlot(sm::kType) == xdrop::SampleSlot::Whole, "Seltmann holds one sample (its data starts with the path)");
  expect(xdrop::rowTakesSample(sm::kType), "audio dropped on a Seltmann channel row loads into Seltmann (not replaced by a sampler)");
  sm::Model m;
  expect(xdrop::withSample(sm::kType, sm::formatData("/old.wav", m), "/new.wav") == "/new.wav", "a dropped file replaces the model (analysed afresh)");
}

void testCatalog() {
  const PluginInfo* info = findPlugin(sm::kType);
  expect(info && info->name == "Seltmann" && info->kind == PluginKind::Generator && info->usesSample, "catalogue: Seltmann instrument");
  expect(info && info->presets.size() >= 2 && info->presets.size() <= 6, "catalogue: 2-6 factory sounds");
  expect(isFlagship(sm::kType) && pluginBrand(sm::kType)->family == BrandFamily::Sample, "brand: Sample family flagship");
  const std::string kw = pluginKeywords(sm::kType);
  bool all = true;
  for (const char* k : {"chop", "chopper", "sample chop", "flip", "loop maker", "microsampling", "beat slicer", "bpm"}) all = all && kw.find(k) != std::string::npos;
  expect(all, "brand: search keywords");
  // short / old param vectors normalise
  PluginState st = defaultPluginState(sm::kType);
  st.params.resize(3);
  normalizePluginState(st);
  expect(st.params.size() == sm::SmCount && st.params[sm::SmSync] == sm::SyncStretch, "old states normalise to the defaults");
  if (!info) return;
  // extremes stay finite
  seltsynth::Spec sp;
  sp.bars = 6;
  const auto song = seltsynth::make(sp);
  const std::string path = songFile(song, "extreme.wav");
  PluginState ex = stateWith(path, nullptr);
  bool finite = true;
  for (int k = 0; k < sm::SmCount; ++k)
    for (bool hi : {false, true}) {
      PluginState s2 = ex;
      s2.params[static_cast<size_t>(k)] = static_cast<float>(hi ? info->params[static_cast<size_t>(k)].max : info->params[static_cast<size_t>(k)].min);
      ChannelAux aux;
      packs::prepareAux(s2, aux);
      auto r = play(s2, aux, 20000, 256, {{0, 48, 15000}, {100, 60, 15000}, {200, 62, 5000}});
      for (float v : r.L) finite = finite && std::isfinite(v) && std::fabs(v) < 8;
    }
  expect(finite, "parameter extremes: finite output");
}

// ------------------------------------------------------------------ validation mode (not part of ctest)
int validate(const std::string& dir) {
  std::FILE* f = std::fopen((fs::path(dir) / "index.txt").string().c_str(), "r");
  if (!f) return 1;
  char line[1024];
  int n = 0, exact = 0, octave = 0;
  double fsum = 0, dsum = 0;
  while (std::fgets(line, sizeof line, f)) {
    std::string l(line);
    while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
    const size_t bar = l.find('|');
    if (bar == std::string::npos) continue;
    const std::string id = l.substr(0, bar), name = l.substr(bar + 1);
    // tempo from the converted project
    std::FILE* j = std::fopen((fs::path(dir) / (id + ".json")).string().c_str(), "r");
    if (!j) continue;
    std::string js;
    char buf[65536];
    size_t got;
    while ((got = std::fread(buf, 1, sizeof buf, j)) > 0) js.append(buf, got);
    std::fclose(j);
    const size_t tp = js.find("\"tempo\"");
    if (tp == std::string::npos) continue;
    const double tempo = std::atof(js.c_str() + js.find(':', tp) + 1);
    SamplePtr s = loadSample((fs::path(dir) / (id + ".wav")).string());
    if (!s || tempo <= 0) continue;
    sm::Analysis a;
    const auto t0 = std::chrono::steady_clock::now();
    sm::analyse(*s, a);
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    // reference: the project grid from bar 1 over the audible span
    double firstT = 0, lastT = s->seconds();
    for (size_t i = 0; i < s->frames(); ++i)
      if (std::fabs(s->left[i]) > 0.01f) {
        firstT = i / s->sampleRate;
        break;
      }
    for (size_t i = s->frames(); i-- > 0;)
      if (std::fabs(s->left[i]) > 0.01f) {
        lastT = i / s->sampleRate;
        break;
      }
    std::vector<double> ref, refDown, est, estDown;
    for (int k = 0; k * 60.0 / tempo <= lastT; ++k) {
      const double t = k * 60.0 / tempo;
      if (t + 0.1 < firstT) continue;
      ref.push_back(t);
      if (k % 4 == 0) refDown.push_back(t);
    }
    for (size_t k = 0; k < a.beats.size(); ++k) {
      const double t = a.beats[k] / s->sampleRate;
      if (t + 0.1 < firstT || t > lastT + 0.1) continue;
      est.push_back(t);
      if ((static_cast<int>(k) - a.bar1) % std::max(1, a.beatsPerBar) == 0) estDown.push_back(t);
    }
    const bool ok = std::fabs(a.bpm - tempo) / tempo < 0.02;
    bool oct = ok;
    for (double m : {0.5, 2.0}) oct = oct || std::fabs(a.bpm - tempo * m) / (tempo * m) < 0.02;
    const double F = seltsynth::fmeasure(est, ref), D = seltsynth::fmeasure(estDown, refDown);
    // bar phase error (beats, mod 4) of the estimated bar 1
    int phaseErr = -1;
    if (!estDown.empty()) phaseErr = ((static_cast<int>(std::lround(estDown.front() * tempo / 60.0)) % 4) + 4) % 4;
    std::printf("%-58s tempo %7.2f est %7.2f conf %.2f %s  beatF %.3f downF %.3f (bar 1 off by %d)  %.1fs (%.1fs audio)\n", name.substr(0, 58).c_str(), tempo, a.bpm,
                a.confidence, ok ? "exact " : oct ? "octave" : "WRONG ", F, D, phaseErr, secs, s->seconds());
    ++n;
    exact += ok;
    octave += oct;
    fsum += ok ? F : 0;
    dsum += ok ? D : 0;
  }
  std::fclose(f);
  std::printf("FL demo songs: %d, exact tempo %d, tempo or octave %d, mean beat F (exact ones) %.3f, mean downbeat F %.3f\n", n, exact, octave,
              exact ? fsum / exact : 0.0, exact ? dsum / exact : 0.0);
  return 0;
}

}  // namespace

void* operator new(std::size_t n) {
  if (gWatch) ++gAllocs;
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) {
  if (gWatch) ++gAllocs;
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

int main(int argc, char** argv) {
  const std::string only = argc > 1 ? argv[1] : "";
  if (only == "validate" && argc > 2) return validate(argv[2]);
  if (only == "writeloop" && argc > 2) {  // a synthetic 2-bar break for the UI flow check (seltmann_flow.cmake)
    seltsynth::LoopSpec sp;
    sp.bpm = 96;
    sp.bars = 2;
    sp.late = 0.01;
    const auto song = seltsynth::loop(sp);
    return sm::writeLoopWav(argv[2], song.sample->left, song.sample->right.vec(), song.sample->sampleRate) ? 0 : 1;
  }
  if (only == "writetake" && argc > 2) {  // J-110: the same break as a recorded take (32-bit float, Loupe's WAV writer)
    seltsynth::LoopSpec sp;
    sp.bpm = 96;
    sp.bars = 2;
    sp.late = 0.01;
    const auto song = seltsynth::loop(sp);
    packs::edison::Audio au;
    au.rate = song.sample->sampleRate;
    au.channels = 2;
    au.L = song.sample->left;
    au.R = song.sample->right.vec();
    return packs::edison::writeWavFile(argv[2], packs::edison::makeContent(au), 32) ? 0 : 1;
  }
  if (only.empty() || only == "tempo") testTempo();
  if (only.empty() || only == "sections") testSections();
  if (only.empty() || only == "model") testModel();
  if (only.empty() || only == "patterns") testPatterns();
  if (only.empty() || only == "regions") testRegions();
  if (only.empty() || only == "instrument") testInstrument();
  if (only.empty() || only == "renders") testRenders();
  if (only.empty() || only == "long") testLongFile();
  if (only.empty() || only == "loops") testLoops();
  if (only.empty() || only == "names") testNameHints();
  if (only.empty() || only == "auto") testAutoChop();
  if (only.empty() || only == "drop") testDropRules();
  if (only.empty() || only == "catalog") testCatalog();
  sm::waitAnalysisIdle();
  return finish();
}
