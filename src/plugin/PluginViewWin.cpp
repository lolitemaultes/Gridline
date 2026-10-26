// Gridline — the plugin's editor on Windows (PH-09): a child window of the host's window
// with a WGL (OpenGL) context. Every view of the process shares GL objects with a hidden root
// context. The view redraws on its own window timer (Windows hosts have no editor timer API);
// mouse and keyboard messages go to the view's Dear ImGui context. Sizes are window pixels;
// the host's content scale makes the interface larger.
//
// Hosted plugins' windows (WinHostWindow): a top-level window owned by the host's window that
// shows Gridline (Windows keeps owned windows above their owner), with the plugin's interface in
// a child window on top and the bar's GL view under it; the bar draws from Instance::idle.
#if defined(_WIN32)
#include <windows.h>
#include <windowsx.h>
#include <GL/gl.h>
#include <algorithm>
#include <cfloat>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include "../../third_party/imgui/imgui.h"
#include "PluginCore.h"
#include "PluginView.h"

namespace gl::plugin {

class WinView;

namespace {

const wchar_t* const kViewClass = L"GridlinePluginGLView";
const wchar_t* const kTopClass = L"GridlinePluginHostWindow";
const wchar_t* const kHolderClass = L"GridlinePluginHolder";
constexpr UINT_PTR kTimerId = 1;

HINSTANCE moduleInstance() {
  HMODULE h = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     reinterpret_cast<LPCWSTR>(&moduleInstance), &h);
  return h;
}

std::wstring widen(const std::string& s) {
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
  return w;
}

bool setPixelFormat(HDC dc) {
  PIXELFORMATDESCRIPTOR pfd{};
  pfd.nSize = sizeof pfd;
  pfd.nVersion = 1;
  pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
  pfd.iPixelType = PFD_TYPE_RGBA;
  pfd.cColorBits = 32;
  pfd.cAlphaBits = 8;
  pfd.cStencilBits = 8;
  pfd.iLayerType = PFD_MAIN_PLANE;
  const int pf = ChoosePixelFormat(dc, &pfd);
  return pf != 0 && SetPixelFormat(dc, pf, &pfd);
}

LRESULT CALLBACK viewProc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK topProc(HWND, UINT, WPARAM, LPARAM);

// The window classes and the hidden root context every view shares GL objects with.
struct Shared {
  bool tried = false;
  HWND rootWnd = nullptr;
  HDC rootDc = nullptr;
  HGLRC root = nullptr;
  bool init() {
    if (tried) return root != nullptr;
    tried = true;
    HINSTANCE hi = moduleInstance();
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.style = CS_OWNDC | CS_DBLCLKS;
    wc.lpfnWndProc = viewProc;
    wc.hInstance = hi;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kViewClass;
    RegisterClassExW(&wc);
    WNDCLASSEXW tc{};
    tc.cbSize = sizeof tc;
    tc.lpfnWndProc = topProc;
    tc.hInstance = hi;
    tc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    tc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    tc.lpszClassName = kTopClass;
    RegisterClassExW(&tc);
    WNDCLASSEXW hc{};
    hc.cbSize = sizeof hc;
    hc.lpfnWndProc = DefWindowProcW;
    hc.hInstance = hi;
    hc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    hc.lpszClassName = kHolderClass;
    RegisterClassExW(&hc);
    rootWnd = CreateWindowExW(0, kHolderClass, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, hi, nullptr);
    if (!rootWnd) return false;
    rootDc = GetDC(rootWnd);
    if (!setPixelFormat(rootDc)) return false;
    root = wglCreateContext(rootDc);
    return root != nullptr;
  }
};

Shared& shared() {
  static Shared s;
  return s;
}

ImGuiKey keyOf(WPARAM vk, LPARAM lp) {
  if (vk >= 'A' && vk <= 'Z') return static_cast<ImGuiKey>(ImGuiKey_A + static_cast<int>(vk - 'A'));
  if (vk >= '0' && vk <= '9') return static_cast<ImGuiKey>(ImGuiKey_0 + static_cast<int>(vk - '0'));
  if (vk >= VK_F1 && vk <= VK_F12) return static_cast<ImGuiKey>(ImGuiKey_F1 + static_cast<int>(vk - VK_F1));
  if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return static_cast<ImGuiKey>(ImGuiKey_Keypad0 + static_cast<int>(vk - VK_NUMPAD0));
  const bool extended = (HIWORD(lp) & KF_EXTENDED) != 0;
  switch (vk) {
    case VK_RETURN: return extended ? ImGuiKey_KeypadEnter : ImGuiKey_Enter;
    case VK_TAB: return ImGuiKey_Tab;
    case VK_SPACE: return ImGuiKey_Space;
    case VK_BACK: return ImGuiKey_Backspace;
    case VK_DELETE: return ImGuiKey_Delete;
    case VK_ESCAPE: return ImGuiKey_Escape;
    case VK_LEFT: return ImGuiKey_LeftArrow;
    case VK_RIGHT: return ImGuiKey_RightArrow;
    case VK_UP: return ImGuiKey_UpArrow;
    case VK_DOWN: return ImGuiKey_DownArrow;
    case VK_HOME: return ImGuiKey_Home;
    case VK_END: return ImGuiKey_End;
    case VK_PRIOR: return ImGuiKey_PageUp;
    case VK_NEXT: return ImGuiKey_PageDown;
    case VK_INSERT: return ImGuiKey_Insert;
    case VK_OEM_MINUS: return ImGuiKey_Minus;
    case VK_OEM_PLUS: return ImGuiKey_Equal;
    case VK_OEM_4: return ImGuiKey_LeftBracket;
    case VK_OEM_6: return ImGuiKey_RightBracket;
    case VK_OEM_5: return ImGuiKey_Backslash;
    case VK_OEM_1: return ImGuiKey_Semicolon;
    case VK_OEM_7: return ImGuiKey_Apostrophe;
    case VK_OEM_COMMA: return ImGuiKey_Comma;
    case VK_OEM_PERIOD: return ImGuiKey_Period;
    case VK_OEM_2: return ImGuiKey_Slash;
    case VK_OEM_3: return ImGuiKey_GraveAccent;
    case VK_DECIMAL: return ImGuiKey_KeypadDecimal;
    case VK_DIVIDE: return ImGuiKey_KeypadDivide;
    case VK_MULTIPLY: return ImGuiKey_KeypadMultiply;
    case VK_SUBTRACT: return ImGuiKey_KeypadSubtract;
    case VK_ADD: return ImGuiKey_KeypadAdd;
    case VK_SHIFT: return ImGuiKey_LeftShift;
    case VK_CONTROL: return extended ? ImGuiKey_RightCtrl : ImGuiKey_LeftCtrl;
    case VK_MENU: return extended ? ImGuiKey_RightAlt : ImGuiKey_LeftAlt;
    case VK_LWIN: return ImGuiKey_LeftSuper;
    case VK_RWIN: return ImGuiKey_RightSuper;
    default: return ImGuiKey_None;
  }
}

LPCWSTR cursorOf(int c) {
  switch (c) {
    case ImGuiMouseCursor_TextInput: return IDC_IBEAM;
    case ImGuiMouseCursor_ResizeAll: return IDC_SIZEALL;
    case ImGuiMouseCursor_ResizeNS: return IDC_SIZENS;
    case ImGuiMouseCursor_ResizeEW: return IDC_SIZEWE;
    case ImGuiMouseCursor_ResizeNESW: return IDC_SIZENESW;
    case ImGuiMouseCursor_ResizeNWSE: return IDC_SIZENWSE;
    case ImGuiMouseCursor_Hand: return IDC_HAND;
    case ImGuiMouseCursor_NotAllowed: return IDC_NO;
    default: return IDC_ARROW;
  }
}

}  // namespace

