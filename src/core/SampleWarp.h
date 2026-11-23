// Gridline: warp markers on a sample (the sampler, Kerf, Pads and Seltmann),
// and the edits every waveform's warp mode makes (sample instruments, Loupe,
// the Tracker sample editor). The markers are the same WarpMarker as an audio
// clip's (core/TimeWarp.h): `at` a point of the file in seconds, `to` the
// warped time it plays at. Here warped time is plain seconds from the start of
// the warped sample (no song ticks), so one set of operations serves every
// place that is not a playlist clip.
//
// A sample's warp is kept with the instrument (PluginState::warp, compact
// JSON): on or off, the loop's tempo, and the markers. It is rendered off the
// audio thread into the sample the voices play (engine/SampleWarpRender.h), so
// every note plays the warped audio; peers render it from the markers.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "TimeWarp.h"

namespace gl {

struct PluginState;

struct SampleWarp {
  bool on = false;      // the warped sample plays (off: the markers are kept but the sample plays as it is)
  double bpm = 0;       // the loop's tempo (0: not set); markers snap to its beats
  std::vector<WarpMarker> markers;
  bool active() const { return on && markers.size() >= 2 && !warpIsIdentity(markers); }
  bool operator==(const SampleWarp& o) const { return on == o.on && bpm == o.bpm && markers == o.markers; }
  bool operator!=(const SampleWarp& o) const { return !(*this == o); }
};

// Compact JSON ({"on":true,"bpm":92,"m":[[id,at,to],...]}); empty text for a default warp.
std::string sampleWarpToText(const SampleWarp& w);
bool sampleWarpFromText(const std::string& text, SampleWarp& out);
SampleWarp sampleWarpOf(const PluginState& st);
void setSampleWarp(PluginState& st, const SampleWarp& w);
// The instrument types whose sample can be warped (sampler, Kerf, Pads, Seltmann).
bool sampleWarpSupported(const std::string& type);

// A marker in the warped sample: its id, warped time and the speed of the section after it
// (source seconds per warped second; 1: unchanged).
struct WarpMarkerAt {
  uint32_t id = 0;
  double w = 0;
  double speed = 1;
};
std::vector<WarpMarkerAt> sampleWarpViews(const std::vector<WarpMarker>& m);
// The warped length of a file `fileSec` long, and the warped time of file second s (and back).
double sampleWarpLength(const std::vector<WarpMarker>& m, double fileSec);
double sampleWarpTimeOf(const std::vector<WarpMarker>& m, double s);
double sampleWarpSourceAt(const std::vector<WarpMarker>& m, double w);

// ---- edits (each keeps the markers valid: normalizeWarp) ----
// A new marker at warped time w (with `onsets`, file seconds: on the nearest one within `reach`
// seconds). Nothing moves. Returns its id (0: too close to another, outside the file).
uint32_t sampleWarpAdd(std::vector<WarpMarker>& m, double w, double fileSec, const std::vector<double>* onsets = nullptr, double reach = 0.04);
// The marker's audio now plays at warped time w: the sections on both sides stretch (within the
// speed limits). The file's start is pinned by a marker of its own on the first move.
bool sampleWarpMove(std::vector<WarpMarker>& m, uint32_t id, double w, double fileSec);
// The marker stays where it plays and pins the audio heard at warped time w now (Shift+drag).
bool sampleWarpSlide(std::vector<WarpMarker>& m, uint32_t id, double w, double fileSec);
bool sampleWarpRemove(std::vector<WarpMarker>& m, uint32_t id);
// Markers on the onsets (file seconds) that have none yet; nothing moves. Returns how many.
int sampleWarpAtOnsets(std::vector<WarpMarker>& m, const std::vector<double>& onsets, double fileSec);
// Moves markers toward the nearest line of a grid of `step` warped seconds (strength 1: onto it).
int sampleWarpQuantize(std::vector<WarpMarker>& m, double step, double strength, double fileSec);
// Where a dragged marker goes: it keeps its offset from the pointer and lands on the nearest line
// of `step` seconds (0: free).
double sampleWarpDragTarget(double markerW, double grabW, double pointerW, double step);

}  // namespace gl
