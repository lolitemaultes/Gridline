// Gridline — K-85: editing an audio clip's warp markers (core/TimeWarp.h).
//
// Positions are song ticks; every operation acts on one clip and keeps the
// markers valid (normalizeWarp), and the clip's own start (its first sample at
// the clip's left edge) where it is. Dragging a marker stretches the sections
// on both sides of it; the audio before the clip's start and after its last
// marker is not moved. Undo, collaboration and saving see plain Clip edits.
#pragma once
#include <cstdint>
#include <set>
#include <vector>
#include "../core/Automation.h"
#include "../core/Groove.h"
#include "../core/Song.h"

namespace gl {

// A marker as the playlist shows it: its song tick, and whether it moved anything.
struct WarpMarkerView {
  uint32_t id = 0;
  double tick = 0;
  double speed = 1;  // the section after it, against the clip's own speed (1: unchanged)
};

// The clip's markers inside its span, left to right.
std::vector<WarpMarkerView> warpMarkerViews(const Song& song, const Clip& c, const TempoMap& map);
// The song tick where the clip plays source second `s` (its own direction); NaN outside.
double clipTickOfSource(const Song& song, const Clip& c, const TempoMap& map, double s);

// A new marker on the audio playing at `tick` (with `snap`: on the nearest
// transient within 40 ms). Nothing moves yet. Returns its id (0: none, e.g. too
// close to another marker or outside the clip).
uint32_t addWarpMarker(Song& song, ObjectId clip, const TempoMap& map, double tick, bool snap, double snapTicks = -1);
// The marker's audio moved to play at `tick` (the sections on both sides
// stretch, within the speed limits). The first drag of a clip pins its start
// with a marker of its own, so the clip's first sample stays at its left edge.
bool moveWarpMarker(Song& song, ObjectId clip, const TempoMap& map, uint32_t id, double tick);
// The marker stays where it plays and pins other audio: the audio heard at `tick` now.
bool slideWarpMarker(Song& song, ObjectId clip, const TempoMap& map, uint32_t id, double tick);
bool deleteWarpMarker(Song& song, ObjectId clip, uint32_t id);
// Removes every marker: the clip plays at its own speed again.
bool clearWarp(Song& song, ObjectId clip);
// Markers on the clip's transients (none moves anything). Returns how many were added.
int warpMarkersAtTransients(Song& song, ObjectId clip, const TempoMap& map, double sensitivity = 0.5);
// Moves markers toward the grid: strength 1 lands them on it, 0.5 halfway.
// `only` (when set): those markers only. Returns how many moved.
int quantizeWarpMarkers(Song& song, ObjectId clip, const TempoMap& map, Tick grid, double strength,
                        const std::set<uint32_t>* only = nullptr);
// Audio quantize: markers on the transients, then quantized. Returns how many moved.
int quantizeAudioClip(Song& song, ObjectId clip, const TempoMap& map, Tick grid, double strength, double sensitivity = 0.5);

// K-151: the clip's transients as song ticks with their peak level (a groove
// taken from audio: groove::extract).
std::vector<groove::Onset> clipGrooveOnsets(const Song& song, const Clip& c, const TempoMap& map, double sensitivity = 0.5);
// K-151: the song's swing applied to an audio clip: warp markers on its transients,
// each moved toward the nearest line of the swung grid (song bars, the groove's
// grid; the pattern step counts as sixteenths). strength 1 lands them on it.
// Returns how many moved.
int grooveAudioClip(Song& song, ObjectId clip, const TempoMap& map, double strength, double sensitivity = 0.5);
int grooveAudioClip(const Song& song, Clip& c, const TempoMap& map, double strength, double sensitivity = 0.5);
// The swung grid line nearest song tick t (the song's groove and swing).
double grooveTarget(const groove::Warp& w, double t);

// The same on a clip of `song` the caller holds (a copy being edited: the
// playlist edits it and records the result as one clip edit).
// snapTicks > 0 also limits the transient search to that many ticks either side of `tick` (the
// playlist passes a few pixels, so a deep zoom never pulls a marker far from the pointer).
uint32_t addWarpMarker(const Song& song, Clip& c, const TempoMap& map, double tick, bool snap, double snapTicks = -1);
bool moveWarpMarker(const Song& song, Clip& c, const TempoMap& map, uint32_t id, double tick);
bool slideWarpMarker(const Song& song, Clip& c, const TempoMap& map, uint32_t id, double tick);

// Where a dragged marker goes: it keeps its offset from the pointer (a grab
// never jumps) and lands on the nearest line of `grid` (1: free, Alt), as a
// clip move does. `markerTick` and `grabTick`: the marker and the pointer at
// the grab.
double warpDragTick(double markerTick, Tick grabTick, Tick pointerTick, Tick grid);
bool deleteWarpMarker(Clip& c, uint32_t id);
bool clearWarp(Clip& c);
int warpMarkersAtTransients(const Song& song, Clip& c, const TempoMap& map, double sensitivity = 0.5);
int quantizeWarpMarkers(const Song& song, Clip& c, const TempoMap& map, Tick grid, double strength, const std::set<uint32_t>* only = nullptr);
int quantizeAudioClip(const Song& song, Clip& c, const TempoMap& map, Tick grid, double strength, double sensitivity = 0.5);

}  // namespace gl
