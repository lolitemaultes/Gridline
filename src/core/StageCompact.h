// Gridline - compact groups on the Stage (app/StageView.cpp, docs/stage-view.md "Groups"). On by default
// (the setting ui.stage.compactGroups; GRIDLINE_STAGE_COMPACT=0 or 1 overrides it): a group at rest is a
// small stacked chip at its spot, the size of one track's token, so the stage around and behind it stays
// free for clicks, drops and drags. It opens into its tray when it has the focus and closes up again
// when it loses it. Pure, no ImGui (tests/test_stage_compact.cpp):
//   - the chip on screen (its face, the cards stacked behind it, what answers the pointer);
//   - the focus rules: which group is open this frame, and how far each one is open (the animation).
#pragma once
#include <algorithm>
#include <cstddef>
#include <map>
#include <set>
#include <vector>
#include "StageMap.h"
#include "Types.h"

namespace gl::stage {

constexpr float kStackStep = 3.0f;  // px at scale 1: each card stacked behind the chip's face (up and right)
constexpr double kOpenTime = 0.14;  // seconds the tray takes to open or close
constexpr double kDwell = 0.25;     // seconds a dragged track rests on a closed group before it opens
constexpr double kGrace = 0.35;     // seconds a group stays open after a drag leaves it (or a tile is lifted out)

// A closed group's chip: its face is exactly the token a track on its own would have at that spot.
struct Chip {
  Rect face;    // the front card
  Rect hit;     // what answers the pointer: the face and the cards behind it
  int cards = 1;  // cards drawn behind the face (1 for two tracks, 2 for three or more)
};
inline Chip chipLayout(const Geo& g, double pan, double depth, int count) {
  Chip c;
  c.face = g.token(pan, depth);
  c.cards = count >= 3 ? 2 : 1;
  const float st = kStackStep * g.scale * static_cast<float>(c.cards);
  c.hit = {c.face.x0, c.face.y0 - st, c.face.x1 + st, c.face.y1};
  return c;
}
inline Chip shifted(Chip c, float dx, float dy) {
  c.face = {c.face.x0 + dx, c.face.y0 + dy, c.face.x1 + dx, c.face.y1 + dy};
  c.hit = {c.hit.x0 + dx, c.hit.y0 + dy, c.hit.x1 + dx, c.hit.y1 + dy};
  return c;
}

// One frame's view of the stage for the focus rules.
struct FocusIn {
  std::vector<std::vector<ObjectId>> groups;  // each group's members (this frame's groups)
  std::vector<ObjectId> selection;            // the chosen tracks
  bool dragging = false;                      // a track (or several) is being moved
  ObjectId dragOver = kNoId;  // a member of the group under the moving pointer (its chip, or its tray when open)
  ObjectId dragFrom = kNoId;  // a member of the group a tile was just lifted out of (the drag's first frame only)
  double dt = 0;              // seconds since the last frame
};

// Which group is open. At most one at a time:
//   - not dragging: the one whose chip was clicked (until the choice moves to a track outside it, a click
//     on the empty floor or Esc), else the one the choice moved into (every chosen track in it: a tile
//     clicked, Tab onto a member, a drop that joined it), else none (after a short grace when a drag just
//     left it). The choice a project opens with does not open anything: only a change of it does;
//   - dragging: the one under the pointer once it has rested there kDwell (at once when it is already
//     open), and for kGrace after the pointer leaves it. A tray a tile is lifted from stays open kGrace
//     and then closes even under the pointer, so the tile can be put down behind the group; resting on
//     its chip kDwell opens it again (to put the tile back).
class GroupFocus {
 public:
  void pin(ObjectId member) { pinned_ = member, hold_ = kNoId; }
  void close() { pinned_ = kNoId, auto_ = kNoId, hold_ = kNoId, over_ = kNoId, source_ = kNoId, holdUntil_ = 0; }
  ObjectId pinned() const { return pinned_; }

