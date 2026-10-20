// Gridline — J-41 tests: FL point shapes, the shared articulator engine, the
// Envelope Controller articulator model (engine, decoder, encoder),
// automation curve model switch, Slicex region engine, Patcher graphs,
// Fruity Slicer stretch, Autogun / Sakura levels. Corpus checks read FL
// Studio's shipped content in place when it is installed.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <string>
#include <vector>
#include "Check.h"
#include "core/Automation.h"
#include "core/AutomationEdit.h"
#include "core/FlCurves.h"
#include "core/Json.h"
#include "core/PluginInfo.h"
#include "core/Project.h"
#include "core/flp/Flp.h"
#include "core/flp/FlpEncode.h"
#include "core/flp/FlpPresets.h"
#include "engine/FlArticulator.h"
#include "engine/Generators.h"
#include "engine/Program.h"
#include "engine/SliceProcess.h"
#include "packs/Packs.h"
#include "packs/fl_corpus.h"
#include "packs/modulation/envc.h"
#include "packs/modulation/modulation.h"
#include "packs/modulation/patchv2.h"
#include "core/PluginParams.h"
#include <fstream>
#include <functional>
#include <array>

using namespace gl;
namespace md = gl::packs::modulation;

namespace {

namespace fs = std::filesystem;

bool near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) <= eps; }

// ------------------------------------------------------------ point shapes
void testShapes() {
  using namespace flcurve;
  bool ends = true, linear = true, mono = true;
  for (int s : {0, 1, 7, 8, 9, 11, 12}) {
    for (double t : {-1.0, -0.6, -0.2, 0.3, 0.7, 1.0}) {
      ends = ends && near(shape(s, t, 0), 0, 1e-9) && near(shape(s, t, 1), 1, 1e-9);
      double prev = -1;
      for (int i = 0; i <= 200; ++i) {
        const double y = shape(s, t, i / 200.0);
        mono = mono && y >= prev - 1e-12;
        prev = y;
      }
    }
    for (int i = 0; i <= 10; ++i) linear = linear && near(shape(s, 0, i / 10.0), i / 10.0, 1e-12);
  }
  expect(ends, "shapes: single / double curves 1-3 and half sine run 0 → 1");
  expect(linear, "shapes: tension 0 is linear for the curve families");
  expect(mono, "shapes: curve families are monotonic");
  // Single curve kernel: 10^(6|t|) range; t > 0 starts fast.
  const double k = 1.0 - (std::pow(10.0, 3.0 * 0.5) - 1.0) / (std::pow(10.0, 3.0) - 1.0);
  expect(near(singleCurve(0.5, 0.5), k, 1e-12) && singleCurve(0.5, 0.25) > 0.5 && singleCurve(-0.5, 0.25) < 0.25,
         "shapes: single curve law (t > 0 fast start, t < 0 slow start)");
  expect(near(shape(1, 0.7, 0.5), 0.5, 1e-12) && near(shape(1, 0.7, 0.25), 0.5 * singleCurve(0.7, 0.5), 1e-12),
         "shapes: double curve = two halves of the single curve");
  expect(stepCount(0) == 1000 && stepCount(0.5) == 16 && stepCount(1) == 0, "shapes: step counts n(t) (1000 / 16 / 0)");
  expect(near(shape(2, 0.3, 0.99), 0) && near(shape(4, 0.5, 0), 0, 1e-12) && near(shape(4, 0.5, 1), 1, 1e-12),
         "shapes: hold stays, smooth stairs end at the point");
  {
    // Stairs t = 0.5: 17 + 1 levels; pulse alternates 0 / 1.
    double y = shape(3, 0.5, 0.5);
    expect(y > 0.4 && y < 0.6, "shapes: stairs midpoint");
    int flips = 0;
    double prev = shape(5, 0.5, 0);
    for (int i = 1; i <= 1000; ++i) {
      double v = shape(5, 0.5, i / 1000.0);
      flips += v != prev;
      prev = v;
    }
    expect(flips >= 30 && flips <= 36, "shapes: pulse has 2(n+1) edges at t = 0.5");
  }
  expect(near(shape(6, -0.5, 0), 0, 1e-12) && near(shape(6, -0.5, 1), 1, 1e-9) && near(shape(6, 0.5, 0), 0, 1e-12) &&
             near(shape(6, 0.5, 1), 1, 1e-12),
         "shapes: wave (sine for t < 0, triangle for t >= 0) ends at the point");
  expect(near(shape(7, 1, 0.5), 1 - std::pow(0.5, 6), 1e-12), "shapes: single curve 2 law x − t(u⁶ − u)");
  expect(near(shape(9, 1, 0.5), std::sin(3.14159265358979 / 4), 1e-9), "shapes: half sine at |t| = 1");
  expect(near(shape(10, 0, 0.5), 0.5, 1e-12) && near(shape(10, 0, 0.25), 3 * 0.0625 - 2 * 0.015625, 1e-12),
         "shapes: smooth without neighbours is the cubic ease");
  expect(near(smooth(0, 0.5, 1, 1), 0.5, 1e-12) && near(smooth(0, 0.3, 1, 1), 0.3, 1e-12), "shapes: smooth with unit tangents is linear");
  expect(near(smoothTangent(1, 1), 1, 1e-12) && smoothTangent(-1, 1) == 0 && smoothTangent(3, 1) < 3, "shapes: monotone smooth tangents");
}

