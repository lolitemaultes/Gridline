// Gridline — κ1: reference parity for the parametric EQ, reverb and delays.
// Curves and band designs against analytic targets; FL preset / render-test
// decoding when an FL Studio installation is present (skipped otherwise).
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>
#include "../src/core/DelayCurves.h"
#include "../src/core/PluginInfo.h"
#include "../src/core/Song.h"
#include "../src/core/PluginParams.h"
#include "../src/core/Project.h"
#include "../src/core/flp/Flp.h"
#include "../src/core/flp/FlpPresets.h"
#include "../src/engine/EqDesign.h"
#include "../src/engine/Effects.h"
#include "Check.h"

using namespace gl;
using namespace gl::pp;
namespace fs = std::filesystem;

namespace {

constexpr double kSr = 48000;
const char* kFlData = "/Applications/FL Studio 2026.app/Contents/Resources/FL/Data";

bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

PluginState eqWith(std::initializer_list<std::pair<int, float>> set) {
  PluginState s = defaultPluginState("gl.eq");
  for (auto [k, v] : set) s.params[static_cast<size_t>(k)] = v;
  return s;
}
double bandDb(const PluginState& s, int band, double hz, double sr = kSr) {
  EqBandFilters f;
  designEqBand(s, band, sr, f);
  return dsp::gainToDb(eqBandMagnitude(f, hz, sr));
}
int bandStages(const PluginState& s, int band) {
  EqBandFilters f;
  designEqBand(s, band, kSr, f);
  return f.count;
}

// Analog prototype magnitudes (dB) for comparison.
double analogPeakDb(double f, double f0, double bwOct, double gainDb) {
  double Q = 1.0 / (2.0 * std::sinh(0.5 * std::log(2.0) * bwOct));
  double A = std::pow(10.0, gainDb / 40.0);
  return 10.0 * std::log10(eqref::analogMag2(eqref::Peak, f / f0, Q, A));
}

std::string flPreset(const char* plugin, const char* name) {
  return std::string(kFlData) + "/Patches/Plugin presets/Effects/" + plugin + "/" + name + ".fst";
}

void testEq() {
  // ---- defaults follow the reference: log-spaced bands, shelves + peaks, widths
  PluginState d = defaultPluginState("gl.eq");
  expect(d.params[EqModel] == 1 && near(d.params[eqParam(0, EqFreq)], 20 * std::pow(1000.0, 1.0 / 6), 0.01) &&
             near(d.params[eqParam(3, EqFreq)], 20 * std::sqrt(1000.0), 0.01),
         "EQ defaults: reference band frequencies (1/6 + k/9 of the log range)");
  expect(near(d.params[eqFl(0, EqWidth)], 40124.0 / 65536, 1e-6) && near(d.params[eqFl(2, EqWidth)], 25412.0 / 65536, 1e-6),
         "EQ defaults: widths (shelves slope 1, peaks 2 octaves)");
  for (int b = 0; b < kEqBands; ++b) expect(bandStages(d, b) == 0, "EQ: flat peaks / shelves are bypassed");

  // ---- peak: gain at f0, half gain (dB) at the bandwidth edges (bilinear BW form)
  PluginState pk = eqWith({{eqParam(3, EqFreq), 1000}, {eqParam(3, EqGain), 12}});
  double bw = 0.1 + 4.9 * pk.params[eqFl(3, EqWidth)];
  expect(near(bw, 2.0, 0.01), "EQ peak: default width = 2 octaves");
  expect(near(bandDb(pk, 3, 1000), 12.0, 0.01), "EQ peak: +12 dB at its frequency");
  expect(near(bandDb(pk, 3, 1000 * std::pow(2.0, bw / 2)), 6.0, 0.35) && near(bandDb(pk, 3, 1000 * std::pow(2.0, -bw / 2)), 6.0, 0.35),
         "EQ peak: half gain at ±bandwidth/2");
  // Gentle orders split the gain over identical sections (same centre gain).
  pk.params[eqFl(3, EqFlOrder)] = EqGentle8;
  expect(bandStages(pk, 3) == 4 && near(bandDb(pk, 3, 1000), 12.0, 0.05), "EQ peak gentle 8: 4 sections, same centre gain");
  // Steep orders: narrower skirts.
  PluginState pn = eqWith({{eqParam(3, EqFreq), 1000}, {eqParam(3, EqGain), 12}});
  pk.params[eqFl(3, EqFlOrder)] = EqSteep8;
  expect(bandStages(pk, 3) == 4 && bandDb(pk, 3, 4000) < bandDb(pn, 3, 4000) - 1.0, "EQ peak steep 8: narrower skirt at 2 octaves");

  // ---- cuts: default width is Butterworth (−3.01 dB at fc, 12 dB/oct)
  PluginState lc = eqWith({{eqParam(0, EqType), EqLowCut}, {eqParam(0, EqFreq), 1000}, {eqFl(0, EqWidth), static_cast<float>(1 - std::cbrt(1.0 / 11))}});
  expect(near(bandDb(lc, 0, 1000), -3.01, 0.02), "EQ low cut: Butterworth −3 dB at fc");
  expect(near(bandDb(lc, 0, 125), -36.1, 0.4), "EQ low cut: 12 dB/oct (−36 dB three octaves below)");
  expect(near(bandDb(lc, 0, 16000), 0.0, 0.05), "EQ low cut: flat above");
  lc.params[eqFl(0, EqFlOrder)] = EqGentle8;
  expect(bandStages(lc, 0) == 4 && near(bandDb(lc, 0, 1000), -12.04, 0.1), "EQ low cut gentle 8: four Butterworth sections (−12 dB at fc)");
  lc.params[eqFl(0, EqFlOrder)] = EqSteep8;
  expect(bandStages(lc, 0) == 4 && near(bandDb(lc, 0, 1000), -3.1, 0.2) && near(bandDb(lc, 0, 500), -48.25, 0.5) && bandDb(lc, 0, 250) < -90,
         "EQ low cut steep 8: flat to the corner, then 48 dB/oct");
  // Width controls the resonance: narrow width → resonant peak at fc.
  PluginState hc = eqWith({{eqParam(6, EqType), EqHighCut}, {eqParam(6, EqFreq), 2000}, {eqFl(6, EqWidth), 0.0f}});
  expect(near(bandDb(hc, 6, 2000), 20 * std::log10(0.70710678 * 10.0), 0.05), "EQ high cut: width 0 → Q = 7.07 (+17 dB at fc)");
  hc.params[eqFl(6, EqWidth)] = 1.0f;
  expect(near(bandDb(hc, 6, 2000), 20 * std::log10(0.070710678), 0.05), "EQ high cut: width 1 → Q = 0.0707");

  // ---- band pass / notch
  PluginState bp = eqWith({{eqParam(2, EqType), EqBandPass}, {eqParam(2, EqFreq), 1000}});
  expect(near(bandDb(bp, 2, 1000), 0.0, 0.01) && bandDb(bp, 2, 100) < -12, "EQ band pass: unity at fc, falls off");
  PluginState nt = eqWith({{eqParam(2, EqType), EqNotch}, {eqParam(2, EqFreq), 1000}});
  expect(bandDb(nt, 2, 1000) < -60 && near(bandDb(nt, 2, 10000), 0.0, 0.3), "EQ notch: null at fc");
  // Level has no effect on cut / band types.
  nt.params[eqParam(2, EqGain)] = 12;
  expect(bandDb(nt, 2, 10000) < 0.3, "EQ notch: level ignored");

  // ---- shelves: S = 1 at the default width, half gain at fc
  PluginState ls = eqWith({{eqParam(0, EqFreq), 200}, {eqParam(0, EqGain), 9}});
  expect(near(bandDb(ls, 0, 10), 9.0, 0.05) && near(bandDb(ls, 0, 200), 4.5, 0.05) && near(bandDb(ls, 0, 20000), 0.0, 0.05),
         "EQ low shelf: full gain below, half at fc, 0 above");
  PluginState hs = eqWith({{eqParam(6, EqFreq), 4000}, {eqParam(6, EqGain), -9}});
  expect(near(bandDb(hs, 6, 23900), -9.0, 0.2) && near(bandDb(hs, 6, 4000), -4.5, 0.05), "EQ high shelf: cut above, half at fc");
  hs.params[eqFl(6, EqFlOrder)] = EqSteep6;
  expect(bandStages(hs, 6) == 3 && near(bandDb(hs, 6, 23900), -9.0, 0.3), "EQ high shelf steep 6: same total gain over 3 sections");

  // ---- high quality: decramped peaks follow the analog curve near Nyquist
  PluginState hq = eqWith({{eqParam(5, EqFreq), 14000}, {eqParam(5, EqGain), 9}});
  double analog = analogPeakDb(19000, 14000, 2.0, 9);
  double std19 = bandDb(hq, 5, 19000, 44100);
  hq.params[EqHQ] = 1;
  double hq19 = bandDb(hq, 5, 19000, 44100);
  expect(std::fabs(hq19 - analog) < 1.0 && std::fabs(hq19 - analog) < std::fabs(std19 - analog), "EQ HQ: matched response near Nyquist");
  expect(near(bandDb(hq, 5, 14000, 44100), 9.0, 0.6), "EQ HQ: centre gain kept");
  PluginState hql = eqWith({{eqParam(0, EqType), EqLowCut}, {eqParam(0, EqFreq), 1000}, {EqHQ, 1}, {eqFl(0, EqWidth), static_cast<float>(1 - std::cbrt(1.0 / 11))}});
  expect(near(bandDb(hql, 0, 1000), -3.01, 0.3) && near(bandDb(hql, 0, 16000), 0.0, 0.2), "EQ HQ low cut: Butterworth");

  // ---- audio: the effect applies the designed response; listen solos a band
  {
    auto fx = createEffect("gl.eq");
    PluginState s = eqWith({{eqParam(3, EqFreq), 1000}, {eqParam(3, EqGain), 12}});
    ProcessContext ctx;
    ctx.sampleRate = kSr;
    const int N = 9600;
    std::vector<float> L(N), R(N);
    for (int i = 0; i < N; ++i) L[i] = R[i] = 0.1f * static_cast<float>(std::sin(2 * dsp::kPi * 1000 * i / kSr));
    fx->process(s, ctx, L.data(), R.data(), N);
    double rmsOut = 0;
    for (int i = N / 2; i < N; ++i) rmsOut += static_cast<double>(L[i]) * L[i];
    rmsOut = std::sqrt(rmsOut / (N / 2));
    expect(near(dsp::gainToDb(rmsOut / (0.1 / std::sqrt(2.0))), 12.0, 0.2), "EQ effect: +12 dB at the band frequency");
  }

  // ---- linear phase: same magnitude, symmetric impulse, latency N + N/2
  {
    auto fx = createEffect("gl.eq");
    PluginState s = eqWith({{eqParam(3, EqFreq), 1000}, {eqParam(3, EqGain), 12}, {EqLinear, 3}});  // Medium = 2048 taps
    const int taps = eqLinearTaps(3);
    ProcessContext ctx;
    ctx.sampleRate = kSr;
    const int N = 4 * taps + 4800;
    std::vector<float> L(static_cast<size_t>(N), 0.f), R(static_cast<size_t>(N), 0.f);
    L[10] = R[10] = 1.f;
    for (int i = 0; i < N; i += 256) fx->process(s, ctx, L.data() + i, R.data() + i, std::min(256, N - i));
    int pk = 0;
    for (int i = 0; i < N; ++i)
      if (std::fabs(L[static_cast<size_t>(i)]) > std::fabs(L[static_cast<size_t>(pk)])) pk = i;
    double asym = 0, energy = 0;
    for (int k = 1; k < 600; ++k) {
      asym += std::fabs(L[static_cast<size_t>(pk - k)] - L[static_cast<size_t>(pk + k)]);
      energy += std::fabs(L[static_cast<size_t>(pk + k)]);
    }
    expect(std::abs(pk - (10 + taps + taps / 2)) <= 1 && asym < 0.02 * energy, "EQ linear phase: symmetric impulse, latency N + N/2");
    // magnitude at 1 kHz from the impulse response
    double re = 0, im = 0;
    for (int i = 0; i < N; ++i) re += L[static_cast<size_t>(i)] * std::cos(2 * dsp::kPi * 1000 * i / kSr), im -= L[static_cast<size_t>(i)] * std::sin(2 * dsp::kPi * 1000 * i / kSr);
    expect(near(dsp::gainToDb(std::hypot(re, im)), 12.0, 0.3), "EQ linear phase: +12 dB at the band frequency");
  }

  // ---- legacy states migrate to the reference model
  {
    PluginState old;
    old.type = "gl.eq";
    old.params.assign(static_cast<size_t>(EqSpectrum + 1), 0.f);
    old.params[eqParam(0, EqType)] = EqLowCut;
    old.params[eqParam(0, EqFreq)] = 100;
    old.params[eqParam(0, EqQ)] = 0.707f;
    old.params[eqParam(0, EqSlope)] = 1;  // 24 dB
    old.params[eqParam(3, EqType)] = EqPeak;
    old.params[eqParam(3, EqFreq)] = 1000;
    old.params[eqParam(3, EqQ)] = 1.0f;
    old.params[eqParam(3, EqGain)] = 6;
    old.params[eqExt(3, EqOrder)] = EqOrderNormal;
    for (int b = 0; b < kEqBands; ++b) old.params[eqExt(b, EqEnabled)] = 1;
    normalizePluginState(old);
    expect(old.params[EqModel] == 1 && old.params[eqFl(0, EqFlOrder)] == EqSteep4 && near(old.params[eqFl(0, EqWidth)], 1 - std::cbrt(0.9 / 9.9), 0.01),
           "EQ migration: cut Q/slope → width/order");
    double bwOld = 2.0 / std::log(2.0) * std::asinh(0.5);
    expect(near(0.1 + 4.9 * old.params[eqFl(3, EqWidth)], bwOld, 0.01) && near(bandDb(old, 3, 1000), 6.0, 0.01), "EQ migration: peak Q → bandwidth");
  }

  // ---- FL presets / render test (skipped without FL Studio)
  if (fs::exists(flPreset("Fruity Parametric EQ 2", "Default"))) {
    flp::PresetResult r;
    expect(flp::importFlPreset(flPreset("Fruity Parametric EQ 2", "Default"), r) && r.state.type == "gl.eq" && r.exact, "FL PEQ2 Default preset decodes exactly");
    PluginState def = defaultPluginState("gl.eq");
    bool same = true;
    for (int b = 0; b < kEqBands; ++b)
      for (int idx : {eqParam(b, EqType), eqParam(b, EqFreq), eqParam(b, EqGain), eqFl(b, EqWidth), eqFl(b, EqFlOrder)})
        same = same && near(r.state.params[static_cast<size_t>(idx)], def.params[static_cast<size_t>(idx)], 0.02 * std::max(1.0f, def.params[static_cast<size_t>(idx)]));
    expect(same, "FL PEQ2 Default preset = Gridline defaults");
    expect(flp::importFlPreset(flPreset("Fruity Parametric EQ 2", "40Hz cut"), r), "FL PEQ2 '40Hz cut' decodes");
    expect(r.state.params[eqParam(0, EqType)] == EqLowCut && r.state.params[eqFl(0, EqFlOrder)] == EqSteep8 &&
               near(r.state.params[eqParam(0, EqFreq)], 20 * std::pow(1000.0, 4523.0 / 65536), 0.01) && bandDb(r.state, 0, 15) < -24,
           "FL PEQ2 '40Hz cut': steep-8 high pass at 32 Hz");
    expect(flp::importFlPreset(flPreset("Fruity Parametric EQ 2", "Old Telephone"), r) && r.state.params[eqParam(5, EqType)] == EqOff &&
               near(r.state.params[eqParam(4, EqGain)], 8.8, 1e-4),
           "FL PEQ2 'Old Telephone': disabled band 6, band 5 +8.8 dB");
  } else {
    std::printf("# FL Studio not installed: preset checks skipped\n");
  }
}

// ---- runs an effect over a whole buffer in 256-sample blocks
struct Buf2 {
  std::vector<float> l, r;
};
Buf2 runEffect(const PluginState& s, Buf2 in, double tempo = 120) {
  auto fx = createEffect(s.type);
  ProcessContext ctx;
  ctx.sampleRate = kSr;
  ctx.tempo = tempo;
  for (size_t i = 0; i < in.l.size(); i += 256) {
    int n = static_cast<int>(std::min<size_t>(256, in.l.size() - i));
    fx->process(s, ctx, in.l.data() + i, in.r.data() + i, n);
  }
  return in;
}
Buf2 impulse(size_t n, size_t at = 10) {
  Buf2 b;
  b.l.assign(n, 0.f), b.r.assign(n, 0.f);
  b.l[at] = b.r[at] = 1.f;
  return b;
}
// Index of the largest |sample| in [from, to).
size_t peakAt(const std::vector<float>& v, size_t from, size_t to) {
  size_t best = from;
  for (size_t i = from; i < std::min(to, v.size()); ++i)
    if (std::fabs(v[i]) > std::fabs(v[best])) best = i;
  return best;
}

void testDelay() {
  // ---- curves (reference values)
  expect(near(dly::freeTimeSec(0), 0.001, 1e-6) && near(dly::freeTimeSec(1), 1.0, 1e-6), "Delay curve: free time 1 ms .. 1 s");
  expect(near(dly::freeTimeNorm(dly::freeTimeSec(0.37)), 0.37, 1e-6), "Delay curve: free time inverse");
  expect(near(dly::modRateHz(0), 1 / 8.872, 1e-6) && near(dly::modRateHz(1), 1 / 0.05, 0.05) && near(dly::modRateNorm(dly::modRateHz(0.42)), 0.42, 1e-6),
         "Delay curve: modulation rate 0.11 .. 20 Hz (period table)");
  expect(near(dly::cutoffHz(0, 0), 270, 1e-9) && near(dly::cutoffHz(0, 0.5), 1300 + 0.9995 * 0 + (1300 - 1300), 300) &&
             near(dly::cutoffHz(1, 0), 20, 1e-9) && near(dly::cutoffHz(1, 1), 4200 - 0.001 * 2900, 1) && near(dly::cutoffHz(2, 1), 7000 + 0.6 * 15000, 1e-6),
         "Delay curve: filter cutoff tables per mode");
  expect(near(dly::cutoffNorm(0, dly::cutoffHz(0, 0.63)), 0.63, 1e-6) && near(dly::cutoffNorm(1, dly::cutoffHz(1, 0.2)), 0.2, 1e-6), "Delay curve: cutoff inverse");
  expect(near(dly::filterQ(0, 0), 0.707, 1e-9) && near(dly::filterQ(0, 1), 1.707, 1e-9) && near(dly::filterQ(2, 1), 1.257, 1e-9), "Delay curve: filter Q");
  expect(near(dly::bits(0), 2, 1e-9) && near(dly::bits(1), 16, 1e-9) && near(dly::bits(0.75), 9, 1e-9) && near(dly::bitsNorm(9), 0.75, 1e-9), "Delay curve: bits");
  expect(near(dly::rateFactor(1), 1, 1e-9) && near(dly::rateFactor(0), 1 + 0.7435 * 127 - 0.0171 * 127 * 127 + 0.0002 * 127 * 127 * 127, 0.05) &&
             near(dly::rateNorm(dly::rateHz(0.3)), 0.3, 1e-4),
         "Delay curve: rate reduction factor");
  expect(near(dly::distThreshDb(1), -28.5, 1e-9) && near(dly::distMakeup(0), 1, 1e-9) && dly::distMakeup(1) > 1, "Delay curve: distortion threshold / make-up");

  // ---- timing per sync division (true stereo, no feedback, dry off, 120 BPM: a beat = 0.5 s)
  const size_t N = static_cast<size_t>(kSr * 2.6);
  for (int ticks : {12, 24, 32, 48, 64, 72, 96, 128, 144, 192, 288, 384, 576}) {
    PluginState s = defaultPluginState("gl.delay");
    s.params[DlyTicks] = static_cast<float>(ticks);
    s.params[DlyFeedback] = 0, s.params[DlyDry] = 0, s.params[DlyFilter3] = 3, s.params[DlyDistType] = 0;
    Buf2 o = runEffect(s, impulse(N));
    size_t expectAt = 10 + static_cast<size_t>(std::floor(ticks / 192.0 * 0.5 * kSr));
    char name[80];
    std::snprintf(name, sizeof name, "Delay sync: %d ticks echo at %.4f s", ticks, ticks / 192.0 * 0.5);
    expect(std::abs(static_cast<long>(peakAt(o.l, 0, N)) - static_cast<long>(expectAt)) <= 1, name);
  }
  // ---- free time
  {
    PluginState s = defaultPluginState("gl.delay");
    s.params[DlySync] = 0, s.params[DlyTimeMs] = 250, s.params[DlyFeedback] = 0, s.params[DlyDry] = 0, s.params[DlyFilter3] = 3, s.params[DlyDistType] = 0;
    Buf2 o = runEffect(s, impulse(N));
    expect(std::abs(static_cast<long>(peakAt(o.l, 0, N)) - static_cast<long>(10 + 12000)) <= 1, "Delay free time: 250 ms");
  }
  // ---- R/L offset (true stereo: one side + T·x²/4)
  {
    PluginState s = defaultPluginState("gl.delay");
    s.params[DlyTicks] = 192, s.params[DlyTimeR] = 1, s.params[DlyFeedback] = 0, s.params[DlyDry] = 0, s.params[DlyFilter3] = 3, s.params[DlyDistType] = 0;
    Buf2 o = runEffect(s, impulse(N));
    size_t T = static_cast<size_t>(0.5 * kSr);
    expect(std::abs(static_cast<long>(peakAt(o.l, 0, N)) - static_cast<long>(10 + T)) <= 1 &&
               std::abs(static_cast<long>(peakAt(o.r, 0, N)) - static_cast<long>(10 + T + T / 4)) <= 1,
           "Delay R/L offset +100%: right side a quarter of the time later");
  }
  // ---- ping-pong: left at T, right at 2T, feedback scales each pass
  {
    PluginState s = defaultPluginState("gl.delay");
    s.params[DlyType3] = 2, s.params[DlyTicks] = 96, s.params[DlyFeedback] = 0.5f, s.params[DlyDry] = 0, s.params[DlyFilter3] = 3, s.params[DlyDistType] = 0;
    Buf2 o = runEffect(s, impulse(N));
    size_t T = static_cast<size_t>(0.25 * kSr);
    float l1 = o.l[10 + T], r2 = o.r[10 + 2 * T], l3 = o.l[10 + 3 * T];
    expect(std::fabs(o.r[10 + T]) < 1e-3 && std::fabs(o.l[10 + 2 * T]) < 1e-3, "Delay ping-pong: echoes alternate sides");
    expect(near(r2 / l1, 0.5, 0.02) && near(l3 / r2, 0.5, 0.02), "Delay ping-pong: feedback 50 % per pass");
  }
  // ---- feedback chain also processes the first echo (reference flow): a low-pass dulls echo 1
  {
    PluginState s = defaultPluginState("gl.delay");
    s.params[DlyFeedback] = 0, s.params[DlyDry] = 0, s.params[DlyFilter3] = 0, s.params[DlyCutoff3] = 300, s.params[DlyDistType] = 0;
    Buf2 o = runEffect(s, impulse(N));
    size_t T = static_cast<size_t>(0.5 * kSr);
    expect(std::fabs(o.l[10 + T]) < 0.1, "Delay: the input passes the feedback filter before the first echo");
  }
  // ---- high feedback with distortion stays bounded; off type is dry only
  {
    PluginState s = defaultPluginState("gl.delay");
    s.params[DlyFeedback] = 1.25f, s.params[DlyTicks] = 24, s.params[DlyDistType] = 1, s.params[DlyDistThresh] = -12;
    Buf2 in;
    in.l.assign(N, 0.f), in.r.assign(N, 0.f);
    uint32_t seed = 1;
    for (size_t i = 0; i < N; ++i) {
      seed = seed * 1664525u + 1013904223u;
      in.l[i] = in.r[i] = 0.3f * (static_cast<float>(seed >> 8) / 8388608.0f - 1.0f);
    }
    Buf2 o = runEffect(s, in);
    float pk = 0;
    bool fin = true;
    for (float x : o.l) pk = std::max(pk, std::fabs(x)), fin = fin && std::isfinite(x);
    expect(fin && pk < 4, "Delay: 125 % feedback bounded by the loop limiter");
    s.params[DlyType3] = 3, s.params[DlyDry] = 0;
    o = runEffect(s, impulse(N));
    pk = 0;
    for (float x : o.l) pk = std::max(pk, std::fabs(x));
    expect(pk < 1e-6, "Delay type off: no echoes");
  }
  // ---- old states keep the legacy model
  {
    PluginState old;
    old.type = "gl.delay";
    old.params.assign(static_cast<size_t>(DlyInputPan + 1), 0.f);
    normalizePluginState(old);
    expect(old.params[DlyModel] == 0, "Delay: pre-κ1 states keep the legacy model");
    expect(defaultPluginState("gl.delay").params[DlyModel] == 1, "Delay: new instances use the reference model");
  }
  if (fs::exists(flPreset("Fruity Delay 3", "Default"))) {
    flp::PresetResult r;
    expect(flp::importFlPreset(flPreset("Fruity Delay 3", "Ping pong"), r) && r.state.type == "gl.delay" && r.state.params[DlyType3] == 2 &&
               r.state.params[DlyTicks] == 96 && near(r.state.params[DlyTimeR], -1, 1e-6),
           "FL Delay 3 'Ping pong': type, 1/8 note, offset");
    expect(flp::importFlPreset(flPreset("Fruity Delay 3", "Bandpass"), r) && r.state.params[DlyFilter3] == 2 &&
               near(r.state.params[DlyCutoff3], dly::cutoffHz(2, 3562 / 6000.0), 0.5) && near(r.state.params[DlyResonance], 2138 / 6000.0, 1e-5),
           "FL Delay 3 'Bandpass': band-pass feedback filter");
    expect(flp::importFlPreset(flPreset("Fruity Delay 3", "Chorus"), r) && r.state.params[DlySync] == 0 &&
               near(r.state.params[DlyModRate3], dly::modRateHz(1032 / 6000.0 * 0.99), 1e-3) && near(r.state.params[DlyModDepth], 3806 / 6000.0, 1e-5),
           "FL Delay 3 'Chorus': free time, modulation rate / depth");
    // J-26: Delay 2 imports onto the classic tempo-delay model (exact).
    expect(flp::importFlPreset(flPreset("Fruity Delay 2", "Widen"), r) && r.state.params[DlyModel] == 2 && r.exact &&
               near(r.state.params[DlyClsOffset], -(std::pow(1001.0, 394 / 512.0) - 1) / 2, 0.01),
           "FL Delay 2 'Widen': classic model, offset curve (101 ms left)");
  }
}

// RT60 estimated from an impulse response (Schroeder backward integration, −5..−25 dB fit).
double rt60(const std::vector<float>& l, const std::vector<float>& r) {
  size_t n = l.size();
  std::vector<double> e(n);
  double acc = 0;
  for (size_t i = n; i-- > 0;) acc += static_cast<double>(l[i]) * l[i] + static_cast<double>(r[i]) * r[i], e[i] = acc;
  double e0 = e[0];
  size_t i5 = 0, i25 = 0;
  for (size_t i = 0; i < n; ++i) {
    double db = 10 * std::log10(std::max(1e-30, e[i] / e0));
    if (!i5 && db <= -5) i5 = i;
    if (!i25 && db <= -25) {
      i25 = i;
      break;
    }
  }
  if (!i5 || !i25) return -1;
  return (static_cast<double>(i25 - i5) / kSr) * 3.0;
}

void testReverb() {
  PluginState d = defaultPluginState("gl.reverb");
  expect(near(d.params[RvbDecayTime], 1.5, 1e-6) && near(d.params[RvbSize], 0.5, 1e-6) && near(d.params[RvbHighCut], 4000, 1e-3) &&
             near(d.params[RvbLowCut], 75, 1e-3) && near(d.params[RvbDampHz], 4000, 1e-3),
         "Reverb defaults: reference default state (1.5 s, size 50, cuts 75 Hz / 4 kHz, damping 4 kHz)");
  for (double target : {0.8, 2.0, 4.0}) {
    PluginState s = defaultPluginState("gl.reverb");
    s.params[RvbDecayTime] = static_cast<float>(target), s.params[RvbDry] = 0, s.params[RvbEarly] = 0, s.params[RvbHighCut] = 22100,
    s.params[RvbDampHz] = 22100, s.params[RvbLowCut] = 20, s.params[RvbBassMult] = 1, s.params[RvbWet] = 1;
    Buf2 o = runEffect(s, impulse(static_cast<size_t>(kSr * (target * 2.5 + 0.5))));
    double t = rt60(o.l, o.r);
    char name[80];
    std::snprintf(name, sizeof name, "Reverb decay %.1f s: measured RT60 %.2f s", target, t);
    expect(t > target * 0.9 && t < target * 1.1, name);
  }
  {
    PluginState s = defaultPluginState("gl.reverb");
    s.params[RvbPreDelay] = 100, s.params[RvbDry] = 0, s.params[RvbEarly] = 0;
    Buf2 o = runEffect(s, impulse(static_cast<size_t>(kSr)));
    size_t first = 0;
    for (size_t i = 0; i < o.l.size(); ++i)
      if (std::fabs(o.l[i]) > 1e-5 || std::fabs(o.r[i]) > 1e-5) {
        first = i;
        break;
      }
    expect(first >= 10 + static_cast<size_t>(0.1 * kSr) - 2, "Reverb pre-delay 100 ms: nothing before it");
    s.params[RvbPreSync] = 1;  // 100 ms value = 38.4 ticks of 1/192 beat; at 60 BPM → 200 ms
    o = runEffect(s, impulse(static_cast<size_t>(kSr)), 60);
    first = 0;
    for (size_t i = 0; i < o.l.size(); ++i)
      if (std::fabs(o.l[i]) > 1e-5 || std::fabs(o.r[i]) > 1e-5) {
        first = i;
        break;
      }
    expect(first >= 10 + static_cast<size_t>(0.2 * kSr) - 2 && first < 10 + static_cast<size_t>(0.2 * kSr) + 2000, "Reverb tempo pre-delay follows the tempo");
  }
  if (fs::exists(flPreset("Fruity Reeverb 2", "Default"))) {
    flp::PresetResult r;
    expect(flp::importFlPreset(flPreset("Fruity Reeverb 2", "Cathedral"), r) && r.state.type == "gl.reverb", "FL Reeverb 2 'Cathedral' decodes");
    expect(near(r.state.params[RvbDecayTime], 4.5, 1e-5) && near(r.state.params[RvbSize], 0.9, 1e-6) && near(r.state.params[RvbPreDelay], 27 * 1000 / 384.0, 1e-3) &&
               near(r.state.params[RvbLowCut], 33, 1e-3) && near(r.state.params[RvbHighCut], 2100, 1e-3) && near(r.state.params[RvbDampHz], 3100, 1e-3) &&
               near(r.state.params[RvbBassMult], 1.5, 1e-5) && near(r.state.params[RvbCrossover], 298, 1e-3),
           "FL Reeverb 2 'Cathedral': decay, size, pre-delay, cuts, damping, bass, crossover");
    expect(flp::importFlPreset(flPreset("Fruity Reeverb 2", "Default"), r) && near(r.state.params[RvbWet], (std::pow(11.0, 87 / 256.0) - 1) / (std::pow(11.0, 0.5) - 1), 1e-5),
           "FL Reeverb 2 'Default': wet level curve");
  }
}

// Render tests (FL Studio's own test projects): the effect lands on insert 1 with its settings.
void testRenderTests() {
  const std::string dir = std::string(kFlData) + "/System/Render tests/";
  if (!fs::exists(dir + "Fruity Parametric EQ 2.flp")) return;
  auto slotOf = [&](const char* name, const char* type) -> PluginState {
    Song song;
    flp::Report rep;
    std::string err;
    if (!importFlp(dir + name + ".flp", song, rep, &err)) return {};
    for (const auto& m : song.mixer)
      for (const auto& sl : m.slots)
        if (sl.plugin.type == type) return sl.plugin;
    return {};
  };
  PluginState eq = slotOf("Fruity Parametric EQ 2", "gl.eq");
  expect(eq.type == "gl.eq" && near(eq.params[eqParam(0, EqGain)], -17.7, 1e-4) && near(eq.params[eqParam(0, EqFreq)], 20 * std::pow(1000.0, 26283.0 / 65536), 0.01) &&
             eq.params[eqParam(6, EqType)] == EqHighShelf && near(eq.params[eqFl(0, EqWidth)], 0, 1e-6),
         "Render test PEQ2: insert 1 EQ with the project's bands");
  PluginState rv = slotOf("Fruity Reeverb 2", "gl.reverb");
  expect(rv.type == "gl.reverb" && near(rv.params[RvbDecayTime], 4.5, 1e-5) && near(rv.params[RvbCrossover], 298, 1e-3), "Render test Reeverb 2: settings (version 10000 state)");
  PluginState dl = slotOf("Fruity Delay 2", "gl.delay");
  expect(dl.type == "gl.delay" && dl.params[DlyModel] == 2 && near(dl.params[DlyTicks], 144, 1e-6), "Render test Delay 2: classic delay, ticks");
  PluginState db = slotOf("Fruity Delay Bank", "gl.multidelay");
  expect(db.type == "gl.multidelay", "Render test Delay Bank: multi delay");
  // J-1: Delay Bank imports run the multi delay's reference model (appended "Delay model" choice).
  const PluginInfo* mdi = findPlugin("gl.multidelay");
  bool refModel = false;
  for (size_t i = 0; mdi && i < mdi->params.size() && i < db.params.size(); ++i)
    if (mdi->params[i].name == "Delay model") refModel = db.params[i] == 1;
  expect(refModel, "Render test Delay Bank: reference delay model");
}

}  // namespace

int main() {
  testEq();
  testDelay();
  testReverb();
  testRenderTests();
  return finish();
}
