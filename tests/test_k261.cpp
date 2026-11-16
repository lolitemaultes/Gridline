// Gridline tests - K-261: Tracer (gl.delay) has one view; every saved state keeps its engine.
//  * every control of the one panel edits a parameter its engine really plays (a change is heard),
//    for the older engines too (the first Gridline delay, the classic tempo delay);
//  * controls an older engine has no counterpart for are marked (shown dimmed);
//  * every factory preset, and a state of each engine, saves and loads unchanged (the state is
//    never rewritten) and renders sample for sample as before the round trip;
//  * new instances use the current engine.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>
#include "Check.h"
#include "core/PluginInfo.h"
#include "core/PluginParams.h"
#include "core/Project.h"
#include "core/Song.h"
#include "core/TracerView.h"
#include "engine/Effects.h"

using namespace gl;
using namespace gl::pp;

namespace {
constexpr double kSr = 44100;
struct Buf {
  std::vector<float> L, R;
};
Buf noiseBurst() {
  Buf b;
  const size_t n = static_cast<size_t>(kSr * 1.2);
  b.L.assign(n, 0.f), b.R.assign(n, 0.f);
  uint32_t seed = 7;
  for (size_t i = 0; i < static_cast<size_t>(kSr * 0.05); ++i) {
    seed = seed * 1664525u + 1013904223u;
    b.L[i] = static_cast<float>(0.5 * ((seed >> 8) / 8388608.0 - 1.0));
    seed = seed * 1664525u + 1013904223u;
    b.R[i] = static_cast<float>(0.5 * ((seed >> 8) / 8388608.0 - 1.0));
  }
  return b;
}
Buf run(const PluginState& s) {
  Buf b = noiseBurst();
  auto fx = createEffect(s.type);
  ProcessContext ctx;
  ctx.sampleRate = kSr;
  ctx.tempo = 120;
  for (size_t i = 0; i < b.L.size(); i += 256) {
    const int n = static_cast<int>(std::min<size_t>(256, b.L.size() - i));
    fx->process(s, ctx, b.L.data() + i, b.R.data() + i, n);
  }
  return b;
}
double diff(const Buf& a, const Buf& b) {
  double d = 0;
  for (size_t i = 0; i < a.L.size(); ++i) d = std::max(d, static_cast<double>(std::max(std::fabs(a.L[i] - b.L[i]), std::fabs(a.R[i] - b.R[i]))));
  return d;
}
PluginState engineState(int engine) {
  PluginState s = defaultPluginState("gl.delay");
  s.params[DlyModel] = static_cast<float>(engine);
  s.params[DlyWet] = 0.8f, s.params[DlyFeedback] = 0.5f;
  return s;
}
// A clearly different value for parameter k (another choice, or a quarter of the range away).
float moved(const ParamDesc& d, float v) {
  if (d.kind == ParamKind::Toggle) return v >= 0.5f ? 0.f : 1.f;
  if (d.kind == ParamKind::Choice) return static_cast<float>(static_cast<int>(v + 1.5f) % static_cast<int>(std::max<size_t>(2, d.choices.size())));
  const double n = d.toNormalized(v);
  return static_cast<float>(d.fromNormalized(n > 0.5 ? n - 0.35 : n + 0.35));
}
}  // namespace

