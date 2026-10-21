// Gridline — Q-5: reverb room visual. Each control maps monotonically to its
// own visual cue (taken from the active network's view), the figure stays
// inside its box at every extreme and box size, easing converges without
// overshoot independent of the frame rate, the side-count morph never pops,
// and building a frame allocates nothing.
#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <vector>
#include "../../src/core/PluginInfo.h"
#include "../../src/core/PluginParams.h"
#include "../../src/engine/GraphModels.h"
#include "../../src/engine/ReverbRoomViz.h"
#include "../Check.h"

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
using namespace gl::roomviz;

namespace {

constexpr double kSr = 48000;

void setp(PluginState& s, int k, double v) {
  if (static_cast<size_t>(k) >= s.params.size()) s.params.resize(static_cast<size_t>(k) + 1, 0.f);
  s.params[static_cast<size_t>(k)] = static_cast<float>(v);
}
PluginState model(int m) {
  PluginState s = defaultPluginState("gl.reverb");
  setp(s, RvbModel, m);
  return s;
}
Look look(const PluginState& s) { return lookFor(graphmodel::reverbView(s, kSr, 120)); }

// Walks a control over values and checks a measure of the look rises (dir +1) or falls (-1) strictly.
template <class M>
bool monotonic(int mdl, int param, std::initializer_list<double> values, int dir, M measure, const char* what) {
  PluginState s = model(mdl);
  double prev = 0;
  bool first = true, ok = true;
  for (double v : values) {
    setp(s, param, v);
    const double m = measure(look(s));
    if (!first && !(dir > 0 ? m > prev + 1e-6 : m < prev - 1e-6)) {
      std::printf("#   %s: %g -> %g at %g\n", what, prev, m, v);
      ok = false;
    }
    prev = m, first = false;
  }
  expect(ok, what);
  return ok;
}

double field(const Look& l, int f) { return l[f]; }
double rippleSum(const Look& l) {
  static Geometry g;
  build(l, 0, 0, 300, 200, g);
  double s = 0;
  for (float v : g.rippleLevel) s += v;
  return s;
}
double sparkSum(const Look& l) {
  double s = 0;
  for (int e = 0; e < kMaxTaps; ++e) s += l[kErLvl0 + e];
  return s;
}
// Horizontal extent / height of the prism (pixels) in a fixed box.
void prismExtent(const Look& l, double& w, double& h, double& baseY) {
  static Geometry g;
  build(l, 0, 0, 300, 200, g);
  double x0 = 1e9, x1 = -1e9, y0 = 1e9, y1 = -1e9;
  for (int i = 0; i < g.segCount; ++i)
    for (const Pt& p : {g.segs[static_cast<size_t>(i)].a, g.segs[static_cast<size_t>(i)].b})
      x0 = std::min(x0, static_cast<double>(p.x)), x1 = std::max(x1, static_cast<double>(p.x)), y0 = std::min(y0, static_cast<double>(p.y)),
      y1 = std::max(y1, static_cast<double>(p.y));
  w = x1 - x0, h = y1 - y0, baseY = y1;
}

void testMapping() {
  // Room size: more sides and more twist (hall / plate: 0..1; full: 1..100).
  for (int m : {0, 1}) {
    monotonic(m, RvbSize, {0.0, 0.2, 0.4, 0.6, 0.8, 1.0}, +1, [](const Look& l) { return field(l, kSides); }, "room size -> side count rises (hall/plate)");
    monotonic(m, RvbSize, {0.0, 0.2, 0.4, 0.6, 0.8, 1.0}, +1, [](const Look& l) { return field(l, kTwist); }, "room size -> twist rises (hall/plate)");
  }
  monotonic(2, RvbFRoom, {1, 20, 40, 60, 80, 100}, +1, [](const Look& l) { return field(l, kSides); }, "full: room -> side count rises");
  expect(std::fabs(sidesFor(0) - 3) < 1e-9 && std::fabs(sidesFor(1) - 28) < 1e-6, "side count law: triangle .. 28 sides");
  // Diffusion: wider and shorter.
  for (int m : {0, 1}) {
    monotonic(m, RvbDiffusion, {0.0, 0.25, 0.5, 0.75, 1.0}, +1, [](const Look& l) { double w, h, b; prismExtent(l, w, h, b); return w; }, "diffusion -> prism wider");
    monotonic(m, RvbDiffusion, {0.0, 0.25, 0.5, 0.75, 1.0}, +1, [](const Look& l) { double w, h, b; prismExtent(l, w, h, b); return w / h; }, "diffusion -> prism squatter");
  }
  monotonic(2, RvbFDiffusion, {0, 25, 50, 75, 100}, +1, [](const Look& l) { return field(l, kRadius); }, "full: diffusion -> radius");
  // Decay: ripples carry more level; the decay view reaches -60 dB later.
  monotonic(0, RvbDecayTime, {0.2, 0.5, 1, 2, 5, 10, 20}, +1, rippleSum, "hall decay -> ripple level rises");
  monotonic(1, RvbDecayTime, {0.2, 0.5, 1, 2, 5, 10, 20}, +1, rippleSum, "plate decay -> ripple level rises");
  monotonic(2, RvbFDecay, {2, 5, 10, 20, 50, 100, 200}, +1, rippleSum, "full decay -> ripple level rises");
  // Early reflections: sparks brighten; the plate has none.
  monotonic(0, RvbEarly, {0.0, 0.25, 0.5, 0.75, 1.0}, +1, sparkSum, "hall ER level -> sparks brighten");
  monotonic(2, RvbFEr, {0, 40, 80, 120, 160}, +1, sparkSum, "full ER level -> sparks brighten");
  expect(sparkSum(look(model(1))) == 0, "plate: no reflection sparks (the network has no taps)");
  // Damping / colour: top ring brightness.
  monotonic(0, RvbDampHz, {800, 2000, 4000, 8000, 16000}, +1, [](const Look& l) { return field(l, kBright); }, "hall damping cutoff up -> brighter top");
  monotonic(1, RvbDampHz, {800, 2000, 4000, 8000, 16000}, +1, [](const Look& l) { return field(l, kBright); }, "plate damping cutoff up -> brighter top");
  monotonic(2, RvbFDamp, {8, 20, 40, 80, 160}, +1, [](const Look& l) { return field(l, kBright); }, "full damping cutoff up -> brighter top");
  monotonic(0, RvbColor, {-1, -0.5, 0, 0.5, 1}, +1, [](const Look& l) { return field(l, kBright); }, "hall colour -> brighter top");
  // Bass multiplier: base ring weight.
  monotonic(0, RvbBassMult, {0.4, 0.8, 1.2, 2, 3}, +1, [](const Look& l) { return field(l, kBass); }, "hall bass -> base weight");
  monotonic(1, RvbBassMult, {0.4, 0.8, 1.2, 2, 3}, +1, [](const Look& l) { return field(l, kBass); }, "plate bass -> base weight");
  monotonic(2, RvbFBass, {30, 60, 100, 200, 300}, +1, [](const Look& l) { return field(l, kBass); }, "full bass -> base weight");
  // Width: horizontal spread (hall |width|; full separation narrows towards mono).
  monotonic(0, RvbWidth, {0.0, 0.25, 0.5, 0.75, 1.0}, +1, [](const Look& l) { return field(l, kStretch); }, "hall width -> wider figure");
  monotonic(2, RvbFSep, {-64, -32, 0, 32, 64}, -1, [](const Look& l) { return field(l, kStretch); }, "full separation -> narrower figure");
  // Pre-delay: the room lifts off its floor.
  monotonic(0, RvbPreDelay, {0, 20, 60, 150, 384}, +1, [](const Look& l) { return field(l, kLift); }, "hall pre-delay -> lift");
  monotonic(0, RvbPreDelay, {0, 20, 60, 150, 384}, -1, [](const Look& l) { double w, h, b; prismExtent(l, w, h, b); return b; }, "hall pre-delay -> base rises in the box");
  monotonic(2, RvbFPredelay, {0, 20, 60, 150, 384}, +1, [](const Look& l) { return field(l, kLift); }, "full pre-delay -> lift");
  // Wet: glow; modulation: waviness.
  monotonic(0, RvbWet, {0.05, 0.25, 0.5, 1.0}, +1, [](const Look& l) { return field(l, kWet); }, "hall wet -> glow");
  monotonic(2, RvbFWet, {10, 50, 100, 160}, +1, [](const Look& l) { return field(l, kWet); }, "full wet -> glow");
  monotonic(0, RvbModDepth, {0.0, 0.3, 0.6, 1.0}, +1, [](const Look& l) { return field(l, kMod); }, "hall mod depth -> waviness");
  // Distinct cues: the decay control moves nothing but the decay cues.
  {
    PluginState s = model(0);
    setp(s, RvbDecayTime, 1);
    Look a = look(s);
    setp(s, RvbDecayTime, 8);
    Look b = look(s);
    bool same = true;
    for (int f : {kSides, kTwist, kRadius, kRoom, kStretch, kWet, kLift, kMod}) same = same && a[f] == b[f];
    expect(same && b[kLogT60] > a[kLogT60], "decay changes only the decay cues");
  }
}

void testTruth() {
  // Ripple k's level is the tail's level (60 dB scale) at its time; the decay
  // view crosses -60 dB at pre-delay + T60.
  PluginState s = model(2);
  setp(s, RvbFDecay, 30), setp(s, RvbFPredelay, 96);
  graphmodel::ReverbView v = graphmodel::reverbView(s, kSr, 120);
  Look l = lookFor(v);
  static Geometry g;
  build(l, 0, 0, 300, 200, g);
  bool ok = true;
  for (int k = 0; k < kRipples; ++k) ok = ok && std::fabs(g.rippleLevel[static_cast<size_t>(k)] - std::max(0.0, 1 - rippleSec(k) / v.rt60)) < 1e-4;
  expect(ok, "ripple levels = tail level at the ripple times (engine T60)");
  Decay d;
  buildDecay(l, d);
  int cross = -1;
  for (int i = 0; i <= kDecayPts; ++i)
    if (d.mid[static_cast<size_t>(i)] <= 0.f) {
      cross = i;
      break;
    }
  const double crossSec = d.span * cross / kDecayPts, want = v.preSec + v.rt60;
  std::printf("# decay view crosses -60 dB at %.3f s, pre + T60 = %.3f s\n", crossSec, want);
  expect(cross > 0 && std::fabs(crossSec - want) <= d.span / kDecayPts + 1e-6, "decay view: -60 dB at pre-delay + T60");
  expect(d.mid[0] == 1.f && std::fabs(d.pre - v.preSec) < 1e-6, "decay view: full level through the pre-delay");
}

void testBounds() {
  // Every extreme of every field, several box sizes / aspects (UI scales 1..2, HiDPI).
  const float boxes[][4] = {{0, 0, 152, 104}, {10, 20, 162, 124}, {0, 0, 253, 173}, {0, 0, 456, 312}, {0, 0, 120, 160}, {0, 0, 300, 90}};
  bool ok = true;
  static Geometry g;
  for (const auto& b : boxes)
    for (int combo = 0; combo < 4096 && ok; ++combo) {
      Look l = look(model(combo % 3));
      l[kSides] = (combo & 1) ? 28.f : 3.f + static_cast<float>(combo % 7) * 0.37f;
      l[kTwist] = (combo & 2) ? 0.785f : 0.f;
      l[kRadius] = (combo & 4) ? 1.f : 0.f;
      l[kRoom] = (combo & 8) ? 1.f : 0.f;
      l[kStretch] = (combo & 16) ? 1.2f : 0.8f;
      l[kLift] = (combo & 32) ? 1.f : 0.f;
      l[kMod] = (combo & 64) ? 1.f : 0.f;
      for (int e = 0; e < kMaxTaps; ++e) l[kErSec0 + e] = (combo & 128) ? 30.f : 0.f, l[kErLvl0 + e] = 1;
      build(l, b[0], b[1], b[2], b[3], g);
      auto in = [&](const Pt& p) { return p.x >= b[0] - 0.01f && p.x <= b[2] + 0.01f && p.y >= b[1] - 0.01f && p.y <= b[3] + 0.01f && p.depth >= 0 && p.depth <= 1; };
      for (int i = 0; i < g.segCount; ++i) ok = ok && in(g.segs[static_cast<size_t>(i)].a) && in(g.segs[static_cast<size_t>(i)].b);
      for (const auto& r : g.ripple)
        for (const Pt& p : r) ok = ok && in(p);
      for (const Pt& p : g.shadow) ok = ok && in(p);
      for (const Pt& p : g.spark) ok = ok && in(p);
      if (!ok) std::printf("#   out of box: combo %d box %g x %g\n", combo, static_cast<double>(b[2] - b[0]), static_cast<double>(b[3] - b[1]));
    }
  expect(ok, "figure stays inside its box at every extreme and box size");
  // Far-to-near order.
  Look l = look(model(0));
  l[kSides] = 12;
  build(l, 0, 0, 300, 200, g);
  bool sorted = true;
  for (int i = 1; i < g.segCount; ++i) {
    const Seg &a = g.segs[static_cast<size_t>(i - 1)], &c = g.segs[static_cast<size_t>(i)];
    sorted = sorted && a.a.depth + a.b.depth >= c.a.depth + c.b.depth;
  }
  expect(sorted && g.segCount == 36, "edges sorted far to near (3 per side)");
  // Framing is fixed: the same look in a twice as large box is exactly twice as large.
  static Geometry g2;
  build(l, 0, 0, 600, 400, g2);
  expect(std::fabs(g2.segs[0].a.x - 2 * g.segs[0].a.x) < 1e-3f && std::fabs(g2.unit - 2 * g.unit) < 1e-5f, "geometry scales with the box (crisp at every UI scale)");
}

void testMorph() {
  // Crossing an integer side count moves no vertex by more than a fraction of a pixel.
  Look l = look(model(0));
  static Geometry a, b;
  bool ok = true;
  for (float n : {3.f, 4.f, 5.f, 9.f, 17.f, 27.f}) {
    l[kSides] = n - 1e-4f;
    build(l, 0, 0, 300, 200, a);
    l[kSides] = n + 1e-4f;
    build(l, 0, 0, 300, 200, b);
    auto near = [](const Geometry& x, const Geometry& y) {
      for (int i = 0; i < x.capCount; ++i) {
        double best = 1e9;
        for (int j = 0; j < y.capCount; ++j)
          best = std::min(best, static_cast<double>(std::hypot(x.cap[static_cast<size_t>(i)].x - y.cap[static_cast<size_t>(j)].x, x.cap[static_cast<size_t>(i)].y - y.cap[static_cast<size_t>(j)].y)));
        if (best > 0.5) return false;
      }
      return true;
    };
    ok = ok && b.capCount == a.capCount + 1 && near(a, b) && near(b, a);
    // The new upright fades in from zero.
    float newAlpha = 1;
    for (int i = 0; i < b.segCount; ++i)
      if (b.segs[static_cast<size_t>(i)].kind == 2) newAlpha = std::min(newAlpha, b.segs[static_cast<size_t>(i)].alpha);
    ok = ok && newAlpha < 0.01f;
  }
  expect(ok, "side count morph: no vertex jumps, the new upright fades in");
}

void testEase() {
  PluginState s = model(0);
  Look a = look(s);
  setp(s, RvbSize, 1), setp(s, RvbDecayTime, 12), setp(s, RvbDiffusion, 0), setp(s, RvbPreDelay, 200), setp(s, RvbModel, 2);
  Look b = look(s);
  {
    Ease e;
    expect(!e.step(a, 1.0 / 60) && e.cur.v == a.v, "ease: first step snaps (editor opens on the state)");
    bool overshoot = false, moving = true;
    int frames = 0;
    while (moving && frames < 1000) {
      Look before = e.cur;
      moving = e.step(b, 1.0 / 60);
      for (int f = 0; f < kFieldCount; ++f) {
        const float lo = std::min(a[f], b[f]) - 1e-6f, hi = std::max(a[f], b[f]) + 1e-6f;
        overshoot = overshoot || e.cur[f] < lo || e.cur[f] > hi;
        // each step moves towards the target
        overshoot = overshoot || std::fabs(b[f] - e.cur[f]) > std::fabs(b[f] - before[f]) + 1e-6f;
      }
      ++frames;
    }
    std::printf("# ease settled in %d frames at 60 fps\n", frames);
    expect(!moving && e.cur.v == b.v && frames < 120, "ease converges to the target (snaps at the end, < 2 s)");
    expect(!overshoot, "ease: monotone approach, no overshoot");
    expect(!e.step(b, 1.0 / 60), "ease: idle once settled (no redraw work)");
  }
  {  // frame-rate independence: 0.2 s at 15 / 60 / 240 fps
    Look r[3];
    const int fps[3] = {15, 60, 240};
    for (int k = 0; k < 3; ++k) {
      Ease e;
      e.step(a, 0);
      const int n = fps[k] / 5;  // 0.2 s
      for (int i = 0; i < n; ++i) e.step(b, 1.0 / fps[k]);
      r[k] = e.cur;
    }
    double worst = 0;
    for (int f = kSides; f < kErSec0; ++f) {
      const double range = std::max(1e-3, std::fabs(static_cast<double>(b[f] - a[f])));
      worst = std::max({worst, std::fabs(static_cast<double>(r[0][f] - r[1][f])) / range, std::fabs(static_cast<double>(r[2][f] - r[1][f])) / range});
    }
    std::printf("# ease frame-rate spread %.2e of the move\n", worst);
    expect(worst < 1e-3, "ease is frame-rate independent");
  }
}

void testNoAlloc() {
  PluginState s = model(2);
  graphmodel::ReverbView v = graphmodel::reverbView(s, kSr, 120);
  static Ease e;
  static Geometry g;
  static Decay d;
  e.step(lookFor(v), 0);
  setp(s, RvbFRoom, 90);
  graphmodel::ReverbView v2 = graphmodel::reverbView(s, kSr, 120);
  gAllocs = 0;
  gArmed = true;
  for (int i = 0; i < 120; ++i) {
    Look t = lookFor(i & 1 ? v2 : v);
    e.step(t, 1.0 / 60);
    build(e.cur, 0, 0, 253, 173, g);
    buildDecay(e.cur, d);
  }
  gArmed = false;
  expect(gAllocs == 0, "no allocation per frame (look, ease, geometry, decay view)");
}

}  // namespace

int main() {
  testMapping();
  testTruth();
  testBounds();
  testMorph();
  testEase();
  testNoAlloc();
  return finish();
}
