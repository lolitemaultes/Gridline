// Gridline — piano roll event lane editing (Q-4): the note property lane under
// the grid (velocity, release, pan, fine pitch, mod X / Y).
//
// Pure functions on a note list so the piano roll and the tests share them.
// Positions are in ticks (doubles, so sub-tick pointer positions stay exact),
// values are the property's normalised 0..1 (tools::notePropNorm). The rules
// follow FL Studio's event lane:
//
//  * Paint (left drag): every frame the piano roll sweeps the segment from the
//    previous pointer position to the current one. Every position on the way
//    edits the note stem nearest to it (all notes on that tick, so chords move
//    together), so fast drags never skip notes, very short notes are hit and a
//    click between stems takes the closer one. A note gets the segment's value
//    at its start (clamped to the segment's ends).
//  * Line (right drag): a straight line from A to the pointer; every note whose
//    start lies between them gets the line's value at its start.
// Both act on the selected notes only when a selection exists (`only`).
#pragma once
#include <cstddef>
#include <vector>
#include "NoteTools.h"

namespace gl::notelane {

// Value setter: writes a normalised value into one property of a note.
using SetFn = void (*)(Note& n, tools::NoteProp p, double v);

// Value of the segment (t0,v0)-(t1,v1) at tick t (clamped to the segment's
// ends; a vertical segment gives v1).
double segmentValue(double t0, double v0, double t1, double v1, double t);

// Paint targets: the editable notes (`only` empty = all) whose start is the
// nearest editable start for some position in [min(t0,t1), max(t0,t1)].
std::vector<size_t> nearestInSweep(const std::vector<Note>& notes, const std::vector<bool>& only, double t0, double t1);
// Line targets: the editable notes whose start lies in [min(t0,t1), max(t0,t1)].
std::vector<size_t> notesBetween(const std::vector<Note>& notes, const std::vector<bool>& only, double t0, double t1);

// Paint sweep: sets the targets to the segment's value at their start. Returns
// true when a note changed. `set` defaults to tools::setNotePropNorm.
bool paintSegment(std::vector<Note>& notes, const std::vector<bool>& only, tools::NoteProp p, double t0, double v0, double t1,
                  double v1, SetFn set = nullptr);

// Line tool: sets the notes between A and B (widened by `slackTicks` either
// side, e.g. half a pixel so a stem on the end pixel counts) to the line's
// value at their start (clamped to the ends). Returns true when a note changed.
bool applyLine(std::vector<Note>& notes, const std::vector<bool>& only, tools::NoteProp p, double tA, double vA, double tB, double vB,
               SetFn set = nullptr, double slackTicks = 0);

}  // namespace gl::notelane