// ------------------------------------------------------- articulator engine
struct EnvRun {
  std::vector<double> out;  // per tick
};

EnvRun runEnv(const flart::Record& r, const flart::Scales& sc, double centre, int ticks, int releaseAt, double tempo = 120, int ppq = 96) {
  EnvRun o;
  const flart::Prepared p = flart::prepare(r);
  flart::EnvState s;
  flart::envStart(s, p, sc, centre);
  const double dt = flart::unitsPerTick((r.flags & flart::kTempoTime) != 0, tempo, ppq);
  for (int i = 0; i < ticks; ++i) {
    if (i == releaseAt) flart::envRelease(s, p, sc, centre);
    flart::envTick(s, p, sc, centre, dt);
    o.out.push_back(s.out);
  }
  return o;
}

void testEngine() {
  // Default ADSR: 0.75 units attack (0.375 s), 1.5 decay to 0.5, release 0.25.
  flart::Record env = flart::defaultEnvelope();
  env.enabled = -1;
  flart::Scales sc;
  // 120 BPM, PPQ 96: 192 ticks per second; 1.0 unit = 0.5 s = 96 ticks.
  EnvRun a = runEnv(env, sc, 0, 600, 400);
  expect(near(a.out[71], 1.0, 1e-9) && a.out[70] < 1.0, "engine: attack reaches point 1 after 0.75 units (72 ticks)");
  expect(a.out[35] > 0.6, "engine: tension 0.2 bends the attack to a fast start");
  expect(near(a.out[71 + 144], 0.5, 1e-9) && near(a.out[399], 0.5, 1e-12), "engine: decay to the sustain point, then holds");
  if (std::getenv("GL_J41_DEBUG"))
    for (int i = 396; i < 430; ++i) std::printf("# rel %d %.6f\n", i, a.out[static_cast<size_t>(i)]);
  expect(near(a.out[400 + 23], 0.0, 1e-9) && a.out[400 + 22] > 0.0 && a.out[400] < 0.5, "engine: release falls to 0 in 0.25 units (from the release tick)");
  // Tempo-based time: 1.0 unit = 1 beat (96 ticks at any tempo).
  flart::Record tb = env;
  tb.flags |= flart::kTempoTime;
  EnvRun b = runEnv(tb, sc, 0, 200, -1, 60);
  expect(near(b.out[71], 1.0, 1e-9) && b.out[70] < 1.0, "engine: tempo-based times are beats");
  EnvRun c = runEnv(env, sc, 0, 200, -1, 60);
  expect(near(c.out[35], 1.0, 1e-9) && c.out[34] < 1.0, "engine: free times are tempo-independent (0.5 s per unit)");
  // Sustain offset moves the sustain level; scalers stretch the phases.
  flart::Scales s2;
  s2.sustain = 0.25, s2.decay = 0.5, s2.attack = 2;
  EnvRun d = runEnv(env, s2, 0, 600, -1);
  expect(near(d.out[143], 1.0, 1e-9) && d.out[142] < 1.0, "engine: attack scale ×2");
  expect(near(d.out[143 + 72], 0.75, 1e-9), "engine: decay scale ×0.5 reaches the offset sustain (0.75)");
  // Release mid-attack: continues from the current level.
  EnvRun e = runEnv(env, sc, 0, 120, 20);
  if (std::getenv("GL_J41_DEBUG"))
    for (int i = 16; i < 48; ++i) std::printf("# mid %d %.9f\n", i, e.out[static_cast<size_t>(i)]);
  expect(e.out[20] < e.out[19] && e.out[19] - e.out[20] < 0.08 && e.out[20 + 23] < 1e-9 && e.out[20 + 22] > 0,
         "engine: release mid-attack starts at the current level");
  // Loop: points 1..2 repeat while held.
  flart::Record lp = env;
  lp.loop = 0, lp.sustain = 2;  // segments 1 and 2 repeat (from the sustain value, no jump)
  EnvRun f = runEnv(lp, sc, 0, 800, -1);
  double lo = 1, hi = 0;
  for (int i = 400; i < 800; ++i) lo = std::min(lo, f.out[static_cast<size_t>(i)]), hi = std::max(hi, f.out[static_cast<size_t>(i)]);
  expect(lo < 0.55 && hi > 0.95, "engine: loop start → sustain loops while the note is held");
  // Centre: bipolar envelopes report value − 0.5.
  EnvRun g = runEnv(env, sc, 0.5, 100, -1);
  expect(near(g.out[71], 0.5, 1e-9), "engine: value minus the articulator centre");
  // LFO speed law.
  expect(near(flart::lfoRate(40898) * 2, 1.0, 2e-3) && near(flart::lfoRate(40898), 0.5, 1e-3), "engine: LFO speed 40898 = 1 Hz free, 0.5 / beat synced");
  expect(near(flart::lfoRate(65536) * 2, 32.0, 1e-9), "engine: LFO speed max 32 Hz");
  flart::LfoShape tri;
  tri.set(0, 0, 0, false);
  expect(near(tri.value(0), 1, 1e-9) && near(tri.value(0x80000000u), -1, 1e-6) && near(tri.value(tri.startPhase(0)), 0, 1e-6),
         "engine: LFO triangle (+1 → −1 → +1), starts on the rising zero crossing");
  flart::LfoShape sq;
  sq.set(0, 0, 64, false);
  expect(near(sq.value(0xF0000000u), 1, 1e-9), "engine: pulse width plateau at +1");
  // Maps: two points → linear.
  flart::Record mp = flart::defaultMap(0.5);
  mp.pts[0].value = 0, mp.pts[1].value = 1;
  const flart::Prepared pm = flart::prepare(mp);
  expect(near(flart::mapValue(pm, 0.25, 0.5), -0.25, 1e-12) && !pm.flat && flart::prepare(flart::defaultMap(0.5)).flat, "engine: maps");
  // Record text round trip (doubles exact).
  flart::Record t = env;
  t.pts[1].time = 0.1 + 0.2;
  t.pts[2].value = std::nextafter(0.5, 1.0);
  flart::Record back;
  expect(flart::readText(flart::writeText(t), back) && back == t, "engine: record text round-trips bit-exactly");
  std::vector<uint8_t> bin;
  flart::writeRecord(bin, t);
  flart::Record back2;
  size_t at = 0;
  expect(flart::readRecord(bin.data(), bin.size(), at, flart::KindEnv, back2) && back2 == t && at == bin.size(), "engine: record binary round trip");
}

