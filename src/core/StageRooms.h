// Gridline - the Stage's room types (Song::stage.roomType; docs/stage-view.md). "No room" first (the
// default: distance and focus are kept on the tracks but change nothing in the sound), then the
// rooms. Each room sets the Room track's reverb (size, decay, filters, density), the walls the early
// reflections come from, how far back the back of the stage sounds and the level of a new room send.
// Saved by key, so the list can grow or be reordered without changing old projects.
#pragma once
#include <algorithm>
#include <string>
#include "I18n.h"

namespace gl::rooms {

struct Room {
  const char* key;    // saved in the project ("roomKind")
  const char* name;   // shown (English, marked for translation)
  const char* hint;   // one line: what it sounds like
  // The Room track's reverb (gl.reverb): model (0 hall, 1 plate), size 0..1, decay (s), low cut and
  // high cut (Hz), high damping (Hz), bass multiplier, diffusion 0..1, the reverb's own early
  // reflections 0..1 and modulation 0..1.
  int model;
  double size, decay, lowCut, highCut, dampHz, bass, diffusion, er, mod;
  // The stage in the room (engine/DepthStage.h earlyTaps): half the width and the depth to the back
  // wall (m), and how much of a reflection the walls give back.
  double halfWidth, depth, wall;
  float depthStrength;  // Song::stage.depthStrength the room brings (0 gentle .. 1 strong)
  double sendDb;        // a new room send's level
};

// Index 0 is "No room". The others are tuned so distance reads in each: the low cut keeps the room
// out of the bass (no mud), the damping and high cut keep the tail soft without losing presence.
inline constexpr Room kList[] = {
    {"none", i18n::N_("No space"), i18n::N_("Distance and focus are kept but not heard: pick a space to hear depth"), 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0, 6, 8, 0.5, 0.5f, -12},
    {"small", i18n::N_("Small room"), i18n::N_("Close and dry: short reflections from near walls"), 0, 0.18, 0.45, 220, 9000, 7000,
     0.8, 0.7, 0.5, 0.0, 3.5, 4.5, 0.55, 0.35f, -15},
    {"studio", i18n::N_("Studio"), i18n::N_("A live room: tight, clear and natural"), 0, 0.35, 0.8, 180, 10000, 8000, 0.85, 0.8,
     0.4, 0.0, 6, 8, 0.5, 0.5f, -12},
    {"club", i18n::N_("Club"), i18n::N_("A low room with a hard floor: punchy, short and a little dark"), 0, 0.42, 1.0, 250, 7000,
     5000, 0.7, 0.9, 0.45, 0.0, 8, 10, 0.6, 0.55f, -14},
    {"chamber", i18n::N_("Chamber"), i18n::N_("A tall bright room: dense and smooth, kind to strings and voices"), 0, 0.5, 1.4, 160,
     11000, 9000, 0.9, 1.0, 0.35, 0.1, 5, 10, 0.55, 0.5f, -13},
    {"hall", i18n::N_("Hall"), i18n::N_("A concert hall: warm and wide with a long even tail"), 0, 0.7, 2.2, 150, 8500, 6500, 1.0,
     1.0, 0.3, 0.2, 10, 16, 0.45, 0.65f, -12},
    {"bighall", i18n::N_("Big hall"), i18n::N_("A large hall: deep and spacious, the back of the stage far away"), 0, 0.88, 3.0, 160,
     7500, 5500, 0.95, 1.0, 0.25, 0.25, 14, 20, 0.4, 0.8f, -13},
    {"church", i18n::N_("Church"), i18n::N_("Stone and height: a long soft tail that blooms"), 0, 0.95, 4.2, 200, 6500, 4500, 0.85,
     1.0, 0.25, 0.3, 9, 24, 0.5, 0.85f, -15},
    {"plate", i18n::N_("Plate"), i18n::N_("A bright smooth plate with no walls, sitting behind the sound"), 1, 0.5, 1.6, 250, 12000,
     10000, 0.7, 1.0, 0.0, 0.0, 6, 8, 0.25, 0.45f, -15},
    {"open", i18n::N_("Open air"), i18n::N_("Outdoors: distance with almost no tail and only a faint echo"), 0, 0.12, 0.3, 300, 6000,
     4000, 0.6, 0.5, 0.2, 0.0, 40, 40, 0.15, 0.9f, -22},
};
inline constexpr int kCount = static_cast<int>(sizeof(kList) / sizeof(kList[0]));
inline constexpr int kNone = 0, kSmall = 1, kStudio = 2, kHall = 5, kBigHall = 6, kPlate = 8;

inline const Room& get(int type) { return kList[std::clamp(type, 0, kCount - 1)]; }
inline const char* key(int type) { return get(type).key; }
inline int ofKey(const std::string& k) {
  for (int i = 0; i < kCount; ++i)
    if (k == kList[i].key) return i;
  return kStudio;  // a room this build does not know: still a room
}
// The room types of an early Stage build (0 small room, 1 studio, 2 hall, 3 big hall, 4 plate).
inline int ofEarly(int v) {
  static constexpr int map[5] = {kSmall, kStudio, kHall, kBigHall, kPlate};
  return map[std::clamp(v, 0, 4)];
}

}  // namespace gl::rooms
