// Gridline: K-2 round 2, Reeverb 2 compatibility. States load the way the
// reference plugin reads them (version 10000 / 10001 layouts, older words below
// 10000 read one knob late, unknown versions ignored), out-of-range values map
// to settings with the same sound, the full model's version 2 (first-tap
// reflections, single-precision LFO phase, block level ramps) is what new
// instances and imports get while older states keep version 1, and a changed
// instance with an old origin layout exports in the current layout. All on
// hand-made states (no FL content).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include "../../src/core/PluginInfo.h"
#include "../../src/core/PluginParams.h"
#include "../../src/core/Song.h"
#include "../../src/core/flp/Flp.h"
#include "../../src/core/flp/FlpEncode.h"
#include "../../src/core/flp/FlpFx.h"
#include "../../src/engine/Effects.h"
#include "../../src/engine/FullReverbRef.h"
#include "../Check.h"

using namespace gl;

namespace {

struct Blob {
  std::vector<uint8_t> b;
  Blob& i32(int32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(static_cast<uint32_t>(v) >> (8 * i)));
    return *this;
  }
  Blob& ints(std::initializer_list<int32_t> v) {
    for (int32_t x : v) i32(x);
    return *this;
  }
  Blob& u8(uint8_t v) {
    b.push_back(v);
    return *this;
  }
};

bool decode(const std::vector<uint8_t>& state, flp::PluginMapping& m) {
  flp::enc::EncodeContext c;
  if (!flp::enc::decodeState("Fruity Reeverb 2", state, m, c)) return false;
  normalizePluginState(m.state);
  return true;
}
float P(const flp::PluginMapping& m, int k) { return m.state.params[static_cast<size_t>(k)]; }
int knob(const flp::PluginMapping& m, int tag) { return static_cast<int>(std::lround(P(m, pp::RvbFLowCut + tag))); }

// Renders an impulse (left input) through a state; returns the stereo output.
std::vector<float> impulse(const PluginState& s, int frames, int block = 512, double sr = 44100) {
  auto fx = createEffect(s.type);
  std::vector<float> out(static_cast<size_t>(frames) * 2), L(static_cast<size_t>(block)), R(static_cast<size_t>(block));
  ProcessContext ctx;
  ctx.sampleRate = sr;
  for (int f0 = 0; f0 < frames; f0 += block) {
    const int n = std::min(block, frames - f0);
    std::fill(L.begin(), L.end(), 0.f), std::fill(R.begin(), R.end(), 0.f);
    if (f0 == 0) L[0] = 1;
    fx->process(s, ctx, L.data(), R.data(), n);
    for (int i = 0; i < n; ++i) out[2 * static_cast<size_t>(f0 + i)] = L[static_cast<size_t>(i)], out[2 * static_cast<size_t>(f0 + i) + 1] = R[static_cast<size_t>(i)];
  }
  return out;
}

PluginState full(std::initializer_list<std::pair<int, float>> v) {
  PluginState s = defaultPluginState("gl.reverb");
  s.params[pp::RvbModel] = 2;
  for (const auto& [k, x] : v) s.params[static_cast<size_t>(k)] = x;
  return s;
}

// Distinct reflection onsets (local peaks over a threshold) in one channel.
int reflectionTaps(const std::vector<float>& o, int ch, int frames) {
  int taps = 0;
  bool in = false;
  for (int i = 1; i < frames; ++i) {
    const bool on = std::fabs(o[2 * static_cast<size_t>(i) + static_cast<size_t>(ch)]) > 2e-3f;
    if (on && !in) ++taps;
    in = on;
  }
  return taps;
}

}  // namespace

