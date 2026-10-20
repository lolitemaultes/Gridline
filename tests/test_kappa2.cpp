// Gridline — κ2 dynamics parity tests: Limiter, Compressor, Soft Clipper,
// Transient Shaper, Multiband Compressor, Mastering Dynamics (Maximus
// counterpart) and Enhance (Soundgoodizer counterpart). Static curves vs
// settings, timing, ceilings, gates, crossover flatness, FL preset decoding
// (when an FL Studio installation is present).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>
#include "Check.h"
#include "core/PluginInfo.h"
#include "core/PluginParams.h"
#include "core/Song.h"
#include "core/flp/Flp.h"
#include "core/flp/FlpPresets.h"
#include "packs/dynamics/dynamics.h"
#include "engine/Dsp.h"
#include "engine/Effects.h"

using namespace gl;
using namespace gl::pp;

namespace {

const double kSr = 48000.0;
const std::string kFl = "/Applications/FL Studio 2026.app/Contents/Resources/FL";
const std::string kFlPresets = kFl + "/Data/Patches/Plugin presets/Effects/";

struct Buf {
  std::vector<float> L, R;
  explicit Buf(size_t n = 0) : L(n, 0.f), R(n, 0.f) {}
  size_t size() const { return L.size(); }
};

Buf run(Effect& fx, const PluginState& st, Buf b, int block = 256) {
  ProcessContext ctx;
  ctx.sampleRate = kSr;
  ctx.tempo = 120;
  for (size_t i = 0; i < b.size(); i += static_cast<size_t>(block)) {
    int n = static_cast<int>(std::min<size_t>(static_cast<size_t>(block), b.size() - i));
    fx.process(st, ctx, b.L.data() + i, b.R.data() + i, n);
  }
  return b;
}
Buf run(const PluginState& st, const Buf& b, int block = 256) {
  auto fx = createEffect(st.type);
  fx->reset();
  return run(*fx, st, b, block);
}
Buf sine(double hz, double amp, double seconds) {
  Buf b(static_cast<size_t>(seconds * kSr));
  for (size_t i = 0; i < b.size(); ++i) {
    double v = amp * std::sin(2 * M_PI * hz * static_cast<double>(i) / kSr);
    b.L[i] = b.R[i] = static_cast<float>(v);
  }
  return b;
}
Buf noise(double amp, double seconds, uint32_t seed = 7) {
  Buf b(static_cast<size_t>(seconds * kSr));
  uint32_t s = seed;
  for (size_t i = 0; i < b.size(); ++i) {
    s ^= s << 13, s ^= s >> 17, s ^= s << 5;
    b.L[i] = static_cast<float>(amp * ((s & 0xFFFF) / 32767.5 - 1.0));
    s ^= s << 13, s ^= s >> 17, s ^= s << 5;
    b.R[i] = static_cast<float>(amp * ((s & 0xFFFF) / 32767.5 - 1.0));
  }
  return b;
}
double peak(const Buf& b, size_t from = 0, size_t to = 0) {
  if (!to) to = b.size();
  double p = 0;
  for (size_t i = from; i < to; ++i) p = std::max(p, static_cast<double>(std::max(std::fabs(b.L[i]), std::fabs(b.R[i]))));
  return p;
}
double rms(const Buf& b, size_t from, size_t to) {
  double s = 0;
  for (size_t i = from; i < to; ++i) s += 0.5 * (b.L[i] * b.L[i] + b.R[i] * b.R[i]);
  return std::sqrt(s / std::max<size_t>(1, to - from));
}
bool finiteAll(const Buf& b) {
  for (size_t i = 0; i < b.size(); ++i)
    if (!std::isfinite(b.L[i]) || !std::isfinite(b.R[i])) return false;
  return true;
}
PluginState state(const char* type) { return defaultPluginState(type); }
bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }
bool flPreset(const std::string& rel, flp::PresetResult& out) {
  std::string p = kFlPresets + rel;
  if (!std::filesystem::exists(p)) return false;
  return flp::importFlPreset(p, out, kFl) && out.kind == flp::PresetResult::Plugin;
}

