// Gridline: a note's vibrato (K-262), the math shared by the piano roll, playback and the exports.
// Header-inline and allocation-free: the audio thread calls value() while notes play.
//
// The swing is a wave (Shape) whose depth grows over the fade-in after an optional delay and falls
// over the fade-out at the note's end; its rate can sweep from `rate` to `rateEnd` over the note.
// `centre` places it: 0.5 around the pitch, 0 only above, 1 only below.
#pragma once
#include <algorithm>
#include <cmath>
#include "Types.h"

namespace gl::vib {

enum class Shape : uint8_t { Sine = 0, Triangle, Square, SawUp, SawDown, Count };

inline constexpr float kMaxDepth = 1200.0f;  // cents
inline constexpr float kMinRateHz = 0.1f, kMaxRateHz = 30.0f;
inline constexpr float kMinRateBeats = 1.0f / 64.0f, kMaxRateBeats = 8.0f;  // synced: a cycle's length

inline const char* shapeName(Shape s) {
  switch (s) {
    case Shape::Sine: return "Sine";
    case Shape::Triangle: return "Triangle";
    case Shape::Square: return "Soft square";
    case Shape::SawUp: return "Saw up";
    case Shape::SawDown: return "Saw down";
    default: return "";
  }
}

// Every field in range (a file, a script, a collaborator's edit). NaN becomes the default.
inline void clamp(NoteVibrato& v) {
  const NoteVibrato d;
  auto fin = [](float x, float def) { return std::isfinite(x) ? x : def; };
  v.depth = std::clamp(fin(v.depth, 0), 0.0f, kMaxDepth);
  const float lo = v.sync ? kMinRateBeats : kMinRateHz, hi = v.sync ? kMaxRateBeats : kMaxRateHz;
  v.rate = std::clamp(fin(v.rate, v.sync ? 0.25f : d.rate), lo, hi);
  v.rateEnd = fin(v.rateEnd, 0) <= 0 ? 0.0f : std::clamp(v.rateEnd, lo, hi);
  v.delay = std::clamp(fin(v.delay, 0), 0.0f, 0.95f);
  v.fadeIn = std::clamp(fin(v.fadeIn, d.fadeIn), 0.0f, 1.0f);
  v.fadeOut = std::clamp(fin(v.fadeOut, 0), 0.0f, 1.0f);
  v.fadeInCurve = std::clamp(fin(v.fadeInCurve, 0), -1.0f, 1.0f);
  v.fadeOutCurve = std::clamp(fin(v.fadeOutCurve, 0), -1.0f, 1.0f);
  if (v.shape >= static_cast<uint8_t>(Shape::Count)) v.shape = 0;
  v.phase = fin(v.phase, 0);
  v.phase -= std::floor(v.phase);
  v.centre = std::clamp(fin(v.centre, d.centre), 0.0f, 1.0f);
}

// The wave at phase p (cycles), -1..1.
inline double wave(Shape s, double p) {
  constexpr double kTwoPi = 6.283185307179586;
  const double f = p - std::floor(p);
  switch (s) {
    case Shape::Triangle: return f < 0.25 ? 4 * f : f < 0.75 ? 2 - 4 * f : 4 * f - 4;
    case Shape::Square: return std::tanh(4.0 * std::sin(kTwoPi * f)) / std::tanh(4.0);
    case Shape::SawUp: return 2 * f - 1;
    case Shape::SawDown: return 1 - 2 * f;
    default: return std::sin(kTwoPi * f);
  }
}

// A fade's shape: x 0..1, tension -1..1 (positive rises early, negative late).
inline double curve(double x, float tension) {
  x = std::clamp(x, 0.0, 1.0);
  if (tension == 0) return x;
  return std::pow(x, std::exp2(-2.0 * static_cast<double>(tension)));
}

// The rate in Hz at the start (end = false) or the end of the note.
inline double rateHz(const NoteVibrato& v, double beatSec, bool end = false) {
  const float r = end && v.rateEnd > 0 ? v.rateEnd : v.rate;
  if (!v.sync) return r;
  return 1.0 / std::max(1e-6, static_cast<double>(r) * std::max(1e-6, beatSec));
}

// The depth envelope (0..1) at fraction f of the note.
inline double envelope(const NoteVibrato& v, double f) {
  if (f < v.delay) return 0;
  double e = 1;
  if (v.fadeIn > 0) e = curve((f - v.delay) / v.fadeIn, v.fadeInCurve);
  if (v.fadeOut > 0) e *= curve((1.0 - f) / v.fadeOut, v.fadeOutCurve);
  return std::clamp(e, 0.0, 1.0);
}

// The wave's phase (cycles, the start phase included) `t` seconds into a note of `len` seconds.
inline double phaseAt(const NoteVibrato& v, double t, double len, double beatSec) {
  const double d = static_cast<double>(v.delay) * len, tau = std::max(0.0, t - d), span = std::max(1e-9, len - d);
  const double r0 = rateHz(v, beatSec), r1 = rateHz(v, beatSec, true);
  return static_cast<double>(v.phase) + r0 * tau + (r1 - r0) * tau * tau / (2.0 * span);
}

// The pitch offset in semitones `t` seconds into a note `len` seconds long; `beatSec` is a beat's
// length (synced rates). 0 for a note without vibrato.
inline double value(const NoteVibrato& v, double t, double len, double beatSec) {
  if (!(v.depth > 0) || len <= 0) return 0;
  const double f = t / len;
  const double env = envelope(v, f);
  if (env <= 0) return 0;
  const double w = wave(static_cast<Shape>(v.shape < static_cast<uint8_t>(Shape::Count) ? v.shape : 0), phaseAt(v, t, len, beatSec));
  const double k = 2.0 * static_cast<double>(v.centre) - 1.0;
  const double cents = static_cast<double>(v.depth) * (w * (1.0 - std::fabs(k) / 2.0) - k / 2.0);
  return cents * env / 100.0;
}

// Presets: our own names and values.
struct Preset {
  const char* name;
  float depth, rate, delay, fadeIn, fadeOut;
  uint8_t shape;
};
inline constexpr Preset kPresets[] = {
    {"Gentle", 15.0f, 5.0f, 0.15f, 0.35f, 0.0f, 0},
    {"Singer", 35.0f, 5.6f, 0.25f, 0.30f, 0.05f, 0},
    {"Violin", 22.0f, 6.4f, 0.10f, 0.20f, 0.0f, 0},
    {"Wide", 80.0f, 4.5f, 0.05f, 0.20f, 0.10f, 0},
    {"Fast trill", 100.0f, 11.0f, 0.0f, 0.0f, 0.0f, 3},
};
inline constexpr int kPresetCount = static_cast<int>(sizeof kPresets / sizeof kPresets[0]);

inline NoteVibrato fromPreset(const Preset& p) {
  NoteVibrato v;
  v.depth = p.depth;
  v.rate = p.rate;
  v.delay = p.delay;
  v.fadeIn = p.fadeIn;
  v.fadeOut = p.fadeOut;
  v.shape = p.shape;
  return v;
}

// A note made of two (split) keeps its vibrato on each part with the times rescaled so each part
// sounds as the whole did over its span: `from`..`to` (fractions of the old note) becomes the part.
inline NoteVibrato part(const NoteVibrato& v, double from, double to) {
  NoteVibrato o = v;
  if (!v.active() || to <= from) return o;
  const double span = to - from;
  auto map = [&](double x) { return std::clamp((x - from) / span, 0.0, 1.0); };
  o.delay = static_cast<float>(std::min(0.95, map(v.delay)));
  const double inEnd = v.delay + v.fadeIn, outStart = 1.0 - v.fadeOut;
  o.fadeIn = v.fadeIn > 0 ? static_cast<float>(std::max(0.0, map(inEnd) - o.delay)) : 0.0f;
  o.fadeOut = v.fadeOut > 0 ? static_cast<float>(1.0 - map(outStart)) : 0.0f;
  clamp(o);
  return o;
}

}  // namespace gl::vib
