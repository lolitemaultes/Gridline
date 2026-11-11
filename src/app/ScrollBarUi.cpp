// Gridline: the slim view scrollers of the timeline editors.
#include "ScrollBarUi.h"

#include <algorithm>
#include <cmath>

#include "Theme.h"

namespace gl::scrollbarui {

using theme::pal;

float Layout::toRail(float contentPx) const {
  const float k = railLen() / std::max(1.0f, content);
  return (vertical ? p0.y : p0.x) + contentPx * k;
}

Layout layout(ImVec2 p0, ImVec2 p1, bool vertical, float view, float content, float scroll, float minThumb) {
  Layout L;
  L.vertical = vertical;
  L.p0 = p0, L.p1 = p1;
  L.view = std::max(1.0f, view);
  L.content = std::max(L.view, content);
  const float rail = std::max(1.0f, L.railLen());
  const float len = std::min(rail, std::max(minThumb, rail * std::min(1.0f, L.view / L.content)));
  const float start = vertical ? p0.y : p0.x;
  const float pos = L.content > L.view ? (rail - len) * std::clamp(scroll / (L.content - L.view), 0.0f, 1.0f) : 0.0f;
  L.t0 = start + pos, L.t1 = L.t0 + len;
  L.cap = std::min(vertical ? 7.0f : 8.0f, len / 3.0f);
  return L;
}

Part hit(const Layout& L, ImVec2 m) {
  // The vertical rail takes the 2 px gap beside it too (as the playlist's).
  const bool in = L.vertical ? m.x >= L.p0.x && m.x < L.p1.x + 2 && m.y >= L.p0.y && m.y < L.p1.y
                             : m.x >= L.p0.x && m.x < L.p1.x && m.y >= L.p0.y && m.y < L.p1.y;
  if (!in) return Part::None;
  const float v = L.along(m);
  if (v >= L.t0 && v < L.t0 + L.cap) return Part::LowEnd;
  if (v >= L.t1 - L.cap && v < L.t1) return Part::HighEnd;
  if (v >= L.t0 && v < L.t1) return Part::Thumb;
  return Part::Rail;
}

int press(Drag& d, const Layout& L, Part part, ImVec2 m, double zoom, float scroll) {
  const float v = L.along(m);
  d = Drag{};
  d.vertical = L.vertical;
  if (part == Part::Thumb) {
    d.part = part;
    d.grab = v - L.t0;
    return 0;
  }
  if (part == Part::LowEnd || part == Part::HighEnd) {
    const bool high = part == Part::HighEnd;
    d.part = part;
    d.grab = v - (high ? L.t1 : L.t0);
    d.fixed = high ? L.t0 : L.t1;
    d.len0 = L.t1 - L.t0;
    d.z0 = zoom;
    d.edgePx = high ? 0.0f : L.view;  // dragging the high end holds the view's start, the low end its end
    d.edgeUnits = (static_cast<double>(d.edgePx) + scroll) / std::max(1e-9, zoom);
    return 0;
  }
  if (part == Part::Rail) return v < L.t0 ? -1 : 1;
  return 0;
}

float thumbScroll(const Drag& d, const Layout& L, ImVec2 m) {
  if (L.content <= L.view) return 0.0f;
  const float start = L.vertical ? L.p0.y : L.p0.x;
  const float t = (L.along(m) - d.grab - start) / std::max(1.0f, L.railLen() - (L.t1 - L.t0));
  return std::clamp(t, 0.0f, 1.0f) * (L.content - L.view);
}

double endZoom(const Drag& d, ImVec2 m, float minThumb, float maxLen) {
  if (!d.zooming()) return d.z0;
  const float end = (d.vertical ? m.y : m.x) - d.grab;
  const float len = std::clamp(d.part == Part::HighEnd ? end - d.fixed : d.fixed - end, minThumb, std::max(minThumb, maxLen));
  return d.z0 * static_cast<double>(d.len0) / static_cast<double>(len);
}

void draw(ImDrawList* dl, const Layout& L, bool hot, Part lit) {
  const auto& P = pal();
  const ImU32 rail = P.dark ? P.well : P.surface950, thumb = hot ? P.surface600 : P.surface700;
  const ImU32 grip = P.dark ? P.textFaint : P.textDim;
  if (L.vertical) {
    dl->AddRectFilled(L.p0, ImVec2(L.p1.x + 2, L.p1.y), rail);
    const float x0 = L.p0.x + 2, x1 = L.p1.x;
    dl->AddRectFilled(ImVec2(x0, L.t0), ImVec2(x1, L.t1), thumb, 3);
    if (!hot) return;
    if (lit == Part::LowEnd) dl->AddRectFilled(ImVec2(x0, L.t0), ImVec2(x1, L.t0 + L.cap), P.hot, 3, ImDrawFlags_RoundCornersTop);
    else dl->AddLine(ImVec2(x0 + 1, L.t0 + L.cap - 0.5f), ImVec2(x1 - 1, L.t0 + L.cap - 0.5f), grip, 1.0f);
    if (lit == Part::HighEnd) dl->AddRectFilled(ImVec2(x0, L.t1 - L.cap), ImVec2(x1, L.t1), P.hot, 3, ImDrawFlags_RoundCornersBottom);
    else dl->AddLine(ImVec2(x0 + 1, L.t1 - L.cap + 0.5f), ImVec2(x1 - 1, L.t1 - L.cap + 0.5f), grip, 1.0f);
  } else {
    dl->AddRectFilled(ImVec2(L.p0.x, L.p0.y - 1), ImVec2(L.p1.x, L.p1.y + 1), rail);
    const float y0 = L.p0.y + 2, y1 = L.p1.y - 1;
    dl->AddRectFilled(ImVec2(L.t0, y0), ImVec2(L.t1, y1), thumb, 3);
    if (!hot) return;
    if (lit == Part::LowEnd) dl->AddRectFilled(ImVec2(L.t0, y0), ImVec2(L.t0 + L.cap, y1), P.hot, 3, ImDrawFlags_RoundCornersLeft);
    else dl->AddLine(ImVec2(L.t0 + L.cap - 0.5f, y0 + 2), ImVec2(L.t0 + L.cap - 0.5f, y1 - 2), grip, 1.0f);
    if (lit == Part::HighEnd) dl->AddRectFilled(ImVec2(L.t1 - L.cap, y0), ImVec2(L.t1, y1), P.hot, 3, ImDrawFlags_RoundCornersRight);
    else dl->AddLine(ImVec2(L.t1 - L.cap + 0.5f, y0 + 2), ImVec2(L.t1 - L.cap + 0.5f, y1 - 2), grip, 1.0f);
  }
}

}  // namespace gl::scrollbarui
