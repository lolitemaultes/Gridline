// Gridline — Turntable (gl.turntable) tests (J-206): catalogue entry and
// presets, the built-in sound (plays until a sample is loaded, an imported
// empty deck stays silent), notes, the reference laws as measured plain
// numbers (motor, acceleration and speed steps, hold with Position sweeps,
// each Smooth control at both ends, Turns, pan, volume and mute, automation in
// the reference's "from MIDI" form), scratch automation through the hand,
// parameter extremes incl. NaN, block-size invariance, other sample rates,
// J-205 states, the FL state (every layout, the options tail, the empty deck,
// automation mapping), FL's own render-test project (sample, recorded
// position events) and the export round trip, and the Turntable FX effect.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <string>
#include <vector>
#include "../Check.h"
#include "core/PluginInfo.h"
#include "core/Params.h"
#include "core/Song.h"
#include "core/flp/Flp.h"
#include "core/flp/FlpEncode.h"
#include "core/flp/FlpExport.h"
#include "core/flp/FlpFx2.h"
#include "engine/Effects.h"
#include "engine/Generators.h"
#include "engine/Sample.h"
#include "packs/Packs.h"
#include "packs/spectral/turntable.h"

using namespace gl;
namespace sp = gl::packs::spectral;
namespace fs = std::filesystem;

namespace {

constexpr double kSr = 44100;
constexpr int kTestFrames = 22050;

// The measurement sample: 0.5 s of 16-bit stereo at 44.1 kHz from integer
// formulas (a triangle left, a saw right), written as a plain PCM file so it
// loads to the same 16-bit values the reference was given.
int16_t testL(int i) {
  const int t = (i * 13) % 2000;
  return static_cast<int16_t>((t < 1000 ? t : 2000 - t) * 24 - 12000);
}
int16_t testR(int i) { return static_cast<int16_t>(((i * 29) % 3000) * 16 - 24000); }

std::string testSample() {
  static std::string path;
  if (!path.empty()) return path;
  const fs::path d = fs::temp_directory_path() / "gl_turntable_j206";
  std::error_code ec;
  fs::create_directories(d, ec);
  path = (d / "t.wav").string();
  std::ofstream f(path, std::ios::binary);
  auto u32 = [&](uint32_t x) { f.write(reinterpret_cast<const char*>(&x), 4); };
  auto u16 = [&](uint16_t x) { f.write(reinterpret_cast<const char*>(&x), 2); };
  const uint32_t bytes = kTestFrames * 4;
  f.write("RIFF", 4), u32(36 + bytes), f.write("WAVEfmt ", 8), u32(16), u16(1), u16(2), u32(44100), u32(44100 * 4), u16(4), u16(16);
  f.write("data", 4), u32(bytes);
  for (int i = 0; i < kTestFrames; ++i) {
    const int16_t l = testL(i), r = testR(i);
    f.write(reinterpret_cast<const char*>(&l), 2);
    f.write(reinterpret_cast<const char*>(&r), 2);
  }
  return path;
}

struct Rig {
  PluginState st;
  ChannelAux aux;
  ProcessContext ctx;
  std::unique_ptr<Generator> g;
  std::vector<float> L, R;
  explicit Rig(const std::function<void(PluginState&)>& tweak = nullptr, bool builtIn = false, double sr = kSr) {
    st = defaultPluginState("gl.turntable");
    if (!builtIn) st.data = testSample();
    if (tweak) tweak(st);
    prepare();
    g = packs::createGenerator("gl.turntable");
    ctx.sampleRate = sr;
  }
  void prepare() {
    aux = ChannelAux{};
    packs::prepareAux(st, aux);
  }
  void set(int k, float v) { st.params[static_cast<size_t>(k)] = v; }
  // Renders `frames` in blocks of `block`; returns the RMS of the new part.
  double run(int64_t frames, int block = 256) {
    const size_t base = L.size();
    L.resize(base + static_cast<size_t>(frames), 0.f);
    R.resize(base + static_cast<size_t>(frames), 0.f);
    for (int64_t k = 0; k < frames; k += block) {
      const int n = static_cast<int>(std::min<int64_t>(block, frames - k));
      g->process(st, aux, ctx, L.data() + base + k, R.data() + base + k, n);
    }
    double e = 0;
    for (size_t i = base; i < L.size(); ++i) e += static_cast<double>(L[i]) * L[i] + static_cast<double>(R[i]) * R[i];
    return std::sqrt(e / (2.0 * std::max<double>(1, static_cast<double>(frames))));
  }
  float pos() const { return sp::turntableTelemetry(g.get())->pos.load(); }
  float speed() const { return sp::turntableTelemetry(g.get())->speed.load(); }
  bool finite() const {
    for (size_t i = 0; i < L.size(); ++i)
      if (!std::isfinite(L[i]) || !std::isfinite(R[i])) return false;
    return true;
  }
};

bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }
double circ(double a, double b) {
  double d = std::fabs(a - b);
  d -= std::floor(d);
  return std::min(d, 1.0 - d);
}

void testCatalogue() {
  const PluginInfo* info = findPlugin("gl.turntable");
  expect(info != nullptr, "gl.turntable is in the catalogue");
  if (!info) return;
  expect(info->kind == PluginKind::Generator && info->usesSample, "an instrument that loads a sample");
  expect(info->params.size() == static_cast<size_t>(sp::TtCount), "parameter count");
  expect(info->presets.size() >= 2 && info->presets.size() <= 6, "2 to 6 factory presets");
  expect(info->name == "Turntable", "named Turntable");
  expect(info->editorWidth == 470, "editor width: the reference panel at 1.9 times its size");
  const PluginInfo* old = findPlugin("gl.scratcher");
  expect(old && old->kind == PluginKind::Effect && old->name == "Turntable FX", "the effect keeps its type, shown as Turntable FX");
  expect(packs::createGenerator("gl.turntable") != nullptr, "created");
  expect(sp::turntableTelemetry(packs::createGenerator("gl.turntable").get()) != nullptr, "telemetry of the runtime");
}

