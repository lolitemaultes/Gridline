// Gridline: K-248, warp markers on a sample instrument's sample (core/SampleWarp.h,
// engine/SampleWarpRender.h). Checks:
//  * the markers' text round trip and the project field (saved only when set);
//  * identity markers (or warp off) leave the very same sample: old projects sound the same;
//  * a moved marker moves the hit under it: the onset lands where the marker went, within 1 ms;
//  * the warped sample is the same at every render (prepared once, off the audio thread: the voice
//    plays a finished sample and allocates nothing);
//  * edits: add (snapping to a transient within reach), move (the start pinned, neighbours kept in
//    order), slide, remove, add at transients, quantize at a strength;
//  * Kerf's channel data (prepareBuiltinAux) plays the warped sample.
#include <cmath>
#include <filesystem>
#include <string>
#include "Check.h"
#include "Hermetic.h"
#include "core/PluginInfo.h"
#include "core/PluginParams.h"
#include "core/SampleWarp.h"
#include "engine/Program.h"
#include "engine/Sample.h"
#include "engine/SampleWarpRender.h"
#include "engine/Slicing.h"
#include "packs/edison/edison.h"

using namespace gl;
namespace ed = gl::packs::edison;

namespace {

constexpr double kRate = 44100;
std::string gDir;

// Two seconds with short bursts at 0.5 s and 1.5 s.
SamplePtr clicks() {
  auto s = std::make_shared<Sample>();
  s->sampleRate = kRate;
  s->left.assign(static_cast<size_t>(2 * kRate), 0.0f);
  for (double at : {0.5, 1.5})
    for (int i = 0; i < 400; ++i)
      s->left[static_cast<size_t>(at * kRate) + static_cast<size_t>(i)] = 0.8f * static_cast<float>(std::exp(-i / 80.0) * std::sin(i * 0.7));
  s->setMono();
  return s;
}

double firstOnsetAfter(const Sample& s, double from) {
  for (int64_t i = static_cast<int64_t>(from * s.sampleRate); i < static_cast<int64_t>(s.frames()); ++i)
    if (std::fabs(s.left[static_cast<size_t>(i)]) > 0.02f) return static_cast<double>(i) / s.sampleRate;
  return -1;
}

void testText() {
  SampleWarp w;
  w.on = true;
  w.bpm = 120;
  w.markers = {{1, 0.0, 0.0}, {2, 0.5, 0.6}, {3, 2.0, 2.0}};
  SampleWarp back;
  expect(sampleWarpFromText(sampleWarpToText(w), back) && back == w, "the markers' text round trip");
  PluginState st;
  st.type = "gl.sampler";
  expect(st.warp.empty() && !sampleWarpOf(st).on, "no warp: nothing stored");
  setSampleWarp(st, w);
  expect(!st.warp.empty() && sampleWarpOf(st) == w, "stored on the plugin state");
  setSampleWarp(st, SampleWarp{});
  expect(st.warp.empty(), "cleared: nothing stored (the project file stays as it was)");
  expect(sampleWarpSupported("gl.sampler") && sampleWarpSupported("gl.slicer") && sampleWarpSupported("gl.pads") && !sampleWarpSupported("gl.eq"),
         "the sample instruments take warp markers");
}

void testIdentity() {
  const SamplePtr s = clicks();
  SampleWarp w;
  w.on = true;
  w.markers = {{1, 0.0, 0.0}, {2, 1.0, 1.0}, {3, 2.0, 2.0}};
  expect(warpedSample(s, w) == s, "identity markers: the very same sample");
  w.markers[1].to = 1.2;
  w.on = false;
  expect(warpedSample(s, w) == s, "warp off: the very same sample");
}

void testMoved() {
  const SamplePtr s = clicks();
  SampleWarp w;
  w.on = true;
  w.markers = {{1, 0.0, 0.0}, {2, 0.5, 0.6}, {3, 2.0, 2.0}};
  const SamplePtr o = warpedSample(s, w);
  expect(o && o != s, "moved markers: a warped sample");
  if (!o) return;
  const double on = firstOnsetAfter(*o, 0.0);
  std::printf("# onset %.4f s (marker at 0.6), length %.4f s\n", on, o->seconds());
  expect(std::fabs(on - 0.6) <= 0.001, "the hit lands where its marker went (within 1 ms)");
  expect(std::fabs(o->seconds() - 2.0) <= 0.002, "the length: the last marker's time");
  const double on2 = firstOnsetAfter(*o, 1.0);
  expect(std::fabs(on2 - (0.6 + 1.0 * 1.4 / 1.5)) <= 0.002, "the hit in the next section lands where the section stretches it");
  // The same every time, and from the cache.
  const SamplePtr again = renderWarpedSample(*s, w.markers);
  expect(again && again->left == o->left, "the warped sample is the same at every render");
  expect(warpedSample(s, w) == o, "prepared once (the cache)");
}

void testEdits() {
  const double fileSec = 2.0;
  std::vector<WarpMarker> m;
  const std::vector<double> onsets = {0.5, 1.5};
  const uint32_t a = sampleWarpAdd(m, 0.51, fileSec, &onsets, 0.04);
  expect(a != 0 && m.size() >= 1, "add");
  bool snapped = false;
  for (const WarpMarker& k : m)
    if (k.id == a) snapped = std::fabs(k.at - 0.5) < 1e-6;
  expect(snapped, "a new marker snaps to the transient within reach");
  expect(sampleWarpMove(m, a, 0.7, fileSec), "move");
  expect(std::fabs(sampleWarpTimeOf(m, 0.5) - 0.7) < 1e-6, "the moved marker's audio plays at its new time");
  expect(std::fabs(sampleWarpTimeOf(m, 0.0)) < 1e-6, "the start stays pinned");
  sampleWarpMove(m, a, -5.0, fileSec);
  const std::vector<WarpMarkerAt> v = sampleWarpViews(m);
  bool ordered = true;
  for (size_t i = 1; i < v.size(); ++i) ordered = ordered && v[i].w > v[i - 1].w;
  expect(ordered, "a marker cannot pass its neighbour");
  expect(sampleWarpRemove(m, a), "remove");
  std::vector<WarpMarker> t;
  const int n = sampleWarpAtOnsets(t, onsets, fileSec);
  expect(n == 2, "add at transients: one marker per transient");
  // Quantize: markers at 0.52 and 1.47 on a 0.5 s grid at full strength.
  std::vector<WarpMarker> q = {{1, 0.0, 0.0}, {2, 0.52, 0.52}, {3, 1.47, 1.47}, {4, 2.0, 2.0}};
  sampleWarpQuantize(q, 0.5, 1.0, fileSec);
  expect(std::fabs(sampleWarpTimeOf(q, 0.52) - 0.5) < 1e-6 && std::fabs(sampleWarpTimeOf(q, 1.47) - 1.5) < 1e-6, "quantize at 100 %: on the grid");
  std::vector<WarpMarker> h = {{1, 0.0, 0.0}, {2, 0.52, 0.52}, {4, 2.0, 2.0}};
  sampleWarpQuantize(h, 0.5, 0.5, fileSec);
  expect(std::fabs(sampleWarpTimeOf(h, 0.52) - 0.51) < 1e-6, "quantize at 50 %: halfway");
}

void testKerf() {
  ed::Audio a = ed::makeAudio(kRate, 1, static_cast<int64_t>(2 * kRate));
  const SamplePtr c = clicks();
  for (size_t i = 0; i < c->left.size(); ++i) a.L[i] = c->left[i];
  const std::string path = gDir + "/clicks.wav";
  ed::writeWavFile(path, ed::makeContent(a), 32);
  PluginState st;
  st.type = "gl.slicer";
  st.data = path;
  SampleWarp w;
  w.on = true;
  w.markers = {{1, 0.0, 0.0}, {2, 0.5, 0.6}, {3, 2.0, 2.0}};
  setSampleWarp(st, w);
  ChannelAux aux;
  expect(prepareBuiltinAux(st, aux) && !aux.samples.empty() && aux.samples[0], "Kerf prepares its sample");
  if (aux.samples.empty() || !aux.samples[0]) return;
  expect(std::fabs(firstOnsetAfter(*aux.samples[0], 0.0) - 0.6) <= 0.001, "Kerf plays the warped sample");
  // Slice at warp markers: a slice where the marker plays (0.6 s), cut from the warped audio.
  st.params.assign(static_cast<size_t>(gl::pp::SlCount), 0.0f);
  st.params[static_cast<size_t>(gl::pp::SlMode)] = static_cast<float>(gl::pp::SlModeWarp);
  ChannelAux aux2;
  prepareBuiltinAux(st, aux2);
  std::printf("# %zu slices\n", aux2.slices.size());
  expect(aux2.slices.size() == 2 && aux2.slices[0] == 0 && std::llabs(aux2.slices[1] - static_cast<int64_t>(0.6 * kRate)) <= 1,
         "Slice at warp markers: a slice where the marker plays");
  expect(warpSlices(st, *aux2.samples[0]) == aux2.slices, "the editor shows the engine's slices");
}

}  // namespace

int main() {
  gDir = gl::test::hermeticTestEnv("k248w");
  testText();
  testIdentity();
  testMoved();
  testEdits();
  testKerf();
  return finish();
}
