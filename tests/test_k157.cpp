// Gridline K-157 (GitHub issue #33): the main window is an ordinary top-level window that a tiling
// window manager tiles, not a floating one.
//
// Every platform: the main window (src/app/MainWindowRole.h, the program's own calls) ends
// resizable, decorated, with a minimum and no maximum size, and none of the flags that make a
// window manager treat it as a tool window; a hidden window stays hidden.
//
// X11 (Linux and the BSDs, where X11 development files exist): the test starts its own Xvfb (never
// the desktop's display) and a minimal window manager of its own on it, which, like i3, sway's
// XWayland support, Hyprland and bspwm, decides at a map request whether the window floats: fixed
// size (minimum == maximum), transient for another window, a dialog / utility / splash type or a
// modal state. Two readings of its records: "each map" (i3: decided again at every map) and
// "first map" (Hyprland's XWayland windows: decided once, at the X window's first map, and kept
// when it is unmapped and mapped again). The sequences up to 0.7.4 (hints changed on the mapped
// splash) and of 0.7.5 (the splash unmapped and mapped again) are run too, to show the model sees
// both reported faults; the main window from its first map (createMain) tiles under both readings
// and is mapped exactly once.
#ifndef SDL_MAIN_HANDLED
#define SDL_MAIN_HANDLED
#endif
#include "Check.h"
#include "app/MainWindowRole.h"
#include <SDL.h>
#include <SDL_syswm.h>
#include <cstdio>
#include <cstdlib>
#include <string>

#if defined(GRIDLINE_K157_X11)
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <atomic>
#include <chrono>
#include <csignal>
#include <fcntl.h>
#include <mutex>
#include <poll.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>
#endif

using namespace gl;

namespace {

[[maybe_unused]] void pump(int ms) {
  const Uint32 until = SDL_GetTicks() + static_cast<Uint32>(ms);
  while (SDL_GetTicks() < until) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
    }
    SDL_Delay(5);
  }
}

void checkFlags(SDL_Window* w) {
  const Uint32 f = SDL_GetWindowFlags(w);
  expect((f & SDL_WINDOW_RESIZABLE) != 0, "main window: resizable");
  expect((f & SDL_WINDOW_BORDERLESS) == 0, "main window: decorated");
  expect((f & (SDL_WINDOW_ALWAYS_ON_TOP | SDL_WINDOW_SKIP_TASKBAR)) == 0, "main window: not kept on top, in the task bar");
  expect((f & (SDL_WINDOW_UTILITY | SDL_WINDOW_TOOLTIP | SDL_WINDOW_POPUP_MENU)) == 0, "main window: not a utility, tooltip or menu");
  expect((f & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_FULLSCREEN_DESKTOP)) == 0, "main window: not fullscreen");
  int minW = 0, minH = 0, maxW = 0, maxH = 0;
  SDL_GetWindowMinimumSize(w, &minW, &minH);
  SDL_GetWindowMaximumSize(w, &maxW, &maxH);
  expect(minW == winrole::kMainMinW && minH == winrole::kMainMinH, "main window: minimum size 960x600");
  expect(maxW == 0 && maxH == 0, "main window: no maximum size");
}

#if defined(GRIDLINE_K157_X11)
// ---- the test's own X server
pid_t gXvfb = -1;
std::string startXvfb() {
  int fds[2];
  if (pipe(fds) != 0) return {};
  gXvfb = fork();
  if (gXvfb == 0) {
    close(fds[0]);
    const int null = open("/dev/null", O_WRONLY);
    if (null >= 0) dup2(null, 2);  // its notes on display numbers already in use
    const std::string fd = std::to_string(fds[1]);
    execlp("Xvfb", "Xvfb", "-displayfd", fd.c_str(), "-screen", "0", "1920x1080x24", "-nolisten", "tcp", static_cast<char*>(nullptr));
    _exit(127);
  }
  close(fds[1]);
  if (gXvfb < 0) {
    close(fds[0]);
    return {};
  }
  std::string num;
  pollfd p{fds[0], POLLIN, 0};
  for (int waited = 0; waited < 10000;) {
    if (poll(&p, 1, 200) > 0) {
      char c = 0;
      if (read(fds[0], &c, 1) != 1) break;
      if (c == '\n') break;
      num += c;
    } else {
      waited += 200;
    }
  }
  close(fds[0]);
  return num.empty() ? std::string() : ":" + num;
}
void stopXvfb() {
  if (gXvfb > 0) {
    kill(gXvfb, SIGTERM);
    waitpid(gXvfb, nullptr, 0);
    gXvfb = -1;
  }
}

