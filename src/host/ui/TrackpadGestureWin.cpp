// Gridline — K-410: Windows Precision Touchpad & WM_GESTURE capture (and Linux
// stub; Linux trackpad gestures are deferred to 0.8.0).
//
// On Windows:
//  - Subclasses the main SDL HWND via SetWindowLongPtrW(GWLP_WNDPROC) (no extra
//    link libraries needed; restores cleanly on WM_NCDESTROY / uninstall()).
//  - Enables and handles WM_GESTURE (GID_ZOOM -> PinchZoom, GID_PAN -> Pan2D)
//    using dynamically resolved user32.dll entry points so MSVC, clang-cl, and
//    MinGW all build cleanly with zero warnings.
//  - Distinguishes Windows 10/11 Precision Touchpad sub-notch WM_MOUSEWHEEL /
//    WM_MOUSEHWHEEL deltas (abs(delta) % WHEEL_DELTA != 0) from discrete mouse
//    wheel clicks:
//      * Ctrl + sub-notch WM_MOUSEWHEEL -> PinchZoom (consumed so SDL does not
//        double-handle it).
//      * Unmodified sub-notch WM_MOUSEWHEEL / WM_MOUSEHWHEEL -> Pan2D (passed
//        through for ImGui scroll panels; gesture canvases consume Pan2D and
//        skip io.MouseWheel on that frame to prevent double-scrolling).
#include "../../core/TrackpadGesture.h"

#if defined(_WIN32)
#include <windows.h>
#include <cmath>
#include "QuietWindows.h"

namespace gl::gesture::native {

namespace {

HWND gHwnd = nullptr;
WNDPROC gOrigProc = nullptr;
WakeFn gWake = nullptr;
bool gInstalled = false;
WinZoomTracker gZoom;
WinPanTracker gPan;

#if defined(WM_GESTURE) && defined(GID_ZOOM) && defined(GID_PAN)
using GetGestureInfoFn = BOOL(WINAPI*)(HGESTUREINFO, PGESTUREINFO);
using CloseGestureInfoHandleFn = BOOL(WINAPI*)(HGESTUREINFO);
using SetGestureConfigFn = BOOL(WINAPI*)(HWND, DWORD, UINT, PGESTURECONFIG, UINT);

GetGestureInfoFn gGetGestureInfo = nullptr;
CloseGestureInfoHandleFn gCloseGestureInfoHandle = nullptr;
SetGestureConfigFn gSetGestureConfig = nullptr;

void loadGestureApi() {
  static bool loaded = false;
  if (loaded) return;
  loaded = true;
  if (HMODULE u32 = GetModuleHandleW(L"user32.dll")) {
    gGetGestureInfo = reinterpret_cast<GetGestureInfoFn>(reinterpret_cast<void*>(GetProcAddress(u32, "GetGestureInfo")));
    gCloseGestureInfoHandle = reinterpret_cast<CloseGestureInfoHandleFn>(reinterpret_cast<void*>(GetProcAddress(u32, "CloseGestureInfoHandle")));
    gSetGestureConfig = reinterpret_cast<SetGestureConfigFn>(reinterpret_cast<void*>(GetProcAddress(u32, "SetGestureConfig")));
  }
}

void enableGesturesForWindow(HWND hwnd) {
  loadGestureApi();
  if (!gSetGestureConfig || !hwnd) return;
  GESTURECONFIG cfgs[2] = {};
  cfgs[0].dwID = GID_ZOOM;
  cfgs[0].dwWant = GC_ZOOM;
  cfgs[0].dwBlock = 0;
  cfgs[1].dwID = GID_PAN;
  cfgs[1].dwWant = GC_PAN;
  cfgs[1].dwBlock = 0;
  gSetGestureConfig(hwnd, 0, 2, cfgs, static_cast<UINT>(sizeof(GESTURECONFIG)));
}
#endif

POINT screenToClientPt(HWND hwnd, int screenX, int screenY) {
  POINT pt{screenX, screenY};
  ScreenToClient(hwnd, &pt);
  return pt;
}

LRESULT CALLBACK gestureWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  WNDPROC orig = gOrigProc;

  if (msg == WM_NCDESTROY) {
    if (hwnd == gHwnd && orig) {
      SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(orig));
    }
    gHwnd = nullptr;
    gOrigProc = nullptr;
    gInstalled = false;
    gZoom.reset();
    gPan.reset();
    return orig ? CallWindowProcW(orig, hwnd, msg, wParam, lParam) : DefWindowProcW(hwnd, msg, wParam, lParam);
  }

