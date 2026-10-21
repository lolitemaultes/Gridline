// Gridline — J-42: effects exactness tier F. Reference models for the Love
// Philter (Octa Filter engine 1), the Convolver's IR build, the Delay Bank's 2x
// resampler, Maximus' linear-phase splits and the FL Studio Mobile Rack FX
// modules (Pocket Rack). FL corpus checks run only when FL Studio is installed.
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>
#include "../src/core/PluginInfo.h"
#include "../src/core/PluginParams.h"
#include "../src/core/flp/Flp.h"
#include "../src/core/flp/FlpCoverage.h"
#include "../src/core/flp/FlpPresets.h"
#include "../src/engine/Effects.h"
#include "../src/engine/Sample.h"
#include "../src/packs/Packs.h"
#include "../src/packs/dynamics/modrack_ref.h"
#include "../src/packs/creative/creative.h"
#include "../src/packs/creative/octafilter_ref.h"
#include "../src/packs/creative/octafilter_refeng.h"
#include "../src/packs/dynamics/dynamics.h"
#include "../src/packs/modulation/modulation.h"
#include "../src/packs/dynamics/maximus_ref.h"
#include "../src/packs/dynamics/pocket_mods.h"
#include "../src/packs/spacefx/sfx_convolver.h"
#include "../src/packs/spacefx/spacefx.h"
#include "Check.h"

using namespace gl;
namespace fs = std::filesystem;
namespace cr = gl::packs::creative;
namespace dy = gl::packs::dynamics;
namespace sx = gl::packs::spacefx;
namespace maxref = gl::packs::dynamics::maxref;

namespace {
constexpr double kSr = 48000;
const char* kFl = "/Applications/FL Studio 2026.app/Contents/Resources/FL";
const std::string kFx = std::string(kFl) + "/Data/Patches/Plugin presets/Effects";
bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }
bool haveFl() { return fs::exists(kFx); }

// Exact presets / instances of an FL plugin: {exact, total}.
std::pair<int, int> presetsExact(const std::string& folder) {
  int n = 0, ex = 0;
  if (!fs::exists(kFx + "/" + folder)) return {0, 0};
  for (const auto& e : fs::recursive_directory_iterator(kFx + "/" + folder)) {
    if (e.path().extension() != ".fst") continue;
    flp::PresetResult r;
    if (!flp::importFlPreset(e.path().string(), r) || r.kind != flp::PresetResult::Plugin) continue;
    ++n, ex += r.exact;
  }
  return {ex, n};
}
std::pair<int, int> projectsExact(const std::string& name) {
  int n = 0, ex = 0;
  flp::CoverageFiles cf = flp::coverageFiles(kFl);
  for (const std::string& path : cf.projects) {
    flp::File f;
    std::string err;
    if (!flp::readFile(path, f, &err)) continue;
    flp::Sections sec = flp::split(f);
    Song song;
    flp::Report rep;
    flp::Context c(f, song, rep);
    c.factoryData = kFl;
    c.projectDir = fs::path(path).parent_path().string();
    auto one = [&](const std::vector<flp::Event>& evs) {
      const flp::Event* nm = flp::find(evs, 201);
      if (!nm || nm->text() != name) return;
      flp::PluginMapping m;
      const flp::PluginDecoder* d = flp::findDecoder(name);
      if (d && (*d)(c, evs, m)) ++n, ex += m.exact;
    };
    for (const auto& [k, evs] : sec.channels) one(evs);
    for (const auto& ins : sec.inserts)
      for (const auto& sl : ins.slots) one(sl.events);
  }
  return {ex, n};
}

void noise(std::vector<float>& L, std::vector<float>& R, uint32_t seed, float amp = 0.3f) {
  for (size_t i = 0; i < L.size(); ++i) {
    seed = seed * 1664525u + 1013904223u;
    L[i] = amp * ((seed >> 8) / 8388608.0f - 1.0f);
    seed = seed * 1664525u + 1013904223u;
    R[i] = amp * ((seed >> 8) / 8388608.0f - 1.0f);
  }
}
bool finite(const std::vector<float>& v) {
  for (float x : v)
    if (!std::isfinite(x)) return false;
  return true;
}
double peak(const std::vector<float>& v, size_t from = 0) {
  double p = 0;
  for (size_t i = from; i < v.size(); ++i) p = std::max(p, static_cast<double>(std::fabs(v[i])));
  return p;
}
// Runs an effect over the buffers in blocks of `blk`.
void run(Effect& fx, const PluginState& s, std::vector<float>& L, std::vector<float>& R, int blk, bool playing = true) {
  ProcessContext ctx;
  ctx.sampleRate = kSr, ctx.tempo = 120, ctx.playing = playing;
  for (size_t o = 0; o < L.size(); o += static_cast<size_t>(blk)) {
    const int n = static_cast<int>(std::min(L.size() - o, static_cast<size_t>(blk)));
    ctx.tick = static_cast<double>(o) / kSr * 2.0 * ctx.ppq;
    fx.process(s, ctx, L.data() + o, R.data() + o, n);
  }
}