// ======================================================================= Limiter
void testLimiter() {
  // Ceiling is never exceeded (hard clip stage), with +12 dB into -3 dB.
  {
    PluginState st = state("gl.limiter");
    st.params[LimGain] = 12;
    st.params[LimCeiling] = -3;
    Buf out = run(st, noise(0.9, 1.0));
    expect(peak(out) <= dsp::dbToGain(-3) + 1e-6, "limiter: output never above the ceiling");
    expect(finiteAll(out), "limiter: finite output");
  }
  // Look-ahead: the attack time delays the signal by exactly that many samples.
  {
    PluginState st = state("gl.limiter");
    st.params[LimAttack] = 0.002f;
    Buf in(4800);
    in.L[100] = in.R[100] = 0.5f;
    Buf out = run(st, in);
    size_t at = 0;
    for (size_t i = 0; i < out.size(); ++i)
      if (std::fabs(out.L[i]) > 0.1f) {
        at = i;
        break;
      }
    expect(at == 100 + 96, "limiter: latency = attack (2 ms = 96 samples)");
  }
  // Limiting: a steady sine 6 dB over the ceiling settles at the ceiling, and
  // the waveform is scaled (not clipped): crest factor stays that of a sine.
  {
    PluginState st = state("gl.limiter");
    st.params[LimCeiling] = -6;
    Buf out = run(st, sine(200, 1.0, 1.0));
    size_t a = out.size() / 2, b = out.size();
    double pk = peak(out, a, b), r = rms(out, a, b);
    expect(near(dsp::gainToDb(pk), -6.0, 0.15), "limiter: sine settles at the ceiling");
    expect(near(pk / r, std::sqrt(2.0), 0.05), "limiter: limits by gain, not by clipping");
  }
  // Compressor static curve: 4:1 above -20 dB (RMS detector), level -8 dB RMS.
  {
    PluginState st = state("gl.limiter");
    st.params[LimView] = 1;
    st.params[LimCompThresh] = -20;
    st.params[LimCompRatio] = 4;
    st.params[LimCompRms] = 0.02f;
    double amp = dsp::dbToGain(-8.0) * std::sqrt(2.0);
    Buf out = run(st, sine(300, amp, 1.5));
    double lvIn = -8.0, expect_ = -20.0 + (lvIn + 20.0) / 4.0;
    double got = dsp::gainToDb(rms(out, out.size() / 2, out.size()));
    expect(near(got, expect_, 0.4), "limiter: compressor 4:1 static curve (RMS)");
  }
  // Expansion (ratio 1:2 = 0.5) raises the level above the threshold.
  {
    PluginState st = state("gl.limiter");
    st.params[LimCompThresh] = -20;
    st.params[LimCompRatio] = 0.5f;
    st.params[LimCompRms] = 0.02f;
    double amp = dsp::dbToGain(-14.0) * std::sqrt(2.0);
    Buf out = run(st, sine(300, amp, 1.5));
    double got = dsp::gainToDb(rms(out, out.size() / 2, out.size()));
    expect(near(got, -20.0 + 6.0 * 2.0, 0.5), "limiter: expansion 1:2 above the threshold");
  }
  // Noise gate: a quiet signal below the threshold falls to the noise gain.
  {
    PluginState st = state("gl.limiter");
    st.params[LimGateThresh] = -30;
    st.params[LimGateFloor] = -40;
    st.params[LimGateRelease] = 0.05f;
    Buf out = run(st, sine(300, dsp::dbToGain(-40), 1.0));
    double got = dsp::gainToDb(peak(out, out.size() / 2, out.size()));
    expect(near(got, -80.0, 1.0), "limiter: gate drops a -40 dB signal by the noise gain (40 dB)");
    st.params[LimGateThresh] = -50;
    out = run(st, sine(300, dsp::dbToGain(-40), 1.0));
    got = dsp::gainToDb(peak(out, out.size() / 2, out.size()));
    expect(near(got, -40.0, 0.2), "limiter: gate stays open above the threshold");
  }
  // Release: after a loud burst the gain recovers; longer release = slower.
  {
    auto recovery = [](float rel) {
      PluginState st = state("gl.limiter");
      st.params[LimCeiling] = -12;
      st.params[LimRelease] = rel;
      Buf in = sine(200, 1.0, 1.0);
      for (size_t i = in.size() / 4; i < in.size(); ++i) in.L[i] = in.R[i] = static_cast<float>(0.1 * std::sin(2 * M_PI * 200 * i / kSr));
      Buf out = run(st, in);
      size_t t = in.size() / 4 + static_cast<size_t>(0.05 * kSr);
      return dsp::gainToDb(peak(out, t, t + 480) / 0.1);
    };
    double fast = recovery(0.02f), slow = recovery(0.5f);
    expect(fast > slow + 3.0, "limiter: longer release recovers more slowly");
    expect(fast > -1.0, "limiter: 20 ms release has recovered after 50 ms");
  }
  // Saturation: soft curve approaches (never reaches) min(ceiling, 1).
  {
    PluginState st = state("gl.limiter");
    st.params[LimSaturation] = -6;
    st.params[LimAttack] = 0.0001f;
    st.params[LimGain] = 12;
    Buf out = run(st, sine(100, 1.0, 0.5));
    // limiter holds peaks at the ceiling (1.0), the soft curve maps 1.0 to 0.5 + 0.5 (1 - e^-1)
    expect(near(peak(out), 0.5 + 0.5 * (1 - std::exp(-1.0)), 0.01), "limiter: exponential saturation above its threshold");
  }
  // Old / short param vectors are padded.
  {
    PluginState st;
    st.type = "gl.limiter";
    st.params = {3, -1, 0.1f};
    normalizePluginState(st);
    expect(st.params.size() == static_cast<size_t>(LimCount) && st.params[LimCompShape] == 0, "limiter: old state padded");
  }
  // FL decoder: exact knob laws on shipped presets.
  flp::PresetResult r;
  if (flPreset("Fruity Limiter/Max loudness.fst", r)) {
    const auto& p = r.state.params;
    expect(r.state.type == "gl.limiter", "limiter preset: type");
    expect(near(p[LimGain], 20 * std::log10((std::pow(7.0, 1.654) - 1) / 6), 0.01), "limiter preset: gain law (7^x-1)/6");
    expect(near(p[LimAttack], 0.002, 0.0001), "limiter preset: attack 1115 = 2 ms");
    expect(near(p[LimRelease], 0.0855, 0.001), "limiter preset: release 5000 = 85.5 ms");
    expect(near(p[LimHold], 0.002, 0.0001), "limiter preset: sustain window law");
  }
  if (flPreset("Fruity Limiter/Max loudness (slight saturation).fst", r)) {
    const auto& p = r.state.params;
    expect(near(p[LimCeiling], 20 * std::log10((std::pow(3.0, 1.18) - 1) / 2), 0.01), "limiter preset: ceiling law (3^x-1)/2");
    expect(near(p[LimSaturation], 20 * std::log10((std::pow(3.0, 0.76) - 1) / 2), 0.01), "limiter preset: saturation law");
  }
  if (flPreset("Fruity Limiter/Drum compression.fst", r)) {
    const auto& p = r.state.params;
    expect(near(p[LimCompRatio], 1.0 + 0.38 * (std::exp(3.9318256327243257) - 1.0), 0.01), "limiter preset: ratio law (1000 = 20:1)");
    expect(near(p[LimCompThresh], 20 * std::log10((std::pow(7.0, 0.2) - 1) / 6), 0.01) && near(p[LimCompShape], 0.644, 1e-4), "limiter preset: threshold + knee");
  }
  if (flPreset("Fruity Limiter/Drum expanding.fst", r))
    expect(near(r.state.params[LimCompRatio], 1.0, 1e-4) && near(r.state.params[LimCompShape], -0.776, 1e-4), "limiter preset: 1:1 ratio + negative knee");
  if (flPreset("Fruity Limiter/Noise gate.fst", r))
    expect(r.state.params[LimGateFloor] <= -79.9f && r.state.params[LimGateThresh] > -60, "limiter preset: noise gate");
}


