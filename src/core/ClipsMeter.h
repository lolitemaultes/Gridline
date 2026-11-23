// Gridline: the Clips layout's track meters (K-383): their dB scale and ballistics.
//
// The meters show each mixer track's post-fader peak (the engine's take peak since the last frame,
// MixerMeters). The scale runs from kFloorDb at the bottom to 0 dBFS at the top and is weighted to
// the top: with x = (dB - floor) / -floor, the height is kLinear * x + (1 - kLinear) * x^2, so the
// last 12 dB below 0 take about 30 % of the height and -18 dBFS sits a little above the middle
// (about 0.56), where the shared -60..+6 dB scale put it at 0.64 and -6 dB at 0.82. Anything at or
// over 0 dBFS fills the meter and lights it red. Green runs up to kYellowDb, yellow from there to
// 0 dBFS.
//
// Ballistics: instant attack, a release of kReleaseDbPerSec (by the real elapsed time) and a short
// peak hold, the shared LevelMeter's logic with these two values.
#pragma once

#include <algorithm>
#include <cmath>

#include "LevelMeter.h"

namespace gl::clipsmeter {

constexpr float kFloorDb = -60.0f;
constexpr float kLinear = 0.35f;
constexpr float kYellowDb = -6.0f;
constexpr float kReleaseDbPerSec = 20.0f;
constexpr float kHoldSeconds = 0.6f;
// The scale's labelled ticks (dB under 0): 0, 12, 24, 36, 48, 60.
constexpr int kTickStepDb = 12;

// dB to the 0..1 height on the meter (0 at and under the floor, 1 at and over 0 dBFS).
inline float pos(float db) {
  if (!(db > kFloorDb)) return 0.0f;
  if (db >= 0.0f) return 1.0f;
  const float x = (db - kFloorDb) / -kFloorDb;
  return kLinear * x + (1.0f - kLinear) * x * x;
}
// The height back to dB (the inverse of pos; 0 gives the floor).
inline float dbAt(float p) {
  if (!(p > 0.0f)) return kFloorDb;
  if (p >= 1.0f) return 0.0f;
  const float a = 1.0f - kLinear, b = kLinear;
  const float x = (-b + std::sqrt(b * b + 4.0f * a * p)) / (2.0f * a);
  return kFloorDb + x * -kFloorDb;
}
// A linear gain's height (silence and NaN at the bottom).
inline float gainPos(float gain) {
  if (!(gain > 0.0f)) return 0.0f;
  if (!std::isfinite(gain)) return 1.0f;
  return pos(20.0f * std::log10(gain));
}

// A meter with the Clips ballistics.
inline LevelMeter makeMeter() { return LevelMeter(kReleaseDbPerSec, kHoldSeconds); }

}  // namespace gl::clipsmeter
