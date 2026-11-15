// Gridline - menu bar styles of the layout presets (K-79, core/MenuStyle.h):
// the menu definition (MenuDef.h) each block stands for. The studio blocks are
// the studio menus (Menus.cpp); the gathered blocks only call existing commands.
#include <algorithm>
#include <string>
#include "../../third_party/imgui/imgui.h"
#include "../core/I18n.h"
#include "../core/MenuOps.h"
#include "../core/MenuStyle.h"
#include "../core/Meter.h"
#include "App.h"
#include "Keys.h"
#include "LayoutPresets.h"
#include "LayoutShellUi.h"
#include "LogicPlugins.h"
#include "LogicNewTracks.h"
#include "MenuDef.h"
#include "imgui_hooks/ImGuiItemHook.h"
#include "MidiUi.h"
#include "Windows.h"

namespace gl {

namespace md = menudef;
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
      md::item(N_("New tracks..."), [](App&) { newtracks::open(); }),
      md::item(N_("New track"), [](App& a) { a.later([&a] { ops::insertPlaylistTrack(a.doc, currentTrack(a) + 1); }); })
          .when([](App& a) { return static_cast<int>(a.doc.song().tracks.size()) < kMaxPlaylistTracks; }),
      md::submenu(N_("New instrument track"), {md::provider([](App& a) { addChannelMenu(a); })}),
      md::item(N_("New track with the same settings"), [](App& a) { a.later([&a] { ops::clonePlaylistTrack(a.doc, currentTrack(a)); }); })
          .when([](App& a) { return static_cast<int>(a.doc.song().tracks.size()) < kMaxPlaylistTracks; }),
      md::item(N_("New mixer track"),
               [](App& a) {
                 const ObjectId id = a.doc.addMixerTrack();
                 if (id != kNoId) a.selMixer = id;
               }),
      md::separator(),
      md::item(N_("Delete track"), [](App& a) { a.later([&a] { ops::deletePlaylistTrack(a.doc, currentTrack(a)); }); })
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
      md::item(N_("Mixer"), [](App& a) { showLower(a, 2); }),
      md::item(N_("Step sequencer"), [](App& a) { showLower(a, 1); }),
      md::item(N_("Piano roll"), [](App& a) { showLower(a, 0); }),
      md::item(N_("Smart controls"), [](App& a) { showLower(a, 3); }),
      md::separator(),
      md::toggle(N_("Undo history"), nullptr, [](App& a) { return &a.showHistory; }),
      md::toggle(N_("Performance"), nullptr, [](App& a) { return &a.showPerformance; }),
      md::toggle(N_("Typing keyboard to piano"), "options.typingKeyboard", [](App& a) { return &a.typingKeyboard; }),
      md::separator(),
      md::item(N_("Hide all plugin windows"), "view.hidePlugins", [](App& a) { logicplug::toggleHidden(a); })
          .check([](App&) { return logicplug::hidden(); }),
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
    default: return nullptr;
  }
}

}  // namespace

const menudef::Menu& menudef::gathered(int which) {
  switch (which) {
    case 0: return navigateDef();
    case 1: return recordDef();
    case 2: return mixDef();
    default: return programDef();
  }
}

void drawStyleMenu(App& a, const menustyle::Menu& m, imhook::MenuWalker* w) {
  bool first = true;
  for (const menustyle::Item& it : m.items) {
    const md::Menu* def = blockMenu(it.block);
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
