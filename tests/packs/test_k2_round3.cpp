// Gridline: K-2 round 3 (0.7.2 parameter audit). Auto Pan's reference LFO law
// (speed curve, phase step once per control period, linear gain ramps) for new
// instances and imports while older states keep the first curve; Keyboard Mod
// imports version 3 states and the reference smoothing times; Multi Delay's
// zero-time minimum (older states keep the 0.002-step delay); new-instance
// defaults of Multi Delay, Peak Follower and Pitch Shifter at the reference
// plugin's, with the factory presets unchanged. All on hand-made states.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include "../../src/core/PluginInfo.h"
#include "../../src/core/PluginParams.h"
#include "../../src/core/flp/Flp.h"
#include "../../src/core/flp/FlpEncode.h"
#include "../../src/engine/Effects.h"
#include "../../src/packs/modulation/modulation.h"
#include "../../src/packs/modulation/modulation_dsp.h"
#include "../../src/packs/spacefx/sfx_flcurves.h"
#include "../../src/packs/spacefx/spacefx.h"
#include "../../src/packs/spectral/spectral.h"
#include "../Check.h"

using namespace gl;

namespace {

struct Blob {
  std::vector<uint8_t> b;
  Blob& i32(int32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(static_cast<uint32_t>(v) >> (8 * i)));
    return *this;
  }
  Blob& f32(float v) {
    uint32_t u;
    static_assert(sizeof u == sizeof v, "f32");
    std::copy(reinterpret_cast<const uint8_t*>(&v), reinterpret_cast<const uint8_t*>(&v) + 4, reinterpret_cast<uint8_t*>(&u));
    return i32(static_cast<int32_t>(u));
  }
  Blob& u8(uint8_t v) {
    b.push_back(v);
    return *this;
  }
};

bool decode(const char* fl, const std::vector<uint8_t>& state, flp::PluginMapping& m) {
  flp::enc::EncodeContext c;
  if (!flp::enc::decodeState(fl, state, m, c)) return false;
  normalizePluginState(m.state);
  return true;
}
float P(const PluginState& s, int k) { return s.params[static_cast<size_t>(k)]; }

// Renders a constant input (L = 0.5, R = 0.25) through an effect state.
std::vector<float> renderDc(const PluginState& s, int frames, int block) {
  auto fx = createEffect(s.type);
  std::vector<float> out(static_cast<size_t>(frames) * 2), L(static_cast<size_t>(block)), R(static_cast<size_t>(block));
  ProcessContext ctx;
  ctx.sampleRate = 44100;
  for (int f0 = 0; f0 < frames; f0 += block) {
    const int n = std::min(block, frames - f0);
    std::fill(L.begin(), L.end(), 0.5f), std::fill(R.begin(), R.end(), 0.25f);
    fx->process(s, ctx, L.data(), R.data(), n);
    for (int i = 0; i < n; ++i) out[2 * static_cast<size_t>(f0 + i)] = L[static_cast<size_t>(i)], out[2 * static_cast<size_t>(f0 + i) + 1] = R[static_cast<size_t>(i)];
  }
  return out;
}

// Mean period (samples) of the volume LFO from rising crossings of the left gain through 1.
double lfoPeriod(const std::vector<float>& o) {
  std::vector<size_t> ups;
  for (size_t i = 2; i < o.size() / 2; ++i)
    if (o[2 * (i - 1)] < 0.5f && o[2 * i] >= 0.5f) ups.push_back(i);
  if (ups.size() < 3) return 0;
  return static_cast<double>(ups.back() - ups[1]) / static_cast<double>(ups.size() - 2);
}

bool finite(const std::vector<float>& v) {
  for (float x : v)
    if (!std::isfinite(x)) return false;
  return true;
}

const PluginInfo::Preset* preset(const PluginInfo* p, const std::string& name) {
  for (const auto& pr : p->presets)
    if (pr.name == name) return &pr;
  return nullptr;
}
bool presetHas(const PluginInfo::Preset* pr, int idx, float v) {
  if (!pr) return false;
  for (const auto& [k, x] : pr->values)
    if (k == idx && std::fabs(x - v) < 1e-4f) return true;
  return false;
}

}  // namespace

