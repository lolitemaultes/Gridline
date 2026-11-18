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
#include "LiveIo.h"
#include "LiveLook.h"
#include "UndoUi.h"
#include "Keys.h"
#include "LayoutPresets.h"
#include "HostedUi.h"
#include "LayoutShellUi.h"
#include "LogicPlugins.h"
#include "UpdateUi.h"
#include "LogicNewTracks.h"
#include "MenuDef.h"
#include "imgui_hooks/ImGuiItemHook.h"
#include "MidiUi.h"
#include "TrackerEditors.h"
#include "TrackerView.h"
#include "Windows.h"
#include "AudioRecordUi.h"
#include "FreezeUi.h"
#include "../core/MenuParity.h"
#include "../engine/ClipOps.h"
#include <set>

namespace gl {

namespace md = menudef;
using namespace menus;
using i18n::N_;

namespace {

bool notClips(App& a) { return !livelook::active(a); }  // N45-07 / N45-09
bool studioModes(App& a) { return notClips(a) && !logicplug::active(a); }  // the play modes: neither Clips nor Smooth

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
      // R02-15: as the reference's Navigate menu.
      md::item(N_("Go to position..."), "transport.goToPosition", [](App& a) { goToPositionPrompt(a); }).only([](App& a) { return logicplug::active(a); }),  // (the Tracks layout's Navigate)
      md::item(N_("Previous marker"), "transport.prevMarker", [](App& a) { gotoMarker(a, -1); }).only([](App& a) { return logicplug::active(a); }),
      md::item(N_("Next marker"), "transport.nextMarker", [](App& a) { gotoMarker(a, 1); }).only([](App& a) { return logicplug::active(a); }),
      md::separator(),
      md::item(N_("Cycle"), "transport.cycle", [](App& a) { toggleCycle(a); })
          .check([](App& a) { return a.playlist.selEnd > a.playlist.selStart; })
          .only([](App& a) { return !livelook::active(a); }),
      // K-112c checkpoint 56 (N55-04): Clips' loop brace, apart from the time selection, as the reference's.
      md::item(N_("Loop"), "transport.cycle", [](App& a) { livelook::toggleLoop(a); })
          .check([](App& a) { return livelook::loopOn(a); })
          .only([](App& a) { return livelook::active(a); }),
      md::item(N_("Loop selection"), "playlist.loopMarkers", [](App& a) { livelook::loopFromSelection(a); })
          .only([](App& a) { return livelook::active(a); }),
      md::item(N_("Set the cycle to the selected regions"), "transport.cycleSelection", [](App& a) { cycleToSelection(a); })
          .when([](App& a) { return !a.playlist.selected.empty(); }).only([](App& a) { return logicplug::active(a); }),
      // Checkpoint 47: the locators work with Cycle off too, as the reference's (Go To and Set Locators)
      md::item(N_("Set the locators to the selected regions"), [](App& a) { locatorsToSelection(a); })
          .when([](App& a) { return !a.playlist.selected.empty(); }).only([](App& a) { return logicplug::active(a); }),
      md::item(N_("Go to the left locator"), [](App& a) { gotoLocator(a, false); })
          .when([](App& a) { Tick l, r; return locators(a, l, r); }).only([](App& a) { return logicplug::active(a); }),
      md::item(N_("Go to the right locator"), [](App& a) { gotoLocator(a, true); })
          .when([](App& a) { Tick l, r; return locators(a, l, r); }).only([](App& a) { return logicplug::active(a); }),
      md::item(N_("Add marker"), "playlist.marker", [](App& a) { tracksAddMarker(a); }),
      md::separator(),  // K-112b: grouped as MAP.md 2.1: go to, scroll, zoom, then our play modes
      md::item(N_("Follow the song position"), [](App& a) { a.settings.set("ui.autoScroll", !a.settings.getBool("ui.autoScroll")); })
          .check([](App& a) { return a.settings.getBool("ui.autoScroll"); }),
      md::separator(),
      md::submenu(N_("Zoom"), {md::item(N_("Zoom in"), "playlist.zoomIn", [](App& a) { a.playlist.zoomRequest *= 1.5f; }),
                               md::item(N_("Zoom out"), "playlist.zoomOut", [](App& a) { a.playlist.zoomRequest /= 1.5f; }),
                               md::item(N_("Zoom to fit the song"), "playlist.zoomFit", [](App& a) { a.playlist.fitRequest = true; })})
          .only([](App& a) { return !logicplug::active(a); }),  // R04-09: zoom is in the Tracks layout's View menu
      // N45-07 / N45-09: Studio's play modes; Clips has none (its grid and arrangement are its modes),
      // nor has Smooth (R04-09).
      md::separator().only(notClips),
      md::item(N_("Song mode"), "transport.songMode", [](App& a) { a.engine.setMode(PlayMode::Song); })
          .check([](App& a) { return a.engine.mode() == PlayMode::Song; })
          .only(studioModes),
      md::item(N_("Pattern mode"),
               [](App& a) {
                 if (logicplug::active(a)) a.rackReveal.quietSwitch(PlayMode::Pattern);  // R02-22: no step sequencer pops up
                 a.engine.setMode(PlayMode::Pattern);
               }).check([](App& a) { return a.engine.mode() == PlayMode::Pattern; })
          .only(studioModes),
      md::item(N_("Live mode"), [](App& a) { a.engine.setMode(a.engine.mode() == PlayMode::Live ? PlayMode::Song : PlayMode::Live); })
          .check([](App& a) { return a.engine.mode() == PlayMode::Live; })
          .only(studioModes),
  };
  return m;
}

