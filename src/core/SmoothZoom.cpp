// Gridline - J-188: smooth zooming of horizontal timelines (see SmoothZoom.h).
#include "SmoothZoom.h"

#include <algorithm>
#include <cmath>

#include "SmoothSnap.h"

namespace gl::smoothzoom {

namespace {
// A fixed point farther out than this (a pure scroll with a hair of zoom) is
// no anchor worth gliding around: such a change applies at once.
constexpr double kMaxAnchorPx = 1e7;
bool usable(double z) { return z > 0 && std::isfinite(z); }
}  // namespace

void Glide::adopt(double z, double s) {
  active_ = false;
  z_ = Z_ = z;
  s_ = S_ = s;
}

void Glide::retarget(double z, double s, const Params& p) {
  if (!p.smooth || !usable(z) || !usable(z_) || !std::isfinite(s) || !std::isfinite(s_)) return adopt(z, s);
  const double r = std::log(z / z_);
  if (std::fabs(r) <= p.eps) return adopt(z, s);
  // The point that sits at the same pixel in the drawn view and in the target:
  // (A + s_) / z_ == (A + s) / z.
  const double A = (s / z - s_ / z_) / (1.0 / z_ - 1.0 / z);
  if (!std::isfinite(A) || std::fabs(A) > kMaxAnchorPx) return adopt(z, s);
  A_ = A;
  T_ = (s_ + A) / z_;
  Z_ = z;
  S_ = s;
  active_ = true;
}

void Glide::begin(float& px, float& scroll, bool continuous, const Params& p) {
  begun_ = true;
  if (!hasOut_ || !continuous) {
    hasOut_ = false;
    adopt(px, scroll);
  } else if (px != outZ_) {
    retarget(px, scroll, p);  // a zoom written from outside (menu, zoom tool): glides from the drawn view
  } else if (scroll != outS_) {
    if (active_) {  // scrolled while gliding: the glide moves along (same zoom, the new scroll)
      s_ = scroll;
      T_ = (s_ + A_) / z_;
      S_ = T_ * Z_ - A_;
      if (S_ < 0) retarget(Z_, 0.0, p);  // never ends left of the start
    } else {
      adopt(px, scroll);
    }
  }
  if (active_ && !p.smooth) adopt(Z_, S_);  // switched off mid-glide: there at once
  if (active_ || !p.smooth) {
    px = static_cast<float>(Z_);
    scroll = static_cast<float>(S_);
  }
  logZ_ = px;
  logS_ = scroll;
}

void Glide::end(float& px, float& scroll, double dt, bool animate, const Params& p) {
  if (!begun_ || !hasOut_) {
    adopt(px, scroll);
  } else if (px != logZ_) {
    if (animate) retarget(px, scroll, p);
    else adopt(px, scroll);
  } else if (scroll != logS_) {
    if (active_) retarget(px, scroll, p);  // a new end for the glide (still continuous)
    else adopt(px, scroll);
  } else if (!active_) {
    adopt(px, scroll);
  }
  if (active_) {
    const double lz = std::log(z_), lZ = std::log(Z_);
    const double l = smooth::approach(lz, lZ, dt, p.tau);
    if (std::fabs(l - lZ) <= p.eps) {
      adopt(Z_, S_);
    } else {
      z_ = std::exp(l);
      s_ = T_ * z_ - A_;
    }
  }
  px = static_cast<float>(z_);
  scroll = static_cast<float>(s_);
  outZ_ = px;
  outS_ = scroll;
  hasOut_ = true;
  begun_ = false;
}

RowAnchor rowAnchorAt(const float* top, int n, double base, double y, double px) {
  RowAnchor a;
  a.px = px;
  n = std::max(0, n);
  const double end = n > 0 ? top[n] : 0.0;
  if (n == 0 || y >= end) {
    a.row = n;
    a.frac = base > 0 ? (y - end) / base : 0.0;
    return a;
  }
  // The last row whose top is at or above y (empty folded rows are skipped).
  const float* it = std::upper_bound(top, top + n + 1, static_cast<float>(std::max(0.0, y)));
  const int r = std::clamp(static_cast<int>(it - top) - 1, 0, n - 1);
  const double h = static_cast<double>(top[r + 1]) - top[r];
  a.row = r;
  a.frac = h > 0 ? std::clamp((y - top[r]) / h, 0.0, 1.0) : 0.0;
  if (y < 0) a.frac = base > 0 ? y / base : 0.0;  // above the content (an overscroll): base heights above row 0
  return a;
}

double rowAnchorPos(const float* top, int n, double base, const RowAnchor& a) {
  n = std::max(0, n);
  if (a.row >= n) return (n > 0 ? top[n] : 0.0) + a.frac * base;
  const int r = std::max(0, a.row);
  if (a.frac < 0) return top[r] + a.frac * base;
  return top[r] + a.frac * (static_cast<double>(top[r + 1]) - top[r]);
}

void RowGlide::zoomTo(double from, double to, const RowAnchor& a, bool animate, const Params& p) {
  anchor_ = a;
  pending_ = true;
  if (!usable(to)) return;
  H_ = to;
  if (!animate || !p.smooth || !usable(from) || std::fabs(std::log(to / from)) <= p.eps) {
    h_ = to;
    active_ = false;
  } else {
    h_ = from;
    active_ = true;
  }
}

void RowGlide::finish() {
  if (!active_) return;
  h_ = H_;
  active_ = false;
  pending_ = true;
}

double RowGlide::step(double h, double dt, const Params& p) {
  anchored_ = pending_ || active_;
  pending_ = false;
  if (active_ && !p.smooth) h_ = H_, active_ = false;  // switched off mid-glide: there at once
  if (active_) {
    const double lh = std::log(h_), lH = std::log(H_);
    const double l = smooth::approach(lh, lH, dt, p.tau);
    if (std::fabs(l - lH) <= p.eps) {
      h_ = H_;
      active_ = false;
    } else {
      h_ = std::exp(l);
    }
  } else if (!anchored_) {
    h_ = h;
  }
  out_ = static_cast<float>(h_);
  hasOut_ = true;
  return h_;
}

}  // namespace gl::smoothzoom