// ---- built-in sound
void testBuiltin() {
  const SamplePtr b = sp::turntableBuiltinSample();
  expect(b && b->frames() > 44100 && b->frames() < 4 * 44100 && near(b->sampleRate, 44100, 1e-9), "the built-in sound: about a bar at 44.1 kHz");
  expect(b == sp::turntableBuiltinSample(), "built once");
  if (b) {
    double peak = 0, e = 0;
    bool fin = true;
    for (size_t i = 0; i < b->frames(); ++i) {
      const double x = b->left[i];
      fin = fin && std::isfinite(x);
      peak = std::max(peak, std::fabs(x));
      e += x * x;
    }
    expect(fin && peak > 0.5 && peak <= 1.0 && std::sqrt(e / static_cast<double>(b->frames())) > 0.03, "the built-in sound is loud and clean");
  }
  PluginState st = defaultPluginState("gl.turntable");
  expect(st.data.empty() && sp::turntableSample(st) == b, "a new Turntable plays the built-in sound");
  {
    Rig r(nullptr, true);
    expect(r.aux.custom != nullptr, "the built-in sound is prepared for the deck");
    r.g->noteOn(1, 60, 1.0f, 0);
    expect(r.run(22050) > 0.05, "the built-in sound plays after noteOn");
  }
  {  // an FL scratcher without a sample: an empty deck, silent like the reference
    Rig r([](PluginState& s) { s.params[sp::TtEmpty] = 1.f; }, true);
    expect(sp::turntableSample(r.st) == nullptr && r.aux.custom == nullptr, "Empty deck: no sample");
    r.g->noteOn(1, 60, 1.0f, 0);
    r.set(sp::TtTransport, 2);
    expect(r.run(22050) == 0.0 && r.finite(), "Empty deck: silent");
  }
  {  // a loaded sample replaces the built-in sound
    PluginState s = defaultPluginState("gl.turntable");
    s.data = testSample();
    const SamplePtr f = sp::turntableSample(s);
    expect(f && f != b && f->frames() == static_cast<size_t>(kTestFrames), "a loaded sample replaces the built-in sound");
    s.data = "/no/such/file.wav";
    expect(sp::turntableSample(s) == nullptr, "a missing file: no sound (not the built-in one)");
  }
}

void testNotes() {
  {  // default: Cue + motor, so a key starts the record at the cue at full speed
    Rig r;
    r.run(2048);
    r.g->noteOn(1, 60, 1.0f, 0);
    expect(r.run(11025) > 0.05, "sound after noteOn");
    expect(circ(r.pos(), 11025.0 / (kTestFrames - 1)) < 0.003, "a note plays from the cue at the sample's own speed");
    r.g->noteOff(1, 0);
    r.run(44100);
    expect(std::fabs(r.speed()) < 0.02, "the platter stops after the key (motor paused)");
  }
  {  // key track: an octave up plays twice as fast
    Rig r;
    r.g->noteOn(1, 72, 1.0f, 0);
    r.run(5512);
    expect(circ(r.pos(), 2.0 * 5512.0 / (kTestFrames - 1)) < 0.003, "key track: C6 plays at twice the speed");
  }
  {  // Notes off: keys do nothing, the motor selector decides
    Rig r([](PluginState& s) { s.params[sp::TtNotes] = sp::TnOff; });
    r.g->noteOn(1, 60, 1.0f, 0);
    r.run(4096);
    expect(r.pos() == 0.0f && std::fabs(r.speed()) < 1e-6, "Notes off: a key does not move the platter");
  }
}

// ---- the reference laws, measured (FL's own scratcher run as a local test
// oracle on the same sample, states and control changes; see PLAN.md). The
// reference starts its deck one block after the sample arrives, so its frame
// F is Gridline's frame F - block. Control changes: {frame, tag, value, flags}
// (flags 1: the control's own units; 33: "from MIDI", 0..2^30 across the range).
struct RefEvent {
  long frame;
  int tag, value, flags;
};
struct RefPoint {
  long frame;
  float l, r;
};
struct RefCase {
  const char* name;
  float v[14];
  int block;
  std::vector<RefEvent> events;
  std::vector<RefPoint> points;
};

