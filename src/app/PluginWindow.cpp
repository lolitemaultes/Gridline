// Gridline — plugin windows of installed (VST3 / CLAP / VST2 / AU) plugins.
//
// An installed plugin opens as itself: a Gridline-owned native window whose
// top is the plugin's own interface (attached to a container view), with a
// compact control bar under it and the settings pages unfolding under the
// bar (geometry: host/EditorLayout.h). The bar and the panel are drawn with
// Dear ImGui in the window's own GL + ImGui context, sharing the main
// context's GL objects and font atlas, so they look like the rest of
// Gridline. The plugin view sits above the GL surface and is never covered.
//
// Sandboxed plugins show their interface in a window of the sandbox process;
// Gridline docks a bar-only window right under it and follows it.
#include <SDL.h>
#include <SDL_opengl.h>
#include <SDL_syswm.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <set>
#include <thread>
#include <vector>
#include "../core/I18n.h"
#include "../host/EditorLayout.h"
#include "../host/HostedPlugins.h"
#include "../host/PluginPrefs.h"
#include "../host/ui/NativeView.h"
#include "App.h"
#include "HostedUi.h"
#include "HostedUiPages.h"
#include "PluginWindowBar.h"
#include "Theme.h"
#include "Widgets.h"
#include "../../third_party/imgui/imgui.h"
#include "../../third_party/imgui/imgui_impl_opengl3.h"
#include "../../third_party/imgui/imgui_impl_sdl2.h"

namespace gl {

namespace lay = host::layout;
namespace native = host::native;

namespace {

bool gLog = false;
int gStartPanel = -1;  // dev: settings panel page new windows start with
int gDevResizeW = 0, gDevResizeH = 0;  // dev: first window dragged to this size after a few frames
bool gDevResized = false;
std::string gDevAction;  // dev: bar button pressed on the first window ("generic", "sandbox", "settings")
SDL_Window* gMainWin = nullptr;
SDL_GLContext gMainGl = nullptr;
std::thread::id gUiThread;

#if defined(__APPLE__)
const char* kGlsl = "#version 150";
#else
const char* kGlsl = "#version 130";
#endif

native::Window nativeOf(SDL_Window* w) {
  native::Window n;
  if (!w) return n;
  SDL_SysWMinfo info;
  SDL_VERSION(&info.version);
  if (!SDL_GetWindowWMInfo(w, &info)) return n;
#if defined(__APPLE__)
  if (info.subsystem == SDL_SYSWM_COCOA) n.handle = static_cast<void*>(info.info.cocoa.window);
#elif defined(SDL_VIDEO_DRIVER_X11)
  if (info.subsystem == SDL_SYSWM_X11) {
    n.display = static_cast<void*>(info.info.x11.display);
    n.handle = reinterpret_cast<void*>(static_cast<uintptr_t>(info.info.x11.window));
  }
#endif
  return n;
}

lay::Rect workArea() {
  int d = gMainWin ? SDL_GetWindowDisplayIndex(gMainWin) : 0;
  SDL_Rect r;
  if (SDL_GetDisplayUsableBounds(d < 0 ? 0 : d, &r) != 0) return lay::Rect{0, 0, 1440, 900};
  return lay::Rect{r.x, r.y, r.w, r.h};
}

std::map<uint64_t, std::pair<int, int>>& places() {  // remembered window positions per target
  static std::map<uint64_t, std::pair<int, int>> m;
  return m;
}
std::map<uint64_t, std::pair<int, int>>& sizes() {  // last size of resizable interfaces per target
  static std::map<uint64_t, std::pair<int, int>> m;
  return m;
}
// Makes a GL context current for real. SDL skips the call when it believes
// the context is current already, but plugin interfaces that draw with
// OpenGL (Kickstart, JUCE GL editors…) switch the thread's context behind
// SDL's back: clear first, then bind.
void makeCurrent(SDL_Window* win, SDL_GLContext gl) {
  if (!win || !gl) return;
  SDL_GL_MakeCurrent(win, nullptr);
  SDL_GL_MakeCurrent(win, gl);
}

// Restores the main window's GL and ImGui contexts.
struct ContextGuard {
  ImGuiContext* imgui = ImGui::GetCurrentContext();
  SDL_Window* win = SDL_GL_GetCurrentWindow();
  SDL_GLContext gl = SDL_GL_GetCurrentContext();
  ~ContextGuard() {
    if (win && gl) makeCurrent(win, gl);
    ImGui::SetCurrentContext(imgui);
  }
};

using Action = pluginbar::Action;

struct PluginWindow {
  App* app = nullptr;
  PluginTarget target;
  std::string type, name, format;
  host::Backend* backend = nullptr;
  std::unique_ptr<host::Editor> editor;
  SDL_Window* win = nullptr;
  SDL_GLContext gl = nullptr;
  ImGuiContext* ctx = nullptr;
  native::Window nw;
  void* container = nullptr;
  bool external = false;  // sandboxed: the interface is in the sandbox process' window
  bool resizable = false;
  bool dead = false;
  bool shown = false;
  int pw = 0, ph = 0;  // plugin view size
  int askedW = 0, askedH = 0;  // size the plugin asked for first (report)
  int panelPage = -1;  // -1: panel folded
  lay::WindowLayout L;
  int applying = 0;  // inside our own SDL_SetWindowSize
  // Resize asked for off the UI thread (CLAP request_resize is thread-safe).
  std::atomic<int> pendingW{0}, pendingH{0};
  bool notifySize = false;  // tell the plugin its new size on the next idle
  int extFrame[4] = {0, 0, 0, 0};
  Action action = Action::None;
  std::string capturePath;  // dev check: capture on the next draw
  int frames = 0;           // frames drawn since opening

