// Gridline — LV2 WindowsUI child windows (see Lv2Native.h): the interface's HWND inside the
// host's container window.
#if defined(_WIN32)
#include <windows.h>
#include "Lv2Native.h"

namespace gl::host::lv2::native {

namespace {
HWND hwnd(void* p) { return static_cast<HWND>(p); }
}  // namespace

// WindowsUIs normally create their window as a child of ui:parent themselves.
bool embed(void* widget, void* parent) {
  if (!widget || !parent) return false;
  if (GetParent(hwnd(widget)) != hwnd(parent)) {
    LONG_PTR style = GetWindowLongPtrW(hwnd(widget), GWL_STYLE);
    SetWindowLongPtrW(hwnd(widget), GWL_STYLE, (style & ~static_cast<LONG_PTR>(WS_POPUP | WS_CAPTION | WS_THICKFRAME)) | WS_CHILD);
    if (!SetParent(hwnd(widget), hwnd(parent))) return false;
    SetWindowPos(hwnd(widget), nullptr, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
  }
  ShowWindow(hwnd(widget), SW_SHOWNA);
  return true;
}

bool widgetSize(void* widget, int& w, int& h) {
  RECT r{};
  if (!widget || !GetWindowRect(hwnd(widget), &r) || r.right - r.left < 1 || r.bottom - r.top < 1) return false;
  w = r.right - r.left;
  h = r.bottom - r.top;
  return true;
}

void setWidgetSize(void* widget, int w, int h) {
  if (widget) SetWindowPos(hwnd(widget), nullptr, 0, 0, w > 1 ? w : 1, h > 1 ? h : 1, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void remove(void* widget) {
  if (widget && IsWindow(hwnd(widget))) ShowWindow(hwnd(widget), SW_HIDE);
}

}  // namespace gl::host::lv2::native
#endif
