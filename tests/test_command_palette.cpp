// Gridline - K-93: the command palette's ranking, typo tolerance, recent list and its
// shortcut in every shortcut set (core/CommandPalette.h, core/Keymap.h).
#include <cstdio>
#include <string>
#include <vector>
#include "Check.h"
#include "core/CommandPalette.h"
#include "core/Keymap.h"

using namespace gl;
using cmdpal::Entry;

namespace {

Entry row(const char* id, const char* label, const char* path = "", cmdpal::Kind kind = cmdpal::Kind::Command) {
  Entry e;
  e.id = id;
  e.label = label;
  e.path = path;
  e.kind = kind;
  return e;
}

std::vector<Entry> sample() {
  std::vector<Entry> v = {
      row("save", "Save", "File"),
      row("saveas", "Save as...", "File"),
      row("savever", "Save new version", "File"),
      row("tmpl", "Save as template...", "File"),
      row("mixer", "Mixer", "View", cmdpal::Kind::Window),
      row("mixlay", "Mixer layout", "View"),
      row("remix", "Remix the selection", "Tools"),
      row("pdc", "Plugin delay compensation", "Options"),
      row("metro", "Metronome", "Options"),
      row("midi", "Song as MIDI file...", "File > Export"),
      row("dev", "Output device", "Settings > Audio", cmdpal::Kind::Setting),
      row("trio", "Trio Synth", "Instruments > Synth", cmdpal::Kind::Plugin),
      row("zoomin", "Zoom in", "View"),
      row("zoomfit", "Zoom to fit the song", "View"),
  };
  for (size_t i = 0; i < v.size(); ++i) v[i].order = static_cast<int>(i);
  return v;
}

std::string top(const std::vector<Entry>& v, const std::string& q, int kind = -1, const std::vector<std::string>& recent = {}) {
  const auto r = cmdpal::rank(v, q, recent, kind);
  return r.empty() ? std::string("-") : v[static_cast<size_t>(r.front().index)].id;
}

int place(const std::vector<Entry>& v, const std::string& q, const std::string& id) {
  const auto r = cmdpal::rank(v, q, {}, -1);
  for (size_t i = 0; i < r.size(); ++i)
    if (v[static_cast<size_t>(r[i].index)].id == id) return static_cast<int>(i);
  return -1;
}

}  // namespace

