// Gridline — J-113 / J-114 / J-116 detachable windows (see Viewports.h, docs/detachable-windows.md).
#include "Viewports.h"
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <set>
#include <SDL_opengl.h>
#include "../../third_party/imgui/imgui_impl_opengl3.h"
#include "../../third_party/imgui/imgui_internal.h"
#include "../core/I18n.h"
#include "../core/WindowPlacement.h"
#include "../host/ui/NativeView.h"
#include "App.h"
#include "DialogLayout.h"
#include "LiveResize.h"
#include "Theme.h"
#include "Widgets.h"
#include "SysWm.h"

namespace gl::viewports {
namespace {

// Room left above a window placed outside for its OS title bar (OS units).
constexpr float kOsTitle = 32.0f;

struct VRect {
  ImVec2 pos, size;
};

struct State {
  SDL_Window* main = nullptr;
  SDL_GLContext gl = nullptr;
  bool installed = false, hidden = false, virtualMonitors = false, available = false;
  std::string reason;
  int disableIn = -1;  // frames until viewports go off (the windows are brought back first)
  float scale = 1.0f;
  ImVec2 display{0, 0};  // main window, logical
  ImVec2 mainOs{0, 0};   // main window's client origin, OS units
  bool haveMainOs = false;
  ImGuiID lastEvent = 0;  // mouse input events up to this id are main-window relative already
  ImGuiID dockspace = 0;  // the main window's dock space (GridlineDock)
  // The backend's platform callbacks, wrapped below.
  void (*createWindow)(ImGuiViewport*) = nullptr;
  void (*destroyWindow)(ImGuiViewport*) = nullptr;
  void (*showWindow)(ImGuiViewport*) = nullptr;
  void (*setPos)(ImGuiViewport*, ImVec2) = nullptr;
  ImVec2 (*getPos)(ImGuiViewport*) = nullptr;
  void (*setSize)(ImGuiViewport*, ImVec2) = nullptr;
  ImVec2 (*getSize)(ImGuiViewport*) = nullptr;
  ImVec2 (*getFbScale)(ImGuiViewport*) = nullptr;
  void (*setFocus)(ImGuiViewport*) = nullptr;
  bool (*getFocus)(ImGuiViewport*) = nullptr;
  std::map<ImGuiViewport*, VRect> virt;  // --hidden: where each (never shown) window is, ImGui coordinates
  std::set<Uint32> ours;                 // SDL ids of every window created for a viewport
  // Monitors in OS units (re-read on display events and every couple of seconds).
  std::vector<SDL_Rect> monBounds, monWork;
  Uint64 monReadAt = 0;
  bool monDirty = true;
  // layout.ini [GridlineViewports]: the main window and scale saved coordinates are relative to,
  // where each window docks back (Home) and its last place outside (Out, OS units).
  place::SavedMain saved;
  bool reanchorPending = false;
  std::map<std::string, place::Home> homes;
  std::map<std::string, place::Rect> lastOut;
  struct Pending {
    ImGuiID id;
    ImVec2 pos, size;
    int frames;
  };
  std::vector<Pending> pendingOut;  // popped out / detached once undocked
  std::vector<ImGuiID> toggles;     // pop-out / dock-back button presses
  std::vector<ImGuiID> closeReqs;   // J-116: a detached window's OS close button / Cmd+W: docked back
  std::map<ImGuiID, int> homing;    // windows docking back: not re-flagged as detached meanwhile (until it is back in the main window or closed)
  std::map<std::string, ImRect> buttons;  // last drawn pop-out / dock-back buttons (dev)
  std::map<ImGuiID, int> appeared;        // windows that appeared lately: frame
  std::map<std::string, ImVec2> drawn;    // J-116 (dev): the size each detached window was last drawn at
  int drawnFrames = 0;                    // frames that drew detached windows
  // J-109: keyboard focus over Gridline's own windows.
  Uint32 focusId = 0;
  bool lostArmed = false;
  Uint64 lostAt = 0;
  std::vector<Uint32> hiddenForMinimize;
  // Dev.
  std::vector<std::pair<std::string, std::string>> shots;
  double frameMs[120] = {}, windowsMs[120] = {};  // frame interval; time spent updating / drawing / swapping detached windows
  int frameN = 0;
  Uint64 lastRender = 0;
};
State S;

double seconds(Uint64 a, Uint64 b) { return static_cast<double>(b - a) / static_cast<double>(SDL_GetPerformanceFrequency()); }
Uint64 now() { return SDL_GetPerformanceCounter(); }

bool on() { return ImGui::GetCurrentContext() && (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) != 0; }
bool isMain(const ImGuiViewport* vp) { return vp == ImGui::GetMainViewport(); }
ImVec2 toOs(ImVec2 p) { return ImVec2(S.mainOs.x + p.x * S.scale, S.mainOs.y + p.y * S.scale); }
ImVec2 fromOs(ImVec2 p) { return ImVec2((p.x - S.mainOs.x) / S.scale, (p.y - S.mainOs.y) / S.scale); }
ImVec2 rounded(ImVec2 v) { return ImVec2(std::round(v.x), std::round(v.y)); }
SDL_Window* sdlOf(const ImGuiViewport* vp) {
  return vp && vp->PlatformHandle ? SDL_GetWindowFromID(static_cast<Uint32>(reinterpret_cast<intptr_t>(vp->PlatformHandle))) : nullptr;
}
Uint32 idOf(const ImGuiViewport* vp) { return vp ? static_cast<Uint32>(reinterpret_cast<intptr_t>(vp->PlatformHandle)) : 0; }
bool shortLived(const ImGuiWindow* w) { return (w->Flags & (ImGuiWindowFlags_Popup | ImGuiWindowFlags_Tooltip | ImGuiWindowFlags_ChildMenu)) != 0; }
const char* nameOf(const ImGuiWindow* w) { return ui::stableWindowName(w->Name); }
// A window of its own outside the main window (not a popup / menu / tooltip).
bool isOut(const ImGuiWindow* root) { return root && root->Viewport && !isMain(root->Viewport) && !shortLived(root); }
// A viewport some window (not a popup / menu / tooltip) is in and owns (an abandoned one keeps
// pointing at its last window for a frame or two).
bool owned(const ImGuiViewportP* vp) { return vp->Window && vp->Window->Viewport == vp && !shortLived(vp->Window); }
bool buttonsOn();

host::native::Window nativeOf(SDL_Window* w) {
  host::native::Window n;
  if (!w) return n;
  SDL_SysWMinfo info;
  SDL_VERSION(&info.version);
  if (!SDL_GetWindowWMInfo(w, &info)) return n;
#if defined(__APPLE__)
  if (info.subsystem == SDL_SYSWM_COCOA) n.handle = static_cast<void*>(info.info.cocoa.window);
#elif defined(_WIN32)
  if (info.subsystem == SDL_SYSWM_WINDOWS) n.handle = static_cast<void*>(info.info.win.window);
#elif defined(SDL_VIDEO_DRIVER_X11)
  if (info.subsystem == SDL_SYSWM_X11) {
    n.display = static_cast<void*>(info.info.x11.display);
    n.handle = reinterpret_cast<void*>(static_cast<uintptr_t>(info.info.x11.window));
  }
#endif
  return n;
}

// ---- dev trace (GRIDLINE_VP_TRACE=1): SDL mouse / window events and the per-frame state ----
bool traceOn() {
  static const bool t = std::getenv("GRIDLINE_VP_TRACE") != nullptr;
  return t;
}
int SDLCALL traceWatch(void*, SDL_Event* e) {
  if (e->type == SDL_MOUSEMOTION)
    std::printf("trace ev motion win %u x %d y %d state %u\n", e->motion.windowID, e->motion.x, e->motion.y, e->motion.state);
  else if (e->type == SDL_MOUSEBUTTONDOWN || e->type == SDL_MOUSEBUTTONUP)
    std::printf("trace ev button %s win %u x %d y %d\n", e->type == SDL_MOUSEBUTTONDOWN ? "down" : "up", e->button.windowID, e->button.x, e->button.y);
  else if (e->type == SDL_WINDOWEVENT)
    std::printf("trace ev window %u event %d data %d %d\n", e->window.windowID, e->window.event, e->window.data1, e->window.data2);
  return 0;
}
void traceFrame() {
  ImGuiContext& g = *GImGui;
  int gx = 0, gy = 0;
  const Uint32 gb = SDL_GetGlobalMouseState(&gx, &gy);
  SDL_Window* kf = SDL_GetKeyboardFocus();
  SDL_Window* mf = SDL_GetMouseFocus();
  std::printf("trace frame %d global %d %d buttons %u kbfocus %u mousefocus %u mainos %.0f %.0f moving \"%s\"\n", g.FrameCount, gx, gy, gb,
              kf ? SDL_GetWindowID(kf) : 0, mf ? SDL_GetWindowID(mf) : 0, static_cast<double>(S.mainOs.x), static_cast<double>(S.mainOs.y),
              g.MovingWindow ? nameOf(g.MovingWindow) : "");
  for (const ImGuiInputEvent& e : g.InputEventsQueue)
    if (e.Type == ImGuiInputEventType_MousePos)
      std::printf("trace   mousepos %.1f %.1f (main-relative OS units)\n", static_cast<double>(e.MousePos.PosX), static_cast<double>(e.MousePos.PosY));
  for (ImGuiViewportP* vp : g.Viewports) {
    SDL_Window* w = sdlOf(vp);
    int x = 0, y = 0, ww = 0, hh = 0;
    if (w) SDL_GetWindowPosition(w, &x, &y), SDL_GetWindowSize(w, &ww, &hh);
    const ImGuiWindow* vw = vp->Window;
    std::printf("trace   viewport \"%s\" imgui %.1f %.1f %.1f x %.1f os window %u %d %d %d x %d detached-flag %d homing %d\n", vw ? nameOf(vw) : "(main)",
                static_cast<double>(vp->Pos.x), static_cast<double>(vp->Pos.y), static_cast<double>(vp->Size.x), static_cast<double>(vp->Size.y),
                w ? SDL_GetWindowID(w) : 0, x, y, ww, hh, vw && (vw->WindowClass.ViewportFlagsOverrideSet & ImGuiViewportFlags_NoAutoMerge) ? 1 : 0,
                vw && S.homing.count(vw->ID) ? 1 : 0);
  }
  std::fflush(stdout);
}

// ---- platform callbacks in Gridline's coordinate space ----

void wCreate(ImGuiViewport* vp) {
  const ImVec2 p = vp->Pos, s = vp->Size;
  vp->Pos = rounded(toOs(p));  // the backend creates the SDL window at viewport->Pos / Size
  vp->Size = rounded(ImVec2(s.x * S.scale, s.y * S.scale));
  S.createWindow(vp);
  vp->Pos = p;
  vp->Size = s;
  if (S.hidden) S.virt[vp] = {p, s};
  SDL_Window* w = sdlOf(vp);
  if (!w) return;
  S.ours.insert(SDL_GetWindowID(w));
  const ImGuiWindow* win = static_cast<ImGuiViewportP*>(vp)->Window;
  if (!win || shortLived(win)) return;
  // A detached window: decorated by the OS (its title bar moves it, its edges resize it).
  if (win->Flags & ImGuiWindowFlags_AlwaysAutoResize) {
    SDL_SetWindowResizable(w, SDL_FALSE);  // fixed-size editors stay fixed
  } else {
    const ImVec2 m = ImGui::GetStyle().WindowMinSize;
    SDL_SetWindowMinimumSize(w, std::max(1, static_cast<int>(std::lround(m.x * S.scale))), std::max(1, static_cast<int>(std::lround(m.y * S.scale))));
  }
#if !defined(__APPLE__)
  // Windows: owned by the main window (above it, minimised with it); X11: transient for it.
  // macOS: kept above the main window by ordering instead (onEvent): an AppKit child window
  // would be dragged along whenever the main window moves.
  if (!S.hidden) host::native::keepAbove(nativeOf(w), nativeOf(S.main), true);
#endif
}
void wDestroy(ImGuiViewport* vp) {
  S.virt.erase(vp);
  if (const Uint32 id = idOf(vp)) S.ours.erase(id);
  S.destroyWindow(vp);
}
void wShow(ImGuiViewport* vp) {
  if (S.hidden) return;
  S.showWindow(vp);
}
void wSetPos(ImGuiViewport* vp, ImVec2 p) {
  if (S.hidden) {
    S.virt[vp].pos = p;
    return;
  }
  S.setPos(vp, rounded(toOs(p)));
}
ImVec2 wGetPos(ImGuiViewport* vp) {
  if (isMain(vp)) return ImVec2(0, 0);
  if (S.hidden) {
    auto it = S.virt.find(vp);
    return it != S.virt.end() ? it->second.pos : vp->Pos;
  }
  return fromOs(S.getPos(vp));
}
void wSetSize(ImGuiViewport* vp, ImVec2 s) {
  if (S.hidden) S.virt[vp].size = s;
  S.setSize(vp, rounded(ImVec2(s.x * S.scale, s.y * S.scale)));
}
ImVec2 wGetSize(ImGuiViewport* vp) {
  if (isMain(vp)) return ImGui::GetIO().DisplaySize;
  if (S.hidden) {
    auto it = S.virt.find(vp);
    if (it != S.virt.end()) return it->second.size;
  }
  const ImVec2 s = S.getSize(vp);
  return ImVec2(s.x / S.scale, s.y / S.scale);
}
ImVec2 wFbScale(ImGuiViewport* vp) {
  if (isMain(vp)) return ImGui::GetIO().DisplayFramebufferScale;
  const ImVec2 f = S.getFbScale(vp);
  return ImVec2(f.x * S.scale, f.y * S.scale);
}
void wSetFocus(ImGuiViewport* vp) {
  if (!S.hidden) S.setFocus(vp);
}
bool wGetFocus(ImGuiViewport* vp) { return !S.hidden && S.getFocus(vp); }

// GRIDLINE PATCH (J-113) filter: a window that stops fitting the main window never gets an OS
// window of its own because of that (not while it is dragged, J-116: not when it is released
// outside either, not when it grows past the edge: it stays clipped to the main window, only the
// pop-out button detaches), except popups / menus / tooltips (they may stick out of a small detached window) and a
// window opened outside on purpose (centre outside the main window within the few frames it
// takes to appear and centre itself: a dialog over a detached window). Detached windows get
// theirs as windows that always want one (ImGuiViewportFlags_NoAutoMerge in their window class).
bool lateCreate(ImGuiWindow* w) {
  if (shortLived(w)) return true;
  const ImGuiContext& g = *GImGui;
  const auto ap = S.appeared.find(w->ID);
  if (!w->Appearing && (ap == S.appeared.end() || g.FrameCount - ap->second > 4)) return false;
  if (g.MovingWindow && g.MovingWindow->RootWindowDockTree == w->RootWindowDockTree) return false;
  const ImGuiViewport* m = ImGui::GetMainViewport();
  // J-158: where it is meant to be. A window centred with a pivot (plugin editors, dialogs) is still
  // at its old place, or at the raw anchor, on the frame it measures its size: its anchor tells
  // where it opens. Otherwise the centre of its rectangle, as large as the main window at most (a
  // window saved larger than the main window is now, its top-left corner inside, opens inside).
  ImVec2 c;
  if (w->SetWindowPosVal.x != FLT_MAX && w->SetWindowPosVal.y != FLT_MAX)
    c = w->SetWindowPosVal;
  else
    c = ImVec2(w->Pos.x + std::min(w->Size.x, m->Size.x) * 0.5f, w->Pos.y + std::min(w->Size.y, m->Size.y) * 0.5f);
  return !ImRect(m->Pos, ImVec2(m->Pos.x + m->Size.x, m->Pos.y + m->Size.y)).Contains(c);
}

// ---- detached windows: the window class flag, homes ----

template <typename F>
void forNodeWindows(ImGuiDockNode* n, F&& f) {
  if (!n) return;
  for (ImGuiWindow* w : n->Windows) f(w);
  forNodeWindows(n->ChildNodes[0], f);
  forNodeWindows(n->ChildNodes[1], f);
}
// Detached: the window (or every window of a dock node host) always wants an OS window of its
// own, which ImGui then never merges back into the main one by itself.
void setDetachedFlag(ImGuiWindow* root, bool out) {
  auto set = [out](ImGuiWindow* w) {
    if (out) w->WindowClass.ViewportFlagsOverrideSet |= ImGuiViewportFlags_NoAutoMerge;
    else w->WindowClass.ViewportFlagsOverrideSet &= ~ImGuiViewportFlags_NoAutoMerge;
  };
  if (root->DockNodeAsHost) forNodeWindows(root->DockNodeAsHost, set);
  set(root);
}
bool flagged(const ImGuiWindow* w) { return (w->WindowClass.ViewportFlagsOverrideSet & ImGuiViewportFlags_NoAutoMerge) != 0; }
// The window a dock node host stands for (its visible tab), else the window itself.
ImGuiWindow* keyWindow(ImGuiWindow* root) {
  if (ImGuiDockNode* n = root ? root->DockNodeAsHost : nullptr) {
    if (n->VisibleWindow) return n->VisibleWindow;
    ImGuiWindow* first = nullptr;
    forNodeWindows(n, [&](ImGuiWindow* w) { first = first ? first : w; });
    return first;
  }
  return root;
}

// A panel or editor that can be popped out (and is snapshotted for docking back): a top-level,
// movable window with a name of its own that docks, or that marked itself (ui::MarkDetachable).
// (Docked windows are child windows of their dock node's host.)
bool isPanel(const ImGuiWindow* w) {
  if (!w || w->IsFallbackWindow || shortLived(w) || (w->Flags & (ImGuiWindowFlags_Modal | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_DockNodeHost)))
    return false;
  if ((w->Flags & ImGuiWindowFlags_ChildWindow) && !w->DockIsActive) return false;
  if (w->Name[0] == '#' && w->Name[1] == '#') return false;  // ##topbar, ##status, ##dockhost, ...
  return !(w->Flags & ImGuiWindowFlags_NoDocking) || ui::IsMarkedDetachable(w);
}

ImRect mainRect() {
  if (S.display.x > 0 && S.display.y > 0) return ImRect(ImVec2(0, 0), S.display);  // this frame's (before NewFrame too)
  const ImGuiViewport* m = ImGui::GetMainViewport();
  return ImRect(m->Pos, ImVec2(m->Pos.x + m->Size.x, m->Pos.y + m->Size.y));
}
place::Rect workArea() {
  const ImRect r = mainRect();
  return place::Rect{r.Min.x, r.Min.y + theme::kTopBarH, std::max(1.0f, r.GetWidth()), std::max(1.0f, r.GetHeight() - theme::kTopBarH - theme::kStatusBarH)};
}
bool nodeInMain(ImGuiDockNode* n) {
  ImGuiDockNode* r = n ? ImGui::DockNodeGetRootNode(n) : nullptr;
  if (!r) return false;
  if (r->IsDockSpace()) return r->ID == S.dockspace || (r->HostWindow && isMain(r->HostWindow->Viewport));
  return r->HostWindow && r->HostWindow->Viewport && isMain(r->HostWindow->Viewport);
}

// Every frame (before NewFrame): where each panel inside the main window would dock back to,
// remembered while it is there (never while it is dragged).
void snapshotHomes() {
  ImGuiContext& g = *GImGui;
  ImGuiWindow* moving = g.MovingWindow ? g.MovingWindow->RootWindowDockTree : nullptr;
  const ImRect mr = mainRect();
  for (ImGuiWindow* w : g.Windows) {
    if (!w->WasActive || !isPanel(w)) continue;
    ImGuiWindow* r = w->RootWindowDockTree;
    if (r == moving || !r->Viewport || !isMain(r->Viewport) || S.homing.count(r->ID)) continue;
    place::Home h;
    if (w->DockIsActive && w->DockNode) {
      ImGuiDockNode* n = w->DockNode;
      if (!nodeInMain(n)) continue;
      h.have = h.docked = true;
      h.node = n->ID;
      if (ImGuiDockNode* p = n->ParentNode) {
        const int idx = p->ChildNodes[0] == n ? 0 : 1;
        const int axis = p->SplitAxis == ImGuiAxis_X ? 0 : 1;
        h.parent = p->ID;
        h.dir = axis == 0 ? (idx == 0 ? ImGuiDir_Left : ImGuiDir_Right) : (idx == 0 ? ImGuiDir_Up : ImGuiDir_Down);
        const float total = axis == 0 ? p->Size.x : p->Size.y, mine = axis == 0 ? n->Size.x : n->Size.y;
        h.share = total > 1 ? std::clamp(mine / total, 0.05f, 0.95f) : 0.5f;
      }
    } else if (!w->DockIsActive && w->DockId == 0) {
      if (!mr.Contains(w->Rect())) continue;  // partly outside (clipped): not a place to come back to
      h.have = true;
      h.floating = place::Rect{std::round(w->Pos.x), std::round(w->Pos.y), std::round(w->SizeFull.x), std::round(w->SizeFull.y)};
    } else {
      continue;
    }
    place::Home& cur = S.homes[nameOf(w)];
    if (place::homeLines(cur) != place::homeLines(h) || cur.have != h.have) {
      cur = h;
      ImGui::MarkIniSettingsDirty();
    }
  }
}

std::vector<place::Rect> monitorRects() {
  std::vector<place::Rect> out;
  for (const ImGuiPlatformMonitor& m : ImGui::GetPlatformIO().Monitors) out.push_back({m.WorkPos.x, m.WorkPos.y, m.WorkSize.x, m.WorkSize.y});
  return out;
}

// Put a window outside the main window at `pos` (ImGui units), kept on a monitor with room for
// its OS title bar; it gets its own OS window when it is next begun.
void placeOut(ImGuiWindow* root, ImVec2 pos, ImVec2 size, bool setSize) {
  const bool fixed = (root->Flags & ImGuiWindowFlags_AlwaysAutoResize) != 0;
  if (!setSize || fixed) size = root->SizeFull;
  const std::vector<place::Rect> mons = monitorRects();
  place::Rect r{pos.x, pos.y, size.x, size.y};
  r = place::keepTitleOnMonitors(r, mons);
  r = place::keepOsTitleOnMonitor(r, mons, kOsTitle / S.scale);
  const ImVec2 p(std::round(r.x), std::round(r.y));
  ImGui::SetWindowPos(root, p, ImGuiCond_Always);
  if (setSize && !fixed) ImGui::SetWindowSize(root, size, ImGuiCond_Always);
  if (ImGuiDockNode* n = root->DockNodeAsHost) {
    n->Pos = p;
    if (setSize) n->Size = size;
  }
  setDetachedFlag(root, true);
  S.homing.erase(root->ID);
  ImGui::MarkIniSettingsDirty();
}

// The OS rectangle of a detached window, remembered for "Pop out" (its last place outside).
void rememberOut(ImGuiWindow* root) {
  ImGuiWindow* key = keyWindow(root);
  const ImGuiViewport* vp = root ? root->Viewport : nullptr;
  if (!key || !vp || isMain(vp)) return;
  const ImVec2 p = toOs(vp->Pos);
  const place::Rect r{std::round(p.x), std::round(p.y), std::round(vp->Size.x * S.scale), std::round(vp->Size.y * S.scale)};
  place::Rect& cur = S.lastOut[nameOf(key)];
  if (cur.x != r.x || cur.y != r.y || cur.w != r.w || cur.h != r.h) {
    cur = r;
    ImGui::MarkIniSettingsDirty();
  }
}

// Back into the main window: docked where it was (its node, else split off the node it was split
// from on the same side and share, else a tab of the central node), or, when it was floating
// inside, at that place.
void dockHome(ImGuiWindow* root) {
  ImGuiContext& g = *GImGui;
  root = root ? root->RootWindowDockTree : nullptr;
  ImGuiWindow* key = keyWindow(root);
  if (!root || !key) return;
  setDetachedFlag(root, false);
  S.homing[root->ID] = 30;
  const auto it = S.homes.find(nameOf(key));
  const bool docked = it != S.homes.end() && it->second.have && it->second.docked;
  ImGuiDockNode* target = nullptr;
  ImGuiDir dir = ImGuiDir_None;
  float ratio = 0.5f;
  if (docked) {
    const place::Home& h = it->second;
    ImGuiDockNode* n = ImGui::DockContextFindNodeByID(&g, h.node);
    ImGuiDockNode* p = h.parent ? ImGui::DockContextFindNodeByID(&g, h.parent) : nullptr;
    if (n && n->IsLeafNode() && nodeInMain(n)) {
      target = n;
    } else if (p && nodeInMain(p) && h.dir >= 0) {
      target = p;
      dir = static_cast<ImGuiDir>(h.dir);
      ratio = place::splitRatioFor(h.dir, h.share);
    }
  }
  const bool dockable = !(key->Flags & ImGuiWindowFlags_NoDocking);
  if (!target && dockable && (it == S.homes.end() || it->second.docked))
    if (ImGuiDockNode* c = S.dockspace ? ImGui::DockBuilderGetCentralNode(S.dockspace) : nullptr) target = c;  // no usable home: a tab of the centre
  if (target) {
    ImGui::DockContextQueueDock(&g, nullptr, target, root, dir, ratio, false);
    return;
  }
  // Floating inside the main window: where it was, else cascaded.
  const place::Rect area = workArea();
  place::Rect r = it != S.homes.end() && it->second.have && !it->second.docked ? it->second.floating
                                                                              : place::cascade(0, root->SizeFull.x, root->SizeFull.y, area);
  r = place::fitInside(r, area);
  ImGui::SetWindowPos(root, ImVec2(r.x, r.y), ImGuiCond_Always);
  if (!(root->Flags & ImGuiWindowFlags_AlwaysAutoResize)) ImGui::SetWindowSize(root, ImVec2(r.w, r.h), ImGuiCond_Always);
  if (ImGuiDockNode* n = root->DockNodeAsHost) n->Pos = ImVec2(r.x, r.y), n->Size = ImVec2(r.w, r.h);
}

// "Pop out into its own window": the panel's last place outside (recovered onto a monitor), else
// beside the main window with its current size.
void popOut(ImGuiWindow* w) {
  if (!w || !isPanel(w)) return;
  ImGuiContext& g = *GImGui;
  const std::vector<place::Rect> mons = monitorRects();
  ImVec2 size = w->DockIsActive ? w->Size : w->SizeFull;
  place::Rect r;
  const auto it = S.lastOut.find(nameOf(w));
  if (it != S.lastOut.end()) {
    const ImVec2 p = fromOs(ImVec2(it->second.x, it->second.y));
    r = place::recoverOnScreen(place::Rect{p.x, p.y, it->second.w / S.scale, it->second.h / S.scale}, mons);
  } else {
    const ImRect m = mainRect();
    r = place::popOutPlace(size.x, size.y, std::max(m.Min.y + theme::kTopBarH, w->Pos.y), place::Rect{m.Min.x, m.Min.y, m.GetWidth(), m.GetHeight()}, mons,
                           24.0f, kOsTitle / S.scale);
  }
  size = ImVec2(r.w, r.h);
  if (w->DockIsActive || w->DockId != 0) {
    ImGui::DockContextQueueUndockWindow(&g, w);
    S.pendingOut.push_back({w->ID, ImVec2(r.x, r.y), size, 10});
  } else {
    placeOut(w->RootWindowDockTree, ImVec2(r.x, r.y), size, true);
  }
}

// J-116: Gridline's own close button (title bar X, tab X, dock node X) of a detached panel docks
// it back like its OS close button: the panel stays open. Dialogs close as ImGui does.
bool closeFilter(ImGuiWindow* w) {
  if (!enabled() || !isPanel(w) || !isOut(w->RootWindowDockTree)) return false;
  S.closeReqs.push_back(w->ID);
  return true;
}

void toggle(ImGuiWindow* w) {
  if (!w) return;
  ImGuiWindow* root = w->RootWindowDockTree;
  if (isOut(root)) dockHome(root);
  else popOut(w);
}

// The dock node window-menu button (reserved on the right of every tab bar while detaching is
// on) is the docked panels' pop-out / dock-back button: a click toggles instead of a menu.
void windowMenu(ImGuiContext* ctx, ImGuiDockNode* node, ImGuiTabBar* tabBar) {
  if (!buttonsOn()) {
    ImGui::DockNodeWindowMenuHandler_Default(ctx, node, tabBar);
    return;
  }
  if (node->VisibleWindow) S.toggles.push_back(node->VisibleWindow->ID);
  ImGui::CloseCurrentPopup();
}

// ---- layout.ini ----

struct IniEntry {
  int kind;  // 0 main, 1 home, 2 out
  std::string name;
};
std::deque<IniEntry> gIni;
void* iniOpen(ImGuiContext*, ImGuiSettingsHandler*, const char* name) {
  if (std::strcmp(name, "Main") == 0) gIni.push_back({0, ""});
  else if (std::strncmp(name, "Home:", 5) == 0 && name[5]) gIni.push_back({1, name + 5});
  else if (std::strncmp(name, "Out:", 4) == 0 && name[4]) gIni.push_back({2, name + 4});
  else return nullptr;
  return &gIni.back();
}
void iniLine(ImGuiContext*, ImGuiSettingsHandler*, void* entry, const char* line) {
  const IniEntry& e = *static_cast<IniEntry*>(entry);
  if (e.kind == 0) {
    place::readSavedMainLine(line, S.saved);
  } else if (e.kind == 1) {
    place::Home h = S.homes[e.name];
    if (place::readHomeLine(line, h)) S.homes[e.name] = h;
  } else {
    place::Rect r;
    if (place::readOutLine(line, r)) S.lastOut[e.name] = r;
  }
}
void iniClear(ImGuiContext*, ImGuiSettingsHandler*) {
  S.saved = {};
  S.homes.clear();
  S.lastOut.clear();
}
void iniApply(ImGuiContext*, ImGuiSettingsHandler*) {
  S.reanchorPending = S.saved.have;
  gIni.clear();
}
void iniWrite(ImGuiContext*, ImGuiSettingsHandler* h, ImGuiTextBuffer* buf) {
  if (S.haveMainOs)
    buf->appendf("[%s][Main]\nMainOs=%d,%d\nScale=%.4f\n\n", h->TypeName, static_cast<int>(S.mainOs.x), static_cast<int>(S.mainOs.y),
                 static_cast<double>(S.scale));
  for (const auto& [name, home] : S.homes)
    if (home.have) buf->appendf("[%s][Home:%s]\n%s\n", h->TypeName, name.c_str(), place::homeLines(home).c_str());
  for (const auto& [name, r] : S.lastOut)
    buf->appendf("[%s][Out:%s]\nRect=%.0f,%.0f,%.0f,%.0f\n\n", h->TypeName, name.c_str(), static_cast<double>(r.x), static_cast<double>(r.y),
                 static_cast<double>(r.w), static_cast<double>(r.h));
}

// ---- monitors ----

void readMonitors() {
  S.monBounds.clear();
  S.monWork.clear();
  for (int n = 0, count = SDL_GetNumVideoDisplays(); n < count; ++n) {
    SDL_Rect r{}, u{};
    if (SDL_GetDisplayBounds(n, &r) != 0 || r.w <= 0 || r.h <= 0) continue;
    if (SDL_GetDisplayUsableBounds(n, &u) != 0 || u.w <= 0 || u.h <= 0) u = r;
    S.monBounds.push_back(r);
    S.monWork.push_back(u);
  }
  S.monReadAt = now();
  S.monDirty = false;
}

void publishMonitors() {
  ImGuiPlatformIO& pio = ImGui::GetPlatformIO();
  pio.Monitors.resize(0);
  auto add = [&](ImVec2 mp, ImVec2 ms, ImVec2 wp, ImVec2 ws, int n) {
    ImGuiPlatformMonitor m;
    m.MainPos = mp, m.MainSize = ms, m.WorkPos = wp, m.WorkSize = ws;
    m.DpiScale = 1.0f;  // the interface scale is Gridline's (J-91), not ImGui's
    m.PlatformHandle = reinterpret_cast<void*>(static_cast<intptr_t>(n));
    pio.Monitors.push_back(m);
  };
  if (S.virtualMonitors || S.monBounds.empty()) {
    add(ImVec2(0, 0), S.display, ImVec2(0, 0), S.display, 0);
    if (S.virtualMonitors) add(ImVec2(S.display.x, 0), S.display, ImVec2(S.display.x, 0), S.display, 1);
    return;
  }
  for (size_t i = 0; i < S.monBounds.size(); ++i) {
    const SDL_Rect& b = S.monBounds[i];
    const SDL_Rect& u = S.monWork[i];
    add(fromOs(ImVec2(static_cast<float>(b.x), static_cast<float>(b.y))), ImVec2(static_cast<float>(b.w) / S.scale, static_cast<float>(b.h) / S.scale),
        fromOs(ImVec2(static_cast<float>(u.x), static_cast<float>(u.y))), ImVec2(static_cast<float>(u.w) / S.scale, static_cast<float>(u.h) / S.scale),
        static_cast<int>(i));
  }
}

bool isDetachedId(ImGuiID viewportId) { return viewportId != 0 && viewportId != ImGui::GetMainViewport()->ID; }

// The main window moved by `d` (ImGui units): ImGui's coordinates are relative to it, so the
// detached windows re-read where their OS windows are, and closed ones are shifted to stay put.
void mainMoved(ImVec2 d) {
  ImGuiContext& g = *GImGui;
  for (ImGuiViewportP* vp : g.Viewports)
    if (!isMain(vp) && vp->PlatformWindowCreated) vp->PlatformRequestMove = true;
  for (ImGuiWindow* w : g.Windows)
    if (!w->Active && !w->WasActive && isDetachedId(w->ViewportId)) {
      w->Pos = ImVec2(w->Pos.x - d.x, w->Pos.y - d.y);
      w->ViewportPos = ImVec2(w->ViewportPos.x - d.x, w->ViewportPos.y - d.y);
    }
  for (ImGuiWindowSettings* s = g.SettingsWindows.begin(); s != nullptr; s = g.SettingsWindows.next_chunk(s))
    if (!ImGui::FindWindowByID(s->ID) && isDetachedId(s->ViewportId))
      s->ViewportPos = ImVec2ih(static_cast<short>(std::lround(s->ViewportPos.x - d.x)), static_cast<short>(std::lround(s->ViewportPos.y - d.y)));
  const ImRect mr = mainRect();
  for (int i = 0; i < g.DockContext.Nodes.Data.Size; ++i) {
    auto* n = static_cast<ImGuiDockNode*>(g.DockContext.Nodes.Data[i].val_p);
    if (!n || !n->IsFloatingNode() || (n->HostWindow && n->HostWindow->WasActive)) continue;
    if (!mr.Contains(ImRect(n->Pos, ImVec2(n->Pos.x + n->Size.x, n->Pos.y + n->Size.y)))) n->Pos = ImVec2(n->Pos.x - d.x, n->Pos.y - d.y);
  }
}

// Once after layout.ini is loaded: detached windows saved relative to the main window of the
// last session are put back at the same OS position (recovered onto a monitor if theirs is gone).
void reanchorLoaded() {
  S.reanchorPending = false;
  ImGuiContext& g = *GImGui;
  const std::vector<place::Rect> mons = monitorRects();
  auto fix = [&](float& x, float& y, float w, float h) {
    place::reanchor(x, y, S.saved.x, S.saved.y, S.saved.scale, S.mainOs.x, S.mainOs.y, S.scale);
    const place::Rect r = place::recoverOnScreen({x, y, w, h}, mons);
    x = std::round(r.x), y = std::round(r.y);
  };
  for (ImGuiWindowSettings* s = g.SettingsWindows.begin(); s != nullptr; s = g.SettingsWindows.next_chunk(s)) {
    if (!isDetachedId(s->ViewportId) || s->IsChild) continue;
    float x = static_cast<float>(s->ViewportPos.x + s->Pos.x), y = static_cast<float>(s->ViewportPos.y + s->Pos.y);
    fix(x, y, static_cast<float>(s->Size.x), static_cast<float>(s->Size.y));
    s->ViewportPos = ImVec2ih(static_cast<short>(x), static_cast<short>(y));
    s->Pos = ImVec2ih(0, 0);
  }
  const ImRect mr = mainRect();
  for (int i = 0; i < g.DockContext.Nodes.Data.Size; ++i) {
    auto* n = static_cast<ImGuiDockNode*>(g.DockContext.Nodes.Data[i].val_p);
    if (!n || !n->IsFloatingNode()) continue;
    const ImRect nr(n->Pos, ImVec2(n->Pos.x + n->Size.x, n->Pos.y + n->Size.y));
    if (!mr.Contains(nr)) fix(n->Pos.x, n->Pos.y, n->Size.x, n->Size.y);
  }
}

// J-117: the title bar as ImGui laid it out and drew it in Begin() this frame. Not
// TitleBarRect(): that follows window->Pos, and a window moved after it was drawn (the old
// clamp of the channel rack did, every frame of a drag past the edge) would put the pop-out
// button where the title bar is not. InnerRect is set from the laid-out position in Begin() and
// never moved by SetWindowPos.
ImRect laidOutTitleBar(const ImGuiWindow* w) {
  const ImVec2 p(w->InnerRect.Min.x - w->DecoOuterSizeX1, w->InnerRect.Min.y - w->DecoOuterSizeY1);
  return ImRect(p, ImVec2(p.x + w->SizeFull.x, p.y + w->TitleBarHeight));
}

ImGuiWindow* findWindow(const std::string& name) {
  for (ImGuiWindow* w : GImGui->Windows)
    if (name == w->Name || name == ui::stableWindowName(w->Name)) return w;
  return nullptr;
}

bool isPanelRoot(const ImGuiWindow* w) {
  if (shortLived(w)) return false;
  if (w->DockId != 0 && !(w->Flags & ImGuiWindowFlags_DockNodeHost)) return false;  // docked: moves with its node
  return !(w->Flags & ImGuiWindowFlags_ChildWindow) || (w->Flags & ImGuiWindowFlags_DockNodeHost);
}

int viewportIndex(const ImGuiViewport* vp) {
  const ImGuiContext& g = *GImGui;
  for (int i = 0; i < g.Viewports.Size; ++i)
    if (g.Viewports[i] == vp) return i;
  return -1;
}

// --- dev: one detached window rendered into an image (its OS window is never read back) ---
void writeViewportShot(ImGuiViewport* vp, const std::string& path) {
  ImDrawData* dd = vp ? vp->DrawData : nullptr;
  if (!dd || !dd->Valid) return;
  using GenFB = void (*)(GLsizei, GLuint*);
  using BindFB = void (*)(GLenum, GLuint);
  using FBTex = void (*)(GLenum, GLenum, GLenum, GLuint, GLint);
  constexpr GLenum kFb = 0x8D40, kColor0 = 0x8CE0;
  auto gen = reinterpret_cast<GenFB>(SDL_GL_GetProcAddress("glGenFramebuffers"));
  auto bind = reinterpret_cast<BindFB>(SDL_GL_GetProcAddress("glBindFramebuffer"));
  auto attach = reinterpret_cast<FBTex>(SDL_GL_GetProcAddress("glFramebufferTexture2D"));
  auto del = reinterpret_cast<GenFB>(SDL_GL_GetProcAddress("glDeleteFramebuffers"));
  const int w = static_cast<int>(std::lround(dd->DisplaySize.x * dd->FramebufferScale.x));
  const int h = static_cast<int>(std::lround(dd->DisplaySize.y * dd->FramebufferScale.y));
  if (!gen || !bind || !attach || w <= 0 || h <= 0) return;
  GLuint tex = 0, fbo = 0;
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D, tex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  glBindTexture(GL_TEXTURE_2D, 0);
  gen(1, &fbo);
  bind(kFb, fbo);
  attach(kFb, kColor0, GL_TEXTURE_2D, tex, 0);
  glViewport(0, 0, w, h);
  glClearColor(0, 0, 0, 1);
  glClear(GL_COLOR_BUFFER_BIT);
  ImGui_ImplOpenGL3_RenderDrawData(dd);
  std::vector<unsigned char> px(static_cast<size_t>(w) * static_cast<size_t>(h) * 3);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, px.data());
  bind(kFb, 0);
  if (del) del(1, &fbo);
  glDeleteTextures(1, &tex);
  if (FILE* f = std::fopen(path.c_str(), "wb")) {
    std::fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int y = h - 1; y >= 0; --y) std::fwrite(px.data() + static_cast<size_t>(y) * static_cast<size_t>(w) * 3, 1, static_cast<size_t>(w) * 3, f);
    std::fclose(f);
  }
}

