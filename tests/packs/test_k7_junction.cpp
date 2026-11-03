// Gridline - K-7: Junction graphs wired by hand. Routing renders (chains, parallel, feedback,
// notes through note processors, sidechain, controllers), save / load, older states, undo per
// edit, no allocation on the audio path, block-size invariance, FL patches.
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <new>
#include <string>
#include <vector>
#include "../Check.h"
#include "fl_corpus.h"
#include "../../src/core/flp/FlpExport.h"
#include "../../src/core/Document.h"
#include "../../src/core/Json.h"
#include "../../src/core/PluginParams.h"
#include "../../src/core/Project.h"
#include "../../src/engine/AudioEngine.h"
#include "../../src/engine/Effects.h"
#include "../../src/engine/Render.h"
#include "../../src/packs/Packs.h"
#include "../../src/packs/modulation/junction.h"
#include "../../src/packs/modulation/modulation.h"
#include "../../src/packs/modulation/modulation_dsp.h"

namespace {
thread_local bool gCount = false;
std::atomic<long> gAllocs{0};
}  // namespace
void* operator new(std::size_t n) {
  if (gCount) gAllocs.fetch_add(1, std::memory_order_relaxed);
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) {
  if (gCount) gAllocs.fetch_add(1, std::memory_order_relaxed);
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"  // the replaced operator new is malloc
#endif
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

using namespace gl;
namespace md = gl::packs::modulation;

namespace {

constexpr double kSr = 44100.0;

md::JLink audio(uint32_t s, uint32_t d, float gain = 1, int dp = 0, int sp = 0) {
  md::JLink l;
  l.src = s, l.dst = d, l.gain = gain, l.dp = dp, l.sp = sp;
  return l;
}
md::JLink notes(uint32_t s, uint32_t d, int sp = 0) {
  md::JLink l;
  l.kind = md::JKind::Notes;
  l.src = s, l.dst = d, l.sp = sp;
  return l;
}
md::JLink param(uint32_t s, int sp, uint32_t d, int dp, float lo = 0, float hi = 1) {
  md::JLink l;
  l.kind = md::JKind::Param;
  l.src = s, l.sp = sp, l.dst = d, l.dp = dp, l.lo = lo, l.hi = hi;
  return l;
}

// An effect that delays by a fixed number of frames and reports it.
class TestDelay final : public Effect {
 public:
  void process(const PluginState&, const ProcessContext&, float* L, float* R, int n) override {
    for (int k = 0; k < n; ++k) {
      const float a = l_[static_cast<size_t>(p_)], b = r_[static_cast<size_t>(p_)];
      l_[static_cast<size_t>(p_)] = L[k], r_[static_cast<size_t>(p_)] = R[k];
      L[k] = a, R[k] = b;
      if (++p_ == kLat) p_ = 0;
    }
  }
  void reset() override {}
  int latencyFor(const PluginState&, double) const override { return kLat; }
  static constexpr int kLat = 100;

 private:
  std::vector<float> l_ = std::vector<float>(kLat), r_ = std::vector<float>(kLat);
  int p_ = 0;
};
std::unique_ptr<Effect> testFactory(const std::string& type) {
  if (type == "gl.utility") return std::make_unique<TestDelay>();
  return nullptr;
}

// A prepared host effect fed with an input, in blocks.
struct FxRun {
  std::unique_ptr<Effect> fx;
  ChannelAux aux;
  PluginState st;
  ProcessContext ctx;
  explicit FxRun(const PluginState& s) : st(s) {
    fx = gl::createEffect(st.type);
    fx->prepareHost(st, kSr);
    packs::prepareAux(st, aux);
    ctx.sampleRate = kSr;
  }
  void run(std::vector<float>& L, std::vector<float>& R, int block) {
    for (size_t at = 0; at < L.size(); at += static_cast<size_t>(block)) {
      const int n = static_cast<int>(std::min(L.size() - at, static_cast<size_t>(block)));
      fx->processAux(st, aux, ctx, L.data() + at, R.data() + at, n);
    }
  }
};

// What a Program build does to a running host, the audio path staying on the previous data.
void buildStep(FxRun& r, const PluginState& st) {
  ChannelAux next;
  r.fx->prepareHost(st, kSr);
  packs::prepareAux(st, next);
  r.st = st;
  r.aux = next;
}

PluginState fxHost(const md::JGraph& g) {
  PluginState s = defaultPluginState("gl.patcherfx");
  s.data = md::serializeJunction(g);
  return s;
}
PluginState instHost(const md::JGraph& g) {
  PluginState s = defaultPluginState("gl.patcher");
  s.data = md::serializeJunction(g);
  return s;
}

std::vector<float> impulse(size_t n, size_t at = 0) {
  std::vector<float> v(n, 0.f);
  v[at] = 1.f;
  return v;
}

Document instrumentDoc(const PluginState& host) {
  Document d(makeEmptySong());
  d.setParam(songParam(SongParam::Tempo), 120.0, false);
  d.setPatternLength(d.song().patterns[0].id, 1, 16);
  d.addClip(d.song().patterns[0].id, 0, 0);
  ObjectId ch = d.addChannel("junction", host.type);
  d.editChannel(ch, "set", [&](Channel& c) { c.plugin = host; });
  d.addNote(d.song().patterns[0].id, ch, {0, 192, 60, 100});
  d.addNote(d.song().patterns[0].id, ch, {96, 96, 67, 90});
  return d;
}

AudioBuffer render(const Document& d, int block = 512) {
  RenderOptions o;
  o.tail = 96;
  o.blockSize = block;
  return renderSong(d.song(), o);
}

bool finite(const std::vector<float>& v) {
  for (float x : v)
    if (!std::isfinite(x)) return false;
  return true;
}
double diff(const std::vector<float>& a, const std::vector<float>& b) {
  if (a.size() != b.size()) return 1e30;
  double m = 0;
  for (size_t k = 0; k < a.size(); ++k) m = std::max(m, std::fabs(static_cast<double>(a[k]) - static_cast<double>(b[k])));
  return m;
}

void testModel() {
  md::JGraph g;
  const uint32_t a = md::jAddNode(g, defaultPluginState("gl.filter"), 10, 20);
  const uint32_t b = md::jAddNode(g, defaultPluginState("gl.delay"), 30, 40);
  g.node(a)->name = "Tone";
  g.node(a)->color = 3;
  g.node(b)->mode = md::JMode::Bypass;
  g.node(b)->mix = 0.25f;
  expect(md::jConnect(g, audio(md::kJInputs, a), true), "connect input to a module");
  expect(md::jConnect(g, audio(a, b, 0.5f), true), "connect module to module");
  std::string why;
  expect(!md::jConnect(g, audio(a, b, 0.5f), true, &why) && !why.empty(), "a repeated cable is refused");
  expect(!md::jConnect(g, audio(md::kJOutputs, a), true), "the outputs do not send");
  expect(!md::jConnect(g, notes(md::kJInputs, a), true), "the effect version has no notes input");
  expect(!md::jConnect(g, audio(md::kJInputs, a, 1, 0, 0), false), "the instrument version has no audio input");
  g.links.push_back(param(md::kJInputs, 2, a, pp::FltCutoff, 0.1f, 0.9f));
  g.controls.push_back({2, "Bright"});
  g.snaps.push_back({"Open", {{2, 0.75f}}});
  const std::string text = md::serializeJunction(g);
  md::JGraph back;
  expect(md::isJunction3(text) && md::parseJunction(text, back) && back == g, "the graph survives its text");
  expect(md::serializeJunction(back) == text, "the text is stable");
  // Garbage in the text never gets in.
  md::JGraph bad;
  expect(md::parseJunction("{\"v\":3,\"nodes\":[{\"id\":3,\"type\":\"gl.filter\"},{\"id\":20,\"type\":\"gl.filter\"},{\"id\":20,\"type\":\"gl.delay\"}],\"links\":[[0,99,0,20,0],[9,1,0,20,0]]}", bad) &&
             bad.nodes.size() == 1 && bad.links.empty(),
         "reserved ids, repeated ids and cables to nowhere are dropped");
  // Note loops are refused, audio loops are feedback.
  md::JGraph n;
  const uint32_t x = md::jAddNode(n, defaultPluginState("gl.arp"), 0, 0), y = md::jAddNode(n, defaultPluginState("gl.chord"), 0, 0);
  expect(md::jConnect(n, notes(x, y), false) && !md::jConnect(n, notes(y, x), false), "a note loop is refused");
  md::JGraph f;
  const uint32_t p = md::jAddNode(f, defaultPluginState("gl.filter"), 0, 0), q = md::jAddNode(f, defaultPluginState("gl.delay"), 0, 0);
  md::jConnect(f, audio(p, q), true);
  md::jConnect(f, audio(q, p), true);
  std::vector<char> fb = md::jFeedbackLinks(f);
  expect(fb.size() == 2 && !fb[0] && fb[1], "the cable closing a loop is the feedback cable");
  // Removing a module removes its cables.
  md::jRemoveNode(f, p);
  expect(f.links.empty() && f.nodes.size() == 1, "removing a module removes its cables");
  // Param input takes one source.
  md::JGraph c;
  const uint32_t t = md::jAddNode(c, defaultPluginState("gl.filter"), 0, 0);
  md::jConnect(c, param(md::kJInputs, 0, t, pp::FltCutoff), true);
  md::jConnect(c, param(md::kJInputs, 1, t, pp::FltCutoff), true);
  expect(c.links.size() == 1 && c.links[0].sp == 1, "a new source replaces the old one on a parameter");
  const int hp = md::jExpose(c, t, pp::FltReso, "Bite");
  expect(hp == md::PhMap0 && c.controls.size() == 1 && c.controls[0].name == "Bite" && md::jExpose(c, t, pp::FltReso, "Bite") == hp, "exposing a parameter takes one slot");
  md::jRemoveControl(c, hp);
  expect(c.controls.empty() && c.links.size() == 1, "removing a control removes what it drives");
  md::jAutoArrange(g);
  expect(g.node(b)->x > g.node(a)->x, "auto-arrange goes in signal order");
}

void testChains() {
  // Input through a filter and a delay: same as running them one after the other.
  md::JGraph g;
  const uint32_t a = md::jAddNode(g, defaultPluginState("gl.filter"), 0, 0), b = md::jAddNode(g, defaultPluginState("gl.delay"), 0, 0);
  g.links = {audio(md::kJInputs, a), audio(a, b), audio(b, md::kJOutputs)};
  std::vector<float> L = impulse(4096), R = impulse(4096);
  FxRun h(fxHost(g));
  h.run(L, R, 512);
  std::vector<float> L2 = impulse(4096), R2 = impulse(4096);
  auto f1 = gl::createEffect("gl.filter"), f2 = gl::createEffect("gl.delay");
  PluginState s1 = defaultPluginState("gl.filter"), s2 = defaultPluginState("gl.delay");
  ChannelAux none;
  ProcessContext ctx;
  ctx.sampleRate = kSr;
  for (size_t at = 0; at < L2.size(); at += 256) {
    const int n = static_cast<int>(std::min<size_t>(256, L2.size() - at));
    f1->processAux(s1, none, ctx, L2.data() + at, R2.data() + at, n);
    f2->processAux(s2, none, ctx, L2.data() + at, R2.data() + at, n);
  }
  expect(diff(L, L2) < 1e-6 && diff(R, R2) < 1e-6, "a chain sounds like its plugins in series");

  // Parallel paths add up with their cable gains.
  md::JGraph p;
  const uint32_t d = md::jAddNode(p, defaultPluginState("gl.utility"), 0, 0);
  (void)d;
  p.links = {audio(md::kJInputs, md::kJOutputs, 0.5f), audio(md::kJInputs, md::kJOutputs, 0.25f, 0, 0)};
  // (the same pair twice is refused by the editor; the runtime just adds them)
  std::vector<float> L3(64, 1.f), R3(64, 1.f);
  FxRun h3(fxHost(p));
  h3.run(L3, R3, 64);
  expect(std::fabs(L3[10] - 0.75f) < 1e-6f && std::fabs(R3[10] - 0.75f) < 1e-6f, "parallel cables add up with their gains");

  // Not wired: silent. Wired straight through: the input.
  md::JGraph e;
  std::vector<float> L4(64, 1.f), R4(64, 1.f);
  FxRun h4(fxHost(e));
  h4.run(L4, R4, 64);
  expect(L4[10] == 0.f, "nothing wired, nothing out");

  // Bypass passes through, mute is silent, mix blends.
  md::JGraph m;
  const uint32_t q = md::jAddNode(m, defaultPluginState("gl.filter"), 0, 0);
  m.links = {audio(md::kJInputs, q), audio(q, md::kJOutputs)};
  m.node(q)->mode = md::JMode::Bypass;
  std::vector<float> L5(64, 0.5f), R5(64, 0.5f);
  FxRun h5(fxHost(m));
  h5.run(L5, R5, 64);
  expect(std::fabs(L5[10] - 0.5f) < 1e-6f, "bypass passes the input through");
  m.node(q)->mode = md::JMode::Mute;
  std::vector<float> L6(64, 0.5f), R6(64, 0.5f);
  FxRun h6(fxHost(m));
  h6.run(L6, R6, 64);
  expect(L6[10] == 0.f, "mute is silent");
}

void testFeedback() {
  // A feedback cable delays by exactly kJFeedbackFrames, whatever the block size.
  md::JGraph g;
  const uint32_t a = md::jAddNode(g, defaultPluginState("gl.gate"), 0, 0), b = md::jAddNode(g, defaultPluginState("gl.gate"), 0, 0);
  g.node(a)->mode = md::JMode::Bypass;
  g.node(b)->mode = md::JMode::Bypass;
  g.links = {audio(md::kJInputs, a), audio(a, b), audio(b, a, 0.5f), audio(b, md::kJOutputs)};
  for (int block : {64, 100, 256, 512}) {
    std::vector<float> L = impulse(2048, 10), R = impulse(2048, 10);
    FxRun h(fxHost(g));
    h.run(L, R, block);
    char name[80];
    std::snprintf(name, sizeof name, "feedback cable: %d-frame echoes at half level (block %d)", md::kJFeedbackFrames, block);
    expect(L[10] == 1.f && std::fabs(L[10 + md::kJFeedbackFrames] - 0.5f) < 1e-6f && std::fabs(L[10 + 2 * md::kJFeedbackFrames] - 0.25f) < 1e-6f && L[11] == 0.f, name);
  }
  // Runaway feedback stays finite.
  md::JGraph r;
  const uint32_t c = md::jAddNode(r, defaultPluginState("gl.gate"), 0, 0);
  r.node(c)->mode = md::JMode::Bypass;
  r.links = {audio(md::kJInputs, c), audio(c, c, 16.0f), audio(c, md::kJOutputs)};
  std::vector<float> L(1 << 16, 1.f), R(1 << 16, 1.f);
  FxRun h(fxHost(r));
  h.run(L, R, 512);
  expect(finite(L) && finite(R), "runaway feedback stays finite");
}

void testLatency() {
  setTestEffectFactory(testFactory);
  md::JGraph g;
  const uint32_t a = md::jAddNode(g, defaultPluginState("gl.utility"), 0, 0);
  g.links = {audio(md::kJInputs, a), audio(a, md::kJOutputs), audio(md::kJInputs, md::kJOutputs, 0.5f)};
  PluginState st = fxHost(g);
  std::vector<float> L = impulse(1024, 5), R = impulse(1024, 5);
  FxRun h(st);
  h.run(L, R, 128);
  expect(L[5] == 0.f && std::fabs(L[5 + TestDelay::kLat] - 1.5f) < 1e-6f, "parallel paths stay aligned over a latent module");
  expect(h.fx->latencyFor(st, kSr) == TestDelay::kLat, "the host reports the latency of its longest path");
  setTestEffectFactory(nullptr);
}

// ---------------------------------------------------------------- FL patches and latency
// What FL's own Patcher library does, measured on it as a local oracle (K-AC, impulse through a
// latent effect beside a direct cable; the wiring below is the same):
//   every module's inputs are lined up by the latency each plugin reports, at any block size;
//   the patch reports its longest path to the host; a module's level scales its output and
//   leaves its latency alone.
std::vector<uint8_t> flBytes(std::initializer_list<uint64_t> u64s, float gain = -1) {
  std::vector<uint8_t> b;
  for (uint64_t v : u64s)
    for (int k = 0; k < 8; ++k) b.push_back(static_cast<uint8_t>(v >> (8 * k)));
  if (gain >= 0) {
    uint32_t g = 0;
    std::memcpy(&g, &gain, 4);
    for (int k = 0; k < 4; ++k) b.push_back(static_cast<uint8_t>(g >> (8 * k)));
    b.resize(b.size() + 12, 0);
  }
  return b;
}
std::vector<uint8_t> flJoin(std::initializer_list<std::vector<uint8_t>> parts) {
  std::vector<uint8_t> b;
  for (const auto& p : parts) b.insert(b.end(), p.begin(), p.end());
  return b;
}
std::vector<uint8_t> flU32s(std::initializer_list<uint32_t> vs) {
  std::vector<uint8_t> b;
  for (uint32_t v : vs)
    for (int k = 0; k < 4; ++k) b.push_back(static_cast<uint8_t>(v >> (8 * k)));
  return b;
}

constexpr uint64_t kFlIn = 1000, kFlOut = 1001;
// Port of module k.
uint64_t flPortIn(int k) { return 2000 + 2 * static_cast<uint64_t>(k); }
uint64_t flPortOut(int k) { return 2001 + 2 * static_cast<uint64_t>(k); }

// An FL patch as import stores it: `plugins` are the modules, `level` FL's level of each, `wires`
// (source port, destination port).
md::PatchV2 flPatch(const std::vector<std::string>& plugins, const std::vector<float>& level, const std::vector<std::pair<uint64_t, uint64_t>>& wires, bool align) {
  md::PatchV2 g;
  g.align = align;
  for (size_t k = 0; k < plugins.size(); ++k) {
    md::PatchV2Node n;
    n.plugin = defaultPluginState(plugins[k]);
    n.flName = plugins[k];
    n.tail = "-";
    const auto port = [](uint64_t id, uint32_t kind) { return md::patchChunkBytes(5009, flJoin({flU32s({0, 0}), flBytes({id}), flU32s({kind, 0, 0})})); };
    float lv = level[k];
    uint32_t lvBits = 0;
    std::memcpy(&lvBits, &lv, 4);
    n.head = md::patchHex(flJoin({md::patchChunkBytes(5007, flU32s({lvBits, 0, 0, 0})), port(flPortIn(static_cast<int>(k)), 3), port(flPortOut(static_cast<int>(k)), 2)}));
    g.items.push_back("#" + std::to_string(k));
    g.nodes.push_back(std::move(n));
  }
  const auto io = [&](uint64_t id, uint32_t kind, const char* name) {
    std::vector<uint8_t> nm(name, name + std::strlen(name));
    g.items.push_back(md::patchHex(md::patchChunkBytes(0, flJoin({md::patchChunkBytes(1400, flJoin({flBytes({id}), flU32s({0, kind, 2, 0, 0})})), md::patchChunkBytes(1402, nm)}))));
  };
  io(kFlIn, 7, "Audio input");
  io(kFlOut, 6, "Audio output");
  for (const auto& [s, d] : wires) g.items.push_back(md::patchHex(md::patchChunkBytes(0, md::patchChunkBytes(1300, flBytes({d, s}, 1.0f)))));
  return g;
}
PluginState flHost(const md::PatchV2& g) {
  PluginState s = defaultPluginState("gl.patcherfx");
  s.data = md::serializePatchV2(g);
  return s;
}
// Where an impulse at frame 64 lands, by block size.
std::vector<float> flRun(const PluginState& st, int block = 128) {
  std::vector<float> L = impulse(2048, 64), R = impulse(2048, 64);
  FxRun h(st);
  h.run(L, R, block);
  return L;
}

void testFlLatency() {
  setTestEffectFactory(testFactory);  // gl.utility: 100 samples of latency, reported
  const int lat = TestDelay::kLat;
  const auto near = [](float a, float b) { return std::fabs(a - b) < 1e-5f; };
  const std::string D = "gl.utility";
  // A latent module beside a direct cable.
  const std::vector<std::pair<uint64_t, uint64_t>> parallel = {{kFlIn, flPortIn(0)}, {flPortOut(0), kFlOut}, {kFlIn, kFlOut}};
  {
    const md::PatchV2 g = flPatch({D}, {1}, parallel, true);
    const PluginState st = flHost(g);
    expect(md::patchV2RunsAsGraph(g, kSr), "FL patch: a latent module takes the graph runtime");
    for (int block : {64, 128, 256, 1000}) {
      const std::vector<float> L = flRun(st, block);
      const std::string what = "FL patch: the direct cable lines up with the latent module, as FL (block " + std::to_string(block) + ")";
      expect(near(L[64], 0) && near(L[static_cast<size_t>(64 + lat)], 2), what.c_str());
    }
    FxRun h(st);
    expect(h.fx->latencyFor(st, kSr) == lat, "FL patch: the host is told the longest path, as FL");
    md::PatchV2 again;
    expect(md::parsePatchV2(st.data, again) && again.align && !again.run3 && again == g, "FL patch: the marker survives save and load");
  }
  {
    // A project from before: same wiring, no marker, plays as it did and stays byte for byte.
    const md::PatchV2 g = flPatch({D}, {1}, parallel, false);
    const PluginState st = flHost(g);
    expect(!md::patchV2RunsAsGraph(g, kSr) && st.data.find("align") == std::string::npos, "older FL patch: stays in its runtime, its data unchanged");
    const std::vector<float> L = flRun(st);
    expect(near(L[64], 1) && near(L[static_cast<size_t>(64 + lat)], 1), "older FL patch: keeps its sound, the paths unaligned");
    FxRun h(st);
    expect(h.fx->latencyFor(st, kSr) == 0, "older FL patch: still reports no latency");
    md::PatchV2 r3 = g;
    r3.run3 = true;
    FxRun h3(flHost(r3));
    expect(h3.fx->latencyFor(h3.st, kSr) == 0, "older FL patch in the graph runtime: still reports no latency");
  }
  {
    // Into a module in the middle, and two chained latencies beside a direct cable.
    const std::vector<float> mid = flRun(flHost(flPatch({D, "gl.filter"}, {1, 1}, {{kFlIn, flPortIn(0)}, {flPortOut(0), flPortIn(1)}, {kFlIn, flPortIn(1)}, {flPortOut(1), kFlOut}}, true)));
    float early = 0;
    for (int k = 0; k < 64 + lat; ++k) early = std::max(early, std::fabs(mid[static_cast<size_t>(k)]));
    expect(early < 1e-6f, "FL patch: a module's inputs line up too, not only the outputs");
    const std::vector<float> chain = flRun(flHost(flPatch({D, D}, {1, 1}, {{kFlIn, flPortIn(0)}, {flPortOut(0), flPortIn(1)}, {flPortOut(1), kFlOut}, {kFlIn, kFlOut}}, true)));
    expect(near(chain[static_cast<size_t>(64 + 2 * lat)], 2) && near(chain[static_cast<size_t>(64 + lat)], 0), "FL patch: a chain of two lines up with the direct cable");
    const std::vector<float> two = flRun(flHost(flPatch({D, D}, {1, 1}, {{kFlIn, flPortIn(0)}, {flPortOut(0), kFlOut}, {kFlIn, flPortIn(1)}, {flPortOut(1), kFlOut}}, true)));
    expect(near(two[static_cast<size_t>(64 + lat)], 2), "FL patch: two even paths stay together");
  }
  {
    // FL's module level: 0.7 alone gives 0.7 at the latency and no dry signal; 0.3 and 0 beside a
    // direct cable give 1.3 and 1, the latency staying.
    const std::vector<float> alone = flRun(flHost(flPatch({D}, {0.7f}, {{kFlIn, flPortIn(0)}, {flPortOut(0), kFlOut}}, true)));
    expect(near(alone[64], 0) && near(alone[static_cast<size_t>(64 + lat)], 0.7f), "FL patch: a module's level scales its output, no dry part");
    const std::vector<float> low = flRun(flHost(flPatch({D}, {0.3f}, parallel, true)));
    expect(near(low[64], 0) && near(low[static_cast<size_t>(64 + lat)], 1.3f), "FL patch: a low level keeps the latency");
    const std::vector<float> off = flRun(flHost(flPatch({D}, {0}, parallel, true)));
    expect(near(off[64], 0) && near(off[static_cast<size_t>(64 + lat)], 1), "FL patch: level 0 keeps the latency");
    const std::vector<float> flat = flRun(flHost(flPatch({"gl.filter"}, {0.5f}, {{kFlIn, flPortIn(0)}, {flPortOut(0), kFlOut}}, true)));
    float peak = 0;
    for (float x : flat) peak = std::max(peak, std::fabs(x));
    expect(peak > 0 && peak < 0.75f && md::jV2HasLevels(flPatch({"gl.filter"}, {0.5f}, {}, true)), "FL patch: a level with no latency runs too");
    // From before: the old reading of that value stays.
    md::PatchV2 old = flPatch({D}, {0.7f}, {{kFlIn, flPortIn(0)}, {flPortOut(0), kFlOut}}, false);
    old.run3 = true;
    const md::JGraph og = md::junctionFromV2(old);
    expect(og.nodes.size() == 1 && std::fabs(og.nodes[0].mix - 0.7f) < 1e-6f, "older FL patch in the graph runtime: its mix stays dry / wet");
  }
  {
    // Nothing latent: no reason to leave the runtime it always ran in.
    const md::PatchV2 g = flPatch({"gl.filter"}, {1}, {{kFlIn, flPortIn(0)}, {flPortOut(0), kFlOut}, {kFlIn, kFlOut}}, true);
    expect(!md::patchV2RunsAsGraph(g, kSr), "FL patch: no latency inside, the version 2 runtime stays");
    FxRun h(flHost(g));
    expect(h.fx->latencyFor(h.st, kSr) == 0, "FL patch: no latency inside, none reported");
  }
  {
    // A value edit on an imported patch running as a graph reaches it without a rebuild.
    PluginState st = flHost(flPatch({D}, {1}, parallel, true));
    FxRun r(st);
    md::JRuntime* ru = md::junctionRuntime(r.fx.get());
    expect(ru && ru->graphCounts().builds == 1, "FL patch: prepared once");
    std::vector<float> L(512, 0.f), R(512, 0.f);
    r.run(L, R, 128);
    PluginState inner = defaultPluginState(D);
    if (!inner.params.empty()) inner.params[0] = inner.params[0] == 0 ? 0.5f : 0.f;
    expect(md::junctionSetNodeState(st, md::kJFirstId, inner), "FL patch: module state set");
    buildStep(r, st);
    if (ru) expect(ru->graphCounts().builds == 1 && ru->graphCounts().valueEdits == 1, "FL patch: a value edit keeps the running graph");
  }
  setTestEffectFactory(nullptr);
}

void testNotes() {
  md::JGraph g;
  const uint32_t a = md::jAddNode(g, defaultPluginState("gl.synth"), 0, 0);
  g.links = {notes(md::kJInputs, a), audio(a, md::kJOutputs)};
  AudioBuffer on = render(instrumentDoc(instHost(g)));
  expect(on.peak() > 0.01f, "notes reach the instrument over its cable");
  md::JGraph off = g;
  off.links = {audio(a, md::kJOutputs)};
  expect(render(instrumentDoc(instHost(off))).peak() == 0.f, "an instrument without a notes cable stays silent");
  md::JGraph noOut = g;
  noOut.links = {notes(md::kJInputs, a)};
  expect(render(instrumentDoc(instHost(noOut))).peak() == 0.f, "an instrument without an audio cable is not heard");

  // Through a note processor: a chord plays more than the note.
  md::JGraph c;
  const uint32_t ch = md::jAddNode(c, defaultPluginState("gl.chord"), 0, 0), s = md::jAddNode(c, defaultPluginState("gl.synth"), 0, 0);
  c.links = {notes(md::kJInputs, ch), notes(ch, s), audio(s, md::kJOutputs)};
  AudioBuffer viaChord = render(instrumentDoc(instHost(c)));
  md::JGraph c2 = c;
  c2.links = {notes(md::kJInputs, s), audio(s, md::kJOutputs)};
  AudioBuffer direct = render(instrumentDoc(instHost(c2)));
  expect(viaChord.peak() > 0.01f && viaChord.data != direct.data, "notes go through a note processor to the instrument");

  // Split: the zones go to their own instruments.
  md::JGraph sp;
  const uint32_t z = md::jAddNode(sp, defaultPluginState("gl.split"), 0, 0), lo = md::jAddNode(sp, defaultPluginState("gl.synth"), 0, 0),
                 hi = md::jAddNode(sp, defaultPluginState("gl.synth"), 0, 0);
  auto zone = [&](int k, float a0, float a1) {
    PluginState& ps = sp.node(z)->plugin;
    ps.params[static_cast<size_t>(md::splitParam(k, md::SpOn))] = 1;
    ps.params[static_cast<size_t>(md::splitParam(k, md::SpLow))] = a0;
    ps.params[static_cast<size_t>(md::splitParam(k, md::SpHigh))] = a1;
  };
  zone(0, 0, 63);
  zone(1, 64, 127);
  sp.links = {notes(md::kJInputs, z), notes(z, lo, 1), notes(z, hi, 2), audio(lo, md::kJOutputs)};
  Document d = instrumentDoc(instHost(sp));
  AudioBuffer lowOnly = render(d);
  md::JGraph sp2 = sp;
  sp2.links = {notes(md::kJInputs, z), notes(z, lo, 1), notes(z, hi, 2), audio(hi, md::kJOutputs)};
  AudioBuffer highOnly = render(instrumentDoc(instHost(sp2)));
  expect(lowOnly.peak() > 0.01f && highOnly.peak() > 0.01f && lowOnly.data != highOnly.data, "a split sends its zones to their own instruments");

  // Bypassed note processor passes the notes on; muted one stops them.
  md::JGraph b = c;
  b.node(ch)->mode = md::JMode::Bypass;
  expect(render(instrumentDoc(instHost(b))).data == direct.data, "a bypassed note processor passes the notes");
  b.node(ch)->mode = md::JMode::Mute;
  expect(render(instrumentDoc(instHost(b))).peak() == 0.f, "a muted note processor stops the notes");
}

void testParams() {
  // A macro drives a filter inside: moving it changes the sound, in the range of its cable.
  md::JGraph g;
  const uint32_t s = md::jAddNode(g, defaultPluginState("gl.synth"), 0, 0), f = md::jAddNode(g, defaultPluginState("gl.filter"), 0, 0);
  g.links = {notes(md::kJInputs, s), audio(s, f), audio(f, md::kJOutputs), param(md::kJInputs, 0, f, pp::FltCutoff, 0.1f, 0.9f)};
  g.controls.push_back({0, "Tone"});
  PluginState st = instHost(g);
  st.params[0] = 0.0f;
  AudioBuffer low = render(instrumentDoc(st));
  st.params[0] = 1.0f;
  AudioBuffer high = render(instrumentDoc(st));
  expect(low.peak() > 0.001f && low.data != high.data, "a macro cable moves the parameter");
  // An exposed parameter is an automation target of the host: Param slot k.
  md::JGraph h = g;
  const int hp = md::jExpose(h, f, pp::FltReso, "Bite");
  PluginState sa = instHost(h), sb = instHost(h);
  sa.params[static_cast<size_t>(hp)] = 0.0f;
  sb.params[static_cast<size_t>(hp)] = 1.0f;
  expect(render(instrumentDoc(sa)).data != render(instrumentDoc(sb)).data, "an exposed parameter drives its module");

  // A controller output drives another module: X-Y pad X → filter cutoff.
  md::JGraph c;
  const uint32_t xy = md::jAddNode(c, defaultPluginState("gl.xyctl"), 0, 0), flt = md::jAddNode(c, defaultPluginState("gl.filter"), 0, 0);
  c.links = {audio(md::kJInputs, flt), audio(flt, md::kJOutputs), param(xy, md::XyX < 0 ? 0 : 0, flt, pp::FltCutoff)};
  auto hostAt = [&](float x) {
    md::JGraph t = c;
    t.node(xy)->plugin.params[md::XyX] = x;
    t.node(xy)->plugin.params[md::XySmooth] = 0;
    return fxHost(t);
  };
  std::vector<float> n1(1 << 14), n2(1 << 14), r1, r2;
  uint32_t seed = 12345;
  for (size_t k = 0; k < n1.size(); ++k) {
    seed = seed * 1664525u + 1013904223u;
    n1[k] = n2[k] = static_cast<float>(seed >> 8) / 8388608.0f - 1.0f;
  }
  r1 = n1, r2 = n2;
  FxRun a(hostAt(0.1f)), b(hostAt(0.9f));
  a.run(n1, r1, 256);
  b.run(n2, r2, 256);
  double ea = 0, eb = 0;
  for (size_t k = 8192; k < n1.size(); ++k) ea += n1[k] * n1[k], eb += n2[k] * n2[k];
  expect(eb > ea * 1.5, "a controller output moves a parameter of another module");
}

void testSidechain() {
  // The ducker hears the sidechain port.
  md::JGraph g;
  const uint32_t d = md::jAddNode(g, defaultPluginState("gl.scduck"), 0, 0);
  g.links = {audio(md::kJInputs, d), audio(md::kJInputs, d, 1, 1), audio(d, md::kJOutputs)};
  md::JGraph h = g;
  h.links = {audio(md::kJInputs, d), audio(d, md::kJOutputs)};
  std::vector<float> a(8192, 0.5f), b(8192, 0.5f), c(8192, 0.5f), e(8192, 0.5f);
  FxRun fa(fxHost(g)), fb(fxHost(h));
  fa.run(a, b, 256);
  fb.run(c, e, 256);
  expect(finite(a) && std::fabs(a.back()) < std::fabs(c.back()) - 0.01f, "the sidechain port reaches the module");
}

void testBlockSize() {
  // Instruments, a note processor, parallel paths, a feedback cable and a controller.
  md::JGraph g;
  const uint32_t arp = md::jAddNode(g, defaultPluginState("gl.arp"), 0, 0), s = md::jAddNode(g, defaultPluginState("gl.synth"), 0, 0),
                 f = md::jAddNode(g, defaultPluginState("gl.filter"), 0, 0), dl = md::jAddNode(g, defaultPluginState("gl.delay"), 0, 0),
                 xy = md::jAddNode(g, defaultPluginState("gl.xyctl"), 0, 0);
  g.links = {notes(md::kJInputs, arp), notes(arp, s), audio(s, f), audio(f, dl, 0.7f), audio(dl, f, 0.2f), audio(f, md::kJOutputs), audio(dl, md::kJOutputs, 0.5f),
             param(xy, 0, f, pp::FltCutoff, 0.2f, 0.8f)};
  const AudioBuffer a = render(instrumentDoc(instHost(g)), 512);
  expect(a.peak() > 0.001f, "the graph sounds");
  bool same = true;
  for (int block : {64, 100, 333}) same = same && render(instrumentDoc(instHost(g)), block).data == a.data;
  expect(same, "the output does not depend on the block size");
}

void testAllocations() {
  md::JGraph g;
  const uint32_t arp = md::jAddNode(g, defaultPluginState("gl.arp"), 0, 0), s = md::jAddNode(g, defaultPluginState("gl.synth"), 0, 0),
                 f = md::jAddNode(g, defaultPluginState("gl.filter"), 0, 0), dl = md::jAddNode(g, defaultPluginState("gl.delay"), 0, 0),
                 xy = md::jAddNode(g, defaultPluginState("gl.xyctl"), 0, 0);
  g.links = {notes(md::kJInputs, arp), notes(arp, s), audio(s, f), audio(f, dl), audio(dl, f, 0.2f), audio(f, md::kJOutputs), param(xy, 0, f, pp::FltCutoff, 0.2f, 0.8f),
             param(md::kJInputs, 0, dl, pp::DlyFeedback, 0.0f, 0.5f)};
  g.controls.push_back({0, "Echoes"});
  Document d = instrumentDoc(instHost(g));
  ProgramBuilder builder;
  builder.setSampleRate(kSr);
  AudioEngine eng(kSr);
  eng.setProgram(builder.build(d.song()));
  eng.setMode(PlayMode::Song);
  eng.play();
  std::vector<float> L(512), R(512);
  for (int k = 0; k < 20; ++k) eng.process(L.data(), R.data(), 512);  // warm: first sync, voices
  gAllocs = 0;
  gCount = true;
  for (int k = 0; k < 200; ++k) eng.process(L.data(), R.data(), 512);
  gCount = false;
  long allocs = gAllocs.load();
  expect(allocs == 0, "playing the graph allocates nothing");
  // An edit: the new graph is prepared here, adopted on the audio path without allocating.
  md::JGraph e = g;
  md::jAddNode(e, defaultPluginState("gl.chorus"), 0, 0);
  md::jRemoveNode(e, dl);
  PluginState ns = instHost(e);
  d.editChannel(d.song().channels.back().id, "edit", [&](Channel& c) { c.plugin = ns; });
  eng.setProgram(builder.build(d.song()));
  gAllocs = 0;
  gCount = true;
  for (int k = 0; k < 50; ++k) eng.process(L.data(), R.data(), 512);
  gCount = false;
  allocs = gAllocs.load();
  char buf[120];
  std::snprintf(buf, sizeof buf, "rewiring and playing on allocates nothing on the audio path (%ld)", allocs);
  expect(allocs == 0, buf);
}

void testOldStates() {
  // No data: the default chain still runs the chain runtime and reads as a graph.
  PluginState fresh = defaultPluginState("gl.patcher");
  expect(fresh.data.empty(), "a new Junction keeps empty data");
  md::JGraph dg = md::junctionOf(fresh);
  expect(dg.nodes.size() == 3 && !dg.links.empty(), "the default chain reads as a graph");
  // Chain data runs as before; its graph sounds the same.
  for (int m : {0, 1}) {
    PluginState chain = defaultPluginState("gl.patcher");
    chain.params[0] = m ? 0.2f : 0.8f;
    PluginState v3 = chain;
    v3.data = md::serializeJunction(md::junctionOf(chain));
    const AudioBuffer a = render(instrumentDoc(chain)), b = render(instrumentDoc(v3));
    expect(a.peak() > 0.001f && diff(a.data, b.data) < 1e-4, m ? "converted default instrument chain sounds the same (low macro)" : "converted default instrument chain sounds the same");
  }
  // Effect chain with a parallel branch and a mix.
  md::PatchGraph pg;
  md::PatchNode f1, f2, f3;
  f1.plugin = defaultPluginState("gl.filter");
  f2.plugin = defaultPluginState("gl.delay");
  f2.parallel = true;
  f2.mix = 0.5f;
  f3.plugin = defaultPluginState("gl.chorus");
  f3.mix = 0.3f;
  pg.nodes = {f1, f2, f3};
  pg.macros[0].push_back({0, pp::FltCutoff, 0.2f, 0.9f});
  PluginState chainFx = defaultPluginState("gl.patcherfx");
  chainFx.data = md::serializePatchGraph(pg);
  PluginState convFx = chainFx;
  convFx.data = md::serializeJunction(md::junctionOf(chainFx));
  std::vector<float> a(1 << 14), b(1 << 14);
  uint32_t seed = 99;
  for (size_t k = 0; k < a.size(); ++k) {
    seed = seed * 1664525u + 1013904223u;
    a[k] = b[k] = (static_cast<float>(seed >> 8) / 8388608.0f - 1.0f) * 0.3f;
  }
  std::vector<float> ar = a, br = b;
  FxRun ra(chainFx), rb(convFx);
  ra.run(a, ar, 512);
  rb.run(b, br, 512);
  expect(diff(a, b) < 1e-4 && diff(ar, br) < 1e-4, "converted effect chain (parallel branch, mix, macro) sounds the same");
  // Short parameter vectors (before the track outputs).
  PluginState oldState = defaultPluginState("gl.patcherfx");
  oldState.params.resize(md::PhCountV2);
  normalizePluginState(oldState);
  expect(static_cast<int>(oldState.params.size()) == md::PhCountV3 && oldState.params[md::PhSend0] == -1.f, "older states load with no track outputs");
  PluginState tiny = defaultPluginState("gl.patcherfx");
  tiny.params.resize(md::PhCount);
  normalizePluginState(tiny);
  expect(tiny.params[md::PhMap0] == 0.f && tiny.params[md::PhMix] == 1.f, "version 1 parameter vectors still load");
  // Garbage data never crashes.
  PluginState junk = defaultPluginState("gl.patcherfx");
  junk.data = "{\"v\":3,not json";
  ChannelAux ax;
  packs::prepareAux(junk, ax);
  std::vector<float> j1(256, 0.1f), j2(256, 0.1f);
  auto jfx = gl::createEffect("gl.patcherfx");
  ProcessContext ctx;
  jfx->processAux(junk, ax, ctx, j1.data(), j2.data(), 256);
  expect(finite(j1), "broken graph text does not break the host");
}

void testSaveUndo() {
  md::JGraph g;
  const uint32_t s = md::jAddNode(g, defaultPluginState("gl.synth"), 120, 80), f = md::jAddNode(g, defaultPluginState("gl.filter"), 320, 80);
  g.node(s)->name = "Lead";
  g.links = {notes(md::kJInputs, s), audio(s, f, 0.8f), audio(f, md::kJOutputs), param(md::kJInputs, 1, f, pp::FltCutoff, 0.2f, 0.7f)};
  g.controls.push_back({1, "Tone"});
  Document d = instrumentDoc(instHost(g));
  const std::string path = "k7_junction_test.gridline.json";
  std::string err;
  expect(saveProject(path, d.song(), &err), "project saves");
  Song back;
  expect(loadProject(path, back, &err), "project loads");
  std::remove(path.c_str());
  md::JGraph bg;
  expect(!back.channels.empty() && md::parseJunction(back.channels.back().plugin.data, bg) && bg == g, "the graph comes back from the file");
  AudioBuffer before = render(d);
  Document reopened(back);
  expect(render(reopened).data == before.data, "the reopened project sounds the same");

  // One undo step per edit, the way the editor stores them.
  ObjectId ch = d.song().channels.back().id;
  const std::string start = d.song().channels.back().plugin.data;
  auto commit = [&](const md::JGraph& next, const char* label) {
    PluginState st = d.song().channel(ch)->plugin;
    st.data = md::serializeJunction(next);
    d.editChannel(ch, label, [&](Channel& c) { c.plugin = st; });
  };
  md::JGraph e = g;
  md::jAddNode(e, defaultPluginState("gl.delay"), 500, 80);
  commit(e, "add module");
  const std::string one = d.song().channel(ch)->plugin.data;
  md::jConnect(e, audio(f, e.nodes.back().id), false);
  commit(e, "connect");
  d.undo();
  expect(d.song().channel(ch)->plugin.data == one, "undo takes back one cable");
  d.undo();
  expect(d.song().channel(ch)->plugin.data == start, "undo takes back the module");
  d.redo();
  expect(d.song().channel(ch)->plugin.data == one, "redo brings it back");
}

void testPatches() {
  const auto& list = md::junctionPatches();
  expect(list.size() >= 6, "factory patches for both versions");
  for (size_t k = 0; k < list.size(); ++k) {
    const md::JPatch& p = list[k];
    const bool fx = p.forFx;
    md::JGraph g = md::junctionPatch(k, fx);
    std::string text = md::serializeJunction(g);
    md::JGraph back;
    bool ok = md::parseJunction(text, back) && back == g;
    char name[120];
    if (fx) {
      std::vector<float> L(1 << 14), R(1 << 14);
      uint32_t seed = 7;
      for (size_t i = 0; i < L.size(); ++i) {
        seed = seed * 1664525u + 1013904223u;
        L[i] = R[i] = i < 4000 ? (static_cast<float>(seed >> 8) / 8388608.0f - 1.0f) * 0.4f : 0.f;
      }
      FxRun h(fxHost(g));
      h.run(L, R, 512);
      double e = 0;
      for (float v : L) e += v * v;
      ok = ok && finite(L) && finite(R) && e > 0.01;
    } else {
      const AudioBuffer a = render(instrumentDoc(instHost(g)));
      ok = ok && a.peak() > 0.001f && finite(a.data);
    }
    std::snprintf(name, sizeof name, "factory patch %s sounds", p.name);
    expect(ok, name);
  }
}

}  // namespace

double relDiff(const std::vector<float>& a, const std::vector<float>& b) {
  if (a.size() != b.size() || a.empty()) return 1e30;
  double e = 0, t = 0;
  for (size_t k = 0; k < a.size(); ++k) {
    const double d = static_cast<double>(a[k]) - static_cast<double>(b[k]);
    e += d * d;
    t += static_cast<double>(a[k]) * static_cast<double>(a[k]);
  }
  return t > 0 ? std::sqrt(e / t) : (e > 0 ? 1e30 : 0);
}

// FL patches: how many run as wired, and the graph reading sounds like the FL patch runtime.
void testNodeState() {
  PluginState inner = defaultPluginState("gl.filter");
  inner.params[0] = 0.25f;
  // A chain stays a chain, a graph a graph.
  PluginState chain = defaultPluginState("gl.patcherfx");
  md::JGraph cg = md::junctionOf(chain);
  expect(!cg.nodes.empty(), "module state: the default chain has modules");
  if (cg.nodes.empty()) return;
  inner.type = cg.nodes[0].plugin.type;
  inner.params = cg.nodes[0].plugin.params;
  inner.params[0] = inner.params[0] + 0.0f;
  expect(md::junctionSetNodeState(chain, cg.nodes[0].id, inner) && !md::isJunction3(chain.data), "module state: a chain stays a chain");
  md::JGraph g;
  const uint32_t a = md::jAddNode(g, defaultPluginState("gl.delay"), 0, 0);
  PluginState host = fxHost(g);
  PluginState d2 = defaultPluginState("gl.filter");
  expect(md::junctionSetNodeState(host, a, d2) && md::isJunction3(host.data) && md::junctionOf(host).node(a)->plugin.type == "gl.filter", "module state: a graph stays a graph");
  expect(!md::junctionSetNodeState(host, 999, d2), "module state: an unknown module is refused");
}

void testFlCorpus() {
  if (!testfl::flInstalled()) {
    expect(true, "FL Studio not installed: Patcher corpus skipped");
    return;
  }
  const auto pres = testfl::decodePresets("Patcher");
  size_t v2 = 0, graph = 0, tried = 0, close = 0, finiteRuns = 0, ranGraph = 0, freeRunning = 0, sameUnaligned = 0, even = 0, evenClose = 0, unexplained = 0;
  size_t marked = 0, latent = 0, reportedRight = 0, latentGraph = 0, latentRendered = 0, latentSame = 0;
  for (const auto& d : pres) {
    if (!d.ok || !md::isPatchV2(d.state.data)) continue;
    ++v2;
    md::PatchV2 g;
    if (!md::parsePatchV2(d.state.data, g)) continue;
    // K-AC: an import runs as FL's Patcher does: lined up, its latency told to the host.
    marked += g.align;
    {
      const int want = md::junctionLatency(md::junctionFromV2(g), kSr);
      if (want > 0 && !g.run3) {
        ++latent;
        latentGraph += md::patchV2RunsAsGraph(g, kSr);
        std::unique_ptr<Generator> gn = d.state.type == "gl.patcher" ? gl::createGenerator(d.state.type) : nullptr;
        std::unique_ptr<Effect> fx = d.state.type == "gl.patcherfx" ? gl::createEffect(d.state.type) : nullptr;
        reportedRight += (gn ? gn->latencyFor(d.state, kSr) : fx ? fx->latencyFor(d.state, kSr) : -1) == want;
        // Through the whole engine it sounds as the same graph wired by hand.
        if (d.state.type == "gl.patcher" && latentRendered < 8) {
          PluginState wired = d.state;
          wired.data = md::serializeJunction(md::junctionFromV2(g));
          const AudioBuffer imported = render(instrumentDoc(d.state));
          if (imported.peak() > 1e-3f && finite(imported.data) && relDiff(imported.data, render(instrumentDoc(d.state)).data) < 1e-3) {
            ++latentRendered;
            latentSame += relDiff(imported.data, render(instrumentDoc(wired)).data) < 1e-3;
          }
        }
      }
    }
    if (g.run3) {
      ++graph;
      if (d.state.type == "gl.patcher" && ranGraph < 8) {
        ++ranGraph;
        finiteRuns += finite(render(instrumentDoc(d.state)).data);
      }
      continue;
    }
    if (d.state.type != "gl.patcher" || tried >= 32 || !md::patchV2Topology(g).unsupported.empty()) continue;
    PluginState v3 = d.state;
    v3.data = md::serializeJunction(md::junctionFromV2(g));
    // The version 2 runtime as projects from before play it.
    PluginState before = d.state;
    md::PatchV2 oldData = g;
    oldData.align = false;
    before.data = md::serializePatchV2(oldData);
    const AudioBuffer a = render(instrumentDoc(before));
    if (a.peak() < 1e-3f) continue;
    if (relDiff(a.data, render(instrumentDoc(before)).data) > 1e-3) {  // free-running noise or oscillators
      ++freeRunning;
      continue;
    }
    ++tried;
    // The one difference between the runtimes: the graph runtime lines parallel paths up by
    // their latency as FL does (K-AC), the version 2 runtime sums them as they come. With that
    // off they must match sample by sample, every patch.
    md::junctionCompensationForTests(false);
    const AudioBuffer unaligned = render(instrumentDoc(v3));
    md::junctionCompensationForTests(true);
    const double plain = relDiff(a.data, unaligned.data);
    if (plain < 0.05) ++sameUnaligned;
    else std::printf("# differs without compensation %.3f: %s\n", plain, d.where.c_str());
    // With it on, a patch whose paths have equal latency stays the same, the output only moved
    // by the latency the host reports. A patch with uneven paths sums differently, as in FL, and
    // it has to be one that the compensation acts on.
    const md::JGraph asGraph = md::junctionFromV2(g);
    const int uneven = md::junctionUnevenCables(asGraph, kSr), reported = md::junctionLatency(asGraph, kSr);
    const AudioBuffer bb = render(instrumentDoc(v3));
    double rd = 1e30;
    for (int l : {0, reported, -reported}) {
      std::vector<float> xa, xb;
      for (size_t k = 8192; k + 8192 < a.data.size(); ++k) {
        xa.push_back(a.data[k]);
        xb.push_back(bb.data[static_cast<size_t>(static_cast<long>(k) + l)]);
      }
      rd = std::min(rd, relDiff(xa, xb));
    }
    if (rd < 0.05) ++close;
    if (uneven == 0) {
      ++even;
      evenClose += rd < 0.05;
    }
    if (rd >= 0.05) {
      std::printf("# compensation changes the sum (%.3f; %d cables delayed, latency %d): %s\n", rd, uneven, reported, d.where.c_str());
      unexplained += uneven == 0;
    }
  }
  char buf[200];
  std::snprintf(buf, sizeof buf, "FL patches: every import carries the marker (%zu/%zu)", marked, v2);
  expect(v2 > 0 && marked == v2, buf);
  std::snprintf(buf, sizeof buf, "FL patches with latency: %zu/%zu run as graphs, %zu report it to the host", latentGraph, latent, reportedRight);
  expect(latent > 0 && latentGraph == latent && reportedRight == latent, buf);
  std::snprintf(buf, sizeof buf, "FL patches with latency sound as the same graph wired by hand (%zu/%zu)", latentSame, latentRendered);
  expect(latentRendered > 0 && latentSame == latentRendered, buf);
  std::snprintf(buf, sizeof buf, "FL patches: %zu presets, %zu FL graphs, %zu run in the graph runtime (%zu/%zu of those tried finite)", pres.size(), v2, graph, finiteRuns, ranGraph);
  expect(v2 > 0 && finiteRuns == ranGraph, buf);
  std::snprintf(buf, sizeof buf, "FL patches read as a graph match the FL patch runtime with compensation off (%zu/%zu within 5%%, %zu free running)", sameUnaligned, tried, freeRunning);
  expect(tried > 0 && sameUnaligned == tried, buf);
  std::snprintf(buf, sizeof buf, "patches with even paths match with compensation on (%zu/%zu); all compensated: %zu/%zu", evenClose, even, close, tried);
  expect(evenClose == even && unexplained == 0, buf);
}

double rmsOf(FxRun& r, double hz) {
  static double phase = 0;
  std::vector<float> L(4096), R(4096);
  for (size_t k = 0; k < L.size(); ++k) {
    L[k] = R[k] = static_cast<float>(0.5 * std::sin(phase));
    phase += 2.0 * 3.14159265358979 * hz / kSr;
  }
  r.run(L, R, 256);
  double e = 0;
  for (size_t k = 2048; k < L.size(); ++k) e += static_cast<double>(L[k]) * static_cast<double>(L[k]);
  return std::sqrt(e / 2048.0);
}

// Edits that change values only reach the running graph: nothing is prepared again.
void testValueEdits() {
  md::JGraph g;
  const uint32_t f = md::jAddNode(g, defaultPluginState("gl.filter"), 0, 0), dl = md::jAddNode(g, defaultPluginState("gl.filter"), 0, 0);
  g.links = {audio(md::kJInputs, f), audio(f, dl), audio(dl, md::kJOutputs)};
  FxRun r(fxHost(g));
  md::JRuntime* rt = md::junctionRuntime(r.fx.get());
  expect(rt != nullptr, "values: the runtime is reachable");
  if (!rt) return;
  const AuxData* first = r.aux.custom.get();
  double base = rmsOf(r, 5000);
  expect(base > 0.2 && rt->graphCounts().builds == 1, "values: the first build prepares the graph");

  const auto edited = [&](const std::function<void(md::JGraph&)>& fn) {
    PluginState st = r.st;
    md::JGraph e = md::junctionOf(st);
    fn(e);
    st.data = md::serializeJunction(e);
    return st;
  };
  // A knob of a module.
  buildStep(r, edited([&](md::JGraph& e) { e.node(f)->plugin.params[pp::FltCutoff] = 200.0f; }));
  expect(r.aux.custom.get() == first, "values: a knob keeps the prepared graph");
  double low = rmsOf(r, 5000);
  expect(low < base * 0.2, "values: the new knob value reaches the sound");
  expect(rt->graphCounts().builds == 1 && rt->graphCounts().valueEdits == 1, "values: a knob is no rebuild");
  // Undo of it, as the stored state coming back.
  buildStep(r, edited([&](md::JGraph& e) { e.node(f)->plugin.params[pp::FltCutoff] = 20000.0f; }));
  expect(std::fabs(rmsOf(r, 5000) - base) < base * 0.02, "values: the old value restores the sound");
  // A cable's level.
  buildStep(r, edited([&](md::JGraph& e) { e.links[2].gain = 0.5f; }));
  expect(std::fabs(rmsOf(r, 5000) - base * 0.5) < base * 0.02 && r.aux.custom.get() == first, "values: a cable's level reaches the sound, the graph stays");
  buildStep(r, edited([&](md::JGraph& e) { e.links[2].gain = 1.0f; }));
  // A module's mix (a filter closed, half dry).
  buildStep(r, edited([&](md::JGraph& e) {
    e.node(dl)->plugin.params[pp::FltCutoff] = 20.0f;
    e.node(dl)->mix = 0.5f;
  }));
  const double half = rmsOf(r, 5000);
  expect(half > base * 0.3 && half < base * 0.7 && r.aux.custom.get() == first, "values: a module's mix reaches the sound, the graph stays");
  buildStep(r, edited([&](md::JGraph& e) {
    e.node(dl)->plugin.params[pp::FltCutoff] = 20000.0f;
    e.node(dl)->mix = 1.0f;
  }));
  // The look of the map.
  const uint64_t builds = rt->graphCounts().builds;
  buildStep(r, edited([&](md::JGraph& e) {
    e.node(f)->x += 40;
    e.node(f)->name = "Renamed";
    e.node(f)->color = 3;
    e.inX += 10;
  }));
  rmsOf(r, 5000);
  expect(rt->graphCounts().builds == builds && r.aux.custom.get() == first, "values: moving, renaming and colouring a module prepare nothing");
  // Nothing changed for this host.
  const uint64_t same = rt->graphCounts().unchanged;
  buildStep(r, r.st);
  expect(rt->graphCounts().builds == builds && rt->graphCounts().unchanged == same + 1 && r.aux.custom.get() == first, "values: an edit elsewhere in the project prepares nothing here");
  // The wiring: prepared anew, running modules kept.
  buildStep(r, edited([&](md::JGraph& e) { e.links.push_back(audio(md::kJInputs, md::kJOutputs, 0.25f)); }));
  expect(rt->graphCounts().builds == builds + 1 && r.aux.custom.get() != first, "values: a new cable prepares the graph");
  rmsOf(r, 5000);
  // A new latency moves compensation delays: not a value any more.
  {
    setTestEffectFactory(testFactory);
    md::JGraph t;
    const uint32_t a = md::jAddNode(t, defaultPluginState("gl.utility"), 0, 0);
    t.links = {audio(md::kJInputs, a), audio(a, md::kJOutputs), audio(md::kJInputs, md::kJOutputs)};
    t.nodes[0].mix = 1.0f;
    FxRun u(fxHost(t));
    md::JRuntime* ru = md::junctionRuntime(u.fx.get());
    std::vector<float> L(512), R(512);
    u.run(L, R, 256);
    t.nodes[0].mix = 0.25f;  // below one half a module reports no latency
    PluginState st = fxHost(t);
    buildStep(u, st);
    expect(ru->graphCounts().builds == 2, "values: a change of latency prepares the graph");
    setTestEffectFactory(nullptr);
  }
  // A parameter a cable drives: the rebuild gives it back to the cable.
  {
    md::JGraph t;
    const uint32_t x = md::jAddNode(t, defaultPluginState("gl.xyctl"), 0, 0), c = md::jAddNode(t, defaultPluginState("gl.filter"), 0, 0);
    t.links = {audio(md::kJInputs, c), audio(c, md::kJOutputs), param(x, 0, c, pp::FltCutoff)};
    FxRun u(fxHost(t));
    md::JRuntime* ru = md::junctionRuntime(u.fx.get());
    rmsOf(u, 5000);
    t.node(c)->plugin.params[pp::FltCutoff] = 500.0f;
    buildStep(u, fxHost(t));
    expect(ru->graphCounts().builds == 2, "values: a parameter driven by a cable prepares the graph");
    rmsOf(u, 5000);
    t.node(c)->plugin.params[pp::FltReso] = 0.5f;
    buildStep(u, fxHost(t));
    expect(ru->graphCounts().builds == 2, "values: its other parameters do not");
  }
  // Through the real Program build, playing, without allocating on the audio path.
  {
    md::JGraph t;
    const uint32_t s = md::jAddNode(t, defaultPluginState("gl.synth"), 0, 0), c = md::jAddNode(t, defaultPluginState("gl.filter"), 0, 0);
    t.links = {notes(md::kJInputs, s), audio(s, c), audio(c, md::kJOutputs)};
    Document d = instrumentDoc(instHost(t));
    const ObjectId ch = d.song().channels.back().id;
    ProgramBuilder builder;
    builder.setSampleRate(kSr);
    AudioEngine eng(kSr);
    eng.setProgram(builder.build(d.song()));
    eng.setMode(PlayMode::Song);
    eng.play();
    std::vector<float> L(512), R(512);
    for (int k = 0; k < 8; ++k) eng.process(L.data(), R.data(), 512);
    auto chrt = builder.channelRuntime(ch);
    md::JRuntime* ru = chrt && chrt->gen ? md::junctionRuntime(chrt->gen.get()) : nullptr;
    expect(ru != nullptr, "values: the instrument's runtime is reachable");
    if (ru) {
      const uint64_t b0 = ru->graphCounts().builds;
      long allocs = 0;
      for (int step = 0; step < 32; ++step) {  // a knob drag, one value per frame
        t.node(c)->plugin.params[pp::FltCutoff] = 20000.0f / static_cast<float>(1 + step);
        d.editChannel(ch, "drag", [&](Channel& cc) { cc.plugin = instHost(t); }, 77);
        eng.setProgram(builder.build(d.song()));
        gAllocs = 0;
        gCount = true;
        eng.process(L.data(), R.data(), 512);
        gCount = false;
        allocs += gAllocs.load();
      }
      expect(ru->graphCounts().builds == b0 && ru->graphCounts().valueEdits >= 31, "values: a knob drag through the Program build prepares nothing");
      expect(allocs == 0, "values: they arrive without allocating");
      const uint64_t v0 = ru->graphCounts().valueEdits, u0 = ru->graphCounts().unchanged;
      d.setParam(songParam(SongParam::Tempo), 133.0, false);
      eng.setProgram(builder.build(d.song()));
      eng.process(L.data(), R.data(), 512);
      expect(ru->graphCounts().builds == b0 && ru->graphCounts().valueEdits == v0 && ru->graphCounts().unchanged > u0, "values: an edit elsewhere leaves the graph");
    }
  }
  // The largest graph: numbers, and a drag that stays far below a full prepare.
  {
    md::JGraph big;
    uint32_t last = md::kJInputs;
    for (int k = 0; k < md::kJMaxNodes; ++k) {
      const uint32_t id = md::jAddNode(big, defaultPluginState("gl.filter"), 20.0f + static_cast<float>(k % 16) * 190.0f, 20.0f + static_cast<float>(k / 16) * 110.0f);
      big.links.push_back(audio(last, id));
      last = id;
    }
    big.links.push_back(audio(last, md::kJOutputs));
    using Clock = std::chrono::steady_clock;
    const auto ms = [](Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
    Clock::time_point t0 = Clock::now();
    FxRun u(fxHost(big));
    const double prepared = ms(t0, Clock::now());
    md::JRuntime* ru = md::junctionRuntime(u.fx.get());
    std::vector<float> L(4096, 0.1f), R(4096, 0.1f);
    t0 = Clock::now();
    u.run(L, R, 256);
    const double rendered = ms(t0, Clock::now());
    double worst = 0, total = 0;
    const int steps = 20;
    for (int k = 0; k < steps; ++k) {
      big.node(md::kJFirstId + static_cast<uint32_t>(k))->plugin.params[pp::FltCutoff] = 5000.0f - static_cast<float>(k);
      const PluginState st = fxHost(big);
      t0 = Clock::now();
      buildStep(u, st);
      const double one = ms(t0, Clock::now());
      worst = std::max(worst, one), total += one;
      std::fill(L.begin(), L.begin() + 256, 0.1f);
      std::fill(R.begin(), R.begin() + 256, 0.1f);
      u.fx->processAux(u.st, u.aux, u.ctx, L.data(), R.data(), 256);
    }
    std::printf("# %d modules: full prepare %.1f ms; value edit %.2f ms (worst %.2f); render %.1f %% of real time\n", md::kJMaxNodes, prepared, total / steps, worst,
                100.0 * rendered / (1000.0 * 4096.0 / kSr));
    expect(ru->graphCounts().builds == 1, "values: the largest graph: a drag prepares nothing");
    // The times above are information only: under CI load a wall-clock comparison says nothing (K-X).
    // The counts are what proves the path: every edit took the value route, none prepared anything.
    expect(ru->graphCounts().valueEdits == static_cast<uint64_t>(steps), "values: the largest graph: every edit went the value route");
  }
}

// What export leaves out is in its report: nothing drops silently.
void testFlExportNotes() {
  if (!testfl::flInstalled()) return;
  md::JGraph g;
  const uint32_t syn = md::jAddNode(g, defaultPluginState("gl.synth"), 0, 0), f = md::jAddNode(g, defaultPluginState("gl.filter"), 0, 0),
                 nest = md::jAddNode(g, defaultPluginState("gl.patcherfx"), 0, 0);
  g.links = {notes(md::kJInputs, syn), audio(syn, f), audio(md::kJInputs, f), audio(f, nest), audio(nest, md::kJOutputs), audio(f, md::kJOutputs)};
  Document d = instrumentDoc(instHost(g));
  flp::Bytes bytes;
  flp::ExportReport rep;
  flp::ExportOptions o;
  o.factoryData = testfl::flData();
  expect(flp::exportFlp(d.song(), bytes, rep, o), "export notes: the patch exports");
  std::string all;
  for (const auto& w : rep.warnings) all += w + "\n";
  for (const auto& q : rep.plugins) all += q.note + "\n";
  expect(all.find("a Junction inside a Junction") != std::string::npos, "export report names a Junction inside a Junction");
  expect(all.find("a cable from the audio input is left out") != std::string::npos, "export report names an instrument patch's audio input cable");
}

// Hand-wired graphs through the FL exporter and back.
void testFlExport() {
  const auto& list = md::junctionPatches();
  for (size_t i = 0; i < list.size(); ++i) {
    const bool fx = !list[i].forInstrument;
    const md::JGraph g = md::junctionPatch(i, fx);
    PluginState host = fx ? fxHost(g) : instHost(g);
    Document d = instrumentDoc(defaultPluginState("gl.patcher"));
    if (fx) {
      d.editMixerTrack(d.song().mixer[1].id, "set", [&](MixerTrack& m) { m.slots[0].plugin = host; });
    } else {
      d.editChannel(d.song().channels.back().id, "set", [&](Channel& c) { c.plugin = host; });
    }
    flp::Bytes bytes;
    flp::ExportReport rep;
    flp::ExportOptions o;
    o.factoryData = testfl::flData();
    std::string label = std::string(list[i].name) + ": ";
    expect(flp::exportFlp(d.song(), bytes, rep, o), (label + "exports").c_str());
    Song back;
    flp::Report ir;
    std::string err;
    const bool ok = importFlpBytes(bytes, back, ir, &err, testfl::flData(), "", false);
    expect(ok, (label + "imports").c_str());
    if (!ok) continue;
    const PluginState* st = nullptr;
    if (fx) {
      for (const auto& m : back.mixer)
        for (const auto& sl : m.slots)
          if (sl.plugin.type == "gl.patcherfx") st = &sl.plugin;
    } else {
      for (const auto& c : back.channels)
        if (c.plugin.type == "gl.patcher" && !c.plugin.data.empty()) st = &c.plugin;
    }
    expect(st != nullptr, (label + "comes back as a Junction").c_str());
    if (!st) continue;
    const md::JGraph r = md::junctionOf(*st);
    size_t counts[2][3] = {};
    for (const auto& l : g.links) ++counts[0][static_cast<int>(l.kind)];
    for (const auto& l : r.links) ++counts[1][static_cast<int>(l.kind)];
    for (const auto& pl : rep.plugins)
      if (!pl.note.empty() && (pl.glType == "gl.patcher" || pl.glType == "gl.patcherfx")) std::printf("# note: %s\n", pl.note.c_str());
    std::printf("# %s: modules %zu -> %zu, audio %zu -> %zu, notes %zu -> %zu, params %zu -> %zu\n", list[i].name, g.nodes.size(), r.nodes.size(), counts[0][0], counts[1][0], counts[0][1], counts[1][1], counts[0][2], counts[1][2]);
    size_t leftOut = 0;
    bool lostCables = false;
    for (const auto& pl : rep.plugins)
      if (pl.glType == host.type)
        for (size_t at = pl.note.find("no FL plugin"); at != std::string::npos; at = pl.note.find("no FL plugin", at + 1)) ++leftOut;
    lostCables = leftOut > 0;
    expect(r.nodes.size() + leftOut == g.nodes.size(), (label + "keeps its modules; the export note names any it leaves out").c_str());
    if (!lostCables) expect(counts[1][0] == counts[0][0] && counts[1][1] == counts[0][1], (label + "keeps its audio and note cables").c_str());
    if (lostCables) continue;
    double gain = -1;
    for (const auto& l : r.links)
      if (l.kind == md::JKind::Audio && l.gain != 1.0f) gain = static_cast<double>(l.gain);
    double want = -1;
    for (const auto& l : g.links)
      if (l.kind == md::JKind::Audio && l.gain != 1.0f) want = static_cast<double>(l.gain);
    expect((want < 0 && gain < 0) || std::fabs(gain - want) < 1e-4, (label + "keeps its cable levels").c_str());
  }
  // A Panel cable to a parameter FL's plugin has comes back as an exposed parameter.
  md::JGraph g;
  const uint32_t dl = md::jAddNode(g, defaultPluginState("gl.delay"), 100, 40);
  g.links = {audio(md::kJInputs, dl), audio(dl, md::kJOutputs, 0.5f), param(md::kJInputs, 0, dl, 49)};
  g.controls.push_back({0, "Time"});
  Document d = instrumentDoc(defaultPluginState("gl.patcher"));
  d.editMixerTrack(d.song().mixer[1].id, "set", [&](MixerTrack& m) { m.slots[0].plugin = fxHost(g); });
  flp::Bytes bytes;
  flp::ExportReport rep;
  flp::ExportOptions o;
  o.factoryData = testfl::flData();
  expect(flp::exportFlp(d.song(), bytes, rep, o), "a Panel cable exports");
  Song back;
  flp::Report ir;
  std::string err;
  const PluginState* st = nullptr;
  if (importFlpBytes(bytes, back, ir, &err, testfl::flData(), "", false))
    for (const auto& m : back.mixer)
      for (const auto& sl : m.slots)
        if (sl.plugin.type == "gl.patcherfx") st = &sl.plugin;
  expect(st != nullptr, "a Panel cable comes back in a Junction");
  if (st) {
    const md::JGraph r = md::junctionOf(*st);
    size_t exposed = 0;
    for (const auto& l : r.links) exposed += l.kind == md::JKind::Param && l.src == md::kJInputs && l.dp == 49;
    expect(exposed == 1, "a Panel cable to a parameter FL has comes back as an exposed parameter");
  }
}

int main() {
  testModel();
  testChains();
  testFeedback();
  testLatency();
  testFlLatency();
  testNotes();
  testParams();
  testSidechain();
  testBlockSize();
  testAllocations();
  testOldStates();
  testSaveUndo();
  testPatches();
  testNodeState();
  testValueEdits();
  testFlExport();
  testFlExportNotes();
  testFlCorpus();
  return finish();
}