// ===================================================================== Love Philter
void testLovePhilter() {
  // LFO speed law: (10001^(raw/65536) − 1)/625 cycles per beat.
  auto rate = [](double raw) { return (std::pow(10001.0, raw / 65536.0) - 1.0) / 625.0; };
  expect(near(rate(40898), 0.5, 2e-3) && near(rate(35989), 0.25, 1e-3) && near(rate(45819), 1.0, 2e-3), "love philter: LFO speed law (corpus values)");
  // Shaper: default triangle starts at 0 rising; its quarter phase is +1.
  cr::ref::LfoShape sh;
  sh.setup(0, 0, 0, 0);
  expect(near(sh.at(sh.start), 0, 1e-6) && sh.at(sh.start + 0x10000000u) > 0 && near(sh.at(sh.start + 0x40000000u), 1, 1e-6),
         "love philter: LFO shaper triangle (start 0 rising, top at +1/4)");
  sh.setup(0, 0, 64, 0);  // pulse 0.5: plateau at +1 past E
  expect(sh.at(0xF0000000u) == 1.f, "love philter: LFO pulse plateau");
  // Lime 2-pole: unity at DC.
  cr::ref::DesignIn di;
  di.type = cr::FtLp2, di.mode = 1, di.c = 0.5, di.r = 0.3, di.K = 44100.0 / kSr;
  cr::ref::Design d = cr::ref::design(di);
  cr::ref::StageS z;
  double y = 0;
  for (int i = 0; i < 20000; ++i) y = cr::ref::runStage(d, d.st[0], z, 1.0);
  expect(near(y, 1.0, 1e-6), "love philter: 2-pole spring low pass passes DC at unity");
  // Staggered triple: earlier stages higher (fc^0.75 per step).
  di.type = cr::FtSvf, di.mode = 5;
  cr::ref::Design d3 = cr::ref::design(di);
  expect(d3.n == 3 && d3.st[0].v[0] > d3.st[1].v[0] && d3.st[1].v[0] > d3.st[2].v[0], "love philter: staggered stages, last stage at the base cutoff");
  // Mango (ladder), driven and resonant: finite.
  di.type = cr::FtLadder, di.mode = 3, di.r = 1, di.lowRaw = 128;
  cr::ref::Design dm = cr::ref::design(di);
  cr::ref::StageS zm[3];
  bool fin = true;
  for (int i = 0; i < 48000; ++i) {
    double x = std::sin(i * 0.05);
    for (int k = 0; k < dm.n; ++k) x = cr::ref::runStage(dm, dm.st[k], zm[k], x);
    fin = fin && std::isfinite(x);
  }
  expect(fin, "love philter: driven resonant 4-pole stays finite");
  // Engine: reference by default for new instances, legacy for states saved before it.
  PluginState s = defaultPluginState("gl.octafilter");
  expect(s.params.size() == static_cast<size_t>(cr::OfTotal) && s.params[static_cast<size_t>(cr::OfModel)] == 1.f, "love philter: new instances run the reference engine");
  PluginState old = s;
  old.params.resize(static_cast<size_t>(cr::OfCount));
  normalizePluginState(old);
  expect(old.params[static_cast<size_t>(cr::OfModel)] == 0.f, "love philter: pre-J-42 states keep the legacy engine");
  // Block-size invariance and oversampling (2x .. 16x) finite.
  for (int os : {0, 1, 4}) {
    PluginState st = s;
    st.params[static_cast<size_t>(cr::OfOversample)] = static_cast<float>(os);
    st.params[static_cast<size_t>(cr::unitParam(0, cr::UType))] = cr::FtLpRbj;
    std::vector<float> L1(12000), R1(12000);
    noise(L1, R1, 7);
    std::vector<float> L2 = L1, R2 = R1;
    auto a = createEffect("gl.octafilter"), b = createEffect("gl.octafilter");
    run(*a, st, L1, R1, 64), run(*b, st, L2, R2, 1000);
    double diff = 0;
    for (size_t i = 0; i < L1.size(); ++i) diff = std::max(diff, static_cast<double>(std::fabs(L1[i] - L2[i])));
    expect(finite(L1) && diff < 1e-6 && peak(L1, 2000) > 0.01,
           ("love philter: reference engine block-size invariant, " + std::to_string(1 << os) + "x").c_str());
  }
  // Silence in → silence out.
  {
    auto fx = createEffect("gl.octafilter");
    std::vector<float> L(8000, 0.f), R(8000, 0.f);
    run(*fx, s, L, R, 256);
    expect(peak(L) < 1e-12 && peak(R) < 1e-12, "love philter: silence in, silence out");
  }
  if (haveFl()) {
    auto pr = projectsExact("Fruity Love Philter");
    auto ps = presetsExact("Fruity Love Philter");
    std::printf("# Love Philter: projects %d/%d, presets %d/%d exact\n", pr.first, pr.second, ps.first, ps.second);
    expect(pr.second == 49 && pr.first == 49 && ps.first == ps.second && ps.second >= 47, "love philter: every shipped state exact (0 before J-42)");
  }
}