int main() {
  using namespace pp;
  flp::PluginMapping m;

  // ------------------------------------------------------------------ Auto Pan (PanOMatic) LFO law
  {
    Blob b;  // pan 128, volume 256, sine, target volume, amount 128, speed 50000
    for (int32_t v : {128, 256, 0, 2, 128, 50000}) b.i32(v);
    expect(decode("Fruity PanOMatic", b.b, m) && P(m.state, ApLfoLaw) == 1 && P(m.state, ApClassic) == 1, "auto pan: imports use LFO law 2");
    expect(P(defaultPluginState("gl.autopan"), ApLfoLaw) == 1, "auto pan: new instances use LFO law 2");
    PluginState old = m.state;
    old.params.resize(static_cast<size_t>(ApLfoLaw));
    normalizePluginState(old);
    expect(P(old, ApLfoLaw) == 0, "auto pan: a state saved before the law parameter keeps law 1");

    const int frames = 44100 * 2;
    const std::vector<float> o = renderDc(m.state, frames, 512);
    const double u = 65536.0 - 50000.0, want = u * (std::pow(50000.0, u / 65536.0) - 1.0) * 2048.0 / 44100.0;
    const double per = lfoPeriod(o);
    expect(std::fabs(per - want) < 0.01 * want, "auto pan: law 2 period follows the reference speed curve");
    double lo = 9, hi = -9;
    for (int i = 4096; i < frames; ++i) lo = std::min(lo, static_cast<double>(o[2 * static_cast<size_t>(i)])), hi = std::max(hi, static_cast<double>(o[2 * static_cast<size_t>(i)]));
    expect(std::fabs(hi - 0.75) < 0.01 && std::fabs(lo - 0.25) < 0.01, "auto pan: full volume amount swings the gain 0.5 .. 1.5");
    const std::vector<float> o2 = renderDc(m.state, frames, 37);
    float d = 0;
    for (size_t i = 0; i < o.size(); ++i) d = std::max(d, std::fabs(o[i] - o2[i]));
    expect(d < 1e-6f, "auto pan: law 2 is block-size invariant");
    const double perOld = lfoPeriod(renderDc(old, frames, 512));
    expect(perOld > 0 && std::fabs(perOld - want) > 0.5 * want, "auto pan: law 1 keeps its own speed curve");
    PluginState ext = m.state;
    for (float sp : {0.f, 55000.f / 65536.f}) {
      ext.params[ApSpeedK] = sp;
      ext.params[ApTarget] = 1;
      expect(finite(renderDc(ext, 8192, 256)), "auto pan: law 2 speed extremes stay finite");
    }
    PluginState st = m.state;  // LFO off: static pan / volume, the same in both laws after the start
    st.params[ApTarget] = 0, st.params[ApStaticPan] = -0.3f, st.params[ApStaticVol] = 0.9f;
    PluginState st1 = st;
    st1.params[ApLfoLaw] = 0;
    const std::vector<float> a = renderDc(st, 4096, 128), c = renderDc(st1, 4096, 128);
    expect(std::fabs(a[8000] - c[8000]) < 1e-5f && std::fabs(a[8001] - c[8001]) < 1e-5f, "auto pan: static settings sound the same in both laws");
  }

  // ------------------------------------------------------------------ Keyboard Mod (Keyboard Controller)
  {
    namespace mod = gl::packs::modulation;
    Blob b;  // version 3: attack 128, release 64, 132 key values, 14-byte tail
    b.i32(3).i32(128).i32(64);
    for (int k = 0; k < 132; ++k) b.f32(k == 60 ? 0.25f : 1.0f);
    b.i32(0).i32(256).i32(0).u8(0).u8(0);
    expect(decode("Fruity Keyboard Controller", b.b, m), "keyboard mod: version 3 state decodes");
    expect(std::fabs(P(m.state, mod::KcAttack) - 0.1f * (std::sqrt(11.f) - 1.f)) < 1e-4f &&
               std::fabs(P(m.state, mod::KcRelease) - 0.1f * (std::pow(11.f, 0.25f) - 1.f)) < 1e-4f,
           "keyboard mod: smoothing 0..256 maps to 0.1 (11^x - 1) s");
    const std::vector<float> keys = mod::parseKeyValues(m.state.data);
    expect(keys.size() >= 128 && std::fabs(keys[60] - 0.25f) < 0.01f && std::fabs(keys[59] - 1.f) < 0.01f, "keyboard mod: version 3 key values read from offset 12");
    Blob v2;  // version 2: 13-byte tail
    v2.i32(2).i32(256).i32(0);
    for (int k = 0; k < 132; ++k) v2.f32(0.5f);
    v2.i32(48).i32(71).i32(0).u8(0);
    expect(decode("Fruity Keyboard Controller", v2.b, m) && std::fabs(P(m.state, mod::KcAttack) - 1.f) < 1e-4f && P(m.state, mod::KcRelease) <= 0.0011f,
           "keyboard mod: version 2 still decodes (1000 ms attack, instant release)");
  }

  // ------------------------------------------------------------------ Multi Delay zero time
  {
    namespace sfx = gl::packs::spacefx;
    PluginState s = defaultPluginState("gl.multidelay");
    expect(P(s, sfx::MdZeroTime) == 1, "multi delay: new instances put the step minimum at zero time");
    s.params[sfx::MdDry] = 0, s.params[sfx::MdWet] = 1, s.params[sfx::MdModel] = 1;
    for (int k = 0; k < sfx::kTaps; ++k) s.params[static_cast<size_t>(sfx::mdParam(k, sfx::MdOn))] = k == 0 ? 1.f : 0.f;
    s.params[static_cast<size_t>(sfx::mdParam(0, sfx::MdSteps))] = 0.002f;
    s.params[static_cast<size_t>(sfx::mdParam(0, sfx::MdOffset))] = 0;
    s.params[static_cast<size_t>(sfx::mdParam(0, sfx::MdFeedback))] = 0;
    s.params[static_cast<size_t>(sfx::mdExt(0, sfx::MdxLeaveFirst))] = 1;  // the first echo unscaled by the feedback volume
    s.params[static_cast<size_t>(sfx::mdParam(0, sfx::MdInFType))] = 0;
    s.params[static_cast<size_t>(sfx::mdParam(0, sfx::MdFbFType))] = 0;
    auto onset = [](const PluginState& st) {
      auto fx = createEffect(st.type);
      std::vector<float> L(4096, 0.f), R(4096, 0.f);
      L[2048] = R[2048] = 1;  // after the gain ramps settle
      ProcessContext ctx;
      ctx.sampleRate = 44100, ctx.tempo = 120;
      for (int f0 = 0; f0 < 4096; f0 += 256) fx->process(st, ctx, L.data() + f0, R.data() + f0, 256);
      int pk = -1;
      float best = 1e-4f;
      for (int i = 2048; i < 4096; ++i)
        if (std::fabs(L[static_cast<size_t>(i)]) > best) best = std::fabs(L[static_cast<size_t>(i)]), pk = i - 2048;
      return pk;
    };
    PluginState old = s;
    old.params[sfx::MdZeroTime] = 0;
    const int a = onset(s), b = onset(old);
    std::printf("# multi delay echo at %d (zero time) / %d (legacy) samples\n", a, b);
    expect(a >= 0 && a <= 2, "multi delay: a tap at the step minimum sounds at zero time");
    expect(b >= 10 && b <= 12, "multi delay: older states keep the 0.002-step delay");
    {  // A gate on a zero-time tap: the gate period comes from the delay time. This hung forever at period 0.
      bool ok = true;
      for (int mode : {0, 1, 2, 3})
        for (int div : {1, 4, 16}) {
          PluginState g = s;
          g.params[static_cast<size_t>(sfx::mdParam(0, sfx::MdFbMode))] = static_cast<float>(mode);
          g.params[static_cast<size_t>(sfx::mdParam(0, sfx::MdFeedback))] = 0.5f;
          g.params[static_cast<size_t>(sfx::mdParam(0, sfx::MdGateDiv))] = static_cast<float>(div);
          auto fx = createEffect(g.type);
          std::vector<float> L(2048, 0.25f), R(2048, 0.25f);
          ProcessContext ctx;
          ctx.sampleRate = 44100, ctx.tempo = 120;
          fx->process(g, ctx, L.data(), R.data(), 2048);
          ok = ok && finite(L) && finite(R);
        }
      expect(ok, "multi delay: a gate on a zero-time tap returns and stays finite");
      // The same with a raw time of 0 ms (a hand-edited or damaged state, below the knob's range).
      PluginState g = s;
      g.params[static_cast<size_t>(sfx::mdExt(0, sfx::MdxTempo))] = 0;
      g.params[static_cast<size_t>(sfx::mdParam(0, sfx::MdTimeMs))] = 0;
      g.params[static_cast<size_t>(sfx::mdParam(0, sfx::MdGateDiv))] = 4;
      auto fx = createEffect(g.type);
      std::vector<float> L(2048, 0.25f), R(2048, 0.25f);
      ProcessContext ctx;
      ctx.sampleRate = 44100, ctx.tempo = 120;
      fx->process(g, ctx, L.data(), R.data(), 2048);
      expect(finite(L) && finite(R), "multi delay: a gate at a raw time of 0 ms returns and stays finite");
      // K-AD: a very long time automated to a very short one wraps the gate position in one step.
      g.params[static_cast<size_t>(sfx::mdParam(0, sfx::MdTimeMs))] = 2000;
      auto fx2 = createEffect(g.type);
      for (int block = 0; block < 40; ++block) {
        std::fill(L.begin(), L.end(), 0.25f), std::fill(R.begin(), R.end(), 0.25f);
        fx2->process(g, ctx, L.data(), R.data(), 2048);
      }
      g.params[static_cast<size_t>(sfx::mdParam(0, sfx::MdTimeMs))] = 0.1f;
      fx2->process(g, ctx, L.data(), R.data(), 2048);
      expect(finite(L) && finite(R), "multi delay: a gated time drop wraps and stays finite");
    }
    PluginState shortS = defaultPluginState("gl.multidelay");
    shortS.params.resize(static_cast<size_t>(sfx::MdZeroTime));
    normalizePluginState(shortS);
    expect(P(shortS, sfx::MdZeroTime) == 0, "multi delay: a state saved before the switch keeps the old minimum");
  }

  // ------------------------------------------------------------------ Multi Delay gate law
  {
    namespace sfx = gl::packs::spacefx;
    namespace flc = gl::packs::spacefx::flc;
    PluginState base = defaultPluginState("gl.multidelay");
    expect(P(base, sfx::MdGateLaw) == 0, "multi delay: new instances keep the block-free gate law 1");
    PluginState saved = base;
    saved.params.resize(static_cast<size_t>(sfx::MdGateLaw));
    normalizePluginState(saved);
    expect(P(saved, sfx::MdGateLaw) == 0, "multi delay: a state saved before the switch keeps gate law 1");
    {
      Blob st;
      st.i32(16);
      for (int k = 0; k < 133; ++k) st.i32(0);
      for (int k = 0; k < 8 * 32; ++k) st.u8(0);
      flp::PluginMapping im;
      expect(decode("Fruity Delay Bank", st.b, im) && P(im.state, sfx::MdGateLaw) == 1, "multi delay: FL imports use gate law 2");
    }
    base.params[sfx::MdGateLaw] = 1;
    base.params[sfx::MdDry] = 0, base.params[sfx::MdWet] = 1, base.params[sfx::MdModel] = 1;
    for (int k = 0; k < sfx::kTaps; ++k) base.params[static_cast<size_t>(sfx::mdParam(k, sfx::MdOn))] = k == 0 ? 1.f : 0.f;
    auto PI = [&](PluginState& s, int f, float v) { s.params[static_cast<size_t>(sfx::mdParam(0, f))] = v; };
    PI(base, sfx::MdFbMode, sfx::MfbOff);  // no delay line: the output is the gate window itself
    PI(base, sfx::MdInLevel, 1), PI(base, sfx::MdOutLevel, 1), PI(base, sfx::MdInPan, 0), PI(base, sfx::MdOutPan, 0);
    PI(base, sfx::MdInFType, 0), PI(base, sfx::MdGateShape, 0), PI(base, sfx::MdOffset, 0);
    base.params[static_cast<size_t>(sfx::mdExt(0, sfx::MdxTempo))] = 0;
    auto render = [](const PluginState& st, int block) {
      auto fx = createEffect(st.type);
      std::vector<float> L(8192, 1.f), R(8192, 1.f);
      ProcessContext ctx;
      ctx.sampleRate = 44100, ctx.tempo = 120;
      for (int f0 = 0; f0 < 8192; f0 += block) fx->process(st, ctx, L.data() + f0, R.data() + f0, std::min(block, 8192 - f0));
      return L;
    };
    // 150 time units / 8 = 215.33 samples, 215 in the table: a slot runs to the table's end, and each block
    // starts a new slot from the rounded position.
    PluginState g = base;
    PI(g, sfx::MdTimeMs, 39.0625f), PI(g, sfx::MdGateDiv, 8);
    auto expected = [](int block) {
      const double period = 215.33203125;
      std::vector<float> w(8192);
      double pos = 0;
      int idx = 0, left = 0;
      for (int i = 0; i < 8192; ++i) {
        if (i % block == 0) left = 0;
        if (left <= 0) {
          if (pos >= period) pos -= period;
          idx = static_cast<int>(std::lrint(pos)) % 215;
          left = 215 - idx;
        }
        w[static_cast<size_t>(i)] = static_cast<float>(flc::mdGrainWindow(idx, 215, 0));
        ++idx, --left;
        pos += 1;
      }
      return w;
    };
    auto worst = [](const std::vector<float>& a, const std::vector<float>& b) {
      float e = 0;
      for (size_t i = 64; i < a.size(); ++i) e = std::max(e, std::fabs(a[i] - b[i]));
      return e;
    };
    for (int block : {64, 512, 1024}) {
      const float e = worst(render(g, block), expected(block));
      std::printf("# gate law 2, block %d: worst window error %g\n", block, static_cast<double>(e));
      expect(e < 1e-4f, "multi delay: gate law 2 follows whole slots in each block");
    }
    PluginState oldGate = g;
    oldGate.params[sfx::MdGateLaw] = 0;
    const std::vector<float> o64 = render(oldGate, 64), o1024 = render(oldGate, 1024);
    expect(worst(o64, o1024) < 1e-6f, "multi delay: gate law 1 stays free of the block size");
    expect(worst(o1024, render(g, 1024)) > 1e-3f, "multi delay: the two gate laws differ on a fractional slot");
    PluginState shortSlot = base;
    PI(shortSlot, sfx::MdTimeMs, 0.2604167f), PI(shortSlot, sfx::MdGateDiv, 16);  // 1 unit: one sample per slot
    expect(finite(render(shortSlot, 512)), "multi delay: a one-sample gate slot stays finite");
    PluginState zero = base;
    zero.params[static_cast<size_t>(sfx::mdExt(0, sfx::MdxTempo))] = 1;
    PI(zero, sfx::MdSteps, 0.002f), PI(zero, sfx::MdGateDiv, 4);
    const std::vector<float> z = render(zero, 512);
    bool open = true;
    for (size_t i = 64; i < z.size(); ++i) open = open && std::fabs(z[i] - 1.f) < 1e-4f;
    expect(open, "multi delay: a gate at time 0 leaves the audio ungated");
    PluginState on = base;
    on.params[static_cast<size_t>(sfx::mdExt(0, sfx::MdxTempo))] = 1;
    PI(on, sfx::MdSteps, 4.f / 480.f), PI(on, sfx::MdGateDiv, 16);
    {
      // Tempo 140: 39.375 / 16 = 2.46 samples, a table of two: open, closed.
      auto fx = createEffect(on.type);
      std::vector<float> L(4096, 1.f), R(4096, 1.f);
      ProcessContext ctx;
      ctx.sampleRate = 44100, ctx.tempo = 140;
      fx->process(on, ctx, L.data(), R.data(), 4096);
      bool binary = true;
      int closed = 0;
      for (size_t i = 64; i < L.size(); ++i) {
        binary = binary && (std::fabs(L[i]) < 1e-4f || std::fabs(L[i] - 1.f) < 1e-4f);
        closed += std::fabs(L[i]) < 1e-4f ? 1 : 0;
      }
      expect(binary && closed > 1000, "multi delay: a two-sample gate table alternates open and closed");
    }
  }

  // ------------------------------------------------------------------ reference defaults, factory presets unchanged
  {
    namespace sfx = gl::packs::spacefx;
    namespace mod = gl::packs::modulation;
    namespace spc = gl::packs::spectral;
    const PluginInfo* md = findPlugin("gl.multidelay");
    const PluginState d = defaultPluginState("gl.multidelay");
    expect(P(d, sfx::MdWet) == 1 && P(d, sfx::mdParam(1, sfx::MdOn)) == 0 && P(d, sfx::mdParam(0, sfx::MdOutPan)) == 0 &&
               std::fabs(P(d, sfx::mdParam(0, sfx::MdSteps)) - 3.f) < 1e-6f,
           "multi delay: new instances start at the reference defaults");
    {  // A new instance is audible as it stands: the dry signal, then one tap's echo at 3 steps (375 ms at 120 bpm).
      auto fx = createEffect("gl.multidelay");
      std::vector<float> L(24000, 0.f), R(24000, 0.f);
      L[1024] = R[1024] = 1;
      ProcessContext ctx;
      ctx.sampleRate = 44100, ctx.tempo = 120;
      for (size_t f0 = 0; f0 + 256 <= L.size(); f0 += 256) fx->process(d, ctx, L.data() + f0, R.data() + f0, 256);
      float early = 0, echo = 0;
      for (size_t i = 1026; i < 1024 + 16400; ++i) early = std::max(early, std::fabs(L[i]));
      for (size_t i = 1024 + 16500; i < 1024 + 16580; ++i) echo = std::max(echo, std::fabs(L[i]));
      std::printf("# new multi delay: dry %.3f, echo %.3f, between %.6f\n", static_cast<double>(L[1024]), static_cast<double>(echo), static_cast<double>(early));
      expect(std::fabs(L[1024] - 1.f) < 1e-3f && echo > 0.3f && early < 1e-3f, "multi delay: a new instance plays the dry signal and its echo at 3 steps");
    }
    const auto* wide = preset(md, "Wide pair");
    expect(presetHas(wide, sfx::MdWet, 0.5f) && presetHas(wide, sfx::mdParam(1, sfx::MdOn), 1.f) && presetHas(wide, sfx::mdParam(0, sfx::MdOutPan), -0.5f),
           "multi delay: the factory presets keep their former values");
    const PluginInfo* pk = findPlugin("gl.peakctl");
    expect(P(defaultPluginState("gl.peakctl"), mod::PkRAmount) == 179 && presetHas(preset(pk, "Bar wobble"), mod::PkRamp, 1.f),
           "peak follower: reference defaults, presets keep theirs");
    const PluginInfo* ps = findPlugin("gl.pitch");
    const PluginState pd = defaultPluginState("gl.pitch");
    expect(P(pd, spc::PsPreserve) == 1 && P(pd, spc::PsGrain) == 85 && P(pd, spc::PsEngine) == spc::kPsHifi, "pitch shifter: reference defaults, Hi-fi engine");
    expect(P(pd, spc::PsDelay) == 0, "pitch shifter: the delay time stays 0 ms (the Classic engines apply it in every delay mode)");
    expect(presetHas(preset(ps, "Unity"), spc::PsPreserve, 0.f), "pitch shifter: the factory presets keep their former values");
    // Saved states store every value: a full old state loads unchanged.
    PluginState full = defaultPluginState("gl.peakctl");
    full.params[static_cast<size_t>(mod::PkRAmount)] = 128;
    normalizePluginState(full);
    expect(P(full, mod::PkRAmount) == 128, "peak follower: a saved state keeps its stored value");
  }
  return finish();
}
