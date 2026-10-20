// Gridline — automation point editing (J-5): the operations behind the
// automation clip editor, the pattern-automation lane and automation
// recording, kept free of UI so they can be tested. Every function works on a
// sorted point list (AutoPoint::pos ascending) and keeps it sorted; selections
// are point indices.
#pragma once
#include <cstdint>
#include <set>
#include <string>
#include <vector>
#include "Song.h"

namespace gl::autoedit {

using Points = std::vector<AutoPoint>;
using Selection = std::set<int>;

// ---- curve modes --------------------------------------------------------------
// Point-menu order (smooth, hold, the three single curves, the three double
// curves, half sine, stairs, smooth stairs, pulse, wave).
const std::vector<AutoShape>& menuOrder();
// FL Studio stores a point's curve as a mode byte (0 … 12). Unknown modes read
// as the single curve.
AutoShape shapeFromFlMode(int mode);
int flModeOf(AutoShape s);
// Stable text keys for files ("curve", "scurve2", "halfsine", …).
const char* shapeKey(AutoShape s);
AutoShape shapeFromKey(const std::string& key);
// FL's tension sign is the reverse of Gridline's (FL: negative = slow start).
inline float tensionFromFl(float t) { return -t; }

// Value (0..1) of a bare point list at tick t (same curve maths as clips).
double valueAt(const Points& pts, double t, bool fl = false);  // fl: FL's exact laws (J-41)

// ---- point edits ----------------------------------------------------------------
// Adds a point (after points at the same tick); returns its index.
int addPoint(Points& pts, const AutoPoint& p);
// Deletes the selected points. At least `keep` points stay (the earliest
// unselected ones survive first). Returns how many were removed.
int deletePoints(Points& pts, const Selection& sel, size_t keep = 1);
// Moves the selected points of `base` by dt ticks and dv (value) into `out`.
// Selected points keep their order relative to unselected neighbours (dt is
// limited so they cannot pass them) and stay >= 0; values clamp to 0..1.
// Returns the dt actually applied.
Tick movePoints(const Points& base, const Selection& sel, Tick dt, double dv, Points& out);
// Snaps the selected points (all when the selection is empty) to the nearest
// multiple of `grid`. Points that land on the same tick keep only the last.
// Returns the new selection (indices after the edit).
Selection quantize(Points& pts, const Selection& sel, Tick grid);
// Mirrors values (v → 1 − v) of the selection (all when empty).
void flipVertical(Points& pts, const Selection& sel);
// Mirrors the selection (all when empty) in time inside its own span; the
// curve of each segment travels with it (tension sign flips: slow start ↔ fast start).
void flipHorizontal(Points& pts, const Selection& sel);
// Level scaling: v → centre + (v − centre) · multiplier + offset, clamped.
void scaleLevels(Points& pts, const Selection& sel, double centre, double multiplier, double offset);
void setShape(Points& pts, const Selection& sel, AutoShape s);
void setTension(Points& pts, const Selection& sel, float tension);
// Box selection: points with pos in [t0, t1] and value in [v0, v1].
Selection selectBox(const Points& pts, Tick t0, Tick t1, double v0, double v1);

// ---- clipboard -------------------------------------------------------------------
// Copies the selection with positions relative to its first point.
Points copyPoints(const Points& pts, const Selection& sel);
// Pastes at `at`. Replace mode removes the existing points inside the pasted
// span first; insert mode shifts the points at or after `at` by the span.
// Returns the pasted points' indices.
Selection pastePoints(Points& pts, const Points& clip, Tick at, bool replace);
// Ticks covered by a clipboard (last relative position, at least 1).
Tick clipSpan(const Points& clip);

// ---- thinning (recording, decimate) ------------------------------------------------
// Removes points that a straight line through their neighbours already
// reproduces within `tolerance` (value units). Kept points become straight
// lines, except that a jump (two points on one tick) keeps its hold.
Points thin(const Points& pts, double tolerance);
// Replaces the points of [from, to] with `recorded` (absolute ticks, any
// order), thinned with `tolerance`. Values just before/after the range are kept.
void mergeRecorded(Points& pts, Points recorded, Tick from, Tick to, double tolerance);

// ---- LFO tool --------------------------------------------------------------------
enum class LfoShape : uint8_t { Sine, Triangle, Square, SawUp, SawDown, Count };
const char* lfoShapeName(LfoShape s);
struct LfoSettings {
  LfoShape shape = LfoShape::Sine;
  double periodStart = 96, periodEnd = 96;  // ticks per cycle at the start / end of the range (speed sweep)
  double centreStart = 0.5, centreEnd = 0.5;  // middle value
  double amountStart = 0.5, amountEnd = 0.5;  // peak deviation from the middle (0..0.5 covers the full range)
  double phase = 0.0;      // 0..1 cycle offset at the start
  double tension = 0.0;    // -1..1 extra bend of every segment
  double pulseWidth = 0.5; // square: part of the cycle at the high value
};
// The LFO over [from, to) as automation points (a few per cycle: curves, not samples).
Points lfoPoints(const LfoSettings& s, Tick from, Tick to);
// Replaces the points of [from, to] with the LFO.
void applyLfo(Points& pts, const LfoSettings& s, Tick from, Tick to);

// ---- shapes ----------------------------------------------------------------------
// Fits a shape (any positions) into [from, to): positions scale to the span,
// existing points in the span are replaced.
void applyShape(Points& pts, const Points& shape, Tick from, Tick to);

}  // namespace gl::autoedit