// --------------------------------------------------- Envelope Controller
struct CtlRun {
  std::vector<std::array<float, 8>> out;  // per block
};

CtlRun runCtl(const PluginState& s, const std::vector<std::pair<int, double>>& notes, double seconds, double offAt, int block = 256,
              double tempo = 120, const std::function<void(int, PluginState&)>& perBlock = {}) {
  CtlRun r;
  auto g = createGenerator("gl.envctl");
  ChannelAux aux;
  prepareChannelAux(s, aux);
  ProcessContext ctx;
  ctx.sampleRate = 44100;
  ctx.tempo = tempo;
  ctx.ppq = 96;
  std::vector<float> L(static_cast<size_t>(block)), R(static_cast<size_t>(block));
  const size_t n = static_cast<size_t>(seconds * 44100), off = static_cast<size_t>(offAt * 44100);
  uint32_t id = 1;
  PluginState st = s;
  for (auto [p, v] : notes) g->noteOn(id++, p, static_cast<float>(v), 0);
  int bi = 0;
  for (size_t pos = 0; pos < n; pos += static_cast<size_t>(block), ++bi) {
    const int k = static_cast<int>(std::min<size_t>(static_cast<size_t>(block), n - pos));
    if (off >= pos && off < pos + static_cast<size_t>(k))
      for (uint32_t i = 1; i < id; ++i) g->noteOff(i, static_cast<int>(off - pos));
    if (perBlock) perBlock(bi, st);
    g->process(st, aux, ctx, L.data(), R.data(), k);
    std::array<float, 8> o{};
    g->controlOutputs(o.data(), 8);
    r.out.push_back(o);
  }
  return r;
}

