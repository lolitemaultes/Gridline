// Gridline — J-1: tier-2 effects, reference parity (pitch corrector, spreader,
// pitch shifter, hall, convolver, BBD chorus, vintage phaser, morph EQ, multi
// delay, vocoder, band splitter, scope) plus the engine hooks they need (MIDI
// port bus, multi-destination send outputs). FL preset / project checks run
// only when an FL Studio installation is present.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>
#include "../src/core/PluginInfo.h"
#include "../src/core/PluginParams.h"
#include "../src/core/Project.h"
#include "../src/core/Song.h"
#include "../src/core/flp/Flp.h"
#include "../src/core/flp/FlpPresets.h"
#include "../src/engine/Effects.h"
#include "../src/engine/Generators.h"
#include "Check.h"

using namespace gl;
namespace fs = std::filesystem;

namespace jt {
constexpr double kSr = 48000;
inline const char* kFlData = "/Applications/FL Studio 2026.app/Contents/Resources/FL/Data";
inline const char* kFlFx = "/Applications/FL Studio 2026.app/Contents/Resources/FL/Data/Patches/Plugin presets/Effects";
inline bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }
inline bool haveFl() { return fs::exists(kFlData); }
// Runs an effect over a buffer pair in blocks of `block` frames.
inline void run(Effect& fx, const PluginState& s, std::vector<float>& L, std::vector<float>& R, int block = 256,
                double sr = kSr) {
  ProcessContext ctx;
  ctx.sampleRate = sr;
  for (size_t i = 0; i < L.size(); i += static_cast<size_t>(block)) {
    int n = static_cast<int>(std::min<size_t>(static_cast<size_t>(block), L.size() - i));
    fx.process(s, ctx, L.data() + i, R.data() + i, n);
  }
}
inline bool finite(const std::vector<float>& v) {
  for (float x : v)
    if (!std::isfinite(x)) return false;
  return true;
}
inline double rms(const std::vector<float>& v, size_t from = 0) {
  double a = 0;
  for (size_t i = from; i < v.size(); ++i) a += static_cast<double>(v[i]) * v[i];
  return v.size() > from ? std::sqrt(a / static_cast<double>(v.size() - from)) : 0.0;
}
inline std::vector<float> sine(double hz, size_t n, double amp = 0.5, double sr = kSr) {
  std::vector<float> v(n);
  for (size_t i = 0; i < n; ++i)
    v[i] = static_cast<float>(amp * std::sin(2 * 3.14159265358979 * hz * static_cast<double>(i) / sr));
  return v;
}
// Converts one FL preset file (path relative to the FL effect preset folder).
// Returns false when the file is missing or not converted.
inline bool flPreset(const std::string& rel, flp::PresetResult& out) {
  fs::path p = fs::path(kFlFx) / rel;
  if (!fs::exists(p)) return false;
  return flp::importFlPreset(p.string(), out) && out.kind == flp::PresetResult::Plugin;
}
}  // namespace jt

// One include per area, so the areas can be worked on independently.
#include "j1/j1_engine.inc"
#include "j1/j1_spectral.inc"
#include "j1/j1_spectral2.inc"
#include "j1/j1_space.inc"
#include "j1/j1_space2.inc"
#include "j1/j1_utility.inc"

int main() {
  testJ1Engine();
  testJ1Spectral();
  testJ1Spectral2();
  testJ1Space();
  testJ1Space2();
  testJ1Utility();
  return finish();
}
