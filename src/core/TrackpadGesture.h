// Gridline — K-410: platform-agnostic trackpad gesture bus (macOS + Windows;
// Linux deferred to 0.8.0).
//
// Native platform layers (host/ui/TrackpadGestureMac.mm on macOS,
// host/ui/TrackpadGestureWin.cpp on Windows) and synthetic test/dev-input hooks
// post TrackpadGesture events into the thread-safe queue below. Once per frame
// GestureState drains the queue, scales window-point coordinates to logical UI
// coordinates (when not already logical), and lets the hovered canvas consume
// PinchZoom, SmartZoom, or Pan2D without double-handling discrete mouse wheels.
//
// Pure C++17: no SDL, ImGui, or OS headers (unit tested in
// tests/test_k410_trackpad_gestures.cpp).
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>
#include "Timeline.h"

namespace gl::gesture {

enum class Kind : uint8_t {
  PinchZoom,  // continuous magnification delta around cursor (x, y)
  SmartZoom,  // two-finger double-tap toggle centered on cursor (x, y)
  Pan2D,      // smooth two-finger 2D pan delta (dx, dy) in px at cursor (x, y)
};

enum class Phase : uint8_t { None, Began, Changed, Ended, Cancelled };

struct TrackpadGesture {
  Kind kind = Kind::PinchZoom;
  Phase phase = Phase::Changed;
  float x = 0.0f;              // cursor x (window points if !logical, ImGui logical px if logical)
  float y = 0.0f;              // cursor y (top-left origin)
  float magnification = 0.0f;  // PinchZoom: signed continuous delta (zoom factor = exp(magnification))
  float dx = 0.0f;             // Pan2D: horizontal scroll delta in px (+ = scroll right / later time)
  float dy = 0.0f;             // Pan2D: vertical scroll delta in px (+ = scroll down / lower rows)
  bool precise = true;         // true for trackpad/precision touchpad, false for discrete mouse wheel
  bool logical = false;        // true when x/y/dx/dy are already in ImGui logical px
  uint32_t windowId = 0;       // optional SDL window ID (0 = main / unspecified)
};

// Converts a continuous magnification delta into a strictly positive, symmetric
// zoom multiplier: pinchFactor(+m) * pinchFactor(-m) == 1.0 exactly, avoiding
// asymmetric drift over repeated zoom-in / zoom-out cycles.
double pinchFactor(double magnification);

// Master enable flag (synchronized from setting "input.trackpadGestures").
bool enabled();
void setEnabled(bool on);

// Thread-safe event bus.
void post(const TrackpadGesture& g);
bool hasPending();
std::vector<TrackpadGesture> take();
void clear();

// Per-frame gesture dispatcher owned by the UI layer (or instantiated directly
// in unit tests).
class GestureState {
 public:
  void beginFrame(bool gesturesEnabled, float uiScale, const std::vector<TrackpadGesture>& events);
  void endFrame();

  bool empty() const { return items_.empty(); }
  size_t count() const { return items_.size(); }

  // Consumes all matching unconsumed PinchZoom events for a canvas rect.
  // Sums continuous magnification deltas in log-space and reports the latest
  // cursor position in (outX, outY). Returns true if any pinch was consumed.
  bool consumePinch(bool hovered, float x0, float y0, float x1, float y1, float& magnificationOut, float& outX, float& outY,
                    bool ignoreBounds = false);

  // Consumes the first matching unconsumed SmartZoom event for a canvas rect.
  bool consumeSmartZoom(bool hovered, float x0, float y0, float x1, float y1, float& outX, float& outY, bool ignoreBounds = false);

  // Consumes all matching unconsumed precise Pan2D events for a canvas rect.
  // Sums (dx, dy) in logical pixels. Returns true if any precise pan arrived
  // (callers then skip discrete io.MouseWheel handling on that frame so there
  // is never any double-scrolling).
  bool consumePan2D(bool hovered, float x0, float y0, float x1, float y1, float& dxOut, float& dyOut, bool ignoreBounds = false);

 private:
  struct Item {
    TrackpadGesture g;
    bool consumed = false;
  };
  bool matches(const Item& it, Kind kind, bool hovered, float x0, float y0, float x1, float y1, bool ignoreBounds) const;
  std::vector<Item> items_;
};

// Stateful SmartZoom toggle for horizontal timeline views (Playlist, Piano Roll,
// Automation Editor): alternates cleanly between fitted / previous view and a
// zoomed-in detail view centered on the pointer's tick.
struct SmartZoomTimeline {
  bool zoomedIn = false;
  bool hasSaved = false;
  float savedPx = 0.0f;
  float savedScroll = 0.0f;
  float targetPx = 0.0f;

  void toggle(const Song& s, float& pxPerTick, float& scrollX, float anchorPx, float fitPxPerTick, float fitScrollX, float minScrollX = 0.0f,
              double zoomMul = 2.5);
  void reset() { *this = SmartZoomTimeline{}; }
};

// Stateful SmartZoom toggle for frame-span waveform views (Loupe).
struct SmartZoomRange {
  bool zoomedIn = false;
  bool hasSaved = false;
  double savedStart = 0.0;
  double savedLen = 0.0;
  double targetLen = 0.0;

  void toggle(double& viewStart, double& viewLen, double totalFrames, double anchorFrame, double zoomMul = 3.0);
  void reset() { *this = SmartZoomRange{}; }
};

// Pure trackers shared with the Windows WM_GESTURE / Precision Touchpad layer
// so their delta math is unit-tested on every platform.
struct WinZoomTracker {
  uint64_t lastDist = 0;
  bool active = false;

  // Returns continuous log magnification delta for a GID_ZOOM step.
  float step(uint64_t dist, bool begin, bool end);
  void reset() { *this = WinZoomTracker{}; }
};

struct WinPanTracker {
  int lastX = 0;
  int lastY = 0;
  bool active = false;

  // Outputs scroll delta (dx, dy) in pixels for a GID_PAN step.
  void step(int x, int y, bool begin, bool end, float& outDx, float& outDy);
  void reset() { *this = WinPanTracker{}; }
};

// Windows: distinguishes sub-notch Precision Touchpad wheel messages from
// discrete 120-unit mouse-wheel detents.
bool isPrecisionWheelDelta(int wheelDelta);

namespace native {
using WakeFn = void (*)();
// Resolves an OS window handle into an offset relative to the main window's
// content top-left (in OS window points) and its SDL window ID. Returns true
// only for Gridline's own UI windows (main + detached viewports), never for
// foreign hosted plugin windows.
using WindowOffsetFn = bool (*)(void* nativeWindow, float& offsetX, float& offsetY, uint32_t& sdlWindowId);

bool install(void* mainNativeWindow, WakeFn wake = nullptr, WindowOffsetFn windowOffset = nullptr);
void uninstall();
bool installed();
}  // namespace native

}  // namespace gl::gesture