void testEnvController() {
  const PluginInfo* info = findPlugin("gl.envctl");
  expect(info && info->params.size() == static_cast<size_t>(md::EcCount) && info->outputs.size() == 8 && info->legacyModelParam == md::EcModel,
         "envctl: articulator model parameters, 8 outputs, legacy model switch");
  PluginState s = defaultPluginState("gl.envctl");
  expect(s.params[md::EcModel] == 1, "envctl: new instances use the articulator model");
  {
    PluginState old;
    old.type = "gl.envctl";
    old.params.assign(static_cast<size_t>(md::EcContinuous + 1), 0.0f);
    normalizePluginState(old);
    expect(old.params[md::EcModel] == 0, "envctl: pre-J-41 states keep the simple model");
  }
  // Default controller: output 1 = the ADSR (bipolar, base 0.5, env 1).
  CtlRun r = runCtl(s, {{60, 0.8}}, 2.0, 1.2);
  if (std::getenv("GL_J41_DEBUG"))
    for (size_t i = 0; i < 80; ++i) std::printf("# ctl %zu %.6f\n", i, static_cast<double>(r.out[i][0]));
  // 256-sample blocks: 172.27 blocks per second.
  const auto at = [&](double sec, int k = 0) { return r.out[std::min(r.out.size() - 1, static_cast<size_t>(sec * 44100 / 256))][static_cast<size_t>(k)]; };
  float peak = 0;
  for (size_t i = 55; i < 75; ++i) peak = std::max(peak, r.out[i][0]);
  expect(at(0.2) > 0.6 && peak > 0.985f && at(0.34) < peak, "envctl: attack to 1.0 in 0.375 s");
  expect(near(at(1.15), 0.5, 1e-3), "envctl: sustain 0.5");
  expect(at(1.4) < 1e-3, "envctl: release after the note");
  expect(near(at(0.5, 1), 0.5, 1e-6), "envctl: articulator 2 (envelope off) holds its base");
  // Block-size invariance.
  CtlRun r2 = runCtl(s, {{60, 0.8}}, 2.0, 1.2, 64);
  bool inv = true;
  for (size_t i = 0; i < r2.out.size(); i += 4) {
    const size_t j = i / 4;
    if (j < r.out.size() && (i + 3) < r2.out.size()) inv = inv && r2.out[i + 3][0] == r.out[j][0];
  }
  expect(inv, "envctl: outputs independent of the block size");
  // Key ranges: articulator 3 only on note 48.
  {
    PluginState k = s;
    md::EnvcModel m = md::defaultEnvc();
    m.arts[2].env.enabled = -1;
    m.arts[2].keyLo = m.arts[2].keyHi = 48;
    k.data = md::writeEnvc(m);
    CtlRun a = runCtl(k, {{60, 1.0}}, 0.5, 1.0), b = runCtl(k, {{48, 1.0}}, 0.5, 1.0);
    float bmax = 0;
    for (const auto& o : b.out) bmax = std::max(bmax, o[2]);
    expect(near(a.out.back()[2], 0.5, 1e-6) && bmax > 0.98f, "envctl: key range per articulator");
  }
  // Continuous output follows the global mod X through a mod X map.
  {
    PluginState c = s;
    c.params[md::EcContinuous] = 1;
    md::EnvcModel m = md::defaultEnvc();
    m.arts[0].maps[md::MapModX].pts[0].value = 0.0;
    m.arts[0].maps[md::MapModX].pts[1].value = 1.0;
    c.data = md::writeEnvc(m);
    c.params[md::EcModX] = 0.9f;
    CtlRun a = runCtl(c, {}, 0.2, 1.0);
    expect(near(a.out.back()[0], 0.9, 1e-6), "envctl: continuous output = base + mod X map at the global mod X");
  }
  // Unipolar product: envelope × velocity map.
  {
    PluginState p = s;
    p.params[md::artParam(0, md::AfBase)] = 0;
    md::EnvcModel m = md::defaultEnvc();
    m.arts[0].flags = 0;
    for (auto& mp : m.arts[0].maps) mp = flart::defaultMap(1.0);
    m.arts[0].maps[md::MapVel].pts[0].value = 0.0;  // velocity map 0 → 1
    p.data = md::writeEnvc(m);
    CtlRun a = runCtl(p, {{60, 0.5}}, 0.4, 1.0);
    float mx = 0;
    for (const auto& o : a.out) mx = std::max(mx, o[0]);
    if (std::getenv("GL_J41_DEBUG")) std::printf("# product max %.6f\n", static_cast<double>(mx));
    expect(near(mx, 0.5, 0.01), "envctl: product mode scales the envelope by the velocity map");
  }
  // LFO: free 1 Hz, amplitude envelope full → two cycles in 2 s.
  {
    PluginState l = s;
    l.params[md::artParam(0, md::AfEnvAmt)] = 0;
    l.params[md::artParam(0, md::AfLfoAmt)] = 1;
    l.params[md::artParam(0, md::AfLfoSpeed)] = 40898.0f / 65536.0f;
    md::EnvcModel m = md::defaultEnvc();
    m.arts[0].lfo.enabled = -1;
    m.arts[0].lfo.pts = {flart::Point{0, 1.0, 0, 0, 0, 0, 0}};
    m.arts[0].lfo.sustain = -1;
    l.data = md::writeEnvc(m);
    CtlRun a = runCtl(l, {{60, 1.0}}, 2.0, 3.0, 64);
    int up = 0;
    for (size_t i = 1; i < a.out.size(); ++i) up += (a.out[i - 1][0] < 0.5f && a.out[i][0] >= 0.5f);
    float mx = 0, mn = 1;
    for (const auto& o : a.out) mx = std::max(mx, o[0]), mn = std::min(mn, o[0]);
    expect(up == 2 && mx > 0.98f && mn < 0.02f, "envctl: LFO at 1 Hz, full depth");
  }
  // Note switch presets on the corpus play their articulator only.
  if (testfl::flInstalled()) {
    flp::PresetResult pr;
    const std::string f = testfl::flData() + "/Data/Patches/Plugin presets/Generators/Fruity Envelope Controller/Misc/Note switches (C2-G2 poly).fst";
    if (flp::importFlPreset(f, pr, testfl::flData())) {
      CtlRun a = runCtl(pr.state, {{25, 1.0}}, 0.3, 1.0);
      bool only = a.out.back()[1] > 0.5f;
      for (int k : {0, 2, 3, 4, 5, 6, 7}) only = only && a.out.back()[static_cast<size_t>(k)] < 0.5f;
      expect(only, "envctl: note switches preset (articulator 2 on C#2 only)");
    }
  }
}

