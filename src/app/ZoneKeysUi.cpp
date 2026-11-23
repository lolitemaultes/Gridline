// Gridline - Studio's zone keys in the app. See ZoneKeysUi.h.
#include "ZoneKeysUi.h"
#include <SDL.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "../../third_party/imgui/imgui.h"
#include "../../third_party/imgui/imgui_internal.h"
#include "../core/Keymap.h"
#include "../core/Settings.h"
#include "App.h"
#include "CommandPaletteUi.h"
#include "Keys.h"
#include "LayoutPresets.h"
#include "LayoutShellUi.h"
#include "WindowRaise.h"

namespace gl::zonekeysui {

namespace {

zones::Layer gLayer;
zones::History gHistory;
zones::Action gPending;      // the jump asked for by the last keys (done in frame())
bool gTabSwallowed = false;  // the layer took this Tab press (its key-up goes to the layer too)

const char* stableOf(const ImGuiWindow* w) {
  if (!w) return "";
  const char* id = std::strstr(w->Name, "###");
  return id ? id + 3 : w->Name;
}

// The zone a window belongs to (0: none) and its place in it.
int zoneOf(const zones::Zones& z, const std::string& window, int* place = nullptr) {
  for (int k = 0; k < zones::kZones; ++k)
    for (int i = 0; i < 2; ++i)
      if (!window.empty() && z[static_cast<size_t>(k)].windows[static_cast<size_t>(i)] == window) {
        if (place) *place = i;
        return k + 1;
      }
  return 0;
}

std::string focusedWindow() {
  const ImGuiWindow* nav = GImGui ? GImGui->NavWindow : nullptr;
  return nav ? stableOf(nav->RootWindowDockTree ? nav->RootWindow : nav) : std::string();
}

// The zone's window shown and given the keys (opened when closed; a hidden tab comes forward).
bool show(App& a, const std::string& window) {
  bool* f = layoutui::windowOpenFlag(a, window.c_str());
  if (!f) return false;
  if (!*f) *f = true;
  else ui::bringTabForward(window.c_str());
  a.focusWindow = window;
  return true;
}

void jump(App& a, int zone, bool record) {
  const zones::Zones z = load(a);
  if (zone < 1 || zone > zones::kZones) return;
  const zones::Zone& zn = z[static_cast<size_t>(zone - 1)];
  int here = 0;
  const bool there = zoneOf(z, focusedWindow(), &here) == zone;
  const int pick = zones::pick(zn, there, here);
  if (pick < 0) return;
  const std::string& w = zn.windows[static_cast<size_t>(pick)];
  if (!show(a, w)) return;
  if (record && w != "Browser") gHistory.visit(zone);  // the browser is a stop on the way, not a place
  std::printf("zones jump %d %s\n", zone, w.c_str());
  std::fflush(stdout);
}

}  // namespace

bool enabled(const App& a) { return a.settings.getBool("studio.zoneKeys"); }

bool keySetTakesTab() {
  const keys::Profile p = keys::keymap().profile();
  if (p == keys::Profile::Live || p == keys::Profile::Tracker) return true;  // the view swap, the next track
  // A command of the whole window on plain Tab (a view's own Tab, as the Stage's next track, is left to
  // that view: zone keys are off while it has the keys).
  for (const auto& [id, chords] : keys::profileDefaults(p, false)) {
    const keys::Command* cmd = keys::findCommand(id);
    if (!cmd || cmd->context != keys::Context::Global) continue;
    for (const keys::Chord& c : chords)
      if (c.key == keys::Key::Tab && c.mods == 0) return true;
  }
  return false;
}

bool live(App& a) {
  if (!enabled(a) || layoutui::active() != layout::Preset::Studio || keySetTakesTab()) return false;
  if (!GImGui) return false;
  const ImGuiIO& io = ImGui::GetIO();
  if (io.WantTextInput || GImGui->OpenPopupStack.Size > 0 || ImGui::GetTopMostPopupModal()) return false;
  if (cmdpalette::isOpen() || a.home.visible()) return false;
  if (a.showStage && keys::focusContext() == keys::Context::Stage) return false;  // the Stage keeps its Tab
  return true;
}

bool sdlEvent(App& a, const SDL_Event& e, unsigned mainWindow) {
  if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
    gLayer.cancel();
    gTabSwallowed = false;
    return false;
  }
  if (e.type != SDL_KEYDOWN && e.type != SDL_KEYUP) return false;
  // A plugin's own window (or another of Gridline's windows) has the keys: not ours.
  if (e.key.windowID != 0 && mainWindow != 0 && e.key.windowID != mainWindow) return false;
  const bool down = e.type == SDL_KEYDOWN;
  const SDL_Keycode k = e.key.keysym.sym;
  const bool mods = (e.key.keysym.mod & (KMOD_CTRL | KMOD_GUI | KMOD_ALT | KMOD_SHIFT)) != 0;
  if (k == SDLK_TAB) {
    if (down) {
      if (!live(a) || mods) {
        gLayer.cancel();
        return false;
      }
      gLayer.tabDown(e.key.repeat != 0);
      gTabSwallowed = true;
      return true;
    }
    if (!gTabSwallowed) return false;
    gTabSwallowed = false;
    const zones::Action act = gLayer.tabUp();
    if (act.kind != zones::Action::None) gPending = act;
    return true;
  }
  if (k >= SDLK_1 && k <= SDLK_9 && gLayer.held()) {
    if (!live(a)) {
      gLayer.cancel();
      return false;
    }
    if (!down) return true;  // its key-up too (the typing keyboard never saw the press)
    if (e.key.repeat) return true;
    const zones::Action act = gLayer.digit(static_cast<int>(k - SDLK_0));
    if (act.kind != zones::Action::None) gPending = act;
    return act.swallow;
  }
  return false;
}