// ======================================================================= Compressor
void testCompressor() {
  auto base = []() {
    PluginState st = state("gl.comp");
    st.params[CmpThreshold] = -20;
    st.params[CmpRatio] = 4;
    st.params[CmpKnee] = 0;
    st.params[CmpAttack] = 0.0001f;
    st.params[CmpRelease] = 0.3f;
    return st;
  };
  {  // hard-knee static curve on peaks
    PluginState st = base();
    Buf out = run(st, sine(200, dsp::dbToGain(-8), 1.5));
    double got = dsp::gainToDb(peak(out, out.size() / 2, out.size()));
    expect(near(got, -20.0 + 12.0 / 4.0, 0.5), "comp: 4:1 above -20 dB (peak detector)");
  }
  {  // below the threshold: unity (DC high-pass is transparent at 200 Hz)
    PluginState st = base();
    Buf out = run(st, sine(200, dsp::dbToGain(-30), 1.0));
    expect(near(dsp::gainToDb(peak(out, out.size() / 2, out.size())), -30.0, 0.1), "comp: unity below the threshold");
  }
  {  // soft knee (15 dB): reduction at the threshold = (1/r - 1) * knee / 4
    PluginState st = base();
    st.params[CmpKnee] = 15;
    Buf out = run(st, sine(200, dsp::dbToGain(-20), 1.5));
    double got = dsp::gainToDb(peak(out, out.size() / 2, out.size()));
    expect(near(got, -20.0 - 0.75 * 15.0 / 4.0, 0.4), "comp: soft knee reduction at the threshold");
  }
  {  // attack: 90 % of the (linear) gain change after the attack time
    PluginState st = base();
    st.params[CmpAttack] = 0.01f;
    st.params[CmpLimiter] = 0;  // no look-ahead latency
    Buf in(static_cast<size_t>(0.5 * kSr));
    for (size_t i = 0; i < in.size(); ++i) {
      double a = i < in.size() / 2 ? 0.01 : 1.0;
      in.L[i] = in.R[i] = static_cast<float>(a * std::sin(2 * M_PI * 1000 * i / kSr));
    }
    Buf out = run(st, in);
    size_t t0 = in.size() / 2;
    double gFinal = dsp::dbToGain(-20.0 + 20.0 / 4.0);  // output peak at full scale input
    size_t t10 = t0 + static_cast<size_t>(0.010 * kSr);
    double pk10 = peak(out, t10, t10 + 48), pk0 = peak(out, t0, t0 + 48);
    double frac = (pk0 - pk10) / (pk0 - gFinal);
    expect(frac > 0.8 && frac < 0.97, "comp: attack reaches ~90 % after the attack time");
  }
  {  // output limiter keeps the output under 0 dBFS
    PluginState st = base();
    st.params[CmpRatio] = 1;
    st.params[CmpOutput] = 30;
    Buf out = run(st, noise(0.5, 1.0));
    expect(peak(out) <= 1.0 && finiteAll(out), "comp: output limiter holds 0 dBFS");
    st.params[CmpLimiter] = 0;
    out = run(st, noise(0.5, 1.0));
    expect(peak(out) > 2.0, "comp: limiter can be switched off");
  }
  {  // vintage: reduction stops growing above the knee end
    PluginState st = base();
    st.params[CmpCharacter] = 1;
    st.params[CmpKnee] = 7;
    st.params[CmpThreshold] = -30;
    st.params[CmpLimiter] = 0;
    Buf a = run(st, sine(200, dsp::dbToGain(2), 1.0)), b = run(st, sine(200, dsp::dbToGain(-4), 1.0));
    double ga = dsp::gainToDb(peak(a, a.size() / 2, a.size())) - 2, gb = dsp::gainToDb(peak(b, b.size() / 2, b.size())) + 4;
    double want = 1.25 * std::pow(4.0, 0.25) * (0.25 - 1.0) * 20.0 / std::log(10.0);
    expect(near(ga, want, 0.5) && near(gb, want, 0.5), "comp: vintage gain reduction stops growing above the knee");
  }
  flp::PresetResult r;
  if (flPreset("Fruity Compressor/Vintage.fst", r)) {
    const auto& p = r.state.params;
    expect(r.state.type == "gl.comp" && r.exact, "comp preset: exact mapping");
    expect(near(p[CmpThreshold], -15, 1e-4) && near(p[CmpRatio], 8, 1e-4) && near(p[CmpOutput], 8.6, 1e-4), "comp preset: threshold/ratio/gain");
    expect(near(p[CmpAttack], 0.0292, 1e-6) && near(p[CmpRelease], 1.532, 1e-6), "comp preset: attack/release");
    expect(p[CmpCharacter] == 1 && near(p[CmpKnee], 7, 1e-6) && p[CmpFastRelease] == 0, "comp preset: type 2 = vintage");
  }
  if (flPreset("Fruity Compressor/Brickwall.fst", r))
    expect(r.state.params[CmpFastRelease] == 1 && r.state.params[CmpKnee] == 0, "comp preset: type 4 = hard, fast release");
}


