// Gridline - K-177: the desktop's frame around Gridline's windows (title bar, borders) follows
// the theme, so no light system title bar sits above a dark Gridline (GitHub issue 42: "the white
// bar at the top").
//   Windows: dark title bar mode (Windows 10 and later) and, on Windows 11, the caption and its
//            text in the theme's chrome and text colours.
//   macOS:   the window's appearance, dark or light (title bar, traffic lights).
//   X11:     _GTK_THEME_VARIANT dark / light, which GNOME and other GTK-styled window managers
//            read for the decoration they draw.
// Wayland decorations come from the desktop (or libdecor) and are left alone.
//
// Header only, SDL only (as MainWindowRole.h): the program (main.cpp) includes it. The UI object
// library is also linked into the Gridline Suite plugins, which have no SDL.
#pragma once
#include <SDL.h>

#include <cstdint>
#include <cstring>
#include <initializer_list>

#include "SysWm.h"
#include "Theme.h"

#if defined(__APPLE__) || (!defined(_WIN32) && defined(SDL_VIDEO_DRIVER_X11))
#include <dlfcn.h>
#endif

namespace gl::winframe {

struct Look {
  bool dark = true;
  uint32_t caption = 0;  // 0xRRGGBB
  uint32_t text = 0;     // 0xRRGGBB
  bool operator==(const Look& o) const { return dark == o.dark && caption == o.caption && text == o.text; }
  bool operator!=(const Look& o) const { return !(*this == o); }
};

namespace detail {
inline Look gLast;
inline int gCount = 0;

inline uint32_t rgbOf(ImU32 c) {
  return ((c >> IM_COL32_R_SHIFT) & 0xFF) << 16 | ((c >> IM_COL32_G_SHIFT) & 0xFF) << 8 | ((c >> IM_COL32_B_SHIFT) & 0xFF);
}

#if defined(_WIN32)
using DwmSetFn = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
inline DwmSetFn dwmSet() {
  static DwmSetFn fn = [] {
    HMODULE m = LoadLibraryW(L"dwmapi.dll");
    return m ? reinterpret_cast<DwmSetFn>(reinterpret_cast<void*>(GetProcAddress(m, "DwmSetWindowAttribute"))) : nullptr;
  }();
  return fn;
}
#endif

#if defined(__APPLE__)
// AppKit through the Objective-C runtime (resolved at run time; SDL has already loaded AppKit).
using Id = void*;
using Sel = void*;
struct ObjC {
  Id (*getClass)(const char*) = nullptr;
  Sel (*selName)(const char*) = nullptr;
  void* msgSend = nullptr;
  bool ok() const { return getClass && selName && msgSend; }
};
inline const ObjC& objc() {
  static ObjC o = [] {
    ObjC r;
    r.getClass = reinterpret_cast<decltype(r.getClass)>(dlsym(RTLD_DEFAULT, "objc_getClass"));
    r.selName = reinterpret_cast<decltype(r.selName)>(dlsym(RTLD_DEFAULT, "sel_registerName"));
    r.msgSend = dlsym(RTLD_DEFAULT, "objc_msgSend");
    return r;
  }();
  return o;
}
#endif

#if !defined(_WIN32) && !defined(__APPLE__) && defined(SDL_VIDEO_DRIVER_X11)
// Xlib from the library SDL already loaded (as NativeViewX11.cpp does).
struct Xl {
  unsigned long (*internAtom)(void*, const char*, int) = nullptr;
  int (*changeProperty)(void*, unsigned long, unsigned long, unsigned long, int, int, const unsigned char*, int) = nullptr;
  int (*flush)(void*) = nullptr;
  bool ok() const { return internAtom && changeProperty && flush; }
};
inline const Xl& xl() {
  static Xl x = [] {
    Xl r;
    void* h = nullptr;
    for (const char* name : {"libX11.so.6", "libX11.so"})
      if (!h) h = dlopen(name, RTLD_LAZY | RTLD_NOLOAD);
    if (!h) return r;
    r.internAtom = reinterpret_cast<decltype(r.internAtom)>(dlsym(h, "XInternAtom"));
    r.changeProperty = reinterpret_cast<decltype(r.changeProperty)>(dlsym(h, "XChangeProperty"));
    r.flush = reinterpret_cast<decltype(r.flush)>(dlsym(h, "XFlush"));
    return r;
  }();
  return x;
}
#endif
}  // namespace detail

// The frame look of the current theme (theme::pal()): dark faces, chrome, text.
inline Look current() {
  const theme::Palette& P = theme::pal();
  Look l;
  l.dark = P.dark;
  l.caption = detail::rgbOf(P.chrome);
  l.text = detail::rgbOf(P.text);
  return l;
}

// Gives one window that look.
inline void apply(SDL_Window* w, const Look& l) {
  if (!w) return;
  detail::gLast = l;
  ++detail::gCount;
  SDL_SysWMinfo info;
  SDL_VERSION(&info.version);
  if (!SDL_GetWindowWMInfo(w, &info)) return;
#if defined(_WIN32)
  if (info.subsystem != SDL_SYSWM_WINDOWS) return;
  detail::DwmSetFn set = detail::dwmSet();
  if (!set) return;
  HWND h = info.info.win.window;
  const BOOL dark = l.dark ? TRUE : FALSE;
  // DWMWA_USE_IMMERSIVE_DARK_MODE: 20 (Windows 10 20H1 and later), 19 before.
  if (FAILED(set(h, 20, &dark, sizeof dark))) set(h, 19, &dark, sizeof dark);
  // Windows 11: DWMWA_CAPTION_COLOR (35) and DWMWA_TEXT_COLOR (36), COLORREF 0x00BBGGRR.
  auto ref = [](uint32_t rgb) { return static_cast<DWORD>(((rgb & 0xFF) << 16) | (rgb & 0xFF00) | ((rgb >> 16) & 0xFF)); };
  const DWORD cap = ref(l.caption), txt = ref(l.text);
  set(h, 35, &cap, sizeof cap);
  set(h, 36, &txt, sizeof txt);
#elif defined(__APPLE__)
  using detail::Id;
  using detail::Sel;
  if (info.subsystem != SDL_SYSWM_COCOA) return;
  const detail::ObjC& o = detail::objc();
  if (!o.ok()) return;
  Id nsWindow = static_cast<Id>(info.info.cocoa.window);
  auto send1s = reinterpret_cast<Id (*)(Id, Sel, const char*)>(o.msgSend);
  auto send1 = reinterpret_cast<Id (*)(Id, Sel, Id)>(o.msgSend);
  auto sendV = reinterpret_cast<void (*)(Id, Sel, Id)>(o.msgSend);
  Id nsString = o.getClass("NSString"), nsAppearance = o.getClass("NSAppearance");
  if (!nsString || !nsAppearance || !nsWindow) return;
  Id name = send1s(nsString, o.selName("stringWithUTF8String:"), l.dark ? "NSAppearanceNameDarkAqua" : "NSAppearanceNameAqua");
  Id appearance = name ? send1(nsAppearance, o.selName("appearanceNamed:"), name) : nullptr;
  if (appearance) sendV(nsWindow, o.selName("setAppearance:"), appearance);
#elif defined(SDL_VIDEO_DRIVER_X11)
  if (info.subsystem != SDL_SYSWM_X11) return;
  const detail::Xl& x = detail::xl();
  if (!x.ok()) return;
  void* dpy = static_cast<void*>(info.info.x11.display);
  const unsigned long win = static_cast<unsigned long>(info.info.x11.window);
  const unsigned long prop = x.internAtom(dpy, "_GTK_THEME_VARIANT", 0), utf8 = x.internAtom(dpy, "UTF8_STRING", 0);
  const char* v = l.dark ? "dark" : "light";
  x.changeProperty(dpy, win, prop, utf8, 8, 0 /* PropModeReplace */, reinterpret_cast<const unsigned char*>(v), static_cast<int>(std::strlen(v)));
  x.flush(dpy);
#else
  (void)info;
#endif
}

// Once per frame: every Gridline window gets the current look when the theme changed or a new
// window opened since the last call. Cheap when nothing changed.
inline void sync() {
  static Look last;
  static bool first = true;
  static uint32_t maxId = 0;
  const Look l = current();
  // New windows get ids above every earlier one (SDL counts up).
  uint32_t top = maxId;
  for (uint32_t id = maxId + 1; id < maxId + 64; ++id)
    if (SDL_GetWindowFromID(id)) top = id;
  if (!first && l == last && top == maxId) return;
  const uint32_t from = (!first && l == last) ? maxId + 1 : 1;
  for (uint32_t id = from; id <= top; ++id)
    if (SDL_Window* w = SDL_GetWindowFromID(id)) apply(w, l);
  first = false;
  last = l;
  maxId = top;
}

// Dev and tests: the look last given to any window, and how many times windows were updated.
inline Look lastApplied() { return detail::gLast; }
inline int applyCount() { return detail::gCount; }

}  // namespace gl::winframe