const std::vector<RefCase>& refCases() {
  static const std::vector<RefCase> c = {
    {"fwd", {0.0f, 1.0f, 0.5f, 0.25f, 0.5f, 0.78039217f, 0.0f, 0.5f, 1.0f, 0.5f, 0.907999992f, 0.999000013f, 0.400000006f, 9.0f}, 512, {}, {{3000, 0.174442485f, 0.195118189f}, {9000, 0.149798483f, -0.293868512f}, {17000, 0.0381163731f, 0.0794034302f}, {26000, -0.154973656f, -0.510995507f}, {36000, 0.107503928f, 0.420033664f}, {44000, 0.154875055f, 0.377254277f}}},
    {"rev", {0.0f, 1.0f, 0.5f, 0.25f, 0.5f, 0.78039217f, 0.0f, 0.5f, -1.0f, 0.5f, 0.907999992f, 0.999000013f, 0.400000006f, 9.0f}, 512, {}, {{3000, -0.00210738601f, -0.11088112f}, {9000, -0.239575118f, 0.570758402f}, {17000, -0.201588809f, 0.0968076363f}, {26000, -0.0584046841f, -0.435803533f}, {36000, 0.0938868597f, -0.301565379f}, {44000, -0.230658814f, -0.251354545f}}},
    {"fast", {0.0f, 1.0f, 1.0f, 1.0f, 0.5f, 0.78039217f, 0.0f, 0.5f, 1.0f, 0.5f, 0.907999992f, 0.999000013f, 0.400000006f, 9.0f}, 512, {}, {{3000, -0.241283908f, 0.392396271f}, {9000, -0.0238366984f, 0.253901362f}, {17000, 0.209225446f, 0.288105577f}, {26000, 0.0140242595f, 0.146007866f}, {36000, -0.206286922f, 0.343443424f}, {44000, -0.207229733f, -0.0086763408f}}},
    {"slowacc", {0.0f, 1.0f, 0.5f, 0.0f, 0.5f, 0.78039217f, 0.0f, 0.5f, 1.0f, 0.5f, 0.907999992f, 0.999000013f, 0.400000006f, 9.0f}, 512, {}, {{3000, 0.0175648853f, 0.0261221267f}, {9000, 0.0594048612f, 0.0821385682f}, {17000, 0.10808327f, 0.119846664f}, {26000, 0.152869478f, 0.232898608f}, {36000, 0.120202765f, -0.0414376743f}, {44000, 0.187096149f, 0.170489624f}}},
    {"steps", {0.0f, 1.0f, 0.5f, 0.25f, 0.5f, 0.78039217f, 0.0f, 0.5f, 1.0f, 0.5f, 0.907999992f, 0.999000013f, 0.400000006f, 9.0f}, 512, {{10000, 2, 25, 1}, {20000, 3, 80, 1}, {30000, 8, -1, 1}}, {{3000, 0.174442485f, 0.195118189f}, {9000, 0.149798483f, -0.293868512f}, {17000, 0.0261469372f, 0.146321282f}, {26000, -0.00579827465f, -0.0495868959f}, {36000, 0.108362846f, -0.197802082f}, {44000, -0.223963022f, 0.0787813291f}}},
    {"hold", {0.0f, 1.0f, 0.5f, 0.25f, 0.5f, 0.78039217f, 1.0f, 0.5f, 0.0f, 0.5f, 0.907999992f, 0.999000013f, 0.400000006f, 9.0f}, 512, {{5000, 0, 6000, 1}, {15000, 0, 15000, 1}, {25000, 0, 3000, 1}, {35000, 0, 29000, 1}}, {{3000, -3.9482701e-12f, -7.89654019e-12f}, {9000, 0.191132113f, -0.0698380023f}, {17000, -0.231900528f, 0.4547593f}, {26000, -0.248199686f, 0.161774755f}, {36000, 0.200567976f, 0.413817644f}, {44000, -0.0230660103f, -0.122911088f}}},
    {"hold_s1_0", {0.0f, 1.0f, 0.5f, 0.25f, 0.5f, 0.78039217f, 1.0f, 0.5f, 0.0f, 0.0f, 0.907999992f, 0.999000013f, 0.400000006f, 9.0f}, 512, {{5000, 0, 6000, 1}, {15000, 0, 15000, 1}, {25000, 0, 3000, 1}, {35000, 0, 29000, 1}}, {{3000, -3.9482701e-12f, -7.89654019e-12f}, {9000, -0.149056986f, -0.237713426f}, {17000, 0.204362348f, 0.551522672f}, {26000, -0.284212947f, -0.332259536f}, {36000, -0.147947788f, -0.296557903f}, {44000, -0.0967850685f, -0.107278273f}}},
    {"hold_s1_1", {0.0f, 1.0f, 0.5f, 0.25f, 0.5f, 0.78039217f, 1.0f, 0.5f, 0.0f, 1.0f, 0.907999992f, 0.999000013f, 0.400000006f, 9.0f}, 512, {{5000, 0, 6000, 1}, {15000, 0, 15000, 1}, {25000, 0, 3000, 1}, {35000, 0, 29000, 1}}, {{3000, -3.9482701e-12f, -7.89654019e-12f}, {9000, 0.122278854f, -0.141138867f}, {17000, 0.000282621884f, -0.39022395f}, {26000, -0.037006177f, 0.202874362f}, {36000, 0.182693228f, -0.640062034f}, {44000, -0.0573442504f, -0.157713458f}}},
    {"hold_s2_0", {0.0f, 1.0f, 0.5f, 0.25f, 0.5f, 0.78039217f, 1.0f, 0.5f, 0.0f, 0.5f, 0.0f, 0.999000013f, 0.400000006f, 9.0f}, 512, {{5000, 0, 6000, 1}, {15000, 0, 15000, 1}, {25000, 0, 3000, 1}, {35000, 0, 29000, 1}}, {{3000, -3.9482701e-12f, -7.89654019e-12f}, {9000, -0.141929433f, 0.0897236764f}, {17000, 0.0155651383f, -0.619775295f}, {26000, 0.0521717966f, -0.380762696f}, {36000, 0.052305758f, -0.545383096f}, {44000, 0.0518936962f, 0.296646088f}}},
    {"hold_s2_1", {0.0f, 1.0f, 0.5f, 0.25f, 0.5f, 0.78039217f, 1.0f, 0.5f, 0.0f, 0.5f, 1.0f, 0.999000013f, 0.400000006f, 9.0f}, 512, {{5000, 0, 6000, 1}, {15000, 0, 15000, 1}, {25000, 0, 3000, 1}, {35000, 0, 29000, 1}}, {{3000, -3.9482701e-12f, -7.89654019e-12f}, {9000, -0.0311946645f, 0.181349248f}, {17000, -0.172822893f, 0.24656713f}, {26000, -0.0173883736f, 0.117263943f}, {36000, 0.0974539071f, 0.0602382459f}, {44000, 0.180950955f, 0.0654414296f}}},
    {"hold_s3_0", {0.0f, 1.0f, 0.5f, 0.25f, 0.5f, 0.78039217f, 1.0f, 0.5f, 0.0f, 0.5f, 0.907999992f, 0.0f, 0.400000006f, 9.0f}, 512, {{5000, 0, 6000, 1}, {15000, 0, 15000, 1}, {25000, 0, 3000, 1}, {35000, 0, 29000, 1}}, {{3000, -3.9482701e-12f, -7.89654019e-12f}, {9000, 0.00104738527f, -0.00155764981f}, {17000, 0.00408892659f, -0.00584447384f}, {26000, -0.193145022f, -0.112571888f}, {36000, 0.198997259f, -0.394996464f}, {44000, 1.8459863e-05f, -2.74531358e-05f}}},
    {"hold_s4_0", {0.0f, 1.0f, 0.5f, 0.25f, 0.5f, 0.78039217f, 1.0f, 0.5f, 0.0f, 0.5f, 0.907999992f, 0.999000013f, 0.0f, 9.0f}, 512, {{5000, 0, 6000, 1}, {15000, 0, 15000, 1}, {25000, 0, 3000, 1}, {35000, 0, 29000, 1}}, {{3000, -3.9482701e-12f, -7.89654019e-12f}, {9000, -0.0611083061f, 0.0909196436f}, {17000, 0.0470217764f, 0.172382966f}, {26000, -0.00584454229f, -0.0085354764f}, {36000, -0.175238788f, 0.176629618f}, {44000, -0.0694065392f, -0.540051877f}}},
    {"hold_s4_1", {0.0f, 1.0f, 0.5f, 0.25f, 0.5f, 0.78039217f, 1.0f, 0.5f, 0.0f, 0.5f, 0.907999992f, 0.999000013f, 1.0f, 9.0f}, 512, {{5000, 0, 6000, 1}, {15000, 0, 15000, 1}, {25000, 0, 3000, 1}, {35000, 0, 29000, 1}}, {{3000, -3.9482701e-12f, -7.89654019e-12f}, {9000, -0.135299802f, 0.350091487f}, {17000, -0.24754931f, 0.48131156f}, {26000, 0.271079659f, -0.165998861f}, {36000, -0.108174287f, -0.071814388f}, {44000, -0.0840735286f, -0.0124734128f}}},
    {"rot", {0.25f, 1.0f, 0.5f, 0.25f, 0.5f, 0.78039217f, 1.0f, 0.5f, 0.0f, 0.5f, 0.907999992f, 0.999000013f, 0.400000006f, 10.0f}, 512, {}, {{3000, -0.19322899f, 0.235089093f}, {9000, 0.165556833f, 0.239125252f}, {17000, -0.00622731633f, 0.00926045887f}, {26000, 0.0f, 0.0f}, {36000, 0.0f, 0.0f}, {44000, 0.0f, 0.0f}}},
    {"panL", {0.0f, 1.0f, 0.5f, 0.25f, 0.0f, 0.78039217f, 0.0f, 0.5f, 1.0f, 0.5f, 0.907999992f, 0.999000013f, 0.400000006f, 9.0f}, 512, {}, {{3000, 0.174442485f, 5.81497488e-07f}, {9000, 0.149798483f, -8.75796388e-07f}, {17000, 0.0381163731f, 2.36640659e-07f}, {26000, -0.154973656f, -1.52288533e-06f}, {36000, 0.107503928f, 1.25179781e-06f}, {44000, 0.154875055f, 1.12430541e-06f}}},
    {"panR", {0.0f, 1.0f, 0.5f, 0.25f, 1.0f, 0.78039217f, 0.0f, 0.5f, 1.0f, 0.5f, 0.907999992f, 0.999000013f, 0.400000006f, 9.0f}, 512, {}, {{3000, 5.19879109e-07f, 0.195118189f}, {9000, 4.46434257e-07f, -0.293868512f}, {17000, 1.13595647e-07f, 0.0794034302f}, {26000, -4.61857496e-07f, -0.510995507f}, {36000, 3.20386675e-07f, 0.420033664f}, {44000, 4.61563644e-07f, 0.377254277f}}},
    {"muteauto", {0.0f, 1.0f, 0.5f, 0.25f, 0.5f, 0.78039217f, 0.0f, 0.5f, 1.0f, 0.5f, 0.907999992f, 0.999000013f, 0.400000006f, 9.0f}, 512, {{10000, 1, 1, 1}, {20000, 1, 0, 1}, {25000, 5, 255, 1}}, {{3000, 0.174442485f, 0.195118189f}, {9000, 0.149798483f, -0.293868512f}, {17000, 0.0f, 0.0f}, {26000, -0.198265031f, -0.653740346f}, {36000, 0.137756392f, 0.538234532f}, {44000, 0.198458135f, 0.483416677f}}},
    {"auto_tr", {0.0f, 1.0f, 0.5f, 0.25f, 0.5f, 0.78039217f, 0.0f, 0.5f, 0.0f, 0.5f, 0.907999992f, 0.999000013f, 0.400000006f, 9.0f}, 512, {{5000, 8, 1073741824, 33}, {15000, 8, 536870912, 33}, {25000, 8, 0, 33}, {35000, 8, 805306368, 33}}, {{3000, -3.9482701e-12f, -7.89654019e-12f}, {9000, -0.194208816f, 0.287743509f}, {17000, -0.10737057f, -0.353836387f}, {26000, 0.0286354944f, -0.453004986f}, {36000, 0.211404651f, 0.411210507f}, {44000, 0.217464134f, -0.109032772f}}},
    {"auto_speed", {0.0f, 1.0f, 0.5f, 0.25f, 0.5f, 0.78039217f, 0.0f, 0.5f, 1.0f, 0.5f, 0.907999992f, 0.999000013f, 0.400000006f, 9.0f}, 512, {{5000, 2, 268435456, 33}, {15000, 2, 1073741824, 33}, {25000, 4, 0, 33}, {30000, 5, 536870912, 33}}, {{3000, 0.174442485f, 0.195118189f}, {9000, 0.18217738f, 0.31109485f}, {17000, -0.234768316f, 0.346494079f}, {26000, -0.169507265f, 0.00192687486f}, {36000, 0.0884670243f, 5.66253618e-07f}, {44000, -0.0123721231f, -9.80547156e-07f}}},
    {"auto_hold", {0.0f, 1.0f, 0.5f, 0.25f, 0.5f, 0.78039217f, 0.0f, 0.5f, 0.0f, 0.5f, 0.907999992f, 0.999000013f, 0.400000006f, 9.0f}, 512, {{5000, 6, 1073741824, 33}, {6000, 0, 357913941, 33}, {15000, 13, 1073741824, 33}, {25000, 9, 0, 33}}, {{3000, -3.9482701e-12f, -7.89654019e-12f}, {9000, -0.240283638f, -0.361686409f}, {17000, -0.0472511947f, 0.255633116f}, {26000, 0.268000752f, 0.229184911f}, {36000, -0.0143050095f, 0.0212743971f}, {44000, 0.0f, 0.0f}}},
    {"auto_vol", {0.0f, 1.0f, 0.5f, 0.25f, 0.5f, 0.78039217f, 0.0f, 0.5f, 1.0f, 0.5f, 0.907999992f, 0.999000013f, 0.400000006f, 9.0f}, 512, {{5000, 5, 268435456, 33}, {15000, 5, 805306368, 33}, {25000, 4, 268435456, 33}, {35000, 5, 107374182, 33}}, {{3000, 0.174442485f, 0.195118189f}, {9000, 0.0481768623f, -0.0945113897f}, {17000, 0.0365840532f, 0.0762113258f}, {26000, -0.14874354f, -0.247026041f}, {36000, 0.0141636655f, 0.0276698451f}, {44000, 0.0194568112f, 0.0236971248f}}},
    {"auto_acc", {0.0f, 1.0f, 0.5f, 0.25f, 0.5f, 0.78039217f, 0.0f, 0.5f, 1.0f, 0.5f, 0.907999992f, 0.999000013f, 0.400000006f, 9.0f}, 512, {{3000, 3, 536870912, 33}, {15000, 2, 357913941, 33}, {25000, 7, 357913941, 33}}, {{3000, 0.174442485f, 0.195118189f}, {9000, 0.136151671f, 0.199765995f}, {17000, -0.1569556f, 0.202427104f}, {26000, -0.140936032f, -0.281377256f}, {36000, -0.129227161f, 0.0592336468f}, {44000, -0.0192251746f, 0.145602301f}}},
    {"auto_sm", {0.0f, 1.0f, 0.5f, 0.25f, 0.5f, 0.78039217f, 1.0f, 0.5f, 0.0f, 0.5f, 0.907999992f, 0.999000013f, 0.400000006f, 9.0f}, 512, {{3000, 9, 322122547, 33}, {4000, 0, 536870912, 33}, {15000, 10, 153391689, 33}, {16000, 0, 119304647, 33}, {25000, 12, 357913941, 33}, {26000, 0, 715827882, 33}, {35000, 11, 536870912, 33}}, {{3000, -3.9482701e-12f, -7.89654019e-12f}, {9000, -0.103897326f, -0.120671012f}, {17000, 0.163630739f, 0.11928378f}, {26000, 0.012290189f, -0.0660068914f}, {36000, 0.0534541905f, 0.0806788355f}, {44000, 0.0f, 0.0f}}},
  };
  return c;
}

