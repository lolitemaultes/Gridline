// Gridline — the plugin's editor on X11 (PH-09): a child window of the
// host's window with a GLX context. One X connection serves every view of
// the process (GL objects shared through the first context); the host's
// timer (CLAP timer support, VST 3 IRunLoop) calls tick(), which reads the
// pending window events of all views and draws this one. Sizes are window
// pixels; the host's content scale makes the interface larger.
#if !defined(__APPLE__) || defined(GRIDLINE_SYNTAX_CHECK_X11)
#include <algorithm>
#include <cfloat>
#include <map>
#include <string>
#include "../../third_party/imgui/imgui.h"
#include "PluginCore.h"
#include "PluginView.h"
// Xlib last: its macros (None, Bool, …) would clash with Gridline's names.
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>

// GLX entry points (libGL); declared here so only the Xlib headers are needed.
extern "C" {
typedef struct __GLXcontextRec* GLXContext;
typedef XID GLXDrawable;
XVisualInfo* glXChooseVisual(Display* dpy, int screen, int* attribList);
GLXContext glXCreateContext(Display* dpy, XVisualInfo* vis, GLXContext shareList, Bool direct);
void glXDestroyContext(Display* dpy, GLXContext ctx);
Bool glXMakeCurrent(Display* dpy, GLXDrawable drawable, GLXContext ctx);
void glXSwapBuffers(Display* dpy, GLXDrawable drawable);
}

namespace gl::plugin {

class X11View;

namespace {

// GLX attribute tokens (GL/glx.h).
constexpr int kGlxRgba = 4, kGlxDoubleBuffer = 5, kGlxRedSize = 8, kGlxGreenSize = 9, kGlxBlueSize = 10, kGlxAlphaSize = 11,
              kGlxStencilSize = 13;

struct Shared {
  Display* dpy = nullptr;
  XVisualInfo* visual = nullptr;
  GLXContext root = nullptr;  // share group
  std::map<Window, X11View*> views;
  Cursor cursors[ImGuiMouseCursor_COUNT] = {};
  bool init() {
    if (dpy) return visual != nullptr;
    dpy = XOpenDisplay(nullptr);
    if (!dpy) return false;
    int attrs[] = {kGlxRgba, kGlxDoubleBuffer, kGlxRedSize, 8, kGlxGreenSize, 8, kGlxBlueSize, 8, kGlxAlphaSize, 8, kGlxStencilSize, 8, None};
    visual = glXChooseVisual(dpy, DefaultScreen(dpy), attrs);
    if (!visual) return false;
    root = glXCreateContext(dpy, visual, nullptr, True);
    const unsigned shapes[ImGuiMouseCursor_COUNT] = {XC_left_ptr, XC_xterm, XC_fleur, XC_sb_v_double_arrow, XC_sb_h_double_arrow,
                                                    XC_bottom_left_corner, XC_bottom_right_corner, XC_hand2, XC_left_ptr, XC_X_cursor};
    for (int k = 0; k < ImGuiMouseCursor_COUNT; ++k) cursors[k] = XCreateFontCursor(dpy, shapes[k]);
    return root != nullptr;
  }
};

Shared& shared() {
  static Shared s;
  return s;
}

ImGuiKey keyOf(KeySym k) {
  if (k >= XK_a && k <= XK_z) return static_cast<ImGuiKey>(ImGuiKey_A + static_cast<int>(k - XK_a));
  if (k >= XK_A && k <= XK_Z) return static_cast<ImGuiKey>(ImGuiKey_A + static_cast<int>(k - XK_A));
  if (k >= XK_0 && k <= XK_9) return static_cast<ImGuiKey>(ImGuiKey_0 + static_cast<int>(k - XK_0));
  if (k >= XK_F1 && k <= XK_F12) return static_cast<ImGuiKey>(ImGuiKey_F1 + static_cast<int>(k - XK_F1));
  if (k >= XK_KP_0 && k <= XK_KP_9) return static_cast<ImGuiKey>(ImGuiKey_Keypad0 + static_cast<int>(k - XK_KP_0));
  switch (k) {
    case XK_Return: return ImGuiKey_Enter;
    case XK_KP_Enter: return ImGuiKey_KeypadEnter;
    case XK_Tab: return ImGuiKey_Tab;
    case XK_space: return ImGuiKey_Space;
    case XK_BackSpace: return ImGuiKey_Backspace;
    case XK_Delete: return ImGuiKey_Delete;
    case XK_Escape: return ImGuiKey_Escape;
    case XK_Left: return ImGuiKey_LeftArrow;
    case XK_Right: return ImGuiKey_RightArrow;
    case XK_Up: return ImGuiKey_UpArrow;
    case XK_Down: return ImGuiKey_DownArrow;
    case XK_Home: return ImGuiKey_Home;
    case XK_End: return ImGuiKey_End;
    case XK_Page_Up: return ImGuiKey_PageUp;
    case XK_Page_Down: return ImGuiKey_PageDown;
    case XK_Insert: return ImGuiKey_Insert;
    case XK_minus: return ImGuiKey_Minus;
    case XK_equal: return ImGuiKey_Equal;
    case XK_bracketleft: return ImGuiKey_LeftBracket;
    case XK_bracketright: return ImGuiKey_RightBracket;
    case XK_backslash: return ImGuiKey_Backslash;
    case XK_semicolon: return ImGuiKey_Semicolon;
    case XK_apostrophe: return ImGuiKey_Apostrophe;
    case XK_comma: return ImGuiKey_Comma;
    case XK_period: return ImGuiKey_Period;
    case XK_slash: return ImGuiKey_Slash;
    case XK_grave: return ImGuiKey_GraveAccent;
    case XK_KP_Decimal: return ImGuiKey_KeypadDecimal;
    case XK_KP_Divide: return ImGuiKey_KeypadDivide;
    case XK_KP_Multiply: return ImGuiKey_KeypadMultiply;
    case XK_KP_Subtract: return ImGuiKey_KeypadSubtract;
    case XK_KP_Add: return ImGuiKey_KeypadAdd;
    case XK_Shift_L: return ImGuiKey_LeftShift;
    case XK_Shift_R: return ImGuiKey_RightShift;
    case XK_Control_L: return ImGuiKey_LeftCtrl;
    case XK_Control_R: return ImGuiKey_RightCtrl;
    case XK_Alt_L: return ImGuiKey_LeftAlt;
    case XK_Alt_R: return ImGuiKey_RightAlt;
    case XK_Super_L: return ImGuiKey_LeftSuper;
    case XK_Super_R: return ImGuiKey_RightSuper;
    default: return ImGuiKey_None;
  }
}

}  // namespace

bool viewSupported(WindowApi api) { return api == WindowApi::X11; }
WindowApi nativeWindowApi() { return WindowApi::X11; }

class X11View final : public View {
 public:
  explicit X11View(Instance& inst) : inst_(inst), frame_(inst) {}
  ~X11View() override { detach(); }