const md::Menu& recordDef() {
  static const md::Menu m = {
      md::item(N_("Record"), "transport.record", [](App& a) { a.recordArmed = !a.recordArmed; }).check([](App& a) { return a.recordArmed; }),
      md::separator(),
      md::toggle(N_("Metronome"), "options.metronome", [](App& a) { return &a.metronome; }),
      md::item(N_("Count-in"), "options.countIn", [](App& a) { a.countInBars = a.countInBars > 0 ? 0 : std::max(1, a.countInLength); }).check([](App& a) { return a.countInBars > 0; }),
      md::toggle(N_("Step record"), nullptr, [](App& a) { return &a.stepRecord; }),
      md::submenu(N_("Recording options"), {md::provider([](App& a) { recordOptionsMenu(a); })}),
      md::separator(),
      md::toggle(N_("Typing keyboard to piano"), "options.typingKeyboard", [](App& a) { return &a.typingKeyboard; }),
      md::submenu(N_("Typing keyboard options"), {md::provider([](App& a) { typingKeyboardMenu(a); })}),
      md::toggle(N_("Multilink to controllers"), nullptr, [](App& a) { return &a.multilink; }),
  };
  return m;
}

// N46-09 (round 04): Clips' recording submenu: each row once (Step record is in Recording options,
// so not again here); Session record joins the arrangement's Record.
const md::Menu& clipsRecordDef() {
  static const md::Menu m = {
      md::item(N_("Arrangement record"), "transport.record", [](App& a) { livelook::toggleArrangementRecord(a); }).check([](App& a) { return a.recordArmed; }),
      md::item(N_("Session record"), "transport.sessionRecord", [](App& a) { livelook::toggleSessionRecord(a); })
          .check([](App& a) { return livelook::sessionRecording(a); }),
      md::separator(),
      md::toggle(N_("Metronome"), "options.metronome", [](App& a) { return &a.metronome; }),
      md::item(N_("Count-in"), "options.countIn", [](App& a) { a.countInBars = a.countInBars > 0 ? 0 : std::max(1, a.countInLength); }).check([](App& a) { return a.countInBars > 0; }),
      md::submenu(N_("Recording options"), {md::provider([](App& a) { recordOptionsMenu(a); })}),
      md::separator(),
      md::toggle(N_("Typing keyboard to piano"), "options.typingKeyboard", [](App& a) { return &a.typingKeyboard; }),
      md::submenu(N_("Typing keyboard options"), {md::provider([](App& a) { typingKeyboardMenu(a); })}),
  };  // N49-12: no Multilink in Clips
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
      md::item(N_("Count-in"), "options.countIn", [](App& a) { a.countInBars = a.countInBars > 0 ? 0 : std::max(1, a.countInLength); }).check([](App& a) { return a.countInBars > 0; }),
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
      md::toggle(N_("Stage"), "view.stage", [](App& a) { return &a.showStage; }).only([](App& a) { return !logicplug::active(a); }),  // R05-18
      md::toggle(N_("Master chain"), nullptr, [](App& a) { return &a.showMaster; }),
      md::toggle(N_("Performance"), nullptr, [](App& a) { return &a.showPerformance; }),
      md::separator(),
      // R02-15: as the reference's Mix menu (guide: Mix > Show Automation, or A).
      md::item(N_("Show automation"), "view.automation", [](App&) { toggleAutomationLanes(); }).check([](App& a) { return automationLanesShown(a); }),
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
      md::item(N_("Rename track..."), "edit.rename", [](App& a) { menus::renameFocused(a); }),  // R04-09: Shift-Return
      md::item(N_("Mute track"), "track.mute", [](App& a) { tracksMuteSelected(a); }),
      md::item(N_("Solo track"), "track.solo", [](App& a) { tracksSoloSelected(a); }),
      md::separator(),
      // R04-09: the reference's track functions, where Gridline has them.
      md::item(N_("Create track stack"), [](App& a) { tracksCreateStack(a); })
          .when([](App& a) { return tracksCanStack(a); })
          .tip(N_("The selected tracks (neighbours, not in a stack yet) go into a new stack track above them")),
      md::item(N_("Freeze track"), [](App& a) { tracksFreeze(a); })
          .check([](App& a) { return tracksFrozen(a); })
          .when([](App& a) { return tracksCanFreeze(a); })
          .tip(N_("Renders the track's instrument or audio once and unloads its plugins; again: unfreeze")),
      md::item(N_("Bounce track in place"), [](App& a) { tracksBounceInPlace(a); })
          .tip(N_("Renders the selected tracks' regions to one audio region on a new track below; the originals are muted")),
      md::submenu(N_("Configure track header"), {md::provider([](App& a) { configureTrackHeaderItems(a); })}),
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
  // K-297 (round 8): the pane the menu opens takes the keys (they stayed in the tracks area).
  a.focusWindow = which == 0 ? "Piano roll" : which == 1 ? "Channel rack" : which == 2 ? "Mixer" : "Smart controls";
}

