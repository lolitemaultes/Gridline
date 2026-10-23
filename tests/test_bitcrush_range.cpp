// Gridline — J-92 user report: "the bitcrush effect, when I tweaked the bits
// parameter it had a cap and wouldn't make bits high". Every bit depth 1..24
// of gl.bitcrush is reachable (descriptor, knob law, ParamId set / get,
// project save / load) and audibly cleaner than the one below it, up to a
// near-transparent 24 bits; Downsample 1..50 the same; the other bit crushers
// (Tracer's bit depth, Hex Synth lo-fi bits) reach their top too.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include "../src/core/Params.h"
#include "../src/core/PluginInfo.h"
#include "../src/core/PluginParams.h"
#include "../src/core/Project.h"
#include "../src/core/Song.h"
#include "../src/engine/Effects.h"
#include "Check.h"
#include "Hermetic.h"

using namespace gl;

namespace {

constexpr double kSr = 44100;

// Error of the crushed signal against the dry one (dB re the dry RMS).
double errorDb(PluginState st, int frames = 22050) {
  auto fx = createEffect(st.type);
  if (!fx) return 999;
  fx->prepareHost(st, kSr);
  std::vector<float> L(static_cast<size_t>(frames)), R(static_cast<size_t>(frames)), dry(static_cast<size_t>(frames));
  for (int i = 0; i < frames; ++i) dry[static_cast<size_t>(i)] = static_cast<float>(0.5 * std::sin(2 * 3.141592653589793 * 997.0 * i / kSr));
  L = dry, R = dry;
  ProcessContext c;
  c.sampleRate = kSr, c.offline = true;
  for (int pos = 0; pos < frames; pos += 256) fx->process(st, c, L.data() + pos, R.data() + pos, std::min(256, frames - pos));
  double e = 0, s = 0;
  for (int i = 1000; i < frames; ++i) {
    const double d = static_cast<double>(L[static_cast<size_t>(i)]) - dry[static_cast<size_t>(i)];
    e += d * d, s += static_cast<double>(dry[static_cast<size_t>(i)]) * dry[static_cast<size_t>(i)];
  }
  return e > 0 ? 10 * std::log10(e / s) : -300;
}

void testBits() {
  const PluginInfo* info = findPlugin("gl.bitcrush");
  expect(info != nullptr, "gl.bitcrush exists");
  if (!info) return;
  const ParamDesc& d = info->params[pp::BcBits];
  // The knob's full travel reaches both ends; the value text shows them.
  expect(d.fromNormalized(1.0) == 24.0 && d.fromNormalized(0.0) == 1.0, "Bits: knob travel 0..1 reaches 1..24");
  expect(d.clamp(24) == 24 && d.format(24) != d.format(23), "Bits: 24 is a distinct, unclamped value");
  PluginState st = defaultPluginState("gl.bitcrush");
  st.params[pp::BcDownsample] = 1;  // isolate the bit depth
  st.params[pp::BcMix] = 1;
  double prev = 999;
  int ok = 0;
  std::string line;
  for (int b = 1; b <= 24; ++b) {
    st.params[pp::BcBits] = static_cast<float>(b);
    const double e = errorDb(st);
    char buf[32];
    std::snprintf(buf, sizeof buf, " %d:%.0f", b, e);
    line += buf;
    // Each extra bit about 6 dB cleaner (at least 3 dB, until the float floor).
    if (e < prev - 3 || e < -130) ++ok;
    prev = e;
  }
  std::printf("# bits -> error dB:%s\n", line.c_str());
  {  // diagnostic: at the default downsample
    PluginState s2 = defaultPluginState("gl.bitcrush");
    std::string l2;
    for (int b : {4, 8, 12, 16, 20, 24}) {
      s2.params[pp::BcBits] = static_cast<float>(b);
      char buf[32];
      std::snprintf(buf, sizeof buf, " %d:%.1f", b, errorDb(s2));
      l2 += buf;
    }
    std::printf("# default downsample, bits -> error dB:%s\n", l2.c_str());
  }
  expect(ok == 24, "Bits 1..24: every extra bit makes the output cleaner (monotonic distortion)");
  st.params[pp::BcBits] = 24;
  expect(errorDb(st) < -125, "Bits 24: near transparent");
  st.params[pp::BcBits] = 1;
  expect(errorDb(st) > -15, "Bits 1: heavily crushed");
  // Fractional depths between whole bits move smoothly.
  st.params[pp::BcBits] = 12.0f;
  const double e12 = errorDb(st);
  st.params[pp::BcBits] = 12.5f;
  const double e125 = errorDb(st);
  st.params[pp::BcBits] = 13.0f;
  const double e13 = errorDb(st);
  expect(e125 < e12 && e125 > e13, "Bits 12 / 12.5 / 13: fractional depths in between");
}

void testDownsample() {
  PluginState st = defaultPluginState("gl.bitcrush");
  const PluginInfo* info = findPlugin("gl.bitcrush");
  const ParamDesc& d = info->params[pp::BcDownsample];
  expect(d.fromNormalized(1.0) == 50.0, "Downsample: knob travel reaches 50");
  st.params[pp::BcBits] = 24;
  double prev = -999;
  int ok = 0;
  std::string line;
  for (int k : {1, 2, 4, 8, 16}) {  // beyond, the held signal is noise-like (aliasing), not ordered
    st.params[pp::BcDownsample] = static_cast<float>(k);
    const double e = errorDb(st);
    char buf[32];
    std::snprintf(buf, sizeof buf, " %d:%.1f", k, e);
    line += buf;
    if (e > prev) ++ok;
    prev = e;
  }
  std::printf("# downsample -> error dB:%s\n", line.c_str());
  expect(ok == 5, "Downsample 1..16: more reduction, more error (monotonic)");
  st.params[pp::BcDownsample] = 1;
  expect(errorDb(st) < -125, "Downsample 1 with 24 bits: transparent (the default no longer masks the bit depth)");
  st.params[pp::BcDownsample] = 50;
  expect(errorDb(st) > -10, "Downsample 50: heavily reduced");
  // The default instance: the bit depth is what you hear (each 4 bits ~24 dB cleaner).
  PluginState def = defaultPluginState("gl.bitcrush");
  def.params[pp::BcBits] = 8;
  const double e8 = errorDb(def);
  def.params[pp::BcBits] = 16;
  const double e16 = errorDb(def);
  expect(e16 < e8 - 30, "default instance: Bits 8 -> 16 is clearly cleaner (no masking by the rate reduction)");
}

// Song-level: ParamId set / get and project save / load keep the top values.
void testRoundTrip() {
  Song s = makeEmptySong();
  s.mixer[1].slots[0].plugin = defaultPluginState("gl.bitcrush");
  const ObjectId t = s.mixer[1].id;
  expect(setParam(s, slotParam(t, 0, pp::BcBits), 24.0) && setParam(s, slotParam(t, 0, pp::BcDownsample), 50.0), "set Bits 24 / Downsample 50");
  Json j;
  Song back;
  expect(jsonParse(jsonStringify(songToJson(s)), j) && songFromJson(j, back), "project round trip");
  expect(getParam(back, slotParam(t, 0, pp::BcBits)).value_or(0) == 24.0 && getParam(back, slotParam(t, 0, pp::BcDownsample)).value_or(0) == 50.0,
         "Bits 24 / Downsample 50 survive save / load");
}

// Other bit crushers reach their top.
void testOthers() {
  const PluginInfo* dly = findPlugin("gl.delay");
  expect(dly && dly->params[pp::DlyBits].fromNormalized(1.0) == 16.0, "Tracer bit depth: knob reaches 16");
  const PluginInfo* hex = findPlugin("gl.hexsynth");
  bool found = false;
  if (hex)
    for (const auto& p : hex->params)
      if (p.name == "FX 1 lo-fi bits") found = p.fromNormalized(1.0) == p.max;
  expect(found, "Hex Synth lo-fi bits: knob reaches its top");
}

}  // namespace

int main() {
  gl::test::hermeticTestEnv("bitcrush_range");
  testBits();
  testDownsample();
  testRoundTrip();
  testOthers();
  return finish();
}