  bool attach(void* parent) override {
    Shared& s = shared();
    const Window p = static_cast<Window>(reinterpret_cast<uintptr_t>(parent));
    if (!p || win_ || !s.init()) return false;
    XSetWindowAttributes swa{};
    swa.colormap = XCreateColormap(s.dpy, p, s.visual->visual, AllocNone);
    swa.border_pixel = 0;
    swa.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask | KeyPressMask | KeyReleaseMask |
                     StructureNotifyMask | EnterWindowMask | LeaveWindowMask | FocusChangeMask;
    colormap_ = swa.colormap;
    win_ = XCreateWindow(s.dpy, p, 0, 0, static_cast<unsigned>(w_), static_cast<unsigned>(h_), 0, s.visual->depth, InputOutput,
                         s.visual->visual, CWColormap | CWBorderPixel | CWEventMask, &swa);
    if (!win_) return false;
    ctx_ = glXCreateContext(s.dpy, s.visual, s.root, True);
    if (!ctx_) {
      detach();
      return false;
    }
    s.views[win_] = this;
    XMapWindow(s.dpy, win_);
    XFlush(s.dpy);
    return true;
  }

  void detach() override {
    Shared& s = shared();
    if (ctx_) {
      glXMakeCurrent(s.dpy, win_, ctx_);
      frame_.shutdown();
      glXMakeCurrent(s.dpy, None, nullptr);
      glXDestroyContext(s.dpy, ctx_);
      ctx_ = nullptr;
    } else {
      frame_.shutdown();
    }
    if (win_) {
      s.views.erase(win_);
      XDestroyWindow(s.dpy, win_);
      win_ = 0;
    }
    if (colormap_) {
      XFreeColormap(s.dpy, colormap_);
      colormap_ = 0;
    }
    if (s.dpy) XFlush(s.dpy);
    glReady_ = false;
  }

  void setSize(int w, int h) override {
    w_ = std::max(w, kMinViewW);
    h_ = std::max(h, kMinViewH);
    if (win_) {
      XResizeWindow(shared().dpy, win_, static_cast<unsigned>(w_), static_cast<unsigned>(h_));
      XFlush(shared().dpy);
    }
  }
  void size(int& w, int& h) const override {
    w = w_;
    h = h_;
  }
  void setScale(double s) override { scale_ = std::clamp(s, 0.5, 4.0); }
  void setVisible(bool on) override {
    if (!win_) return;
    if (on)
      XMapWindow(shared().dpy, win_);
    else
      XUnmapWindow(shared().dpy, win_);
    XFlush(shared().dpy);
    visible_ = on;
  }
  int frames() const override { return frames_; }

