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
// macOS: sandboxed plugins show their interface in a window of the sandbox
// process; Gridline docks a bar-only window right under it and follows it.
// Linux: the sandbox process attaches the interface to this window's container,
// so a sandboxed plugin's window is like any other (Sandbox.cpp embedsEditors).
//
// Keys (J-66): Gridline's global shortcuts work while a plugin window has
// the keyboard, as in FL Studio. Keys the plugin's interface does not use
// (it passes them up to the container, see host/KeyForward.h; the sandbox
// process forwards them the same way) and keys typed while Gridline's own
// parts of the window have the keyboard go to the main window's handlers as
// if typed there. A text field of the interface keeps every key, and the
// interface always sees a key first (its own computer-keyboard notes).
#include "GlRecovery.h"
#include <SDL.h>
#include <SDL_opengl.h>
#include "SysWm.h"
#include <algorithm>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <thread>
#include <vector>
#include "../core/Crash.h"
#include "../core/I18n.h"
#include "../core/Log.h"
#include "../core/UiScale.h"
#include "../host/EditorClose.h"
#include "../host/EditorLayout.h"
#include "../host/HostedPlugins.h"
#include "../host/PluginPrefs.h"
#include "../host/ui/NativeView.h"
#include "ForeignGl.h"
#include "FreezeUi.h"
#include "App.h"
#include "AppIcon.h"
#include "HostedUi.h"
#include "HostedUiPages.h"
#include "IdSweep.h"
#include "IdlePace.h"
#include "LayoutPresets.h"
#include "PluginWindowBar.h"
#include "ToastText.h"
#include "VideoDriver.h"
#include "Viewports.h"
#include "Theme.h"
#include "Widgets.h"
#include "../../third_party/imgui/imgui.h"
#include "../../third_party/imgui/imgui_internal.h"
#include "../../third_party/imgui/imgui_impl_opengl3.h"
#include "../../third_party/imgui/imgui_impl_sdl2.h"
#include "KeysSdl.h"

namespace gl {

namespace lay = host::layout;
namespace native = host::native;

namespace {

bool gLog = false;
int gStartPanel = -1;  // dev: settings panel page new windows start with
float gUiScale = 1.0f;  // K-35: the main window's interface scale, this frame
int gDevResizeW = 0, gDevResizeH = 0;  // dev: first window dragged to this size after a few frames
bool gDevResized = false;
std::string gDevAction;  // dev: bar button pressed on the first window ("generic", "sandbox", "settings")
bool gDevKeys = false;  // dev: Space typed into the first window's interface (J-66 check)
bool gDrawStats = false;  // dev: plugin interface drawing is counted (--frame-stats)
SDL_Window* gMainWin = nullptr;
int gDevCoverFrames = 0;  // dev: frames a tooltip stays over the first interface inside Gridline
bool gKeyboardToMain = false;  // a closing plugin window had the keyboard
SDL_GLContext gMainGl = nullptr;
std::thread::id gUiThread;


// K-153: SDL's Wayland driver. A plugin's X11 interface cannot embed in Gridline's Wayland windows, so
// it opens in a window of its own on XWayland (host/ui/NativeView.h sideWindowOpen), and Gridline's bar
// for it is a window of its own too, the way the macOS sandbox's interfaces are shown (external
// editors). Wayland lets no program place or stack its windows, so the two are not docked together.
class SideWindowEditor final : public host::Editor {
 public:
  SideWindowEditor(std::unique_ptr<host::Editor> inner, std::string title, double scale)
      : inner_(std::move(inner)), title_(std::move(title)), scale_(scale) {}
  ~SideWindowEditor() override {
    {
      foreigngl::ForeignGlScope g;
      inner_.reset();  // the interface goes before the window it is in
    }
    if (side_) native::sideWindowClose(side_);
  }
  SideWindowEditor(const SideWindowEditor&) = delete;
  SideWindowEditor& operator=(const SideWindowEditor&) = delete;

  bool size(int& w, int& h) override { return inner_->size(w, h); }
  bool canResize() const override { return inner_->canResize(); }
  void constrain(int& w, int& h) override { inner_->constrain(w, h); }
  bool external() const override { return true; }
  bool isOpen() const override { return open_; }
  bool attach(void*) override {
    native::installX11SafetyHandlers();
    foreigngl::ForeignGlScope g;  // the interface may draw with GLX while Gridline draws with EGL
    int w = 640, h = 420;
    inner_->size(w, h);
    w = std::max(w, 64), h = std::max(h, 64);
    side_ = native::sideWindowOpen(title_, w, h, inner_->canResize());
    if (!side_) return false;
    w_ = w, h_ = h;
    inner_->setContentScale(scale_);
    inner_->requestResize = [this](int nw, int nh) {  // any thread: applied on the next idle
      pendingW_.store(nw);
      pendingH_.store(nh);
      return true;
    };
    const bool attached = inner_->attach(side_);
    native::installX11SafetyHandlers();  // re-ensure plugin did not reset XSetErrorHandler(nullptr)
    if (!attached) {
      native::sideWindowClose(side_);
      side_ = nullptr;
      return false;
    }
    int w2 = w_, h2 = h_;  // some interfaces (VST2) know their size only once open
    if (inner_->size(w2, h2) && (w2 != w_ || h2 != h_)) resizeTo(w2, h2);
    open_ = true;
    return true;
  }
  void setSize(int w, int h) override {
    foreigngl::ForeignGlScope g;
    inner_->setSize(w, h);
  }
  void idle() override {
    foreigngl::ForeignGlScope g;
    if (open_) {
      const int pw = pendingW_.exchange(0), ph = pendingH_.exchange(0);
      if (pw > 0 && ph > 0 && (pw != w_ || ph != h_)) resizeTo(pw, ph);  // (the size it asked for: not told back, see pluginResize)
      int w = w_, h = h_;
      if (!native::sideWindowPump(side_, w, h)) open_ = false;  // the user closed it
      if (open_ && (w != w_ || h != h_)) {  // the window manager resized it
        int cw = w, ch = h;
        inner_->constrain(cw, ch);
        w_ = w, h_ = h;
        inner_->setSize(cw, ch);
        if (cw != w || ch != h) resizeTo(cw, ch);
      }
    }
    inner_->idle();
  }
  bool windowFrame(int& x, int& y, int& w, int& h) const override {
    if (!open_) return false;
    x = kNoPlace, y = kNoPlace, w = w_, h = h_;  // the bar is as wide as the interface; Wayland places both
    return true;
  }
  bool popKey(host::KeyEvent& e) override { return side_ && native::popKey(side_, e); }
  bool hostKey(bool down, int usage, uint32_t ch, uint32_t mods) override { return inner_->hostKey(down, usage, ch, mods); }
  void detachOffThread() override { inner_->detachOffThread(); }

 private:
  // The window to the interface's size (it asked for it, or it was told it already).
  void resizeTo(int w, int h) {
    native::sideWindowResize(side_, w, h, inner_->canResize());
    w_ = w, h_ = h;
  }
  std::unique_ptr<host::Editor> inner_;
  std::string title_;
  double scale_ = 1.0;
  void* side_ = nullptr;
  bool open_ = false;
  int w_ = 0, h_ = 0;
  std::atomic<int> pendingW_{0}, pendingH_{0};
};

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

constexpr Uint8 kForwardedKey = 0x67;
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
  e.key.padding2 = kForwardedKey;  // 0.7.8: not dropped again as a docked interface's own key
  SDL_PushEvent(&e);
}
// Smooth: the arrow keys stay with the plugin's window (host::keepInPluginWindow).
bool keepsKey(int usage) { return host::keepInPluginWindow(layoutui::active() == layout::Preset::Logic, usage); }
void sendToMain(const host::KeyEvent& k) {
  const int usage = host::hidUsageOfMacKey(k.code);
  if (keepsKey(usage)) return;
  sendToMain(k.down, k.repeat, usage, k.mods);
}
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

// K-37: how long the UI thread waits for a bridged plugin's interface to close before it gets on
// with its frames, and how long the plugin then has before it counts as lost.
constexpr int kBridgedCloseWaitMs = 300, kBridgedGoingAwayWaitMs = 2000;
bool gInterfaceAbandoned = false;  // a bridged interface never came back from closing
Uint32 bridgedGiveUpMs() {
  static const Uint32 ms = [] {
    const char* e = std::getenv("GRIDLINE_DEV_BRIDGED_GIVE_UP_MS");  // tests
    return e ? static_cast<Uint32>(std::atoi(e)) : Uint32{15000};
  }();
  return ms;
}
void traceBridgedClose(const char* what) {  // the test's and the harness's view of it
  static const bool on = std::getenv("GRIDLINE_DEV_BRIDGED_PLUGINS") != nullptr || std::getenv("GRIDLINE_DEV_PLUGIN_KEYS") != nullptr;
  if (on) std::fprintf(stderr, "bridged close: %s\n", what);
}

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
  unsigned hiddenBy = 0;  // K-112: set aside (HostedEditorHide bits): off screen, still open and running
  int pw = 0, ph = 0;  // plugin view size
  int askedW = 0, askedH = 0;  // size the plugin asked for first (report)
  int panelPage = -1;  // -1: panel folded
  lay::WindowLayout L;
  int applying = 0;  // inside our own SDL_SetWindowSize
  // Resize asked for off the UI thread (CLAP request_resize is thread-safe).
  std::atomic<int> pendingW{0}, pendingH{0};
  bool notifySize = false;  // tell the plugin its new size on the next idle (it got another than it asked for)
  int resizeAsks = 0;       // sizes the plugin asked for that changed the window (dev: plugwin info)
  int extFrame[4] = {0, 0, 0, 0};
  Action action = Action::None;
  std::string capturePath;  // dev check: capture on the next draw
  int frames = 0;           // frames drawn since opening
  uint64_t themeGen = 0;    // K-395: resync this window's ImGui style when the theme changes
  std::set<int> heldKeys;   // scancodes whose key-down went to the main window (J-66)
  int rootX = 0, rootY = 0;  // K-37: position the interface was last told about (Linux)
  bool rootKnown = false;
  bool complete = false;  // the window finished opening
  const char* closeWhy = nullptr;  // why it closes, for the debug log (issue reports)
  // K-37: the bridged plugin's interface is closing on a helper thread; the window is hidden.
  std::unique_ptr<host::EditorClose> closing;
  Uint32 closingSince = 0;
  bool reopenAfterClose = false;
  bool instanceGoing = false;  // closed because the plugin instance is being destroyed
  double scale = 1.0;  // K-35: window pixels per unit of the bar (X11 at a system scale)
  ImU32 lastScaledEvent = 0;
  // 0.7.8: inside Gridline. The interface sits in one of Gridline's own windows (`host`: the main
  // window, or the one its pane was popped out into), drawn into its pane by drawDockedInterface and
  // placed after each frame (placeDockedInterfaces). No window, GL or ImGui context of its own.
  bool docked = false;
  SDL_Window* host = nullptr;
  ImGuiID hostRoot = 0, hostViewport = 0;  // the pane's ImGui window (root of its dock tree) and viewport
  int drawnFrame = -1;                     // ImGui frame the pane last drew it
  ImVec2 regionMin, regionMax, clipMin, clipMax;  // where it was drawn (ImGui coordinates)
  lay::Rect shownFull, shownVisible;       // what the container shows now (window units)
  std::vector<lay::Rect> shownHoles;
  bool placed = false;
  bool clicked = false;  // the user clicked the interface since it opened (until then it never keeps the keyboard)
  bool wasOpening = false;   // its interface (out of process) was still opening last frame
  bool stuckTitle = false;   // its window's title says the plugin is not responding

