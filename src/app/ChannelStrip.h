// Gridline - the channel strip of a plugin window (volume, pan, pitch and the
// preview keyboard at the top of the channel settings window) shows and hides
// with the eye button in the window's header row. Hidden by default; the
// choice is global (Settings key "ui.channelStrip", saved with the settings)
// rather than per window, so every plugin window opens the same way.
//
// Showing it slides the strip down from under the header (ease-out, about
// 140 ms), hiding it slides it back up; the strip's area is clipped while it
// moves and the rest of the window follows it smoothly. With Interface
// animations off (or the system asking for reduced motion) it switches at once.
//
// Installed plugins that open their own interface do so in a window of their
// own (PluginWindow.cpp) that has no channel strip, so a native plugin view is
// never moved by this animation.
#pragma once
#include <algorithm>
#include <cmath>

namespace gl::chstrip {

inline constexpr double kSlideSeconds = 0.14;  // a full open or close

// Ease-out (cubic): fast start, gentle landing.
inline double easeOut(double x) {
  x = std::clamp(x, 0.0, 1.0);
  const double r = 1.0 - x;
  return 1.0 - r * r * r;
}

// The shown fraction of the strip (0 hidden .. 1 shown) over time. Pure: the
// caller passes the clock (ImGui::GetTime(), which follows --fixed-dt).
struct Reveal {
  bool target = false;  // shown or hidden at the end
  double from = 0.0;    // fraction when the current slide started
  double start = 0.0;   // when it started
  double span = 0.0;    // its length in seconds (0: no slide)
  bool ready = false;   // initialised from the setting

  // The fraction shown at `now`.
  double at(double now) const {
    const double to = target ? 1.0 : 0.0;
    if (span <= 0.0) return to;
    return from + (to - from) * easeOut((now - start) / span);
  }
  bool moving(double now) const { return span > 0.0 && now - start < span; }
  // Starts at `shown` with no slide (window opened, settings loaded).
  void reset(bool shown) {
    target = shown;
    from = shown ? 1.0 : 0.0;
    span = 0.0;
    ready = true;
  }
  // Slides toward `shown` from wherever it is now; a reversal mid-way takes
  // only the time the remaining distance needs.
  void set(bool shown, double now, bool animate) {
    if (shown == target && ready) return;
    const double cur = ready ? at(now) : (shown ? 0.0 : 1.0);
    target = shown;
    ready = true;
    from = cur;
    start = now;
    span = animate ? kSlideSeconds * std::fabs((shown ? 1.0 : 0.0) - cur) : 0.0;
  }
};

}  // namespace gl::chstrip
