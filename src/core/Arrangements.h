// Gridline — several playlist arrangements per song, and the live area.
//
// A song plays one arrangement at a time. The current one lives in
// Song::playlist / tracks / markers / live so every editor keeps working on
// those; the others are parked in Song::arrangements (see Song.h). These
// helpers switch, add, copy, rename and remove arrangements and keep parked
// arrangements consistent when patterns, channels or automation clips go away.
#pragma once
#include <functional>
#include <string>
#include <vector>
#include "Song.h"

namespace gl {

int arrangementCount(const Song& s);
std::string arrangementName(const Song& s, int index);
// Makes Song::arrangements explicit (one entry per arrangement).
void ensureArrangementList(Song& s);
// Parks the current arrangement and makes `index` current. False if out of range.
bool selectArrangement(Song& s, int index);
// Appends a new arrangement (empty, or a copy of the current one with new clip
// ids) and returns its index; the current arrangement does not change.
int addArrangement(Song& s, const std::string& name, bool copyCurrent);
bool renameArrangement(Song& s, int index, const std::string& name);
// Removes an arrangement (never the last one). Removing the current one makes
// its neighbour current.
bool removeArrangement(Song& s, int index);
// A name not used by any arrangement yet ("Arrangement 2", …).
std::string uniqueArrangementName(const Song& s, const std::string& base);

// Visits the clips of every arrangement (the current one first).
void forEachArrangementClip(Song& s, const std::function<void(Clip&)>& fn);
void forEachArrangementClip(const Song& s, const std::function<void(const Clip&)>& fn);
// Drops clips of parked arrangements whose pattern, audio channel or
// automation clip no longer exists. Returns how many were removed.
int pruneParkedArrangements(Song& s);

// --- live area -------------------------------------------------------------
// True if the clip sits in the current arrangement's live area (launched by
// hand, not played by the song).
inline bool inLiveArea(const Song& s, const Clip& c) { return c.start < s.live.areaEnd; }
// Clip indices (into Song::playlist) of the live clips on one track, by start.
std::vector<int> liveClips(const Song& s, int track);
// Launch grid in ticks for a track setting (`clipLength` for LaunchGrid::ClipLength); 0 = immediately.
Tick launchGridTicks(const Song& s, LaunchGrid g, Tick clipLength);
const char* launchGridName(LaunchGrid g);
const char* padModeName(PadMode m);
const char* followModeName(FollowMode m);

}  // namespace gl