bool viewSupported(WindowApi api) { return api == WindowApi::Win32; }
WindowApi nativeWindowApi() { return WindowApi::Win32; }

// A WGL child window drawing a Surface: Gridline's workspace (the editor, with its Instance)
// or the control bar of a hosted plugin's window.
class WinView final : public View {
 public:
  explicit WinView(Instance& inst) : inst_(&inst), own_(std::make_unique<Frame>(inst)), surface_(own_.get()), timer_(true) {
    inst.minViewSize(minW_, minH_);  // J-131: a suite editor is sized by its plugin
    inst.preferredViewSize(w_, h_);
  }
  // A bar: no minimum size, no instance housekeeping, drawn by its window's tick().
  WinView(Surface& s, int w, int h) : surface_(&s), minW_(1), minH_(1), w_(w), h_(h) {}
  ~WinView() override { detach(); }

  bool attach(void* parent) override {
    Shared& s = shared();
    HWND p = static_cast<HWND>(parent);
    if (!p || wnd_ || !s.init()) return false;
    wnd_ = CreateWindowExW(0, kViewClass, L"Gridline", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0, 0, w_, h_, p, nullptr,
                           moduleInstance(), nullptr);
    if (!wnd_) return false;
    SetWindowLongPtrW(wnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    dc_ = GetDC(wnd_);
    if (!setPixelFormat(dc_) || !(ctx_ = wglCreateContext(dc_))) {
      detach();
      return false;
    }
    wglShareLists(s.root, ctx_);  // one share group: fonts and textures serve every view
    if (timer_) SetTimer(wnd_, kTimerId, 16, nullptr);
    if (inst_) inst_->setEditorView(this);
    return true;
  }

  void detach() override {
    if (inst_ && inst_->editorView() == this) {
      closeHostedWindows(*inst_);  // their bars share this frame's fonts
      inst_->setEditorView(nullptr);
    }
    if (ctx_) {
      const GlBinding prev = currentGl();
      HGLRC mine = ctx_;
      wglMakeCurrent(dc_, ctx_);
      surface_->shutdown();
      wglMakeCurrent(nullptr, nullptr);
      wglDeleteContext(ctx_);
      ctx_ = nullptr;
      if (prev.b != mine) restoreGl(prev);
    } else {
      surface_->shutdown();
    }
    if (wnd_) {
      KillTimer(wnd_, kTimerId);
      SetWindowLongPtrW(wnd_, GWLP_USERDATA, 0);
      if (dc_) ReleaseDC(wnd_, dc_);
      DestroyWindow(wnd_);
      wnd_ = nullptr;
      dc_ = nullptr;
    }
    glReady_ = false;
  }

  void setSize(int w, int h) override {
    w_ = std::max(w, minW_);
    h_ = std::max(h, minH_);
    if (wnd_) SetWindowPos(wnd_, nullptr, 0, 0, w_, h_, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
  }
  void size(int& w, int& h) const override {
    w = w_;
    h = h_;
  }
  void setScale(double s) override { scale_ = std::clamp(s, 0.5, 4.0); }
  void setVisible(bool on) override {
    if (!wnd_) return;
    ShowWindow(wnd_, on ? SW_SHOWNA : SW_HIDE);
    visible_ = on;
    if (inst_ && inst_->editorView() == this) setHostedWindowsVisible(*inst_, on);
  }
  int frames() const override { return frames_; }
  Frame* frame() override { return glReady_ ? own_.get() : nullptr; }
  HWND hwnd() const { return wnd_; }
  void tick() override { render(); }

  void render() {
    if (!wnd_ || !ctx_ || !visible_ || rendering_) return;
    rendering_ = true;
    // Housekeeping first: hosted plugins' interfaces may bind their own GL context.
    if (inst_) inst_->idle();
    if (wnd_ && ctx_) {
      wglMakeCurrent(dc_, ctx_);
      if (!glReady_) glReady_ = surface_->init("#version 130");
      if (glReady_) {
        surface_->draw(static_cast<int>(w_ / scale_), static_cast<int>(h_ / scale_), w_, h_);
        SwapBuffers(dc_);
        cursor_ = surface_->cursor();
        ++frames_;
      }
    }
    rendering_ = false;
  }

  LRESULT message(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
      case WM_TIMER:
        if (wp == kTimerId) render();
        return 0;
      case WM_SIZE:
        w_ = std::max(1, static_cast<int>(LOWORD(lp)));
        h_ = std::max(1, static_cast<int>(HIWORD(lp)));
        return 0;
      case WM_ERASEBKGND: return 1;
      case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(wnd_, &ps);
        EndPaint(wnd_, &ps);
        return 0;
      }
      case WM_GETDLGCODE: return DLGC_WANTALLKEYS | DLGC_WANTCHARS;  // keys stay with Gridline inside dialogs
      case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
          SetCursor(LoadCursorW(nullptr, cursorOf(cursor_)));
          return TRUE;
        }
        break;
      default: break;
    }
    if (!glReady_) return DefWindowProcW(wnd_, msg, wp, lp);
    surface_->makeCurrent();
    ImGuiIO& io = ImGui::GetIO();
    switch (msg) {
      case WM_MOUSEMOVE: {
        if (!tracking_) {
          TRACKMOUSEEVENT t{sizeof(TRACKMOUSEEVENT), TME_LEAVE, wnd_, 0};
          tracking_ = TrackMouseEvent(&t) != 0;
        }
        io.AddMousePosEvent(toLogical(GET_X_LPARAM(lp)), toLogical(GET_Y_LPARAM(lp)));
        return 0;
      }
      case WM_MOUSELEAVE:
        tracking_ = false;
        io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
        return 0;
      case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK: case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK:
      case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP: {
        const bool down = msg != WM_LBUTTONUP && msg != WM_RBUTTONUP && msg != WM_MBUTTONUP;
        const int b = (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK || msg == WM_LBUTTONUP) ? 0
                      : (msg == WM_RBUTTONDOWN || msg == WM_RBUTTONDBLCLK || msg == WM_RBUTTONUP) ? 1
                                                                                                  : 2;
        io.AddMousePosEvent(toLogical(GET_X_LPARAM(lp)), toLogical(GET_Y_LPARAM(lp)));
        io.AddMouseButtonEvent(b, down);
        if (down) {
          SetFocus(wnd_);
          if (++captured_ == 1) SetCapture(wnd_);
        } else if (captured_ > 0 && --captured_ == 0) {
          ReleaseCapture();
        }
        return 0;
      }
      case WM_CAPTURECHANGED: captured_ = 0; return 0;
      case WM_MOUSEWHEEL: io.AddMouseWheelEvent(0, static_cast<float>(GET_WHEEL_DELTA_WPARAM(wp)) / WHEEL_DELTA); return 0;
      case WM_MOUSEHWHEEL: io.AddMouseWheelEvent(-static_cast<float>(GET_WHEEL_DELTA_WPARAM(wp)) / WHEEL_DELTA, 0); return 0;
      case WM_KEYDOWN: case WM_KEYUP: case WM_SYSKEYDOWN: case WM_SYSKEYUP: {
        const bool down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
        io.AddKeyEvent(ImGuiMod_Ctrl, (GetKeyState(VK_CONTROL) & 0x8000) != 0);
        io.AddKeyEvent(ImGuiMod_Shift, (GetKeyState(VK_SHIFT) & 0x8000) != 0);
        io.AddKeyEvent(ImGuiMod_Alt, (GetKeyState(VK_MENU) & 0x8000) != 0);
        io.AddKeyEvent(ImGuiMod_Super, ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) != 0);
        const ImGuiKey k = keyOf(wp, lp);
        if (k != ImGuiKey_None) io.AddKeyEvent(k, down);
        if (msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP) break;  // Alt+F4 and the menu keys still reach the host
        return 0;
      }
      case WM_CHAR:
        if (wp >= 0x20 && wp != 0x7F) io.AddInputCharacterUTF16(static_cast<ImWchar16>(wp));
        return 0;
      case WM_SETFOCUS: io.AddFocusEvent(true); return 0;
      case WM_KILLFOCUS: io.AddFocusEvent(false); return 0;
      default: break;
    }
    return DefWindowProcW(wnd_, msg, wp, lp);
  }

 private:
  float toLogical(int v) const { return static_cast<float>(v) / static_cast<float>(scale_) / surface_->uiScale(); }

  Instance* inst_ = nullptr;
  std::unique_ptr<Frame> own_;
  Surface* surface_ = nullptr;
  bool timer_ = false;
  int minW_ = kMinViewW, minH_ = kMinViewH;
  HWND wnd_ = nullptr;
  HDC dc_ = nullptr;
  HGLRC ctx_ = nullptr;
  int w_ = kDefaultViewW, h_ = kDefaultViewH;
  double scale_ = 1.0;
  int frames_ = 0;
  int cursor_ = 0;
  int captured_ = 0;
  bool tracking_ = false;
  bool glReady_ = false;
  bool visible_ = true;
  bool rendering_ = false;
};

