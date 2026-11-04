// Gridline - K-41: reported latency against the delay an impulse really has, for the five effects
// where imported patches sum parallel paths by it. FL imports take the figures FL Studio's plugins
// report; states from before keep the report and the timing they had.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <system_error>
#include <vector>
#include "../Check.h"
#include "fl_corpus.h"
#include "../../src/core/PluginInfo.h"
#include "../../src/core/PluginParams.h"
#include "../../src/core/flp/FlpEncode.h"
#include "../../src/engine/Effects.h"
#include "../../src/packs/Packs.h"
#include "../../src/packs/dynamics/dynamics.h"
#include "../../src/packs/spacefx/spacefx.h"
#include "../../src/packs/spectral/spectral.h"

using namespace gl;
namespace sp = gl::packs::spectral;
namespace sx = gl::packs::spacefx;
namespace dy = gl::packs::dynamics;

namespace {

const int kAt = 8192, kFrames = 24000;

struct Hit {
  int peak = -1;  // largest sample, frames after the input impulse
  int first = -1;
  float value = 0;
};

// An impulse through the effect as the engine runs it (prepared data, the transport as given).
Hit impulse(const PluginState& st, double sr = 44100, int block = 512, bool playing = true, float amp = 0.25f) {
  auto fx = createEffect(st.type);
  fx->prepareHost(st, sr);
  ChannelAux aux;
  packs::prepareAux(st, aux);
  ProcessContext ctx;
  ctx.sampleRate = sr;
  ctx.tempo = 140;
  ctx.playing = playing;
  std::vector<float> L(static_cast<size_t>(kFrames), 0.f), R(static_cast<size_t>(kFrames), 0.f);
  L[static_cast<size_t>(kAt)] = R[static_cast<size_t>(kAt)] = amp;
  for (int i = 0; i < kFrames;) {
    const int n = std::min(block, kFrames - i);
    ctx.tick = i * ctx.tempo * ctx.ppq / (60.0 * sr);
    fx->processAux(st, aux, ctx, L.data() + i, R.data() + i, n);
    i += n;
  }
  Hit h;
  for (int i = 0; i < kFrames; ++i) {
    const float x = L[static_cast<size_t>(i)];
    if (h.first < 0 && std::fabs(x) > 1e-3f) h.first = i - kAt;
    if (std::fabs(x) > std::fabs(h.value)) h.value = x, h.peak = i - kAt;
  }
  return h;
}

int reported(const PluginState& st, double sr = 44100) {
  auto fx = createEffect(st.type);
  return fx->latencyFor(st, sr);
}

void line(const std::string& what, int want, int got) {
  char b[200];
  std::snprintf(b, sizeof b, "%s: %d (got %d)", what.c_str(), want, got);
  expect(want == got, b);
}

std::vector<uint8_t> i32s(std::initializer_list<int> vs) {
  std::vector<uint8_t> b;
  for (int v : vs)
    for (int k = 0; k < 4; ++k) b.push_back(static_cast<uint8_t>(static_cast<uint32_t>(v) >> (8 * k)));
  return b;
}

bool decode(const std::string& name, const std::vector<uint8_t>& state, flp::PluginMapping& m, const std::string& factory = "") {
  flp::enc::EncodeContext c;
  c.factoryData = factory;
  if (!flp::enc::decodeState(name, state, m, c)) return false;
  normalizePluginState(m.state);
  return true;
}

// ---------------------------------------------------------------- Time gate (15 samples)
void testTimeGate() {
  PluginState st = defaultPluginState("gl.timegate");
  st.params[pp::TgModel] = 1;
  expect(st.params[pp::TgLatencyLaw] == 0.f, "time gate: a new instance has no latency law");
  line("time gate, from before: reports", 0, reported(st));
  line("time gate, from before: its delay is unchanged", 15, impulse(st).peak);

  PluginState old = st;
  old.params.resize(static_cast<size_t>(pp::TgLatencyLaw));
  normalizePluginState(old);
  expect(old.params[pp::TgLatencyLaw] == 0.f && reported(old) == 0, "time gate: a state saved before the marker stays at 0");

  st.params[pp::TgLatencyLaw] = 1;
  for (double sr : {22050.0, 44100.0, 48000.0, 96000.0})
    for (int block : {64, 512, 1024}) {
      const std::string w = "time gate, import, " + std::to_string(static_cast<int>(sr)) + " / " + std::to_string(block);
      line(w + ": reports", 15, reported(st, sr));
      line(w + ": the impulse arrives", 15, impulse(st, sr, block).peak);
    }
  line("time gate, stopped: passes straight through, as it did", 0, impulse(st, 44100, 512, false).peak);
  PluginState legacy = defaultPluginState("gl.timegate");
  legacy.params[pp::TgLatencyLaw] = 1;
  line("time gate: the legacy engine has none to report", 0, reported(legacy));
}

// ---------------------------------------------------------------- Vintage phaser (44 samples)
void testPhaser() {
  PluginState st = defaultPluginState("gl.vphaser");
  st.params[sx::VpMix] = 0;  // the path around the phaser: it has the delay too
  st.params[sx::VpNoise] = 0;
  const auto with = [&](int law, bool hq) {
    PluginState s = st;
    s.params[sx::VpLatencyLaw] = static_cast<float>(law);
    s.params[sx::VpHq] = hq ? 1.f : 0.f;
    return s;
  };
  expect(st.params[sx::VpLatencyLaw] == 0.f, "phaser: a new instance has no latency law");
  line("phaser, from before: reports", 34, reported(with(0, false)));
  line("phaser, from before: its delay is unchanged", 34, impulse(with(0, false)).peak);
  line("phaser, from before, high quality: reports", 39, reported(with(0, true)));
  line("phaser, from before, high quality: its delay is unchanged", 39, impulse(with(0, true)).peak);
  PluginState old = with(0, false);
  old.params.resize(static_cast<size_t>(sx::VpLatencyLaw));
  normalizePluginState(old);
  expect(old.params[sx::VpLatencyLaw] == 0.f && reported(old) == 34, "phaser: a state saved before the marker stays at 34");

  for (bool hq : {false, true})
    for (double sr : {44100.0, 48000.0, 96000.0})
      for (int block : {64, 512, 1024}) {
        const std::string w = std::string("phaser, import, ") + (hq ? "hq " : "") + std::to_string(static_cast<int>(sr)) + " / " + std::to_string(block);
        line(w + ": reports", 44, reported(with(1, hq), sr));
        line(w + ": the impulse arrives", 44, impulse(with(1, hq), sr, block).peak);
      }
  const std::vector<uint8_t> state = [] {
    std::vector<uint8_t> b = i32s({3});
    for (int k = 0; k < 36; ++k) {
      const std::vector<uint8_t> z = i32s({0});
      b.insert(b.end(), z.begin(), z.end());
    }
    b.push_back(0);
    return b;
  }();
  flp::PluginMapping m;
  expect(decode("Vintage Phaser", state, m) && m.state.params[sx::VpLatencyLaw] == 1.f, "phaser: FL import takes the latency law");
}

// ---------------------------------------------------------------- Pitch shifter (a different algorithm)
// The voice engine's report stays its own: the dry path has exactly that delay and the grains land
// around it. FL's plugin reports less (1228) for an algorithm of its own, so no figure is copied.
void testPitch() {
  PluginState st = defaultPluginState("gl.pitch");
  st.params[sp::PsEngine] = sp::kPsVoice;
  for (int detector : {0, 1})
    for (double sr : {44100.0, 48000.0, 96000.0}) {
      PluginState s = st;
      s.params[sp::PsDetector] = static_cast<float>(detector);
      const std::string w = "pitch shifter, voice, detector " + std::to_string(detector) + " at " + std::to_string(static_cast<int>(sr));
      s.params[sp::PsMix] = 0;
      for (int block : {64, 512}) line(w + ", dry path: the report is its delay, block " + std::to_string(block), reported(s, sr), impulse(s, sr, block).peak);
      s.params[sp::PsMix] = 1;
      expect(std::abs(impulse(s, sr).peak - reported(s, sr)) <= 2, (w + ", grains: an impulse lands with it").c_str());
    }
  line("pitch shifter, voice, detector 0: the figure older projects have", 1670, [&] {
    PluginState s = st;
    s.params[sp::PsDetector] = 0;
    return reported(s);
  }());
  PluginState s = st;
  s.params[sp::PsEngine] = sp::kPsMusic;
  line("pitch shifter, music: nothing compensated", 0, reported(s));
  s.params[sp::PsMix] = 0;
  line("pitch shifter, music: its dry path has no delay", 0, impulse(s).peak);
}

// ---------------------------------------------------------------- Vocoder
void testVocoder() {
  PluginState st = defaultPluginState("gl.vocoder");
  st.params[sp::VxModel] = 1;
  st.params[sp::VxLrEncoding] = 1;
  // The pass-through alone: the bands are silent, the carrier passes.
  st.params[sp::VcWetLevel] = -60;
  st.params[sp::VcModLevel] = -60;
  st.params[sp::VcCarLevel] = 0;
  st.params[sp::VxCarLp] = static_cast<float>(sp::kVxOffHigh);
  const auto with = [&](int law, float enhance) {
    PluginState s = st;
    s.params[sp::VxLatencyLaw] = static_cast<float>(law);
    s.params[sp::VxEnhance] = enhance;
    return s;
  };
  expect(st.params[sp::VxLatencyLaw] == 0.f, "vocoder: a new instance has no latency law");
  line("vocoder, from before: reports", 0, reported(with(0, 0)));
  line("vocoder, from before, enhancer on: reports", 0, reported(with(0, 0.5f)));
  line("vocoder, from before: no delay added", 0, impulse(with(0, 0)).peak);
  line("vocoder, from before, enhancer on: no delay added", 0, impulse(with(0, 0.5f)).peak);
  PluginState old = with(0, 0);
  old.params.resize(static_cast<size_t>(sp::VxLatencyLaw));
  normalizePluginState(old);
  expect(old.params[sp::VxLatencyLaw] == 0.f, "vocoder: a state saved before the marker has no latency law");

  struct R {
    double sr;
    int ms4;
  };
  for (const R& r : {R{22050, 88}, R{44100, 176}, R{48000, 192}, R{88200, 352}, R{96000, 384}, R{192000, 768}}) {
    const std::string w = "vocoder, import at " + std::to_string(static_cast<int>(r.sr));
    line(w + ", plain: reports", 0, reported(with(1, 0), r.sr));
    line(w + ", plain: arrives", 0, impulse(with(1, 0), r.sr).peak);
    line(w + ", +16: reports", 16, reported(with(2, 0), r.sr));
    line(w + ", +16: arrives", 16, impulse(with(2, 0), r.sr).peak);
    line(w + ", enhancer: reports", r.ms4, reported(with(1, 0.5f), r.sr));
    line(w + ", enhancer, +16: reports", r.ms4 + 16, reported(with(2, 0.5f), r.sr));
  }
  for (int block : {64, 1024}) line("vocoder, import, +16, block " + std::to_string(block), 16, impulse(with(2, 0), 44100, block).peak);

  // FL's state flag selects the 16 samples; a shipped program runs as the enhancer stage.
  if (!testfl::flInstalled()) return;
  std::error_code ec;
  std::vector<PluginState> maps;
  namespace fs = std::filesystem;
  for (fs::recursive_directory_iterator it(testfl::flData() + "/Data/Patches/Plugin presets/Effects/Vocodex", ec), end; !ec && it != end; it.increment(ec)) {
    if (it->path().extension() != ".fst") continue;
    flp::PresetResult r;
    if (flp::importFlPreset(it->path().string(), r, testfl::flData()) && r.kind == flp::PresetResult::Plugin) maps.push_back(r.state);
  }
  expect(maps.size() >= 20, "vocoder: the shipped presets decode");
  int with16 = 0, withEnhancer = 0, truthful = 0;
  for (const PluginState& m : maps) {
    with16 += m.params[sp::VxLatencyLaw] == 2.f;
    PluginState s = m;
    s.params[sp::VxLrEncoding] = 1;
    s.params[sp::VxCarSource] = 1;
    s.params[sp::VcWetLevel] = -60;
    s.params[sp::VcModLevel] = -60;
    s.params[sp::VcCarLevel] = 0;
    s.params[sp::VxCarLp] = static_cast<float>(sp::kVxOffHigh);
    withEnhancer += s.params[sp::VxEnhance] > 0;
    for (double sr : {44100.0, 48000.0, 96000.0}) {
      const int want = (s.params[sp::VxEnhance] > 0 ? static_cast<int>(0.004 * sr) : 0) + 16;
      const Hit h = impulse(s, sr);
      truthful += reported(s, sr) == want && h.first == want;
    }
  }
  expect(with16 == static_cast<int>(maps.size()) && withEnhancer == static_cast<int>(maps.size()) - 2, "vocoder: every shipped preset has the flag, all but two the enhancer");
  char b[120];
  std::snprintf(b, sizeof b, "vocoder: reported equals measured, shipped presets, three rates (%d of %d)", truthful, 3 * static_cast<int>(maps.size()));
  expect(truthful == 3 * static_cast<int>(maps.size()), b);
}

// ---------------------------------------------------------------- Enhancer (no change: the figures agree)
void testEnhancer() {
  // Without FL's program data the built-in voicing runs: its report is its real delay.
  flp::PluginMapping m;
  expect(decode("Soundgoodizer", i32s({3, 0, 600}), m) && m.state.type == "gl.enhance", "enhancer: decodes without FL's data");
  const Hit h = impulse(m.state);
  expect(std::abs(h.peak - reported(m.state)) <= 2 && reported(m.state) == 162, "enhancer, built-in voicing: the report is its delay (162, within 2)");
  if (!testfl::flInstalled()) return;
  struct C {
    int mode, want44, want96;
  };
  for (const C& c : {C{0, 153, 334}, C{1, 88, 192}, C{2, 153, 334}, C{3, 176, 384}}) {
    flp::PluginMapping p;
    expect(decode("Soundgoodizer", i32s({3, c.mode, 600}), p, testfl::flData()), "enhancer: decodes");
    const std::string w = "enhancer, FL's program " + std::to_string(c.mode);
    line(w + " at 44100: reports", c.want44, reported(p.state, 44100));
    line(w + " at 96000: reports", c.want96, reported(p.state, 96000));
    line(w + " at 44100: arrives", c.want44, impulse(p.state, 44100).first);
    line(w + " at 96000: arrives", c.want96, impulse(p.state, 96000).first);
  }
}

}  // namespace

int main() {
  testTimeGate();
  testPhaser();
  testPitch();
  testVocoder();
  testEnhancer();
  return finish();
}