// ===================================================================== Convolver
void testConvolver() {
  namespace rf = sx::detail::ref;
  expect(near(rf::eqGain(0.5), 1, 0) && near(rf::eqGain(1.0), 4.0, 1e-5) && rf::eqGain(0.0) == 0.f, "convolver: EQ gain law (3^(2v) - 1)/2");
  expect(near(rf::eqX(22050), 1, 1e-9) && near(rf::eqX(130.8127826502993), std::log(2.0) / std::log(2 * 22050 / 130.8127826502993), 1e-9),
         "convolver: EQ frequency axis (130.8 Hz knee, 22.05 kHz end)");
  // Separation / pan matrix: hard right folds the left in at -3 dB, centre is unity.
  float m[4];
  rf::sepPan(1, 1, 0, m);
  expect(near(m[0], 0, 1e-6) && near(m[1], 0.70710678, 1e-6) && near(m[3], 0.70710678, 1e-6), "convolver: pan envelope matrix (hard right)");
  rf::sepPan(0, 2, 0, m);
  expect(m[0] == 2 && m[1] == 2 && m[2] == 0, "convolver: volume envelope = 2 x value");
  // Normalise: a constant IR ends with a 300 ms RMS peak of 0.05.
  sx::detail::IrBuf ir;
  ir.rate = 48000;
  ir.p[0].assign(48000, 0.5f), ir.p[1].assign(48000, 0.5f);
  rf::normalise(ir);
  expect(near(ir.p[0][1000], 0.05, 1e-6), "convolver: normalise to a 300 ms RMS peak of 0.05");
  // Self-convolution of an impulse is the impulse.
  sx::detail::IrBuf d;
  d.rate = 48000;
  d.p[0].assign(1000, 0.f), d.p[1].assign(1000, 0.f);
  d.p[0][0] = d.p[1][0] = 1;
  rf::selfConvolution(d, 1.0);
  expect(near(d.p[0][0], 1, 1e-5) && near(d.p[0][5], 0, 1e-6), "convolver: self-convolution law (impulse unchanged)");
  // Flat EQ = plain gain; a sloped EQ keeps the IR length and is zero-phase.
  sx::detail::IrBuf e = d;
  std::vector<sx::EnvPoint> flat = {{0, 1.0, 0}, {1, 1.0, 0}};
  rf::eqCurve(e, flat, 1.0, 48000, 1);
  expect(near(e.p[0][0], 4.0, 1e-5), "convolver: flat EQ curve = gain only");
  sx::detail::IrBuf q = d;
  std::vector<sx::EnvPoint> tilt = {{0, 0.75, 0}, {1, 0.25, 0}};
  rf::eqCurve(q, tilt, 1.0, 48000, 1);
  size_t pk = 0;
  for (size_t i = 0; i < q.len(); ++i)
    if (std::fabs(q.p[0][i]) > std::fabs(q.p[0][pk])) pk = i;
  expect(q.len() == 1000 && pk == 0 && std::isfinite(q.p[0][0]), "convolver: EQ curve FIR is zero-phase (peak stays at 0)");
  // Old states keep the editing chain.
  PluginState s = defaultPluginState("gl.convolver");
  PluginState old = s;
  old.params.resize(static_cast<size_t>(sx::CvModel));
  normalizePluginState(old);
  expect(s.params[static_cast<size_t>(sx::CvModel)] == 0.f && old.params[static_cast<size_t>(sx::CvModel)] == 0.f, "convolver: editing chain by default and for old states");
  if (haveFl()) {
    auto pr = projectsExact("Fruity Convolver");
    auto ps = presetsExact("Fruity Convolver");
    std::printf("# Convolver: projects %d/%d, presets %d/%d exact\n", pr.first, pr.second, ps.first, ps.second);
    expect(pr.first == pr.second && pr.second == 21 && ps.first == ps.second && ps.second == 261, "convolver: every shipped state exact (17/21, 259/261 before)");
  }
}

