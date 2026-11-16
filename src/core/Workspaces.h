// Gridline - K-237: named workspaces of a layout (plan option A, built on the panes of option C).
//
// A workspace is a named arrangement inside one layout: which windows are open and the layout's
// region state (pane sizes, the pane tab strips, the window each pane shows). Each layout keeps its
// own list, saved per person in settings.json under "layout.<id>.workspaces", never in a project.
// The arrangement is kept in Gridline's own terms (no dock file), and the app rebuilds the layout
// from it, so a switch never loads a raw dock file mid-session.
//
// A layout starts with one workspace (the arrangement it has today), so nothing changes until the
// person adds a second one; starter sets per layout can be added from the View menu.
//
// Pure data and rules, no ImGui; tested alone (tests/test_k237_workspaces.cpp).
#pragma once
#include <set>
#include <string>
#include <vector>
#include "LayoutPreset.h"

namespace gl::workspaces {

struct Arrangement {
  std::set<std::string> open;  // the pane windows open (stable names)
  std::string regionState;     // the layout's region state (layout::toJson text, "" = as designed)
  bool operator==(const Arrangement& o) const { return open == o.open && regionState == o.regionState; }
};

struct Workspace {
  std::string name;
  Arrangement saved;      // what "Revert to saved" goes back to
  Arrangement current;    // as the person left it (equal to saved while auto-save is on)
  bool operator==(const Workspace& o) const { return name == o.name && saved == o.saved && current == o.current; }
  bool changed() const { return !(saved == current); }
};

struct Set {
  std::vector<Workspace> items;
  int active = 0;
  bool autoSave = true;  // changes are kept as they happen (no "changed" dot)
  bool operator==(const Set& o) const { return items == o.items && active == o.active && autoSave == o.autoSave; }
};

constexpr int kMaxWorkspaces = 20;
constexpr int kMaxName = 40;
constexpr int kKeyed = 9;  // the first nine have switch keys (view.workspace1 .. view.workspace9)

// A layout's first set: one workspace named for its main view, holding `now` (today's arrangement).
Set initial(layout::Preset p, const Arrangement& now);
// The starter workspaces of a layout (names, and the pane windows each opens). The region state
// is left as designed.
std::vector<Workspace> starters(layout::Preset p);

// A clean name: trimmed, control characters dropped, at most kMaxName characters. "" when nothing is left.
std::string cleanName(const std::string& name);
// "Workspace N" with the first N not taken.
std::string freeName(const Set& s, const std::string& base = "Workspace");

// Adds a workspace after the active one and makes it active. Refused (-1) past kMaxWorkspaces.
// An empty or taken name becomes a free one. Returns its index.
int add(Set& s, const std::string& name, const Arrangement& a);
// Renames: refused for an empty name or one another workspace has (case-insensitive).
bool rename(Set& s, int i, const std::string& name);
// Removes a workspace (never the last one). The active one moves to its neighbour.
// `removed` (optional) gets it with its index, for the undo toast.
bool remove(Set& s, int i, Workspace* removed = nullptr);
// Puts a removed workspace back at its index (clamped) and makes it active.
void restore(Set& s, const Workspace& w, int index);
// A copy right after it, named "<name> copy" (or a free variant), made active.
int duplicate(Set& s, int i);
// Moves workspace `from` to position `to`; the active one stays the same workspace.
bool move(Set& s, int from, int to);

// Keeps the person's latest arrangement in the active workspace (and as saved, with auto-save).
void record(Set& s, const Arrangement& now);
// Switches: records `now` into the active workspace first, then makes `i` active and returns the
// arrangement to apply (nullptr when `i` is out of range or already active).
const Arrangement* select(Set& s, int i, const Arrangement& now);
// "Save current arrangement" and "Revert to saved" for the active workspace.
void save(Set& s);
const Arrangement* revert(Set& s);

// JSON text for settings.json and back. Broken text gives an empty set (the app then makes the
// initial one); names are cleaned, duplicates renamed, the list capped, the active index clamped.
std::string toJson(const Set& s);
Set fromJson(const std::string& text);
std::string settingsKey(layout::Preset p);  // "layout.<id>.workspaces"

}  // namespace gl::workspaces
