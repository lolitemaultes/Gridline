// Gridline — Q-5: reverb room visual geometry (see ReverbRoomViz.h).
#include "ReverbRoomViz.h"
#include <algorithm>
#include <cmath>

namespace gl::roomviz {

namespace {

constexpr double kPi = 3.14159265358979323846;
// Camera: looking down on the floor at kPitch, mild perspective at kDist.
constexpr double kPitch = 0.42, kDist = 4.5;
// World extents the framing is fitted to (every control's extreme stays inside).
constexpr double kMaxStretch = 1.28, kFloorR = 1.18, kMaxH = 0.82, kMaxLift = 0.16;
constexpr double kYaw0 = -kPi * 0.5 + 0.33;
// Floor time axis (time after the input, s) → ripple / spark radius.
constexpr double kT0 = 0.004, kT1 = 8.0;

float f(double x) { return static_cast<float>(x); }

// Room radius and height for diffusion (0..1) and room size (0..1).
void prism(double rad01, double room01, double& r, double& h) {
  const double sc = 0.85 + 0.15 * std::clamp(room01, 0.0, 1.0), rad = std::clamp(rad01, 0.0, 1.0);
  r = (0.3 + 0.27 * rad) * sc, h = (kMaxH - 0.36 * rad) * sc;
}

struct Cam {
  double cp = std::cos(kPitch), sp = std::sin(kPitch);
  double sx0 = 0, sx1 = 0, sy0 = 0, sy1 = 0, dv0 = 0, dv1 = 0;  // framing extents
  double scale = 1, ox = 0, oy = 0;
  double dv(double y, double z) const { return kDist + z * cp - y * sp; }
  void raw(double x, double y, double z, double& sx, double& sy, double& d) const {
    d = dv(y, z);
    sx = x / d, sy = -(y * cp + z * sp) / d;
  }
  // Framing: the reachable figure at every extreme, i.e. the floor's outer
  // ring at full width plus the room's top ring for each radius / scale / lift
  // extreme (the prism's own corners lie between them).
  void extend(double x, double y, double z, bool& first) {
    double sx, sy, d;
    raw(x, y, z, sx, sy, d);
    if (first) sx0 = sx1 = sx, sy0 = sy1 = sy, dv0 = dv1 = d, first = false;
    sx0 = std::min(sx0, sx), sx1 = std::max(sx1, sx), sy0 = std::min(sy0, sy), sy1 = std::max(sy1, sy);
    dv0 = std::min(dv0, d), dv1 = std::max(dv1, d);
  }
  Cam(float x0, float y0, float x1, float y1) {
    bool first = true;
    constexpr int kA = 96;
    for (int i = 0; i < kA; ++i) {
      const double a = 2 * kPi * i / kA, c = std::cos(a), s = std::sin(a);
      extend(c * kFloorR * kMaxStretch, 0, s * kFloorR, first);
      for (int e = 0; e < 8; ++e) {
        double r, h;
        prism(e & 1 ? 1.0 : 0.0, e & 2 ? 1.0 : 0.0, r, h);
        const double y = (e & 4 ? kMaxLift : 0.0) + h;
        extend(c * r * kMaxStretch, y, s * r, first);
      }
    }
    const double w = std::max(1e-3, static_cast<double>(x1 - x0)), h = std::max(1e-3, static_cast<double>(y1 - y0));
    scale = 0.996 * std::min(w / (sx1 - sx0), h / (sy1 - sy0));  // sampled circles: a hair of slack
    ox = x0 + 0.5 * (w - (sx1 - sx0) * scale) - sx0 * scale;
    oy = y0 + 0.5 * (h - (sy1 - sy0) * scale) - sy0 * scale;
  }
  Pt project(double x, double y, double z) const {
    double sx, sy, d;
    raw(x, y, z, sx, sy, d);
    Pt p;
    p.x = f(ox + sx * scale), p.y = f(oy + sy * scale);
    p.depth = f(std::clamp((d - dv0) / std::max(1e-9, dv1 - dv0), 0.0, 1.0));
    return p;
  }
};

double floorU(double sec) { return std::clamp(std::log(std::max(kT0, sec) / kT0) / std::log(kT1 / kT0), 0.0, 1.0); }

}  // namespace

double Look::t60() const { return std::exp(static_cast<double>(v[kLogT60])); }
double Look::t60Low() const { return std::exp(static_cast<double>(v[kLogT60Low])); }
double Look::t60High() const { return std::exp(static_cast<double>(v[kLogT60High])); }

double sidesFor(double size01) { return std::clamp(std::exp(std::clamp(size01, 0.0, 1.0) * 3.258096538) - 1.0 + 3.0, 3.0, 28.0); }

Look lookFor(const graphmodel::ReverbView& rv) {
  Look l;
  const double size = std::clamp(rv.size, 0.0, 1.0);
  l[kSides] = f(sidesFor(size));
  l[kTwist] = f(size * 0.785);
  l[kRadius] = f(std::clamp(rv.diffusion, 0.0, 1.0));
  l[kRoom] = f(size);
  l[kStretch] = f(0.72 + 0.28 * std::clamp(rv.spread, 0.0, 2.0));
  const bool frozen = rv.freeze != 0;
  const double mid = std::max(1e-6, rv.rt60);
  const double hi = frozen ? 1.0 : std::clamp(rv.rt60High / mid, 0.0, 1.0);
  l[kBright] = f(std::clamp(0.75 * std::pow(hi, 0.6) + 0.25 * (0.5 + 0.5 * std::clamp(rv.color, -1.0, 1.0)), 0.0, 1.0));
  const double lo = frozen ? 1.0 : std::clamp(rv.rt60Low / mid, 1.0 / 3.0, 3.0);
  l[kBass] = f(0.5 + 0.5 * std::log(lo) / std::log(3.0));
  // Output levels on each network's own scale (full: the level curve at the slider's top).
  const double levelTop = rv.model == 2 ? 0.34771 : 1.0;
  l[kWet] = f(std::sqrt(std::clamp(rv.wet / levelTop, 0.0, 1.0)));
  const double erN = std::sqrt(std::clamp(rv.er / levelTop, 0.0, 1.0));
  l[kLift] = f(1.0 - std::exp(-std::max(0.0, rv.preSec) / 0.12));
  l[kMod] = f(std::clamp(rv.modDepth, 0.0, 1.0));
  l[kFreeze] = frozen ? 1.f : 0.f;
  auto lg = [](double t) { return f(std::log(std::clamp(t, kMinT60, kMaxT60))); };
  l[kLogT60] = lg(frozen ? kMaxT60 : rv.rt60);
  l[kLogT60Low] = lg(frozen ? kMaxT60 : rv.rt60Low);
  l[kLogT60High] = lg(frozen ? kMaxT60 : rv.rt60High);
  l[kPre] = f(std::max(0.0, rv.preSec));
  double gmax = 1e-9;
  for (int e = 0; e < rv.erCount; ++e) gmax = std::max(gmax, rv.erGain[static_cast<size_t>(e)]);
  const int n = std::min(rv.erCount, kMaxTaps);
  for (int e = 0; e < kMaxTaps; ++e) {
    l[kErSec0 + e] = e < n ? f(rv.erSec[static_cast<size_t>(e)]) : 0.f;
    l[kErLvl0 + e] = e < n ? f(erN * rv.erGain[static_cast<size_t>(e)] / gmax) : 0.f;
  }
  return l;
}

bool Ease::step(const Look& target, double dt) {
  if (!primed) {
    cur = target, primed = true;
    return false;
  }
  const float a = f(1.0 - std::exp(-std::max(0.0, dt) / kTau));
  bool moving = false;
  for (int i = 0; i < kFieldCount; ++i) {
    float& c = cur[i];
    const float t = target[i];
    // A tap that appears takes its time at once and fades in (no sliding in from 0).
    if (i >= kErSec0 && i < kErLvl0 && cur[i - kErSec0 + kErLvl0] <= 1e-4f) {
      c = t;
      continue;
    }
    const float d = t - c;
    if (std::fabs(d) <= 1e-4f * std::max(1.f, std::fabs(t))) {
      c = t;
      continue;
    }
    c += d * a;
    moving = true;
  }
  return moving;
}

double rippleSec(int k) { return 0.12 * std::pow(2.2, k); }
double rippleLevel(int k, double t60, bool freeze) { return freeze ? 1.0 : static_cast<double>(decayLevel(rippleSec(k), t60, false)); }

void build(const Look& l, float x0, float y0, float x1, float y1, Geometry& g) {
  const Cam cam(x0, y0, x1, y1);
  g.x0 = x0, g.y0 = y0, g.x1 = x1, g.y1 = y1;
  g.unit = std::max(0.5f, std::min(x1 - x0, y1 - y0) / 100.f);
  const double stretch = std::clamp(static_cast<double>(l[kStretch]), 0.5, kMaxStretch);
  double r, h;
  prism(l[kRadius], l[kRoom], r, h);
  const double yb = kMaxLift * std::clamp(static_cast<double>(l[kLift]), 0.0, 1.0), yt = yb + h;
  const double sides = std::clamp(static_cast<double>(l[kSides]), 3.0, static_cast<double>(kMaxSides));
  const int m = std::clamp(static_cast<int>(std::ceil(sides - 1e-6)), 3, kMaxSides);
  const double frac = std::clamp(sides - (m - 1), 0.0, 1.0);
  const double step = 2 * kPi / sides, twist = l[kTwist];
  // Ring vertices.
  Pt top[kMaxSides], bot[kMaxSides];
  for (int i = 0; i < m; ++i) {
    const double ab = kYaw0 + i * step, at = ab + twist;
    bot[i] = cam.project(std::cos(ab) * r * stretch, yb, std::sin(ab) * r);
    top[i] = cam.project(std::cos(at) * r * stretch, yt, std::sin(at) * r);
  }
  int n = 0;
  for (int i = 0; i < m; ++i) {
    const int j = (i + 1) % m;
    g.segs[static_cast<size_t>(n++)] = Seg{top[i], top[j], 1.f, 0};
    g.segs[static_cast<size_t>(n++)] = Seg{bot[i], bot[j], 1.f, 1};
    g.segs[static_cast<size_t>(n++)] = Seg{bot[i], top[i], i == m - 1 ? f(frac) : 1.f, 2};
  }
  g.segCount = n;
  // Far to near (insertion sort on the mean depth: fixed storage, ≤ 84 edges).
  auto key = [](const Seg& s) { return s.a.depth + s.b.depth; };
  for (int i = 1; i < n; ++i) {
    Seg s = g.segs[static_cast<size_t>(i)];
    int j = i - 1;
    while (j >= 0 && key(g.segs[static_cast<size_t>(j)]) < key(s)) {
      g.segs[static_cast<size_t>(j + 1)] = g.segs[static_cast<size_t>(j)];
      --j;
    }
    g.segs[static_cast<size_t>(j + 1)] = s;
  }
  g.capCount = m;
  for (int i = 0; i < m; ++i) g.cap[static_cast<size_t>(i)] = top[i], g.base[static_cast<size_t>(i)] = bot[i];
  // Floor: shadow under the base, ripples at fixed times after the pre-delay.
  for (int p = 0; p < kRipplePts; ++p) {
    const double a = 2 * kPi * p / kRipplePts;
    g.shadow[static_cast<size_t>(p)] = cam.project(std::cos(a) * r * 1.12 * stretch, 0, std::sin(a) * r * 1.12);
  }
  const double rIn = r * 1.1 + 0.06, rOut = 0.96 * kFloorR, mod = std::clamp(static_cast<double>(l[kMod]), 0.0, 1.0);
  const double t60 = l.t60();
  for (int k = 0; k < kRipples; ++k) {
    const double sec = rippleSec(k), rr = rIn + floorU(sec) * (rOut - rIn);
    g.rippleSec[static_cast<size_t>(k)] = f(sec);
    // Freeze eases in through the level (kFreeze is eased too).
    const double lv = decayLevel(sec, t60, false);
    g.rippleLevel[static_cast<size_t>(k)] = f(lv + (1.0 - lv) * std::clamp(static_cast<double>(l[kFreeze]), 0.0, 1.0));
    for (int p = 0; p < kRipplePts; ++p) {
      const double a = 2 * kPi * p / kRipplePts;
      const double wob = 1.0 + 0.025 * mod * std::sin(a * 7 + k * 1.3);
      g.ripple[static_cast<size_t>(k)][static_cast<size_t>(p)] = cam.project(std::cos(a) * rr * wob * stretch, 0, std::sin(a) * rr * wob);
    }
  }
  for (int e = 0; e < kMaxTaps; ++e) {
    const double sec = l[kErSec0 + e], rr = r * 1.12 + floorU(sec) * (rOut - r * 1.12);
    const double a = kYaw0 + 0.6 + e * 2.39996323;  // golden angle: taps spread around the room
    g.spark[static_cast<size_t>(e)] = cam.project(std::cos(a) * rr * stretch, 0, std::sin(a) * rr);
    g.sparkLevel[static_cast<size_t>(e)] = std::clamp(l[kErLvl0 + e], 0.f, 1.f);
  }
}

float decayLevel(double secAfterPre, double t60, bool freeze) {
  if (freeze || secAfterPre <= 0) return 1.f;
  return f(std::clamp(1.0 - secAfterPre / std::max(1e-3, t60), 0.0, 1.0));
}

double decaySpan(const Look& l) {
  // Frozen (eased 0..1): the tail holds, a fixed 2 s window.
  const double fz = std::clamp(static_cast<double>(l[kFreeze]), 0.0, 1.0);
  const double longest = std::min(30.0, std::max({l.t60(), l.t60Low(), l.t60High()})) * (1 - fz) + 2.0 * fz;
  double erEnd = 0;
  for (int e = 0; e < kMaxTaps; ++e)
    if (l[kErLvl0 + e] > 1e-3f) erEnd = std::max(erEnd, static_cast<double>(l[kErSec0 + e]));
  return std::clamp(std::max({0.5, static_cast<double>(l[kPre]) + longest * 1.1, erEnd * 1.25}), 0.5, 40.0);
}

void buildDecay(const Look& l, Decay& d) {
  const double span = decaySpan(l), pre = std::max(0.0, static_cast<double>(l[kPre]));
  const float fz = std::clamp(l[kFreeze], 0.f, 1.f);
  d.span = f(span), d.pre = f(pre);
  for (int i = 0; i <= kDecayPts; ++i) {
    const double t = span * i / kDecayPts - pre;
    const size_t k = static_cast<size_t>(i);
    auto lv = [&](double t60) { const float v = decayLevel(t, t60, false); return v + (1.f - v) * fz; };
    d.lo[k] = lv(l.t60Low()), d.mid[k] = lv(l.t60()), d.hi[k] = lv(l.t60High());
  }
}

}  // namespace gl::roomviz
