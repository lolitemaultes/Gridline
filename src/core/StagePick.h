// Gridline - tracks from the playlist on the Stage (docs/stage-view.md, "Tracks from the playlist").
// Finds the mixer tracks a part of the arrangement plays through, and acts on them on the Stage:
//   - from the playlist selection: the selected clips (an audio clip's channel, a pattern clip's
//     channels with notes or steps, through each channel's mixer routing) or the selected playlist
//     tracks (their clips and the mixer track or instrument they are linked to);
//   - from a marker area: every clip that plays inside a time range (from a marker to the next, between
//     two markers, or the time selection). A clip that straddles an edge counts; a clip that only
//     touches it (ends where the range starts) does not; muted clips and clips on muted tracks do not
//     play, so they are left out unless asked for.
// With `buses` the tracks they send into come too (a drum bus, a group), as "Bring tracks in" does;
// never the master or a room's space track. The actions (add to the stage, move to a room, arrange
// together) are one undo step each and go through the Document (pan, distance and the rooms' own
// edits), so saving, undo and collaborators see them like any other Stage edit. Nothing new is saved.
#pragma once
#include <string>
#include <vector>
#include "Document.h"
#include "Song.h"
#include "StageMap.h"

namespace gl::stagepick {

struct Options {
  bool buses = true;   // the tracks they send into come too
  bool muted = false;  // muted clips (and clips on muted playlist tracks) count
};

struct Found {
  std::vector<ObjectId> tracks;  // mixer order
  std::vector<ObjectId> buses;   // the part of `tracks` reached only through sends
  int clips = 0;                 // clips that counted
  int mutedLeftOut = 0;          // muted clips left out
  int toMaster = 0;              // clips whose channels play straight into the master (no track of their own)
  bool empty() const { return tracks.empty(); }
};

// The selected clips' tracks (automation clips have none). `o.muted` decides whether muted clips count.
Found fromClips(const Song& s, const std::vector<ObjectId>& clips, const Options& o);
// The selected playlist tracks (lanes of the current arrangement): their clips, and the mixer track an
// audio track is linked to or the channel an instrument track plays.
Found fromTracks(const Song& s, const std::vector<int>& lanes, const Options& o);
// Every clip overlapping [start, end) (end <= start: nothing).
Found fromRange(const Song& s, Tick start, Tick end, const Options& o);

// ---------------------------------------------------------------- marker areas
struct Area {
  Tick start = 0, end = 0;
  int marker = -1, markerTo = -1;  // the markers it runs between (markerTo -1: the song end)
  std::string name;                // the first marker's name ("" when it has none)
};
// One area per marker: from it to the next marker (the last one: to the song end, at least a bar).
std::vector<Area> markerAreas(const Song& s);
// From marker `from` to marker `to` (to <= from or out of range: the next marker). marker -1: none.
Area betweenMarkers(const Song& s, int from, int to);
// "Verse" or "Marker 3": the name a new room gets from the area.
std::string areaName(const Song& s, const Area& a);

// ---------------------------------------------------------------- places
// Free places for `n` tracks across the front of a stage where `taken` stand: the front row first,
// from the centre outward, left and right in turn, a step (0.2) apart and clear of every taken place
// on that row, then the next rows back. Sorted left to right. `avoidCentre`: never the centre front
// (pan 0, distance 0), which a track that is not in use would not be shown at.
std::vector<stage::Pos> frontPlaces(const std::vector<stage::Pos>& taken, size_t n, bool avoidCentre);

enum class Arrange { SpreadLeftRight, Forward, Back, LineUpFront, Centre, Left, Right, Count };
// Where `in` (mixer order) go. Spread and Line up keep the left-to-right order they had; the shifts
// move the whole set by one step and stop at the stage's edges, so their spacing never changes.
std::vector<stage::Pos> arrange(const std::vector<stage::Pos>& in, Arrange how);

// ---------------------------------------------------------------- edits (one undo step each)
// The tracks of `ids` room `room`'s stage does not show yet (in another room, or not in use) come
// onto it: they move into the room and stand in free places across its front. `everyTrack`: the
// stage shows every insert track (ui.stage.tracks). Returns how many were added.
int addToStage(Document& d, const std::vector<ObjectId>& ids, int room, bool everyTrack = false);
// Moves `ids` to room `room`, or to a new room named `newName` (room < 0; its settings copied from
// room `copyFrom`). `outRoom` gets the room they are in now. Returns how many moved.
int moveToRoom(Document& d, const std::vector<ObjectId>& ids, int room, const std::string& newName, int copyFrom, int* outRoom = nullptr);
// Arranges the tracks of `ids` (locked ones stay) as `how` says. Returns how many moved.
int arrangeTracks(Document& d, const std::vector<ObjectId>& ids, Arrange how);

}  // namespace gl::stagepick