  int panelH() const { return panelPage >= 0 ? lay::kPanelH : 0; }
  void computeLayout() { L = external ? lay::layoutBarWindow(extFrame[2] > 0 ? extFrame[2] : 420, panelH()) : lay::layoutWindow(pw, ph, panelH()); }

  void log(const char* what) {
    if (!gLog) return;
    const lay::BarLayout B = lay::layoutBar(L.w);
    std::fprintf(stderr, "[plugin-window] %s | %s (%s)%s: plugin %dx%d (asked %dx%d)%s -> window %dx%d, bar %d (%d row%s), panel %d, scale %.1f\n", what,
                 name.c_str(), format.c_str(), external ? " sandbox" : "", pw, ph, askedW, askedH, resizable ? " resizable" : "", L.w, L.h, L.bar.h,
                 B.rows, B.rows == 1 ? "" : "s", L.panel.h, native::backingScale(nw));
  }

  // Window and container follow the current layout.
  void apply() {
    computeLayout();
    if (!win) return;
    int cw = 0, ch = 0;
    SDL_GetWindowSize(win, &cw, &ch);
    if (cw != L.w || ch != L.h) {
      ++applying;
      SDL_SetWindowSize(win, L.w, L.h);
      --applying;
    }
    if (container) native::setContainerFrame(nw, container, L.plugin);
  }

  // The plugin asks for a new size (UI thread).
  bool pluginResize(int w, int h) {
    if (dead) return false;
    const int ow = pw, oh = ph;
    pw = std::max(w, lay::kMinPluginW);
    ph = std::max(h, lay::kMinPluginH);
    apply();
    notifySize = true;
    if (pw != ow || ph != oh) log("plugin resized");
    return true;
  }

  // The user dragged the window to w x h.
  void userResize(int w, int h) {
    if (external || !editor || (w == L.w && h == L.h)) return;
    if (!resizable) {  // fixed-size interface: the window keeps fitting it
      apply();
      return;
    }
    int nw2 = 0, nh2 = 0;
    lay::pluginSizeForWindow(w, h, panelH(), nw2, nh2);
    editor->constrain(nw2, nh2);
    pw = std::max(nw2, lay::kMinPluginW);
    ph = std::max(nh2, lay::kMinPluginH);
    apply();
    editor->setSize(pw, ph);
    log("window resized");
  }

  void setPanel(int page) {
    if (page == panelPage) return;
    panelPage = page;
    apply();
    log(page >= 0 ? "settings unfolded" : "settings folded");
  }

