// Gridline — native pieces of a plugin window on Windows (see NativeView.h): the container a
// hosted plugin's interface attaches to is a child window of the SDL window (HWND); "keep
// above" makes the plugin window an owned window of the main window (Windows keeps owned
// windows above their owner and minimises them with it).
//
// Keys (J-66): Windows sends keys to the focused window, the plugin's own. A keyboard hook on
// the UI thread sees them first: while a plugin view (not a text field) inside a container
// has the focus, the keys Gridline's global shortcuts use (Space, F1-F12, Ctrl / Alt chords)
// are queued for Gridline as well; text fields keep every key.
#if defined(_WIN32)
#include <windows.h>
#include <algorithm>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include "NativeView.h"
#include "../../core/UiScale.h"

namespace gl::host::native {

namespace {

const wchar_t* const kContainerClass = L"GridlinePluginContainer";

HWND hwndOf(const void* p) { return static_cast<HWND>(const_cast<void*>(p)); }

HINSTANCE moduleInstance() {
  HMODULE h = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     reinterpret_cast<LPCWSTR>(&moduleInstance), &h);
  return h;
}

bool registerClass() {
  static const bool ok = [] {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = moduleInstance();
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = kContainerClass;
    return RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
  }();
  return ok;
}

// ---- keys
struct Keys {
  std::mutex mu;
  std::map<HWND, std::deque<KeyEvent>> queues;  // watched containers
  std::map<HWND, HWND> savedFocus;
  HHOOK hook = nullptr;
};
Keys& keys() {
  static Keys k;
  return k;
}

bool isTextField(HWND w) {
  wchar_t cls[64] = {};
  GetClassNameW(w, cls, 63);
  return wcsstr(cls, L"Edit") != nullptr || wcsstr(cls, L"EDIT") != nullptr || wcsstr(cls, L"RichEdit") != nullptr;
}

// The watched container holding `w`, nullptr when none.
HWND containerOf(HWND w) {
  Keys& k = keys();
  for (HWND p = w; p; p = GetParent(p))
    if (k.queues.count(p)) return p;
  return nullptr;
}

uint32_t modsNow() {
  uint32_t m = 0;
  if (GetKeyState(VK_SHIFT) & 0x8000) m |= kKeyShift;
  if (GetKeyState(VK_CONTROL) & 0x8000) m |= kKeyCtrl;
  if (GetKeyState(VK_MENU) & 0x8000) m |= kKeyAlt;
  if ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) m |= kKeyCmd;
  return m;
}

LRESULT CALLBACK keyHook(int code, WPARAM vk, LPARAM lp) {
  if (code == HC_ACTION) {
    Keys& k = keys();
    std::lock_guard<std::mutex> g(k.mu);
    HWND focus = GetFocus();
    HWND c = focus ? containerOf(focus) : nullptr;
    if (c && focus != c && !isTextField(focus)) {
      const uint32_t mods = modsNow();
      const int vkey = static_cast<int>(vk);
      const int usage = hidUsageOfWinVk(vkey);
      if (forwardsToHost(usage, mods)) {  // J-91: the same rule as X11 (host/KeyForward.h)
        KeyEvent e;
        e.down = (lp & (1u << 31)) == 0;
        e.repeat = e.down && (lp & (1 << 30)) != 0;
        e.code = macKeyOfHidUsage(usage);
        e.mods = mods;
        if (e.code != 0xFFFF) k.queues[c].push_back(e);
      }
    }
  }
  return CallNextHookEx(nullptr, code, vk, lp);
}

void ensureHook() {
  Keys& k = keys();
  if (!k.hook) k.hook = SetWindowsHookExW(WH_KEYBOARD, keyHook, nullptr, GetCurrentThreadId());
}

}  // namespace

Window windowOfView(void* view) {
  Window w;
  if (HWND h = hwndOf(view)) w.handle = GetAncestor(h, GA_ROOT);
  return w;
}

void* createContainer(const Window& w, const layout::Rect& r) {
  if (!w.handle || !registerClass()) return nullptr;
  HWND c = CreateWindowExW(0, kContainerClass, L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, r.x, r.y, std::max(1, r.w),
                           std::max(1, r.h), hwndOf(w.handle), nullptr, moduleInstance(), nullptr);
  if (c) watchKeys(c);
  return c;
}

void setContainerFrame(const Window&, void* container, const layout::Rect& r) {
  if (container) SetWindowPos(hwndOf(container), nullptr, r.x, r.y, std::max(1, r.w), std::max(1, r.h), SWP_NOZORDER | SWP_NOACTIVATE);
}

