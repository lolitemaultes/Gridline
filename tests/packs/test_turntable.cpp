// Gridline — Turntable (gl.turntable) tests: catalogue entry and presets, the
// built-in phrase, notes (motor, cue, key track), the motor switch and speed,
// the hand spring following Position (scratch gestures move the play
// position), Turns, mute, parameter extremes, block-size invariance, old /
// short states, the FL state mapping (hand-made bytes and, when installed,
// FL's own render-test project) and the old Turntable FX effect.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>
#include "../Check.h"
#include "core/AudioBuffer.h"
#include "core/PluginInfo.h"
#include "core/Song.h"
#include "core/WavWriter.h"
#include "core/flp/Flp.h"
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
constexpr double kPi = 3.14159265358979323846;

// A 1 s stereo test sample: a 441 Hz sine (100 cycles per revolution).
std::string testSample() {
  static std::string path;
  if (!path.empty()) return path;
  const fs::path d = fs::temp_directory_path() / "gl_turntable_test";
  std::error_code ec;
  fs::create_directories(d, ec);
  const size_t n = static_cast<size_t>(kSr);
  AudioBuffer b(2, n);
  for (size_t i = 0; i < n; ++i)
    for (size_t c = 0; c < 2; ++c) b.data[c * n + i] = static_cast<float>(0.5 * std::sin(2 * kPi * 441.0 * static_cast<double>(i) / kSr));
  path = (d / "sine.wav").string();
  writeWav(path, b, static_cast<int>(kSr), 24);
  return path;
}