const md::Menu& windowDef() {
  static const md::Menu m = {
      // K-112B: the window keys (Cmd with a digit) as the reference's Window menu shows them; the
      // View menu keeps the single letters.
      md::item(N_("Main window"), "view.playlist", [](App& a) { a.showPlaylist = true, a.focusWindow = "Playlist"; })
          .keyText([](App&) { return keys::modLabel("view.playlist"); }),
      md::item(N_("Mixer"), "view.mixer", [](App& a) { showLower(a, 2); }).keyText([](App&) { return keys::modLabel("view.mixer"); }),
      md::item(N_("Step sequencer"), "view.channelRack", [](App& a) { showLower(a, 1); }),
      md::item(N_("Piano roll"), "view.pianoRoll", [](App& a) { showLower(a, 0); }).keyText([](App&) { return keys::modLabel("view.pianoRoll"); }),
      md::item(N_("Smart controls"), [](App& a) { showLower(a, 3); }),
      md::separator(),
      md::toggle(N_("Undo history"), "edit.history", [](App& a) { return &a.showHistory; }),
      md::toggle(N_("Performance"), nullptr, [](App& a) { return &a.showPerformance; }),
      md::toggle(N_("Typing keyboard to piano"), "options.typingKeyboard", [](App& a) { return &a.typingKeyboard; }),
      md::separator(),
      md::item(N_("Hide all plugin windows"), "view.hidePlugins",
               [](App& a) {
                 toggleHostedEditorsHidden();  // K-297: as the V key (installed plugins' windows too)
                 logicplug::toggleHidden(a);
               })
          .check([](App&) { return logicplug::hidden(); }),
  };
  return m;
}

// R04-09: the Tracks layout's View menu: its panes, with their single-letter keys (the commands
// run as their keys do, next frame).
void injectNext(const char* id) { keys::inject(id, ImGui::GetFrameCount() + 1); }

