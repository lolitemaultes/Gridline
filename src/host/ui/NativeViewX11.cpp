// Gridline — native pieces of a plugin window (X11). Xlib is resolved at run
// time from the library SDL's X11 driver already loaded, so Gridline links no
// X11 library itself; without it the plugin attaches to the window directly.
//
// Keys (J-66 on X11, J-91): the X server sends a key to the window that has
// the keyboard - a window of the plugin's interface inside the container -
// so SDL (and Gridline's shortcuts) never see it. Gridline watches the
// container's window tree on an X connection of its own: every window of
// the interface that takes keys (another X client selected KeyPress on it)
// is selected by Gridline's connection too, and X sends Gridline a copy of
// each key the interface gets. X cannot tell whether the interface used a
// key, so only the keys of Gridline's global shortcuts are queued for
// Gridline (Space, F1-F12, Ctrl / Alt chords: host/KeyForward.h
// forwardsToHost); the interface keeps getting every key, its text fields
// included. Windows nobody takes keys on are left alone: their keys go up to
// Gridline's own window as before (SDL, PluginWindow.cpp routeKey).
#if !defined(__APPLE__) && !defined(_WIN32)
#include <dlfcn.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <initializer_list>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "NativeView.h"
#include "../../core/UiScale.h"

namespace gl::host::native {

namespace {
using XWin = unsigned long;

// The Xlib structures used here (layout fixed by the X11 ABI; no X headers needed).
struct XKeyEv {
  int type;
  unsigned long serial;
  int sendEvent;
  void* display;
  XWin window, root, subwindow;
  unsigned long time;
  int x, y, xRoot, yRoot;
  unsigned int state, keycode;
  int sameScreen;
};
union XEv {
  int type;
  XKeyEv key;
  long pad[24];
};
struct XWinAttrs {
  int x, y, width, height, borderWidth, depth;
  void* visual;
  XWin root;
  int cls, bitGravity, winGravity, backingStore;
  unsigned long backingPlanes, backingPixel;
  int saveUnder;
  unsigned long colormap;
  int mapInstalled, mapState;
  long allEventMasks, yourEventMask, doNotPropagateMask;
  int overrideRedirect;
  void* screen;
};
using XErrHandler = int (*)(void*, void*);

constexpr long kKeyPressMask = 1L << 0, kKeyReleaseMask = 1L << 1, kSubstructureNotifyMask = 1L << 19;
constexpr int kKeyPress = 2, kKeyRelease = 3, kCreateNotify = 16, kMapNotify = 19, kReparentNotify = 21;

struct Xlib {
  XWin (*createSimpleWindow)(void*, XWin, int, int, unsigned, unsigned, unsigned, unsigned long, unsigned long) = nullptr;
  int (*mapWindow)(void*, XWin) = nullptr;
  int (*moveResizeWindow)(void*, XWin, int, int, unsigned, unsigned) = nullptr;
  int (*destroyWindow)(void*, XWin) = nullptr;
  int (*setTransientForHint)(void*, XWin, XWin) = nullptr;
  int (*flush)(void*) = nullptr;
  char* (*resourceManagerString)(void*) = nullptr;
  // Keys (J-91).
  void* (*openDisplay)(const char*) = nullptr;
  int (*queryTree)(void*, XWin, XWin*, XWin*, XWin**, unsigned*) = nullptr;
  int (*selectInput)(void*, XWin, long) = nullptr;
  int (*getWindowAttributes)(void*, XWin, XWinAttrs*) = nullptr;
  int (*pending)(void*) = nullptr;
  int (*nextEvent)(void*, XEv*) = nullptr;
  int (*xfree)(void*) = nullptr;
  int (*sync)(void*, int) = nullptr;
  XErrHandler (*setErrorHandler)(XErrHandler) = nullptr;
  unsigned long (*keycodeToKeysym)(void*, unsigned char, int, int) = nullptr;  // XkbKeycodeToKeysym
  bool ok = false;
  bool keys = false;
};
const Xlib& xlib() {
  static Xlib x = [] {
    Xlib l;
    // libX11.so.6 (Linux, FreeBSD, NetBSD); OpenBSD numbers its libraries differently.
    void* h = nullptr;
    for (const char* name : {"libX11.so.6", "libX11.so"}) {
      if (!h) h = dlopen(name, RTLD_LAZY | RTLD_NOLOAD);
      if (!h) h = dlopen(name, RTLD_LAZY);
    }
    if (!h) return l;
    auto sym = [h](const char* n) { return dlsym(h, n); };
    l.createSimpleWindow = reinterpret_cast<decltype(l.createSimpleWindow)>(sym("XCreateSimpleWindow"));
    l.mapWindow = reinterpret_cast<decltype(l.mapWindow)>(sym("XMapWindow"));
    l.moveResizeWindow = reinterpret_cast<decltype(l.moveResizeWindow)>(sym("XMoveResizeWindow"));
    l.destroyWindow = reinterpret_cast<decltype(l.destroyWindow)>(sym("XDestroyWindow"));
    l.setTransientForHint = reinterpret_cast<decltype(l.setTransientForHint)>(sym("XSetTransientForHint"));
    l.flush = reinterpret_cast<decltype(l.flush)>(sym("XFlush"));
    l.resourceManagerString = reinterpret_cast<decltype(l.resourceManagerString)>(sym("XResourceManagerString"));
    l.openDisplay = reinterpret_cast<decltype(l.openDisplay)>(sym("XOpenDisplay"));
    l.queryTree = reinterpret_cast<decltype(l.queryTree)>(sym("XQueryTree"));
    l.selectInput = reinterpret_cast<decltype(l.selectInput)>(sym("XSelectInput"));
    l.getWindowAttributes = reinterpret_cast<decltype(l.getWindowAttributes)>(sym("XGetWindowAttributes"));
    l.pending = reinterpret_cast<decltype(l.pending)>(sym("XPending"));
    l.nextEvent = reinterpret_cast<decltype(l.nextEvent)>(sym("XNextEvent"));
    l.xfree = reinterpret_cast<decltype(l.xfree)>(sym("XFree"));
    l.sync = reinterpret_cast<decltype(l.sync)>(sym("XSync"));
    l.setErrorHandler = reinterpret_cast<decltype(l.setErrorHandler)>(sym("XSetErrorHandler"));
    l.keycodeToKeysym = reinterpret_cast<decltype(l.keycodeToKeysym)>(sym("XkbKeycodeToKeysym"));
    l.ok = l.createSimpleWindow && l.mapWindow && l.moveResizeWindow && l.destroyWindow && l.flush;
    l.keys = l.openDisplay && l.queryTree && l.selectInput && l.getWindowAttributes && l.pending && l.nextEvent && l.xfree && l.sync &&
             l.setErrorHandler && l.keycodeToKeysym;
    return l;
  }();
  return x;
}
XWin win(const void* p) { return static_cast<XWin>(reinterpret_cast<uintptr_t>(p)); }
void* ptr(XWin w) { return reinterpret_cast<void*>(static_cast<uintptr_t>(w)); }
unsigned dim(int v) { return static_cast<unsigned>(v > 1 ? v : 1); }

// ---- keys (J-91)
struct Watched {
  std::set<XWin> windows;  // windows of the interface whose keys Gridline gets a copy of
  std::deque<KeyEvent> queue;
  int rescanIn = 0;   // frames until the window tree is read again
  bool dirty = true;  // the tree changed (a window was created / mapped / reparented)
};
struct Keys {
  void* dpy = nullptr;  // Gridline's own connection: its event masks are its own
  bool tried = false;
  std::map<XWin, Watched> containers;
};
Keys& keyState() {
  static Keys k;
  return k;
}
void* keyDisplay() {
  Keys& k = keyState();
  if (!k.tried) {
    k.tried = true;
    if (xlib().keys) k.dpy = xlib().openDisplay(nullptr);
  }
  return k.dpy;
}

// X errors on Gridline's connection (a window of the interface went away
// between reading the tree and selecting on it) are expected and ignored;
// errors of other connections go to the handler that was installed before.
XErrHandler gPrevHandler = nullptr;
int ignoreOwnErrors(void* d, void* e) {
  if (d == keyState().dpy) return 0;
  return gPrevHandler ? gPrevHandler(d, e) : 0;
}
struct ErrorGuard {
  explicit ErrorGuard(void* d) : dpy(d) { gPrevHandler = xlib().setErrorHandler(ignoreOwnErrors); }
  ~ErrorGuard() {
    xlib().sync(dpy, 0);  // the errors of the calls above arrive while the guard is installed
    xlib().setErrorHandler(gPrevHandler);
  }
  ErrorGuard(const ErrorGuard&) = delete;
  ErrorGuard& operator=(const ErrorGuard&) = delete;
  void* dpy;
};

// Reads the container's window tree: tree changes are watched on every
// window, keys on the interface's windows that take keys.
void scan(void* dpy, XWin container, Watched& w) {
  const Xlib& x = xlib();
  ErrorGuard g(dpy);
  std::vector<XWin> todo{container};
  std::set<XWin> seen;
  while (!todo.empty() && seen.size() < 512) {
    const XWin cur = todo.back();
    todo.pop_back();
    if (!seen.insert(cur).second) continue;
    XWin root = 0, parent = 0, *kids = nullptr;
    unsigned n = 0;
    if (x.queryTree(dpy, cur, &root, &parent, &kids, &n)) {
      for (unsigned i = 0; i < n; ++i) todo.push_back(kids[i]);
      if (kids) x.xfree(kids);
    }
    long mask = kSubstructureNotifyMask;
    if (cur != container) {
      XWinAttrs at{};
      if (x.getWindowAttributes(dpy, cur, &at) && x11WatchesKeys(at.allEventMasks & ~at.yourEventMask)) mask |= kKeyPressMask | kKeyReleaseMask;
    }
    x.selectInput(dpy, cur, mask);
    if (mask & kKeyPressMask) w.windows.insert(cur);
  }
  for (auto it = w.windows.begin(); it != w.windows.end();) it = seen.count(*it) ? std::next(it) : w.windows.erase(it);
}

// Events on Gridline's connection: the copies of global shortcut keys are
// queued for the container whose interface got them.
void drain(void* dpy) {
  const Xlib& x = xlib();
  Keys& k = keyState();
  while (x.pending(dpy) > 0) {
    XEv e{};
    x.nextEvent(dpy, &e);
    if (e.type == kCreateNotify || e.type == kMapNotify || e.type == kReparentNotify) {
      for (auto& kv : k.containers) kv.second.dirty = true;
      continue;
    }
    if (e.type != kKeyPress && e.type != kKeyRelease) continue;
    for (auto& kv : k.containers) {
      Watched& w = kv.second;
      if (!w.windows.count(e.key.window)) continue;
      const int usage = hidUsageOfX11Keysym(x.keycodeToKeysym(dpy, static_cast<unsigned char>(e.key.keycode), 0, 0));
      KeyEvent ke;
      ke.down = e.type == kKeyPress;
      ke.code = macKeyOfHidUsage(usage);
      ke.mods = modsOfX11State(e.key.state);
      if (ke.code != 0xFFFF && forwardsToHost(usage, ke.mods)) w.queue.push_back(ke);
      break;
    }
  }
}
}  // namespace

std::string xResources(const Window& w) {
  const Xlib& x = xlib();
  if (!x.resourceManagerString || !w.display) return {};
  const char* r = x.resourceManagerString(w.display);
  return r ? std::string(r) : std::string();
}

PointerSize pointerSize(const Window& w) {
  PointerSize r;
  double size = 0;
  // 1. the X resource database (what the desktop writes for the X clients: device pixels)
  if (w.display) {
    const std::string res = xResources(w);
    if (const int n = uiscale::parseXcursorSize(res.c_str()); n > 0) size = n, r.source = "Xcursor.size " + std::to_string(n);
  }
  // 2. the environment (sessions set it for every client, Wayland ones too)
  if (size <= 0)
    if (const char* e = std::getenv("XCURSOR_SIZE"); e && *e) {
      const int n = std::atoi(e);
      if (n >= 8 && n <= 512) size = n, r.source = "XCURSOR_SIZE " + std::to_string(n);
    }
  // 3. GNOME's setting (logical pixels; asked once, gsettings reads dconf without a session bus)
  if (size <= 0) {
    static int gnome = -1;
    if (gnome < 0) {
      gnome = 0;
      if (std::getenv("GRIDLINE_NO_GSETTINGS") == nullptr)
        if (FILE* f = popen("gsettings get org.gnome.desktop.interface cursor-size 2>/dev/null", "r")) {
          char buf[64] = {0};
          if (std::fgets(buf, sizeof buf, f)) {
            const char* p = buf;
            while (*p && (*p < '0' || *p > '9')) ++p;  // "int32 24" or "24"
            const int n = std::atoi(p);
            if (n >= 8 && n <= 512) gnome = n;
          }
          pclose(f);
        }
    }
    if (gnome > 0) {
      double k = 1;
      if (const char* g = std::getenv("GDK_SCALE"); g && *g && w.display) k = std::max(1.0, std::atof(g));  // X11: device pixels
      size = gnome * k;
      r.source = "GNOME cursor-size " + std::to_string(gnome);
    }
  }
  if (size <= 0) r.source = "default 24";
  r.arrowH = uiscale::pointerArrowHeight(uiscale::PointerSystem::Unix, size, 1.0);
  return r;
}

void* createContainer(const Window& w, const layout::Rect& r) {
  if (!w.handle) return nullptr;
  const Xlib& x = xlib();
  if (!x.ok || !w.display) return w.handle;  // attach to the window itself (top-left)
  XWin c = x.createSimpleWindow(w.display, win(w.handle), r.x, r.y, dim(r.w), dim(r.h), 0, 0, 0);
  if (!c) return w.handle;
  x.mapWindow(w.display, c);
  x.flush(w.display);
  watchKeys(ptr(c));
  return ptr(c);
}

void setContainerFrame(const Window& w, void* container, const layout::Rect& r) {
  const Xlib& x = xlib();
  if (!x.ok || !w.display || !container || container == w.handle) return;
  x.moveResizeWindow(w.display, win(container), r.x, r.y, dim(r.w), dim(r.h));
  x.flush(w.display);
}

void destroyContainer(const Window& w, void* container) {
  const Xlib& x = xlib();
  if (!x.ok || !w.display || !container || container == w.handle) return;
  unwatchKeys(container);
  x.destroyWindow(w.display, win(container));
  x.flush(w.display);
}

void keepAbove(const Window& child, const Window& parent, bool on) {
  const Xlib& x = xlib();
  if (!x.ok || !x.setTransientForHint || !child.display || !child.handle || !parent.handle) return;
  x.setTransientForHint(child.display, win(child.handle), on ? win(parent.handle) : 0);
  x.flush(child.display);
}

Window windowOfView(void*) { return {}; }
double backingScale(const Window&) { return 1.0; }
long windowNumber(const Window&) { return -1; }
bool captureWindow(const Window&, const std::string&, const unsigned char*, int, int) { return false; }
void startDrawStats(void*) {}

// Keys (J-91, see the top of this file).
void watchKeys(void* container) {
  if (!container || !keyDisplay()) return;
  keyState().containers[win(container)];
}
void unwatchKeys(void* container) {
  Keys& k = keyState();
  auto it = k.containers.find(win(container));
  if (it == k.containers.end()) return;
  if (k.dpy) {
    ErrorGuard g(k.dpy);
    for (XWin w : it->second.windows) xlib().selectInput(k.dpy, w, 0);
    xlib().selectInput(k.dpy, win(container), 0);
  }
  k.containers.erase(it);
}
bool containerKey(void*, void*, bool) { return false; }
bool popKey(void* container, KeyEvent& e) {
  Keys& k = keyState();
  auto it = k.containers.find(win(container));
  if (it == k.containers.end() || !k.dpy) return false;
  Watched& w = it->second;
  if (w.queue.empty()) {  // once a frame (the caller drains the queue): the tree, then the events
    if (w.dirty || --w.rescanIn <= 0) {
      scan(k.dpy, it->first, w);
      w.dirty = false;
      w.rescanIn = 60;  // and about once a second: other connections' windows change without telling
    }
    drain(k.dpy);
  }
  if (w.queue.empty()) return false;
  e = w.queue.front();
  w.queue.pop_front();
  return true;
}
// X cannot tell which window of the interface has a text field: the
// interface keeps every key, Gridline takes only its global shortcuts.
KeyFocus keyFocus(void*) { return KeyFocus::Host; }
void saveKeyFocus(void*) {}
void restoreKeyFocus(void*) {}
void* devTestKeyView(void*, int) { return nullptr; }
bool devSendKey(void*, uint16_t, bool, uint32_t, const char*) { return false; }
DrawStats takeDrawStats() { return {}; }
void devDrag(const Window&, void*, int, int, double) {}
void devPostMouse(const Window&, int, double, double) {}
void orderAbove(const Window&, const Window&) {}
void devLiveResize(const Window&, double, double, int) {}
void devPerformClose(const Window&) {}
// J-116: X11 has no modal resize loop: ConfigureNotify arrives in the event pump, which returns.
bool inModalSizeLoop() { return false; }
bool frameClockStart(const Window&) { return false; }
void frameClockFollow(const Window&) {}
void frameClockWait() {}
void frameClockStop() {}
void keepAwake(bool) {}

}  // namespace gl::host::native
#endif