// ===================================================================== Delay Bank
void testDelayBank() {
  // With every unit off, the 2x path is an all-pass (up then down): energy kept.
  PluginState s = defaultPluginState("gl.multidelay");
  s.params[static_cast<size_t>(sx::MdModel)] = 1;
  s.params[static_cast<size_t>(sx::MdOversample)] = 1;
  for (int k = 0; k < sx::kTaps; ++k) s.params[static_cast<size_t>(sx::mdParam(k, sx::MdOn))] = 0;
  s.params[static_cast<size_t>(sx::MdDry)] = 1;
  auto fx = createEffect("gl.multidelay");
  std::vector<float> L(48000, 0.f), R(48000, 0.f);
  for (int i = 0; i < 48000; ++i) L[static_cast<size_t>(i)] = R[static_cast<size_t>(i)] = static_cast<float>(0.5 * std::sin(i * 2 * 3.14159265 * 1000 / kSr));
  run(*fx, s, L, R, 256);
  double e = 0;
  for (size_t i = 24000; i < L.size(); ++i) e += static_cast<double>(L[i]) * L[i];
  expect(finite(L) && near(std::sqrt(e / 24000), 0.5 / std::sqrt(2.0), 2e-3), "delay bank: 2x resampler round trip keeps the level (all-pass)");
  if (haveFl()) {
    auto ps = presetsExact("Fruity Delay Bank");
    auto pr = projectsExact("Fruity Delay Bank");
    std::printf("# Delay Bank: projects %d/%d, presets %d/%d exact\n", pr.first, pr.second, ps.first, ps.second);
    expect(ps.first == ps.second && pr.first == pr.second, "delay bank: every shipped state exact (oversampled ones included)");
  }
}

// ===================================================================== Maximus linear-phase splits
maxref::RefState flatState() {
  maxref::RefState s;
  const int g[7] = {19800, 1, 48065, 1, 1115, 1000, -1};
  for (int k = 0; k < 7; ++k) s.raw[k] = g[k];
  const int b[13] = {1, 1000, 1000, 0, 0, 1000, 1115, 0, 2, 5000, 3, 2882, 0};  // no compression, unity gains
  for (int k = 0; k < 4; ++k)
    for (int t = 0; t < 13; ++t) s.raw[maxref::rawIndex(k, t)] = b[t];
  s.safetyOff = true;
  return s;
}
void testMaximusSplits() {
  // Legacy FIR: the three bands sum to a pure delay of (N − 1)/2.
  maxref::RefState s = flatState();
  s.fir = true;
  maxref::Engine eng;
  eng.configure(s, kSr);
  std::vector<float> L(6000), R(6000);
  noise(L, R, 3);
  std::vector<float> L0 = L;
  eng.process(L.data(), R.data(), static_cast<int>(L.size()));
  const int N = maxref::firTaps(kSr), D = (N - 1) / 2;
  double err = 0;
  for (size_t i = 1000; i < L.size(); ++i) err = std::max(err, static_cast<double>(std::fabs(L[i] - L0[i - static_cast<size_t>(D)])));
  expect(N == 193 && err < 2e-4 && eng.latency() == D, "maximus: legacy FIR split sums to a pure (N-1)/2 delay");
  // LIN: FFT bank, latency 3N/4, fades in, then close to the delayed input.
  maxref::RefState l = flatState();
  l.lin = true, l.linQuality = 2, l.linSpeed = 2;  // 10 ms change fade
  maxref::Engine el;
  el.configure(l, kSr);
  std::vector<float> A(30000), B(30000);
  noise(A, B, 5);
  std::vector<float> A0 = A;
  el.process(A.data(), B.data(), static_cast<int>(A.size()));
  const int Nl = maxref::linSize(kSr, 2), Dl = 3 * Nl / 4;
  double num = 0, den = 0;
  for (size_t i = 10000; i < A.size(); ++i) {
    const double dlt = A[i] - A0[i - static_cast<size_t>(Dl)];
    num += dlt * dlt, den += static_cast<double>(A0[i]) * A0[i];
  }
  expect(finite(A) && Nl == 2048 && el.latency() == Dl && num / den < 0.05, "maximus: FFT bank split ≈ the delayed input (3N/4 latency)");
  if (haveFl()) {
    auto pr = projectsExact("Maximus");
    std::printf("# Maximus: projects %d/%d exact\n", pr.first, pr.second);
    expect(pr.first == pr.second && pr.second >= 125, "maximus: every shipped project instance exact (106/125 before)");
  }
}

