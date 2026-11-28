// Gridline - marker areas: the arrangement cut at its markers. Each area runs from a marker to
// the next one (the last to the end); a stretch before the first marker is an area of its own.
// The one place the song is cut into areas: the Track Receipt's areas and the Export audio
// dialog's marker areas (engine/MarkerAreas.h, which adds content, display names and the export)
// both use it, so they cut the song in the same places.
#pragma once
#include <string>
#include <vector>
#include "Song.h"

namespace gl::markers {

// True for the markers that open an area: plain markers, with or without a playback action.
// Meter and key markers change the meter or key inside an area and open none.
bool isAreaMarker(const Marker& m);

struct Area {
  Tick start = 0, end = 0;  // song ticks [start, end)
  std::string name;         // the marker's name ("" for the stretch before the first marker, or an unnamed marker)
  int marker = -1;          // index into the markers it was made from (-1: before the first marker)
};

// The areas of `markers` inside [start, end) (song ticks; markers need not be sorted). Every
// area marker starts an area; markers at one position make one area, named by the first named
// one (the first one when none has a name). Areas that would be empty (a marker at or after
// `end`) are left out; a marker before `start` moves to it, the one nearest to `start` naming
// the area there. No area marker before `end`: empty (the caller treats the whole range as one
// section).
std::vector<Area> areas(const std::vector<Marker>& markers, Tick start, Tick end);

}  // namespace gl::markers
