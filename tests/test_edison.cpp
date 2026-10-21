// Gridline — J-7 audio editor (gl.edison) tests: edit operations on synthetic
// signals (sample-exact), undo / redo, resampling, stretch / pitch accuracy,
// spectral tools, analysis, slice detection, file round trips, the FL state
// decoder, the slot runtime and recording with pre-roll.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include "Check.h"
#include "core/PluginInfo.h"
#include "core/flp/Flp.h"
#include "engine/Dsp.h"
#include "engine/Effects.h"
#include "packs/edison/edison.h"

using namespace gl;
using namespace gl::packs::edison;
using gl::dsp::dbToGain;
namespace fs = std::filesystem;

namespace {

constexpr double kTau = 6.283185307179586;

Audio ramp(int64_t n, int channels = 2) {
  Audio a = makeAudio(1000, channels, n);
  for (int64_t i = 0; i < n; ++i) {
    a.L[static_cast<size_t>(i)] = static_cast<float>(i) / 1024.f;
    a.R[static_cast<size_t>(i)] = channels == 1 ? a.L[static_cast<size_t>(i)] : -static_cast<float>(i) / 2048.f;
  }
  return a;
}

Audio sine(double hz, double seconds, double rate = 44100, double amp = 0.5, int channels = 2, double phase = 0) {
  Audio a = makeAudio(rate, channels, static_cast<int64_t>(seconds * rate));
  for (size_t i = 0; i < a.L.size(); ++i) a.L[i] = a.R[i] = static_cast<float>(amp * std::sin(kTau * hz * static_cast<double>(i) / rate + phase));
  return a;
}

Audio saw(double hz, double seconds, double rate = 44100) {
  Audio a = makeAudio(rate, 2, static_cast<int64_t>(seconds * rate));
  for (size_t i = 0; i < a.L.size(); ++i) {
    double ph = std::fmod(hz * static_cast<double>(i) / rate, 1.0);
    a.L[i] = a.R[i] = static_cast<float>(0.5 * (2 * ph - 1));
  }
  return a;
}

double rmsOf(const std::vector<float>& x, int64_t a, int64_t b) {
  double s = 0;
  for (int64_t i = a; i < b; ++i) s += static_cast<double>(x[static_cast<size_t>(i)]) * x[static_cast<size_t>(i)];
  return b > a ? std::sqrt(s / static_cast<double>(b - a)) : 0.0;
}

// Amplitude of frequency hz in x[a, b) (single-bin DFT).
double toneAmp(const std::vector<float>& x, int64_t a, int64_t b, double hz, double rate) {
  double re = 0, im = 0;
  for (int64_t i = a; i < b; ++i) {
    double w = kTau * hz * static_cast<double>(i) / rate;
    re += x[static_cast<size_t>(i)] * std::cos(w);
    im += x[static_cast<size_t>(i)] * std::sin(w);
  }
  return 2.0 * std::sqrt(re * re + im * im) / static_cast<double>(b - a);
}

// Dominant frequency by zero-crossing count over the middle of the signal.
double zcFreq(const std::vector<float>& x, double rate) {
  const size_t a = x.size() / 4, b = x.size() * 3 / 4;
  size_t first = 0, last = 0;
  int n = 0;
  for (size_t i = a + 1; i < b; ++i)
    if (x[i - 1] < 0 && x[i] >= 0) {
      if (n == 0) first = i;
      last = i;
      ++n;
    }
  return n > 1 ? (n - 1) * rate / static_cast<double>(last - first) : 0.0;
}

bool same(const std::vector<float>& a, const std::vector<float>& b, float tol = 0.f) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (std::fabs(a[i] - b[i]) > tol) return false;
  return true;
}

std::string tmpDir() {
  fs::path d = fs::temp_directory_path() / "gridline-edison-tests";
  std::error_code ec;
  fs::create_directories(d, ec);
  return d.string();
}

}  // namespace

