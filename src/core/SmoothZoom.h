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

// K-24: vertical zoom (row / track height) of a view whose rows may differ
// in height (per-track multipliers, rounding, folded groups taking no room).
// The anchor is a point of the content named by its row and how far into
// that row it lies (0..1 of the row's height; row == row count: below the
// last row, `frac` then counts base row heights), plus its pixel offset from
// the view's top. Every frame the view lays its rows out at the drawn height
// and sets its scroll to (row top + frac * row height) - px, clamped to its
// range: the point stays under the pointer exactly, whatever the layout.
// The drawn height eases to the target in log space (the J-37 ease, kTau),
// like the horizontal Glide; retargeting mid-flight is continuous.
struct RowAnchor {
  int row = 0;
  double frac = 0;
  double px = 0;
};

// The anchor of the content point `y` (px from the content top) among `n`
// rows whose tops are top[0..n] (top[n]: the content height; folded rows are
// empty spans), `base` the base row height (for points below the last row),
// `px` its offset from the view's top. And back: the point's content y.
RowAnchor rowAnchorAt(const float* top, int n, double base, double y, double px);
double rowAnchorPos(const float* top, int n, double base, const RowAnchor& a);

class RowGlide {
 public:
  // An outside write of the height since the last frame (a menu): `h` is not
  // what this glide last drew.
  bool outside(float h) const { return hasOut_ && h != out_; }
  bool hasOutput() const { return hasOut_; }
  float output() const { return out_; }
  // Where zoom steps compose: the glide's target while it runs, else `h`.
  double target(double h) const { return active_ ? H_ : h; }
  // A zoom from the drawn height `from` to `to` (already clamped by the view)
  // around `a`. `animate` false (a trackpad, a scrollbar drag: 1:1) or
  // smoothing off: there at once.
  void zoomTo(double from, double to, const RowAnchor& a, bool animate, const Params& p);
  // The glide moves along with a scroll: the same pixel, a new content point.
  void setAnchor(const RowAnchor& a) { anchor_ = a; }
  // The rest of the glide at once (a plain scroll arrived).
  void finish();
  // Once a frame, after the inputs: returns the drawn height for this frame
  // (`h`: what the view holds; unchanged when nothing zooms).
  double step(double h, double dt, const Params& p);
  // This frame's scroll follows the anchor (a glide runs or a zoom just applied).
  bool anchored() const { return anchored_; }
  bool active() const { return active_; }
  const RowAnchor& anchor() const { return anchor_; }
  // The scroll the view wrote last frame while anchored (an outside scroll write since moves the anchor).
  float scrollOut = 0;
  void reset() { *this = RowGlide(); }

 private:
  bool active_ = false, pending_ = false, anchored_ = false, hasOut_ = false;
  double h_ = 0, H_ = 0;
  float out_ = 0;
  RowAnchor anchor_;
};

}  // namespace gl::smoothzoom