  void close() {
    if (dead) return;
    if (gLog) std::fprintf(stderr, "[plugin-window] closed | %s\n", name.c_str());
    dead = true;
    if (win) {
      int x = 0, y = 0;
      SDL_GetWindowPosition(win, &x, &y);
      if (!external) places()[target.key()] = {x, y};
      if (!external && resizable) sizes()[target.key()] = {pw, ph};
    }
    if (external && extFrame[2] > 0) places()[target.key()] = {extFrame[0], extFrame[1]};
    editor.reset();  // detaches the plugin's view before its container and window go
    if (container) native::destroyContainer(nw, container);
    container = nullptr;
    if (ctx) {
      ContextGuard g;
      ImGuiContext* dying = ctx;
      if (gl) makeCurrent(win, gl);
      ImGui::SetCurrentContext(ctx);
      ImGui_ImplOpenGL3_Shutdown();
      ImGui_ImplSDL2_Shutdown();
      ImGui::DestroyContext(ctx);
      ctx = nullptr;
      if (g.imgui == dying) g.imgui = nullptr;
      if (g.win == win) g.win = nullptr;
    }
    if (gl) SDL_GL_DeleteContext(gl);
    gl = nullptr;
    if (win) {
      if (gMainWin) native::keepAbove(nw, nativeOf(gMainWin), false);
      SDL_DestroyWindow(win);
    }
    win = nullptr;
    if (backend) backend->onGoingAway = nullptr;
    backend = nullptr;
  }
  ~PluginWindow() { close(); }
};

std::map<uint64_t, std::unique_ptr<PluginWindow>>& windows() {
  static std::map<uint64_t, std::unique_ptr<PluginWindow>> m;
  return m;
}

PluginWindow* windowFor(const PluginTarget& t) {
  auto it = windows().find(t.key());
  return it != windows().end() && !it->second->dead ? it->second.get() : nullptr;
}

// GL + ImGui context of a plugin window (shares the main context's GL
// objects and font atlas). The main contexts are current again afterwards.
bool createSurface(PluginWindow& w) {
  ContextGuard g;
  if (gMainWin && gMainGl) makeCurrent(gMainWin, gMainGl);
  ImGuiContext* mainCtx = ImGui::GetCurrentContext();
  ImFontAtlas* atlas = mainCtx ? ImGui::GetIO().Fonts : nullptr;
  ImFont* defFont = mainCtx ? ImGui::GetIO().FontDefault : nullptr;
  const ImGuiStyle style = mainCtx ? ImGui::GetStyle() : ImGuiStyle();
  SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);
  w.gl = SDL_GL_CreateContext(w.win);
  SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 0);
  if (!w.gl) return false;
  makeCurrent(w.win, w.gl);
  SDL_GL_SetSwapInterval(0);  // the main window paces the frames
  w.ctx = ImGui::CreateContext(atlas);
  ImGui::SetCurrentContext(w.ctx);
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.LogFilename = nullptr;
  io.ConfigDragClickToInputText = true;
  io.FontDefault = defFont;
  ImGui::GetStyle() = style;
  ImGui_ImplSDL2_InitForOpenGL(w.win, w.gl);
  ImGui_ImplOpenGL3_Init(kGlsl);
  return true;
}

