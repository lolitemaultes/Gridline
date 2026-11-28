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
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <initializer_list>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>
#include "NativeView.h"
#include "X11ErrorPolicy.h"
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
struct XConfigureEv {
  int type;
  unsigned long serial;
  int sendEvent;
  void* display;
  XWin event, window;
  int x, y, width, height, borderWidth;
  XWin above;
  int overrideRedirect;
};
struct XClientMessageEv {
  int type;
  unsigned long serial;
  int sendEvent;
  void* display;
  XWin window;
  unsigned long messageType;
  int format;
  long l[5];
};
struct XAnyEv {
  int type;
  unsigned long serial;
  int sendEvent;
  void* display;
  XWin window;
};
union XEv {
  int type;
  XAnyEv any;
  XKeyEv key;
  XConfigureEv configure;
  XClientMessageEv client;
  long pad[24];
};
struct XSizeHintsRec {  // XSizeHints
  long flags;
  int x, y, width, height, minWidth, minHeight, maxWidth, maxHeight, widthInc, heightInc;
  int minAspectX, minAspectY, maxAspectX, maxAspectY;
  int baseWidth, baseHeight, winGravity;
};
struct XClassHintRec {  // XClassHint
  char* resName;
  char* resClass;
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
struct XErrorEv {  // XErrorEvent
  int type;
  void* display;
  XWin resourceid;
  unsigned long serial;
  unsigned char errorCode;
  unsigned char requestCode;
  unsigned char minorCode;
};
using XErrHandler = int (*)(void*, void*);
using XIOErrHandler = int (*)(void*);
using XIOErrExitHandler = void (*)(void*, void*);
using XGetErrorTextFn = int (*)(void*, int, char*, int);

constexpr long kKeyPressMask = 1L << 0, kKeyReleaseMask = 1L << 1, kStructureNotifyMask = 1L << 17, kSubstructureNotifyMask = 1L << 19;
constexpr int kKeyPress = 2, kKeyRelease = 3, kCreateNotify = 16, kMapNotify = 19, kReparentNotify = 21, kConfigureNotify = 22;
constexpr int kClientMessage = 33;
constexpr long kPMinSize = 1L << 4, kPMaxSize = 1L << 5;

// K-432 (GL-8006CE18): permanent non-fatal X11 error state and handlers.
// Stored outside xlib() so callbacks never recurse into xlib() static init.
x11policy::PolicyState& x11Policy() {
  static x11policy::PolicyState st;
  return st;
}
std::atomic<XGetErrorTextFn> gGetErrorText{nullptr};

void writeStderrRaw(const char* data, size_t len) noexcept {
  while (len > 0) {
    const ssize_t n = ::write(2, data, len);
    if (n <= 0) break;
    data += n;
    len -= static_cast<size_t>(n);
  }
}

int gridlineX11ErrorHandler(void* dpy, void* evPtr) {
  x11policy::ErrorEvent ev;
  ev.display = dpy;
  if (evPtr) {
    const auto* xe = static_cast<const XErrorEv*>(evPtr);
    if (!ev.display) ev.display = xe->display;
    ev.resourceId = static_cast<uint64_t>(xe->resourceid);
    ev.serial = static_cast<uint64_t>(xe->serial);
    ev.errorCode = xe->errorCode;
    ev.requestCode = xe->requestCode;
    ev.minorCode = xe->minorCode;
  }
  const x11policy::Evaluation eval = x11Policy().handleError(ev);
  if (eval.decision == x11policy::ErrorDecision::LogDetail || eval.decision == x11policy::ErrorDecision::LogSummary) {
    char errText[128] = {};
    if (XGetErrorTextFn fn = gGetErrorText.load(std::memory_order_acquire); fn && ev.display) {
      fn(ev.display, static_cast<int>(ev.errorCode), errText, static_cast<int>(sizeof errText));
      errText[sizeof(errText) - 1] = '\0';
    }
    char line[384] = {};
    const size_t len = x11policy::formatErrorMessage(ev, errText, eval, line, sizeof line);
    if (len > 0) writeStderrRaw(line, len);
  }
  return eval.returnCode;  // Always 0: never exit() on X11 protocol errors.
}

int gridlineX11IOErrorHandler(void* dpy) {
  // Per Xlib's contract, an XIOErrorHandler must not return (Xlib calls exit(1) immediately if it
  // does). Exit cleanly via _Exit(1) after logging so static destructors do not race with active
  // worker threads, and leave crash cleanExit unset so the next launch recovers any unsaved work.
  char line[256] = {};
  const int n = std::snprintf(line, sizeof line,
                              "[x11] fatal X11 I/O error on display %p (X server or XWayland connection lost); leaving cleanly without aborting\n", dpy);
  if (n > 0) writeStderrRaw(line, static_cast<size_t>(n) >= sizeof(line) ? sizeof(line) - 1 : static_cast<size_t>(n));
  std::fflush(nullptr);
  std::_Exit(1);
}

void gridlineX11IOErrorExitHandler(void* dpy, void*) {
  char line[192] = {};
  const int n = std::snprintf(line, sizeof line, "[x11] X11 display %p I/O connection closed (X server or XWayland disconnected)\n", dpy);
  if (n > 0) writeStderrRaw(line, static_cast<size_t>(n) >= sizeof(line) ? sizeof(line) - 1 : static_cast<size_t>(n));
}

struct Xlib {
  int (*initThreads)() = nullptr;
  XGetErrorTextFn getErrorText = nullptr;
  XIOErrHandler (*setIOErrorHandler)(XIOErrHandler) = nullptr;
  void (*setIOErrorExitHandler)(void*, XIOErrExitHandler, void*) = nullptr;
  uintptr_t defaultErrorFn = 0;
  uintptr_t defaultIOErrorFn = 0;
  XWin (*createSimpleWindow)(void*, XWin, int, int, unsigned, unsigned, unsigned, unsigned long, unsigned long) = nullptr;
  int (*mapWindow)(void*, XWin) = nullptr;
  int (*unmapWindow)(void*, XWin) = nullptr;  // 0.7.8: docked interfaces
  int (*moveResizeWindow)(void*, XWin, int, int, unsigned, unsigned) = nullptr;
  int (*destroyWindow)(void*, XWin) = nullptr;
  int (*setTransientForHint)(void*, XWin, XWin) = nullptr;
  int (*flush)(void*) = nullptr;
  char* (*resourceManagerString)(void*) = nullptr;
  // Keys (J-91).
  void* (*openDisplay)(const char*) = nullptr;
  int (*closeDisplay)(void*) = nullptr;
  int (*queryTree)(void*, XWin, XWin*, XWin*, XWin**, unsigned*) = nullptr;
  int (*selectInput)(void*, XWin, long) = nullptr;
  int (*getWindowAttributes)(void*, XWin, XWinAttrs*) = nullptr;
  int (*pending)(void*) = nullptr;
  int (*nextEvent)(void*, XEv*) = nullptr;
  int (*xfree)(void*) = nullptr;
  int (*sync)(void*, int) = nullptr;
  XErrHandler (*setErrorHandler)(XErrHandler) = nullptr;
  unsigned long (*keycodeToKeysym)(void*, unsigned char, int, int) = nullptr;  // XkbKeycodeToKeysym
  int (*translateCoordinates)(void*, XWin, XWin, int, int, int*, int*, XWin*) = nullptr;
  int (*sendEvent)(void*, XWin, int, long, XEv*) = nullptr;
  // Side windows (K-153).
  unsigned long (*internAtom)(void*, const char*, int) = nullptr;
  int (*setWMProtocols)(void*, XWin, unsigned long*, int) = nullptr;
  int (*storeName)(void*, XWin, const char*) = nullptr;
  int (*changeProperty)(void*, XWin, unsigned long, unsigned long, int, int, const unsigned char*, int) = nullptr;
  void (*setWMNormalHints)(void*, XWin, XSizeHintsRec*) = nullptr;
  int (*setClassHint)(void*, XWin, XClassHintRec*) = nullptr;
  int (*resizeWindow)(void*, XWin, unsigned, unsigned) = nullptr;
  int (*defaultScreen)(void*) = nullptr;
  XWin (*rootWindow)(void*, int) = nullptr;
  int (*eventsQueued)(void*, int) = nullptr;
  int (*connectionNumber)(void*) = nullptr;
  bool ok = false;
  bool keys = false;
};

void ensureSafetyHandlers(const Xlib& x) {
  if (x.getErrorText) gGetErrorText.store(x.getErrorText, std::memory_order_release);
  // Serialize swap-and-check across threads so concurrent calls cannot race on XSetErrorHandler.
  static std::mutex mu;
  std::lock_guard<std::mutex> lk(mu);
  if (x.setErrorHandler) {
    XErrHandler prev = x.setErrorHandler(gridlineX11ErrorHandler);
    const uintptr_t prevAddr = x11policy::functionAddress(prev);
    const uintptr_t ourAddr = x11policy::functionAddress(gridlineX11ErrorHandler);
    Dl_info info{};
    const bool haveSym = prevAddr != 0 && dladdr(reinterpret_cast<const void*>(prevAddr), &info) != 0 && info.dli_sname != nullptr;
    const x11policy::HandlerKind kind =
        x11policy::classifyHandler(prevAddr, ourAddr, x.defaultErrorFn, x11policy::isDefaultErrorSymbol(haveSym ? info.dli_sname : nullptr));
    if (kind == x11policy::HandlerKind::ExternalCustom) x.setErrorHandler(prev);
  }
  if (x.setIOErrorHandler) {
    XIOErrHandler prev = x.setIOErrorHandler(gridlineX11IOErrorHandler);
    const uintptr_t prevAddr = x11policy::functionAddress(prev);
    const uintptr_t ourAddr = x11policy::functionAddress(gridlineX11IOErrorHandler);
    Dl_info info{};
    const bool haveSym = prevAddr != 0 && dladdr(reinterpret_cast<const void*>(prevAddr), &info) != 0 && info.dli_sname != nullptr;
    const x11policy::HandlerKind kind =
        x11policy::classifyHandler(prevAddr, ourAddr, x.defaultIOErrorFn, x11policy::isDefaultIOErrorSymbol(haveSym ? info.dli_sname : nullptr));
    if (kind == x11policy::HandlerKind::ExternalCustom) x.setIOErrorHandler(prev);
  }
}

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
    l.initThreads = reinterpret_cast<decltype(l.initThreads)>(sym("XInitThreads"));
    if (l.initThreads) l.initThreads();  // K-432: before any XOpenDisplay (especially on Wayland!)
    l.getErrorText = reinterpret_cast<decltype(l.getErrorText)>(sym("XGetErrorText"));
    l.setIOErrorHandler = reinterpret_cast<decltype(l.setIOErrorHandler)>(sym("XSetIOErrorHandler"));
    l.setIOErrorExitHandler = reinterpret_cast<decltype(l.setIOErrorExitHandler)>(sym("XSetIOErrorExitHandler"));
    l.defaultErrorFn = reinterpret_cast<uintptr_t>(sym("_XDefaultError"));
    l.defaultIOErrorFn = reinterpret_cast<uintptr_t>(sym("_XDefaultIOError"));
    l.createSimpleWindow = reinterpret_cast<decltype(l.createSimpleWindow)>(sym("XCreateSimpleWindow"));
    l.mapWindow = reinterpret_cast<decltype(l.mapWindow)>(sym("XMapWindow"));
    l.unmapWindow = reinterpret_cast<decltype(l.unmapWindow)>(sym("XUnmapWindow"));
    l.moveResizeWindow = reinterpret_cast<decltype(l.moveResizeWindow)>(sym("XMoveResizeWindow"));
    l.destroyWindow = reinterpret_cast<decltype(l.destroyWindow)>(sym("XDestroyWindow"));
    l.setTransientForHint = reinterpret_cast<decltype(l.setTransientForHint)>(sym("XSetTransientForHint"));
    l.flush = reinterpret_cast<decltype(l.flush)>(sym("XFlush"));
    l.resourceManagerString = reinterpret_cast<decltype(l.resourceManagerString)>(sym("XResourceManagerString"));
    l.openDisplay = reinterpret_cast<decltype(l.openDisplay)>(sym("XOpenDisplay"));
    l.closeDisplay = reinterpret_cast<decltype(l.closeDisplay)>(sym("XCloseDisplay"));
    l.queryTree = reinterpret_cast<decltype(l.queryTree)>(sym("XQueryTree"));
    l.selectInput = reinterpret_cast<decltype(l.selectInput)>(sym("XSelectInput"));
    l.getWindowAttributes = reinterpret_cast<decltype(l.getWindowAttributes)>(sym("XGetWindowAttributes"));
    l.pending = reinterpret_cast<decltype(l.pending)>(sym("XPending"));
    l.nextEvent = reinterpret_cast<decltype(l.nextEvent)>(sym("XNextEvent"));
    l.xfree = reinterpret_cast<decltype(l.xfree)>(sym("XFree"));
    l.sync = reinterpret_cast<decltype(l.sync)>(sym("XSync"));
    l.setErrorHandler = reinterpret_cast<decltype(l.setErrorHandler)>(sym("XSetErrorHandler"));
    l.keycodeToKeysym = reinterpret_cast<decltype(l.keycodeToKeysym)>(sym("XkbKeycodeToKeysym"));
    l.translateCoordinates = reinterpret_cast<decltype(l.translateCoordinates)>(sym("XTranslateCoordinates"));
    l.sendEvent = reinterpret_cast<decltype(l.sendEvent)>(sym("XSendEvent"));
    l.internAtom = reinterpret_cast<decltype(l.internAtom)>(sym("XInternAtom"));
    l.setWMProtocols = reinterpret_cast<decltype(l.setWMProtocols)>(sym("XSetWMProtocols"));
    l.storeName = reinterpret_cast<decltype(l.storeName)>(sym("XStoreName"));
    l.changeProperty = reinterpret_cast<decltype(l.changeProperty)>(sym("XChangeProperty"));
    l.setWMNormalHints = reinterpret_cast<decltype(l.setWMNormalHints)>(sym("XSetWMNormalHints"));
    l.setClassHint = reinterpret_cast<decltype(l.setClassHint)>(sym("XSetClassHint"));
    l.resizeWindow = reinterpret_cast<decltype(l.resizeWindow)>(sym("XResizeWindow"));
    l.defaultScreen = reinterpret_cast<decltype(l.defaultScreen)>(sym("XDefaultScreen"));
    l.rootWindow = reinterpret_cast<decltype(l.rootWindow)>(sym("XRootWindow"));
    l.eventsQueued = reinterpret_cast<decltype(l.eventsQueued)>(sym("XEventsQueued"));
    l.connectionNumber = reinterpret_cast<decltype(l.connectionNumber)>(sym("XConnectionNumber"));
    l.ok = l.createSimpleWindow && l.mapWindow && l.moveResizeWindow && l.destroyWindow && l.flush;
    l.keys = l.openDisplay && l.queryTree && l.selectInput && l.getWindowAttributes && l.pending && l.nextEvent && l.xfree && l.sync &&
             l.setErrorHandler && l.keycodeToKeysym;
    ensureSafetyHandlers(l);
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
    const Xlib& x = xlib();
    if (x.keys) {
      k.dpy = x.openDisplay(nullptr);
      if (k.dpy && x.setIOErrorExitHandler) x.setIOErrorExitHandler(k.dpy, gridlineX11IOErrorExitHandler, nullptr);
    }
  }
  return k.dpy;
}

