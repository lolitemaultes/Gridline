// K-37 (K-AD): range extensions behind a marker. Old states and old automation points keep their plain
// values; new limiter instances and FL imports take the new range.
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include "../../src/core/PluginInfo.h"
#include "../../src/core/PluginParams.h"
#include "../../src/core/flp/FlpEncode.h"
#include "../../src/engine/Effects.h"
#include "../../src/engine/EqDesign.h"
#include "../../src/packs/spectral/spectral.h"
#include "../Check.h"

using namespace gl;

namespace {

bool near(double a, double b, double e = 1e-4) { return std::fabs(a - b) <= e; }

const ParamDesc& desc(const char* type, int i) { return findPlugin(type)->params[static_cast<size_t>(i)]; }

std::vector<float> render(const PluginState& s) {
  auto fx = createEffect(s.type);
  ProcessContext ctx;
  ctx.sampleRate = 44100;
  ctx.tempo = 120;
  std::vector<float> l(256), r(256), out;
  uint32_t seed = 1;
  for (int block = 0; block < 40; ++block) {
    for (size_t i = 0; i < l.size(); ++i) {
      seed = seed * 1664525u + 1013904223u;
      const float n = static_cast<float>(static_cast<int32_t>(seed)) / 2147483648.f;
      const float burst = (block % 8) < 2 ? 1.5f : 0.02f;
      l[i] = n * burst, r[i] = -n * burst;
    }
    fx->process(s, ctx, l.data(), r.data(), static_cast<int>(l.size()));
    out.insert(out.end(), l.begin(), l.end());
  }
  return out;
}

double diffDb(const std::vector<float>& a, const std::vector<float>& b) {
  double e = 0;
  for (size_t i = 0; i < a.size() && i < b.size(); ++i) e += (static_cast<double>(a[i]) - b[i]) * (static_cast<double>(a[i]) - b[i]);
  return e <= 0 ? -999 : 10 * std::log10(e);
}

std::vector<uint8_t> limiterBytes(int release, int compRelease, int gateRelease) {
  const int knobs[18] = {1000, 1000, 1000, 1115, 3, release, 3, 1000, 1000, 0, 0, 0, compRelease, 1, 1000, 0, 0, gateRelease};
  std::vector<uint8_t> b(4 + 18 * 4 * 2 + 32, 0);
  b[0] = 7;
  std::memcpy(b.data() + 4, knobs, sizeof knobs);
  std::memcpy(b.data() + 4 + sizeof knobs, knobs, sizeof knobs);
  return b;
}

std::vector<uint8_t> eqV1Bytes(int freq0) {
  std::vector<int32_t> t(29, 0);
  static const int defFreq[7] = {5777, 13024, 19904, 33145, 42327, 49307, 55825};
  for (size_t b = 0; b < 7; ++b) {
    t[7 + b] = defFreq[b];
    t[14 + b] = 17500;
    t[21 + b] = 6;
  }
  t[7] = freq0;
  t[0] = 1800;
  t[28] = 128;
  std::vector<uint8_t> out(t.size() * 4);
  std::memcpy(out.data(), t.data(), out.size());
  return out;
}

std::vector<uint8_t> vocoderBytes(int low, int high, int attack) {
  const int bands = 16;
  const size_t knobs = 17 + 4 * static_cast<size_t>(bands);
  std::vector<uint8_t> b(knobs + 12 * 4, 0);
  const int head[2] = {1, bands};
  std::memcpy(b.data(), head, sizeof head);
  const int k[12] = {low, high, 64, 0, 0, 50, attack, 100, 0, 0, 0, 128};
  std::memcpy(b.data() + knobs, k, sizeof k);
  return b;
}

}  // namespace

