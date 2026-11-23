// Gridline — "seltmann" pack tests (0.7.8): switching the sequence while it plays, and blank steps.
//   switch    the instrument as the audio thread runs it (prepared data from the state, the warp
//             renders made by the worker in real time, the data before freed a few blocks after a
//             change, as the engine does): a layout picked while the pattern plays (other region
//             lengths, other chops, mid-bar and on a bar line, then undo back) plays at the right
//             pitch from its first step, at the same level, with no new clicks, with Follow:
//             Stretch and Resample.
//   blanks    Remove from the sequence leaves a blank of the same length (the sequence keeps its
//             length, every later step its place and its sound); Delete step still takes a step
//             out; a blank is filled with a region; blanks save, load and are what a linked sent
//             pattern follows; the factory layouts' own blanks are the same thing.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include "../Check.h"
#include "core/PluginInfo.h"
#include "core/Song.h"
#include "engine/Generators.h"
#include "packs/Packs.h"
#include "packs/seltmann/seltmann.h"
#include "packs/seltmann/seltmann_editor.h"

using namespace gl;
namespace sm = gl::packs::seltmann;
namespace fs = std::filesystem;

namespace {

constexpr double kRate = 44100, kF0 = 220, kSrcBpm = 100, kTempo = 130;

fs::path tmpDir() {
  const fs::path d = fs::temp_directory_path() / "gl_seltmann_switch_test";
  std::error_code ec;
  fs::create_directories(d, ec);
  return d;
}

// A 220 Hz sine, 16 bars at 100 bpm.
SamplePtr sine() {
  auto s = std::make_shared<Sample>();
  s->sampleRate = kRate;
  std::vector<float> L(static_cast<size_t>(kRate * 60.0 / kSrcBpm * 4 * 16));
  for (size_t i = 0; i < L.size(); ++i) L[i] = static_cast<float>(0.5 * std::sin(2 * 3.14159265358979 * kF0 * static_cast<double>(i) / kRate));
  s->left = L;
  s->right = L;
  return s;
}

// Four one-bar regions from bar 1, played in order.
sm::Model baseModel(const Sample& src) {
  sm::Model m;
  m.srcFrames = static_cast<int64_t>(src.frames());
  m.srcRate = kRate;
  m.grid = sm::flatGrid(src, kSrcBpm);
  m.chop.mode = sm::ChopGrid;
  m.chop.division = sm::Div1Bar;
  m.chop.zeroCross = false;
  sm::setRegionBars(m, 0, 4);
  sm::rechop(m, &src);
  sm::straightPattern(m);
  return m;
}

const sm::Preset* preset(const std::string& name) {
  for (const auto& p : sm::factoryPresets())
    if (p.name == name) return &p;
  return nullptr;
}

PluginState stateOf(const std::string& path, const sm::Model& m, int sync) {
  PluginState st = defaultPluginState(sm::kType);
  st.data = sm::formatData(path, m);
  st.params[sm::SmSync] = static_cast<float>(sync);
  st.params[sm::SmVelocity] = 0;
  return st;
}

// Median frequency of a window (zero-crossing periods: a join between slices only spoils one).
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

double cents(double f, double ref) { return f > 0 ? 1200.0 * std::log2(f / ref) : 0; }

struct Switch {
  std::string label;
  std::vector<std::pair<int64_t, const PluginState*>> at;  // (frame, state) changes after the start
};

struct Result {
  double worstCents = 0;  // worst window after the first switch
  double worstLevel = 0;  // worst window RMS against the steady level (dB)
  int badWindows = 0;     // windows more than 25 cents off
  int windows = 0;
  double jumpBefore = 0, jumpAfter = 0;  // largest sample-to-sample step before / after the first switch
};

// Plays the pattern key with the song running (bar-locked), paced like the audio thread (blocks
// of 512 in real time) so the warp worker has the time it really has. As in the engine, every
// change prepares the instance's data anew (a program rebuild) and the data before goes two blocks
// later. The project has played for two seconds before the measured part starts.
Result runSwitches(const PluginState& first, const Switch& sw, int64_t warm, int64_t after, double ref) {
  auto gen = createGenerator(sm::kType);
  auto cur = std::make_unique<ChannelAux>();
  std::unique_ptr<ChannelAux> old;
  int oldBlocks = 0;
  packs::prepareAux(first, *cur);
  const PluginState* st = &first;
  {
    ProcessContext ctx;
    ctx.sampleRate = kRate, ctx.tempo = kTempo, ctx.ppq = 96;
    std::vector<float> L(512), R(512);
    for (int i = 0; i < 200; ++i) {
      gen->process(first, *cur, ctx, L.data(), R.data(), 512);
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
  const int64_t total = warm + after;
  std::vector<float> L(static_cast<size_t>(total), 0.f), R(static_cast<size_t>(total), 0.f);
  ProcessContext ctx;
  ctx.sampleRate = kRate, ctx.tempo = kTempo, ctx.ppq = 96, ctx.playing = true;
  const int block = 512;
  gen->noteOn(1, 48, 1.0f, 0);
  for (int64_t pos = 0; pos < total; pos += block) {
    const int n = static_cast<int>(std::min<int64_t>(block, total - pos));
    for (const auto& [f, s2] : sw.at)
      if (f >= pos && f < pos + n) {
        st = s2;
        old = std::move(cur);
        cur = std::make_unique<ChannelAux>();
        packs::prepareAux(*st, *cur);
        oldBlocks = 2;
      }
    ctx.tick = static_cast<double>(pos) / kRate * kTempo / 60.0 * ctx.ppq;
    ctx.jumped = pos == 0;
    const auto t0 = std::chrono::steady_clock::now();
    gen->process(*st, *cur, ctx, L.data() + pos, R.data() + pos, n);
    if (old && --oldBlocks <= 0) old.reset();  // the program before is freed
    std::this_thread::sleep_until(t0 + std::chrono::microseconds(static_cast<int64_t>(n / kRate * 1e6)));
  }
  auto rms = [&](size_t a, size_t b) {
    double s = 0;
    for (size_t i = a; i < b; ++i) s += static_cast<double>(L[i]) * L[i];
    return std::sqrt(s / static_cast<double>(std::max<size_t>(1, b - a)));
  };
  const double steady = rms(static_cast<size_t>(warm / 4), static_cast<size_t>(warm));
  Result r;
  const size_t win = static_cast<size_t>(kRate * 0.05);
  const size_t from = static_cast<size_t>(sw.at.empty() ? warm : sw.at.front().first);
  for (size_t i = static_cast<size_t>(warm / 4); i < static_cast<size_t>(total); ++i) {
    double& j = i < from ? r.jumpBefore : r.jumpAfter;
    j = std::max(j, static_cast<double>(std::fabs(L[i] - L[i - 1])));
  }
  for (size_t i = from; i + win <= static_cast<size_t>(total); i += win) {
    const double f = windowFreq(L, i, i + win);
    const double c = std::fabs(cents(f, ref));
    const double lv = rms(i, i + win);
    ++r.windows;
    if (std::getenv("SWITCH_DEBUG")) std::printf("#   %6.3f s  %7.2f Hz  rms %.4f\n", static_cast<double>(i) / kRate, f, lv);
    if (f <= 0) continue;  // a rest
    r.worstCents = std::max(r.worstCents, c);
    if (c > 25) ++r.badWindows;
    if (lv > 1e-4 && steady > 0) r.worstLevel = std::max(r.worstLevel, std::fabs(20 * std::log10(lv / steady)));
  }
  std::printf("# %-44s worst %6.1f cents, %2d/%2d windows over 25 cents, level within %.2f dB, jumps %.4f / %.4f\n", sw.label.c_str(),
              r.worstCents, r.badWindows, r.windows, r.worstLevel, r.jumpBefore, r.jumpAfter);
  return r;
}

void checkRun(const Result& r, const std::string& label) {
  expect(r.badWindows == 0, ("switch: " + label + ": the right pitch from the first step").c_str());
  expect(r.worstLevel < 2, ("switch: " + label + ": the same level (no doubled steps)").c_str());
  // a sine at 0.4: the steepest step is 2 pi 220 / 44100 * 0.4 = 0.0125; joins are declicked
  expect(r.jumpAfter <= std::max(0.02, r.jumpBefore * 1.25), ("switch: " + label + ": no click").c_str());
}

void testSwitch() {
  SamplePtr src = sine();
  const std::string path = (tmpDir() / "switch_sine.wav").string();
  expect(sm::writeLoopWav(path, src->left, src->right.vec(), kRate), "switch: write the sine");
  src = sampleCache().get(path);
  expect(src && src->frames() > 0, "switch: the sine decodes");
  if (!src) return;
  const sm::Model base = baseModel(*src);
  const double bar = 60.0 / kTempo * 4 * kRate;  // project bar (frames)
  const int64_t warm = static_cast<int64_t>(bar * 2);
  const int64_t after = static_cast<int64_t>(kRate * 1.6);
  for (int sync : {sm::SyncStretch, sm::SyncResample}) {
    const bool stretch = sync == sm::SyncStretch;
    const char* sn = stretch ? "stretch" : "resample";
    // Resample plays the source faster at the faster tempo: its pitch follows the speed
    const double want = stretch ? kF0 : kF0 * kTempo / kSrcBpm;
    // regions of 2, 1, 4, 8 and 1 bars; bar and half-bar steps; a flip on eighths
    const std::vector<std::string> names = {"Two Bar Loop", "One Bar Loop", "Call and Reply", "Long Story", "Instant Flip"};
    for (const auto& nm : names) {
      const sm::Preset* p = preset(nm);
      expect(p != nullptr, ("switch: layout " + nm).c_str());
      if (!p) continue;
      const sm::Model b = sm::previewModel(base, src.get(), *p, 1);
      const PluginState s0 = stateOf(path, base, sync), s1 = stateOf(path, b, sync);
      for (double where : {0.5, 1.0}) {  // mid-bar, on the bar line
        Switch sw;
        sw.label = std::string(sn) + ": " + nm + (where < 1 ? " mid-bar" : " on the bar line");
        sw.at = {{static_cast<int64_t>(warm - bar + bar * where), &s1}};
        checkRun(runSwitches(s0, sw, warm, after, want), sw.label);
      }
    }
    // a layout, then undo back to the sequence before (each a new program), on bar lines
    {
      const sm::Preset* p = preset("Long Story");
      if (p) {
        const sm::Model b = sm::previewModel(base, src.get(), *p, 1);
        const PluginState s0 = stateOf(path, base, sync), s1 = stateOf(path, b, sync), s2 = stateOf(path, base, sync);
        Switch sw;
        sw.label = std::string(sn) + ": Long Story, then undo";
        sw.at = {{static_cast<int64_t>(warm), &s1}, {static_cast<int64_t>(warm + bar), &s2}};
        checkRun(runSwitches(s0, sw, warm, static_cast<int64_t>(bar * 2), want), sw.label);
      }
    }
  }
  // the sequence typed or edited (same region) while it plays: a rest and a new order
  {
    sm::Model b = base;
    sm::Pattern pt = b.pattern;
    expect(sm::patternFromText("4 . 2 1", pt), "switch: a typed sequence");
    b.pattern.steps = pt.steps;
    const PluginState s0 = stateOf(path, base, sm::SyncStretch), s1 = stateOf(path, b, sm::SyncStretch);
    Switch sw;
    sw.label = "stretch: typed sequence on the bar line";
    sw.at = {{static_cast<int64_t>(warm), &s1}};
    checkRun(runSwitches(s0, sw, warm, after, kF0), sw.label);
  }
}

// ------------------------------------------------------------------ blanks
void testBlanks() {
  SamplePtr src = sine();
  sm::Model m = baseModel(*src);
  sm::Pattern pt = m.pattern;
  expect(sm::patternFromText("1 2 ~ 3:p+5 4:b1.5", pt), "blanks: a sequence with a tie, a pitch and an own length");
  m.pattern.steps = pt.steps;
  m.pattern.unitBeats = 1;
  const int ns = static_cast<int>(m.pattern.steps.size());
  std::vector<double> starts;
  for (int i = 0; i < ns; ++i) starts.push_back(sm::stepStart(m.pattern, i, 0));
  const double cycle = sm::cycleBeats(m), stepsB = m.pattern.stepsBeats();
  // Remove from the sequence: a blank in its place
  for (int i = 0; i < ns; ++i) {
    sm::Model c = m;
    expect(sm::clearStep(c, i), "blanks: a step becomes a blank");
    const sm::Step& s = c.pattern.steps[static_cast<size_t>(i)];
    bool same = static_cast<int>(c.pattern.steps.size()) == ns && sm::cycleBeats(c) == cycle && c.pattern.stepsBeats() == stepsB;
    for (int j = 0; j < ns; ++j) same = same && sm::stepStart(c.pattern, j, 0) == starts[static_cast<size_t>(j)];
    for (int j = 0; j < ns; ++j)
      if (j != i) same = same && c.pattern.steps[static_cast<size_t>(j)] == m.pattern.steps[static_cast<size_t>(j)];
    char nm[96];
    std::snprintf(nm, sizeof nm, "blanks: step %d removed: same length, every other step in place", i + 1);
    expect(same && s.src == sm::kRest && sm::stepSlice(c.pattern, s, static_cast<int>(c.slices.size())) < 0, nm);
    expect(!sm::clearStep(c, i), "blanks: a blank stays a blank (nothing to do)");
    // fill it again: the region plays in the same place
    expect(sm::fillStep(c, i, 2) && sm::stepSlice(c.pattern, c.pattern.steps[static_cast<size_t>(i)], static_cast<int>(c.slices.size())) == 2,
           "blanks: a blank filled with region C plays C");
    expect(sm::stepStart(c.pattern, i, 0) == starts[static_cast<size_t>(i)] && sm::cycleBeats(c) == cycle, "blanks: ... in the blank's place");
  }
  expect(!sm::fillStep(m, 0, 99) && !sm::fillStep(m, 99, 0) && !sm::clearStep(m, -1), "blanks: out of range does nothing");
  // Delete step: the later steps move up
  {
    sm::Model c = m;
    expect(sm::deleteStep(c, 1) && static_cast<int>(c.pattern.steps.size()) == ns - 1, "blanks: Delete step takes it out");
    expect(sm::stepStart(c.pattern, 1, 0) == starts[1], "blanks: ... the next step moves into its place");
    sm::Model one = m;
    one.pattern.steps.resize(1);
    expect(!sm::deleteStep(one, 0), "blanks: the last step stays");
  }
  // save and load: the blank keeps its tie and its own length
  {
    sm::Model c = m;
    sm::clearStep(c, 1);  // tied (2 units)
    sm::clearStep(c, 3);  // its own length
    sm::Model r;
    expect(sm::modelFromText(sm::modelToText(c), r) && r.pattern == c.pattern, "blanks: save and load");
    expect(r.pattern.steps[1].ties == 1 && r.pattern.steps[3].beats == 1.5 && sm::cycleBeats(r) == cycle, "blanks: ... with their lengths");
  }
  // the sound: a blank is silent and nothing else changes (sample for sample outside it)
  for (int sync : {sm::SyncOriginal, sm::SyncResample, sm::SyncStretch}) {
    sm::RenderOpts o;
    o.sampleRate = kRate;
    o.tempo = kTempo;
    o.sync = sync;
    sm::Model c = m;
    sm::clearStep(c, 2);
    std::vector<float> L0, R0, L1, R1;
    const bool ok = sm::renderPatternLoop(m, src, o, L0, R0) && sm::renderPatternLoop(c, src, o, L1, R1) && L0.size() == L1.size();
    const double bpm = sync == sm::SyncOriginal ? kSrcBpm : kTempo;
    const auto at = [&](double beat) { return static_cast<size_t>(std::llround(beat * 60.0 / bpm * kRate)); };
    const size_t b0 = at(starts[2]), b1 = at(starts[3]);
    double outside = 0, inside = 0, level = 0;
    for (size_t i = 0; ok && i < L0.size(); ++i) {
      level = std::max(level, static_cast<double>(std::fabs(L1[i])));
      if (i + 64 < b0 || i > b1 + static_cast<size_t>(kRate * 0.05)) outside = std::max(outside, static_cast<double>(std::fabs(L0[i] - L1[i])));
      if (i > b0 + static_cast<size_t>(kRate * 0.05) && i < b1) inside = std::max(inside, static_cast<double>(std::fabs(L1[i])));
    }
    std::printf("# blanks sync %d: outside the blank %.3g, inside %.3g, peak %.3f\n", sync, outside, inside, level);
    expect(ok && level > 0.2 && outside < 1e-6 && inside < 1e-6,"blanks: the blank is silent, the rest of the loop sounds as before");
  }
  // the linked sent pattern follows: the blank's notes go, every other note keeps its time
  {
    Song s = makeEmptySong();
    Channel ch;
    ch.id = s.allocateId();
    ch.plugin = defaultPluginState(sm::kType);
    s.channels.push_back(ch);
    const std::vector<float> params = ch.plugin.params;
    const ObjectId pid = sm::sendPatternToSong(s, ch.id, m, params, 1, 0, 0, true);
    const gl::Pattern* p = s.pattern(pid);
    expect(p && sm::linkState(*p, ch.id) == sm::LinkFollowing, "blanks: a linked sent pattern");
    if (p) {
      const std::vector<Note> before = p->notes.at(ch.id);
      sm::Model c = m;
      sm::clearStep(c, 1);
      gl::Pattern q = *p;
      expect(sm::refreshLinked(s, q, ch.id, c, params), "blanks: ... follows the removal");
      const std::vector<Note>& after = q.notes.at(ch.id);
      bool kept = after.size() + 1 == before.size();
      for (const Note& nt : after) {
        bool found = false;
        for (const Note& b : before) found = found || (b.start == nt.start && b.pitch == nt.pitch && b.length == nt.length);
        kept = kept && found;
      }
      expect(kept, "blanks: ... the blank's note goes, every other note keeps its time");
    }
  }
  // the factory layouts' own blanks are the same blanks (a rest of one step)
  {
    const sm::Preset* p = preset("Two-Step Shuffle");
    expect(p != nullptr, "blanks: a factory layout with blanks");
    if (p) {
      sm::Model c = sm::previewModel(baseModel(*src), src.get(), *p, 1);
      int blanks = 0;
      for (const auto& s : c.pattern.steps) blanks += s.src == sm::kRest && s.ties == 0 && s.beats == 0 ? 1 : 0;
      sm::Model d = c;
      const int first = [&] {
        for (int i = 0; i < static_cast<int>(c.pattern.steps.size()); ++i)
          if (c.pattern.steps[static_cast<size_t>(i)].src != sm::kRest) return i;
        return -1;
      }();
      sm::clearStep(d, first);
      expect(blanks > 0 && d.pattern.steps[static_cast<size_t>(first)] == c.pattern.steps[1], "blanks: ... a removed step is exactly such a blank");
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  const std::string only = argc > 1 ? argv[1] : "";
  if (only.empty() || only == "blanks") testBlanks();
  if (only.empty() || only == "switch") testSwitch();
  sm::waitAnalysisIdle();
  return finish();
}
