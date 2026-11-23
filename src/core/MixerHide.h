// Gridline — hiding, narrowing and folding mixer strips (GitHub #58). Pure logic (no UI): which
// strips the mixer views show and how, the edits behind the menus (each one undo step, saved with
// the project and shared with collaborators like any mixer track edit), and the tracks a control
// surface's banks walk. A hidden or folded track still plays and routes exactly as before: this is
// only how the mixer shows it. The master is never hidden, narrowed or folded away.
#pragma once
#include <cstddef>
#include <string>
#include <vector>
#include "Document.h"
#include "Song.h"

namespace gl::mixhide {

// How one place in a mixer view shows.
enum class Show {
  Strip,   // the track's full strip
  Narrow,  // the track's slim strip (name, meter, mute and solo)
  Group,   // a folded group's one strip (stands where its first member would)
};
struct Entry {
  size_t index = 0;  // mixer index (Group: its first member in the order given)
  Show show = Show::Strip;
  std::string group;  // Group: the group's name
  int members = 0;    // Group: how many tracks it holds
};

// Whether `group` is folded (any member says so, as a linked group).
bool groupFolded(const Song& s, const std::string& group);
// Whether track `i` has no strip of its own (hidden, or in a folded group). Never the master.
bool trackHidden(const Song& s, size_t i);
// The places a view shows for the mixer indexes `order` (in the view's own order): hidden tracks
// left out, each folded group once (where its first member stands), the others full or narrow.
std::vector<Entry> arrange(const Song& s, const std::vector<size_t>& order);
// How many tracks have no strip of their own (hidden, or in a folded group).
int hiddenCount(const Song& s);
// The tracks hidden by themselves (not by a folded group), in mixer order.
std::vector<ObjectId> hiddenTracks(const Song& s);
// The folded groups, in mixer order of their first member.
std::vector<std::string> foldedGroups(const Song& s);

// Hides or shows the tracks (the master is skipped). One undo step; false when nothing changed.
bool setHidden(Document& d, const std::vector<ObjectId>& tracks, bool hidden);
// Narrow (slim) strips or full ones. One undo step.
bool setNarrow(Document& d, const std::vector<ObjectId>& tracks, bool narrow);
// Folds a group to one strip, or opens it again. One undo step.
bool setGroupFolded(Document& d, const std::string& group, bool folded);
// Every hidden track shown and every folded group opened, in one undo step.
bool showAll(Document& d);
// A folded group's strip: mute and solo act on every member (one undo step each).
bool groupMuted(const Song& s, const std::string& group);  // every member muted
bool groupSoloed(const Song& s, const std::string& group);  // any member soloed
bool setGroupMute(Document& d, const std::string& group, bool muted);
bool setGroupSolo(Document& d, const std::string& group, bool solo);

// The insert tracks a control surface's banks walk (mixer indexes, mixer order): every insert the
// mixer shows a strip of its own for, so a hidden track or a folded group's members are skipped.
std::vector<int> surfaceTracks(const Song& s);

}  // namespace gl::mixhide