void dump(int frame) {
  ImGuiContext& g = *GImGui;
  double ms = 0;
  const int n = std::min(S.frameN, 120);
  double wms = 0;
  for (int i = 0; i < n; ++i) ms += S.frameMs[i], wms += S.windowsMs[i];
  std::printf("viewports frame %d: available %d enabled %d detached %d mainos %.0f %.0f scale %.3f monitors %d frame-ms %.3f windows-ms %.3f\n", frame,
              S.available ? 1 : 0, enabled() ? 1 : 0, detachedCount(), static_cast<double>(S.mainOs.x), static_cast<double>(S.mainOs.y),
              static_cast<double>(S.scale), g.PlatformIO.Monitors.Size, n > 0 ? ms / n : 0.0, n > 0 ? wms / n : 0.0);
  if (!S.available) std::printf("  unavailable: %s\n", S.reason.c_str());
  for (int i = 0; i < g.PlatformIO.Monitors.Size; ++i) {
    const ImGuiPlatformMonitor& m = g.PlatformIO.Monitors[i];
    std::printf("  monitor %d pos %.1f %.1f size %.1f %.1f\n", i, static_cast<double>(m.WorkPos.x), static_cast<double>(m.WorkPos.y),
                static_cast<double>(m.WorkSize.x), static_cast<double>(m.WorkSize.y));
  }
  for (int i = 0; i < g.Viewports.Size; ++i) {
    const ImGuiViewportP* vp = g.Viewports[i];
    SDL_Window* w = isMain(vp) ? S.main : sdlOf(vp);
    int x = 0, y = 0, ww = 0, hh = 0, resizable = 0, decorated = 0;
    if (w) {
      SDL_GetWindowPosition(w, &x, &y), SDL_GetWindowSize(w, &ww, &hh);
      resizable = (SDL_GetWindowFlags(w) & SDL_WINDOW_RESIZABLE) ? 1 : 0;
      decorated = (SDL_GetWindowFlags(w) & SDL_WINDOW_BORDERLESS) ? 0 : 1;
    }
    std::printf("  viewport %d window \"%s\" pos %.1f %.1f size %.1f %.1f fb %.2f platform %d os %d %d %d %d resizable %d decorated %d\n", i,
                vp->Window ? nameOf(vp->Window) : "(main)", static_cast<double>(vp->Pos.x), static_cast<double>(vp->Pos.y),
                static_cast<double>(vp->Size.x), static_cast<double>(vp->Size.y), static_cast<double>(vp->FramebufferScale.x),
                vp->PlatformWindowCreated ? 1 : 0, x, y, ww, hh, resizable, decorated);
  }
  for (ImGuiWindow* w : g.Windows) {
    if (!w->WasActive || std::strchr(w->Name, '/') || shortLived(w)) continue;
    if ((w->Flags & ImGuiWindowFlags_ChildWindow) && !w->DockIsActive && !(w->Flags & ImGuiWindowFlags_DockNodeHost)) continue;
    // "laid": where it was laid out and drawn this frame (J-117: differs from pos when something
    // moved it after it was drawn).
    const ImRect laid = laidOutTitleBar(w);
    // J-158: "content": the size its content wants (ImGui's ideal content size), "scroll": its scroll range.
    std::printf("  window \"%s\" pos %.1f %.1f size %.1f %.1f viewport %d docked %d laid %.0f %.0f content %.0f %.0f scroll %.0f %.0f\n", nameOf(w),
                static_cast<double>(w->Pos.x), static_cast<double>(w->Pos.y), static_cast<double>(w->Size.x), static_cast<double>(w->Size.y),
                viewportIndex(w->Viewport), w->DockIsActive ? 1 : 0, static_cast<double>(laid.Min.x), static_cast<double>(laid.Min.y),
                static_cast<double>(w->ContentSizeIdeal.x), static_cast<double>(w->ContentSizeIdeal.y), static_cast<double>(w->ScrollMax.x),
                static_cast<double>(w->ScrollMax.y));
  }
  std::fflush(stdout);
}

