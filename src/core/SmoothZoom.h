// Gridline - J-188: smooth zooming of horizontal timelines (playlist, piano
// roll, automation editor).
//
// A view keeps its zoom (px per tick) and horizontal scroll (px) as floats
// and changes them in many places: wheel and keyboard zoom, the zoom tool,
// fit / zoom to selection, menus. A Glide sits around that code without
// changing it: at the top of the view's zoom code begin() hands the code the
// *logical* view (where the last zoom wants to end up), so repeated zoom
// steps compose exactly as they always did (the same final zoom and scroll);
// after it end() turns the logical view into the *drawn* one.
//
// The drawn zoom eases towards the target in log space (the J-37 exponential
// ease: frame-rate independent, no overshoot, continuous when retargeted
// mid-flight), and the scroll is tied to it so that one point of the
// timeline stays put on screen throughout: the fixed point of the move from
// the drawn view to the target. For a zoom around the pointer (or any
// anchor) that point is the anchor itself, at every frame.
//
// Only zoom changes glide: a scroll alone (wheel, scrollbar, following the
// song position) is adopted at once, and while a zoom glides it moves the
// glide along with it. Pure: no UI dependencies (tests/test_smooth_zoom.cpp).
#pragma once

namespace gl::smoothzoom {

// Ease time constant: ~110 ms until 1 % of a zoom step is left.
constexpr double kTau = 0.024;

struct Params {
  bool smooth = true;  // off (setting, reduced motion): every zoom applies at once
  double tau = kTau;
  double eps = 1e-4;  // |ln(drawn / target)| below this counts as arrived
};

class Glide {
 public:
  // Top of the view's zoom code. `px` / `scroll`: what the view holds (the
  // last drawn view, or an outside write since: a menu setting the zoom
  // glides from where the view is drawn, a scroll moves the glide along).
  // `continuous`: the view was drawn last frame (false: adopt at once).
  // Replaces them by the logical view for the zoom code to work on.
  void begin(float& px, float& scroll, bool continuous, const Params& p);
  // After the zoom code: `px` / `scroll` is the view it wants. `animate`
  // false (a trackpad gesture, which must follow the fingers 1:1) applies a
  // zoom change at once. Writes the drawn view for this frame (`dt` seconds
  // after the last one).
  void end(float& px, float& scroll, double dt, bool animate, const Params& p);

  bool active() const { return active_; }
  double targetZoom() const { return Z_; }
  double targetScroll() const { return S_; }
  // Pixels from the view's left edge of the point that stays put (valid while active).
  double anchorPx() const { return A_; }
  void reset() { *this = Glide(); }

 private:
  void adopt(double z, double s);
  void retarget(double z, double s, const Params& p);  // from the drawn view to (z, s)
  bool begun_ = false;
  bool hasOut_ = false;
  bool active_ = false;
  float outZ_ = 0, outS_ = 0;  // last drawn view, exactly as written
  float logZ_ = 0, logS_ = 0;  // logical view handed to the zoom code by begin()
  double z_ = 1, s_ = 0;       // drawn view
  double Z_ = 1, S_ = 0;       // target (logical) view
  double A_ = 0, T_ = 0;       // anchor: tick T_ stays at A_ px (scroll = T_ * z - A_)
};

}  // namespace gl::smoothzoom