int main() {
  namespace sp = gl::packs::spectral;
  flp::enc::EncodeContext c;
  // ---------------------------------------------------------- Limiter
  {
    const char* T = "gl.limiter";
    const PluginInfo* info = findPlugin(T);
    // Old automation points on the legacy knobs are the same plain values, at both ends too.
    for (int i : {pp::LimRelease, pp::LimCompRelease, pp::LimGateRelease}) {
      const ParamDesc& d = desc(T, i);
      expect(near(d.fromNormalized(0), 0.001) && near(d.fromNormalized(1), 2.0), "legacy release ends");
      expect(near(d.fromNormalized(0.5), std::sqrt(0.002)), "legacy release middle");
    }
    // The new knob follows the FL wheel's law: 4 * (501^n - 1) ms.
    const ParamDesc& w = desc(T, pp::LimReleaseW);
    expect(w.fromNormalized(0) == 0.0 && near(w.fromNormalized(1), 2.0), "new release ends");
    expect(near(w.fromNormalized(0.5), 0.004 * (std::sqrt(501.0) - 1.0), 1e-9), "new release curve");
    expect(near(w.toNormalized(w.fromNormalized(0.37)), 0.37, 1e-9), "new release round trip");
    // Marker: new instance 1; a short old vector 0; presets 0.
    PluginState fresh = defaultPluginState(T);
    expect(fresh.params[pp::LimRange] == 1.f, "new instance uses the new range");
    bool legacyZero = false;
    for (const auto& kv : info->legacyDefaults)
      if (kv.first == pp::LimRange && kv.second == 0.f) legacyZero = true;
    expect(legacyZero, "marker legacy default");
    PluginState old;
    old.type = T;
    old.params.assign(fresh.params.begin(), fresh.params.begin() + pp::LimRange);
    old.params[pp::LimRelease] = 0.3f;
    normalizePluginState(old);
    expect(old.params[pp::LimRange] == 0.f, "old state stays on the old range");
    expect(near(old.params[pp::LimRelease], 0.3), "old state keeps its value");
    bool presetsOld = !info->presets.empty();
    for (const auto& pr : info->presets) {
      bool found = false;
      for (const auto& kv : pr.values)
        if (kv.first == pp::LimRange && kv.second == 0.f) found = true;
      presetsOld = presetsOld && found;
    }
    expect(presetsOld, "presets stay on the old range");
    // Old state: the new knobs do nothing; marker 1 with equal time renders the same.
    old.params[pp::LimGain] = 18.f;
    old.params[pp::LimCompThresh] = -18.f;
    old.params[pp::LimCompRatio] = 4.f;
    PluginState oldMoved = old;
    oldMoved.params[pp::LimReleaseW] = 1.5f;
    const auto a = render(old);
    expect(diffDb(a, render(oldMoved)) < -200, "old state ignores the new knob");
    PluginState same = old;
    same.params[pp::LimRange] = 1.f;
    same.params[pp::LimReleaseW] = old.params[pp::LimRelease];
    same.params[pp::LimCompReleaseW] = old.params[pp::LimCompRelease];
    same.params[pp::LimGateReleaseW] = old.params[pp::LimGateRelease];
    expect(diffDb(a, render(same)) < -200, "new knob at the same time renders the same");
    PluginState zero = same;
    zero.params[pp::LimReleaseW] = 0.f;
    zero.params[pp::LimCompReleaseW] = 0.f;
    zero.params[pp::LimGateReleaseW] = 0.f;
    const auto z = render(zero);
    bool finite = true;
    for (float v : z) finite = finite && std::isfinite(v);
    expect(finite, "0 ms release stays finite");
    expect(diffDb(a, z) > -60, "0 ms release is audible against 300 ms");
    // FL import: raw 0 reaches 0 ms, automation drives the new knob with the wheel's own law.
    flp::PluginMapping m;
    expect(flp::enc::decodeState("Fruity Limiter", limiterBytes(0, 0, 0), m, c), "limiter state decodes");
    normalizePluginState(m.state);
    expect(m.state.params[pp::LimRange] == 1.f && m.state.params[pp::LimReleaseW] == 0.f, "FL raw 0 is 0 ms");
    expect(m.state.params[pp::LimCompReleaseW] == 0.f && m.state.params[pp::LimGateReleaseW] == 0.f, "FL raw 0 is 0 ms (comp, noise)");
    expect(near(m.state.params[pp::LimRelease], 0.001), "the legacy knob keeps its floor");
    expect(m.param && m.param(5) == pp::LimReleaseW, "FL automation targets the new knob");
    expect(m.value && near(m.value(5, 0.5), 0.004 * (std::sqrt(501.0) - 1.0), 1e-4), "FL automation position maps through");
    flp::PluginMapping m2;
    expect(flp::enc::decodeState("Fruity Limiter", limiterBytes(5000, 6666, 4187), m2, c), "limiter default decodes");
    normalizePluginState(m2.state);
    expect(near(m2.state.params[pp::LimReleaseW], 0.0855, 2e-4) && near(m2.state.params[pp::LimRelease], m2.state.params[pp::LimReleaseW]), "both knobs agree above 1 ms");
    // Export: a state on the legacy knob writes that time, an import stays verbatim.
    const std::vector<uint8_t> origin = limiterBytes(5000, 6666, 4187);
    const auto verbatim = flp::enc::encode(m2.state, "Fruity Limiter", &origin, c);
    expect(verbatim.quality == flp::enc::Result::Verbatim, "unchanged import stays verbatim");
    PluginState legacyNative = defaultPluginState(T);
    legacyNative.params[pp::LimRange] = 0.f;
    legacyNative.params[pp::LimRelease] = 0.5f;
    const auto out = flp::enc::encode(legacyNative, "", nullptr, c);
    flp::PluginMapping back;
    expect(out.hasState && flp::enc::decodeState("Fruity Limiter", out.state, back, c), "legacy state exports");
    normalizePluginState(back.state);
    expect(near(back.state.params[pp::LimReleaseW], 0.5, 2e-3), "legacy release time reaches the export");
  }
  // ---------------------------------------------------------- EQ, classic band model
  {
    flp::PluginMapping low, floor20;
    expect(flp::enc::decodeState("Fruity Parametric EQ", eqV1Bytes(0), low, c), "EQ v1 decodes");
    expect(flp::enc::decodeState("Fruity Parametric EQ", eqV1Bytes(860), floor20, c), "EQ v1 decodes at 20 Hz");
    normalizePluginState(low.state), normalizePluginState(floor20.state);
    expect(near(low.state.params[pp::EqClassicLow], 10.0), "raw 0 reaches 10 Hz");
    expect(low.state.params[pp::eqParam(0, pp::EqFreq)] == 20.f, "the band frequency keeps its 20 Hz floor");
    EqBandFilters a, b;
    designEqBandClassic(low.state, 0, 44100, a);
    designEqBandClassic(floor20.state, 0, 44100, b);
    expect(a.count > 0 && a.stage[0].b1 != b.stage[0].b1, "the classic design uses the low end");
    PluginState plain = defaultPluginState("gl.eq");
    expect(plain.params[pp::EqClassicLow] == 20.f, "the default leaves every state as it was");
    PluginState above = low.state;
    above.params[pp::eqParam(0, pp::EqFreq)] = 40.f;
    PluginState above2 = above;
    above2.params[pp::EqClassicLow] = 20.f;
    designEqBandClassic(above, 0, 44100, a);
    designEqBandClassic(above2, 0, 44100, b);
    expect(a.stage[0].b1 == b.stage[0].b1, "only used at the floor");
    const std::vector<uint8_t> origin = eqV1Bytes(0);
    expect(flp::enc::encode(low.state, "Fruity Parametric EQ", &origin, c).quality == flp::enc::Result::Verbatim, "EQ import stays verbatim");
  }
  // ---------------------------------------------------------- Vocoder
  {
    const char* T = "gl.vocoder";
    const ParamDesc& lo = desc(T, sp::VcLowFreq);
    expect(near(lo.fromNormalized(0), 20) && near(lo.fromNormalized(1), 2000), "narrow low band range unchanged");
    expect(near(desc(T, sp::VcHighFreq).fromNormalized(0), 1000) && near(desc(T, sp::VcAttack).fromNormalized(1), 200), "narrow ranges unchanged");
    expect(defaultPluginState(T).params[sp::VcRange] == 0.f, "states start narrow (only the legacy engine reads this)");
    flp::PluginMapping m;
    const std::vector<uint8_t> st = vocoderBytes(65536, 0, 1000);
    expect(flp::enc::decodeState("Fruity Vocoder", st, m, c), "vocoder decodes");
    normalizePluginState(m.state);
    expect(m.state.params[sp::VcRange] == 1.f, "FL import takes the wide range");
    expect(near(m.state.params[sp::VcLowW], 16000, 1) && near(m.state.params[sp::VcLowFreq], 2000, 1e-3), "low band above the old end");
    expect(near(m.state.params[sp::VcHighW], 10, 1e-3) && near(m.state.params[sp::VcHighFreq], 1000, 1e-3), "high band below the old end");
    expect(near(m.state.params[sp::VcAttackW], 1000, 1e-3) && near(m.state.params[sp::VcAttack], 200, 1e-3), "attack above the old end");
    expect(m.param && m.param(6) == sp::VcAttackW, "FL automation targets the wide knob");
    expect(flp::enc::encode(m.state, "Fruity Vocoder", &st, c).quality == flp::enc::Result::Verbatim, "vocoder import stays verbatim");
    // An instance imported before: its narrow values stay in force in the engine.
    PluginState oldImp = m.state;
    oldImp.params[sp::VcRange] = 0.f;
    PluginState wideEq = m.state;
    wideEq.params[sp::VcLowW] = oldImp.params[sp::VcLowFreq];
    wideEq.params[sp::VcHighW] = oldImp.params[sp::VcHighFreq];
    wideEq.params[sp::VcAttackW] = oldImp.params[sp::VcAttack];
    expect(diffDb(render(oldImp), render(wideEq)) < -200, "narrow and wide knobs at equal values render the same");
  }
  return finish();
}
