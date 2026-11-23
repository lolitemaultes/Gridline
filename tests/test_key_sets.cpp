// Gridline - K-112: the key sets as data (core/Keymap.h).
//   - Studio, Live and Logic (with and without the separate Command key) bind every command
//     exactly as before (tests/k112/key_sets.tsv, recorded before the change); the new
//     typing.octave1..8 commands have no key in them.
//   - Tracker: F1 to F8 pick the typing octave and nothing else; the window commands that had
//     them have other keys; no chord is given twice where it would clash (same context, or a
//     context and the global one) beyond what Studio already has; and every chord the Tracker
//     set adds opens, shows or picks something (no key that deletes, clears or rewrites).
//   - K-106 cp25: the Tracker set's every binding (rows "tracker") and the Tracker's own keys
//     (core/TrackerKeys.h, rows "tracker.<area>") match the golden too; no own key repeats a chord in
//     its area; none takes a chord the Tracker set gives a global command (sameJob() apart); the rows
//     naming a command are bound so; every command the Tracker set gives a key is in the help
//     (TrackerKeys' screen keys or rows).
//   cwd: the build dir; the golden is read from the source tree (GL_SOURCE_DIR).
//   "gridline_tests_key_sets --print": the Tracker rows as the golden has them (to update it on purpose).
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include "Check.h"
#include "core/Keymap.h"
#include "core/TrackerKeys.h"

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

