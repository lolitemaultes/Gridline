// Gridline - J-186: play lights, the interface side (no ImGui here: unit tested).
//
//  * Channel light (channel rack): the bar on a channel's name button lights
//    when a note starts on the channel (a flash scaled by its velocity), stays
//    lit while a note is held or the channel's output sounds (brightness
//    follows the output level), then fades out smoothly (kRelease s). A muted
//    channel stays dark.
//  * Region lights (Kerf): every region a voice plays is lit (full while its
//    note is held, following the voice's level in its release), a region that
//    started since the last frame flashes even when it already ended, and the
//    voices' read positions are kept per region for a playhead line.
//
// Both read the engine's lock-free signals (NoteActivity, ChannelRuntime::peak,
// RegionActivity) once per UI frame.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>
#include "../engine/RegionActivity.h"

namespace gl::playlight {

constexpr float kRelease = 0.22f;      // s: fade once nothing sounds
constexpr float kHeldFloor = 0.45f;    // a held note (even a quiet one) keeps at least this
constexpr float kFloorDb = -54.0f;     // output level that starts to light
constexpr float kFullDb = -12.0f;      // output level that lights fully
constexpr float kLevelMin = 0.15f;     // brightness at kFloorDb

// Peak brightness of a note start: soft notes still show, hard ones are full.
inline float hitPeak(float velocity) { return 0.35f + 0.65f * std::clamp(velocity, 0.0f, 1.0f); }

// Output peak (linear) -> brightness: dark below kFloorDb, kLevelMin .. 1 up to kFullDb.
inline float levelLight(float peak) {
  if (!(peak > 0.0f)) return 0.0f;
  const float db = 20.0f * std::log10(peak);
  if (db < kFloorDb) return 0.0f;
  return kLevelMin + (1.0f - kLevelMin) * std::clamp((db - kFloorDb) / (kFullDb - kFloorDb), 0.0f, 1.0f);
}

// What the light draws with (smoothstep of its linear value).
inline float shade(float v) {
  const float x = std::clamp(v, 0.0f, 1.0f);
  return x * x * (3.0f - 2.0f * x);
}

struct ChannelLight {
  float value = 0;     // linear 0..1
  uint32_t seen = 0;   // note starts already shown
  bool primed = false;

  // One UI frame. `starts` / `velocity`: the channel's note-start counter and
  // the latest start's velocity; `held`: a note is held; `peak`: the last
  // block's output peak; `dt`: seconds since the previous update.
  void update(uint32_t starts, float velocity, bool held, float peak, bool muted, float dt, bool animate = true) {
    const bool hit = primed && starts != seen;
    seen = starts;
    primed = true;
    if (muted) {
      value = 0;
      return;
    }
    float target = levelLight(peak);
    if (held) target = std::max(target, kHeldFloor);
    if (hit) target = std::max(target, hitPeak(velocity));
    if (target >= value || !animate)
      value = target;
    else
      value = std::max(target, value - std::max(0.0f, dt) / kRelease);
  }
  // A row that was not drawn for a while (scrolled away): resume without a stale flash.
  void reprime() { primed = false; }
  float alpha() const { return shade(value); }
  bool lit() const { return value > 0.003f; }
};

// The regions of one Kerf instance.
struct RegionLights {
  std::vector<float> value;   // per region, linear 0..1
  std::vector<float> peakSeen;  // per region, the loudest voice level seen while it sounds
  struct Head {
    int region = -1;
    int64_t frame = -1;
  };
  Head heads[RegionActivity::kSlots];  // live read positions this frame
  int headCount = 0;
  uint32_t seen[RegionActivity::kSlots] = {};
  bool primed = false;

  void update(const RegionActivity& ra, int regions, float dt, bool animate = true) {
    const size_t n = static_cast<size_t>(std::max(0, regions));
    value.resize(n, 0.0f);
    peakSeen.resize(n, 0.0f);
    std::vector<float> target(n, 0.0f);
    headCount = 0;
    for (int i = 0; i < RegionActivity::kSlots; ++i) {
      const uint32_t st = ra.starts(i);
      const bool hit = primed && st != seen[i];
      seen[i] = st;
      if (hit) {
        const int r = ra.startedRegion(i);
        if (r >= 0 && static_cast<size_t>(r) < n) target[static_cast<size_t>(r)] = 1.0f;
      }
      const int r = ra.region(i);
      if (r < 0 || static_cast<size_t>(r) >= n) continue;
      const size_t q = static_cast<size_t>(r);
      const float lv = std::max(0.0f, ra.level(i));
      peakSeen[q] = std::max(peakSeen[q], lv);
      float t = 1.0f;
      if (!ra.held(i)) t = peakSeen[q] > 1e-6f ? std::clamp(0.25f + 0.6f * lv / peakSeen[q], 0.25f, 0.85f) : 0.6f;  // release tail
      target[q] = std::max(target[q], t);
      const int64_t f = ra.frame(i);
      if (f >= 0) heads[headCount++] = Head{r, f};
    }
    primed = true;
    for (size_t q = 0; q < n; ++q) {
      if (target[q] >= value[q] || !animate)
        value[q] = target[q];
      else
        value[q] = std::max(target[q], value[q] - std::max(0.0f, dt) / kRelease);
      if (value[q] <= 0.0f) peakSeen[q] = 0.0f;
    }
  }
  float light(int r) const { return r >= 0 && static_cast<size_t>(r) < value.size() ? shade(value[static_cast<size_t>(r)]) : 0.0f; }
  bool lit(int r) const { return light(r) > 0.003f; }
};

}  // namespace gl::playlight