// ======================================================================= Soft clipper
void testClipper() {
  PluginState st = state("gl.clipper");
  expect(near(st.params[ClpThreshold], -2.144, 0.01) && st.params[ClpCeilingMode] == 1, "clipper: reference defaults");
  {  // static curve: linear below the threshold, exponential approach to 1 above
    st.params[ClpThreshold] = static_cast<float>(dsp::gainToDb(0.5));
    Buf in(64);
    const double xs[4] = {0.25, 0.5, 1.0, 3.0};
    for (int i = 0; i < 4; ++i) in.L[static_cast<size_t>(i)] = in.R[static_cast<size_t>(i)] = static_cast<float>(xs[i]);
    Buf out = run(st, in, 64);
    bool ok = true;
    for (int i = 0; i < 4; ++i) {
      double x = xs[i], want = x <= 0.5 ? x : 0.5 + 0.5 * (1 - std::exp(-(x - 0.5) / 0.5));
      ok = ok && near(out.L[static_cast<size_t>(i)], want, 1e-5);
    }
    expect(ok, "clipper: exponential soft-clip curve");
    Buf hot = run(st, sine(100, 8.0, 0.2));
    expect(peak(hot) < 1.0, "clipper: never reaches full scale");
  }
  {  // post gain law: FL 160 -> (11^1.25 - 1) / 10
    flp::PresetResult r;
    if (flPreset("Fruity Soft Clipper/Default.fst", r)) {
      expect(r.exact && near(r.state.params[ClpThreshold], 20 * std::log10(100.0 / 128.0), 1e-3) && near(r.state.params[ClpPostGain], 0, 1e-4),
             "clipper preset: default threshold 100 / post 128");
    }
  }
}