const md::Menu& tracksViewDef() {
  // (each item runs its command as the key does, next frame)
  static const md::Menu m = {
      md::item(N_("Library"), "view.library", [](App&) { injectNext("view.library"); }),
      md::item(N_("Inspector"), "view.inspector", [](App&) { injectNext("view.inspector"); }),
      md::item(N_("Quick help"), "view.quickHelp", [](App&) { injectNext("view.quickHelp"); }).check([](App& a) { return a.settings.getBool("ui.logicQuickHelp"); }),
      md::item(N_("Toolbar"), "view.toolbar", [](App&) { injectNext("view.toolbar"); }).check([](App& a) { return a.settings.getBool("ui.logicToolbar"); }),
      md::separator(),
      md::item(N_("Smart controls"), "view.smartControls", [](App&) { injectNext("view.smartControls"); }),
      md::item(N_("Editors"), "view.editors", [](App&) { injectNext("view.editors"); }),
      md::item(N_("List editors"), "view.lists", [](App&) { injectNext("view.lists"); }),
      md::item(N_("Notes"), "view.notes", [](App&) { injectNext("view.notes"); }),
      md::item(N_("Project files"), "view.files", [](App&) { injectNext("view.files"); }),
      md::separator(),
      md::item(N_("Global tracks"), "view.globalTracks", [](App&) { injectNext("view.globalTracks"); }).check([](App& a) { return a.settings.getBool("ui.logicGlobalTracks"); }),
      md::item(N_("Show automation"), "view.automation", [](App&) { toggleAutomationLanes(); }).check([](App& a) { return automationLanesShown(a); }),
      md::separator(),
      md::item(N_("Zoom in"), "playlist.zoomIn", [](App& a) { a.playlist.zoomRequest *= 1.5f; }),
      md::item(N_("Zoom out"), "playlist.zoomOut", [](App& a) { a.playlist.zoomRequest /= 1.5f; }),
      md::item(N_("Zoom to fit the song"), "playlist.zoomFit", [](App& a) { a.playlist.fitRequest = true; }),
  };
  return m;
}

// R04-09: the Tracks layout's File additions: the reference's Close items and its Bounce submenu
// (the whole song or the cycle area through Render, regions and tracks rendered in place).
const md::Menu& tracksFileDef() {
  static const md::Menu m = {
      md::separator(),
      md::item(N_("Close window"), "window.close", [](App&) { closeFrontWindow(false); }),  // K-297's shared close (Cmd+W)
      md::item(N_("Close project"), [](App& a) { a.later([&a] { a.closeProject(); }); })
          .tip(N_("Closes the project (asking to save changes) and starts an empty one")),
      md::submenu(N_("Bounce"),
                  {md::item(N_("Project or section..."), "file.render", [](App& a) { exportui::openRender(a, exportui::Preset::Audio); }),
                   md::item(N_("Regions in place"), [](App& a) { regionsBounceInPlace(a); })
                       .when([](App& a) { return !a.playlist.selected.empty(); })
                       .tip(N_("Renders the selected regions to one audio region in their place; the originals go")),
                   md::item(N_("Tracks in place"), [](App& a) { tracksBounceInPlace(a); })
                       .tip(N_("Renders the selected tracks' regions to one audio region on a new track below; the originals are muted"))}),
  };
  return m;
}

// K-112c (round 45): Create in the Clips layout, as the reference's (MAP.md 2.4): new tracks first,
// then a marker, then the imports; Studio's Add follows in a submenu (MenuStyle.cpp).

// Checkpoint 49 (N47-13): Studio's Add is no submenu here; its effect row (on the selected track's
// mixer track) and its automation clip row join the tracks and the clips. Its pattern, mixer track
// and "8 tracks" rows are Insert MIDI clip, New return track and the new track rows here.
const md::Entry& addRow(md::Kind kind) {
  for (const md::Entry& e : md::studio(2))
    if (e.kind == kind && !e.label) return e;  // the effect submenu (its label is made when shown), the automation provider
  static const md::Entry none = md::separator();
  return none;
}

