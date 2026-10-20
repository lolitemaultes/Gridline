// Gridline — the plugin's editor (PH-09): Gridline's whole interface in a
// child view of the host's plugin window, drawn with OpenGL 3 + Dear ImGui.
// macOS: an NSView with its own OpenGL context (PluginViewMac.mm), redrawn by
// a main-thread timer. X11: a child window with a GLX context
// (PluginViewX11.cpp); the host's timer (CLAP timer support, VST3 run loop)
// calls tick(). Every view has its own Dear ImGui context; OpenGL objects are
// shared between all views of the process (one share group).
#pragma once
#include <functional>
#include <memory>
#include <string>

namespace gl::plugin {

class Instance;

// Window systems the views attach to.
enum class WindowApi { Cocoa, X11 };
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
  // The interface asks the host for a new window size (logical pixels).
  std::function<bool(int w, int h)> requestResize;
};

std::unique_ptr<View> createView(Instance& inst);

// Draws one frame of Gridline's workspace for `inst` into the current GL
// context (PluginFrame.cpp). Shared by every platform view.
class Frame {
 public:
  explicit Frame(Instance& inst);
  ~Frame();
  // Creates the Dear ImGui context, fonts and the renderer (GL context current).
  bool init(const char* glslVersion);
  void shutdown();
  // Makes the frame's ImGui context current (input events go to it).
  void makeCurrent();
  // Logical window size, framebuffer size in pixels.
  void draw(int winW, int winH, int fbW, int fbH);
  float uiScale() const { return uiScale_; }  // logical pixel = uiScale view units
  // Mouse cursor Dear ImGui wants (ImGuiMouseCursor).
  int cursor() const;

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
