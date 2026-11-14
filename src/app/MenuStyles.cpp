// Gridline - menu bar styles of the layout presets (K-79, core/MenuStyle.h):
// the items each block draws. The studio blocks are the studio menus
// (Menus.cpp) as they are; the gathered blocks only call existing commands.
#include <algorithm>
#include <string>
#include "../../third_party/imgui/imgui.h"
#include "../core/I18n.h"
#include "../core/MenuStyle.h"
#include "../core/Meter.h"
#include "App.h"
#include "Keys.h"
#include "LayoutPresets.h"
#include "MidiUi.h"
#include "Windows.h"

namespace gl {

namespace {

void navigateItems(App& a) {
  const Tick bar = std::max<Tick>(1, a.doc.song().barTicks());
  const Tick pos = static_cast<Tick>(a.engine.position());
  const bool songBars = a.engine.mode() != PlayMode::Pattern;  // K-193: real bars of the song's meter map (as the top bar)
  if (ImGui::MenuItem(i18n::tr("Go to the start"), keys::label("transport.toStart"))) a.engine.setPosition(0);
  if (ImGui::MenuItem(i18n::tr("Back one bar"), keys::label("transport.rewindBar")))
    a.engine.setPosition(std::max<Tick>(0, songBars ? MeterMap(a.doc.song()).barStartAt(std::max<Tick>(0, pos - 1)) : (pos - 1) / bar * bar));
  if (ImGui::MenuItem(i18n::tr("Forward one bar"), keys::label("transport.forwardBar")))
    a.engine.setPosition(songBars ? MeterMap(a.doc.song()).nextBar(pos) : (pos / bar + 1) * bar);
  ImGui::Separator();
  const bool live = a.engine.mode() == PlayMode::Live;
  if (ImGui::MenuItem(i18n::tr("Song mode"), keys::label("transport.songMode"), a.engine.mode() == PlayMode::Song)) a.engine.setMode(PlayMode::Song);
  if (ImGui::MenuItem(i18n::tr("Pattern mode"), nullptr, a.engine.mode() == PlayMode::Pattern)) a.engine.setMode(PlayMode::Pattern);
  if (ImGui::MenuItem(i18n::tr("Live mode"), nullptr, live)) a.engine.setMode(live ? PlayMode::Song : PlayMode::Live);
  ImGui::Separator();
  if (ImGui::MenuItem(i18n::tr("Zoom in"), keys::label("playlist.zoomIn"))) a.playlist.zoomRequest *= 1.5f;
  if (ImGui::MenuItem(i18n::tr("Zoom out"), keys::label("playlist.zoomOut"))) a.playlist.zoomRequest /= 1.5f;
  if (ImGui::MenuItem(i18n::tr("Zoom to fit the song"), keys::label("playlist.zoomFit"))) a.playlist.fitRequest = true;
  bool follow = a.settings.getBool("ui.autoScroll");
  if (ImGui::MenuItem(i18n::tr("Follow the song position"), nullptr, follow)) a.settings.set("ui.autoScroll", !follow);
}

void recordItems(App& a) {
  if (ImGui::MenuItem(i18n::tr("Record"), keys::label("transport.record"), a.recordArmed)) a.recordArmed = !a.recordArmed;
  ImGui::Separator();
  ImGui::MenuItem(i18n::tr("Metronome"), keys::label("options.metronome"), &a.metronome);
  if (ImGui::MenuItem(i18n::tr("Count-in"), keys::label("options.countIn"), a.countInBars > 0)) a.countInBars = a.countInBars > 0 ? 0 : 1;
  ImGui::MenuItem(i18n::tr("Step record"), nullptr, &a.stepRecord);
  if (ImGui::BeginMenu(i18n::tr("Recording options"))) {
    recordOptionsMenu(a);
    ImGui::EndMenu();
  }
  ImGui::Separator();
  ImGui::MenuItem(i18n::tr("Typing keyboard to piano"), keys::label("options.typingKeyboard"), &a.typingKeyboard);
  if (ImGui::BeginMenu(i18n::tr("Typing keyboard options"))) {
    typingKeyboardMenu(a);
    ImGui::EndMenu();
  }
  ImGui::MenuItem(i18n::tr("Multilink to controllers"), nullptr, &a.multilink);
}

void mixItems(App& a) {
  ImGui::MenuItem(i18n::tr("Mixer"), keys::label("view.mixer"), &a.showMixer);
  ImGui::MenuItem(i18n::tr("Stage"), keys::label("view.stage"), &a.showStage);
  ImGui::MenuItem(i18n::tr("Master chain"), nullptr, &a.showMaster);
  ImGui::MenuItem(i18n::tr("Performance"), nullptr, &a.showPerformance);
  ImGui::Separator();
  if (ImGui::BeginMenu(i18n::tr("Mixer layout"))) {
    const int cur = static_cast<int>(a.settings.getNumber("ui.mixerLayout"));
    const char* names[] = {i18n::N_("Compact strips"), i18n::N_("Normal strips"), i18n::N_("Wide strips")};
    for (int i = 0; i < 3; ++i)
      if (ImGui::MenuItem(i18n::tr(names[i]), nullptr, cur == i)) a.settings.set("ui.mixerLayout", i);
    ImGui::EndMenu();
  }
}

void programItems(App& a) {
  if (ImGui::MenuItem(i18n::tr("About Gridline"))) a.showAbout = true;
  ImGui::Separator();
  if (ImGui::MenuItem(i18n::tr("Settings..."), keys::label("view.settings"))) {
    a.showSettings = true;
    a.focusWindow = "Settings";
  }
  if (ImGui::MenuItem(i18n::tr("Keyboard shortcuts..."), keys::label("help.shortcuts"))) a.showShortcuts = true;
  if (ImGui::MenuItem(i18n::tr("Plugin manager..."))) a.showPluginManager = true;
  if (ImGui::MenuItem(i18n::tr("Welcome screen"))) a.home.showHome(a.now);
  ImGui::Separator();
  if (ImGui::MenuItem(i18n::tr("Quit"), keys::label("file.quit"))) a.quitRequested = true;
}

void block(App& a, menustyle::Block b) {
  using B = menustyle::Block;
  switch (b) {
    case B::File: menuFile(a); break;
    case B::Edit: menuEdit(a); break;
    case B::Add: menuAdd(a); break;
    case B::Patterns: menuPatterns(a); break;
    case B::View: menuView(a); break;  // its Layout section holds the presets (K-79)
    case B::Options: menuOptions(a); break;
    case B::Tools: menuTools(a); break;
    case B::Help: menuHelp(a); break;
    case B::Navigate: navigateItems(a); break;
    case B::Record: recordItems(a); break;
    case B::Mix: mixItems(a); break;
    case B::Program: programItems(a); break;
    default: break;
  }
}

}  // namespace

void drawStyleMenu(App& a, const menustyle::Menu& m) {
  bool first = true;
  for (const menustyle::Item& it : m.items) {
    if (it.submenu) {
      if (!first) ImGui::Separator();
      if (ImGui::BeginMenu(i18n::tr(it.submenu))) {
        block(a, it.block);
        ImGui::EndMenu();
      }
    } else {
      block(a, it.block);
    }
    first = false;
  }
}

}  // namespace gl
