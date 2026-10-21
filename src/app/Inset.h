// Gridline — minimum inset between a control and the panel it sits on (Q-7).
// Pure geometry (no ImGui), shared by the editors, the layout audit
// (LayoutAudit.h, --layout-audit) and the tests.
//
//  * fit() shrinks a control's rectangle so it keeps at least `min` px from
//    every edge of its frame (sides that already clear it stay put): editors
//    place controls with it instead of nudging coordinates one by one.
//  * check() finds, for every control, the innermost frame holding its centre
//    and reports controls closer than `min` px to an edge of that frame (or
//    sticking out of it).
#pragma once
#include <algorithm>
#include <cstddef>
#include <vector>

namespace gl::inset {

// Least clear space (logical px, scale 1) between a control and its panel's edge.
constexpr float kMinInset = 3.0f;
// Same in reference-form units for skin forms: scales with the form (never below kMinInset).
constexpr float kMinInsetUnits = 4.0f;
inline float minFor(float formScale) { return std::max(kMinInset, kMinInsetUnits * formScale); }

struct Rect {
  float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  float w() const { return x1 - x0; }
  float h() const { return y1 - y0; }
  float area() const { return std::max(0.0f, w()) * std::max(0.0f, h()); }
  bool contains(float x, float y) const { return x >= x0 && x <= x1 && y >= y0 && y <= y1; }
};

// The frame shrunk by `min` on every side.
inline Rect inner(const Rect& frame, float min) { return Rect{frame.x0 + min, frame.y0 + min, frame.x1 - min, frame.y1 - min}; }

// `r` shrunk (never moved past its other edge) so it keeps `min` px from every
// edge of `frame`; sides that already clear it are unchanged.
inline Rect fit(const Rect& frame, const Rect& r, float min = kMinInset) {
  const Rect in = inner(frame, min);
  Rect o = r;
  o.x0 = std::max(o.x0, in.x0);
  o.y0 = std::max(o.y0, in.y0);
  o.x1 = std::min(o.x1, in.x1);
  o.y1 = std::min(o.y1, in.y1);
  if (o.x1 < o.x0) o.x1 = o.x0;
  if (o.y1 < o.y0) o.y1 = o.y0;
  return o;
}

// `r` moved (not resized) so it keeps `min` px from every edge of `frame`;
// on an axis where it is larger than the room left it is shrunk instead.
inline Rect place(const Rect& frame, const Rect& r, float min = kMinInset) {
  const Rect in = inner(frame, min);
  Rect o = r;
  auto axis = [](float& a0, float& a1, float lo, float hi) {
    const float len = a1 - a0;
    if (len > hi - lo) {
      a0 = std::max(a0, lo), a1 = std::min(a1, hi);
      if (a1 < a0) a1 = a0;
      return;
    }
    if (a0 < lo) a0 = lo, a1 = lo + len;
    if (a1 > hi) a1 = hi, a0 = hi - len;
  };
  axis(o.x0, o.x1, in.x0, in.x1);
  axis(o.y0, o.y1, in.y0, in.y1);
  return o;
}

// Smallest gap between `r` and the edges of `frame` (negative: r sticks out).
// `side` gets 0 left, 1 top, 2 right, 3 bottom.
inline float gap(const Rect& frame, const Rect& r, int* side = nullptr) {
  const float g[4] = {r.x0 - frame.x0, r.y0 - frame.y0, frame.x1 - r.x1, frame.y1 - r.y1};
  int k = 0;
  for (int i = 1; i < 4; ++i)
    if (g[i] < g[k]) k = i;
  if (side) *side = k;
  return g[k];
}

struct Issue {
  size_t control = 0;  // index into the controls
  size_t frame = 0;    // index into the frames
  float gap = 0;       // px (negative: outside)
  int side = 0;        // 0 left, 1 top, 2 right, 3 bottom
};

// Frames are panels, wells, graph boxes, sections; controls are interactive
// items and labels. A frame that a control (nearly) fills is the control's own
// box (a graph or pad), not its panel: frames must be clearly larger than the
// control (area > 1.2x) to count.
inline std::vector<Issue> check(const std::vector<Rect>& frames, const std::vector<Rect>& controls, float min = kMinInset) {
  std::vector<Issue> out;
  for (size_t c = 0; c < controls.size(); ++c) {
    const Rect& r = controls[c];
    if (r.w() <= 0 || r.h() <= 0) continue;
    const float cx = (r.x0 + r.x1) * 0.5f, cy = (r.y0 + r.y1) * 0.5f;
    size_t best = frames.size();
    for (size_t f = 0; f < frames.size(); ++f) {
      const Rect& fr = frames[f];
      if (!fr.contains(cx, cy) || fr.area() <= r.area() * 1.2f) continue;
      if (fr.w() < r.w() - 0.5f || fr.h() < r.h() - 0.5f) continue;  // narrower than the control: an overlay, not its panel
      if (best == frames.size() || fr.area() < frames[best].area()) best = f;
    }
    if (best == frames.size()) continue;
    int side = 0;
    const float g = gap(frames[best], r, &side);
    if (g < min - 0.01f) out.push_back(Issue{c, best, g, side});
  }
  return out;
}

}  // namespace gl::inset
