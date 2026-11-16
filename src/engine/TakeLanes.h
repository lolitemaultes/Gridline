// Gridline — take lanes and comping (K-123).
//
// Loop recording leaves every pass of a track on that track as clips of one
// take group (Clip::takeGroup, Clip::take = the pass). The track's own row
// plays the comp: the clips that are not muted. With PlaylistTrack::takeLanes
// on, the playlist shows one lane per take under the track; a swipe across a
// lane picks that take for the range (the clips are split at its edges and
// every other take is muted there). All of it is ordinary song data, so it is
// saved, undone and shared like any other clip edit. Flatten renders the comp
// into one file and one clip.
#pragma once
#include <string>
#include <vector>
#include "../core/Song.h"

namespace gl::takes {

// Lanes of a playlist track: 1 + the highest take number of the take-group
// clips on it (0: the track holds no takes).
int laneCount(const Song& song, int track);
// The track has takes and shows them as lanes.
bool lanesShown(const Song& song, int track);
// The take groups with clips on a track (in order of first appearance).
std::vector<ObjectId> groupsOn(const Song& song, int track);

// Within [a, b) take `take` plays wherever it has audio, for every take group on
// the track; elsewhere the comp stays. Clips of the same take that meet again
// with their audio continuous are joined. True when anything changed.
bool compRange(Song& song, int track, int take, Tick a, Tick b);
// A click on a lane: take `take` plays over the comp section at `at` (the piece
// playing there, else the take's own clip there). True when anything changed.
bool compPick(Song& song, int track, int take, Tick at);
// The take that plays at `at` on the track (-1: none).
int playingTake(const Song& song, int track, Tick at);
// Which takes play anywhere on the track (index = take number).
std::vector<char> takesInComp(const Song& song, int track);
// Joins neighbouring pieces of the same take whose audio runs on without a jump
// (what repeated comps leave behind). Returns the number of joins.
int joinPieces(Song& song, ObjectId group);

// Removes the clips of the group that are not in the comp (the muted ones).
// Returns how many were removed.
int removeUnused(Song& song, ObjectId group);
// Renders the comp of `group` (its playing clips, with short crossfades at the
// joins) into one WAV in `dir`, and replaces every clip of the group with one
// clip of it on the same track, played through the takes' mixer track. kNoId
// (and `err`) on failure.
ObjectId flatten(Song& song, ObjectId group, const std::string& dir, std::string* err = nullptr);
// Projects saved before take lanes: moves a group spread over several tracks
// onto its first track and shows the lanes. Returns the clips moved.
int gatherIntoLanes(Song& song, ObjectId group);
// The group's clips lie on more than one track.
bool spreadOverTracks(const Song& song, ObjectId group);

}  // namespace gl::takes
