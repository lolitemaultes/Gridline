// Gridline - lit keys and pad flashes (J-77), the interface side of the
// engine's per-channel NoteActivity: no ImGui here (unit tested).
//
//  * Keys (piano roll keyboard, channel preview keyboard, editor keyboards):
//    full while the note is held, then a short smooth fade (kKeyRelease s).
//    A note that started and ended between two frames still blinks.
//  * Pads (drum instruments): every hit restarts a flash - a quick bright
//    attack (kFlashAttack s) then an ease-out fade over kFlashDecay s, its
//    height scaled by the hit's velocity.
#pragma once
#include <algorithm>
#include <cstdint>
#include "../engine/NoteActivity.h"

namespace gl::keylight {

constexpr float kKeyRelease = 0.16f;   // s: key fade after the note ends
constexpr float kFlashAttack = 0.025f; // s: pad flash rise
constexpr float kFlashDecay = 1.0f;    // s: pad flash fade

// Peak brightness of a hit: soft hits still show, hard hits are full.
inline float flashPeak(float velocity) { return 0.35f + 0.65f * std::clamp(velocity, 0.0f, 1.0f); }

// Pad flash `t` seconds after a hit of `velocity` (0 once it has faded).
inline float flashEnvelope(float t, float velocity) {
  if (t < 0) return 0;
  const float peak = flashPeak(velocity);
  if (t < kFlashAttack) return peak * (0.6f + 0.4f * t / kFlashAttack);  // starts bright: the hit reads at once
  const float u = std::min(1.0f, (t - kFlashAttack) / kFlashDecay);
  const float r = 1.0f - u;
  return peak * r * r * r;  // ease-out: fast at first, settling softly
}

// Smoothstep of a key's linear level (what the key draws with).
inline float keyAlpha(float level) {
  const float x = std::clamp(level, 0.0f, 1.0f);
  return x * x * (3.0f - 2.0f * x);
}

struct ChannelLights {
  float level[NoteActivity::kKeys] = {};      // key light 0..1 (linear; draw with keyAlpha)
  float flashAge[NoteActivity::kKeys] = {};   // s since the key's last hit
  float flashVel[NoteActivity::kKeys] = {};   // that hit's velocity (0 = no hit yet)
  uint32_t seen[NoteActivity::kKeys] = {};
  bool down[NoteActivity::kKeys] = {};  // held (or hit) this frame
  bool primed = false;
  bool animated = true;  // J-91: Interface animations; off: lights switch on and off, no fades

  // One UI frame: `dt` seconds since the previous update.
  void update(const NoteActivity& na, float dt, bool animate = true) {
    animated = animate;
    for (int k = 0; k < NoteActivity::kKeys; ++k) {
      const uint32_t h = na.hits(k);
      const bool hit = primed && h != seen[k];
      seen[k] = h;
      if (hit) {
        flashAge[k] = 0;
        flashVel[k] = std::max(0.01f, na.velocity(k));
      } else if (flashVel[k] > 0) {
        flashAge[k] += dt;
        if (flashAge[k] > kFlashAttack + kFlashDecay) flashVel[k] = 0;
      }
      down[k] = na.held(k) || hit;
      if (down[k])
        level[k] = 1;
      else
        level[k] = animate ? std::max(0.0f, level[k] - dt / kKeyRelease) : 0.0f;
    }
    primed = true;
  }
  float key(int k) const { return k < 0 || k >= NoteActivity::kKeys ? 0.f : keyAlpha(level[k]); }
  float flash(int k) const {
    if (k < 0 || k >= NoteActivity::kKeys || flashVel[k] <= 0) return 0;
    if (!animated) return down[k] ? flashPeak(flashVel[k]) : 0.0f;  // lit while the pad sounds, no fade
    return flashEnvelope(flashAge[k], flashVel[k]);
  }
};

}  // namespace gl::keylight
