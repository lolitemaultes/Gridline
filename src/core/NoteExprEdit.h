// Gridline — K-99: editing and recording a note's expression lanes (MPE pressure and timbre).
//
// A note's pressure and timbre lanes (Note::pressure / Note::timbre, Types.h) are BendPoint lines
// like its pitch bend: ticks after the note's start, the value in `semis` (pressure 0..1, timbre
// -0.5..0.5 from its centre; 0 is neutral in both, as before the first point). The engine plays
// them with the bend line's code (bend::value) and keeps at most bend::kMaxPoints per lane.
//
// Here: the piano roll lane's painting (a sweep or a line over the notes), Alt's reset, the
// display value, and the thinning recorded lanes go through so they fit the engine's slots.
#pragma once
#include <vector>
#include "Types.h"

namespace gl::exprlane {

// The lane of `d` (Pitch: the bend line).
std::vector<BendPoint>& laneOf(Note& n, ExprDim d);
const std::vector<BendPoint>& laneOf(const Note& n, ExprDim d);
// Display value 0..1 of the lane `t` ticks after the note's start (timbre: 0.5 = centre).
float valueAt(const Note& n, ExprDim d, double t);
// Stored value for a display value 0..1.
inline float stored(ExprDim d, float f) { return d == ExprDim::Timbre ? f - 0.5f : f; }

// Paints the segment from (t0, f0) to (t1, f1), pattern ticks and display values 0..1, into every
// note it crosses (only the notes flagged in `only`, when it is not empty): the lane takes the
// segment's values over the span, one point every `grain` ticks, and keeps its shape outside it.
// Returns true when a note changed.
bool paint(std::vector<Note>& notes, const std::vector<bool>& only, ExprDim d, double t0, float f0, double t1, float f1,
           double grain);
// Takes the lane back to neutral over [t0, t1] (pattern ticks) on the notes it crosses.
bool clear(std::vector<Note>& notes, const std::vector<bool>& only, ExprDim d, double t0, double t1);
// Thins a lane to at most `maxPoints` points, dropping the ones a straight line through their
// neighbours already gives within `eps` (it grows until the lane fits). Drops a lane that stays
// neutral throughout.
void simplify(std::vector<BendPoint>& lane, float eps, int maxPoints);

}  // namespace gl::exprlane
