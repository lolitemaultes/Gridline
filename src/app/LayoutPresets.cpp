// Gridline - window layout presets in the interface (K-79). See LayoutPresets.h.
#include "LayoutPresets.h"
#include "LiveLook.h"
#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include "../../third_party/imgui/imgui_internal.h"
#include "../core/I18n.h"
#include "../core/Keymap.h"
#include "../core/LayoutShell.h"
#include "../core/Log.h"
#include "../core/Settings.h"
#include "App.h"
#include "Keys.h"
#include "LayoutShellUi.h"
#include "Theme.h"
#include "ThemeSettings.h"
#include "Widgets.h"

namespace gl::layoutui {

using layout::Preset;
using theme::pal;

namespace {
Preset gActive = Preset::Studio;
bool gHasOverride = false;
Preset gOverride = Preset::Studio;
bool gSwitchPending = false;
Preset gSwitchTo = Preset::Studio;

// The tracks set tells Command from Control on macOS. GRIDLINE_DEV_KEYS_NO_CMD=1 (dev and tests) gives a
// macOS run the set every other system has, so k112_menus checks the Linux and Windows menu tree on a Mac.
bool separateCmd() {
#if defined(__APPLE__)
  static const bool fold = [] {
    const char* e = std::getenv("GRIDLINE_DEV_KEYS_NO_CMD");
    return e && *e && *e != '0';
  }();
  return !fold;
#else
  return false;
#endif
}

keys::Profile toProfile(Preset p) {
  switch (p) {
    case Preset::Live: return keys::Profile::Live;
    case Preset::Logic: return keys::Profile::Logic;
    case Preset::Tracker: return keys::Profile::Tracker;  // K-112: F1 to F8 pick the typing octave
    default: return keys::Profile::Studio;
  }
}

ImGuiWindow* findWindow(const char* stable) {
  for (ImGuiWindow* w : GImGui->Windows)
    if (std::strcmp(ui::stableWindowName(w->Name), stable) == 0) return w;
  return nullptr;
}
}  // namespace

Preset active() { return gActive; }
void setActive(Preset p) { gActive = p; }

void setDevOverride(Preset p) {
  gHasOverride = true;
  gOverride = p;
}
bool hasDevOverride() { return gHasOverride; }

Preset menuStyle(const App& a) { return gHasOverride ? gOverride : layout::menuStyle(a.settings); }

void applyKeyProfile(App& a) {
  const Preset p = gHasOverride ? gOverride : layout::keyProfile(a.settings);
  keys::keymap().setProfile(toProfile(p), separateCmd());
  std::string err;
  // A dev run with --layout keeps the user's overrides out (they belong to the set in the settings).
  const std::string own = gHasOverride ? std::string() : a.settings.getString(layout::bindingsKey(p));
  if (!keys::keymap().load(own, &err)) logging::warning("app", err);
}

void saveKeymap(App& a) {
  if (gHasOverride) return;
  a.settings.set(layout::bindingsKey(layout::keyProfile(a.settings)), keys::keymap().serialize());
}

void choose(App& a, Preset p, const layout::Linked& linked) {
  if (gHasOverride) {  // a dev run: this run only
    gOverride = p;
  } else {
    layout::choose(a.settings, p, linked);
    a.settings.set("ui.layoutChosen", true);
  }
  applyKeyProfile(a);
  if (p != gActive) {
    gSwitchPending = true;
    gSwitchTo = p;
  }
}

bool switchPending() { return gSwitchPending; }

bool takeSwitch(Preset* out) {
  if (!gSwitchPending) return false;
  gSwitchPending = false;
  *out = gSwitchTo;
  return true;
}

void viewLayoutMenu(App& a) {
  const layout::Linked linked = layout::linkedFromSettings(a.settings);
  // Each item leads with the layout's glyph, drawn in the menu's icon column (widened for it past the
  // check mark's gutter). K-297: the room was spaces before the name, which the system menu bar, the
  // command palette and the menu tree showed as part of the label.
  const float glyphW = std::round(ImGui::GetFontSize() * 1.15f);
  const float spaceW = std::max(1.0f, ImGui::CalcTextSize(" ").x);
  const float gutterW = std::trunc(ImGui::GetFontSize() * 1.35f);  // the check mark's column (MenuItemEx)
  const std::string room(static_cast<size_t>(std::ceil((gutterW + glyphW + 8.0f) / spaceW)), ' ');
  for (int i = 0; i < layout::kPresetCount; ++i) {
    const Preset p = static_cast<Preset>(i);
    const bool on = gActive == p;
    if (ImGui::MenuItemEx(i18n::tr(layout::displayName(p)), room.c_str(), nullptr, on))
      choose(a, p, linked);
    const ImVec2 r0 = ImGui::GetItemRectMin(), r1 = ImGui::GetItemRectMax();
    const float x = r0.x + GImGui->CurrentWindow->DC.MenuColumns.OffsetLabel - 8.0f - glyphW * 0.5f;
    drawGlyph(ImGui::GetWindowDrawList(), ImVec2(x, std::round((r0.y + r1.y) * 0.5f)), glyphW, p,
              ImGui::GetColorU32(on ? ImGuiCol_Text : ImGuiCol_TextDisabled));
    if (ImGui::IsItemHovered()) ui::Hint(i18n::tr(layout::blurb(p)));
  }
  ImGui::Separator();
  if (ImGui::MenuItem(i18n::tr("Layout settings..."))) {
    a.showSettings = true;
    a.settingsTab = 4;
    a.focusWindow = "Settings";
  }
}

void showPresetWindows(App& a, Preset p) {
  switch (p) {
    case Preset::Live:
      a.showBrowser = a.showPiano = a.showPlaylist = a.showMixer = true;
      a.showRack = false;  // a tab of the bottom panel when opened (F6)
      a.showLivePads = true;  // the second main view, behind the playlist (Tab)
      a.focusWindow = !gHasOverride && a.settings.getString("ui.layout.live.view") == "pads" ? "Live pads" : "Playlist";
      break;
    case Preset::Logic:
      a.showBrowser = a.showPlaylist = true;
      a.showMixer = a.showPiano = a.showRack = a.showHistory = false;  // the bottom and right panes start hidden
      a.showLivePads = false;
      break;
    default:
      a.showBrowser = a.showRack = a.showPiano = a.showPlaylist = a.showMixer = true;
      break;
  }
}

void enterPreset(App& a, Preset p) {
  if (shellBuilt(a, p)) return;  // K-112: the region shell places the windows by their homes
  if (p == Preset::Live && !a.showLivePads) {
    a.showLivePads = true;  // the second main view, a tab behind the playlist
    if (ImGuiWindow* w = findWindow("Playlist")) ImGui::FocusWindow(w);
  }
}

namespace {
double gSwitchedAt = -1.0;
constexpr double kFade = 0.16;  // seconds
}  // namespace

void noteSwitched(double now) { gSwitchedAt = now; }

void drawSwitchFade(double now) {
  if (gSwitchedAt < 0) return;
  const double t = (now - gSwitchedAt) / kFade;
  if (t >= 1.0 || t < 0.0) {
    gSwitchedAt = -1.0;
    return;
  }
  const float k = static_cast<float>(1.0 - t);
  ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::GetForegroundDrawList(vp)->AddRectFilled(vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y),
                                                  theme::withAlpha(pal().surface950, 0.45f * k * k));
}

