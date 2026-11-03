// K-37 (K-AD): the Retro Phaser's echo law 2. Older states and factory presets keep the first law;
// FL imports and new instances take the second.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>
#include "../../src/core/PluginInfo.h"
#include "../../src/core/flp/FlpEncode.h"
#include "../../src/engine/Effects.h"
#include "../../src/packs/Packs.h"
#include "../../src/packs/spacefx/sfx_flcurves.h"
#include "../../src/packs/spacefx/spacefx.h"
#include "../Check.h"

using namespace gl;
using namespace gl::packs::spacefx;

namespace {

struct Out {
  std::vector<float> L, R;
};
// An impulse (left 1, right `right`) at `at`, through the phaser's echo with the phaser itself wet at 0.
Out impulse(const PluginState& st, int at, int frames, float right = 0.f) {
  Out o;
  o.L.assign(static_cast<size_t>(frames), 0.f);
  o.R.assign(static_cast<size_t>(frames), 0.f);
  o.L[static_cast<size_t>(at)] = 1.f;
  o.R[static_cast<size_t>(at)] = right;
  auto fx = packs::createEffect(st.type);
  ProcessContext ctx;
  ctx.sampleRate = 44100;
  ctx.tempo = 120;
  for (int p = 0; p < frames; p += 256) fx->process(st, ctx, o.L.data() + p, o.R.data() + p, std::min(256, frames - p));
  return o;
}
PluginState echoState(float law) {
  PluginState s = defaultPluginState("gl.vphaser");
  auto set = [&](int i, float v) { s.params[static_cast<size_t>(i)] = v; };
  set(VpModel, 1), set(VpManual, 1), set(VpMix, 0), set(VpDelayOn, 1), set(VpDelayKeep, 1), set(VpDelaySync, 0);
  set(VpDelayTime, 100), set(VpDelayMix, 1), set(VpDelayStereo, 0.5f), set(VpDelayFb, 0), set(VpDelayLp, 320), set(VpDelayHp, 20);
  set(VpEchoLaw, law);
  return s;
}
// Largest absolute value in [from, to).
float peak(const std::vector<float>& x, int from, int to) {
  float m = 0;
  for (int i = from; i < to && i < static_cast<int>(x.size()); ++i) m = std::max(m, std::fabs(x[static_cast<size_t>(i)]));
  return m;
}
bool finite(const Out& o) {
  bool ok = true;
  for (float v : o.L) ok = ok && std::isfinite(v);
  for (float v : o.R) ok = ok && std::isfinite(v);
  return ok;
}

}  // namespace

