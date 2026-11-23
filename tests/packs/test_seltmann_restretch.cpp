// Gridline — "seltmann" pack tests (0.7.8): offline renders (exports) under tempo automation.
//   equal   a project with Seltmann channels rendered with a constant tempo, a tempo ramp, a new
//           tempo every bar and several channels sounds exactly as the reference way of making
//           the warped renders does (one whole render of the region for every tempo it meets),
//           sample for sample, with no more stretch work
//   speed   the same renders with the renders made in pieces: the stretch work and the time under
//           tempo changes stay within generous bounds of the constant tempo's (a whole render per
//           tempo, as before, took about 50 times the constant tempo's work per bar with the ramp)
//   ramp    a sine chopped in sixteenths, held, at a constant tempo and under a 100 -> 140 bpm ramp:
//           every slice of the export plays at the source pitch (within 5 cents: a ramp plays a
//           render made within 3 cents of its tempo; before, a slice starting on a new tempo played
//           the plain resampled source, up to 574 cents sharp) and at the level the instrument plays it in real time; at the constant tempo the
//           export is the reference render sample for sample and real time plays the same pitch.
//           Real time (the instrument run as the audio thread runs it, the worker making its renders
//           as the tempo moves) plays every slice of the ramp within 10 cents once the first render
//           for the tempo exists (before, the worker waited for the tempo to hold still: 63 of 64
//           slices played the plain resampled source, up to 574 cents sharp); a tempo that steps
//           every bar plays every slice in tune but the first one or two after each step (the
//           render of a new tempo takes a moment), no more of them than before.
//           RAMP_DEBUG=1 prints every slice of both.
// RESTRETCH_BARS=<n> renders n bars (equal, ramp: 4, speed: 16); RESTRETCH_DUMP=<dir> writes each
// render there as raw floats (to compare builds).
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include "../Check.h"
#include "core/Automation.h"
#include "core/Params.h"
#include "core/PluginInfo.h"
#include "core/Song.h"
#include "engine/Generators.h"
#include "engine/Render.h"
#include "engine/TimeStretch.h"
#include "packs/Packs.h"
#include "packs/seltmann/seltmann.h"

using namespace gl;
namespace sm = gl::packs::seltmann;
namespace fs = std::filesystem;

