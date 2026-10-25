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
//
// Keys (J-66): Gridline's global shortcuts work while a plugin window has
// the keyboard, as in FL Studio. Keys the plugin's interface does not use
// (it passes them up to the container, see host/KeyForward.h; the sandbox
// process forwards them the same way) and keys typed while Gridline's own
// parts of the window have the keyboard go to the main window's handlers as
// if typed there. A text field of the interface keeps every key, and the
// interface always sees a key first (its own computer-keyboard notes).
#include <SDL.h>
#include <SDL_opengl.h>
#include "SysWm.h"
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
#include "../core/UiScale.h"
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
bool gDevKeys = false;  // dev: Space typed into the first window's interface (J-66 check)
bool gDrawStats = false;  // dev: plugin interface drawing is counted (--frame-stats)
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

// The usable area of the main window's display or (J-113) of the display holding the
// remembered position `at`, so an editor left on another monitor reopens there.
lay::Rect workArea(const std::pair<int, int>* at = nullptr) {
  int d = gMainWin ? SDL_GetWindowDisplayIndex(gMainWin) : 0;
  SDL_Rect r;
  const SDL_Point p{at ? at->first + 40 : 0, at ? at->second + 12 : 0};  // a point on its title bar
  for (int n = 0, count = at ? SDL_GetNumVideoDisplays() : 0; n < count; ++n)
    if (SDL_GetDisplayBounds(n, &r) == 0 && SDL_PointInRect(&p, &r)) d = n;
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

// J-66: a key for Gridline's main window, as if typed there (its shortcuts,
// transport, typing keyboard): queued, handled by the main loop's event poll.
void sendToMain(bool down, bool repeat, int scancode, uint32_t mods) {
  if (!gMainWin || scancode <= 0 || scancode >= SDL_NUM_SCANCODES) return;
  SDL_Event e{};
  e.type = down ? SDL_KEYDOWN : SDL_KEYUP;
  e.key.windowID = SDL_GetWindowID(gMainWin);
  e.key.state = down ? SDL_PRESSED : SDL_RELEASED;
  e.key.repeat = repeat ? 1 : 0;
  e.key.keysym.scancode = static_cast<SDL_Scancode>(scancode);
  e.key.keysym.sym = SDL_GetKeyFromScancode(e.key.keysym.scancode);
  Uint16 m = KMOD_NONE;
  if (mods & host::kKeyShift) m |= KMOD_LSHIFT;
  if (mods & host::kKeyCtrl) m |= KMOD_LCTRL;
  if (mods & host::kKeyAlt) m |= KMOD_LALT;
  if (mods & host::kKeyCmd) m |= KMOD_LGUI;
  e.key.keysym.mod = m;
  SDL_PushEvent(&e);
}
void sendToMain(const host::KeyEvent& k) { sendToMain(k.down, k.repeat, host::hidUsageOfMacKey(k.code), k.mods); }
uint32_t modsOf(Uint16 m) {
  uint32_t r = 0;
  if (m & KMOD_SHIFT) r |= host::kKeyShift;
  if (m & KMOD_CTRL) r |= host::kKeyCtrl;
  if (m & KMOD_ALT) r |= host::kKeyAlt;
  if (m & KMOD_GUI) r |= host::kKeyCmd;
  return r;
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
  std::set<int> heldKeys;   // scancodes whose key-down went to the main window (J-66)

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
      const bool hadKeyboard = (SDL_GetWindowFlags(win) & SDL_WINDOW_INPUT_FOCUS) != 0;
      for (int sc : heldKeys) sendToMain(false, false, sc, 0);
      heldKeys.clear();
      if (gMainWin) native::keepAbove(nw, nativeOf(gMainWin), false);
      SDL_DestroyWindow(win);
      // The keyboard goes back to the main window (not to another app's window).
      if (hadKeyboard && gMainWin && !(SDL_GetWindowFlags(gMainWin) & SDL_WINDOW_HIDDEN)) SDL_RaiseWindow(gMainWin);
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

// A key SDL saw in a plugin window (J-66). On macOS SDL sees every key of
// the key window, also those AppKit delivers to the plugin's interface: while
// the interface has the keyboard, the container decides (unused keys come
// back through popKey). Otherwise the key was typed into Gridline's part of
// the window (its control bar, the window itself) and goes to the main
// window, unless a text field of the bar is being edited.
void routeKey(PluginWindow& w, const SDL_Event& ev) {
  const bool down = ev.type == SDL_KEYDOWN;
  const int sc = static_cast<int>(ev.key.keysym.scancode);
  if (!down) {  // key-ups of keys that went to the main window follow them
    if (w.heldKeys.erase(sc)) sendToMain(false, false, sc, modsOf(ev.key.keysym.mod));
    return;
  }
  if (w.container && native::keyFocus(w.container) != native::KeyFocus::Host) return;
  if (w.ctx) {
    ImGuiContext* prev = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(w.ctx);
    const bool typing = ImGui::GetIO().WantTextInput;
    ImGui::SetCurrentContext(prev);
    if (typing) return;
  }
  // X11: the plugin may use keys typed into the host's window (VST3 / VST2).
  if (w.editor && !w.external && w.editor->hostKey(true, sc, 0, modsOf(ev.key.keysym.mod))) return;
  w.heldKeys.insert(sc);
  sendToMain(true, ev.key.repeat != 0, sc, modsOf(ev.key.keysym.mod));
}

// Keys the plugin's interface did not use (in-process container or the
// sandbox window) go to the main window.
void forwardPluginKeys(PluginWindow& w) {
  host::KeyEvent k;
  if (w.container)
    while (native::popKey(w.container, k)) sendToMain(k);
  if (w.external && w.editor)
    while (w.editor->popKey(k)) sendToMain(k);
}

// Dev check (--plugin-window-keys): Space typed into the interface with a
// view that passes keys on, a text field, a view that uses every key; only
// the first toggles the transport.
void devKeys(PluginWindow& w) {
  static const int kinds[3] = {0, 2, 1};
  static const char* names[3] = {"interface view", "interface text field", "interface using the key"};
  static bool before = false;
  static int result[3] = {-1, -1, -1};
  const int step = w.frames - 20;
  if (step == -10) {  // the interface's own view first (reported, not checked: up to the plugin)
    void* v = native::devTestKeyView(w.container, 3);
    before = w.app->engine.playing();
    if (v) {
      native::devSendKey(w.container, 0x31, true, 0, " ");
      native::devSendKey(w.container, 0x31, false, 0, " ");
    }
    std::fprintf(stderr, "[plugin-window] key check: the interface's own view %s\n", v ? "has the keyboard" : "does not take the keyboard");
  }
  if (step == -1) {
    const bool now = w.app->engine.playing();
    std::fprintf(stderr, "[plugin-window] key check: Space in the interface's own view -> transport %s\n", now != before ? "toggled" : "unchanged");
    if (now != before) w.app->engine.stop();
  }
  if (step < 0 || step >= 3 * 12) return;
  const int k = step / 12, phase = step % 12;
  if (phase == 0) {
    native::devTestKeyView(w.container, kinds[k]);
    before = w.app->engine.playing();
    native::devSendKey(w.container, 0x31, true, 0, " ");
    native::devSendKey(w.container, 0x31, false, 0, " ");
  } else if (phase == 11) {
    result[k] = w.app->engine.playing() != before ? 1 : 0;
    std::fprintf(stderr, "[plugin-window] key check: Space in %s -> transport %s\n", names[k], result[k] ? "toggled" : "unchanged");
    if (k == 2) {
      const bool ok = result[0] == 1 && result[1] == 0 && result[2] == 0;
      std::fprintf(stderr, "[plugin-window] key check: %s\n", ok ? "PASS" : "FAIL");
    }
  }
}

// Dev check (--plugin-window-keys with GRIDLINE_SANDBOX_DEV_KEYS=1): the
// sandbox process types Space into its interface; the transport toggles here.
void devSandboxKeys(PluginWindow& w) {
  static bool before = false, done = false;
  if (done) return;
  if (w.frames == 2) before = w.app->engine.playing();
  if (w.frames > 2 && w.app->engine.playing() != before) {
    std::fprintf(stderr, "[plugin-window] key check: Space in the sandboxed interface -> transport toggled (frame %d)\n", w.frames);
    done = true;
  } else if (w.frames == 400) {
    std::fprintf(stderr, "[plugin-window] key check: Space in the sandboxed interface -> transport unchanged\n");
    done = true;
  }
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
void setPluginWindowDrawStats(bool on) { gDrawStats = on; }
void setPluginWindowDevKeys(bool on) { gDevKeys = on; }
void setPluginWindowDevResize(int w, int h) {
  gDevResizeW = w;
  gDevResizeH = h;
}

bool hostedEditorOpen(const PluginTarget& t) { return windowFor(t) != nullptr; }

bool hostedEditorUnderMouse(PluginTarget& t, float& fx, float& fy) {
  SDL_Window* f = SDL_GetMouseFocus();
  if (!f) return false;
  for (const auto& [key, w] : windows()) {
    if (!w || w->dead || w->win != f) continue;
    int mx = 0, my = 0, ww = 0, wh = 0;
    SDL_GetMouseState(&mx, &my);  // relative to the window with the mouse focus
    SDL_GetWindowSize(f, &ww, &wh);
    if (ww <= 0 || wh <= 0) return false;
    t = w->target;
    fx = std::clamp(static_cast<float>(mx) / static_cast<float>(ww), 0.0f, 1.0f);
    fy = std::clamp(static_cast<float>(my) / static_cast<float>(wh), 0.0f, 1.0f);
    return true;
  }
  return false;
}

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
  const lay::Rect work = workArea(place != places().end() ? &place->second : nullptr);
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
    {  // macOS: the backing scale; X11 (pixels): the desktop's scale, as Gridline's own interface (J-91)
      double cs = native::backingScale(w.nw);
#if !defined(__APPLE__) && !defined(_WIN32)
      cs = std::max(cs, static_cast<double>(uiscale::detected()));
#endif
      editor->setContentScale(cs);
    }
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
    if (ev.type == SDL_KEYDOWN || ev.type == SDL_KEYUP) routeKey(*w, ev);
    if (ev.type == SDL_WINDOWEVENT && w->container) {
      if (ev.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) native::restoreKeyFocus(w->container);
      if (ev.window.event == SDL_WINDOWEVENT_FOCUS_LOST) native::saveKeyFocus(w->container);
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
      if (gDrawStats && w.container && w.frames % 30 == 1) native::startDrawStats(w.container);  // interfaces add views late
      if (gDrawStats && w.container && w.frames == 240)  // dev: GRIDLINE_DEV_DRAG="x,y,seconds" turns a knob of the interface
        if (const char* d = std::getenv("GRIDLINE_DEV_DRAG")) {
          int dx = 0, dy = 0;
          double secs = 0;
          if (std::sscanf(d, "%d,%d,%lf", &dx, &dy, &secs) == 3) native::devDrag(w.nw, w.container, dx, dy, secs);
        }
      const int pwq = w.pendingW.exchange(0), phq = w.pendingH.exchange(0);
      if (pwq > 0 && phq > 0) w.pluginResize(pwq, phq);
      if (w.notifySize) {
        w.notifySize = false;
        w.editor->setSize(w.pw, w.ph);
      }
      w.editor->idle();
      if (gDevKeys && w.container) devKeys(w);
      if (gDevKeys && w.external) devSandboxKeys(w);
      forwardPluginKeys(w);
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
