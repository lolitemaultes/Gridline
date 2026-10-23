// Gridline - J-91: interface motion switches and the metronome swing (see Motion.h).
#include "Motion.h"
#include <algorithm>
#include <cmath>

namespace gl::motion {

Switches effective(bool animations, bool smoothSnap, bool smoothScroll, bool elastic, bool reducedMotion) {
  Switches s;
  s.animations = animations && !reducedMotion;
  s.smoothSnap = s.animations && smoothSnap;
  s.smoothScroll = s.animations && smoothScroll;
  s.elastic = s.animations && elastic;
  return s;
}

double metronomeSwing(double beats, double bpm, bool swinging) {
  if (!swinging || !std::isfinite(beats) || beats < 0) return 1.0;
  const double k = std::floor(beats);
  const double frac = beats - k;
  const double side = std::fmod(k, 2.0) < 0.5 ? 1.0 : -1.0;  // even beats face right, odd beats left
  const double turn = std::clamp(kSwingSeconds * std::max(1.0, bpm) / 60.0, 0.05, 0.45);  // the turn's length in beats
  if (frac >= turn) return side;
  const double u = frac / turn;
  const double e = u * u * (3.0 - 2.0 * u);  // ease in and out
  return -side * std::cos(e * 3.14159265358979323846);  // from the previous side (-side) through 0 to `side`
}

}  // namespace gl::motion