// K-432 (GL-8006CE18): X errors on Gridline's own connection (a plugin window went away between
// reading the tree and selecting on it) are suppressed silently via lock-free guard slots inside
// the permanent handler — never by swapping XSetErrorHandler (which races with plugin threads and
// previously restored libX11's fatal _XDefaultError on Wayland).
struct ErrorGuard {
  explicit ErrorGuard(void* d) : dpy(d) {
    installX11SafetyHandlers();
    slot = x11Policy().enterGuard(dpy);
  }
  ~ErrorGuard() {
    if (dpy && xlib().sync) xlib().sync(dpy, 0);  // flush errors for dpy while guard is active
    x11Policy().leaveGuard(dpy, slot);
  }
  ErrorGuard(const ErrorGuard&) = delete;
  ErrorGuard& operator=(const ErrorGuard&) = delete;
  void* dpy = nullptr;
  int slot = -1;
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
      if (x.getWindowAttributes(dpy, cur, &at) && x11WatchesKeys(at.allEventMasks & ~at.yourEventMask, w.windows.count(cur) != 0)) mask |= kKeyPressMask | kKeyReleaseMask;
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

// 0.7.8: docked interfaces. A child window clips its children to itself: the container sits in a
// clipping window of its own, moved and sized to the part shown. X cannot cut holes here without
// the shape extension, so layout::dockedShow hides an interface with something in front of it.
namespace {
struct DockedX {
  XWin clip = 0;
  bool mapped = true;
};
std::map<void*, DockedX>& dockedX() {
  static std::map<void*, DockedX> m;
  return m;
}
}  // namespace

void* createDockedContainer(const Window& w, const layout::Rect& full) {
  const Xlib& x = xlib();
  if (!w.handle || !x.ok || !x.unmapWindow || !w.display) return nullptr;
  XWin clip = x.createSimpleWindow(w.display, win(w.handle), full.x, full.y, dim(full.w), dim(full.h), 0, 0, 0);
  if (!clip) return nullptr;
  XWin c = x.createSimpleWindow(w.display, clip, 0, 0, dim(full.w), dim(full.h), 0, 0, 0);
  if (!c) {
    x.destroyWindow(w.display, clip);
    x.flush(w.display);
    return nullptr;
  }
  x.mapWindow(w.display, c);
  x.mapWindow(w.display, clip);
  x.flush(w.display);
  dockedX()[ptr(c)] = DockedX{clip, true};
  watchKeys(ptr(c));
  return ptr(c);
}

void setDockedFrame(const Window& w, void* container, const layout::Rect& full, const layout::Rect& visible, const std::vector<layout::Rect>&) {
  const Xlib& x = xlib();
  auto it = dockedX().find(container);
  if (it == dockedX().end() || !x.ok || !x.unmapWindow || !w.display) return;
  DockedX& d = it->second;
  if (visible.empty()) {
    if (d.mapped) x.unmapWindow(w.display, d.clip);
    d.mapped = false;
  } else {
    x.moveResizeWindow(w.display, d.clip, visible.x, visible.y, dim(visible.w), dim(visible.h));
    x.moveResizeWindow(w.display, win(container), full.x - visible.x, full.y - visible.y, dim(full.w), dim(full.h));
    if (!d.mapped) x.mapWindow(w.display, d.clip);
    d.mapped = true;
  }
  x.flush(w.display);
}

void destroyDockedContainer(const Window& w, void* container) {
  const Xlib& x = xlib();
  auto it = dockedX().find(container);
  if (it == dockedX().end()) return;
  const XWin clip = it->second.clip;
  dockedX().erase(it);
  unwatchKeys(container);
  if (!x.ok || !w.display) return;
  x.destroyWindow(w.display, win(container));
  x.destroyWindow(w.display, clip);
  x.flush(w.display);
}

bool dockedHolesCut() { return false; }
bool takeDockedPress(void*) { return false; }
void releaseKeyFocus(void*) {}

void containerMoved(const Window& w, void* container) {
  const Xlib& x = xlib();
  if (!x.ok || !x.sendEvent || !x.translateCoordinates || !x.getWindowAttributes || !w.display || !container || container == w.handle) return;
  XWinAttrs at{};
  if (!x.getWindowAttributes(w.display, win(container), &at)) return;
  int rx = 0, ry = 0;
  XWin child = 0;
  if (!x.translateCoordinates(w.display, win(container), at.root, 0, 0, &rx, &ry, &child)) return;
  XEv e{};
  e.configure.type = kConfigureNotify;
  e.configure.display = w.display;
  e.configure.event = win(container);
  e.configure.window = win(container);
  e.configure.x = rx;
  e.configure.y = ry;
  e.configure.width = at.width;
  e.configure.height = at.height;
  e.configure.borderWidth = at.borderWidth;
  x.sendEvent(w.display, win(container), 0, kStructureNotifyMask, &e);
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

void installX11SafetyHandlers() {
  const Xlib& x = xlib();
  ensureSafetyHandlers(x);
}

Window windowOfView(void*) { return {}; }
bool xServerAnswers() {
  const char* name = std::getenv("DISPLAY");
  if (!name || !*name) return false;
  const Xlib& x = xlib();
  if (!x.openDisplay || !x.closeDisplay) return false;
  void* d = x.openDisplay(nullptr);
  if (!d) return false;
  x.closeDisplay(d);
  return true;
}

// ---- side windows (K-153: plugin interfaces when Gridline's windows are Wayland windows)
namespace {
struct Side {
  bool closed = false;
  int w = 0, h = 0;
  bool sized = false;  // the window manager changed the size since the last pump
};
struct SideState {
  void* dpy = nullptr;
  bool tried = false;
  unsigned long wmProtocols = 0, wmDelete = 0;
  std::map<XWin, Side> windows;
};
SideState& sideState() {
  static SideState st;
  return st;
}
bool sideReady() {
  const Xlib& x = xlib();
  return x.ok && x.openDisplay && x.internAtom && x.setWMProtocols && x.storeName && x.setWMNormalHints && x.resizeWindow &&
         x.defaultScreen && x.rootWindow && x.selectInput && x.pending && x.nextEvent;
}
void* sideDisplay() {
  SideState& st = sideState();
  if (!st.tried) {
    st.tried = true;
    const char* name = std::getenv("DISPLAY");
    const Xlib& x = xlib();
    if (name && *name && sideReady()) st.dpy = x.openDisplay(nullptr);
    if (st.dpy) {
      if (x.setIOErrorExitHandler) x.setIOErrorExitHandler(st.dpy, gridlineX11IOErrorExitHandler, nullptr);
      st.wmProtocols = x.internAtom(st.dpy, "WM_PROTOCOLS", 0);
      st.wmDelete = x.internAtom(st.dpy, "WM_DELETE_WINDOW", 0);
    }
  }
  return st.dpy;
}
void sideHints(void* dpy, XWin w, int width, int height, bool resizable) {
  XSizeHintsRec hints{};
  if (!resizable) {
    hints.flags = kPMinSize | kPMaxSize;
    hints.minWidth = hints.maxWidth = width;
    hints.minHeight = hints.maxHeight = height;
  } else {
    hints.flags = kPMinSize;
    hints.minWidth = hints.minHeight = 64;
  }
  xlib().setWMNormalHints(dpy, w, &hints);
}
void sideDrain() {
  SideState& st = sideState();
  const Xlib& x = xlib();
  while (st.dpy && x.pending(st.dpy) > 0) {
    XEv e{};
    x.nextEvent(st.dpy, &e);
    auto it = st.windows.find(e.any.window);
    if (it == st.windows.end()) continue;
    if (e.type == kClientMessage && e.client.messageType == st.wmProtocols && static_cast<unsigned long>(e.client.l[0]) == st.wmDelete)
      it->second.closed = true;
    if (e.type == kConfigureNotify && (e.configure.width != it->second.w || e.configure.height != it->second.h)) {
      it->second.w = e.configure.width, it->second.h = e.configure.height;
      it->second.sized = true;
    }
  }
}
}  // namespace

void* sideWindowOpen(const std::string& title, int w, int h, bool resizable) {
  installX11SafetyHandlers();
  void* dpy = sideDisplay();
  if (!dpy) return nullptr;
  const Xlib& x = xlib();
  SideState& st = sideState();
  const XWin root = x.rootWindow(dpy, x.defaultScreen(dpy));
  const XWin s = x.createSimpleWindow(dpy, root, 0, 0, dim(w), dim(h), 0, 0, 0);
  if (!s) return nullptr;
  x.storeName(dpy, s, title.c_str());
  if (x.changeProperty) {  // the title in UTF-8 (_NET_WM_NAME), for names with accents
    const unsigned long netName = x.internAtom(dpy, "_NET_WM_NAME", 0), utf8 = x.internAtom(dpy, "UTF8_STRING", 0);
    x.changeProperty(dpy, s, netName, utf8, 8, 0 /* PropModeReplace */, reinterpret_cast<const unsigned char*>(title.data()), static_cast<int>(title.size()));
  }
  if (x.setClassHint) {  // the panel groups it with Gridline: the class its own windows have (app/AppIcon.h)
    const char* id = std::getenv("SDL_VIDEO_WAYLAND_WMCLASS");
    std::string name = "gridline-plugin", cls = id && *id ? id : "gridline-app";
    XClassHintRec ch{&name[0], &cls[0]};
    x.setClassHint(dpy, s, &ch);
  }
  unsigned long protocols[] = {st.wmDelete};
  x.setWMProtocols(dpy, s, protocols, 1);
  sideHints(dpy, s, w, h, resizable);
  x.selectInput(dpy, s, kStructureNotifyMask);
  x.mapWindow(dpy, s);
  x.flush(dpy);
  st.windows[s] = Side{false, w, h, false};
  watchKeys(ptr(s));  // Gridline's shortcuts typed in the interface (J-91)
  return ptr(s);
}

void sideWindowResize(void* side, int w, int h, bool resizable) {
  SideState& st = sideState();
  auto it = st.windows.find(win(side));
  if (!st.dpy || it == st.windows.end()) return;
  sideHints(st.dpy, it->first, w, h, resizable);
  xlib().resizeWindow(st.dpy, it->first, dim(w), dim(h));
  xlib().flush(st.dpy);
  it->second.w = w, it->second.h = h;
}

bool sideWindowPump(void* side, int& w, int& h) {
  sideDrain();
  SideState& st = sideState();
  auto it = st.windows.find(win(side));
  if (it == st.windows.end()) return false;
  if (it->second.sized) {
    w = it->second.w, h = it->second.h;
    it->second.sized = false;
  }
  return !it->second.closed;
}

void sideWindowClose(void* side) {
  SideState& st = sideState();
  auto it = st.windows.find(win(side));
  if (!st.dpy || it == st.windows.end()) return;
  unwatchKeys(side);
  xlib().destroyWindow(st.dpy, it->first);
  xlib().flush(st.dpy);
  st.windows.erase(it);
}

double backingScale(const Window&) { return 1.0; }
long windowNumber(const Window&) { return -1; }
bool captureWindow(const Window&, const std::string&, const unsigned char*, int, int) { return false; }
bool captureStack(const Window&, const unsigned char*, int, int, const std::string&) { return false; }
long windowLevel(const Window&) { return 0; }
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
void setTakesNoKeys(const Window&, bool) {}
void raiseChild(const Window&, const Window&) {}  // the window manager raises a clicked transient
void devLiveResize(const Window&, double, double, int) {}
void devPerformClose(const Window&) {}
std::string devCommandW(const Window&, int) { return "no AppKit"; }
// J-116: X11 has no modal resize loop: ConfigureNotify arrives in the event pump, which returns.
bool inModalSizeLoop() { return false; }
bool frameClockStart(const Window&) { return false; }
void frameClockFollow(const Window&) {}
void frameClockWait() {}
void frameClockStop() {}
void frameClockIdle(bool) {}
bool frameClockSleep(double) { return false; }

// 0.7.8 (X11) + 0.7.9 (Wayland): the idle sleep on SDL's own display connection
// (NativeView.h displayWaitStart / displayWaitStartWayland). A wake from another thread is a
// byte written to a pipe of its own, which is safe from any thread (and from a signal handler).
namespace {
enum class WaitKind : uint8_t { None = 0, X11, Wayland };
struct WlClient {
  int (*getFd)(void*) = nullptr;
  int (*prepareRead)(void*) = nullptr;
  int (*flush)(void*) = nullptr;
  int (*readEvents)(void*) = nullptr;
  void (*cancelRead)(void*) = nullptr;
  bool ok = false;
};
const WlClient& wlClient() {
  static const WlClient w = [] {
    WlClient c;
    void* h = ::dlopen("libwayland-client.so.0", RTLD_LAZY | RTLD_LOCAL);
    if (!h) h = ::dlopen("libwayland-client.so", RTLD_LAZY | RTLD_LOCAL);
    if (!h) return c;
    c.getFd = reinterpret_cast<decltype(c.getFd)>(::dlsym(h, "wl_display_get_fd"));
    c.prepareRead = reinterpret_cast<decltype(c.prepareRead)>(::dlsym(h, "wl_display_prepare_read"));
    c.flush = reinterpret_cast<decltype(c.flush)>(::dlsym(h, "wl_display_flush"));
    c.readEvents = reinterpret_cast<decltype(c.readEvents)>(::dlsym(h, "wl_display_read_events"));
    c.cancelRead = reinterpret_cast<decltype(c.cancelRead)>(::dlsym(h, "wl_display_cancel_read"));
    c.ok = c.getFd && c.prepareRead && c.flush && c.readEvents && c.cancelRead;
    return c;
  }();
  return w;
}
WaitKind gWaitKind = WaitKind::None;
void* gWaitDisplay = nullptr;
int gWaitFd = -1;                  // SDL's X11 or Wayland display fd
std::atomic<int> gWakeWrite{-1};   // the pipe frameClockWake writes to
int gWakeRead = -1;

bool initWakePipe() {
  if (gWakeRead >= 0) return true;
  int p[2];
  if (::pipe(p) != 0) return false;
  for (int e : p) {
    ::fcntl(e, F_SETFL, ::fcntl(e, F_GETFL) | O_NONBLOCK);
    ::fcntl(e, F_SETFD, FD_CLOEXEC);
  }
  gWakeRead = p[0];
  gWakeWrite.store(p[1]);
  return true;
}
}  // namespace

bool displayWaitStart(void* xDisplay) {
  if (gWaitKind != WaitKind::None) return gWaitKind == WaitKind::X11 && gWaitDisplay == xDisplay;
  const Xlib& x = xlib();
  if (!xDisplay || !x.eventsQueued || !x.connectionNumber || !x.flush) return false;
  const int fd = x.connectionNumber(xDisplay);
  if (fd < 0 || !initWakePipe()) return false;
  gWaitDisplay = xDisplay;
  gWaitFd = fd;
  gWaitKind = WaitKind::X11;
  return true;
}

bool displayWaitStartWayland(void* wlDisplay) {
  if (gWaitKind != WaitKind::None) return gWaitKind == WaitKind::Wayland && gWaitDisplay == wlDisplay;
  const WlClient& wl = wlClient();
  if (!wlDisplay || !wl.ok) return false;
  const int fd = wl.getFd(wlDisplay);
  if (fd < 0 || !initWakePipe()) return false;
  gWaitDisplay = wlDisplay;
  gWaitFd = fd;
  gWaitKind = WaitKind::Wayland;
  return true;
}

bool displaySleep(double seconds) {
  if (gWaitKind == WaitKind::None || !gWaitDisplay) return false;
  const int ms = seconds <= 0 ? 0 : static_cast<int>(std::min(seconds * 1000.0 + 0.999, 2.0e9));
  if (gWaitKind == WaitKind::Wayland) {
    const WlClient& wl = wlClient();
    // Canonical Wayland race-free poll: prepare_read returns -1 when events are already queued
    // on the default queue (wake immediately without sleeping); 0 acquires the read lock.
    if (wl.prepareRead(gWaitDisplay) != 0) return true;
    if (wl.flush(gWaitDisplay) < 0 && errno != EAGAIN) {
      wl.cancelRead(gWaitDisplay);
      return true;
    }
    pollfd p[2] = {};
    p[0].fd = gWaitFd;
    p[0].events = POLLIN;
    p[1].fd = gWakeRead;
    p[1].events = POLLIN;
    const int r = ::poll(p, 2, ms);
    if (p[0].revents & POLLIN) wl.readEvents(gWaitDisplay);
    else wl.cancelRead(gWaitDisplay);
    if (p[1].revents & POLLIN) {
      char buf[64];
      while (::read(gWakeRead, buf, sizeof buf) > 0) {
      }
    }
    return r != 0;
  }
  const Xlib& x = xlib();
  // Events Xlib has already read off the connection (another call on it read them) are not on the
  // socket any more: they are there now. The requests waiting to go out go first (QueuedAfterFlush).
  if (x.eventsQueued(gWaitDisplay, 2) > 0) return true;
  pollfd p[2] = {};
  p[0].fd = gWaitFd;
  p[0].events = POLLIN;
  p[1].fd = gWakeRead;
  p[1].events = POLLIN;
  const int r = ::poll(p, 2, ms);
  if (p[1].revents & POLLIN) {
    char buf[64];
    while (::read(gWakeRead, buf, sizeof buf) > 0) {
    }
  }
  return r != 0;  // (a signal that ended the wait counts as a wake: the loop looks again)
}

void frameClockWake() {
  const int fd = gWakeWrite.load();
  if (fd < 0) return;
  const char b = 1;
  [[maybe_unused]] const ssize_t n = ::write(fd, &b, 1);  // (a full pipe already wakes it)
}
bool windowOnScreen(const Window&) { return true; }
void watchOnScreen(const Window&) {}
int onScreenChanges() { return 0; }
void keepAwake(bool) {}

}  // namespace gl::host::native
#endif
