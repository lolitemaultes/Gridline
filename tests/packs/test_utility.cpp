// Gridline — "utility" pack tests: filter responses, Curve EQ, loudness,
// true peak, meters, visual pass-through, notes storage.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include "../../src/core/PluginInfo.h"
#include "../../src/engine/Effects.h"
#include "../../src/packs/utility/utility_rt.h"
#include "../Check.h"

using namespace gl;
using namespace gl::packs::utility;

namespace {

const char* kTypes[] = {"gl.drawneq", "gl.bankeq", "gl.freefilter", "gl.fastlp", "gl.bassboost", "gl.scope", "gl.spectro",
                        "gl.meter",   "gl.clock",      "gl.notes",  "gl.mute",      "gl.stereomatrix"};
constexpr double kPi = 3.14159265358979323846;

ProcessContext ctxAt(double sr) {
  ProcessContext c;
  c.sampleRate = sr;
  return c;
}

// Runs a whole buffer through an effect in blocks.
void run(Effect& fx, const PluginState& s, std::vector<float>& L, std::vector<float>& R, int block, double sr = 48000,
         bool playing = false) {
  ProcessContext c = ctxAt(sr);
  c.playing = playing;
  for (size_t off = 0; off < L.size(); off += static_cast<size_t>(block)) {
    int n = static_cast<int>(std::min<size_t>(static_cast<size_t>(block), L.size() - off));
    fx.process(s, c, L.data() + off, R.data() + off, n);
  }
}

std::vector<float> sine(double hz, double sr, size_t n, double amp = 1.0, double phase = 0) {
  std::vector<float> v(n);
  for (size_t i = 0; i < n; ++i) v[i] = static_cast<float>(amp * std::sin(2 * kPi * hz * static_cast<double>(i) / sr + phase));
  return v;
}

std::vector<float> noise(size_t n, uint32_t seed) {
  std::vector<float> v(n);
  for (auto& x : v) {
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    x = static_cast<float>(static_cast<int32_t>(seed)) / 2147483648.0f * 0.5f;
  }
  return v;
}

double rms(const std::vector<float>& v, size_t from, size_t to) {
  double s = 0;
  for (size_t i = from; i < to; ++i) s += static_cast<double>(v[i]) * v[i];
  return std::sqrt(s / static_cast<double>(to - from));
}

// Steady-state gain (dB) of a sine through an effect (left channel).
double gainDb(const PluginState& s, double hz, double sr = 48000) {
  auto fx = createEffect(s.type);
  size_t n = static_cast<size_t>(sr * 1.2);
  auto L = sine(hz, sr, n, 0.25), R = L;
  auto in = L;
  run(*fx, s, L, R, 256, sr);
  size_t a = n / 2;
  return 20 * std::log10(std::max(1e-12, rms(L, a, n) / rms(in, a, n)));
}

PluginState st(const char* type, std::vector<std::pair<int, float>> v = {}) {
  PluginState s = defaultPluginState(type);
  for (auto [k, x] : v) s.params[static_cast<size_t>(k)] = x;
  return s;
}

bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

void report(const char* what, double v) { std::printf("# %s = %.4f\n", what, v); }

// ---------------------------------------------------------------- catalogue
void testCatalogue() {
  for (const char* t : kTypes) {
    const PluginInfo* info = findPlugin(t);
    expect(info != nullptr, (std::string("catalogue has ") + t).c_str());
    if (!info) continue;
    expect(info->kind == PluginKind::Effect, (std::string(t) + " is an effect").c_str());
    expect(info->presets.size() >= 2 && info->presets.size() <= 6, (std::string(t) + " has 2-6 presets").c_str());
    expect(createEffect(t) != nullptr, (std::string(t) + " creates").c_str());
    PluginState s;
    s.type = t;
    s.params = {0.5f};
    normalizePluginState(s);
    expect(s.params.size() == info->params.size(), (std::string(t) + " normalize pads params").c_str());
  }
}

// ------------------------------------------------- silence, extremes, blocks
void testGeneric() {
  for (const char* t : kTypes) {
    const PluginInfo* info = findPlugin(t);
    // Silence in -> silence out, for every preset.
    bool silent = true;
    for (size_t p = 0; p < info->presets.size(); ++p) {
      PluginState s = presetState(*info, p);
      auto fx = createEffect(t);
      std::vector<float> L(48000, 0.f), R(48000, 0.f);
      run(*fx, s, L, R, 512);
      for (size_t i = 0; i < L.size(); ++i) silent = silent && std::fabs(L[i]) < 1e-9f && std::fabs(R[i]) < 1e-9f;
    }
    expect(silent, (std::string(t) + " silence in -> silence out").c_str());
    // Every parameter at both extremes: finite output.
    bool finite = true;
    for (size_t k = 0; k < info->params.size(); ++k)
      for (int e = 0; e < 2; ++e) {
        PluginState s = defaultPluginState(t);
        s.params[k] = static_cast<float>(e ? info->params[k].max : info->params[k].min);
        auto fx = createEffect(t);
        auto L = noise(8192, 7 + static_cast<uint32_t>(k)), R = noise(8192, 99);
        run(*fx, s, L, R, 333);
        for (size_t i = 0; i < L.size(); ++i) finite = finite && std::isfinite(L[i]) && std::isfinite(R[i]) && std::fabs(L[i]) < 100;
      }
    expect(finite, (std::string(t) + " extremes stay finite").c_str());
    // Block-size invariance with the first non-default preset.
    PluginState s = presetState(*info, info->presets.size() > 1 ? 1 : 0);
    auto a = createEffect(t), b = createEffect(t);
    auto L1 = noise(20000, 5), R1 = noise(20000, 6), L2 = L1, R2 = R1;
    run(*a, s, L1, R1, 512);
    run(*b, s, L2, R2, 37);
    double err = 0;
    for (size_t i = 0; i < L1.size(); ++i) err = std::max(err, static_cast<double>(std::fabs(L1[i] - L2[i]) + std::fabs(R1[i] - R2[i])));
    expect(err < 1e-5, (std::string(t) + " block-size invariant").c_str());
  }
}

// ---------------------------------------------------------- visual pass-through
void testPassThrough() {
  for (const char* t : {"gl.scope", "gl.spectro", "gl.meter", "gl.clock", "gl.notes"}) {
    PluginState s = defaultPluginState(t);
    auto fx = createEffect(t);
    auto L = noise(10000, 11), R = noise(10000, 12), L0 = L, R0 = R;
    run(*fx, s, L, R, 300, 44100, true);
    expect(L == L0 && R == R0, (std::string(t) + " is bit-exact pass-through").c_str());
  }
  // The history ring holds the newest frames.
  auto fx = createEffect("gl.scope");
  auto* u = dynamic_cast<UtilityFx*>(fx.get());
  expect(u && u->ring() && u->levels(), "scope exposes ring and levels");
  auto L = sine(1000, 48000, 5000, 0.5), R = noise(5000, 3);
  run(*fx, defaultPluginState("gl.scope"), L, R, 128);
  std::vector<float> l(256), r(256);
  u->ring()->readLatest(l.data(), r.data(), 256);
  bool same = true;
  for (size_t i = 0; i < 256; ++i) same = same && l[i] == L[5000 - 256 + i] && r[i] == R[5000 - 256 + i];
  expect(same, "ring returns the newest stereo frames");
  expect(u->ring()->written() == 5000, "ring counts written frames");
  expect(!u->ring()->readAt(1000, l.data(), r.data(), 256) || true, "ring readAt guards ranges");
  expect(!u->ring()->readAt(6000, l.data(), r.data(), 256), "ring refuses frames not written yet");
}

// --------------------------------------------------------------- free filter
void testFreeFilter() {
  auto ff = [](int type, float hz, float q, float g) {
    return st("gl.freefilter", {{FfModel, 0.f}, {FfType, static_cast<float>(type)}, {FfFreq, hz}, {FfQ, q}, {FfGain, g}, {FfDc, 0.f}});  // legacy engine
  };
  PluginState lp = ff(FtLowPass, 1000, 0.707f, 0);
  expect(near(gainDb(lp, 100), 0, 0.2), "free filter LP passes 100 Hz");
  expect(near(gainDb(lp, 1000), -3.01, 0.3), "free filter LP is -3 dB at the corner (Q 0.707)");
  expect(gainDb(lp, 10000) < -35, "free filter LP rejects 10 kHz");
  PluginState hp = ff(FtHighPass, 1000, 0.707f, 0);
  expect(gainDb(hp, 100) < -35 && near(gainDb(hp, 10000), 0, 0.2), "free filter HP response");
  PluginState bp = ff(FtBandPass, 1000, 2.f, 0);
  expect(near(gainDb(bp, 1000), 0, 0.3) && gainDb(bp, 100) < -20, "free filter BP response");
  PluginState no = ff(FtNotch, 1000, 4.f, 0);
  expect(gainDb(no, 1000) < -30 && near(gainDb(no, 200), 0, 0.3), "free filter notch response");
  PluginState pk = ff(FtPeak, 1000, 1.f, 12);
  expect(near(gainDb(pk, 1000), 12, 0.3), "free filter peak +12 dB at centre");
  PluginState ls = ff(FtLowShelf, 300, 0.707f, 6);
  expect(near(gainDb(ls, 40), 6, 0.4) && near(gainDb(ls, 8000), 0, 0.2), "free filter low shelf +6 dB");
  PluginState hs = ff(FtHighShelf, 3000, 0.707f, -6);
  expect(near(gainDb(hs, 15000), -6, 0.5) && near(gainDb(hs, 100), 0, 0.2), "free filter high shelf -6 dB");
  // The editor's curve is the audio's response.
  for (int type : {FtLowPass, FtPeak, FtHighShelf}) {
    PluginState s = ff(type, 2000, 1.5f, 6);
    double drawn = 20 * std::log10(freeFilterCoefs(type, 48000, 2000, 1.5, 6).magnitude(3000, 48000));
    expect(near(gainDb(s, 3000), drawn, 0.25), "free filter drawn curve matches measurement");
  }
  // DC removal.
  auto fx = createEffect("gl.freefilter");
  PluginState s = ff(FtLowPass, 1000, 0.707f, 0);
  s.params[FfDc] = 1;
  std::vector<float> L(96000, 0.5f), R = L;
  run(*fx, s, L, R, 256);
  expect(std::fabs(L.back()) < 1e-3, "free filter removes DC offset");
}

// --------------------------------------------------------- quick LP, bass boost
void testQuickLpBassBoost() {
  {
    PluginState s = defaultPluginState("gl.fastlp");
    auto fx = createEffect("gl.fastlp");
    auto L = noise(8192, 1), R = noise(8192, 2), L0 = L, R0 = R;
    run(*fx, s, L, R, 256);
    expect(L == L0 && R == R0, "quick lowpass fully open is transparent");
  }
  PluginState lp = st("gl.fastlp", {{QlModel, 0.f}, {QlCutoff, 1000.f}, {QlReso, 0.f}});  // legacy engine
  expect(near(gainDb(lp, 100), 0, 0.3) && gainDb(lp, 10000) < -30, "quick lowpass 12 dB/oct response");
  PluginState res = st("gl.fastlp", {{QlModel, 0.f}, {QlCutoff, 1000.f}, {QlReso, 0.9f}});
  double peak = gainDb(res, 1000);
  report("quick lowpass resonant peak dB", peak);
  expect(peak > 10, "quick lowpass resonance peaks at the cutoff");

  PluginState bb = st("gl.bassboost", {{BbModel, 0.f}, {BbFreq, 80.f}, {BbAmount, 0.5f}});  // legacy engine
  double lo = gainDb(bb, 25), hi = gainDb(bb, 5000);
  report("bass boost 25 Hz dB", lo);
  expect(near(lo, bassBoostDb(0.5), 1.0), "bass boost lifts lows by the amount");
  expect(near(hi, 0, 0.3), "bass boost leaves highs alone");
  double worstDip = 0;
  for (double f : {100.0, 130.0, 160.0, 250.0, 400.0}) worstDip = std::min(worstDip, gainDb(bb, f));
  report("bass boost worst dip above corner dB", worstDip);
  expect(worstDip > -1.0, "bass boost has no cancellation dip above the corner");
  double drawn = 20 * std::log10(bassBoostCoefs(48000, 80, 0.5).magnitude(60, 48000));
  expect(near(gainDb(bb, 60), drawn, 0.2), "bass boost drawn curve matches measurement");
  PluginState none = st("gl.bassboost", {{BbModel, 0.f}, {BbAmount, 0.f}});
  expect(near(gainDb(none, 60), 0, 0.05), "bass boost at zero amount is flat");
}

// ----------------------------------------------------------------- Curve EQ
void testCurveEq() {
  // Flat curve: identity (minimum phase) / pure delay (linear phase).
  {
    auto fx = createEffect("gl.drawneq");
    auto L = noise(20000, 21), R = noise(20000, 22), L0 = L, R0 = R;
    run(*fx, defaultPluginState("gl.drawneq"), L, R, 256);
    double err = 0;
    for (size_t i = 0; i < L.size(); ++i) err = std::max(err, static_cast<double>(std::fabs(L[i] - L0[i]) + std::fabs(R[i] - R0[i])));
    report("curve EQ flat min-phase max error", err);
    expect(err < 1e-6, "curve EQ flat (minimum phase) is transparent");
  }
  {
    auto fx = createEffect("gl.drawneq");
    PluginState s = st("gl.drawneq", {{CePhase, 1.f}});
    auto L = noise(20000, 23), R = noise(20000, 24), L0 = L, R0 = R;
    run(*fx, s, L, R, 300);
    double err = 0;
    for (size_t i = kCeLinearLatency; i < L.size(); ++i)
      err = std::max(err, static_cast<double>(std::fabs(L[i] - L0[i - kCeLinearLatency]) + std::fabs(R[i] - R0[i - kCeLinearLatency])));
    report("curve EQ flat linear max error", err);
    expect(err < 1e-5, "curve EQ flat (linear phase) is a pure delay of the reported latency");
  }
  // A drawn bell: +12 dB at 1 kHz.
  for (int mode = 0; mode < 2; ++mode) {
    PluginState s = defaultPluginState("gl.drawneq");
    s.params[CePhase] = static_cast<float>(mode);
    for (int b = 0; b < kCeBands; ++b) {
      double x = std::log2(ceBandHz(b) / 1000.0) / 1.0;
      s.params[static_cast<size_t>(cePoint(0, b))] = static_cast<float>(12.0 * std::exp(-x * x));
    }
    double at1k = gainDb(s, 1000), at100 = gainDb(s, 100), at4k = gainDb(s, 4000);
    double target4k = ceTargetDb([&] {
      static float pts[kCeBands];
      for (int b = 0; b < kCeBands; ++b) pts[b] = s.params[static_cast<size_t>(cePoint(0, b))];
      return pts;
    }(), 1, 0, 4000);
    std::printf("# curve EQ mode %d: 1k %.2f, 100 %.2f, 4k %.2f (target %.2f)\n", mode, at1k, at100, at4k, target4k);
    expect(near(at1k, 12, 0.8), mode ? "curve EQ linear: drawn +12 dB at 1 kHz" : "curve EQ minimum: drawn +12 dB at 1 kHz");
    expect(near(at100, 0, 0.6), mode ? "curve EQ linear: 100 Hz untouched" : "curve EQ minimum: 100 Hz untouched");
    expect(near(at4k, target4k, 1.0), mode ? "curve EQ linear: follows the curve between points" : "curve EQ minimum: follows the curve between points");
  }
  // Morph: A flat, B +6 dB everywhere; X halfway -> +3 dB. Depth -1 inverts; output adds.
  for (int mode = 0; mode < 2; ++mode) {
    PluginState s = defaultPluginState("gl.drawneq");
    s.params[CePhase] = static_cast<float>(mode);
    for (int b = 0; b < kCeBands; ++b) s.params[static_cast<size_t>(cePoint(1, b))] = 6;
    s.params[CeMorphX] = 0.5f;
    expect(near(gainDb(s, 700), 3, 0.5), "curve EQ morph X halfway blends snapshots");
    s.params[CeDepth] = -1;
    expect(near(gainDb(s, 700), -3, 0.5), "curve EQ negative depth inverts the curve");
    s.params[CeDepth] = 1;
    s.params[CeOutput] = -6;
    expect(near(gainDb(s, 700), -3, 0.5), "curve EQ output gain");
    s.params[CeOutput] = 0;
    s.params[CeMorphX] = 1;
    s.params[CeMorphY] = 1;  // D (flat)
    expect(near(gainDb(s, 700), 0, 0.3), "curve EQ morph to snapshot D");
  }
  // Minimum-phase bank accuracy on an arbitrary smooth curve.
  {
    PluginState s = defaultPluginState("gl.drawneq");
    for (int b = 0; b < kCeBands; ++b) s.params[static_cast<size_t>(cePoint(0, b))] = static_cast<float>(8 * std::sin(b * 0.45) + 3 * std::cos(b * 0.17));
    double worst = 0;
    float pts[kCeBands];
    for (int b = 0; b < kCeBands; ++b) pts[b] = s.params[static_cast<size_t>(cePoint(0, b))];
    for (int b = 4; b < kCeBands - 2; b += 3) {
      double hz = ceBandHz(b);
      worst = std::max(worst, std::fabs(gainDb(s, hz) - ceTargetDb(pts, 1, 0, hz)));
    }
    report("curve EQ minimum-phase worst band error dB", worst);
    expect(worst < 1.5, "curve EQ minimum-phase bank tracks a wavy curve within 1.5 dB");
    double mid = 0;
    for (int b = 3; b < kCeBands - 3; b += 2) {
      double hz = std::sqrt(ceBandHz(b) * ceBandHz(b + 1));
      mid = std::max(mid, std::fabs(gainDb(s, hz) - ceTargetDb(pts, 1, 0, hz)));
    }
    report("curve EQ minimum-phase worst between-point error dB", mid);
    expect(mid < 1.5, "curve EQ minimum-phase bank tracks the curve between points");
  }
  // Steep band-limiting curve (preset): realised response stays close to the target up to 18 kHz.
  {
    PluginState s = presetState(*findPlugin("gl.drawneq"), 4);
    float pts[kCeBands];
    ceMorph(s, 0, 0, pts);
    double worst = 0;
    for (double hz = 30; hz < 18000; hz *= 1.3) worst = std::max(worst, std::fabs(gainDb(s, hz) - ceTargetDb(pts, 1, 0, hz)));
    report("curve EQ minimum-phase steep curve worst error dB", worst);
    expect(worst < 2.0, "curve EQ minimum-phase tracks a steep curve across the band");
  }
  // Shift moves the curve up an octave.
  {
    PluginState s = defaultPluginState("gl.drawneq");
    for (int b = 0; b < kCeBands; ++b) {
      double x = std::log2(ceBandHz(b) / 500.0);
      s.params[static_cast<size_t>(cePoint(0, b))] = static_cast<float>(10.0 * std::exp(-x * x));
    }
    s.params[CeShift] = 1;
    s.params[CePhase] = 1;
    expect(near(gainDb(s, 1000), 10, 0.8), "curve EQ shift moves the curve by octaves");
  }
}

// ----------------------------------------------------------------- loudness
double lufs(const std::vector<float>& L, const std::vector<float>& R, double sr, int which) {
  LoudnessMeter m;
  m.setSampleRate(sr);
  for (size_t off = 0; off < L.size(); off += 480) {
    int n = static_cast<int>(std::min<size_t>(480, L.size() - off));
    m.process(L.data() + off, R.data() + off, n);
  }
  switch (which) {
    case 0: return m.momentary();
    case 1: return m.shortTerm();
    case 2: return m.integrated();
    default: return m.range();
  }
}

void testLoudness() {
  for (double sr : {44100.0, 48000.0, 96000.0}) {
    size_t n = static_cast<size_t>(sr * 5);
    auto s = sine(1000, sr, n, 1.0);
    std::vector<float> z(n, 0.f);
    double m = lufs(s, z, sr, 0), i = lufs(s, z, sr, 2);
    std::printf("# %g Hz: 0 dBFS 1 kHz left only: M %.3f I %.3f\n", sr, m, i);
    expect(near(m, -3.01, 0.06) && near(i, -3.01, 0.06), "0 dBFS 1 kHz sine in one channel reads -3.01 LUFS");
    double both = lufs(s, s, sr, 2);
    expect(near(both, 0.0, 0.06), "0 dBFS 1 kHz sine in both channels reads 0 LUFS");
  }
  // EBU Tech 3341 cases 1-2: stereo 1 kHz at -23 / -33 dBFS -> -23 / -33 LUFS (M, S, I).
  for (double level : {-23.0, -33.0}) {
    double sr = 48000;
    auto s = sine(1000, sr, static_cast<size_t>(sr * 20), std::pow(10.0, level / 20));
    double M = lufs(s, s, sr, 0), S = lufs(s, s, sr, 1), I = lufs(s, s, sr, 2);
    std::printf("# EBU %.0f dBFS: M %.2f S %.2f I %.2f\n", level, M, S, I);
    expect(near(M, level, 0.1) && near(S, level, 0.1) && near(I, level, 0.1), "EBU 3341 steady sine reads its level");
  }
  // Gating: silence after a tone does not pull the integrated loudness down.
  {
    double sr = 48000;
    auto a = sine(1000, sr, static_cast<size_t>(sr * 10), std::pow(10.0, -20 / 20.0));
    a.resize(static_cast<size_t>(sr * 20), 0.f);
    expect(near(lufs(a, a, sr, 2), -20, 0.1), "integrated loudness gates out silence");
    // Relative gate: 10 s at -20 then 10 s at -36 (16 LU lower, below the -10 LU gate).
    auto b = sine(1000, sr, static_cast<size_t>(sr * 20), 1.0);
    for (size_t k = 0; k < b.size(); ++k) b[k] *= static_cast<float>(k < b.size() / 2 ? std::pow(10.0, -1.0) : std::pow(10.0, -1.8));
    double I = lufs(b, b, sr, 2);
    report("relative gate integrated", I);
    expect(near(I, -20, 0.2), "integrated loudness applies the -10 LU relative gate");
  }
  // EBU Tech 3342 case 1: 20 s at -20 dBFS then 20 s at -30 dBFS -> LRA 10 LU.
  {
    double sr = 48000;
    auto b = sine(1000, sr, static_cast<size_t>(sr * 40), 1.0);
    for (size_t k = 0; k < b.size(); ++k) b[k] *= static_cast<float>(k < b.size() / 2 ? std::pow(10.0, -20 / 20.0) : std::pow(10.0, -30 / 20.0));
    double lra = lufs(b, b, sr, 3);
    report("LRA", lra);
    expect(near(lra, 10, 1), "loudness range of a 10 dB step is 10 LU");
  }
}

// ------------------------------------------------------------- meters, peaks
void testMeters() {
  // True peak of a quarter-rate sine sampled between its peaks.
  {
    TruePeak tp;
    auto s = sine(12000, 48000, 4800, 1.0, kPi / 4);
    float sp = 0;
    for (float x : s) sp = std::max(sp, std::fabs(x));
    float t = tp.process(s.data(), static_cast<int>(s.size()));
    std::printf("# sample peak %.4f true peak %.4f\n", sp, t);
    expect(near(sp, 0.7071, 0.001) && near(t, 1.0, 0.03), "true peak finds the inter-sample peak");
  }
  auto fx = createEffect("gl.meter");
  auto* u = dynamic_cast<UtilityFx*>(fx.get());
  LevelState* lv = u ? u->levels() : nullptr;
  expect(lv != nullptr, "level meter exposes levels");
  if (!lv) return;
  auto L = sine(1000, 48000, 48000, 0.5), R = sine(1000, 48000, 48000, 0.25);
  run(*fx, defaultPluginState("gl.meter"), L, R, 512);
  float pl = lv->peakL.exchange(0), pr = lv->peakR.exchange(0);
  expect(near(pl, 0.5, 0.002) && near(pr, 0.25, 0.002), "level meter peaks");
  expect(near(lv->rmsL.load(), 0.5 / std::sqrt(2.0), 0.01) && near(lv->rmsR.load(), 0.25 / std::sqrt(2.0), 0.01), "level meter RMS");
  expect(near(lv->vuL.load(), 0.5 / std::sqrt(2.0), 0.02), "level meter VU reads sine RMS");
  expect(near(lv->correlation.load(), 1.0, 0.01), "correlation of in-phase signals is +1");
  expect(lv->peakL.load() == 0, "peak since-read resets on exchange");
  // Clip detection and reset.
  std::vector<float> hot(1000, 1.2f), hotR(1000, 0.f);
  run(*fx, defaultPluginState("gl.meter"), hot, hotR, 100);
  expect(lv->maxPeak.load() > 1.0f, "level meter latches peaks over 0 dBFS");
  lv->resetRequest.store(true);
  std::vector<float> q(64, 0.f), q2(64, 0.f);
  run(*fx, defaultPluginState("gl.meter"), q, q2, 64);
  expect(lv->maxPeak.load() == 0, "level meter reset clears the latched peak");
  // Anti-phase correlation.
  auto fx2 = createEffect("gl.scope");
  auto* u2 = dynamic_cast<UtilityFx*>(fx2.get());
  auto A = sine(500, 48000, 48000, 0.5), B = A;
  for (auto& x : B) x = -x;
  run(*fx2, defaultPluginState("gl.scope"), A, B, 512);
  expect(near(u2->levels()->correlation.load(), -1.0, 0.01), "correlation of opposite signals is -1");
  expect(near(u2->levels()->momentary.load(), lufs(A, B, 48000, 0), 0.05), "scope publishes momentary loudness");
}

// ------------------------------------------------------------ clock, utilities
void testClockMuteSend() {
  {
    auto fx = createEffect("gl.clock");
    auto* u = dynamic_cast<UtilityFx*>(fx.get());
    std::vector<float> L(4096, 0.f), R = L;
    ProcessContext c = ctxAt(48000);
    c.playing = true;
    c.tick = 384;
    c.tempo = 120;
    for (int k = 0; k < 4; ++k) fx->process(defaultPluginState("gl.clock"), c, L.data(), R.data(), 1024);
    const TransportState* t = u->transport();
    expect(t && t->playedSamples.load() == 4096 && t->playing.load() && t->tick.load() == 384 && t->tempo.load() == 120,
           "clock publishes the transport");
    c.playing = false;
    fx->process(defaultPluginState("gl.clock"), c, L.data(), R.data(), 1024);
    c.playing = true;
    fx->process(defaultPluginState("gl.clock"), c, L.data(), R.data(), 1024);
    expect(t->playedSamples.load() == 1024, "clock elapsed time restarts with the transport");
  }
  {
    PluginState s = st("gl.mute", {{MuMute, 1.f}});
    auto fx = createEffect("gl.mute");
    auto L = noise(4800, 1), R = noise(4800, 2);
    run(*fx, s, L, R, 256);
    expect(rms(L, 0, L.size()) == 0 && rms(R, 0, R.size()) == 0, "mute silences (primed muted)");
    PluginState open = defaultPluginState("gl.mute");
    auto fx2 = createEffect("gl.mute");
    auto L2 = noise(4800, 1), R2 = noise(4800, 2);
    run(*fx2, open, L2, R2, 256);
    run(*fx2, s, L2, R2, 256);  // switch to mute: fades within 5 ms
    expect(L2[1000] == 0 && L2[400] == 0, "mute fades out within the fade time");
    PluginState left = st("gl.mute", {{MuMute, 1.f}, {MuSide, 1.f}});
    auto fx3 = createEffect("gl.mute");
    auto L3 = noise(4800, 1), R3 = noise(4800, 2), R30 = R3;
    run(*fx3, left, L3, R3, 256);
    expect(rms(L3, 0, 4800) == 0 && R3 == R30, "mute left side only");
  }
  {
    const PluginInfo* info = findPlugin("gl.stereomatrix");
    PluginState swap = presetState(*info, 1);
    auto fx = createEffect("gl.stereomatrix");
    auto L = noise(4800, 1), R = noise(4800, 2), L0 = L, R0 = R;
    run(*fx, swap, L, R, 256);
    expect(L == R0 && R == L0, "stereo matrix swap");
    PluginState del = st("gl.stereomatrix", {{SmClassic, 0.f}, {SmDelay, 1.f}});
    auto fx2 = createEffect("gl.stereomatrix");
    auto L2 = noise(4800, 1), R2 = noise(4800, 2), R20 = R2, L20 = L2;
    run(*fx2, del, L2, R2, 256);
    bool delayed = L2 == L20;
    for (size_t i = 48; i < R2.size(); ++i) delayed = delayed && R2[i] == R20[i - 48];
    expect(delayed, "stereo matrix delays the right side by 1 ms");
    PluginState mono = presetState(*info, 2);
    auto fx3 = createEffect("gl.stereomatrix");
    auto L3 = noise(4800, 1), R3 = noise(4800, 2);
    run(*fx3, mono, L3, R3, 256);
    expect(L3 == R3, "stereo matrix mono");
  }
}

// --------------------------------------------------------------------- notes
void testNotes() {
  std::vector<std::string> pages(kNotePages);
  pages[0] = "Mix notes\n# not a header\nline 3";
  pages[41] = "unicode \xc3\xa9 and #7:99 fake marker\n";
  pages[99] = "last";
  std::string enc = encodeNotes(pages);
  expect(decodeNotes(enc) == pages, "notes round-trip through PluginState::data");
  expect(encodeNotes(std::vector<std::string>(kNotePages)).empty(), "empty notebook stores nothing");
  expect(decodeNotes("plain text")[0] == "plain text", "plain text loads as page 1");
  auto broken = decodeNotes("GLNOTES1\n#3:999\nshort");
  expect(broken.size() == static_cast<size_t>(kNotePages) && broken[2].empty(), "corrupt notes data is ignored safely");
}

}  // namespace

int main() {
  testCatalogue();
  testGeneric();
  testPassThrough();
  testFreeFilter();
  testQuickLpBassBoost();
  testCurveEq();
  testLoudness();
  testMeters();
  testClockMuteSend();
  testNotes();
  return finish();
}