  // This frame's open group (an index into in.groups, -1 none); openness(i) after it.
  int update(const FocusIn& in) {
    clock_ += std::max(0.0, in.dt);
    auto groupOf = [&](ObjectId id) {
      if (id == kNoId) return -1;
      for (size_t i = 0; i < in.groups.size(); ++i)
        if (std::find(in.groups[i].begin(), in.groups[i].end(), id) != in.groups[i].end()) return static_cast<int>(i);
      return -1;
    };
    auto holds = [&](int gi, const std::vector<ObjectId>& ids) {
      if (gi < 0 || ids.empty()) return false;
      for (ObjectId id : ids)
        if (std::find(in.groups[static_cast<size_t>(gi)].begin(), in.groups[static_cast<size_t>(gi)].end(), id) == in.groups[static_cast<size_t>(gi)].end())
          return false;
      return true;
    };
    // The pin goes when its track is no longer in a group (automation parted it) or the choice moves
    // to tracks outside it.
    if (pinned_ != kNoId && groupOf(pinned_) < 0) pinned_ = kNoId;
    if (auto_ != kNoId && groupOf(auto_) < 0) auto_ = kNoId;
    const std::set<ObjectId> sel(in.selection.begin(), in.selection.end());
    const bool dropped = wasDragging_ && !in.dragging;
    wasDragging_ = in.dragging;
    if ((sel != lastSel_ || dropped) && started_) {
      if (pinned_ != kNoId && !in.selection.empty() && !holds(groupOf(pinned_), in.selection)) pinned_ = kNoId;
      const int g = in.selection.empty() ? -1 : groupOf(in.selection.front());
      auto_ = holds(g, in.selection) ? in.selection.front() : kNoId;
    }
    lastSel_ = sel;
    started_ = true;
    int open = -1;
    if (in.dragging) {
      if (in.dragFrom != kNoId) hold_ = in.dragFrom, holdUntil_ = clock_ + kGrace, source_ = in.dragFrom;
      if (in.dragOver != over_) over_ = in.dragOver, overSince_ = clock_;
      const int g = groupOf(over_);
      const bool rested = clock_ - overSince_ >= kDwell - 1e-9;
      if (g >= 0 && g == groupOf(source_)) {  // the tray the tile came from: only its grace, or a rest on its chip
        if (g != open_ && rested) open = g, source_ = kNoId, hold_ = over_, holdUntil_ = clock_ + kGrace;
      } else if (g >= 0 && (g == open_ || rested)) {
        open = g;
        hold_ = over_, holdUntil_ = clock_ + kGrace;
      }
      if (open < 0 && clock_ < holdUntil_) open = groupOf(hold_);
    } else {
      over_ = kNoId, source_ = kNoId;
      if (pinned_ != kNoId) open = groupOf(pinned_);
      if (open < 0 && auto_ != kNoId) open = groupOf(auto_);
      if (open < 0 && clock_ < holdUntil_) open = groupOf(hold_);
    }
    if (open_ >= 0 && open != open_) overSince_ = clock_;  // a group just closed (or another opened): the rest starts again
    open_ = open;
    // The animation: each track carries how far its group is open, so a group keeps its value when its
    // members change (a tile lifted out, another joining).
    std::map<ObjectId, float> next;
    amount_.assign(in.groups.size(), 0.0f);
    const float step = kOpenTime > 0 ? static_cast<float>(std::max(0.0, in.dt) / kOpenTime) : 1.0f;
    for (size_t i = 0; i < in.groups.size(); ++i) {
      float cur = 0;
      for (ObjectId id : in.groups[i])
        if (auto it = anim_.find(id); it != anim_.end()) cur = std::max(cur, it->second);
      const float target = static_cast<int>(i) == open ? 1.0f : 0.0f;
      cur = target > cur ? std::min(target, cur + step) : std::max(target, cur - step);
      amount_[i] = cur;
      for (ObjectId id : in.groups[i]) next[id] = cur;
    }
    anim_ = std::move(next);
    return open;
  }
  int open() const { return open_; }
  // How far group i (of the last update) is open: 0 the chip .. 1 the whole tray.
  float openness(size_t i) const { return i < amount_.size() ? amount_[i] : 0.0f; }
  double clock() const { return clock_; }

 private:
  ObjectId pinned_ = kNoId, auto_ = kNoId, hold_ = kNoId, over_ = kNoId, source_ = kNoId;
  bool started_ = false, wasDragging_ = false;
  double clock_ = 0, holdUntil_ = 0, overSince_ = 0;
  int open_ = -1;
  std::set<ObjectId> lastSel_;
  std::map<ObjectId, float> anim_;
  std::vector<float> amount_;
};

// The ease of the open / close animation (0 .. 1 in, 0 .. 1 out): fast at first, settling softly.
inline float openEase(float t) {
  t = std::clamp(t, 0.0f, 1.0f);
  return 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
}
// A rectangle between a and b (t 0: a, 1: b).
inline Rect lerpRect(const Rect& a, const Rect& b, float t) {
  return {a.x0 + (b.x0 - a.x0) * t, a.y0 + (b.y0 - a.y0) * t, a.x1 + (b.x1 - a.x1) * t, a.y1 + (b.y1 - a.y1) * t};
}

}  // namespace gl::stage