std::string switchCheck(App& a) {
  struct W {
    bool open;
    const char* name;
  };
  const W ws[] = {{a.showPlaylist, "Playlist"}, {a.showMixer, "Mixer"},          {a.showBrowser, "Browser"},
                  {a.showPiano, "Piano roll"},  {a.showRack, "Channel rack"},    {a.showLivePads, "Live pads"},
                  {a.showHistory, "Undo history"}};
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  std::string bad;
  for (const W& w : ws) {
    if (!w.open) continue;
    ImGuiWindow* win = findWindow(w.name);
    if (!win) {
      bad += std::string(" missing:") + w.name;
      continue;
    }
    const bool tabBehind = win->DockNode && win->DockNode->TabBar && win->DockNode->VisibleWindow && win->DockNode->VisibleWindow != win;
    if (!win->Active && !tabBehind) {
      bad += std::string(" not-drawn:") + w.name;
      continue;
    }
    if (!win->Active || win->Viewport != vp) continue;
    if (win->Size.x < 40 || win->Size.y < 40) bad += std::string(" tiny:") + w.name;
    if (win->Pos.x + win->Size.x < vp->Pos.x + 8 || win->Pos.y + win->Size.y < vp->Pos.y + 8 || win->Pos.x > vp->Pos.x + vp->Size.x - 8 ||
        win->Pos.y > vp->Pos.y + vp->Size.y - 8)
      bad += std::string(" off-screen:") + w.name;
  }
  return bad;
}

std::string openWindows(App& a) {
  const std::pair<bool, const char*> ws[] = {{a.showPlaylist, "playlist"}, {a.showMixer, "mixer"},  {a.showBrowser, "browser"},
                                             {a.showPiano, "piano"},       {a.showRack, "rack"},    {a.showHistory, "history"}};
  std::string out;
  for (const auto& [open, id] : ws)
    if (open) out += (out.empty() ? "" : ",") + std::string(id);
  return out;
}

// ---- pane groups (logic preset) ----

namespace {
struct Member {
  bool App::*flag;
  const char* window;
};
const Member kBottom[] = {{&App::showMixer, "Mixer"}, {&App::showPiano, "Piano roll"}, {&App::showRack, "Channel rack"}};
int gLastEditor = 1;  // piano roll
}  // namespace

