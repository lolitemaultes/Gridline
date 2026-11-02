// Gridline — J-210: recorded events (FL's pattern events) play as FL plays them.
//
//  * The lane model: an events clip sends nothing before its first point in
//    each pass (the parameter keeps its value), its steps are found exactly
//    (plain, offset and looping clips), and the flag survives a project save.
//  * Engine: a Turntable whose Position is driven by a pattern of recorded
//    events receives every step on its exact sample (the first sample at or
//    after the event's tick), for any host block size. Proven against the
//    plugin run directly with the events applied on those samples (what FL's
//    own scratcher does with them, matched in J-206): the engine render is the
//    same up to the channel's gain. The events are those of FL's scratcher
//    render test, written here as plain numbers (tick steps and Position
//    values in the plugin's 0..30000 units).
//  * FLP import: the render test (when FL Studio is installed) imports its
//    recorded events as an events lane and renders the same way.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>
#include "../Check.h"
#include "core/Automation.h"
#include "core/AudioBuffer.h"
#include "core/Params.h"
#include "core/PatternAutomation.h"
#include "core/PluginInfo.h"
#include "core/Project.h"
#include "core/Song.h"
#include "core/WavWriter.h"
#include "core/flp/Flp.h"
#include "core/flp/FlpExport.h"
#include "engine/AudioEngine.h"
#include "engine/Generators.h"
#include "engine/Program.h"
#include "packs/Packs.h"
#include "packs/spectral/turntable.h"

using namespace gl;
namespace fs = std::filesystem;
namespace sp = gl::packs::spectral;

