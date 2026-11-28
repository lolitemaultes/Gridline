// Gridline — marker areas: the stretches of the current arrangement between one
// song marker and the next, each exported on its own (File > Export > Marker
// areas). Someone who keeps several songs in one arrangement, a marker at the
// start of each, gets one file (or one folder of stems) per song.
//
// An area runs from its marker to the next marker at a later position; the last
// one runs to the end of the song (the end of its last clip). Meter and key
// markers are not area markers: they change the meter or key inside an area.
// The stretch before the first marker is an area only when a clip sounds there.
// Markers at the same position open one area (named by the first named one).
//
// Each area renders through the normal export (runExport, Range scope): time 0
// of its file is the area's start, the tempo, meter and automation are what
// playback has there, the tail (Leave / Wrap) is the release and effects ringing
// after the area's last note, never the next area's notes.
#pragma once
#include <atomic>
#include <functional>
#include <string>
#include <vector>
#include "../core/Song.h"
#include "ExportJob.h"

namespace gl {

struct MarkerArea {
  Tick start = 0, end = 0;  // song ticks, end > start
  std::string name;         // the marker's name; "Marker N" for an unnamed one, "Start" before the first marker
  int marker = -1;          // index in Song::markers of the marker that opens it (-1: before the first marker)
  bool hasContent = false;  // a clip that sounds (not muted, not automation) overlaps it
};

// The current arrangement's marker areas in order, cut where the Track Receipt cuts them
// (markers::areas, core/MarkerAreas.h, which also says which markers open an area). The
// Session's slot clips (before the live area's end, Clips layout) never count: the areas are the
// arrangement's. No area markers: no areas.
std::vector<MarkerArea> markerAreas(const Song& song);

// File names (no extension) of the areas from a pattern: %P project, %M marker (area) name,
// %N area number (01, 02...), %R sample rate, %D date. Made safe for file systems (no
// separators or reserved characters, no reserved Windows names, not empty); names that would
// repeat (ignoring case) get " 2", " 3"...
inline constexpr const char* kMarkerAreaPattern = "%P - %M";
std::vector<std::string> markerAreaFileNames(const std::vector<MarkerArea>& areas, const std::string& pattern, const std::string& project,
                                             int sampleRate = 44100);

// What became of each area of an export.
struct MarkerAreaResult {
  std::string name;                // its file name (no extension)
  std::vector<std::string> files;  // written
  std::vector<std::string> errors;
  bool done = false;               // rendered to the end (files kept even when a later area is cancelled)
};

// Exports `areas` one after the other with the settings `s` (formats, quality, tail, splits,
// MIDI), each as `names[i]` in `s.folder`; with a split on, each area gets its own folder
// `s.folder/names[i]` holding its mix and its parts. Cancel stops at once: the areas finished
// before stay on disk, the one under way is removed. `progress` gets 0..1 over every area.
// The outcome's files are every file written and kept; `perArea` (if given) has one entry per area.
ExportOutcome runMarkerAreaExport(const Song& song, const ExportSettings& s, const std::vector<MarkerArea>& areas,
                                  const std::vector<std::string>& names,
                                  const std::function<void(double, const std::string&)>& progress = {},
                                  const std::atomic<bool>* cancel = nullptr, std::vector<MarkerAreaResult>* perArea = nullptr);

}  // namespace gl