// ---- the pop-out / dock-back button ----

// One small monochrome glyph, ImGui's close-button weight: a window frame with an arrow leaving
// it to the upper right (pop out) or coming into it (dock back in). SVG icons
// (window-popout / window-dockin) on the old glyph's grid: the box spans -1..1.
void drawGlyph(ImDrawList* dl, const ImRect& bb, bool dockIn, ImU32 col) {
  const float s = bb.GetWidth();
  const float t = std::max(1.0f, std::round(s / 13.0f));
  ui::drawIconGlyph(dl, dockIn ? ui::Icon::WindowDockin : ui::Icon::WindowPopout, bb.GetCenter(), s * 0.5f, t, col);
}

const char* buttonTip(bool out) { return out ? i18n::tr("Dock back into Gridline") : i18n::tr("Pop out into its own window"); }

void paintButton(ImDrawList* dl, const ImRect& bb, ImU32 base, bool hovered, bool held, bool out) {
  ImGuiContext& g = *GImGui;
  dl->PushClipRect(ImVec2(bb.Min.x - 1, bb.Min.y - 1), ImVec2(bb.Max.x + 1, bb.Max.y + 1), false);
  dl->AddRectFilled(ImVec2(bb.Min.x - 1, bb.Min.y - 1), ImVec2(bb.Max.x + 1, bb.Max.y + 1), base);
  if (hovered || held) dl->AddRectFilled(bb.Min, bb.Max, ImGui::GetColorU32(held ? ImGuiCol_ButtonActive : ImGuiCol_ButtonHovered), g.Style.FrameRounding);
  drawGlyph(dl, bb, out, ImGui::GetColorU32(ImGuiCol_Text));
  dl->PopClipRect();
}