// ===================================================================== Pocket Rack
void testPocketRack() {
  namespace pk = gl::packs::dynamics::pocket;
  // Every module type: finite output from noise, parameter extremes finite.
  for (int ty = 1; ty < dy::kPkTypes; ++ty) {
    auto m = pk::makeModule(ty);
    if (!m) continue;
    for (int mode = 0; mode < 3; ++mode) {
      float p[pk::kParams];
      dy::pocketModuleDefaults(ty, p);
      if (mode == 1)
        for (int i = 1; i < pk::kParams; ++i) p[i] = 1;
      if (mode == 2)
        for (int i = 1; i < pk::kParams; ++i) p[i] = 0;
      auto mm = pk::makeModule(ty);
      mm->set(p);
      mm->prepare(kSr);
      pk::Host h;
      h.pos = 0, h.bpm = 120, h.playing = true;
      std::vector<float> L(9600), R(9600);
      noise(L, R, static_cast<uint32_t>(ty * 3 + mode));
      for (size_t o = 0; o < L.size(); o += 480) mm->process(L.data() + o, R.data() + o, 480, h);
      expect(finite(L) && finite(R), ("pocket rack: module " + std::string(dy::pocketModuleName(ty)) + " finite (" +
                                                       (mode == 0 ? "defaults" : mode == 1 ? "all max" : "all min") + ")")
                                                          .c_str());
    }
  }
  auto runMod = [](int ty, const std::vector<std::pair<int, float>>& set, std::vector<float>& L, std::vector<float>& R) {
    float p[pk::kParams];
    dy::pocketModuleDefaults(ty, p);
    for (auto& [i, v] : set) p[i] = v;
    auto m = pk::makeModule(ty);
    m->set(p);
    m->prepare(kSr);
    pk::Host h;
    h.pos = 0, h.bpm = 120, h.playing = true;
    for (size_t o = 0; o < L.size(); o += 512) m->process(L.data() + o, R.data() + o, static_cast<int>(std::min<size_t>(512, L.size() - o)), h);
  };
  {  // Eight-band EQ flat: an 8-sample delay.
    std::vector<float> L(2000), R(2000);
    noise(L, R, 11);
    const std::vector<float> L0 = L;
    runMod(7, {}, L, R);
    double err = 0;
    for (size_t i = 8; i < L.size(); ++i) err = std::max(err, static_cast<double>(std::fabs(L[i] - L0[i - 8])));
    expect(err < 1e-4, "pocket rack: flat eight-band EQ = 8-sample delay");
  }
  {  // Four-band EQ flat: 1 sample.
    std::vector<float> L(2000), R(2000);
    noise(L, R, 12);
    const std::vector<float> L0 = L;
    runMod(3, {}, L, R);
    double err = 0;
    for (size_t i = 1; i < L.size(); ++i) err = std::max(err, static_cast<double>(std::fabs(L[i] - L0[i - 1])));
    expect(err < 1e-4, "pocket rack: flat four-band EQ = 1-sample delay");
  }
  {  // Eight-band EQ: +12 dB knob → +24 dB peak (V = 10^(G/20) in the cookbook formulas).
    pk::Bq q = pk::bandDesign(kSr, 12, 1000, 1, 1);
    const double w = 2 * pk::kPi * 1000 / kSr;
    auto mag = [&](const pk::Bq& b) {
      const std::complex<double> z1 = std::polar(1.0, -w), z2 = z1 * z1;
      return std::abs((static_cast<double>(b.b0) + static_cast<double>(b.b1) * z1 + static_cast<double>(b.b2) * z2) /
                      (1.0 + static_cast<double>(b.a1) * z1 + static_cast<double>(b.a2) * z2));
    };
    expect(near(20 * std::log10(mag(q)), 24, 0.05), "pocket rack: band designer realises 2 x G dB");
  }
  {  // Tape echo: first echo after 2 + (N − 2)·0.36 samples at 0.24.
    std::vector<float> L(30000, 0.f), R(30000, 0.f);
    L[100] = R[100] = 1;
    runMod(18, {}, L, R);
    size_t at = 0;
    for (size_t i = 200; i < L.size(); ++i)
      if (std::fabs(L[i]) > std::fabs(L[at])) at = i;
    // Stamp-after-step order: one tape period plus one sample, linearly interpolated over two samples.
    const double D = 2 + (kSr - 2) * 0.36 + 1;
    expect(near(static_cast<double>(at) - 100, D, 1) && near(std::fabs(L[at]) + std::fabs(L[at + 1]), 0.24, 0.005), "pocket rack: tape echo time and level (Mix x Feedback)");
  }
  {  // Gain & balance at its default: unity.
    std::vector<float> L(1000, 0.25f), R(1000, -0.25f);
    runMod(20, {}, L, R);
    expect(near(L[500], 0.25, 1e-5) && near(R[500], -0.25, 1e-5), "pocket rack: gain & balance default = unity");
  }
  {  // Limiter: 2 ms look-ahead, holds a 0 dB ceiling on a hot sine.
    std::vector<float> L(24000), R(24000);
    for (size_t i = 0; i < L.size(); ++i) L[i] = R[i] = static_cast<float>(2.0 * std::sin(i * 0.05));
    runMod(28, {}, L, R);
    expect(peak(L, 2000) < 1.02, "pocket rack: limiter ceiling");
  }
  {  // Rack plugin: neutral wrapper, post gain ramp from −6 dB, presets.
    PluginState s = defaultPluginState("gl.pocketrack");
    s.params[static_cast<size_t>(dy::pkParam(0, dy::PkType))] = 20;
    float d[pk::kParams];
    dy::pocketModuleDefaults(20, d);
    for (int i = 0; i < pk::kParams; ++i) s.params[static_cast<size_t>(dy::pkParam(0, dy::PkP0 + i))] = d[i];
    auto fx = createEffect("gl.pocketrack");
    std::vector<float> L(4000, 0.5f), R(4000, 0.5f);
    run(*fx, s, L, R, 256);
    expect(near(L[0], 0.5 * (0.5 + 0.005), 1e-4) && near(L[3000], 0.5, 1e-5), "pocket rack: module wrapper post gain starts at -6 dB and settles at unity");
    expect(fx->latencyFor(s, kSr) == 0, "pocket rack: latency 0 for gain module");
    s.params[static_cast<size_t>(dy::pkParam(1, dy::PkType))] = 28;
    dy::pocketModuleDefaults(28, d);
    for (int i = 0; i < pk::kParams; ++i) s.params[static_cast<size_t>(dy::pkParam(1, dy::PkP0 + i))] = d[i];
    expect(fx->latencyFor(s, kSr) == 96, "pocket rack: limiter latency int(0.002 sr)");
  }
  {  // Silence in → silence out for the rack's factory presets.
    const PluginInfo* info = findPlugin("gl.pocketrack");
    bool ok = info && info->presets.size() >= 2;
    for (const auto& pr : info->presets) {
      PluginState s = defaultPluginState("gl.pocketrack");
      for (auto& [k, v] : pr.values) s.params[static_cast<size_t>(k)] = v;
      auto fx = createEffect("gl.pocketrack");
      std::vector<float> L(9600, 0.f), R(9600, 0.f);
      run(*fx, s, L, R, 512);
      ok = ok && peak(L) < 1e-6 && peak(R) < 1e-6;
    }
    expect(ok, "pocket rack: factory presets, silence in → silence out");
  }
  if (haveFl()) {
    auto ps = presetsExact("FL Studio Mobile Rack FX");
    std::printf("# FL Studio Mobile Rack FX: presets %d/%d exact\n", ps.first, ps.second);
    expect(ps.second == 31 && ps.first >= 29, "pocket rack: shipped racks exact except the random drift and pitch modules (8 before)");
  }
}

