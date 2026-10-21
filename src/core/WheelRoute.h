// Gridline — J-83: where the mouse wheel goes over a control in a scrolling pane.
//
// The normal mixer adjusts the control under the pointer with the wheel (the
// reference's behaviour). In the thin mixer (J-38: the strips stack into rows
// and the pane scrolls vertically) that made scrolling through the tracks
// change every gain fader, pan knob and send knob the pointer passed over.
// So when a pane *scrolls*, the plain wheel scrolls it and a control takes the
// wheel only when the user asks for it:
//   * the wheel-adjust modifier is held (keymap "mixer.wheelAdjust", Ctrl /
//     Cmd): fine steps on the control under the pointer;
//   * or the control has wheel focus: it was clicked (pressed and released
//     without dragging) and the pointer has not left it since. A drag does
//     not give focus, so letting go of a fader and scrolling on never moves it.
// A scroll gesture is a run of wheel events less than kGestureGap apart (a
// trackpad flick and its OS momentum tail are one gesture). A gesture that
// started by scrolling the pane stays a pane scroll to its end, whatever
// passes under the pointer, and a press during it does not start a drag: the
// tail of a flick never changes a parameter.
// Direct manipulation is never affected: drag, double-click reset, right-click
// menu and typed values work in every layout.
//
// Settings > Interface "Mouse wheel over controls in scrolling panes"
// (ui.mixerWheel, J-83; J-84 extends it from the mixer to the channel rack):
// adjust / scroll / auto (scroll while the pane scrolls: the thin mixer; a
// channel rack whose list scrolls vertically or whose steps scroll sideways).
// The layout is read every frame, so resizing a pane flips the behaviour at once.
//
// Pure: no UI dependencies, unit tested (tests/test_wheel_route.cpp).
#pragma once
#include <cstdint>

namespace gl::wheelroute {

enum class Mode : int { Adjust = 0, Scroll = 1, Auto = 2 };
inline constexpr Mode kDefaultMode = Mode::Auto;
// Setting value (number) -> mode; anything unknown is the default.
Mode modeFromSetting(double v);

// Does the plain wheel scroll this pane (true) or adjust its controls (false)?
// `narrow`: the pane is in its thin, vertically scrolling layout.
bool paneScrolls(Mode m, bool narrow);
// J-84 / Q-15: the channel rack scrolls (auto) when its channel list scrolls
// vertically or its step area scrolls sideways (long patterns, narrow rack).
bool rackScrolls(Mode m, bool rowsScroll, bool stepsScroll);

inline constexpr double kGestureGap = 0.30;  // s between wheel events of one gesture
inline constexpr float kFineScale = 0.25f;   // modifier + wheel: a quarter step

enum class Route { Adjust, Scroll };

struct Query {
  bool paneScrolls = false;  // policy of the pane the control is in
  bool modifier = false;     // the wheel-adjust modifier is held
  bool focused = false;      // the control has wheel focus (clicked)
};

// One per UI: the gesture and focus state. Call beginFrame() / endFrame()
// around every frame; controls ask route() and claim() the wheel they take.
class Router {
 public:
  void beginFrame(double now, float wheel);
  // Folds the frame into the gesture: wheel input no control claimed went to
  // the pane, so the gesture is a pane scroll from here on. Drops the wheel
  // focus when its control was not hovered this frame.
  void endFrame();

  Route route(const Query& q) const;
  // Wheel step scale for a control that routes Adjust: 1, or kFineScale
  // while the modifier is held in a scrolling pane.
  static float scale(const Query& q);
  // A control used this frame's wheel input.
  void claim() { claimed_ = true; }

  // A pane scroll gesture (or its momentum) is still running.
  bool scrolling() const;
  // A press on a control in a scrolling pane may start a drag (not while the
  // pane is being scrolled).
  bool mayStartDrag(bool paneScrolls) const { return !(paneScrolls && scrolling()); }

  // Wheel focus.
  void hovered(uint64_t id) {
    if (id == focus_) focusSeen_ = true;
  }
  void focus(uint64_t id) {
    focus_ = id;
    focusSeen_ = true;
  }
  bool hasFocus(uint64_t id) const { return id != 0 && id == focus_; }
  uint64_t focused() const { return focus_; }

 private:
  enum class Latch { None, Scroll, Adjust };
  double now_ = 0, last_ = -1e9;  // time of this frame / of the last wheel event
  float wheel_ = 0;
  bool claimed_ = false;
  Latch latch_ = Latch::None;
  uint64_t focus_ = 0;
  bool focusSeen_ = false;
};

}  // namespace gl::wheelroute
