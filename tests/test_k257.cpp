// K-257: the Shaper grows into a rhythmic gate with lanes of their own lengths (independent
// sequencing, loops of any number of steps) and a band split (gate the lows or the highs only).
// States saved before play exactly as before.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>
#include "Check.h"
#include "core/PluginInfo.h"
#include "core/PluginParams.h"
#include "engine/Effects.h"
#include "engine/Generators.h"

using namespace gl;
using namespace gl::pp;

namespace {

constexpr double kSr = 48000, kTempo = 120;  // one beat = 24000 samples; a 1-bar pattern step (1/16) = 6000

std::vector<float> render(PluginState st, const std::vector<float>& in) {
  auto fx = createEffect("gl.shaper");
  ProcessContext ctx;
  ctx.sampleRate = kSr;
  ctx.tempo = kTempo;
  std::vector<float> L = in, R = in;
  for (size_t at = 0; at < in.size(); at += 256) {
    const int n = static_cast<int>(std::min<size_t>(256, in.size() - at));
    fx->process(st, ctx, L.data() + at, R.data() + at, n);
  }
  return L;
}

std::vector<float> sine(double hz, size_t n, float amp = 0.5f) {
  std::vector<float> v(n);
  for (size_t i = 0; i < n; ++i) v[i] = amp * static_cast<float>(std::sin(2 * 3.14159265358979 * hz * static_cast<double>(i) / kSr));
  return v;
}

double rms(const std::vector<float>& v, size_t a, size_t b) {
  double s = 0;
  for (size_t i = a; i < b; ++i) s += static_cast<double>(v[i]) * v[i];
  return std::sqrt(s / static_cast<double>(std::max<size_t>(1, b - a)));
}

}  // namespace

int main() {
  const PluginInfo* info = findPlugin("gl.shaper");
  expect(info && static_cast<int>(info->params.size()) == ShCount, "the Shaper has its new parameters");
  // 1. A state saved before loads as 16 steps per lane and the full band, and plays the same.
  PluginState full = presetState(*info, 2);  // "Gate 1/8"
  PluginState old = full;
  old.params.resize(static_cast<size_t>(ShVolSteps));
  normalizePluginState(old);
  expect(old.params[ShVolSteps] == kShaperPoints - 1 && old.params[ShBand] == 0, "an older state reads 16 steps and the full band");
  const std::vector<float> noise = [] {
    std::vector<float> v(48000);
    uint32_t r = 1;
    for (float& x : v) r = r * 1664525u + 1013904223u, x = static_cast<float>(static_cast<int32_t>(r)) / 4.3e9f;
    return v;
  }();
  expect(render(old, noise) == render(full, noise), "an older state plays exactly as before");

  // 2. A three-step volume lane: on, off, off, on, off, off ... against the 16-step pattern.
  PluginState st = defaultPluginState("gl.shaper");
  for (int k = 0; k < kShaperPoints; ++k) st.params[static_cast<size_t>(ShVol + k)] = k == 0 ? 1.f : 0.f;
  st.params[ShSmooth] = 0.5f;
  st.params[ShVolSteps] = 2;  // 3 steps
  const std::vector<float> dc(6000 * 10, 0.5f);
  const std::vector<float> g = render(st, dc);
  bool pattern = true;
  for (int step = 0; step < 9; ++step) {
    const float v = g[static_cast<size_t>(step * 6000 + 3000)];
    const bool on = step % 3 == 0;
    if (on ? std::fabs(v - 0.5f) > 1e-3f : std::fabs(v) > 1e-3f) pattern = false;
    std::printf("# step %d: %.4f\n", step, static_cast<double>(v));
  }
  expect(pattern, "a lane of 3 steps repeats every 3 steps");
  st.params[ShVolSteps] = kShaperPoints - 1;
  const std::vector<float> g16 = render(st, dc);
  expect(std::fabs(g16[3 * 6000 + 3000]) < 1e-3f && std::fabs(g16[3000] - 0.5f) < 1e-3f, "16 steps: the whole pattern as before");

  // 3. The band split: the volume shut, only the chosen band is gated.
  PluginState b = defaultPluginState("gl.shaper");
  for (int k = 0; k < kShaperPoints; ++k) b.params[static_cast<size_t>(ShVol + k)] = 0.f;
  b.params[ShBandFreq] = 250;
  const std::vector<float> lo = sine(60, 48000), hi = sine(5000, 48000);
  b.params[ShBand] = 1;  // the lows
  const double loIn = rms(lo, 24000, 48000), hiIn = rms(hi, 24000, 48000);
  const double loOut1 = rms(render(b, lo), 24000, 48000), hiOut1 = rms(render(b, hi), 24000, 48000);
  b.params[ShBand] = 2;  // the highs
  const double loOut2 = rms(render(b, lo), 24000, 48000), hiOut2 = rms(render(b, hi), 24000, 48000);
  std::printf("# lows gated: 60 Hz %.4f -> %.4f, 5 kHz %.4f -> %.4f; highs gated: 60 Hz -> %.4f, 5 kHz -> %.4f\n", loIn, loOut1, hiIn, hiOut1, loOut2, hiOut2);
  expect(loOut1 < 0.05 * loIn && hiOut1 > 0.97 * hiIn, "Lows: the lows are gated, the highs pass");
  expect(hiOut2 < 0.05 * hiIn && loOut2 > 0.95 * loIn, "Highs: the highs are gated, the lows pass");
  {  // nothing shaped: the two bands add back to the full level at every frequency
    PluginState n = defaultPluginState("gl.shaper");
    n.params[ShBand] = 1;
    bool flat = true;
    for (double hz : {40.0, 120.0, 250.0, 600.0, 3000.0, 12000.0}) {
      const std::vector<float> x = sine(hz, 48000);
      const double ratio = rms(render(n, x), 24000, 48000) / rms(x, 24000, 48000);
      std::printf("# unshaped split at %.0f Hz: %.4f\n", hz, ratio);
      flat = flat && std::fabs(ratio - 1) < 0.01;
    }
    expect(flat, "with nothing shaped the band split keeps the level of every frequency");
  }
  // 4. The factory preset that uses both is there.
  bool found = false;
  for (const auto& p : info->presets) found = found || p.name == "Three against four, highs";
  expect(found, "a factory preset shows the new lanes and the band split");
  return finish();
}
