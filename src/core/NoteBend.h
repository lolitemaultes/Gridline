// Gridline - K-25: per-note pitch bend lines (the piano roll's Pitch bend tool).
//
// A note's bend is a line of points (BendPoint in Types.h): ticks after the note's start and
// semitones off its pitch. The line starts at 0 on the note's start (unless a point sits there),
// runs through the points (each point's curve shapes the segment that ends at it) and holds the
// last value to the note's end. Times stay in ticks, so a resize keeps the bend's timing and
// points past the note's end wait there until the note is lengthened again.
//
// value() is allocation free and works on a plain array, so the audio thread evaluates the
// copies it keeps in fixed slots with the same code the editor draws with.
#pragma once
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>
#include "Types.h"

namespace gl::bend {

// K-28: ease in / ease out joined the first three (ids stay: files keep their meaning).
enum Curve : uint8_t { Straight = 0, Smooth = 1, Step = 2, EaseIn = 3, EaseOut = 4, CurveCount = 5 };
inline constexpr int kMaxPoints = 32;      // per note (editor, imports and the engine's slots)
inline constexpr float kMaxSemis = 48.0f;  // +-4 octaves

namespace detail {
// Power curve: a > 0 starts slowly (x^e), a < 0 starts fast; |a| = 1 is a steep knee.
inline double powCurve(double a, double x) {
  if (a == 0) return x;
  const double e = std::pow(2.0, 3.3 * std::fabs(a));
  return a > 0 ? std::pow(x, e) : 1.0 - std::pow(1.0 - x, e);
}
inline double cosS(double x) { return 0.5 - 0.5 * std::cos(x * 3.14159265358979323846); }
inline double sharpS(double x) { return x < 0.5 ? 0.5 * powCurve(0.9, 2.0 * x) : 1.0 - 0.5 * powCurve(0.9, 2.0 * (1.0 - x)); }
}  // namespace detail

// The segment shape: 0..1 -> 0..1. `tension` (-1..1) is the curve amount: a straight segment
// bends into an ease (positive: slow start, negative: fast start), smooth gets steeper (positive)
// or flatter (negative, -1 = straight), ease in / ease out get stronger (positive) or gentler
// (-1 = straight). A step has none. The editor draws and the engine plays through this function.
inline double shape(uint8_t curve, double tension, double x) {
  const double t = tension < -1.0 ? -1.0 : tension > 1.0 ? 1.0 : tension;
  switch (curve) {
    case Smooth: {
      const double c = detail::cosS(x);
      if (t > 0) return c + (detail::sharpS(x) - c) * t;
      if (t < 0) return c + (x - c) * -t;
      return c;
    }
    case Step: return x < 1.0 ? 0.0 : 1.0;
    case EaseIn: return detail::powCurve(0.45 * (1.0 + t), x);
    case EaseOut: return detail::powCurve(-0.45 * (1.0 + t), x);
    default: return detail::powCurve(t, x);
  }
}
inline double shape(uint8_t curve, double x) { return shape(curve, 0.0, x); }
// True when the segment ending at `p` is neither a plain straight ramp nor a step (drawn and
// exported as a sampled curve).
inline bool curved(const BendPoint& p) {
  return p.curve == Smooth || p.curve == EaseIn || p.curve == EaseOut || (p.curve == Straight && p.tension != 0);
}

// Semitones at `t` ticks after the note's start.
inline double value(const BendPoint* p, int n, double t) {
  if (!p || n <= 0) return 0.0;
  double t0 = 0, v0 = 0;
  for (int i = 0; i < n; ++i) {
    const double t1 = p[i].tick, v1 = p[i].semis;
    if (t < t1) {
      const double span = t1 - t0;
      if (span <= 1e-9) return v1;
      return v0 + (v1 - v0) * shape(p[i].curve, p[i].tension, (t - t0) / span);
    }
    t0 = t1, v0 = v1;
  }
  return v0;
}
inline double value(const std::vector<BendPoint>& b, double t) { return value(b.data(), static_cast<int>(b.size()), t); }

// True when the line never leaves 0 (no points, or every point at 0).
bool flat(const std::vector<BendPoint>& b);
// Sorts by time, clamps values / times / tensions, merges points at the same time (the last
// wins), keeps at most kMaxPoints and drops a line that is flat everywhere.
void normalize(std::vector<BendPoint>& b);
// The part of `b` from `at` ticks on, as a line starting at the new note's start: points after
// `at` shifted back, and a start point holding the value at `at` when it is not 0.
std::vector<BendPoint> tail(const std::vector<BendPoint>& b, double at);
// The part of `b` before `at` (a note shortened by a split): points up to `at`, and a closing
// point with the value at `at` when a later point shaped the line there.
std::vector<BendPoint> head(const std::vector<BendPoint>& b, double at);
// Splits note `n` at `cut` ticks from its start into left / right bend lines.
void split(const Note& n, Tick cut, Note& left, Note& right);
// Glue: `b`'s line appended to `a`'s (b starts `offset` ticks after a). Points of `a` past the
// offset are dropped, `b`'s start value is pinned so the joined line plays what both played.
void append(std::vector<BendPoint>& a, const std::vector<BendPoint>& b, double offset);
// K-29: the editor's double-click edits.
// Removes point `k` only: every other point keeps its time, value, shape and amount. The glide
// that ended at the next point now starts at the previous point (or the note's start) and keeps
// the next point's shape and amount (a point's shape is always the glide into it); without a
// next point the line holds the previous value. Normalized after (a line left flat is cleared).
// False when `k` is out of range.
bool removePoint(std::vector<BendPoint>& b, size_t k);
// The glide into point `k` back to a plain straight ramp (shape Straight, amount 0). False when
// `k` is out of range or the segment already is one.
bool resetCurve(std::vector<BendPoint>& b, size_t k);
// The point whose glide (incoming segment) holds the time `t`: the first point after `t`, or -1
// past the last point (the line holds there: no curve).
int segmentAt(const std::vector<BendPoint>& b, double t);
// Rescales point times (project PPQ change).
void rescale(std::vector<BendPoint>& b, int fromPpq, int toPpq);

// Samples (ticks from the note start, semitones), e.g. MIDI pitch bend during a note, as a line.
// Each sample holds until the next, except that samples at most `rampGap` ticks apart are read as
// a sampled ramp (joined straight):
// straight segments through the samples, thinned to within `tolerance` semitones and kMaxPoints.
std::vector<BendPoint> fromSamples(const std::vector<std::pair<double, double>>& samples, double tolerance = 0.02, double rampGap = 0);

// K-28: a bend line as slide notes (FL Studio export). FL notes have no bend line; a slide note
// glides the sounding notes of its channel and colour to its own pitch (key + fine pitch, in 10
// cent steps) over its length. The line from the note's start to `length` ticks becomes:
//   - `start`: the value at the note's start (the exported note itself starts there);
//   - one slide per straight ramp (at the ramp's start, as long as the ramp, aiming at its end);
//   - a curve (smooth, eases, a bent straight) as several shorter ramps that stay within
//     `tolerance` semitones of it (K-29: cut where it bends most, at most 32 per segment, none
//     shorter than one tick);
//   - a step as a one-tick slide at its point; a hold as nothing.
// Times are whole ticks (FL notes have no fractions); pieces never overlap.
struct SlidePiece {
  int64_t tick = 0;    // from the note's start
  int64_t length = 1;  // ticks, >= 1
  double target = 0;   // semitones from the note's pitch the slide aims at
};
struct SlidePlan {
  double start = 0;  // semitones at the note's start
  std::vector<SlidePiece> pieces;
  int curves = 0;   // curved segments played as several ramps
  int steps = 0;    // steps played as one-tick slides
  int rounded = 0;  // point times moved to whole ticks
};
SlidePlan toSlides(const std::vector<BendPoint>& b, double length, double tolerance = 0.03);

}  // namespace gl::bend
