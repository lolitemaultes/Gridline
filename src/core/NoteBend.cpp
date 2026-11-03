// Gridline - K-25: per-note pitch bend lines (see NoteBend.h).
#include "NoteBend.h"
#include <algorithm>

namespace gl::bend {

bool flat(const std::vector<BendPoint>& b) {
  for (const auto& p : b)
    if (std::fabs(p.semis) > 1e-6f) return false;
  return true;
}

void normalize(std::vector<BendPoint>& b) {
  for (auto& p : b) {
    if (!std::isfinite(p.tick) || p.tick < 0) p.tick = 0;
    if (!std::isfinite(p.semis)) p.semis = 0;
    p.semis = std::clamp(p.semis, -kMaxSemis, kMaxSemis);
    if (p.curve >= CurveCount) p.curve = Straight;
    p.tension = std::isfinite(p.tension) ? std::clamp(p.tension, -1.0f, 1.0f) : 0.0f;  // K-28
  }
  std::stable_sort(b.begin(), b.end(), [](const BendPoint& x, const BendPoint& y) { return x.tick < y.tick; });
  b.erase(std::unique(b.begin(), b.end(), [](const BendPoint& x, const BendPoint& y) { return x.tick == y.tick && x.semis == y.semis; }),
          b.end());
  if (b.size() > static_cast<size_t>(kMaxPoints)) b.resize(static_cast<size_t>(kMaxPoints));
  if (flat(b)) b.clear();
}

std::vector<BendPoint> tail(const std::vector<BendPoint>& b, double at) {
  std::vector<BendPoint> out;
  if (b.empty()) return out;
  const double v = value(b, at);
  if (std::fabs(v) > 1e-6) out.push_back({0.0f, static_cast<float>(v), Straight});
  for (const auto& p : b)
    if (p.tick > at) out.push_back({static_cast<float>(p.tick - at), p.semis, p.curve, p.tension});
  normalize(out);
  return out;
}

std::vector<BendPoint> head(const std::vector<BendPoint>& b, double at) {
  std::vector<BendPoint> out;
  for (const auto& p : b)
    if (p.tick <= at) out.push_back(p);
  if (out.size() < b.size()) {  // a later point shaped the line up to `at`: close it there
    const double v = value(b, at), last = out.empty() ? 0.0 : out.back().semis;
    if (std::fabs(v - last) > 1e-6) out.push_back({static_cast<float>(at), static_cast<float>(v), Straight});
  }
  normalize(out);
  return out;
}

void split(const Note& n, Tick cut, Note& left, Note& right) {
  const double at = static_cast<double>(cut);
  left.bend = head(n.bend, at);
  right.bend = tail(n.bend, at);
}

void append(std::vector<BendPoint>& a, const std::vector<BendPoint>& b, double offset) {
  if (a.empty() && b.empty()) return;
  const double va = value(a, offset), vb = value(b, 0.0);
  a.erase(std::remove_if(a.begin(), a.end(), [&](const BendPoint& p) { return p.tick >= offset; }), a.end());
  a.push_back({static_cast<float>(offset), static_cast<float>(va), Straight});
  a.push_back({static_cast<float>(offset), static_cast<float>(vb), Step});  // b's start (a jump when they differ)
  for (const auto& p : b)
    if (p.tick > 0) a.push_back({static_cast<float>(p.tick + offset), p.semis, p.curve, p.tension});
  normalize(a);
}

bool removePoint(std::vector<BendPoint>& b, size_t k) {
  if (k >= b.size()) return false;
  b.erase(b.begin() + static_cast<std::ptrdiff_t>(k));
  normalize(b);
  return true;
}

bool resetCurve(std::vector<BendPoint>& b, size_t k) {
  if (k >= b.size() || (b[k].curve == Straight && b[k].tension == 0.0f)) return false;
  b[k].curve = Straight;
  b[k].tension = 0.0f;
  return true;
}

int segmentAt(const std::vector<BendPoint>& b, double t) {
  for (size_t k = 0; k < b.size(); ++k)
    if (t < b[k].tick) return static_cast<int>(k);
  return -1;
}

void rescale(std::vector<BendPoint>& b, int fromPpq, int toPpq) {
  if (fromPpq <= 0 || toPpq <= 0 || fromPpq == toPpq) return;
  const double k = static_cast<double>(toPpq) / fromPpq;
  for (auto& p : b) p.tick = static_cast<float>(p.tick * k);
}

namespace {
// Ramer-Douglas-Peucker on (t, v): keeps the points further than `tol` (in v) from the chord.
void thin(const std::vector<std::pair<double, double>>& s, size_t a, size_t z, double tol, std::vector<bool>& keep) {
  if (z <= a + 1) return;
  double worst = -1;
  size_t at = a;
  const double ta = s[a].first, va = s[a].second, tz = s[z].first, vz = s[z].second;
  for (size_t i = a + 1; i < z; ++i) {
    const double span = tz - ta;
    const double on = span > 1e-9 ? va + (vz - va) * (s[i].first - ta) / span : va;
    const double d = std::fabs(s[i].second - on) + (span <= 1e-9 ? std::fabs(s[i].second - vz) : 0.0);
    if (d > worst) worst = d, at = i;
  }
  if (worst <= tol) return;
  keep[at] = true;
  thin(s, a, at, tol, keep);
  thin(s, at, z, tol, keep);
}
}  // namespace

std::vector<BendPoint> fromSamples(const std::vector<std::pair<double, double>>& samples, double tolerance, double rampGap) {
  if (samples.empty()) return {};
  // Each sample holds until the next one: a staircase (two points per change), which the
  // thinning turns into ramps where the samples are dense and keeps as jumps where they are not.
  std::vector<std::pair<double, double>> s;
  double prev = 0;
  for (size_t i = 0; i < samples.size(); ++i) {
    const double t = std::max(0.0, samples[i].first), v = std::clamp(samples[i].second, -static_cast<double>(kMaxSemis), static_cast<double>(kMaxSemis));
    if (i == 0) {
      if (t > 0) s.push_back({0.0, 0.0}), s.push_back({t, 0.0});
    } else {
      if (t - samples[i - 1].first > rampGap) s.push_back({t, prev});  // held until here (a jump), unless densely sampled
    }
    s.push_back({t, v});
    prev = v;
  }
  std::vector<BendPoint> out;
  for (double tol = std::max(1e-4, tolerance);; tol *= 2) {
    std::vector<bool> keep(s.size(), false);
    keep.front() = keep.back() = true;
    thin(s, 0, s.size() - 1, tol, keep);
    out.clear();
    for (size_t i = 0; i < s.size(); ++i)
      if (keep[i]) out.push_back({static_cast<float>(s[i].first), static_cast<float>(s[i].second), Straight});
    if (!out.empty() && out.front().tick == 0 && out.front().semis == 0) out.erase(out.begin());  // the implicit start
    if (out.size() <= static_cast<size_t>(kMaxPoints) || tol > 96) break;
  }
  normalize(out);
  return out;
}

namespace {
// K-AB: one pass of toSlides at one tolerance.
SlidePlan planSlides(const std::vector<BendPoint>& line, double length, double tolerance, double grid, double base) {
  SlidePlan plan;
  auto snap = [&](double v) { return grid > 0 ? std::round((base + v) / grid) * grid - base : v; };
  const int64_t end = std::max<int64_t>(1, std::llround(length));
  std::vector<SlidePiece> raw;
  double t0 = 0, v0 = value(line, 0.0);
  plan.start = snap(v0);
  double at = plan.start;  // where the slides have got to (on the grid)
  plan.offGrid = grid > 0 && std::fabs(at - v0) > 0.005;
  for (const BendPoint& p : line) {
    const double t1 = p.tick, v1 = p.semis;
    if (t1 <= 0) {  // the start value (already in plan.start)
      v0 = v1;
      continue;
    }
    if (t1 > length) break;
    if (std::fabs(t1 - std::round(t1)) > 1e-3) ++plan.rounded;
    const int64_t a = std::llround(t0), z = std::llround(t1);
    const double span = t1 - t0, landing = snap(v1);
    if (grid > 0 && !curved(p) && std::fabs(landing - v1) > 0.005) plan.offGrid = true;
    auto segV = [&](double t) { return v0 + (v1 - v0) * shape(p.curve, p.tension, std::clamp((t - t0) / span, 0.0, 1.0)); };
    if (std::fabs(v1 - v0) < 1e-6) {
      // a hold: nothing to slide
    } else if (p.curve == Step || z <= a) {  // a jump at the point (or a ramp shorter than a tick)
      if (z < end && landing != at) raw.push_back({z, 1, landing});
      at = landing;
      plan.steps += p.curve == Step;
    } else if (!curved(p)) {
      if (landing != at) raw.push_back({a, z - a, landing});
      at = landing;
    } else {
      ++plan.curves;
      // From where the last ramp landed, the longest ramp that stays within the tolerance. Doubling
      // then halving: few ends are tried, each at few places, so the work doesn't grow with length.
      int64_t cur = a;
      while (cur < z) {
        auto fits = [&](int64_t c, double& target) {
          const double exact = c == z ? v1 : segV(static_cast<double>(c));
          double tries[2] = {snap(exact), snap(exact)};
          if (grid > 0 && c < z) tries[1] = tries[0] + (tries[0] < exact ? grid : -grid);  // the other neighbour may fit better
          const int64_t len = c - cur;
          const int checks = static_cast<int>(std::min<int64_t>(len * 2, 64));
          for (const double q : tries) {
            double worst = std::fabs(q - exact);
            for (int i = 1; i < checks && worst <= tolerance; ++i) {
              const double f = static_cast<double>(i) / checks;
              worst = std::max(worst, std::fabs(at + (q - at) * f - segV(static_cast<double>(cur) + f * static_cast<double>(len))));
            }
            if (worst <= tolerance) {
              target = q;
              return true;
            }
          }
          return false;
        };
        double target = cur + 1 == z ? landing : snap(segV(static_cast<double>(cur + 1)));  // one tick always goes
        int64_t lo = 1, hi = 0;
        for (int64_t len = 2; lo < z - cur; len *= 2) {
          const int64_t tryLen = std::min(len, z - cur);
          double q = 0;
          if (!fits(cur + tryLen, q)) {
            hi = tryLen;
            break;
          }
          lo = tryLen, target = q;
        }
        while (hi > lo + 1) {
          const int64_t mid = lo + (hi - lo) / 2;
          double q = 0;
          if (fits(cur + mid, q)) lo = mid, target = q;
          else hi = mid;
        }
        if (target != at) raw.push_back({cur, lo, target});  // a ramp to where it already is: a hold
        at = target;
        cur += lo;
      }
    }
    t0 = t1, v0 = v1;
  }
  // Rounding can make neighbours touch (a step and the ramp after it on one tick): a later piece
  // starts once the earlier one has arrived; one swallowed whole only changes its target.
  for (SlidePiece pc : raw) {
    if (pc.tick >= end) continue;
    if (!plan.pieces.empty()) {
      SlidePiece& last = plan.pieces.back();
      const int64_t lastEnd = last.tick + last.length, pcEnd = pc.tick + pc.length;
      if (pc.tick < lastEnd) {
        if (pcEnd <= lastEnd) {
          last.target = pc.target;
          continue;
        }
        if (pc.tick > last.tick) {
          last.length = pc.tick - last.tick;
        } else {
          pc.tick = lastEnd;
          pc.length = pcEnd - lastEnd;
        }
      }
    }
    plan.pieces.push_back(pc);
  }
  return plan;
}
}  // namespace

// K-28, K-AB: see NoteBend.h.
SlidePlan toSlides(const std::vector<BendPoint>& b, double length, double tolerance, double grid, double base) {
  if (b.empty() || !(length > 0)) return {};
  const std::vector<BendPoint> line = head(b, length);
  bool relaxed = false;
  SlidePlan plan;
  for (double tol = std::max(1e-4, tolerance);; tol *= 1.25) {
    plan = planSlides(line, length, tol, grid, base);
    if (static_cast<int>(plan.pieces.size()) <= kMaxSlidesPerLine || tol > 96) break;  // past 96: only the line's own points are left
    relaxed = true;
  }
  plan.relaxed = relaxed;
  return plan;
}

}  // namespace gl::bend