// Corpus: every shipped state decodes exactly, encodes back natively and,
// where FL's records agree with the knob block, byte for byte.
void testEnvCorpus() {
  if (!testfl::flInstalled()) {
    std::printf("# FL Studio not installed: corpus checks skipped\n");
    return;
  }
  int n = 0, exact = 0, ok = 0, native = 0, bytes = 0, bytesTotal = 0;
  auto check = [&](const std::vector<uint8_t>& state, const PluginState& g, bool ex) {
    ++n;
    ok += !g.type.empty();
    exact += ex;
    flp::enc::EncodeContext c;
    flp::enc::Result r = flp::enc::encode(g, "", nullptr, c);
    native += r.quality == flp::enc::Result::Exact;
    md::EnvcState st;
    if (md::parseEnvcState(state.data(), state.size(), st) && st.recordsMatchKnobs) {
      ++bytesTotal;
      bytes += md::writeEnvcState(st) == state;
    }
  };
  for (const auto& f : testfl::presetFiles("Fruity Envelope Controller")) {
    flp::File file;
    if (!flp::readFile(f, file, nullptr)) continue;
    std::vector<uint8_t> state;
    for (const auto& e : file.events)
      if (e.id == 213) state.assign(e.data, e.data + e.size);
    flp::PresetResult pr;
    if (!flp::importFlPreset(f, pr, testfl::flData())) continue;
    check(state, pr.state, pr.exact);
  }
  const int presets = n;
  for (const auto& d : testfl::decodeProjects("Fruity Envelope Controller")) check({}, d.state, d.exact);
  std::printf("# envctl corpus: %d states, %d decoded, %d exact, %d native exact, bytes %d/%d\n", n, ok, exact, native, bytes, bytesTotal);
  expect(presets >= 22 && exact == n && native == n, "envctl corpus: every shipped state exact, native encode exact");
  expect(bytes == bytesTotal && bytesTotal >= 20, "envctl corpus: states rewrite byte for byte");
}

// --------------------------------------------- automation curve model switch
void testAutomationModel() {
  AutomationClip a;
  expect(a.flCurves, "automation: new clips use FL's shape laws");
  a.points = {{0, 0.0f, 0, AutoShape::Curve}, {96, 1.0f, -0.5f, AutoShape::Curve}};  // Gridline −0.5 = FL +0.5 (fast start)
  const double fl = automationValue(a, 48);
  expect(near(fl, flcurve::singleCurve(0.5, 0.5), 1e-6), "automation: FL single curve law (sign flipped)");
  AutomationClip old = a;
  old.flCurves = false;
  expect(near(automationValue(old, 48), autoShape(AutoShape::Curve, -0.5, 0.5), 1e-6) && !near(automationValue(old, 48), fl, 1e-3),
         "automation: legacy clips keep the Gridline curve");
  // Stairs differ between the models; smooth uses neighbour tangents.
  AutomationClip st = a;
  st.points[1].shape = AutoShape::Stairs, st.points[1].tension = 0;
  expect(near(automationValue(st, 48), flcurve::shape(3, 0, 0.5), 1e-6), "automation: FL stairs");
  AutomationClip sm;
  sm.points = {{0, 0, 0, AutoShape::Curve}, {96, 0.5f, 0, AutoShape::Smooth}, {192, 1, 0, AutoShape::Smooth}};
  // Equal slopes on both sides: unit tangent at the joint (0.5 + 0.5·0.625, 0.5·0.375).
  expect(near(automationValue(sm, 144), 0.8125, 1e-6) && near(automationValue(sm, 48), 0.1875, 1e-6), "automation: smooth joins neighbours");
  // Project files: the flag round-trips; files without it load the legacy curves.
  Song s;
  AutomationClip c = a;
  c.owner = kSongId, c.index = static_cast<uint32_t>(SongParam::Tempo);
  c.id = s.allocateId();
  s.automations.push_back(c);
  AutomationClip c2 = old;
  c2.owner = kSongId, c2.index = static_cast<uint32_t>(SongParam::Tempo);
  c2.id = s.allocateId();
  s.automations.push_back(c2);
  Song back;
  std::string err;
  const bool loaded = songFromJson(songToJson(s), back, &err);
  if (!loaded) std::printf("# %s\n", err.c_str());
  expect(loaded && back.automations.size() == 2 && back.automations[0].flCurves && !back.automations[1].flCurves,
         "automation: curve model saved per clip");
}


// ---------------------------------------------------------------- render helpers
struct GenOut {
  bool finite = true;
  float peak = 0;
  double rms = 0;
};
GenOut renderGen(const PluginState& s, const std::vector<std::pair<int, double>>& notes, double seconds, double offAt) {
  GenOut o;
  auto g = createGenerator(s.type);
  if (!g) return o;
  ChannelAux aux;
  prepareChannelAux(s, aux);
  ProcessContext ctx;
  ctx.sampleRate = 44100, ctx.tempo = 120, ctx.ppq = 96;
  const int block = 256;
  std::vector<float> L(block), R(block);
  const size_t n = static_cast<size_t>(seconds * 44100), off = static_cast<size_t>(offAt * 44100);
  uint32_t id = 1;
  for (auto [p, v] : notes) g->noteOn(id++, p, static_cast<float>(v), 0);
  double sum = 0;
  for (size_t pos = 0; pos < n; pos += block) {
    const int k = static_cast<int>(std::min<size_t>(block, n - pos));
    if (off >= pos && off < pos + static_cast<size_t>(k))
      for (uint32_t i = 1; i < id; ++i) g->noteOff(i, static_cast<int>(off - pos));
    std::fill(L.begin(), L.end(), 0.0f), std::fill(R.begin(), R.end(), 0.0f);
    g->process(s, aux, ctx, L.data(), R.data(), k);
    for (int i = 0; i < k; ++i) {
      o.finite = o.finite && std::isfinite(L[static_cast<size_t>(i)]) && std::isfinite(R[static_cast<size_t>(i)]);
      o.peak = std::max({o.peak, std::fabs(L[static_cast<size_t>(i)]), std::fabs(R[static_cast<size_t>(i)])});
      sum += 0.5 * (static_cast<double>(L[static_cast<size_t>(i)]) * L[static_cast<size_t>(i)] + static_cast<double>(R[static_cast<size_t>(i)]) * R[static_cast<size_t>(i)]);
    }
  }
  o.rms = n > 0 ? std::sqrt(sum / static_cast<double>(n)) : 0.0;
  return o;
}

