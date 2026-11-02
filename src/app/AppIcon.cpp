#include "AppIcon.h"

#include <SDL.h>

#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

#include "AppIconImage.h"
#include "SysWm.h"

namespace gl::appicon {

void prepareIdentity() {
#if !defined(_WIN32) && !defined(__APPLE__)
  // SDL 2 reads these when its video driver starts (X11: WM_CLASS res_name and res_class;
  // Wayland: xdg_toplevel app_id). Without them both are the program's file name, gridline-app,
  // which no desktop entry is called.
  if (!std::getenv("SDL_VIDEO_X11_WMCLASS")) setenv("SDL_VIDEO_X11_WMCLASS", kDesktopId, 0);
  if (!std::getenv("SDL_VIDEO_WAYLAND_WMCLASS")) setenv("SDL_VIDEO_WAYLAND_WMCLASS", kDesktopId, 0);
#endif
}

#if defined(_WIN32)
namespace {
HICON resourceIcon(int px) {
  static std::map<int, HICON> cache;  // one per size, kept for the program's life (shared by windows)
  auto it = cache.find(px);
  if (it != cache.end()) return it->second;
  // Resource 1 is Gridline.ico (cmake/windows/gridline.rc.in): LoadImage picks its best size.
  HICON h = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON, px, px, LR_DEFAULTCOLOR));
  cache[px] = h;
  return h;
}
}  // namespace
#else
namespace {
// The embedded icon as an SDL surface, made once (SDL_SetWindowIcon copies it).
SDL_Surface* iconSurface(int size) {
  static SDL_Surface* s = nullptr;
  static bool tried = false;
  if (tried) return s;
  tried = true;
  Image im = decode(size);
  if (im.w <= 0) return nullptr;
  s = SDL_CreateRGBSurfaceWithFormat(0, im.w, im.h, 32, SDL_PIXELFORMAT_RGBA32);
  if (!s) return nullptr;
  for (int y = 0; y < im.h; ++y)
    std::memcpy(static_cast<Uint8*>(s->pixels) + y * s->pitch, im.rgba.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(im.w) * 4,
                static_cast<std::size_t>(im.w) * 4);
  return s;
}
}  // namespace
#endif

void apply(SDL_Window* w) {
  if (!w) return;
#if defined(_WIN32)
  SDL_SysWMinfo info;
  SDL_VERSION(&info.version);
  if (!SDL_GetWindowWMInfo(w, &info) || info.subsystem != SDL_SYSWM_WINDOWS) return;
  HWND hwnd = info.info.win.window;
  const UINT dpi = GetDpiForWindow(hwnd) ? GetDpiForWindow(hwnd) : 96;
  if (HICON bigIcon = resourceIcon(GetSystemMetricsForDpi(SM_CXICON, dpi)))
    SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(bigIcon));
  if (HICON smallIcon = resourceIcon(GetSystemMetricsForDpi(SM_CXSMICON, dpi)))
    SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(smallIcon));
#elif defined(__APPLE__)
  // Inside Gridline.app the Dock and Cmd+Tab show the bundle's Gridline.icns (every size up to
  // 1024 px); replacing it at run time would only lower its quality. A bare program (a build
  // folder, a terminal) has no bundle icon: give the Dock the embedded one.
  static int bundled = -1;
  if (bundled < 0) {
    bundled = 0;
    if (char* base = SDL_GetBasePath()) {
      bundled = std::string(base).find(".app/Contents/") != std::string::npos ? 1 : 0;
      SDL_free(base);
    }
  }
  if (bundled) return;
  if (SDL_Surface* s = iconSurface(512)) SDL_SetWindowIcon(w, s);
#else
  if (SDL_Surface* s = iconSurface(256)) SDL_SetWindowIcon(w, s);
#endif
}

}  // namespace gl::appicon