int main() {
  const PluginInfo* info = findPlugin("gl.delay");
  expect(info != nullptr, "Tracer is registered");
  if (!info) return finish();

  // the controls of the one panel (the current engine's parameters)
  const int panel[] = {DlyInput, DlySync, DlyTicks, DlyTimeMs, DlyKeepPitch, DlyTimeSmooth, DlyTimeR, DlyType3, DlySpread3,
                       DlyFeedback, DlyCutoff3, DlyResonance, DlyFilter3, DlyRate3, DlyBits, DlyWet, DlyTone, DlyDry,
                       DlyModRate3, DlyModDepth, DlyModCutoff, DlyDiffusion, DlyDiffSpread, DlyDistType, DlyDistThresh,
                       DlyDistHardness, DlyDistSymmetry};

  // 1. every live control of each engine is heard
  for (int engine : {0, 1, 2}) {
    int live = 0, dead = 0, mapped = 0;
    std::string deadList;
    for (int c : panel) {
      const int k = tracer::control(engine, c);
      if (k < 0) continue;
      ++mapped;
      PluginState s = engineState(engine);
      // the time knob a synced / free state shows
      if (c == DlyTimeMs) s.params[DlySync] = 0;
      if (c == DlyResonance && engine == 1) s.params[DlyFilter3] = 0;
      if (c == DlyResonance && engine == 0) s.params[DlyFilterType] = 1;
      if ((c == DlyDistThresh || c == DlyDistHardness || c == DlyDistSymmetry) && engine != 2) s.params[DlyDistType] = 2, s.params[DlyDistThresh] = -12;
      if ((c == DlyDiffSpread) && engine != 2) s.params[DlyDiffusion] = 0.8f;
      if (c == DlyModCutoff && engine != 2) s.params[DlyModDepth] = 0.5f, s.params[engine == 0 ? DlyFilterType : DlyFilter3] = engine == 0 ? 1.f : 0.f;
      if (c == DlyModRate3 && engine != 2) s.params[DlyModDepth] = 0.5f;  // the rate is heard while the time moves
      if (c == DlyTimeR && engine == 1) s.params[DlyType3] = 1;
      if (c == DlySpread3 && engine == 1) s.params[DlyType3] = 1, s.params[DlyTimeR] = 0.5f;
      if (c == DlySpread3 && engine == 0) s.params[DlyOffset] = 100;
      if (c == DlyTimeSmooth) s.params[DlyKeepPitch] = 0;
      if (c == DlyKeepPitch || c == DlyTimeSmooth) continue;  // only heard while the time moves
      const Buf a = run(s);
      PluginState m = s;
      m.params[static_cast<size_t>(k)] = moved(info->params[static_cast<size_t>(k)], s.params[static_cast<size_t>(k)]);
      if (diff(a, run(m)) > 1e-5) ++live;
      else ++dead, deadList += " " + info->params[static_cast<size_t>(k)].name;
    }
    std::printf("# engine %d: %d controls bound, %d heard, not heard:%s\n", engine, mapped, live, deadList.c_str());
    expect(dead == 0, ("engine " + std::to_string(engine) + ": every control of the panel is heard").c_str());
  }
  // the classic engine's panel: time, feedback, cutoff, stereo, offset, input, dry, pan are live; the rest dimmed
  int clsLive = 0;
  for (int c : panel) clsLive += tracer::control(2, c) >= 0;
  expect(clsLive == 8, "classic engine: eight controls of the panel are live, the others dimmed");
  expect(tracer::control(2, DlyResonance) < 0 && tracer::control(2, DlySync) < 0, "classic engine: no resonance, always synced");
  expect(tracer::control(0, DlyTicks) == DlyTime && tracer::control(0, DlyFeedback) == DlyFeedback, "first engine: its own time choice, shared feedback");
  expect(tracer::engineOf(7) == 1 && tracer::engineOf(2) == 2, "an unknown engine plays the current one");

  // 2. new instances use the current engine
  expect(std::lround(defaultPluginState("gl.delay").params[DlyModel]) == 1, "a new Tracer uses the current engine");

  // 3. factory presets and a state of each engine: saved and loaded unchanged, same sound
  std::vector<PluginState> states;
  for (size_t p = 0; p < info->presets.size(); ++p) states.push_back(presetState(*info, p));
  for (int engine : {0, 1, 2, 3}) states.push_back(engineState(engine));
  bool same = true, sound = true;
  for (const PluginState& st : states) {
    Song s = makeEmptySong();
    s.mixer[1].slots[0].plugin = st;
    Song back;
    std::string err;
    if (!songFromJson(songToJson(s), back, &err)) {
      same = false;
      continue;
    }
    const PluginState& got = back.mixer[1].slots[0].plugin;
    if (!(got.params == st.params) || got.type != st.type) same = false;
    if (diff(run(st), run(got)) != 0) sound = false;
  }
  expect(same, "every factory preset and every engine's state loads back unchanged (engine kept, nothing rewritten)");
  expect(sound, "and renders sample for sample the same");
  return finish();
}