// Docked: drawn over the dock node's window-menu button (its click comes to windowMenu).
void dockedButton(ImGuiWindow* w) {
  ImGuiContext& g = *GImGui;
  const ImGuiStyle& st = g.Style;
  ImGuiDockNode* n = w->DockNode;
  if (!n || n->VisibleWindow != w || !n->HasWindowMenuButton || n->IsHiddenTabBar() || n->IsNoTabBar() || !n->HostWindow) return;
  const float sz = g.FontSize;
  float maxX = n->Pos.x + n->Size.x - st.WindowBorderSize - st.FramePadding.x;
  if (n->HasCloseButton) maxX -= sz + st.ItemInnerSpacing.x;
  const ImVec2 p(maxX - sz, n->Pos.y + st.FramePadding.y);
  const ImRect bb(p, ImVec2(p.x + sz, p.y + sz));
  const ImGuiID id = ImGui::DockNodeGetWindowMenuButtonId(n);
  const bool hovered = g.HoveredId == id, held = g.ActiveId == id;
  const bool out = isOut(w->RootWindowDockTree);
  ImGuiWindow* host = n->HostWindow;
  paintButton(host->DrawList, bb, ImGui::GetColorU32(host->Collapsed ? ImGuiCol_TitleBgCollapsed : n->IsFocused ? ImGuiCol_TitleBgActive : ImGuiCol_TitleBg),
              hovered, held, out);
  if (hovered && g.HoveredIdTimer > 0.5f) ImGui::SetTooltip("%s", buttonTip(out));
  S.buttons[nameOf(w)] = bb;
}

