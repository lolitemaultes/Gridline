// Gridline — "synths" pack, J-22 group "va": editor helpers on the instrument
// skin (iskin::) form canvas — captioned wheels / sliders / switches /
// selectors / digit wheels, section panels and the trance-gate step boxes.
#pragma once
#include <algorithm>
#include <functional>
#include <string>
#include "../../app/App.h"
#include "../../app/InstrumentSkin.h"
#include "../../app/PluginSkin.h"
#include "synths_widgets.h"

namespace gl::packs::synths::va {

inline void caption(const iskin::Skin& k, float x, float y, float w, const char* s, float size = 8) {
  if (s && *s) iskin::text(k, x, y, w, 11, s, size, k.c.label, 1);
}
inline void wheelCap(App& a, const PluginTarget& t, const iskin::Skin& k, int p, float x, float y, float s, ImU32 col, const char* cap,
                     bool centred = false) {
  iskin::wheel(a, t, k, p, x, y, s, col, centred ? iskin::WheelCentered : 0);
  caption(k, x - 10, y + s, s + 20, cap);
}
inline void sliderCap(App& a, const PluginTarget& t, const iskin::Skin& k, int p, float x, float y, float w, float h, ImU32 col,
                      const char* cap, bool centred = false) {
  iskin::slider(a, t, k, p, x, y, w, h, col, centred);
  caption(k, x - 8, y + h + 1, w + 16, cap);
}
// Square push switch (toggle param) with its caption under it.
inline void toggleCap(App& a, const PluginTarget& t, const iskin::Skin& k, int p, float x, float y, float w, float h, const char* cap) {
  const ImU32 lit = k.c.led, off = k.c.ledOff, edge = k.c.panelLine;
  iskin::switchBtn(a, t, k, p, x, y, w, h, [=](ImDrawList* dl, ImVec2 p0, ImVec2 p1, bool on) {
    dl->AddRectFilled(p0, p1, iskin::rgb(0x2A2A2A), 3);
    dl->AddRect(p0, p1, edge, 3);
    const float r = std::min(p1.x - p0.x, p1.y - p0.y) * 0.18f;
    dl->AddCircleFilled(ImVec2((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f), r, on ? lit : off);
  });
  caption(k, x - 8, y + h + 1, w + 16, cap);
}
// Saw / pulse shape switch (choice 0/1) drawn as the waveform glyph.
inline void shapeBtn(App& a, const PluginTarget& t, const iskin::Skin& k, int p, float x, float y, const char* cap) {
  const ImU32 lit = k.c.fill, dim = k.c.textDim;
  iskin::switchBtn(a, t, k, p, x, y, 31, 31, [=](ImDrawList* dl, ImVec2 p0, ImVec2 p1, bool pulse) {
    dl->AddRectFilled(p0, p1, iskin::rgb(0x2A2A2A), 3);
    const float w = p1.x - p0.x, h = p1.y - p0.y;
    const ImVec2 a0(p0.x + w * 0.2f, p0.y + h * 0.7f);
    const float x1 = p0.x + w * 0.5f, x2 = p0.x + w * 0.8f, top = p0.y + h * 0.3f, bot = p0.y + h * 0.7f;
    if (pulse) {
      const ImVec2 pts[6] = {a0, ImVec2(a0.x, top), ImVec2(x1, top), ImVec2(x1, bot), ImVec2(x2, bot), ImVec2(x2, top)};
      dl->AddPolyline(pts, 6, lit, 0, 1.6f);
    } else {
      const ImVec2 pts[5] = {a0, ImVec2(x1, top), ImVec2(x1, bot), ImVec2(x2, top), ImVec2(x2, bot)};
      dl->AddPolyline(pts, 5, lit, 0, 1.6f);
    }
    (void)dim;
  });
  caption(k, x - 8, y + 32, 47, cap);
}
// Shared-skin roles for selector wells (slate well instead of the LCD colour).
inline skin::Skin selSkin(const iskin::Skin& k) {
  skin::Skin s = k.shared();
  s.lcdBg = k.c.panel;
  s.lcdText = k.c.textDim;
  return s;
}
// Vertical (or horizontal) selector list over a choice param.
inline void selCap(App& a, const PluginTarget& t, const iskin::Skin& k, int p, float x, float y, float w, float h,
                   const char* const* labels, int n, bool horizontal = false) {
  skin::switchSel(a, t, p, k.at(x, y), ImVec2(w * k.s, h * k.s), selSkin(k), labels, n, horizontal);
}
inline void digiCap(App& a, const PluginTarget& t, const iskin::Skin& k, int p, float x, float y, float w, float h, const char* cap,
                    const std::function<std::string(double)>& fmt) {
  iskin::digi(a, t, k, p, x, y, w, h, fmt);
  if (cap && *cap) caption(k, x - 8, y + h + 1, w + 16, cap);
}
// Section panel with its title in the top-left corner.
inline void panel(const iskin::Skin& k, float x, float y, float w, float h, const char* title) {
  iskin::bevel(k, x, y, w, h, iskin::lerp(k.c.bgBottom, iskin::rgb(0x000000), 0.25f), k.c.panelLine, 4);
  if (title && *title) iskin::text(k, x + 6, y + 1, w - 12, 12, title, 8, k.c.textDim, 0, false, true);
}
// Trance-gate / sequencer step: lit box bound to a toggle param.
inline void stepBox(App& a, const PluginTarget& t, const iskin::Skin& k, int p, float x, float y, float w, float h) {
  const ImU32 lit = k.c.fill, off = iskin::rgb(0x343634), edge = k.c.panelLine;
  iskin::switchBtn(a, t, k, p, x, y, w, h, [=](ImDrawList* dl, ImVec2 p0, ImVec2 p1, bool on) {
    dl->AddRectFilled(p0, p1, on ? lit : off, 2);
    dl->AddRect(p0, p1, edge, 2);
  });
}
// Live output scope placed on the form.
inline void liveScopeAt(App& a, const PluginTarget& t, const iskin::Skin& k, float x, float y, float w, float h) {
  ImGui::SetCursorScreenPos(k.at(x, y));
  ui2::liveScope(a, t, ImVec2(w * k.s, h * k.s));
}
// Value of a "key value" line in PluginState::data ("" when absent).
inline std::string dataLine(const std::string& data, const char* key) {
  const std::string k = std::string(key) + " ";
  size_t at = 0;
  while (at < data.size()) {
    size_t nl = data.find('\n', at);
    if (nl == std::string::npos) nl = data.size();
    if (data.compare(at, k.size(), k) == 0) return data.substr(at + k.size(), nl - at - k.size());
    at = nl + 1;
  }
  return {};
}

void drawTwinVaEditor(App& a, const PluginTarget& t, float width);
void drawSawSwarmEditor(App& a, const PluginTarget& t, float width);
void drawFingerBassEditor(App& a, const PluginTarget& t, float width);

}  // namespace gl::packs::synths::va
