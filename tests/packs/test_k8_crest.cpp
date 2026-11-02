// Gridline: K-8 Crest round: the reference engine as the default of new instances,
// oversampling, long curves, the point shapes, FL import values and the FLP round
// trip; older states keep their engine and laws; a measured gain curve per band.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <cstdlib>
#include <memory>
#include <new>
#include <string>
#include <thread>
#include <vector>
#include "../../src/core/PluginInfo.h"
#include "../../src/core/Song.h"
#include "../../src/core/flp/Flp.h"
#include "../../src/core/flp/FlpEncode.h"
#include "../../src/core/flp/FlpPresets.h"
#include "../../src/engine/Effects.h"
#include "../../src/packs/dynamics/dynamics.h"
#include "../../src/packs/dynamics/maximus_ref.h"
#include "../Check.h"

// Counts allocations while armed (the audio thread's part of a run).
static std::atomic<bool> gArmed{false};
static std::atomic<long> gAllocs{0};
void* operator new(std::size_t n) {
  if (gArmed.load(std::memory_order_relaxed)) gAllocs.fetch_add(1, std::memory_order_relaxed);
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return operator new(n); }
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"  // the replaced operator new is malloc
#endif
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

using namespace gl;
namespace fs = std::filesystem;
namespace dy = gl::packs::dynamics;
namespace mr = gl::packs::dynamics::maxref;

