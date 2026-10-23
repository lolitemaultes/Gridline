// Gridline - J-99: smooth freehand drawing.
//
// Every editor surface where a curve, band levels, a waveform or a table is
// painted with the mouse (vocoder bands, drawn EQs, harmonic bars, drawn
// waves, envelope tables...) feeds its pointer samples through one Stroke:
//
//  1. the pointer path is filtered with a one-euro filter (jitter goes away
//     at slow speeds, fast moves follow without lag);
//  2. every cell between the previous and the current pointer position is
//     filled (linear along the surface's own axis, so a log-frequency
//     display interpolates in log frequency): fast strokes leave no gaps
//     or stair steps;
//  3. the painted cells get a gentle edge-preserving (bilateral Gaussian)
//     pass along that same axis, wrapping around for single-cycle waves.
//     It reads the untouched neighbours for continuity but only ever writes
//     the cells the stroke touched: untouched cells stay bit-identical.
//
// "Raw" (Alt held while drawing) keeps step 2 only: no filtering, no pass.
// The pure model lives here (unit tested in test_draw_smooth); the ImGui
// glue, per-surface preferences and undo grouping are in app/DrawSmoothUi.h.
#pragma once
#include <cstdint>
#include <vector>

namespace gl::drawsmooth {

// Default smoothing amount (0 = off, 1 = strongest); Settings "draw.smoothAmount".
constexpr double kDefaultAmount = 0.5;

// Surfaces with their own on/off switch (Settings "draw.smooth.<id>", on by
// default; core/Settings.cpp registers one key per id).
inline constexpr const char* kSurfaces[] = {
    "facet", "cipher", "bankeq", "curveeq", "banddelay", "lattice", "keyctl",
};

struct Options {
  double amount = kDefaultAmount;  // 0..1; 0 behaves like raw
  bool cyclic = false;             // single-cycle wave: the pass wraps around the ends
  bool raw = false;                // Alt: fill gaps only (no pointer filter, no pass)
  float lo = 0.0f, hi = 1.0f;      // value range (clamp, and the scale of "an edge")
};

// One-euro filter (Casiez et al. 2012): an adaptive low-pass whose cut-off
// rises with speed. Units: value per second for the speed term.
class OneEuro {
 public:
  OneEuro(double minCutoff = 4.0, double beta = 0.6, double dCutoff = 1.0) : minCutoff_(minCutoff), beta_(beta), dCutoff_(dCutoff) {}
  void reset() { primed_ = false; }
  double filter(double x, double t);
  double value() const { return x_; }

 private:
  double minCutoff_, beta_, dCutoff_;
  bool primed_ = false;
  double x_ = 0, dx_ = 0, t_ = 0;
};

// Cell centres along the draw axis, normalised 0..1 and ascending. Empty
// means uniform cells ((i + 0.5) / n). Pass the display positions for
// surfaces whose cells are not evenly spaced on screen (linear bins on a
// log-frequency display): interpolation and smoothing then follow the axis.
using Axis = std::vector<double>;

// Index of the cell that contains x (0..1) on `axis` with n cells.
int cellAt(const Axis& axis, int n, double x);

// Fills every cell whose centre lies between x0 and x1 (plus the cells that
// contain the two ends) with the straight line from (x0, v0) to (x1, v1),
// clamped at the ends, and marks them touched. Returns the number of cells.
int fillSegment(std::vector<float>& vals, std::vector<uint8_t>& touched, const Axis& axis, double x0, double v0, double x1, double v1);

// The edge-preserving pass: out[i] for touched i is a bilateral Gaussian
// average of `in` around i along the axis (wrapping when cyclic); untouched
// cells are copied from `in` unchanged. `amount` 0..1, `range` = hi - lo.
void smoothTouched(const std::vector<float>& in, const std::vector<uint8_t>& touched, std::vector<float>& out, const Axis& axis,
                   double amount, bool cyclic, float range);

// Kernel width (in cells) used for n cells at `amount`.
double sigmaCells(int n, double amount);

// Breakpoint-curve surfaces (point lists such as an IR envelope or EQ): a
// stroke over n uniform cells spanning [0, span) replaces the points inside the
// painted span with one point per painted cell plus two edge points that carry
// the old curve's values there (vA, vB), so the curve outside stays exactly as
// it was. `pts` is sorted by x. Returns false (pts unchanged) without a span.
struct CurvePoint {
  double x = 0, v = 0;
};
bool touchedSpan(const std::vector<uint8_t>& touched, double span, double& xa, double& xb);
std::vector<CurvePoint> spliceIntoCurve(const std::vector<CurvePoint>& pts, double span, const std::vector<float>& vals,
                                        const std::vector<uint8_t>& touched, double vA, double vB);

// One freehand stroke over n cells.
class Stroke {
 public:
  // Starts a stroke over a snapshot of the surface's current values.
  void begin(const float* values, int n, const Options& o, Axis axis = {});
  // A pointer sample: x along the axis (0..1), value v, time t (seconds).
  void add(double x, double v, double t);
  // Raw switch mid-stroke (Alt pressed or released).
  void setRaw(bool raw) { opt_.raw = raw; }
  bool active() const { return active_; }
  void end() { active_ = false; }
  int size() const { return static_cast<int>(base_.size()); }
  // The values to show and commit: the snapshot with the stroke applied.
  const std::vector<float>& result() const { return out_; }
  const std::vector<uint8_t>& touched() const { return touched_; }
  const std::vector<float>& original() const { return base_; }

 private:
  void recompute();
  Options opt_;
  Axis axis_;
  std::vector<float> base_, painted_, out_;
  std::vector<uint8_t> touched_;
  OneEuro fx_, fv_;
  bool active_ = false, hasLast_ = false;
  double lastX_ = 0, lastV_ = 0;
};

}  // namespace gl::drawsmooth
