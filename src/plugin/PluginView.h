// Gridline — the plugin's editor (PH-09): Gridline's whole interface in a
// child view of the host's plugin window, drawn with OpenGL 3 + Dear ImGui.
// macOS: an NSView with its own OpenGL context (PluginViewMac.mm), redrawn by
// a main-thread timer. Windows: a child window with a WGL context and its own
// timer (PluginViewWin.cpp). X11: a child window with a GLX context
// (PluginViewX11.cpp); the host's timer (CLAP timer support, VST3 run loop)
// calls tick(). Every view has its own Dear ImGui context; OpenGL objects are
// shared between all views of the process (one share group).
//
// Installed plugins hosted inside Gridline (VST3, CLAP, VST2, AU, LV2) show
// their own interface in a HostWindow: a top-level window owned by the
// host's window that shows Gridline (macOS: an AppKit child window ordered
// above it; X11: a transient window), laid out like the application's plugin
// windows (J-29: the plugin's view on top, Gridline's control bar under it).
#pragma once
#include <functional>
#include <memory>
#include <string>
#include "../host/EditorLayout.h"
#include "../host/KeyForward.h"

namespace gl::plugin {

class Instance;
class Frame;

// Something drawn with Dear ImGui into a view's OpenGL context.
class Surface {
 public:
  virtual ~Surface() = default;
  // Creates the ImGui context and the renderer (GL context current).
  virtual bool init(const char* glslVersion) = 0;
  virtual void shutdown() = 0;
  // Makes the surface's ImGui context current (input events go to it).
  virtual void makeCurrent() = 0;
  // Logical size, framebuffer size in pixels.
  virtual void draw(int winW, int winH, int fbW, int fbH) = 0;
  virtual float uiScale() const { return 1.0f; }  // logical pixel = uiScale view units
  // Mouse cursor Dear ImGui wants (ImGuiMouseCursor).
  virtual int cursor() const = 0;
};

// Window systems the views attach to.
enum class WindowApi { Cocoa, X11, Win32 };
bool viewSupported(WindowApi api);
WindowApi nativeWindowApi();

inline constexpr int kDefaultViewW = 1280, kDefaultViewH = 800;  // logical pixels
inline constexpr int kMinViewW = 640, kMinViewH = 400;

class View {
 public:
  virtual ~View() = default;
  // parent: NSView* (Cocoa) / X11 Window id. False when the view cannot be created.
  virtual bool attach(void* parent) = 0;
  virtual void detach() = 0;
  // Logical size (points on macOS, pixels / scale on X11).
  virtual void setSize(int w, int h) = 0;
  virtual void size(int& w, int& h) const = 0;
  // Content scale the host reports (X11; macOS uses the backing scale).
  virtual void setScale(double s) = 0;
  virtual void setVisible(bool on) = 0;
  // X11: pumps window events and draws a frame (host timer). macOS: no-op.
  virtual void tick() = 0;
  // Frames drawn so far (tests).
  virtual int frames() const = 0;
  // The workspace frame drawn by this view (its ImGui context exists after the first frame).
  virtual Frame* frame() = 0;
  // The interface asks the host for a new window size (logical pixels).
  std::function<bool(int w, int h)> requestResize;
};

std::unique_ptr<View> createView(Instance& inst);

// A window for a hosted plugin's own interface (see the top of this file).
// Sizes and positions in the window system's logical units; positions are
// global and top-left based.
class HostWindow {
 public:
  virtual ~HostWindow() = default;
  // Parent of the hosted plugin's interface (NSView* / X11 Window id);
  // nullptr for a bar-only window (sandboxed plugins).
  virtual void* container() const = 0;
  // Window content size, the plugin's area and the bar's area.
  virtual void setLayout(const host::layout::WindowLayout& L) = 0;
  virtual void show() = 0;
  virtual void setVisible(bool on) = 0;
  virtual void raise() = 0;
  virtual bool topLeft(int& x, int& y) const = 0;
  virtual void moveTopLeft(int x, int y) = 0;
  virtual double backingScale() const = 0;
  // X11: reads window events and draws the bar (from the host's timer, via
  // Instance::idle). macOS: the bar draws on its own timer.
  virtual void tick() = 0;
  // Checks (tests, dev): bar frames drawn, owned by the window showing
  // Gridline's editor, on screen, size of the plugin's own view.
  virtual int barFrames() const = 0;
  virtual bool ownedByEditor() const = 0;
  virtual bool visible() const = 0;
  virtual bool pluginViewSize(int& w, int& h) const = 0;
  // Image of the window's content (PNG); false when unsupported.
  virtual bool capture(const std::string& pngPath) = 0;
  // A key the plugin's interface did not use (J-66; the sandbox process'
  // window forwards them too): it goes to Gridline's editor, as if typed
  // there (Gridline's shortcuts). macOS; no-op elsewhere.
  virtual void forwardKey(const host::KeyEvent& k) { (void)k; }
  // The user closed the window / resized it to w x h (content size).
  std::function<void()> onClose;
  std::function<void(int w, int h)> onUserResize;
};
// `owner` is the view showing Gridline's editor; `place` an optional
// remembered top-left position; `index` cascades new windows.
std::unique_ptr<HostWindow> createHostWindow(View& owner, const std::string& title, const host::layout::WindowLayout& L, bool resizable,
                                             bool barOnly, Surface& bar, const int* place, int index);

// The calling thread's current OpenGL binding, saved before and restored
// after foreign code runs (plugin interfaces opening may bind their own).
struct GlBinding {
  void* a = nullptr;
  void* b = nullptr;
  unsigned long c = 0;
};
GlBinding currentGl();
void restoreGl(const GlBinding& g);

// Hosted plugins' windows of an instance (HostedWindows.cpp): shown and
// hidden with Gridline's editor, closed with it.
void setHostedWindowsVisible(Instance& inst, bool on);
void closeHostedWindows(Instance& inst);

// Draws one frame of Gridline's workspace for `inst` into the current GL
// context (PluginFrame.cpp). Shared by every platform view.
class Frame final : public Surface {
 public:
  explicit Frame(Instance& inst);
  ~Frame() override;
  // Creates the Dear ImGui context, fonts and the renderer (GL context current).
  bool init(const char* glslVersion) override;
  void shutdown() override;
  void makeCurrent() override;
  void draw(int winW, int winH, int fbW, int fbH) override;
  float uiScale() const override { return uiScale_; }
  int cursor() const override;
  // The frame's ImGui context (ImGuiContext*): plugin windows' bars share its fonts.
  void* imguiContext() const { return ctx_; }

 private:
  Instance& inst_;
  void* ctx_ = nullptr;  // ImGuiContext*
  bool rendererReady_ = false;
  bool layoutDone_ = false;
  bool placeRack_ = false;
  float rack_[4] = {0, 0, 0, 0};
  float lastDock_[2] = {0, 0};
  float uiScale_ = 1.0f;
  double last_ = 0;
  int frameNo_ = 0;
  std::string iniPath_;
};

}  // namespace gl::plugin