namespace {
constexpr double kSr = 48000;
const char* kPresets = "/Applications/FL Studio 2026.app/Contents/Resources/FL/Data/Patches/Plugin presets/Effects/Maximus";

struct Buf {
  std::vector<float> L, R;
};
Buf noise(double sec, float amp, uint32_t seed = 7) {
  const size_t n = static_cast<size_t>(sec * kSr);
  Buf b{std::vector<float>(n), std::vector<float>(n)};
  for (size_t i = 0; i < n; ++i) {
    seed = seed * 1664525u + 1013904223u;
    b.L[i] = amp * (static_cast<float>(seed >> 8) / 8388608.f - 1.f);
    seed = seed * 1664525u + 1013904223u;
    b.R[i] = amp * (static_cast<float>(seed >> 8) / 8388608.f - 1.f);
  }
  return b;
}
Buf sine(double hz, float amp, double sec) {
  const size_t n = static_cast<size_t>(sec * kSr);
  Buf b{std::vector<float>(n), std::vector<float>(n)};
  for (size_t i = 0; i < n; ++i) b.L[i] = b.R[i] = amp * static_cast<float>(std::sin(2.0 * 3.14159265358979 * hz * static_cast<double>(i) / kSr));
  return b;
}
Buf run(const PluginState& st, Buf in, int block = 512) {
  auto fx = createEffect(st.type);
  ProcessContext ctx;
  ctx.sampleRate = kSr;
  fx->prepareHost(st, kSr);
  const int n = static_cast<int>(in.L.size());
  for (int i = 0; i < n; i += block) fx->process(st, ctx, in.L.data() + i, in.R.data() + i, std::min(block, n - i));
  return in;
}
bool finite(const Buf& b) {
  for (size_t i = 0; i < b.L.size(); ++i)
    if (!std::isfinite(b.L[i]) || !std::isfinite(b.R[i])) return false;
  return true;
}
double peak(const Buf& b, size_t from = 0) {
  double p = 0;
  for (size_t i = from; i < b.L.size(); ++i) p = std::max({p, static_cast<double>(std::fabs(b.L[i])), static_cast<double>(std::fabs(b.R[i]))});
  return p;
}
double rms(const Buf& b, size_t from = 0) {
  double s = 0;
  for (size_t i = from; i < b.L.size(); ++i) s += static_cast<double>(b.L[i]) * b.L[i] + static_cast<double>(b.R[i]) * b.R[i];
  return std::sqrt(s / static_cast<double>(2 * (b.L.size() - from)));
}
double maxDiff(const Buf& a, const Buf& b) {
  double d = 0;
  for (size_t i = 0; i < a.L.size(); ++i) d = std::max({d, static_cast<double>(std::fabs(a.L[i] - b.L[i])), static_cast<double>(std::fabs(a.R[i] - b.R[i]))});
  return d;
}
int rawParam(int band, int tag) { return dy::MsRef0 + dy::MrRaw0 + mr::rawIndex(band, tag); }
void put(PluginState& s, int k, double v) { s.params[static_cast<size_t>(k)] = static_cast<float>(v); }
double xDb(double db) { return std::log(2.0 * std::pow(10.0, db / 20.0) + 1.0) / std::log(9.0); }

// A curve through the given (x, y, tension, type) points.
struct Pt {
  double x, y, t;
  int type;
};
void setCurve(PluginState& s, int band, const std::vector<Pt>& pts) {
  const int np = std::min(static_cast<int>(pts.size()), dy::kMsCurvePts);
  put(s, dy::msExt(band, dy::MxPoints), np);
  for (int k = 0; k < np; ++k) {
    const Pt& q = pts[static_cast<size_t>(k == np - 1 ? static_cast<int>(pts.size()) - 1 : k)];
    put(s, dy::msExt(band, dy::MxPt0 + k * 3), q.x), put(s, dy::msExt(band, dy::MxPt0 + k * 3 + 1), q.y), put(s, dy::msExt(band, dy::MxPt0 + k * 3 + 2), q.t);
    put(s, dy::MsRef0 + dy::MrPtType0 + band * dy::kMsCurvePts + k, q.type);
  }
  dy::LongCurve lc;
  lc.n = static_cast<int>(pts.size());
  for (int k = 0; k < lc.n; ++k) {
    const Pt& q = pts[static_cast<size_t>(k)];
    lc.x[k] = static_cast<float>(q.x), lc.y[k] = static_cast<float>(q.y), lc.t[k] = static_cast<float>(q.t), lc.type[k] = q.type;
  }
  s.data = dy::masterLongCurvesWith(s.data, band, lc.n > dy::kMsCurvePts ? &lc : nullptr);
}

void testDefaults() {
  PluginState d = defaultPluginState("gl.master");
  expect(d.params[dy::MsRef0 + dy::MrModel] == 1 && d.params[dy::MsRef0 + dy::MrRevision] == 1 && d.params[dy::MsRef0 + dy::MrTarget] == 3,
         "new instance: reference engine, current laws, master band selected");
  const Buf in = noise(0.3, 0.1f);
  Buf o = run(d, in);
  expect(finite(o) && peak(o, 8192) > 0.05 && rms(o, 8192) > 0.25 * rms(in), "new instance passes a quiet signal");
  Buf z{std::vector<float>(24000, 0.f), std::vector<float>(24000, 0.f)};
  expect(peak(run(d, z)) == 0, "silence in, silence out (reference engine)");
  PluginState oldEngine = d;
  put(oldEngine, dy::MsRef0 + dy::MrModel, 0);
  expect(peak(run(oldEngine, z)) == 0, "silence in, silence out (legacy engine)");
  for (int os = 0; os <= 3; ++os) {
    PluginState s = d;
    put(s, dy::MsRef0 + dy::MrOversample, os);
    expect(peak(run(s, z)) == 0, "silence in, silence out (oversampled)");
  }
  // Every factory preset does something of its own, stays finite and bounded.
  const PluginInfo* pi = findPlugin("gl.master");
  expect(pi && pi->presets.size() >= 2 && pi->presets.size() <= 6, "two to six factory presets");
  const Buf hot = noise(0.5, 0.5f);
  const Buf base = run(d, hot);
  int differ = 0;
  for (const auto& p : pi->presets) {
    PluginState s = defaultPluginState("gl.master");
    for (const auto& kv : p.values) s.params[static_cast<size_t>(kv.first)] = kv.second;
    Buf o2 = run(s, hot);
    expect(finite(o2) && peak(o2) < 4.0, ("preset finite: " + p.name).c_str());
    differ += maxDiff(o2, base) > 1e-3;
  }
  expect(differ == static_cast<int>(pi->presets.size()), "every factory preset changes the sound");
}

void testExtremes() {
  const Buf hot = noise(0.2, 0.9f);
  int bad = 0, n = 0;
  auto check = [&](const PluginState& s) {
    ++n;
    if (!finite(run(s, hot))) ++bad;
  };
  const PluginInfo* pi = findPlugin("gl.master");
  for (int k = 0; k < 59; ++k) {
    const ParamDesc& d = pi->params[static_cast<size_t>(dy::MsRef0 + dy::MrRaw0 + k)];
    for (double v : {d.min, d.max}) {
      PluginState s = defaultPluginState("gl.master");
      put(s, dy::MsRef0 + dy::MrRaw0 + k, v);
      check(s);
    }
  }
  for (int os = 1; os <= 3; ++os) {  // everything at once, each end
    for (int end = 0; end < 2; ++end) {
      PluginState s = defaultPluginState("gl.master");
      for (int k = 0; k < 59; ++k) {
        const ParamDesc& d = pi->params[static_cast<size_t>(dy::MsRef0 + dy::MrRaw0 + k)];
        put(s, dy::MsRef0 + dy::MrRaw0 + k, end ? d.max : d.min);
      }
      put(s, dy::MsRef0 + dy::MrOversample, os);
      check(s);
    }
  }
  char name[96];
  std::snprintf(name, sizeof name, "extremes stay finite (%d of %d bad)", bad, n);
  expect(bad == 0, name);
}

void testBlocksAndLatency() {
  const Buf nz = noise(0.6, 0.5f);
  auto invariant = [&](const PluginState& s, const char* name) { expect(maxDiff(run(s, nz, 64), run(s, nz, 1000)) < 1e-6, name); };
  PluginState d = defaultPluginState("gl.master");
  invariant(d, "block-size invariant: default");
  for (int os = 1; os <= 3; ++os) {
    PluginState s = d;
    put(s, dy::MsRef0 + dy::MrOversample, os);
    put(s, rawParam(1, mr::TSatThr), -500);
    invariant(s, "block-size invariant: oversampled, saturating");
  }
  // Reported latency = where a quiet impulse lands.
  for (int os = 0; os <= 3; ++os) {
    PluginState s = d;
    put(s, dy::MsRef0 + dy::MrOversample, os);
    auto fx = createEffect("gl.master");
    const int rep = fx->latencyFor(s, kSr);
    Buf b{std::vector<float>(48000, 0.f), std::vector<float>(48000, 0.f)};
    b.L[20000] = b.R[20000] = 0.01f;
    Buf o = run(s, b);
    size_t best = 0;
    float bv = 0;
    for (size_t i = 0; i < o.L.size(); ++i)
      if (std::fabs(o.L[i]) > bv) bv = std::fabs(o.L[i]), best = i;
    char name[120];
    std::snprintf(name, sizeof name, "oversampling stages %d: latency %d, impulse at +%d", os, rep, static_cast<int>(best) - 20000);
    expect(rep > 0 && std::abs(static_cast<int>(best) - 20000 - rep) <= 3, name);
  }
}

// Older states: what they were saved with keeps its sound.
void testOldStates() {
  // Saved before the reference engine existed.
  PluginState legacy = defaultPluginState("gl.master");
  legacy.params.resize(static_cast<size_t>(dy::kMsCount));
  normalizePluginState(legacy);
  expect(legacy.params[dy::MsRef0 + dy::MrModel] == 0, "state older than the reference engine stays on the legacy engine");
  PluginState explicitLegacy = defaultPluginState("gl.master");
  put(explicitLegacy, dy::MsRef0 + dy::MrModel, 0);
  put(explicitLegacy, dy::MsRef0 + dy::MrTarget, 0);
  const Buf nz = noise(0.5, 0.7f);
  expect(maxDiff(run(legacy, nz), run(explicitLegacy, nz)) == 0, "old short vector renders as the legacy engine");
  // Full old vector with the model stored: unchanged by the new default.
  PluginState full = explicitLegacy;
  normalizePluginState(full);
  expect(full.params[dy::MsRef0 + dy::MrModel] == 0, "stored legacy engine is kept");
  // Saved by an import before this round: the reference engine with the former laws.
  PluginState imported = defaultPluginState("gl.master");
  imported.params.resize(static_cast<size_t>(dy::MsRef0 + dy::MrOversample));
  normalizePluginState(imported);
  expect(imported.params[dy::MsRef0 + dy::MrModel] == 1 && imported.params[dy::MsRef0 + dy::MrRevision] == 0 && imported.params[dy::MsRef0 + dy::MrOversample] == 0,
         "older reference state keeps revision 0");
  // Golden numbers of the engine before this round (measured on that code).
  PluginState g = imported;
  put(g, dy::MsRef0 + dy::MrRaw0 + 0, 25000), put(g, rawParam(1, mr::TPre), 1400), put(g, rawParam(3, mr::TPre), 1300);
  put(g, rawParam(1, mr::TRel), 7000), put(g, rawParam(1, mr::TSatThr), -300), put(g, rawParam(2, mr::TSep), 500);
  put(g, rawParam(0, mr::TSusMode), 1);
  const Buf gn = noise(1.0, 0.6f, 11);
  Buf o = run(g, gn);
  char name[160];
  std::snprintf(name, sizeof name, "revision 0 keeps the former sound (peak %.6f rms %.6f)", peak(o, 8192), rms(o, 8192));
  expect(std::fabs(peak(o, 8192) - 0.97535950) < 2e-5 && std::fabs(rms(o, 8192) - 0.26603842) < 2e-5, name);
}

// The engine's static curve, measured per band: steady sine in the band's middle.
void testGainCurves() {
  const double hz[4] = {45, 800, 14000, 800};
  const double thr = -18, ratio = 4;
  auto expected = [&](double db) { return db <= thr ? db : thr + (db - thr) / ratio; };
  for (int b = 0; b < 4; ++b) {
    PluginState s = defaultPluginState("gl.master");
    for (int k = 0; k < 4; ++k) put(s, rawParam(k, mr::TMode), k == b ? 0 : (b == 3 ? 1 : 1));
    // The knee and the slope on the drawn curve's axes (0.975 ceiling safety off for a clean reading).
    put(s, dy::MsRef0 + dy::MrSafetyOff, 1);
    std::vector<Pt> pts{{0, 0, 0, 0}};
    for (double db = -60; db <= 12.01; db += 3) pts.push_back({xDb(db), xDb(expected(db)), 0, 0});
    setCurve(s, b, pts);
    put(s, rawParam(b, mr::TAtt), 500), put(s, rawParam(b, mr::TRel), 2000);
    double worst = 0;
    for (double db : {-40.0, -24.0, -18.0, -12.0, -6.0}) {
      Buf in = sine(hz[b], static_cast<float>(std::pow(10.0, db / 20.0)), 1.0);
      Buf o = run(s, in);
      const double measured = 20.0 * std::log10(rms(o, 24000) / rms(in, 24000)) + db;
      worst = std::max(worst, std::fabs(measured - expected(db)));
    }
    char name[120];
    std::snprintf(name, sizeof name, "measured gain curve, band %d: within %.2f dB of the drawn curve", b, worst);
    expect(worst < 0.5, name);
  }
}

void testCurves() {
  // Long curves: text form round trip, garbage refused, the renderer sees all the points.
  dy::LongCurve lc, back;
  lc.n = 12;
  for (int k = 0; k < 12; ++k) lc.x[k] = static_cast<float>(k) / 11.f, lc.y[k] = 0.4f * static_cast<float>(k) / 11.f, lc.t[k] = k % 2 ? 0.25f : 0.f, lc.type[k] = k % 3;
  const std::string text = dy::masterLongCurvesWith("", 2, &lc);
  expect(dy::masterLongCurve(text, 2, back) && back.n == 12 && back.x[7] == lc.x[7] && back.t[3] == lc.t[3] && back.type[5] == lc.type[5], "long curve text round trip");
  expect(!dy::masterLongCurve(text, 1, back), "another band has no long curve");
  expect(dy::masterLongCurvesWith(text, 2, nullptr).empty(), "removing the only long curve empties the data");
  for (const char* junk : {"", "crv", "crv1 zz", "crv1 0 ffff", "crv1 0 2 1 2", "crv1 123456789 1"}) {
    dy::LongCurve j;
    expect(!dy::masterLongCurve(junk, 0, j), "bad long-curve text is refused");
  }
  PluginState s = defaultPluginState("gl.master");
  std::vector<Pt> zig;
  for (int k = 0; k < 16; ++k) zig.push_back({k / 15.0, (k / 15.0) * 0.45 + (k % 2 ? 0.03 : 0.0), 0, 0});
  setCurve(s, 3, zig);
  const mr::Curve c = dy::masterRefCurve(s, 3);
  expect(c.n == 16 && std::fabs(mr::curveAt(c, 5 / 15.0) - zig[5].y) < 1e-5, "a long curve runs with all its points");
  const Buf nz = noise(0.3, 0.7f);
  PluginState s2 = s;
  s2.data.clear();
  expect(maxDiff(run(s, nz), run(s2, nz)) > 1e-4, "the long curve changes the sound against its short parameters");
  // Smooth shape: a lone smooth point has flat ends, between smooth points it keeps its slope.
  mr::Curve sm;
  sm.n = 4;
  const double xs[4] = {0, 0.3, 0.6, 1}, ys[4] = {0, 0.2, 0.3, 0.5};
  for (int k = 0; k < 4; ++k) sm.x[k] = xs[k], sm.y[k] = ys[k], sm.t[k] = 0, sm.type[k] = 10;
  sm.prepareTangents();
  bool mono = true;
  double prev = -1;
  for (int k = 0; k <= 200; ++k) {
    const double y = mr::curveAt(sm, k / 200.0);
    mono = mono && y >= prev - 1e-9 && std::isfinite(y);
    prev = y;
  }
  expect(mono && sm.tanE[1] > 0 && sm.tanS[2] > 0, "smooth points: monotone, tangents joined");
  // Tangents as measured on the reference: four smooth segments through these dB points.
  {
    mr::Curve m;
    m.n = 5;
    const double mx[5] = {0, xDb(-30), xDb(-18), xDb(-6), 1}, my[5] = {0, xDb(-30), xDb(-20), xDb(-14), xDb(-8)};
    for (int k = 0; k < 5; ++k) m.x[k] = mx[k], m.y[k] = my[k], m.t[k] = 0, m.type[k] = 10;
    m.prepareTangents();
    const double want[4][2] = {{0, 0.8709}, {1.1751, 0.6528}, {1.4809, 0.7154}, {1.4052, 0.0003}};
    double worst = 0;
    for (int k = 0; k < 4; ++k) worst = std::max({worst, std::fabs(m.tanS[k + 1] - want[k][0]), std::fabs(m.tanE[k + 1] - want[k][1])});
    char nm[96];
    std::snprintf(nm, sizeof nm, "smooth tangents as measured (worst %.4f)", worst);
    expect(worst < 0.002, nm);
  }
  // Double bend 3 keeps both ends and the middle.
  expect(std::fabs(mr::segShape(0, 0.5, 12)) < 1e-3 && std::fabs(mr::segShape(1, 0.5, 12) - 1) < 1e-3 && std::fabs(mr::segShape(0.5, 0.5, 12) - 0.5) < 1e-3, "double bend 3 shape");
}

// The hard rule: after the Program build's prepareHost, process allocates nothing, whatever moves.
void testNoAllocation() {
  {  // The counter itself works.
    gAllocs = 0;
    gArmed = true;
    std::vector<int>* v = new std::vector<int>(100);
    gArmed = false;
    expect(gAllocs.load() >= 2 && v->size() == 100, "allocation counter sees allocations");
    delete v;
  }
  auto noAlloc = [](PluginState base, std::vector<std::pair<int, double>> moves, const char* name) {
    auto fx = createEffect(base.type);
    ProcessContext ctx;
    ctx.sampleRate = kSr;
    // As the build does for automated parameters: prepared at their maximum.
    PluginState maxed = base;
    const PluginInfo* pi = findPlugin(base.type);
    for (const auto& m : moves) maxed.params[static_cast<size_t>(m.first)] = static_cast<float>(pi->params[static_cast<size_t>(m.first)].max);
    fx->prepareHost(maxed, kSr);
    std::vector<PluginState> steps;
    for (int i = 0; i < 12; ++i) {
      PluginState s = base;
      for (const auto& m : moves) s.params[static_cast<size_t>(m.first)] = static_cast<float>(i % 2 ? m.second : base.params[static_cast<size_t>(m.first)]);
      steps.push_back(std::move(s));
    }
    Buf in = noise(0.2, 0.5f);
    gAllocs = 0;
    gArmed = true;
    int at = 0;
    for (size_t i = 0; i + 512 <= in.L.size(); i += 512, ++at)
      fx->process(steps[static_cast<size_t>(at / 4) % steps.size()], ctx, in.L.data() + i, in.R.data() + i, 512);
    gArmed = false;
    char nm[140];
    std::snprintf(nm, sizeof nm, "no allocation in process: %s (%ld)", name, gAllocs.load());
    expect(gAllocs.load() == 0 && finite(in), nm);
  };
  const int split = dy::MsRef0 + dy::MrRaw0;
  PluginState d = defaultPluginState("gl.master");
  noAlloc(d, {}, "default");
  noAlloc(d, {{split, 30000}, {split + 2, 52000}, {rawParam(1, mr::TPre), 1500}}, "split and gain automation");
  PluginState f = d;
  put(f, dy::MsRef0 + dy::MrFir, 1);
  noAlloc(f, {{split, 30000}, {split + 2, 52000}}, "split automation, older linear-phase filters");
  noAlloc(f, {{split, 30000}, {dy::MsRef0 + dy::MrOversample, 2}}, "split and oversampling automation, older linear-phase filters");
  noAlloc(f, {{rawParam(0, mr::TMode), 3}}, "band off with the older linear-phase filters");
  PluginState l = d;
  put(l, dy::MsRef0 + dy::MrLin, 1);
  noAlloc(l, {{split, 30000}}, "split automation, linear-phase split");
  noAlloc(l, {{dy::MsRef0 + dy::MrLinQuality, 0}}, "quality automation, linear-phase split");
  noAlloc(d, {{dy::MsRef0 + dy::MrLin, 1}}, "linear-phase split switched on by automation");
  noAlloc(d, {{dy::MsRef0 + dy::MrOversample, 3}}, "oversampling automation");
  // A change that skipped the build (generic edits): no allocation either, it waits for the next build.
  {
    auto fx = createEffect("gl.master");
    ProcessContext ctx;
    ctx.sampleRate = kSr;
    fx->prepareHost(d, kSr);
    PluginState s = d;
    put(s, dy::MsRef0 + dy::MrOversample, 3), put(s, dy::MsRef0 + dy::MrLin, 1);
    Buf in = noise(0.1, 0.5f);
    gAllocs = 0;
    gArmed = true;
    fx->process(d, ctx, in.L.data(), in.R.data(), 512);
    fx->process(s, ctx, in.L.data() + 512, in.R.data() + 512, 512);
    gArmed = false;
    expect(gAllocs.load() == 0 && finite(in), "unprepared oversampling and linear phase: no allocation");
  }
  // Builds while it runs: storage changes hands without a race the render can see.
  {
    auto fx = createEffect("gl.master");
    ProcessContext ctx;
    ctx.sampleRate = kSr;
    fx->prepareHost(d, kSr);
    std::atomic<bool> stop{false};
    PluginState a = d, b = d;
    put(b, dy::MsRef0 + dy::MrLin, 1), put(b, dy::MsRef0 + dy::MrOversample, 2), put(b, dy::MsRef0 + dy::MrLinQuality, 0);
    std::thread ui([&] {
      for (int i = 0; i < 8 && !stop; ++i) {
        PluginState q = b;
        put(q, dy::MsRef0 + dy::MrOversample, i % 4);
        fx->prepareHost(q, kSr);
      }
      stop = true;
    });
    Buf in = noise(0.2, 0.5f);
    bool ok = true;
    for (int r = 0; r < 200 && !stop; ++r) {
      Buf c = in;
      for (size_t i = 0; i + 512 <= c.L.size(); i += 512) fx->process(r % 2 ? a : b, ctx, c.L.data() + i, c.R.data() + i, 512);
      ok = ok && finite(c);
    }
    stop = true;
    ui.join();
    expect(ok, "Program builds during render stay finite");
  }
}

bool decodeState(const std::vector<uint8_t>& state, flp::PluginMapping& m);

void testFlp() {
  // Round trip: all the stored words, options, a long curve with its shapes.
  PluginState s = defaultPluginState("gl.master");
  const int words[7] = {25000, 0, 50000, 1, 2000, 800, 40000};
  for (int k = 0; k < 7; ++k) put(s, dy::MsRef0 + dy::MrRaw0 + k, words[k]);
  for (int b = 0; b < 4; ++b) {
    const int w[13] = {b == 2 ? 1 : 0, 1100 + b, 900 + b, 100 * b - 200, b % 2 ? 250 : -250, 950, 1500 + b, 300, 3, 6000, 4, 2000, b % 2};
    for (int k = 0; k < 13; ++k) put(s, rawParam(b, k), w[k]);
  }
  put(s, dy::MsRef0 + dy::MrOversample, 2), put(s, dy::MsRef0 + dy::MrMasterMid, 0), put(s, dy::MsRef0 + dy::MrSolo, 0), put(s, dy::MsRef0 + dy::MrTarget, 2);
  put(s, dy::MsRef0 + dy::MrSafetyOff, 1), put(s, dy::MsRef0 + dy::MrLin, 1), put(s, dy::MsRef0 + dy::MrLinQuality, 4), put(s, dy::MsRef0 + dy::MrLinSpeed, 2);
  std::vector<Pt> pts;
  for (int k = 0; k < 11; ++k) pts.push_back({k / 10.0, (k / 10.0) * 0.5, k % 2 ? 0.25 : 0.0, k % 4 == 3 ? 10 : k % 2});
  setCurve(s, 1, pts);
  flp::enc::EncodeContext ec;
  const flp::enc::Result r = flp::enc::encode(s, "", nullptr, ec);
  expect(r.hasState && r.flName == "Maximus" && !r.dataMismatch, "encodes with its long curve");
  flp::PluginMapping m;
  expect(r.hasState && decodeState(r.state, m), "the written state decodes");
  bool same = true;
  for (int k = dy::MrRaw0; k < dy::MrRaw0 + 59; ++k) same = same && m.state.params[static_cast<size_t>(dy::MsRef0 + k)] == s.params[static_cast<size_t>(dy::MsRef0 + k)];
  expect(same, "round trip: all stored words");
  bool opts = true;
  for (int k : {dy::MrOversample, dy::MrSafetyOff, dy::MrLin, dy::MrLinQuality, dy::MrLinSpeed, dy::MrTarget, dy::MrSolo})
    opts = opts && m.state.params[static_cast<size_t>(dy::MsRef0 + k)] == s.params[static_cast<size_t>(dy::MsRef0 + k)];
  expect(opts, "round trip: options, oversampling among them");
  expect(m.state.data == s.data && m.exact && m.state.params[dy::MsRef0 + dy::MrRevision] == 1, "round trip: long curve, exact, current laws");
  const Buf nz = noise(0.4, 0.6f);
  expect(maxDiff(run(s, nz), run(m.state, nz)) < 1e-6, "round trip renders the same");

  if (!fs::exists(kPresets)) return;
  // Factory presets load exactly, carry FL's stored words, and survive export.
  int n = 0, exact = 0, trip = 0;
  for (const auto& e : fs::recursive_directory_iterator(kPresets)) {
    if (e.path().extension() != ".fst") continue;
    ++n;
    flp::PresetResult pr;
    if (!flp::importFlPreset(e.path().string(), pr) || pr.state.type != "gl.master") continue;
    exact += pr.exact;
    normalizePluginState(pr.state);
    const flp::enc::Result rr = flp::enc::encode(pr.state, "", nullptr, ec);
    flp::PluginMapping back2;
    if (rr.hasState && decodeState(rr.state, back2)) {
      bool eq = back2.state.data == pr.state.data;
      for (int k = 0; k < dy::kMrCount; ++k) eq = eq && back2.state.params[static_cast<size_t>(dy::MsRef0 + k)] == pr.state.params[static_cast<size_t>(dy::MsRef0 + k)];
      trip += eq;
    }
  }
  char name[96];
  std::snprintf(name, sizeof name, "factory presets: %d of %d load exactly", exact, n);
  expect(n > 30 && exact == n, name);
  std::snprintf(name, sizeof name, "factory presets: %d of %d survive export and import", trip, n);
  expect(trip == n, name);
  flp::PresetResult def;
  if (flp::importFlPreset(std::string(kPresets) + "/Default.fst", def)) {
    const PluginState fresh = defaultPluginState("gl.master");
    bool eq = true;
    for (int k = 0; k < 59; ++k) eq = eq && def.state.params[static_cast<size_t>(dy::MsRef0 + dy::MrRaw0 + k)] == fresh.params[static_cast<size_t>(dy::MsRef0 + dy::MrRaw0 + k)];
    expect(eq, "a new instance holds the words of the plugin's own default preset");
  }
}

bool decodeState(const std::vector<uint8_t>& state, flp::PluginMapping& m) {
  std::vector<uint8_t> ev;
  auto data = [&](uint8_t id, const std::vector<uint8_t>& p) {
    ev.push_back(id);
    size_t n = p.size();
    do {
      uint8_t x = n & 0x7F;
      n >>= 7;
      ev.push_back(static_cast<uint8_t>(x | (n ? 0x80 : 0)));
    } while (n);
    ev.insert(ev.end(), p.begin(), p.end());
  };
  std::vector<uint8_t> name;
  for (char ch : std::string("Maximus")) name.push_back(static_cast<uint8_t>(ch)), name.push_back(0);
  name.push_back(0), name.push_back(0);
  data(201, name);
  data(213, state);
  std::vector<uint8_t> f = {'F', 'L', 'h', 'd', 6, 0, 0, 0, 0, 0, 1, 0, 96, 0, 'F', 'L', 'd', 't'};
  const uint32_t len = static_cast<uint32_t>(ev.size());
  for (int i = 0; i < 4; ++i) f.push_back(static_cast<uint8_t>(len >> (8 * i)));
  f.insert(f.end(), ev.begin(), ev.end());
  static flp::File file;
  std::string err;
  if (!flp::readBytes(f, file, &err)) return false;
  static Song song;
  static flp::Report rep;
  flp::Context c(file, song, rep);
  auto it = flp::decoders().find("Maximus");
  if (it == flp::decoders().end()) return false;
  m = flp::PluginMapping{};
  m.flName = "Maximus";
  if (!it->second(c, file.events, m) || m.glType.empty()) return false;
  normalizePluginState(m.state);
  return true;
}
}  // namespace

int main() {
  testDefaults();
  testExtremes();
  testBlocksAndLatency();
  testOldStates();
  testGainCurves();
  testCurves();
  testNoAllocation();
  testFlp();
  return finish();
}