void testMeasured() {
  static const double lo[14] = {0, 0, 0, 0, -128, 0, 0, 0, -1, 0, 0, 0, 0, 0};
  static const double hi[14] = {30000, 1, 100, 100, 128, 255, 1, 100, 1, 1000, 1000, 1000, 1000, 18};
  double worst = 0;
  for (const RefCase& rc : refCases()) {
    PluginState st = defaultPluginState("gl.turntable");
    sp::turntableFromReference(rc.v, 14, st.params);
    st.data = testSample();
    ChannelAux aux;
    packs::prepareAux(st, aux);
    auto g = packs::createGenerator("gl.turntable");
    ProcessContext ctx;
    ctx.sampleRate = kSr;
    const long frames = 44100 - rc.block;
    std::vector<float> L(static_cast<size_t>(frames), 0.f), R(static_cast<size_t>(frames), 0.f);
    size_t ei = 0;
    for (long f0 = 0; f0 < frames;) {
      while (ei < rc.events.size() && rc.events[ei].frame - rc.block <= f0) {
        const RefEvent& e = rc.events[ei++];
        const double n = (e.flags & 0x20) ? e.value / 1073741824.0 : (e.value - lo[e.tag]) / (hi[e.tag] - lo[e.tag]);
        st.params[static_cast<size_t>(sp::turntableParamOfReference(e.tag))] = static_cast<float>(sp::turntableValueOfReference(e.tag, n));
      }
      long n = std::min<long>(rc.block, frames - f0);
      const long next = ei < rc.events.size() ? rc.events[ei].frame - rc.block : frames;
      if (next > f0 && next < f0 + n) n = next - f0;
      g->process(st, aux, ctx, L.data() + f0, R.data() + f0, static_cast<int>(n));
      f0 += n;
    }
    double err = 0;
    for (const RefPoint& p : rc.points) {
      const size_t i = static_cast<size_t>(p.frame - rc.block);
      err = std::max({err, std::fabs(static_cast<double>(L[i]) - p.l), std::fabs(static_cast<double>(R[i]) - p.r)});
    }
    worst = std::max(worst, err);
    char nm[96];
    std::snprintf(nm, sizeof nm, "measured law: %s", rc.name);
    // The references were captured on x86_64; on other CPUs (Apple Silicon fuses multiply-adds) the
    // tiny rounding differences grow through a second of platter motion, so the bound is looser.
#if defined(__x86_64__) || defined(_M_X64)
    const double tol = 2e-4;
#else
    const double tol = 5e-3;
#endif
    expect(err < tol, nm);
    if (err > 1e-6) std::printf("# %s: %.3g\n", rc.name, err);
  }
  std::printf("# measured laws: worst difference %.3g\n", worst);
}

