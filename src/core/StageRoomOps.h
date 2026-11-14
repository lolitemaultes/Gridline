// Gridline - Stage rooms (docs/stage-view.md, "Rooms"). A project's mixer tracks are grouped into one
// or more rooms, for example one per song of an album. Each room has its own stage settings (its space
// and that space's track, the back of the stage, the stage width, echoes, pre-delay) and its own
// tracks; a track is in exactly one room (MixerTrack::room, kNoId = the first room). Song::stage is
// the first room and Song::stageRooms the others, so a one-room project is saved exactly as before
// rooms existed. All rooms always play: the room shown on the Stage is only what one computer looks at.
//
// Rooms are addressed by index here (0 = the first, roomCount() - 1 the last). Every edit is one undo
// step and goes through the Document, so undo, saving and collaboration see it like any other change.
#pragma once
#include <string>
#include <vector>
#include "Document.h"
#include "Song.h"

namespace gl::roomops {

// ---------------------------------------------------------------- reading ----
int roomCount(const Song& s);
// Room `i` (clamped into range: never fails).
const StageSettings& roomAt(const Song& s, int i);
StageSettings* roomAtMut(Song& s, int i);  // nullptr when out of range
// The id of room `i` (the first room may have none: kNoId).
ObjectId roomIdAt(const Song& s, int i);
// The index of the room with id `id` (kNoId: the first), -1 when no room has it.
int roomIndex(const Song& s, ObjectId id);
// The room a track stands in (an id no room has any more reads as the first room).
int trackRoom(const Song& s, const MixerTrack& m);
int trackRoom(const Song& s, ObjectId track);
// The room whose space track `track` is (-1: none).
int spaceRoom(const Song& s, ObjectId track);
bool isSpaceTrack(const Song& s, ObjectId track);
// The room's name: its own, else "Room N" (English; the UI translates "Room %d").
std::string roomName(const Song& s, int i);
// The room's colour: its own, else one from a small palette by position.
Rgb roomColor(const Song& s, int i);
// The name a room's space track gets: "Room" in a one-room project, else "<room> space".
std::string spaceTrackName(const Song& s, int i);
// The mixer tracks of room `i` in mixer order (never the master or a space track).
std::vector<ObjectId> roomTracks(const Song& s, int i);
// K-270: a track in use: a channel plays into it, or it has effects, an input, a name, routing to
// another track or a place on the stage (what the Stage shows by default). Never the master or a space track.
bool trackInUse(const Song& s, const MixerTrack& m);
// Room `i`'s tracks in use (mixer order): the tracks that keep it from being deleted. Its space track
// never counts (it goes with the room), nor do its unused inserts.
std::vector<ObjectId> roomUsedTracks(const Song& s, int i);
// Why room `i` cannot be deleted. `tracks`: how many tracks it still has (HasTracks).
enum class DeleteBlock { None, NoSuchRoom, LastRoom, HasTracks };
DeleteBlock deleteBlock(const Song& s, int i, size_t* tracks = nullptr);
// K-270: soloed rooms (this computer only, never saved): the tracks that still play when only the rooms
// with ids `rooms` are heard (theirs, everything downstream of them such as buses and their spaces, and
// the master), sorted. Empty when none of `rooms` exists.
std::vector<ObjectId> hearOnlyKeep(const Song& s, const std::vector<ObjectId>& rooms);
// Mutes every other track in `s` (a copy for the engine, built on the UI thread). False: nothing to do
// (one room, or none of `rooms` exists).
bool hearOnly(Song& s, const std::vector<ObjectId>& rooms);
// The tracks that feed `tracks` through sends (recursively), not already in `tracks`, never the master
// or a space track.
std::vector<ObjectId> feedersOf(const Song& s, const std::vector<ObjectId>& tracks);
// The tracks used by arrangement `arr` (Song::arrangements index; the current one is the playlist):
// the mixer tracks of every channel playing in its clips (patterns' notes and steps, audio clips),
// plus the tracks they send into, never the master or a space track. Mixer order.
std::vector<ObjectId> tracksOfArrangement(const Song& s, int arr);
// The tracks used by pattern `pattern` (its channels' mixer tracks).
std::vector<ObjectId> tracksOfPattern(const Song& s, ObjectId pattern);
// Arrangements other than `arr` that also use `track` (names; for "also used in").
std::vector<std::string> otherArrangementsUsing(const Song& s, ObjectId track, int arr);

// ---------------------------------------------------------------- templates ----
// What a room template holds: the room's sound settings only, never its tracks or their places.
struct RoomTemplate {
  std::string name;
  std::string hint;        // one line (factory: English, marked for translation)
  bool factory = false;
  std::string folder;      // below the templates folder (user templates)
  std::string file;        // absolute path (user templates)
  int roomType = 0;        // the space (core/StageRooms.h)
  float depthStrength = 0.5f;
  float width = 1.0f;
  float level = 0.8f;      // the space track's fader (0.8: 0 dB)
  double size = -1;        // the space's size 0 .. 1 (-1: the space type's own)
  std::vector<float> reverb;  // the space's whole reverb state (empty: the space type's own)
  bool preDelay = true, early = true, frontQuiet = true;
  bool spaceOn = true;     // the space track unmuted
};
const std::vector<RoomTemplate>& factoryTemplates();
// The settings of room `i` as a template named `name`.
RoomTemplate templateOf(const Song& s, int i, const std::string& name);
std::string templateFolder(const std::string& presetRoot);  // <presets>/Stage rooms
std::vector<RoomTemplate> listUserTemplates(const std::string& presetRoot);
std::string templateToJsonText(const RoomTemplate& t);
bool templateFromJsonText(const std::string& text, RoomTemplate& out, std::string* err = nullptr);
// Saves as <presets>/Stage rooms/<relName>.json (relName may hold folders). `path` gets the file.
bool saveUserTemplate(const std::string& presetRoot, const RoomTemplate& t, const std::string& relName, std::string* path = nullptr,
                      std::string* err = nullptr);
bool deleteUserTemplate(const RoomTemplate& t);

// ---------------------------------------------------------------- edits ----
// A new room after the last one, named `name` ("" = "Room N") with colour `color` (0 = automatic):
// its settings copied from room `copyFrom` (its space, depth, width, echoes and the space track's sound,
// never its tracks), or taken from `tmpl` when given. With a space chosen its space track is made at
// once. It starts empty. One undo step. Returns the new room's index (-1: none).
int addRoom(Document& d, int copyFrom, const RoomTemplate* tmpl = nullptr, const std::string& name = "", Rgb color = 0);
bool renameRoom(Document& d, int i, const std::string& name);
bool setRoomColor(Document& d, int i, Rgb color);
// Moves room `from` to position `to`. One undo step.
bool moveRoom(Document& d, int from, int to);
// Deletes room `i` (K-270): only an empty room (deleteBlock() == None: no tracks in use; move them to
// another room first) and never the last one. Its space track goes with it; its unused inserts go to the
// room before it (the next one for the first room). One undo step. False: refused, nothing changed.
bool deleteRoom(Document& d, int i);
// Moves `tracks` into room `to`: they keep their places, effects, routing and channel links, and take
// on the new room's sound (a room send goes to the new room's space; one still at the old space's
// level takes the new one's; a track that plays into the master with no room send gets one into the
// new room's space, at a new track's level). `feeders`: the tracks that send into them come too. Tracks already
// there, the master and space tracks are skipped. One undo step. Returns how many moved.
int moveTracksToRoom(Document& d, const std::vector<ObjectId>& tracks, int to, bool feeders = false);
// Sets room `i`'s settings from a template (its tracks and their places stay). One undo step.
bool applyTemplate(Document& d, int i, const RoomTemplate& t);

}  // namespace gl::roomops