int main() {
  const PluginInfo* info = findPlugin("gl.vphaser");
  expect(info != nullptr, "phaser exists");
  if (!info) return finish();

  // ---------------------------------------------------------- markers
  expect(defaultPluginState("gl.vphaser").params[VpEchoLaw] == 1.f, "new instance: echo law 2");
  for (size_t p = 0; p < info->presets.size(); ++p) expect(presetState(*info, p).params[VpEchoLaw] == 0.f, "factory preset: echo law 1");
  PluginState old = defaultPluginState("gl.vphaser");
  old.params.resize(static_cast<size_t>(VpEchoLaw));
  normalizePluginState(old);
  expect(old.params[VpEchoLaw] == 0.f, "a state saved before the marker keeps echo law 1");

  // FL import: law 2.
  {
    std::vector<uint8_t> b;
    auto i32 = [&](int v) {
      for (int k = 0; k < 4; ++k) b.push_back(static_cast<uint8_t>(static_cast<uint32_t>(v) >> (8 * k)));
    };
    i32(3);
    for (int i = 0; i < 36; ++i) i32(0);
    b.push_back(0);
    flp::PluginMapping m;
    flp::enc::EncodeContext c;
    expect(flp::enc::decodeState("Vintage Phaser", b, m, c) && m.state.params[VpEchoLaw] == 1.f, "FL import: echo law 2");
  }

  // ---------------------------------------------------------- the balance curve
  expect(std::fabs(flc::vpEchoBalance(0.5) - 1.0) < 1e-9 && flc::vpEchoBalance(0) == 0.0 && std::fabs(flc::vpEchoBalance(1) - std::sqrt(2.0)) < 1e-9, "balance: 0, 1, sqrt(2)");
  expect(std::fabs(flc::vpEchoBalance(0.25) - 0.573) < 2e-3 && std::fabs(flc::vpEchoBalance(0.75) - 1.28) < 2e-3, "balance: quarter positions");

  // ---------------------------------------------------------- law 2, measured against the first law
  const int at = 8000, T = 4410, frames = at + 3 * T + 2000;
  {
    // The tap keeps its height with the low pass closed: the filters only sit in the feedback path.
    const Out two = impulse(echoState(1), at, frames), one = impulse(echoState(0), at, frames);
    const float direct = peak(two.L, at, at + 400);
    const float tap2 = peak(two.L, at + T - 100, at + T + 200), tap1 = peak(one.L, at + T - 100, at + T + 200);
    expect(direct > 0.5f && std::fabs(tap2 - direct) < 0.02f * direct, "law 2: the tap reaches the output unfiltered");
    expect(tap1 < 0.5f * direct, "law 1: the tap goes through the filters");
    expect(peak(two.R, 0, frames) < 1e-6f, "law 2: no cross-feed to the other channel");
  }
  {
    // Balance: the left channel moves towards its direct sound, the right towards its echo.
    PluginState s = echoState(1);
    s.params[VpDelayLp] = 20480;
    s.params[VpDelayStereo] = 0.f;
    const Out o = impulse(s, at, frames, 1.f);
    const float lD = peak(o.L, at, at + 400), lT = peak(o.L, at + T - 100, at + T + 200), rD = peak(o.R, at, at + 400), rT = peak(o.R, at + T - 100, at + T + 200);
    expect(lD < 1e-4f && lT > 1.0f, "balance 0: left is echo only");
    expect(rT < 1e-4f && rD > 1.0f, "balance 0: right is direct only");
  }
  {
    // Feedback comes back one sample later than the tap, through the filters.
    PluginState s = echoState(1);
    s.params[VpDelayLp] = 20480;
    s.params[VpDelayFb] = 0.5f;
    const Out o = impulse(s, at, frames);
    auto peakIndex = [&](int from, int to) {
      int best = from;
      for (int i = from; i < to; ++i)
        if (std::fabs(o.L[static_cast<size_t>(i)]) > std::fabs(o.L[static_cast<size_t>(best)])) best = i;
      return best;
    };
    const int a = peakIndex(at, at + 400), b = peakIndex(a + T - 100, a + T + 100), c = peakIndex(b + T - 100, b + T + 100);
    expect(b - a == T - 1 || b - a == T, "law 2: first tap at the echo time");
    expect(c - b >= T && c - b <= T + 1, "law 2: the loop takes one sample more");
    const float t1 = peak(o.L, b - 50, b + 50), t2 = peak(o.L, c - 50, c + 50);
    expect(t2 > 0.3f * t1 && t2 < 0.6f * t1, "law 2: feedback level");
  }
  {
    // Glide mode rests in single precision: settled, repeatable, block-size independent.
    PluginState s = echoState(1);
    s.params[VpDelayKeep] = 0;
    s.params[VpDelayLp] = 20480;
    const Out a = impulse(s, at, frames);
    expect(finite(a) && peak(a.L, at + T - 100, at + T + 200) > 0.2f, "law 2: glide mode echoes");
    expect(a.L == impulse(s, at, frames).L, "law 2: repeatable");
  }
  // Extremes stay finite in both laws.
  for (float law : {0.f, 1.f}) {
    PluginState s = echoState(law);
    s.params[VpDelayFb] = 1;
    s.params[VpDelayStereo] = 1;
    s.params[VpDelayTime] = 1;
    expect(finite(impulse(s, 100, 20000)), "extremes stay finite");
    PluginState k = s;
    k.params[VpDelayKeep] = 0;
    expect(finite(impulse(k, 100, 20000)), "extremes stay finite (glide)");
  }
  return finish();
}
