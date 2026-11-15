// Gridline - K-112: the key sets as data (core/Keymap.h).
//   - Studio, Live and Logic (with and without the separate Command key) bind every command
//     exactly as before (tests/k112/key_sets.tsv, recorded before the change); the new
//     typing.octave1..8 commands have no key in them.
//   - Tracker: F1 to F8 pick the typing octave and nothing else; the window commands that had
//     them have other keys; no chord is given twice where it would clash (same context, or a
//     context and the global one) beyond what Studio already has; and every chord the Tracker
//     set adds opens, shows or picks something (no key that deletes, clears or rewrites).
//   cwd: the build dir; the golden is read from the source tree (GL_SOURCE_DIR).
#include <algorithm>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include "Check.h"
#include "core/Keymap.h"

using namespace gl::keys;

namespace {

std::string row(const Keymap& k, const Command& c) {
  std::string s;
  for (const Chord& ch : k.bindings(c.id)) s += format(ch) + ";";
  return s;
}

// Chords bound to two commands that can fire together (same context, or a context and Global).
std::set<std::string> clashes(const Keymap& k) {
  std::map<std::string, std::vector<const Command*>> by;  // chord -> commands
  for (const Command& c : commands())
    for (const Chord& ch : k.bindings(c.id)) by[format(ch)].push_back(&c);
  std::set<std::string> out;
  for (const auto& [chord, cs] : by)
    for (size_t i = 0; i < cs.size(); ++i)
      for (size_t j = i + 1; j < cs.size(); ++j)
        if (cs[i]->context == cs[j]->context || cs[i]->context == Context::Global || cs[j]->context == Context::Global)
          out.insert(chord + " " + cs[i]->id + " " + cs[j]->id);
  return out;
}

bool startsWith(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }

}  // namespace

int main() {
  // ---- Studio, Live, Logic: unchanged ----
  std::ifstream in(std::string(GL_SOURCE_DIR) + "/tests/k112/key_sets.tsv");
  expect(in.good(), "golden key sets readable");
  std::map<std::string, std::string> golden;  // "<set>\t<id>" -> chords
  for (std::string line; std::getline(in, line);) {
    std::istringstream ls(line);
    std::string set, id, ctx, chords;
    std::getline(ls, set, '\t');
    std::getline(ls, id, '\t');
    std::getline(ls, ctx, '\t');
    std::getline(ls, chords);
    golden[set + "\t" + id] = chords;
  }
  expect(golden.size() > 1000, "golden has every set");
  int compared = 0, differ = 0;
  for (int sep = 0; sep < 2; ++sep)
    for (Profile p : {Profile::Studio, Profile::Live, Profile::Logic}) {
      Keymap k;
      k.setProfile(p, sep != 0);
      const std::string set = std::string(profileId(p)) + (sep ? "+cmd" : "");
      for (const Command& c : commands()) {
        const std::string id = c.id;
        if (startsWith(id, "typing.octave")) {
          expect(k.bindings(id).empty(), (set + ": " + id + " has no key").c_str());
          continue;
        }
        auto it = golden.find(set + "\t" + id);
        expect(it != golden.end(), (set + ": " + id + " in the golden").c_str());
        if (it == golden.end()) continue;
        ++compared;
        if (it->second != row(k, c)) {
          ++differ;
          expect(false, (set + ": " + id + " was " + it->second + " now " + row(k, c)).c_str());
        }
      }
    }
  expect(compared == static_cast<int>(golden.size()), "every golden row compared");
  expect(differ == 0, "no binding changed in Studio, Live or Logic");

  // ---- Tracker ----
  Keymap studio, tracker;
  studio.setProfile(Profile::Studio, false);
  tracker.setProfile(Profile::Tracker, false);
  expect(std::string(profileId(Profile::Tracker)) == "tracker" && profileFromId("tracker") == Profile::Tracker, "Tracker id");
  const Key fk[8] = {Key::F1, Key::F2, Key::F3, Key::F4, Key::F5, Key::F6, Key::F7, Key::F8};
  for (int i = 0; i < 8; ++i) {
    const std::string id = "typing.octave" + std::to_string(i + 1);
    const auto& b = tracker.bindings(id);
    expect(b.size() == 1 && b[0].key == fk[i] && b[0].mods == 0, (id + " on F" + std::to_string(i + 1)).c_str());
  }
  // F1..F8 alone do nothing else in Tracker.
  for (const Command& c : commands())
    for (const Chord& ch : tracker.bindings(c.id))
      for (int i = 0; i < 8; ++i)
        if (ch.key == fk[i] && ch.mods == 0)
          expect(startsWith(c.id, "typing.octave"), (std::string(c.id) + " must not have F" + std::to_string(i + 1) + " in Tracker").c_str());
  // The commands that lost their F-key are still reachable by a key.
  for (const char* id : {"help.shortcuts", "edit.rename", "pattern.new", "view.playlist", "view.pianoRoll", "view.channelRack", "view.pluginPicker"})
    expect(!tracker.bindings(id).empty(), (std::string(id) + " keeps a key in Tracker").c_str());
  // No clash Studio does not already have.
  const std::set<std::string> sc = clashes(studio);
  for (const std::string& c : clashes(tracker)) expect(sc.count(c) != 0, ("Tracker clash: " + c).c_str());
  // The macOS menu bar's own keys (Hide, Hide others) never take a chord a set binds, in every
  // set, Command told apart or not, with the person's own overrides too.
  for (Profile p : {Profile::Studio, Profile::Live, Profile::Logic, Profile::Tracker})
    for (bool sep : {false, true}) {
      Keymap km;
      km.setProfile(p, sep);
      for (const SystemMenuKey& k : systemMenuKeys(km))
        for (const Command& c : commands())
          for (const Chord& x : km.bindings(c.id))
            expect(x != k.chord, (std::string(profileId(p)) + (sep ? " (Cmd apart)" : "") + ": the menu's " + k.item + " key " + format(k.chord) +
                                  " clashes with " + c.id)
                                     .c_str());
    }
  {
    Keymap live;
    live.setProfile(Profile::Live);
    bool hide = false;
    for (const SystemMenuKey& k : systemMenuKeys(live)) hide = hide || std::string(k.item) == "hide";
    expect(!hide, "Live: Command+H stops all sound, so Hide has no key");
    Keymap logic;
    logic.setProfile(Profile::Logic, true);
    hide = false;
    for (const SystemMenuKey& k : systemMenuKeys(logic)) hide = hide || std::string(k.item) == "hide";
    expect(hide, "Logic on macOS: Command+H is free, Hide keeps it");
    logic.set("tools.stopSound", {Chord{Key::H, kCmd}});  // the person binds it
    hide = false;
    for (const SystemMenuKey& k : systemMenuKeys(logic)) hide = hide || std::string(k.item) == "hide";
    expect(!hide, "Logic: a person's own Command+H binding wins over Hide");
  }
  // Every chord Tracker adds opens, shows or picks (never deletes, clears or rewrites).
  for (const Command& c : commands()) {
    const auto& sb = studio.bindings(c.id);
    for (const Chord& ch : tracker.bindings(c.id)) {
      if (std::find(sb.begin(), sb.end(), ch) != sb.end()) continue;
      const std::string id = c.id;
      const bool safe = startsWith(id, "view.") || startsWith(id, "help.") || startsWith(id, "typing.") || id == "edit.rename" || id == "pattern.new";
      expect(safe, ("Tracker adds " + format(ch) + " to " + id + ": not an open / show / pick command").c_str());
    }
  }
  return finish();
}
