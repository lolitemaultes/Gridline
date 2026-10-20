// Gridline — LV2 X11UI child windows (see Lv2Native.h). Xlib is resolved at
// run time; Gridline links no X11 library. Written for Linux but not run on
// the development machine (macOS): compiled there in principle, untested.
#if !defined(__APPLE__)
#include <dlfcn.h>
#include <cstdint>
#include "Lv2Native.h"

namespace gl::host::lv2::native {

namespace {
using XWin = unsigned long;
struct Xlib {
  void* (*openDisplay)(const char*) = nullptr;
  int (*getGeometry)(void*, XWin, XWin*, int*, int*, unsigned*, unsigned*, unsigned*, unsigned*) = nullptr;
  int (*resizeWindow)(void*, XWin, unsigned, unsigned) = nullptr;
  int (*flush)(void*) = nullptr;
  void* display = nullptr;
  bool ok = false;
};
Xlib& xlib() {
  static Xlib x = [] {
    Xlib l;
    void* h = dlopen("libX11.so.6", RTLD_LAZY | RTLD_NOLOAD);
    if (!h) h = dlopen("libX11.so.6", RTLD_LAZY);
    if (!h) return l;
    l.openDisplay = reinterpret_cast<decltype(l.openDisplay)>(dlsym(h, "XOpenDisplay"));
    l.getGeometry = reinterpret_cast<decltype(l.getGeometry)>(dlsym(h, "XGetGeometry"));
    l.resizeWindow = reinterpret_cast<decltype(l.resizeWindow)>(dlsym(h, "XResizeWindow"));
    l.flush = reinterpret_cast<decltype(l.flush)>(dlsym(h, "XFlush"));
    if (l.openDisplay && l.getGeometry && l.resizeWindow && l.flush) l.display = l.openDisplay(nullptr);
    l.ok = l.display != nullptr;
    return l;
  }();
  return x;
}
XWin win(const void* p) { return static_cast<XWin>(reinterpret_cast<uintptr_t>(p)); }
}  // namespace

// X11UIs create their window as a child of ui:parent themselves.
bool embed(void* widget, void* parent) { return widget != nullptr && parent != nullptr; }

bool widgetSize(void* widget, int& w, int& h) {
  Xlib& x = xlib();
  if (!x.ok || !widget) return false;
  XWin root = 0;
  int px = 0, py = 0;
  unsigned ww = 0, hh = 0, border = 0, depth = 0;
  if (!x.getGeometry(x.display, win(widget), &root, &px, &py, &ww, &hh, &border, &depth) || ww < 1 || hh < 1) return false;
  w = static_cast<int>(ww);
  h = static_cast<int>(hh);
  return true;
}

void setWidgetSize(void* widget, int w, int h) {
  Xlib& x = xlib();
  if (!x.ok || !widget) return;
  x.resizeWindow(x.display, win(widget), static_cast<unsigned>(w > 1 ? w : 1), static_cast<unsigned>(h > 1 ? h : 1));
  x.flush(x.display);
}

void remove(void*) {}

}  // namespace gl::host::lv2::native
#endif