// ---- what a tiling window manager sees when a window asks to be mapped
struct MapRecord {
  Window win = 0;
  long minW = 0, minH = 0, maxW = 0, maxH = 0;
  bool hasMin = false, hasMax = false, transient = false, modal = false, undecorated = false;
  std::string type;  // _NET_WM_WINDOW_TYPE, empty when not set
  bool fixed() const { return hasMin && hasMax && minW > 0 && minW == maxW && minH == maxH; }
  bool floats() const {
    const bool toolType = type == "_NET_WM_WINDOW_TYPE_DIALOG" || type == "_NET_WM_WINDOW_TYPE_UTILITY" ||
                          type == "_NET_WM_WINDOW_TYPE_TOOLBAR" || type == "_NET_WM_WINDOW_TYPE_SPLASH" ||
                          type == "_NET_WM_WINDOW_TYPE_MENU" || type == "_NET_WM_WINDOW_TYPE_POPUP_MENU" ||
                          type == "_NET_WM_WINDOW_TYPE_DROPDOWN_MENU" || type == "_NET_WM_WINDOW_TYPE_TOOLTIP" ||
                          type == "_NET_WM_WINDOW_TYPE_NOTIFICATION";
    return fixed() || transient || modal || toolType;
  }
  std::string text() const {
    char b[320];
    std::snprintf(b, sizeof b, "window 0x%lx: min %s max %s, type %s, transient %s, decorations %s%s -> %s", static_cast<unsigned long>(win),
                  hasMin ? (std::to_string(minW) + "x" + std::to_string(minH)).c_str() : "none",
                  hasMax ? (maxW >= 0x7fff ? std::string("unlimited") : std::to_string(maxW) + "x" + std::to_string(maxH)).c_str() : "none",
                  type.empty() ? "unset" : type.c_str(), transient ? "yes" : "no", undecorated ? "off" : "on", modal ? ", modal" : "",
                  floats() ? "FLOATS" : "tiles");
    return b;
  }
};

std::mutex gMu;
std::vector<MapRecord> gMaps;
std::atomic<bool> gStop{false}, gWmReady{false}, gWmFailed{false};

int quietErrors(Display*, XErrorEvent*) { return 0; }

// Interned once, before the first map request: interned in the middle of the first request's
// reads, the first window's properties came back wrong (Xlib 1.7 on Ubuntu 22.04).
Atom gTypeAtom = 0, gStateAtom = 0, gMotifAtom = 0;

std::vector<std::string> atomNames(Display* d, Window w, Atom prop) {
  std::vector<std::string> out;
  Atom type = 0;
  int format = 0;
  unsigned long n = 0, after = 0;
  unsigned char* data = nullptr;
  if (XGetWindowProperty(d, w, prop, 0, 64, False, XA_ATOM, &type, &format, &n, &after, &data) == Success && data) {
    if (format == 32)
      for (unsigned long i = 0; i < n; ++i) {
        char* name = XGetAtomName(d, reinterpret_cast<Atom*>(data)[i]);
        if (name) {
          out.emplace_back(name);
          XFree(name);
        }
      }
    XFree(data);
  }
  return out;
}

MapRecord readHints(Display* d, Window w) {
  MapRecord r;
  r.win = w;
  XSizeHints* sh = XAllocSizeHints();
  long supplied = 0;
  if (sh && XGetWMNormalHints(d, w, sh, &supplied)) {
    r.hasMin = (sh->flags & PMinSize) != 0;
    r.hasMax = (sh->flags & PMaxSize) != 0;
    r.minW = sh->min_width;
    r.minH = sh->min_height;
    r.maxW = sh->max_width;
    r.maxH = sh->max_height;
  }
  if (sh) XFree(sh);
  Window parent = 0;
  r.transient = XGetTransientForHint(d, w, &parent) != 0 && parent != 0;
  const auto types = atomNames(d, w, gTypeAtom);
  if (!types.empty()) r.type = types.front();
  for (const auto& s : atomNames(d, w, gStateAtom)) r.modal = r.modal || s == "_NET_WM_STATE_MODAL";
  Atom type = 0;
  int format = 0;
  unsigned long n = 0, after = 0;
  unsigned char* data = nullptr;
  if (XGetWindowProperty(d, w, gMotifAtom, 0, 5, False, AnyPropertyType, &type, &format, &n, &after, &data) == Success && data) {
    const long* v = reinterpret_cast<const long*>(data);
    if (format == 32 && n >= 3 && (v[0] & 2) != 0) r.undecorated = v[2] == 0;  // MWM_HINTS_DECORATIONS
    XFree(data);
  }
  return r;
}