int main() {
  // ------------------------------------------------------------------ catalogue / runtime
  {
    const PluginInfo* info = findPlugin(kType);
    expect(info && info->kind == PluginKind::Effect && info->params.size() == static_cast<size_t>(EdCount), "gl.edison is an effect with its parameters");
    expect(info && info->presets.size() >= 2 && info->presets.size() <= 6, "2-6 factory presets");
    expect(info && info->editorWidth >= 900, "wide editor");
    auto fx = createEffect(kType);
    expect(fx && dynamic_cast<EdisonFx*>(fx.get()), "runtime is EdisonFx");
  }
  {  // pass-through, silence, mute input, capture ring
    EdisonFx fx;
    PluginState st = defaultPluginState(kType);
    ProcessContext ctx;
    std::vector<float> L(256), R(256), inL(256), inR(256);
    for (int i = 0; i < 256; ++i) inL[static_cast<size_t>(i)] = std::sin(i * 0.1f) * 0.5f, inR[static_cast<size_t>(i)] = std::cos(i * 0.07f) * 0.3f;
    std::vector<float> z(256, 0.f);
    L = z, R = z;
    fx.process(st, ctx, L.data(), R.data(), 256);
    expect(same(L, z) && same(R, z), "silence in -> silence out");
    L = inL, R = inR;
    fx.process(st, ctx, L.data(), R.data(), 256);
    expect(same(L, inL) && same(R, inR), "input passes through unchanged");
    size_t from = 256;
    std::vector<float> cL(256), cR(256);
    std::vector<uint8_t> pl(256);
    size_t got = fx.capture().read(from, 256, cL.data(), cR.data(), pl.data());
    expect(got == 256 && same(cL, inL) && same(cR, inR), "capture ring holds the input sample-exact");
    st.params[EdMuteInput] = 1;
    L = inL, R = inR;
    fx.process(st, ctx, L.data(), R.data(), 256);
    expect(same(L, z) && same(R, z), "mute input silences the slot");
  }
  {  // playback of the prepared sample, loop
    EdisonFx fx;
    PluginState st = defaultPluginState(kType);
    Audio a = ramp(300);
    a.rate = 44100;
    ChannelAux aux;
    aux.samples.push_back(toSample(a, "mem"));
    ProcessContext ctx;
    std::vector<float> L(128, 0.f), R(128, 0.f);
    fx.play(10, 0, false);
    fx.processAux(st, aux, ctx, L.data(), R.data(), 128);
    bool ok = true;
    for (int i = 0; i < 128; ++i) ok = ok && L[static_cast<size_t>(i)] == a.L[static_cast<size_t>(10 + i)] && R[static_cast<size_t>(i)] == a.R[static_cast<size_t>(10 + i)];
    expect(ok && fx.isPlaying(), "playback reproduces the audio sample-exact");
    fx.stop();
    fx.play(100, 110, true);
    std::fill(L.begin(), L.end(), 0.f);
    std::fill(R.begin(), R.end(), 0.f);
    fx.processAux(st, aux, ctx, L.data(), R.data(), 25);
    expect(L[0] == a.L[100] && L[10] == a.L[100] && L[24] == a.L[114 - 10], "loop playback wraps to the loop start");
  }

  {  // short / old parameter vectors, parameter extremes stay finite
    PluginState st;
    st.type = kType;
    normalizePluginState(st);
    const PluginInfo* info = findPlugin(kType);
    expect(st.params.size() == static_cast<size_t>(EdCount) && info && st.params[EdRecMode] == static_cast<float>(info->params[EdRecMode].def),
           "short parameter vector padded with defaults");
    EdisonFx fx;
    ChannelAux aux;
    Audio a = sine(220, 0.2, 44100, 0.9);
    aux.samples.push_back(toSample(a, "x"));
    ProcessContext ctx;
    bool finite = true;
    for (int pass = 0; pass < 2; ++pass) {
      PluginState ps = defaultPluginState(kType);
      for (int k = 0; k < EdCount; ++k) ps.params[static_cast<size_t>(k)] = static_cast<float>(pass ? info->params[static_cast<size_t>(k)].max : info->params[static_cast<size_t>(k)].min);
      fx.play(0, 0, true);
      std::vector<float> L(512, 0.5f), R(512, -0.5f);
      for (int b = 0; b < 40; ++b) {
        ctx.playing = (b / 10) % 2 == 1;
        fx.processAux(ps, aux, ctx, L.data(), R.data(), 512);
        for (int i = 0; i < 512; ++i) finite = finite && std::isfinite(L[static_cast<size_t>(i)]) && std::isfinite(R[static_cast<size_t>(i)]);
      }
      fx.scrub(true, 3000);
      fx.processAux(ps, aux, ctx, L.data(), R.data(), 512);
      fx.scrub(false, 3000);
    }
    expect(finite, "parameter extremes (loop, follow transport, gain, scrub) give finite output");
    // Block-size invariance of playback.
    std::vector<float> out[2];
    for (int v = 0; v < 2; ++v) {
      EdisonFx f2;
      PluginState ps = defaultPluginState(kType);
      ps.params[EdLoop] = 1;
      f2.play(100, 5000, true);
      const int bs = v ? 512 : 61;
      std::vector<float> L(4096, 0.f), R(4096, 0.f);
      for (int s0 = 0; s0 < 4096; s0 += bs) f2.processAux(ps, aux, ctx, L.data() + s0, R.data() + s0, std::min(bs, 4096 - s0));
      out[v] = L;
    }
    expect(out[0] == out[1], "playback is block-size invariant");
  }

  // ------------------------------------------------------------------ edit operations (sample-exact)
  {
    Content c = makeContent(ramp(100));
    Content d = deleteRange(c, {10, 20});
    expect(d.frames() == 90 && d.a().L[9] == c.a().L[9] && d.a().L[10] == c.a().L[20] && d.a().R[89] == c.a().R[99], "delete removes exactly the range");
    Audio clip = extract(c.a(), {10, 20});
    expect(clip.frames() == 10 && clip.L[0] == c.a().L[10] && clip.R[9] == c.a().R[19], "copy extracts the range");
    Content p = insertAudio(d, 10, clip);
    expect(same(p.a().L, c.a().L) && same(p.a().R, c.a().R), "cut + paste restores the original");
    Content s = insertSilence(c, 50, 7);
    expect(s.frames() == 107 && s.a().L[50] == 0 && s.a().L[56] == 0 && s.a().L[57] == c.a().L[50], "insert silence");
    Content cr = cropTo(c, {30, 40});
    expect(cr.frames() == 10 && cr.a().L[0] == c.a().L[30] && cr.a().L[9] == c.a().L[39], "trim (crop) keeps the selection");
    Content rp = replaceRange(c, {0, 50}, extract(c.a(), {90, 95}));
    expect(rp.frames() == 55 && rp.a().L[0] == c.a().L[90] && rp.a().L[5] == c.a().L[50], "paste replaces the selection");
    Content sil = silenceRange(c, {5, 8});
    expect(sil.a().L[5] == 0 && sil.a().L[7] == 0 && sil.a().L[8] == c.a().L[8] && sil.frames() == 100, "silence range");
  }
  {  // fades, gain, normalize, reverse, invert, DC, channel ops
    Audio one = makeAudio(1000, 2, 11);
    for (int i = 0; i < 11; ++i) one.L[static_cast<size_t>(i)] = one.R[static_cast<size_t>(i)] = 1.f;
    Content c = makeContent(one);
    Content fi = fadeIn(c, {0, 11});
    bool lin = true;
    for (int i = 0; i < 11; ++i) lin = lin && std::fabs(fi.a().L[static_cast<size_t>(i)] - i / 10.f) < 1e-6f;
    expect(lin, "linear fade in: 0 .. 1 in equal steps");
    Content fo = fadeOut(c, {0, 11}, Curve::Linear);
    expect(fo.a().L[0] == 1.f && fo.a().L[10] == 0.f && std::fabs(fo.a().L[5] - 0.5f) < 1e-6f, "linear fade out");
    bool curves = true;
    for (int k = 0; k < static_cast<int>(Curve::Count); ++k) {
      Curve cv = static_cast<Curve>(k);
      curves = curves && curveGain(cv, 0) == 0 && std::fabs(curveGain(cv, 1) - 1) < 1e-12 && curveGain(cv, 0.5) > 0 && curveGain(cv, 0.5) < 1;
    }
    expect(curves, "fade curves run 0 -> 1");
    Content fs2 = fadeIn(c, {0, 11}, Curve::Slow);
    expect(std::fabs(fs2.a().L[5] - 0.25f) < 1e-6f, "slow-start curve is quadratic");
    Content r = makeContent(ramp(64));
    Content g = gain(r, {0, 64}, 20 * std::log10(2.0));
    bool dbl = true;
    for (int i = 0; i < 64; ++i) dbl = dbl && std::fabs(g.a().L[static_cast<size_t>(i)] - 2 * r.a().L[static_cast<size_t>(i)]) < 1e-6f;
    expect(dbl, "gain +6.02 dB doubles every sample");
    Content nm = normalize(r, {0, 64}, -6.0);
    float pk = 0;
    for (float v : nm.a().L) pk = std::max(pk, std::fabs(v));
    for (float v : nm.a().R) pk = std::max(pk, std::fabs(v));
    expect(std::fabs(pk - static_cast<float>(dbToGain(-6.0))) < 1e-6f, "normalize sets the peak to the target");
    Content rv = reverse(r, {0, 64});
    expect(rv.a().L[0] == r.a().L[63] && rv.a().R[63] == r.a().R[0], "reverse mirrors the range");
    Content rv2 = reverse(r, {10, 20});
    expect(rv2.a().L[10] == r.a().L[19] && rv2.a().L[9] == r.a().L[9] && rv2.a().L[20] == r.a().L[20], "reverse touches only the selection");
    Content inv = invert(r, {0, 64});
    expect(inv.a().L[5] == -r.a().L[5] && inv.a().R[7] == -r.a().R[7], "invert negates");
    Audio dc = sine(100, 0.1, 1000, 0.3);
    for (auto& v : dc.L) v += 0.25f;
    for (auto& v : dc.R) v -= 0.1f;
    Content dcc = removeDc(makeContent(dc), {0, dc.frames()});
    double mL = 0, mR = 0;
    for (size_t i = 0; i < dcc.a().L.size(); ++i) mL += dcc.a().L[i], mR += dcc.a().R[i];
    expect(std::fabs(mL / 100) < 1e-6 && std::fabs(mR / 100) < 1e-6, "DC removal zeroes the mean");
    Content sw = swapChannels(r);
    expect(sw.a().L == r.a().R && sw.a().R == r.a().L, "swap channels");
    Content mo = toMono(r);
    expect(mo.a().channels == 1 && mo.a().L == mo.a().R && mo.a().L[9] == 0.5f * (r.a().L[9] + r.a().R[9]), "mix to mono");
    Content st = toStereo(mo);
    expect(st.a().channels == 2 && st.a().L == mo.a().L && st.a().R == mo.a().L, "mono to stereo");
    Content env = applyEnvelope(makeContent(one), {0, 11}, {{0, 0}, {0.5, 2}, {1, 1}});
    expect(env.a().L[0] == 0.f && std::fabs(env.a().L[5] - 2.f) < 1e-6f && std::fabs(env.a().L[10] - 1.f) < 1e-6f, "drawn amplitude envelope");
    Audio pad = makeAudio(1000, 2, 30);
    for (int i = 10; i < 20; ++i) pad.L[static_cast<size_t>(i)] = 0.5f;
    Range kept;
    Content tr = trimSilence(makeContent(pad), -40, &kept);
    expect(tr.frames() == 10 && kept.a == 10 && kept.b == 20, "trim side noise");
  }
  {  // markers / regions follow edits
    Content c = makeContent(ramp(100));
    c = addMarker(c, 50, "A");
    c = addMarker(c, 10, "B");
    c = addRegion(c, {60, 80}, "R");
    c = setLoop(c, {40, 90});
    expect(c.markers.size() == 2 && c.markers[0].name == "B" && c.markers[1].pos == 50, "markers sorted by position");
    Content d = deleteRange(c, {5, 15});
    expect(d.markers.size() == 1 && d.markers[0].pos == 40 && d.regions[0].start == 50 && d.regions[0].end == 70 && d.loopStart == 30 && d.loopEnd == 80,
           "delete shifts markers, regions and loop; drops markers inside");
    Content i = insertSilence(c, 0, 5);
    expect(i.markers[0].pos == 15 && i.regions[0].start == 65 && i.loopEnd == 95, "insert shifts markers");
    Content rn = renameMarker(c, 0, "Kick");
    expect(rn.markers[0].name == "Kick", "rename marker");
    Content rm = removeRegion(c, 0);
    expect(rm.regions.empty(), "remove region");
    Audio z = makeAudio(1000, 2, 100);
    for (int k = 0; k < 100; ++k) z.L[static_cast<size_t>(k)] = z.R[static_cast<size_t>(k)] = (k < 33 ? 0.4f : -0.4f);
    expect(nearestZeroCrossing(z, 30, 10) == 33, "nearest zero crossing");
    Content snapped = snapMarkersToZero(addMarker(makeContent(z), 37, "m"), 10);
    expect(snapped.markers[0].pos == 33, "snap marker to zero crossing");
  }
  {  // undo / redo
    History h(makeContent(ramp(50)));
    const Content orig = h.cur();
    h.push("Reverse", reverse(h.cur(), {0, 50}));
    h.push("Delete", deleteRange(h.cur(), {0, 10}));
    expect(h.cur().frames() == 40 && h.canUndo() && h.undoLabel() == "Delete", "history records steps");
    h.undo();
    expect(h.cur().frames() == 50 && h.cur().a().L[0] == orig.a().L[49] && h.canRedo(), "undo restores the previous version");
    h.undo();
    expect(same(h.cur().a().L, orig.a().L) && same(h.cur().a().R, orig.a().R) && !h.canUndo(), "undo to the original exactly");
    h.redo();
    h.redo();
    expect(h.cur().frames() == 40 && !h.canRedo(), "redo replays both steps");
    h.undo();
    h.push("Gain", gain(h.cur(), {0, 50}, -6));
    expect(!h.canRedo() && h.labels().size() == 2, "a new edit clears the redo list");
  }

  // ------------------------------------------------------------------ resampling, stretch, pitch
  {
    Audio a = sine(1000, 0.5, 44100, 0.5);
    Audio b = resampleAudio(a, 48000, 1);
    expect(b.frames() == static_cast<int64_t>(std::llround(a.frames() * 48000.0 / 44100.0)) && b.rate == 48000, "resampled length");
    double err = 0;
    for (int64_t i = 500; i < b.frames() - 500; ++i)
      err = std::max(err, std::fabs(b.L[static_cast<size_t>(i)] - 0.5 * std::sin(kTau * 1000 * static_cast<double>(i) / 48000)));
    expect(err < 1e-3, "44.1 -> 48 kHz sinc conversion error < -60 dB");
    Audio dn = resampleAudio(sine(15000, 0.3, 48000, 0.5), 22050, 1);
    expect(rmsOf(dn.L, 200, dn.frames() - 200) < 0.01, "downsampling removes content above the new Nyquist");
    Content rc = resample(addMarker(makeContent(a), 4410, "m"), 22050, 0);
    expect(rc.markers[0].pos == 2205 && rc.a().rate == 22050, "markers move with the rate");
  }
  {
    Audio a = sine(440, 1.0, 44100, 0.5);
    for (Stretch m : {Stretch::Elastic, Stretch::Transient}) {
      Audio s = stretchAudio(a, 1.5, 0, m, 1);
      double f = zcFreq(s.L, 44100);
      char name[96];
      std::snprintf(name, sizeof name, "%s stretch x1.5: exact length, pitch kept (%.1f Hz)", stretchName(m), f);
      expect(s.frames() == 66150 && std::fabs(f - 440) < 4.4, name);
    }
    Audio p = stretchAudio(a, 1.0, 12, Stretch::Elastic, 1);
    double fp = zcFreq(p.L, 44100);
    expect(p.frames() == a.frames() && std::fabs(fp - 880) < 8.8, "pitch shift +12: length kept, frequency doubled");
    Audio q = stretchAudio(a, 1.0, -5, Stretch::Elastic, 2);
    double target = 440 * std::pow(2.0, -5 / 12.0);
    expect(q.frames() == a.frames() && std::fabs(zcFreq(q.L, 44100) - target) < target * 0.01, "pitch shift -5 within 1 %");
    Audio t = stretchAudio(a, 1.0, 12, Stretch::Tape, 1);
    expect(std::llabs(t.frames() - a.frames() / 2) <= 1 && std::fabs(zcFreq(t.L, 44100) - 880) < 8.8, "tape mode: an octave up plays twice as fast");
    Audio half = stretchAudio(a, 0.5, 0, Stretch::Elastic, 1);
    expect(half.frames() == 22050 && std::fabs(rmsOf(half.L, 2000, 20000) - 0.5 / std::sqrt(2.0)) < 0.05, "compression keeps the level");
    Content c = addMarker(makeContent(a), 22050, "mid");
    Content cs = stretch(c, {0, a.frames()}, 2.0, 0, Stretch::Elastic, 0);
    expect(cs.frames() == 88200 && cs.markers.size() == 1 && cs.markers[0].pos == 44100, "stretch scales markers inside the range");
  }

  // ------------------------------------------------------------------ spectral tools
  {
    Audio a = sine(1000, 0.5, 44100, 0.25);
    Content c = makeContent(a);
    Content e = equalize(c, {0, a.frames()}, {{20, 20 * std::log10(2.0)}, {20000, 20 * std::log10(2.0)}});
    double err = 0;
    for (int64_t i = 0; i < a.frames(); ++i) err = std::max(err, static_cast<double>(std::fabs(e.a().L[static_cast<size_t>(i)] - 2 * a.L[static_cast<size_t>(i)])));
    expect(err < 1e-4, "flat +6 dB drawn EQ doubles the signal sample-aligned");
    Audio mix = sine(100, 0.5, 44100, 0.25);
    Audio hi = sine(5000, 0.5, 44100, 0.25);
    for (size_t i = 0; i < mix.L.size(); ++i) mix.L[i] += hi.L[i], mix.R[i] += hi.R[i];
    Content lc = equalize(makeContent(mix), {0, mix.frames()}, {{50, -40}, {500, -40}, {1000, 0}, {20000, 0}});
    const int64_t n = mix.frames();
    expect(toneAmp(lc.a().L, 4000, n - 4000, 100, 44100) < 0.25 * 0.05 && std::fabs(toneAmp(lc.a().L, 4000, n - 4000, 5000, 44100) - 0.25) < 0.01,
           "low-cut curve: 100 Hz down > 26 dB, 5 kHz untouched");
    expect(std::fabs(eqCurveDb({{100, 0}, {1000, 10}}, std::sqrt(100.0 * 1000)) - 5) < 1e-9, "EQ curve interpolates on a log axis");
  }
  {  // denoise with a profile
    Audio a = makeAudio(44100, 2, 88200);
    uint32_t seed = 7;
    for (size_t i = 0; i < a.L.size(); ++i) {
      seed = seed * 1664525u + 1013904223u;
      float nz = (static_cast<float>(seed >> 8) / 16777216.f * 2.f - 1.f) * 0.02f;
      float tone = i >= 44100 ? 0.4f * static_cast<float>(std::sin(kTau * 440 * static_cast<double>(i) / 44100)) : 0.f;
      a.L[i] = a.R[i] = tone + nz;
    }
    Content c = makeContent(a);
    NoiseProfile p = acquireNoise(a, {0, 40000});
    expect(p.valid() && p.mag.size() == 1025, "noise profile acquired");
    Content d = denoise(c, {0, a.frames()}, p, 30, 0.5);
    double before = rmsOf(a.L, 5000, 35000), after = rmsOf(d.a().L, 5000, 35000);
    double tb = toneAmp(a.L, 50000, 85000, 440, 44100), ta = toneAmp(d.a().L, 50000, 85000, 440, 44100);
    expect(20 * std::log10(after / before) < -12, "denoise: noise-only part down > 12 dB");
    expect(std::fabs(20 * std::log10(ta / tb)) < 1.0, "denoise: tone level kept within 1 dB");
  }
  {  // convolution / reverb / blur
    Audio x = sine(300, 0.1, 44100, 0.3);
    Audio id = makeAudio(44100, 2, 1);
    id.L[0] = id.R[0] = 1;
    Audio y = convolveAudio(x, id);
    expect(y.frames() == x.frames() && same(y.L, x.L, 1e-6f), "convolution with a unit impulse is identity");
    Audio dl = makeAudio(44100, 2, 11);
    dl.L[10] = dl.R[10] = 1;
    Audio y2 = convolveAudio(x, dl);
    bool sh = y2.frames() == x.frames() + 10;
    for (int64_t i = 0; i < x.frames() && sh; ++i) sh = std::fabs(y2.L[static_cast<size_t>(i + 10)] - x.L[static_cast<size_t>(i)]) < 1e-6f;
    expect(sh, "convolution with a delayed impulse delays exactly");
    Audio longIr = makeAudio(44100, 2, 5000);
    longIr.L[4000] = longIr.R[4000] = 0.5f;
    Audio y3 = convolveAudio(x, longIr);
    expect(std::fabs(y3.L[4000 + 100] - 0.5f * x.L[100]) < 1e-5f, "FFT convolution (long kernel) is exact");
    ReverbSettings rs;
    rs.decay = 0.5;
    Content c = makeContent(x);
    Content rv = reverb(c, {0, x.frames()}, rs);
    expect(rv.frames() > x.frames() + 20000 && rmsOf(rv.a().L, x.frames() + 1000, x.frames() + 5000) > 1e-4, "reverb renders its tail past the end");
    Audio ir = makeReverbIr(44100, rs);
    expect(ir.frames() > 22050 && rmsOf(ir.L, 0, 2000) > rmsOf(ir.L, ir.frames() - 2000, ir.frames()) * 100, "reverb impulse decays");
    Audio imp = makeAudio(44100, 2, 4000);
    imp.L[100] = imp.R[100] = 1;
    Content bl = blur(makeContent(imp), {0, 4000}, 20, 1.0);
    double e = 0;
    float pk = 0;
    for (int64_t i = 0; i < bl.frames(); ++i) e += bl.a().L[static_cast<size_t>(i)] * bl.a().L[static_cast<size_t>(i)], pk = std::max(pk, std::fabs(bl.a().L[static_cast<size_t>(i)]));
    expect(pk < 0.3f && std::fabs(e - 1.0) < 0.05, "blur spreads an impulse, energy kept");
  }
  {  // gate / declick / spectral edit
    Audio a = makeAudio(44100, 2, 44100);
    for (size_t i = 0; i < a.L.size(); ++i) {
      bool loud = i >= 11025 && i < 33075;
      float v = loud ? 0.5f * static_cast<float>(std::sin(kTau * 200 * static_cast<double>(i) / 44100)) : 0.001f * ((i & 1) ? 1.f : -1.f);
      a.L[i] = a.R[i] = v;
    }
    Content g = gate(makeContent(a), {0, a.frames()}, -40, 2, 20, 30);
    expect(rmsOf(g.a().L, 0, 10000) < 1e-6 && rmsOf(g.a().L, 40000, 44100) < 1e-6, "gate silences the quiet parts");
    expect(std::fabs(rmsOf(g.a().L, 12000, 32000) - rmsOf(a.L, 12000, 32000)) < 1e-6, "gate leaves the loud part");
    Audio s = sine(300, 0.2, 44100, 0.4);
    Audio clean = s;
    for (int k : {1000, 3000, 5000}) s.L[static_cast<size_t>(k)] += 0.6f, s.R[static_cast<size_t>(k)] -= 0.5f;
    int fixed = 0;
    Content dc = declick(makeContent(s), {0, s.frames()}, 0.5, &fixed);
    double worst = 0;
    for (int k : {1000, 3000, 5000})
      for (int j = -2; j <= 2; ++j) worst = std::max(worst, static_cast<double>(std::fabs(dc.a().L[static_cast<size_t>(k + j)] - clean.L[static_cast<size_t>(k + j)])));
    std::printf("# declick fixed=%d worst=%g\n", fixed, worst);
    expect(fixed >= 3 && worst < 0.01, "declick repairs injected clicks");
    expect(std::fabs(rmsOf(dc.a().L, 6000, 8000) - rmsOf(clean.L, 6000, 8000)) < 1e-6, "declick leaves clean audio alone");
    Audio m = sine(1000, 1.0, 44100, 0.3);
    Audio h = sine(5000, 1.0, 44100, 0.3);
    for (size_t i = 0; i < m.L.size(); ++i) m.L[i] += h.L[i], m.R[i] += h.R[i];
    Content sp = spectralGain(makeContent(m), {{10000, 30000, 800, 1200}}, -40);
    expect(toneAmp(sp.a().L, 14000, 26000, 1000, 44100) < 0.3 * 0.05, "spectral brush attenuates the painted band");
    expect(std::fabs(toneAmp(sp.a().L, 14000, 26000, 5000, 44100) - 0.3) < 0.01, "spectral brush leaves other frequencies");
    expect(same(std::vector<float>(sp.a().L.begin(), sp.a().L.begin() + 5000), std::vector<float>(m.L.begin(), m.L.begin() + 5000)),
           "spectral edit leaves audio outside the brush untouched");
  }

  // ------------------------------------------------------------------ analysis
  {
    Audio a = sine(1000, 3.0, 48000, 0.1);
    Levels L = measure(a, {0, a.frames()});
    expect(std::fabs(L.lufs - (-20.0)) < 0.1, "BS.1770: -20 dBFS stereo 1 kHz sine reads -20 LUFS (48 kHz)");
    expect(std::fabs(L.peakDb - (-20.0)) < 0.01 && std::fabs(L.rmsDb - (-23.01)) < 0.02, "peak and RMS readouts");
    Audio b = sine(1000, 3.0, 44100, 0.1);
    expect(std::fabs(integratedLufs(b, {0, b.frames()}) - (-20.0)) < 0.1, "BS.1770 at 44.1 kHz");
    Audio m = makeAudio(48000, 2, 144000);
    for (size_t i = 0; i < m.L.size(); ++i) m.L[i] = static_cast<float>(std::sin(kTau * 997 * static_cast<double>(i) / 48000));
    m.channels = 2;
    expect(std::fabs(integratedLufs(m, {0, m.frames()}) - (-3.01)) < 0.1, "BS.1770 reference: 0 dBFS 997 Hz in one channel = -3.01 LUFS");
    Audio tp = sine(11025, 0.5, 44100, 1.0, 2, kTau / 8);
    Levels T = measure(tp, {0, tp.frames()});
    expect(T.peakDb < -2.5 && std::fabs(T.truePeakDb) < 0.3, "true peak finds inter-sample peaks");
    PitchInfo p = detectPitch(saw(220, 0.5), {0, 22050});
    expect(std::fabs(p.hz - 220) < 1.1 && p.note == 57 && noteName(p.note) == "A3", "pitch detection on a saw");
    PitchInfo ps = detectPitch(sine(1318.51, 0.3), {0, 13230});
    expect(ps.note == 88, "pitch detection on a high sine");
    PitchInfo found;
    Content tuned = tuneToNote(makeContent(sine(452, 1.0, 44100, 0.4)), {0, 0}, -1, &found);
    PitchInfo after = detectPitch(tuned.a(), {0, tuned.frames()});
    expect(found.note == 69 && std::fabs(after.hz - 440) < 2.0 && tuned.frames() == 44100, "tune to note: 452 Hz -> A4");
  }
  {  // onsets, slices, tempo
    const double rate = 44100;
    Audio a = makeAudio(rate, 2, static_cast<int64_t>(rate * 4));
    std::vector<int64_t> truth;
    uint32_t seed = 3;
    for (int k = 0; k < 8; ++k) {
      int64_t at = static_cast<int64_t>(k * 0.5 * rate) + 1000 + k * 37;
      truth.push_back(at);
      for (int64_t i = 0; i < 6000; ++i) {
        seed = seed * 1664525u + 1013904223u;
        float nz = static_cast<float>(seed >> 8) / 16777216.f * 2.f - 1.f;
        float v = 0.8f * nz * static_cast<float>(std::exp(-static_cast<double>(i) / 900.0));
        a.L[static_cast<size_t>(at + i)] += v;
        a.R[static_cast<size_t>(at + i)] += v;
      }
    }
    std::vector<int64_t> on = detectOnsets(a, {0, a.frames()}, 0.5);
    bool ok = on.size() == truth.size();
    int64_t worst = 0;
    for (size_t k = 0; ok && k < on.size(); ++k) worst = std::max<int64_t>(worst, std::llabs(on[k] - truth[k]));
    char name[160];
    std::snprintf(name, sizeof name, "beat detection finds every hit (%zu/%zu, worst %lld samples)", on.size(), truth.size(), static_cast<long long>(worst));
    expect(ok && worst <= 64, name);
    double bpm = estimateTempo(on, rate);
    expect(std::fabs(bpm - 120) < 2, "tempo estimate 120 BPM");
    Content sl = setSliceMarkers(makeContent(a), on);
    expect(sl.markers.size() == truth.size() && sl.markers[0].name == "Slice 1" && sl.markers[7].name == "Slice 8", "slices become markers");
  }
  {  // peak cache + spectrogram
    Audio a = saw(37, 3.0);
    for (size_t i = 0; i < a.R.size(); ++i) a.R[i] = 0.3f * static_cast<float>(std::sin(static_cast<double>(i) * 0.001));
    PeakCache pc;
    pc.build(a);
    bool ok = pc.built();
    for (int t = 0; t < 50 && ok; ++t) {
      int64_t f0 = static_cast<int64_t>((static_cast<uint64_t>(t) * 2654435761u) % static_cast<uint64_t>(a.frames()));
      int64_t f1 = std::min<int64_t>(a.frames(), f0 + 1 + (t % 3 == 0 ? t : (t * 40503) % 30000));
      for (int c = 0; c < 2; ++c) {
        float mn, mx, bm = a.ch(c)[f0], bx = a.ch(c)[f0];
        pc.query(a, c, f0, f1, mn, mx);
        for (int64_t i = f0; i < f1; ++i) bm = std::min(bm, a.ch(c)[i]), bx = std::max(bx, a.ch(c)[i]);
        // Coarse levels may include neighbouring frames (never less than the truth);
        // short spans are scanned exactly.
        ok = ok && mn <= bm && mx >= bx && (f1 - f0 >= 64 || (mn == bm && mx == bx));
      }
    }
    expect(ok, "peak pyramid matches brute-force min/max");
    Spectrogram sg = computeSpectrogram(sine(4000, 0.5), 1024, 200);
    int best = 0;
    for (int b = 0; b < sg.bins; ++b)
      if (sg.at(sg.cols / 2, b) > sg.at(sg.cols / 2, best)) best = b;
    expect(sg.cols <= 200 && sg.cols > 50 && std::abs(best - static_cast<int>(std::lround(4000.0 * 1024 / 44100))) <= 1, "spectrogram peak at the tone");
  }

  // ------------------------------------------------------------------ files
  {
    const std::string dir = tmpDir();
    Audio a = sine(523.25, 0.25, 44100, 0.7);
    for (size_t i = 0; i < a.R.size(); ++i) a.R[i] = -a.L[i] * 0.5f;
    Content c = addRegion(addMarker(addMarker(makeContent(a), 100, "Start"), 5000, "Hit"), {2000, 4000}, "Phrase");
    c = setLoop(c, {1000, 9000});
    for (int bits : {16, 24, 32}) {
      std::string p = dir + "/rt" + std::to_string(bits) + ".wav";
      Content back;
      bool ok = writeWavFile(p, c, bits) && loadContent(p, back);
      float tol = bits == 32 ? 0.f : 1.0f / static_cast<float>(1 << (bits - 1));
      SamplePtr s = loadSample(p);
      char name[96];
      std::snprintf(name, sizeof name, "WAV %d-bit round trip (audio, markers, region, loop)", bits);
      expect(ok && same(back.a().L, a.L, tol) && same(back.a().R, a.R, tol) && back.markers.size() == 2 && back.markers[1].name == "Hit" &&
                 back.markers[1].pos == 5000 && back.regions.size() == 1 && back.regions[0].end == 4000 && back.regions[0].name == "Phrase" &&
                 back.loopStart == 1000 && back.loopEnd == 9000 && s && same(s->left, a.L, tol),
             name);
    }
    // FLAC: quantized audio comes back bit-exact (decoded by the app's own loader).
    for (int bits : {16, 24}) {
      Audio q = a;
      const float full = static_cast<float>(1 << (bits - 1));
      for (auto* v : {&q.L, &q.R})
        for (auto& x : *v) x = std::nearbyint(x * full) / full;
      std::string p = dir + "/rt" + std::to_string(bits) + ".flac";
      bool ok = writeFlacFile(p, q, bits);
      SamplePtr s = loadSample(p);
      char name[64];
      std::snprintf(name, sizeof name, "FLAC %d-bit round trip is lossless", bits);
      expect(ok && s && s->frames() == static_cast<size_t>(q.frames()) && same(s->left, q.L) && same(s->right, q.R), name);
    }
    Audio mono = sine(200, 0.1, 22050, 0.5, 1);
    std::string mp = dir + "/mono.flac";
    SamplePtr ms = writeFlacFile(mp, mono, 16) ? loadSample(mp) : nullptr;
    expect(ms && ms->sampleRate == 22050 && ms->frames() == static_cast<size_t>(mono.frames()), "FLAC mono at 22.05 kHz");
    Audio silent = makeAudio(48000, 2, 10000);
    std::string sp = dir + "/silent.flac";
    SamplePtr ss = writeFlacFile(sp, silent, 24) ? loadSample(sp) : nullptr;
    expect(ss && ss->frames() == 10000 && std::filesystem::file_size(sp) < 2000, "FLAC constant subframes for silence");
    std::string ep = dir + "/sel.wav";
    Content sel;
    expect(exportFile(ep, c, {2000, 3000}, FileFormat::Wav, 32) && loadContent(ep, sel) && sel.frames() == 1000 && sel.a().L[0] == a.L[2000],
           "export selection");
    expect(audioHash(a) == audioHash(Audio(a)) && audioHash(a) != audioHash(gain(makeContent(a), {0, 10}, 1).a()), "content hash");
    std::string rp = renderPath("/tmp/x", "My take #1", 0x1234abcdULL);
    expect(rp.find("My_take__1-") != std::string::npos && rp.size() > 4 && rp.substr(rp.size() - 4) == ".wav", "render path is file-name safe");
  }
  {  // plugin state JSON
    State s;
    s.path = "/a/b c.wav";
    s.hash = "00ff";
    s.name = "Take";
    s.markers = {{10, "m1"}, {200, "m \"2\""}};
    s.regions = {{5, 50, "r"}};
    s.loopStart = 1;
    s.loopEnd = 99;
    State d;
    expect(decodeState(encodeState(s), d) && d.path == s.path && d.hash == "00ff" && d.markers.size() == 2 && d.markers[1].name == "m \"2\"" &&
               d.regions[0].end == 50 && d.loopEnd == 99,
           "state round trip");
    State p;
    expect(decodeState("/x/y.wav", p) && p.path == "/x/y.wav", "a bare sample path is accepted");
  }

  // ------------------------------------------------------------------ FL Studio state
  {
    // Synthetic version-6 state: header ints, RIFF with audio + a marker, editor bytes, settings.
    Audio a = sine(100, 0.05, 44100, 0.5);
    Content c = addMarker(makeContent(a), 300, "fl marker");
    const std::string dir = tmpDir();
    std::string wp = dir + "/fl.wav";
    writeWavFile(wp, c, 16);
    std::ifstream f(wp, std::ios::binary);
    std::vector<uint8_t> riff((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::vector<uint8_t> b;
    auto i32 = [&b](int32_t v) {
      for (int k = 0; k < 4; ++k) b.push_back(static_cast<uint8_t>(static_cast<uint32_t>(v) >> (8 * k)));
    };
    i32(6);
    i32(0);
    i32(1);
    i32(0);
    i32(static_cast<int32_t>(riff.size()));
    b.insert(b.end(), riff.begin(), riff.end());
    for (int k = 0; k < 20; ++k) b.push_back(0xAB);
    for (int32_t v : {3, 1, 5, 3, 1}) i32(v);
    FlEdison fl;
    expect(decodeFlState(b.data(), b.size(), fl) && fl.version == 6 && fl.head.size() == 3 && fl.hasAudio && fl.recMode == 3 && fl.followHost &&
               fl.extra.size() == 20 && fl.content.markers.size() == 1 && fl.content.frames() == a.frames(),
           "FL Edison state: header, embedded audio, editor bytes, settings");
    PluginState ps;
    std::string note;
    expect(mapFlState(b.data(), b.size(), dir + "/fl-audio", ps, &note) && ps.type == kType && ps.params[EdRecMode] == 3 && ps.params[EdRecMax] == 4 &&
               ps.params[EdFollowHost] == 1,
           "FL state maps to gl.edison parameters");
    State st;
    decodeState(ps.data, st);
    Content back;
    expect(!st.path.empty() && loadContent(st.path, back) && back.frames() == a.frames() && st.markers.size() == 1 && st.flVersion == 6 &&
               !st.flExtra.empty(),
           "FL embedded audio rendered to a file referenced by the state");
    // J-39: version word 3 | 13 << 16 — the audio size is a u64.
    {
      std::vector<uint8_t> b13;
      auto w32 = [&b13](uint32_t v) {
        for (int k = 0; k < 4; ++k) b13.push_back(static_cast<uint8_t>(v >> (8 * k)));
      };
      w32(0x000D0003u);
      w32(0), w32(0), w32(0);
      w32(static_cast<uint32_t>(riff.size())), w32(0);
      b13.insert(b13.end(), riff.begin(), riff.end());
      for (int k = 0; k < 12; ++k) w32(0);
      FlEdison f13;
      expect(decodeFlState(b13.data(), b13.size(), f13) && f13.version == 0x000D0003 && f13.head.size() == 3 && f13.hasAudio &&
                 f13.content.frames() == a.frames() && f13.settings.size() == 12,
             "FL Edison state version 13: u64 audio size, 12 settings");
    }
    std::vector<uint8_t> junk(7, 0xFF);
    PluginState pj;
    expect(!mapFlState(junk.data(), junk.size(), dir, pj, nullptr) && pj.type == kType, "garbage FL state keeps a valid placeholder");
    // FL Studio's own shipped Edison presets (when installed).
    const std::string presets = "/Applications/FL Studio 2026.app/Contents/Resources/FL/Data/Patches/Plugin presets/Effects/Edison/";
    std::error_code ec;
    if (fs::exists(presets, ec)) {
      int decoded = 0;
      bool onPlay = false, onInput = false;
      for (auto& e : fs::directory_iterator(presets, ec)) {
        if (e.path().extension() != ".fst") continue;
        std::ifstream pf(e.path(), std::ios::binary);
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(pf)), std::istreambuf_iterator<char>());
        // The plugin state is FL event 213 inside the preset; find its payload by the RIFF.
        for (size_t o = 0; o + 3 < bytes.size(); ++o) {
          if (bytes[o] != 213) continue;
          size_t len = 0, sh = 0, q = o + 1;
          while (q < bytes.size()) {
            len |= static_cast<size_t>(bytes[q] & 0x7F) << sh;
            sh += 7;
            if (!(bytes[q++] & 0x80)) break;
          }
          if (q + len > bytes.size() || len < 32) continue;
          FlEdison x;
          if (!decodeFlState(bytes.data() + q, len, x)) continue;
          ++decoded;
          std::string fn = e.path().filename().string();
          if (fn.find("playback") != std::string::npos) onPlay = onPlay || (x.recMode == 3 && x.followHost);
          if (fn.find("audio input") != std::string::npos) onInput = onInput || x.recMode == 2;
          break;
        }
      }
      expect(decoded == 5 && onPlay && onInput, "FL's shipped Edison presets decode (record mode, host sync)");
    }
    const flp::PluginDecoder* dec = flp::findDecoder("Edison");
    expect(dec != nullptr, "FLP import: Edison decoder registered");
  }

  // ------------------------------------------------------------------ recording
  {
    const double rate = 1000;
    Recorder r;
    Recorder::Config cfg;
    cfg.mode = RecOnInput;
    cfg.rate = rate;
    cfg.thresholdDb = -20;
    cfg.preRollSec = 0.2;  // 200 frames
    r.arm(cfg);
    std::vector<float> L(1500, 0.f), R(1500, 0.f);
    for (int i = 0; i < 1500; ++i) L[static_cast<size_t>(i)] = R[static_cast<size_t>(i)] = i < 1000 ? 0.001f * static_cast<float>(i % 7) : 0.5f;
    for (int s = 0; s < 1500; s += 64) r.feed(L.data() + s, R.data() + s, nullptr, static_cast<size_t>(std::min(64, 1500 - s)));
    expect(r.recording() && r.recordedFrames() == 700, "on-signal trigger keeps 200 pre-roll frames");
    r.stop();
    Audio t = r.take();
    bool pre = t.frames() == 700;
    for (int i = 0; i < 200 && pre; ++i) pre = t.L[static_cast<size_t>(i)] == L[static_cast<size_t>(800 + i)];
    expect(pre && t.L[200] == 0.5f && t.L[699] == 0.5f, "take = pre-roll + everything after the trigger, sample-exact");

    Recorder p;
    cfg.mode = RecOnPlay;
    cfg.preRollSec = 0.05;
    p.arm(cfg);
    std::vector<uint8_t> play(300, 0);
    for (int i = 100; i < 250; ++i) play[static_cast<size_t>(i)] = 1;
    std::vector<float> X(300);
    for (int i = 0; i < 300; ++i) X[static_cast<size_t>(i)] = static_cast<float>(i);
    p.feed(X.data(), X.data(), play.data(), 300);
    Audio tp = p.take();
    expect(!p.recording() && tp.frames() == 200 && tp.L[0] == 50.f && tp.L[50] == 100.f && tp.L[199] == 249.f,
           "on-playback: starts with the transport (with pre-roll), stops when it stops");

    Recorder m;
    cfg.mode = RecNow;
    cfg.preRollSec = 0;
    cfg.maxSec = 0.1;
    m.arm(cfg);
    m.feed(X.data(), X.data(), nullptr, 300);
    Audio tm = m.take();
    expect(tm.frames() == 100 && tm.L[0] == 0.f && tm.L[99] == 99.f && !m.armed(), "record now with a length limit");

    Recorder g;
    cfg.mode = RecInput;
    cfg.maxSec = 0;
    cfg.thresholdDb = -20;
    cfg.gateHoldSec = 0.01;
    g.arm(cfg);
    std::vector<float> Y(400, 0.f);
    for (int i = 50; i < 100; ++i) Y[static_cast<size_t>(i)] = 0.5f;
    for (int i = 300; i < 350; ++i) Y[static_cast<size_t>(i)] = 0.5f;
    g.feed(Y.data(), Y.data(), nullptr, 400);
    g.stop();
    Audio tg = g.take();
    expect(tg.frames() == 50 + 10 + 50 + 10, "while-signal mode pauses during silence (after its hold time)");

    // Slot capture -> recorder (what the editor does every frame).
    EdisonFx fx;
    PluginState st = defaultPluginState(kType);
    ProcessContext ctx;
    ctx.sampleRate = 1000;
    Recorder rr;
    Recorder::Config c2;
    c2.mode = RecOnInput;
    c2.rate = 1000;
    c2.thresholdDb = -20;
    c2.preRollSec = 0.1;
    rr.arm(c2);
    size_t readPos = 0;
    std::vector<float> bl(32), br(32), dl(4096), dr(4096);
    std::vector<uint8_t> dp(4096);
    for (int blk = 0; blk < 40; ++blk) {
      for (int i = 0; i < 32; ++i) bl[static_cast<size_t>(i)] = br[static_cast<size_t>(i)] = blk * 32 + i >= 640 ? 0.25f : 0.f;
      fx.process(st, ctx, bl.data(), br.data(), 32);
      size_t n = fx.capture().read(readPos, dl.size(), dl.data(), dr.data(), dp.data());
      rr.feed(dl.data(), dr.data(), dp.data(), n);
    }
    rr.stop();
    Audio tr = rr.take();
    expect(tr.frames() == 100 + 640 && tr.L[99] == 0.f && tr.L[100] == 0.25f, "slot capture feeds the recorder with pre-roll");
  }
  return finish();
}