const md::Menu& clipsCreateDef() {
  static const md::Menu m = {
      md::item(N_("New audio track"), "track.newAudio", [](App& a) { a.later([&a] { livelook::newTrack(a, true); }); })
          .tip(N_("A track for audio clips and recording, on a mixer track of its own, after the selected track")),
      md::item(N_("New MIDI track"), "track.newMidi", [](App& a) { a.later([&a] { livelook::newTrack(a, false); }); })
          .tip(N_("An empty track for pattern clips, after the selected track")),
      md::submenu(N_("New instrument track"), {md::provider([](App& a) { addChannelMenu(a); })}),
      // C44-16: a return track (every shown track sends to it, at zero), scenes and clips of the grid.
      md::item(N_("New return track"), "track.newReturn", [](App& a) { livelook::newReturnTrack(a); })
          .tip(N_("A mixer track the shown tracks send to (their sends start at zero)")),
      addRow(md::Kind::Submenu),
      md::separator(),
      md::item(N_("Insert scene"), [](App& a) { livelook::gridInsertScene(a); })
          .tip(N_("An empty row of slots under the selected slot's scene")),
      md::item(N_("Capture and insert scene"), "clips.captureScene", [](App& a) { livelook::gridCaptureScene(a); })
          .tip(N_("A new scene under the selected one with the clips playing now, which play on from it")),
      md::item(N_("Insert MIDI clip"), "clips.insertMidiClip", [](App& a) { livelook::gridInsertMidiClip(a); })
          .tip(N_("A new one-bar MIDI clip in the selected slot of a MIDI track")),
      addRow(md::Kind::Provider),
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
      // Checkpoint 48: each band shows or hides on its own; both shown stack, the clip band over the
      // device band (refs 456, 466). Shift+Tab swaps one for the other.
      md::item(N_("Clip band"), [](App& a) { layoutui::setRegionVisible(a, "live.clip", !bandShown(a, "live.clip"), false); })
          .check([](App& a) { return bandShown(a, "live.clip"); }),
      md::item(N_("Device band"), [](App& a) { layoutui::setRegionVisible(a, "live.device", !bandShown(a, "live.device"), false); })
          .check([](App& a) { return bandShown(a, "live.device"); }),
      md::item(N_("Clip and device bands"), "view.detail", [](App& a) { livelook::toggleDetail(a); })
          .check([](App& a) { return bandShown(a, "live.clip") || bandShown(a, "live.device"); }),
      md::toggle(N_("Mixer"), "view.mixer", [](App& a) { return &a.showMixer; }),
      // Checkpoint 56 (N55-10): In/Out and Info View are in the View menu, as the reference's.
      md::separator(),
  };
  return m;
}

const md::Menu& programDef() {
  static const md::Menu m = {
      md::item(N_("About Gridline"), [](App& a) { trackerview::openAbout(a); }),
      md::item(N_("Check for updates..."), [](App& a) { updui::checkNow(a); }).only([](App&) { return updui::started(); }),  // R02-22
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
      // TRK-1118: the selected instrument's own editor in a window (an installed plugin's window, a built-in's
      // editor floating), as the instrument list's menu and the Channel frame's button open it.
      md::item(N_("Open plugin window"), [](App& a) { trackereditors::openPluginWindow(a, a.selChannel); })
          .when([](App& a) { return a.doc.song().channel(a.selChannel) != nullptr; }),
      md::item(N_("Audio editor"), "view.audioEditor", [](App& a) { a.settings.set("ui.loupePinned", !a.settings.getBool("ui.loupePinned")); })
          .check([](App& a) { return a.settings.getBool("ui.loupePinned"); }),
      md::item(N_("Files"), "view.files", [](App&) { trackerview::setHeaderSwap(trackerview::headerSwap() == 2 ? 0 : 2); })
          .check([](App&) { return trackerview::headerSwap() == 2; }),
      md::item(N_("Tracker help"), "help.trackerKeys", [](App&) { trackerview::setHeaderSwap(trackerview::headerSwap() == 1 ? 0 : 1); })
          .check([](App&) { return trackerview::headerSwap() == 1; }),
      md::separator(),
      md::item(N_("Header band"), "view.headerBand", [](App& a) { trackerview::setExtended(a, !trackerview::extended()); })
          .check([](App&) { return !trackerview::extended(); }),
      md::item(N_("Transpose box"), "view.transposeBox", [](App& a) { trackerview::toggleScopeTool(a, 1); })
          .check([](App&) { return trackerview::scopeTool() == 1; }),
      md::item(N_("Advanced edit"), "view.advancedEdit", [](App& a) { trackerview::toggleScopeTool(a, 3); })
          .check([](App&) { return trackerview::scopeTool() == 3; }),
      md::item(N_("Clean up box"), "view.cleanUp", [](App& a) { trackerview::toggleScopeTool(a, 2); })
          .check([](App&) { return trackerview::scopeTool() == 2; }),
      md::item(N_("MIDI box"), "view.midiBox", [](App& a) { trackerview::toggleScopeTool(a, 4); })
          .check([](App&) { return trackerview::scopeTool() == 4; }),
      md::item(N_("Sample tools box"), "view.sampleTools", [](App& a) { trackerview::toggleScopeTool(a, 5); })
          .check([](App&) { return trackerview::scopeTool() == 5; }),
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
      md::item(nullptr, "edit.undo", [](App& a) { undoui::undo(a); })  // (N49-06: waits while Clips records)
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
    case B::TracksView: return &tracksViewDef();
    case B::TrackerEdit: return &trackerEditDef();
    case B::TrackerPattern: return &trackerPatternDef();
    case B::TrackerView: return &trackerViewDef();
    case B::ClipsCreate: return &clipsCreateDef();
    case B::ClipsNavigate: return &clipsNavigateDef();
    case B::ClipsRecord: return &clipsRecordDef();
    case B::TracksFile: return &tracksFileDef();
    case B::ClipsFile: return &md::clipsFile();
    case B::ClipsEdit: return &md::clipsEdit();
    default: return nullptr;
  }
}

}  // namespace

