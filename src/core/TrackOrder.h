// Gridline: putting mixer tracks and playlist tracks in a new order (GitHub issue #46).
//
// Mixer tracks are referenced by id almost everywhere (channels, sends, sidechains, the Stage's
// room, playlist audio tracks, controller links, automation owners), so those follow a track by
// themselves. A reorder fixes the places that store a mixer *index* instead, so the routing
// follows the tracks, not the numbers:
//   * send level parameters (Params.h sendParam: kSendBase + destination index) wherever a
//     ParamId is stored: automation clips, pattern automation, MIDI / OSC controller links and
//     internal controller links;
//   * effect parameters that name a destination track (PluginInfo::sendParams, the Mixer Send's
//     target), as a mixer index or as a route number of the hosting track (J-45);
//   * side input route numbers (PluginInfo::sideInParam) of effects and instruments.
// Playlist clips sit on a track by its index: a reorder moves them with their tracks, and track
// groups (PlaylistTrack::groupSize) keep their members.
#pragma once
#include <set>
#include <vector>
#include "Document.h"
#include "Song.h"

namespace gl::trackorder {

// ---------------------------------------------------------------- mixer ----
// Puts the mixer in `order` (every mixer track id once, the master first) and remaps every stored
// mixer index (see above). False, with the song unchanged, when `order` is not such a list.
bool reorderMixer(Song& s, const std::vector<ObjectId>& order);
// The mixer order after moving `ids` (kept in their mixer order; the master never moves) so they
// stand before the track at mixer index `before` (1 .. size; size = after the last track).
// Empty when nothing can move.
std::vector<ObjectId> mixerOrderAfterMove(const Song& s, const std::vector<ObjectId>& ids, int before);
// The same as one undo step ("move mixer track" / "move mixer tracks"). `dock` other than
// kKeepDock also docks the moved tracks there (MixerDock: -1 left, 0 middle, 1 right), as when
// they are dropped among the strips of another dock. False when nothing changed.
inline constexpr int kKeepDock = 99;
bool moveMixerTracks(Document& d, const std::vector<ObjectId>& ids, int before, int dock = kKeepDock);
// Moves the tracks one place left (dir < 0) or right (dir > 0) keeping their order; refused when
// one of them is already at that end. One undo step.
bool stepMixerTracks(Document& d, const std::vector<ObjectId>& ids, int dir);

// ------------------------------------------------------------- playlist ----
struct PlaylistMove {
  std::vector<int> order;  // order[new index] = old index
  std::set<int> moved;     // old indexes of the tracks that move (group members included)
  bool empty() const { return order.empty(); }
};
// The tracks to move with `tracks`: a group header brings its group along.
std::set<int> withGroups(const Song& s, const std::vector<int>& tracks);
// The playlist order after moving `tracks` (and their groups) so they stand before track `before`
// (0 .. size; size = after the last). Empty when nothing would change.
PlaylistMove playlistOrderAfterMove(const Song& s, const std::vector<int>& tracks, int before);
// Applies a move to the current arrangement: tracks (name, colour, height, mute, lock, icon, mode
// and links, live settings) and their clips. A group keeps its members; tracks dropped inside a
// group join it, members moved out of it leave it.
bool reorderPlaylist(Song& s, const PlaylistMove& m);
// One undo step. `newIndexes` (optional) receives where the moved tracks are now, in order.
bool movePlaylistTracks(Document& d, const std::vector<int>& tracks, int before, std::vector<int>* newIndexes = nullptr);
// One place up (dir < 0) or down (dir > 0), past the neighbouring track (a whole group when the
// neighbour heads a folded group). One undo step.
bool stepPlaylistTracks(Document& d, const std::vector<int>& tracks, int dir, std::vector<int>* newIndexes = nullptr);

}  // namespace gl::trackorder
