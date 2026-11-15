// Gridline - a pattern clip's playback settings (K-112b3): transpose, velocity offset, delay and
// playback quantise, applied to the clip's notes as they play. The pattern itself stays as
// written, so every other clip of the same pattern plays it unchanged. Shown in the Logic
// layout's Region card, Live's Clip panel and the clip's own menu; saved only when set.
#pragma once
#include <algorithm>
#include <cmath>
#include <string>
#include "Types.h"

namespace gl::clipplay {

inline bool active(const Clip& c) { return c.playTranspose != 0 || c.playVelocity != 0 || c.playDelay != 0 || c.playQuantize != 0; }

// One note of the clip: `on` / `off` in song ticks (with fractions), `start` the clip's start,
// `pitch` a MIDI key, `velocity` 0..1. Quantise snaps the start to the grid counted from the clip's
// start (the note keeps its length), then the delay moves the note; nothing starts before tick 0.
inline void apply(const Clip& c, Tick start, Tick& on, Tick& off, float& onFrac, float& offFrac, float& pitch, float& velocity) {
  if (c.playQuantize > 0) {
    const double rel = static_cast<double>(on - start) + onFrac;
    const double q = static_cast<double>(c.playQuantize);
    const Tick snapped = start + static_cast<Tick>(std::llround(rel / q) * c.playQuantize);
    const double len = std::max(1.0, static_cast<double>(off - on) + offFrac - onFrac);
    on = snapped, onFrac = 0;
    const double end = static_cast<double>(on) + len;
    off = static_cast<Tick>(std::floor(end)), offFrac = static_cast<float>(end - std::floor(end));
  }
  if (c.playDelay != 0) {
    const Tick d = std::max<Tick>(c.playDelay, -on);
    on += d, off += d;
  }
  if (c.playTranspose != 0) pitch = std::clamp(pitch + static_cast<float>(c.playTranspose), 0.0f, 127.0f);
  if (c.playVelocity != 0) velocity = std::clamp(velocity + static_cast<float>(c.playVelocity) / 127.0f, 1.0f / 127.0f, 1.0f);
}

// A short tag for the clip's title ("+3 st", "vel -10", ...): what is set, so every layout shows it.
inline std::string tag(const Clip& c) {
  std::string t;
  auto add = [&](const std::string& x) { t += (t.empty() ? "" : " \xC2\xB7 ") + x; };
  auto sgn = [](long long v) { return (v > 0 ? "+" : "") + std::to_string(v); };
  if (c.playTranspose != 0) add(sgn(c.playTranspose) + " st");
  if (c.playVelocity != 0) add("vel " + sgn(c.playVelocity));
  if (c.playDelay != 0) add("delay " + sgn(static_cast<long long>(c.playDelay)));
  if (c.playQuantize != 0) add("Q");
  return t;
}

}  // namespace gl::clipplay