// Floating (inside or outside): an extra title-bar button left of the close button.
void floatingButton(ImGuiWindow* w) {
  ImGuiContext& g = *GImGui;
  const ImGuiStyle& st = g.Style;
  const ImRect tb = laidOutTitleBar(w);
  const float sz = g.FontSize;
  float maxX = tb.Max.x - st.FramePadding.x;
  if (w->HasCloseButton) maxX -= sz + st.ItemInnerSpacing.x;
  const ImVec2 p(maxX - sz, tb.Min.y + st.FramePadding.y);
  const ImRect bb(p, ImVec2(p.x + sz, p.y + sz));
  if (bb.Min.x < tb.Min.x + sz * 3) return;  // too narrow for it
  const bool out = isOut(w->RootWindowDockTree);
  ImGui::Begin(w->Name);  // append to the window (its title bar is its own: hover and clicks are)
  ImGui::PushClipRect(tb.Min, tb.Max, false);
  const ImGuiID id = w->GetID("#GLPOPOUT");
  const ImGuiItemFlags backup = g.CurrentItemFlags;
  g.CurrentItemFlags |= ImGuiItemFlags_NoNavDefaultFocus;
  w->DC.NavLayerCurrent = ImGuiNavLayer_Menu;
  bool hovered = false, held = false, pressed = false;
  if (ImGui::ItemAdd(bb, id)) pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held);
  const bool tip = hovered && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip);
  w->DC.NavLayerCurrent = ImGuiNavLayer_Main;
  g.CurrentItemFlags = backup;
  const bool lit = g.NavWindow && w->RootWindowForTitleBarHighlight == g.NavWindow->RootWindowForTitleBarHighlight;
  paintButton(w->DrawList, bb, ImGui::GetColorU32(w->Collapsed ? ImGuiCol_TitleBgCollapsed : lit ? ImGuiCol_TitleBgActive : ImGuiCol_TitleBg), hovered,
              held, out);
  ImGui::PopClipRect();
  ImGui::End();
  if (tip) ImGui::SetTooltip("%s", buttonTip(out));
  if (pressed) S.toggles.push_back(w->ID);
  S.buttons[nameOf(w)] = bb;
}

bool buttonsOn() { return S.installed && enabled() && S.disableIn < 0; }

void freezeDetachedMove() {
  ImGuiContext& g = *GImGui;
  if (g.MovingWindow && isOut(g.MovingWindow->RootWindowDockTree)) g.MovingWindow = nullptr;
}

}  // namespace

void install(SDL_Window* mainWindow, SDL_GLContext gl, const Options& o) {
  ImGuiIO& io = ImGui::GetIO();
  ImGuiPlatformIO& pio = ImGui::GetPlatformIO();
  S.main = mainWindow;
  S.gl = gl;
  S.hidden = o.hidden;
  const char* driver = SDL_GetCurrentVideoDriver();
  const bool wayland = driver && std::strcmp(driver, "wayland") == 0;
  S.available = !wayland && (io.BackendFlags & ImGuiBackendFlags_PlatformHasViewports) && (io.BackendFlags & ImGuiBackendFlags_RendererHasViewports) &&
                pio.Platform_CreateWindow && pio.Platform_GetWindowPos;
  if (wayland)
    S.reason = i18n::N_("Not available on Wayland. Start Gridline with SDL_VIDEODRIVER=x11 to use it through XWayland.");
  else if (!S.available)
    S.reason = i18n::N_("Not available with this display system.");
  // J-114: detached windows are ordinary decorated OS windows (the OS moves and resizes them);
  // popups, menus and tooltips stay undecorated (ImGui does that for short-lived windows).
  io.ConfigViewportsNoDecoration = false;
  io.ConfigViewportsNoTaskBarIcon = true;  // one taskbar entry: the main window
  if (S.available) {
    S.createWindow = pio.Platform_CreateWindow, pio.Platform_CreateWindow = wCreate;
    S.destroyWindow = pio.Platform_DestroyWindow, pio.Platform_DestroyWindow = wDestroy;
    S.showWindow = pio.Platform_ShowWindow, pio.Platform_ShowWindow = wShow;
    S.setPos = pio.Platform_SetWindowPos, pio.Platform_SetWindowPos = wSetPos;
    S.getPos = pio.Platform_GetWindowPos, pio.Platform_GetWindowPos = wGetPos;
    S.setSize = pio.Platform_SetWindowSize, pio.Platform_SetWindowSize = wSetSize;
    S.getSize = pio.Platform_GetWindowSize, pio.Platform_GetWindowSize = wGetSize;
    S.getFbScale = pio.Platform_GetWindowFramebufferScale, pio.Platform_GetWindowFramebufferScale = wFbScale;
    S.setFocus = pio.Platform_SetWindowFocus, pio.Platform_SetWindowFocus = wSetFocus;
    S.getFocus = pio.Platform_GetWindowFocus, pio.Platform_GetWindowFocus = wGetFocus;
    GImGui->ViewportLateCreateFilter = lateCreate;
    GImGui->CloseRequestFilter = closeFilter;
    GImGui->DockNodeWindowMenuHandler = windowMenu;
  }
  static ImGuiSettingsHandler h;
  h.TypeName = "GridlineViewports";
  h.TypeHash = ImHashStr("GridlineViewports");
  h.ClearAllFn = iniClear;
  h.ReadOpenFn = iniOpen;
  h.ReadLineFn = iniLine;
  h.ApplyAllFn = iniApply;
  h.WriteAllFn = iniWrite;
  ImGui::AddSettingsHandler(&h);
  if (traceOn()) SDL_AddEventWatch(traceWatch, nullptr);
  if (SDL_Window* k = SDL_GetKeyboardFocus(); k == mainWindow) S.focusId = SDL_GetWindowID(mainWindow);
  S.installed = true;
}

void setVirtualMonitors(bool v) { S.virtualMonitors = v; }
bool available() { return S.available; }
std::string unavailableReason() { return S.reason; }
bool enabled() { return S.available && on(); }

void setAllowed(bool allowed) {
  if (!S.installed) return;
  ImGuiIO& io = ImGui::GetIO();
  const bool want = allowed && S.available;
  if (want) {
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    S.disableIn = -1;
  } else if (on()) {
    if (S.disableIn < 0) {  // bring everything back, let the windows dock / merge into the main one, then switch off
      gatherAll();
      S.disableIn = 4;
    } else if (--S.disableIn <= 0) {
      io.ConfigFlags &= ~ImGuiConfigFlags_ViewportsEnable;  // the OS windows go with their viewports
      S.disableIn = -1;
    }
  }
}