void windowManager(std::string display) {
  Display* d = XOpenDisplay(display.c_str());
  if (!d) {
    gWmFailed = true;
    return;
  }
  const Window root = DefaultRootWindow(d);
  XSelectInput(d, root, SubstructureRedirectMask | SubstructureNotifyMask);
  XSync(d, False);
  XWindowAttributes ra{};
  XGetWindowAttributes(d, root, &ra);
  if ((ra.all_event_masks & SubstructureRedirectMask) == 0) gWmFailed = true;
  gTypeAtom = XInternAtom(d, "_NET_WM_WINDOW_TYPE", False);
  gStateAtom = XInternAtom(d, "_NET_WM_STATE", False);
  gMotifAtom = XInternAtom(d, "_MOTIF_WM_HINTS", False);
  XSync(d, False);
  gWmReady = true;
  while (!gStop) {
    while (XPending(d)) {
      XEvent e;
      XNextEvent(d, &e);
      if (e.type == MapRequest) {
        MapRecord r = readHints(d, e.xmaprequest.window);
        {
          std::lock_guard<std::mutex> lock(gMu);
          gMaps.push_back(r);
        }
        XMapWindow(d, e.xmaprequest.window);
        XFlush(d);
      } else if (e.type == ConfigureRequest) {
        const XConfigureRequestEvent& c = e.xconfigurerequest;
        XWindowChanges ch{};
        ch.x = c.x;
        ch.y = c.y;
        ch.width = c.width;
        ch.height = c.height;
        ch.border_width = c.border_width;
        ch.sibling = c.above;
        ch.stack_mode = c.detail;
        XConfigureWindow(d, c.window, static_cast<unsigned>(c.value_mask), &ch);
        XFlush(d);
      }
    }
    pollfd p{ConnectionNumber(d), POLLIN, 0};
    poll(&p, 1, 5);
  }
  XCloseDisplay(d);
}

Window xwindowOf(SDL_Window* w) {
  SDL_SysWMinfo info;
  SDL_VERSION(&info.version);
  if (!SDL_GetWindowWMInfo(w, &info) || info.subsystem != SDL_SYSWM_X11) return 0;
  return info.info.x11.window;
}

std::vector<MapRecord> mapsOf(Window w) {
  std::vector<MapRecord> out;
  std::lock_guard<std::mutex> lock(gMu);
  for (const auto& r : gMaps)
    if (r.win == w) out.push_back(r);
  return out;
}

void printMaps(const char* what, const std::vector<MapRecord>& maps) {
  std::printf("# %s: %zu map request(s)\n", what, maps.size());
  for (const auto& r : maps) std::printf("#   %s\n", r.text().c_str());
}
#endif

}  // namespace

