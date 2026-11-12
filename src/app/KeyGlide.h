// Gridline: an audition keyboard that follows the mouse. While the button is held, the key under the
// pointer sounds; sliding onto another key stops the one left behind and starts the new one, so a drag
// plays a glissando one key at a time, as the piano roll's keyboard does. Releasing stops it. Every
// on-screen keyboard of the built-in instruments' editors plays this way.
#pragma once
#include <cstdio>
#include <map>
#include <string>
#include "App.h"
#include "../../third_party/imgui/imgui.h"

namespace gl::keyglide {

// Call every frame after the keyboard's button. `active`: the button is held; `under`: the key under the
// pointer (-1 when it is off the keys: the last key keeps sounding); `held`: the widget's sounding key
// (-1 for none), kept by the caller. Plays only on a channel (`ch` != kNoId). True when a key started.
inline bool follow(App& a, ObjectId ch, bool active, int under, int& held, float velocity) {
  if (ch == kNoId) {
    held = -1;
    return false;
  }
  if (active && under >= 0 && under <= 127 && under != held) {
    if (held >= 0) a.engine.noteOff(ch, held);
    a.engine.noteOn(ch, under, velocity);
    held = under;
    return true;
  }
  if (!active && held >= 0) {
    a.engine.noteOff(ch, held);
    held = -1;
  }
  return false;
}

// The sounding key of a keyboard kind that several editors share (one static per kind): it belongs to
// the keyboard item that was pressed last, on that item's channel.
struct Held {
  int note = -1;
  ImGuiID item = 0;
  ObjectId ch = kNoId;
};

// follow() for the last ImGui item (the keyboard's button) and `ch` (kNoId: not a channel, silent).
// Pressing it takes `h` over (a key another keyboard still holds stops); the other keyboards sharing
// `h` leave it alone. Returns the key it sounds (-1 for none).
inline int followItem(App& a, ObjectId ch, int under, Held& h, float velocity) {
  const ImGuiID item = ImGui::GetItemID();
  const bool active = ImGui::IsItemActive();
  if (active && h.item != item) {
    if (h.note >= 0 && h.ch != kNoId) a.engine.noteOff(h.ch, h.note);
    h = Held{-1, item, ch};
  }
  if (h.item != item) return -1;
  follow(a, h.ch, active, under, h.note, velocity);
  return h.note;
}

// dev (`keybeds` in --dev-input): each keyboard drawn in the last frames, by name: its rectangle, lowest
// key, white key count and the key it sounds.
struct DevBed {
  float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  int lo = 0, whites = 0, held = -1, frame = 0;
};
inline std::map<std::string, DevBed>& devBeds() {
  static std::map<std::string, DevBed> m;
  return m;
}
inline void devNote(const std::string& name, ImVec2 p0, ImVec2 p1, int lo, int whites, int held) {
  devBeds()[name] = DevBed{p0.x, p0.y, p1.x, p1.y, lo, whites, held, ImGui::GetFrameCount()};
}
inline std::string devReport() {
  std::string out;
  const int now = ImGui::GetFrameCount();
  for (const auto& [name, b] : devBeds()) {
    if (now - b.frame > 2) continue;  // not drawn lately
    char line[256];
    std::snprintf(line, sizeof line, "keybed %s x0 %.1f y0 %.1f x1 %.1f y1 %.1f lo %d whites %d held %d\n", name.c_str(), static_cast<double>(b.x0),
                  static_cast<double>(b.y0), static_cast<double>(b.x1), static_cast<double>(b.y1), b.lo, b.whites, b.held);
    out += line;
  }
  return out;
}

}  // namespace gl::keyglide