void renderWindow(PluginWindow& w) {
  if (w.dead || !w.win || !w.ctx) return;
  if (!w.shown) return;
  host::HostedInstance* hi = hostedInstanceFor(*w.app, w.target);
  makeCurrent(w.win, w.gl);
  ImGui::SetCurrentContext(w.ctx);
  ImGui_ImplOpenGL3_NewFrame();
  ImGui_ImplSDL2_NewFrame();
  ImGui::NewFrame();
  pluginbar::Bar bar;
  bar.app = w.app;
  bar.target = w.target;
  bar.type = w.type;
  bar.L = w.L;
  bar.panelPage = w.panelPage;
  pluginbar::draw(bar, hi, static_cast<float>(w.L.bar.y));
  w.panelPage = bar.panelPage;
  if (bar.action != Action::None) w.action = bar.action;
  if (bar.wantPanel != pluginbar::kNoPanelChange) w.setPanel(bar.wantPanel);
  ImGui::Render();
  int fbw = 0, fbh = 0;
  SDL_GL_GetDrawableSize(w.win, &fbw, &fbh);
  glViewport(0, 0, fbw, fbh);
  float bg[4];
  pluginbar::background(bg);
  glClearColor(bg[0], bg[1], bg[2], 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
  if (!w.capturePath.empty()) {  // dev check: this window only, GL bar laid over its views
    int ww = 0, wh = 0;
    SDL_GetWindowSize(w.win, &ww, &wh);
    const int rows = wh > 0 ? static_cast<int>(std::lround(static_cast<double>(w.L.bar.h + w.L.panel.h) * fbh / wh)) : 0;
    std::vector<unsigned char> px(static_cast<size_t>(fbw) * static_cast<size_t>(std::max(rows, 1)) * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, fbw, rows, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    if (!native::captureWindow(w.nw, w.capturePath, px.data(), fbw, rows)) std::fprintf(stderr, "plugin window capture failed\n");
    w.capturePath.clear();
  }
  SDL_GL_SwapWindow(w.win);
}

void runAction(PluginWindow& w) {
  const Action act = w.action;
  w.action = Action::None;
  if (act == Action::None || w.dead) return;
  // Copies: the action may close (destroy) this window.
  App& a = *w.app;
  const PluginTarget t = w.target;
  const std::string type = w.type;
  pluginbar::runAction(a, t, type, act);
}

}  // namespace

// ---------------------------------------------------------------- public

void setPluginWindowLog(bool on) { gLog = on; }
void setPluginWindowPanel(int page) { gStartPanel = std::clamp(page, -1, 2); }
void setPluginWindowDevAction(const std::string& action) { gDevAction = action; }
void setPluginWindowDevResize(int w, int h) {
  gDevResizeW = w;
  gDevResizeH = h;
}

bool hostedEditorOpen(const PluginTarget& t) { return windowFor(t) != nullptr; }

void closeHostedEditor(App& a, const PluginTarget& t) {
  auto it = windows().find(t.key());
  if (it == windows().end()) return;
  it->second->close();
  windows().erase(it);
  (void)a;
}

void closeAllHostedEditors(App& a) {
  for (auto& [k, w] : windows()) w->close();
  windows().clear();
  (void)a;
}

bool openHostedEditor(App& a, const PluginTarget& t) {
  if (PluginWindow* ex = windowFor(t)) {
    if (ex->win) SDL_RaiseWindow(ex->win);
    return true;
  }
  host::HostedInstance* hi = hostedInstanceFor(a, t);
  if (!hi || hi->crashed()) {
    a.notify(hi ? "This plugin crashed: reload it first" : "This plugin is not loaded", true);
    return false;
  }
  host::Backend* b = hi->backend();
  const host::HostedClass& cls = hi->hostedClass();
  if (!b->hasEditor()) {
    a.notify(cls.name + " has no interface of its own: Gridline shows its parameters");
    return false;
  }
  std::unique_ptr<host::Editor> editor = b->createEditor();
  if (!editor) {
    a.notify(cls.name + ": its interface cannot be shown on this system", true);
    return false;
  }
  if (gUiThread == std::thread::id()) gUiThread = std::this_thread::get_id();
  auto pw = std::make_unique<PluginWindow>();
  PluginWindow& w = *pw;
  w.app = &a;
  w.target = t;
  w.type = cls.type;
  w.name = cls.name;
  w.format = b->format();
  w.backend = b;
  w.external = editor->external();
  const auto place = places().find(t.key());
  const lay::Rect work = workArea();
  const std::string title = cls.name + "  \xC2\xB7  " + hostedTargetName(a, t);
  Uint32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_HIDDEN;

  if (w.external) {  // sandboxed: the interface opens in the sandbox process' window
    if (place != places().end()) {
      editor->placeX = place->second.first;
      editor->placeY = place->second.second;
    }
    if (!editor->attach(nullptr)) {
      a.notify(cls.name + ": the sandboxed interface could not be opened", true);
      return false;
    }
    w.computeLayout();
    w.win = SDL_CreateWindow(title.c_str(), work.x, work.y, w.L.w, w.L.h, flags | SDL_WINDOW_BORDERLESS);
  } else {
    int ew = 640, eh = 420;
    editor->size(ew, eh);
    w.askedW = ew, w.askedH = eh;
    w.pw = std::max(ew, lay::kMinPluginW);
    w.ph = std::max(eh, lay::kMinPluginH);
    w.resizable = editor->canResize();
    w.panelPage = gStartPanel;
    w.computeLayout();
    const lay::Rect r = lay::placeWindow(w.L.w, w.L.h, work, place != places().end(), place != places().end() ? place->second.first : 0,
                                         place != places().end() ? place->second.second : 0, static_cast<int>(windows().size()));
    w.win = SDL_CreateWindow(title.c_str(), r.x, r.y, w.L.w, w.L.h, flags | (w.resizable ? SDL_WINDOW_RESIZABLE : 0));
  }
  if (!w.win) {
    a.notify(std::string("Cannot open a window: ") + SDL_GetError(), true);
    return false;
  }
  w.nw = nativeOf(w.win);
  if (!createSurface(w)) {
    a.notify(std::string("Cannot draw the plugin window: ") + SDL_GetError(), true);
    w.editor = std::move(editor);
    w.close();
    return false;
  }
  PluginWindow* raw = &w;
  if (!w.external) {
    w.container = native::createContainer(w.nw, w.L.plugin);
    editor->setContentScale(native::backingScale(w.nw));
    editor->requestResize = [raw](int nw, int nh) {
      if (raw->dead) return false;
      if (std::this_thread::get_id() != gUiThread) {  // applied on the next idle
        raw->pendingW.store(nw);
        raw->pendingH.store(nh);
        return true;
      }
      return raw->pluginResize(nw, nh);
    };
    if (!w.container || !editor->attach(w.container)) {
      w.editor = std::move(editor);
      w.close();
      a.notify(cls.name + ": its interface could not be attached", true);
      return false;
    }
    // Some editors (VST2) know their final size only once open.
    int w2 = w.pw, h2 = w.ph;
    if (editor->size(w2, h2) && (w2 != w.pw || h2 != w.ph)) {
      w.pw = w2, w.ph = h2;
      w.apply();
    }
    // A resizable interface reopens at the size the user left it at.
    if (auto sz = sizes().find(t.key()); w.resizable && sz != sizes().end() && (sz->second.first != w.pw || sz->second.second != w.ph)) {
      int rw = sz->second.first, rh = sz->second.second;
      editor->constrain(rw, rh);
      w.pw = std::max(rw, lay::kMinPluginW), w.ph = std::max(rh, lay::kMinPluginH);
      w.apply();
      editor->setSize(w.pw, w.ph);
    }
    SDL_SetWindowMinimumSize(w.win, lay::kMinWindowW, w.L.bar.h + lay::kMinPluginH);
    SDL_ShowWindow(w.win);
    w.shown = true;
    if (gMainWin && !(SDL_GetWindowFlags(gMainWin) & SDL_WINDOW_HIDDEN)) native::keepAbove(w.nw, nativeOf(gMainWin), true);
    SDL_RaiseWindow(w.win);
  }
  w.editor = std::move(editor);
  b->onGoingAway = [raw]() { raw->close(); };
  w.log("opened");
  windows()[t.key()] = std::move(pw);
  return true;
}

bool hostedEditorEvent(App& a, const SDL_Event& ev) {
  Uint32 id = 0;
  switch (ev.type) {
    case SDL_WINDOWEVENT: id = ev.window.windowID; break;
    case SDL_MOUSEMOTION: id = ev.motion.windowID; break;
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP: id = ev.button.windowID; break;
    case SDL_MOUSEWHEEL: id = ev.wheel.windowID; break;
    case SDL_KEYDOWN:
    case SDL_KEYUP: id = ev.key.windowID; break;
    case SDL_TEXTINPUT: id = ev.text.windowID; break;
    case SDL_TEXTEDITING: id = ev.edit.windowID; break;
    default: return false;
  }
  for (auto& [k, w] : windows()) {
    if (w->dead || !w->win || SDL_GetWindowID(w->win) != id) continue;
    if (w->ctx) {
      ImGuiContext* prev = ImGui::GetCurrentContext();
      ImGui::SetCurrentContext(w->ctx);
      ImGui_ImplSDL2_ProcessEvent(&ev);
      ImGui::SetCurrentContext(prev);
    }
    if (ev.type == SDL_WINDOWEVENT) {
      if (ev.window.event == SDL_WINDOWEVENT_CLOSE) {
        w->action = Action::Close;
        runAction(*w);
      } else if (ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED && !w->applying) {
        w->userResize(ev.window.data1, ev.window.data2);
      }
    }
    return true;
  }
  (void)a;
  return false;
}

void renderHostedEditors(App& a) {
  if (windows().empty()) return;
  {
    ContextGuard g;
    for (auto& [k, w] : windows()) renderWindow(*w);
  }
  for (auto& [k, w] : windows())
    if (w->action != Action::None) {
      runAction(*w);
      break;  // the map may have changed
    }
  (void)a;
}

bool screenshotHostedEditor(const PluginTarget& t, const std::string& pngPath) {
  PluginWindow* w = windowFor(t);
  if (!w) return false;
  w->capturePath = pngPath;  // taken when the window draws next (this frame)
  return true;
}

void hostedui::pluginWindowsIdle(App& a) {
  if (!gMainWin) {
    gMainWin = SDL_GL_GetCurrentWindow();
    gMainGl = SDL_GL_GetCurrentContext();
  }
  // Plugin interfaces may have bound their own GL context during the event
  // loop: the main window draws next.
  if (!windows().empty()) makeCurrent(gMainWin, gMainGl);
  if (gUiThread == std::thread::id()) gUiThread = std::this_thread::get_id();
  for (auto it = windows().begin(); it != windows().end();) {
    PluginWindow& w = *it->second;
    if (!w.dead && w.editor) {
      if (++w.frames == 5 && gDevResizeW > 0 && !gDevResized && w.win) {  // dev: as if the user dragged the window corner
        if (gLog) std::fprintf(stderr, "[plugin-window] user drags window to %dx%d\n", gDevResizeW, gDevResizeH);
        SDL_SetWindowSize(w.win, gDevResizeW, gDevResizeH);
        gDevResized = true;
      }
      if (w.frames == 8 && !gDevAction.empty()) {  // dev: as if the user pressed a bar button
        if (gLog) std::fprintf(stderr, "[plugin-window] bar button: %s\n", gDevAction.c_str());
        if (gDevAction == "generic") w.action = Action::Generic;
        if (gDevAction == "sandbox") w.action = Action::Sandbox;
        if (gDevAction == "settings") w.setPanel(0);
        gDevAction.clear();
      }
      const int pwq = w.pendingW.exchange(0), phq = w.pendingH.exchange(0);
      if (pwq > 0 && phq > 0) w.pluginResize(pwq, phq);
      if (w.notifySize) {
        w.notifySize = false;
        w.editor->setSize(w.pw, w.ph);
      }
      w.editor->idle();
      if (w.external) {
        if (!w.editor->isOpen()) {
          w.close();
        } else {
          int f[4] = {0, 0, 0, 0};
          if (w.editor->windowFrame(f[0], f[1], f[2], f[3]) && f[2] > 0) {
            const bool moved = f[0] != w.extFrame[0] || f[1] != w.extFrame[1] || f[2] != w.extFrame[2] || f[3] != w.extFrame[3];
            if (moved || !w.shown) {
              std::copy(f, f + 4, w.extFrame);
              w.computeLayout();
              ++w.applying;
              SDL_SetWindowSize(w.win, w.L.w, w.L.h);
              SDL_SetWindowPosition(w.win, f[0], f[1] + f[3]);  // docked under the sandbox window
              --w.applying;
              if (!w.shown) {
                SDL_ShowWindow(w.win);
                w.shown = true;
                if (gMainWin && !(SDL_GetWindowFlags(gMainWin) & SDL_WINDOW_HIDDEN)) native::keepAbove(w.nw, nativeOf(gMainWin), true);
                w.log("sandbox window docked");
              }
            }
          }
        }
      }
    }
    it = w.dead ? windows().erase(it) : std::next(it);
  }
  pluginbar::reopenPending(a);  // windows to show again on new instances
  routePluginWindows(a);
}

}  // namespace gl
