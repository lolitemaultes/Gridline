// Gridline — smooth scrolling with elastic edges (J-67).
//
// One scroll axis of a view (playlist rows, piano roll keys, a list, ...).
// Wheel notches move a *target*; the drawn position eases towards it with
// the J-37 exponential ease (frame-rate independent, never overshoots), so a
// notch glides into place in ~80 ms and still lands exactly where the notch
// put it (a whole track row, three piano keys, ...): "along the grid".
// Precise deltas (trackpad finger movement, OS momentum) move the drawn
// position 1:1 at once: no lag under the finger.
//
// The range is enforced *inside* the model: input past an edge never moves
// the in-range position past the limit and back (the old views added the
// delta after clamping, so every frame of a gesture held against an edge drew
// the view past the limit and snapped it back next frame: the "jumping").
// Instead the excess stretches an elastic overscroll with growing resistance
// (bounded by maxOver px) that springs back once input stops. Each axis is
// its own Axis, so an axis held against its edge never disturbs the other.
//
// Pure: no UI dependencies, unit tested (tests/test_smooth_scroll.cpp).
#pragma once

namespace gl::smoothscroll {

struct Params {
  bool smooth = true;        // ease wheel notches (off: notches jump, as before)
  bool elastic = true;       // overscroll past the edges (off: hard stop)
  double tau = 0.018;        // ease time constant, s (1 % left after ~83 ms)
  double maxOver = 48.0;     // overscroll bound, px (approached, never reached)
  double resist = 0.55;      // rubber band stiffness (lower: stiffer)
  double hold = 0.06;        // overscroll holds this long after the last input, s
  double backTau = 0.055;    // spring back time constant, s (settles in ~0.25 s)
  double eps = 0.05;         // closer than this counts as arrived, px
};

// Rubber band: displayed overscroll for a raw stretch (input px past the
// edge). 1:1-ish at first, resistance grows with distance, |result| < maxOver.
double rubber(double stretch, const Params& p);
// Inverse of rubber() (|over| < maxOver).
double unrubber(double over, const Params& p);

class Axis {
 public:
  // Adopts a position written by someone else (zoom, fit, follow playback,
  // scrollbar drag, ...): the animation stops there. Returns true when `v`
  // was not this axis' own last output (within `tol`: views keep floats,
  // ImGui windows round to whole pixels).
  bool sync(double v, double tol = 1e-3);
  // Forces the position (no animation, no overscroll).
  void reset(double v);

  // Scroll input of `delta` px (positive: towards hi). `notch` = a wheel step
  // (eased); otherwise a precise delta (applied 1:1).
  void scroll(double delta, bool notch, double lo, double hi, const Params& p);

  // Advances `dt` seconds inside [lo, hi] and returns the drawn position
  // (in range, plus the elastic overscroll outside it).
  double update(double dt, double lo, double hi, const Params& p);

  double output() const { return out_; }
  double target() const { return target_; }
  double overscroll() const { return over_; }
  bool animating() const { return value_ != target_ || stretch_ != 0 || over_ != 0; }

 private:
  double value_ = 0;    // drawn in-range position
  double target_ = 0;   // where the input wants the in-range position
  double stretch_ = 0;  // raw input past the edge (signed)
  double over_ = 0;     // drawn overscroll
  double idle_ = 1e9;   // seconds since the last input
  double out_ = 0;      // value_ + over_ as last returned
  bool hasOut_ = false;
  bool easing_ = false;  // the last input was a notch: the drawn position eases
};

// Wheel input of one frame: a notch (mouse wheel click, integral step) or a
// precise fractional delta (trackpad / momentum).
bool isNotch(float wheel);

}  // namespace gl::smoothscroll
