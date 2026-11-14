// Gridline - K-79 window layout presets: the per-user setting and its round
// trip, one layout file per preset with Studio unchanged, the menu bar styles
// reaching every studio menu, the shortcut sets without clashes, short or
// foreign settings normalised, and the preset geometry at every window size.
// K-122: a preset never changes the theme or any colour setting, and an older
// settings file that still links the palette loads with the user's own look.
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include "Check.h"
#include "core/Keymap.h"
#include "core/LayoutPreset.h"
#include "core/MenuStyle.h"
#include "core/Json.h"
#include "core/Settings.h"

using namespace gl;
namespace fs = std::filesystem;
using layout::Preset;

namespace {

const Preset kAll[] = {Preset::Studio, Preset::Live, Preset::Logic};

std::string tempDir(const char* name) {
  const fs::path d = fs::temp_directory_path() / name;
  std::error_code ec;
  fs::remove_all(d, ec);
  fs::create_directories(d, ec);
  return d.string();
}

void testSettingRoundTrip() {
  Settings st;
  expect(layout::current(st) == Preset::Studio, "default: the studio layout");
  expect(layout::keyProfile(st) == Preset::Studio && layout::menuStyle(st) == Preset::Studio, "default: studio shortcuts and menus");
  layout::Linked all;
  layout::choose(st, Preset::Logic, all);
  expect(layout::current(st) == Preset::Logic && layout::keyProfile(st) == Preset::Logic && layout::menuStyle(st) == Preset::Logic,
         "choosing a preset with every tick switches the shortcuts and menus too");
  expect(st.getString("theme.controls") == "slate", "the control style stays the user's in the logic layout");
  const std::string dir = tempDir("gridline_k79_settings");
  const std::string path = (fs::path(dir) / "settings.json").string();
  expect(st.save(path), "settings saved");
  Settings back;
  expect(back.load(path), "settings loaded");
  expect(layout::current(back) == Preset::Logic && back.getString("ui.layout") == "logic", "the preset is kept across a restart");
  expect(layout::keyProfile(back) == Preset::Logic && layout::menuStyle(back) == Preset::Logic, "the linked items are kept too");
  // Opting out: only the window layout changes.
  layout::Linked none;
  none.keys = none.menus = false;
  layout::choose(back, Preset::Live, none);
  expect(layout::current(back) == Preset::Live && layout::keyProfile(back) == Preset::Logic && layout::menuStyle(back) == Preset::Logic,
         "with the ticks cleared only the layout changes");
  expect(!layout::linkedFromSettings(back).keys && !layout::linkedFromSettings(back).menus, "the cleared ticks are remembered");
  layout::choose(back, Preset::Studio, all);
  expect(layout::current(back) == Preset::Studio && layout::keyProfile(back) == Preset::Studio, "back to studio");
}

void testLayoutFiles() {
  expect(layout::iniFileName(Preset::Studio) == "layout.ini", "studio keeps layout.ini (no migration for current users)");
  expect(layout::versionKey(Preset::Studio) == "ui.layoutVersion", "studio keeps its version key");
  expect(layout::bindingsKey(Preset::Studio) == "keys.bindings", "studio keeps its shortcut overrides key");
  std::set<std::string> files, versions, bindings;
  Settings st;
  for (Preset p : kAll) {
    files.insert(layout::iniFileName(p));
    versions.insert(layout::versionKey(p));
    bindings.insert(layout::bindingsKey(p));
    expect(st.set(layout::versionKey(p), 1) && st.set(layout::bindingsKey(p), std::string()), (std::string("settings keys registered: ") + layout::id(p)).c_str());
    expect(layout::fromId(layout::id(p)) == p, (std::string("id round trip: ") + layout::id(p)).c_str());
    expect(layout::displayName(p) && *layout::displayName(p) && layout::blurb(p) && *layout::blurb(p), "every preset has a name and a line");
  }
  expect(files.size() == 3 && versions.size() == 3 && bindings.size() == 3, "each preset has its own layout file, version and overrides");
  expect(std::string(layout::displayName(Preset::Studio)) == "Studio" && std::string(layout::displayName(Preset::Live)) == "Live" &&
             std::string(layout::displayName(Preset::Logic)) == "Logic",
         "display names");
  // Until each becomes a whole style (K-112), Live and Logic are shown as previews.
  expect(!layout::isPreview(Preset::Studio) && layout::isPreview(Preset::Live) && layout::isPreview(Preset::Logic) &&
             layout::previewTag() && *layout::previewTag(),
         "live and logic are marked as previews");
}

void testNormalise() {
  const std::string dir = tempDir("gridline_k79_short");
  const std::string path = (fs::path(dir) / "settings.json").string();
  {
    std::ofstream f(path);
    f << R"({"format":"gridline-settings","version":1,"values":{"ui.layout":"tracks","keys.profile":"nonsense","ui.menuStyle":"PERFORMANCE"}})";
  }
  Settings st;
  expect(st.load(path), "a short settings file loads");
  expect(layout::normalize(st), "normalising repairs it");
  expect(st.getString("ui.layout") == "logic", "the earlier id 'tracks' reads as logic");
  expect(st.getString("keys.profile") == "studio", "an unknown shortcut set falls back to studio");
  expect(st.getString("ui.menuStyle") == "live", "case and earlier ids are repaired ('PERFORMANCE' -> live)");
  expect(st.getBool("ui.layoutLinkKeys") && st.getBool("ui.layoutLinkMenus"), "missing ticks default to on");
  expect(!layout::normalize(st), "a normal file is left alone");
  Settings fresh;
  expect(!layout::normalize(fresh), "defaults need no repair");
}

// The settings file as text without the keys a preset is allowed to change.
std::string appearanceOf(const Settings& s, const std::string& path) {
  if (!s.save(path)) return "<unsaved>";
  std::ifstream f(path, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  Json j;
  if (!jsonParse(ss.str(), j)) return "<unparsed>";
  Json* v = j.find("values") ? &j.object["values"] : nullptr;
  if (!v) return "<no values>";
  for (const char* k : {"ui.layout", "keys.profile", "ui.menuStyle", "ui.layoutLinkKeys", "ui.layoutLinkMenus", "ui.layoutChosen"})
    v->object.erase(k);
  return jsonStringify(*v);
}

// K-122: switching through every layout and back leaves the theme and colour settings exactly
// as they were, with a custom theme too, whatever the ticks say.
void testLayoutsKeepTheme() {
  const std::string dir = tempDir("gridline_k122_keep");
  const std::string path = (fs::path(dir) / "settings.json").string();
  Settings st;
  st.set("theme.mode", "light");
  st.set("ui.darkTheme", false);
  st.set("theme.accent", "#3D63DD");
  st.set("theme.hot", "#12AB34");
  st.set("theme.led", "#4ECCFF");
  st.set("theme.playhead", "#FF00FF");
  st.set("theme.contrast", 0.4);
  st.set("theme.brightness", -0.3);
  st.set("theme.controls", "lumen");
  st.set("ui.scale", 1.25);
  const std::string before = appearanceOf(st, path);
  expect(before.find("#3D63DD") != std::string::npos && before.find("lumen") != std::string::npos, "the custom look is in the snapshot");
  expect(before.find("theme.palette") == std::string::npos && before.find("LinkPalette") == std::string::npos &&
             before.find("controlsFor") == std::string::npos,
         "no layout palette settings are written");
  layout::Linked all, none;
  none.keys = none.menus = false;
  const Preset order[] = {Preset::Live, Preset::Logic, Preset::Studio, Preset::Logic, Preset::Live, Preset::Studio};
  bool same = true;
  for (const layout::Linked& l : {all, none})
    for (Preset p : order) {
      layout::choose(st, p, l);
      same = same && appearanceOf(st, path) == before;
    }
  expect(same, "every layout, with or without the ticks, leaves every theme and colour setting byte-identical");
  layout::choose(st, Preset::Studio, all);
  expect(appearanceOf(st, path) == before, "back in studio: the user's own look, unchanged");
}

// K-122: a settings file from 0.7.4 builds that still has the palette link loads cleanly,
// drops the old keys and gives back the user's own control style.
void testOldPaletteLink() {
  const std::string dir = tempDir("gridline_k122_old");
  const std::string path = (fs::path(dir) / "settings.json").string();
  auto write = [&](const char* values) {
    std::ofstream f(path, std::ios::binary);
    f << R"({"format":"gridline-settings","version":1,"values":{)" << values << "}}";
  };
  write(R"("ui.layout":"logic","ui.layoutLinkPalette":true,"theme.palette":"logic","theme.controls":"graphite",)"
        R"("theme.controlsFor.studio":"lumen","theme.controlsFor.live":"hairline","theme.controlsFor.logic":"graphite",)"
        R"("theme.accent":"#3D63DD","ui.layoutLinkKeys":false)");
  Settings st;
  std::string err;
  expect(st.load(path, &err) && err.empty(), "an old file with the palette link loads");
  expect(layout::current(st) == Preset::Logic && !st.getBool("ui.layoutLinkKeys"), "its layout and other ticks are kept");
  expect(st.getString("theme.controls") == "lumen", "left on the logic palette: the user's own control style comes back");
  expect(st.getString("theme.accent") == "#3D63DD", "the user's accent is kept");
  expect(!st.set("ui.layoutLinkPalette", true) && !st.set("theme.palette", "logic"), "the palette keys are no longer settings");
  const std::string saved = appearanceOf(st, (fs::path(dir) / "resaved.json").string());
  expect(saved.find("theme.palette") == std::string::npos && saved.find("LinkPalette") == std::string::npos &&
             saved.find("controlsFor") == std::string::npos,
         "the old keys are dropped on the next save");
  expect(!layout::normalize(st), "the loaded file needs no further repair");

  // A style the user picked while in the layout stays.
  write(R"("theme.palette":"live","theme.controls":"classic","theme.controlsFor.studio":"slate","ui.layoutLinkPalette":false)");
  Settings picked;
  expect(picked.load(path) && picked.getString("theme.controls") == "classic", "a control style picked since stays the user's");
  // On the studio palette nothing about the look changes.
  write(R"("theme.palette":"studio","theme.controls":"hairline","theme.controlsFor.studio":"lumen")");
  Settings studio;
  expect(studio.load(path) && studio.getString("theme.controls") == "hairline", "on the studio palette the control style is untouched");
  // Earlier ids and other case, and a wrong type, load too.
  write(R"("theme.palette":"PERFORMANCE","theme.controls":"hairline","theme.controlsFor.studio":"slate","ui.layoutLinkPalette":"yes")");
  Settings odd;
  expect(odd.load(path) && odd.getString("theme.controls") == "slate", "an earlier palette id reads the same way");
}

void testMenus() {
  // Studio: the bar as it always was.
  const char* studio[] = {"File", "Edit", "Add", "Patterns", "View", "Options", "Tools", "Help"};
  const auto& sm = menustyle::menus(Preset::Studio);
  bool same = sm.size() == 8;
  for (size_t i = 0; same && i < 8; ++i) same = sm[i].name && std::string(sm[i].name) == studio[i] && sm[i].items.size() == 1 && !sm[i].items[0].submenu;
  expect(same, "studio menu bar unchanged: File Edit Add Patterns View Options Tools Help");
  for (Preset p : kAll)
    for (menustyle::Block b : menustyle::studioBlocks()) {
      char name[96];
      std::snprintf(name, sizeof name, "%s style reaches studio menu %d", layout::id(p), static_cast<int>(b));
      expect(menustyle::reaches(p, b), name);
    }
  auto names = [](Preset p) {
    std::string s;
    for (const auto& m : menustyle::menus(p))
      if (m.name) s += std::string(s.empty() ? "" : " ") + m.name;
    return s;
  };
  expect(names(Preset::Live) == "File Edit Create View Options Tools Help", "live menu order");
  expect(names(Preset::Logic) == "File Edit Track Patterns Navigate Record Mix View Tools Help", "logic menu order");
  int programs = 0;
  for (const auto& m : menustyle::menus(Preset::Logic)) programs += m.name ? 0 : 1;
  expect(programs == 1, "logic has one program menu (on the mark)");
}

bool hasCmd(const std::map<std::string, std::vector<keys::Chord>>& m) {
  for (const auto& [id, chords] : m)
    for (const auto& c : chords)
      if (c.mods & keys::kCmd) return true;
  return false;
}

std::string first(const keys::Keymap& km, const char* id) {
  const auto& b = km.bindings(id);
  return b.empty() ? std::string() : keys::format(b.front());
}

void testKeys() {
  // Studio: the table itself, nothing moved.
  const auto& studio = keys::profileDefaults(keys::Profile::Studio, false);
  bool same = true;
  for (const auto& c : keys::commands()) same &= studio.at(c.id) == c.defaults;
  expect(same, "studio shortcut set is the default table");
  for (keys::Profile p : {keys::Profile::Studio, keys::Profile::Live, keys::Profile::Logic})
    for (bool sep : {false, true}) {
      keys::Keymap km;
      km.setProfile(p, sep);
      const auto clashes = km.allConflicts();
      for (const auto& c : clashes) std::printf("# %s/%d clash: %s\n", keys::profileId(p), sep ? 1 : 0, c.c_str());
      char name[96];
      std::snprintf(name, sizeof name, "%s set (Cmd %s) has no same-context clashes", keys::profileId(p), sep ? "separate" : "folded");
      expect(clashes.empty(), name);
      expect(keys::profileDefaults(p, sep).size() == keys::commands().size(), "every command has an entry in every set");
      // The mixer track menu's Select items show Alt+L and Ctrl+Alt+L (Cmd+Alt+L where Cmd is its own key):
      // every set keeps both, so the labels never name a key that does nothing.
      std::snprintf(name, sizeof name, "%s set (Cmd %s): Alt+L and Ctrl+Alt+L select what is routed to the mixer track",
                    keys::profileId(p), sep ? "separate" : "folded");
      const bool cmd = km.separateCmd();
      expect(first(km, "mixer.selectRoutedChannels") == "Alt+L" && first(km, "mixer.selectRoutedTo") == (cmd ? "Cmd+Alt+L" : "Ctrl+Alt+L") &&
                 km.lookup(keys::Context::Mixer, keys::parse("Alt+L")).size() == 1 &&
                 km.lookup(keys::Context::Mixer, keys::parse(cmd ? "Cmd+Alt+L" : "Ctrl+Alt+L")).size() == 1,
             name);
    }
  expect(!hasCmd(keys::profileDefaults(keys::Profile::Logic, false)), "logic on Linux and Windows: no Cmd chords");
  expect(!hasCmd(keys::profileDefaults(keys::Profile::Live, false)) && !hasCmd(keys::profileDefaults(keys::Profile::Live, true)),
         "live never separates Cmd");
  expect(hasCmd(keys::profileDefaults(keys::Profile::Logic, true)), "logic on macOS uses Cmd");
  {
    keys::Keymap km;
    km.setProfile(keys::Profile::Live, true);
    expect(!km.separateCmd(), "only the logic set separates Cmd");
    expect(first(km, "view.toggleMain") == "Tab", "live: Tab switches the main view");
    expect(first(km, "view.mixer") == "Ctrl+Alt+M" && first(km, "view.browser") == "Ctrl+Alt+B", "live: view keys on Ctrl+Alt");
    expect(first(km, "options.metronome") == "O" && first(km, "options.typingKeyboard") == "M", "live: O metronome, M typing keyboard");
    expect(first(km, "playlist.duplicate") == "Ctrl+D" && first(km, "playlist.merge") == "Ctrl+J", "live: Ctrl+D duplicate, Ctrl+J join");
    expect(first(km, "playlist.toolDraw") == "B" && km.bindings("playlist.toolSlip").empty(), "live: B draws, the other tool letters are free");
    expect(first(km, "transport.record") == "F9", "live: F9 records");
  }
  {
    keys::Keymap km;
    km.setProfile(keys::Profile::Logic, false);
    expect(first(km, "view.browser") == "B" && first(km, "view.mixer") == "X" && first(km, "view.editors") == "E" && first(km, "view.pianoRoll") == "P",
           "logic: B X E P open the panes");
    expect(first(km, "transport.record") == "R" && first(km, "transport.toStart") == "Enter" && first(km, "options.metronome") == "K",
           "logic: R records, Return goes to the start, K metronome");
    expect(km.bindings("playlist.toolPaint").empty() && km.bindings("piano.toolDraw").empty(), "logic: tool letters are unbound");
    expect(first(km, "playlist.mute") == "Ctrl+Alt+M", "logic on Linux: the control chords are Ctrl+Alt");
    expect(first(km, "file.save") == "Ctrl+S", "logic on Linux: command chords stay Ctrl");
    keys::Keymap mac;
    mac.setProfile(keys::Profile::Logic, true);
    expect(mac.separateCmd() && first(mac, "file.save") == "Cmd+S", "logic on macOS: Save is Cmd+S");
    expect(first(mac, "playlist.mute") == "Ctrl+M", "logic on macOS: Control is its own modifier");
    expect(keys::parse("Cmd+S") == mac.bindings("file.save").front(), "Cmd chords parse");
  }
  // Overrides belong to one set.
  keys::Keymap km;
  km.setProfile(keys::Profile::Logic, false);
  km.set("view.mixer", {keys::parse("F3")});
  const std::string tracksOverrides = km.serialize();
  expect(tracksOverrides.find("view.mixer") != std::string::npos, "an override in the logic set");
  km.setProfile(keys::Profile::Studio, false);
  km.load("");  // the studio set's own (none)
  expect(first(km, "view.mixer") == "F9", "the studio set does not see the logic override");
  km.setProfile(keys::Profile::Logic, false);
  km.load(tracksOverrides);
  expect(first(km, "view.mixer") == "F3", "the logic override comes back with its set");
  km.set("view.browser", {keys::parse("B")});
  expect(!km.customized("view.browser"), "a set's own default is not an override");
}

void testGeometry() {
  const float sizes[][2] = {{952, 526}, {1016, 558}, {1272, 638}, {1432, 818}, {1912, 998}, {2552, 1358}, {3832, 2078}};
  for (const auto& sz : sizes) {
    const float w = sz[0], h = sz[1];
    char name[128];
    const layout::LiveLayout P = layout::liveLayout(w, h);
    bool okP = P.area.contains(P.browser) && P.area.contains(P.main) && P.area.contains(P.mixer) && P.area.contains(P.bottom) &&
               !P.browser.overlaps(P.main) && !P.main.overlaps(P.mixer) && !P.mixer.overlaps(P.bottom) && !P.browser.overlaps(P.bottom);
    okP = okP && P.bottomFrac > 0 && P.bottomFrac < 0.5f && P.browserFrac > 0 && P.browserFrac < 0.5f && P.mixerFrac > 0 && P.mixerFrac < 0.7f;
    okP = okP && P.main.h >= 0.25f * h && P.main.w >= 0.6f * w && P.browser.w >= 240 && P.browser.w <= 340;
    std::snprintf(name, sizeof name, "live layout at %.0fx%.0f: panes inside, no overlap, main view keeps room", static_cast<double>(w), static_cast<double>(h));
    expect(okP, name);
    const layout::LogicLayout T = layout::logicLayout(w, h);
    bool okT = T.area.contains(T.browser) && T.area.contains(T.main) && T.area.contains(T.bottom) && T.area.contains(T.right) &&
               !T.browser.overlaps(T.main) && !T.main.overlaps(T.bottom) && !T.main.overlaps(T.right) && !T.bottom.overlaps(T.right);
    okT = okT && T.browserFrac > 0 && T.browserFrac < 1 && T.rightFrac > 0 && T.rightFrac < 1 && T.bottomFrac > 0 && T.bottomFrac < 1;
    okT = okT && T.main.w >= 0.45f * w && T.main.h >= 0.45f * h && T.browser.w >= 238 && T.browser.w <= 320;
    std::snprintf(name, sizeof name, "logic layout at %.0fx%.0f: panes inside, no overlap, the playlist keeps room", static_cast<double>(w), static_cast<double>(h));
    expect(okT, name);
  }
  const layout::LiveLayout P = layout::liveLayout(1432, 818);
  expect(P.browser.w == 300 && P.bottom.h == 245, "live at 1440x900: browser 300, bottom panel 245");
  const layout::LogicLayout T = layout::logicLayout(1912, 998);
  expect(T.browser.w == 320 && T.right.w == 340, "logic at 1920x1080: browser 320, right column 340");
}

}  // namespace

int main() {
  testSettingRoundTrip();
  testLayoutFiles();
  testNormalise();
  testLayoutsKeepTheme();
  testOldPaletteLink();
  testMenus();
  testKeys();
  testGeometry();
  return finish();
}
