// Gridline: K-239, Kerf's Fit to tempo with Keep pitch. The loop (4 beats of a 440 Hz tone, 2 s:
// 120 BPM) plays at 100 BPM. Checks:
//  * Keep pitch off (the old fit, and the default): the loop plays 2.4 s and its pitch follows
//    (440 x 2 / 2.4), unchanged from before;
//  * Keep pitch on: the loop plays 2.4 s at 440 Hz (through the one stretch, engine/TimeStretch);
//    a slice keeps its pitch, and a note an octave up plays an octave up at the same length;
//    the slice's own speed stays a speed (its pitch following, as before);
//  * the slice's level is the same as the old fit's (the stretch adds no loss or boost);
//  * no fit (Fit to tempo off): Keep pitch changes nothing, sample for sample;
//  * a new parameter, off in old projects; the Fit to tempo preset keeps the pitch;
//  * K-351: on for new instances, while a state saved before it (shorter) loads with it off and
//    renders sample for sample as the old fit, and a saved value (on or off) is kept.
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>
#include "Check.h"
#include "core/PluginInfo.h"
#include "core/PluginParams.h"
#include "engine/Dsp.h"
#include "engine/Generators.h"

using namespace gl;
using namespace gl::pp;

namespace {

constexpr double kSr = 44100.0;

struct Buf {
  std::vector<float> l, r;
};

SamplePtr tone(size_t frames, double hz) {
  auto s = std::make_shared<Sample>();
  s->sampleRate = kSr;
  s->left.resize(frames), s->right.resize(frames);
  for (size_t i = 0; i < frames; ++i) s->left[i] = s->right[i] = static_cast<float>(0.5 * std::sin(2 * dsp::kPi * hz * static_cast<double>(i) / kSr));
  return s;
}

PluginState kerf(std::initializer_list<std::pair<int, float>> set) {
  PluginState s = defaultPluginState("gl.slicer");
  s.params[static_cast<size_t>(SlMode)] = static_cast<float>(SlModeManual);
  s.params[static_cast<size_t>(slMarker(0))] = 0.f;
  s.params[static_cast<size_t>(slMarker(1))] = 0.5f;
  s.params[static_cast<size_t>(SlDeclick)] = 0.f;
  s.params[static_cast<size_t>(SlLoopBeats)] = 4.f;
  for (auto [k, v] : set) s.params[static_cast<size_t>(k)] = v;
  return s;
}

// One held note at `pitch` for n frames at `tempo` (prepareHost first, as a program build does).
Buf run(const PluginState& s, const ChannelAux& aux, size_t n, double pitch = 60, double tempo = 100) {
  auto g = createGenerator(s.type);
  g->prepareHost(s, kSr);
  ProcessContext c;
  c.sampleRate = kSr;
  c.tempo = tempo;
  Buf out;
  out.l.assign(n, 0.f), out.r.assign(n, 0.f);
  g->noteOn(1, pitch, 1.f, 0, NoteProps{});
  for (size_t pos = 0; pos < n; pos += 256) {
    const int k = static_cast<int>(std::min<size_t>(256, n - pos));
    g->process(s, aux, c, out.l.data() + pos, out.r.data() + pos, k);
  }
  return out;
}

double freq(const std::vector<float>& v, size_t a, size_t b) {
  int n = 0;
  size_t first = 0, last = 0;
  for (size_t i = a + 1; i < b && i < v.size(); ++i)
    if (v[i - 1] < 0 && v[i] >= 0) {
      if (n == 0) first = i;
      last = i, ++n;
    }
  return n > 1 ? (n - 1) * kSr / static_cast<double>(last - first) : 0.0;
}

double rms(const std::vector<float>& v, size_t a, size_t b) {
  double acc = 0;
  b = std::min(b, v.size());
  for (size_t i = a; i < b; ++i) acc += static_cast<double>(v[i]) * v[i];
  return b > a ? std::sqrt(acc / static_cast<double>(b - a)) : 0.0;
}

// Seconds until the sound ends (the last frame above -60 dB).
double soundLength(const std::vector<float>& v) {
  for (size_t i = v.size(); i-- > 0;)
    if (std::fabs(v[i]) > 1e-3f) return static_cast<double>(i + 1) / kSr;
  return 0;
}

}  // namespace