struct Rig {
  PluginState st;
  ChannelAux aux;
  ProcessContext ctx;
  std::unique_ptr<Generator> g;
  std::vector<float> L, R;
  explicit Rig(const std::function<void(PluginState&)>& tweak = nullptr, bool builtIn = false) {
    st = defaultPluginState("gl.turntable");
    if (!builtIn) st.data = testSample();
    if (tweak) tweak(st);
    packs::prepareAux(st, aux);
    g = packs::createGenerator("gl.turntable");
    ctx.sampleRate = kSr;
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
// Distance on the circle (positions wrap at 1).
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
  const PluginInfo* old = findPlugin("gl.scratcher");
  expect(old && old->kind == PluginKind::Effect && old->name == "Turntable FX", "the effect keeps its type, shown as Turntable FX");
  expect(packs::createGenerator("gl.turntable") != nullptr, "created");
  expect(sp::turntableTelemetry(packs::createGenerator("gl.turntable").get()) != nullptr, "telemetry of the runtime");
}

void testNotes() {
  {  // default: the motor is paused, so nothing plays until a key
    Rig r;
    expect(r.run(4410) < 1e-6, "silent without notes (motor paused)");
    r.g->noteOn(1, 60, 1.0f, 0);
    const double rms = r.run(22050);
    expect(rms > 0.1, "sound after noteOn");
    // Cue + motor starts at the cue at full speed: half a second at 1x = half a revolution.
    expect(near(r.pos(), 0.5, 0.01), "a note plays from the cue at the sample's speed");
    expect(near(r.speed(), 1.0, 0.01), "platter at 1x on C5");
    r.g->noteOff(1, 0);
    r.run(44100);
    expect(std::fabs(r.speed()) < 0.02, "the platter winds down after the key is let go");
  }
  {  // key track: an octave up turns twice as fast
    Rig r;
    r.g->noteOn(1, 72, 1.0f, 0);
    r.run(11025);
    expect(near(r.pos(), 0.5, 0.01) && near(r.speed(), 2.0, 0.02), "key track: C6 plays at 2x");
  }
  {  // key track off: every key at Speed
    Rig r([](PluginState& s) { s.params[sp::TtKeyTrack] = 0; });
    r.g->noteOn(1, 48, 1.0f, 0);
    r.run(11025);
    expect(near(r.speed(), 1.0, 0.01), "key track off: every key at Speed");
  }
  {  // cue point
    Rig r([](PluginState& s) { s.params[sp::TtCue] = 0.25f; });
    r.run(4410);
    r.g->noteOn(1, 60, 1.0f, 0);
    r.run(4410);
    expect(near(r.pos(), 0.35, 0.01), "a note starts at the cue point");
  }
  {  // Notes off: keys do nothing
    Rig r([](PluginState& s) { s.params[sp::TtNotes] = sp::TnOff; });
    r.g->noteOn(1, 60, 1.0f, 0);
    expect(r.run(8820) < 1e-6, "Notes off: keys leave the motor alone");
  }
  {  // the built-in phrase plays when no sample is loaded
    Rig r(nullptr, true);
    r.g->noteOn(1, 60, 1.0f, 0);
    expect(r.run(22050) > 0.01, "the built-in phrase plays without a loaded sample");
  }
}

void testMotor() {
  {  // forward: the platter spins up with inertia and loops the record
    Rig r([](PluginState& s) {
      s.params[sp::TtNotes] = sp::TnOff;
      s.params[sp::TtTransport] = 2;
    });
    r.run(441);
    const float early = r.speed();
    r.run(88200);
    expect(early > 0.0f && early < 0.5f, "the motor accelerates (inertia)");
    expect(near(r.speed(), 1.0, 0.01), "forward at Speed 100 % = the sample's own speed");
    expect(r.run(4410) > 0.1, "sound while the motor runs");
  }
  {  // reverse at double speed
    Rig r([](PluginState& s) {
      s.params[sp::TtNotes] = sp::TnOff;
      s.params[sp::TtTransport] = 0;
      s.params[sp::TtSpeed] = 2;
    });
    r.run(88200);
    expect(near(r.speed(), -2.0, 0.02), "reverse at Speed 200 %");
  }
  {  // more acceleration reaches the speed sooner
    auto at = [](float acc) {
      Rig r([acc](PluginState& s) {
        s.params[sp::TtNotes] = sp::TnOff;
        s.params[sp::TtTransport] = 2;
        s.params[sp::TtAccel] = acc;
      });
      r.run(4410);
      return r.speed();
    };
    expect(at(1.0f) > at(0.0f) + 0.1f, "Acceleration sets how fast the platter follows the motor");
  }
  {  // pause winds the platter down
    Rig r([](PluginState& s) {
      s.params[sp::TtNotes] = sp::TnOff;
      s.params[sp::TtTransport] = 2;
    });
    r.run(88200);
    r.set(sp::TtTransport, 1);
    r.run(132300);
    expect(std::fabs(r.speed()) < 0.01, "Pause stops the platter");
  }
}

void testHand() {
  Rig r([](PluginState& s) {
    s.params[sp::TtNotes] = sp::TnOff;
    s.params[sp::TtTransport] = 2;
    s.params[sp::TtHold] = 1;
    s.params[sp::TtPosition] = 0.5f;
  });
  r.run(88200);
  expect(circ(r.pos(), 0.5) < 0.005, "Hold: the platter settles on Position");
  // A scratch gesture: Position moves forward by 0.1 over 0.1 s.
  for (int i = 1; i <= 20; ++i) {
    r.set(sp::TtPosition, 0.5f + 0.005f * static_cast<float>(i));
    r.run(220);
  }
  const double moving = r.run(2205);
  r.run(44100);
  expect(circ(r.pos(), 0.6) < 0.005, "a scratch gesture moves the play position");
  expect(moving > 0.01, "the record sounds while it is moved");
  expect(r.run(4410) < 0.01, "the held record is quiet when the hand stops");
  // Turns: one more revolution forward plays through the record once more.
  r.set(sp::TtTurns, 1);
  bool forward = false;
  for (int i = 0; i < 40; ++i) {
    r.run(441);
    forward = forward || r.speed() > 0.5f;
  }
  r.run(88200);
  expect(forward && circ(r.pos(), 0.6) < 0.005, "Turns adds whole revolutions to the hand");
  // Let go: the motor takes the platter back to speed from where the hand left it.
  r.set(sp::TtHold, 0);
  r.run(88200);
  expect(near(r.speed(), 1.0, 0.01), "let go: the motor takes over");
  // Mute cuts the output.
  r.set(sp::TtMute, 1);
  r.run(4410);
  expect(r.run(4410) < 1e-4, "Mute cuts the output");
}

void testPan() {
  Rig r([](PluginState& s) {
    s.params[sp::TtNotes] = sp::TnOff;
    s.params[sp::TtTransport] = 2;
    s.params[sp::TtPan] = 1;
  });
  r.run(44100);
  double l = 0, rr = 0;
  for (size_t i = 22050; i < r.L.size(); ++i) l += std::fabs(r.L[i]), rr += std::fabs(r.R[i]);
  expect(l < 1e-3 * rr && rr > 1, "pan right leaves the right side only");
}

void testExtremes() {
  const PluginInfo* info = findPlugin("gl.turntable");
  bool ok = true;
  for (int side = 0; side < 2; ++side) {
    Rig r([&](PluginState& s) {
      for (size_t k = 0; k < s.params.size(); ++k) s.params[k] = static_cast<float>(side ? info->params[k].max : info->params[k].min);
    });
    r.g->noteOn(1, 127, 1.0f, 0);
    r.run(44100, 512);
    r.g->noteOn(2, 0, 1.0f, 0);
    r.run(22050, 37);
    ok = ok && r.finite();
  }
  {  // out-of-range and non-finite values are clamped
    Rig r([](PluginState& s) {
      for (float& v : s.params) v = 1e9f;
      s.params[sp::TtPosition] = NAN;
    });
    r.g->noteOn(1, 60, 1.0f, 0);
    r.run(22050);
    ok = ok && r.finite();
  }
  expect(ok, "parameter extremes give finite output");
  {  // a missing sample is silent, not a crash
    Rig r([](PluginState& s) { s.data = "/nonexistent/turntable.wav"; });
    r.g->noteOn(1, 60, 1.0f, 0);
    expect(r.run(4410) == 0.0, "missing sample: silent");
  }
}

void testBlocks() {
  auto render = [](int block) {
    Rig r([](PluginState& s) {
      s.params[sp::TtNotes] = sp::TnCue;
      s.params[sp::TtTransport] = 2;
      s.params[sp::TtAccel] = 0.6f;
    });
    r.run(1000, block);
    r.g->noteOn(1, 64, 1.0f, 0);
    r.run(20000, block);
    r.set(sp::TtHold, 1);
    r.set(sp::TtPosition, 0.3f);
    r.run(20000, block);
    r.g->noteOff(1, 0);
    r.set(sp::TtHold, 0);
    r.run(20000, block);
    return r.L;
  };
  const auto a = render(64), b = render(1024), c = render(333);
  double d = 0;
  for (size_t i = 0; i < a.size(); ++i) d = std::max({d, static_cast<double>(std::fabs(a[i] - b[i])), static_cast<double>(std::fabs(a[i] - c[i]))});
  expect(d < 1e-6, "block-size invariant");
}

void testRates() {
  // The platter feels the same at 96 kHz: same speed after the same time.
  auto at = [](double sr) {
    Rig r([](PluginState& s) {
      s.params[sp::TtNotes] = sp::TnOff;
      s.params[sp::TtTransport] = 2;
    });
    r.ctx.sampleRate = sr;
    r.run(static_cast<int64_t>(sr * 0.05));
    return r.speed();
  };
  expect(near(at(44100), at(96000), 0.02), "motor inertia is the same at 44.1 and 96 kHz");
}

void testStates() {
  PluginState s;
  s.type = "gl.turntable";
  s.params = {0.25f, 0.f, 1.5f};
  normalizePluginState(s);
  expect(s.params.size() == static_cast<size_t>(sp::TtCount), "short state padded");
  expect(near(s.params[sp::TtSpeed], 1.5, 1e-6) && near(s.params[sp::TtVolume], 0.78, 1e-6) && near(s.params[sp::TtNotes], sp::TnCue, 1e-6),
         "short state keeps its values, defaults after");
  // Presets apply cleanly and play.
  const PluginInfo* info = findPlugin("gl.turntable");
  bool presets = true;
  for (const auto& pr : info->presets) {
    Rig r([&](PluginState& st) {
      for (const auto& [k, v] : pr.values) st.params[static_cast<size_t>(k)] = v;
    });
    r.g->noteOn(1, 60, 1.0f, 0);
    r.run(22050);
    presets = presets && r.finite();
  }
  expect(presets, "factory presets play");
}

// ---- FL state
std::vector<uint8_t> flState(const float (&v)[14], const std::string& path) {
  std::vector<uint8_t> b;
  auto u32 = [&](uint32_t x) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(x >> (8 * i)));
  };
  u32(1001000);
  for (float f : v) {
    uint32_t x;
    std::memcpy(&x, &f, 4);
    u32(x);
  }
  u32(static_cast<uint32_t>(path.size()));
  b.insert(b.end(), path.begin(), path.end());
  return b;
}