// ======================================================================= Multiband
void testMultiband() {
  auto passAll = [](int ftype) {
    PluginState st = state("gl.multiband");
    st.params[MbFilterType] = static_cast<float>(ftype);
    st.params[MbLowXover] = 200;
    st.params[MbHighXover] = 3000;
    for (int b = 0; b < 3; ++b) st.params[MbLowState + b] = 1;  // bypass: filters only
    return st;
  };
  auto gainAt = [](const PluginState& st, double hz) {
    Buf out = run(st, sine(hz, 0.25, 0.6));
    return dsp::gainToDb(rms(out, out.size() / 2, out.size()) / (0.25 / std::sqrt(2.0)));
  };
  {  // Linkwitz-Riley option: bands sum flat
    PluginState st = passAll(2);
    double worst = 0;
    for (double hz : {60.0, 200.0, 700.0, 3000.0, 9000.0}) worst = std::max(worst, std::fabs(gainAt(st, hz)));
    expect(worst < 0.1, "multiband: Linkwitz-Riley bands sum flat");
  }
  {  // reference Butterworth bands: +3 dB where low and mid overlap at the edge, flat away from it
    PluginState st = passAll(0);
    expect(near(gainAt(st, 200), 3.0, 0.4), "multiband: Butterworth edges overlap (+3 dB at the edge)");
    expect(std::fabs(gainAt(st, 1000)) < 0.6, "multiband: Butterworth flat between edges");
  }
  {  // linear-phase FIR: flat between edges, 52-sample latency
    PluginState st = passAll(1);
    expect(std::fabs(gainAt(st, 1000)) < 0.5, "multiband: FIR passband");
    Buf in(2048);
    in.L[100] = in.R[100] = 1.0f;
    Buf out = run(st, in);
    size_t at = 0;
    double best = 0;
    for (size_t i = 0; i < out.size(); ++i)
      if (std::fabs(out.L[i]) > best) best = std::fabs(out.L[i]), at = i;
    expect(at == 152, "multiband: FIR latency 52 samples");
  }
  {  // gain law T / (T + over * (1 - 1/ratio)) on a steady sine
    PluginState st = passAll(2);
    st.params[MbLowXover] = 20000;
    st.params[MbHighXover] = 20000;
    st.params[MbLowState] = 0;
    st.params[MbMidState] = 2;
    st.params[MbHighState] = 2;
    st.params[MbLowThresh] = -20;
    st.params[MbLowRatio] = 2;
    st.params[MbLowHard] = 1;
    st.params[MbLowAtk] = 0.0001f;
    Buf out = run(st, sine(1000, 0.5, 0.6));
    double want = 0.5 * 0.1 / (0.1 + (0.5 - 0.1) * 0.5);
    expect(near(peak(out, out.size() / 2, out.size()), want, want * 0.04), "multiband: band gain law");
  }
  {  // limiter keeps the output under full scale
    PluginState st = passAll(2);
    st.params[MbLimiter] = 1;
    st.params[MbOutput] = 9;
    Buf out = run(st, noise(0.9, 0.5));
    expect(peak(out) <= 1.0 && finiteAll(out), "multiband: output limiter");
  }
  flp::PresetResult r;
  if (flPreset("Fruity Multiband Compressor/Default.fst", r)) {
    const auto& p = r.state.params;
    expect(r.exact && near(p[MbLowThresh], -18.0, 0.01) && near(p[MbMidThresh], -15.25, 0.01) && near(p[MbLowRatio], 2.0, 1e-3),
           "multiband preset: thresholds / ratio");
    expect(near(p[MbLowXover], 300.3, 0.5) && near(p[MbHighXover], 9236.7, 2.0) && near(p[MbMidLowEdge], 300.3, 0.5) && p[MbEdgeMode] == 1,
           "multiband preset: band edges");
    expect(near(p[MbLowAtk], 0.01, 1e-6) && near(p[MbLowRel], 0.1, 1e-6) && near(p[MbLowHard], 0.8, 1e-3) && near(p[MbLowGain], 0.0, 1e-3),
           "multiband preset: attack / release / knee / gain");
  }
  if (flPreset("Fruity Multiband Compressor/Mosquite.fst", r))
    expect(r.state.params[MbMidState] == 2 && r.state.params[MbLowState] == 2 && r.state.params[MbHighState] == 0, "multiband preset: muted bands");
  if (flPreset("Fruity Multiband Compressor/Bass boost.fst", r))
    expect(r.state.params[MbHighState] == 1 && r.state.params[MbFilterType] == 0, "multiband preset: bypassed bands");
}


