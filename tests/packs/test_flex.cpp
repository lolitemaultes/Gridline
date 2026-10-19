// Gridline — J-4 tests: the preset-player (FLEX) states. Its packs ship
// access-protected, so Gridline keeps the state (pack, preset, every control)
// and plays the closest built-in instrument; these tests check that every
// shipped state maps, keeps its selection and sounds.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include "../../src/core/PluginInfo.h"
#include "../../src/engine/Generators.h"
#include "../../src/packs/Packs.h"
#include "../Check.h"
#include "fl_corpus.h"

using namespace gl;

namespace {

float renderPeak(const PluginState& st) {
  auto g = gl::createGenerator(st.type);  // core + pack instruments
  if (!g) return -1;
  ChannelAux aux;
  packs::prepareAux(st, aux);
  ProcessContext ctx;
  ctx.sampleRate = 44100;
  ctx.tempo = 120;
  std::vector<float> L(512), R(512);
  float peak = 0;
  g->noteOn(1, 60, 0.8f, 0);
  for (int b = 0; b < 40; ++b) {
    std::fill(L.begin(), L.end(), 0.f), std::fill(R.begin(), R.end(), 0.f);
    if (b == 20) g->noteOff(1, 0);
    g->process(st, aux, ctx, L.data(), R.data(), 512);
    for (size_t i = 0; i < L.size(); ++i) {
      if (!std::isfinite(L[i]) || !std::isfinite(R[i])) return -1;
      peak = std::max({peak, std::fabs(L[i]), std::fabs(R[i])});
    }
  }
  return peak;
}

}  // namespace

int main() {
  if (!testfl::flInstalled()) {
    std::printf("# FL Studio not installed: FLEX corpus checks skipped\n");
    expect(true, "skipped");
    return finish();
  }
  auto all = testfl::decodeProjects("FLEX");
  // Single-plugin presets shipped with FL (system / database copies).
  for (const char* rel : {"/Data/System/FLEX.fst", "/Data/Patches/Plugin database/Generators/Synth classic/FLEX.fst"}) {
    testfl::Decoded d;
    d.where = rel;
    flp::PresetResult r;
    d.ok = flp::importFlPreset(testfl::flData() + rel, r, testfl::flData()) && r.kind == flp::PresetResult::Plugin;
    d.state = r.state;
    all.push_back(d);
  }
  int total = 0, decoded = 0, kept = 0, sounding = 0;
  for (const auto& d : all) {
    ++total;
    if (!d.ok) {
      std::printf("# not decoded: %s\n", d.where.c_str());
      continue;
    }
    ++decoded;
    testfl::maybeDump(d);
    kept += d.state.data.rfind("flex 1\npack ", 0) == 0 && d.state.data.find("\nv ") != std::string::npos;
    const float p = renderPeak(d.state);
    sounding += p > 1e-3f;
    if (p <= 1e-3f) std::printf("# silent: %s (%g)\n", d.where.c_str(), p);
  }
  std::printf("# FLEX: %d states, %d mapped, %d keep pack / preset / controls, %d sounding\n", total, decoded, kept, sounding);
  expect(total >= 50 && decoded == total, "every shipped FLEX state maps to a built-in instrument");
  expect(kept == decoded, "the FLEX selection and controls are kept in the state");
  expect(sounding == decoded, "every mapped state sounds");
  return finish();
}