namespace {

constexpr int kSr = 44100;

// FL's scratcher render test: 259 recorded Position events over one 384-tick
// bar at 96 PPQ (tick steps from tick 0, then the values: a ramp up and down).
const char* kSteps =
    "12212121212121212121212121212121212121212121212121212121212121212121212121212121212121212121212121212121212121"
    "21212121212121212121212121212112121212121121212121212112121212121121212121212112121212121121212121212112121212"
    "121121212121212112121212121121212121211";
std::vector<AutoPoint> renderTestEvents() {
  std::vector<AutoPoint> pts;
  Tick t = 0;
  int v = 4;
  for (const char* c = kSteps; *c; ++c) {
    t += *c - '0';
    AutoPoint p;
    p.pos = t;
    p.value = static_cast<float>(v / 30000.0);
    p.shape = AutoShape::Hold;
    pts.push_back(p);
    v += pts.size() < 128 ? 1 : -1;
  }
  return pts;
}

// ---- lane model ----------------------------------------------------------------

std::shared_ptr<const AutomationClip> clipOf(std::vector<std::pair<Tick, float>> pts, bool events, Tick loop = 0) {
  AutomationClip c;
  c.owner = 5;
  c.index = 1;
  c.events = events;
  c.loop = loop;
  c.flCurves = false;
  for (auto [t, v] : pts) {
    AutoPoint p;
    p.pos = t;
    p.value = v;
    p.shape = AutoShape::Hold;
    c.points.push_back(p);
  }
  return std::make_shared<const AutomationClip>(c);
}

void testLaneModel() {
  AutomationLane l;
  l.owner = 5, l.index = 1;
  const auto ev = clipOf({{10, 0.2f}, {30, 0.6f}}, true);
  l.inst.push_back({100, 200, 0, ev});  // pass 1
  l.inst.push_back({200, 300, 0, ev});  // pass 2
  expect(l.hasEvents(), "an events clip makes an events lane");
  expect(!l.sendsAt(0) && !l.sendsAt(105) && !l.sendsAt(109.9), "nothing is sent before the first event of the first pass");
  expect(l.sendsAt(110) && std::fabs(l.valueAt(110) - 0.2) < 1e-6 && std::fabs(l.valueAt(129.9) - 0.2) < 1e-6, "the first event steps on its tick and holds");
  expect(l.sendsAt(130) && std::fabs(l.valueAt(130) - 0.6) < 1e-6 && l.sendsAt(199), "the next one steps on its tick");
  expect(!l.sendsAt(200) && !l.sendsAt(209), "the next pass sends nothing before its first event (the last value holds)");
  expect(l.sendsAt(210) && std::fabs(l.valueAt(210) - 0.2) < 1e-6, "and steps on it");
  expect(!l.sendsAt(300) && !l.sendsAt(1e6), "nothing after the last pass");
  expect(l.nextEventStep(0) == 110 && l.nextEventStep(109) == 110 && l.nextEventStep(110) == 130 && l.nextEventStep(130) == 210 &&
             l.nextEventStep(230) == std::numeric_limits<double>::infinity(),
         "steps: each event of each pass, none after the last");
  // A plain clip's lane: always sends (its first value before the first point).
  AutomationLane q;
  q.inst.push_back({0, 100, 0, clipOf({{10, 0.2f}}, false)});
  expect(!q.hasEvents() && q.sendsAt(0) && std::fabs(q.valueAt(0) - 0.2) < 1e-6 &&
             q.nextEventStep(0) == std::numeric_limits<double>::infinity(),
         "an ordinary clip sends everywhere and has no event steps");
  // Content offset and a looping events clip.
  AutomationLane o;
  o.inst.push_back({0, 100, 15, clipOf({{10, 0.2f}, {20, 0.4f}}, true, 25)});  // content 15.. loops every 25
  expect(o.sendsAt(0) && std::fabs(o.valueAt(0) - 0.2) < 1e-6, "offset: content past the first event sends its value");
  expect(o.nextEventStep(0) == 5 && o.nextEventStep(5) == 20 && o.nextEventStep(20) == 30, "offset + loop: steps at 5, 20 (next cycle), 30");
  expect(!o.sendsAt(10.5) && o.sendsAt(20), "a loop cycle sends nothing before its first event");
}

void testProjectFlag() {
  Song s = makeEmptySong();
  Channel c;
  c.id = s.allocateId();
  c.plugin = defaultPluginState("gl.turntable");
  c.mixerTrack = s.masterId();
  s.channels.push_back(c);
  Pattern p;
  p.id = s.allocateId();
  PatternAutomation lane = patternLane(c.id, channelPluginParam(c.id, sp::TtPosition).index, renderTestEvents());
  lane.events = true;
  p.automation.push_back(lane);
  s.patterns.push_back(p);
  Song back;
  std::string err;
  expect(songFromJson(songToJson(s), back, &err), "the project loads back");
  const Pattern* bp = back.pattern(p.id);
  expect(bp && bp->automation.size() == 1 && bp->automation[0].events && bp->automation[0] == lane, "an events lane stays one (saved and loaded)");
  s.pattern(p.id)->automation[0].events = false;
  Song back2;
  const Pattern* bp2 = songFromJson(songToJson(s), back2, &err) ? back2.pattern(p.id) : nullptr;
  expect(bp2 && bp2->automation.size() == 1 && !bp2->automation[0].events, "an ordinary lane stays ordinary");
}

// ---- engine --------------------------------------------------------------------

// A loop with some texture (any step of the platter shows in the sound).
std::string writeLoop(const fs::path& dir) {
  const size_t n = 44100;
  AudioBuffer b(2, n);
  uint32_t r = 12345;
  for (size_t i = 0; i < n; ++i) {
    r = r * 1664525u + 1013904223u;
    const double t = static_cast<double>(i) / kSr;
    const double v = 0.4 * std::sin(2 * 3.14159265358979 * 220 * t) + 0.2 * std::sin(2 * 3.14159265358979 * 1375 * t) + 0.1 * (static_cast<double>(r >> 8) / 8388608.0 - 1.0);
    b.plane(0)[i] = static_cast<float>(v);
    b.plane(1)[i] = static_cast<float>(-v * 0.8);
  }
  const fs::path f = dir / "loop.wav";
  writeWav(f.string(), b, kSr, 16);
  return f.string();
}

struct Fixture {
  Song s;
  ObjectId ch = kNoId, pat = kNoId;
  Fixture(const std::string& sample, const std::vector<AutoPoint>& events) {
    s = makeEmptySong();
    s.tempo = 140;
    s.ppq = 96;
    Channel c;
    c.id = s.allocateId();
    c.name = "Turntable";
    c.plugin = defaultPluginState("gl.turntable");
    c.plugin.params[sp::TtPosition] = 0.111f;
    c.plugin.params[sp::TtHold] = 1.0f;
    c.plugin.data = sample;
    c.mixerTrack = s.masterId();
    s.channels.push_back(c);
    ch = c.id;
    Pattern p;
    p.id = s.allocateId();
    p.bars = 1;
    PatternAutomation lane = patternLane(c.id, channelPluginParam(c.id, sp::TtPosition).index, events);
    lane.events = true;
    p.automation.push_back(lane);
    s.patterns.push_back(p);
    pat = p.id;
  }
};

// The pattern looped `passes` times through the engine (fed `block` frames per call;
// `fixed`: the offline renderer's fixed engine blocks).
std::vector<float> renderEngine(const Song& s, ObjectId pat, int64_t frames, int block, bool fixed) {
  ProgramBuilder builder;
  builder.setSampleRate(kSr);
  auto prog = builder.build(s);
  AudioEngine eng(kSr);
  eng.setProgram(prog);
  eng.setLooping(true);
  eng.setOffline(true);
  if (fixed) eng.setFixedBlock(AudioEngine::kOfflineBlock);
  eng.setMode(PlayMode::Pattern);
  eng.setPattern(pat);
  eng.play();
  std::vector<float> L(static_cast<size_t>(frames)), R(static_cast<size_t>(frames)), out;
  for (int64_t pos = 0; pos < frames; pos += block) {
    const int n = static_cast<int>(std::min<int64_t>(block, frames - pos));
    eng.process(L.data() + pos, R.data() + pos, n);
  }
  for (int64_t i = 0; i < frames; ++i) out.push_back(L[static_cast<size_t>(i)]), out.push_back(R[static_cast<size_t>(i)]);
  return out;
}

// The plugin alone: each event's value set on the first sample at or after its
// tick (pattern ticks, looping every `len`), the starting Position untouched.
std::vector<float> renderDirect(const PluginState& st0, const std::vector<AutoPoint>& ev, Tick len, int64_t frames) {
  PluginState st = st0;
  ChannelAux aux;
  packs::prepareAux(st, aux);
  // K-23: the 0.7.1 instrument (a channel) or the Turntable effect (a mixer slot, silent input).
  auto g = st.type == "gl.turntable" ? packs::createGenerator("gl.turntable") : nullptr;
  auto fx = g ? nullptr : packs::createEffect(st.type);
  ProcessContext ctx;
  ctx.sampleRate = kSr;
  const double spt = kSr * 60.0 / (140.0 * 96.0);
  std::vector<std::pair<int64_t, float>> at;
  for (int pass = 0; static_cast<double>(pass) * len * spt < static_cast<double>(frames); ++pass) {
    const int64_t base = static_cast<int64_t>(std::ceil(static_cast<double>(pass) * len * spt - 1e-9));
    for (const AutoPoint& p : ev)
      at.push_back({base + static_cast<int64_t>(std::ceil(static_cast<double>(p.pos) * spt - 1e-9)), p.value});
  }
  std::vector<float> L(512), R(512), out;
  size_t ei = 0;
  for (int64_t f0 = 0; f0 < frames;) {
    while (ei < at.size() && at[ei].first <= f0) st.params[sp::TtPosition] = at[ei++].second;
    int64_t n = std::min<int64_t>(512, frames - f0);
    if (ei < at.size() && at[ei].first < f0 + n) n = at[ei].first - f0;
    std::fill(L.begin(), L.end(), 0.f);
    std::fill(R.begin(), R.end(), 0.f);
    if (g) g->process(st, aux, ctx, L.data(), R.data(), static_cast<int>(n));
    else fx->processAux(st, aux, ctx, L.data(), R.data(), static_cast<int>(n));
    for (int64_t i = 0; i < n; ++i) out.push_back(L[static_cast<size_t>(i)]), out.push_back(R[static_cast<size_t>(i)]);
    f0 += n;
  }
  return out;
}

// Largest difference after fitting each side's gain (the channel's level and pan).
double fittedDiff(const std::vector<float>& a, const std::vector<float>& ref, double* corr = nullptr) {
  double worst = 0, c = 1;
  for (int side = 0; side < 2; ++side) {
    double ab = 0, bb = 0, aa = 0;
    for (size_t i = side; i < a.size() && i < ref.size(); i += 2) ab += a[i] * static_cast<double>(ref[i]), bb += static_cast<double>(ref[i]) * ref[i], aa += a[i] * static_cast<double>(a[i]);
    const double g = bb > 0 ? ab / bb : 0;
    c = std::min(c, aa > 0 && bb > 0 ? ab / std::sqrt(aa * bb) : 0);
    for (size_t i = side; i < a.size() && i < ref.size(); i += 2) worst = std::max(worst, std::fabs(a[i] - g * ref[i]));
  }
  if (corr) *corr = c;
  return worst;
}

double peak(const std::vector<float>& v) {
  double p = 0;
  for (float x : v) p = std::max(p, static_cast<double>(std::fabs(x)));
  return p;
}

void testEngineSteps(const std::string& sample) {
  const std::vector<AutoPoint> ev = renderTestEvents();
  expect(ev.size() == 259 && ev.front().pos == 1 && ev.back().pos == 383, "the render test's 259 events, ticks 1..383");
  Fixture f(sample, ev);
  const Tick len = f.s.barTicks();
  const int64_t frames = static_cast<int64_t>(std::ceil(2.05 * len * kSr * 60.0 / (140.0 * 96.0)));  // two passes and a bit
  const std::vector<float> ref = renderDirect(f.s.channels[0].plugin, ev, len, frames);
  expect(peak(ref) > 0.05, "the scratching sounds");
  for (int block : {512, 333, 64, 1}) {
    double corr = 0;
    const double d = fittedDiff(renderEngine(f.s, f.pat, frames, block, false), ref, &corr);
    std::printf("# engine (host blocks of %d) vs the plugin stepped on each event's sample: max diff %.3g, correlation %.6f\n", block, d, corr);
    expect(d < 1e-4, ("every event reaches the plugin on its sample (host blocks of " + std::to_string(block) + ")").c_str());
  }
  double corr = 0;
  const double d = fittedDiff(renderEngine(f.s, f.pat, frames, 512, true), ref, &corr);
  std::printf("# offline engine blocks: max diff %.3g, correlation %.6f\n", d, corr);
  expect(d < 1e-4, "the same with the offline renderer's fixed blocks");
  // Without the events flag the lane holds its first value from the start (an
  // ordinary clip): the start differs (the platter does not travel from 0.111).
  Fixture g(sample, ev);
  g.s.pattern(g.pat)->automation[0].events = false;
  expect(fittedDiff(renderEngine(g.s, g.pat, frames, 512, false), ref) > 1e-2, "an ordinary lane sets its first value from the start instead");
}

// FL's render test: the import -> render path (installed FL only).
void testRenderTestImport(const std::string& sample) {
  const std::string f = "/Applications/FL Studio 2026.app/Contents/Resources/FL/Data/System/Render tests/Fruity Scratcher.flp";
  std::error_code ec;
  if (!fs::exists(f, ec)) {
    std::printf("# FL render test not installed: import -> render skipped\n");
    return;
  }
  Song s;
  flp::Report rep;
  std::string err;
  expect(importFlp(f, s, rep, &err), "FL's render test imports");
  // K-23: the scratcher is a mixer effect: it lands on its insert's slot as the Turntable effect.
  auto deckOf = [](Song& song, ParamId& pos) -> PluginState* {
    for (MixerTrack& t : song.mixer)
      for (size_t k = 0; k < t.slots.size(); ++k)
        if (t.slots[k].plugin.type == "gl.turntablefx") {
          pos = slotParam(t.id, static_cast<int>(k), sp::TtPosition);
          return &t.slots[k].plugin;
        }
    return nullptr;
  };
  ParamId posId{};
  PluginState* tt = deckOf(s, posId);
  expect(tt != nullptr, "the scratcher lands on its mixer slot as the Turntable effect");
  const PatternAutomation* lane = nullptr;
  const Pattern* pat = nullptr;
  if (tt)
    for (const Pattern& p : s.patterns)
      if (const PatternAutomation* l = findPatternLane(p, posId.owner, posId.index)) lane = l, pat = &p;
  expect(lane && lane->events, "its recorded Position events import as an events lane");
  expect(s.playlist.empty(), "its song is empty (FL plays the pattern; Gridline opens it in pattern mode)");
  if (!lane || !tt) return;
  const std::vector<AutoPoint> want = renderTestEvents();
  bool same = lane->points.size() == want.size();
  for (size_t i = 0; same && i < want.size(); ++i) same = lane->points[i].pos == want[i].pos && std::fabs(lane->points[i].value - want[i].value) < 1e-7f;
  expect(same, "the events land on their ticks with their values");
  // FL export and back: the events are written as recorded events again and import as such.
  {
    flp::ExportOptions opt;
    flp::ExportReport xr;
    flp::Bytes bytes;
    expect(flp::exportFlp(s, bytes, xr, opt), "the project exports to FL");
    Song s2;
    flp::Report rep2;
    expect(importFlpBytes(bytes, s2, rep2, &err), "the exported project imports");
    const PatternAutomation* l2 = nullptr;
    ParamId pos2{};
    if (deckOf(s2, pos2))
      for (const Pattern& p : s2.patterns)
        if (const PatternAutomation* l = findPatternLane(p, pos2.owner, pos2.index)) l2 = l;
    bool back = l2 && l2->events && l2->points.size() == want.size();
    for (size_t i = 0; back && i < want.size(); ++i) back = l2->points[i].pos == want[i].pos && std::fabs(l2->points[i].value - want[i].value) < 1e-7f;
    expect(back, "FL round trip: the recorded events come back as events, on the same ticks with the same values");
  }
  // The plugin's loop is swapped for the test sound (the comparison needs no FL audio).
  tt->data = sample;
  const Tick len = s.patternContentLength(*pat);
  expect(len == 384, "one bar per pass");
  const int64_t frames = static_cast<int64_t>(std::ceil(2.05 * static_cast<double>(len) * kSr * 60.0 / (s.tempo * s.ppq)));
  expect(std::fabs(s.tempo - 140.0) < 1e-9 && s.ppq == 96, "140 BPM at 96 PPQ");
  const std::vector<float> ref = renderDirect(*tt, lane->points, len, frames);
  double corr = 0;
  const double d = fittedDiff(renderEngine(s, pat->id, frames, 512, true), ref, &corr);
  std::printf("# imported render test vs the plugin stepped on each event's sample: max diff %.3g, correlation %.6f\n", d, corr);
  expect(d < 1e-4, "the imported project plays its recorded scratching on the events' samples");
}

}  // namespace

int main() {
  testLaneModel();
  testProjectFlag();
  const fs::path dir = fs::temp_directory_path() / ("gridline_j210_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::error_code ec;
  fs::create_directories(dir, ec);
  const std::string sample = writeLoop(dir);
  testEngineSteps(sample);
  testRenderTestImport(sample);
  fs::remove_all(dir, ec);
  return finish();
}