// ---- scratching (the editor and automation drive the hand through Hold + Position)
void testScratch() {
  Rig r([](PluginState& s) {
    s.params[sp::TtNotes] = sp::TnOff;
    s.params[sp::TtHold] = 1.f;
  });
  r.run(4410);
  expect(r.pos() < 1e-4f, "the hand holds the record at Position");
  // A scratch gesture: Position steps forward over 0.2 s, then back.
  double maxPos = 0;
  for (int k = 0; k < 20; ++k) {
    r.set(sp::TtPosition, 0.02f * static_cast<float>(k + 1));
    r.run(441, 441);
    maxPos = std::max(maxPos, static_cast<double>(r.pos()));
  }
  r.run(22050);
  expect(near(r.pos(), 0.4, 0.002), "the platter follows the hand to the new Position");
  expect(maxPos > 0.2, "the play position moves during the gesture");
  r.set(sp::TtPosition, 0.1f);
  r.run(22050);
  expect(near(r.pos(), 0.1, 0.002), "pulled back");
  // Turns: a whole revolution further plays the same spot after a full turn.
  r.set(sp::TtTurns, 1.f);
  double peakSpeed = 0;
  for (int k = 0; k < 40; ++k) {
    r.run(1102, 1102);
    peakSpeed = std::max(peakSpeed, std::fabs(static_cast<double>(r.speed())));
  }
  expect(circ(r.pos(), 0.1) < 0.003 && peakSpeed > 0.5, "Turns +1: one more revolution, back on the same spot");
  // Letting go: the motor takes over.
  r.set(sp::TtHold, 0.f);
  r.set(sp::TtTurns, 0.f);
  r.set(sp::TtTransport, 2.f);
  r.run(44100);
  expect(r.speed() > 0.5, "let go: the motor runs the record forward");
  expect(r.finite(), "scratching stays finite");
}

void testExtremes() {
  const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
  const float vals[] = {nan, inf, -inf, -1e30f, 1e30f, 0.f, 1.f};
  bool fin = true;
  for (float v : vals)
    for (int k = 0; k < sp::TtCount; ++k) {
      Rig r([&](PluginState& s) { s.params[static_cast<size_t>(k)] = v; });
      r.g->noteOn(1, 60, 1.0f, 0);
      r.run(2048, 512);
      r.set(sp::TtHold, 1.f);
      r.run(2048, 512);
      fin = fin && r.finite() && std::isfinite(r.pos()) && std::isfinite(r.speed());
    }
  expect(fin, "every parameter at NaN / infinity / huge stays finite");
  {  // all at once, both modes
    Rig r([&](PluginState& s) {
      for (float& p : s.params) p = nan;
    });
    r.g->noteOn(1, 60, 1.0f, 0);
    r.run(4096);
    expect(r.finite(), "all parameters NaN: finite");
  }
  {  // fastest everything
    Rig r([](PluginState& s) {
      s.params[sp::TtSpeed] = 2.f, s.params[sp::TtAccel] = 1.f, s.params[sp::TtTransport] = 2.f, s.params[sp::TtVolume] = 1.f;
      s.params[sp::TtNotes] = sp::TnOff;
    });
    r.run(44100);
    r.set(sp::TtHold, 1.f);
    for (int k = 0; k < 4; ++k) r.set(sp::TtSmooth1 + k, k % 2 ? 0.f : 1.f);
    r.set(sp::TtTurns, 9.f);
    r.run(44100);
    r.set(sp::TtTurns, -9.f);
    r.run(44100);
    expect(r.finite(), "extreme settings and turns stay finite");
  }
  // The reference mappings never cast a non-finite value.
  float v[14];
  for (float& x : v) x = nan;
  std::vector<float> p;
  sp::turntableFromReference(v, 14, p);
  bool ok = p.size() == static_cast<size_t>(sp::TtCount);
  for (float x : p) ok = ok && std::isfinite(x);
  expect(ok, "reference values NaN: defaults");
  bool auto_ok = true;
  for (int t = -1; t < 15; ++t)
    auto_ok = auto_ok && std::isfinite(sp::turntableValueOfReference(t, std::nan(""))) && std::isfinite(sp::turntableValueOfReference(t, 1e300));
  expect(auto_ok, "automation values NaN / huge: finite");
  std::vector<float> nanParams(sp::TtCount, nan);
  sp::turntableToReference(nanParams, v);
  ok = true;
  for (float x : v) ok = ok && std::isfinite(x);
  expect(ok, "export of NaN parameters: finite");
}

