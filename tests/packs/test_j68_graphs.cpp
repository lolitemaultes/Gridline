// Gridline — J-68: live editor graphics. The models behind the editor graphs
// must match what the engines actually do (EQ curve = measured response,
// reverb decay / reflections = measured impulse response, transfer curves =
// static engine gain), graphics recompute only when their inputs change, and
// the engines touched for the graphics stay allocation-free while processing.
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>
#include <vector>
#include "../../src/core/PluginInfo.h"
#include "../../src/core/PluginParams.h"
#include "../../src/engine/Delay3Ref.h"
#include "../../src/engine/Effects.h"
#include "../../src/engine/EqDesign.h"
#include "../../src/engine/GraphModels.h"
#include "../../src/engine/LiveGraph.h"
#include "../Check.h"

// ---- allocation counter (armed only around process() calls)
static std::atomic<bool> gArmed{false};
static std::atomic<long> gAllocs{0};
void* operator new(std::size_t n) {
  if (gArmed.load(std::memory_order_relaxed)) gAllocs.fetch_add(1, std::memory_order_relaxed);
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

using namespace gl;
using namespace gl::pp;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kSr = 48000;

ProcessContext ctx() {
  ProcessContext c;
  c.sampleRate = kSr;
  c.tempo = 120;
  return c;
}

void setp(PluginState& s, int k, double v) {
  if (static_cast<size_t>(k) >= s.params.size()) s.params.resize(static_cast<size_t>(k) + 1, 0.f);
  s.params[static_cast<size_t>(k)] = static_cast<float>(v);
}

// Runs L/R through fx in blocks of 256.
void run(Effect& fx, const PluginState& s, std::vector<float>& L, std::vector<float>& R) {
  ProcessContext c = ctx();
  for (size_t i = 0; i < L.size(); i += 256) {
    int n = static_cast<int>(std::min<size_t>(256, L.size() - i));
    fx.process(s, c, L.data() + i, R.data() + i, n);
  }
}

// Steady-state gain (dB) of a sine at hz through the effect.
double sineGainDb(Effect& fx, const PluginState& s, double hz, double amp = 0.25) {
  fx.reset();
  const size_t n = static_cast<size_t>(kSr * 1.0);
  std::vector<float> L(n), R(n);
  for (size_t i = 0; i < n; ++i) L[i] = R[i] = static_cast<float>(amp * std::sin(2 * kPi * hz * static_cast<double>(i) / kSr));
  run(fx, s, L, R);
  double ein = 0, eout = 0;
  for (size_t i = n / 2; i < n; ++i) {
    double x = amp * std::sin(2 * kPi * hz * static_cast<double>(i) / kSr);
    ein += x * x, eout += static_cast<double>(L[i]) * L[i];
  }
  return 10 * std::log10(eout / ein);
}

// Mono impulse response (left output) of `sec` seconds.
std::vector<float> impulse(Effect& fx, const PluginState& s, double sec) {
  fx.reset();
  std::vector<float> L(static_cast<size_t>(kSr * sec)), R(L.size());
  L[0] = R[0] = 1.f;
  run(fx, s, L, R);
  return L;
}

// T60 from a Schroeder backward integral, fitted between -5 and -25 dB.
double measureT60(const std::vector<float>& ir) {
  std::vector<double> e(ir.size() + 1, 0.0);
  for (size_t i = ir.size(); i-- > 0;) e[i] = e[i + 1] + static_cast<double>(ir[i]) * ir[i];
  const double e0 = e[0];
  size_t a = 0, b = 0;
  for (size_t i = 0; i < ir.size(); ++i) {
    double db = 10 * std::log10(std::max(1e-30, e[i] / e0));
    if (!a && db <= -5) a = i;
    if (db <= -25) {
      b = i;
      break;
    }
  }
  if (!a || b <= a) return -1;
  return 60.0 / 20.0 * static_cast<double>(b - a) / kSr;
}

void testMemo() {
  livegraph::Memo<std::vector<float>> m;
  int calls = 0;
  auto f = [&](std::vector<float>& v) { v.assign(4, 1.f), ++calls; };
  const uint64_t k1 = livegraph::Hasher().f(1.f).d(48000).value(), k2 = livegraph::Hasher().f(2.f).d(48000).value();
  for (int frame = 0; frame < 100; ++frame) m.get(k1, f);
  expect(calls == 1 && m.computes == 1, "graph memo: 100 frames with unchanged inputs compute once");
  m.get(k2, f);
  m.get(k2, f);
  expect(calls == 2, "graph memo: a parameter change recomputes once");
  m.get(k1, f);
  expect(calls == 3, "graph memo: changing back recomputes");
  expect(k1 != k2 && livegraph::Hasher().f(1.f).d(44100).value() != k1, "graph memo: hash covers params and sample rate");
  auto& a = livegraph::memo<int>(7, 1);
  auto& b = livegraph::memo<int>(7, 2);
  auto& c = livegraph::memo<int>(8, 1);
  expect(&a != &b && &a != &c && &a == &livegraph::memo<int>(7, 1), "graph memo store: one memo per instance and graphic");
}

void testEq() {
  PluginState s = defaultPluginState("gl.eq");
  setp(s, eqParam(1, EqType), 5), setp(s, eqParam(1, EqFreq), 120), setp(s, eqParam(1, EqGain), 5);
  setp(s, eqParam(3, EqType), 6), setp(s, eqParam(3, EqFreq), 1000), setp(s, eqParam(3, EqGain), -7);
  setp(s, eqParam(5, EqType), 7), setp(s, eqParam(5, EqFreq), 6000), setp(s, eqParam(5, EqGain), 4);
  auto fx = createEffect("gl.eq");
  double worst = 0;
  for (double hz : {60.0, 120.0, 400.0, 1000.0, 2500.0, 6000.0, 12000.0}) {
    double model = 0;
    for (int b = 0; b < kEqBands; ++b) {
      EqBandFilters f;
      designEqBand(s, b, kSr, f);
      model += dsp::gainToDb(eqBandMagnitude(f, hz, kSr));
    }
    model += eqParamOr(s, EqMain, 0);
    double meas = sineGainDb(*fx, s, hz);
    worst = std::max(worst, std::fabs(meas - model));
  }
  std::printf("# eq curve vs measured: worst %.4f dB\n", worst);
  expect(worst < 0.1, "parametric EQ curve equals the measured response within 0.1 dB");
}

PluginState fullReverb(int decay, int damp, int er, int wet) {
  PluginState s = defaultPluginState("gl.reverb");
  setp(s, RvbModel, 2);
  setp(s, RvbFLowCut, 19), setp(s, RvbFHighCut, 221), setp(s, RvbFPredelay, 0), setp(s, RvbFRoom, 60), setp(s, RvbFDiffusion, 100);
  setp(s, RvbFDecay, decay), setp(s, RvbFDamp, damp), setp(s, RvbFBass, 100), setp(s, RvbFCross, 500), setp(s, RvbFSep, 0);
  setp(s, RvbFDry, 0), setp(s, RvbFEr, er), setp(s, RvbFWet, wet), setp(s, RvbFModDepth, 0);
  return s;
}

void testReverb() {
  auto fx = createEffect("gl.reverb");
  {  // full model: the tail's T60 follows the decay control
    PluginState s = fullReverb(20, 221, 0, 128);
    graphmodel::ReverbView v = graphmodel::reverbView(s, kSr, 120);
    double t = measureT60(impulse(*fx, s, 6.0));
    std::printf("# full reverb: view T60 %.3f s, measured %.3f s\n", v.rt60, t);
    expect(std::fabs(v.decayKnob - 2.0) < 1e-9, "full reverb view: decay control 20 = 2.0 s");
    expect(t > 0 && std::fabs(t / v.rt60 - 1) < 0.15, "full reverb: measured T60 within 15% of the decay view");
    PluginState s2 = fullReverb(40, 221, 0, 128);
    double t2 = measureT60(impulse(*fx, s2, 10.0));
    expect(t2 > t * 1.6, "full reverb: doubling the decay lengthens the measured tail (view follows)");
  }
  {  // full model: reflections at the taps the view draws
    PluginState s = fullReverb(15, 221, 128, 0);
    graphmodel::ReverbView v = graphmodel::reverbView(s, kSr, 120);
    std::vector<float> ir = impulse(*fx, s, 0.5);
    double first = 1e9;  // the left output carries the first seven taps
    for (int e = 0; e < 7; ++e) first = std::min(first, v.erSec[static_cast<size_t>(e)]);
    size_t onset = 0;
    for (size_t i = 0; i < ir.size(); ++i)
      if (std::fabs(ir[i]) > 1e-4f) {
        onset = i;
        break;
      }
    std::printf("# full reverb: first reflection view %.1f samples, measured onset %zu\n", first * kSr, onset);
    expect(v.erCount == 15 && std::fabs(static_cast<double>(onset) - first * kSr) <= 3, "full reverb: first reflection where the view draws it");
    PluginState big = fullReverb(15, 221, 128, 0);
    setp(big, RvbFRoom, 100);
    graphmodel::ReverbView vb = graphmodel::reverbView(big, kSr, 120);
    expect(vb.erSec[0] > v.erSec[0] && vb.size > v.size, "full reverb view: a bigger room spreads the reflections");
  }
  {  // full model: damping shortens the 6 kHz decay in the view
    graphmodel::ReverbView open = graphmodel::reverbView(fullReverb(30, 221, 0, 128), kSr, 120);
    graphmodel::ReverbView damped = graphmodel::reverbView(fullReverb(30, 20, 0, 128), kSr, 120);
    expect(std::fabs(open.rt60High - open.rt60) < 1e-6 && damped.rt60High < damped.rt60 * 0.8, "full reverb view: damping shortens the high band");
  }
  {  // hall model: the knob's T60 is the measured T60
    PluginState s = defaultPluginState("gl.reverb");
    setp(s, RvbModel, 0), setp(s, RvbDecayTime, 2.0), setp(s, RvbDry, 0), setp(s, RvbEarly, 0), setp(s, RvbWet, 1);
    setp(s, RvbDampHz, 22100), setp(s, RvbDamp, 0), setp(s, RvbBassMult, 1), setp(s, RvbModDepth, 0), setp(s, RvbPreDelay, 0);
    setp(s, RvbLowCut, 20), setp(s, RvbHighCut, 20000), setp(s, RvbColor, 0);
    graphmodel::ReverbView v = graphmodel::reverbView(s, kSr, 120);
    double t = measureT60(impulse(*fx, s, 6.0));
    std::printf("# hall reverb: view T60 %.3f s, measured %.3f s\n", v.rt60, t);
    expect(t > 0 && std::fabs(t / v.rt60 - 1) < 0.2, "hall reverb: measured T60 within 20% of the decay view");
  }
  {  // the view follows the active model's own controls (the full model's, not the hall's)
    PluginState s = fullReverb(15, 221, 128, 128);
    setp(s, RvbDecayTime, 9.0);
    graphmodel::ReverbView a = graphmodel::reverbView(s, kSr, 120);
    setp(s, RvbFDecay, 120);
    graphmodel::ReverbView b = graphmodel::reverbView(s, kSr, 120);
    expect(std::fabs(a.decayKnob - 1.5) < 1e-9 && std::fabs(b.decayKnob - 12.0) < 1e-9 && b.rt60 > a.rt60 * 7, "full reverb view: decay follows the full model's control");
  }
}

void testDelay3() {
  for (double thr : {0.2, 0.6, 1.0})
    for (double hard : {0.0, 0.5, 1.0})
      for (double sym : {-0.5, 0.0, 0.5}) {
        const float tn = static_cast<float>(thr);
        const d3ref::Knee k = d3ref::kneeFor(tn, static_cast<float>(hard), static_cast<float>(sym));
        double worst = 0;
        for (int i = -40; i <= 40; ++i) {
          double x = i / 20.0;
          double expectY = static_cast<double>(d3ref::kneeApply(k, static_cast<float>(x))) * d3ref::makeUp(tn);
          worst = std::max(worst, std::fabs(graphmodel::delay3Transfer(1, thr, hard, sym, x) - expectY));
        }
        if (worst > 1e-6) std::printf("# delay3 sat mismatch thr %.1f hard %.1f sym %.1f: %g\n", thr, hard, sym, worst);
        expect(worst <= 1e-6, "delay3 saturate curve = the engine knee law");
      }
  // Limit: the envelope limiter's steady state for a constant input (engine limiter, run to convergence).
  const float tn = 0.6f, lim = std::pow(10.f, d3ref::threshDb(tn) / 20.f) * d3ref::makeUp(tn);
  for (double x : {0.01, 0.1, 0.5, 1.0}) {
    // y = x / max(1, env) with env → |x| / lim  (input divided by the limit level, output scaled back)
    double steady = x / std::max(1.0, x / lim) * 1.0;
    expect(std::fabs(graphmodel::delay3Transfer(0, tn, 0, 0, x) - steady) < 1e-6, "delay3 limit curve = steady-state limiter gain");
  }
  // Monotonic and odd for symmetric settings.
  bool odd = true, mono = true;
  double prev = -1e9;
  for (int i = -50; i <= 50; ++i) {
    double y = graphmodel::delay3Transfer(1, 0.7, 0.3, 0, i / 25.0);
    odd = odd && std::fabs(y + graphmodel::delay3Transfer(1, 0.7, 0.3, 0, -i / 25.0)) < 1e-6;
    mono = mono && y >= prev - 1e-9;
    prev = y;
  }
  expect(odd && mono, "delay3 saturate curve: odd and monotonic at zero symmetry");
}

void testLimiterComp() {
  auto fx = createEffect("gl.limiter");
  for (double ratio : {2.0, 4.0, 0.5})
    for (double knee : {0.0, 6.0}) {
      PluginState s = defaultPluginState("gl.limiter");
      setp(s, LimGain, 0), setp(s, LimCeiling, 0), setp(s, LimSaturation, 0), setp(s, LimGateThresh, -80);
      setp(s, LimCompThresh, -30), setp(s, LimCompRatio, ratio), setp(s, LimCompKnee, knee), setp(s, LimCompRms, 0.001);
      double worst = 0;
      for (double amp : {0.01, 0.05, 0.1, 0.3}) {
        const double rmsDb = 20 * std::log10(amp / std::sqrt(2.0));
        const double model = graphmodel::limiterCompGainDb(s, rmsDb);
        if (amp * std::pow(10.0, model / 20) > 0.9) continue;  // the ceiling takes over (not the compressor curve)
        const double meas = sineGainDb(*fx, s, 1000, amp);
        worst = std::max(worst, std::fabs(meas - model));
      }
      if (ratio == 4.0 && knee == 0.0) expect(std::fabs(graphmodel::limiterCompGainDb(s, -10) + 15) < 1e-6, "limiter compressor curve: 20 dB over at 4:1 = -15 dB");
      std::printf("# limiter compressor ratio %.1f knee %.0f: worst |curve - measured| %.3f dB\n", ratio, knee, worst);
      expect(worst < 0.25, "limiter compressor curve equals the static engine gain");
    }
}

void testLowLift() {
  auto fx = createEffect("gl.lowlift");
  {  // legacy engine with the harmonics off: the drawn total is the measured response
    PluginState s = defaultPluginState("gl.lowlift");
    setp(s, LlModel, 0), setp(s, LlHarmonics, 0), setp(s, LlDiffuse, 0), setp(s, LlBoost, 0.4), setp(s, LlDrive, 0);
    setp(s, LlLowGain, 6), setp(s, LlHighGain, -3), setp(s, LlCrossover, 150), setp(s, LlLowCut, 30), setp(s, LlMix, 1);
    double worst = 0;
    for (double hz : {40.0, 80.0, 150.0, 300.0, 1000.0, 5000.0}) {
      double model = 20 * std::log10(graphmodel::lowLiftResponse(s, kSr, hz).total);
      worst = std::max(worst, std::fabs(sineGainDb(*fx, s, hz) - model));
    }
    std::printf("# low lifter (legacy) response vs measured: worst %.4f dB\n", worst);
    expect(worst < 0.1, "low lifter curve equals the measured response (legacy engine)");
  }
  {  // reference engine, small signals (below the normaliser's floor the shaper is linear)
    PluginState s = defaultPluginState("gl.lowlift");
    setp(s, LlModel, 1), setp(s, LlOn, 1), setp(s, LlLookahead, 1), setp(s, LlMultiband, 0), setp(s, LlDiffuse, 0), setp(s, LlMix, 1);
    setp(s, LlLowDb, 3), setp(s, LlHighDb, -6), setp(s, LlSlopeDb, -20), setp(s, LlPreDb, 0), setp(s, LlXoverHz, 100), setp(s, LlLowCutHz, 25);
    double worst = 0;
    for (double hz : {50.0, 100.0, 300.0, 2000.0}) {
      double model = 20 * std::log10(graphmodel::lowLiftResponse(s, kSr, hz).total);
      worst = std::max(worst, std::fabs(sineGainDb(*fx, s, hz, 0.01) - model));
    }
    std::printf("# low lifter (reference) small-signal response vs measured: worst %.4f dB\n", worst);
    expect(worst < 0.25, "low lifter curve equals the measured small-signal response (reference engine)");
  }
}

void testMultiband() {
  auto fx = createEffect("gl.multiband");
  for (int ft = 0; ft < 3; ++ft)
    for (int band = 0; band < 3; ++band) {
      PluginState s = defaultPluginState("gl.multiband");
      setp(s, MbFilterType, ft), setp(s, MbLimiter, 0), setp(s, MbOutput, 0), setp(s, MbLowXover, 250), setp(s, MbHighXover, 4000), setp(s, MbEdgeMode, 0);
      for (int b = 0; b < 3; ++b) {
        setp(s, MbLowState + b, b == band ? 1 : 2);  // the band under test bypassed (no compression), the others muted
        setp(s, MbLowGain + b * 3, 0);
      }
      const double hz[5] = {60, 250, 1000, 4000, 12000};
      double lo[5], mi[5], hi[5];
      graphmodel::multibandBandResponses(s, kSr, hz, 5, lo, mi, hi);
      const double* g = band == 0 ? lo : band == 1 ? mi : hi;
      double worst = 0;
      for (int i = 0; i < 5; ++i) {
        const double model = 20 * std::log10(std::max(1e-6, g[i]));
        if (model < -40) continue;  // deep stop band: below the measurement's precision
        worst = std::max(worst, std::fabs(sineGainDb(*fx, s, hz[i]) - model));
      }
      std::printf("# multiband filter type %d band %d: worst %.4f dB\n", ft, band, worst);
      expect(worst < 0.1, "multiband band shape equals the measured band response");
    }
}

void testFilter() {
  auto fx = createEffect("gl.filter");
  double worst = 0;
  for (int mode = 0; mode <= 6; ++mode)
    for (int steep = 0; steep < 2; ++steep)
      for (int blend = 0; blend < 2; ++blend) {
        PluginState s = defaultPluginState("gl.filter");
        setp(s, FltModel, 0), setp(s, FltMode, mode), setp(s, FltSlope, steep), setp(s, FltBlend, blend), setp(s, FltCutoff, 1200);
        setp(s, FltReso, 0.6), setp(s, FltGain, 6), setp(s, FltMix, 0.8), setp(s, FltDcBlock, 0);
        setp(s, FltLPLevel, 0.7), setp(s, FltBPLevel, 0.5), setp(s, FltHPLevel, 0.3);
        for (double hz : {200.0, 900.0, 1200.0, 1600.0, 6000.0}) {
          double model = graphmodel::filterResponseDb(s, kSr, hz);
          if (model < -40) continue;
          worst = std::max(worst, std::fabs(sineGainDb(*fx, s, hz) - model));
        }
      }
  {  // the measurement itself is not trivial: a steep low-pass really cuts 6 kHz
    PluginState s = defaultPluginState("gl.filter");
    setp(s, FltModel, 0), setp(s, FltMode, 0), setp(s, FltSlope, 1), setp(s, FltBlend, 0), setp(s, FltCutoff, 1200), setp(s, FltMix, 1);
    const double meas = sineGainDb(*fx, s, 6000);
    std::printf("# filter steep low-pass at 6 kHz: measured %.2f dB, curve %.2f dB\n", meas, graphmodel::filterResponseDb(s, kSr, 6000));
    expect(meas < -40, "filter measurement: a steep low-pass cuts two octaves up by > 40 dB");
  }
  std::printf("# filter curve vs measured (7 modes x slope x blend): worst %.4f dB\n", worst);
  expect(worst < 0.1, "filter curve equals the measured response");
}

void testNoAllocation() {
  struct Case {
    const char* type;
    PluginState st;
  };
  std::vector<Case> cases;
  cases.push_back({"gl.reverb", fullReverb(30, 40, 128, 128)});
  {
    PluginState s = defaultPluginState("gl.delay");
    setp(s, DlyModel, 3);
    setp(s, DlyD3Raw0 + d3ref::PDistType, 1), setp(s, DlyD3Raw0 + d3ref::PThresh, 0.7);
    cases.push_back({"gl.delay", s});
  }
  cases.push_back({"gl.eq", defaultPluginState("gl.eq")});
  for (Case& c : cases) {
    auto fx = createEffect(c.type);
    std::vector<float> L(4096), R(4096);
    for (size_t i = 0; i < L.size(); ++i) L[i] = R[i] = static_cast<float>(0.3 * std::sin(i * 0.05));
    run(*fx, c.st, L, R);  // warm-up (first block may size buffers)
    gAllocs = 0;
    gArmed = true;
    run(*fx, c.st, L, R);
    gArmed = false;
    std::string name = std::string(c.type) + ": no allocation while processing";
    expect(gAllocs.load() == 0, name.c_str());
  }
}

}  // namespace

int main() {
  testMemo();
  testEq();
  testReverb();
  testDelay3();
  testLimiterComp();
  testLowLift();
  testMultiband();
  testFilter();
  testNoAllocation();
  return finish();
}