  int panelH() const { return panelPage >= 0 ? static_cast<int>(std::lround(lay::kPanelH * scale)) : 0; }
  void computeLayout() { L = external ? lay::layoutBarWindow(extFrame[2] > 0 ? extFrame[2] : 420, panelH()) : lay::layoutWindow(pw, ph, panelH(), scale); }

  bool abandoned = false;  // the helper thread may still call back: never freed

  void giveKeyboardBack() {
    if (!win) return;
    const bool hadKeyboard = (SDL_GetWindowFlags(win) & SDL_WINDOW_INPUT_FOCUS) != 0;
    for (int sc : heldKeys) sendToMain(false, false, sc, 0);
    heldKeys.clear();
    if (gMainWin) native::keepAbove(nw, nativeOf(gMainWin), false);
    if (hadKeyboard && gMainWin && !(SDL_GetWindowFlags(gMainWin) & SDL_WINDOW_HIDDEN)) gKeyboardToMain = true;
  }

  void log(const char* what) {
    if (!gLog) return;
    const lay::BarLayout B = lay::layoutBar(lay::barUnits(L, scale).w);
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

  // The plugin asks for a new size (UI thread). The window takes it; the plugin is told its size
  // (Editor::setSize) only when that is not what it asked for (below Gridline's minimum). Telling
  // it the size it just asked for made a loop with a plugin that asks again from inside its
  // set-size call (some frameworks do): a set-size and a resize request every frame, the
  // interface laid out and drawn again each time (a window that flickers and a busy UI thread).
  // CLAP: the host need not call set_size after request_resize; VST3: the frame's resizeView
  // already told the view (Vst3Backend.cpp onResize); AU, VST2, LV2: the plugin sized itself.
  bool pluginResize(int w, int h) {
    if (dead) return false;
    const int ow = pw, oh = ph;
    pw = std::max(w, lay::kMinPluginW);
    ph = std::max(h, lay::kMinPluginH);
    apply();
    if (pw != w || ph != h) notifySize = true;
    if (pw != ow || ph != oh) {
      log("plugin resized");
      ++resizeAsks;
    }
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
    lay::pluginSizeForWindow(w, h, panelH(), nw2, nh2, scale);
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
    if (complete) {  // Settings > Debug: which path closed an interface window (a window that flashes and closes)
      const std::string why = closeWhy ? closeWhy : "Gridline closed it";
      logging::info("plugins", name + ": interface window closed (" + why + ")");
      std::fprintf(stderr, "[plugin-window] %s: interface window closed (%s)\n", name.c_str(), why.c_str());
    }
    dead = true;
    if (win) {
      int x = 0, y = 0;
      SDL_GetWindowPosition(win, &x, &y);
      if (!external) places()[target.key()] = {x, y};
      if (!external && resizable) sizes()[target.key()] = {pw, ph};
    }
    if (external && extFrame[2] > 0 && extFrame[0] != host::Editor::kNoPlace) places()[target.key()] = {extFrame[0], extFrame[1]};
    if (docked && resizable) sizes()[target.key()] = {pw, ph};
    if (backend && backend->lost()) (void)editor.release();  // K-9: closing it would call the lost plugin, and that throws
    if (editor && backend && complete && !external && backend->bridged()) {  // K-37
      backend->setEditorClosing(true);
      closing = std::make_unique<host::EditorClose>(editor.get());
      if (instanceGoing || closing->waitFor(kBridgedCloseWaitMs)) {
        settleClosing(instanceGoing);
        return;
      }
      traceBridgedClose("waiting");
      closingSince = SDL_GetTicks();  // the frames go on; settleClosing() finishes this
      hideForClosing();
      backend->onGoingAway = [this]() { settleClosing(true); };
      return;
    }
    finishClose();
  }

  // K-37: true once the closing is over: the plugin came back, or it counts as lost.
  bool settleClosing(bool going) {
    if (!closing) return true;
    bool done = closing->done();
    if (!done && going) done = closing->waitFor(kBridgedGoingAwayWaitMs);
    if (!done && !going && SDL_GetTicks() - closingSince < bridgedGiveUpMs()) return false;
    const bool clean = done && !closing->failed();
    traceBridgedClose(clean ? "closed" : "gave up");
    backend->setEditorClosing(false);
    if (!clean) {
      if (!done) backend->setLostReason("its interface did not close");
      backend->markLost();
      (void)editor.release();  // the helper thread may still be inside it
      abandoned = !done;
      if (abandoned) gInterfaceAbandoned = true;
    }
    closing.reset();
    finishClose();
    return true;
  }

  void hideForClosing() {
    if (docked && container) native::setDockedFrame(nw, container, shownFull, lay::Rect{}, {});
    giveKeyboardBack();
    if (win) SDL_HideWindow(win);
    if (gKeyboardToMain) SDL_RaiseWindow(gMainWin);
    gKeyboardToMain = false;
  }

  void finishClose() {
    editor.reset();  // detaches the plugin's view before its container and window go
    // A sandboxed interface embedded here (Linux) whose process is busy has not taken its window out
    // of the container yet: destroying the container (and the window it is in) would destroy the
    // plugin's window under it. They go once the process answers or is stopped (hidden meanwhile).
    const bool later = container && backend && backend->awaitingAnswer();
    void* c = container;
    const bool wasDocked = docked;
    const native::Window cw = nw;
    auto dropContainer = [c, wasDocked, cw]() {
      if (c && wasDocked) native::destroyDockedContainer(cw, c);
      else if (c) native::destroyContainer(cw, c);
    };
    if (later && wasDocked) native::setDockedFrame(nw, c, shownFull, lay::Rect{}, {});
    if (!later) dropContainer();
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
    if (win) giveKeyboardBack();
    if (later) {
      if (win) SDL_HideWindow(win);
      SDL_Window* sw = win;
      if (gLog || std::getenv("GRIDLINE_DEV_PLUGIN_WINDOWS")) std::fprintf(stderr, "[plugin-window] %s: container kept until its process answers\n", name.c_str());
      backend->whenAnswered([dropContainer, sw, n = name]() {
        dropContainer();
        if (sw) SDL_DestroyWindow(sw);
        if (std::getenv("GRIDLINE_DEV_PLUGIN_WINDOWS")) std::fprintf(stderr, "[plugin-window] %s: container destroyed after its process answered\n", n.c_str());
      });
    } else if (win) {
      SDL_DestroyWindow(win);
    }
    // The keyboard goes back to the main window (not to another app's window).
    if (gKeyboardToMain) SDL_RaiseWindow(gMainWin);
    gKeyboardToMain = false;
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

// K-37: windows whose bridged plugin is still closing its interface (never destroyed: outlives the
// other statics, whose destruction at exit must not call a plugin).
std::vector<std::unique_ptr<PluginWindow>>& closingWindows() {
  static auto* v = new std::vector<std::unique_ptr<PluginWindow>>;
  return *v;
}

// A window that is closed and leaves the list of open windows.
void retire(std::unique_ptr<PluginWindow> w) {
  if (w && w->closing) closingWindows().push_back(std::move(w));
}

PluginWindow* windowFor(const PluginTarget& t) {
  auto it = windows().find(t.key());
  return it != windows().end() && !it->second->dead ? it->second.get() : nullptr;
}

// GitHub issue #47: a plugin window is shown on top of the main window and stays there. On X11 the
// window manager reads WM_TRANSIENT_FOR when it maps a window: set after mapping, a window manager
// with focus stealing prevention (Xfce's, KDE's) may map the new window under the main window the
// click went to, and keep it there. It looked as if the window flashed and closed; it was open
// behind the main window, so the next click on its channel closed it. So on X11 and Windows the
// owner is set before the window is shown. macOS: a child window can only be added once the
// window is on screen (adding it orders it in), so there it stays after.
//
// 0.7.8: `takeKeys` false (a window coming back after it was set aside) or a mouse button held (it
// appears during a press: a press that selected another track) and it comes to the front leaving the
// keys where they are. macOS made every window SDL shows the key window: the main window lost the
// keys, which ended the drag the press started and every note held on the typing keyboard.
static bool mouseHeld() { return SDL_GetGlobalMouseState(nullptr, nullptr) != 0; }
void showAboveMain(PluginWindow& w, bool takeKeys = true) {
  if (w.hiddenBy != 0) return;  // set aside: it shows when the last reason goes
  const bool keep = gMainWin && !(SDL_GetWindowFlags(gMainWin) & SDL_WINDOW_HIDDEN);
#if defined(__APPLE__)
  const bool quiet = !takeKeys || mouseHeld();
  if (quiet) native::setTakesNoKeys(w.nw, true);
  SDL_ShowWindow(w.win);
  if (quiet) native::setTakesNoKeys(w.nw, false);
  if (keep) native::keepAbove(w.nw, nativeOf(gMainWin), true);
#else
  (void)takeKeys;  // (macOS only: elsewhere the owner rules above decide, as before)
  if (keep) native::keepAbove(w.nw, nativeOf(gMainWin), true);
  SDL_ShowWindow(w.win);
#endif
  w.shown = true;
  static const bool trace = std::getenv("GRIDLINE_DEV_PLUGIN_WINDOWS") != nullptr;  // tests
  if (gLog || trace) std::fprintf(stderr, "[plugin-window] shown | %s: owner %s\n", w.name.c_str(), !keep ? "none (main window hidden)"
#if defined(__APPLE__)
                                                                                        : "set after it was shown (child window)");
#else
                                                                                        : "set before it was shown");
#endif
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
  ui::installGlyphHook();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.LogFilename = nullptr;
  io.ConfigDragClickToInputText = true;
  io.ConfigDebugHighlightIdConflicts = idsweep::highlight();  // K-459: Dear ImGui's ID popup for developers only
  io.FontDefault = defFont;
  ImGui::GetStyle() = style;
  theme::applyImGuiStyle();
  w.themeGen = theme::generation();
  ImGui_ImplSDL2_InitForOpenGL(w.win, w.gl);
  ImGui_ImplOpenGL3_Init(glrecovery::glsl());
  return true;
}

void renderWindow(PluginWindow& w) {
  if (w.dead || !w.win || !w.ctx || !w.gl) return;
  if (!w.shown) return;
  host::HostedInstance* hi = hostedInstanceFor(*w.app, w.target);
  makeCurrent(w.win, w.gl);
  ImGui::SetCurrentContext(w.ctx);
  if (w.themeGen != theme::generation()) {
    theme::applyImGuiStyle();
    w.themeGen = theme::generation();
  }
  ImGui_ImplOpenGL3_NewFrame();
  ImGui_ImplSDL2_NewFrame();
  if (w.scale != 1.0) {  // K-35: as the main window: the bar in interface units, drawn at the scale
    ImGuiIO& io = ImGui::GetIO();
    const float s = static_cast<float>(w.scale);
    int ww = 0, wh = 0, dw = 0, dh = 0;
    SDL_GetWindowSize(w.win, &ww, &wh);
    SDL_GL_GetDrawableSize(w.win, &dw, &dh);
    if (ww > 0 && wh > 0) {
      io.DisplaySize = ImVec2(static_cast<float>(ww) / s, static_cast<float>(wh) / s);
      io.DisplayFramebufferScale = ImVec2(static_cast<float>(dw) / io.DisplaySize.x, static_cast<float>(dh) / io.DisplaySize.y);
    }
    ImGuiContext& g = *ImGui::GetCurrentContext();
    for (ImGuiInputEvent& e : g.InputEventsQueue) {  // queued mouse positions are in window pixels
      if (e.EventId <= w.lastScaledEvent) continue;
      w.lastScaledEvent = e.EventId;
      if (e.Type == ImGuiInputEventType_MousePos && e.MousePos.PosX > -FLT_MAX * 0.5f) {
        e.MousePos.PosX /= s;
        e.MousePos.PosY /= s;
      }
    }
  }
  ImGui::NewFrame();
  pluginbar::Bar bar;
  bar.app = w.app;
  bar.target = w.target;
  bar.type = w.type;
  bar.L = lay::barUnits(w.L, w.scale);
  bar.panelPage = w.panelPage;
  bar.dock = !w.external && videodriver::interfaceProblem().empty();  // 0.7.8: it can go inside Gridline
  pluginbar::draw(bar, hi, static_cast<float>(bar.L.bar.y));
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
    // K-437: Gridline's own part (bar and panel) on its own as well: the window server's image of the
    // whole window needs the screen recording permission, which a dev run must not ask for.
    if (std::FILE* f = std::fopen((w.capturePath + ".bar.ppm").c_str(), "wb")) {
      std::fprintf(f, "P6\n%d %d\n255\n", fbw, std::max(rows, 1));
      for (int y = std::max(rows, 1) - 1; y >= 0; --y)
        for (int x = 0; x < fbw; ++x) std::fwrite(&px[(static_cast<size_t>(y) * static_cast<size_t>(fbw) + static_cast<size_t>(x)) * 4], 1, 3, f);
      std::fclose(f);
    }
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
  static const bool trace = std::getenv("GRIDLINE_DEV_PLUGIN_KEYS") != nullptr;
  bool usedByPlugin = false;
  try {
    usedByPlugin = w.editor && !w.external && !(w.backend && w.backend->lost()) && w.editor->hostKey(true, sc, 0, modsOf(ev.key.keysym.mod));
  } catch (...) {
    if (w.backend) w.backend->markLost();
  }
  if (usedByPlugin) {
    if (trace) std::fprintf(stderr, "plugkey window %d used by the plugin\n", sc);
    return;
  }
  if (keepsKey(sc)) {
    if (trace) std::fprintf(stderr, "plugkey window %d kept (Smooth arrows)\n", sc);
    return;
  }
  if (trace) std::fprintf(stderr, "plugkey window %d to main\n", sc);
  w.heldKeys.insert(sc);
  sendToMain(true, ev.key.repeat != 0, sc, modsOf(ev.key.keysym.mod));
}

// Keys the plugin's interface did not use (in-process container or the
// sandbox window) go to the main window.
void forwardPluginKeys(PluginWindow& w) {
  static const bool trace = std::getenv("GRIDLINE_DEV_PLUGIN_KEYS") != nullptr;  // the yabridge test reads these lines
  host::KeyEvent k;
  if (w.container)
    while (native::popKey(w.container, k)) {
      if (trace) std::fprintf(stderr, "plugkey copy %s code %d mods %u\n", k.down ? "down" : "up", k.code, k.mods);
      sendToMain(k);
    }
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
void setPluginWindowUiScale(float scale) { gUiScale = scale; }
void setPluginWindowPanel(int page) { gStartPanel = std::clamp(page, -1, 3); }  // K-437: 3 the Internet page
void setPluginWindowDevAction(const std::string& action) { gDevAction = action; }
void setPluginWindowDrawStats(bool on) { gDrawStats = on; }
void setPluginWindowDevKeys(bool on) { gDevKeys = on; }
void setPluginWindowDevResize(int w, int h) {
  gDevResizeW = w;
  gDevResizeH = h;
}

bool hostedEditorOpen(const PluginTarget& t) { return windowFor(t) != nullptr; }

bool hostedEditorEngaged(const PluginTarget& t) {
  const PluginWindow* w = windowFor(t);
  if (!w) return false;
  if (w->docked) {  // inside Gridline: in use while the pointer is over its part of the host window, or it has the keyboard
    if (w->dead || !w->host || !w->container) return false;
    const lay::Rect& v = w->shownVisible;
    const bool shown = w->hiddenBy == 0 && v.w > 0 && v.h > 0;
    int gx = 0, gy = 0, hx = 0, hy = 0;
    SDL_GetGlobalMouseState(&gx, &gy);  // the native view takes the mouse events, so SDL's own window state goes stale
    SDL_GetWindowPosition(w->host, &hx, &hy);
    const int mx = gx - hx, my = gy - hy;
    const bool over = mx >= v.x && mx < v.right() && my >= v.y && my < v.bottom();
    const bool keys = native::keyFocus(w->container) != native::KeyFocus::Host &&
                      (SDL_GetWindowFlags(w->host) & SDL_WINDOW_INPUT_FOCUS) != 0;
    return idlepace::editorInUse(false, shown, over, keys);
  }
  const bool sdlWindow = w->win && !w->external;
  const bool shown = w->shown && w->hiddenBy == 0;
  return idlepace::editorInUse(!sdlWindow, shown, sdlWindow && SDL_GetMouseFocus() == w->win,
                               sdlWindow && (SDL_GetWindowFlags(w->win) & SDL_WINDOW_INPUT_FOCUS) != 0);
}

void devPrintPluginWindows(int frame) {
  std::printf("plugwin frame %d: %zu windows\n", frame, windows().size());
  for (const auto& [k, w] : windows()) {
    int x = 0, y = 0, ww = 0, wh = 0;
    if (w->win) {
      SDL_GetWindowPosition(w->win, &x, &y);
      SDL_GetWindowSize(w->win, &ww, &wh);
    }
    std::printf("  plugwin \"%s\" format %s external %d window 0x%lx container 0x%lx at %d,%d size %dx%d plugin %d,%d %dx%d resizable %d\n", w->name.c_str(),
                w->format.c_str(), w->external ? 1 : 0, static_cast<unsigned long>(reinterpret_cast<uintptr_t>(w->nw.handle)),
                static_cast<unsigned long>(reinterpret_cast<uintptr_t>(w->container)), x, y, ww, wh, w->L.plugin.x, w->L.plugin.y, w->L.plugin.w, w->L.plugin.h,
                w->resizable ? 1 : 0);
    if (w->docked)  // 0.7.8: inside Gridline
      std::printf("  plugwin \"%s\" inside 1 host 0x%lx at %d,%d size %dx%d visible %d,%d %dx%d holes %zu drawn %d\n", w->name.c_str(),
                  static_cast<unsigned long>(reinterpret_cast<uintptr_t>(w->nw.handle)), w->shownFull.x, w->shownFull.y, w->shownFull.w, w->shownFull.h,
                  w->shownVisible.x, w->shownVisible.y, w->shownVisible.w, w->shownVisible.h, w->shownHoles.size(), w->drawnFrame);
    std::printf("  plugwin \"%s\" slot %llu/%d level %ld shown %d aside %u instance 0x%lx\n", w->name.c_str(), static_cast<unsigned long long>(w->target.owner),
                w->target.slot, native::windowLevel(w->nw), w->shown ? 1 : 0, w->hiddenBy,
                static_cast<unsigned long>(reinterpret_cast<uintptr_t>(w->backend)));  // 0.7.8
  }
  std::fflush(stdout);
}

// Stress tests: the longest the plugin windows' idle took (their plugins' calls) and the longest
// time between two of them (a frame held up anywhere), since the last report.
namespace {
double gIdleLongestMs = 0, gGapLongestMs = 0;
std::chrono::steady_clock::time_point gLastIdle{};
}  // namespace
void devPrintPluginStalls(App& a, int frame) {
  std::printf("plugwin frame %d: stalls idle %.0f ms apart %.0f ms windows %zu closing %zu\n", frame, gIdleLongestMs, gGapLongestMs, windows().size(),
              closingWindows().size());
  for (const auto& [k, w] : windows()) {
    host::HostedInstance* hi = hostedInstanceFor(a, w->target);
    std::printf("  plugwin \"%s\" size %dx%d resizes %d responding %d crashed %d\n", w->name.c_str(), w->pw, w->ph, w->resizeAsks,
                hi && hi->backend()->notResponding() ? 0 : 1, hi && hi->crashed() ? 1 : 0);
  }
  std::fflush(stdout);
  gIdleLongestMs = gGapLongestMs = 0;
}

bool devMoveHostedEditor(int x, int y) {
  for (const auto& [k, w] : windows()) {
    if (!w || w->dead || !w->win || !gMainWin) continue;
    int mx = 0, my = 0;
    SDL_GetWindowPosition(gMainWin, &mx, &my);
    SDL_SetWindowPosition(w->win, mx + x, my + y);
    return true;
  }
  return false;
}

bool devDragHostedEditor(int x, int y, double seconds) {
  for (const auto& [k, w] : windows()) {
    if (!w || w->dead || !w->container || !w->win) continue;
#if defined(__APPLE__)
    native::devDrag(w->nw, w->container, x, y, seconds);
    return true;
#else
    (void)x, (void)y, (void)seconds;
    return false;
#endif
  }
  return false;
}

bool hostedEditorUnderMouse(PluginTarget& t, float& fx, float& fy) {
  // k078-collabplug: an interface inside Gridline takes the mouse itself (Gridline's own windows see
  // no movement over it): the pointer is in it when it is over the part of the host window it shows.
  for (const auto& [key, w] : windows()) {
    if (!w || w->dead || !w->docked || !w->host || !w->container || w->hiddenBy != 0) continue;
    const lay::Rect& v = w->shownVisible;
    if (v.w <= 0 || v.h <= 0) continue;
    int gx = 0, gy = 0, hx = 0, hy = 0;
    SDL_GetGlobalMouseState(&gx, &gy);
    SDL_GetWindowPosition(w->host, &hx, &hy);
    const int mx = gx - hx, my = gy - hy;
    if (mx < v.x || mx >= v.right() || my < v.y || my >= v.bottom()) continue;
    const lay::Rect& f = w->shownFull.w > 0 && w->shownFull.h > 0 ? w->shownFull : v;
    t = w->target;
    fx = std::clamp(static_cast<float>(mx - f.x) / static_cast<float>(std::max(1, f.w)), 0.0f, 1.0f);
    fy = std::clamp(static_cast<float>(my - f.y) / static_cast<float>(std::max(1, f.h)), 0.0f, 1.0f);
    return true;
  }
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

// K-112: a window set aside leaves the screen (unmapped, not closed): its interface, its instance and
// its place stay, and it comes back as it was when its last reason goes. The keyboard goes back to
// the main window first, as when a window closes. Sandboxed interfaces live in the sandbox's own
// window, which this process cannot hide: they are never set aside.
static void setAside(PluginWindow& w, unsigned reasons, bool hide) {
  if (w.dead || w.external || w.closing || w.docked) return;  // (inside Gridline: its pane shows or hides it)
  const unsigned before = w.hiddenBy;
  w.hiddenBy = hide ? (w.hiddenBy | reasons) : (w.hiddenBy & ~reasons);
  if (before == 0 && w.hiddenBy != 0 && w.shown && w.win) {
    w.giveKeyboardBack();
    SDL_HideWindow(w.win);
    w.shown = false;
    if (gKeyboardToMain && gMainWin) SDL_RaiseWindow(gMainWin);
    gKeyboardToMain = false;
    w.log("set aside");
  } else if (before != 0 && w.hiddenBy == 0 && !w.shown && w.win && w.complete) {
    showAboveMain(w, false);
    w.log("shown again");
  }
}

bool setHostedEditorHidden(const PluginTarget& t, bool hidden, unsigned reasons) {
  PluginWindow* w = windowFor(t);
  if (!w || w->external || w->docked) return false;
  setAside(*w, reasons, hidden);
  return true;
}

bool hostedEditorHidden(const PluginTarget& t) {
  const PluginWindow* w = windowFor(t);
  return w && w->hiddenBy != 0;
}

std::vector<PluginTarget> openHostedEditorTargets() {
  std::vector<PluginTarget> out;
  for (const auto& [k, w] : windows())
    if (!w->dead) out.push_back(w->target);
  return out;
}

bool toggleHostedEditorsHidden() {
  bool anyShown = false, anyHidden = false;
  for (const auto& [k, w] : windows()) {
    if (w->dead || w->external) continue;
    if (w->hiddenBy == 0) anyShown = true;
    if (w->hiddenBy & kHideAll) anyHidden = true;
  }
  const bool hide = anyShown || !anyHidden;
  for (auto& [k, w] : windows()) setAside(*w, kHideAll, hide);
  return hide;
}

void closeHostedEditor(App& a, const PluginTarget& t, const char* why) {
  auto it = windows().find(t.key());
  if (it == windows().end()) return;
  if (why && !it->second->closeWhy) it->second->closeWhy = why;
  const bool docked = it->second->docked;
  it->second->close();
  auto w = std::move(it->second);
  windows().erase(it);
  retire(std::move(w));
  if (docked) hostedui::hidePane(a, t);  // 0.7.8: inside Gridline its window closes with it
}

void closeAllHostedEditors(App& a) {
  for (auto& [k, w] : windows()) {
    w->close();
    retire(std::move(w));
  }
  windows().clear();
  (void)a;
}

bool hostedEditorAbandoned() { return gInterfaceAbandoned; }

void settleClosingHostedEditors() {
  auto& list = closingWindows();
  for (auto& w : list) {
    w->settleClosing(true);
    if (w->abandoned) (void)w.release();  // the helper thread may still call back into it
  }
  list.clear();
}

// The same failure clicked again renews its message rather than stacking another one.
static void notifyOnce(App& a, const std::string& text, const std::string& format) {
  const std::string details = format + ", video driver " + (SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "none");
  std::fprintf(stderr, "[plugin-window] %s (%s)\n", text.c_str(), details.c_str());
  for (Toast& t : a.toasts)
    if (t.english == text) {
      t.until = a.now + toast::lifetime(true);
      return;
    }
  a.notify(text, true, details);
}

bool openHostedEditor(App& a, const PluginTarget& t) {
  if (!freezeui::beforeEditorOpens(a, t)) return false;  // K-87: a frozen plugin is unloaded; a hibernating one wakes
  if (PluginWindow* ex = windowFor(t)) {
    if (ex->win) SDL_RaiseWindow(ex->win);
    if (ex->docked) hostedui::showInside(a, t);  // 0.7.8: its pane to the front
    return true;
  }
  for (auto& c : closingWindows())  // K-37: it opens as soon as its old interface has closed
    if (c->target.key() == t.key() && c->closing) {
      c->reopenAfterClose = true;
      return true;
    }
  host::HostedInstance* hi = hostedInstanceFor(a, t);
  if (!hi && hostedLoading(a, t)) {  // 0.7.8: it loads before the next frame: the window opens then
    hostedui::reopenLater(t);
    return true;
  }
  if (!hi || hi->crashed()) {
    a.notify(hi ? "This plugin crashed: reload it first" : "This plugin is not loaded", true);
    return false;
  }
  host::Backend* b = hi->backend();
  const host::HostedClass& cls = hi->hostedClass();
  if (!b->editorAvailable()) {
    if (!b->lost()) a.notify(cls.name + " has no interface of its own: Gridline shows its parameters");
    return false;
  }
  if (hostedui::opensInside(a, t)) {  // 0.7.8: inside Gridline: its pane shows it (drawDockedInterface opens it)
    hostedui::showInside(a, t);
    return true;
  }
  // K-153: on SDL's Wayland driver there is no X11 window of Gridline's to embed an interface in. With
  // XWayland the interface opens in a window of its own there (SideWindowEditor); an interface that
  // opens its own window anyway (LV2 external) opens as it is. Without XWayland: say why, before asking
  // the plugin for a view (Gridline's own panel with its parameters stays).
  const std::string problem = videodriver::interfaceProblem();
  if (!problem.empty() && !native::xServerAnswers()) {
    notifyOnce(a, cls.name + ": " + problem, b->format());
    return false;
  }
  std::unique_ptr<host::Editor> editor;
  native::installX11SafetyHandlers();
  crash::InsideScope inside("a plugin opening its window:", cls.name);
  try {
    editor = b->createEditor();
  } catch (...) {  // a bridged plugin whose other process is gone; the next pump reports the crash
    b->markLost();
    return false;
  }
  native::installX11SafetyHandlers();
  if (!editor) {
    a.notify(cls.name + ": its interface cannot be shown on this system", true);
    return false;
  }
  if (!problem.empty() && !editor->external()) {
    const std::string sideTitle = cls.name + "  \xC2\xB7  " + hostedTargetName(a, t);
    editor = std::make_unique<SideWindowEditor>(std::move(editor), sideTitle, std::max(1.0, static_cast<double>(uiscale::detected())));
    if (gLog) std::printf("plugwin \"%s\": in a window of its own on XWayland (Gridline is on Wayland)\n", cls.name.c_str());
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
#if defined(__linux__)  // X11: window pixels; the other systems as they were
  if (!w.external) w.scale = std::clamp(static_cast<double>(gUiScale), 1.0, 4.0);
#endif
  const auto place = places().find(t.key());
  const lay::Rect work = workArea(place != places().end() ? &place->second : nullptr);
  const std::string title = cls.name + "  \xC2\xB7  " + hostedTargetName(a, t);
  Uint32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_HIDDEN;

  if (w.external) {  // sandboxed (macOS), LV2 external, or K-153's XWayland window: the interface has its own window
    if (place != places().end()) {
      editor->placeX = place->second.first;
      editor->placeY = place->second.second;
    }
    if (!editor->attach(nullptr)) {
      if (!problem.empty())
        notifyOnce(a, cls.name + ": " + i18n::tr("its interface could not open in a window of its own on XWayland"), b->format());
      else
        a.notify(cls.name + ": the sandboxed interface could not be opened", true);
      return false;
    }
    w.computeLayout();
    // Docked under the interface's window it needs no frame; on Wayland (K-153) nothing docks it, so it
    // has the desktop's title bar to be moved by. There it is resizable: SDL 2.0's Wayland driver crashes
    // in the title bar library (libdecor) on a fixed-size window, as it did here on Weston.
    w.win = SDL_CreateWindow(title.c_str(), work.x, work.y, w.L.w, w.L.h, flags | (problem.empty() ? SDL_WINDOW_BORDERLESS : SDL_WINDOW_RESIZABLE));
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
  appicon::apply(w.win);  // K-14
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
    const bool hadContainer = w.container != nullptr;
    const bool attached = hadContainer && editor->attach(w.container);
    native::installX11SafetyHandlers();  // re-ensure plugin did not reset XSetErrorHandler(nullptr)
    if (!attached) {
      w.editor = std::move(editor);
      w.close();
      if (!hadContainer)
        notifyOnce(a, cls.name + ": Gridline could not make a window for its interface", b->format());
      else if (b->bridged())
        notifyOnce(a, cls.name + ": the plugin's bridge refused to open its interface. Check that it works in another host, and its logs", b->format());
      else
        notifyOnce(a, cls.name + ": the plugin refused to open its interface in Gridline's window", b->format());
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
    SDL_SetWindowMinimumSize(w.win, static_cast<int>(std::lround(lay::kMinWindowW * w.scale)), w.L.bar.h + lay::kMinPluginH);
    showAboveMain(w);
    if (!mouseHeld()) SDL_RaiseWindow(w.win);  // (during a press it stays out of the way of the keys, above)
  }
  w.editor = std::move(editor);
  b->onGoingAway = [raw]() {
    raw->instanceGoing = true;
    if (!raw->closeWhy) raw->closeWhy = "the plugin was unloaded or loaded again";
    raw->close();
  };
  w.complete = true;
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
      keys::processEvent(ev);  // GitHub #79
      ImGui::SetCurrentContext(prev);
    }
    if (ev.type == SDL_KEYDOWN || ev.type == SDL_KEYUP) routeKey(*w, ev);
    // A click on the window (its interface or its bar) brings it in front of the other plugin
    // windows: on macOS they are all child windows of the main window, which AppKit keeps in the
    // order they opened unless one is put on top again.
    if (gMainWin && (ev.type == SDL_MOUSEBUTTONDOWN || (ev.type == SDL_WINDOWEVENT && ev.window.event == SDL_WINDOWEVENT_FOCUS_GAINED)))
      native::raiseChild(w->nw, nativeOf(gMainWin));
    if (ev.type == SDL_WINDOWEVENT && w->container) {
      if (ev.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) native::restoreKeyFocus(w->container);
      if (ev.window.event == SDL_WINDOWEVENT_FOCUS_LOST) native::saveKeyFocus(w->container);
    }
    if (ev.type == SDL_WINDOWEVENT) {
      if (ev.window.event == SDL_WINDOWEVENT_CLOSE) {
        w->closeWhy = "its window was closed by its close button or the desktop";
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

// K-47: a driver reset. The bars of the plugin windows lose their GL with the main window; their ImGui
// contexts stay. (A plugin's own interface is the plugin's: it may draw with GL of its own.)
void pluginWindowsGraphicsLost() {
  ContextGuard g;
  auto drop = [](PluginWindow& w) {
    if (!w.ctx || !w.gl) return;
    makeCurrent(w.win, w.gl);
    ImGui::SetCurrentContext(w.ctx);
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    SDL_GL_MakeCurrent(w.win, nullptr);
    SDL_GL_DeleteContext(w.gl);
    w.gl = nullptr;
  };
  for (auto& [key, w] : windows()) drop(*w);
  for (auto& w : closingWindows()) drop(*w);
  g.win = nullptr;  // what it would restore is gone
  g.gl = nullptr;
}

void pluginWindowsGraphicsBack(SDL_Window* mainWindow, SDL_GLContext mainGl) {
  ContextGuard g;
  gMainWin = mainWindow;
  gMainGl = mainGl;
  auto back = [&](PluginWindow& w) {
    if (!w.ctx || !w.win || w.gl) return;
    makeCurrent(mainWindow, mainGl);
    SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);
    w.gl = SDL_GL_CreateContext(w.win);
    SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 0);
    if (!w.gl) return;  // (its bar stays undrawn; the plugin's own view is not affected)
    makeCurrent(w.win, w.gl);
    SDL_GL_SetSwapInterval(0);
    ImGui::SetCurrentContext(w.ctx);
    ImGui_ImplSDL2_InitForOpenGL(w.win, w.gl);
    ImGui_ImplOpenGL3_Init(glrecovery::glsl());
    glrecovery::restoreTextures();
  };
  for (auto& [key, w] : windows()) back(*w);
  for (auto& w : closingWindows()) back(*w);
  g.win = mainWindow;
  g.gl = mainGl;
}

// K-133: a plugin's own interface may draw with OpenGL of its own. The reset took that too, and
// the plugin is never told: its view would stay black or draw into a context that is gone. Every
// open interface is closed and opened again, where it was, so the plugin builds its view (and its
// OpenGL) anew. A sandboxed interface lives in the sandbox's process, which lost its context too.
int reopenHostedEditorsAfterGraphicsReset(App& a) {
  std::vector<PluginTarget> open;
  for (const auto& [key, w] : windows())
    if (w && !w->dead) open.push_back(w->target);
  for (const PluginTarget& t : open) closeHostedEditor(a, t);
  int reopened = 0;
  for (const PluginTarget& t : open) {
    const bool ok = openHostedEditor(a, t);
    reopened += ok ? 1 : 0;
    std::fprintf(stderr, "[plugin-window] interface %s after the graphics reset\n", ok ? "opened again" : "could not be opened again");
  }
  return reopened;
}

bool screenshotHostedEditor(const PluginTarget& t, const std::string& pngPath) {
  PluginWindow* w = windowFor(t);
  if (!w) return false;
  w->capturePath = pngPath;  // taken when the window draws next (this frame)
  return true;
}

// 0.7.8: an effect moved to another slot (or track) takes its running instance along: its window
// follows it, so it keeps showing that plugin and closes, saves and reloads with it.
static void followMovedInstances(App& a) {
  std::vector<std::pair<uint64_t, PluginTarget>> moves;
  for (const auto& [k, w] : windows()) {
    if (w->dead || !w->backend || !w->complete) continue;
    host::HostedInstance* hi = hostedInstanceFor(a, w->target);
    if (hi && hi->backend() == w->backend) continue;
    PluginTarget t;
    if (hostedTargetOf(a, w->backend, t) && t.key() != k) moves.push_back({k, t});
  }
  std::vector<std::unique_ptr<PluginWindow>> moved;
  for (const auto& [k, t] : moves) {
    auto it = windows().find(k);
    if (it == windows().end()) continue;
    std::unique_ptr<PluginWindow> w = std::move(it->second);
    windows().erase(it);
    w->target = t;
    if (w->win) SDL_SetWindowTitle(w->win, (w->name + "  \xC2\xB7  " + hostedTargetName(a, t)).c_str());
    w->log("follows its plugin to another slot");
    moved.push_back(std::move(w));
  }
  for (auto& w : moved) {
    const uint64_t k = w->target.key();
    if (windows().count(k)) {  // (cannot happen: one instance per slot) never two windows on one slot
      w->closeWhy = "another window shows this slot";
      w->close();
      retire(std::move(w));
      continue;
    }
    windows()[k] = std::move(w);
  }
}

// 0.7.8 (user report: plugin windows covered Gridline's menus and notices): where each plugin window
// is on screen, OS units (a sandboxed interface: its own window and the bar docked under it).
static void windowRects(const PluginWindow& w, std::vector<SDL_Rect>& out) {
  if (w.win) {
    SDL_Rect r{0, 0, 0, 0};
    SDL_GetWindowPosition(w.win, &r.x, &r.y);
    SDL_GetWindowSize(w.win, &r.w, &r.h);
    if (r.w > 0 && r.h > 0) out.push_back(r);
  }
  if (w.external && w.extFrame[2] > 0 && w.extFrame[0] != host::Editor::kNoPlace) out.push_back(SDL_Rect{w.extFrame[0], w.extFrame[1], w.extFrame[2], w.extFrame[3]});
}
// Gridline's overlays stay in front of the plugin windows: the windows' places go to the viewports,
// which lift a menu, popup, tooltip, modal dialog or notice over one into a top-most OS window of
// its own. What they cannot lift (a dialog, or every overlay where windows outside the main one are
// off) makes the plugin windows under it step aside (kHideCovered) until it closes. A window that
// stepped aside for that still counts where it was, so it does not come back while it is covered.
static void keepOverlaysClear() {
  std::vector<SDL_Rect> covers;
  for (const auto& [k, w] : windows())
    if (!w->dead && !w->closing && w->complete && (w->shown || w->hiddenBy == kHideCovered)) windowRects(*w, covers);
  viewports::setCovers(covers);
  const std::vector<viewports::OverlayRect>& over = viewports::overlaysOverCovers();
  for (auto& [k, w] : windows()) {
    if (w->dead || w->external || w->closing || !w->complete) continue;
    std::vector<SDL_Rect> mine;
    windowRects(*w, mine);
    bool covered = false;
    for (const viewports::OverlayRect& o : over)
      for (const SDL_Rect& r : mine)
        covered = covered || (!o.lifted && o.x < r.x + r.w && o.x + o.w > r.x && o.y < r.y + r.h && o.y + o.h > r.y);
    if (covered != ((w->hiddenBy & kHideCovered) != 0)) {
      if (gLog) std::fprintf(stderr, "[plugin-window] %s | %s\n", covered ? "steps aside for a Gridline window over it" : "back: nothing of Gridline's over it", w->name.c_str());
      setAside(*w, kHideCovered, covered);
    }
  }
}

// ---------------------------------------------------------------- 0.7.8: inside Gridline

namespace {
// Interfaces that would not sit inside Gridline this session (shown by a process of their own, or the
// plugin refused): they open in windows of their own.
std::set<uint64_t>& embedRefused() {
  static std::set<uint64_t> s;
  return s;
}
std::map<uint64_t, int>& dockAttempt() {  // the frame each target's interface last tried to open inside
  static std::map<uint64_t, int> m;
  return m;
}
std::set<int>& dockSwallowed() {  // scancodes whose key-down went to a docked interface
  static std::set<int> s;
  return s;
}

bool settingInside(App& a) {
  if (const char* e = std::getenv("GRIDLINE_PLUGIN_WINDOWS")) return std::strcmp(e, "own") != 0;  // tests: "own" or "inside"
  return a.settings.getBool("ui.pluginsInside");
}

SDL_Window* windowOfViewport(const ImGuiViewport* vp) {
  if (!vp || !vp->PlatformHandle) return nullptr;
  return SDL_GetWindowFromID(static_cast<Uint32>(reinterpret_cast<intptr_t>(vp->PlatformHandle)));
}

// Window units (points on macOS, pixels elsewhere) per ImGui unit: every Gridline window has the
// main window's interface scale.
float dockScale() {
  const ImGuiViewport* m = ImGui::GetMainViewport();
  if (gMainWin && m && m->Size.x > 0) {
    int ww = 0, wh = 0;
    SDL_GetWindowSize(gMainWin, &ww, &wh);
    if (ww > 0) return static_cast<float>(ww) / m->Size.x;
  }
  return gUiScale > 0 ? gUiScale : 1.0f;
}

lay::Rect toWindowUnits(ImVec2 p0, ImVec2 p1, ImVec2 origin, float s) {
  const int x0 = static_cast<int>(std::lround((p0.x - origin.x) * s)), y0 = static_cast<int>(std::lround((p0.y - origin.y) * s));
  const int x1 = static_cast<int>(std::lround((p1.x - origin.x) * s)), y1 = static_cast<int>(std::lround((p1.y - origin.y) * s));
  return lay::Rect{x0, y0, std::max(0, x1 - x0), std::max(0, y1 - y0)};
}

bool closingFor(const PluginTarget& t) {
  for (auto& c : closingWindows())
    if (c->target.key() == t.key() && c->closing) return true;
  return false;
}

// Closes a docked interface without touching its pane (it opens again where the pane is now).
void dropDocked(const PluginTarget& t, const char* why) {
  auto it = windows().find(t.key());
  if (it == windows().end() || !it->second->docked) return;
  if (!it->second->closeWhy) it->second->closeWhy = why;
  it->second->close();
  auto w = std::move(it->second);
  windows().erase(it);
  retire(std::move(w));
}

enum class DockOpen { Opened, OwnWindow, Failed, NotYet };

// Opens the target's interface inside `host` (one of Gridline's windows), hidden until it is placed.
DockOpen openDocked(App& a, const PluginTarget& t, SDL_Window* host) {
  host::HostedInstance* hi = hostedInstanceFor(a, t);
  if (!hi || hi->crashed()) return DockOpen::NotYet;
  if (!freezeui::beforeEditorOpens(a, t)) return DockOpen::NotYet;
  host::Backend* b = hi->backend();
  const host::HostedClass& cls = hi->hostedClass();
  if (!b->editorAvailable()) return DockOpen::Failed;
  std::unique_ptr<host::Editor> editor;
  native::installX11SafetyHandlers();
  {
    crash::InsideScope inside("a plugin opening its interface inside Gridline:", cls.name);
    try {
      editor = b->createEditor();
    } catch (...) {
      b->markLost();
      return DockOpen::NotYet;
    }
  }
  native::installX11SafetyHandlers();
  if (!editor) {
    a.notify(cls.name + ": its interface cannot be shown on this system", true);
    return DockOpen::Failed;
  }
  if (editor->external()) return DockOpen::OwnWindow;  // shown by a process (or a window) of its own
  if (gUiThread == std::thread::id()) gUiThread = std::this_thread::get_id();
  auto pw = std::make_unique<PluginWindow>();
  PluginWindow& w = *pw;
  w.app = &a;
  w.target = t;
  w.type = cls.type;
  w.name = cls.name;
  w.format = b->format();
  w.backend = b;
  w.docked = true;
  w.host = host;
  w.nw = nativeOf(host);
  int ew = 640, eh = 420;
  editor->size(ew, eh);
  w.askedW = ew, w.askedH = eh;
  w.pw = std::max(ew, lay::kMinPluginW);
  w.ph = std::max(eh, lay::kMinPluginH);
  w.resizable = editor->canResize();
  w.computeLayout();
  w.container = native::createDockedContainer(w.nw, lay::Rect{0, 0, w.pw, w.ph});
  if (!w.container) {
    notifyOnce(a, cls.name + ": Gridline could not make room for its interface", b->format());
    return DockOpen::Failed;
  }
  w.shownFull = lay::Rect{0, 0, w.pw, w.ph};
  native::setDockedFrame(w.nw, w.container, w.shownFull, lay::Rect{}, {});  // shown once placed
  {
    double cs = native::backingScale(w.nw);
#if !defined(__APPLE__) && !defined(_WIN32)
    cs = std::max(cs, static_cast<double>(uiscale::detected()));
#endif
    editor->setContentScale(cs);
  }
  PluginWindow* raw = &w;
  editor->requestResize = [raw](int nw, int nh) {
    if (raw->dead) return false;
    if (std::this_thread::get_id() != gUiThread) {  // applied on the next idle
      raw->pendingW.store(nw);
      raw->pendingH.store(nh);
      return true;
    }
    return raw->pluginResize(nw, nh);
  };
  bool attached = false;
  {
    crash::InsideScope inside("a plugin opening its interface inside Gridline:", cls.name);
    try {
      attached = editor->attach(w.container);
    } catch (...) {
      b->markLost();
    }
  }
  native::installX11SafetyHandlers();
  if (!attached) {
    w.editor = std::move(editor);
    w.close();
    if (b->bridged())
      notifyOnce(a, cls.name + ": the plugin's bridge refused to open its interface. Check that it works in another host, and its logs", b->format());
    else
      notifyOnce(a, cls.name + ": the plugin refused to open its interface inside Gridline", b->format());
    return DockOpen::Failed;
  }
  int w2 = w.pw, h2 = w.ph;  // some interfaces (VST2) know their size only once open
  if (editor->size(w2, h2) && (w2 != w.pw || h2 != w.ph)) w.pw = std::max(w2, lay::kMinPluginW), w.ph = std::max(h2, lay::kMinPluginH);
  if (auto sz = sizes().find(t.key()); w.resizable && sz != sizes().end() && (sz->second.first != w.pw || sz->second.second != w.ph)) {
    int rw = sz->second.first, rh = sz->second.second;
    editor->constrain(rw, rh);
    w.pw = std::max(rw, lay::kMinPluginW), w.ph = std::max(rh, lay::kMinPluginH);
    editor->setSize(w.pw, w.ph);
  }
  w.editor = std::move(editor);
  b->onGoingAway = [raw]() {
    raw->instanceGoing = true;
    if (!raw->closeWhy) raw->closeWhy = "the plugin was unloaded or loaded again";
    raw->close();
  };
  w.complete = true;
  w.log("opened inside Gridline");
  windows()[t.key()] = std::move(pw);
  return DockOpen::Opened;
}

// Gridline's windows, menus, tooltips and notices drawn in front of the docked interface's pane
// this frame, ImGui coordinates. True when a modal dialog is open over it (it hides then).
bool coversOf(const PluginWindow& w, const ImGuiViewport* vp, std::vector<ImRect>& out) {
  ImGuiContext& g = *GImGui;
  if (const ImGuiWindow* modal = ImGui::GetTopMostPopupModal();
      modal && modal->Viewport == vp && (!modal->RootWindowDockTree || modal->RootWindowDockTree->ID != w.hostRoot))
    return true;
  int hostAt = -1;
  for (int i = 0; i < g.Windows.Size; ++i)
    if (g.Windows[i]->ID == w.hostRoot) hostAt = i;
  const ImGuiWindowFlags overlay = ImGuiWindowFlags_Popup | ImGuiWindowFlags_Tooltip | ImGuiWindowFlags_ChildMenu | ImGuiWindowFlags_Modal;
  for (int i = 0; i < g.Windows.Size; ++i) {
    const ImGuiWindow* o = g.Windows[i];
    if (!o->Active || o->Hidden || o->Viewport != vp || o->Size.x <= 0 || o->Size.y <= 0) continue;
    const bool isOverlay = (o->Flags & overlay) != 0;
    if (!isOverlay && o->RootWindowDockTree && o->RootWindowDockTree->ID == w.hostRoot) continue;  // its own pane
    if (!isOverlay && (o->Flags & ImGuiWindowFlags_ChildWindow)) continue;  // inside a window counted already
    if (!isOverlay && i <= hostAt) continue;  // behind it
    out.push_back(o->Rect());
  }
  return false;
}
}  // namespace

bool hostedui::opensInside(App& a, const PluginTarget& t) {
  host::HostedInstance* hi = hostedInstanceFor(a, t);
  if (!hi) return false;
  bool canEmbed = videodriver::interfaceProblem().empty() && !embedRefused().count(t.key());
#if defined(__APPLE__)
  canEmbed = canEmbed && !hi->sandboxed();  // the sandbox process shows it in a window of its own
#endif
  return lay::chooseInterfacePlace(settingInside(a), host::hostOptions(hi->hostedClass().type).window, canEmbed) == lay::InterfacePlace::Inside;
}

void hostedui::putInterfaceInside(App& a, const PluginTarget& t, bool inside) {
  host::HostedInstance* hi = hostedInstanceFor(a, t);
  if (!hi) return;
  const std::string type = hi->hostedClass().type;
  host::HostOptions o = host::hostOptions(type);
  o.window = lay::interfaceChoiceFor(settingInside(a), inside);
  host::setHostOptions(type, o);
  embedRefused().erase(t.key());
  PluginWindow* w = windowFor(t);
  if (inside) {
    if (w && !w->docked) closeHostedEditor(a, t, "it was put inside Gridline");
    showInside(a, t);
  } else {
    if (w && w->docked) closeHostedEditor(a, t, "it was put in a window of its own");
    openHostedEditor(a, t);
  }
}

float hostedui::dockedWidth(App& a, const PluginTarget& t, float fallback) {
  if (!showsInterfaceInside(a, t)) return fallback;
  const float s = dockScale();
  if (const PluginWindow* w = windowFor(t); w && w->docked) return std::max(fallback, std::ceil(static_cast<float>(w->pw) / s));
  if (auto sz = sizes().find(t.key()); sz != sizes().end()) return std::max(fallback, std::ceil(static_cast<float>(sz->second.first) / s));
  return fallback;
}

void hostedui::drawDockedInterface(App& a, const PluginTarget& t, float width) {
  ImGuiWindow* iw = ImGui::GetCurrentWindow();
  if (!iw) return;
  ImGuiViewport* vp = iw->Viewport;
  SDL_Window* hostWin = windowOfViewport(vp);
  if (!gMainWin) {
    gMainWin = SDL_GL_GetCurrentWindow();
    gMainGl = SDL_GL_GetCurrentContext();
  }
  PluginWindow* w = windowFor(t);
  const float s = dockScale();
  if (w && !w->docked) {  // (its own window is open: the pane says so)
    ImGui::TextColored(theme::vec(theme::pal().textDim), "%s", i18n::tr("Its interface is open in a window of its own."));
    return;
  }
  if (w && hostWin && w->host != hostWin) {  // the pane was popped out or docked back: it opens again in its new window
    dropDocked(t, "its plugin window moved into another window");
    w = nullptr;
  }
  if (!w && hostWin && !closingFor(t)) {
    const int frame = ImGui::GetFrameCount();
    auto at = dockAttempt().find(t.key());
    if (at == dockAttempt().end() || frame - at->second > 30) {
      dockAttempt()[t.key()] = frame;
      const DockOpen r = openDocked(a, t, hostWin);
      if (r == DockOpen::Opened) {
        dockAttempt().erase(t.key());
        w = windowFor(t);
      } else if (r == DockOpen::OwnWindow || r == DockOpen::Failed) {
        embedRefused().insert(t.key());  // this session: its own window (Failed: Gridline's panel, with its Interface button)
        if (r == DockOpen::OwnWindow) {
          host::HostedInstance* hi = hostedInstanceFor(a, t);
          const std::string name = hi ? hi->hostedClass().name : std::string("This plugin");
          a.notify(name + ": " + i18n::tr("its interface runs in a process of its own here, so it opens in a window of its own"));
          a.later([&a, t] { openHostedEditor(a, t); });
        }
        return;
      }
    }
  }
  int pwi = 640, phi = 420;
  if (w) {
    pwi = w->pw, phi = w->ph;
  } else if (auto sz = sizes().find(t.key()); sz != sizes().end()) {
    pwi = sz->second.first, phi = sz->second.second;
  }
  const ImVec2 size(std::max(1.0f, std::round(static_cast<float>(pwi) / s)), std::max(1.0f, std::round(static_cast<float>(phi) / s)));
  if (width > size.x) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::floor((width - size.x) * 0.5f));
  const ImVec2 p0 = ImGui::GetCursorScreenPos(), p1(p0.x + size.x, p0.y + size.y);
  // Under the interface (seen while it opens, or where it is cut away): the editors' dark face.
  ImGui::GetWindowDrawList()->AddRectFilled(p0, p1, theme::darkFaces().surface950);
  ImGui::InvisibleButton("##dockedinterface", size);  // the pane does not move when a click comes through
  if (!w) return;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  w->drawnFrame = ImGui::GetFrameCount();
  w->regionMin = p0, w->regionMax = p1;
  w->clipMin = dl->GetClipRectMin(), w->clipMax = dl->GetClipRectMax();
  w->hostRoot = iw->RootWindowDockTree ? iw->RootWindowDockTree->ID : iw->ID;
  w->hostViewport = vp ? vp->ID : 0;
  if (w->container && native::takeDockedPress(w->container)) {  // a click on it brings its window to the front
    ImGui::FocusWindow(iw);
    w->clicked = true;
  }
  if (gDevCoverFrames > 0) {  // dev (tests): a tooltip in the middle of the interface
    --gDevCoverFrames;
    ImGui::SetNextWindowPos(ImVec2(std::floor((p0.x + p1.x) * 0.5f), std::floor((p0.y + p1.y) * 0.5f)), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::BeginTooltip();
    ImGui::TextUnformatted("Dev tooltip over an interface");
    ImGui::EndTooltip();
  }
}

void hostedui::devCoverDocked(int frames) { gDevCoverFrames = frames; }

void hostedui::placeDockedInterfaces() {
  if (windows().empty()) return;
  const int frame = ImGui::GetFrameCount();
  const float s = dockScale();
  const bool holesCut = native::dockedHolesCut();
  for (auto& [k, wp] : windows()) {
    PluginWindow& w = *wp;
    if (!w.docked || w.dead || !w.container || w.closing) continue;
    lay::Rect full = w.shownFull, visible;
    std::vector<lay::Rect> holes;
    const ImGuiViewport* vp = w.drawnFrame == frame ? ImGui::FindViewportByID(w.hostViewport) : nullptr;
    if (vp && windowOfViewport(vp) == w.host) {
      full = toWindowUnits(w.regionMin, w.regionMax, vp->Pos, s);
      full.w = w.pw, full.h = w.ph;  // the interface's own size (the pane rounds)
      const ImVec2 c0(std::max(w.clipMin.x, vp->Pos.x), std::max(w.clipMin.y, vp->Pos.y));
      const ImVec2 c1(std::min(w.clipMax.x, vp->Pos.x + vp->Size.x), std::min(w.clipMax.y, vp->Pos.y + vp->Size.y));
      const lay::Rect clip = toWindowUnits(c0, c1, vp->Pos, s);
      std::vector<ImRect> over;
      if (!coversOf(w, vp, over)) {
        std::vector<lay::Rect> covers;
        for (const ImRect& r : over) covers.push_back(toWindowUnits(r.Min, r.Max, vp->Pos, s));
        const lay::DockedShow d = lay::dockedShow(full, clip, covers, holesCut);
        visible = d.visible;
        holes = d.holes;
      }
    }
    // An interface opened, re-opened in another window or shown again never takes the keyboard on its
    // own (some plugins make their view first responder when it opens): only a click on it gives it.
    if (!w.clicked && native::keyFocus(w.container) != native::KeyFocus::Host) native::releaseKeyFocus(w.container);
    if (w.placed && full == w.shownFull && visible == w.shownVisible && holes == w.shownHoles) continue;  // nothing moved: the window system is not asked again
    native::setDockedFrame(w.nw, w.container, full, visible, holes);
    if (gLog && (visible.empty() != w.shownVisible.empty() || !w.placed))
      std::fprintf(stderr, "[plugin-window] %s | %s\n", visible.empty() ? "inside Gridline, hidden" : "inside Gridline, shown", w.name.c_str());
    w.shownFull = full, w.shownVisible = visible, w.shownHoles = holes;
    w.placed = true;
  }
}

bool hostedui::devCaptureMainWindow(const std::string& pngPath) {
  SDL_Window* m = gMainWin ? gMainWin : SDL_GL_GetCurrentWindow();
  return m && native::captureWindow(nativeOf(m), pngPath, nullptr, 0, 0);
}

bool hostedui::devDockedKey(int kind, bool space) {
  for (auto& [k, w] : windows()) {
    if (!w->docked || w->dead || !w->container) continue;
    if (kind == 3) kind = 2;  // 3: a text field that takes the keyboard on its own (no click), as some plugins do when they open
    else w->clicked = true;  // (as a click on it)
    if (kind >= 0 && !native::devTestKeyView(w->container, kind)) return false;
    if (space) {
      native::devSendKey(w->container, 0x31, true, 0, " ");
      native::devSendKey(w->container, 0x31, false, 0, " ");
    }
    return true;
  }
  return false;
}

void hostedui::keyboardToMainWindow() {
  if (!gMainWin || (SDL_GetWindowFlags(gMainWin) & SDL_WINDOW_HIDDEN)) return;
  bool ownWindow = false;
  for (const auto& [k, w] : windows()) {
    if (w->dead) continue;
    if (w->docked && w->container) {  // inside Gridline: as a click on Gridline's part of the window
      if (native::keyFocus(w->container) != native::KeyFocus::Host) native::releaseKeyFocus(w->container);
    } else if (w->win) {
      ownWindow = true;
    }
  }
  if (ownWindow && !(SDL_GetWindowFlags(gMainWin) & SDL_WINDOW_INPUT_FOCUS)) SDL_RaiseWindow(gMainWin);
}

bool hostedui::dockedKeyEvent(const SDL_Event& ev) {
  if (windows().empty()) return false;
  if (ev.type == SDL_KEYUP) return dockSwallowed().erase(static_cast<int>(ev.key.keysym.scancode)) > 0;
  Uint32 id = 0;
  switch (ev.type) {
    case SDL_KEYDOWN:
      if (ev.key.padding2 == kForwardedKey) return false;  // a key the interface passed on
      id = ev.key.windowID;
      break;
    case SDL_TEXTINPUT: id = ev.text.windowID; break;
    case SDL_TEXTEDITING: id = ev.edit.windowID; break;
    case SDL_MOUSEBUTTONDOWN: id = ev.button.windowID; break;
    default: return false;
  }
  for (const auto& [k, w] : windows()) {
    if (!w->docked || w->dead || !w->container || !w->host || SDL_GetWindowID(w->host) != id) continue;
    if (native::keyFocus(w->container) == native::KeyFocus::Host) continue;
    if (ev.type == SDL_MOUSEBUTTONDOWN) {  // a click on Gridline's part of the window: the keyboard comes back
      const lay::Rect& v = w->shownVisible;
      const bool onIt = ev.button.x >= v.x && ev.button.x < v.right() && ev.button.y >= v.y && ev.button.y < v.bottom();
      if (!onIt) native::releaseKeyFocus(w->container);
      return false;
    }
    if (ev.type == SDL_KEYDOWN) dockSwallowed().insert(static_cast<int>(ev.key.keysym.scancode));
    return true;  // the interface has the keyboard: the key was its own (unused ones come back through its container)
  }
  return false;
}

void hostedui::pluginWindowsIdle(App& a) {
  const auto idleStart = std::chrono::steady_clock::now();
  if (gLastIdle != std::chrono::steady_clock::time_point{})
    gGapLongestMs = std::max(gGapLongestMs, std::chrono::duration<double, std::milli>(idleStart - gLastIdle).count());
  struct Timed {  // (every way out of the idle below)
    std::chrono::steady_clock::time_point t0;
    ~Timed() {
      gLastIdle = std::chrono::steady_clock::now();
      gIdleLongestMs = std::max(gIdleLongestMs, std::chrono::duration<double, std::milli>(gLastIdle - t0).count());
    }
  } timed{idleStart};
  followMovedInstances(a);
  keepOverlaysClear();
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
    if (!w.dead && w.docked && !hostedui::showsInterfaceInside(a, w.target)) {  // 0.7.8: its pane closed or shows something else
      if (!w.closeWhy) w.closeWhy = "its plugin window closed or shows another plugin";
      w.close();
    }
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
        if (gDevAction == "offline") w.setPanel(3);  // K-437: the OFFLINE button opens the Internet page
        if (gDevAction.rfind("panel", 0) == 0) w.setPanel(std::atoi(gDevAction.c_str() + 5));  // "panel0".."panel3"
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
      try {
        crash::InsideScope inside("a plugin window's idle:", w.name);
        if (w.notifySize) {
          w.notifySize = false;
          w.editor->setSize(w.pw, w.ph);
        }
        w.editor->idle();
      } catch (...) {
        if (w.backend) w.backend->markLost();  // K-9: the instance's update closes this window
      }
      if (gDevKeys && w.container) devKeys(w);
      if (gDevKeys && w.external) devSandboxKeys(w);
      forwardPluginKeys(w);
#if defined(__linux__)
      if (w.container && (w.win || (w.docked && w.host))) {  // K-37: tell the interface where its window went
        int rx = 0, ry = 0;
        SDL_GetWindowPosition(w.win ? w.win : w.host, &rx, &ry);
        rx += w.shownFull.x, ry += w.shownFull.y;  // (inside Gridline: where its pane put it too)
        if (w.rootKnown && (rx != w.rootX || ry != w.rootY)) native::containerMoved(w.nw, w.container);
        w.rootX = rx, w.rootY = ry, w.rootKnown = true;
      }
#endif
      // An interface still opening in the plugin's own process that then refused: said so (it closes below).
      const bool opening = w.editor->opening();
      if (w.wasOpening && !opening && !w.editor->isOpen() && !(w.backend && w.backend->crashed()))
        a.notify(w.name + ": " + i18n::tr("its interface could not be opened"), true);
      if (w.wasOpening && !opening && w.editor->isOpen() && !w.external) {  // opened late: its own size now
        int sw = w.pw, sh = w.ph;
        if (w.editor->size(sw, sh) && (sw != w.pw || sh != w.ph)) w.pluginResize(sw, sh);
        if (w.editor->canResize() != w.resizable) {
          w.resizable = w.editor->canResize();
          if (w.win) SDL_SetWindowResizable(w.win, w.resizable ? SDL_TRUE : SDL_FALSE);
        }
        w.log("opened late");
      }
      w.wasOpening = opening;
      // The plugin's process is busy: its own window says so in its title (Gridline goes on; it closes as ever).
      if (const bool stuck = w.backend && w.backend->notResponding(); w.win && stuck != w.stuckTitle) {
        w.stuckTitle = stuck;
        const std::string title = w.name + "  \xC2\xB7  " + hostedTargetName(a, w.target);
        SDL_SetWindowTitle(w.win, (stuck ? title + "  (" + i18n::tr("not responding") + ")" : title).c_str());
      }
      if (!w.external && !w.editor->isOpen()) {  // a sandboxed interface whose process died
        w.closeWhy = "its interface closed itself";
        w.close();
      }
      if (w.external) {
        if (!w.editor->isOpen()) {
          w.closeWhy = "its interface closed itself";
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
              if (f[0] != host::Editor::kNoPlace) SDL_SetWindowPosition(w.win, f[0], f[1] + f[3]);  // docked under the sandbox window
              --w.applying;
              if (!w.shown) {
                showAboveMain(w);
                w.log("sandbox window docked");
              }
            }
          }
        }
      }
    }
    if (w.dead) {
      auto gone = std::move(it->second);
      it = windows().erase(it);
      retire(std::move(gone));
    } else {
      it = std::next(it);
    }
  }
  auto& closingList = closingWindows();
  for (size_t i = 0; i < closingList.size();) {
    if (!closingList[i]->settleClosing(false)) {
      ++i;
      continue;
    }
    std::unique_ptr<PluginWindow> w = std::move(closingList[i]);
    closingList.erase(closingList.begin() + static_cast<std::ptrdiff_t>(i));
    const bool reopenIt = w->reopenAfterClose && !w->abandoned;
    const PluginTarget t = w->target;
    if (w->abandoned) (void)w.release();  // the helper thread may still call back into it
    w.reset();
    if (reopenIt) openHostedEditor(a, t);
  }
  pluginbar::reopenPending(a);  // windows to show again on new instances
  routePluginWindows(a);
}

}  // namespace gl