// K-112B (checkpoint 39), as the reference's Track menu behaves: a new track right below the current
// one with its settings (name, colour, height) and none of its clips.
namespace {
std::vector<int> selectedTracks(const App& a) {
  std::vector<int> v(a.playlist.trackSel.begin(), a.playlist.trackSel.end());
  if (v.empty())
    if (const int f = logicplug::focusTrack(a); f >= 0) v.push_back(f);
  const int n = static_cast<int>(a.doc.song().tracks.size());
  v.erase(std::remove_if(v.begin(), v.end(), [n](int t) { return t < 0 || t >= n; }), v.end());
  return v;
}
}  // namespace

void tracksMuteSelected(App& a) {
  const std::vector<int> v = selectedTracks(a);
  if (v.empty()) return;
  const bool to = !a.doc.song().tracks[static_cast<size_t>(v.front())].muted;
  a.doc.editSong(to ? "mute track" : "unmute track", [&](Song& x) {
    for (const int t : v) x.tracks[static_cast<size_t>(t)].muted = to;
  });
}

void tracksSoloSelected(App& a) {
  // R05-03: as the reference's S (Toggle Channel Strip Solo): solo is the track's channel strip's own
  // state. Other tracks go quiet without being muted, so their M stays as the person set it, and
  // ending the solo leaves every mute as it was.
  const std::vector<int> v = selectedTracks(a);
  const Song& s = a.doc.song();
  std::vector<ObjectId> strips;
  for (const int t : v)
    if (const MixerTrack* m = logicplug::trackStrip(s, t); m && std::find(strips.begin(), strips.end(), m->id) == strips.end()) strips.push_back(m->id);
  if (strips.empty()) {
    a.setHint(i18n::tr("This track has no channel strip to solo yet"));
    return;
  }
  bool all = true;
  for (const ObjectId id : strips) all = all && s.mixerTrack(id)->solo;
  const bool to = !all;
  a.doc.editSong(to ? "solo track" : "unsolo track", [&](Song& x) {
    for (MixerTrack& m : x.mixer)
      if (std::find(strips.begin(), strips.end(), m.id) != strips.end()) m.solo = to, m.soloAlone = false;
  });
}

void configureTrackHeaderItems(App& a) {
  struct Part {
    int bit;
    const char* name;
  };
  static const Part parts[] = {{kHeadIcon, N_("Icon")},           {kHeadPower, N_("On/off button")}, {kHeadMuteSolo, N_("Mute and solo buttons")},
                               {kHeadRecord, N_("Record button")}, {kHeadVolume, N_("Volume slider")}, {kHeadPan, N_("Pan knob")}};
  const int hide = static_cast<int>(a.settings.getNumber("ui.logicHeaderHide"));
  for (const Part& pt : parts)
    if (ImGui::MenuItem(i18n::tr(pt.name), nullptr, (hide & pt.bit) == 0)) a.settings.set("ui.logicHeaderHide", hide ^ pt.bit);
}

bool trackHeaderShows(App& a, int part) { return (static_cast<int>(a.settings.getNumber("ui.logicHeaderHide")) & part) == 0; }

