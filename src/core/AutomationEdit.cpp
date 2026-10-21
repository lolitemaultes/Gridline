// Gridline — automation point editing (see AutomationEdit.h).
#include "AutomationEdit.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include "Automation.h"

namespace gl::autoedit {

namespace {
constexpr double kPi = 3.14159265358979323846;

void sortStable(Points& pts) {
  std::stable_sort(pts.begin(), pts.end(), [](const AutoPoint& x, const AutoPoint& y) { return x.pos < y.pos; });
}
bool inSel(const Selection& sel, size_t i) { return sel.empty() || sel.count(static_cast<int>(i)) > 0; }
// Shapes whose time reversal is the same shape with the tension negated.
bool reversesByTension(AutoShape s) {
  switch (s) {
    case AutoShape::Curve: case AutoShape::Curve2: case AutoShape::Curve3: case AutoShape::Smooth: case AutoShape::HalfSine:
      return true;
    default: return false;
  }
}
// Removes the points with pos in [from, to].
void eraseRange(Points& pts, Tick from, Tick to) {
  pts.erase(std::remove_if(pts.begin(), pts.end(), [&](const AutoPoint& p) { return p.pos >= from && p.pos <= to; }), pts.end());
}
}  // namespace

// ------------------------------------------------------------------ modes ----

const std::vector<AutoShape>& menuOrder() {
  static const std::vector<AutoShape> order = {AutoShape::Smooth,   AutoShape::Hold,     AutoShape::Curve,   AutoShape::Curve2,
                                               AutoShape::Curve3,   AutoShape::SCurve,   AutoShape::SCurve2, AutoShape::SCurve3,
                                               AutoShape::HalfSine, AutoShape::Stairs,   AutoShape::SmoothStairs,
                                               AutoShape::Pulse,    AutoShape::Wave};
  return order;
}

// FL mode bytes (historical menu order): 0 single curve, 1 double curve, 2 hold,
// 3 stairs, 4 smooth stairs, 5 pulse, 6 wave, 7 single curve 2, 8 double curve 2,
// 9 half sine, 10 smooth, 11 single curve 3, 12 double curve 3.
AutoShape shapeFromFlMode(int mode) {
  switch (mode) {
    case 1: return AutoShape::SCurve;
    case 2: return AutoShape::Hold;
    case 3: return AutoShape::Stairs;
    case 4: return AutoShape::SmoothStairs;
    case 5: return AutoShape::Pulse;
    case 6: return AutoShape::Wave;
    case 7: return AutoShape::Curve2;
    case 8: return AutoShape::SCurve2;
    case 9: return AutoShape::HalfSine;
    case 10: return AutoShape::Smooth;
    case 11: return AutoShape::Curve3;
    case 12: return AutoShape::SCurve3;
    default: return AutoShape::Curve;
  }
}

int flModeOf(AutoShape s) {
  for (int m = 0; m <= 12; ++m)
    if (shapeFromFlMode(m) == s) return m;
  return 0;
}

const char* shapeKey(AutoShape s) {
  switch (s) {
    case AutoShape::SCurve: return "scurve";
    case AutoShape::Hold: return "hold";
    case AutoShape::Stairs: return "stairs";
    case AutoShape::Smooth: return "smooth";
    case AutoShape::Pulse: return "pulse";
    case AutoShape::Wave: return "wave";
    case AutoShape::Curve2: return "curve2";
    case AutoShape::Curve3: return "curve3";
    case AutoShape::SCurve2: return "scurve2";
    case AutoShape::SCurve3: return "scurve3";
    case AutoShape::HalfSine: return "halfsine";
    case AutoShape::SmoothStairs: return "smoothstairs";
    default: return "curve";
  }
}

AutoShape shapeFromKey(const std::string& key) {
  for (int k = 0; k < static_cast<int>(AutoShape::Count); ++k)
    if (key == shapeKey(static_cast<AutoShape>(k))) return static_cast<AutoShape>(k);
  return AutoShape::Curve;
}

double valueAt(const Points& p, double t, bool fl) { return pointsValue(p, t, fl); }

// ------------------------------------------------------------------ edits ----

int addPoint(Points& pts, const AutoPoint& p) {
  auto it = std::upper_bound(pts.begin(), pts.end(), p.pos, [](Tick v, const AutoPoint& q) { return v < q.pos; });
  return static_cast<int>(pts.insert(it, p) - pts.begin());
}

int deletePoints(Points& pts, const Selection& sel, size_t keep) {
  std::vector<int> rm;
  for (int i : sel)
    if (i >= 0 && static_cast<size_t>(i) < pts.size()) rm.push_back(i);
  while (!rm.empty() && pts.size() - rm.size() < keep) rm.erase(rm.begin());
  for (auto it = rm.rbegin(); it != rm.rend(); ++it) pts.erase(pts.begin() + *it);
  return static_cast<int>(rm.size());
}

Tick movePoints(const Points& base, const Selection& sel, Tick dt, double dv, Points& out) {
  out = base;
  if (sel.empty()) return 0;
  Tick lo = std::numeric_limits<Tick>::min() / 4, hi = std::numeric_limits<Tick>::max() / 4;
  const int n = static_cast<int>(base.size());
  for (int i : sel) {
    if (i < 0 || i >= n) continue;
    int l = i - 1, r = i + 1;
    while (l >= 0 && sel.count(l)) --l;
    while (r < n && sel.count(r)) ++r;
    Tick left = l >= 0 ? base[static_cast<size_t>(l)].pos : 0;
    lo = std::max(lo, left - base[static_cast<size_t>(i)].pos);
    if (r < n) hi = std::min(hi, base[static_cast<size_t>(r)].pos - base[static_cast<size_t>(i)].pos);
  }
  dt = std::clamp(dt, std::min(lo, Tick{0}), std::max(hi, Tick{0}));
  for (int i : sel) {
    if (i < 0 || i >= n) continue;
    AutoPoint& p = out[static_cast<size_t>(i)];
    p.pos += dt;
    p.value = static_cast<float>(std::clamp(static_cast<double>(p.value) + dv, 0.0, 1.0));
  }
  return dt;
}

Selection quantize(Points& pts, const Selection& sel, Tick grid) {
  Selection outSel;
  if (grid <= 0 || pts.empty()) return sel;
  struct Tagged {
    AutoPoint p;
    bool selected;
  };
  std::vector<Tagged> v;
  for (size_t i = 0; i < pts.size(); ++i) {
    Tagged t{pts[i], inSel(sel, i)};
    if (t.selected) t.p.pos = std::max<Tick>(0, (t.p.pos + grid / 2) / grid * grid);
    v.push_back(t);
  }
  std::stable_sort(v.begin(), v.end(), [](const Tagged& x, const Tagged& y) { return x.p.pos < y.p.pos; });
  // Same tick after snapping: the last point wins (a selected point beats an unselected one).
  std::vector<Tagged> keep;
  for (const auto& t : v) {
    if (!keep.empty() && keep.back().p.pos == t.p.pos && (keep.back().selected || t.selected)) {
      if (t.selected || !keep.back().selected) keep.back() = t;
      continue;
    }
    keep.push_back(t);
  }
  pts.clear();
  for (size_t i = 0; i < keep.size(); ++i) {
    pts.push_back(keep[i].p);
    if (keep[i].selected && !sel.empty()) outSel.insert(static_cast<int>(i));
  }
  return outSel;
}

void flipVertical(Points& pts, const Selection& sel) {
  for (size_t i = 0; i < pts.size(); ++i)
    if (inSel(sel, i)) pts[i].value = 1.0f - pts[i].value;
}

void flipHorizontal(Points& pts, const Selection& sel) {
  if (pts.size() < 2) return;
  size_t i0 = 0, i1 = pts.size() - 1;
  if (!sel.empty()) {
    i0 = static_cast<size_t>(std::max(0, *sel.begin()));
    i1 = std::min(pts.size() - 1, static_cast<size_t>(std::max(0, *sel.rbegin())));
  }
  if (i1 <= i0) return;
  const Points old = pts;
  const Tick lo = old[i0].pos, hi = old[i1].pos;
  for (size_t j = 0; j <= i1 - i0; ++j) {
    const AutoPoint& src = old[i1 - j];
    AutoPoint& dst = pts[i0 + j];
    dst.pos = lo + hi - src.pos;
    dst.value = src.value;
    if (j == 0) {  // the segment entering the range keeps its curve
      dst.shape = old[i0].shape;
      dst.tension = old[i0].tension;
    } else {  // segment (j-1 → j) is the old segment ending at old point i1 - j + 1, reversed
      const AutoPoint& seg = old[i1 - j + 1];
      dst.shape = seg.shape;
      dst.tension = reversesByTension(seg.shape) ? -seg.tension : seg.tension;
    }
  }
}

void scaleLevels(Points& pts, const Selection& sel, double centre, double multiplier, double offset) {
  for (size_t i = 0; i < pts.size(); ++i)
    if (inSel(sel, i))
      pts[i].value = static_cast<float>(std::clamp(centre + (pts[i].value - centre) * multiplier + offset, 0.0, 1.0));
}

void setShape(Points& pts, const Selection& sel, AutoShape s) {
  for (size_t i = 0; i < pts.size(); ++i)
    if (inSel(sel, i)) pts[i].shape = s;
}

void setTension(Points& pts, const Selection& sel, float tension) {
  for (size_t i = 0; i < pts.size(); ++i)
    if (inSel(sel, i)) pts[i].tension = std::clamp(tension, -1.0f, 1.0f);
}

Selection selectBox(const Points& pts, Tick t0, Tick t1, double v0, double v1) {
  if (t1 < t0) std::swap(t0, t1);
  if (v1 < v0) std::swap(v0, v1);
  Selection s;
  for (size_t i = 0; i < pts.size(); ++i)
    if (pts[i].pos >= t0 && pts[i].pos <= t1 && pts[i].value >= v0 && pts[i].value <= v1) s.insert(static_cast<int>(i));
  return s;
}

// -------------------------------------------------------------- clipboard ----

Points copyPoints(const Points& pts, const Selection& sel) {
  Points out;
  for (size_t i = 0; i < pts.size(); ++i)
    if (sel.count(static_cast<int>(i))) out.push_back(pts[i]);
  if (!out.empty()) {
    Tick base = out.front().pos;
    for (auto& p : out) p.pos -= base;
  }
  return out;
}

Tick clipSpan(const Points& clip) { return clip.empty() ? 1 : std::max<Tick>(1, clip.back().pos); }

Selection pastePoints(Points& pts, const Points& clip, Tick at, bool replace) {
  Selection sel;
  if (clip.empty()) return sel;
  at = std::max<Tick>(0, at);
  const Tick span = clipSpan(clip);
  if (replace) {
    eraseRange(pts, at, at + span);
  } else {
    for (auto& p : pts)
      if (p.pos >= at) p.pos += span;
  }
  std::vector<int> added;
  for (const auto& c : clip) {
    AutoPoint p = c;
    p.pos += at;
    pts.push_back(p);
  }
  // Re-sort, tracking the pasted points (they were appended at the end).
  std::vector<std::pair<AutoPoint, bool>> v;
  for (size_t i = 0; i < pts.size(); ++i) v.push_back({pts[i], i + clip.size() >= pts.size()});
  std::stable_sort(v.begin(), v.end(), [](const auto& x, const auto& y) { return x.first.pos < y.first.pos; });
  pts.clear();
  for (size_t i = 0; i < v.size(); ++i) {
    pts.push_back(v[i].first);
    if (v[i].second) sel.insert(static_cast<int>(i));
  }
  return sel;
}

// --------------------------------------------------------------- thinning ----

Points thin(const Points& in, double tolerance) {
  if (in.size() <= 2) return in;
  std::vector<char> keep(in.size(), 0);
  keep[0] = 1;
  keep[in.size() - 1] = 1;
  // Iterative Ramer–Douglas–Peucker on (tick, value); the error is the
  // vertical distance to the chord.
  std::vector<std::pair<size_t, size_t>> stack{{0, in.size() - 1}};
  while (!stack.empty()) {
    auto [a, b] = stack.back();
    stack.pop_back();
    if (b <= a + 1) continue;
    const double t0 = static_cast<double>(in[a].pos), t1 = static_cast<double>(in[b].pos);
    double worst = -1;
    size_t at = a;
    for (size_t i = a + 1; i < b; ++i) {
      double f = t1 > t0 ? (static_cast<double>(in[i].pos) - t0) / (t1 - t0) : 0.0;
      double chord = in[a].value + (in[b].value - in[a].value) * f;
      double err = std::fabs(in[i].value - chord);
      if (err > worst) worst = err, at = i;
    }
    if (worst > tolerance) {
      keep[at] = 1;
      stack.push_back({a, at});
      stack.push_back({at, b});
    }
  }
  Points out;
  for (size_t i = 0; i < in.size(); ++i)
    if (keep[i]) {
      AutoPoint p = in[i];
      // A jump (two points on one tick) keeps its hold; everything else is a straight line.
      p.shape = !out.empty() && out.back().pos == p.pos ? AutoShape::Hold : AutoShape::Curve;
      p.tension = 0;
      out.push_back(p);
    }
  return out;
}

void mergeRecorded(Points& pts, Points recorded, Tick from, Tick to, double tolerance) {
  if (recorded.empty()) return;
  if (to < from) std::swap(from, to);
  for (auto& p : recorded) p.pos = std::clamp(p.pos, from, to);
  sortStable(recorded);
  recorded = thin(recorded, tolerance);
  eraseRange(pts, from, to);
  for (const auto& p : recorded) pts.push_back(p);
  sortStable(pts);
}

// -------------------------------------------------------------------- LFO ----

const char* lfoShapeName(LfoShape s) {
  switch (s) {
    case LfoShape::Sine: return "Sine";
    case LfoShape::Triangle: return "Triangle";
    case LfoShape::Square: return "Square";
    case LfoShape::SawUp: return "Saw up";
    case LfoShape::SawDown: return "Saw down";
    default: return "?";
  }
}

namespace {
double frac(double x) { return x - std::floor(x); }
// Waveform in [-1, 1] at phase φ (cycles).
double wave(LfoShape s, double ph, double pw) {
  double f = frac(ph);
  switch (s) {
    case LfoShape::Triangle: return f < 0.25 ? 4 * f : f < 0.75 ? 2 - 4 * f : 4 * f - 4;
    case LfoShape::Square: return f < pw ? 1.0 : -1.0;
    case LfoShape::SawUp: return 2 * f - 1;
    case LfoShape::SawDown: return 1 - 2 * f;
    default: return std::sin(2 * kPi * f);
  }
}
// Key phases (fractions of a cycle) where the LFO places points.
std::vector<double> keys(LfoShape s, double pw) {
  switch (s) {
    case LfoShape::Triangle: return {0.25, 0.75};
    case LfoShape::Square: return {0.0, pw};
    case LfoShape::SawUp: case LfoShape::SawDown: return {0.0};
    default: return {0.0, 0.25, 0.5, 0.75};
  }
}
// Sine segments are single curves at ±0.25 (a quarter sine, see Automation.cpp):
// the quarter that ends at phase q eases out (towards a peak) or in (away from it).
float sineTension(double q) {
  double f = frac(q + 1e-9);
  return (f < 0.01 || (f > 0.49 && f < 0.51)) ? 0.25f : -0.25f;  // ends at a zero crossing: slow start
}
}  // namespace

Points lfoPoints(const LfoSettings& s, Tick from, Tick to) {
  Points out;
  if (to <= from) return out;
  const double span = static_cast<double>(to - from);
  const double pw = std::clamp(s.pulseWidth, 0.02, 0.98);
  const double user = std::clamp(s.tension, -1.0, 1.0);
  auto lerp = [](double a, double b, double u) { return a + (b - a) * u; };
  auto valueAtPhase = [&](double ph, double u) {
    return std::clamp(lerp(s.centreStart, s.centreEnd, u) + lerp(s.amountStart, s.amountEnd, u) * wave(s.shape, ph, pw), 0.0, 1.0);
  };
  auto period = [&](double u) { return std::max(1.0, lerp(s.periodStart, s.periodEnd, u)); };
  const std::vector<double> ks = keys(s.shape, pw);
  auto add = [&](double t, double v, AutoShape sh, double ten) {
    AutoPoint p;
    p.pos = from + static_cast<Tick>(std::llround(t));
    p.value = static_cast<float>(v);
    p.shape = sh;
    p.tension = static_cast<float>(std::clamp(ten, -1.0, 1.0));
    out.push_back(p);
  };
  double ph = s.phase;
  add(0, valueAtPhase(ph, 0), AutoShape::Curve, 0);
  const double step = std::max(0.25, span / 20000.0);
  for (double t = 0; t < span;) {
    double dt = std::min(step, span - t);
    double u = (t + dt * 0.5) / span;
    double next = ph + dt / period(u);
    // Every key phase crossed in (ph, next].
    for (double n = std::floor(ph); n <= std::floor(next); n += 1.0)
      for (double k : ks) {
        double q = n + k;
        if (q <= ph || q > next) continue;
        double tk = t + dt * (q - ph) / (next - ph), uk = tk / span;
        switch (s.shape) {
          case LfoShape::Sine: add(tk, valueAtPhase(q, uk), AutoShape::Curve, sineTension(q) + user); break;
          case LfoShape::Triangle: add(tk, valueAtPhase(q, uk), AutoShape::Curve, user); break;
          case LfoShape::Square: add(tk, valueAtPhase(q + 1e-9, uk), AutoShape::Hold, 0); break;
          default: {  // saw: end of the ramp, then the jump back
            double c = lerp(s.centreStart, s.centreEnd, uk), amt = lerp(s.amountStart, s.amountEnd, uk);
            double top = s.shape == LfoShape::SawUp ? 1.0 : -1.0;
            add(tk, std::clamp(c + amt * top, 0.0, 1.0), AutoShape::Curve, user);
            add(tk, std::clamp(c - amt * top, 0.0, 1.0), AutoShape::Hold, 0);
            break;
          }
        }
      }
    ph = next;
    t += dt;
  }
  // Closing point at the end of the range.
  double tension = 0;
  AutoShape sh = AutoShape::Curve;
  if (s.shape == LfoShape::Sine) tension = sineTension(std::floor(ph * 4.0 + 1.0) / 4.0) + user;
  else if (s.shape == LfoShape::Square) sh = AutoShape::Hold;
  else tension = user;
  if (out.back().pos < to) add(span, valueAtPhase(ph, 1.0), sh, tension);
  return out;
}

void applyLfo(Points& pts, const LfoSettings& s, Tick from, Tick to) {
  Points l = lfoPoints(s, from, to);
  if (l.empty()) return;
  eraseRange(pts, from, to);
  for (const auto& p : l) pts.push_back(p);
  sortStable(pts);
}

// ----------------------------------------------------------------- shapes ----

void applyShape(Points& pts, const Points& shape, Tick from, Tick to) {
  if (shape.empty() || to <= from) return;
  const Tick lo = shape.front().pos, hi = std::max(shape.back().pos, lo + 1);
  const double scale = static_cast<double>(to - from) / static_cast<double>(hi - lo);
  eraseRange(pts, from, to);
  for (const auto& sp : shape) {
    AutoPoint p = sp;
    p.pos = from + static_cast<Tick>(std::llround(static_cast<double>(sp.pos - lo) * scale));
    pts.push_back(p);
  }
  sortStable(pts);
}

}  // namespace gl::autoedit