void frame(App& a) {
  const bool on = enabled(a) && layoutui::active() == layout::Preset::Studio && !keySetTakesTab();
  // Tabbing only in text fields while zone keys are on (as Clips has it): Tab no longer moves the keys
  // into the next field of the focused window. Set every frame (the Clips and Studio frames reset it).
  if (on && GImGui) GImGui->ConfigNavEnableTabbing = ImGui::GetIO().WantTextInput;
  if (!on) {
    gLayer.cancel();
    gPending = {};
    return;
  }
  if (ImGui::GetIO().WantTextInput) gLayer.cancel();
  // A click into a zone's window counts as being there (Tab alone then comes back to it).
  {
    const zones::Zones z = load(a);
    const std::string w = focusedWindow();
    const int zone = zoneOf(z, w);
    if (zone > 0 && zone != gHistory.current() && w != "Browser" && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) gHistory.visit(zone);
  }
  const zones::Action act = gPending;
  gPending = {};
  if (act.kind == zones::Action::Zone) {
    jump(a, act.zone, true);
  } else if (act.kind == zones::Action::Back) {
    const int z = gHistory.back();
    if (z > 0) jump(a, z, false);
  }
}

zones::Zones load(const App& a) { return zones::fromText(a.settings.getString("studio.zones")); }

void store(App& a, const zones::Zones& z) {
  const std::string text = z == zones::defaults() ? std::string() : zones::toText(z);
  if (a.settings.getString("studio.zones") != text) a.settings.set("studio.zones", text);
}

const std::vector<const char*>& choices() {
  static const std::vector<const char*> kChoices = {"Playlist", "Piano roll", "Mixer", "Channel rack", "Browser", "Stage", "Loupe", "Automation", "Undo history"};
  return kChoices;
}

void devCommand(App& a, const std::vector<std::string>& args) {
  if (args.empty()) return;
  const std::string& c = args[0];
  if (c == "on" || c == "off") {
    a.settings.set("studio.zoneKeys", c == "on");
  } else if (c == "jump" && args.size() > 1) {
    jump(a, std::atoi(args[1].c_str()), true);
  } else if (c == "back") {
    const int z = gHistory.back();
    if (z > 0) jump(a, z, false);
  } else if (c == "set" && args.size() > 2) {  // "zones set <n> <window>[|<window>]" (words joined)
    zones::Zones z = load(a);
    const int n = std::atoi(args[1].c_str());
    std::string w;
    for (size_t i = 2; i < args.size(); ++i) w += (i > 2 ? " " : "") + args[i];
    if (n >= 1 && n <= zones::kZones) {
      const size_t bar = w.find('|');
      z[static_cast<size_t>(n - 1)].windows = {w.substr(0, bar), bar == std::string::npos ? std::string() : w.substr(bar + 1)};
      store(a, z);
    }
  } else if (c == "dump") {
    std::printf("ZONES on %d live %d tabset %d held %d current %d previous %d focus [%s] tabbing %d zones %s\n", enabled(a) ? 1 : 0, live(a) ? 1 : 0,
                keySetTakesTab() ? 1 : 0, gLayer.held() ? 1 : 0, gHistory.current(), gHistory.previous(), focusedWindow().c_str(),
                GImGui && GImGui->ConfigNavEnableTabbing ? 1 : 0, zones::toText(load(a)).c_str());
    std::fflush(stdout);
  }
}

}  // namespace gl::zonekeysui
