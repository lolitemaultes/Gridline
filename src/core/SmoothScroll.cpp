// Gridline — smooth scrolling with elastic edges (J-67). See SmoothScroll.h.
#include "SmoothScroll.h"

#include <algorithm>
#include <cmath>

namespace gl::smoothscroll {

namespace {

double approach(double cur, double target, double dt, double tau) {
  if (!(dt > 0)) return cur;
  if (!(tau > 0)) return target;
  return target + (cur - target) * std::exp(-dt / tau);
}

}  // namespace

double rubber(double stretch, const Params& p) {
  if (!(p.maxOver > 0) || stretch == 0 || !std::isfinite(stretch)) return 0;
  const double a = std::fabs(stretch) * p.resist;
  const double o = p.maxOver * a / (a + p.maxOver);
  return stretch < 0 ? -o : o;
}

double unrubber(double over, const Params& p) {
  if (!(p.maxOver > 0) || over == 0 || !(p.resist > 0)) return 0;
  const double o = std::min(std::fabs(over), p.maxOver * 0.999);
  const double s = p.maxOver * o / (p.resist * (p.maxOver - o));
  return over < 0 ? -s : s;
}

bool isNotch(float wheel) {
  const double a = std::fabs(static_cast<double>(wheel));
  return a >= 0.999 && std::fabs(a - std::round(a)) < 1e-3;
}

bool Axis::sync(double v, double tol) {
  if (hasOut_ && std::fabs(v - out_) <= tol) return false;
  reset(v);
  return true;
}

void Axis::reset(double v) {
  value_ = target_ = out_ = v;
  stretch_ = over_ = 0;
  idle_ = 1e9;
  easing_ = false;
  hasOut_ = true;
}

void Axis::scroll(double delta, bool notch, double lo, double hi, const Params& p) {
  if (delta == 0 || !std::isfinite(delta)) return;
  if (hi < lo) hi = lo;
  const bool ease = notch && p.smooth;
  idle_ = 0;
  // Moving back towards the range first unwinds the overscroll, 1:1 in drawn
  // px. A notch instead drops the stretch (it eases away) and moves the whole
  // step from the edge, so it still lands on its row.
  const double o = rubber(stretch_, p);
  if (o != 0 && (delta > 0) != (o > 0) && ease) {
    stretch_ = 0;
  } else if (o != 0 && (delta > 0) != (o > 0)) {
    double no = o + delta;
    if (no == 0 || (no > 0) != (o > 0)) {
      delta = no;  // the rest goes into the range
      no = 0;
    } else {
      delta = 0;
    }
    stretch_ = unrubber(no, p);
    if (!ease) over_ = no;
  }
  if (delta != 0) {
    const double t = target_ + delta;
    const double c = std::clamp(t, lo, hi);
    const double moved = c - std::clamp(target_, lo, hi);
    target_ = c;
    if (!ease) value_ = std::clamp(value_ + moved, lo, hi);
    if (p.elastic) stretch_ += t - c;
    if (!ease) over_ = rubber(stretch_, p);
  }
  easing_ = ease || (easing_ && value_ != target_);  // a glide under way finishes
}

double Axis::update(double dt, double lo, double hi, const Params& p) {
  if (!(dt > 0) || !std::isfinite(dt)) dt = 0;
  if (hi < lo) hi = lo;
  if (!hasOut_) reset(lo);
  // The range may have changed (content resized): the in-range part is
  // always inside it (no animation of a shrinking view, as before).
  target_ = std::clamp(target_, lo, hi);
  value_ = std::clamp(value_, lo, hi);
  if (easing_) value_ = approach(value_, target_, dt, p.tau);
  else value_ = target_;
  if (std::fabs(value_ - target_) <= p.eps) value_ = target_;
  // Overscroll: held while input continues, then springs back.
  if (!p.elastic) {
    stretch_ = 0;
  } else if (stretch_ != 0) {
    const double before = idle_;
    const double decay = std::max(0.0, before + dt - std::max(before, p.hold));
    if (decay > 0) stretch_ *= std::exp(-decay / std::max(1e-6, p.backTau));
  }
  idle_ += dt;
  double goal = rubber(stretch_, p);
  if (std::fabs(goal) <= p.eps) {
    stretch_ = 0;
    goal = 0;
  }
  over_ = easing_ ? approach(over_, goal, dt, p.tau) : goal;
  if (std::fabs(over_ - goal) <= p.eps) over_ = goal;
  if (!p.elastic) over_ = 0;
  if (value_ == target_ && stretch_ == 0 && over_ == 0) easing_ = false;
  out_ = value_ + over_;
  hasOut_ = true;
  return out_;
}

}  // namespace gl::smoothscroll