void testBlocks() {
  auto render = [](int block) {
    Rig r([](PluginState& s) {
      s.params[sp::TtNotes] = sp::TnOff;
      s.params[sp::TtTransport] = 2.f;
      s.params[sp::TtPan] = 0.4f;
    });
    r.run(30000, block);
    return r.L;
  };
  const std::vector<float> a = render(512), b = render(64), c = render(17);
  // The first block's targets differ (they are taken at block ends); the rest must agree.
  double d = 0;
  for (size_t i = 1024; i < a.size(); ++i) d = std::max({d, std::fabs(static_cast<double>(a[i]) - b[i]), std::fabs(static_cast<double>(a[i]) - c[i])});
  expect(d < 1e-6, "block-size invariant with steady controls");
}

void testRates() {
  for (double sr : {48000.0, 96000.0, 22050.0}) {
    Rig r(
        [](PluginState& s) {
          s.params[sp::TtNotes] = sp::TnOff;
          s.params[sp::TtTransport] = 2.f;
          s.params[sp::TtAccel] = 1.f;
        },
        false, sr);
    r.run(static_cast<int64_t>(sr * 2));
    char nm[64];
    std::snprintf(nm, sizeof nm, "%.0f Hz: the motor plays at the sample's own speed", sr);
    expect(near(r.speed(), 1.0, 0.02) && r.finite(), nm);
  }
}

// ---- states
void testStates() {
  {  // short state padded
    PluginState s;
    s.type = "gl.turntable";
    s.params = {0.25f, 0.f, 1.5f};
    normalizePluginState(s);
    expect(s.params.size() == static_cast<size_t>(sp::TtCount), "short state padded");
    expect(near(s.params[sp::TtSpeed], 1.5, 1e-6) && near(s.params[sp::TtVolume], 0.78, 1e-6), "short state keeps its values, defaults after");
  }
  {  // a J-205 state (17 values, the built-in phrase without a file) migrates and plays
    PluginState s;
    s.type = "gl.turntable";
    s.params = {0.f, 0.f, 1.f, 0.25f, 0.f, 0.78f, 0.f, 0.5f, 1.f, 0.5f, 0.908f, 0.999f, 0.4f, 0.f, 2.f, 1.f, 0.f};
    expect(s.params.size() == static_cast<size_t>(sp::kTtJ205Count), "a J-205 state has 17 values");
    normalizePluginState(s);
    expect(s.params.size() == static_cast<size_t>(sp::TtCount) && s.params[sp::TtTimecode] == 0 && s.params[sp::TtRiaa] == 0 &&
               s.params[sp::TtZoom] == 0 && s.params[sp::TtEmpty] == 0,
           "J-205 state: the new settings take their defaults");
    Rig r([&](PluginState& st) { st = s; }, true);
    r.g->noteOn(1, 60, 1.0f, 0);
    expect(r.run(22050) > 0.05, "J-205 state without a file plays the built-in sound");
  }
  // Presets apply cleanly and play.
  const PluginInfo* info = findPlugin("gl.turntable");
  bool presets = true;
  for (const auto& pr : info->presets) {
    Rig r([&](PluginState& st) {
      for (const auto& [k, v] : pr.values) st.params[static_cast<size_t>(k)] = v;
    });
    r.g->noteOn(1, 60, 1.0f, 0);
    r.run(22050);
    r.set(sp::TtTransport, 2.f);
    r.run(22050);
    presets = presets && r.finite() && r.L.size() == 44100;
  }
  expect(presets, "factory presets play");
}

// ---- FL state
std::vector<uint8_t> flState(uint32_t version, const float (&v)[14], const std::string& path, int zoom = 1, int timecode = -1, int riaa = 0) {
  std::vector<uint8_t> b;
  auto u32 = [&](uint32_t x) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(x >> (8 * i)));
  };
  u32(version);
  for (float f : v) {
    uint32_t x;
    std::memcpy(&x, &f, 4);
    u32(x);
  }
  u32(static_cast<uint32_t>(path.size()));
  b.insert(b.end(), path.begin(), path.end());
  if (version > 1001000) b.push_back(static_cast<uint8_t>(zoom));
  if (version >= 1001002) u32(static_cast<uint32_t>(timecode)), b.push_back(static_cast<uint8_t>(riaa));
  return b;
}