  void tick() override {
    Shared& s = shared();
    if (!s.dpy) return;
    while (XPending(s.dpy)) {
      XEvent e;
      XNextEvent(s.dpy, &e);
      auto it = s.views.find(e.xany.window);
      if (it != s.views.end()) it->second->event(e);
    }
    render();
  }

 private:
  float toLogical(int v) const { return static_cast<float>(v) / static_cast<float>(scale_) / frame_.uiScale(); }

  void event(const XEvent& e) {
    if (!glReady_) return;
    frame_.makeCurrent();
    ImGuiIO& io = ImGui::GetIO();
    switch (e.type) {
      case ConfigureNotify:
        w_ = e.xconfigure.width;
        h_ = e.xconfigure.height;
        break;
      case MotionNotify: io.AddMousePosEvent(toLogical(e.xmotion.x), toLogical(e.xmotion.y)); break;
      case LeaveNotify: io.AddMousePosEvent(-FLT_MAX, -FLT_MAX); break;
      case ButtonPress:
      case ButtonRelease: {
        const bool down = e.type == ButtonPress;
        io.AddMousePosEvent(toLogical(e.xbutton.x), toLogical(e.xbutton.y));
        const unsigned b = e.xbutton.button;
        if (b == Button1) io.AddMouseButtonEvent(0, down);
        else if (b == Button3) io.AddMouseButtonEvent(1, down);
        else if (b == Button2) io.AddMouseButtonEvent(2, down);
        else if (down && b == Button4) io.AddMouseWheelEvent(0, 1);
        else if (down && b == Button5) io.AddMouseWheelEvent(0, -1);
        else if (down && b == 6) io.AddMouseWheelEvent(1, 0);
        else if (down && b == 7) io.AddMouseWheelEvent(-1, 0);
        if (down) XSetInputFocus(shared().dpy, win_, RevertToParent, CurrentTime);
        break;
      }
      case KeyPress:
      case KeyRelease: {
        const bool down = e.type == KeyPress;
        XKeyEvent ke = e.xkey;
        char text[32] = {};
        KeySym sym = 0;
        const int n = XLookupString(&ke, text, sizeof text - 1, &sym, nullptr);
        io.AddKeyEvent(ImGuiMod_Ctrl, (ke.state & ControlMask) != 0);
        io.AddKeyEvent(ImGuiMod_Shift, (ke.state & ShiftMask) != 0);
        io.AddKeyEvent(ImGuiMod_Alt, (ke.state & Mod1Mask) != 0);
        io.AddKeyEvent(ImGuiMod_Super, (ke.state & Mod4Mask) != 0);
        const ImGuiKey k = keyOf(sym);
        if (k != ImGuiKey_None) io.AddKeyEvent(k, down);
        if (down && n > 0 && !(ke.state & ControlMask)) {
          text[n] = 0;
          if (static_cast<unsigned char>(text[0]) >= 0x20 && text[0] != 0x7F) io.AddInputCharactersUTF8(text);
        }
        break;
      }
      default: break;
    }
  }

  void render() {
    Shared& s = shared();
    if (!win_ || !ctx_ || !visible_) return;
    glXMakeCurrent(s.dpy, win_, ctx_);
    if (!glReady_) {
      glReady_ = frame_.init("#version 130");
      if (!glReady_) return;
    }
    inst_.idle();
    frame_.draw(static_cast<int>(w_ / scale_), static_cast<int>(h_ / scale_), w_, h_);
    glXSwapBuffers(s.dpy, win_);
    const int c = frame_.cursor();
    if (c != cursor_ && c >= 0 && c < ImGuiMouseCursor_COUNT) {
      cursor_ = c;
      XDefineCursor(s.dpy, win_, s.cursors[c]);
    }
    XFlush(s.dpy);
    ++frames_;
  }

  Instance& inst_;
  Frame frame_;
  Window win_ = 0;
  Colormap colormap_ = 0;
  GLXContext ctx_ = nullptr;
  int w_ = kDefaultViewW, h_ = kDefaultViewH;
  double scale_ = 1.0;
  int frames_ = 0;
  int cursor_ = -1;
  bool glReady_ = false;
  bool visible_ = true;
};

std::unique_ptr<View> createView(Instance& inst) { return std::make_unique<X11View>(inst); }

}  // namespace gl::plugin
#endif
