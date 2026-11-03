// K-37 (K-AD): Sweep Kick's second engine and its channel envelopes. Older states keep the first
// engine and its sound; FL imports and new instances take the new one.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>
#include "../../src/core/PluginInfo.h"
#include "../../src/core/flp/FlpEncode.h"
#include "../../src/engine/ChannelIns.h"
#include "../../src/engine/Generators.h"
#include "../../src/packs/Packs.h"
#include "../../src/packs/models/drums.h"
#include "../Check.h"

using namespace gl;
using namespace gl::packs::models::dr;

namespace {

constexpr float kFlLevel1 = 100.0f / 128.0f;  // FL's default note velocity: level 1

struct Out {
  std::vector<float> L, R;
};
Out render(const PluginState& st, int frames, int offAt = -1, double pitch = 60, float vel = kFlLevel1, int block = 256, double bpm = 140) {
  Out o;
  o.L.assign(static_cast<size_t>(frames), 0.f);
  o.R.assign(static_cast<size_t>(frames), 0.f);
  auto g = packs::createGenerator(st.type);
  ChannelAux aux;
  if (!st.ins.empty()) {
    auto ins = std::make_shared<InsSettings>();
    if (parseIns(st.ins, *ins) && ins->active()) aux.ins = ins;
  }
  ProcessContext ctx;
  ctx.sampleRate = 44100;
  ctx.tempo = bpm;
  for (int pos = 0; pos < frames; pos += block) {
    const int n = std::min(block, frames - pos);
    if (pos == 0) g->noteOn(1, pitch, vel, 0);
    if (offAt >= pos && offAt < pos + n) g->noteOff(1, offAt - pos);
    ctx.tick = pos * bpm * ctx.ppq / (60.0 * 44100);
    g->process(st, aux, ctx, o.L.data() + pos, o.R.data() + pos, n);
  }
  return o;
}
double energy(const std::vector<float>& a, int from = 0, int to = -1) {
  double e = 0;
  for (size_t i = static_cast<size_t>(from); i < (to < 0 ? a.size() : static_cast<size_t>(to)); ++i) e += static_cast<double>(a[i]) * a[i];
  return e;
}
bool same(const std::vector<float>& a, const std::vector<float>& b) { return a == b; }

std::vector<uint8_t> flState(int version, const int ctl[6], const std::vector<std::vector<int>>& env = {}, int cutoff = 256, int type = 0) {
  std::vector<uint8_t> b;
  auto i32 = [&](int v) {
    for (int k = 0; k < 4; ++k) b.push_back(static_cast<uint8_t>(static_cast<uint32_t>(v) >> (8 * k)));
  };
  i32(version);
  for (int k = 0; k < 6; ++k) i32(ctl[k]);
  i32(4479715);
  if (version == 1) {
    for (int t = 0; t < 5; ++t) {
      std::vector<int> r = {t == 1 ? 4 : 0, 0, 100, 20000, 20000, 30000, 50, 20000, 0, 100, 20000, 0, 32950, 0, 0, 0, t == 1 ? -101 : 0};
      if (static_cast<size_t>(t) < env.size() && !env[static_cast<size_t>(t)].empty()) r = env[static_cast<size_t>(t)];
      for (int v : r) i32(v);
    }
    for (int v : {0, 0, cutoff, 0, 0, type, 0}) i32(v);
  }
  return b;
}
bool decode(const std::vector<uint8_t>& st, flp::PluginMapping& m) {
  flp::enc::EncodeContext c;
  if (!flp::enc::decodeState("Fruit Kick", st, m, c)) return false;
  normalizePluginState(m.state);
  return true;
}

}  // namespace