void testFlState() {
  flp::File file;
  Song song;
  flp::Report report;
  flp::Context ctx{file, song, report};
  auto decode = [&](const std::vector<uint8_t>& b, flp::PluginMapping& m) {
    return flp::fx2::decodeEmbedded(ctx, "Fruity Scratcher", b.data(), b.size(), m);
  };
  // The values of FL's render test: hand on the record at 11.1 %, motor paused.
  const float v[14] = {0.111f, 1.0f, 0.5f, 0.25f, 0.5f, 0.78f, 1.0f, 0.5f, 0.0f, 0.5f, 0.908f, 0.999f, 0.4f, 9.0f};
  flp::PluginMapping m;
  const bool ok = decode(flState(1001000, v, "C:\\Samples\\Breaks\\loop.wav"), m);
  expect(ok && m.glType == "gl.turntable" && m.state.type == "gl.turntable", "Fruity Scratcher imports as the Turntable instrument");
  auto P = [&](const flp::PluginMapping& x, int k) { return static_cast<double>(x.state.params[static_cast<size_t>(k)]); };
  if (ok && m.state.params.size() == static_cast<size_t>(sp::TtCount)) {
    expect(near(P(m, sp::TtPosition), 0.111, 1e-6) && P(m, sp::TtMute) == 0 && near(P(m, sp::TtSpeed), 1.0, 1e-6) &&
               near(P(m, sp::TtAccel), 0.25, 1e-6),
           "position, sound, speed, acceleration");
    expect(near(P(m, sp::TtPan), 0.0, 1e-6) && near(P(m, sp::TtVolume), 0.78, 1e-6) && P(m, sp::TtHold) == 1 && near(P(m, sp::TtSense), 0.5, 1e-6),
           "pan, volume, hold, sensitivity");
    expect(P(m, sp::TtTransport) == 1 && P(m, sp::TtTurns) == 0 && P(m, sp::TtNotes) == sp::TnOff, "motor paused, no extra turns, played by its motor");
    expect(near(P(m, sp::TtSmooth1), 0.5, 1e-6) && near(P(m, sp::TtSmooth2), 0.908, 1e-6) && near(P(m, sp::TtSmooth3), 0.999, 1e-6) &&
               near(P(m, sp::TtSmooth4), 0.4, 1e-6),
           "smooth 1..4");
    expect(P(m, sp::TtZoom) == 0 && P(m, sp::TtTimecode) == 0 && P(m, sp::TtRiaa) == 0 && P(m, sp::TtEmpty) == 0, "version 1001000: no options tail");
  }
  expect(m.state.data.find('\\') == std::string::npos && m.state.data.size() >= 8 && m.state.data.compare(m.state.data.size() - 8, 8, "loop.wav") == 0,
         "the sample path is kept");
  // The automation mapping: tag -> parameter, the control's steps.
  expect(m.param && m.value && m.rawNorm, "automation mapping set");
  if (m.param && m.value && m.rawNorm) {
    expect(m.param(0) == sp::TtPosition && m.param(8) == sp::TtTransport && m.param(13) == sp::TtTurns && m.param(14) == -1, "automation targets");
    expect(near(m.value(5, 0.25), 64.0 / 255.0, 1e-9) && near(m.value(2, 1.0), 2.0, 1e-9) && m.value(8, 0.75) == 2 && m.value(13, 1.0) == 9 &&
               near(m.value(4, 0.0), -1.0, 1e-9),
           "automation values on the control's steps (volume a quarter = 64 / 255)");
    expect(near(m.rawNorm(0, 15000), 0.5, 1e-9) && near(m.rawNorm(4, 0), 0.5, 1e-9) && near(m.rawNorm(8, 1), 1.0, 1e-9), "recorded control values");
  }
  // Sound off (stored 0) = Mute, reverse motor, rotations; no path = the empty deck.
  const float w[14] = {0.f, 0.f, 1.f, 0.f, 1.f, 1.f, 0.f, 1.f, -1.f, 0.f, 0.f, 0.f, 0.f, 11.f};
  flp::PluginMapping m2;
  const bool ok2 = decode(flState(1001001, w, "", 2), m2);
  expect(ok2 && P(m2, sp::TtMute) == 1 && P(m2, sp::TtTransport) == 0 && P(m2, sp::TtSpeed) == 2 && P(m2, sp::TtPan) == 1 && P(m2, sp::TtTurns) == 2,
         "mute, reverse, double speed, pan right, +2 turns");
  expect(ok2 && m2.state.data.empty() && P(m2, sp::TtEmpty) == 1 && P(m2, sp::TtZoom) == 1, "no sample: the empty deck; the large editor");
  // The current layout: timecode format and phono preamp.
  flp::PluginMapping m3;
  expect(decode(flState(1001002, v, "", 1, 4, 1), m3) && P(m3, sp::TtTimecode) == 5 && P(m3, sp::TtRiaa) == 1 && P(m3, sp::TtZoom) == 0,
         "version 1001002: timecode format and phono preamp kept");
  // The older integer layout.
  {
    std::vector<uint8_t> b;
    auto u32 = [&](uint32_t x) {
      for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(x >> (8 * i)));
    };
    u32(3);
    u32(2), b.push_back('a'), b.push_back('b');  // the unused string
    b.push_back(0);                             // sound off
    u32(0);                                     // unused
    u32(100), u32(40), u32(255), u32(static_cast<uint32_t>(-128));  // speed, accel, volume, pan
    b.push_back(1);                                                  // hold
    u32(80), u32(1), u32(12);                                        // sensitivity, transport forward, rotations
    u32(100), u32(200), u32(300), u32(400);                          // smooth
    const std::string p = "C:\\x\\old.wav";
    u32(static_cast<uint32_t>(p.size()));
    b.insert(b.end(), p.begin(), p.end());
    flp::PluginMapping m4;
    const bool ok4 = decode(b, m4);
    expect(ok4 && P(m4, sp::TtMute) == 1 && near(P(m4, sp::TtSpeed), 2.0, 1e-6) && near(P(m4, sp::TtAccel), 0.4, 1e-6) &&
               near(P(m4, sp::TtVolume), 1.0, 1e-6) && near(P(m4, sp::TtPan), -1.0, 1e-6) && P(m4, sp::TtHold) == 1 &&
               near(P(m4, sp::TtSense), 0.8, 1e-6) && P(m4, sp::TtTransport) == 2 && P(m4, sp::TtTurns) == 3 && near(P(m4, sp::TtSmooth4), 0.4, 1e-6),
           "the older integer layout imports");
    expect(ok4 && m4.state.data.size() >= 7 && m4.state.data.compare(m4.state.data.size() - 7, 7, "old.wav") == 0, "the older layout's sample path");
  }
  std::vector<uint8_t> junk(9, 0xFF);
  flp::PluginMapping m5;
  expect(!decode(junk, m5), "a broken state is not imported");
  std::vector<uint8_t> nanState = flState(1001002, v, "");
  for (size_t i = 4; i < 60; ++i) nanState[i] = 0xFF;  // every value NaN
  flp::PluginMapping m6;
  bool fin = decode(nanState, m6);
  for (float x : m6.state.params) fin = fin && std::isfinite(x);
  expect(fin, "NaN values import as defaults");
}