namespace {
// The selected tracks when they are one run of neighbours outside any stack, sorted; else empty.
std::vector<int> stackRun(App& a) {
  std::vector<int> v = selectedTracks(a);
  std::sort(v.begin(), v.end());
  const Song& s = a.doc.song();
  if (v.empty() || static_cast<int>(s.tracks.size()) >= kMaxPlaylistTracks) return {};
  for (size_t k = 0; k < v.size(); ++k)
    if ((k > 0 && v[k] != v[k - 1] + 1) || ops::trackGroupOf(s, v[k]).first >= 0) return {};
  return v;
}
std::vector<freeze::Unit> frozenTargets(App& a) {
  std::vector<freeze::Unit> out;
  for (const int t : selectedTracks(a)) {
    freeze::Unit u;
    if (freezeui::trackUnit(a.doc.song(), t, u)) out.push_back(u);
    else if (const Channel* ch = logicplug::trackSound(a.doc.song(), t, nullptr)) out.push_back(freeze::channelUnit(ch->id));  // a free track: the sound it plays
  }
  return out;
}
}  // namespace

bool tracksCanStack(App& a) { return !stackRun(a).empty(); }

// As the reference's Create Track Stack: a new track above the selected ones holds them (Gridline's
// track group: the head folds and colours them), one undo step, the new stack selected.
void tracksCreateStack(App& a) {
  const std::vector<int> v = stackRun(a);
  if (v.empty()) {
    a.setHint(i18n::tr("Select neighbouring tracks that are not in a stack yet"));
    return;
  }
  const int at = v.front(), n = static_cast<int>(v.size());
  a.later([&a, at, n] {
    a.doc.editSong("create track stack", [&](Song& x) {
      PlaylistTrack head;
      head.name = "Track stack";
      head.groupSize = n;
      x.tracks.insert(x.tracks.begin() + at, head);
      for (Clip& c : x.playlist)
        if (c.track >= at) ++c.track;
    });
    a.playlist.trackSel = {at};
  });
}

bool tracksCanFreeze(App& a) { return !frozenTargets(a).empty(); }
bool tracksFrozen(App& a) {
  const std::vector<freeze::Unit> u = frozenTargets(a);
  return !u.empty() && freeze::frozen(a.doc.song(), u.front());
}
void tracksFreeze(App& a) {
  const std::vector<freeze::Unit> u = frozenTargets(a);
  if (u.empty()) {
    a.setHint(i18n::tr("This track has no instrument or audio of its own to freeze"));
    return;
  }
  if (freeze::frozen(a.doc.song(), u.front())) a.later([&a, u] { freezeui::unfreezeUnits(a, u); });
  else a.later([&a, u] { freezeui::freezeUnits(a, u); });
}

void tracksBounceInPlace(App& a) {
  std::vector<int> v = selectedTracks(a);
  std::sort(v.rbegin(), v.rend());  // from the last: a bounce below one track never moves another's
  const std::string dir = renderedDir(a);
  const double sr = a.engine.sampleRate();
  a.later([&a, v, dir, sr] {
    int done = 0;
    a.doc.beginGroup("bounce tracks in place");
    for (const int t : v) {
      ObjectId nid = kNoId;
      a.doc.editSong("bounce track", [&](Song& x) { nid = bounceTrack(x, t, dir, sr); });
      if (nid != kNoId) ++done;
    }
    a.doc.endGroup();
    a.notify(done ? i18n::tr("Bounced in place: the original regions are muted") : i18n::tr("Nothing to bounce on the selected tracks"), done == 0);
  });
}

void regionsBounceInPlace(App& a) {
  std::set<ObjectId> sel;
  for (const ObjectId id : a.playlist.selected)
    if (const Clip* c = a.doc.song().clip(id); c && !logicplug::laneClip(a.doc.song(), *c)) sel.insert(id);
  if (sel.empty()) {
    a.setHint(i18n::tr("Select the regions to bounce first"));
    return;
  }
  const std::string dir = renderedDir(a);
  const double sr = a.engine.sampleRate();
  a.later([&a, sel, dir, sr] {
    ObjectId nid = kNoId;
    a.doc.editSong("bounce regions in place", [&](Song& x) { nid = consolidateClips(x, sel, dir, sr, true); });
    if (nid != kNoId) a.playlist.selected = {nid};
    else a.notify(i18n::tr("Nothing to render"), true);
  });
}