int main() {
  using namespace pp;
  char name[200];
  flp::PluginMapping m;

  // ---- loader: version 10001 / 10000 as stored.
  {
    Blob b;
    b.ints({10001, 120, 60, 38, 80, 90, 25, 70, 150, 700, -20, 128, 100, 140, 900, 300}).u8(1).u8(1);
    expect(decode(b.b, m) && m.exact && knob(m, 0) == 120 && knob(m, 5) == 25 && knob(m, 9) == -20 && knob(m, 13) == 900 && knob(m, 14) == 300 &&
               P(m, RvbPreSync) == 1 && P(m, RvbInputMode) == 1 && P(m, RvbFRev) == 1,
           "reeverb 2: version 10001 decodes as stored, version 2 model");
    Blob c;
    c.ints({10000, 120, 60, 38, 80, 90, 25, 70, 150, 700, -20, 128, 100, 140}).u8(1).u8(0);
    expect(decode(c.b, m) && m.exact && knob(m, 12) == 140 && knob(m, 13) == 684 && knob(m, 14) == 0 && P(m, RvbPreSync) == 1 && P(m, RvbInputMode) == 0,
           "reeverb 2: version 10000 has 13 knobs and the flags, modulation at its defaults");
    Blob s;  // 10001 cut short: the modulation knobs missing
    s.ints({10001, 120, 60, 38, 80, 90, 25, 70, 150, 700, -20, 128, 100, 140});
    expect(decode(s.b, m) && knob(m, 12) == 140 && knob(m, 13) == 684 && P(m, RvbPreSync) == 0, "reeverb 2: a short 10001 state keeps the missing knobs at defaults");
  }
  // ---- loader: no version word (oldest layout): the first value is taken as a version, knobs one tag late.
  {
    Blob b;  // 13 knobs + 1 flag byte, as the oldest states
    b.ints({33, 60, 12, 70, 80, 25, 70, 150, 700, 10, 128, 100, 90}).u8(0);
    const bool ok = decode(b.b, m);
    expect(ok && knob(m, 0) == 60 && knob(m, 1) == 12 && knob(m, 2) == 70 && knob(m, 3) == 80 && knob(m, 4) == 25 && knob(m, 5) == 70 &&
               knob(m, 6) == 150 && knob(m, 7) == 300 && knob(m, 8) == 25 && knob(m, 9) == 64 && knob(m, 10) == 100 && knob(m, 11) == 90 && knob(m, 12) == 0,
           "reeverb 2: unversioned state read one knob late, wet from the flag byte");
    expect(ok && !m.exact, "reeverb 2: a bass multiplier beyond the range is approximated (not exact)");
    Blob t;
    t.ints({33, 60, 12, 70, 80, 25, 70, 150, 700, 10, 128, 100, 90}).u8(7).u8(1);
    expect(decode(t.b, m) && knob(m, 12) == 160 && P(m, RvbPreSync) == 0 && P(m, RvbInputMode) == 0,
           "reeverb 2: a short last knob is zero-padded (0x0107 = 263, clamped), no flags");
    Blob v;
    v.ints({5, 120, 60, 38, 80, 90, 25, 70, 150, 700, -20, 128, 100, 140, 900, 300}).u8(1).u8(1);
    expect(decode(v.b, m) && knob(m, 0) == 120 && knob(m, 12) == 140 && knob(m, 13) == 684 && P(m, RvbPreSync) == 0,
           "reeverb 2: a version word under 10000 reads 13 knobs and no flags");
    Blob w;
    w.ints({10002, 120, 60, 38, 80, 90, 25, 70, 150, 700, -20, 128, 100, 140, 900, 300}).u8(1).u8(1);
    expect(decode(w.b, m) && knob(m, 0) == 75 && knob(m, 2) == 0 && knob(m, 11) == 64 && knob(m, 12) == 64 && P(m, RvbPreSync) == 0,
           "reeverb 2: an unknown later version keeps the defaults");
  }
  // ---- out-of-range values with an exact equivalent.
  {
    std::vector<double> v = {40, 0, 50, 100, 15, 40, 0, 500, 0, 128, 128, 87, 0, 684, 0};
    expect(flp::fx::reeverb2Fit(v) && v[1] == 221 && v[6] == 221 && v[7] == 100 && v[8] == 25 && v[9] == 64,
           "reeverb 2: cut 0 = off, crossover 0 = no bass shelf, separation over 64 = merged (exact)");
    std::vector<double> w = {75, 40, 0, 50, 300, 15, 40, 100, 500, 0, 128, 64, 64, 684, 0};
    expect(!flp::fx::reeverb2Fit(w) && w[4] == 100, "reeverb 2: diffusion over 100 is clamped and reported");
    // Crossover 0 sounds like a 100 % bass multiplier (any crossover): the shelf adds nothing.
    PluginState a = full({{RvbFCross, 25.f}, {RvbFBass, 100.f}}), b = full({{RvbFCross, 1200.f}, {RvbFBass, 100.f}});
    const auto oa = impulse(a, 30000), ob = impulse(b, 30000);
    double d = 0;
    for (size_t i = 0; i < oa.size(); ++i) d = std::max(d, static_cast<double>(std::fabs(oa[i] - ob[i])));
    expect(d < 1e-6, "reeverb 2: with a 100 % bass multiplier the crossover has no effect");
  }
  // ---- version 2 reflections: the first tap per side only; version 1 keeps all taps.
  {
    PluginState v2 = full({{RvbFDry, 0.f}, {RvbFWet, 0.f}, {RvbFEr, 128.f}, {RvbFDiffusion, 0.f}});
    PluginState v1 = v2;
    v1.params[RvbFRev] = 0;
    const auto o2 = impulse(v2, 20000), o1 = impulse(v1, 20000);
    const int l2 = reflectionTaps(o2, 0, 20000), r2 = reflectionTaps(o2, 1, 20000), l1 = reflectionTaps(o1, 0, 20000), r1 = reflectionTaps(o1, 1, 20000);
    std::snprintf(name, sizeof name, "reeverb 2 v2: one reflection per side (L %d R %d); v1 keeps the taps (L %d R %d)", l2, r2, l1, r1);
    expect(l2 == 1 && r2 == 1 && l1 >= 6 && r1 >= 6, name);
    // The first tap is the same in both versions (position and level).
    double d = 0;
    for (int i = 0; i < 900; ++i) d = std::max(d, static_cast<double>(std::fabs(o2[2 * static_cast<size_t>(i)] - o1[2 * static_cast<size_t>(i)])));
    expect(d < 1e-7, "reeverb 2 v2: the first reflection is unchanged");
  }
  // ---- migration: a state saved before version 2 (no RvbFRev) keeps version 1.
  {
    PluginState old = full({});
    old.params.resize(static_cast<size_t>(RvbFRev));
    normalizePluginState(old);
    expect(old.params.size() == static_cast<size_t>(RvbCount) && old.params[RvbFRev] == 0, "reeverb 2: older states open as version 1");
    PluginState fresh = defaultPluginState("gl.reverb");
    expect(fresh.params[RvbFRev] == 1, "reeverb 2: new instances are version 2");
    // Old full-model state renders exactly as the version 1 engine.
    PluginState v1 = full({{RvbFModDepth, 900.f}, {RvbFModSpeed, 1500.f}});
    v1.params[RvbFRev] = 0;
    PluginState shortSt = v1;
    shortSt.params.resize(static_cast<size_t>(RvbFRev));
    normalizePluginState(shortSt);
    const auto a = impulse(v1, 40000), b = impulse(shortSt, 40000);
    double d = 0;
    for (size_t i = 0; i < a.size(); ++i) d = std::max(d, static_cast<double>(std::fabs(a[i] - b[i])));
    expect(d == 0, "reeverb 2: an old state renders exactly as before");
  }
  // ---- version 2 level ramps: block ramps, finite, settle to the target.
  {
    PluginState s = full({{RvbFDry, 128.f}, {RvbFEr, 0.f}, {RvbFWet, 0.f}});  // dry only: output = dry level x input
    auto fx = createEffect("gl.reverb");
    ProcessContext ctx;
    std::vector<float> L(512, 0.5f), R(512, 0.5f);
    for (int b = 0; b < 8; ++b) {  // let the start-up ramps settle
      std::fill(L.begin(), L.end(), 0.5f), std::fill(R.begin(), R.end(), 0.5f);
      fx->process(s, ctx, L.data(), R.data(), 512);
    }
    s.params[RvbFDry] = 0;
    double last = 1;
    bool mono = true, finite = true;
    for (int b = 0; b < 4; ++b) {
      std::fill(L.begin(), L.end(), 0.5f), std::fill(R.begin(), R.end(), 0.5f);
      fx->process(s, ctx, L.data(), R.data(), 512);
      for (int i = 0; i < 512; ++i) {
        finite = finite && std::isfinite(L[static_cast<size_t>(i)]);
        const double g = std::fabs(L[static_cast<size_t>(i)]) / 0.5;
        if (b < 2 && g > last + 1e-6) mono = false;
        last = std::min(last, g);
      }
    }
    // 0.001 per sample at 44.1 kHz: dry 1 -> 0 takes 1000 samples (two blocks); the reverb tail stays.
    expect(finite && mono, "reeverb 2 v2: a dry change ramps down without overshoot");
    const auto settle = std::fabs(L[100]) / 0.5;
    std::snprintf(name, sizeof name, "reeverb 2 v2: dry settles to 0 within 1000 samples (residual %.3g)", settle);
    expect(settle < 0.05, name);
  }
  // ---- block-size invariance with steady settings, extremes stay finite.
  {
    PluginState s = full({{RvbFModDepth, 2048.f}, {RvbFModSpeed, 2048.f}, {RvbFRoom, 100.f}, {RvbFDecay, 200.f}, {RvbFWet, 160.f}});
    const auto a = impulse(s, 30000, 64), b = impulse(s, 30000, 1000);
    double d = 0;
    bool fin = true;
    for (size_t i = 0; i < a.size(); ++i) d = std::max(d, static_cast<double>(std::fabs(a[i] - b[i]))), fin = fin && std::isfinite(a[i]);
    expect(d < 1e-6 && fin, "reeverb 2 v2: block-size invariant and finite at the extremes");
    PluginState z = full({{RvbFRoom, 1.f}, {RvbFDecay, 1.f}, {RvbFDiffusion, 0.f}, {RvbFLowCut, 3000.f}, {RvbFHighCut, 5.f}, {RvbFSep, -64.f}});
    const auto c = impulse(z, 20000, 256, 96000);
    bool f2 = true;
    for (float x : c) f2 = f2 && std::isfinite(x);
    expect(f2, "reeverb 2 v2: small room, 96 kHz, finite");
  }
  // ---- export: a changed instance whose origin is unversioned is written in the current layout.
  {
    Blob b;
    b.ints({33, 60, 12, 70, 80, 25, 70, 150, 700, 10, 128, 100, 90}).u8(0);
    flp::PluginMapping o;
    expect(decode(b.b, o), "reeverb 2: unversioned origin decodes");
    PluginState g = o.state;
    flp::enc::EncodeContext c;
    const flp::enc::Result same = flp::enc::encode(g, "Fruity Reeverb 2", &b.b, c);
    expect(same.quality == flp::enc::Result::Verbatim && same.state == b.b, "reeverb 2: an unchanged old origin exports verbatim");
    g.params[RvbFWet] = 110, g.params[RvbFRoom] = 55;
    const flp::enc::Result r = flp::enc::encode(g, "Fruity Reeverb 2", &b.b, c);
    flp::PluginMapping back;
    const bool ok = r.state.size() == 66 && decode(r.state, back);
    expect(ok && r.quality == flp::enc::Result::Exact && knob(back, 12) == 110 && knob(back, 3) == 55 && knob(back, 0) == 60 && knob(back, 13) == 684,
           "reeverb 2: a changed old origin exports as version 10001 with the values FL read");
    PluginState v1 = g;
    v1.params[RvbFRev] = 0;  // the version is Gridline's: an old instance exports the same state
    const flp::enc::Result r1 = flp::enc::encode(v1, "Fruity Reeverb 2", &b.b, c);
    expect(r1.state == r.state, "reeverb 2: the model version is not part of the FL state");
  }
  return finish();
}