void beginFrame(float uiScale, ImVec2 mainLogicalSize) {
  if (!S.installed) return;
  ImGuiStyle& style = ImGui::GetStyle();
  style.WindowMenuButtonPosition = ImGuiDir_None;
  S.scale = uiScale > 0 ? uiScale : 1.0f;
  S.display = mainLogicalSize;
  int x = 0, y = 0;
  SDL_GetWindowPosition(S.main, &x, &y);
  const ImVec2 nowPos(static_cast<float>(x), static_cast<float>(y));
  if (S.haveMainOs && on() && (nowPos.x != S.mainOs.x || nowPos.y != S.mainOs.y))
    mainMoved(ImVec2((nowPos.x - S.mainOs.x) / S.scale, (nowPos.y - S.mainOs.y) / S.scale));
  S.mainOs = nowPos;
  S.haveMainOs = true;
  if (!on()) {
    if (S.reanchorPending) {  // detaching off (or unavailable): windows saved outside come back in
      S.reanchorPending = false;
      gatherAll();
    }
    return;
  }
  if (S.monDirty || seconds(S.monReadAt, now()) > 2.0) readMonitors();
  publishMonitors();
  // The backend reports the pointer in OS coordinates while viewports are on: main-window
  // relative here, divided by the interface scale by main.cpp like every pointer position.
  ImGuiContext& g = *GImGui;
  for (ImGuiInputEvent& e : g.InputEventsQueue)
    if (e.EventId > S.lastEvent && e.Type == ImGuiInputEventType_MousePos && e.MousePos.PosX > -FLT_MAX * 0.5f) {
      e.MousePos.PosX -= S.mainOs.x;
      e.MousePos.PosY -= S.mainOs.y;
    }
  S.lastEvent = g.InputEventsNextEventId - 1;
  if (traceOn()) traceFrame();
  // --hidden: no window is under the pointer as far as the OS knows; ImGui finds it itself.
  if (S.hidden) ImGui::GetIO().BackendFlags &= ~ImGuiBackendFlags_HasMouseHoveredViewport;
  if (S.reanchorPending) reanchorLoaded();
  snapshotHomes();
  // J-116: a detached window is moved by its OS title bar only. Grabbed by its own (ImGui) title
  // bar or tab it never follows the pointer, never shows dock targets and never docks: ImGui's
  // move is dropped (the title bar's id stays active until the release; nothing happens then).
  freezeDetachedMove();
  // J-116: the OS close button (or Cmd+W) of a detached panel docks it back, like the dock-back
  // button; it never closes the panel. (Other windows, a dialog say, close as ImGui does.)
  for (ImGuiViewportP* vp : g.Viewports) {
    if (isMain(vp) || !vp->PlatformRequestClose || !owned(vp)) continue;
    ImGuiWindow* w = vp->Window;
    if (!isPanel(w) && !w->DockNodeAsHost) continue;
    vp->PlatformRequestClose = false;
    S.closeReqs.push_back(w->ID);
  }
  if (buttonsOn()) style.WindowMenuButtonPosition = ImGuiDir_Right;  // floating dock nodes update in NewFrame
}

void afterNewFrame() {
  if (!S.installed) return;
  ImGui::GetStyle().WindowMenuButtonPosition = ImGuiDir_None;
  ImGuiContext& g = *GImGui;
  S.lastEvent = g.InputEventsNextEventId - 1;  // queued later (drags, dev input): never converted
  for (auto it = S.appeared.begin(); it != S.appeared.end();) it = g.FrameCount - it->second > 8 ? S.appeared.erase(it) : std::next(it);
  for (ImGuiWindow* w : g.Windows)
    if (w->Appearing && w->WasActive) S.appeared[w->ID] = g.FrameCount - 1;  // appeared last frame (lateCreate)
  if (!enabled()) {
    S.toggles.clear(), S.closeReqs.clear(), S.pendingOut.clear();
    return;
  }
  const ImRect mr = mainRect();
  freezeDetachedMove();  // (a move started during NewFrame)
  // Popped out while docked: placed once ImGui has undocked it.
  for (auto it = S.pendingOut.begin(); it != S.pendingOut.end();) {
    ImGuiWindow* w = ImGui::FindWindowByID(it->id);
    if (w && w->DockId == 0 && !w->DockIsActive) {
      placeOut(w->RootWindowDockTree, it->pos, it->size, true);
      it = S.pendingOut.erase(it);
    } else if (!w || --it->frames <= 0) {
      it = S.pendingOut.erase(it);
    } else {
      ++it;
    }
  }
  for (ImGuiID id : S.closeReqs)
    if (ImGuiWindow* w = ImGui::FindWindowByID(id); w && isOut(w->RootWindowDockTree)) dockHome(w);
  S.closeReqs.clear();
  std::vector<ImGuiID> t;
  t.swap(S.toggles);
  for (ImGuiID id : t) toggle(ImGui::FindWindowByID(id));
  // Detached windows (also those restored from layout.ini) keep wanting their own OS window; one
  // docked back into the main window never does again (it would detach the moment it is undocked).
  ImGuiViewportP* mainVp = static_cast<ImGuiViewportP*>(ImGui::GetMainViewport());
  for (auto it = S.homing.begin(); it != S.homing.end();) {
    ImGuiWindow* w = ImGui::FindWindowByID(it->first);
    if (!w || (w->RootWindowDockTree->Viewport && isMain(w->RootWindowDockTree->Viewport)) || !w->WasActive) {
      it = S.homing.erase(it);
      continue;
    }
    // Back inside, floating: into the main window now (ImGui's own merge waits for no widget to be
    // active and for no other OS window, a tooltip say, to overlap it).
    ImGuiWindow* r = w->RootWindowDockTree;
    ImGuiViewportP* old = r->Viewport;
    if (old && old->Window == r && r->DockId == 0 && mainRect().Contains(r->Rect())) {
      for (ImGuiWindow* c : g.Windows)
        if (c->Viewport == old) ImGui::SetWindowViewport(c, mainVp);
      r->ViewportOwned = false;
    }
    ++it;
  }
  for (ImGuiViewportP* vp : g.Viewports) {
    ImGuiWindow* w = vp->Window;
    if (!isMain(vp) && owned(vp) && (isPanel(w) || w->DockNodeAsHost) && !S.homing.count(w->ID) && !flagged(w)) setDetachedFlag(w, true);
  }
  for (ImGuiWindow* w : g.Windows) {
    if (!flagged(w) || !w->DockIsActive) continue;
    ImGuiWindow* r = w->RootWindowDockTree;
    if (w->DockNode && nodeInMain(w->DockNode) && (!r->Viewport || isMain(r->Viewport))) setDetachedFlag(w, false);
  }
  // A detached editor reopened: editors open centred over the main window; it goes back to where
  // it was outside instead.
  for (ImGuiWindow* w : g.Windows) {
    if (!w->Appearing || !w->WasActive || !flagged(w) || w->DockIsActive || !isPanel(w)) continue;
    const auto it = S.lastOut.find(nameOf(w));
    if (it == S.lastOut.end() || !mr.Contains(ImVec2(w->Pos.x + w->Size.x * 0.5f, w->Pos.y + w->Size.y * 0.5f))) continue;
    const ImVec2 p = fromOs(ImVec2(it->second.x, it->second.y));
    placeOut(w, p, w->SizeFull, false);
  }
}

void beginDockSpace(ImGuiID dockspace) {
  S.dockspace = dockspace;
  if (buttonsOn()) ImGui::GetStyle().WindowMenuButtonPosition = ImGuiDir_Right;
}
void endDockSpace() { ImGui::GetStyle().WindowMenuButtonPosition = ImGuiDir_None; }

void drawTitleButtons() {
  S.buttons.clear();
  if (!buttonsOn()) return;
  ImGuiContext& g = *GImGui;
  std::vector<ImGuiWindow*> list(g.Windows.begin(), g.Windows.end());
  for (ImGuiWindow* w : list) {
    if (!w->Active || w->Hidden || w->SkipItems || !isPanel(w)) continue;
    if (w->DockIsActive) dockedButton(w);
    else if (!(w->Flags & ImGuiWindowFlags_NoTitleBar)) floatingButton(w);
  }
}

void renderWindows() {
  if (!S.installed) return;
  const Uint64 t = now();
  const int slot = S.frameN % 120;
  if (S.lastRender) S.frameMs[slot] = seconds(S.lastRender, t) * 1000.0, S.windowsMs[slot] = 0, ++S.frameN;
  S.lastRender = t;
  if (!(GImGui->ConfigFlagsCurrFrame & ImGuiConfigFlags_ViewportsEnable)) return;
  ImGui::UpdatePlatformWindows();
  for (ImGuiViewportP* vp : GImGui->Viewports)
    if (!isMain(vp) && owned(vp) && vp->PlatformWindowCreated && !S.homing.count(vp->Window->ID))
      rememberOut(vp->Window);  // not once it is docking back
  for (const auto& [name, path] : S.shots) {
    ImGuiWindow* w = findWindow(name);
    ImGuiViewport* vp = w ? w->RootWindowDockTree->Viewport : nullptr;
    if (vp && !isMain(vp)) writeViewportShot(vp, path);
    std::printf("vpshot %s: %s\n", name.c_str(), vp && !isMain(vp) ? path.c_str() : "not detached");
    std::fflush(stdout);
  }
  S.shots.clear();
  if (GImGui->Viewports.Size > 1) {
    // Each detached window draws with its own GL context (shared with the main one), the
    // backend's standard path; the main window's context is made current again afterwards.
    ImGui::RenderPlatformWindowsDefault();
    SDL_GL_MakeCurrent(S.main, S.gl);
    ++S.drawnFrames;
    for (ImGuiViewportP* vp : GImGui->Viewports)  // dev (J-116): the size each one was drawn at
      if (!isMain(vp) && owned(vp) && vp->DrawData && vp->DrawData->Valid) S.drawn[nameOf(keyWindow(vp->Window))] = vp->DrawData->DisplaySize;
  }
  S.windowsMs[slot] = seconds(t, now()) * 1000.0;
}

void onEvent(const SDL_Event& e) {
  if (!S.installed) return;
  if (e.type == SDL_DISPLAYEVENT) S.monDirty = true;
  if (e.type != SDL_WINDOWEVENT) return;
  const Uint32 id = e.window.windowID, mainId = SDL_GetWindowID(S.main);
  const bool ours = id == mainId || S.ours.count(id) > 0;
  switch (e.window.event) {
    case SDL_WINDOWEVENT_FOCUS_GAINED:
      S.focusId = ours ? id : 0;
      if (ours) S.lostArmed = false;
#if defined(__APPLE__)
      // macOS: the main window came forward; the detached windows stay above it (not AppKit child
      // windows: those would be dragged along whenever the main window moves).
      if (id == mainId && !S.hidden && ImGui::GetCurrentContext())
        for (ImGuiViewportP* vp : GImGui->Viewports)
          if (!isMain(vp) && owned(vp))
            if (SDL_Window* w = sdlOf(vp)) host::native::orderAbove(nativeOf(w), nativeOf(S.main));
#endif
      break;
    case SDL_WINDOWEVENT_FOCUS_LOST:
      if (ours && (S.focusId == id || S.focusId == 0)) {
        S.focusId = 0;
        S.lostArmed = true;
        S.lostAt = now();
      }
      break;
    case SDL_WINDOWEVENT_MINIMIZED:  // the main window minimised: its detached windows go with it
      if (id == mainId && !S.hidden)
        for (ImGuiViewportP* vp : GImGui->Viewports)
          if (SDL_Window* w = isMain(vp) ? nullptr : sdlOf(vp); w && (SDL_GetWindowFlags(w) & SDL_WINDOW_SHOWN)) {
            SDL_HideWindow(w);
            S.hiddenForMinimize.push_back(SDL_GetWindowID(w));
          }
      break;
    case SDL_WINDOWEVENT_RESTORED:
    case SDL_WINDOWEVENT_MAXIMIZED:
    case SDL_WINDOWEVENT_SHOWN:
      if (id == mainId && !S.hiddenForMinimize.empty()) {
        SDL_SetHint(SDL_HINT_WINDOW_NO_ACTIVATION_WHEN_SHOWN, "1");
        for (Uint32 wid : S.hiddenForMinimize)
          if (SDL_Window* w = SDL_GetWindowFromID(wid)) SDL_ShowWindow(w);
        S.hiddenForMinimize.clear();
        SDL_SetHint(SDL_HINT_WINDOW_NO_ACTIVATION_WHEN_SHOWN, "0");
      }
      break;
    default: break;
  }
}

