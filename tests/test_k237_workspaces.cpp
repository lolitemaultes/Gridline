// Gridline - K-237: named workspaces of a layout (core/Workspaces.h): add, rename, remove and
// restore, duplicate, reorder, switching with record, save and revert, starters and the JSON form.
#include <string>
#include "Check.h"
#include "core/Project.h"
#include "core/Song.h"
#include "core/Workspaces.h"

using namespace gl::workspaces;
using gl::layout::Preset;

namespace {
void expect(bool ok, const std::string& name) { expect(ok, name.c_str()); }
Arrangement arr(std::set<std::string> open, const std::string& st = "") {
  Arrangement a;
  a.open = std::move(open);
  a.regionState = st;
  return a;
}
std::string names(const Set& s) {
  std::string out;
  for (const Workspace& w : s.items) out += (out.empty() ? "" : ",") + w.name;
  return out;
}
}  // namespace

int main() {
  const Arrangement today = arr({"Playlist", "Browser"});
  Set s = initial(Preset::Studio, today);
  expect(s.items.size() == 1 && s.items[0].name == "Arrange" && s.items[0].current == today && s.active == 0, "a layout starts with one workspace: today's");
  expect(initial(Preset::Logic, today).items[0].name == "Tracks" && initial(Preset::Tracker, today).items[0].name == "Patterns" &&
             initial(Preset::Live, today).items[0].name == "Session",
         "each layout names its first workspace for its main view");

  // add, names
  expect(add(s, "", arr({"Mixer"})) == 1 && s.items[1].name == "Workspace 2" && s.active == 1, "add: a free name, after the active one, made active");
  expect(add(s, "  Mix\t ", arr({"Mixer"})) == 2 && s.items[2].name == "Mix", "add cleans the name");
  // Add first, then build the message: argument order is unspecified, and MSVC/clang-cl evaluate
  // right to left, so reading items[3] inside the same call indexed past the end (STL fast-fail).
  const int taken = add(s, "mix", today);
  const std::string takenName = taken == 3 && s.items.size() > 3 ? s.items[3].name : std::string();
  expect(taken == 3 && takenName != "mix", "a taken name (any case) becomes a free one: " + takenName);
  expect(cleanName(std::string(60, 'x')).size() == static_cast<size_t>(kMaxName), "names are capped");
  expect(cleanName("\xC3\xA9" + std::string(45, 'e')).size() <= static_cast<size_t>(kMaxName), "a capped name never splits a character");

  // rename
  expect(rename(s, 1, "Edit") && s.items[1].name == "Edit", "rename");
  expect(!rename(s, 1, "   ") && !rename(s, 1, "MIX") && !rename(s, 9, "x"), "rename refuses empty, taken and unknown");
  expect(rename(s, 1, "edit"), "renaming to its own name in another case is fine");

  // remove and restore
  s.active = 2;
  Workspace gone;
  expect(remove(s, 1, &gone) && gone.name == "edit" && s.active == 1 && s.items[1].name == "Mix", "remove keeps the same workspace active");
  restore(s, gone, 1);
  expect(names(s).find("edit") != std::string::npos && s.items[1].name == "edit" && s.active == 1, "restore puts it back where it was");
  Set one = initial(Preset::Studio, today);
  expect(!remove(one, 0), "the last workspace cannot be removed");

  // duplicate and move
  const int d = duplicate(s, 2);
  expect(d == 3 && s.items[3].name == "Mix copy" && s.items[3].current == s.items[2].current && s.active == 3, "duplicate: next to it, made active");
  expect(duplicate(s, 2) == 3 && s.items[3].name != "Mix copy", "a second copy gets a free name");
  const std::string activeName = s.items[static_cast<size_t>(s.active)].name;
  expect(move(s, 0, 4) && s.items[4].name == "Arrange" && s.items[static_cast<size_t>(s.active)].name == activeName, "move keeps the active workspace");
  expect(!move(s, 4, 4) && !move(s, 40, 0), "a move nowhere does nothing");

  // switching
  Set w = initial(Preset::Logic, today);
  add(w, "Mix", arr({"Playlist", "Mixer"}, "{\"member\":{}}"));
  const Arrangement moved = arr({"Playlist", "Mixer", "Browser"});
  const Arrangement* to = select(w, 0, moved);
  expect(to && *to == today && w.active == 0, "select returns the arrangement to apply");
  expect(w.items[1].current == moved && w.items[1].saved == moved, "select keeps what the person left (auto-save on)");
  expect(!select(w, 0, today) && !select(w, 7, today), "selecting the active one or an unknown one does nothing");

  // save and revert without auto-save
  w.autoSave = false;
  record(w, arr({"Playlist"}));
  expect(w.items[0].changed() && w.items[0].saved == today, "without auto-save a change shows and the saved one stays");
  const Arrangement* back = revert(w);
  expect(back && *back == today && !w.items[0].changed(), "revert goes back to the saved arrangement");
  record(w, arr({"Playlist"}));
  save(w);
  expect(!w.items[0].changed() && w.items[0].saved == arr({"Playlist"}), "save stores the current arrangement");

  // cap
  Set big = initial(Preset::Studio, today);
  for (int i = 0; i < 30; ++i) add(big, "", today);
  expect(static_cast<int>(big.items.size()) == kMaxWorkspaces && add(big, "", today) == -1 && duplicate(big, 0) == -1, "at most kMaxWorkspaces");

  // starters
  for (Preset p : {Preset::Studio, Preset::Live, Preset::Logic, Preset::Tracker}) {
    const auto st = starters(p);
    bool ok = st.size() >= 3 && st.size() <= 4;
    for (const Workspace& x : st) ok = ok && !x.name.empty() && !x.saved.open.empty() && x.saved == x.current;
    expect(ok, std::string("starters for ") + gl::layout::id(p));
  }

  // JSON
  w.autoSave = false;
  record(w, arr({"Browser"}));
  const std::string js = toJson(w);
  const Set r = fromJson(js);
  expect(r == w, "JSON round trip: " + js);
  const Set broken = fromJson(R"({"items":[{"name":""},{"name":"A"},{"name":"a"},7],"active":99})");
  expect(names(broken) == "Workspace 1,A,a 3" || (broken.items.size() == 3 && broken.items[1].name == "A" && broken.items[2].name != "a"),
         "broken entries: names made free, duplicates renamed: " + names(broken));
  expect(broken.active == 2, "the active index is clamped");
  expect(fromJson("nonsense").items.empty() && fromJson("").items.empty(), "broken text: an empty set");
  expect(settingsKey(Preset::Logic) == "layout.logic.workspaces", "settings key");

  // K-256: what a workspace keeps: its own control bar and pinned commands, the pane sizes, a dock plan.
  {
    Set k = initial(Preset::Studio, today);
    add(k, "Mix", arr({"Mixer", "Playlist"}));
    const std::string plainText = toJson(k);
    expect(plainText.find("ownBar") == std::string::npos && plainText.find("keepPanes") == std::string::npos && plainText.find("dock") == std::string::npos,
           "a workspace with nothing of its own writes no new keys");
    const Set plainBack = fromJson(plainText);
    expect(plainBack == k && !plainBack.items[1].hasOwnBar() && !plainBack.items[1].hasOwnPins() && plainBack.items[1].keepWindows && plainBack.items[1].keepPanes,
           "an older set reads with no own bar or pins and keeping everything");
    Workspace* mix = activeWorkspace(k);
    expect(mix && mix->name == "Mix", "activeWorkspace is the active one");
    mix->ownBar = R"([{"id":"menus","on":false}])";
    mix->ownPins = "[]";
    mix->keepPanes = false;
    Arrangement withDock = arr({"Mixer"});
    withDock.dock = R"({"tree":{"windows":["Mixer"],"central":true}})";
    record(k, withDock);
    const Set back = fromJson(toJson(k));
    expect(back == k, "own bar, own (empty) pins, keep flags and the dock plan round-trip");
    expect(back.items[1].hasOwnPins() && back.items[1].ownPins == "[]", "an own empty pin list is kept (not the layout's)");
    expect(back.items[1].current.dock == withDock.dock, "the arrangement keeps its dock plan");
    // Switching: the arrangement comes back, and the workspace switched to carries its own bar.
    const Arrangement* to = select(k, 0, withDock);
    expect(to && *to == today && !activeWorkspace(k)->hasOwnBar(), "switching to a workspace without its own bar: the layout's bar");
    to = select(k, 1, today);
    expect(to && to->dock == withDock.dock && activeWorkspace(k)->hasOwnBar() && activeWorkspace(k)->ownBar == mix->ownBar,
           "switching back: its dock plan and its own bar");
    // Revert: the arrangement goes back, the own bar stays (it is not part of the arrangement).
    k.autoSave = false;
    record(k, arr({"Browser"}));
    const Arrangement* r = revert(k);
    expect(r && r->dock == withDock.dock && activeWorkspace(k)->hasOwnBar(), "revert brings the saved arrangement back and keeps the own bar");
    const int d = duplicate(k, 1);
    expect(d == 2 && k.items[2].ownBar == k.items[1].ownBar && !k.items[2].keepPanes, "a duplicate keeps what the workspace keeps");
  }

  // The opt-in copy in the project (Song::workspaceCopy): written only when present, read back as it was.
  {
    gl::Song song;
    const std::string plain = gl::projectToText(song);
    expect(plain.find("\"workspaces\"") == std::string::npos, "a project without the copy writes no workspaces key");
    gl::Song back;
    expect(gl::loadProjectText(plain, "", back) && back.workspaceCopy.empty(), "and reads back without one");
    song.workspaceCopy["logic"] = js;
    song.workspaceCopy["studio"] = toJson(initial(Preset::Studio, today));
    const std::string withCopy = gl::projectToText(song);
    gl::Song read;
    expect(gl::loadProjectText(withCopy, "", read) && read.workspaceCopy == song.workspaceCopy, "the copy survives a save and an open");
    expect(fromJson(read.workspaceCopy["logic"]) == w, "each layout's set reads back whole");
    expect(read == back && read.workspaceCopy != back.workspaceCopy, "the copy is view state: the song compares the same without it");
    // Unknown layouts and broken values are left out.
    std::string odd = withCopy;
    const size_t at = odd.find("\"workspaces\"");
    expect(at != std::string::npos, "the copy is written under \"workspaces\"");
    if (at != std::string::npos) {
      odd.insert(odd.find('{', at) + 1, "\"elsewhere\": \"{}\", \"tracker\": 7, ");
      gl::Song o;
      expect(gl::loadProjectText(odd, "", o) && o.workspaceCopy.size() == 2 && !o.workspaceCopy.count("elsewhere") && !o.workspaceCopy.count("tracker"),
             "unknown layouts and broken values are dropped");
    }
  }
  return finish();
}