int main() {
#if defined(GRIDLINE_K157_X11)
  XInitThreads();
  const std::string display = startXvfb();
  if (display.empty()) {
    std::printf("SKIP: Xvfb is not available (the test never uses the desktop's own display)\n");
    return 77;
  }
  setenv("DISPLAY", display.c_str(), 1);
  setenv("SDL_VIDEODRIVER", "x11", 1);
  XSetErrorHandler(quietErrors);
  std::thread wm(windowManager, display);
  while (!gWmReady && !gWmFailed) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  expect(!gWmFailed, "the test's window manager runs on its own X server");
#endif
  SDL_SetMainReady();
  if (SDL_Init(SDL_INIT_VIDEO) != 0) {
    std::printf("SKIP: no video (%s)\n", SDL_GetError());
#if defined(GRIDLINE_K157_X11)
    gStop = true;
    wm.join();
    stopXvfb();
#endif
    return 77;
  }
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

  // ---- hidden (automated runs): never shown, ends a normal main window
  {
    SDL_Window* w = winrole::loadsInMainWindow() ? winrole::createMain(true, {1200, 800, 40, 40}) : winrole::createSplash(true);
    expect(w != nullptr, "hidden window created");
    if (w) {
      const Uint32 f = SDL_GetWindowFlags(w);
      if (winrole::loadsInMainWindow())
        expect((f & SDL_WINDOW_HIDDEN) != 0 && (f & SDL_WINDOW_BORDERLESS) == 0 && (f & SDL_WINDOW_RESIZABLE) != 0,
               "X11 / Wayland: the start window is the main window already (hidden, decorated, resizable)");
      else
        expect((f & SDL_WINDOW_HIDDEN) != 0 && (f & SDL_WINDOW_BORDERLESS) != 0 && (f & SDL_WINDOW_RESIZABLE) == 0,
               "macOS / Windows: splash hidden, borderless, fixed size");
      winrole::becomeMain(w, {1200, 800, 40, 40});
      checkFlags(w);
      expect((SDL_GetWindowFlags(w) & SDL_WINDOW_HIDDEN) != 0, "hidden run: the main window stays hidden");
#if defined(GRIDLINE_K157_X11)
      pump(100);
      expect(mapsOf(xwindowOf(w)).empty(), "hidden run: no map request reaches the window manager");
#endif
      SDL_DestroyWindow(w);
    }
  }
#if defined(__APPLE__) || defined(_WIN32)
  expect(!winrole::loadsInMainWindow(), "macOS / Windows: a splash first, turned into the main window in place");
#else
  expect(winrole::loadsInMainWindow(), "X11 / Wayland: the program starts in the main window itself");
#endif

#if defined(GRIDLINE_K157_X11)
  // ---- up to 0.7.4: hints changed on the mapped splash, never mapped again
  {
    SDL_Window* w = winrole::createSplash(false);
    pump(100);
    SDL_SetWindowBordered(w, SDL_TRUE);
    SDL_SetWindowResizable(w, SDL_TRUE);
    SDL_SetWindowSize(w, 1200, 800);
    SDL_SetWindowPosition(w, 40, 40);
    SDL_SetWindowMinimumSize(w, winrole::kMainMinW, winrole::kMainMinH);
    pump(150);
    const auto maps = mapsOf(xwindowOf(w));
    printMaps("0.7.4 (hints changed while mapped)", maps);
    expect(maps.size() == 1 && maps.back().floats(), "model check: the 0.7.4 sequence leaves the main window floating");
    SDL_DestroyWindow(w);
  }
  // ---- 0.7.5: the splash unmapped and mapped again as a normal window
  {
    SDL_Window* w = winrole::createSplash(false);
    pump(100);
    SDL_HideWindow(w);
    SDL_SetWindowBordered(w, SDL_TRUE);
    SDL_SetWindowResizable(w, SDL_TRUE);
    SDL_SetWindowSize(w, 1200, 800);
    SDL_SetWindowPosition(w, 40, 40);
    SDL_SetWindowMinimumSize(w, winrole::kMainMinW, winrole::kMainMinH);
    SDL_ShowWindow(w);
    pump(150);
    const auto maps = mapsOf(xwindowOf(w));
    printMaps("0.7.5 (splash mapped again)", maps);
    expect(maps.size() == 2 && !maps.back().floats(), "model check: the 0.7.5 sequence tiles where the manager decides at each map (i3)");
    expect(!maps.empty() && maps.front().floats(), "model check: the 0.7.5 sequence floats where the first map decides (Hyprland, issue #33)");
    SDL_DestroyWindow(w);
  }
  // ---- now: the main window from its first map
  {
    SDL_Window* w = winrole::createMain(false, {1200, 800, 40, 40});
    pump(150);
    const Window xw = xwindowOf(w);
    expect(xw != 0, "X11 window of the main window");
    auto maps = mapsOf(xw);
    printMaps("first map (createMain)", maps);
    expect(maps.size() == 1, "main window: one map request");
    if (!maps.empty()) {
      const MapRecord& m = maps.front();
      expect(!m.floats(), "main window: tiled at its first map (managers that decide once tile it too)");
      expect(m.hasMin && m.minW == winrole::kMainMinW && m.minH == winrole::kMainMinH, "main window hints at the first map: minimum 960x600");
      expect(!m.hasMax || (m.maxW > m.minW && m.maxH > m.minH), "main window hints at the first map: maximum above the minimum");
      expect(m.type.empty() || m.type == "_NET_WM_WINDOW_TYPE_NORMAL", "main window hints: normal window type");
      expect(!m.transient, "main window hints: not transient for another window");
      expect(!m.undecorated, "main window hints: decorations on");
    }
    winrole::becomeMain(w, {1200, 800, 40, 40});
    pump(150);
    checkFlags(w);
    expect(mapsOf(xw).size() == 1, "main window: never unmapped and mapped again once loaded (issues #33, #52)");
    winrole::becomeMain(w, {1300, 820, 40, 40});  // a dev run's --size after the start
    pump(150);
    int cw = 0, ch = 0;
    SDL_GetWindowSize(w, &cw, &ch);
    expect(cw == 1300 && ch == 820 && mapsOf(xw).size() == 1, "a changed size is applied in place");
    Display* d = XOpenDisplay(display.c_str());
    if (d) {
      XWindowAttributes a{};
      XGetWindowAttributes(d, xw, &a);
      expect(a.map_state == IsViewable, "main window: on screen");
      XCloseDisplay(d);
    }
    SDL_DestroyWindow(w);
  }
#endif

  SDL_Quit();
#if defined(GRIDLINE_K157_X11)
  gStop = true;
  wm.join();
  stopXvfb();
#endif
  return finish();
}