bool takeAppFocusLost() {
  if (!S.lostArmed) return false;
  if (S.focusId != 0) {
    S.lostArmed = false;
    return false;
  }
  // Moving between Gridline's windows is "lost" then "gained", not always in the same batch of events.
  if (detachedCount() > 0 && seconds(S.lostAt, now()) < 0.08) return false;
  S.lostArmed = false;
  return true;
}

Uint32 sdlWindowOf(const std::string& name) {
  if (!S.installed || !ImGui::GetCurrentContext()) return 0;
  if (name == "main") return SDL_GetWindowID(S.main);
  ImGuiWindow* w = findWindow(name);
  const ImGuiViewport* vp = w && w->RootWindowDockTree ? w->RootWindowDockTree->Viewport : nullptr;
  if (!vp) return 0;
  SDL_Window* sw = isMain(vp) ? S.main : sdlOf(vp);
  return sw ? SDL_GetWindowID(sw) : 0;
}

bool ownsWindow(Uint32 sdlId) { return S.installed && sdlId != 0 && S.ours.count(sdlId) > 0; }

std::vector<Uint32> windowIds() { return std::vector<Uint32>(S.ours.begin(), S.ours.end()); }

int detachedCount() {
  if (!S.installed || !ImGui::GetCurrentContext()) return 0;
  int n = 0;
  for (const ImGuiViewportP* vp : GImGui->Viewports)
    if (!isMain(vp) && vp->PlatformWindowCreated && owned(vp)) ++n;
  return n;
}

// Plugin windows open inside the main window. ImGui would otherwise put a window back in the
// viewport it had: one restored from layout.ini (ApplyWindowSettings: its saved ViewportId and
// ViewportPos make WindowSelectViewport create an OS window for it), or one closed while popped
// out (it keeps its ViewportId, its position outside and the NoAutoMerge class flag), and the
// end of afterNewFrame would then flag it detached again. Called before its Begin(): it is
// still drawn as last frame left it when it was open last frame.
void openAttached(const char* name) {
  if (!S.installed || !name || !ImGui::GetCurrentContext()) return;
  ImGuiViewportP* m = static_cast<ImGuiViewportP*>(ImGui::GetMainViewport());
  const place::Rect area = workArea();
  auto centred = [&](float w, float h) {
    return ImVec2(std::round(area.x + std::max(0.0f, (area.w - w) * 0.5f)), std::round(area.y + std::max(0.0f, (area.h - h) * 0.5f)));
  };
  if (ImGuiWindow* w = ImGui::FindWindowByName(name)) {
    if (w->Active || w->WasActive) return;  // open: popped out stays popped out
    const bool out = flagged(w) || (w->Viewport && !isMain(w->Viewport)) || isDetachedId(w->ViewportId) || !mainRect().Contains(w->Rect());
    if (!out) return;
    setDetachedFlag(w, false);
    S.homing.erase(w->ID);
    if (w->Viewport && !isMain(w->Viewport)) ImGui::SetWindowViewport(w, m);
    w->ViewportId = m->ID;
    w->ViewportPos = m->Pos;
    w->ViewportOwned = false;
    w->Pos = centred(w->SizeFull.x, w->SizeFull.y);  // inside, so ImGui keeps it there (it centres itself as it appears)
    return;
  }
  // Not created yet this run: a layout.ini that saved it outside is not followed.
  ImGuiWindowSettings* s = ImGui::FindWindowSettingsByID(ImHashStr(name));
  if (!s || !isDetachedId(s->ViewportId)) return;
  const ImVec2 p = centred(s->Size.x, s->Size.y);
  s->ViewportId = m->ID;
  s->ViewportPos = ImVec2ih(0, 0);
  s->Pos = ImVec2ih(static_cast<short>(p.x), static_cast<short>(p.y));
}

void gatherAll() {
  if (!ImGui::GetCurrentContext()) return;
  ImGuiContext& g = *GImGui;
  ImGuiViewport* m = ImGui::GetMainViewport();
  // Open detached windows dock back where they were.
  std::vector<ImGuiWindow*> out;
  for (ImGuiViewportP* vp : g.Viewports)
    if (!isMain(vp) && owned(vp)) out.push_back(vp->Window);
  for (ImGuiWindow* w : out) dockHome(w);
  // Closed ones (and windows outside the main window in it) come back inside, cascaded.
  const place::Rect area = workArea();
  const ImRect mr = mainRect();
  int slot = 0;
  for (ImGuiWindow* w : g.Windows) {
    if (!isPanelRoot(w) || (w->Flags & ImGuiWindowFlags_NoMove) || S.homing.count(w->ID)) continue;
    const bool active = w->WasActive || w->Active;
    const bool outside = (!active && (isDetachedId(w->ViewportId) || flagged(w))) || (active && isMain(w->Viewport) && !mr.Contains(w->Rect()));
    if (!outside) continue;
    setDetachedFlag(w, false);
    const place::Rect r = place::cascade(slot++, w->SizeFull.x, w->SizeFull.y, area);
    const bool fixed = (w->Flags & ImGuiWindowFlags_AlwaysAutoResize) != 0;
    if (active) {
      ImGui::SetWindowPos(w, ImVec2(r.x, r.y), ImGuiCond_Always);
      if (!fixed) ImGui::SetWindowSize(w, ImVec2(r.w, r.h), ImGuiCond_Always);
    } else {
      w->Pos = ImVec2(r.x, r.y);
      w->ViewportPos = m->Pos;
      w->ViewportId = m->ID;
      if (!fixed) w->Size = w->SizeFull = ImVec2(r.w, r.h);
    }
    if (ImGuiDockNode* n = w->DockNodeAsHost) n->Pos = ImVec2(r.x, r.y), n->Size = fixed ? n->Size : ImVec2(r.w, r.h);
  }
  for (ImGuiWindowSettings* s = g.SettingsWindows.begin(); s != nullptr; s = g.SettingsWindows.next_chunk(s)) {
    if (s->IsChild || ImGui::FindWindowByID(s->ID) || !isDetachedId(s->ViewportId)) continue;
    const place::Rect r = place::cascade(slot++, s->Size.x, s->Size.y, area);
    s->ViewportId = m->ID;
    s->ViewportPos = ImVec2ih(0, 0);
    s->Pos = ImVec2ih(static_cast<short>(r.x), static_cast<short>(r.y));
    if (s->Size.x > 0 && s->Size.y > 0) s->Size = ImVec2ih(static_cast<short>(r.w), static_cast<short>(r.h));
  }
  for (int i = 0; i < g.DockContext.Nodes.Data.Size; ++i) {
    auto* n = static_cast<ImGuiDockNode*>(g.DockContext.Nodes.Data[i].val_p);
    if (!n || !n->IsFloatingNode() || (n->HostWindow && (n->HostWindow->WasActive || n->HostWindow->Active))) continue;
    if (mr.Contains(ImRect(n->Pos, ImVec2(n->Pos.x + n->Size.x, n->Pos.y + n->Size.y)))) continue;
    const place::Rect r = place::cascade(slot++, n->Size.x, n->Size.y, area);
    n->Pos = ImVec2(r.x, r.y);
    n->Size = ImVec2(r.w, r.h);
  }
}

