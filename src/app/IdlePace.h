// Gridline - K-277: idle frame pacing. The interface draws every display refresh while something
// happens (input, playback, sound, a load, an edit, a drag) and, once nothing has for a moment,
// draws a few frames a second until something does again. The main loop waits between those
// frames one refresh at a time and draws at once when input or sound comes (IdlePace::wake).
#pragma once
#include <algorithm>

namespace gl::idlepace {

class IdlePace {
 public:
  static constexpr double kCalmAfter = 1.0;      // seconds without activity before frames slow down
  static constexpr double kIdleInterval = 0.1;   // seconds between frames while idle (10 a second)

  void setEnabled(bool on) { enabled_ = on; }
  bool enabled() const { return enabled_; }
  // Something happened at `now` (seconds): full rate for kCalmAfter from here.
  void active(double now) { lastActive_ = now; }
  // A frame was drawn at `now`.
  void framed(double now) { lastFrame_ = now; }
  bool idle(double now) const { return enabled_ && now - lastActive_ >= kCalmAfter; }
  // Seconds the loop may still wait before it draws the next frame (0: draw now).
  double waitLeft(double now) const { return idle(now) ? std::max(0.0, lastFrame_ + kIdleInterval - now) : 0.0; }

 private:
  bool enabled_ = true;
  double lastActive_ = -1e300, lastFrame_ = -1e300;
};

}  // namespace gl::idlepace