int main() {
  const PluginInfo* info = findPlugin("gl.sweepkick");
  expect(info && info->params.size() == SkCount, "sweep kick: parameter list");
  expect(insSupported("gl.sweepkick"), "sweep kick: the channel takes the envelope page");

  // ---------------------------------------------------------- old states and presets keep their sound
  {
    PluginState old = defaultPluginState("gl.sweepkick");
    old.params.resize(SkModel);  // a state saved before the engine marker
    normalizePluginState(old);
    expect(old.params.size() == SkCount && old.params[SkModel] == 0.f, "old state: first engine");
    expect(defaultPluginState("gl.sweepkick").params[SkModel] == 1.f, "new instance: new engine");
    for (size_t p = 0; p < info->presets.size(); ++p) expect(presetState(*info, p).params[SkModel] == 0.f, "factory preset: first engine");
    PluginState shorter;
    shorter.type = "gl.sweepkick";
    shorter.params = {0.f, -600.f};
    normalizePluginState(shorter);
    expect(shorter.params[SkModel] == 0.f && shorter.params[SkAmpDecay] == 128.f, "very short state: first engine, other defaults");

    // The first engine's law, sample by sample (the formula it has always had).
    const float vel = 0.9f;
    const Out o = render(old, 4000, -1, 60, vel);
    const double sr = 44100, ref = 130.8128;
    const double f0 = ref, f1 = ref * std::pow(2.0, -600.0 / 1200.0) * 0.5;
    const double fk = std::exp(-1.0 / (0.001 * std::pow(2.0, 64.0 / 256.0 * 8.0) * sr));
    const double dec = 0.02 * std::pow(2.0, 128.0 / 256.0 * 7.0);
    const float ampK = static_cast<float>(std::exp(-6.907755 / (dec * sr)));
    double ph = 0;
    float amp = 1;
    bool ok = true;
    for (int t = 0; t < 4000; ++t) {
      ph += (f1 + (f0 - f1) * std::pow(fk, static_cast<double>(t))) / sr;
      ph -= std::floor(ph);
      double x = std::sin(2 * 3.14159265358979323846 * ph);
      x += 0.5 * (x >= 0 ? 1.0 : -1.0) * std::exp(-t / (0.002 * sr));
      x *= amp;
      amp *= ampK;
      if (std::fabs(static_cast<float>(x) * 0.8f * vel - o.L[static_cast<size_t>(t)]) > 1e-5f) ok = false;
    }
    expect(ok, "old state: same samples as the first engine");
    const Out off = render(old, 4000, 400, 60, vel);
    expect(same(o.L, off.L), "old state: a note off changes nothing");
    PluginState oldEnv = old;
    InsSettings s;
    s.t[InsVol].on = true;
    s.t[InsVol].attack = 8000;
    oldEnv.ins = formatIns(s);
    expect(!same(render(oldEnv, 4000, -1, 60, vel).L, o.L), "old state: an envelope switched on in the page works");
  }

  // ---------------------------------------------------------- the new engine's laws
  {
    const int def[6] = {0, -600, 64, 128, 32, 0};
    flp::PluginMapping m;
    expect(decode(flState(0, def), m), "FL state v0 decodes");
    expect(m.state.params[SkModel] == 1.f && m.state.ins.empty() && m.exact, "FL import: new engine, neutral channel");
    const Out o = render(m.state, 8000);
    // Click 32 starts at phase 2^29 (45 degrees), level 1.5, then the constant-power centre pan.
    expect(std::fabs(o.L[0] - 0.75f) < 1e-3f && o.L == o.R, "start level and phase");
    PluginState noClick = m.state;
    noClick.params[SkClick] = 0;
    expect(std::fabs(render(noClick, 16).L[0]) < 1e-6f, "click 0 starts at phase 0");
    // Half-rate key tracking: an octave up raises the sweep by half an octave, not a full one.
    const Out hi = render(m.state, 8000, -1, 72);
    expect(!same(o.L, hi.L), "the key moves the sweep");
    // Note off without a volume envelope ends the note within the 10 ms fade.
    const Out cut = render(m.state, 8000, 2000);
    expect(energy(cut.L, 2000 + 442) < 1e-12 && same(std::vector<float>(cut.L.begin(), cut.L.begin() + 2000), std::vector<float>(o.L.begin(), o.L.begin() + 2000)),
           "note off ends the note");
    // Block size does not change the result.
    expect(same(render(m.state, 8000, -1, 60, kFlLevel1, 64).L, render(m.state, 8000, -1, 60, kFlLevel1, 512).L), "block size invariance");
    // Velocity: FL's level law (11^(v/100) - 1) / 10.
    const float half = render(m.state, 16, -1, 60, 50.0f / 128.0f).L[0];
    expect(std::fabs(half - 0.75f * static_cast<float>((std::pow(11.0, 0.5) - 1.0) / 10.0)) < 1e-3f, "velocity law");
    // Distortion: the host law at the kick's fixed settings; dry 1 - d, wet 3.3 d, pre 0.1.
    PluginState d = m.state;
    d.params[SkDist] = 128;
    const Out dist = render(d, 8000);
    expect(!same(dist.L, o.L) && energy(dist.L) > 0.0, "distortion changes the sound");
    for (float v : {0.0f, 1.0f}) {
      PluginState x = m.state;
      for (int k = 0; k < SkModel; ++k) x.params[static_cast<size_t>(k)] = static_cast<float>(v > 0.5f ? info->params[static_cast<size_t>(k)].max : info->params[static_cast<size_t>(k)].min);
      const Out e = render(x, 8000);
      bool finite = true;
      for (float s : e.L) finite = finite && std::isfinite(s);
      expect(finite, "extremes stay finite");
    }
  }

  // ---------------------------------------------------------- FL's envelope records
  {
    const int def[6] = {0, -600, 64, 128, 32, 0};
    flp::PluginMapping plain, vol, pitch, filt;
    expect(decode(flState(1, def), plain) && plain.state.ins.empty(), "v1 state with neutral records: no envelope text");
    std::vector<std::vector<int>> envV(5);
    envV[1] = {4, -1, 100, 5000, 20000, 5000, 20, 20000, 0, 100, 20000, 0, 32950, 0, 0, 0, -101};
    expect(decode(flState(1, def, envV), vol) && !vol.state.ins.empty(), "volume envelope record imports");
    InsSettings s;
    expect(parseIns(vol.state.ins, s) && s.t[InsVol].on && s.t[InsVol].attack == 5000 && s.t[InsVol].sustain == 20 && s.t[InsVol].tensionR == -101,
           "volume envelope values are FL's raw values");
    const Out a = render(plain.state, 20000), b = render(vol.state, 20000);
    expect(!same(a.L, b.L) && energy(b.L, 0, 64) < energy(a.L, 0, 64), "volume envelope shapes the sound (attack)");
    std::vector<std::vector<int>> envP(5);
    envP[4] = {0, -1, 100, 100, 20000, 10000, 0, 20000, 1200, 100, 20000, 0, 32950, 0, 0, 0, 0};
    expect(decode(flState(1, def, envP), pitch), "pitch envelope record decodes");
    expect(same(render(pitch.state, 20000).L, a.L), "the pitch envelope does not reach the oscillator");
    expect(decode(flState(1, def, {}, 100, 1), filt) && !filt.state.ins.empty(), "levels record: filter imports");
    expect(energy(render(filt.state, 20000).L) < energy(a.L), "the channel filter runs");
    // Release: with a volume envelope the note off starts the release, it does not cut.
    std::vector<std::vector<int>> envR(5);
    envR[1] = {4, -1, 100, 100, 20000, 30000, 128, 20000, 0, 100, 20000, 0, 32950, 0, 0, 0, 0};
    flp::PluginMapping rel;
    expect(decode(flState(1, def, envR), rel), "release state decodes");
    expect(energy(render(rel.state, 8000, 2000).L, 3000) > 1e-3, "release runs after the note off");
  }

  return finish();
}
