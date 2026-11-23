// Gridline - K-277: idle frame pacing. The interface draws every display refresh while something
// happens (input, playback, sound, a load, an edit, a drag) and, once nothing has for a moment,
// draws a few frames a second until something does again. The main loop sleeps between those
// frames and draws at once when input or sound comes (K-142: planWait below; nothing at all while
// no window of Gridline's is on screen).
#pragma once
#include <algorithm>

namespace gl::idlepace {

class IdlePace {
 public:
  static constexpr double kCalmAfter = 1.0;      // seconds without activity before frames slow down
  static constexpr double kIdleInterval = 0.1;   // seconds between frames while idle (10 a second)
  // K-142 (0.7.8): once nothing has happened for kDeepAfter seconds (meters have fallen, every
  // settle is over), 4 frames a second, unless setDeep(false) (an open plugin window: its
  // parameters may move from its own interface, which Gridline sees no input of).
  static constexpr double kDeepAfter = 5.0;
  static constexpr double kDeepInterval = 0.25;

  void setEnabled(bool on) { enabled_ = on; }
  bool enabled() const { return enabled_; }
  void setDeep(bool allowed) { deep_ = allowed; }
  // Something happened at `now` (seconds): full rate for kCalmAfter from here.
  void active(double now) { lastActive_ = now; }
  // A frame was drawn at `now`.
  void framed(double now) { lastFrame_ = now; }
  bool idle(double now) const { return enabled_ && now - lastActive_ >= kCalmAfter; }
  bool deep(double now) const { return deep_ && now - lastActive_ >= kDeepAfter; }
  double interval(double now) const { return deep(now) ? kDeepInterval : kIdleInterval; }
  // Seconds the loop may still wait before it draws the next frame (0: draw now).
  double waitLeft(double now) const { return idle(now) ? std::max(0.0, lastFrame_ + interval(now) - now) : 0.0; }

 private:
  bool enabled_ = true, deep_ = true;
  double lastActive_ = -1e300, lastFrame_ = -1e300;
};

// K-142 (0.7.8): the main loop's wait before its next turn. Shown (a window of Gridline's on
// screen): the idle frames above. Nothing shown (minimised, hidden, covered): no frames at all
// while idle, only the work frames do on their own: a changed project's timed backups and autosave
// (App::tick looks every kHiddenHousekeeping seconds) and, in a CPU report, its next step
// (`reportLeft`, which needs a turn of the loop but no frame: `reportOnly`). What breaks a wait
// early (input, sound, MIDI, playback, an edit) is the loop's own business; `nap` is how long it
// sleeps between looking at what changes without an event: every refresh while shown, four times
// a second while not (input still wakes it at once). K-142 (0.7.8 close): in the long idle (frames
// a quarter second apart) twenty times a second: each look woke the interface thread and the
// window server's link to it, a third of an idle window's processor time; sound that starts with
// no event (a plugin of its own, an input monitored) brings the full rate back a few hundredths of
// a second later, input and MIDI at once as before (UiWake.h).
struct Wait {
  double left = 0;          // seconds still to wait (0: take the turn now)
  bool reportOnly = false;  // the turn is the report's alone: draw nothing
  double nap = 0;           // the longest single sleep within the wait
};
constexpr double kHiddenHousekeeping = 5.0;
constexpr double kHiddenPoll = 0.25;
constexpr double kShownPoll = 1.0 / 60.0;
constexpr double kDeepPoll = 1.0 / 20.0;
inline Wait planWait(const IdlePace& p, double now, bool shown, bool modified, double lastDrawn, double reportLeft) {
  Wait w;
  if (!p.enabled()) return w;
  if (shown) {
    w.left = p.waitLeft(now);
  } else {
    const double housekeeping = modified ? std::max(0.0, lastDrawn + kHiddenHousekeeping - now) : 1e300;
    w.left = std::min(housekeeping, std::max(0.0, reportLeft));
    w.reportOnly = w.left <= 0 && housekeeping > 0;
  }
  w.nap = std::min(w.left, !shown ? kHiddenPoll : p.deep(now) ? kDeepPoll : kShownPoll);
  return w;
}

// GitHub #66: is an open plugin window in use, so that its plugin keeps running (its interface
// may play notes Gridline never sees) and, on X11, the interface keeps the full frame rate? Yes
// while the pointer is over it or it has the keyboard; always when Gridline cannot tell
// (`foreign`: the interface is in a window of another process or one Gridline does not manage);
// never while it is set aside. A click needs the pointer over the window first, so the plugin
// is running before the click lands.
inline bool editorInUse(bool foreign, bool shown, bool pointerOver, bool keyboard) {
  if (foreign) return true;
  return shown && (pointerOver || keyboard);
}

}  // namespace gl::idlepace