namespace {

constexpr double kRate = 44100, kSrcBpm = 100;

fs::path tmpDir() {
  const fs::path d = fs::temp_directory_path() / "gl_seltmann_restretch_test";
  std::error_code ec;
  fs::create_directories(d, ec);
  return d;
}

// Eight bars at 100 bpm of drums and chords: a kick on every beat, a noise hat on the eighths,
// three sines that change every bar (transients and tones, the material a stretch works hardest on).
SamplePtr groove() {
  auto s = std::make_shared<Sample>();
  s->sampleRate = kRate;
  const double beat = 60.0 / kSrcBpm * kRate;
  const auto n = static_cast<size_t>(beat * 4 * 8);
  std::vector<float> L(n), R(n);
  uint32_t seed = 12345;
  const double chords[4][3] = {{220, 277.2, 329.6}, {196, 246.9, 293.7}, {174.6, 220, 261.6}, {164.8, 207.7, 246.9}};
  for (size_t i = 0; i < n; ++i) {
    const double t = static_cast<double>(i) / kRate;
    const double inBeat = std::fmod(static_cast<double>(i), beat) / kRate;
    const double inEighth = std::fmod(static_cast<double>(i), beat / 2) / kRate;
    const auto bar = static_cast<size_t>(static_cast<double>(i) / (beat * 4)) % 4;
    const double kick = 0.6 * std::exp(-inBeat * 18) * std::sin(2 * 3.14159265358979 * (50 * inBeat + 60 * (1 - std::exp(-inBeat * 30)) / 30 * 10));
    seed = seed * 1664525u + 1013904223u;
    const double noise = (static_cast<double>(seed >> 8) / 8388608.0 - 1.0);
    const double hat = 0.15 * std::exp(-inEighth * 60) * noise;
    double pad = 0;
    for (double f : chords[bar]) pad += 0.08 * std::sin(2 * 3.14159265358979 * f * t);
    L[i] = static_cast<float>(kick + hat + pad);
    R[i] = static_cast<float>(kick - hat + pad * 0.9);
  }
  s->left = L;
  s->right = R;
  return s;
}

// A two-bar region from `firstBar`, chopped in sixteenths, played in order.
sm::Model modelAt(const Sample& src, int firstBar) {
  sm::Model m;
  m.srcFrames = static_cast<int64_t>(src.frames());
  m.srcRate = kRate;
  m.grid = sm::flatGrid(src, kSrcBpm);
  m.chop.mode = sm::ChopGrid;
  m.chop.division = sm::Div16th;
  m.chop.zeroCross = false;
  sm::setRegionBars(m, firstBar, 2);
  sm::rechop(m, &src);
  sm::straightPattern(m);
  return m;
}

enum class Tempo { Constant, Ramp, Stepped };
const char* tempoName(Tempo t) { return t == Tempo::Constant ? "constant" : t == Tempo::Ramp ? "ramp" : "stepped"; }

// A project of `bars` bars at 120 bpm (or the tempo automated) with `channels` Seltmann channels
// on the file: the first plays its sequence sent to the playlist as notes, the others the latched
// sequence (the pattern key held for the whole song), each on its own region of the file.
Song project(const std::string& path, const Sample& src, int bars, Tempo tempo, int channels) {
  Song s = makeEmptySong();
  s.tempo = 120;
  const Tick bar = static_cast<Tick>(s.ppq) * 4;
  for (int k = 0; k < channels; ++k) {
    const sm::Model m = modelAt(src, (k * 2) % 6);
    Channel c;
    c.id = s.allocateId();
    c.name = "Seltmann " + std::to_string(k + 1);
    c.plugin = defaultPluginState(sm::kType);
    c.plugin.data = sm::formatData(path, m);
    c.plugin.params[sm::SmVelocity] = 0;
    c.plugin.params[sm::SmVolume] = 0.5f;
    c.mixerTrack = s.masterId();
    s.channels.push_back(c);
    if (k == 0) {
      sm::sendPatternToSong(s, c.id, m, c.plugin.params, std::max(1, bars / 2), 0, 0);
    } else {
      gl::Pattern p;
      p.id = s.allocateId();
      p.name = "Held " + std::to_string(k);
      Note n;
      n.start = 0;
      n.length = bar * bars;
      n.pitch = static_cast<int>(std::lround(c.plugin.params[sm::SmBaseNote])) - 12;
      p.notes[c.id].push_back(n);
      s.patterns.push_back(p);
      Clip cl;
      cl.id = s.allocateId();
      cl.pattern = p.id;
      cl.track = k;
      cl.start = 0;
      cl.length = bar * bars;
      s.playlist.push_back(cl);
    }
  }
  if (tempo != Tempo::Constant) {
    const auto desc = describeParam(s, songParam(SongParam::Tempo));
    AutomationClip a;
    a.id = s.allocateId();
    a.owner = kSongId;
    a.index = static_cast<uint32_t>(SongParam::Tempo);
    auto norm = [&](double bpm) { return static_cast<float>(desc->toNormalized(bpm)); };
    if (tempo == Tempo::Ramp) {
      a.points = {{0, norm(100), 0, AutoShape::Curve}, {bar * bars, norm(140), 0, AutoShape::Curve}};
    } else {
      for (int b = 0; b <= bars; ++b) a.points.push_back({bar * b, norm(100 + (b * 7) % 41), 0, AutoShape::Hold});
    }
    s.automations.push_back(a);
    Clip cl;
    cl.id = s.allocateId();
    cl.kind = ClipKind::Automation;
    cl.source = a.id;
    cl.track = channels;
    cl.start = 0;
    cl.length = bar * bars;
    s.playlist.push_back(cl);
  }
  return s;
}

struct Out {
  AudioBuffer audio;
  double seconds = 0;
  int64_t steps = 0;  // the stretch's work (spectral steps)
};

Out renderOf(const Song& s) {
  Out o;
  RenderOptions ro;
  ro.sampleRate = static_cast<int>(kRate);
  const int64_t s0 = stretch::spectralStepsRun();
  const auto t0 = std::chrono::steady_clock::now();
  o.audio = renderSong(s, ro);
  o.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  o.steps = stretch::spectralStepsRun() - s0;
  return o;
}

void dump(const std::string& name, const AudioBuffer& b) {
  const char* d = std::getenv("RESTRETCH_DUMP");
  if (!d || !*d) return;
  std::ofstream f(fs::path(d) / (name + ".f32"), std::ios::binary);
  for (size_t i = 0; i < b.frames; ++i)
    for (int c = 0; c < 2; ++c) {
      const float v = b.at(c, i);
      f.write(reinterpret_cast<const char*>(&v), sizeof v);
    }
}

double peak(const AudioBuffer& b) {
  double p = 0;
  for (size_t i = 0; i < b.frames; ++i)
    for (int c = 0; c < 2; ++c) p = std::max(p, static_cast<double>(std::fabs(b.at(c, i))));
  return p;
}

// The difference of two renders: the largest sample apart and its level (dBFS, -inf when equal).
struct Diff {
  double maxAbs = 0;
  double db = -std::numeric_limits<double>::infinity();
  bool sameLength = true;
};
Diff diffOf(const AudioBuffer& a, const AudioBuffer& b) {
  Diff d;
  d.sameLength = a.frames == b.frames;
  for (size_t i = 0; i < std::min(a.frames, b.frames); ++i)
    for (int c = 0; c < 2; ++c) d.maxAbs = std::max(d.maxAbs, static_cast<double>(std::fabs(a.at(c, i) - b.at(c, i))));
  if (d.maxAbs > 0) d.db = 20 * std::log10(d.maxAbs);
  return d;
}

SamplePtr loadGroove() {
  const SamplePtr made = groove();
  const std::string path = (tmpDir() / "groove.wav").string();
  expect(sm::writeLoopWav(path, made->left, made->right.vec(), kRate), "restretch: write the groove");
  const SamplePtr src = sampleCache().get(path);
  expect(src && src->frames() > 0, "restretch: the groove decodes");
  return src;
}
std::string groovePath() { return (tmpDir() / "groove.wav").string(); }

struct Case {
  Tempo tempo;
  int channels;
};
constexpr Case kCases[] = {{Tempo::Constant, 1}, {Tempo::Ramp, 1}, {Tempo::Stepped, 1}, {Tempo::Constant, 4}, {Tempo::Stepped, 4}};
std::string nameOf(const Case& c, int bars) {
  return std::string(tempoName(c.tempo)) + "_" + std::to_string(c.channels) + "ch_" + std::to_string(bars) + "bars";
}
int barsFromEnv(int def) {
  const char* e = std::getenv("RESTRETCH_BARS");
  return std::clamp(e && *e ? std::atoi(e) : def, 2, 256);
}

// Every case rendered both ways: the renders made in pieces equal the whole renders sample for sample.
void testEqual() {
  const SamplePtr src = loadGroove();
  if (!src) return;
  const int bars = barsFromEnv(4);
  for (const auto& c : kCases) {
    const Song s = project(groovePath(), *src, bars, c.tempo, c.channels);
    sm::setWholeOfflineRenders(true);
    const Out ref = renderOf(s);
    sm::setWholeOfflineRenders(false);
    const Out got = renderOf(s);
    const std::string name = nameOf(c, bars);
    const Diff d = diffOf(ref.audio, got.audio);
    std::printf("# equal %-24s whole %7.3f s (%7lld steps), pieces %7.3f s (%7lld steps), largest difference %g (%.1f dBFS)\n", name.c_str(),
                ref.seconds, static_cast<long long>(ref.steps), got.seconds, static_cast<long long>(got.steps), d.maxAbs, d.db);
    dump(name + "_whole", ref.audio);
    dump(name + "_pieces", got.audio);
    expect(ref.audio.frames > 0 && peak(ref.audio) > 0.05, ("equal: " + name + ": the render has sound").c_str());
    expect(d.sameLength && d.maxAbs == 0, ("equal: " + name + ": made in pieces, the render is the same sample for sample").c_str());
    expect(got.steps <= ref.steps, ("equal: " + name + ": ... with no more stretch work").c_str());
  }
}

// The renders under tempo changes: their work and time against the constant tempo's.
void testSpeed() {
  const SamplePtr src = loadGroove();
  if (!src) return;
  const int bars = barsFromEnv(16);
  double constant[5] = {0, 0, 0, 0, 0};
  int64_t constantSteps[5] = {0, 0, 0, 0, 0};
  for (const auto& c : kCases) {
    const Song s = project(groovePath(), *src, bars, c.tempo, c.channels);
    const Out o = renderOf(s);
    const std::string name = nameOf(c, bars);
    std::printf("# speed %-24s %7.3f s, %8lld stretch steps, %zu frames, peak %.3f\n", name.c_str(), o.seconds, static_cast<long long>(o.steps),
                o.audio.frames, peak(o.audio));
    dump(name, o.audio);
    expect(o.audio.frames > 0 && peak(o.audio) > 0.05, ("speed: " + name + ": the render has sound").c_str());
    if (c.tempo == Tempo::Constant) {
      constant[c.channels] = o.seconds;
      constantSteps[c.channels] = o.steps;
      continue;
    }
    const double ratio = o.seconds / std::max(1e-3, constant[c.channels]);
    std::printf("# speed %-24s %.2fx the constant tempo's time, %.2fx its stretch work\n", name.c_str(), ratio,
                static_cast<double>(o.steps) / static_cast<double>(std::max<int64_t>(1, constantSteps[c.channels])));
    // The least an exact render can do: a stretch from the region's start to the slices each new
    // tempo plays (a new tempo every bar: up to a region per bar; the ramp: up to one per step
    // that lands on a settled tempo).
    const double work = static_cast<double>(o.steps) / static_cast<double>(std::max<int64_t>(1, constantSteps[c.channels]));
    const double maxWork = c.tempo == Tempo::Ramp ? 8.0 * bars : 1.0 * bars;
    const double maxTime = c.tempo == Tempo::Ramp ? 3.0 * bars : 0.5 * bars + 2;
    expect(work <= maxWork, ("speed: " + name + ": stretch work within its bound").c_str());
    expect(ratio <= maxTime, ("speed: " + name + ": render time within its bound (generous)").c_str());
  }
}

// ---------------------------------------------------------------- ramp: every slice at its pitch
// A 220 Hz sine, eight bars at 100 bpm, chopped in sixteenths (a two-bar region played in order with
// the pattern key held), under a tempo ramp from 100 to 140 bpm. Each slice is measured where it
// sounds (its pitch, by zero-crossing periods, and its level) in the export and in the instrument
// run as the audio thread runs it (the warp renders made by the worker in real time).
constexpr double kF0 = 220;

SamplePtr loadSine() {
  std::vector<float> L(static_cast<size_t>(60.0 / kSrcBpm * kRate * 4 * 8));
  for (size_t i = 0; i < L.size(); ++i) L[i] = static_cast<float>(0.5 * std::sin(2 * 3.14159265358979 * kF0 * static_cast<double>(i) / kRate));
  const std::string path = (tmpDir() / "sine.wav").string();
  expect(sm::writeLoopWav(path, L, L, kRate), "ramp: write the sine");
  const SamplePtr src = sampleCache().get(path);
  expect(src && src->frames() > 0, "ramp: the sine decodes");
  return src;
}

// One Seltmann channel holding its pattern key for `bars` bars, the tempo ramped from `from` to `to`
// (equal: constant).
Song heldProject(const Sample& src, int bars, double from, double to, bool stepped = false) {
  Song s = makeEmptySong();
  s.tempo = from;
  const Tick bar = static_cast<Tick>(s.ppq) * 4;
  const sm::Model m = modelAt(src, 0);
  Channel c;
  c.id = s.allocateId();
  c.name = "Seltmann";
  c.plugin = defaultPluginState(sm::kType);
  c.plugin.data = sm::formatData((tmpDir() / "sine.wav").string(), m);
  c.plugin.params[sm::SmVelocity] = 0;
  c.plugin.params[sm::SmVolume] = 0.5f;
  c.mixerTrack = s.masterId();
  s.channels.push_back(c);
  gl::Pattern p;
  p.id = s.allocateId();
  p.name = "Held";
  Note n;
  n.start = 0;
  n.length = bar * bars;
  n.pitch = static_cast<int>(std::lround(c.plugin.params[sm::SmBaseNote])) - 12;
  p.notes[c.id].push_back(n);
  s.patterns.push_back(p);
  Clip cl;
  cl.id = s.allocateId();
  cl.pattern = p.id;
  cl.track = 0;
  cl.start = 0;
  cl.length = bar * bars;
  s.playlist.push_back(cl);
  if (from != to) {
    const auto desc = describeParam(s, songParam(SongParam::Tempo));
    AutomationClip a;
    a.id = s.allocateId();
    a.owner = kSongId;
    a.index = static_cast<uint32_t>(SongParam::Tempo);
    auto norm = [&](double bpm) { return static_cast<float>(desc->toNormalized(bpm)); };
    a.points = {{0, norm(from), 0, AutoShape::Curve}, {bar * bars, norm(to), 0, AutoShape::Curve}};
    if (stepped) {  // (a new tempo every bar, from `from` towards `to`)
      a.points.clear();
      for (int b = 0; b <= bars; ++b) a.points.push_back({bar * b, norm(from + (to - from) * b / bars), 0, AutoShape::Hold});
    }
    s.automations.push_back(a);
    Clip ac;
    ac.id = s.allocateId();
    ac.kind = ClipKind::Automation;
    ac.source = a.id;
    ac.track = 1;
    ac.start = 0;
    ac.length = bar * bars;
    s.playlist.push_back(ac);
  }
  return s;
}

// The channel of a held project run as the audio thread runs it: blocks of 64 frames (the control
// blocks a tempo automation plays in), the tempo of each block's start, paced in real time so the
// warp worker has the time it really has. Before the song, two seconds stopped at the first tempo
// (the instrument open in the project before Play). `took` (optional): the seconds the song took;
// `madeAt` (optional): when each block of 64 frames was made (seconds on steady_clock).
std::vector<float> realtimeOf(const Song& s, int64_t frames, double* took = nullptr, std::vector<double>* madeAt = nullptr) {
  TempoMap map;
  map.build(s);
  const PluginState& st = s.channels.front().plugin;
  auto gen = createGenerator(sm::kType);
  ChannelAux aux;
  packs::prepareAux(st, aux);
  std::vector<float> L(static_cast<size_t>(frames), 0.f), R(static_cast<size_t>(frames), 0.f);
  ProcessContext ctx;
  ctx.sampleRate = kRate, ctx.ppq = s.ppq, ctx.tempo = map.bpmAt(0);
  {
    std::vector<float> l(512), r(512);
    for (int i = 0; i < 200; ++i) {
      gen->process(st, aux, ctx, l.data(), r.data(), 512);
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
  ctx.playing = true;
  if (madeAt) madeAt->reserve(static_cast<size_t>(frames / 64 + 1));
  gen->noteOn(1, static_cast<double>(std::lround(st.params[sm::SmBaseNote]) - 12), 1.0f, 0);
  const auto t0 = std::chrono::steady_clock::now();
  for (int64_t pos = 0; pos < frames; pos += 64) {
    const int n = static_cast<int>(std::min<int64_t>(64, frames - pos));
    ctx.tick = map.tickAtSeconds(static_cast<double>(pos) / kRate);
    ctx.tempo = map.bpmAt(ctx.tick);
    ctx.jumped = pos == 0;
    if (madeAt) madeAt->push_back(std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count());
    gen->process(st, aux, ctx, L.data() + pos, R.data() + pos, n);
    if (pos % 512 == 0) std::this_thread::sleep_until(t0 + std::chrono::microseconds(static_cast<int64_t>(static_cast<double>(pos) / kRate * 1e6)));
  }
  if (took) *took = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  return L;
}

// Median frequency of a window (zero-crossing periods).
double windowFreq(const std::vector<float>& x, size_t i0, size_t i1) {
  std::vector<double> per;
  double last = -1;
  for (size_t i = std::max<size_t>(1, i0); i < std::min(i1, x.size()); ++i)
    if (x[i - 1] < 0 && x[i] >= 0 && std::fabs(x[i]) + std::fabs(x[i - 1]) > 1e-3f) {
      const double t = static_cast<double>(i - 1) + x[i - 1] / static_cast<double>(x[i - 1] - x[i]);
      if (last >= 0) per.push_back(t - last);
      last = t;
    }
  if (per.size() < 3) return 0;
  std::nth_element(per.begin(), per.begin() + static_cast<long>(per.size() / 2), per.end());
  return kRate / per[per.size() / 2];
}
double rmsDb(const std::vector<float>& x, size_t a, size_t b) {
  double s = 0;
  for (size_t i = a; i < std::min(b, x.size()); ++i) s += static_cast<double>(x[i]) * x[i];
  return 10 * std::log10(std::max(1e-20, s / static_cast<double>(std::max<size_t>(1, b - a))));
}

struct SliceAt {
  double tempo = 0, hz = 0, cents = 0, db = 0;
};
// Every sixteenth of the song (the slice the held pattern plays there), measured in its middle.
std::vector<SliceAt> slicesOf(const Song& s, const std::vector<float>& x, int bars) {
  TempoMap map;
  map.build(s);
  std::vector<SliceAt> out;
  for (int k = 0; k < bars * 16; ++k) {
    const double t0 = k * s.ppq / 4.0, t1 = (k + 1) * s.ppq / 4.0;
    const double f0 = map.secondsAt(t0) * kRate, f1 = map.secondsAt(t1) * kRate;
    const auto a = static_cast<size_t>(f0 + 0.15 * (f1 - f0)), b = static_cast<size_t>(f0 + 0.85 * (f1 - f0));
    SliceAt sl;
    sl.tempo = map.bpmAt(t0);
    sl.hz = windowFreq(x, a, b);
    sl.cents = sl.hz > 0 ? 1200 * std::log2(sl.hz / kF0) : 0;
    sl.db = rmsDb(x, a, b);
    out.push_back(sl);
  }
  return out;
}

void rampCase(const Sample& src, int bars, double from, double to, bool stepped = false) {
  const Song s = heldProject(src, bars, from, to, stepped);
  const Out ex = renderOf(s);
  char name[64];
  std::snprintf(name, sizeof name, "%s: %g -> %g bpm", stepped ? "stepped" : "ramp", from, to);
  dump(std::string(stepped ? "held_stepped_" : "held_ramp_") + std::to_string(static_cast<int>(from)) + "_" + std::to_string(static_cast<int>(to)), ex.audio);
  std::vector<float> exL(ex.audio.frames);
  for (size_t i = 0; i < ex.audio.frames; ++i) exL[i] = ex.audio.at(0, i);
  TempoMap map;
  map.build(s);
  const auto frames = static_cast<int64_t>(map.secondsAt(static_cast<double>(s.ppq) * 4 * bars) * kRate);
  sm::resetWarpPeakForTest();
  const sm::WarpStats w0 = sm::warpStatsForTest();
  const auto rt0 = std::chrono::steady_clock::now();
  double songTook = 0;
  std::vector<double> madeAt;
  sm::setWarpLogForTest(true);
  const std::vector<float> rtL = realtimeOf(s, frames, &songTook, &madeAt);
  const std::vector<sm::WarpLogEntry> made = sm::warpLogForTest();
  sm::setWarpLogForTest(false);
  const sm::WarpStats w1 = sm::warpStatsForTest();
  const double rtSecs = std::chrono::duration<double>(std::chrono::steady_clock::now() - rt0).count();
  // (the export's level is the channel's through the mixer: the two are lined up on their first slice)
  const auto ex1 = slicesOf(s, exL, bars), rt1 = slicesOf(s, rtL, bars);
  const double off = ex1.front().db - rt1.front().db;
  double worstEx = 0, worstRt = 0, worstLevel = 0, worstRtLate = 0;
  constexpr double kLiveCents = 8;  // (as in the instrument)
  int overTen = 0, noneNear = 0;
  double worstPastThere = -1e9;  // the most a slice over 10 cents was further off than the render there allowed (+2 cents)
  int offEx = 0, offRt = 0;
  // real time: in each stretch of one tempo (a bar of the steps; the ramp: all of it) the first two
  // slices may still play without a render near the tempo (the worker makes it meanwhile), every
  // slice after them is in tune
  const size_t seg = stepped ? 16 : ex1.size();
  int fallback = 0;
  std::vector<bool> late(ex1.size(), false);
  for (size_t k = 0; k < rt1.size(); ++k) {
    late[k] = k % seg >= 2;
    fallback += std::fabs(rt1[k].cents) > 10 && !late[k];
  }
  // How far off the renders made in this run that were there when each slice started let it be
  // (cents): a slice plays the render nearest the tempo of the block it starts in, at that tempo
  // over the render's (faster or slower), to its end. (Renders of the cases before, still held, are
  // not in the log: a slice playing one of them is nearer than this, never further.)
  TempoMap tm;
  tm.build(s);
  std::vector<double> nearestThere(ex1.size(), 1e9);
  for (size_t k = 0; k < ex1.size() && !madeAt.empty(); ++k) {
    const auto block = std::min(madeAt.size() - 1, static_cast<size_t>(tm.secondsAt(static_cast<double>(k) * s.ppq / 4.0) * kRate) / 64);
    const double startedAt = madeAt[block], tempo = tm.bpmAt(tm.tickAtSeconds(static_cast<double>(block * 64) / kRate));
    for (const auto& m : made)
      if (m.at <= startedAt) nearestThere[k] = std::min(nearestThere[k], std::fabs(1200 * std::log2(tempo / m.tempo)));
  }
  for (size_t k = 0; k < ex1.size(); ++k) {
    const auto& e = ex1[k];
    const auto& r = rt1[k];
    if (std::getenv("RAMP_DEBUG"))
      std::printf("#   slice %3zu  %6.2f bpm  export %7.2f Hz %+7.1f c %6.1f dB   realtime %7.2f Hz %+7.1f c %6.1f dB\n", k, e.tempo, e.hz, e.cents,
                  e.db, r.hz, r.cents, r.db);
    worstEx = std::max(worstEx, std::fabs(e.cents));
    worstRt = std::max(worstRt, std::fabs(r.cents));
    offEx += std::fabs(e.cents) > 10;
    offRt += std::fabs(r.cents) > 10;
    if (late[k]) worstRtLate = std::max(worstRtLate, std::fabs(r.cents));
    // (on a shared test machine: a slice off by more than the 10 cents only as far as the renders there allow)
    if (late[k] && std::fabs(r.cents) > 10) {
      ++overTen;
      noneNear += nearestThere[k] > kLiveCents;
      worstPastThere = std::max(worstPastThere, std::fabs(r.cents) - (nearestThere[k] + 2));
    }
    worstLevel = std::max(worstLevel, std::fabs(e.db - r.db - off));
  }
  std::printf("# %s, %d bars, %zu slices: export worst %.1f cents (%d over 10), realtime worst %.1f cents (%d over 10), levels within %.2f dB\n",
              name, bars, ex1.size(), worstEx, offEx, worstRt, offRt, worstLevel);
  std::printf("# %s: realtime: %d of the first two slices after a tempo change off by over 10 cents, worst %.1f cents after them\n", name,
              fallback, worstRtLate);
  std::printf("# %s: realtime: the worker made %d renders (%d while the tempo moved), stretching for %.2f s of the %.2f s (%.0f%%); renders held: %.1f MB at most (with those of the cases before, still in their grace period)\n",
              name, w1.renders - w0.renders, w1.moving - w0.moving, w1.workSeconds - w0.workSeconds, rtSecs,
              100 * (w1.workSeconds - w0.workSeconds) / std::max(1e-3, rtSecs), static_cast<double>(w1.peakBytes) / (1 << 20));
  // The 10 cents hold while the worker keeps up: it aims each render where the tempo will be once
  // the render is ready (its last render time plus a wakeup), kLiveCents further on, so a slice is
  // at most about kLiveCents off; after a step in tempo the new tempo's render is there within the
  // two slices excused. What the aim cannot know is the machine's own delays: a wakeup that ends
  // late, a stretch that takes longer than the ones before (another program, a shared virtual
  // machine's other guests), the test's own thread late and then catching up. On a shared test
  // machine (CI set) a slice over the 10 cents is judged by the renders that were there when it
  // started: it may be off by as much as the one it plays, the nearest of them (+2 cents: the pitch
  // measured over a slice's zero crossings), never more (a voice playing a render further off than
  // one that was there is off by more than that on any machine). Anywhere else,
  // the 10 cents. The worker's own figures for the machine's delays (worstLateSeconds) and the
  // fastest the tempo moves are printed to explain a slower machine.
  const double songSecs = static_cast<double>(frames) / kRate;
  const int limited = w1.throttled - w0.throttled;
  double rate = 0;  // the fastest the tempo moves (cents a second) within a stretch of one tempo's renders
  for (size_t k = 0; k + 1 < ex1.size(); ++k)
    if ((k + 1) % seg != 0) rate = std::max(rate, std::fabs(1200 * std::log2(ex1[k + 1].tempo / ex1[k].tempo)) / (15.0 / ex1[k].tempo));
  const double render = (w1.workSeconds - w0.workSeconds) / std::max(1, w1.renders - w0.renders);
  const bool ci = std::getenv("CI") != nullptr;
  if (from != to) {
    std::printf("# %s: realtime: the machine's delays: a render came up to %.3f s later than planned (%.3f s a render), %d wakeups rate limited, the tempo moving up to %.0f cents a second\n",
                name, w1.worstLateSeconds, render, limited, rate);
    std::printf("# %s: realtime: %d slices after the first two over 10 cents, %d of them with the nearest render there when they started over %.0f cents off; the most one was further off than that render: %.1f cents%s\n",
                name, overTen, noneNear, kLiveCents, overTen ? worstPastThere : 0.0, ci ? " (a shared test machine: allowed up to 0)" : "");
  }
  std::printf("# %s: realtime: the song took %.2f s of real time for %.2f s\n", name, songTook, songSecs);
  if (from != to)
    expect(ci ? overTen == 0 || worstPastThere <= 0 : worstRtLate <= 10,
           (std::string(name) + ": real time plays every slice within 10 cents once its tempo's render is there").c_str());
  expect(ex.audio.frames > 0 && ex1.size() == static_cast<size_t>(bars * 16), (std::string(name) + ": the export has every slice").c_str());
  expect(worstEx < 5, (std::string(name) + ": every slice of the export at the source pitch (within 5 cents)").c_str());
  expect(worstLevel < 1, (std::string(name) + ": ... at the level the instrument plays it in real time (within 1 dB)").c_str());
  if (from == to) {
    // (a constant tempo: the worker's render is there from the start, real time plays it too)
    expect(worstRt < 5, (std::string(name) + ": ... and at the pitch real time plays it").c_str());
    // null test: as the reference way (one whole render per tempo), sample for sample
    sm::setWholeOfflineRenders(true);
    const Out ref = renderOf(s);
    sm::setWholeOfflineRenders(false);
    const Diff d = diffOf(ref.audio, ex.audio);
    expect(d.sameLength && d.maxAbs == 0, (std::string(name) + ": the export is the reference render sample for sample").c_str());
  }
}

void testRamp() {
  const SamplePtr src = loadSine();
  if (!src) return;
  const int bars = barsFromEnv(4);
  rampCase(*src, bars, 120, 120);
  rampCase(*src, bars, 100, 140);
  rampCase(*src, bars, 100, 140, true);
}

}  // namespace

int main(int argc, char** argv) {
  const std::string only = argc > 1 ? argv[1] : "";
  if (only.empty() || only == "equal") testEqual();
  if (only.empty() || only == "speed") testSpeed();
  if (only.empty() || only == "ramp") testRamp();
  sm::waitAnalysisIdle();
  return finish();
}