void testFlState() {
  flp::File file;
  Song song;
  flp::Report report;
  flp::Context ctx{file, song, report};
  // The values of FL's render test: hand on the record at 11.1 %, motor paused.
  const float v[14] = {0.111f, 1.0f, 0.5f, 0.25f, 0.5f, 0.78f, 1.0f, 0.5f, 0.0f, 0.5f, 0.908f, 0.999f, 0.4f, 9.0f};
  const std::vector<uint8_t> b = flState(v, "C:\\Samples\\Breaks\\loop.wav");
  flp::PluginMapping m;
  const bool ok = flp::fx2::decodeEmbedded(ctx, "Fruity Scratcher", b.data(), b.size(), m);
  expect(ok && m.glType == "gl.turntable" && m.state.type == "gl.turntable", "Fruity Scratcher imports as the Turntable instrument");
  auto P = [&](int k) { return static_cast<double>(m.state.params[static_cast<size_t>(k)]); };
  if (ok && m.state.params.size() == static_cast<size_t>(sp::TtCount)) {
    expect(near(P(sp::TtPosition), 0.111, 1e-6) && P(sp::TtMute) == 0 && near(P(sp::TtSpeed), 1.0, 1e-6) && near(P(sp::TtAccel), 0.25, 1e-6),
           "position, sound, speed, acceleration");
    expect(near(P(sp::TtPan), 0.0, 1e-6) && near(P(sp::TtVolume), 0.78, 1e-6) && P(sp::TtHold) == 1 && near(P(sp::TtSense), 0.5, 1e-6),
           "pan, volume, hold, sensitivity");
    expect(P(sp::TtTransport) == 1 && P(sp::TtTurns) == 0 && P(sp::TtNotes) == sp::TnOff, "motor paused, no extra turns, played by its motor");
    expect(near(P(sp::TtSmooth1), 0.5, 1e-6) && near(P(sp::TtSmooth2), 0.908, 1e-6) && near(P(sp::TtSmooth3), 0.999, 1e-6) &&
               near(P(sp::TtSmooth4), 0.4, 1e-6),
           "smooth 1..4");
  }
  expect(m.state.data.find('\\') == std::string::npos && m.state.data.size() >= 8 &&
             m.state.data.compare(m.state.data.size() - 8, 8, "loop.wav") == 0,
         "the sample path is kept");
  // Sound off (stored 0) = Mute, reverse motor, rotations.
  const float w[14] = {0.f, 0.f, 1.f, 0.f, 1.f, 1.f, 0.f, 1.f, -1.f, 0.f, 0.f, 0.f, 0.f, 11.f};
  const std::vector<uint8_t> b2 = flState(w, "");
  flp::PluginMapping m2;
  const bool ok2 = flp::fx2::decodeEmbedded(ctx, "Fruity Scratcher", b2.data(), b2.size(), m2);
  expect(ok2 && m2.state.params[sp::TtMute] == 1 && m2.state.params[sp::TtTransport] == 0 && m2.state.params[sp::TtSpeed] == 2 &&
             m2.state.params[sp::TtPan] == 1 && m2.state.params[sp::TtTurns] == 2 && m2.state.data.empty(),
         "mute, reverse, double speed, pan right, +2 turns, no sample");
  std::vector<uint8_t> junk(9, 0xFF);
  flp::PluginMapping m3;
  expect(!flp::fx2::decodeEmbedded(ctx, "Fruity Scratcher", junk.data(), junk.size(), m3), "a broken state is not imported");

  // FL's own render test (installed FL only): the slot plugin becomes a channel on that insert.
  const std::string f = "/Applications/FL Studio 2026.app/Contents/Resources/FL/Data/System/Render tests/Fruity Scratcher.flp";
  std::error_code ec;
  if (!fs::exists(f, ec)) {
    std::printf("# FL render test not installed: slot import skipped\n");
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
  if (tt) {
    expect(tt->plugin.params.size() == static_cast<size_t>(sp::TtCount) && tt->plugin.params[sp::TtHold] == 1, "its settings come along");
    expect(!tt->plugin.data.empty() && tt->plugin.data.find('%') == std::string::npos, "its sample path is resolved");
    expect(fs::exists(fs::u8path(tt->plugin.data), ec), "its sample is found in FL's factory content");
  }
}

void testOldEffect() {
  auto fx = packs::createEffect("gl.scratcher");
  expect(fx != nullptr, "the Turntable FX effect is still created");
  if (!fx) return;
  PluginState st = defaultPluginState("gl.scratcher");
  ProcessContext ctx;
  ctx.sampleRate = kSr;
  std::vector<float> L(4410), R(4410);
  for (size_t i = 0; i < L.size(); ++i) L[i] = R[i] = static_cast<float>(0.3 * std::sin(2 * kPi * 220 * static_cast<double>(i) / kSr));
  fx->process(st, ctx, L.data(), R.data(), static_cast<int>(L.size()));
  bool fin = true;
  for (size_t i = 0; i < L.size(); ++i) fin = fin && std::isfinite(L[i]) && std::isfinite(R[i]);
  expect(fin, "Turntable FX still processes");
}

}  // namespace

int main() {
  testCatalogue();
  testNotes();
  testMotor();
  testHand();
  testPan();
  testExtremes();
  testBlocks();
  testRates();
  testStates();
  testFlState();
  testOldEffect();
  return finish();
}