// ======================================================================= Transient shaper
Buf hits(double seconds) {  // decaying 180 Hz bursts every 250 ms
  Buf b(static_cast<size_t>(seconds * kSr));
  for (size_t i = 0; i < b.size(); ++i) {
    double t = std::fmod(static_cast<double>(i) / kSr, 0.25);
    double v = 0.6 * std::exp(-t / 0.06) * std::sin(2 * M_PI * 180 * t);
    b.L[i] = b.R[i] = static_cast<float>(v);
  }
  return b;
}
void testTransient() {
  PluginState st = state("gl.transient");
  {  // neutral: the complementary split sums back to the input
    Buf in = noise(0.4, 0.5);
    Buf out = run(st, in);
    double err = 0;
    for (size_t i = 0; i < in.size(); ++i) err = std::max(err, static_cast<double>(std::fabs(out.L[i] - in.L[i])));
    expect(err < 2e-3, "transient: neutral settings are transparent");
  }
  auto partDb = [&](const PluginState& s, double from, double to) {
    Buf out = run(s, hits(1.0));
    double e = 0;
    for (size_t k = 0; k < 4; ++k) {
      size_t a = static_cast<size_t>((k * 0.25 + from) * kSr), b = static_cast<size_t>((k * 0.25 + to) * kSr);
      e += rms(out, a, b);
    }
    return dsp::gainToDb(e / 4);
  };
  PluginState full = st;
  full.params[TrSplit] = 5;  // process (almost) everything
  double headN = partDb(full, 0.0, 0.006), tailN = partDb(full, 0.08, 0.2);
  PluginState up = full, down = full, rel = full;
  up.params[TrAttack] = 1;
  down.params[TrAttack] = -1;
  rel.params[TrSustain] = 1;
  expect(partDb(up, 0.0, 0.006) > headN + 1.0, "transient: +attack lifts the onset");
  expect(partDb(down, 0.0, 0.006) < headN - 1.0, "transient: -attack softens the onset");
  expect(partDb(rel, 0.08, 0.2) > tailN + 1.0, "transient: +release lifts the tail");
  {  // balance: full low weight with the high side off = low-passed dry signal only
    PluginState b = full;
    b.params[TrBalance] = -1;
    Buf out = run(b, sine(5000, 0.5, 0.3));
    expect(peak(out, out.size() / 2, out.size()) < 0.01, "transient: balance -100 % mutes the processed high part");
  }
  {  // effect switch off = bypass
    PluginState b = up;
    b.params[TrEnabled] = 0;
    Buf in = hits(0.3), out = run(b, in);
    expect(near(peak(out), peak(in), 1e-6), "transient: effect switch bypasses");
  }
  flp::PresetResult r;
  if (flPreset("Transient Processor/Overdiven.fst", r)) {
    const auto& p = r.state.params;
    expect(r.exact && near(p[TrAttack], 0.5, 1e-6) && near(p[TrDrive], 110.0 / 160, 1e-6) && near(p[TrSustain], 1.0, 1e-6),
           "transient preset: attack / drive / release");
    expect(near(p[TrOutput], 18.0 * 53 / 160 - 9, 1e-4) && near(p[TrSplit], 995.0 * (50.0 / 160) * (50.0 / 160) + 5, 1e-3) && p[TrAttackSpeed] == 2,
           "transient preset: gain / split / shape");
  }
}