// The Tracker's golden rows: "<set>\t<id>\t<context>\t<chords;>", the set's then its own keys'.
std::vector<std::string> trackerGolden() {
  std::vector<std::string> out;
  Keymap tracker;
  tracker.setProfile(Profile::Tracker, false);
  for (const Command& c : commands()) out.push_back("tracker\t" + std::string(c.id) + "\t" + std::to_string(static_cast<int>(c.context)) + "\t" + row(tracker, c));
  for (const gl::trackerkeys::Row& r : gl::trackerkeys::rows()) {
    std::string ch;
    for (const std::string& x : r.chords) ch += x + ";";
    const std::string area = gl::trackerkeys::areaId(r.area);
    out.push_back("tracker." + area + "\t" + r.id + "\t" + area + "\t" + ch);
  }
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc > 1 && std::string(argv[1]) == "--print") {
    for (const std::string& l : trackerGolden()) std::printf("%s\n", l.c_str());
    return 0;
  }
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
  int studioRows = 0;
  for (const auto& [k, v] : golden) studioRows += startsWith(k, "tracker") ? 0 : 1;
  expect(compared == studioRows, "every golden row compared");
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
    expect(hide, "Live: Command+H is free (checkpoint 59, N58-12: Stop all sound is Command+Alt+.), Hide keeps it");
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
      const bool safe = startsWith(id, "view.") || startsWith(id, "help.") || startsWith(id, "typing.") || id == "edit.rename" || id == "pattern.new" ||
                        id == "pattern.clone" ||  // TRK-1118: Duplicate pattern adds a pattern, as New pattern
                        startsWith(id, "wave.");  // K-248: warp mode and the spectrum switch a waveform view
      expect(safe, ("Tracker adds " + format(ch) + " to " + id + ": not an open / show / pick command").c_str());
    }
  }

  // ---- K-106 cp25: the Tracker's keys, its set's and its own, match the golden ----
  {
    std::set<std::string> want;  // the golden's tracker rows, "<set>\t<id>\t<chords>"
    for (const auto& [k, v] : golden)
      if (startsWith(k, "tracker")) want.insert(k + "\t" + v);
    std::set<std::string> have;
    for (const std::string& l : trackerGolden()) {
      const size_t a = l.find('\t'), b = l.find('\t', a + 1), c = l.find('\t', b + 1);
      (void)a;
      have.insert(l.substr(0, b) + "\t" + l.substr(c + 1));  // (the golden map leaves the context out)
    }
    for (const std::string& h : have) expect(want.count(h) != 0, ("not in the golden (or changed): " + h).c_str());
    for (const std::string& w : want) expect(have.count(w) != 0, ("golden row gone: " + w).c_str());
  }
  // K-255: Ctrl+Alt+L is the pattern editor's track loop in the Tracker set: no global command of
  // the set has it (the Mix page's Select routed tracks keeps it in the mixer), and Studio, the
  // Clips and Smooth sets are unchanged (the golden above).
  {
    bool own = false;
    for (const gl::trackerkeys::Row& r : gl::trackerkeys::rows())
      if (std::string(r.id) == "pattern.trackLoop") own = r.chords.size() == 1 && r.chords[0] == "Ctrl+Alt+L";
    expect(own, "the Tracker's track loop is Ctrl+Alt+L");
    const Chord cl = parse("Ctrl+Alt+L");
    expect(tracker.lookup(Context::Global, cl).empty(), "no global command of the Tracker set takes Ctrl+Alt+L");
  }
  // No own key twice in one area (the keys typed into the field at the cursor apart: notes, digits).
  {
    std::map<std::string, std::string> seen;  // "<area> <chord>" -> row id
    for (const gl::trackerkeys::Row& r : gl::trackerkeys::rows()) {
      if (r.typed) continue;
      for (const std::string& c : r.chords) {
        const std::string k = std::string(gl::trackerkeys::areaId(r.area)) + " " + c;
        auto [it, fresh] = seen.emplace(k, r.id);
        expect(fresh, ("Tracker key " + k + " in both " + it->second + " and " + r.id).c_str());
      }
    }
  }
  // An own key never takes a chord the Tracker set gives a global command, unless it is that
  // command's own row (`command`) or both do the same job (sameJob()).
  {
    const auto& same = gl::trackerkeys::sameJob();
    for (const gl::trackerkeys::Row& r : gl::trackerkeys::rows()) {
      for (size_t i = 0; i < r.chords.size(); ++i) {
        const Chord c = parse(r.chords[i]);
        if (!c.valid()) continue;  // (a key the set cannot bind: Caps Lock, < >, the right-hand modifiers)
        for (const Command* g : tracker.lookup(Context::Global, c)) {
          std::string own = r.command ? r.command : "";
          if (const size_t h = own.find('#'); h != std::string::npos) own.replace(h, 1, std::to_string(i + 1));
          if (own == g->id) continue;
          const bool ok = std::find(same.begin(), same.end(), format(c) + "|" + g->id) != same.end();
          expect(ok, (std::string(r.id) + " takes " + format(c) + ", the Tracker set's key for " + g->id).c_str());
        }
      }
      if (r.command) {  // the set binds these keys so
        for (size_t i = 0; i < r.chords.size(); ++i) {
          std::string id = r.command;
          if (const size_t h = id.find('#'); h != std::string::npos) id.replace(h, 1, std::to_string(i + 1));
          const auto& b = tracker.bindings(id);
          expect(std::find(b.begin(), b.end(), parse(r.chords[i])) != b.end(), (std::string(r.id) + ": " + id + " has " + r.chords[i]).c_str());
        }
      }
    }
    for (const std::string& s : same) {  // each pair still holds (the chord is that command's)
      const size_t bar = s.find('|');
      const auto& b = tracker.bindings(s.substr(bar + 1));
      expect(std::find(b.begin(), b.end(), parse(s.substr(0, bar))) != b.end(), ("same job: " + s).c_str());
    }
  }
  // Every command the Tracker set itself gives a key (its own rows, not Studio's left over) is in
  // the help: the screen keys, or an own key's command.
  {
    std::set<std::string> listed;
    for (const gl::trackerkeys::ScreenKey& k : gl::trackerkeys::screenKeys()) {
      expect(findCommand(k.id) != nullptr, (std::string("screen key ") + k.id + " is a command").c_str());
      listed.insert(k.id);
    }
    for (const gl::trackerkeys::Row& r : gl::trackerkeys::rows()) {
      if (!r.command) continue;
      std::string id = r.command;
      if (const size_t h = id.find('#'); h != std::string::npos) {
        for (int i = 1; i <= 8; ++i) listed.insert(id.substr(0, h) + std::to_string(i));
      } else {
        listed.insert(id);
      }
    }
    for (const Command& c : commands()) {
      const auto& sb = studio.bindings(c.id);
      const auto& tb = tracker.bindings(c.id);
      if (tb.empty() || tb == sb) continue;  // (no key, or Studio's: "Every shortcut" lists those)
      expect(listed.count(c.id) != 0, (std::string("the Tracker help lists the Tracker set's key for ") + c.id).c_str());
    }
  }
  // K-256: the workspace keys of every set: each of the nine has a key in the Studio, Clips and Tracker
  // sets (Command told apart or not), and no other command of the set takes it in any context.
  // The Tracker's are Ctrl+Alt+Shift+1 .. 9 (Ctrl+digit is its edit step, Ctrl+Alt+digit its screens).
  for (Profile p : {Profile::Studio, Profile::Live, Profile::Logic, Profile::Tracker})
    for (bool sep : {false, true}) {
      Keymap km;
      km.setProfile(p, sep);
      const std::string set = std::string(profileId(p)) + (sep ? "+cmd" : "");
      for (int i = 1; i <= 9; ++i) {
        const std::string id = "view.workspace" + std::to_string(i);
        const auto& b = km.bindings(id);
        // (Smooth keeps Ctrl+1 .. 4 and 6 .. 8 for its windows: those workspaces go without a key there)
        if (p != Profile::Logic) expect(!b.empty(), (set + ": " + id + " has a key").c_str());
        for (const Chord& ch : b)
          for (const Command& c : commands()) {
            if (id == c.id) continue;
            const auto& o = km.bindings(c.id);
            expect(std::find(o.begin(), o.end(), ch) == o.end(), (set + ": " + id + "'s " + format(ch) + " is also " + c.id).c_str());
          }
      }
    }
  {
    Keymap tr;
    tr.setProfile(Profile::Tracker, false);
    const Key d[9] = {Key::D1, Key::D2, Key::D3, Key::D4, Key::D5, Key::D6, Key::D7, Key::D8, Key::D9};
    for (int i = 0; i < 9; ++i) {
      const auto& b = tr.bindings("view.workspace" + std::to_string(i + 1));
      expect(b.size() == 1 && b[0].key == d[i] && b[0].mods == (kCtrl | kAlt | kShift), "Tracker: workspace keys are Ctrl+Alt+Shift+digit");
    }
    // The Tracker's own keys (the pattern, the macros, the screens) never take one either.
    for (const gl::trackerkeys::Row& r : gl::trackerkeys::rows())
      for (const std::string& c : r.chords) {
        const Chord x = parse(c);
        for (int i = 0; i < 9; ++i) expect(!(x.key == d[i] && x.mods == (kCtrl | kAlt | kShift)), (std::string(r.id) + " takes a workspace key").c_str());
      }
  }
  // K-340: Studio's zone keys (hold Tab, press a digit) need plain Tab free in the Studio set: no command
  // there binds Tab alone or Tab with a digit (the layer is not a chord). The Clips and Tracker sets give
  // Tab its own work in their views, so the setting is greyed there (app/ZoneKeysUi.cpp keySetTakesTab()).
  for (bool sep : {false, true}) {
    Keymap st;
    st.setProfile(Profile::Studio, sep);
    for (const Command& c : commands())
      if (c.context == Context::Global)
        for (const Chord& ch : st.bindings(c.id)) expect(!(ch.key == Key::Tab && ch.mods == 0), (std::string("Studio: plain Tab is ") + c.id).c_str());
  }
  return finish();
}
