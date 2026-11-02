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

// K-28: see NoteBend.h.
SlidePlan toSlides(const std::vector<BendPoint>& b, double length, double tolerance) {
  SlidePlan plan;
  if (b.empty() || !(length > 0)) return plan;
  const std::vector<BendPoint> line = head(b, length);
  plan.start = value(line, 0.0);
  const int64_t end = std::max<int64_t>(1, std::llround(length));
  std::vector<SlidePiece> raw;
  double t0 = 0, v0 = plan.start;
  for (const BendPoint& p : line) {
    const double t1 = p.tick, v1 = p.semis;
    if (t1 <= 0) {  // the start value (already in plan.start)
      v0 = v1;
      continue;
    }
    if (t1 > length) break;
    if (std::fabs(t1 - std::round(t1)) > 1e-3) ++plan.rounded;
    const int64_t a = std::llround(t0), z = std::llround(t1);
    const double span = t1 - t0;
    auto segV = [&](double t) { return v0 + (v1 - v0) * shape(p.curve, p.tension, std::clamp((t - t0) / span, 0.0, 1.0)); };
    if (std::fabs(v1 - v0) < 1e-6) {
      // a hold: nothing to slide
    } else if (p.curve == Step || z <= a) {  // a jump at the point (or a ramp shorter than a tick)
      if (z < end) raw.push_back({z, 1, v1});
      plan.steps += p.curve == Step;
    } else if (!curved(p)) {
      raw.push_back({a, z - a, v1});
    } else {
      ++plan.curves;
      // K-29: the curve at whole ticks, thinned (Ramer-Douglas-Peucker) to the fewest ramps that
      // stay within the tolerance: cuts land where the curve bends most (a steep ease gets its
      // short ramps at its steep end), at most 32 per segment.
      const int64_t stride = std::max<int64_t>(1, (z - a) / 2048);
      std::vector<std::pair<double, double>> smp;
      for (int64_t c = a; c < z; c += stride) smp.push_back({static_cast<double>(c), segV(static_cast<double>(c))});
      smp.push_back({static_cast<double>(z), v1});
      std::vector<bool> keep;
      for (double tol = std::max(1e-4, tolerance);; tol *= 1.5) {
        keep.assign(smp.size(), false);
        keep.front() = keep.back() = true;
        thin(smp, 0, smp.size() - 1, tol, keep);
        if (std::count(keep.begin(), keep.end(), true) <= 33 || tol > 48) break;
      }
      int64_t prev = a;
      for (size_t i = 1; i < smp.size(); ++i)
        if (keep[i]) {
          const int64_t c = static_cast<int64_t>(smp[i].first);
          raw.push_back({prev, c - prev, i + 1 == smp.size() ? v1 : smp[i].second});
          prev = c;
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

}  // namespace gl::bend