void tracksNewSameSettings(App& a) {
  if (static_cast<int>(a.doc.song().tracks.size()) >= kMaxPlaylistTracks) return;
  const int at = currentTrack(a);
  a.later([&a, at] { logicplug::newTrackSameSettings(a, at, false); });  // R02-14: with its own channel strip
}

// Every selected track goes (one undo step), the current one when none is selected; one track always stays.
void tracksDeleteSelected(App& a) {
  std::vector<int> del(a.playlist.trackSel.begin(), a.playlist.trackSel.end());
  if (del.empty()) del.push_back(currentTrack(a));
  const bool lanes = logicplug::active(a);  // R04-13: the Tracks layout's track automation goes with its track
  a.later([&a, del, lanes] {
    a.doc.beginGroup("delete tracks");
    for (auto it = del.rbegin(); it != del.rend(); ++it)
      if (a.doc.song().tracks.size() > 1) {
        if (lanes) logicplug::deleteTrack(a, *it);  // R06-06: with its strip and the sounds only it used
        else ops::deletePlaylistTrack(a.doc, *it, false);
      }
    a.doc.endGroup();
    // R02-14: a track is always selected: the one that took the first deleted one's place.
    const int n = static_cast<int>(a.doc.song().tracks.size());
    a.playlist.trackSel.clear();
    if (n > 0) a.playlist.trackSel.insert(std::min(*std::min_element(del.begin(), del.end()), n - 1));
  });
}

// A marker at the bar nearest the song position (the reference rounds a new marker to the nearest bar).
void tracksAddMarker(App& a) {
  const Song& s = a.doc.song();
  const MeterMap mm(s);
  const Tick pos = static_cast<Tick>(a.engine.position());
  const Tick from = mm.barStartAt(pos), to = mm.nextBar(from);
  const Tick at = pos - from <= to - pos ? from : to;
  for (const Marker& m : s.markers)  // R02-16: one marker per position; a second would hide the first's name
    if (m.pos == at) {
      a.setHint(i18n::tr("A marker is already at this bar: ") + m.name);
      return;
    }
  ops::addMarker(a.doc, at, ops::nextMarkerName(s));
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
// Options' Recording and metronome; New pattern: Pattern; Length: the header's Rows). K-314 (C-04): the
// Metronome stays in Recording and metronome (the header's button is its other control).
bool trackerDrops(menustyle::Block b, const md::Entry& e) {
  using B = menustyle::Block;
  const std::string key = e.key ? e.key : "", label = e.label ? e.label : "";
  if (b == B::Options) return key == "transport.songMode" || label == "Song mode" || key == "options.metronome" || key == "options.typingKeyboard" || label == "Typing keyboard options";
  if (b == B::Add) return key == "pattern.new";
  if (b == B::Patterns) return e.kind == md::Kind::Submenu && label == "Length";  // T-26: the header's Rows is the one control
  if (b == B::Record)  // its options submenu has the same switch
    return e.kind != md::Kind::Submenu && key == "options.typingKeyboard";
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
    // TRK-1118: Clone is the Tracker's Duplicate pattern (shown at once, one undo step), with a second
    // row that also puts the copy in the song after the current entry.
    const bool clone = b == menustyle::Block::Patterns && x.kind == md::Kind::Item && x.key && std::string(x.key) == "pattern.clone";
    if (clone) {
      x.label = N_("Duplicate pattern");
      x.run = [](App& a) { trackerview::duplicatePattern(a, false); };
    }
    out.push_back(std::move(x));
    if (clone) {
      out.push_back(md::item(N_("Duplicate into the song"), [](App& a) { trackerview::duplicatePattern(a, true); })
                         .when([](App& a) { return a.doc.song().pattern(a.curPattern) != nullptr; }));
    }
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
  // K-112D (audit I1): Smooth's menus have the reference's roomy rows, about 24 px a row.
  const bool roomy = !w && logicplug::active(a);
  if (roomy) ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 10.0f));
  // K-112D (audit I4): the separators a faint hairline, as the reference's (white at about 8 %).
  if (roomy) ImGui::PushStyleColor(ImGuiCol_Separator, ImVec4(1.0f, 1.0f, 1.0f, 0.08f));
  struct PopRoomy {
    bool on;
    ~PopRoomy() {
      if (on) ImGui::PopStyleVar(), ImGui::PopStyleColor();
    }
  } popRoomy{roomy};
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
