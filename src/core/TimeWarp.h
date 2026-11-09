// Gridline — K-85: warp markers, a time map shared by everything that plays
// recorded audio at a changing speed (audio clips in the playlist; built so
// other editors can use the same map).
//
// A warp marker pins a point of the file (`at`, seconds, in the file's own
// direction) to a warped time (`to`, seconds at the clip's own speed). Between
// two markers the audio plays evenly stretched so that it spans the distance
// between their warped times; before the first and after the last marker it
// plays at the clip's own speed (slope 1). A clip without markers is not
// warped at all. Markers are kept sorted with `at` and `to` both strictly
// increasing (normalizeWarp).
//
// The warp sits between the file and a clip's existing time map: the clip's
// start, end, slip, loop, stretch, tempo sync and pitch keep their meaning and
// act on warped time (engine/ClipOps.h clipSourceAt, engine/Program.cpp).
// Reversed clips read the file backwards: their warp is the same map seen from
// the file's end (WarpView), so reversing never rewrites the markers.
//
// Evaluation never allocates (the audio thread reads WarpPoint arrays the UI
// thread prepared).
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace gl {

struct WarpMarker {
  uint32_t id = 0;   // unique within its clip (collaboration merges per marker)
  double at = 0;     // file position, seconds, file direction
  double to = 0;     // warped time of that point, seconds
  bool operator==(const WarpMarker& o) const { return id == o.id && at == o.at && to == o.to; }
  bool operator!=(const WarpMarker& o) const { return !(*this == o); }
};

inline constexpr size_t kMaxWarpMarkers = 1024;
inline constexpr double kWarpMinSpeed = 1.0 / 8.0;  // a section's speed against the clip's own, at least
inline constexpr double kWarpMaxSpeed = 8.0;        // and at most
inline constexpr double kWarpMinSection = 0.002;    // seconds of warped time between two markers, at least

// One point of a warp in the clip's direction: warped time `w`, source `s`
// (seconds in the clip's direction, as Clip::audioStart). Sorted by `w`.
struct WarpPoint {
  double w = 0, s = 0;
};

// Source position at warped time `w` (slope 1 outside the points; n == 0: `w`).
double warpSourceAt(const WarpPoint* p, size_t n, double w);
// Warped time of source position `s` (the inverse).
double warpTimeAt(const WarpPoint* p, size_t n, double s);
// Source seconds per warped second at warped time `w` (the section's speed).
double warpSpeedAt(const WarpPoint* p, size_t n, double w);
inline double warpSourceAt(const std::vector<WarpPoint>& v, double w) { return warpSourceAt(v.data(), v.size(), w); }
inline double warpTimeAt(const std::vector<WarpPoint>& v, double s) { return warpTimeAt(v.data(), v.size(), s); }

// The markers seen in a clip's direction: reversed clips read the file from
// `last` (the last frame's position, seconds) backwards.
std::vector<WarpPoint> warpPoints(const std::vector<WarpMarker>& m, bool reversed, double last);

// Every section plays at the clip's own speed (no marker moves anything).
bool warpIsIdentity(const std::vector<WarpMarker>& m);

// Sorted by `at`; non-finite entries dropped; `at` within [0, maxAt]; an
// entry that does not keep `at` and `to` strictly increasing (at least
// kWarpMinSection of warped time and a speed within the limits from the
// marker before it) is dropped; ids made unique and nonzero; at most
// kMaxWarpMarkers. Returns true when it changed anything.
bool normalizeWarp(std::vector<WarpMarker>& m, double maxAt = 1e5);
// A fresh id for a new marker of `m`.
uint32_t nextWarpId(const std::vector<WarpMarker>& m);

}  // namespace gl