void paneGroupsFrame(App& a) {
  static bool prev[3] = {false, false, false};
  static Preset prevPreset = Preset::Count;
  static float heights[2] = {0, 0};  // 0 mixer, 1 editors
  static int shownKind = -1;
  static bool closedByGroup[3] = {false, false, false};  // open before, closed only because the pane is shared
  constexpr int n = 3;
  if (gActive == Preset::Logic && shellBuilt(a, gActive)) return;  // K-112: the shell's exclusive regions do this
  if (gActive != Preset::Logic) {
    if (prevPreset == Preset::Logic)  // leaving: what the shared pane hid comes back
      for (int i = 0; i < n; ++i)
        if (closedByGroup[i]) a.*kBottom[i].flag = true;
    for (bool& c : closedByGroup) c = false;
    prevPreset = gActive;
    return;
  }
  if (prevPreset != Preset::Logic) {  // just switched in: start from what is open
    for (int i = 0; i < n; ++i) prev[i] = a.*kBottom[i].flag;
    prevPreset = Preset::Logic;
    shownKind = -1;
  }
  int opened = -1;
  for (int i = 0; i < n; ++i)
    if (a.*kBottom[i].flag && !prev[i]) {
      opened = i;
      break;
    }
  for (int i = 0; i < n; ++i)
    if (!(a.*kBottom[i].flag) && prev[i]) closedByGroup[i] = false;  // closed by the person: stays closed
  if (opened >= 0) {
    closedByGroup[opened] = false;
    for (int i = 0; i < n; ++i)
      if (i != opened && a.*kBottom[i].flag) {
        a.*kBottom[i].flag = false;
        closedByGroup[i] = true;
      }
    if (opened > 0) gLastEditor = opened;
    // The mixer and the editors keep their own pane heights.
    const int kind = opened == 0 ? 0 : 1;
    if (ImGuiWindow* w = findWindow(kBottom[opened].window))
      if (ImGuiDockNode* node = w->DockId ? ImGui::DockBuilderGetNode(w->DockId) : nullptr) {
        if (shownKind >= 0 && shownKind != kind) heights[shownKind] = node->SizeRef.y;
        if (shownKind != kind && heights[kind] > 0) node->SizeRef.y = heights[kind];
      }
    shownKind = kind;
  } else {
    // Several open at once (a saved layout): the first one stays.
    bool seen = false;
    for (int i = 0; i < n; ++i)
      if (a.*kBottom[i].flag) {
        if (seen) {
          a.*kBottom[i].flag = false;
          closedByGroup[i] = true;
        }
        seen = true;
      }
  }
  for (int i = 0; i < n; ++i) prev[i] = a.*kBottom[i].flag;
}

void toggleEditors(App& a) {
  if (a.showPiano || a.showRack) {
    gLastEditor = a.showRack ? 2 : 1;
    a.showPiano = a.showRack = false;
    return;
  }
  if (gLastEditor == 2) {
    a.showRack = true;
    a.focusWindow = "Channel rack";
  } else {
    a.showPiano = true;
    a.focusWindow = "Piano roll";
  }
}

void toggleMainView(App& a) {
  if (livelook::active(a)) {  // K-112c: the Live layout shows one main view at a time
    livelook::showView(a, !livelook::gridView(a));
    return;
  }
  if (gActive == Preset::Logic && shellBuilt(a, Preset::Logic)) {  // K-112b8: the pads grid beside the tracks, shown or hidden
    const bool shown = a.showLivePads && layout::visible(layout::shell(Preset::Logic), regionState(a, Preset::Logic), "logic.grid");
    a.showLivePads = !shown;
    setRegionVisible(a, "logic.grid", !shown);
    if (!shown) a.focusWindow = "Live pads";
    return;
  }
  ImGuiWindow* pads = findWindow("Live pads");
  ImGuiWindow* list = findWindow("Playlist");
  if (!a.showLivePads || !pads) {
    a.showLivePads = true;
    a.showPlaylist = true;
    a.focusWindow = "Live pads";
    if (!gHasOverride) a.settings.set("ui.layout.live.view", "pads");
    return;
  }
  const bool padsFront = pads->DockIsActive && pads->DockNode && pads->DockNode->TabBar ? pads->DockNode->TabBar->SelectedTabId == pads->TabId
                                                                                         : GImGui->NavWindow == pads;
  ImGuiWindow* to = padsFront ? list : pads;
  if (!to) {
    a.showPlaylist = true;
    return;
  }
  if (to->DockIsActive && to->DockNode && to->DockNode->TabBar) to->DockNode->TabBar->NextSelectedTabId = to->TabId;
  a.focusWindow = padsFront ? "Playlist" : "Live pads";
  if (!gHasOverride) a.settings.set("ui.layout.live.view", padsFront ? "playlist" : "pads");
}

void showPlaylist(App& a) {
  a.showPlaylist = true;
  if (ImGuiWindow* list = findWindow("Playlist"))
    if (list->DockIsActive && list->DockNode && list->DockNode->TabBar) list->DockNode->TabBar->NextSelectedTabId = list->TabId;
  a.focusWindow = "Playlist";
  if (gActive == Preset::Live && a.showLivePads && !gHasOverride) a.settings.set("ui.layout.live.view", "playlist");
}

}  // namespace gl::layoutui