  if (gl::gesture::enabled() && !gl::quiet::on()) {
#if defined(WM_GESTURE) && defined(GID_ZOOM) && defined(GID_PAN)
    if (msg == WM_GESTURE && gGetGestureInfo && gCloseGestureInfoHandle) {
      GESTUREINFO gi = {};
      gi.cbSize = sizeof(GESTUREINFO);
      auto hgi = reinterpret_cast<HGESTUREINFO>(lParam);
      if (gGetGestureInfo(hgi, &gi)) {
        const bool isBegin = (gi.dwFlags & GF_BEGIN) != 0;
        const bool isEnd = (gi.dwFlags & GF_END) != 0;
        const Phase ph = isBegin ? Phase::Began : isEnd ? Phase::Ended : Phase::Changed;
        const POINT pt = screenToClientPt(hwnd, gi.ptsLocation.x, gi.ptsLocation.y);

        if (gi.dwID == GID_ZOOM) {
          const float mag = gZoom.step(static_cast<uint64_t>(gi.ullArguments), isBegin, isEnd);
          if (mag != 0.0f || isBegin || isEnd) {
            TrackpadGesture g;
            g.kind = Kind::PinchZoom;
            g.phase = ph;
            g.x = static_cast<float>(pt.x);
            g.y = static_cast<float>(pt.y);
            g.magnification = mag;
            g.precise = true;
            g.logical = false;
            post(g);
            if (gWake) gWake();
          }
          gCloseGestureInfoHandle(hgi);
          return 0;
        }

        if (gi.dwID == GID_PAN) {
          float dx = 0.0f, dy = 0.0f;
          gPan.step(gi.ptsLocation.x, gi.ptsLocation.y, isBegin, isEnd, dx, dy);
          if (dx != 0.0f || dy != 0.0f) {
            TrackpadGesture g;
            g.kind = Kind::Pan2D;
            g.phase = ph;
            g.x = static_cast<float>(pt.x);
            g.y = static_cast<float>(pt.y);
            g.dx = dx;
            g.dy = dy;
            g.precise = true;
            g.logical = false;
            post(g);
            if (gWake) gWake();
          }
          gCloseGestureInfoHandle(hgi);
          return 0;
        }
      }
    }
#endif

    if (msg == WM_MOUSEWHEEL || msg == WM_MOUSEHWHEEL) {
      const int delta = static_cast<int>(GET_WHEEL_DELTA_WPARAM(wParam));
      if (isPrecisionWheelDelta(delta)) {
        const WORD keys = GET_KEYSTATE_WPARAM(wParam);
        const bool ctrl = (keys & MK_CONTROL) != 0;
        const bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
        const int sx = static_cast<int>(static_cast<short>(LOWORD(lParam)));
        const int sy = static_cast<int>(static_cast<short>(HIWORD(lParam)));
        const POINT pt = screenToClientPt(hwnd, sx, sy);

        if (msg == WM_MOUSEWHEEL && ctrl && !alt) {
          // Windows 10/11 Precision Touchpad pinch sends fine Ctrl+WM_MOUSEWHEEL.
          TrackpadGesture g;
          g.kind = Kind::PinchZoom;
          g.phase = Phase::Changed;
          g.x = static_cast<float>(pt.x);
          g.y = static_cast<float>(pt.y);
          g.magnification = (static_cast<float>(delta) / 120.0f) * 0.25f;
          g.precise = true;
          g.logical = false;
          post(g);
          if (gWake) gWake();
          return 0;
        }

        if (!ctrl && !alt) {
          // Precision Touchpad smooth 2-finger scroll (fractional WHEEL_DELTA).
          const float px = (static_cast<float>(delta) / 120.0f) * 48.0f;
          TrackpadGesture g;
          g.kind = Kind::Pan2D;
          g.phase = Phase::Changed;
          g.x = static_cast<float>(pt.x);
          g.y = static_cast<float>(pt.y);
          g.dx = (msg == WM_MOUSEHWHEEL) ? px : 0.0f;
          g.dy = (msg == WM_MOUSEWHEEL) ? -px : 0.0f;
          g.precise = true;
          g.logical = false;
          post(g);
          if (gWake) gWake();
          // Pass through for ImGui scroll panels; gesture canvases consume Pan2D
          // and skip io.MouseWheel on that frame.
        }
      }
    }
  }

  return orig ? CallWindowProcW(orig, hwnd, msg, wParam, lParam) : DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace

bool install(void* mainNativeWindow, WakeFn wake, WindowOffsetFn windowOffset) {
  (void)windowOffset;
  if (gInstalled) return true;
  if (gl::quiet::on()) return false;
  HWND hwnd = static_cast<HWND>(mainNativeWindow);
  if (!hwnd || !IsWindow(hwnd)) return false;

  gWake = wake;
  gZoom.reset();
  gPan.reset();
  SetLastError(0);
  const LONG_PTR prev = SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(gestureWndProc));
  if (prev == 0 && GetLastError() != 0) {
    gWake = nullptr;
    return false;
  }
  gHwnd = hwnd;
  gOrigProc = reinterpret_cast<WNDPROC>(prev);
  gInstalled = true;
#if defined(WM_GESTURE) && defined(GID_ZOOM) && defined(GID_PAN)
  enableGesturesForWindow(hwnd);
#endif
  return true;
}

void uninstall() {
  if (!gInstalled) return;
  if (gHwnd && IsWindow(gHwnd) && gOrigProc) {
    SetWindowLongPtrW(gHwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(gOrigProc));
  }
  gHwnd = nullptr;
  gOrigProc = nullptr;
  gWake = nullptr;
  gInstalled = false;
  gZoom.reset();
  gPan.reset();
}

bool installed() { return gInstalled; }

}  // namespace gl::gesture::native

#elif !defined(__APPLE__)

namespace gl::gesture::native {

bool install(void* mainNativeWindow, WakeFn wake, WindowOffsetFn windowOffset) {
  (void)mainNativeWindow;
  (void)wake;
  (void)windowOffset;
  return false;  // Linux trackpad gestures deferred to 0.8.0
}

void uninstall() {}

bool installed() { return false; }

}  // namespace gl::gesture::native

#endif