// 16-bit mono WAV in the system temp directory.
std::string writeWav(const std::string& name, const std::vector<float>& x) {
  const std::string path = (fs::temp_directory_path() / name).string();
  std::ofstream f(path, std::ios::binary);
  auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
  auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
  const uint32_t data = static_cast<uint32_t>(x.size() * 2);
  f.write("RIFF", 4), u32(36 + data), f.write("WAVE", 4), f.write("fmt ", 4), u32(16), u16(1), u16(1), u32(44100), u32(88200), u16(2), u16(16);
  f.write("data", 4), u32(data);
  for (float v : x) u16(static_cast<uint16_t>(static_cast<int16_t>(std::lround(std::clamp(v, -1.0f, 1.0f) * 32767.0f))));
  return path;
}

int crossings(const std::vector<float>& x, size_t from, size_t to) {
  int c = 0;
  for (size_t i = from + 1; i < to && i < x.size(); ++i) c += (x[i - 1] < 0) != (x[i] < 0);
  return c;
}

// ------------------------------------------------------------ slice processing
void testSliceProcess() {
  namespace sp = sliceproc;
  expect(near(sp::ratioOf(0), 1) && near(sp::ratioOf(10000), 2, 1e-6) && near(sp::ratioOf(-10000), 0.5, 1e-6), "slicer: stretch ratio 2^(fit/10000)");
  {  // fades: raised cosine at 50 ms or less, else linear in / exponential (tension 10) out
    std::vector<float> a(1000, 1.0f);
    sp::fadeIn(a.data(), 1000, 100, 44100);
    expect(near(a[0], (std::cos((1.0 - 1.0 / 101) * M_PI) + 1) * 0.5, 1e-6) && near(a[99], (std::cos(M_PI / 101) + 1) * 0.5, 1e-6) && a[100] == 1.0f,
           "slicer: short fade in is a raised cosine over t = (i+1)/(n+1)");
    std::vector<float> b(10000, 1.0f);
    sp::fadeIn(b.data(), 10000, 4410, 44100);
    expect(near(b[0], 1.0 / 4411, 1e-6) && near(b[4409], 4410.0 / 4411, 1e-6), "slicer: long fade in is linear");
    std::vector<float> c(10000, 1.0f);
    sp::fadeOut(c.data(), 10000, 4410, 44100);
    expect(c[0] == 1.0f && near(c[5590], (std::pow(11.0, 4410.0 / 4411) - 1) / 10, 1e-5) && near(c[9999], (std::pow(11.0, 1.0 / 4411) - 1) / 10, 1e-6),
           "slicer: long fade out is exponential (tension 10)");
  }
  {  // declick: the last 28 ms fade to zero, earlier frames and trailing silence untouched
    std::vector<float> d(5000, 0.5f);
    for (size_t i = 4990; i < 5000; ++i) d[i] = 0;
    sp::declickEnd(d.data(), 5000, 44100);
    const int64_t n = std::llround(0.7f * 0.04f * 44100.0);
    expect(d[0] == 0.5f && d[static_cast<size_t>(4990 - n - 1)] == 0.5f && std::fabs(d[4989]) < 0.005f && d[4995] == 0.0f &&
               std::fabs(d[static_cast<size_t>(4990 - n)]) < 0.5f,
           "slicer: declick fades the slice end (DC-tracking + 1 ms cosine), skips trailing silence");
  }
  {  // stretch keeps the pitch, the pitch factor moves it
    std::vector<float> x(22050);
    for (size_t i = 0; i < x.size(); ++i) x[i] = static_cast<float>(0.5 * std::sin(2 * M_PI * 440.0 * static_cast<double>(i) / 44100));
    const float* in[1] = {x.data()};
    auto y = sp::stretch(in, 1, 22050, 33075, 1.0, 44100);
    const int c0 = crossings(x, 2000, 20000), c1 = crossings(y[0], 2000, 20000);
    double e = 0;
    for (size_t i = 2000; i < 31000; ++i) e += static_cast<double>(y[0][i]) * y[0][i];
    const double rms = std::sqrt(e / 29000);
    expect(y[0].size() == 33075 && std::abs(c1 - c0) <= 4 && rms > 0.3 && rms < 0.42, "slicer: time-stretch ×1.5 keeps pitch and level");
    auto z = sp::stretch(in, 1, 22050, 22050, 2.0, 44100);
    const int c2 = crossings(z[0], 2000, 20000);
    expect(z[0].size() == 22050 && std::abs(c2 - 2 * c0) <= 8, "slicer: pitch shift an octave up keeps the length");
  }
  {  // whole processing: reverse, stretch with the audio kept, starts scaled
    Sample in;
    in.sampleRate = 44100;
    in.left.resize(1000);
    for (size_t i = 0; i < 1000; ++i) in.left[i] = static_cast<float>(i) / 1000.0f;
    in.setMono();
    sp::Settings st;
    st.declick = false;
    std::vector<int64_t> outStarts;
    SamplePtr r = sp::process(in, {0, 500}, {0, 1}, st, &outStarts);
    expect(r->frames() == 1000 && r->left[500] == in.left[999] && r->left[999] == in.left[500] && r->left[10] == in.left[10] && outStarts.size() == 2,
           "slicer: reversed slice processed in place");
    st.fit = 10000, st.method = 0;
    r = sp::process(in, {0, 500}, {0, 0}, st, &outStarts);
    expect(r->frames() == 2000 && outStarts == std::vector<int64_t>({0, 1000}) && r->left[499] == in.left[499] && r->left[700] == 0.0f &&
               r->left[1000] == in.left[500],
           "slicer: methods 0/1 keep each slice's audio at its stretched start");
  }
  {  // gl.slicer engine 2: the voice plays the processed sample
    std::vector<float> x(44100);
    for (size_t i = 0; i < x.size(); ++i) x[i] = static_cast<float>(0.5 * std::sin(2 * M_PI * 220.0 * static_cast<double>(i) / 44100));
    PluginState s = defaultPluginState("gl.slicer");
    s.data = writeWav("gridline_j41_slicer.wav", x);
    s.params[pp::SlMode] = pp::SlModeManual;
    for (int k = 0; k < 4; ++k) s.params[pp::slMarker(k)] = 0.25f * static_cast<float>(k);
    s.params[pp::SlEngine] = pp::SlEngineProcessed;
    s.params[pp::SlProcFit] = 5000, s.params[pp::SlProcFadeOut] = 4;
    ChannelAux aux;
    prepareChannelAux(s, aux);
    const SamplePtr smp = aux.sample();
    expect(smp && smp->frames() == static_cast<size_t>(std::llround(44100 * sliceproc::ratioOf(5000))) && aux.slices.size() == 4 &&
               aux.slices[1] == std::llround(11025 * sliceproc::ratioOf(5000)),
           "slicer: processed sample prepared (stretched, slice starts scaled)");
    GenOut o = renderGen(s, {{61, 0.8}}, 0.5, 0.45);
    expect(o.finite && o.peak > 0.1f && o.peak < 1.0f, "slicer: processed slice plays");
  }
}

