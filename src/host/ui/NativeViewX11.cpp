// Gridline — native pieces of a plugin window (X11). Xlib is resolved at run
// time from the library SDL's X11 driver already loaded, so Gridline links no
// X11 library itself; without it the plugin attaches to the window directly.
#if !defined(__APPLE__) && !defined(_WIN32)
#include <dlfcn.h>
#include <cstdint>
#include "NativeView.h"

namespace gl::host::native {

namespace {
using XWin = unsigned long;
struct Xlib {
  XWin (*createSimpleWindow)(void*, XWin, int, int, unsigned, unsigned, unsigned, unsigned long, unsigned long) = nullptr;
  int (*mapWindow)(void*, XWin) = nullptr;
  int (*moveResizeWindow)(void*, XWin, int, int, unsigned, unsigned) = nullptr;
  int (*destroyWindow)(void*, XWin) = nullptr;
  int (*setTransientForHint)(void*, XWin, XWin) = nullptr;
  int (*flush)(void*) = nullptr;
  bool ok = false;
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
    l.ok = l.createSimpleWindow && l.mapWindow && l.moveResizeWindow && l.destroyWindow && l.flush;
    return l;
  }();
  return x;
}
XWin win(const void* p) { return static_cast<XWin>(reinterpret_cast<uintptr_t>(p)); }
void* ptr(XWin w) { return reinterpret_cast<void*>(static_cast<uintptr_t>(w)); }
unsigned dim(int v) { return static_cast<unsigned>(v > 1 ? v : 1); }
}  // namespace

void* createContainer(const Window& w, const layout::Rect& r) {
  if (!w.handle) return nullptr;
  const Xlib& x = xlib();
  if (!x.ok || !w.display) return w.handle;  // attach to the window itself (top-left)
  XWin c = x.createSimpleWindow(w.display, win(w.handle), r.x, r.y, dim(r.w), dim(r.h), 0, 0, 0);
  if (!c) return w.handle;
  x.mapWindow(w.display, c);
  x.flush(w.display);
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
// Keys: X11 plugin windows get their keys from the X server directly; Gridline
// forwards what reaches its own window (PluginWindow.cpp, Editor::key).
void watchKeys(void*) {}
void unwatchKeys(void*) {}
bool containerKey(void*, void*, bool) { return false; }
bool popKey(void*, KeyEvent&) { return false; }
KeyFocus keyFocus(void*) { return KeyFocus::Host; }
void saveKeyFocus(void*) {}
void restoreKeyFocus(void*) {}
void* devTestKeyView(void*, int) { return nullptr; }
bool devSendKey(void*, uint16_t, bool, uint32_t, const char*) { return false; }
DrawStats takeDrawStats() { return {}; }
void devDrag(const Window&, void*, int, int, double) {}
bool frameClockStart(const Window&) { return false; }
void frameClockFollow(const Window&) {}
void frameClockWait() {}
void frameClockStop() {}

}  // namespace gl::host::native
#endif
