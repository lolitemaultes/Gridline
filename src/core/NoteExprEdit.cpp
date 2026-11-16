// Gridline — K-99: note expression lanes (see NoteExprEdit.h).
#include "NoteExprEdit.h"
#include <algorithm>
#include <cmath>
#include "NoteBend.h"

namespace gl::exprlane {

std::vector<BendPoint>& laneOf(Note& n, ExprDim d) {
  return d == ExprDim::Pressure ? n.pressure : d == ExprDim::Timbre ? n.timbre : n.bend;
}
const std::vector<BendPoint>& laneOf(const Note& n, ExprDim d) {
  return d == ExprDim::Pressure ? n.pressure : d == ExprDim::Timbre ? n.timbre : n.bend;
}

float valueAt(const Note& n, ExprDim d, double t) {
  const float v = static_cast<float>(bend::value(laneOf(n, d), t));
  return d == ExprDim::Timbre ? std::clamp(v + 0.5f, 0.0f, 1.0f) : d == ExprDim::Pressure ? std::clamp(v, 0.0f, 1.0f) : v;
}

void simplify(std::vector<BendPoint>& lane, float eps, int maxPoints) {
  if (std::all_of(lane.begin(), lane.end(), [](const BendPoint& p) { return std::fabs(p.semis) < 1e-4f; })) {
    lane.clear();
    return;
  }
  maxPoints = std::max(2, maxPoints);
  // Deviation of point i from the straight line through its neighbours (the line starts at 0 on
  // the note's start, as bend::value plays it). Curved points stay.
  auto dev = [&](size_t i) {
    const double ta = i == 0 ? 0.0 : lane[i - 1].tick, va = i == 0 ? 0.0 : lane[i - 1].semis;
    const double tb = lane[i + 1].tick, vb = lane[i + 1].semis;
    if (lane[i].curve != bend::Straight || lane[i + 1].curve != bend::Straight || lane[i].tension != 0 || lane[i + 1].tension != 0) return 1e9;
    const double span = tb - ta;
    const double lin = span <= 1e-9 ? vb : va + (vb - va) * (lane[i].tick - ta) / span;
    return std::fabs(lin - lane[i].semis);
  };
  for (double e = std::max(1e-6f, eps);; e *= 2.0) {
    for (bool removed = true; removed && lane.size() > 2;) {
      removed = false;
      size_t best = 0;
      double bestDev = e;
      for (size_t i = 0; i + 1 < lane.size(); ++i)
        if (const double d = dev(i); d <= bestDev) best = i, bestDev = d, removed = true;
      if (removed) lane.erase(lane.begin() + static_cast<std::ptrdiff_t>(best));
    }
    if (static_cast<int>(lane.size()) <= maxPoints || e > 4.0) break;
  }
  if (static_cast<int>(lane.size()) > maxPoints) lane.resize(static_cast<size_t>(maxPoints));
}

namespace {
// Replaces the lane over [a, b] (ticks after the note's start) with value(t) every `grain` ticks.
template <typename F>
void replaceSpan(std::vector<BendPoint>& lane, double a, double b, double grain, F value) {
  const std::vector<BendPoint> old = lane;
  std::vector<BendPoint> out;
  for (const auto& p : old)
    if (p.tick < a - 0.5) out.push_back(p);
  if (a > 0.5) out.emplace_back(static_cast<float>(a - 0.5), static_cast<float>(bend::value(old, a - 0.5)));
  grain = std::max(1.0, grain);
  for (double t = a;; t += grain) {
    const double tt = std::min(t, b);
    out.emplace_back(static_cast<float>(tt), value(tt));
    if (tt >= b) break;
  }
  out.emplace_back(static_cast<float>(b + 0.5), static_cast<float>(bend::value(old, b + 0.5)));
  for (const auto& p : old)
    if (p.tick > b + 0.5) out.push_back(p);
  lane.swap(out);
}

template <typename F>
bool eachNote(std::vector<Note>& notes, const std::vector<bool>& only, ExprDim d, double t0, double t1, double grain, F value) {
  const double lo = std::min(t0, t1), hi = std::max(t0, t1);
  const bool any = std::find(only.begin(), only.end(), true) != only.end();
  bool changed = false;
  for (size_t i = 0; i < notes.size(); ++i) {
    Note& n = notes[i];
    if (any && !(i < only.size() && only[i])) continue;
    const double s = static_cast<double>(n.start), e = s + static_cast<double>(n.length);
    if (hi < s || lo > e) continue;
    const double a = std::max(lo, s) - s, b = std::min(hi, e) - s;
    std::vector<BendPoint>& lane = laneOf(n, d);
    const std::vector<BendPoint> before = lane;
    replaceSpan(lane, a, b, grain, [&](double rel) { return value(s + rel); });
    simplify(lane, 0.002f, bend::kMaxPoints);
    changed = changed || !(lane == before);
  }
  return changed;
}
}  // namespace

bool paint(std::vector<Note>& notes, const std::vector<bool>& only, ExprDim d, double t0, float f0, double t1, float f1,
           double grain) {
  f0 = std::clamp(f0, 0.0f, 1.0f), f1 = std::clamp(f1, 0.0f, 1.0f);
  return eachNote(notes, only, d, t0, t1, grain, [&](double t) {
    const double span = t1 - t0;
    const double f = std::fabs(span) < 1e-9 ? f1 : f0 + (f1 - f0) * std::clamp((t - t0) / span, 0.0, 1.0);
    return stored(d, static_cast<float>(f));
  });
}

bool clear(std::vector<Note>& notes, const std::vector<bool>& only, ExprDim d, double t0, double t1) {
  return eachNote(notes, only, d, t0, t1, 1e9, [](double) { return 0.0f; });
}

}  // namespace gl::exprlane
