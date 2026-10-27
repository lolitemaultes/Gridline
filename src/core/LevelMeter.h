// Gridline: peak level meter ballistics (J-143), shared by the mixer strip
// meters, the top bar OUT meter and the small widget meters so they all read
// the same way.
//
// Input: the largest |sample| the engine saw since the UI last looked (a
// "take" peak, so nothing between two frames is missed). Ballistics run in
// dB: instant attack, a linear release of kReleaseDbPerSec scaled by the real
// elapsed time (the same fall at 30 or 240 frames per second), a per-channel
// peak hold of kHoldSeconds that then falls at the release rate, and a clip
// lamp lit for kClipSeconds after a real over (a sample above 0 dBFS; a
// full-scale 1.0 is not an over). The scale is kFloorDb..kCeilDb, the same
// -60..+6 dB the mixer meters draw; anything at or under the floor rests at 0.
#pragma once

#include <algorithm>
#include <cmath>

namespace gl {

struct LevelMeter {
  static constexpr float kFloorDb = -60.0f;
  static constexpr float kCeilDb = 6.0f;
  static constexpr float kReleaseDbPerSec = 24.0f;
  static constexpr float kHoldSeconds = 1.5f;
  static constexpr float kClipSeconds = 3.0f;

  // Linear gain to dB: silence, NaN and anything under the floor read as the
  // floor, infinity as the top of the scale.
  static float toDb(float gain) {
    if (!(gain > 0.0f)) return kFloorDb;
    if (!std::isfinite(gain)) return kCeilDb;
    return std::max(kFloorDb, 20.0f * std::log10(gain));
  }
  // dB to the 0..1 position on the meter's scale.
  static float toPos(float db) { return std::clamp((db - kFloorDb) / (kCeilDb - kFloorDb), 0.0f, 1.0f); }
  // dB back to linear gain; the floor reads as silence (0).
  static float toGain(float db) { return db <= kFloorDb ? 0.0f : std::pow(10.0f, db / 20.0f); }

  // One UI frame: the peaks taken since the last frame and the seconds since it.
  void update(float peakL, float peakR, float dt) {
    if (!(dt >= 0.0f) || !std::isfinite(dt)) dt = 0.0f;
    const float fall = kReleaseDbPerSec * dt;
    const float in[2] = {peakL, peakR};
    for (int c = 0; c < 2; ++c) {
      const float db = toDb(in[c]);
      level_[c] = std::max(db, std::max(kFloorDb, level_[c] - fall));
      if (db >= hold_[c]) {
        hold_[c] = db;
        holdLeft_[c] = kHoldSeconds;
      } else if (holdLeft_[c] > dt) {
        holdLeft_[c] -= dt;
      } else {  // hold time over: the marker falls (at the release rate) to the bar
        const float spill = dt - std::max(0.0f, holdLeft_[c]);
        holdLeft_[c] = 0.0f;
        hold_[c] = std::max(level_[c], std::max(kFloorDb, hold_[c] - kReleaseDbPerSec * spill));
      }
    }
    if (peakL > 1.0f || peakR > 1.0f) clipLeft_ = kClipSeconds;
    else clipLeft_ = std::max(0.0f, clipLeft_ - dt);
  }
  void reset() { *this = LevelMeter{}; }

  float levelDb(int ch) const { return level_[ch & 1]; }
  float holdDb(int ch) const { return hold_[ch & 1]; }
  float level(int ch) const { return toGain(level_[ch & 1]); }  // linear, 0 at rest
  float hold(int ch) const { return toGain(hold_[ch & 1]); }
  bool clipping() const { return clipLeft_ > 0.0f; }
  bool atRest() const {
    return level_[0] <= kFloorDb && level_[1] <= kFloorDb && hold_[0] <= kFloorDb && hold_[1] <= kFloorDb && !clipping();
  }

 private:
  float level_[2] = {kFloorDb, kFloorDb};
  float hold_[2] = {kFloorDb, kFloorDb};
  float holdLeft_[2] = {0.0f, 0.0f};
  float clipLeft_ = 0.0f;
};

}  // namespace gl