// --------------------------------------------------------------- Fruity Slicer
std::vector<uint8_t> fruitySlicerState(const std::string& path, const std::vector<int32_t>& starts, int32_t pitch, int32_t fit, int32_t fadeOut, uint8_t rev2) {
  std::vector<uint8_t> b;
  auto i32 = [&](int32_t v) {
    for (int k = 0; k < 4; ++k) b.push_back(static_cast<uint8_t>((static_cast<uint32_t>(v) >> (8 * k)) & 0xFF));
  };
  auto f32 = [&](float f) {
    int32_t v;
    std::memcpy(&v, &f, 4);
    i32(v);
  };
  i32(16), f32(4.0f), f32(120.0f), i32(pitch), i32(fit), i32(4), i32(0), i32(fadeOut);
  b.push_back(static_cast<uint8_t>(path.size()));
  b.insert(b.end(), path.begin(), path.end());
  i32(static_cast<int32_t>(starts.size()));
  for (size_t k = 0; k < starts.size(); ++k) {
    b.push_back(0);  // unnamed
    i32(starts[k]), i32(-1), f32(-1.0f);
    b.push_back(k == 2 ? rev2 : 0);
  }
  b.push_back(1), i32(48), b.push_back(0), i32(44100), b.push_back(1), b.push_back(1), b.push_back(1), i32(0);
  return b;
}

void testFruitySlicer() {
  std::vector<float> x(44100, 0.25f);
  const std::string wav = writeWav("gridline_j41_fslicer.wav", x);
  flp::enc::EncodeContext c;
  flp::PluginMapping m;
  bool ok = flp::enc::decodeState("Fruity Slicer", fruitySlicerState(wav, {0, 11025, 22050, 33075}, 0, 0, 4, 1), m, c);
  expect(ok && m.glType == "gl.slicer" && m.exact && m.state.params[pp::SlEngine] == pp::SlEngineProcessed && m.state.params[pp::SlProcFadeOut] == 4 &&
             m.state.params[pp::SlProcMethod] == 4 && m.state.params[pp::SlProcDeclick] == 1 && m.state.params[pp::slParam(2, pp::SliceReverse)] == 1 &&
             m.state.params[pp::SlBaseNote] == 48 && near(m.state.params[pp::slMarker(1)], 0.25, 1e-7),
         "Fruity Slicer: processing settings decode (unstretched = exact)");
  flp::PluginMapping s2;
  ok = flp::enc::decodeState("Fruity Slicer", fruitySlicerState(wav, {0, 22050}, -200, 766, 0, 0), s2, c);
  expect(ok && !s2.exact && s2.state.params[pp::SlProcFit] == 766 && s2.state.params[pp::SlProcPitch] == -200 && s2.state.params[pp::SlPitch] == 0,
         "Fruity Slicer: stretched / pitch-shifted states decode approximately (pitch applied offline)");
  if (testfl::flInstalled()) {
    auto proj = testfl::decodeProjects("Fruity Slicer");
    size_t okn = 0, exact = 0, played = 0;
    for (const auto& d : proj) {
      if (!d.ok || d.state.type != "gl.slicer") continue;
      ++okn;
      exact += d.exact;
      GenOut o = renderGen(d.state, {{60, 0.8}, {61, 0.8}}, 0.6, 0.5);
      played += o.finite && o.peak > 1e-3f && o.peak < 4.0f;
    }
    char buf[160];
    std::snprintf(buf, sizeof buf, "Fruity Slicer projects: %zu/%zu decode, %zu exact (unstretched), %zu play", okn, proj.size(), exact, played);
    expect(okn == proj.size() && exact >= 3 && played == okn, buf);
  }
}