int main() {
  // ---- word scores: whole > start > word start > initials > anywhere > in order > typo ----
  expect(cmdpal::wordScore("save", "Save") > cmdpal::wordScore("save", "Save as..."), "the whole label beats its start");
  expect(cmdpal::wordScore("save", "Save as...") > cmdpal::wordScore("save", "Autosave now"), "the label's start beats a match inside a word");
  expect(cmdpal::wordScore("lay", "Mixer layout") > cmdpal::wordScore("lay", "Display"), "a word start beats the middle of a word");
  expect(cmdpal::wordScore("pdc", "Plugin delay compensation") > cmdpal::wordScore("pdc", "updated clips"), "initials beat letters in order");
  expect(cmdpal::wordScore("xyz", "Mixer") < 0, "unrelated letters do not match");
  expect(cmdpal::wordScore("", "Mixer") == 0, "an empty word matches everything");
  std::vector<int> hits;
  cmdpal::wordScore("lay", "Mixer layout", &hits);
  expect(hits.size() == 3 && hits[0] == 6 && hits[2] == 8, "the matched letters are reported (for lighting them)");

  // ---- typos ----
  expect(cmdpal::editDistance("metronome", "metronome", 2) == 0, "edit distance 0 for the same word");
  expect(cmdpal::editDistance("metornome", "metronome", 2) == 1, "an adjacent swap is one edit");
  expect(cmdpal::editDistance("mtronome", "metronome", 2) == 1, "a missing letter is one edit");
  expect(cmdpal::editDistance("abc", "xyzuvw", 1) == 2, "the distance stops at limit + 1");
  expect(cmdpal::wordScore("metornome", "Metronome") > 0, "a swapped pair still finds Metronome");
  expect(cmdpal::wordScore("mxer", "Mixer") > 0, "a missing letter still finds Mixer");
  expect(cmdpal::wordScore("mixre", "Mixer layout") > 0, "a typo inside a longer label still matches");
  expect(cmdpal::wordScore("qqqq", "Mixer") < 0, "far off words do not match");

  // ---- ranking ----
  const auto v = sample();
  expect(top(v, "save") == "save", "\"save\": Save first");
  expect(place(v, "save", "saveas") == 1, "\"save\": Save as... second (shorter label first)");
  expect(top(v, "save as") == "saveas", "\"save as\": the words together pick Save as...");
  expect(top(v, "mix") == "mixer", "\"mix\": Mixer before Mixer layout and Remix");
  expect(place(v, "mix", "remix") > place(v, "mix", "mixlay"), "a later word start ranks after the label start");
  expect(top(v, "pdc") == "pdc", "initials find Plugin delay compensation");
  expect(top(v, "metornome") == "metro", "a typo still ranks Metronome first");
  expect(top(v, "export midi") == "midi", "words can match the path (File > Export) and the label");
  expect(top(v, "zoom fit") == "zoomfit", "two words: both must match");
  expect(place(v, "zoom fit", "zoomin") < 0, "a row missing one of the words is left out");
  expect(top(v, "output", static_cast<int>(cmdpal::Kind::Setting)) == "dev", "a category limits the rows");
  expect(top(v, "save", static_cast<int>(cmdpal::Kind::Plugin)) == "-", "nothing of another kind");
  expect(top(v, "trio") == "trio", "plugins are found by name");
  {
    auto w = v;
    w[1].enabled = false;  // Save as... greyed
    expect(place(w, "save", "saveas") > place(v, "save", "saveas"), "a disabled row ranks lower");
    expect(place(w, "save", "saveas") >= 0, "disabled rows are not hidden");
  }
  // Translated rows are found by their English name too.
  {
    std::vector<Entry> t = {row("m", "[M\xC3\xAFx\xC3\xA9r]", "View")};
    t[0].labelEn = "Mixer";
    expect(top(t, "mixer") == "m", "the English name finds a translated row");
    t[0].alias = "Show the mixing desk";
    expect(top(t, "desk") == "m", "the alias (keymap description) finds the row");
  }
  // Recent rows: listed first for an empty query, and win ties.
  {
    const std::vector<std::string> recent = {"metro", "trio"};
    const auto r = cmdpal::rank(v, "", recent, -1);
    expect(r.size() == v.size(), "the empty query lists every row once");
    expect(v[static_cast<size_t>(r[0].index)].id == "metro" && v[static_cast<size_t>(r[1].index)].id == "trio" && r[0].recent && r[1].recent,
           "the empty query lists the recent rows first, newest first");
    expect(!r[2].recent && v[static_cast<size_t>(r[2].index)].id == "save", "then the other rows in their order");
    const auto rp = cmdpal::rank(v, "", recent, static_cast<int>(cmdpal::Kind::Plugin));
    expect(rp.size() == 1 && rp[0].recent, "a category keeps only its own recent rows");
    expect(top(v, "save", -1, {"savever"}) == "save", "an exact name still beats a recent near match");
    std::vector<Entry> tie = {row("a", "Zoom in", "Playlist"), row("b", "Zoom in", "Piano roll")};
    tie[1].order = 1;
    expect(top(tie, "zoom in") == "a", "ties keep the menu order");
    expect(top(tie, "zoom in", -1, {"b"}) == "b", "a recent row wins a tie");
  }

  // ---- the recent list ----
  {
    std::vector<std::string> r;
    cmdpal::pushRecent(r, "a");
    cmdpal::pushRecent(r, "b");
    cmdpal::pushRecent(r, "a");
    expect(r.size() == 2 && r[0] == "a" && r[1] == "b", "pushing an id again moves it to the front");
    for (int i = 0; i < 20; ++i) cmdpal::pushRecent(r, "x" + std::to_string(i));
    expect(r.size() == cmdpal::kRecentMax && r[0] == "x19", "the list keeps the newest ids only");
    const std::string text = cmdpal::serializeRecent(r);
    expect(cmdpal::parseRecent(text) == r, "serialize and parse round-trip");
    expect(cmdpal::parseRecent("a\n\nb\na\r\nc") == std::vector<std::string>({"a", "b", "c"}), "empty lines and duplicates are dropped");
    expect(cmdpal::parseRecent("").empty(), "an empty setting is an empty list");
  }

  // ---- the shortcut: Ctrl+K (Cmd+K on macOS) where it is free, Ctrl+Shift+P everywhere ----
  {
    const keys::Chord ctrlK = keys::parse("Ctrl+K"), ctrlShiftP = keys::parse("Ctrl+Shift+P");
    const keys::Command* pc = keys::findCommand("view.commandPalette");
    expect(pc != nullptr && pc->context == keys::Context::Global, "the palette is a global command");
    for (keys::Profile p : {keys::Profile::Studio, keys::Profile::Live, keys::Profile::Logic})
      for (bool sep : {false, true}) {
        keys::Keymap km;
        km.setProfile(p, sep);
        const auto& b = km.bindings("view.commandPalette");
        char name[128];
        std::snprintf(name, sizeof name, "%s set (Cmd %s): the palette has a chord and no clash", keys::profileId(p), sep ? "separate" : "folded");
        bool clash = false;
        for (const keys::Chord& c : b) clash |= !km.conflicts("view.commandPalette", c).empty();
        expect(!b.empty() && !clash, name);
      }
    keys::Keymap studio;
    expect(studio.bindings("view.commandPalette").size() == 2 && studio.bindings("view.commandPalette")[0] == ctrlK &&
               studio.bindings("view.commandPalette")[1] == ctrlShiftP,
           "studio: Ctrl+K and Ctrl+Shift+P");
    expect(keys::format(studio.bindings("rack.graphEditor").front()) == "Ctrl+K", "studio: the channel rack keeps Ctrl+K for its graph editor");
    bool shadowOnlyRack = true;
    for (const keys::Command* c : studio.shadows("view.commandPalette", ctrlK)) shadowOnlyRack &= std::string(c->id) == "rack.graphEditor";
    expect(shadowOnlyRack, "studio: only the channel rack's own Ctrl+K takes the key from the palette");
    expect(studio.shadows("view.commandPalette", ctrlShiftP).empty(), "Ctrl+Shift+P opens the palette in every window");
    keys::Keymap live;
    live.setProfile(keys::Profile::Live, false);
    expect(!live.bindings("view.commandPalette").empty() && live.bindings("view.commandPalette")[0] == ctrlK, "live: Ctrl+K");
    keys::Keymap logic;
    logic.setProfile(keys::Profile::Logic, false);
    expect(keys::format(logic.bindings("options.typingKeyboard").front()) == "Ctrl+K", "logic: Ctrl+K stays the typing keyboard (that set's own key)");
    expect(logic.bindings("view.commandPalette").size() == 1 && logic.bindings("view.commandPalette")[0] == ctrlShiftP, "logic: the palette keeps Ctrl+Shift+P");
    keys::Keymap logicMac;
    logicMac.setProfile(keys::Profile::Logic, true);
    expect(keys::format(logicMac.bindings("options.typingKeyboard").front()) == "Cmd+K", "logic on macOS: Cmd+K stays the typing keyboard");
    expect(logicMac.bindings("view.commandPalette").size() == 1 && keys::format(logicMac.bindings("view.commandPalette")[0]) == "Cmd+Shift+P",
           "logic on macOS: the palette is Cmd+Shift+P");
    // A user binding Ctrl+K to something else through the shortcut editor takes it from the palette.
    keys::Keymap user;
    const auto lost = user.assign("pattern.new", ctrlK, false);
    expect(lost.size() == 1 && lost[0] == "view.commandPalette" && user.bindings("view.commandPalette").size() == 1, "a user's own Ctrl+K wins; the palette keeps Ctrl+Shift+P");
  }

  return finish();
}
