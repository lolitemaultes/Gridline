// Gridline - J-99: smooth freehand drawing (see DrawSmooth.h).
#include "DrawSmooth.h"

#include <algorithm>
#include <cmath>

namespace gl::drawsmooth {

namespace {

constexpr double kPi = 3.14159265358979323846;

double alphaFor(double cutoff, double dt) {
  const double tau = 1.0 / (2.0 * kPi * cutoff);
  return 1.0 / (1.0 + tau / dt);
}

double centre(const Axis& axis, int n, int i) {
  if (static_cast<int>(axis.size()) == n) return axis[static_cast<size_t>(i)];
  return (i + 0.5) / n;
}

}  // namespace

double OneEuro::filter(double x, double t) {
  if (!primed_) {
    primed_ = true;
    x_ = x;
    dx_ = 0;
    t_ = t;
    return x_;
  }
  const double dt = std::max(t - t_, 1e-4);
  t_ = t;
  const double dx = (x - x_) / dt;
  dx_ += alphaFor(dCutoff_, dt) * (dx - dx_);
  const double cutoff = minCutoff_ + beta_ * std::fabs(dx_);
  x_ += alphaFor(cutoff, dt) * (x - x_);
  return x_;
}

int cellAt(const Axis& axis, int n, double x) {
  if (n <= 0) return -1;
  if (static_cast<int>(axis.size()) != n) return std::clamp(static_cast<int>(std::floor(x * n)), 0, n - 1);
  // Nearest centre (cells end halfway between neighbouring centres).
  const auto it = std::lower_bound(axis.begin(), axis.end(), x);
  int i = static_cast<int>(it - axis.begin());
  if (i >= n) return n - 1;
  if (i > 0 && x - axis[static_cast<size_t>(i - 1)] < axis[static_cast<size_t>(i)] - x) --i;
  return i;
}

int fillSegment(std::vector<float>& vals, std::vector<uint8_t>& touched, const Axis& axis, double x0, double v0, double x1, double v1) {
  const int n = static_cast<int>(vals.size());
  if (n <= 0 || static_cast<int>(touched.size()) != n) return 0;
  const int i0 = cellAt(axis, n, x0), i1 = cellAt(axis, n, x1);
  const int lo = std::min(i0, i1), hi = std::max(i0, i1);
  const double span = x1 - x0;
  for (int i = lo; i <= hi; ++i) {
    const double t = std::fabs(span) < 1e-12 ? 1.0 : std::clamp((centre(axis, n, i) - x0) / span, 0.0, 1.0);
    vals[static_cast<size_t>(i)] = static_cast<float>(v0 + (v1 - v0) * t);
    touched[static_cast<size_t>(i)] = 1;
  }
  return hi - lo + 1;
}

double sigmaCells(int n, double amount) {
  if (amount <= 0 || n < 3) return 0;
  return std::min(12.0, std::clamp(amount, 0.0, 1.0) * (1.6 + n / 64.0));
}

void smoothTouched(const std::vector<float>& in, const std::vector<uint8_t>& touched, std::vector<float>& out, const Axis& axis,
                   double amount, bool cyclic, float range) {
  out = in;
  const int n = static_cast<int>(in.size());
  const double sc = sigmaCells(n, amount);
  if (sc <= 0 || static_cast<int>(touched.size()) != n) return;
  const bool uniform = static_cast<int>(axis.size()) != n;
  const double spacing = uniform ? 1.0 / n : (axis.back() - axis.front()) / std::max(1, n - 1);
  const double sx = std::max(sc * spacing, 1e-9);
  const double sr = std::max(1e-6, std::fabs(range) * (0.12 + 0.2 * std::clamp(amount, 0.0, 1.0)));
  const double ix2 = 1.0 / (2 * sx * sx), ir2 = 1.0 / (2 * sr * sr);
  const int reach = std::max(1, static_cast<int>(std::ceil(3 * sc)));
  for (int i = 0; i < n; ++i) {
    if (!touched[static_cast<size_t>(i)]) continue;
    if (!cyclic && (i == 0 || i == n - 1)) continue;  // tables keep their end points
    const double ci = centre(axis, n, i), vi = in[static_cast<size_t>(i)];
    double sw = 0, sv = 0;
    auto acc = [&](int j, double dist) {
      const double dv = in[static_cast<size_t>(j)] - vi;
      const double w = std::exp(-dist * dist * ix2 - dv * dv * ir2);
      sw += w;
      sv += w * in[static_cast<size_t>(j)];
    };
    acc(i, 0);
    if (uniform) {
      const int r = cyclic ? std::min(reach, (n - 1) / 2) : reach;  // each neighbour once round a short cycle
      for (int d = 1; d <= r; ++d)
        for (int j : {i - d, i + d}) {
          if (cyclic) j = ((j % n) + n) % n;
          else if (j < 0 || j >= n) continue;
          acc(j, d * spacing);
        }
    } else if (!cyclic) {  // ascending axis: walk out until 3 sigma
      for (int j = i - 1; j >= 0 && ci - centre(axis, n, j) <= 3 * sx; --j) acc(j, centre(axis, n, j) - ci);
      for (int j = i + 1; j < n && centre(axis, n, j) - ci <= 3 * sx; ++j) acc(j, centre(axis, n, j) - ci);
    } else {
      for (int j = 0; j < n; ++j) {
        if (j == i) continue;
        double dist = centre(axis, n, j) - ci;
        dist -= std::round(dist);  // shortest way round the cycle
        if (std::fabs(dist) <= 3 * sx) acc(j, dist);
      }
    }
    if (sw > 0) out[static_cast<size_t>(i)] = static_cast<float>(sv / sw);
  }
}

bool touchedSpan(const std::vector<uint8_t>& touched, double span, double& xa, double& xb) {
  const int n = static_cast<int>(touched.size());
  int lo = -1, hi = -1;
  for (int i = 0; i < n; ++i)
    if (touched[static_cast<size_t>(i)]) {
      if (lo < 0) lo = i;
      hi = i;
    }
  if (lo < 0) return false;
  xa = span * lo / n;
  xb = span * (hi + 1) / n;
  return true;
}

std::vector<CurvePoint> spliceIntoCurve(const std::vector<CurvePoint>& pts, double span, const std::vector<float>& vals,
                                        const std::vector<uint8_t>& touched, double vA, double vB) {
  double xa = 0, xb = 0;
  const int n = static_cast<int>(touched.size());
  if (static_cast<int>(vals.size()) != n || !touchedSpan(touched, span, xa, xb)) return pts;
  std::vector<CurvePoint> out;
  for (const CurvePoint& p : pts)
    if (p.x < xa) out.push_back(p);
  if (out.empty() || out.back().x < xa) out.push_back({xa, vA});
  for (int i = 0; i < n; ++i) {
    const double c = span * (i + 0.5) / n;
    if (c > xa && c < xb) out.push_back({c, vals[static_cast<size_t>(i)]});  // untouched cells inside the span keep the stroke's copy of the old curve
  }
  if (xb < span || pts.empty() || pts.back().x > xb) out.push_back({xb, vB});
  for (const CurvePoint& p : pts)
    if (p.x > xb) out.push_back(p);
  return out;
}

void Stroke::begin(const float* values, int n, const Options& o, Axis axis) {
  opt_ = o;
  axis_ = static_cast<int>(axis.size()) == n ? std::move(axis) : Axis{};
  base_.assign(values, values + std::max(0, n));
  painted_ = base_;
  out_ = base_;
  touched_.assign(base_.size(), 0);
  const double a = std::clamp(o.amount, 0.0, 1.0);
  fv_ = OneEuro(10.0 - 7.0 * a, 10.0, 1.0);
  active_ = n > 0;
  hasLast_ = false;
}

void Stroke::add(double x, double v, double t) {
  if (!active_) return;
  x = std::clamp(x, 0.0, 1.0);
  v = std::clamp(v, static_cast<double>(std::min(opt_.lo, opt_.hi)), static_cast<double>(std::max(opt_.lo, opt_.hi)));
  double fv = v;
  if (opt_.raw || opt_.amount <= 0)
    fv_.reset();
  else
    fv = fv_.filter(v, t);
  if (hasLast_)
    fillSegment(painted_, touched_, axis_, lastX_, lastV_, x, fv);
  else
    fillSegment(painted_, touched_, axis_, x, fv, x, fv);
  hasLast_ = true;
  lastX_ = x;
  lastV_ = fv;
  recompute();
}

void Stroke::recompute() {
  if (opt_.raw || opt_.amount <= 0)
    out_ = painted_;
  else
    smoothTouched(painted_, touched_, out_, axis_, opt_.amount, opt_.cyclic, opt_.hi - opt_.lo);
}

}  // namespace gl::drawsmooth