// Export: a Turntable state written as the scratcher and read back.
void testExport() {
  flp::enc::EncodeContext ec;
  PluginState g = defaultPluginState("gl.turntable");
  g.params[sp::TtNotes] = sp::TnOff;
  g.params[sp::TtCue] = 0.f;
  g.params[sp::TtPosition] = 0.3f;
  g.params[sp::TtSpeed] = 1.5f;
  g.params[sp::TtPan] = -0.5f;
  g.params[sp::TtTransport] = 0.f;
  g.params[sp::TtTurns] = -3.f;
  g.params[sp::TtSmooth3] = 0.7f;
  g.params[sp::TtZoom] = 1.f;
  g.params[sp::TtTimecode] = 3.f;
  g.params[sp::TtRiaa] = 1.f;
  g.data = "/samples/loop.wav";
  const float v[14] = {0.111f, 1.0f, 0.5f, 0.25f, 0.5f, 0.78f, 1.0f, 0.5f, 0.0f, 0.5f, 0.908f, 0.999f, 0.4f, 9.0f};
  const std::vector<uint8_t> origin = flState(1001000, v, "C:\\a.wav");
  const flp::enc::Result r = flp::enc::encode(g, "Fruity Scratcher", &origin, ec);
  expect(r.hasState && r.flName == "Fruity Scratcher", "exported as Fruity Scratcher");
  expect(r.state.size() >= 4 && r.state[0] == 0x2A && r.state[1] == 0x46 && r.state[2] == 0x0F, "written in the current layout (1001002)");
  flp::PluginMapping back;
  const bool ok = flp::enc::decodeState("Fruity Scratcher", r.state, back, ec);
  bool same = ok && back.state.params.size() == g.params.size();
  for (int k = 0; same && k < sp::TtCount; ++k)
    if (k != sp::TtEmpty) same = std::fabs(back.state.params[static_cast<size_t>(k)] - g.params[static_cast<size_t>(k)]) < 1e-6f;
  expect(same, "export round trip: every setting comes back");
  expect(ok && back.state.data.size() >= 8 && back.state.data.compare(back.state.data.size() - 8, 8, "loop.wav") == 0, "export round trip: the sample");
  expect(r.mismatched.empty() && !r.dataMismatch, "export: nothing reported as lost");
  // An empty deck exports without a path.
  PluginState e = g;
  e.data.clear();
  e.params[sp::TtEmpty] = 1.f;
  const flp::enc::Result re = flp::enc::encode(e, "Fruity Scratcher", &origin, ec);
  flp::PluginMapping eb;
  expect(flp::enc::decodeState("Fruity Scratcher", re.state, eb, ec) && eb.state.data.empty() && eb.state.params[sp::TtEmpty] == 1,
         "an empty deck exports without a sample");
}

// FL's own render test (installed FL only): the slot plugin becomes a channel
// on that insert, with its sample and its recorded position events; the FL
// export writes it back and it imports the same again.
void testRenderTest() {
  const std::string f = "/Applications/FL Studio 2026.app/Contents/Resources/FL/Data/System/Render tests/Fruity Scratcher.flp";
  std::error_code ec;
  if (!fs::exists(f, ec)) {
    std::printf("# FL render test not installed: project import skipped\n");
    return;
  }
  Song s;
  flp::Report rep;
  std::string err;
  expect(importFlp(f, s, rep, &err), "FL's render test imports");
  const Channel* tt = nullptr;
  for (const Channel& ch : s.channels)
    if (ch.plugin.type == "gl.turntable") tt = &ch;
  expect(tt != nullptr && tt->mixerTrack != kNoId && tt->mixerTrack != s.mixer.front().id, "a Turntable channel routed to the plugin's insert");
  bool leftInSlot = false;
  for (const MixerTrack& t : s.mixer)
    for (const EffectSlot& sl : t.slots) leftInSlot = leftInSlot || sl.plugin.type == "gl.turntable" || sl.plugin.type == "gl.scratcher";
  expect(!leftInSlot, "no instrument left in a mixer slot");
  if (!tt) return;
  expect(tt->plugin.params.size() == static_cast<size_t>(sp::TtCount) && tt->plugin.params[sp::TtHold] == 1 && tt->plugin.params[sp::TtEmpty] == 0,
         "its settings come along");
  expect(!tt->plugin.data.empty() && tt->plugin.data.find('%') == std::string::npos, "its sample path is resolved");
  expect(fs::exists(fs::u8path(tt->plugin.data), ec), "its sample is found in FL's factory content");
  size_t points = 0;
  float lo = 1, hi = 0;
  const ParamId pid = channelPluginParam(tt->id, sp::TtPosition);
  for (const Pattern& p : s.patterns)
    for (const PatternAutomation& a : p.automation)
      if (a.owner == pid.owner && a.index == pid.index) {
        points += a.points.size();
        for (const AutoPoint& q : a.points) lo = std::min(lo, q.value), hi = std::max(hi, q.value);
      }
  std::printf("# render test: %zu recorded position points (%.3f .. %.3f)\n", points, static_cast<double>(lo), static_cast<double>(hi));
  expect(points > 10 && hi > lo, "the recorded platter movements drive the Turntable's Position");
  // FL export and back.
  flp::ExportOptions opt;
  flp::ExportReport xr;
  flp::Bytes out;
  expect(flp::exportFlp(s, out, xr, opt), "the project exports to FL");
  Song s2;
  flp::Report rep2;
  expect(importFlpBytes(out, s2, rep2, &err), "the exported project imports");
  const Channel* t2 = nullptr;
  for (const Channel& ch : s2.channels)
    if (ch.plugin.type == "gl.turntable") t2 = &ch;
  bool same = t2 && t2->plugin.params.size() == tt->plugin.params.size() && t2->plugin.data == tt->plugin.data;
  for (size_t k = 0; same && k < tt->plugin.params.size(); ++k) same = std::fabs(t2->plugin.params[k] - tt->plugin.params[k]) < 1e-6f;
  expect(same, "FL round trip: the Turntable comes back with its settings and sample");
}

void testOldEffect() {
  auto fx = packs::createEffect("gl.scratcher");
  expect(fx != nullptr, "the Turntable FX effect is still created");
  if (!fx) return;
  PluginState st = defaultPluginState("gl.scratcher");
  ProcessContext ctx;
  ctx.sampleRate = kSr;
  std::vector<float> L(4410), R(4410);
  for (size_t i = 0; i < L.size(); ++i) L[i] = R[i] = static_cast<float>(0.3 * std::sin(2 * 3.14159265358979 * 220 * static_cast<double>(i) / kSr));
  fx->process(st, ctx, L.data(), R.data(), static_cast<int>(L.size()));
  bool fin = true;
  for (size_t i = 0; i < L.size(); ++i) fin = fin && std::isfinite(L[i]) && std::isfinite(R[i]);
  expect(fin, "Turntable FX still processes");
}

}  // namespace

int main() {
  testCatalogue();
  testBuiltin();
  testNotes();
  testMeasured();
  testScratch();
  testExtremes();
  testBlocks();
  testRates();
  testStates();
  testFlState();
  testExport();
  testRenderTest();
  testOldEffect();
  return finish();
}