// ----------------------------------------------------------------- Patcher v2
void testPatcherV2() {
  if (!testfl::flInstalled()) {
    expect(true, "FL Studio not installed: Patcher corpus skipped");
    return;
  }
  auto pres = testfl::decodePresets("Patcher");
  size_t v2 = 0, round = 0, exact = 0, ran = 0, tried = 0;
  for (const auto& d : pres) {
    if (!d.ok) continue;
    exact += d.exact;
    if (!md::isPatchV2(d.state.data)) continue;
    ++v2;
    md::PatchV2 g;
    if (md::parsePatchV2(d.state.data, g) && md::serializePatchV2(g) == d.state.data) ++round;
    else if (std::getenv("GL_J41_DEBUG")) {
      const std::string b = md::serializePatchV2(g);
      size_t k = 0;
      while (k < b.size() && k < d.state.data.size() && b[k] == d.state.data[k]) ++k;
      std::printf("# v2 text %s @%zu sizes %zu %zu tail [%s] / [%s]\n", d.where.c_str(), k, d.state.data.size(), b.size(), d.state.data.substr(k, 60).c_str(), b.substr(k, 60).c_str());
    }
    const md::PatchV2Topo t = md::patchV2Topology(g);
    if (d.state.type == "gl.patcher" && tried < 12 && t.unsupported.empty()) {
      ++tried;
      GenOut o = renderGen(d.state, {{60, 0.8}}, 0.4, 0.3);
      ran += o.finite;
    }
  }
  char buf[200];
  std::snprintf(buf, sizeof buf, "Patcher presets: %zu decode, %zu exact, %zu FL graphs (v2), %zu text round trips, %zu/%zu instrument patches run finite",
                pres.size(), exact, v2, round, ran, tried);
  expect(exact >= 115 && v2 > 0 && round == v2 && ran == tried, buf);
}

// ------------------------------------------------------------- Control Surface
void testSurface() {
  expect(md::surfaceControlCount("0 Drive\n2 Mix\n#fl 00ff\n") == 2, "surface: the kept FL surface line is not a control");
  if (!testfl::flInstalled()) return;
  auto pres = testfl::decodePresets("Control Surface");
  for (auto& d : testfl::decodeProjects("Control Surface")) pres.push_back(d);
  size_t ok = 0, exact = 0, kept = 0;
  for (const auto& d : pres) {
    if (!d.ok) continue;
    ++ok;
    exact += d.exact;
    kept += d.state.data.find("\n#fl ") != std::string::npos || d.state.data.rfind("#fl ", 0) == 0;
  }
  char buf[160];
  std::snprintf(buf, sizeof buf, "Control Surface presets / projects: %zu/%zu decode, %zu exact, %zu keep the FL surface", ok, pres.size(), exact, kept);
  expect(ok > 0 && ok == pres.size() && exact == ok && kept == ok, buf);
}

// --------------------------------------------------------------- levels
void testLevels() {
  if (!testfl::flInstalled()) {
    expect(true, "FL Studio not installed: level checks skipped");
    return;
  }
  // Autogun: power-normalised partials and the fixed-frequency filter keep
  // the shipped presets well above the old −40 LUFS.
  auto ag = testfl::decodeProjects("Autogun");
  size_t n = 0, audible = 0, sane = 0;
  for (const auto& d : ag) {
    if (!d.ok || d.state.type != "gl.alloy" || n >= 24) continue;
    ++n;
    GenOut o = renderGen(d.state, {{60, 0.8}}, 0.6, 0.5);
    audible += o.finite && o.rms > 0.005;
    sane += o.finite && o.peak < 8.0f;
  }
  char buf[160];
  std::snprintf(buf, sizeof buf, "Autogun presets: %zu/%zu audible (rms > −46 dBFS), %zu bounded", audible, n, sane);
  expect(n > 0 && audible >= n - n / 6 && sane == n, buf);
  // Sakura: the host's per-note level (0.7071·velocity at the centre) scales the voice.
  auto sk = testfl::decodePresets("Sakura");
  for (const auto& d : sk) {
    if (!d.ok || d.state.type != "gl.twinstring") continue;
    GenOut lo = renderGen(d.state, {{60, 0.4}}, 0.3, 0.25), hi = renderGen(d.state, {{60, 0.8}}, 0.3, 0.25);
    if (hi.peak < 1e-3f) continue;
    expect(lo.finite && hi.finite && lo.peak < hi.peak, "Sakura: the per-note host level follows the velocity");
    break;
  }
}
}  // namespace

int main() {
  testShapes();
  testEngine();
  testEnvController();
  testEnvCorpus();
  testAutomationModel();
  testSliceProcess();
  testFruitySlicer();
  testPatcherV2();
  testSurface();
  testLevels();
  return finish();
}
