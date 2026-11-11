// Gridline: the slim view scrollers of the timeline editors (piano roll,
// automation clip editor), in the playlist's look: a dark rail, a rounded
// thumb, grip lines at the thumb's ends while hovered. Drag the thumb to
// scroll, click the rail to page, drag an end to zoom that axis (the other
// end stays put). The view keeps a Drag per scroller, lays the bar out each
// frame, and applies the scroll and zoom through its own smooth scroll and
// zoom code; overviews (notes, points) are drawn by the view over the rail
// with toRail().
#pragma once
#include "../../third_party/imgui/imgui.h"

namespace gl::scrollbarui {

// The playlist's sizes: the horizontal rail's height, the vertical rail's
// width, and the thumbs' shortest lengths (their ends are zoom handles).
constexpr float kBarH = 14.0f, kBarW = 10.0f;
constexpr float kThumbMinH = 24.0f, kThumbMinV = 18.0f;

enum class Part { None, Rail, Thumb, LowEnd, HighEnd };

struct Layout {
  bool vertical = false;
  ImVec2 p0, p1;               // the rail
  float t0 = 0, t1 = 0;        // the thumb's start and end along the axis (screen)
  float cap = 0;               // the end handles' length
  float view = 0, content = 1; // px shown, px in all (at this zoom)
  float railLen() const { return vertical ? p1.y - p0.y : p1.x - p0.x; }
  float along(ImVec2 m) const { return vertical ? m.y : m.x; }
  // Content px -> the rail's axis (screen), for overviews.
  float toRail(float contentPx) const;
};

// `scroll` may overscroll (an elastic edge): the thumb stays in its rail.
Layout layout(ImVec2 p0, ImVec2 p1, bool vertical, float view, float content, float scroll, float minThumb);
// The part under `m` (None outside the rail).
Part hit(const Layout& L, ImVec2 m);

struct Drag {
  Part part = Part::None;  // Thumb, LowEnd or HighEnd while held
  bool vertical = false;
  float grab = 0;          // the pointer's offset into the thumb (or from the grabbed end)
  float fixed = 0;         // ends: the screen position of the end that stays put
  float len0 = 1;          // ends: the thumb's length at the grab
  double z0 = 1;           // ends: the zoom at the grab
  float edgePx = 0;        // ends: the held view edge, from the view's start (0 or the view's size)
  double edgeUnits = 0;    // ends: the content (in zoom units) at that edge
  bool held() const { return part != Part::None; }
  bool zooming() const { return part == Part::LowEnd || part == Part::HighEnd; }
};

// A left press on `part` at `m`: starts a thumb or end drag (returns 0), or
// returns the rail click's page direction (-1 towards the start, 1 towards the end).
int press(Drag& d, const Layout& L, Part part, ImVec2 m, double zoom, float scroll);
// The scroll for the pointer while the thumb is held.
float thumbScroll(const Drag& d, const Layout& L, ImVec2 m);
// The zoom for the pointer while an end is held (before the view's own clamp):
// the thumb's new length sets it. The view then scrolls so that edgeUnits stays
// at edgePx: scroll = edgeUnits * zoom - edgePx.
double endZoom(const Drag& d, ImVec2 m, float minThumb, float maxLen);

// The rail and the thumb; the grips while `hot` (hovered or held), the end `lit` filled.
void draw(ImDrawList* dl, const Layout& L, bool hot, Part lit);

}  // namespace gl::scrollbarui