// ===================================================================== Multiband Delay (band delays) / Peak Controller
void testBandDelayAndPeak() {
  for (int mode = 0; mode < 3; ++mode) {
    PluginState s = defaultPluginState("gl.banddelay");
    s.params[static_cast<size_t>(sx::BdModel)] = 1;
    s.params[static_cast<size_t>(sx::BdRMode)] = static_cast<float>(mode);
    s.params[static_cast<size_t>(sx::BdRScale)] = 0.75f;  // 0.5 s maximum
    s.params[static_cast<size_t>(sx::BdRFeedback)] = 0.5f;
    auto fx = createEffect("gl.banddelay");
    std::vector<float> L(48000, 0.f), R(48000, 0.f);
    L[10] = R[10] = 1;
    run(*fx, s, L, R, 512);
    expect(finite(L) && peak(L, 100) > 1e-3 && peak(L, 100) < 4, ("band delay: reference bank mode " + std::to_string(mode) + " delays and stays finite").c_str());
    expect(fx->latencyFor(s, kSr) == (mode == 2 ? 1536 : 0), "band delay: linear-phase latency 1536");
  }
  {  // Peak controller: block peak, linear per-tick decay, sum / peak / LFO outputs.
    PluginState s = defaultPluginState("gl.peakctl");
    s.params[static_cast<size_t>(gl::packs::modulation::PkRAmount)] = 256;  // 2 x peak
    s.params[static_cast<size_t>(gl::packs::modulation::PkRamp)] = 0;
    auto fx = createEffect("gl.peakctl");
    std::vector<float> L(4800, 0.f), R(4800, 0.f);
    L[0] = 0.25f;
    ProcessContext ctx;
    ctx.sampleRate = kSr, ctx.tempo = 120, ctx.ppq = 96, ctx.playing = false;
    fx->process(s, ctx, L.data(), R.data(), 260);  // one tick = 250 samples at 120 BPM, 96 PPQ
    float out[3];
    fx->controlOutputs(out, 3);
    expect(near(out[1], 0.5, 1e-3) && near(out[0], out[1], 1e-6), "peak controller: peak x amount law (0.25 x 2.0)");
    for (int b = 0; b < 40; ++b) fx->process(s, ctx, L.data() + 260, R.data() + 260, 64);
    fx->controlOutputs(out, 3);
    expect(out[1] < 0.5f && out[1] > 0.f, "peak controller: linear decay per tick");
  }
  {  // Time gate reference: flat slots = the input 15 samples late while playing; silence stays silent.
    PluginState s = defaultPluginState("gl.timegate");
    s.params[static_cast<size_t>(pp::TgModel)] = 1;
    s.params[static_cast<size_t>(pp::TgTimeSlot)] = 0, s.params[static_cast<size_t>(pp::TgVolSlot)] = 0;
    auto fx = createEffect("gl.timegate");
    std::vector<float> L(4000), R(4000);
    noise(L, R, 21);
    const std::vector<float> L0 = L;
    run(*fx, s, L, R, 256, true);
    double err = 0;
    for (size_t i = 15; i < L.size(); ++i) err = std::max(err, static_cast<double>(std::fabs(L[i] - L0[i - 15])));
    expect(err < 1e-6, "time gate (reference): bypassed time path = dry delayed 15 samples");
  }
  if (haveFl()) {
    auto ps = presetsExact("Multiband Delay");
    auto pk = projectsExact("Fruity Peak Controller");
    std::printf("# Multiband Delay presets %d/%d, Peak Controller projects %d/%d exact\n", ps.first, ps.second, pk.first, pk.second);
    expect(ps.first == 17 && ps.second == 17, "band delay: all 17 shipped presets exact (0 before)");
    expect(pk.first >= 31, "peak controller: shipped project instances exact (0 before; LFO shapes 1-4 pending)");
    auto gb = projectsExact("Gross Beat");
    std::printf("# Gross Beat projects %d/%d exact\n", gb.first, gb.second);
    expect(gb.first == 34 && gb.second == 34, "time gate: every shipped project state exact on the reference engine (11 before)");
    auto vx = projectsExact("Vocodex");
    std::printf("# Vocodex projects %d/%d exact (enhancer = mastering program, unison table)\n", vx.first, vx.second);
    // J-65: the built-in carrier plays the installation's own tone presets on the FM reference model.
    expect(vx.first == 12 && vx.second == 12, "vocodex: every project instance exact (built-in carrier tones: J-65)");
  }
}

}  // namespace