bool devCommand(App& a, const std::string& cmd, const std::vector<std::string>& args, int frame) {
  auto num = [&](size_t i) { return i < args.size() ? std::strtof(args[i].c_str(), nullptr) : 0.0f; };
  auto window = [&]() -> ImGuiWindow* {
    ImGuiWindow* w = args.empty() ? nullptr : findWindow(args[0]);
    if (!w) std::printf("%s frame %d: no window \"%s\"\n", cmd.c_str(), frame, args.empty() ? "" : args[0].c_str());
    return w;
  };
  auto viewportOf = [&](ImGuiWindow* w) -> ImGuiViewport* {
    ImGuiViewport* vp = w ? w->RootWindowDockTree->Viewport : nullptr;
    if (!vp || isMain(vp)) {
      std::printf("%s frame %d: \"%s\" is not detached\n", cmd.c_str(), frame, args.empty() ? "" : args[0].c_str());
      return nullptr;
    }
    return vp;
  };
  auto push = [](Uint32 wid, Uint8 ev) {
    SDL_Event e{};
    e.type = SDL_WINDOWEVENT;
    e.window.event = ev;
    e.window.windowID = wid;
    SDL_PushEvent(&e);
  };
  if (cmd == "viewports") {
    dump(frame);
  } else if (cmd == "detach") {  // detach <window> x y [w h]: popped out with its top-left at x y (main-window logical px), a test set-up
    if (ImGuiWindow* w = window()) {
      if (w->DockId != 0) ImGui::DockContextQueueUndockWindow(GImGui, w);
      const ImVec2 size = num(3) > 0 && num(4) > 0 ? ImVec2(num(3), num(4)) : w->SizeFull;
      S.pendingOut.push_back({w->ID, ImVec2(num(1), num(2)), size, 10});
    }
  } else if (cmd == "toggle" || cmd == "popout" || cmd == "dockback") {  // the title-bar button's action (popout / dockback: only that way)
    if (ImGuiWindow* w = window()) {
      const bool out = isOut(w->RootWindowDockTree);
      if (cmd == "toggle" || (cmd == "popout") != out) S.toggles.push_back(w->ID);
    }
  } else if (cmd == "titlebtn") {  // titlebtn <window>: where its pop-out / dock-back button is (main-window logical px of its OS window's space)
    const std::string n = args.empty() ? "" : args[0];
    const auto it = S.buttons.find(n);
    ImGuiWindow* w = findWindow(n);
    if (it == S.buttons.end() || !w)
      std::printf("titlebtn frame %d: %s none\n", frame, n.c_str());
    else
      std::printf("titlebtn frame %d: %s x %.0f y %.0f size %.0f state %s\n", frame, n.c_str(), static_cast<double>(it->second.Min.x),
                  static_cast<double>(it->second.Min.y), static_cast<double>(it->second.GetWidth()), isOut(w->RootWindowDockTree) ? "dockin" : "popout");
  } else if (cmd == "closebtn") {
    // closebtn <window>: where its own (ImGui) title-bar close button was drawn this frame, as
    // titlebtn; "window" is where the window was laid out and "now" where it is (J-117: they
    // differ when something moved it after it was drawn).
    ImGuiWindow* w = findWindow(args.empty() ? "" : args[0]);
    if (!w || !w->HasCloseButton || w->DockIsActive || (w->Flags & ImGuiWindowFlags_NoTitleBar)) {
      std::printf("closebtn frame %d: %s none\n", frame, args.empty() ? "" : args[0].c_str());
    } else {
      const ImGuiStyle& st = ImGui::GetStyle();
      const ImRect tb = laidOutTitleBar(w);
      const float sz = ImGui::GetFontSize();
      std::printf("closebtn frame %d: %s x %.0f y %.0f size %.0f window %.0f %.0f now %.0f %.0f\n", frame, args[0].c_str(),
                  static_cast<double>(tb.Max.x - st.FramePadding.x - sz), static_cast<double>(tb.Min.y + st.FramePadding.y), static_cast<double>(sz),
                  static_cast<double>(tb.Min.x), static_cast<double>(tb.Min.y), static_cast<double>(w->Pos.x), static_cast<double>(w->Pos.y));
    }
  } else if (cmd == "osmove") {  // osmove <window> x y: the OS moves the detached window (its OS title bar dragged) so its client area's top-left is at x y
    if (ImGuiViewport* vp = viewportOf(window())) {
      const ImVec2 p(num(1), num(2));
      if (S.hidden) S.virt[vp].pos = p;
      else S.setPos(vp, rounded(toOs(p)));
      vp->PlatformRequestMove = true;  // as the OS reports a user move (SDL_WINDOWEVENT_MOVED)
    }
  } else if (cmd == "osclose") {  // osclose <window>: its OS close button / Cmd+W (SDL_WINDOWEVENT_CLOSE)
    if (ImGuiViewport* vp = viewportOf(window())) push(idOf(vp), SDL_WINDOWEVENT_CLOSE);
  } else if (cmd == "nsclose") {  // nsclose <window> (macOS): its red close button (-[NSWindow performClose:])
    if (ImGuiViewport* vp = viewportOf(window())) host::native::devPerformClose(nativeOf(sdlOf(vp)));
  } else if (cmd == "liveresize") {
    // liveresize <window> w h [steps]: the user resizes the detached window to w x h (ImGui units)
    // in `steps` steps while the window system blocks the event pump (simulated: SDL's size /
    // expose events for each step are generated from inside the pump with the modal loop forced
    // on). Each step prints the size it set, the size the window was drawn at and the frames drawn.
    const std::string name = args.empty() ? "" : args[0];
    const ImVec2 to(num(1), num(2));
    const int steps = std::max(1, args.size() > 3 ? static_cast<int>(num(3)) : 8);
    liveresize::queueDev([name, to, steps]() {
      ImGuiWindow* w = findWindow(name);
      ImGuiViewport* vp = w ? w->RootWindowDockTree->Viewport : nullptr;
      if (!vp || isMain(vp) || !sdlOf(vp)) {
        std::printf("liveresize: \"%s\" is not detached\n", name.c_str());
        return;
      }
      const ImVec2 from = wGetSize(vp);
      const Uint32 id = idOf(vp);
      liveresize::setSimulatedModal(true);
      for (int k = 1; k <= steps; ++k) {
        const float f = static_cast<float>(k) / static_cast<float>(steps);
        const ImVec2 s(std::round(from.x + (to.x - from.x) * f), std::round(from.y + (to.y - from.y) * f));
        const int before = liveresize::liveFrames();
        ImGuiWindow* cur = findWindow(name);
        ImGuiViewport* v = cur ? cur->RootWindowDockTree->Viewport : nullptr;
        if (!v || isMain(v)) break;
        if (S.hidden) S.virt[v].size = s;
        S.setSize(v, rounded(ImVec2(s.x * S.scale, s.y * S.scale)));  // the OS window takes the size (the user's drag)
        int pw = 0, ph = 0;
        if (SDL_Window* sw = sdlOf(v)) SDL_GetWindowSize(sw, &pw, &ph);
        for (Uint8 ev : {Uint8(SDL_WINDOWEVENT_SIZE_CHANGED), Uint8(SDL_WINDOWEVENT_RESIZED), Uint8(SDL_WINDOWEVENT_EXPOSED)}) {
          SDL_Event e{};
          e.type = SDL_WINDOWEVENT;
          e.window.event = ev;
          e.window.windowID = id;
          e.window.data1 = ev == SDL_WINDOWEVENT_EXPOSED ? 0 : pw;
          e.window.data2 = ev == SDL_WINDOWEVENT_EXPOSED ? 0 : ph;
          SDL_PushEvent(&e);  // SDL's event watch sees it now, inside the (blocked) pump
        }
        const auto d = S.drawn.find(name);
        const ImVec2 ds = d != S.drawn.end() ? d->second : ImVec2(0, 0);
        std::printf("liveresize step %d size %.0f %.0f drawn %.0f %.0f frames %d\n", k, static_cast<double>(s.x), static_cast<double>(s.y),
                    static_cast<double>(ds.x), static_cast<double>(ds.y), liveresize::liveFrames() - before);
      }
      liveresize::setSimulatedModal(false);
      std::fflush(stdout);
    });
  } else if (cmd == "nsresize") {
    // nsresize <window>|main dx dy [steps] (macOS): its bottom-right corner dragged by dx dy (OS
    // units) in `steps` mouse drags through AppKit's own live resize (a real modal resize loop).
    const std::string name = args.empty() ? "" : args[0];
    const float dx = num(1), dy = num(2);
    const int steps = std::max(1, args.size() > 3 ? static_cast<int>(num(3)) : 8);
    liveresize::queueDev([name, dx, dy, steps]() {
      SDL_Window* target = nullptr;
      if (name == "main") {
        target = S.main;
      } else if (ImGuiWindow* w = findWindow(name)) {
        ImGuiViewport* vp = w->RootWindowDockTree->Viewport;
        target = vp && !isMain(vp) ? sdlOf(vp) : nullptr;
      }
      if (!target) {
        std::printf("nsresize: no OS window for \"%s\"\n", name.c_str());
        return;
      }
      int w0 = 0, h0 = 0, w1 = 0, h1 = 0;
      SDL_GetWindowSize(target, &w0, &h0);
      const int before = liveresize::liveFrames(), framesBefore = GImGui->FrameCount;
      const Uint64 t = now();
      host::native::devLiveResize(nativeOf(target), dx, dy, steps);
      SDL_GetWindowSize(target, &w1, &h1);
      std::printf("nsresize %s: os %d %d -> %d %d live-frames %d frames %d in %.1f ms\n", name.c_str(), w0, h0, w1, h1,
                  liveresize::liveFrames() - before, GImGui->FrameCount - framesBefore, seconds(t, now()) * 1000.0);
      std::fflush(stdout);
    });
  } else if (cmd == "livestats") {
    liveresize::dump(frame);
  } else if (cmd == "osresize") {  // osresize <window> w h: the OS resizes it (an edge or corner dragged)
    if (ImGuiViewport* vp = viewportOf(window())) {
      const ImVec2 s(num(1), num(2));
      if (S.hidden) {
        S.virt[vp].size = s;
        vp->PlatformRequestResize = true;
      } else {
        S.setSize(vp, rounded(ImVec2(s.x * S.scale, s.y * S.scale)));
        vp->PlatformRequestResize = true;  // as the OS reports a user resize (SDL_WINDOWEVENT_RESIZED)
      }
    }
  } else if (cmd == "nsmouse") {  // nsmouse move|down|drag|up x y: a mouse event as AppKit delivers it to the main window (macOS)
    const std::string k = args.empty() ? "" : args[0];
    const int kind = k == "down" ? 1 : k == "drag" ? 2 : k == "up" ? 3 : 0;
    host::native::devPostMouse(nativeOf(S.main), kind, num(1) * S.scale, num(2) * S.scale);
  } else if (cmd == "dock") {  // dock <window> <target window>: into the target's dock node
    ImGuiWindow* w = window();
    ImGuiWindow* t = args.size() > 1 ? findWindow(args[1]) : nullptr;
    if (w && t && t->DockId != 0) {
      setDetachedFlag(w->RootWindowDockTree, false);
      S.homing[w->RootWindowDockTree->ID] = 30;
      ImGui::DockBuilderDockWindow(w->Name, t->DockId);
    } else if (w) {
      std::printf("dock frame %d: target not docked\n", frame);
    }
  } else if (cmd == "gather") {
    gatherAll();
  } else if (cmd == "detaching") {
    a.settings.set("ui.detachWindows", args.empty() || args[0] != "off");
  } else if (cmd == "focusmove") {  // the OS focus moves from the focused window (else the main one) to <window>'s ("main")
    const Uint32 to = args.empty() ? 0 : sdlWindowOf(args[0]);
    push(S.focusId ? S.focusId : SDL_GetWindowID(S.main), SDL_WINDOWEVENT_FOCUS_LOST);
    if (to) push(to, SDL_WINDOWEVENT_FOCUS_GAINED);
    std::printf("focusmove frame %d: %s window %u\n", frame, args.empty() ? "" : args[0].c_str(), static_cast<unsigned>(to));
  } else if (cmd == "sdlkeyin") {  // sdlkeyin <window> <key> down|up [mods]: a real SDL key event typed into that window
    SDL_Event e{};
    const bool down = args.size() < 3 || args[2] != "up";
    e.type = down ? SDL_KEYDOWN : SDL_KEYUP;
    e.key.windowID = args.empty() ? 0 : sdlWindowOf(args[0]);
    e.key.state = down ? SDL_PRESSED : SDL_RELEASED;
    const std::string k = args.size() > 1 ? args[1] : "";
    e.key.keysym.sym = k == "space" ? SDLK_SPACE : k.empty() ? 0 : static_cast<SDL_Keycode>(k[0]);
    e.key.keysym.scancode = SDL_GetScancodeFromKey(e.key.keysym.sym);
    const std::string m = args.size() > 3 ? args[3] : "";  // optional modifiers: "ctrl", "super", "shift", joined with '+'
    e.key.keysym.mod = static_cast<Uint16>((m.find("ctrl") != std::string::npos ? KMOD_LCTRL : 0) | (m.find("super") != std::string::npos ? KMOD_LGUI : 0) |
                                           (m.find("shift") != std::string::npos ? KMOD_LSHIFT : 0));
    SDL_PushEvent(&e);
  } else if (cmd == "vpshot") {
    if (args.size() >= 2) S.shots.emplace_back(args[0], args[1]);
  } else if (cmd == "dialogfrom") {  // dialogfrom <window> about|info: that dialog opened while <window> has the focus
    if (ImGuiWindow* w = window()) {
      ImGui::FocusWindow(w);
      const std::string d = args.size() > 1 ? args[1] : "";
      if (d == "about") a.showAbout = true;
      else if (d == "info") a.showInfo = true;
    }
  } else {
    return false;
  }
  std::fflush(stdout);
  return true;
}

}  // namespace gl::viewports
