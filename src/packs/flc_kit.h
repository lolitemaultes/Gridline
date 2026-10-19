// Gridline — small layout kit for the J-19 reference-family pack editors
// (dynamics / synths / models): a context binding the form grid, the skin and
// the plugin target, plus shorthand for the PluginSkin controls placed in form
// units (sections, labelled wheels and rail sliders, switches, menus, checks).
#pragma once
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>
#include "../app/App.h"
#include "../app/PluginSkin.h"
#include "../app/PluginUI.h"
#include "../app/Theme.h"

namespace gl::packs::flk {

struct Ctx {
  App& a;
  const PluginTarget& t;
  const skin::Form& F;
  const skin::Skin& K;
  ImDrawList* dl;
};

inline std::string upper(const char* s) {
  std::string u = s ? s : "";
  for (char& c : u) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return u;
}

// Small-caps text at form (x, y), left aligned.
inline void text(const Ctx& c, float x, float y, const char* s, ImU32 col = 0, float px = 8.0f) {
  skin::text(c.dl, c.F.p(x, y), col ? col : c.K.textDim, s, c.F.font(px));
}
// Small-caps text centred on form x.
inline void textC(const Ctx& c, float cx, float y, const char* s, ImU32 col = 0, float px = 8.0f) {
  skin::label(c.dl, c.F.p(cx, 0).x, c.F.p(0, y).y, s, col ? col : c.K.textDim, c.F.font(px));
}

// Section: well with a caption pill in its top-left corner.
inline void section(const Ctx& c, float x, float y, float w, float h, const char* title) {
  skin::well(c.dl, c.F.p(x, y), c.F.p(x + w, y + h), c.K, 4 * c.F.s);
  if (title && *title) {
    std::string u = upper(title);
    ImGui::PushFont(c.a.fontMonoBold, c.F.font(8.0f));
    float tw = ImGui::CalcTextSize(u.c_str()).x / c.F.s;
    ImGui::PopFont();
    skin::pill(c.dl, c.F.p(x + 3, y + 3), c.F.p(x + std::min(w - 3, tw + 15), y + 15), c.K, u.c_str(), c.F.font(8.0f));
  }
}

// Wheel (square `size`) with its label underneath; label turns into the value while hot.
inline bool wheel(const Ctx& c, int param, float x, float y, float size, const char* label, int flags = skin::WheelBigLine) {
  bool ch = skin::wheel(c.a, c.t, param, c.F.p(x, y), size * c.F.s, c.K, flags);
  bool hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
  std::string s = label ? upper(label) : std::string();
  if (hot) {
    auto d = describeParam(c.a.doc.song(), c.t.pid(param));
    auto v = getParam(c.a.doc.song(), c.t.pid(param));
    if (d && v) s = d->format(*v);
  }
  if (!s.empty()) skin::label(c.dl, c.F.p(x + size * 0.5f, 0).x, c.F.p(0, y + size + 1.5f).y, s.c_str(), hot ? c.K.text : c.K.textDim, c.F.font(7.5f));
  return ch;
}

// Vertical rail slider (w × h) with its label underneath (value while hot).
inline bool vsl(const Ctx& c, int param, float x, float y, float w, float h, const char* label, ImU32 fill = 0) {
  bool ch = skin::vslider(c.a, c.t, param, c.F.p(x, y), c.F.sz(w, h), c.K, fill, false);
  bool hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
  std::string s = label ? upper(label) : std::string();
  if (hot) {
    auto d = describeParam(c.a.doc.song(), c.t.pid(param));
    auto v = getParam(c.a.doc.song(), c.t.pid(param));
    if (d && v) s = d->format(*v);
  }
  if (!s.empty()) skin::label(c.dl, c.F.p(x + w * 0.5f, 0).x, c.F.p(0, y + h + 2).y, s.c_str(), hot ? c.K.text : c.K.textDim, c.F.font(7.5f));
  return ch;
}

inline bool hsl(const Ctx& c, int param, float x, float y, float w, float h) {
  return skin::hslider(c.a, c.t, param, c.F.p(x, y), c.F.sz(w, h), c.K);
}
inline bool sw(const Ctx& c, int param, float x, float y, float w, float h, const char* const* labels, int n, bool horizontal = false) {
  return skin::switchSel(c.a, c.t, param, c.F.p(x, y), c.F.sz(w, h), c.K, labels, n, horizontal);
}
inline bool menu(const Ctx& c, int param, float x, float y, float w, float h) {
  return skin::menuBox(c.a, c.t, param, c.F.p(x, y), c.F.sz(w, h), c.K, c.F.font(8.5f));
}
inline bool chk(const Ctx& c, int param, float x, float y, float w, float h, const char* s) {
  return skin::check(c.a, c.t, param, c.F.p(x, y), c.F.sz(w, h), c.K, s);
}
inline bool tog(const Ctx& c, int param, float x, float y, float w, float h, const char* s) {
  return skin::toggle(c.a, c.t, param, c.F.p(x, y), c.F.sz(w, h), c.K, s, c.F.font(8.0f));
}
inline bool btn(const Ctx& c, const char* id, float x, float y, float w, float h, const char* s, bool on) {
  return skin::button(id, c.F.p(x, y), c.F.sz(w, h), c.K, s, on, c.F.font(8.0f));
}
inline bool digi(const Ctx& c, int param, float x, float y, float w, float h, const char* s = nullptr) {
  return skin::digi(c.a, c.t, param, c.F.p(x, y), c.F.sz(w, h), c.K, s, c.F.font(8.5f));
}
inline void lcd(const Ctx& c, float x, float y, float w, float h, const char* s) {
  skin::lcd(c.dl, c.F.p(x, y), c.F.p(x + w, y + h), c.K, s, c.F.font(8.5f));
}
// Places the ImGui cursor at form (x, y) (for flow-layout widgets inside a form).
inline void cursor(const Ctx& c, float x, float y) { ImGui::SetCursorScreenPos(c.F.p(x, y)); }

}  // namespace gl::packs::flk