// J-42: module-rack speaker on FL's impulse file = impulse × table gain (zero latency).
void testCabinet() {
  if (!haveFl()) return;
  const std::string dir = std::string(kFl) + "/Plugins/Fruity/Effects/Distructor/module/impulses";
  SamplePtr ir = loadSample(dir + "/IMP Cabinet Stereo Metal.wv");
  expect(ir && ir->frames() > 100, "cabinet: impulse file readable in place");
  if (!ir) return;
  dy::cabRate().store(44100.0);
  PluginState s = defaultPluginState("gl.amprack");
  auto set = [&](int i, double v) { s.params[static_cast<size_t>(i)] = static_cast<float>(v); };
  set(dy::RkModel, 1), set(dy::DrCount, 1), set(dy::DrSoftClip, 0);
  dy::drack::ModuleState ms;
  dy::drack::moduleDefaults(ms);
  ms.kind = dy::drack::KSpeaker, ms.ir = 3;
  const int f[dy::DmP0] = {ms.kind, ms.algo, ms.os, ms.removeDc, ms.disabled, ms.ftype, ms.fopt, ms.ir, ms.chorusMode};
  for (int q = 0; q < dy::DmP0; ++q) set(dy::drParam(0, q), f[q]);
  for (int q = 0; q < dy::kDrParams; ++q) set(dy::drParam(0, dy::DmP0 + q), ms.p[q]);
  s.data = "cabdir=" + dir + "\n";
  auto fx = createEffect("gl.amprack");
  fx->prepareHost(s, 44100.0);
  ChannelAux aux;
  expect(packs::prepareAux(s, aux), "cabinet: impulse prepared");
  std::vector<float> L(2000, 0.f), R(2000, 0.f);
  L[0] = 1.f, R[0] = 0.5f;
  ProcessContext ctx;
  ctx.sampleRate = 44100.0;
  for (size_t o = 0; o < L.size(); o += 100) fx->processAux(s, aux, ctx, L.data() + o, R.data() + o, 100);
  double err = 0;
  for (size_t i = 0; i < L.size(); ++i) {
    const double hl = i < ir->frames() ? ir->left[i] : 0.0, hr = i < ir->frames() ? ir->right[i] : 0.0;
    err = std::max({err, std::fabs(L[i] - 0.08 * hl), std::fabs(R[i] - 0.5 * 0.08 * hr)});
  }
  std::printf("# cabinet impulse error %.3g\n", err);
  expect(err < 1e-6, "cabinet: speaker output = impulse file x table gain, zero latency, per channel");
  // Pedal board cabinet: n-th sorted impulse, linear read at i·(len − 1)/N, × 0.2.
  const std::string hdir = std::string(kFl) + "/Plugins/Fruity/Effects/Hardcore/Imp";
  const std::string f2 = dy::cab::boardFile(hdir, 2);
  expect(fs::path(f2).filename() == "Model B.wav", "cabinet: pedal board impulse 2 = second sorted file");
  SamplePtr hb = loadSample(f2);
  PluginState b = defaultPluginState("gl.amprack");
  b.params[static_cast<size_t>(dy::RkModel)] = 2, b.params[static_cast<size_t>(dy::PbValue0 + 11)] = 2;
  b.data = "cabdir=" + hdir + "\n";
  ChannelAux ba;
  const bool ok = hb && packs::prepareAux(b, ba);
  const auto* ka = ok ? dynamic_cast<const dy::cab::Aux*>(ba.custom.get()) : nullptr;
  bool same = ka && ka->k[0];
  if (same) {
    const size_t len = hb->frames();
    for (int i = 0; i < 64; ++i) {
      const double p = i * static_cast<double>(len - 1) / static_cast<double>(len);
      const size_t a = static_cast<size_t>(p);
      const double want = 0.2 * (hb->left[a] + (hb->left[a + 1] - hb->left[a]) * (p - static_cast<double>(a)));
      same = same && std::fabs(ka->k[0]->head[0][i] - want) < 1e-7;
    }
  }
  expect(same, "cabinet: pedal board impulse resampled (linear, (len-1)/N) and scaled 0.2");
}

int main() {
  testCabinet();
  testLovePhilter();
  testConvolver();
  testDelayBank();
  testMaximusSplits();
  testPocketRack();
  testBandDelayAndPeak();
  return finish();
}