// ======================================================================= Maximus → Mastering Dynamics
namespace dy = gl::packs::dynamics;
void testMaster() {
  {  // drawn-curve law: x = log9(2a + 1), out = (9^y − 1) / 2
    dy::DrawnCurve c;  // default: unity to 0 dB, flat above
    expect(near(dy::drawnCurveGain(c, 0.5), 1.0, 1e-9) && near(dy::drawnCurveGain(c, 2.0), 0.5, 1e-9), "master: default curve = limiter at 0 dBFS");
    c.n = 3, c.x[1] = 0.3, c.y[1] = 0.3, c.x[2] = 1, c.y[2] = 0.65, c.t[2] = 0;
    double a = 1.5, xv = std::log(2 * a + 1) / std::log(9.0);
    double y = 0.3 + (0.65 - 0.3) * (xv - 0.3) / 0.7;
    expect(near(dy::drawnCurveGain(c, a) * a, (std::pow(9.0, y) - 1) / 2, 1e-9), "master: output level from the curve");
  }
  {  // runtime: master band with a drawn 0 dB limit holds a +6 dB sine near full scale
    PluginState st = state("gl.master");
    for (int b = 0; b < 4; ++b) st.params[dy::msExt(b, dy::MxCurveMode)] = 1;
    for (int b = 0; b < 3; ++b) st.params[dy::msParam(b, dy::MbMode)] = 0;  // bands bypassed
    st.params[dy::msParam(3, dy::MbSatThresh)] = 0;
    st.params[dy::msParam(3, dy::MbSatCeil)] = 12;
    st.params[dy::msExt(3, dy::MxSatMode)] = 0;
    st.params[dy::msParam(3, dy::MbAttack)] = 1;
    st.params[dy::msExt(3, dy::MxEnvMode)] = 0;
    Buf out = run(st, sine(200, 2.0, 1.0));
    double pk = peak(out, out.size() / 2, out.size());
        expect(pk < 1.12 && pk > 0.85 && finiteAll(out), "master: drawn limiter curve holds the level near 0 dBFS");
  }
  flp::PresetResult r;
  if (flPreset("Maximus/Soundgoodizer presets/a.fst", r)) {
    const auto& p = r.state.params;
    expect(r.state.type == "gl.master", "maximus preset: imports as Mastering Dynamics");
    // program a: high band curve (0,0) (0.5,0.5 tension 0.14) (1,0.5)
    expect(p[dy::msExt(2, dy::MxCurveMode)] == 1 && p[dy::msExt(2, dy::MxPoints)] == 3 && near(p[dy::msExt(2, dy::MxPt0 + 3)], 0.5, 1e-6) &&
               near(p[dy::msExt(2, dy::MxPt0 + 5)], 0.14, 1e-6),
           "maximus preset: drawn curve points");
    expect(near(p[dy::msParam(0, dy::MbPre)], 20 * std::log10((std::pow(7.0, 1.264) - 1) / 6), 1e-3), "maximus preset: band pre gain law");
    expect(near(p[dy::MsLowFreq], 30 * std::pow(2.0, 19800 * 0.000138227748), 0.01), "maximus preset: low band edge law");
  }
  if (flPreset("Maximus/Fancy/Gate.fst", r)) {
    const auto& p = r.state.params;
    expect(p[dy::msExt(3, dy::MxPoints)] == 5 && near(p[dy::msExt(3, dy::MxPt0 + 4)], 0.0, 1e-6), "maximus preset: gate curve on the master band");
  }
}

// ======================================================================= Soundgoodizer → Enhance
void testEnhance() {
  PluginState st = state("gl.enhance");
  expect(st.params.size() == static_cast<size_t>(dy::EnCount), "enhance: program block present");
  {  // embedded program: band mix follows the amount
    PluginState p = st;
    p.params[dy::EnCustom] = 1;
    for (size_t k = 0; k < static_cast<size_t>(dy::kMsCount); ++k) p.params[static_cast<size_t>(dy::EnProg0) + k] = state("gl.master").params[k];
    p.params[dy::EnAmount] = 0;
    Buf a0 = run(p, sine(500, 0.3, 0.5));
    p.params[dy::EnAmount] = 1;
    Buf a1 = run(p, sine(500, 0.3, 0.5));
    expect(finiteAll(a0) && finiteAll(a1), "enhance: embedded program runs");
  }
  // Import from the FL render test project: Soundgoodizer with its shipped programs.
  std::string flp = kFl + "/Data/System/Render tests/Soundgoodizer.flp";
  if (std::filesystem::exists(flp)) {
    Song song;
    flp::Report rep;
    std::string err;
    bool ok = importFlp(flp, song, rep, &err, kFl);
    int found = 0, custom = 0;
    for (const auto& tr : song.mixer)
      for (const auto& sl : tr.slots)
        if (sl.plugin.type == "gl.enhance") {
          ++found;
          if (sl.plugin.params.size() > static_cast<size_t>(dy::EnCustom) && sl.plugin.params[dy::EnCustom] == 1) ++custom;
        }
    expect(ok && found > 0 && custom == found, "soundgoodizer render test: imports with the shipped programs embedded");
  }
}

