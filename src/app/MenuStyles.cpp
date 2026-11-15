// Gridline - menu bar styles of the layout presets (K-79, core/MenuStyle.h):
// the menu definition (MenuDef.h) each block stands for. The studio blocks are
// the studio menus (Menus.cpp); the gathered blocks only call existing commands.
#include <algorithm>
#include <map>
#include <string>
#include "../../third_party/imgui/imgui.h"
#include "../core/I18n.h"
#include "../core/MenuOps.h"
#include "../core/MenuOpsExtra.h"
#include "../core/ProjectFiles.h"
#include "../core/MenuStyle.h"
#include "../core/MixerOps.h"
#include "../core/Meter.h"
#include "../core/TrackModes.h"
#include "App.h"
#include "ContextMenus.h"
#include "LayoutParts.h"
#include "ExportUi.h"
#include "LiveLook.h"
#include "Keys.h"
#include "LayoutPresets.h"
#include "LayoutShellUi.h"
#include "LogicPlugins.h"
#include "LogicNewTracks.h"
#include "MenuDef.h"
#include "imgui_hooks/ImGuiItemHook.h"
#include "MidiUi.h"
#include "TrackerView.h"
#include "Windows.h"

namespace gl {

namespace md = menudef;
using namespace menus;
using i18n::N_;

namespace {

const md::Menu& navigateDef() {
  static const md::Menu m = {
      md::item(N_("Go to the start"), "transport.toStart", [](App& a) { a.engine.setPosition(0); }),
      md::item(N_("Back one bar"), "transport.rewindBar",
               [](App& a) {
                 const Tick bar = std::max<Tick>(1, a.doc.song().barTicks());
                 const Tick pos = static_cast<Tick>(a.engine.position());
                 const bool songBars = a.engine.mode() != PlayMode::Pattern;  // K-193: real bars of the song's meter map (as the top bar)
                 a.engine.setPosition(std::max<Tick>(0, songBars ? MeterMap(a.doc.song()).barStartAt(std::max<Tick>(0, pos - 1)) : (pos - 1) / bar * bar));
               }),
      md::item(N_("Forward one bar"), "transport.forwardBar",
               [](App& a) {
                 const Tick bar = std::max<Tick>(1, a.doc.song().barTicks());
                 const Tick pos = static_cast<Tick>(a.engine.position());
                 const bool songBars = a.engine.mode() != PlayMode::Pattern;
                 a.engine.setPosition(songBars ? MeterMap(a.doc.song()).nextBar(pos) : (pos / bar + 1) * bar);
               }),
      md::submenu(N_("Markers"), {md::provider([](App& a) {  // K-112B: go to a marker (MAP.md 2.1 Go To)
                    const auto& ms = a.doc.song().markers;
                    int shown = 0;
                    for (size_t i = 0; i < ms.size(); ++i) {
                      if (ms[i].name.empty()) continue;
                      ImGui::PushID(static_cast<int>(i));
                      if (ImGui::MenuItem(ms[i].name.c_str())) a.engine.setPosition(ms[i].pos);
                      ImGui::PopID();
                      ++shown;
                    }
                    if (!shown) ImGui::MenuItem(i18n::tr("No markers"), nullptr, false, false);
                  })}),
      md::separator(),
      md::item(N_("Cycle"), "transport.cycle", [](App& a) { toggleCycle(a); })
          .check([](App& a) { return a.playlist.selEnd > a.playlist.selStart; }),
      md::item(N_("Add marker"), "playlist.marker", [](App& a) { tracksAddMarker(a); }),
      md::separator(),  // K-112b: grouped as MAP.md 2.1: go to, scroll, zoom, then our play modes
      md::item(N_("Follow the song position"), [](App& a) { a.settings.set("ui.autoScroll", !a.settings.getBool("ui.autoScroll")); })
          .check([](App& a) { return a.settings.getBool("ui.autoScroll"); }),
      md::separator(),
      md::submenu(N_("Zoom"), {md::item(N_("Zoom in"), "playlist.zoomIn", [](App& a) { a.playlist.zoomRequest *= 1.5f; }),
                               md::item(N_("Zoom out"), "playlist.zoomOut", [](App& a) { a.playlist.zoomRequest /= 1.5f; }),
                               md::item(N_("Zoom to fit the song"), "playlist.zoomFit", [](App& a) { a.playlist.fitRequest = true; })}),
      md::separator(),
      md::item(N_("Song mode"), "transport.songMode", [](App& a) { a.engine.setMode(PlayMode::Song); })
          .check([](App& a) { return a.engine.mode() == PlayMode::Song; }),
      md::item(N_("Pattern mode"), [](App& a) { a.engine.setMode(PlayMode::Pattern); }).check([](App& a) { return a.engine.mode() == PlayMode::Pattern; }),
      md::item(N_("Live mode"), [](App& a) { a.engine.setMode(a.engine.mode() == PlayMode::Live ? PlayMode::Song : PlayMode::Live); })
          .check([](App& a) { return a.engine.mode() == PlayMode::Live; }),
  };
  return m;
}

const md::Menu& recordDef() {
  static const md::Menu m = {
      md::item(N_("Record"), "transport.record", [](App& a) { a.recordArmed = !a.recordArmed; }).check([](App& a) { return a.recordArmed; }),
      md::separator(),
      md::toggle(N_("Metronome"), "options.metronome", [](App& a) { return &a.metronome; }),
      md::item(N_("Count-in"), "options.countIn", [](App& a) { a.countInBars = a.countInBars > 0 ? 0 : 1; }).check([](App& a) { return a.countInBars > 0; }),
      md::toggle(N_("Step record"), nullptr, [](App& a) { return &a.stepRecord; }),
      md::submenu(N_("Recording options"), {md::provider([](App& a) { recordOptionsMenu(a); })}),
      md::separator(),
      md::toggle(N_("Typing keyboard to piano"), "options.typingKeyboard", [](App& a) { return &a.typingKeyboard; }),
      md::submenu(N_("Typing keyboard options"), {md::provider([](App& a) { typingKeyboardMenu(a); })}),
      md::toggle(N_("Multilink to controllers"), nullptr, [](App& a) { return &a.multilink; }),
  };
  return m;
}

// K-112b: Record in the Tracks layout's grouping (MAP.md 2.1): record; its options; the click and
// count-in; step and typing input; controllers.
const md::Menu& tracksRecordDef() {
  static const md::Menu m = {
      md::item(N_("Record"), "transport.record", [](App& a) { a.recordArmed = !a.recordArmed; }).check([](App& a) { return a.recordArmed; }),
      md::submenu(N_("Recording options"), {md::provider([](App& a) { recordOptionsMenu(a); })}),
      md::separator(),
      md::toggle(N_("Metronome"), "options.metronome", [](App& a) { return &a.metronome; }),
      md::item(N_("Count-in"), "options.countIn", [](App& a) { a.countInBars = a.countInBars > 0 ? 0 : 1; }).check([](App& a) { return a.countInBars > 0; }),
      md::separator(),
      md::toggle(N_("Step record"), nullptr, [](App& a) { return &a.stepRecord; }),
      md::toggle(N_("Typing keyboard to piano"), "options.typingKeyboard", [](App& a) { return &a.typingKeyboard; }),
      md::submenu(N_("Typing keyboard options"), {md::provider([](App& a) { typingKeyboardMenu(a); })}),
      md::separator(),
      md::toggle(N_("Multilink to controllers"), nullptr, [](App& a) { return &a.multilink; }),
  };
  return m;
}

const md::Menu& mixDef() {
  static const md::Menu m = {
      md::toggle(N_("Mixer"), "view.mixer", [](App& a) { return &a.showMixer; }),
      md::toggle(N_("Stage"), "view.stage", [](App& a) { return &a.showStage; }),
      md::toggle(N_("Master chain"), nullptr, [](App& a) { return &a.showMaster; }),
      md::toggle(N_("Performance"), nullptr, [](App& a) { return &a.showPerformance; }),
      md::separator(),
      // K-112B (checkpoint 39): groups of the selected mixer tracks, as MAP.md 2.1 / 5.3 open the Mix menu.
      md::submenu(N_("Group"), {md::provider([](App& a) {
                    const Song& s = a.doc.song();
                    const std::vector<ObjectId> ids = mixerSelection(a);
                    const MixerTrack* t = s.mixerTrack(a.selMixer);
                    const bool any = !ids.empty() && t;
                    if (ImGui::MenuItem(i18n::tr("Create group..."), nullptr, false, any))
                      menus::openPrompt("Group name", mixops::newGroupName(s), [&a, ids](const std::string& n) {
                        if (!n.empty()) mixops::setGroup(a.doc, ids, n);
                      });
                    const auto groups = mixops::groupNames(s);
                    if (ImGui::BeginMenu(i18n::tr("Add to group"), any && !groups.empty())) {
                      for (const auto& g : groups)
                        if (ImGui::MenuItem(g.c_str(), nullptr, t->group == g)) mixops::setGroup(a.doc, ids, g);
                      ImGui::EndMenu();
                    }
                    if (ImGui::MenuItem(i18n::tr("Remove from group"), nullptr, false, any && !t->group.empty())) mixops::setGroup(a.doc, ids, "");
                  })}),
      md::item(N_("Plugin delay compensation"),  // K-112b: the mapped Mix menu's options group
               [](App& a) {
                 a.settings.set("audio.pdc", !a.settings.getBool("audio.pdc"));
                 a.syncEngine();
               })
          .check([](App& a) { return a.settings.getBool("audio.pdc"); }),
      md::submenu(N_("Mixer layout"), {md::provider([](App& a) {
                    const int cur = static_cast<int>(a.settings.getNumber("ui.mixerLayout"));
                    const char* names[] = {N_("Compact strips"), N_("Normal strips"), N_("Wide strips")};
                    for (int i = 0; i < 3; ++i)
                      if (ImGui::MenuItem(i18n::tr(names[i]), nullptr, cur == i)) a.settings.set("ui.mixerLayout", i);
                  })}),
  };
  return m;
}

// K-112b: the track the Track menu acts on: the first selected header, else the first selected
// clip's track, else the first track.
int currentTrack(App& a) {
  const Song& s = a.doc.song();
  if (!a.playlist.trackSel.empty()) return *a.playlist.trackSel.begin();
  for (ObjectId id : a.playlist.selected)
    if (const Clip* c = s.clip(id)) return c->track;
  return 0;
}

const md::Menu& trackDef() {
  static const md::Menu m = {
      md::item(N_("New tracks..."), "track.newTracks", [](App&) { newtracks::open(); }),
      md::item(N_("New track"),
               [](App& a) {
                 a.later([&a] {
                   // K-112c: in Clips a track after the tracks in use is made in use, or neither view would list it.
                   const int at = currentTrack(a) + 1;
                   if (livelook::active(a) && at >= trackmodes::usedTrackCount(a.doc.song())) trackmodes::addUsedTrack(a.doc);
                   else ops::insertPlaylistTrack(a.doc, at);
                 });
               })
          .when([](App& a) { return static_cast<int>(a.doc.song().tracks.size()) < kMaxPlaylistTracks; }),
      md::submenu(N_("New instrument track"), {md::provider([](App& a) { addChannelMenu(a); })}),
      md::item(N_("New track with the same settings"), "track.duplicate", [](App& a) { tracksNewSameSettings(a); })
          .when([](App& a) { return static_cast<int>(a.doc.song().tracks.size()) < kMaxPlaylistTracks; }),
      md::item(N_("New mixer track"),
               [](App& a) {
                 const ObjectId id = a.doc.addMixerTrack();
                 if (id != kNoId) a.selMixer = id;
               }),
      md::separator(),
      md::item(N_("Delete track"), "track.delete", [](App& a) { tracksDeleteSelected(a); })
          .when([](App& a) { return a.doc.song().tracks.size() > 1; }),
  };
  return m;
}

// K-112b: shows one of the lower pane's editors (the Tracks layout's editors area).
void showLower(App& a, int which) {
  layoutui::setRegionVisible(a, "logic.lower", true);
  bool* smart = layoutui::windowOpenFlag(a, "Smart controls");
  if (smart) *smart = which == 3;
  a.showMixer = which == 2;
  if (which == 0) a.showPiano = true;
  if (which == 1) a.showRack = true;
}

const md::Menu& windowDef() {
  static const md::Menu m = {
      md::item(N_("Main window"), [](App& a) { a.focusWindow = "Playlist"; }),
      md::item(N_("Mixer"), "view.mixer", [](App& a) { showLower(a, 2); }),
      md::item(N_("Step sequencer"), "view.channelRack", [](App& a) { showLower(a, 1); }),
      md::item(N_("Piano roll"), "view.pianoRoll", [](App& a) { showLower(a, 0); }),
      md::item(N_("Smart controls"), [](App& a) { showLower(a, 3); }),
      md::separator(),
      md::toggle(N_("Undo history"), "edit.history", [](App& a) { return &a.showHistory; }),
      md::toggle(N_("Performance"), nullptr, [](App& a) { return &a.showPerformance; }),
      md::toggle(N_("Typing keyboard to piano"), "options.typingKeyboard", [](App& a) { return &a.typingKeyboard; }),
      md::separator(),
      md::item(N_("Hide all plugin windows"), "view.hidePlugins", [](App& a) { logicplug::toggleHidden(a); })
          .check([](App&) { return logicplug::hidden(); }),
  };
  return m;
}

// K-112c (round 45): Create in the Clips layout, as the reference's (MAP.md 2.4): new tracks first,
// then a marker, then the imports; Studio's Add follows in a submenu (MenuStyle.cpp).
void selectNewTrack(App& a, int t) {
  if (t >= 0) a.playlist.trackSel = {t};
}

const md::Menu& clipsCreateDef() {
  static const md::Menu m = {
      md::item(N_("New audio track"), "track.newAudio", [](App& a) { a.later([&a] { selectNewTrack(a, trackmodes::addAudioTrack(a.doc)); }); })
          .tip(N_("A track for audio clips and recording, on a mixer track of its own, after the shown tracks")),
      md::item(N_("New MIDI track"), "track.newMidi", [](App& a) { a.later([&a] { selectNewTrack(a, trackmodes::addMidiTrack(a.doc)); }); })
          .tip(N_("An empty track for pattern clips, after the shown tracks")),
      md::submenu(N_("New instrument track"), {md::provider([](App& a) { addChannelMenu(a); })}),
      md::separator(),
      md::item(N_("Add marker at the song position"),
               [](App& a) { ops::addMarker(a.doc, std::max<Tick>(0, static_cast<Tick>(a.engine.position())), ops::nextMarkerName(a.doc.song())); }),
      md::separator(),
      md::item(N_("Import audio file..."),
               [](App& a) {
                 menus::fileDialog(a, "Import audio clip", false, importAudioTypes()[0].exts, "", [&a](const std::string& p) { exportui::importAudioAsClip(a, p); });
               }),
      md::item(N_("Import MIDI file..."),
               [](App& a) {
                 menus::fileDialog(a, "Import MIDI file", false, importMidiTypes()[0].exts, "", [&a](const std::string& p) { exportui::importMidiFile(a, p); });
               }),
  };
  return m;
}

bool bandShown(App& a, const char* region) {
  return layout::visible(layout::shell(layout::Preset::Live), layoutui::regionState(a, layout::Preset::Live), region);
}

// K-112c (round 45): Navigate in the Clips layout (MAP.md 2.7): the two main views, then the panels
// and bands; Navigate's song-position items follow.
const md::Menu& clipsNavigateDef() {
  static const md::Menu m = {
      md::item(N_("Arrangement"), [](App& a) { livelook::showView(a, false); }).check([](App& a) { return !livelook::gridView(a); }),
      md::item(N_("Clip grid"), [](App& a) { livelook::showView(a, true); }).check([](App& a) { return livelook::gridView(a); }),
      md::separator(),
      md::toggle(N_("Browser"), "view.browser", [](App& a) { return &a.showBrowser; }),
      // One detail band at a time (round 45): choosing one swaps it in; choosing the shown one hides it.
      md::item(N_("Clip band"), [](App& a) { layoutui::setRegionVisible(a, "live.clip", !bandShown(a, "live.clip")); })
          .check([](App& a) { return bandShown(a, "live.clip"); }),
      md::item(N_("Device band"), [](App& a) { layoutui::setRegionVisible(a, "live.device", !bandShown(a, "live.device")); })
          .check([](App& a) { return bandShown(a, "live.device"); }),
      md::toggle(N_("Mixer"), "view.mixer", [](App& a) { return &a.showMixer; }),
      md::separator(),
  };
  return m;
}

const md::Menu& programDef() {
  static const md::Menu m = {
      md::item(N_("About Gridline"), [](App& a) { a.showAbout = true; }),
      md::separator(),
      md::item(N_("Settings..."), "view.settings",
               [](App& a) {
                 a.showSettings = true;
                 a.focusWindow = "Settings";
               }),
      md::item(N_("Keyboard shortcuts..."), "help.shortcuts", [](App& a) { a.showShortcuts = true; }),
      md::item(N_("Plugin manager..."), [](App& a) { a.showPluginManager = true; }),
      md::item(N_("Welcome screen"), [](App& a) { a.home.showHome(a.now); }),
      md::separator(),
      md::item(N_("Quit"), "file.quit", [](App& a) { a.quitRequested = true; }),
  };
  return m;
}

// K-106: the Tracker's pages and header screens, worded as the header's page strip and buttons
// word them (one name per command), each with the Tracker set's Ctrl+Alt key.
const md::Menu& pagesDef() {
  static const md::Menu m = {
      md::item(N_("Pattern"), "view.patternEditor", [](App& a) { trackerview::showPage(a, "Pattern editor"); }),
      md::item(N_("Song"), "view.playlist", [](App& a) { trackerview::showPage(a, "Playlist"); }),
      md::item(N_("Roll"), "view.pianoRoll", [](App& a) { trackerview::showPage(a, "Piano roll"); }),
      md::item(N_("Steps"), "view.channelRack", [](App& a) { trackerview::showPage(a, "Channel rack"); }),
      md::item(N_("Mix"), "view.mixer", [](App& a) { trackerview::showPage(a, "Mixer"); }),
      md::item(N_("Pads"), "view.pads", [](App& a) { trackerview::showPage(a, "Live pads"); }),
      md::separator(),
      md::item(N_("Channel settings"), "view.channelSettings", [](App& a) { a.settingsChannel = a.settingsChannel != kNoId ? kNoId : a.selChannel; })
          .check([](App& a) { return a.settingsChannel != kNoId; }),
      md::item(N_("Audio editor"), "view.audioEditor", [](App& a) { a.settings.set("ui.loupePinned", !a.settings.getBool("ui.loupePinned")); })
          .check([](App& a) { return a.settings.getBool("ui.loupePinned"); }),
      md::item(N_("Files"), "view.files", [](App&) { trackerview::setHeaderSwap(trackerview::headerSwap() == 2 ? 0 : 2); })
          .check([](App&) { return trackerview::headerSwap() == 2; }),
      md::item(N_("Tracker help"), "help.trackerKeys", [](App&) { trackerview::setHeaderSwap(trackerview::headerSwap() == 1 ? 0 : 1); })
          .check([](App&) { return trackerview::headerSwap() == 1; }),
      md::separator(),
      md::item(N_("Header band"), "view.headerBand", [](App& a) { trackerview::setExtended(a, !trackerview::extended()); })
          .check([](App&) { return !trackerview::extended(); }),
  };
  return m;
}

// K-106: the Tracker's Edit menu. On the Pattern page the clipboard items act on the pattern's
// block (the classic block keys shown); on another page on what that page has selected, as
// Studio's Edit does. Then the row edits of the pattern editor.
bool onPattern(App&) { return trackerview::shown(); }
const char* editKeyOf(const App& a, const char* op) {
  const char* prefix = a.editFocus == App::EditFocus::Playlist ? "playlist." : a.editFocus == App::EditFocus::Rack ? "rack."
                       : a.editFocus == App::EditFocus::Mixer ? "mixer." : "piano.";
  return keys::label((std::string(prefix) + op).c_str());
}
const char* trackerKey(App& a, const char* tracker, const char* op) { return onPattern(a) ? tracker : editKeyOf(a, op); }
const md::Menu& trackerEditDef() {
  static const md::Menu m = {
      md::item(nullptr, "edit.undo", [](App& a) { a.doc.undo(); })
          .text([](App& a) {
            const auto& h = a.doc.history();
            return h.canUndo() ? "Undo " + h.undoLabel() : std::string("Undo");
          })
          .when([](App& a) { return a.doc.history().canUndo(); }),
      md::item(nullptr, "edit.redo", [](App& a) { a.doc.redo(); })
          .text([](App& a) {
            const auto& h = a.doc.history();
            return h.canRedo() ? "Redo " + h.redoLabel() : std::string("Redo");
          })
          .when([](App& a) { return a.doc.history().canRedo(); }),
      md::item(N_("Undo history"), "edit.history", [](App& a) { a.showHistory = !a.showHistory; }).check([](App& a) { return a.showHistory; }),
      md::separator(),
      md::item(N_("Cut"), [](App& a) { onPattern(a) ? trackerview::blockOp(a, trackerview::BlockOp::Cut) : editCut(a); })
          .keyText([](App& a) { return trackerKey(a, "Alt+F3", "cut"); }),
      md::item(N_("Copy"), [](App& a) { onPattern(a) ? trackerview::blockOp(a, trackerview::BlockOp::Copy) : editCopy(a); })
          .keyText([](App& a) { return trackerKey(a, "Alt+F4", "copy"); }),
      md::item(N_("Paste"), [](App& a) { onPattern(a) ? trackerview::blockOp(a, trackerview::BlockOp::Paste) : editPaste(a); })
          .keyText([](App& a) { return trackerKey(a, "Alt+F5", "paste"); })
          .when([](App& a) { return onPattern(a) ? trackerview::hasClip() : canPaste(a); }),
      md::item(N_("Delete"), [](App& a) { onPattern(a) ? trackerview::blockOp(a, trackerview::BlockOp::Clear) : editDelete(a); })
          .keyText([](App& a) { return trackerKey(a, "Delete", "delete"); }),
      md::item(N_("Duplicate"), [](App& a) { editDuplicate(a); })
          .keyText([](App& a) { return editKeyOf(a, "duplicate"); })
          .when([](App& a) { return !onPattern(a) && a.editFocus != App::EditFocus::Mixer; }),
      md::separator(),
      md::item(N_("Select all"), [](App& a) { onPattern(a) ? trackerview::blockOp(a, trackerview::BlockOp::SelectAll) : editSelectAll(a); })
          .keyText([](App& a) { return onPattern(a) ? "" : editKeyOf(a, "selectAll"); }),
      md::item(N_("Deselect"), [](App& a) { onPattern(a) ? trackerview::blockOp(a, trackerview::BlockOp::Deselect) : editDeselect(a); })
          .keyText([](App& a) { return onPattern(a) ? "" : editKeyOf(a, "deselect"); }),
      md::separator(),
      md::item(N_("Insert row"), [](App& a) { trackerview::insertRow(a, false); }).keyText([](App&) { return "Insert"; }).when(onPattern),
      md::item(N_("Insert row in every track"), [](App& a) { trackerview::insertRow(a, true); }).keyText([](App&) { return "Shift+Insert"; }).when(onPattern),
      md::item(N_("Delete row"), [](App& a) { trackerview::deleteRow(a, false); }).when(onPattern),
      md::item(N_("Delete row in every track"), [](App& a) { trackerview::deleteRow(a, true); }).when(onPattern),
      md::item(nullptr, [](App& a) { trackerview::interpolate(a); })
          .text([](App&) {
            const int f = trackerview::cursorField();
            return std::string(i18n::tr(f == 3 ? "Interpolate pan" : f == 4 ? "Interpolate effect" : "Interpolate volume"));
          })
          .when(onPattern),
      md::separator(),
      md::item(N_("Rename..."), "edit.rename", [](App& a) { renameFocused(a); }),
  };
  return m;
}

// K-106: the Tracker's pattern editor settings, at the top of its Pattern menu.
const md::Menu& trackerPatternDef() {
  static const md::Menu m = {
      md::item(N_("Edit mode"), [](App&) { trackerview::setEditMode(!trackerview::editMode()); })
          .check([](App&) { return trackerview::editMode(); })
          .keyText([](App&) { return "Esc"; }),
      md::submenu(N_("Edit step"), {md::provider([](App&) {
                    for (int n = 0; n <= 16; ++n)
                      if (ImGui::MenuItem(std::to_string(n).c_str(), nullptr, trackerview::editStep() == n)) trackerview::setEditStep(n);
                  })}),
      md::item(N_("Follow playback in the pattern"), [](App&) { trackerview::setFollow(!trackerview::follow()); }).check([](App&) { return trackerview::follow(); }),
      md::submenu(N_("Columns"),
                  {md::item(N_("One more note column"), [](App& a) { trackerview::addColumn(a, 1); }),
                   md::item(N_("One note column fewer"), [](App& a) { trackerview::addColumn(a, -1); }),
                   md::separator(),
                   md::item(N_("Volume"), [](App& a) { a.settings.set("tracker.volumeColumn", !a.settings.getBool("tracker.volumeColumn")); })
                       .check([](App& a) { return a.settings.getBool("tracker.volumeColumn"); }),
                   md::item(N_("Pan"), [](App& a) { a.settings.set("tracker.panColumn", !a.settings.getBool("tracker.panColumn")); })
                       .check([](App& a) { return a.settings.getBool("tracker.panColumn"); }),
                   md::item(N_("Effect"), [](App& a) { a.settings.set("tracker.effectColumn", !a.settings.getBool("tracker.effectColumn")); })
                       .check([](App& a) { return a.settings.getBool("tracker.effectColumn"); })}),
      md::submenu(N_("Row numbers"),
                  {md::item(N_("Hexadecimal"), [](App& a) { a.settings.set("tracker.rowBase", std::string("hex")); })
                       .check([](App& a) { return a.settings.getString("tracker.rowBase") != "dec"; }),
                   md::item(N_("Decimal"), [](App& a) { a.settings.set("tracker.rowBase", std::string("dec")); })
                       .check([](App& a) { return a.settings.getString("tracker.rowBase") == "dec"; })}),
      md::separator(),
  };
  return m;
}

// K-106: the Tracker's windows and panels: Studio's View less what the Tracker names otherwise
// (the pages and header screens in View's first part) and Undo history (in Edit), so no command
// shows twice under two names.
bool trackerDuplicate(const md::Entry& e) {
  static const char* const keys[] = {"view.playlist", "view.channelRack", "view.pianoRoll", "view.mixer", "edit.history", "view.closePlugins"};
  for (const char* k : keys)
    if (e.key && std::string(e.key) == k) return true;
  static const char* const labels[] = {"Pads", "Channel settings", "Panels"};
  for (const char* l : labels)
    if (e.label && std::string(e.label) == l) return true;
  return false;
}
const md::Menu& trackerViewDef() {
  static const md::Menu m = [] {
    md::Menu out;
    // The zoom group is Song > Zoom's (the playlist's zoom keys clash with the pattern's PgUp,
    // PgDn and Home); it runs from its heading to the Layout heading.
    bool zoom = false;
    for (const md::Entry& e : md::studio(4)) {
      if (e.kind == md::Kind::Heading) zoom = e.dynLabel != nullptr;
      if (zoom) continue;
      if (!trackerDuplicate(e)) {
        out.push_back(e);
        if (e.key && std::string(e.key) == "view.stage")  // the panels the Tracker has no page or screen for
          out.push_back(md::submenu(N_("Panels"), {md::provider([](App& a) { layoutparts::panelsMenu(a, true); })}));
      }
    }
    return out;
  }();
  return m;
}

const md::Menu* blockMenu(menustyle::Block b) {
  using B = menustyle::Block;
  switch (b) {
    case B::File: return &md::studio(0);
    case B::Edit: return &md::studio(1);
    case B::Add: return &md::studio(2);
    case B::Patterns: return &md::studio(3);
    case B::View: return &md::studio(4);  // its Layout section holds the presets (K-79)
    case B::Options: return &md::studio(5);
    case B::Tools: return &md::studio(6);
    case B::Help: return &md::studio(7);
    case B::Navigate: return &navigateDef();
    case B::Record: return &recordDef();
    case B::Mix: return &mixDef();
    case B::Program: return &programDef();
    case B::Track: return &trackDef();
    case B::Window: return &windowDef();
    case B::TracksEdit: return &md::tracksEdit();
    case B::TracksRecord: return &tracksRecordDef();
    case B::Pages: return &pagesDef();
    case B::TrackerEdit: return &trackerEditDef();
    case B::TrackerPattern: return &trackerPatternDef();
    case B::TrackerView: return &trackerViewDef();
    case B::ClipsCreate: return &clipsCreateDef();
    case B::ClipsNavigate: return &clipsNavigateDef();
    default: return nullptr;
  }
}

}  // namespace

// K-112B (checkpoint 39), as the reference's Track menu behaves: a new track right below the current
// one with its settings (name, colour, height) and none of its clips.
void tracksNewSameSettings(App& a) {
  if (static_cast<int>(a.doc.song().tracks.size()) >= kMaxPlaylistTracks) return;
  const int at = currentTrack(a);
  a.later([&a, at] {
    if (ops::clonePlaylistTrack(a.doc, at, false)) a.playlist.trackSel = {at + 1};
  });
}

// Every selected track goes (one undo step), the current one when none is selected; one track always stays.
void tracksDeleteSelected(App& a) {
  std::vector<int> del(a.playlist.trackSel.begin(), a.playlist.trackSel.end());
  if (del.empty()) del.push_back(currentTrack(a));
  a.later([&a, del] {
    a.doc.beginGroup("delete tracks");
    for (auto it = del.rbegin(); it != del.rend(); ++it)
      if (a.doc.song().tracks.size() > 1) ops::deletePlaylistTrack(a.doc, *it);
    a.doc.endGroup();
    a.playlist.trackSel.clear();
  });
}

// A marker at the bar nearest the song position (the reference rounds a new marker to the nearest bar).
void tracksAddMarker(App& a) {
  const Song& s = a.doc.song();
  const MeterMap mm(s);
  const Tick pos = static_cast<Tick>(a.engine.position());
  const Tick from = mm.barStartAt(pos), to = mm.nextBar(from);
  ops::addMarker(a.doc, pos - from <= to - pos ? from : to, ops::nextMarkerName(s));
}

// Split: the selected clips at the song position; with nothing selected nothing splits, as there.
void tracksSplitAtPlayhead(App& a) {
  if (!a.playlist.selected.empty()) splitClipsAtPlayhead(a);
}

const menudef::Menu& menudef::gathered(int which) {
  switch (which) {
    case 0: return navigateDef();
    case 1: return recordDef();
    case 2: return mixDef();
    default: return programDef();
  }
}

namespace {
// The Tracker bar shows each command once (K-106, one control each): the shared blocks lose the
// rows another Tracker menu already has (Song mode: Song; Metronome and the typing keyboard:
// Options' Recording and metronome; New pattern: Pattern; Length: the header's Rows).
bool trackerDrops(menustyle::Block b, const md::Entry& e) {
  using B = menustyle::Block;
  const std::string key = e.key ? e.key : "", label = e.label ? e.label : "";
  if (b == B::Options) return key == "transport.songMode" || label == "Song mode" || key == "options.metronome" || key == "options.typingKeyboard" || label == "Typing keyboard options";
  if (b == B::Add) return key == "pattern.new";
  if (b == B::Patterns) return e.kind == md::Kind::Submenu && label == "Length";  // T-26: the header's Rows is the one control
  if (b == B::Record) return e.kind != md::Kind::Submenu && key == "options.typingKeyboard";  // its options submenu has the same switch
  return false;
}
// T-26: the shortcuts the Tracker's menus do not show: the playlist's zoom keys (PgUp, PgDn and
// Home move the pattern's cursor here), Find empty's Shift/Ctrl+F4 (the pattern editor copies the
// track and the pattern with them), and F10 on MIDI settings (F10 opens the general page and, in
// the pattern editor, jumps rows).
bool trackerKeyless(const md::Entry& e) {
  const std::string key = e.key ? e.key : "", label = e.label ? e.label : "";
  return key == "playlist.zoomIn" || key == "playlist.zoomOut" || key == "playlist.zoomFit" || key == "pattern.findFirstEmpty" ||
         key == "pattern.findNextEmpty" || (key == "view.settings" && label == "MIDI settings...");
}
void trackerFilter(menustyle::Block b, const md::Menu& in, md::Menu& out) {
  for (const md::Entry& e : in) {
    if (trackerDrops(b, e)) continue;
    md::Entry x = e;
    if (x.kind == md::Kind::Item && trackerKeyless(x)) x.dynKey = [](App&) -> const char* { return nullptr; };
    if (x.kind == md::Kind::Submenu) {
      x.children.clear();
      trackerFilter(b, e.children, x.children);
    }
    out.push_back(std::move(x));
  }
}
const md::Menu* trackerBlock(menustyle::Block b, const md::Menu* def) {
  using B = menustyle::Block;
  static std::map<int, md::Menu> cache;
  if (!def || (b != B::Options && b != B::Add && b != B::Navigate && b != B::Patterns && b != B::Record)) return def;
  auto it = cache.find(static_cast<int>(b));
  if (it == cache.end()) {
    md::Menu out;
    trackerFilter(b, *def, out);
    it = cache.emplace(static_cast<int>(b), std::move(out)).first;
  }
  return &it->second;
}
}  // namespace

void drawStyleMenu(App& a, const menustyle::Menu& m, imhook::MenuWalker* w) {
  bool first = true;
  const bool tracker = layoutui::menuStyle(a) == layout::Preset::Tracker;
  for (const menustyle::Item& it : m.items) {
    const md::Menu* def = tracker ? trackerBlock(it.block, blockMenu(it.block)) : blockMenu(it.block);
    if (it.submenu) {
      if (!first) {
        if (w) w->separator(nullptr);
        else ImGui::Separator();
      }
      const char* lab = i18n::tr(it.submenu);
      if (w ? w->beginMenu(lab, true) : ImGui::BeginMenu(lab)) {
        if (def) md::emit(a, *def, w);
        if (w) w->endMenu();
        else ImGui::EndMenu();
      }
    } else if (def) {
      md::emit(a, *def, w);
    }
    first = false;
  }
}

}  // namespace gl