int main() {
  ChannelAux aux;
  aux.samples = {tone(88200, 440)};  // 2 s: 4 beats at 120 BPM
  const size_t n = static_cast<size_t>(1.8 * kSr);  // the first slice (half the loop) and after
  const float on = 1.f;
  // The old fit: speed, the pitch follows.
  const Buf vari = run(kerf({{SlFitTempo, on}, {SlFitKeepPitch, 0.f}}), aux, n);
  const double fv = freq(vari.l, 4410, 44100), lv = soundLength(vari.l);
  std::printf("# fit by speed: %.1f Hz, %.3f s\n", fv, lv);
  expect(std::fabs(fv - 440 * 2 / 2.4) < 1.5, "Keep pitch off: the pitch follows the tempo (as before)");
  expect(std::fabs(lv - 1.2) < 0.01, "Keep pitch off: the slice plays its fitted length");
  // Keep pitch.
  const Buf kp = run(kerf({{SlFitTempo, on}, {SlFitKeepPitch, on}}), aux, n);
  const double fk = freq(kp.l, 4410, 44100), lk = soundLength(kp.l);
  std::printf("# fit keeping the pitch: %.1f Hz, %.3f s\n", fk, lk);
  expect(std::fabs(fk - 440) < 1.5, "Keep pitch: the slice keeps its pitch at another tempo");
  expect(std::fabs(lk - 1.2) < 0.03, "Keep pitch: the slice plays its fitted length");
  const double rv = rms(vari.l, 4410, 44100), rk = rms(kp.l, 4410, 44100);
  std::printf("# level: speed %.4f, stretch %.4f\n", rv, rk);
  expect(std::fabs(20 * std::log10(rk / rv)) < 1.0, "Keep pitch: the same level as the fit by speed");
  // Faster than the loop (160 BPM): a shorter slice, the same pitch.
  const Buf fast = run(kerf({{SlFitTempo, on}, {SlFitKeepPitch, on}}), aux, n, 60, 160);
  expect(std::fabs(freq(fast.l, 2205, 22050) - 440) < 1.5 && std::fabs(soundLength(fast.l) - 0.75) < 0.03, "Keep pitch at 160 BPM: shorter, the same pitch");
  // A note an octave up, chromatic on slice 1: an octave up, the same length.
  const Buf oct = run(kerf({{SlFitTempo, on}, {SlFitKeepPitch, on}, {SlChromatic, 0.f}}), aux, n, 72);
  const double fo = freq(oct.l, 4410, 44100);
  std::printf("# an octave up: %.1f Hz, %.3f s\n", fo, soundLength(oct.l));
  expect(std::fabs(fo - 880) < 3 && std::fabs(soundLength(oct.l) - 1.2) < 0.03, "Keep pitch: a note's pitch still tunes the slice, its length stays the fit");
  // The slice's speed stays a speed.
  const Buf sp = run(kerf({{SlFitTempo, on}, {SlFitKeepPitch, on}, {slParam3(0, SliceSpeed), 2.f}}), aux, n);
  expect(std::fabs(freq(sp.l, 2205, 22050) - 880) < 3 && std::fabs(soundLength(sp.l) - 0.6) < 0.03, "the slice's speed: faster and higher, as before");
  // No fit: Keep pitch changes nothing.
  const Buf a = run(kerf({{SlFitKeepPitch, 0.f}}), aux, n), b = run(kerf({{SlFitKeepPitch, on}}), aux, n);
  expect(a.l == b.l && a.r == b.r, "Fit to tempo off: Keep pitch changes nothing, sample for sample");
  // The parameter.
  const PluginState d = defaultPluginState("gl.slicer");
  expect(d.params.size() == static_cast<size_t>(SlCount) && d.params[static_cast<size_t>(SlFitKeepPitch)] == 1.f, "Keep pitch: a parameter, on for a new instance");
  // K-351: a state saved before Keep pitch (its parameters end before it) loads with it off.
  PluginState old = kerf({{SlFitTempo, on}});
  old.params.resize(static_cast<size_t>(SlFitKeepPitch));
  normalizePluginState(old);
  expect(old.params.size() == static_cast<size_t>(SlCount) && old.params[static_cast<size_t>(SlFitKeepPitch)] == 0.f,
         "an old project's Kerf loads with Keep pitch off");
  const Buf oldRun = run(old, aux, n);
  expect(oldRun.l == vari.l && oldRun.r == vari.r, "an old project's fitted Kerf renders sample for sample as before");
  // A saved value is kept, either way.
  for (float v : {0.f, 1.f}) {
    PluginState saved = kerf({{SlFitTempo, on}, {SlFitKeepPitch, v}});
    normalizePluginState(saved);
    expect(saved.params[static_cast<size_t>(SlFitKeepPitch)] == v, v > 0 ? "a saved Keep pitch on stays on" : "a saved Keep pitch off stays off");
  }
  // A new instance with Fit to tempo turned on keeps the pitch.
  const Buf fresh = run(kerf({{SlFitTempo, on}}), aux, n);
  expect(std::fabs(freq(fresh.l, 4410, 44100) - 440) < 1.5, "a new Kerf fitted to the tempo keeps its pitch");
  const PluginInfo* info = findPlugin("gl.slicer");
  bool preset = false;
  if (info)
    for (const auto& p : info->presets)
      if (p.name == "Fit to tempo")
        for (const auto& [k, v] : p.values) preset = preset || (k == SlFitKeepPitch && v == 1.f);
  expect(preset, "the Fit to tempo preset keeps the pitch");
  return finish();
}
