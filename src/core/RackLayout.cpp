// Gridline — J-84: channel rack row layout. See RackLayout.h.
#include "RackLayout.h"

#include <algorithm>
#include <cmath>

namespace gl::racklayout {

float contentWidth(int steps, float stepW) {
  if (steps <= 0) return 0;
  return static_cast<float>(steps) * stepW + static_cast<float>(steps - 1) * kStepGap + static_cast<float>((steps - 1) / 4) * kGroupGap;
}

float Layout::stepPos(int k) const { return static_cast<float>(k) * (stepW + kStepGap) + static_cast<float>(k / 4) * kGroupGap; }

int Layout::stepAt(float x) const {
  if (x < 0 || x >= contentW) return -1;
  // Steps are evenly spaced within groups of four: find the group, then the step.
  const float groupW = 4 * (stepW + kStepGap) + kGroupGap;
  const int g = static_cast<int>(x / groupW);
  const float in = x - static_cast<float>(g) * groupW;
  const int k = g * 4 + std::min(3, static_cast<int>(in / (stepW + kStepGap)));
  return k < steps ? k : -1;
}

Layout compute(const Input& in) {
  Layout l;
  l.steps = std::max(1, in.steps);
  const int n = l.steps;
  const float room = std::max(0.0f, in.avail - kNameX - kNameGap - kSelLampW);  // name + step viewport
  // The name yields to the first bars of the pattern: a 16-step pattern at a
  // comfortable step size, up to 32 steps at the smallest size. Longer
  // patterns scroll instead of squeezing the names further.
  const float want = std::max(contentWidth(std::min(n, 16), kStepComfort), contentWidth(std::min(n, 32), kStepMin));
  // Patterns of up to 32 steps can fit without scrolling: there the name may
  // give way down to kNameSqueeze (no scroller that moves a few pixels);
  // longer patterns scroll anyway and keep kNameMin.
  float nameW = std::clamp(room - want, n <= 32 ? kNameSqueeze : kNameMin, kNameMax);
  if (room - nameW < kMinView) nameW = std::max(kNameFloor, room - kMinView);  // tiny rack: keep a few steps visible
  l.nameW = std::floor(nameW);
  l.nameShrunk = l.nameW < kNameMax;
  const float viewMax = std::max(kMinView, room - l.nameW);
  const float gaps = static_cast<float>(n - 1) * kStepGap + static_cast<float>((n - 1) / 4) * kGroupGap;
  l.stepW = std::clamp(std::floor((viewMax - gaps) / static_cast<float>(n)), kStepMin, kStepMax);
  l.contentW = contentWidth(n, l.stepW);
  l.viewW = std::min(l.contentW, viewMax);
  l.scrollMax = std::max(0.0f, l.contentW - l.viewW);
  l.stepsX = kNameX + l.nameW + kNameGap;
  l.lampX = l.stepsX + l.viewW;
  l.rowW = l.lampX + kSelLampW;
  return l;
}

float clampScroll(const Layout& l, float scroll) { return std::clamp(scroll, 0.0f, l.scrollMax); }

float revealStep(const Layout& l, float scroll, int k) {
  if (k < 0 || k >= l.steps || !l.scrolls()) return clampScroll(l, scroll);
  const float x0 = l.stepPos(k), x1 = x0 + l.stepW;
  if (x0 < scroll) scroll = x0;
  else if (x1 > scroll + l.viewW) scroll = x1 - l.viewW;
  return clampScroll(l, scroll);
}

bool thumb(const Layout& l, float scroll, float trackW, float* x0, float* w) {
  if (!l.scrolls() || trackW <= 0 || l.contentW <= 0) return false;
  const float tw = std::max(std::min(trackW, 16.0f), trackW * l.viewW / l.contentW);
  const float t = std::clamp(scroll / l.scrollMax, 0.0f, 1.0f);
  *w = tw;
  *x0 = (trackW - tw) * t;
  return true;
}

float scrollForTrack(const Layout& l, float trackX, float trackW) {
  float x0 = 0, tw = 0;
  if (!thumb(l, 0, trackW, &x0, &tw) || trackW <= tw) return 0;
  return clampScroll(l, (trackX - tw * 0.5f) / (trackW - tw) * l.scrollMax);
}

}  // namespace gl::racklayout
