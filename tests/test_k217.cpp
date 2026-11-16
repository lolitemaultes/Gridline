// Gridline — K-217 tests: Kerf's classic engine (GitHub #54).
//   Stage 1: Mod X, Mod Y and velocity routed to a chosen target with an amount;
//            the defaults keep the old fixed routes.
//   Stage 2: per-slice speed; a slide to another slice's key moves on to that slice.
//   Stage 3: envelope sets (volume ADSR; pan, pitch and filter envelopes), picked per slice.
//   Stage 4: the LFO (shape, rate or tempo sync, delay, per-note phase; volume, pan, pitch, cutoff).
//   Stage 5: slice layers A / B (own cuts and settings; the layer parameter and switch keys).
//   Stage 6: FL Slicex import: articulators, LFO, mod routes and start offsets onto the Classic engine.
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>
#include "Check.h"
#include "core/PluginInfo.h"
#include "core/PluginParams.h"
#include "engine/Dsp.h"
#include "core/flp/Flp.h"
#include "core/flp/FlpEncode.h"
#include "engine/Generators.h"
#include "engine/Program.h"
#include "engine/SlicexEngine.h"

using namespace gl;
using namespace gl::pp;

namespace {

constexpr double kSr = 44100.0;

struct Buf {
  std::vector<float> l, r;
};

ProcessContext ctx() {
  ProcessContext c;
  c.sampleRate = kSr;
  c.tempo = 120;
  return c;
}

SamplePtr makeSample(size_t frames, double hz, double amp = 0.5) {
  auto s = std::make_shared<Sample>();
  s->sampleRate = kSr;
  s->left.resize(frames), s->right.resize(frames);
  for (size_t i = 0; i < frames; ++i) s->left[i] = s->right[i] = static_cast<float>(amp * std::sin(2 * dsp::kPi * hz * static_cast<double>(i) / kSr));
  return s;
}

// Two manual slices (0 %, 50 %) of the loop, declick off.
PluginState kerf(std::initializer_list<std::pair<int, float>> set = {}) {
  PluginState s = defaultPluginState("gl.slicer");
  s.params[static_cast<size_t>(SlMode)] = static_cast<float>(SlModeManual);
  s.params[static_cast<size_t>(slMarker(0))] = 0.f;
  s.params[static_cast<size_t>(slMarker(1))] = 0.5f;
  s.params[static_cast<size_t>(SlDeclick)] = 0.f;
  for (auto [k, v] : set) s.params[static_cast<size_t>(k)] = v;
  return s;
}

// One note (pitch 60, velocity `vel`, note mod X / Y) for n frames; `mid` changes the state halfway.
Buf run(const PluginState& s, const ChannelAux& aux, size_t n, float vel = 1.f, NoteProps np = {}, const PluginState* mid = nullptr) {
  auto g = createGenerator(s.type);
  Buf out;
  out.l.assign(n, 0.f), out.r.assign(n, 0.f);
  g->noteOn(1, 60, vel, 0, np);
  for (size_t pos = 0; pos < n; pos += 256) {
    const int k = static_cast<int>(std::min<size_t>(256, n - pos));
    g->process(mid && pos >= n / 2 ? *mid : s, aux, ctx(), out.l.data() + pos, out.r.data() + pos, k);
  }
  return out;
}

double rms(const std::vector<float>& v, size_t a = 0, size_t b = SIZE_MAX) {
  b = std::min(b, v.size());
  double acc = 0;
  for (size_t i = a; i < b; ++i) acc += static_cast<double>(v[i]) * v[i];
  return b > a ? std::sqrt(acc / static_cast<double>(b - a)) : 0.0;
}

// Frequency from rising zero crossings over [a, b).
double freq(const std::vector<float>& v, size_t a, size_t b) {
  int n = 0;
  size_t first = 0, last = 0;
  for (size_t i = a + 1; i < b; ++i)
    if (v[i - 1] < 0 && v[i] >= 0) {
      if (n == 0) first = i;
      last = i, ++n;
    }
  return n > 1 ? (n - 1) * kSr / static_cast<double>(last - first) : 0.0;
}

bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

void testStage1() {
  ChannelAux aux;
  aux.samples = {makeSample(44100, 300)};
  const auto ix = [](int p) { return static_cast<float>(p); };
  // Defaults: X on cutoff, Y on resonance, velocity on level (as before K-217).
  {
    const PluginState d = defaultPluginState("gl.slicer");
    expect(std::lround(d.params[static_cast<size_t>(SlModXDest)]) == SlDestCutoff && std::lround(d.params[static_cast<size_t>(SlModYDest)]) == SlDestReso &&
               std::lround(d.params[static_cast<size_t>(SlVelDest)]) == SlDestLevel && d.params[static_cast<size_t>(SlModXAmt)] == 1.f,
           "defaults: X cutoff, Y resonance, velocity level");
    ChannelAux hi;
    hi.samples = {makeSample(44100, 3000)};
    const PluginState lp = kerf({{SlFilterType, 1.f}, {SlCutoff, 500.f}});
    const Buf dark = run(lp, hi, 8000), open = run(kerf({{SlFilterType, 1.f}, {SlCutoff, 500.f}, {SlModX, 1.f}}), hi, 8000);
    expect(rms(open.l, 2000) > 3 * rms(dark.l, 2000), "default route: Mod X opens the cutoff");
    const Buf off = run(kerf({{SlFilterType, 1.f}, {SlCutoff, 500.f}, {SlModX, 1.f}, {SlModXDest, ix(SlDestOff)}}), hi, 8000);
    expect(rms(off.l, 2000) == rms(dark.l, 2000), "Mod X routed off: no change");
    const Buf soft = run(kerf(), aux, 6000, 0.25f), loud = run(kerf(), aux, 6000, 1.f);
    expect(near(rms(soft.l, 1000) / rms(loud.l, 1000), 0.25, 0.01), "default route: velocity sets the level");
  }
  // Mod X on pitch: full X is an octave up.
  {
    const Buf b = run(kerf({{SlModXDest, ix(SlDestPitch)}, {SlModX, 1.f}}), aux, 8000);
    expect(near(freq(b.l, 500, 8000), 600, 6), "Mod X on pitch: +12 semitones at full X");
    const Buf neg = run(kerf({{SlModXDest, ix(SlDestPitch)}, {SlModX, 1.f}, {SlModXAmt, -1.f}}), aux, 8000);
    expect(near(freq(neg.l, 500, 8000), 150, 3), "negative amount: an octave down");
  }
  // Mod Y on level: bottom Y is -12 dB.
  {
    const Buf flat = run(kerf(), aux, 6000), low = run(kerf({{SlModYDest, ix(SlDestLevel)}, {SlModY, 0.f}}), aux, 6000);
    expect(near(rms(low.l, 1000) / rms(flat.l, 1000), std::pow(10.0, -12.0 / 20.0), 0.01), "Mod Y on level: -12 dB at the bottom");
  }
  // Note mod X adds to the knob; Mod X on pan.
  {
    NoteProps np;
    np.modX = 1.f;
    const Buf b = run(kerf({{SlModXDest, ix(SlDestPan)}}), aux, 6000, 1.f, np);
    expect(rms(b.l, 1000) < 1e-3 && rms(b.r, 1000) > 0.1, "note Mod X on pan: hard right");
  }
  // Velocity on pitch: the level no longer follows velocity; half velocity is 6 semitones down.
  {
    const PluginState s = kerf({{SlVelDest, ix(SlDestPitch)}});
    const Buf half = run(s, aux, 8000, 0.5f), full = run(s, aux, 8000, 1.f);
    expect(near(freq(half.l, 500, 8000), 300 * std::pow(2.0, -0.5), 3) && near(freq(full.l, 500, 8000), 300, 3), "velocity on pitch");
    expect(near(rms(half.l, 1000) / rms(full.l, 1000), 1.0, 0.03), "velocity on pitch: not on the level too");
  }
  // Velocity on the start point is unipolar (never before the slice); Mod X on start skips in.
  {
    ChannelAux ramp;
    auto smp = std::make_shared<Sample>(*makeSample(44100, 300));
    for (size_t i = 0; i < 22050; ++i) smp->left[i] = smp->right[i] = smp->left[i] * static_cast<float>(i) / 22050.f;  // slice 1 fades in
    ramp.samples = {smp};
    const Buf a = run(kerf(), ramp, 2000), b = run(kerf({{SlModXDest, ix(SlDestStart)}, {SlModX, 1.f}, {SlModXAmt, 0.5f}}), ramp, 2000);
    expect(rms(b.l) > 5 * rms(a.l), "Mod X on start: the slice starts later in");
  }
  // Live: moving Mod X while the note plays retunes it.
  {
    const PluginState s = kerf({{SlModXDest, ix(SlDestPitch)}});
    PluginState up = s;
    up.params[static_cast<size_t>(SlModX)] = 1.f;
    const Buf b = run(s, aux, 16000, 1.f, {}, &up);
    expect(near(freq(b.l, 1000, 7900), 300, 3) && near(freq(b.l, 8500, 16000), 600, 6), "Mod X moved during a note retunes it");
  }
}

// A loop whose first half is a 300 Hz tone and second half a 500 Hz tone.
SamplePtr twoTones() {
  auto s = std::make_shared<Sample>();
  s->sampleRate = kSr;
  s->left.resize(44100), s->right.resize(44100);
  for (size_t i = 0; i < 44100; ++i)
    s->left[i] = s->right[i] = static_cast<float>(0.5 * std::sin(2 * dsp::kPi * (i < 22050 ? 300.0 : 500.0) * static_cast<double>(i) / kSr));
  return s;
}

void testStage2() {
  ChannelAux aux;
  aux.samples = {twoTones()};
  // Per-slice speed: x2 plays an octave up and ends in half the time; slice 2 keeps its own.
  {
    const Buf one = run(kerf(), aux, 30000), fast = run(kerf({{slParam3(0, SliceSpeed), 2.f}}), aux, 30000);
    expect(near(freq(one.l, 500, 10000), 300, 3) && near(freq(fast.l, 500, 10000), 600, 6), "slice speed x2: an octave up");
    expect(rms(one.l, 14000, 20000) > 0.1 && rms(fast.l, 12000, 20000) < 1e-4, "slice speed x2: half as long");
    const PluginState d = defaultPluginState("gl.slicer");
    expect(d.params[static_cast<size_t>(slParam3(5, SliceSpeed))] == 1.f && d.params[static_cast<size_t>(slParam3(5, SliceEnvSet))] == 0.f,
           "slice speed defaults to x1, envelope set to Main");
  }
  // Slide to another slice: note 60 (slice 1, 300 Hz) slides to 61 (slice 2, 500 Hz) over 0.1 s.
  {
    const PluginState s = kerf();
    auto g = createGenerator("gl.slicer");
    Buf out;
    const size_t n = 26000;
    out.l.assign(n, 0.f), out.r.assign(n, 0.f);
    g->noteOn(1, 60, 1.f, 0);
    for (size_t pos = 0; pos < n; pos += 256) {
      if (pos == 4864) g->glide(1, 61, 0.1, 0);
      if (pos == 22016) g->noteOff(1, 0);
      const int k = static_cast<int>(std::min<size_t>(256, n - pos));
      g->process(s, aux, ctx(), out.l.data() + pos, out.r.data() + pos, k);
    }
    expect(near(freq(out.l, 500, 4800), 300, 3), "before the slide: slice 1");
    const double during = freq(out.l, 6400, 7400);  // (after the 30 ms crossfade, a third into the slide)
    expect(during > 465 && during < 495, "the slide starts slice 2 at the pitch sounding (a semitone under its own)");
    expect(near(freq(out.l, 14000, 21500), 500, 5), "after the slide: slice 2 at its own pitch");
    expect(rms(out.l, 14000, 21500) > 0.2 && rms(out.l, 14000, 21500) < 0.45, "the old slice has faded out (one voice sounding)");
    expect(rms(out.l, 24000, 26000) < 1e-3, "note off after the slide ends the new slice");
  }
  // A slide on the same slice (one slice played chromatically) still retunes it.
  {
    const PluginState s = kerf({{SlChromatic, 0.f}});
    auto g = createGenerator("gl.slicer");
    Buf out;
    out.l.assign(16000, 0.f), out.r.assign(16000, 0.f);
    g->noteOn(1, 60, 1.f, 0);
    for (size_t pos = 0; pos < 16000; pos += 256) {
      if (pos == 2048) g->glide(1, 72, 0.05, 0);
      g->process(s, aux, ctx(), out.l.data() + pos, out.r.data() + pos, 256);
    }
    expect(near(freq(out.l, 6000, 11000), 600, 6), "a slide on one slice retunes it (+12)");
  }
}

// Note `pitch` held for n frames.
Buf hold(const PluginState& s, const ChannelAux& aux, double pitch, size_t n) {
  auto g = createGenerator("gl.slicer");
  Buf out;
  out.l.assign(n, 0.f), out.r.assign(n, 0.f);
  g->noteOn(1, pitch, 1.f, 0);
  for (size_t pos = 0; pos < n; pos += 256) {
    const int k = static_cast<int>(std::min<size_t>(256, n - pos));
    g->process(s, aux, ctx(), out.l.data() + pos, out.r.data() + pos, k);
  }
  return out;
}

void testStage3() {
  ChannelAux aux;
  aux.samples = {makeSample(44100, 300)};
  // Main: the volume decays to its sustain.
  {
    const Buf flat = hold(kerf(), aux, 60, 20000);
    const Buf dec = hold(kerf({{SlVolDecay, 0.05f}, {SlVolSustain, 0.25f}}), aux, 60, 20000);
    expect(near(rms(dec.l, 12000, 20000) / rms(flat.l, 12000, 20000), 0.25, 0.02), "Main volume: decays to its sustain");
    expect(near(rms(dec.l, 0, 200) / rms(flat.l, 0, 200), 1.0, 0.1), "Main volume: the attack reaches full level first");
  }
  // A slice on set 2 plays set 2's volume; the others keep Main's.
  {
    const PluginState s = kerf({{slParam3(1, SliceEnvSet), 1.f}, {slVolEnv(1, SlEnvD), 0.02f}, {slVolEnv(1, SlEnvS), 0.f}});
    const Buf one = hold(s, aux, 60, 20000), two = hold(s, aux, 61, 20000);
    expect(rms(one.l, 12000, 20000) > 0.2 && rms(two.l, 12000, 20000) < 1e-3, "slice 2 on set 2 decays away, slice 1 holds (Main)");
  }
  // Pitch envelope: +12 at the start, back to the slice's pitch after its decay.
  {
    const Buf b = hold(kerf({{slModEnv(0, SlEnvPitch, SlEnvA), 0.0005f}, {slModEnv(0, SlEnvPitch, SlEnvD), 0.1f},
                             {slModEnv(0, SlEnvPitch, SlEnvAmt), 12.f}}),
                       aux, 60, 20000);
    expect(freq(b.l, 30, 500) > 450 && near(freq(b.l, 10000, 20000), 300, 3), "pitch envelope: starts near an octave up, settles");
  }
  // Pan envelope with sustain: hard right while held.
  {
    const Buf b = hold(kerf({{slModEnv(0, SlEnvPan, SlEnvA), 0.0005f}, {slModEnv(0, SlEnvPan, SlEnvS), 1.f}, {slModEnv(0, SlEnvPan, SlEnvAmt), 1.f}}),
                       aux, 60, 12000);
    expect(rms(b.l, 4000, 12000) < 1e-3 && rms(b.r, 4000, 12000) > 0.1, "pan envelope: to the right");
  }
  // Filter envelope: a low-pass opened by the envelope at the start, closed after.
  {
    ChannelAux hi;
    hi.samples = {makeSample(44100, 3000)};
    const Buf b = hold(kerf({{SlFilterType, 1.f}, {SlCutoff, 300.f}, {slModEnv(0, SlEnvCut, SlEnvA), 0.0005f}, {slModEnv(0, SlEnvCut, SlEnvD), 0.05f},
                             {slModEnv(0, SlEnvCut, SlEnvAmt), 6.f}}),
                       hi, 60, 20000);
    expect(rms(b.l, 100, 1000) > 4 * rms(b.l, 12000, 20000), "filter envelope: bright at the start, dark after");
  }
  // An amount of 0 leaves the sound exactly as without the envelope.
  {
    const Buf a = hold(kerf(), aux, 60, 8000), b = hold(kerf({{slModEnv(0, SlEnvPitch, SlEnvS), 1.f}}), aux, 60, 8000);
    expect(a.l == b.l, "an envelope at amount 0 changes nothing");
  }
}

// RMS of each 441-frame window (10 ms).
std::vector<double> envelopeOf(const std::vector<float>& v, size_t a, size_t b) {
  std::vector<double> out;
  for (size_t i = a; i + 441 <= b; i += 441) out.push_back(rms(v, i, i + 441));
  return out;
}

void testStage4() {
  ChannelAux aux;
  aux.samples = {makeSample(44100, 300)};
  // Volume LFO at 5 Hz, full depth: the level swings down to near silence 5 times a second.
  {
    const Buf b = hold(kerf({{SlLfoVol, 1.f}, {SlLfoRate, 5.f}}), aux, 60, 22000);
    const auto e = envelopeOf(b.l, 2000, 22000);
    double lo = 1, hi = 0;
    for (double x : e) lo = std::min(lo, x), hi = std::max(hi, x);
    expect(hi > 0.2 && lo < 0.15 * hi, "LFO to volume: a deep tremolo");
    const Buf flat = hold(kerf({{SlLfoRate, 5.f}}), aux, 60, 22000);
    expect(flat.l == hold(kerf(), aux, 60, 22000).l, "every depth 0: the LFO changes nothing");
  }
  // Pitch LFO: the frequency swings by the depth (12 st: 150..600 Hz), square shape.
  {
    const Buf b = hold(kerf({{SlLfoPitch, 12.f}, {SlLfoShape, static_cast<float>(SlLfoSquare)}, {SlLfoRate, 2.f}, {SlPlayToEnd, 1.f}}), aux, 60, 22000);
    const double up = freq(b.l, 1500, 10000), down = freq(b.l, 12500, 21500);
    expect(near(up, 600, 8) && near(down, 150, 3), "LFO to pitch, square: an octave up, then down");
  }
  // Delay: no movement at first, then the full swing.
  {
    const Buf b = hold(kerf({{SlLfoPitch, 12.f}, {SlLfoShape, static_cast<float>(SlLfoSquare)}, {SlLfoRate, 2.f}, {SlLfoDelay, 4.f}}), aux, 60, 4000);
    expect(freq(b.l, 300, 4000) < 330, "LFO delay: the start is (almost) still");
  }
  // Tempo sync: 1/4 at 120 BPM = 2 Hz, the same as a 2 Hz rate.
  {
    const Buf a = hold(kerf({{SlLfoVol, 1.f}, {SlLfoRate, 2.f}}), aux, 60, 20000);
    const Buf b = hold(kerf({{SlLfoVol, 1.f}, {SlLfoRate, 7.f}, {SlLfoSync, 5.f}}), aux, 60, 20000);
    double d = 0;
    for (size_t i = 0; i < a.l.size(); ++i) d = std::max(d, static_cast<double>(std::fabs(a.l[i] - b.l[i])));
    expect(d < 1e-3, "LFO sync 1/4 at 120 BPM: 2 Hz");
  }
  // Pan LFO: left and right take turns.
  {
    const Buf b = hold(kerf({{SlLfoPan, 1.f}, {SlLfoShape, static_cast<float>(SlLfoSquare)}, {SlLfoRate, 2.f}}), aux, 60, 22000);
    expect(rms(b.l, 1500, 10000) < 1e-3 && rms(b.r, 1500, 10000) > 0.1 && rms(b.r, 12500, 21500) < 1e-3, "LFO to pan: right, then left");
  }
}

// Stage 5: a loop whose first half is a 300 Hz sine and second half silence.
SamplePtr halfSample() {
  SamplePtr s = makeSample(44100, 300);
  auto* m = const_cast<Sample*>(s.get());
  for (size_t i = 22050; i < 44100; ++i) m->left[i] = m->right[i] = 0.f;
  return s;
}

// Notes (pitch, at frame) on one generator; the state switches to `later` from frame `switchAt`.
Buf play(const PluginState& s, const ChannelAux& aux, const std::vector<std::pair<double, size_t>>& notes, size_t n, const PluginState* later = nullptr,
         size_t switchAt = SIZE_MAX) {
  auto g = createGenerator("gl.slicer");
  Buf out;
  out.l.assign(n, 0.f), out.r.assign(n, 0.f);
  uint32_t id = 1;
  for (size_t pos = 0; pos < n; pos += 256) {
    for (const auto& [p, at] : notes)
      if (at >= pos && at < pos + 256) g->noteOn(id++, p, 1.f, static_cast<int>(at - pos));
    const int k = static_cast<int>(std::min<size_t>(256, n - pos));
    g->process(later && pos >= switchAt ? *later : s, aux, ctx(), out.l.data() + pos, out.r.data() + pos, k);
  }
  return out;
}

void testStage5() {
  ChannelAux aux;
  aux.samples = {halfSample()};
  const auto B = [](int pid) { return layerPid(1, pid); };
  const PluginInfo* info = findPlugin("gl.slicer");
  expect(info && info->params.size() == static_cast<size_t>(SlCount) && info->params[static_cast<size_t>(B(slParam(0, SliceGain)))].name == "Layer B slice 1 gain" &&
             info->params[static_cast<size_t>(B(slMarker(63)))].name == "Layer B slice 64 position" &&
             info->params[static_cast<size_t>(B(slParam3(63, SliceEnvSet)))].name == "Layer B slice 64 envelope set" &&
             SlCount == SlLayerKey + 1 + kMaxSlices * (1 + SliceFields + SliceFields2 + SliceFields3),
         "layer B: a copy of every per-slice parameter, after the old ones");
  expect(B(SlMode) == SlMode && B(SlModX) == SlModX && B(slParam2(63, SliceKey)) == SlBMarkers + kLayerBlock - 1, "layerPid maps only the per-slice blocks");
  // A (markers 0 / 50 %): key 61 plays the silent half.
  expect(rms(hold(kerf(), aux, 61, 8000).l) < 1e-4, "layer A: the second slice is silent");
  // B without markers of its own cuts where A does and, with default settings, sounds as A.
  {
    const Buf a = hold(kerf(), aux, 60, 8000), b = hold(kerf({{SlLayer, 1.f}}), aux, 60, 8000);
    expect(a.l == b.l && rms(hold(kerf({{SlLayer, 1.f}}), aux, 61, 8000).l) < 1e-4, "an empty layer B plays A's cuts");
  }
  // B's own markers (0 / 25 %): key 61 starts in the sounding half.
  const PluginState bCut = kerf({{SlLayer, 1.f}, {B(slMarker(0)), 0.f}, {B(slMarker(1)), 0.25f}});
  expect(rms(hold(bCut, aux, 61, 8000).l) > 0.1, "layer B: its own cuts");
  // B's per-slice settings: its slice 1 gain, not A's.
  {
    const Buf a = hold(kerf({{B(slParam(0, SliceGain)), -24.f}}), aux, 60, 8000);
    const Buf b = hold(kerf({{SlLayer, 1.f}, {B(slParam(0, SliceGain)), -24.f}}), aux, 60, 8000);
    expect(rms(a.l) > 0.2 && rms(b.l) < 0.1 * rms(a.l), "layer B: its own slice settings (A unaffected)");
    const Buf m = hold(kerf({{SlLayer, 1.f}, {B(slParam(0, SliceMute)), 1.f}}), aux, 60, 8000);
    expect(rms(m.l) < 1e-6, "layer B: its own mute");
  }
  // Key switch (36: A, 37: B): the switch keys do not sound; notes after them play the chosen layer.
  {
    PluginState s = bCut;
    s.params[static_cast<size_t>(SlLayer)] = 0.f;
    s.params[static_cast<size_t>(SlLayerKey)] = 36.f;
    const Buf sw = play(s, aux, {{37, 0}, {61, 512}}, 8000);
    expect(rms(sw.l, 600, 8000) > 0.1, "key switch to B: key 61 plays B's second slice");
    const Buf back = play(s, aux, {{37, 0}, {36, 256}, {61, 512}}, 8000);
    expect(rms(back.l) < 1e-4, "key switch back to A (and the switch keys are silent)");
    expect(rms(play(s, aux, {{61, 0}}, 8000).l) < 1e-4, "no switch: layer A");
    // A switch holds while the parameter stays; moving the parameter takes over.
    PluginState s2 = s;
    s2.params[static_cast<size_t>(SlLayer)] = 1.f;
    const Buf held = play(s, aux, {{37, 0}, {61, 4096}}, 8000);
    expect(rms(held.l, 4200, 8000) > 0.1, "a key switch holds while the parameter stays");
    const Buf autoA = play(s2, aux, {{61, 4096}}, 8000, &s, 2048);
    expect(rms(autoA.l) < 1e-4, "automating the layer to A: A plays");
    const Buf autoB = play(s, aux, {{61, 4096}}, 8000, &s2, 2048);
    expect(rms(autoB.l, 4200, 8000) > 0.1, "automating the layer to B: B plays");
    const Buf over = play(s2, aux, {{36, 1024}, {61, 4096}}, 8000);
    expect(rms(over.l) < 1e-4, "a switch key overrides the parameter");
  }
  // Old states (before layers) open on layer A with the switch keys off.
  {
    PluginState old = kerf();
    old.params.resize(static_cast<size_t>(SlLfoCut + 1));
    normalizePluginState(old);
    expect(old.params.size() == static_cast<size_t>(SlCount) && old.params[static_cast<size_t>(SlLayer)] == 0.f && old.params[static_cast<size_t>(SlLayerKey)] == -1.f &&
               old.params[static_cast<size_t>(B(slMarker(0)))] == -1.f && old.params[static_cast<size_t>(B(slParam3(0, SliceSpeed)))] == 1.f,
           "old states: layer A, no switch keys, B empty");
  }
}

// Stage 6: a Slicex state with articulators maps onto the classic engine's own settings.
using Bytes = std::vector<uint8_t>;
void put32(Bytes& b, uint32_t v) {
  for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
void put16(Bytes& b, uint16_t v) { b.push_back(static_cast<uint8_t>(v & 0xFF)), b.push_back(static_cast<uint8_t>(v >> 8)); }
void chunk(Bytes& out, const char* id, const Bytes& body) {
  out.insert(out.end(), id, id + 4);
  put32(out, static_cast<uint32_t>(body.size()));
  out.insert(out.end(), body.begin(), body.end());
  if (body.size() & 1) out.push_back(0);
}
// 16-bit mono 44.1 kHz sine WAV with cues at `starts`.
Bytes sineWav(uint32_t frames, const std::vector<uint32_t>& starts) {
  Bytes body = {'W', 'A', 'V', 'E'};
  Bytes fmt;
  put16(fmt, 1), put16(fmt, 1), put32(fmt, 44100), put32(fmt, 88200), put16(fmt, 2), put16(fmt, 16);
  chunk(body, "fmt ", fmt);
  Bytes data;
  for (uint32_t i = 0; i < frames; ++i) put16(data, static_cast<uint16_t>(static_cast<int16_t>(12000 * std::sin(i * 0.05))));
  chunk(body, "data", data);
  Bytes c;
  put32(c, static_cast<uint32_t>(starts.size()));
  for (size_t k = 0; k < starts.size(); ++k) {
    put32(c, static_cast<uint32_t>(k + 1)), put32(c, starts[k]);
    c.insert(c.end(), {'d', 'a', 't', 'a'});
    put32(c, 0), put32(c, 0), put32(c, starts[k]);
  }
  chunk(body, "cue ", c);
  Bytes riff = {'R', 'I', 'F', 'F'};
  put32(riff, static_cast<uint32_t>(body.size()));
  riff.insert(riff.end(), body.begin(), body.end());
  return riff;
}
float pv(const PluginState& s, int pid) { return s.params[static_cast<size_t>(pid)]; }

void testStage6() {
  namespace sx = gl::slicex;
  sx::Model md = sx::defaultModel();
  md.decks[0].rawPath = "loop.wav";
  md.decks[0].regions.assign(2, sx::Region{});
  md.decks[0].regions[1].rec[sx::RAmpGroup] = 1;    // region 2 plays with group 2
  md.decks[0].regions[1].rec[sx::RStart] = 6400;    // half way into its first 0.1 s
  // Group 1: a volume envelope (attack 0.2 units = 0.1 s, decay to a sustain of 0.5), a speed LFO,
  // mod X mapped onto the cutoff (rising).
  auto& g0 = md.groups[0];
  g0.arts[sx::TVol].env.enabled = 1;
  g0.arts[sx::TVol].env.pts = {{0, 0, 0, 0, 0, 0, 0}, {0.2, 1, 0, 0, 0, 0, 1}, {0.4, 0.5, 0, 0, 0, 0, 1}, {0.6, 0, 0, 0, 0, 0, 1}};
  g0.arts[sx::TVol].env.sustain = 2;
  g0.arts[sx::TVol].env.extra[0] = g0.arts[sx::TVol].env.extra[1] = g0.arts[sx::TVol].env.extra[3] = 128;
  g0.arts[sx::TSpeed].lfo.enabled = 1;
  g0.arts[sx::TSpeed].floats[2] = 0.25f;
  g0.arts[sx::TCut].maps[0].pts = {{0, 0.2, 0, 0, 0, 0, 0}, {1, 0.8, 0, 0, 0, 0, 2}};
  // Group 2: a pan envelope swinging right.
  auto& g1 = md.groups[1];
  g1.arts[sx::TPan].env.enabled = 1;
  g1.arts[sx::TPan].env.pts = {{0, 0.5, 0, 0, 0, 0, 0}, {0.4, 1.0, 0, 0, 0, 0, 1}, {1.0, 0.5, 0, 0, 0, 0, 1}};
  g1.arts[sx::TPan].env.sustain = -1;
  const Bytes state = sx::writeState(md, {sineWav(44100, {0, 22050}), Bytes{}});
  flp::enc::EncodeContext ec;
  flp::PluginMapping pm;
  const bool ok = flp::enc::decodeState("Slicex", state, pm, ec);
  expect(ok && pm.state.type == "gl.slicer" && std::lround(pv(pm.state, SlEngine)) == 1, "Slicex import: still the region engine");
  if (!ok) return;
  const PluginState& s = pm.state;
  expect(near(pv(s, SlAttack), 0.1, 1e-3) && near(pv(s, SlVolDecay), 0.2, 1e-3) && near(pv(s, SlVolSustain), 0.5, 1e-3) && near(pv(s, SlRelease), 0.3, 1e-3),
         "group 1's volume envelope: Main's ADSR (0.1 s, 0.2 s, 50 %, 0.3 s)");
  expect(std::lround(pv(s, slParam3(0, SliceEnvSet))) == 0 && std::lround(pv(s, slParam3(1, SliceEnvSet))) == 1, "each region takes its amp group's set");
  expect(pv(s, slModEnv(1, SlEnvPan, SlEnvAmt)) > 0.9f && near(pv(s, slModEnv(1, SlEnvPan, SlEnvA)), 0.2, 1e-3) && pv(s, slModEnv(1, SlEnvPan, SlEnvS)) < 0.01f,
         "group 2's pan envelope: set 2 pans right over 0.2 s and back");
  expect(pv(s, SlLfoPitch) > 0.5f && pv(s, SlLfoVol) == 0.f, "the speed LFO: an LFO on the pitch");
  expect(std::lround(pv(s, SlModXDest)) == SlDestCutoff && pv(s, SlModXAmt) > 0.5f && std::lround(pv(s, SlModYDest)) == SlDestOff, "mod X: the cutoff map; mod Y: nothing mapped");
  expect(near(pv(s, slParam(1, SliceStart)), 0.5 * 4410.0 / 22050.0, 1e-3), "region start offset: a share of the region");
  // Switched to the classic engine, the import plays (and region 2 pans right).
  PluginState c = s;
  c.params[static_cast<size_t>(SlEngine)] = 0.f;
  ChannelAux aux;
  prepareChannelAux(c, aux);
  const Buf b = hold(c, aux, 61, 30000);
  expect(rms(b.l, 0, 30000) > 1e-3 && rms(b.r, 8000, 13000) > 2 * rms(b.l, 8000, 13000), "classic: region 2 plays and pans right");
}

}  // namespace

int main() {
  testStage1();
  testStage2();
  testStage3();
  testStage4();
  testStage5();
  testStage6();
  return finish();
}
