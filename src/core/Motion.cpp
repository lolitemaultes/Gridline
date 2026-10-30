// Gridline - J-91: interface motion switches and the metronome tick (see Motion.h).
#include "Motion.h"
#include <algorithm>
#include <cmath>

namespace gl::motion {

Switches effective(bool animations, bool smoothSnap, bool smoothScroll, bool elastic, bool smoothZoom, bool reducedMotion) {
  Switches s;
  s.animations = animations && !reducedMotion;
  s.smoothSnap = s.animations && smoothSnap;
  s.smoothScroll = s.animations && smoothScroll;
  s.elastic = s.animations && elastic;
  s.smoothZoom = s.animations && smoothZoom;
  return s;
}

Switches effective(bool animations, bool smoothSnap, bool smoothScroll, bool elastic, bool reducedMotion) {
  return effective(animations, smoothSnap, smoothScroll, elastic, true, reducedMotion);
}

double metronomeTick(double beats, bool ticking, double anchorBeat) {
  if (!ticking || !std::isfinite(beats) || beats < 0 || !std::isfinite(anchorBeat)) return 1.0;
  const double steps = std::fabs(std::floor(beats) - std::floor(anchorBeat));  // whole beats since the anchor
  return std::fmod(steps, 2.0) < 0.5 ? 1.0 : -1.0;
}

double MetronomeTicker::update(double beats, bool ticking) {
  if (ticking && !ticking_ && std::isfinite(beats) && beats >= 0) anchor_ = std::floor(beats);
  ticking_ = ticking && std::isfinite(beats) && beats >= 0;
  return metronomeTick(beats, ticking_, anchor_);
}

}  // namespace gl::motion