void destroyContainer(const Window&, void* container) {
  if (!container) return;
  unwatchKeys(container);
  DestroyWindow(hwndOf(container));
}

void keepAbove(const Window& child, const Window& parent, bool on) {
  if (!child.handle) return;
  SetWindowLongPtrW(hwndOf(child.handle), GWLP_HWNDPARENT, on && parent.handle ? reinterpret_cast<LONG_PTR>(parent.handle) : 0);
}

double backingScale(const Window&) { return 1.0; }  // SDL window units are pixels (per-monitor DPI aware)
long windowNumber(const Window&) { return -1; }
std::string xResources(const Window&) { return {}; }  // X11 only

PointerSize pointerSize(const Window&) {
  PointerSize r;
  const int h = GetSystemMetrics(SM_CYCURSOR);  // the cursor cell at the system DPI (32 at 100 %)
  r.arrowH = uiscale::pointerArrowHeight(uiscale::PointerSystem::Windows, h > 0 ? h : 0, 1.0);
  r.source = h > 0 ? "SM_CYCURSOR " + std::to_string(h) : std::string("Windows default");
  return r;
}
bool captureWindow(const Window&, const std::string&, const unsigned char*, int, int) { return false; }

void watchKeys(void* container) {
  if (!container) return;
  Keys& k = keys();
  std::lock_guard<std::mutex> g(k.mu);
  ensureHook();
  k.queues[hwndOf(container)];
}
void unwatchKeys(void* container) {
  Keys& k = keys();
  std::lock_guard<std::mutex> g(k.mu);
  k.queues.erase(hwndOf(container));
  k.savedFocus.erase(hwndOf(container));
}
bool containerKey(void*, void*, bool) { return false; }
bool popKey(void* container, KeyEvent& e) {
  Keys& k = keys();
  std::lock_guard<std::mutex> g(k.mu);
  auto it = k.queues.find(hwndOf(container));
  if (it == k.queues.end() || it->second.empty()) return false;
  e = it->second.front();
  it->second.pop_front();
  return true;
}
KeyFocus keyFocus(void* container) {
  HWND f = GetFocus();
  if (!container || !f) return KeyFocus::Host;
  for (HWND p = f; p; p = GetParent(p))
    if (p == hwndOf(container)) return f == p ? KeyFocus::Host : isTextField(f) ? KeyFocus::PluginText : KeyFocus::Plugin;
  return KeyFocus::Host;
}
void saveKeyFocus(void* container) {
  HWND f = GetFocus();
  if (!container || !f || keyFocus(container) == KeyFocus::Host) return;
  Keys& k = keys();
  std::lock_guard<std::mutex> g(k.mu);
  k.savedFocus[hwndOf(container)] = f;
}
void restoreKeyFocus(void* container) {
  Keys& k = keys();
  HWND f = nullptr;
  {
    std::lock_guard<std::mutex> g(k.mu);
    auto it = k.savedFocus.find(hwndOf(container));
    if (it != k.savedFocus.end()) f = it->second;
  }
  if (f && IsWindow(f)) SetFocus(f);
}
void* devTestKeyView(void*, int) { return nullptr; }
bool devSendKey(void*, uint16_t, bool, uint32_t, const char*) { return false; }

bool frameClockStart(const Window&) { return false; }  // vsync swap (SDL)
void frameClockFollow(const Window&) {}
void frameClockWait() {}
void frameClockStop() {}
void keepAwake(bool) {}

void startDrawStats(void*) {}
DrawStats takeDrawStats() { return {}; }
void devDrag(const Window&, void*, int, int, double) {}
void devPostMouse(const Window&, int, double, double) {}
void orderAbove(const Window&, const Window&) {}
void devLiveResize(const Window&, double, double, int) {}
void devPerformClose(const Window&) {}

// J-116: DefWindowProc's modal size / move loop (WM_ENTERSIZEMOVE .. WM_EXITSIZEMOVE) blocks the
// event pump; SDL keeps sending RESIZED (WM_WINDOWPOSCHANGED) and EXPOSED (its 10 ms timer) then.
bool inModalSizeLoop() {
  GUITHREADINFO info{};
  info.cbSize = sizeof info;
  return GetGUIThreadInfo(GetCurrentThreadId(), &info) && (info.flags & GUI_INMOVESIZE) != 0;
}

}  // namespace gl::host::native
#endif
