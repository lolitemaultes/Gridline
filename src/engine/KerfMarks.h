// Gridline — Kerf's time and marker settings (J-182): the musical grid of a
// loop (4/4, from its length in beats), marker snapping (grid sizes, zero
// crossings, grid then the nearest zero crossing), nudging by one snap step,
// the ruler's units (seconds or bars:beats) and the text of exact positions,
// and region names of the classic engine kept after the sample path in the
// plugin data. Pure functions (UI and tests share them); no audio thread use.
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include "Sample.h"

namespace gl::kerfmarks {

// Snap sizes (gl.slicer parameter SlSnap). Off and zero crossings have no grid.
enum Snap { SnapOff, SnapZero, SnapBar, SnapHalfBar, SnapBeat, SnapHalfBeat, SnapThirdBeat, SnapQuarterBeat, SnapSixthBeat, SnapEighthBeat, SnapCount };
inline constexpr int kBeatsPerBar = 4;
inline constexpr int kTicksPerBeat = 96;  // ticks shown in bars:beats:ticks
// Ruler units (SlRuler).
enum Ruler { RulerSeconds, RulerBars, RulerCount };
// Units of a typed position (seconds, samples, bars:beats:ticks).
enum Unit { UnitSeconds, UnitSamples, UnitBars, UnitCount };

// Length of one grid step in beats (0: no grid).
double stepBeats(int snap);
bool isGrid(int snap);

// Beats in the loop: the given count when it is fitted to the tempo, else the
// power-of-two count that puts the loop between 80 and 160 BPM.
double loopBeats(double seconds, bool fitted, double fittedBeats);
// Frames of one beat (`frames` long loop of `beats` beats); 0 when unknown.
double framesPerBeat(double frames, double beats);

// The nearest zero crossing of the mono mix within `reachFrames` of f (f itself when none).
int64_t nearestZero(const Sample& smp, int64_t f, int64_t reachFrames);
// Where a marker dropped at frame f lands: the grid (step of `snap` beats), the
// nearest zero crossing (within 5 ms; SnapZero, or after the grid when
// `thenZero`), clamped to the sample. Off: f clamped.
int64_t snapFrame(const Sample* smp, int64_t f, int snap, bool thenZero, double framesPerBeat);
// One snap step from f in direction dir (-1 / +1): the next grid line, the next
// zero crossing (SnapZero), or 1 ms (no snap); clamped to the sample.
int64_t nudgeFrame(const Sample* smp, int64_t f, int dir, int snap, bool thenZero, double framesPerBeat);

// bars:beats (1-based) of a frame; `ticks` adds :ticks (0..95, three digits).
std::string barsBeats(double frame, double framesPerBeat, bool ticks);
// A position as text in a unit (seconds "1.250 s", samples "55125", bars "2:1:048").
std::string formatPosition(double frame, int unit, double rate, double framesPerBeat);
// Typed text to frames (false: not a position). Seconds accept "1.25" and
// "1.25 s"; samples a whole count; bars "2", "2:3", "2:3:48" (also with '.').
bool parsePosition(const std::string& text, int unit, double rate, double framesPerBeat, double& frame);

// Ruler step in beats so that labels stay `minPx` apart on `pxPerBeat`.
double rulerStepBeats(double pxPerBeat, double minPx);
// Ruler step in seconds likewise (`pxPerSecond`).
double rulerStepSeconds(double pxPerSecond, double minPx);

// Classic engine region names: "<sample path>\nkerf-names\n<slot> <name>"...
// (slot = the marker parameter). Data without names is the path alone.
bool hasNames(const std::string& data);
std::string pathOf(const std::string& data);  // the first line
std::map<int, std::string> namesOf(const std::string& data);
std::string withNames(const std::string& path, const std::map<int, std::string>& names);
// A name as typed, made storable (one line, trimmed, at most 64 bytes on a character boundary).
std::string cleanName(const std::string& s);

}  // namespace gl::kerfmarks
