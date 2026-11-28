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
// sleeps between looking at what changes without an event: every refresh while shown (input still
// wakes it at once). K-142 (0.7.8 close): in the long idle (frames
// a quarter second apart) twenty times a second: each look woke the interface thread and the
// window server's link to it, a third of an idle window's processor time; sound that starts with
// no event (a plugin of its own, an input monitored) brings the full rate back a few hundredths of
// a second later, input and MIDI at once as before (UiWake.h).
// Nothing shown, nothing that changes without an event needs the interface thread (what does wakes
// it: input, MIDI, OSC, a collaborator, a later launch; sound only moves meters nobody sees), so the
// loop sleeps until its next duty, waking only every kHiddenBeat seconds for the freeze watchdog
// (Crash.h: a UI thread silent for 30 s counts as frozen). Four looks a second remain only where an
// input has no wake of its own (`looks`: a game controller attached, whose buttons SDL reads only
// when the loop looks). `dutyLeft`: other timed work that needs a full turn while nothing is shown
// (MIDI's next look for devices plugged in or out where the system does not say, or a retry of its
// service: Midi.h secondsToNextLook).
struct Wait {
  double left = 0;          // seconds still to wait (0: take the turn now)
  bool reportOnly = false;  // the turn is the report's alone: draw nothing
  double nap = 0;           // the longest single sleep within the wait
};
constexpr double kHiddenHousekeeping = 5.0;
constexpr double kHiddenPoll = 0.25;
constexpr double kHiddenBeat = 5.0;
constexpr double kShownPoll = 1.0 / 60.0;
constexpr double kDeepPoll = 1.0 / 20.0;
inline Wait planWait(const IdlePace& p, double now, bool shown, bool modified, double lastDrawn, double reportLeft, bool looks = false, double dutyLeft = 1e300) {
  Wait w;
  if (!p.enabled()) return w;
  if (shown) {
    w.left = p.waitLeft(now);
  } else {
    const double housekeeping = std::min(modified ? std::max(0.0, lastDrawn + kHiddenHousekeeping - now) : 1e300, std::max(0.0, dutyLeft));
    w.left = std::min(housekeeping, std::max(0.0, reportLeft));
    w.reportOnly = w.left <= 0 && housekeeping > 0;
  }
  w.nap = std::min(w.left, !shown ? (looks ? kHiddenPoll : kHiddenBeat) : p.deep(now) ? kDeepPoll : kShownPoll);
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

// 0.7.9 (TASKS.md:1441): SDL >= 2.0.16 falls back to a 1 ms busy-poll loop inside
// SDL_WaitEventTimeout whenever SDL_JoystickEventState is SDL_ENABLE. Pause joystick event
// polling across the kernel wait whenever nothing is shown or no joystick is connected (restored
// before SDL_PumpEvents so hotplug and 4 Hz hidden controller polling still work).
inline bool suppressJoystickWait(bool shown, int numJoysticks) { return !shown || numJoysticks <= 0; }

}  // namespace gl::idlepace
