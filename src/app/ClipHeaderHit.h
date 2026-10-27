// Gridline: J-142, what a click on a playlist clip hits. A clip is a title
// strip (header) over its body. In the header, the little menu arrow and the
// name box (the name's text as drawn, a few pixels of padding around it) open
// the clip's menu on a right-click; the rest of the header behaves like the
// body (a right-click or right-drag deletes). Pure geometry, no ImGui: the
// playlist fills a Geom per drawn clip, tests/test_clip_header_hit.cpp checks it.
#pragma once
#include <algorithm>

namespace gl::clipheader {

enum class Zone { None, Body, Header, Name, Menu };

// Padding (px) around the name's text that still counts as the name box.
constexpr float kNamePad = 3.0f;
// Half width (px) of the menu arrow's hit span around its centre.
constexpr float kMenuHalf = 7.0f;

struct Geom {
  float x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // the clip as drawn
  float headH = 0;                       // height of the title strip
  bool hasMenu = false;                  // the menu arrow is drawn
  float menuX0 = 0, menuX1 = 0;
  bool hasName = false;                  // the name is drawn
  float nameX0 = 0, nameX1 = 0;

  // The arrow drawn centred at `cx`.
  void setMenu(float cx) { hasMenu = true, menuX0 = cx - kMenuHalf, menuX1 = cx + kMenuHalf; }
  // The name drawn from `textX`, `textW` wide (the full text width) in at most
  // `maxW` (it is cut with an ellipsis there). No room: no name box.
  void setName(float textX, float textW, float maxW) {
    const float w = std::min(textW, maxW);
    hasName = w > 0.5f;
    if (!hasName) return;
    nameX0 = textX - kNamePad, nameX1 = textX + w + kNamePad;
    if (hasMenu) nameX0 = std::max(nameX0, menuX1);  // the arrow keeps its own span
    nameX1 = std::min(nameX1, x1);
  }
};

inline Zone hit(const Geom& g, float mx, float my) {
  if (mx < g.x0 || mx >= g.x1 || my < g.y0 || my >= g.y1) return Zone::None;
  if (my >= g.y0 + g.headH) return Zone::Body;
  if (g.hasMenu && mx >= g.menuX0 && mx < g.menuX1) return Zone::Menu;
  if (g.hasName && mx >= g.nameX0 && mx < g.nameX1) return Zone::Name;
  return Zone::Header;
}

// A right-click here opens the clip's menu (else it deletes, like the body).
inline bool opensMenu(Zone z) { return z == Zone::Name || z == Zone::Menu; }

}  // namespace gl::clipheader