namespace {
LRESULT CALLBACK viewProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  auto* v = reinterpret_cast<WinView*>(GetWindowLongPtrW(h, GWLP_USERDATA));
  return v ? v->message(msg, wp, lp) : DefWindowProcW(h, msg, wp, lp);
}
}  // namespace

std::unique_ptr<View> createView(Instance& inst) { return std::make_unique<WinView>(inst); }

// ---------------------------------------------------------------- hosted plugins' windows

class WinHostWindow final : public HostWindow {
 public:
  WinHostWindow(Surface& bar, int w, int h) : bar_(bar, w, h) {}
  ~WinHostWindow() override {
    bar_.detach();
    if (top_) {
      SetWindowLongPtrW(top_, GWLP_USERDATA, 0);
      DestroyWindow(top_);  // the container and the bar holder go with it
    }
  }

  bool create(HWND ownerView, const std::string& title, const host::layout::WindowLayout& L, bool resizable, bool barOnly, const int* place,
              int index) {
    if (!shared().init()) return false;
    owner_ = GetAncestor(ownerView, GA_ROOT);  // the host's top-level window
    resizable_ = resizable;
    style_ = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN | (resizable ? WS_THICKFRAME | WS_MAXIMIZEBOX : 0);
    exStyle_ = barOnly ? WS_EX_TOOLWINDOW : 0;
    RECT r{0, 0, L.w, L.h};
    AdjustWindowRectEx(&r, style_, FALSE, exStyle_);
    int x = 0, y = 0;
    if (place) {
      x = place[0];
      y = place[1];
    } else {
      RECT o{};
      GetWindowRect(owner_, &o);
      x = o.left + 40 + 26 * (index % 8);
      y = o.top + 60 + 26 * (index % 8);
    }
    top_ = CreateWindowExW(exStyle_, kTopClass, widen(title).c_str(), style_, x, y, r.right - r.left, r.bottom - r.top, owner_, nullptr,
                           moduleInstance(), nullptr);
    if (!top_) return false;
    SetWindowLongPtrW(top_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    if (!barOnly)
      container_ = CreateWindowExW(0, kHolderClass, L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, L.plugin.x, L.plugin.y, std::max(1, L.plugin.w),
                                   std::max(1, L.plugin.h), top_, nullptr, moduleInstance(), nullptr);
    holder_ = CreateWindowExW(0, kHolderClass, L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, 0, L.bar.y, L.w, std::max(1, L.h - L.bar.y), top_,
                              nullptr, moduleInstance(), nullptr);
    if (!holder_ || !bar_.attach(holder_)) return false;
    setLayout(L);
    return true;
  }

  void* container() const override { return container_; }

  void setLayout(const host::layout::WindowLayout& L) override {
    if (!top_) return;
    w_ = L.w;
    h_ = L.h;
    layout_ = L;
    RECT r{0, 0, L.w, L.h};
    AdjustWindowRectEx(&r, style_, FALSE, exStyle_);
    applying_ = true;
    SetWindowPos(top_, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    if (container_)
      SetWindowPos(container_, nullptr, L.plugin.x, L.plugin.y, std::max(1, L.plugin.w), std::max(1, L.plugin.h), SWP_NOZORDER | SWP_NOACTIVATE);
    const int bh = std::max(1, L.h - L.bar.y);
    SetWindowPos(holder_, nullptr, 0, L.bar.y, L.w, bh, SWP_NOZORDER | SWP_NOACTIVATE);
    bar_.setSize(L.w, bh);
    applying_ = false;
  }

  void show() override {
    shown_ = true;
    setVisible(true);
  }
  void setVisible(bool on) override {
    if (!top_ || !shown_) return;
    ShowWindow(top_, on ? SW_SHOWNA : SW_HIDE);
  }
  void raise() override {
    if (top_) SetWindowPos(top_, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
  }
  bool topLeft(int& x, int& y) const override {
    RECT r{};
    if (!top_ || !GetWindowRect(top_, &r)) return false;
    x = r.left;
    y = r.top;
    return true;
  }
  void moveTopLeft(int x, int y) override {
    if (top_) SetWindowPos(top_, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
  }
  double backingScale() const override { return top_ ? static_cast<double>(GetDpiForWindow(top_)) / 96.0 : 1.0; }
  void tick() override { bar_.render(); }
  int barFrames() const override { return bar_.frames(); }
  bool ownedByEditor() const override { return top_ && GetWindow(top_, GW_OWNER) == owner_; }
  bool visible() const override { return top_ && IsWindowVisible(top_); }
  bool pluginViewSize(int& w, int& h) const override {
    HWND child = container_ ? GetWindow(container_, GW_CHILD) : nullptr;
    RECT r{};
    if (!child || !GetClientRect(child, &r)) return false;
    w = r.right - r.left;
    h = r.bottom - r.top;
    return true;
  }
  bool capture(const std::string&) override { return false; }

  LRESULT message(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
      case WM_CLOSE:
        if (onClose) onClose();
        return 0;
      case WM_SIZE:
        if (!applying_ && wp != SIZE_MINIMIZED) {
          const int w = LOWORD(lp), h = HIWORD(lp);
          if ((w != w_ || h != h_) && onUserResize) onUserResize(w, h);
        }
        return 0;
      default: return DefWindowProcW(top_, msg, wp, lp);
    }
  }

 private:
  WinView bar_;
  HWND owner_ = nullptr;  // the host's top-level window
  HWND top_ = nullptr, container_ = nullptr, holder_ = nullptr;
  DWORD style_ = 0, exStyle_ = 0;
  host::layout::WindowLayout layout_{};
  int w_ = 0, h_ = 0;
  bool resizable_ = false;
  bool shown_ = false;
  bool applying_ = false;
};

namespace {
LRESULT CALLBACK topProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  auto* w = reinterpret_cast<WinHostWindow*>(GetWindowLongPtrW(h, GWLP_USERDATA));
  return w ? w->message(msg, wp, lp) : DefWindowProcW(h, msg, wp, lp);
}
}  // namespace

std::unique_ptr<HostWindow> createHostWindow(View& owner, const std::string& title, const host::layout::WindowLayout& L, bool resizable,
                                             bool barOnly, Surface& bar, const int* place, int index) {
  HWND ov = static_cast<WinView&>(owner).hwnd();
  if (!ov) return nullptr;
  auto w = std::make_unique<WinHostWindow>(bar, L.w, std::max(1, L.h - L.bar.y));
  if (!w->create(ov, title, L, resizable, barOnly, place, index)) return nullptr;
  return w;
}

GlBinding currentGl() {
  GlBinding g;
  g.a = wglGetCurrentDC();
  g.b = wglGetCurrentContext();
  return g;
}
void restoreGl(const GlBinding& g) {
  if (g.a && g.b) wglMakeCurrent(static_cast<HDC>(g.a), static_cast<HGLRC>(g.b));
}

}  // namespace gl::plugin
#endif
