// Gridline — reference-panel helpers on top of the PluginSkin kit, shared by
// the spacefx / spectral / samplers pack editors (J-19). Positions are in
// reference form units (skin::Form scales them to the window).
#pragma once
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>
#include <vector>
#include "../../app/App.h"
#include "../../app/PluginSkin.h"
#include "../../app/PluginUI.h"
#include "../../app/Theme.h"
#include "../../core/Params.h"

namespace gl::packs::flskin {

using skin::Form;
using skin::Skin;

inline const ParamDesc* pdesc(App& a, const PluginTarget& t, int k) {
  const PluginInfo* info = t.info(a.doc.song());
  return info && k >= 0 && static_cast<size_t>(k) < info->params.size() ? &info->params[static_cast<size_t>(k)] : nullptr;
}
inline float pval(App& a, const PluginTarget& t, int k) {
  const PluginState* st = t.state(a.doc.song());
  return st && k >= 0 && static_cast<size_t>(k) < st->params.size() ? st->params[static_cast<size_t>(k)] : 0.f;
}
inline std::string pfmt(App& a, const PluginTarget& t, int k) {
  const ParamDesc* d = pdesc(a, t, k);
  return d ? d->format(pval(a, t, k)) : std::string();
}

// Small-caps label centred on form x at form y.
inline void cap(ImDrawList* dl, const Form& F, float cx, float y, const char* s, ImU32 col, float px = 7.5f) {
  skin::label(dl, F.p(cx, 0).x, F.p(0, y).y, s, col, F.font(px));
}
// Left-aligned label at form (x, y).
inline void text(ImDrawList* dl, const Form& F, float x, float y, const char* s, ImU32 col, float px = 7.5f) {
  skin::text(dl, F.p(x, y), col, s, F.font(px));
}
// Wheel with a label underneath (form units); `fill` overrides the value colour.
inline bool W(App& a, const PluginTarget& t, int k, const Form& F, float x, float y, float sz, const Skin& K, const char* lbl,
              int flags = skin::WheelBigLine, ImU32 fill = 0) {
  bool ch = skin::wheel(a, t, k, F.p(x, y), sz * F.s, K, flags, fill);
  if (lbl && *lbl) cap(ImGui::GetWindowDrawList(), F, x + sz * 0.5f, y + sz + 1.5f, lbl, K.textDim);
  return ch;
}
// Wheel with its live value in a pill above (reference "hint" labels) and a label underneath.
inline bool WV(App& a, const PluginTarget& t, int k, const Form& F, float x, float y, float sz, const Skin& K, const char* lbl,
               int flags = skin::WheelBigLine, ImU32 fill = 0) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  std::string v = pfmt(a, t, k);
  float pw = std::max(sz + 8, 46.0f);
  skin::valueStrip(dl, F.p(x + sz * 0.5f - pw * 0.5f, y - 17), F.p(x + sz * 0.5f + pw * 0.5f, y - 4), K, v.c_str(), F.font(7.5f));
  return W(a, t, k, F, x, y, sz, K, lbl, flags, fill);
}
// Vertical rail slider with a label underneath.
inline bool V(App& a, const PluginTarget& t, int k, const Form& F, float x, float y, float w, float h, const Skin& K, const char* lbl, ImU32 fill = 0,
              bool ruler = true) {
  bool ch = skin::vslider(a, t, k, F.p(x, y), F.sz(w, h), K, fill, ruler);
  if (lbl && *lbl) cap(ImGui::GetWindowDrawList(), F, x + w * 0.5f, y + h + 2, lbl, K.textDim);
  return ch;
}
// Same, with the value in a strip above.
inline bool VV(App& a, const PluginTarget& t, int k, const Form& F, float x, float y, float w, float h, const Skin& K, const char* lbl, ImU32 fill = 0) {
  std::string v = pfmt(a, t, k);
  float pw = std::max(w + 6, 40.0f);
  skin::valueStrip(ImGui::GetWindowDrawList(), F.p(x + w * 0.5f - pw * 0.5f, y - 17), F.p(x + w * 0.5f + pw * 0.5f, y - 4), K, v.c_str(),
                   F.font(7.5f));
  return V(a, t, k, F, x, y, w, h, K, lbl, fill);
}
// Switch selector over a choice parameter (labels default to the choice names).
inline bool sel(App& a, const PluginTarget& t, int k, const Form& F, float x, float y, float w, float h, const Skin& K, bool horizontal = false,
                const char* const* labels = nullptr) {
  const ParamDesc* d = pdesc(a, t, k);
  if (!d || d->choices.empty()) return false;
  std::vector<const char*> ls;
  for (size_t i = 0; i < d->choices.size(); ++i) ls.push_back(labels ? labels[i] : d->choices[i].c_str());
  return skin::switchSel(a, t, k, F.p(x, y), F.sz(w, h), K, ls.data(), static_cast<int>(ls.size()), horizontal);
}
inline bool chk(App& a, const PluginTarget& t, int k, const Form& F, float x, float y, float w, float h, const Skin& K, const char* s, ImU32 on = 0) {
  return skin::check(a, t, k, F.p(x, y), F.sz(w, h), K, s, on);
}
inline bool tog(App& a, const PluginTarget& t, int k, const Form& F, float x, float y, float w, float h, const Skin& K, const char* s) {
  return skin::toggle(a, t, k, F.p(x, y), F.sz(w, h), K, s, F.font(7.5f));
}
inline bool menu(App& a, const PluginTarget& t, int k, const Form& F, float x, float y, float w, float h, const Skin& K) {
  return skin::menuBox(a, t, k, F.p(x, y), F.sz(w, h), K, F.font(8.0f));
}
inline bool btn(const char* id, const Form& F, float x, float y, float w, float h, const Skin& K, const char* s, bool on) {
  return skin::button(id, F.p(x, y), F.sz(w, h), K, s, on, F.font(7.5f));
}
// Reference section: rounded-top caption strip over a bevel (title may be null).
inline void box(ImDrawList* dl, const Form& F, float x, float y, float w, float h, const Skin& K, const char* title, float capH = 16) {
  if (title && *title) {
    dl->AddRectFilled(F.p(x, y), F.p(x + w, y + capH), K.pillBgC(), 4 * F.s, ImDrawFlags_RoundCornersTop);
    skin::label(dl, F.p(x + w * 0.5f, 0).x, F.p(0, y + capH * 0.5f).y - F.font(8.0f) * 0.55f, title, K.pillTextC(), F.font(8.0f));
    skin::well(dl, F.p(x, y + capH), F.p(x + w, y + h), K, 3 * F.s);
  } else {
    skin::well(dl, F.p(x, y), F.p(x + w, y + h), K, 4 * F.s);
  }
}
// Moves the ImGui cursor to form (x, y) (for graph functions that draw at the cursor).
inline void at(const Form& F, float x, float y) { ImGui::SetCursorScreenPos(F.p(x, y)); }
// Dims a region (controls inactive in the current mode).
inline void dim(ImDrawList* dl, const Form& F, float x, float y, float w, float h, const Skin& K) {
  dl->AddRectFilled(F.p(x, y), F.p(x + w, y + h), theme::withAlpha(K.panel, 0.55f), 3 * F.s);
}

}  // namespace gl::packs::flskin
