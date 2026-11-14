// Gridline - the Stage's box selection (docs/stage-view.md, "Choosing tracks"). Dragging on the empty
// stage draws a box; the tracks it catches are chosen as it grows and shrinks. A plain drag replaces
// the choice, Shift adds to it, Ctrl (Cmd on a Mac) flips each caught track in or out of it.
#pragma once
#include <algorithm>
#include <vector>
#include "Types.h"

namespace gl::stagesel {

enum class BandMode { Replace, Add, Toggle };

// The box from two corners (any order).
struct Box {
  float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  static Box of(float ax, float ay, float bx, float by) { return {std::min(ax, bx), std::min(ay, by), std::max(ax, bx), std::max(ay, by)}; }
  bool contains(float x, float y) const { return x >= x0 && x <= x1 && y >= y0 && y <= y1; }
  // A token (centre, half size) is caught when the box touches it.
  bool touches(float cx, float cy, float half) const { return cx + half >= x0 && cx - half <= x1 && cy + half >= y0 && cy - half <= y1; }
  float width() const { return x1 - x0; }
  float height() const { return y1 - y0; }
};

// The choice while the box catches `caught`, from the choice `before` the drag began.
inline std::vector<ObjectId> applyBand(const std::vector<ObjectId>& before, const std::vector<ObjectId>& caught, BandMode mode) {
  auto has = [](const std::vector<ObjectId>& v, ObjectId id) { return std::find(v.begin(), v.end(), id) != v.end(); };
  std::vector<ObjectId> out;
  switch (mode) {
    case BandMode::Replace: out = caught; break;
    case BandMode::Add:
      out = before;
      for (ObjectId id : caught)
        if (!has(out, id)) out.push_back(id);
      break;
    case BandMode::Toggle:
      for (ObjectId id : before)
        if (!has(caught, id)) out.push_back(id);
      for (ObjectId id : caught)
        if (!has(before, id)) out.push_back(id);
      break;
  }
  return out;
}

// What a press on a token does to the choice: a chosen token keeps the whole choice (a group drag),
// an unchosen one becomes the only choice; Shift adds it, Ctrl/Cmd flips it.
inline std::vector<ObjectId> pressToken(const std::vector<ObjectId>& before, ObjectId id, BandMode mode) {
  const bool chosen = std::find(before.begin(), before.end(), id) != before.end();
  if (mode == BandMode::Replace) return chosen ? before : std::vector<ObjectId>{id};
  return applyBand(before, {id}, mode);
}

}  // namespace gl::stagesel
