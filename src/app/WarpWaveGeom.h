// Gridline: the geometry of warp markers on a waveform (ui::WarpWave), kept
// free of drawing so tests can check it: which marker the pointer is on, and
// whether a section's speed label fits.
#pragma once
#include <cmath>
#include <cstdint>
#include <vector>

namespace gl::warpwave {

// A marker as a waveform shows it: its id, its x (px) and the speed of the section after it
// (source per warped second; 1: unchanged).
struct Mark {
  uint32_t id = 0;
  float x = 0;
  double speed = 1;
};

inline constexpr float kGrab = 4.0f;  // px either side of a marker

// The marker nearest the pointer, less than `grab` + 1 px away (0: none). Markers outside [visX0 - grab,
// visX1 + grab] are not drawn, so they cannot be grabbed.
inline uint32_t hitTest(const std::vector<Mark>& v, float mouseX, float visX0, float visX1, float grab = kGrab) {
  uint32_t hot = 0;
  float best = grab + 1;
  for (const Mark& k : v) {
    const float x = std::floor(k.x) + 0.5f;
    if (x < visX0 - grab || x > visX1 + grab) continue;
    const float d = std::fabs(mouseX - x);
    if (d < best) best = d, hot = k.id;  // (within grab + 1 px, as the playlist always took it)
  }
  return hot;
}

// The section's speed as a label shows it: how long it plays against its own length (100: as recorded).
inline double sectionPercent(double speed) { return 100.0 / (speed > 1e-6 ? speed : 1e-6); }
// A section changed speed (it is tinted and labelled).
inline bool sectionChanged(double speed) { return std::fabs(speed - 1.0) >= 1e-3; }
// A label `textW` wide fits a section `secW` wide with its margins.
inline bool labelFits(float secW, float textW, float bodyH, float textH) { return secW > textW + 10 && bodyH > textH + 6; }

}  // namespace gl::warpwave