// ======================================================================= FL render-test projects
void testRenderProjects() {
  struct Case {
    const char* file;
    const char* type;
  };
  const Case cases[] = {{"Fruity Limiter", "gl.limiter"}, {"Fruity Soft Clipper", "gl.clipper"},
                        {"Transient Processor", "gl.transient"}, {"Fruity Multiband Compressor", "gl.multiband"}, {"Maximus", "gl.master"}};
  for (const Case& c : cases) {
    std::string path = kFl + "/Data/System/Render tests/" + c.file + ".flp";
    if (!std::filesystem::exists(path)) continue;
    Song song;
    flp::Report rep;
    std::string err;
    bool ok = importFlp(path, song, rep, &err, kFl);
    int found = 0;
    bool sane = true;
    for (const auto& tr : song.mixer)
      for (const auto& sl : tr.slots)
        if (sl.plugin.type == c.type) {
          ++found;
          PluginState s = sl.plugin;
          normalizePluginState(s);
          auto fx = createEffect(s.type);
          fx->reset();
          Buf out = run(*fx, s, noise(0.5, 0.25));
          sane = sane && finiteAll(out) && peak(out) < 64;
        }
    if (!(ok && found > 0 && sane)) {
      std::printf("# %s ok=%d found=%d sane=%d err=%s\n", c.file, ok, found, sane, err.c_str());
      for (const auto& tr : song.mixer)
        for (const auto& sl : tr.slots)
          if (!sl.plugin.type.empty()) std::printf("#   slot %s\n", sl.plugin.type.c_str());
      for (const auto& [k, n] : rep.unmapped) std::printf("#   unmapped %s %d\n", k.c_str(), n);
    }
    std::string name = std::string("render test: ") + c.file + " imports and runs";
    expect(ok && found > 0 && sane, name.c_str());
  }
}

// ======================================================================= J-40: loud demo master
// "Asher Postman - Future House" renders pinned at 0 dBFS with a high RMS from
// its first second. That is the project's own master chain, decoded as saved:
// multiband compressor, maximizer, then the limiter with +5 dB gain, a +4 dB
// ceiling and the saturation threshold just below 0 dB. The saturation stage
// tops out at min(ceiling, 0 dBFS), so the output sits at 1.0 by design.
void testLoudDemoMaster() {
  PluginState st = state("gl.limiter");
  st.params[LimGain] = 4.97f;
  st.params[LimCeiling] = 4.04f;
  st.params[LimSaturation] = -0.0717f;
  Buf out = run(st, noise(0.9, 1.0));
  expect(finiteAll(out) && peak(out) <= 1.0 + 1e-6 && peak(out) > 0.97, "loud master: saturation pins the limiter output at 0 dBFS");
  st.params[LimSaturation] = 0;  // saturation off: the hard clip follows the (+4 dB) ceiling
  Buf hot = run(st, noise(0.9, 1.0));
  expect(peak(hot) > 1.2 && peak(hot) <= dsp::dbToGain(4.04) + 1e-5, "loud master: without saturation the output may exceed 0 dBFS up to the ceiling");

  const std::string flp = kFl + "/Data/Demo projects/Demo songs/Asher Postman - Future House.flp";
  if (!std::filesystem::exists(flp)) return;
  Song song;
  flp::Report rep;
  std::string err;
  const bool ok = importFlp(flp, song, rep, &err, kFl);
  expect(ok && !song.mixer.empty(), "loud master: demo imports");
  if (!ok || song.mixer.empty()) return;
  std::vector<std::string> chain;
  const PluginState* lim = nullptr;
  for (const auto& sl : song.mixer[0].slots) {
    if (sl.plugin.type.empty() || sl.plugin.type == "gl.notes") continue;
    chain.push_back(sl.plugin.type);
    if (sl.plugin.type == "gl.limiter") lim = &sl.plugin;
  }
  expect(chain == std::vector<std::string>{"gl.utility", "gl.multiband", "gl.master", "gl.limiter"}, "loud master: master chain decoded in order");
  expect(lim && lim->params.size() > static_cast<size_t>(LimSaturation) && std::fabs(lim->params[LimGain] - 4.97) < 0.1 &&
             std::fabs(lim->params[LimCeiling] - 4.04) < 0.1 && lim->params[LimSaturation] < 0 && lim->params[LimSaturation] > -0.2,
         "loud master: limiter gain +5 dB, ceiling +4 dB, saturation just below 0 dB (as saved)");
}

}  // namespace

int main() {
  testLimiter();
  testCompressor();
  testClipper();
  testMultiband();
  testTransient();
  testMaster();
  testEnhance();
  testRenderProjects();
  testLoudDemoMaster();
  return finish();
}
