// Gridline — J-79: audio clip gain (the playlist's volume line). Clip::gain
// is linear, the value FL keeps in its playlist item (@52). FL's range is
// -96 .. +36 dB; at the bottom the clip is silent. Dragging the volume line
// changes the gain in dB, a fixed amount per pixel, with a short detent at
// 0 dB (a drag passing unity pauses there for a few pixels) that a modifier
// bypasses. Pure: no UI dependencies, unit tested.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace gl::clipgain {

constexpr double kMinDb = -96.0;  // the bottom of the range: silence
constexpr double kMaxDb = 36.0;
// Linear limits of FL's stored value: the top is +36 dB; at the bottom FL
// keeps a value just under -96 dB, which plays as silence.
constexpr float kMaxGain = 63.0957344f;      // 10^(36/20)
constexpr float kSilentGain = 1.56675e-5f;   // stored at the bottom (-96.1 dB)
constexpr float kAudibleGain = 1.58489e-5f;  // 10^(-96/20): lower plays as 0

// dB moved per (logical) pixel of vertical drag, and with the fine modifier.
constexpr double kDbPerPx = 0.2;
constexpr double kFineDbPerPx = 0.02;
// Width (logical px) of the detent at 0 dB.
constexpr double kDetentPx = 8.0;

inline double toDb(float gain) {
  if (!(gain > kAudibleGain * 0.9999f)) return kMinDb - 0.1;  // silent
  return 20.0 * std::log10(static_cast<double>(gain));
}
inline bool silent(float gain) { return !(gain >= kAudibleGain * 0.9999f); }
inline float fromDb(double db) {
  if (db <= kMinDb - 0.05) return kSilentGain;
  db = std::min(db, kMaxDb);
  return std::min(kMaxGain, static_cast<float>(std::pow(10.0, db / 20.0)));
}
// The gain the engine plays (the bottom of the range is silence).
inline float playGain(float gain) { return silent(gain) ? 0.0f : std::min(gain, kMaxGain); }
// A loaded / typed value brought into FL's range (NaN: unity).
inline float clampGain(float gain) {
  if (!std::isfinite(gain)) return 1.0f;
  return std::clamp(gain, 0.0f, kMaxGain);
}

// "-3.2 dB", "+1.5 dB", "0.0 dB", "-inf dB" (silent).
inline std::string label(float gain) {
  if (silent(gain)) return "-inf dB";
  double db = toDb(gain);
  if (std::fabs(db) < 0.05) db = 0.0;
  char b[32];
  std::snprintf(b, sizeof b, db > 0 ? "+%.1f dB" : "%.1f dB", db);
  return b;
}

// A volume line drag (FL's rules, from its playlist self-tests): the gain
// moves a fixed step in dB per pixel the pointer moves (up = louder), and a
// drag that reaches or crosses 0 dB pauses there for `detentPx` pixels before
// moving on, so unity is easy to hit. A drag that starts at 0 dB leaves it
// with its first pixel (no pause at the start). `fine` uses the fine step,
// `free` (FL's snap bypass) skips the pause. Past either end the value stays
// at the limit (below -96 dB: silent).
//
//   Drag d; d.begin(currentDb, fine, free);   // on press (and again when a modifier changes)
//   double db = d.move(pixelsUpSincePress);    // every frame
struct Drag {
  bool fine = false, free = false;
  double detentPx = kDetentPx;

  void begin(double db, bool fineMod = false, bool freeMod = false, double upPxNow = 0) {
    fine = fineMod, free = freeMod, last_ = upPxNow;
    fromZero_ = std::fabs(db) < 1e-9;
    v_ = clampPos(posOf(db));
  }
  // The value after the pointer moved to `upPx` pixels above the press point.
  double move(double upPx) {
    const double d = upPx - last_;
    last_ = upPx;
    if (d == 0) return db();
    if (fromZero_ && !free) v_ = d > 0 ? detentPx * 0.5 : -detentPx * 0.5;  // leave unity at once
    fromZero_ = false;
    v_ = clampPos(v_ + d);
    return db();
  }
  double db() const {
    if (v_ < posOf(kMinDb) - 1e-9) return kMinDb - 0.1;  // silent
    return std::min(kMaxDb, dbOf(v_));
  }
  float gain() const { return fromDb(db()); }

 private:
  double v_ = 0, last_ = 0;
  bool fromZero_ = false;
  double rate() const { return fine ? kFineDbPerPx : kDbPerPx; }
  double half() const { return free ? 0.0 : detentPx * 0.5; }
  double posOf(double db) const {
    if (db > 1e-9) return db / rate() + half();
    if (db < -1e-9) return db / rate() - half();
    return 0.0;
  }
  double dbOf(double v) const {
    if (v > half()) return (v - half()) * rate();
    if (v < -half()) return (v + half()) * rate();
    return 0.0;
  }
  // One pixel below the bottom is silence; nothing goes further.
  double clampPos(double v) const { return std::clamp(v, posOf(kMinDb) - 1.0, posOf(kMaxDb)); }
};

}  // namespace gl::clipgain
