// Gridline — rack skin: per-plugin palettes and vector control styles.
#include "../core/I18n.h"
#include "PluginSkin.h"
#include <functional>
#include <cstring>
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>
#include "../core/Params.h"
#include "App.h"
#include "Theme.h"

namespace gl::skin {

namespace {

constexpr float kPi = 3.14159265f;

ImU32 lerpCol(ImU32 a, ImU32 b, float t) { return theme::mix(a, b, std::clamp(t, 0.0f, 1.0f)); }

void arc(ImDrawList* dl, ImVec2 c, float r, float a0, float a1, ImU32 col, float th) {
  if (a1 <= a0) return;
  int segs = std::max(6, static_cast<int>((a1 - a0) * r * 0.4f));
  dl->PathArcTo(c, r, a0, a1, segs);
  dl->PathStroke(col, 0, th);
}

struct Bound {
  ParamId pid;
  ParamDesc d;
  double val = 0;
  bool ok = false;
};

Bound bind(App& a, const PluginTarget& t, int param) {
  Bound b;
  const Song& s = a.doc.song();
  b.pid = t.pid(param);
  auto d = describeParam(s, b.pid);
  auto v = getParam(s, b.pid);
  if (!d || !v) return b;
  b.d = *d;
  b.val = *v;
  b.ok = true;
  return b;
}

}  // namespace

ImU32 hex(uint32_t c, float alpha) { return theme::rgb(c, alpha); }
ImU32 lighten(ImU32 c, float t) { return lerpCol(c, IM_COL32(255, 255, 255, (c >> IM_COL32_A_SHIFT) & 0xFF), t); }
ImU32 darken(ImU32 c, float t) { return lerpCol(c, IM_COL32(0, 0, 0, (c >> IM_COL32_A_SHIFT) & 0xFF), t); }

Skin Skin::make(uint32_t panelC, uint32_t railC, uint32_t fillC, uint32_t fillPressedC, uint32_t capC, uint32_t textC) {
  Skin k;
  k.panel = hex(panelC);
  k.panelHi = lighten(k.panel, 0.06f);
  k.panelLo = darken(k.panel, 0.22f);
  k.bevel = darken(k.panel, 0.32f);
  k.rail = hex(railC);
  k.fill = hex(fillC);
  k.fillPressed = hex(fillPressedC);
  k.cap = hex(capC);
  k.handle = lighten(k.panel, 0.35f);
  k.text = hex(textC);
  k.textDim = lerpCol(k.text, k.panel, 0.45f);
  k.lcdBg = darken(k.panel, 0.62f);
  k.lcdText = lighten(k.fill, 0.25f);
  k.graphBg = darken(k.panel, 0.55f);
  k.graphGrid = lerpCol(k.graphBg, k.text, 0.10f);
  k.graphGridStrong = lerpCol(k.graphBg, k.text, 0.22f);
  k.graphText = lerpCol(k.graphBg, k.text, 0.45f);
  k.graphCurve = lighten(k.fill, 0.15f);
  k.select = k.fill;
  return k;
}

Skin Skin::fl(uint32_t panelC, uint32_t wellC_, uint32_t railC, uint32_t fillC, uint32_t capC, uint32_t textC) {
  Skin k = make(panelC, railC, fillC, fillC, capC, textC);
  k.fillPressed = lighten(k.fill, 0.18f);
  k.well = hex(wellC_);
  k.pillBg = lighten(k.well, 0.07f);
  k.pillText = k.text;
  k.valueBg = lighten(k.well, 0.22f);
  k.valueText = k.fill;
  k.handle = lighten(k.well, 0.28f);
  return k;
}

ImU32 Skin::wellC() const { return well ? well : theme::withAlpha(bevel, 0.55f); }
ImU32 Skin::pillBgC() const { return pillBg ? pillBg : lighten(panel, 0.08f); }
ImU32 Skin::pillTextC() const { return pillText ? pillText : text; }
ImU32 Skin::valueBgC() const { return valueBg ? valueBg : lcdBg; }
ImU32 Skin::valueTextC() const { return valueText ? valueText : lcdText; }
ImU32 Skin::accent2C() const { return accent2 ? accent2 : lerpCol(fill, text, 0.5f); }

Form Form::fit(float width, float formW) { return Form{ImGui::GetCursorScreenPos(), formW > 0 ? width / formW : 1.0f}; }
float Form::font(float px) const { return std::clamp(px * s, 9.0f, 15.0f); }
void Form::end(float formW, float formH) const {
  ImGui::SetCursorScreenPos(o);
  ImGui::Dummy(ImVec2(formW * s, formH * s));
}

void panel(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const Skin& k, float rounding) {
  dl->AddRectFilledMultiColor(p0, p1, k.panelHi, k.panelHi, k.panel, k.panel);
  dl->AddRect(p0, p1, darken(k.panel, 0.45f), rounding, 0, 1.0f);
}

void bevel(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const Skin& k, float rounding) {
  dl->AddRectFilled(p0, p1, theme::withAlpha(k.bevel, 0.55f), rounding);
  dl->AddRect(p0, p1, darken(k.panel, 0.5f), rounding, 0, 1.0f);
  dl->AddLine(ImVec2(p0.x + rounding, p1.y), ImVec2(p1.x - rounding, p1.y), lighten(k.panel, 0.12f), 1.0f);
}

void label(ImDrawList* dl, float cx, float y, const char* text, ImU32 col, float size) {
  ImGui::PushFont(app().fontMonoBold, size);
  ImVec2 ts = ImGui::CalcTextSize(text);
  dl->AddText(ImVec2(std::round(cx - ts.x * 0.5f), std::round(y)), col, text);
  ImGui::PopFont();
}

void lcd(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const Skin& k, const char* text, float size, bool center) {
  dl->AddRectFilled(p0, p1, k.lcdBg, 2);
  dl->AddRect(p0, p1, darken(k.panel, 0.7f), 2, 0, 1.0f);
  ImGui::PushFont(app().fontMono, size);
  ImVec2 ts = ImGui::CalcTextSize(text);
  float x = center ? (p0.x + p1.x - ts.x) * 0.5f : p0.x + 4;
  dl->PushClipRect(p0, p1, true);
  dl->AddText(ImVec2(std::round(x), std::round((p0.y + p1.y - ts.y) * 0.5f)), k.lcdText, text);
  dl->PopClipRect();
  ImGui::PopFont();
}

void graphWell(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const Skin& k) {
  dl->AddRectFilled(p0, p1, k.graphBg, 2);
  dl->AddRect(ImVec2(p0.x - 1, p0.y - 1), ImVec2(p1.x + 1, p1.y + 1), darken(k.panel, 0.65f), 2, 0, 1.0f);
}

void drawWheel(ImDrawList* dl, ImVec2 c, float r, double norm, bool centered, bool bigLine, bool hot, const Skin& k, ImU32 fill) {
  // Reference style: a dark rail ring around a flat cap; the value fills the
  // ring (from the start, or from the top for centred knobs) and an indicator
  // line runs from the cap centre towards the value.
  const float a0 = kPi * 0.75f, a1 = kPi * 2.25f;
  float an = a0 + static_cast<float>(std::clamp(norm, 0.0, 1.0)) * (a1 - a0);
  float th = std::max(2.0f, r * 0.24f);
  float rr = r - th * 0.5f;
  dl->AddCircleFilled(ImVec2(c.x, c.y + r * 0.06f), r + 0.5f, theme::withAlpha(darken(k.panel, 0.8f), 0.45f), 32);
  arc(dl, c, rr, a0, a1, k.rail, th);
  float from = centered ? kPi * 1.5f : a0;
  ImU32 f = hot ? lighten(fill, 0.18f) : fill;
  if (std::fabs(an - from) > 0.003f) arc(dl, c, rr, std::min(from, an), std::max(from, an), f, th);
  float capR = r - th - 1.0f;
  dl->AddCircleFilled(c, capR, k.cap, 32);
  dl->AddCircle(c, capR, lighten(k.cap, 0.12f), 32, 1.0f);
  ImVec2 dir(std::cos(an), std::sin(an));
  float lw = bigLine ? std::max(2.0f, r * 0.17f) : std::max(1.4f, r * 0.10f);
  ImU32 lc = lighten(k.text, hot ? 0.4f : 0.15f);
  dl->AddLine(ImVec2(c.x + dir.x * capR * (bigLine ? 0.15f : 0.35f), c.y + dir.y * capR * (bigLine ? 0.15f : 0.35f)),
              ImVec2(c.x + dir.x * (capR - 0.5f), c.y + dir.y * (capR - 0.5f)), lc, lw);
}

bool wheel(App& a, const PluginTarget& t, int param, ImVec2 tl, float size, const Skin& k, int flags, ImU32 fill) {
  Bound b = bind(a, t, param);
  if (!b.ok) return false;
  ImGui::PushID(param);
  ImGui::SetCursorScreenPos(tl);
  ImGui::InvisibleButton("##w", ImVec2(size, size));
  bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
  bool changed = pui::dragParamItem(a, b.pid, b.d, b.val, 170.0f, false);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  bool centred = (flags & WheelCentered) || (b.d.min < 0 && b.d.max > 0);
  drawWheel(dl, ImVec2(tl.x + size * 0.5f, tl.y + size * 0.5f), size * 0.5f, b.d.toNormalized(b.val), centred, flags & WheelBigLine, hov || act, k,
            act ? (fill ? lighten(fill, 0.2f) : k.fillPressed) : (fill ? fill : k.fill));
  if (pui::paramLinked(a, b.pid)) dl->AddCircleFilled(ImVec2(tl.x + size * 0.9f, tl.y + size * 0.1f), 2.5f, k.fill);
  ImGui::PopID();
  return changed;
}

bool vslider(App& a, const PluginTarget& t, int param, ImVec2 tl, ImVec2 size, const Skin& k, ImU32 fill, bool ruler) {
  Bound b = bind(a, t, param);
  if (!b.ok) return false;
  ImGui::PushID(param);
  ImGui::SetCursorScreenPos(tl);
  ImGui::InvisibleButton("##vs", size);
  bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
  bool changed = pui::dragParamItem(a, b.pid, b.d, b.val, size.y, false);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  if (!fill) fill = k.fill;
  float cx = std::round(tl.x + size.x * 0.5f);
  float hh = std::max(6.0f, std::min(size.x * 0.55f, 12.0f));  // handle height
  float top = tl.y + hh * 0.5f, bot = tl.y + size.y - hh * 0.5f, travel = bot - top;
  float rw = std::max(3.0f, size.x * 0.22f);
  if (ruler)
    for (int i = 0; i <= 10; ++i) {
      float y = top + travel * i / 10.0f;
      float w = i % 5 == 0 ? size.x * 0.5f : size.x * 0.32f;
      dl->AddLine(ImVec2(cx - w, y), ImVec2(cx - rw, y), theme::withAlpha(k.textDim, 0.45f), 1.0f);
      dl->AddLine(ImVec2(cx + rw, y), ImVec2(cx + w, y), theme::withAlpha(k.textDim, 0.45f), 1.0f);
    }
  dl->AddRectFilled(ImVec2(cx - rw * 0.5f, top), ImVec2(cx + rw * 0.5f, bot), k.rail, rw * 0.5f);
  float n = static_cast<float>(b.d.toNormalized(b.val));
  bool bip = b.d.min < 0 && b.d.max > 0;
  float y0 = bip ? top + (1 - static_cast<float>(b.d.toNormalized(0))) * travel : bot;
  float yv = top + (1 - n) * travel;
  dl->AddRectFilled(ImVec2(cx - rw * 0.5f, std::min(y0, yv)), ImVec2(cx + rw * 0.5f, std::max(y0, yv)), act ? lighten(fill, 0.2f) : fill, rw * 0.5f);
  float hw = std::min(size.x * 0.5f, 9.0f);
  ImU32 hc = act ? lighten(k.handle, 0.3f) : hov ? lighten(k.handle, 0.15f) : k.handle;
  dl->AddRectFilled(ImVec2(cx - hw, yv - hh * 0.5f + 1), ImVec2(cx + hw, yv + hh * 0.5f + 1), theme::withAlpha(darken(k.panel, 0.8f), 0.5f), 2);
  dl->AddRectFilled(ImVec2(cx - hw, yv - hh * 0.5f), ImVec2(cx + hw, yv + hh * 0.5f), hc, 2);
  dl->AddLine(ImVec2(cx - hw + 2, yv), ImVec2(cx + hw - 2, yv), darken(hc, 0.5f), 1.0f);
  ImGui::PopID();
  return changed;
}

bool hslider(App& a, const PluginTarget& t, int param, ImVec2 tl, ImVec2 size, const Skin& k, ImU32 fill) {
  Bound b = bind(a, t, param);
  if (!b.ok) return false;
  ImGui::PushID(param);
  ImGui::SetCursorScreenPos(tl);
  ImGui::InvisibleButton("##hs", size);
  bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
  bool changed = pui::dragParamItem(a, b.pid, b.d, b.val, size.x, true);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  if (!fill) fill = k.fill;
  float cy = std::round(tl.y + size.y * 0.5f);
  float hw = std::max(5.0f, std::min(size.y * 0.5f, 8.0f));
  float l = tl.x + hw * 0.5f, r = tl.x + size.x - hw * 0.5f;
  float rh = std::max(3.0f, size.y * 0.25f);
  dl->AddRectFilled(ImVec2(l, cy - rh * 0.5f), ImVec2(r, cy + rh * 0.5f), k.rail, rh * 0.5f);
  float xv = l + static_cast<float>(b.d.toNormalized(b.val)) * (r - l);
  dl->AddRectFilled(ImVec2(l, cy - rh * 0.5f), ImVec2(xv, cy + rh * 0.5f), act ? lighten(fill, 0.2f) : fill, rh * 0.5f);
  ImU32 hc = act ? lighten(k.handle, 0.3f) : hov ? lighten(k.handle, 0.15f) : k.handle;
  dl->AddRectFilled(ImVec2(xv - hw * 0.5f, tl.y + 1), ImVec2(xv + hw * 0.5f, tl.y + size.y - 1), hc, 2);
  ImGui::PopID();
  return changed;
}

bool switchSel(App& a, const PluginTarget& t, int param, ImVec2 tl, ImVec2 size, const Skin& k, const char* const* labels, int count,
               bool horizontal) {
  Bound b = bind(a, t, param);
  if (!b.ok || count <= 0) return false;
  ImGui::PushID(param);
  ImGui::SetCursorScreenPos(tl);
  ImGui::InvisibleButton("##sw", size);
  bool hov = ImGui::IsItemHovered();
  ImGuiIO& io = ImGui::GetIO();
  int cur = std::clamp(static_cast<int>(std::lround(b.val - b.d.min)), 0, count - 1);
  bool changed = false;
  float cell = (horizontal ? size.x : size.y) / count;
  if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
    float off = horizontal ? io.MousePos.x - tl.x : io.MousePos.y - tl.y;
    int sel = std::clamp(static_cast<int>(off / cell), 0, count - 1);
    if (sel != cur) {
      a.doc.setParam(b.pid, b.d.min + sel, false);
      cur = sel, changed = true;
    }
  }
  if (hov && io.MouseWheel != 0) {
    int sel = std::clamp(cur - static_cast<int>(io.MouseWheel), 0, count - 1);
    if (sel != cur) {
      a.doc.setParam(b.pid, b.d.min + sel, false);
      cur = sel, changed = true;
    }
  }
  if (hov) a.setHint(b.d.name + ": " + b.d.format(b.d.min + cur) + "   (click to choose · wheel steps)");
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 p1(tl.x + size.x, tl.y + size.y);
  dl->AddRectFilled(tl, p1, k.lcdBg, 3);
  dl->AddRect(tl, p1, darken(k.panel, 0.6f), 3, 0, 1.0f);
  float fs = std::clamp((horizontal ? size.y : cell) * 0.62f, 8.0f, 13.0f);
  for (int i = 0; i < count; ++i) {
    ImVec2 c0 = horizontal ? ImVec2(tl.x + cell * i, tl.y) : ImVec2(tl.x, tl.y + cell * i);
    ImVec2 c1 = horizontal ? ImVec2(c0.x + cell, p1.y) : ImVec2(p1.x, c0.y + cell);
    bool on = i == cur;
    if (on) {
      dl->AddRectFilled(ImVec2(c0.x + 2, c0.y + 2), ImVec2(c1.x - 2, c1.y - 2), theme::withAlpha(k.select, 0.28f), 2);
      // indicator LED at the leading edge
      dl->AddRectFilled(ImVec2(c0.x + 3, (c0.y + c1.y) * 0.5f - 2), ImVec2(c0.x + 6, (c0.y + c1.y) * 0.5f + 2), k.select, 1);
    }
    ImGui::PushFont(app().fontMonoBold, fs);
    ImVec2 ts = ImGui::CalcTextSize(labels[i]);
    dl->PushClipRect(c0, c1, true);
    dl->AddText(ImVec2(std::round((c0.x + c1.x - ts.x) * 0.5f + (horizontal ? 0 : 3)), std::round((c0.y + c1.y - ts.y) * 0.5f)),
                on ? lighten(k.select, 0.35f) : k.textDim, labels[i]);
    dl->PopClipRect();
    ImGui::PopFont();
  }
  ImGui::PopID();
  return changed;
}

bool check(App& a, const PluginTarget& t, int param, ImVec2 tl, ImVec2 size, const Skin& k, const char* text, ImU32 on) {
  Bound b = bind(a, t, param);
  if (!b.ok) return false;
  ImGui::PushID(param);
  ImGui::SetCursorScreenPos(tl);
  bool pressed = ImGui::InvisibleButton("##ck", size);
  bool hov = ImGui::IsItemHovered();
  bool isOn = b.val >= 0.5 * (b.d.min + b.d.max) && b.val > b.d.min;
  if (pressed) a.doc.setParam(b.pid, isOn ? b.d.min : b.d.max, false), isOn = !isOn;
  if (hov) a.setHint(b.d.name + ": " + (isOn ? "on" : "off") + "   (click to toggle)");
  ImDrawList* dl = ImGui::GetWindowDrawList();
  float bs = std::min(size.y, 12.0f);
  ImVec2 b0(tl.x + 1, tl.y + (size.y - bs) * 0.5f), b1(b0.x + bs, b0.y + bs);
  dl->AddRectFilled(b0, b1, k.lcdBg, 2);
  dl->AddRect(b0, b1, hov ? k.text : darken(k.panel, 0.6f), 2, 0, 1.0f);
  if (isOn) dl->AddRectFilled(ImVec2(b0.x + 2.5f, b0.y + 2.5f), ImVec2(b1.x - 2.5f, b1.y - 2.5f), on ? on : k.fill, 1);
  if (text && *text) {
    ImGui::PushFont(app().fontMonoBold, std::clamp(size.y * 0.62f, 8.0f, 12.0f));
    ImVec2 ts = ImGui::CalcTextSize(text);
    dl->AddText(ImVec2(b1.x + 5, std::round(tl.y + (size.y - ts.y) * 0.5f)), isOn ? k.text : k.textDim, text);
    ImGui::PopFont();
  }
  ImGui::PopID();
  return pressed;
}

bool digi(App& a, const PluginTarget& t, int param, ImVec2 tl, ImVec2 size, const Skin& k, const char* text, float font) {
  Bound b = bind(a, t, param);
  if (!b.ok) return false;
  ImGui::PushID(param);
  ImGui::SetCursorScreenPos(tl);
  ImGui::InvisibleButton("##dg", size);
  bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
  bool changed = pui::dragParamItem(a, b.pid, b.d, b.val, 60.0f, false);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  std::string s = text ? std::string(text) : b.d.format(b.val);
  Skin kk = k;
  if (hov || act) kk.lcdText = lighten(k.lcdText, 0.3f);
  lcd(dl, tl, ImVec2(tl.x + size.x, tl.y + size.y), kk, s.c_str(), font > 0 ? font : std::clamp(size.y * 0.75f, 8.0f, 13.0f));
  ImGui::PopID();
  return changed;
}

bool button(const char* id, ImVec2 tl, ImVec2 size, const Skin& k, const char* text, bool on, float font) {
  ImGui::SetCursorScreenPos(tl);
  bool pressed = ImGui::InvisibleButton(id, size);
  bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 p1(tl.x + size.x, tl.y + size.y);
  ImU32 bg = on ? theme::withAlpha(k.select, act ? 0.55f : 0.38f) : act ? lighten(k.panelLo, 0.1f) : hov ? lighten(k.panelLo, 0.06f) : k.panelLo;
  dl->AddRectFilled(tl, p1, bg, 3);
  dl->AddRect(tl, p1, on ? k.select : darken(k.panel, 0.55f), 3, 0, 1.0f);
  if (text && *text) {
    ImGui::PushFont(app().fontMonoBold, font > 0 ? font : std::clamp(size.y * 0.5f, 8.0f, 12.0f));
    ImVec2 ts = ImGui::CalcTextSize(text);
    dl->AddText(ImVec2(std::round((tl.x + p1.x - ts.x) * 0.5f), std::round((tl.y + p1.y - ts.y) * 0.5f)),
                on ? lighten(k.select, 0.45f) : hov ? k.text : k.textDim, text);
    ImGui::PopFont();
  }
  return pressed;
}


// ===================================================================== form-unit controls (κ2)

namespace {

ImU32 shade(ImU32 c, float k) { return k >= 0 ? lighten(c, k) : darken(c, -k); }

ImFont* uiFont(bool bold) {
  App& a = app();
  return bold && a.fontBold ? a.fontBold : a.fontRegular;
}

void text(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* s, ImU32 col, float px, int align, bool bold) {
  ImFont* fnt = uiFont(bold);
  ImVec2 ts = fnt ? fnt->CalcTextSizeA(px, 1e6f, 0, s) : ImGui::CalcTextSize(s);
  float x = align == 0 ? p0.x + 2 : align == 2 ? p1.x - ts.x - 2 : (p0.x + p1.x - ts.x) * 0.5f;
  float y = (p0.y + p1.y - ts.y) * 0.5f;
  dl->PushClipRect(ImVec2(p0.x - 1, p0.y - 1), ImVec2(p1.x + 1, p1.y + 1), true);
  dl->AddText(fnt, px, ImVec2(std::floor(x), std::floor(y)), col, s);
  dl->PopClipRect();
}

void arcAny(ImDrawList* dl, ImVec2 c, float r, float a0, float a1, ImU32 col, float th) {
  if (a1 < a0) std::swap(a0, a1);
  if (a1 - a0 < 0.002f) return;
  arc(dl, c, r, a0, a1, col, th);
}

}  // namespace

Form formAt(float formW, float formH, float width, float maxScale) {
  Form f;
  f.o = ImGui::GetCursorScreenPos();
  f.s = std::min(maxScale, std::max(0.5f, width / formW));
  f.w = formW * f.s;
  f.h = formH * f.s;
  return f;
}

void reserve(const Form& f) {
  ImGui::SetCursorScreenPos(f.o);
  ImGui::Dummy(ImVec2(f.w, f.h));
}

// ================================================================ static parts

void bevel(ImDrawList* dl, const Form& f, float x, float y, float w, float h, ImU32 col, int style) {
  ImVec2 p0 = f.p(x, y), p1 = f.p(x + w, y + h);
  float r = style == BevelPlain ? 0.0f : f.d(5);
  ImDrawFlags fl = style == BevelRoundedBottom ? ImDrawFlags_RoundCornersBottom
                   : style == BevelRoundedTop  ? ImDrawFlags_RoundCornersTop
                                               : ImDrawFlags_RoundCornersAll;
  dl->AddRectFilled(p0, p1, col, r, fl);
  if (style != BevelPlain) {  // soft inner edge: lighter top, darker bottom
    dl->AddLine(ImVec2(p0.x + r, p0.y + 0.5f), ImVec2(p1.x - r, p0.y + 0.5f), shade(col, 0.10f), 1.0f);
    dl->AddLine(ImVec2(p0.x + r, p1.y - 0.5f), ImVec2(p1.x - r, p1.y - 0.5f), shade(col, -0.25f), 1.0f);
  }
}

void gradient(ImDrawList* dl, const Form& f, float x, float y, float w, float h, ImU32 top, ImU32 bottom) {
  dl->AddRectFilledMultiColor(f.p(x, y), f.p(x + w, y + h), top, top, bottom, bottom);
}

void label(ImDrawList* dl, const Form& f, float x, float y, float w, float h, const char* s, ImU32 col, ImU32 bg, float px, int align,
           bool bold, bool rounded) {
  ImVec2 p0 = f.p(x, y), p1 = f.p(x + w, y + h);
  if (bg) dl->AddRectFilled(p0, p1, bg, rounded ? f.d(3) : 0.0f);
  text(dl, p0, p1, s, col, f.d(px), align, bold);
}

void caption(ImDrawList* dl, const Form& f, float x, float y, float w, float h, const char* s, ImU32 col, ImU32 bg) {
  ImVec2 p0 = f.p(x, y), p1 = f.p(x + w, y + h);
  dl->AddRectFilled(p0, p1, bg, f.d(h * 0.5f));
  text(dl, p0, p1, s, col, f.d(std::min(11.0f, h * 0.68f)), 1, true);
}

void bracket(ImDrawList* dl, const Form& f, float x0, float x1, float y, ImU32 col) {
  ImVec2 a = f.p(x0, y), b = f.p(x1, y);
  float t = f.d(4);
  dl->AddLine(ImVec2(a.x, a.y - t), a, col, 1.0f);
  dl->AddLine(a, b, col, 1.0f);
  dl->AddLine(ImVec2(b.x, b.y - t), b, col, 1.0f);
  dl->AddLine(ImVec2((a.x + b.x) * 0.5f, a.y), ImVec2((a.x + b.x) * 0.5f, a.y + t), col, 1.0f);
}

void meter(ImDrawList* dl, const Form& f, float x, float y, float w, float h, float db, const MeterLook& look, bool reduction) {
  ImVec2 p0 = f.p(x, y), p1 = f.p(x + w, y + h);
  dl->AddRectFilled(p0, p1, look.bg, f.d(1.5f));
  float k = reduction ? std::clamp(-db / look.rangeDb, 0.0f, 1.0f) : std::clamp(1.0f + db / look.rangeDb, 0.0f, 1.0f);
  if (k <= 0) return;
  float H = p1.y - p0.y - 2;
  if (reduction) {
    dl->AddRectFilled(ImVec2(p0.x + 1, p0.y + 1), ImVec2(p1.x - 1, p0.y + 1 + H * k), look.high);
    return;
  }
  // Segments: low up to -12 dB, mid to -3 dB, high above.
  auto yAt = [&](float dB) { return p1.y - 1 - H * std::clamp(1.0f + dB / look.rangeDb, 0.0f, 1.0f); };
  float top = p1.y - 1 - H * k;
  float yMid = yAt(-12), yHigh = yAt(-3);
  dl->AddRectFilled(ImVec2(p0.x + 1, std::max(top, yMid)), ImVec2(p1.x - 1, p1.y - 1), look.low);
  if (top < yMid) dl->AddRectFilled(ImVec2(p0.x + 1, std::max(top, yHigh)), ImVec2(p1.x - 1, yMid), look.mid);
  if (top < yHigh) dl->AddRectFilled(ImVec2(p0.x + 1, top), ImVec2(p1.x - 1, yHigh), look.high);
}

// ================================================================ parameter controls

void drawWheel(ImDrawList* dl, ImVec2 c, float r, double n, bool centred, bool hov, bool act, const WheelLook& look) {
  n = std::clamp(n, 0.0, 1.0);
  float th = std::max(2.0f, r * look.railWidth);
  float rr = r - th * 0.5f;
  const float kA0 = kPi * 0.5f + look.gap * 0.5f, kA1 = kPi * 2.5f - look.gap * 0.5f;
  float an = kA0 + static_cast<float>(n) * (kA1 - kA0);
  float from = centred ? (kA0 + kA1) * 0.5f : kA0;
  ImU32 fillCol = act ? look.fillPressed : hov ? lerpCol(look.fill, look.fillPressed, 0.4f) : look.fill;
  if (look.ruler) {  // ruler ticks outside the cap, lit up to the value
    const int ticks = 21;
    for (int k = 0; k < ticks; ++k) {
      float ak = kA0 + (kA1 - kA0) * k / (ticks - 1);
      bool lit = (ak >= std::min(from, an) - 1e-3f && ak <= std::max(from, an) + 1e-3f);
      bool major = k % 5 == 0;
      ImVec2 d(std::cos(ak), std::sin(ak));
      float r0 = r - th * (major ? 1.0f : 0.6f);
      dl->AddLine(ImVec2(c.x + d.x * r0, c.y + d.y * r0), ImVec2(c.x + d.x * r, c.y + d.y * r), lit ? fillCol : look.rail, major ? 1.6f : 1.0f);
    }
  } else {  // flat gauge rail and value fill
    arcAny(dl, c, rr, kA0, kA1, look.rail, th);
    arcAny(dl, c, rr, from, an, fillCol, th);
  }
  // Bevelled cap: drop shadow, body, light top-left rim, dark bottom-right rim.
  float cr = r - th - std::max(1.0f, r * 0.06f);
  dl->AddCircleFilled(ImVec2(c.x, c.y + cr * 0.08f), cr, IM_COL32(0, 0, 0, 90), 40);
  dl->AddCircleFilled(c, cr, look.cap, 40);
  arcAny(dl, c, cr - 0.75f, kPi * 1.0f, kPi * 1.75f, shade(look.cap, 0.22f), 1.2f);
  arcAny(dl, c, cr - 0.75f, kPi * 0.0f, kPi * 0.75f, shade(look.cap, -0.35f), 1.2f);
  if (look.dents > 0) {  // cog / dented rim
    for (int k = 0; k < look.dents * 4; ++k) {
      float ak = an + 2.0f * kPi * k / (look.dents * 4);
      ImVec2 d(std::cos(ak), std::sin(ak));
      dl->AddLine(ImVec2(c.x + d.x * cr * 0.82f, c.y + d.y * cr * 0.82f), ImVec2(c.x + d.x * (cr - 0.5f), c.y + d.y * (cr - 0.5f)),
                  shade(look.cap, -0.3f), 1.0f);
    }
  }
  // Long pointer from the centre to the cap edge.
  ImVec2 dir(std::cos(an), std::sin(an));
  dl->AddLine(ImVec2(c.x + dir.x * cr * 0.12f, c.y + dir.y * cr * 0.12f), ImVec2(c.x + dir.x * (cr - 1.0f), c.y + dir.y * (cr - 1.0f)),
              look.pointer, std::max(1.5f, r * 0.09f));
}

bool wheel(App& a, const PluginTarget& t, int param, const Form& f, float x, float y, float w, float h, const WheelLook& look) {
  Bound b = bind(a, t, param);
  if (!b.ok) return false;
  ImGui::PushID(param);
  ImVec2 p0 = f.p(x, y);
  ImGui::SetCursorScreenPos(p0);
  ImGui::InvisibleButton("##w", ImVec2(f.d(w), f.d(h)));
  bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
  bool changed = pui::dragParamItem(a, b.pid, b.d, b.val, 200.0f, false);
  ImGui::PopID();
  float r = f.d(std::min(w, h)) * 0.5f - 1.0f;
  ImVec2 c(p0.x + f.d(w) * 0.5f, p0.y + f.d(h) * 0.5f);
  drawWheel(ImGui::GetWindowDrawList(), c, r, b.d.toNormalized(b.val), look.centred || (b.d.min < 0 && b.d.max > 0), hov, act, look);
  if (pui::paramLinked(a, b.pid)) ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(c.x + r * 0.85f, c.y - r * 0.85f), 2.5f, look.fill);
  return changed;
}

bool digi(App& a, const PluginTarget& t, int param, const Form& f, float x, float y, float w, float h, const DigiLook& look, const char* txt) {
  Bound b = bind(a, t, param);
  if (!b.ok) return false;
  ImGui::PushID(param);
  ImVec2 p0 = f.p(x, y), p1 = f.p(x + w, y + h);
  ImGui::SetCursorScreenPos(p0);
  ImGui::InvisibleButton("##d", ImVec2(p1.x - p0.x, p1.y - p0.y));
  bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
  bool changed = pui::dragParamItem(a, b.pid, b.d, b.val, 60.0f, false);
  ImGui::PopID();
  std::string s;
  if (txt) s = txt;
  else if (b.d.kind == ParamKind::Choice && !b.d.choices.empty())
    s = b.d.choices[static_cast<size_t>(std::clamp<long>(std::lround(b.val), 0, static_cast<long>(b.d.choices.size()) - 1))];
  else s = std::to_string(std::lround(b.val));
  Skin k;
  k.panel = look.bg;
  k.lcdBg = hov || act ? lighten(look.bg, 0.08f) : look.bg;
  k.lcdText = look.text;
  lcd(ImGui::GetWindowDrawList(), p0, p1, k, s.c_str(), f.d(std::min(12.0f, h * 0.62f)));
  return changed;
}

namespace {
bool tabsImpl(const Form& f, float x, float y, float w, float h, const char* const* labels, int count, int& sel, const TabLook& look,
              bool vertical) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  bool changed = false;
  for (int i = 0; i < count; ++i) {
    float cx = vertical ? x : x + w * i / count, cy = vertical ? y + h * i / count : y;
    float cw = vertical ? w : w / count, ch = vertical ? h / count : h;
    ImVec2 p0 = f.p(cx, cy), p1 = f.p(cx + cw, cy + ch);
    if (i > 0) (vertical ? p0.y : p0.x) += 1.0f;
    ImGui::PushID(i);
    ImGui::SetCursorScreenPos(p0);
    ImGui::InvisibleButton("##t", ImVec2(p1.x - p0.x, p1.y - p0.y));
    if (ImGui::IsItemClicked() && sel != i) {
      sel = i;
      changed = true;
    }
    bool hov = ImGui::IsItemHovered();
    ImGui::PopID();
    bool on = sel == i;
    dl->AddRectFilled(p0, p1, on ? look.btnPressed : hov ? shade(look.btn, 0.06f) : look.btn, f.d(3));
    if (on) dl->AddRect(p0, p1, shade(look.btnPressed, -0.3f), f.d(3), 0, 1.0f);
    text(dl, p0, p1, labels[i], on ? look.textPressed : look.text, f.d(std::min(11.0f, ch * 0.6f)), 1, true);
  }
  return changed;
}
}  // namespace

bool tabs(App& a, const PluginTarget& t, int param, const Form& f, float x, float y, float w, float h, const char* const* labels, int count,
          const TabLook& look, bool vertical) {
  Bound bb = bind(a, t, param);
  if (!bb.ok) return false;
  const ParamDesc* d = &bb.d;
  double val = bb.val;
  int sel = static_cast<int>(std::lround(val));
  std::vector<const char*> names;
  if (!labels)
    for (const auto& c : d->choices) names.push_back(c.c_str());
  ImGui::PushID(param);
  bool ch = tabsImpl(f, x, y, w, h, labels ? labels : names.data(), labels ? count : static_cast<int>(names.size()), sel, look, vertical);
  ImGui::PopID();
  if (ch) pui::setParamValue(a, t, param, sel);
  return ch;
}

bool tabsUi(const Form& f, const char* id, float x, float y, float w, float h, const char* const* labels, int count, int& sel, const TabLook& look,
            bool vertical) {
  ImGui::PushID(id);
  bool ch = tabsImpl(f, x, y, w, h, labels, count, sel, look, vertical);
  ImGui::PopID();
  return ch;
}

bool switchSel(App& a, const PluginTarget& t, int param, const Form& f, float x, float y, float w, float h, ImU32 slot, ImU32 lit,
               const char* const* labels) {
  Bound b = bind(a, t, param);
  if (!b.ok) return false;
  std::vector<std::string> names;
  if (b.d.kind == ParamKind::Choice) names = b.d.choices;
  int n = std::max<int>(2, static_cast<int>(names.size()));
  if (names.empty()) names = {"OFF", "ON"};
  std::vector<const char*> lb;
  for (int i = 0; i < n; ++i) lb.push_back(labels && labels[i] ? labels[i] : names[static_cast<size_t>(i)].c_str());
  Skin k;
  k.panel = slot;
  k.lcdBg = slot;
  k.select = lit;
  k.textDim = lighten(slot, 0.55f);
  return switchSel(a, t, param, f.p(x, y), ImVec2(f.d(w), f.d(h)), k, lb.data(), n, w > h);
}

bool ledToggle(App& a, const PluginTarget& t, int param, const Form& f, float x, float y, float w, float h, const char* txt, ImU32 bg,
               ImU32 on, ImU32 textCol) {
  Bound bb = bind(a, t, param);
  if (!bb.ok) return false;
  const ParamDesc* d = &bb.d;
  double val = bb.val;
  bool lit = val >= 0.5;
  ImGui::PushID(param);
  ImVec2 p0 = f.p(x, y), p1 = f.p(x + w, y + h);
  ImGui::SetCursorScreenPos(p0);
  ImGui::InvisibleButton("##l", ImVec2(p1.x - p0.x, p1.y - p0.y));
  bool changed = false;
  if (ImGui::IsItemClicked()) {
    pui::setParamValue(a, t, param, lit ? 0.0 : 1.0);
    lit = !lit;
    changed = true;
  }
  if (ImGui::IsItemHovered()) app().setHint(d->name + ": " + (lit ? "on" : "off"));
  ImGui::PopID();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(p0, p1, bg, f.d(3));
  float lr = f.d(std::min(h, 14.0f) * 0.22f);
  ImVec2 lc(p0.x + f.d(7), (p0.y + p1.y) * 0.5f);
  if (txt && *txt) {
    dl->AddCircleFilled(lc, lr, lit ? on : shade(bg, -0.4f), 16);
    if (lit) dl->AddCircle(lc, lr + 1.5f, theme::withAlpha(on, 0.35f), 16, 1.5f);
    text(dl, ImVec2(p0.x + f.d(13), p0.y), p1, txt, textCol, f.d(std::min(10.0f, h * 0.55f)), 1, true);
  } else {
    dl->AddCircleFilled(ImVec2((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f), lr * 1.4f, lit ? on : shade(bg, -0.4f), 16);
  }
  return changed;
}

// ================================================================ UI-only controls

bool slider(const Form& f, const char* id, float x, float y, float w, float h, float& v, ImU32 rail, ImU32 handle, const char* hintText) {
  bool vertical = h > w;
  ImVec2 p0 = f.p(x, y), p1 = f.p(x + w, y + h);
  ImGui::PushID(id);
  ImGui::SetCursorScreenPos(p0);
  ImGui::InvisibleButton("##sl", ImVec2(p1.x - p0.x, p1.y - p0.y));
  bool changed = false;
  if (ImGui::IsItemActive()) {
    ImVec2 m = ImGui::GetIO().MousePos;
    float k = vertical ? 1.0f - (m.y - p0.y) / (p1.y - p0.y) : (m.x - p0.x) / (p1.x - p0.x);
    k = std::clamp(k, 0.0f, 1.0f);
    if (k != v) v = k, changed = true;
  }
  if (hintText && ImGui::IsItemHovered()) app().setHint(hintText);
  ImGui::PopID();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  float rw = f.d(2.0f);
  ImVec2 c((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f);
  float hs = f.d(4.5f);
  if (vertical) {
    dl->AddRectFilled(ImVec2(c.x - rw, p0.y + hs), ImVec2(c.x + rw, p1.y - hs), rail, rw);
    dl->AddLine(ImVec2(c.x - rw, p0.y + hs), ImVec2(c.x - rw, p1.y - hs), shade(rail, -0.4f));
    float yy = p1.y - hs - (p1.y - p0.y - hs * 2) * v;
    dl->AddRectFilled(ImVec2(p0.x + 1, yy - hs), ImVec2(p1.x - 1, yy + hs), handle, f.d(2));
    dl->AddLine(ImVec2(p0.x + 3, yy), ImVec2(p1.x - 3, yy), shade(handle, 0.45f), 1.0f);
  } else {
    dl->AddRectFilled(ImVec2(p0.x + hs, c.y - rw), ImVec2(p1.x - hs, c.y + rw), rail, rw);
    float xx = p0.x + hs + (p1.x - p0.x - hs * 2) * v;
    dl->AddRectFilled(ImVec2(xx - hs, p0.y + 1), ImVec2(xx + hs, p1.y - 1), handle, f.d(2));
    dl->AddLine(ImVec2(xx, p0.y + 3), ImVec2(xx, p1.y - 3), shade(handle, 0.45f), 1.0f);
  }
  return changed;
}

bool glyphToggle(const Form& f, const char* id, float x, float y, float w, float h, bool& on, ImU32 col,
                 void (*draw)(ImDrawList*, ImVec2, float, ImU32), const char* hintText) {
  ImVec2 p0 = f.p(x, y), p1 = f.p(x + w, y + h);
  ImGui::PushID(id);
  ImGui::SetCursorScreenPos(p0);
  ImGui::InvisibleButton("##g", ImVec2(p1.x - p0.x, p1.y - p0.y));
  bool clicked = ImGui::IsItemClicked();
  bool hov = ImGui::IsItemHovered();
  if (clicked) on = !on;
  if (hintText && hov) app().setHint(hintText);
  ImGui::PopID();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImU32 c = on ? col : theme::withAlpha(col, hov ? 0.55f : 0.3f);
  draw(dl, ImVec2((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f), std::min(p1.x - p0.x, p1.y - p0.y), c);
  return clicked;
}

bool button(const Form& f, const char* id, float x, float y, float w, float h, const char* txt, ImU32 bg, ImU32 textCol, const char* hintText) {
  ImVec2 p0 = f.p(x, y), p1 = f.p(x + w, y + h);
  ImGui::PushID(id);
  ImGui::SetCursorScreenPos(p0);
  bool clicked = ImGui::InvisibleButton("##b", ImVec2(p1.x - p0.x, p1.y - p0.y));
  bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
  if (hintText && hov) app().setHint(hintText);
  ImGui::PopID();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  if (bg) dl->AddRectFilled(p0, p1, act ? shade(bg, -0.2f) : hov ? shade(bg, 0.1f) : bg, f.d(3));
  if (txt) text(dl, p0, p1, txt, textCol, f.d(std::min(11.0f, h * 0.6f)), 1, true);
  return clicked;
}

bool menuButton(const Form& f, const char* id, float x, float y, float w, float h, ImU32 col) {
  ImVec2 p0 = f.p(x, y), p1 = f.p(x + w, y + h);
  ImGui::PushID(id);
  ImGui::SetCursorScreenPos(p0);
  bool clicked = ImGui::InvisibleButton("##m", ImVec2(p1.x - p0.x, p1.y - p0.y));
  bool hov = ImGui::IsItemHovered();
  if (hov) app().setHint("Options");
  ImGui::PopID();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImU32 c = hov ? shade(col, 0.3f) : col;
  float cx = (p0.x + p1.x) * 0.5f, cy = (p0.y + p1.y) * 0.5f, hw = (p1.x - p0.x) * 0.3f, gap = (p1.y - p0.y) * 0.22f;
  for (int i = -1; i <= 1; ++i) dl->AddLine(ImVec2(cx - hw, cy + gap * i), ImVec2(cx + hw, cy + gap * i), c, std::max(1.2f, f.d(1.5f)));
  return clicked;
}

void hint(App& a, const char* s) {
  if (ImGui::IsItemHovered()) a.setHint(s);
}

// =============================================================== reference-form kit (κ3)


namespace {

constexpr float kKitPi = 3.14159265358979f;

const FormCtl kMissing = {"", Ctl::Group, 0, 0, 0, 0, -1, 0, 0, 0, -1, -1, -1, -1, 0, 0, 0, 0, 0, 0, 0, 3};

float luma(ImU32 c) {
  float r = (c >> IM_COL32_R_SHIFT & 0xFF) / 255.f, g = (c >> IM_COL32_G_SHIFT & 0xFF) / 255.f, b = (c >> IM_COL32_B_SHIFT & 0xFF) / 255.f;
  return 0.2126f * r + 0.7152f * g + 0.0722f * b;
}

struct ParamView {
  const ParamDesc* d = nullptr;
  double v = 0;
  double norm = 0;
  bool bipolar = false;
};
ParamView pview(App& a, const PluginTarget& t, int param) {
  ParamView r;
  const Song& s = a.doc.song();
  auto d = describeParam(s, t.pid(param));
  auto v = getParam(s, t.pid(param));
  static ParamDesc keep[8];
  static int slot = 0;
  if (!d || !v) return r;
  slot = (slot + 1) % 8;
  keep[slot] = *d;
  r.d = &keep[slot];
  r.v = *v;
  r.norm = std::clamp(d->toNormalized(*v), 0.0, 1.0);
  r.bipolar = d->min < 0 && d->max > 0 && d->kind == ParamKind::Continuous;
  return r;
}

// Angle (screen convention: y down) of normalised value n on a wheel with the gap a0..a1.
float wheelAngle(const FormCtl& c, double n) {
  float a0 = c.a0, a1 = c.a1;
  if (a0 == 0 && a1 == 0) a0 = kKitPi * 1.25f, a1 = kKitPi * 1.75f;  // default 270° sweep
  float sweep = 2 * kKitPi - (a1 - a0);
  if (sweep <= 0.01f) sweep = 2 * kKitPi;
  float th = a0 - static_cast<float>(n) * sweep;  // maths convention, clockwise
  return -th;                                     // screen convention
}

void kitArc(ImDrawList* dl, ImVec2 c, float r, float from, float to, ImU32 col, float th) {
  if (to < from) std::swap(from, to);
  if (to - from < 0.002f) return;
  int segs = std::max(6, static_cast<int>((to - from) * r * 0.5f));
  dl->PathArcTo(c, r, from, to, segs);
  dl->PathStroke(col, 0, th);
}

void drawCap(ImDrawList* dl, ImVec2 c, float r, ImU32 base, int style, bool hot, float s) {
  if (hot) base = lighten(base, 0.08f);
  dl->AddCircleFilled(ImVec2(c.x, c.y + std::max(1.0f, r * 0.06f)), r, IM_COL32(0, 0, 0, 90), 40);  // drop shadow
  switch (style) {
    case CapChrome:
    case CapChrome2:
    case CapGearChrome: {
      if (style == CapGearChrome) {  // dented rim
        const int dents = 24;
        for (int k = 0; k < dents; ++k) {
          float an = 2 * kKitPi * k / dents;
          dl->AddCircleFilled(ImVec2(c.x + std::cos(an) * r * 0.97f, c.y + std::sin(an) * r * 0.97f), r * 0.09f, darken(base, 0.25f), 8);
        }
      }
      dl->AddCircleFilled(c, r, darken(base, 0.35f), 40);
      // Concentric bands: light from the top-left.
      for (int k = 0; k < 6; ++k) {
        float rr = r * (0.94f - k * 0.13f);
        float t = static_cast<float>(k) / 6.f;
        dl->AddCircleFilled(ImVec2(c.x - r * 0.04f * k, c.y - r * 0.05f * k), rr, lighten(base, 0.05f + t * 0.35f), 40);
      }
      dl->AddCircle(c, r, darken(base, 0.55f), 40, std::max(1.0f, s * 0.7f));
      break;
    }
    case CapSoftBump: {
      dl->AddCircleFilled(c, r, base, 40);
      dl->AddCircleFilled(ImVec2(c.x - r * 0.15f, c.y - r * 0.2f), r * 0.6f, lighten(base, 0.12f), 40);
      dl->AddCircle(c, r, darken(base, 0.4f), 40, std::max(1.0f, s * 0.7f));
      break;
    }
    case CapBevelled:
    default: {
      dl->AddCircleFilled(c, r, darken(base, 0.3f), 40);
      // Bevel: lighter upper rim, darker lower rim, flat face.
      kitArc(dl, c, r * 0.9f, kKitPi * 0.95f, kKitPi * 2.05f, lighten(base, 0.35f), std::max(1.0f, r * 0.14f));
      kitArc(dl, c, r * 0.9f, kKitPi * 0.05f, kKitPi * 0.95f, darken(base, 0.45f), std::max(1.0f, r * 0.14f));
      dl->AddCircleFilled(c, r * 0.8f, base, 40);
      dl->AddCircle(c, r, darken(base, 0.6f), 40, std::max(1.0f, s * 0.6f));
      break;
    }
  }
}

// Standard interaction (pui::dragParamItem) on the last item for a bound parameter.
bool dragBound(App& a, const PluginTarget& t, int param, float pixels, bool horizontal) {
  ParamId pid = t.pid(param);
  auto d = describeParam(a.doc.song(), pid);
  auto v = getParam(a.doc.song(), pid);
  if (!d || !v) return false;
  double val = *v;
  return pui::dragParamItem(a, pid, *d, val, pixels, horizontal);
}

void tooltipHint(const char* hint) {
  if (hint && ImGui::IsItemHovered()) app().setHint(hint);
}

}  // namespace

ImU32 col(int rrggbb, float alpha) { return theme::rgb(static_cast<uint32_t>(rrggbb) & 0xFFFFFF, alpha); }

const FormCtl* FormDef::find(const char* name) const {
  for (int i = 0; i < count; ++i)
    if (std::strcmp(ctls[i].name, name) == 0) return &ctls[i];
  return nullptr;
}

const FormCtl& View::c(const char* name) const {
  const FormCtl* r = f ? f->find(name) : nullptr;
  IM_ASSERT(r && "unknown form control");
  return r ? *r : kMissing;
}

Palette paletteFrom(const FormDef& f) {
  Palette p{};
  p.panel = col(f.bg >= 0 ? f.bg : 0x2B3034);
  int panelC = -1, fill = -1, rail = -1, cap = -1, label = -1;
  for (int i = 0; i < f.count; ++i) {
    const FormCtl& c = f.ctls[i];
    if (c.kind == Ctl::Panel && c.color >= 0 && panelC < 0) panelC = c.color;
    if ((c.kind == Ctl::Wheel || c.kind == Ctl::Slider) && c.fill >= 0 && fill < 0) fill = c.fill;
    if ((c.kind == Ctl::Wheel || c.kind == Ctl::Slider) && c.rail >= 0 && rail < 0) rail = c.rail;
    if (c.kind == Ctl::Wheel && c.cap >= 0 && cap < 0) cap = c.cap;
    if (c.kind == Ctl::Label && c.color >= 0 && label < 0) label = c.color;
  }
  p.raised = panelC >= 0 ? col(panelC) : lighten(p.panel, 0.06f);
  p.bevel = darken(p.raised, 0.35f);
  p.well = darken(p.panel, 0.35f);
  bool darkBg = luma(p.panel) < 0.5f;
  p.text = darkBg ? IM_COL32(0xD8, 0xDD, 0xE2, 255) : IM_COL32(0x20, 0x22, 0x26, 255);
  p.textDim = theme::mix(p.text, p.panel, 0.45f);
  p.fill = fill >= 0 ? col(fill) : IM_COL32(0x9C, 0xD0, 0x5A, 255);
  p.rail = rail >= 0 ? col(rail) : darken(p.raised, 0.3f);
  p.cap = cap >= 0 ? col(cap) : lighten(p.raised, 0.08f);
  p.pointer = IM_COL32(0xEE, 0xF0, 0xF2, 255);
  p.lcdBg = IM_COL32(0x1A, 0x1E, 0x20, 255);
  p.lcdText = theme::mix(p.fill, IM_COL32(255, 255, 255, 255), 0.35f);
  p.labelBg = label >= 0 ? col(label) : darken(p.raised, 0.2f);
  p.handle = lighten(p.raised, 0.25f);
  p.accent2 = lighten(p.fill, 0.25f);
  p.panelHi = lighten(p.panel, 0.06f), p.panelLo = darken(p.panel, 0.22f);
  p.fillPressed = p.fill, p.select = p.fill;
  p.graphBg = darken(p.panel, 0.55f), p.graphGrid = theme::mix(p.graphBg, p.text, 0.10f);
  p.graphGridStrong = theme::mix(p.graphBg, p.text, 0.22f), p.graphText = theme::mix(p.graphBg, p.text, 0.45f);
  p.graphCurve = lighten(p.fill, 0.15f);
  return p;
}

float fitScale(const FormDef& f, float width, float maxScale) { return std::max(0.5f, std::min(maxScale, width / static_cast<float>(f.w))); }

View begin(const FormDef& f, float width, float maxScale, const Palette& pal, float growH) {
  View v;
  v.f = &f;
  v.s = fitScale(f, width, maxScale);
  v.growH = growH;
  v.o = ImGui::GetCursorScreenPos();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(v.o, v.p(static_cast<float>(f.w), static_cast<float>(f.h) + growH), pal.panel, v.px(3));
  return v;
}

void end(const View& v) {
  ImGui::SetCursorScreenPos(v.o);
  ImGui::Dummy(ImVec2(v.f->w * v.s, v.height()));
}

void textAt(ImDrawList* dl, ImVec2 p0, ImVec2 p1, ImU32 c, const char* text, float size, int align, bool mono) {
  if (!text || !*text) return;
  ImFont* font = mono ? app().fontMono : app().fontRegular;
  ImGui::PushFont(font, size);
  ImVec2 ts = ImGui::CalcTextSize(text);
  float x = align < 0 ? p0.x + 2 : align > 0 ? p1.x - ts.x - 2 : (p0.x + p1.x - ts.x) * 0.5f;
  float y = (p0.y + p1.y - ts.y) * 0.5f;
  dl->PushClipRect(ImVec2(p0.x - 1, p0.y - 1), ImVec2(p1.x + 1, p1.y + 1), true);
  dl->AddText(ImVec2(std::floor(x), std::floor(y)), c, text);
  dl->PopClipRect();
  ImGui::PopFont();
}

void panel(const View& v, const char* name, const Palette& pal, const char* title) {
  const FormCtl& c = v.c(name);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImU32 fill = c.color >= 0 ? col(c.color) : pal.raised;
  ImVec2 a = v.p0(c), b = v.p1(c);
  dl->AddRectFilled(a, b, fill, v.px(3));
  dl->AddRect(a, b, darken(fill, 0.3f), v.px(3), 0, 1.0f);
  dl->AddLine(ImVec2(a.x + v.px(3), a.y + 1), ImVec2(b.x - v.px(3), a.y + 1), lighten(fill, 0.12f), 1.0f);
  if (title) {
    ImVec2 tb(b.x, a.y + v.px(18));
    dl->AddRectFilled(a, tb, darken(fill, 0.12f), v.px(3), ImDrawFlags_RoundCornersTop);
    textAt(dl, ImVec2(a.x + v.px(5), a.y), ImVec2(b.x, tb.y), pal.text, title, std::max(10.f, v.px(10.5f)), -1);
  }
}

void bevel(const View& v, const char* name, const Palette& pal) {
  const FormCtl& c = v.c(name);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImU32 fill = c.color >= 0 ? col(c.color) : pal.well;
  float rr = c.style == 1 ? 0.f : v.px(4);
  ImDrawFlags fl = c.style == 3 ? ImDrawFlags_RoundCornersBottom : ImDrawFlags_RoundCornersAll;
  dl->AddRectFilled(v.p0(c), v.p1(c), fill, rr, fl);
  dl->AddRect(v.p0(c), v.p1(c), darken(fill, 0.35f), rr, fl, 1.0f);
}

void label(const View& v, const char* name, const Palette& pal, const char* text, int align, bool rounded) {
  const FormCtl& c = v.c(name);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 a = v.p0(c), b = v.p1(c);
  if (rounded) dl->AddRectFilled(a, b, c.color >= 0 ? col(c.color) : pal.labelBg, v.px(4));
  textAt(dl, a, b, pal.text, text, std::clamp(v.px(9.5f), 10.f, 15.f), align);
}

void lcd(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const Palette& pal, const char* text, int align) {
  dl->AddRectFilled(p0, p1, pal.lcdBg, 3);
  dl->AddRect(p0, p1, darken(pal.lcdBg, 0.5f), 3, 0, 1.0f);
  float h = p1.y - p0.y;
  textAt(dl, ImVec2(p0.x + 3, p0.y), ImVec2(p1.x - 3, p1.y), pal.lcdText, text, std::clamp(h * 0.62f, 10.f, 20.f), align, true);
}

void drawWheel(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const FormCtl& c, const Palette& pal, double norm, bool bipolar, bool hot, float s) {
  ImVec2 ctr((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f);
  float R = std::min(p1.x - p0.x, p1.y - p0.y) * 0.5f;
  ImU32 fill = c.fill >= 0 ? col(c.fill) : pal.fill;
  ImU32 rail = c.rail >= 0 ? col(c.rail) : pal.rail;
  ImU32 capC = c.cap >= 0 ? col(c.cap) : pal.cap;
  float an = wheelAngle(c, norm), aStart = wheelAngle(c, 0), aEnd = wheelAngle(c, 1), aMid = wheelAngle(c, 0.5);
  float aFrom = bipolar ? aMid : aStart;
  float capR = R * 0.74f;
  switch (c.railStyle) {
    case RailGauge: {
      float th = std::max(2.f, R * 0.17f), rr = R - th * 0.5f;
      // aStart > aEnd numerically in screen convention? normalise to increasing order.
      float s0 = std::min(aStart, aEnd), s1 = std::max(aStart, aEnd);
      if (std::fabs(s1 - s0) < 0.01f) s1 = s0 + 2 * kKitPi;
      kitArc(dl, ctr, rr, s0, s1, darken(rail, 0.15f), th);
      kitArc(dl, ctr, rr, aFrom, an, fill, th);
      capR = R - th - std::max(1.5f, R * 0.06f);
      break;
    }
    case RailInterFill:
    case RailInterFillIn: {
      float rr = c.railStyle == RailInterFill ? R * 0.92f : R * 0.86f;
      float s0 = std::min(aStart, aEnd), s1 = std::max(aStart, aEnd);
      kitArc(dl, ctr, rr, s0, s1, rail, std::max(1.5f, R * 0.1f));
      float lo = std::min(aFrom, an), hi = std::max(aFrom, an);
      if (hi - lo > 0.01f) {
        dl->PathLineTo(ctr);
        dl->PathArcTo(ctr, rr, lo, hi, std::max(6, static_cast<int>((hi - lo) * rr * 0.4f)));
        dl->PathFillConvex(theme::withAlpha(fill, 0.55f));
      }
      capR = R * 0.66f;
      break;
    }
    case RailRulerOut: {
      // Ruler ticks around the outside (big steps every 1/10, small in between).
      const int big = 10;
      for (int k = 0; k <= big * 2; ++k) {
        float a = wheelAngle(c, static_cast<double>(k) / (big * 2));
        bool b = k % 2 == 0;
        float r0 = R * (b ? 0.80f : 0.86f), r1 = R * 0.98f;
        dl->AddLine(ImVec2(ctr.x + std::cos(a) * r0, ctr.y + std::sin(a) * r0), ImVec2(ctr.x + std::cos(a) * r1, ctr.y + std::sin(a) * r1),
                    b ? lighten(rail, 0.35f) : lighten(rail, 0.15f), std::max(1.0f, s * (b ? 0.9f : 0.6f)));
      }
      kitArc(dl, ctr, R * 0.98f, aFrom, an, fill, std::max(1.5f, s * 1.2f));
      capR = R * 0.72f;
      break;
    }
    default: break;
  }
  drawCap(dl, ctr, capR, capC, c.capStyle, hot, s);
  // Long-line pointer in the value colour with a light core.
  ImVec2 dir(std::cos(an), std::sin(an));
  float th = std::max(1.5f, capR * 0.13f);
  ImVec2 q0(ctr.x + dir.x * capR * 0.18f, ctr.y + dir.y * capR * 0.18f), q1(ctr.x + dir.x * capR * 0.92f, ctr.y + dir.y * capR * 0.92f);
  dl->AddLine(q0, q1, IM_COL32(0, 0, 0, 120), th + 1.5f);
  dl->AddLine(q0, q1, hot ? lighten(fill, 0.4f) : fill, th);
}

void drawSlider(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const FormCtl& c, const Palette& pal, double norm, bool bipolar, bool hot, float s) {
  bool vert = (p1.y - p0.y) >= (p1.x - p0.x);
  ImU32 fill = c.fill >= 0 ? col(c.fill) : pal.fill;
  ImU32 rail = c.rail >= 0 ? col(c.rail) : pal.rail;
  ImU32 handle = c.cap >= 0 ? col(c.cap) : pal.handle;
  float thick = vert ? p1.x - p0.x : p1.y - p0.y;
  float hl = std::clamp(thick * (c.capStyle == HandleMixer ? 0.62f : 0.42f), 6.f, 40.f);  // handle length along the travel
  float t0 = (vert ? p0.y : p0.x) + hl * 0.5f, t1 = (vert ? p1.y : p1.x) - hl * 0.5f;
  auto pos = [&](double n) { return vert ? t1 - static_cast<float>(n) * (t1 - t0) : t0 + static_cast<float>(n) * (t1 - t0); };
  float mid = vert ? (p0.x + p1.x) * 0.5f : (p0.y + p1.y) * 0.5f;
  float rw = std::max(3.f, thick * 0.16f);
  auto R = [&](float a0, float a1, float b0, float b1) {  // along a, across b
    return vert ? std::make_pair(ImVec2(b0, a0), ImVec2(b1, a1)) : std::make_pair(ImVec2(a0, b0), ImVec2(a1, b1));
  };
  // Ruler ticks (inside, both sides of the rail).
  const int ticks = 10;
  for (int k = 0; k <= ticks; ++k) {
    float a = pos(static_cast<double>(k) / ticks);
    float w = (k % 5 == 0) ? thick * 0.42f : thick * 0.3f;
    ImU32 tc = theme::withAlpha(lighten(rail, 0.3f), k % 5 == 0 ? 0.9f : 0.55f);
    if (vert)
      dl->AddLine(ImVec2(mid - w, a), ImVec2(mid - rw, a), tc, 1.0f), dl->AddLine(ImVec2(mid + rw, a), ImVec2(mid + w, a), tc, 1.0f);
    else
      dl->AddLine(ImVec2(a, mid - w), ImVec2(a, mid - rw), tc, 1.0f), dl->AddLine(ImVec2(a, mid + rw), ImVec2(a, mid + w), tc, 1.0f);
  }
  // Carved rail.
  auto rr = R(std::min(t0, t1) - 2, std::max(t0, t1) + 2, mid - rw * 0.5f, mid + rw * 0.5f);
  dl->AddRectFilled(rr.first, rr.second, darken(rail, 0.55f), rw * 0.5f);
  dl->AddRect(rr.first, rr.second, lighten(rail, 0.1f), rw * 0.5f, 0, 1.0f);
  // Fill from the bottom/left (or the centre for bipolar ranges).
  float from = pos(bipolar ? 0.5 : 0.0), at = pos(norm);
  auto fr = R(std::min(from, at), std::max(from, at), mid - rw * 0.5f + 1, mid + rw * 0.5f - 1);
  dl->AddRectFilled(fr.first, fr.second, fill, rw * 0.4f);
  // Handle.
  auto hr = R(at - hl * 0.5f, at + hl * 0.5f, mid - thick * 0.46f, mid + thick * 0.46f);
  ImU32 hc = hot ? lighten(handle, 0.12f) : handle;
  dl->AddRectFilled(ImVec2(hr.first.x, hr.first.y + 1), ImVec2(hr.second.x, hr.second.y + 1), IM_COL32(0, 0, 0, 110), s * 2);
  if (c.capStyle == HandleClassic) {
    dl->AddRectFilledMultiColor(hr.first, hr.second, lighten(hc, 0.35f), lighten(hc, 0.35f), darken(hc, 0.25f), darken(hc, 0.25f));
  } else {
    dl->AddRectFilled(hr.first, hr.second, hc, s * 2);
  }
  dl->AddRect(hr.first, hr.second, darken(hc, 0.5f), s * 2, 0, 1.0f);
  // Centre line of the handle in the value colour.
  if (vert)
    dl->AddLine(ImVec2(hr.first.x + 2, at), ImVec2(hr.second.x - 2, at), c.capStyle == HandleMixer ? IM_COL32(20, 20, 20, 255) : fill,
                std::max(1.5f, s));
  else
    dl->AddLine(ImVec2(at, hr.first.y + 2), ImVec2(at, hr.second.y - 2), fill, std::max(1.5f, s));
}

bool wheelAt(App& a, const PluginTarget& t, const View& v, const char* like, float x, float y, float w, float h, int param, const Palette& pal) {
  const FormCtl& c = v.c(like);
  ParamView pv = pview(a, t, param);
  if (!pv.d) return false;
  ImVec2 p0 = v.p(x, y), p1 = v.p(x + w, y + h);
  ImGui::PushID(param);
  ImGui::PushID(like);
  ImGui::SetCursorScreenPos(p0);
  ImGui::InvisibleButton("##w", ImVec2(std::max(1.f, p1.x - p0.x), std::max(1.f, p1.y - p0.y)));
  bool hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
  bool ch = dragBound(a, t, param, 180.f, false);
  if (ch) pv = pview(a, t, param);
  drawWheel(ImGui::GetWindowDrawList(), p0, p1, c, pal, pv.norm, pv.bipolar, hot, v.s);
  if (pui::paramLinked(a, t.pid(param))) ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p1.x - v.px(3), p0.y + v.px(3)), v.px(2), pal.accent2);
  ImGui::PopID();
  ImGui::PopID();
  return ch;
}

bool wheel(App& a, const PluginTarget& t, const View& v, const char* name, int param, const Palette& pal) {
  const FormCtl& c = v.c(name);
  return wheelAt(a, t, v, name, static_cast<float>(c.x), static_cast<float>(c.y) + v.dy0(c), static_cast<float>(c.w), static_cast<float>(c.h), param,
                 pal);
}

bool slider(App& a, const PluginTarget& t, const View& v, const char* name, int param, const Palette& pal) {
  const FormCtl& c = v.c(name);
  ParamView pv = pview(a, t, param);
  if (!pv.d) return false;
  ImVec2 p0 = v.p0(c), p1 = v.p1(c);
  bool vert = c.h >= c.w;
  ImGui::PushID(param);
  ImGui::PushID(name);
  ImGui::SetCursorScreenPos(p0);
  ImGui::InvisibleButton("##s", ImVec2(p1.x - p0.x, p1.y - p0.y));
  bool hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
  // Click on the rail jumps there (FL sliders), then drags.
  bool ch = false;
  if (ImGui::IsItemActivated() && !ImGui::GetIO().KeyCtrl && !ImGui::GetIO().KeySuper) {
    ImVec2 m = ImGui::GetIO().MousePos;
    float len = vert ? p1.y - p0.y : p1.x - p0.x;
    double n = vert ? 1.0 - (m.y - p0.y) / len : (m.x - p0.x) / len;
    double cur = pv.norm;
    float hl = std::clamp((vert ? p1.x - p0.x : p1.y - p0.y) * 0.6f, 6.f, 40.f) / len;
    if (std::fabs(n - cur) > hl) {
      pui::setParamValue(a, t, param, pv.d->fromNormalized(std::clamp(n, 0.0, 1.0)), true);
      ch = true;
    }
  }
  ch |= dragBound(a, t, param, vert ? (p1.y - p0.y) : (p1.x - p0.x), !vert);
  if (ch) pv = pview(a, t, param);
  drawSlider(ImGui::GetWindowDrawList(), p0, p1, c, pal, pv.norm, pv.bipolar, hot, v.s);
  ImGui::PopID();
  ImGui::PopID();
  return ch;
}

bool selector(App& a, const PluginTarget& t, const View& v, const char* name, int param, const Palette& pal, const CellFn& draw, int cells,
              int valueOffset) {
  const FormCtl& c = v.c(name);
  ParamView pv = pview(a, t, param);
  if (!pv.d) return false;
  int cols = std::max(1, c.cols), rows = std::max(1, c.rows);
  if (cells <= 0) cells = cols * rows;
  ImVec2 p0 = v.p0(c), p1 = v.p1(c);
  float cw = (p1.x - p0.x) / cols, chh = (p1.y - p0.y) / rows;
  int cur = static_cast<int>(std::lround(pv.v)) - valueOffset;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImGui::PushID(param);
  ImGui::PushID(name);
  ImGui::SetCursorScreenPos(p0);
  ImGui::InvisibleButton("##sel", ImVec2(p1.x - p0.x, p1.y - p0.y));
  bool ch = false;
  bool hov = ImGui::IsItemHovered();
  ImVec2 m = ImGui::GetIO().MousePos;
  int hoverCell = -1;
  if (hov || ImGui::IsItemActive()) {
    int cx = std::clamp(static_cast<int>((m.x - p0.x) / cw), 0, cols - 1), cy = std::clamp(static_cast<int>((m.y - p0.y) / chh), 0, rows - 1);
    hoverCell = cy * cols + cx;
    if (hoverCell >= cells) hoverCell = -1;
  }
  if (ImGui::IsItemActive() && hoverCell >= 0 && hoverCell != cur) {  // click or slide-select
    pui::setParamValue(a, t, param, hoverCell + valueOffset, true);
    cur = hoverCell;
    ch = true;
  }
  if (ImGui::IsItemDeactivated()) a.doc.endGesture();
  if (hov && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup("##selm");
  if (ImGui::BeginPopup("##selm")) {
    if (ImGui::MenuItem(i18n::tr("Reset"))) pui::setParamValue(a, t, param, pv.d->def, false);
    ImGui::EndPopup();
  }
  if (hov) app().setHint(pv.d->name + ": " + pv.d->format(cur + valueOffset));
  dl->AddRectFilled(p0, p1, c.color >= 0 ? col(c.color) : pal.well, v.px(2));
  for (int k = 0; k < cells; ++k) {
    int cx = k % cols, cy = k / cols;
    ImVec2 a0(p0.x + cx * cw, p0.y + cy * chh), a1(a0.x + cw, a0.y + chh);
    bool sel = k == cur;
    ImVec2 i0(a0.x + 1, a0.y + 1), i1(a1.x - 1, a1.y - 1);
    if (sel) {
      dl->AddRectFilled(i0, i1, theme::withAlpha(pal.fill, 0.28f), v.px(2));
      dl->AddRect(i0, i1, pal.fill, v.px(2), 0, std::max(1.f, v.s * 0.8f));
    } else if (k == hoverCell) {
      dl->AddRectFilled(i0, i1, theme::withAlpha(pal.text, 0.08f), v.px(2));
    }
    if (draw) draw(dl, k, i0, i1, sel);
  }
  dl->AddRect(p0, p1, darken(pal.panel, 0.4f), v.px(2), 0, 1.0f);
  ImGui::PopID();
  ImGui::PopID();
  return ch;
}

bool textSelector(App& a, const PluginTarget& t, const View& v, const char* name, int param, const Palette& pal, const char* const* labels,
                  int cells, int valueOffset) {
  float fs = std::clamp(v.px(8.5f), 10.f, 14.f);
  return selector(
      a, t, v, name, param, pal,
      [&](ImDrawList* dl, int k, ImVec2 p0, ImVec2 p1, bool sel) { textAt(dl, p0, p1, sel ? pal.text : pal.textDim, labels[k], fs, 0); }, cells,
      valueOffset);
}

bool check(App& a, const PluginTarget& t, const View& v, const char* name, int param, const Palette& pal, const char* text) {
  const FormCtl& c = v.c(name);
  ParamView pv = pview(a, t, param);
  if (!pv.d) return false;
  ImVec2 p0 = v.p0(c), p1 = v.p1(c);
  ImGui::PushID(param);
  ImGui::PushID(name);
  ImGui::SetCursorScreenPos(p0);
  ImGui::InvisibleButton("##c", ImVec2(p1.x - p0.x, p1.y - p0.y));
  bool on = pv.v >= 0.5;
  bool ch = false;
  if (ImGui::IsItemClicked()) {
    pui::setParamValue(a, t, param, on ? 0.0 : 1.0, false);
    on = !on;
    ch = true;
  }
  if (ImGui::IsItemHovered()) app().setHint(pv.d->name + ": " + (on ? "on" : "off"));
  ImDrawList* dl = ImGui::GetWindowDrawList();
  float h = p1.y - p0.y, led = std::min(h * 0.62f, v.px(10));
  ImVec2 l0(p0.x + v.px(2), (p0.y + p1.y - led) * 0.5f), l1(l0.x + led, l0.y + led);
  dl->AddRectFilled(l0, l1, on ? pal.fill : darken(pal.panel, 0.45f), v.px(1.5f));
  if (on) dl->AddRectFilled(ImVec2(l0.x - 1.5f, l0.y - 1.5f), ImVec2(l1.x + 1.5f, l1.y + 1.5f), theme::withAlpha(pal.fill, 0.25f), v.px(2));
  dl->AddRect(l0, l1, darken(pal.panel, 0.6f), v.px(1.5f), 0, 1.0f);
  if (text && *text) textAt(dl, ImVec2(l1.x + v.px(3), p0.y), p1, on ? pal.text : pal.textDim, text, std::clamp(v.px(8.5f), 10.f, 14.f), -1);
  ImGui::PopID();
  ImGui::PopID();
  return ch;
}

bool checkFree(const View& v, const char* name, const Palette& pal, bool on, const char* text, const char* hint) {
  const FormCtl& c = v.c(name);
  ImVec2 p0 = v.p0(c), p1 = v.p1(c);
  ImGui::PushID(name);
  ImGui::SetCursorScreenPos(p0);
  ImGui::InvisibleButton("##cf", ImVec2(p1.x - p0.x, p1.y - p0.y));
  bool clicked = ImGui::IsItemClicked();
  tooltipHint(hint);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  float h = p1.y - p0.y, led = std::min(h * 0.62f, v.px(10));
  ImVec2 l0(p0.x + v.px(2), (p0.y + p1.y - led) * 0.5f), l1(l0.x + led, l0.y + led);
  dl->AddRectFilled(l0, l1, on ? pal.fill : darken(pal.panel, 0.45f), v.px(1.5f));
  dl->AddRect(l0, l1, darken(pal.panel, 0.6f), v.px(1.5f), 0, 1.0f);
  if (text && *text) textAt(dl, ImVec2(l1.x + v.px(3), p0.y), p1, on ? pal.text : pal.textDim, text, std::clamp(v.px(8.5f), 10.f, 14.f), -1);
  ImGui::PopID();
  return clicked;
}

bool digi(App& a, const PluginTarget& t, const View& v, const char* name, int param, const Palette& pal, const std::function<std::string(double)>& fmt) {
  const FormCtl& c = v.c(name);
  ParamView pv = pview(a, t, param);
  if (!pv.d) return false;
  ImVec2 p0 = v.p0(c), p1 = v.p1(c);
  ImGui::PushID(param);
  ImGui::PushID(name);
  ImGui::SetCursorScreenPos(p0);
  ImGui::InvisibleButton("##d", ImVec2(p1.x - p0.x, p1.y - p0.y));
  bool ch = dragBound(a, t, param, 120.f, false);
  if (ch) pv = pview(a, t, param);
  std::string s = fmt ? fmt(pv.v) : pv.d->format(pv.v);
  lcd(ImGui::GetWindowDrawList(), p0, p1, pal, s.c_str(), 0);
  ImGui::PopID();
  ImGui::PopID();
  return ch;
}

bool combo(App& a, const PluginTarget& t, const View& v, const char* name, int param, const Palette& pal) {
  const FormCtl& c = v.c(name);
  ParamView pv = pview(a, t, param);
  if (!pv.d) return false;
  ImVec2 p0 = v.p0(c), p1 = v.p1(c);
  ImGui::PushID(param);
  ImGui::PushID(name);
  ImGui::SetCursorScreenPos(p0);
  bool clicked = ImGui::InvisibleButton("##cb", ImVec2(p1.x - p0.x, p1.y - p0.y));
  bool hov = ImGui::IsItemHovered();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImU32 bgc = c.color >= 0 ? col(c.color) : pal.well;
  dl->AddRectFilled(p0, p1, hov ? lighten(bgc, 0.08f) : bgc, v.px(2));
  dl->AddRect(p0, p1, darken(bgc, 0.4f), v.px(2), 0, 1.0f);
  std::string s = pv.d->format(pv.v);
  float h = p1.y - p0.y;
  textAt(dl, ImVec2(p0.x + v.px(3), p0.y), ImVec2(p1.x - h, p1.y), pal.text, s.c_str(), std::clamp(h * 0.62f, 10.f, 14.f), -1);
  ImVec2 ac(p1.x - h * 0.5f, (p0.y + p1.y) * 0.5f);
  float aw = h * 0.22f;
  dl->AddTriangleFilled(ImVec2(ac.x - aw, ac.y - aw * 0.5f), ImVec2(ac.x + aw, ac.y - aw * 0.5f), ImVec2(ac.x, ac.y + aw * 0.7f), pal.textDim);
  if (hov) app().setHint(pv.d->name + ": " + s);
  bool ch = false;
  if (clicked) ImGui::OpenPopup("##cbp");
  if (ImGui::BeginPopup("##cbp")) {
    for (size_t k = 0; k < pv.d->choices.size(); ++k)
      if (ImGui::MenuItem(pv.d->choices[k].c_str(), nullptr, static_cast<int>(std::lround(pv.v)) == static_cast<int>(k))) {
        pui::setParamValue(a, t, param, static_cast<double>(k), false);
        ch = true;
      }
    ImGui::EndPopup();
  }
  ImGui::PopID();
  ImGui::PopID();
  return ch;
}

bool comboFree(const View& v, const char* name, const Palette& pal, const char* const* items, int count, int& value, const char* hint) {
  const FormCtl& c = v.c(name);
  ImVec2 p0 = v.p0(c), p1 = v.p1(c);
  ImGui::PushID(name);
  ImGui::SetCursorScreenPos(p0);
  bool clicked = ImGui::InvisibleButton("##cf", ImVec2(p1.x - p0.x, p1.y - p0.y));
  bool hov = ImGui::IsItemHovered();
  tooltipHint(hint);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImU32 bgc = c.color >= 0 ? col(c.color) : pal.well;
  dl->AddRectFilled(p0, p1, hov ? lighten(bgc, 0.08f) : bgc, v.px(2));
  dl->AddRect(p0, p1, darken(bgc, 0.4f), v.px(2), 0, 1.0f);
  float h = p1.y - p0.y;
  const char* cur = value >= 0 && value < count ? items[value] : "";
  textAt(dl, ImVec2(p0.x + v.px(3), p0.y), ImVec2(p1.x - h, p1.y), pal.text, cur, std::clamp(h * 0.62f, 10.f, 14.f), -1);
  ImVec2 ac(p1.x - h * 0.5f, (p0.y + p1.y) * 0.5f);
  float aw = h * 0.22f;
  dl->AddTriangleFilled(ImVec2(ac.x - aw, ac.y - aw * 0.5f), ImVec2(ac.x + aw, ac.y - aw * 0.5f), ImVec2(ac.x, ac.y + aw * 0.7f), pal.textDim);
  bool ch = false;
  if (clicked) ImGui::OpenPopup("##cfp");
  if (ImGui::BeginPopup("##cfp")) {
    for (int k = 0; k < count; ++k)
      if (ImGui::MenuItem(items[k], nullptr, value == k)) value = k, ch = true;
    ImGui::EndPopup();
  }
  ImGui::PopID();
  return ch;
}

bool buttonAt(const View& v, const char* id, float x, float y, float w, float h, const Palette& pal, bool on, const char* text,
              const std::function<void(ImDrawList*, ImVec2, ImVec2, ImU32)>& glyph, const char* hint) {
  ImVec2 p0 = v.p(x, y), p1 = v.p(x + w, y + h);
  ImGui::PushID(id);
  ImGui::SetCursorScreenPos(p0);
  bool clicked = ImGui::InvisibleButton("##b", ImVec2(std::max(1.f, p1.x - p0.x), std::max(1.f, p1.y - p0.y)));
  bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
  tooltipHint(hint);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImU32 base = on ? theme::mix(pal.raised, pal.fill, 0.45f) : pal.raised;
  if (act) base = darken(base, 0.15f);
  else if (hov) base = lighten(base, 0.1f);
  dl->AddRectFilled(p0, p1, base, v.px(2.5f));
  dl->AddRect(p0, p1, on ? pal.fill : darken(pal.raised, 0.45f), v.px(2.5f), 0, 1.0f);
  ImU32 fg = on ? IM_COL32(255, 255, 255, 255) : pal.text;
  if (glyph) glyph(dl, ImVec2(p0.x + v.px(3), p0.y + v.px(3)), ImVec2(p1.x - v.px(3), p1.y - v.px(3)), fg);
  if (text && *text) textAt(dl, p0, p1, fg, text, std::clamp((p1.y - p0.y) * 0.5f, 10.f, 14.f), 0);
  ImGui::PopID();
  return clicked;
}

bool button(const View& v, const char* name, const Palette& pal, bool on, const char* text,
            const std::function<void(ImDrawList*, ImVec2, ImVec2, ImU32)>& glyph, const char* hint) {
  const FormCtl& c = v.c(name);
  return buttonAt(v, name, static_cast<float>(c.x), static_cast<float>(c.y) + v.dy0(c), static_cast<float>(c.w), static_cast<float>(c.h) + v.dy1(c) - v.dy0(c),
                  pal, on, text, glyph, hint);
}

// ------------------------------------------------------------------ glyphs
void glyphMenu(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 c) {
  float h = b.y - a.y;
  for (int k = 0; k < 3; ++k) {
    float y = a.y + h * (0.25f + 0.25f * k);
    dl->AddLine(ImVec2(a.x + 1, y), ImVec2(b.x - 1, y), c, 1.5f);
  }
}
void glyphSnow(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 c) {
  ImVec2 m((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
  float r = std::min(b.x - a.x, b.y - a.y) * 0.45f;
  for (int k = 0; k < 3; ++k) {
    float an = kKitPi * k / 3;
    dl->AddLine(ImVec2(m.x - std::cos(an) * r, m.y - std::sin(an) * r), ImVec2(m.x + std::cos(an) * r, m.y + std::sin(an) * r), c, 1.3f);
  }
}
void glyphMagnet(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 c) {
  ImVec2 m((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
  float r = std::min(b.x - a.x, b.y - a.y) * 0.38f;
  dl->PathArcTo(ImVec2(m.x, m.y), r, 0, kKitPi, 12);
  dl->PathStroke(c, 0, 2.f);
  dl->AddLine(ImVec2(m.x - r, m.y), ImVec2(m.x - r, m.y - r), c, 2.f);
  dl->AddLine(ImVec2(m.x + r, m.y), ImVec2(m.x + r, m.y - r), c, 2.f);
}
void glyphSteps(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 c) {
  float w = b.x - a.x, h = b.y - a.y;
  ImVec2 pts[7] = {ImVec2(a.x, b.y - h * 0.15f), ImVec2(a.x + w * 0.33f, b.y - h * 0.15f), ImVec2(a.x + w * 0.33f, a.y + h * 0.5f),
                   ImVec2(a.x + w * 0.66f, a.y + h * 0.5f), ImVec2(a.x + w * 0.66f, a.y + h * 0.15f), ImVec2(b.x, a.y + h * 0.15f), ImVec2(b.x, a.y + h * 0.15f)};
  dl->AddPolyline(pts, 6, c, 0, 1.5f);
}
void glyphSlide(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 c) {
  float m = (a.y + b.y) * 0.5f;
  dl->AddLine(ImVec2(a.x, m), ImVec2(b.x, m), c, 1.5f);
  dl->AddTriangleFilled(ImVec2(b.x, m), ImVec2(b.x - 4, m - 3.5f), ImVec2(b.x - 4, m + 3.5f), c);
  dl->AddCircleFilled(ImVec2(a.x + 3, m), 2.5f, c);
}
void glyphWave(ImDrawList* dl, int shape, ImVec2 a, ImVec2 b, ImU32 c, float th) {
  ImVec2 pts[33];
  float w = b.x - a.x, h = b.y - a.y, m = (a.y + b.y) * 0.5f;
  for (int i = 0; i <= 32; ++i) {
    float x = i / 32.f, y;
    switch (shape) {
      case 1: y = x < 0.25f ? 4 * x : x < 0.75f ? 2 - 4 * x : 4 * x - 4; break;
      case 2: y = x < 0.5f ? 1.f : -1.f; break;
      case 3: y = 1 - 2 * x; break;
      default: y = std::sin(2 * kKitPi * x); break;
    }
    pts[i] = ImVec2(a.x + x * w, m - y * h * 0.42f);
  }
  if (shape == 2) {
    ImVec2 q[6] = {ImVec2(a.x, m - h * 0.42f), ImVec2(a.x + w * 0.5f, m - h * 0.42f), ImVec2(a.x + w * 0.5f, m + h * 0.42f),
                   ImVec2(b.x, m + h * 0.42f), ImVec2(b.x, m + h * 0.42f), ImVec2(b.x, m + h * 0.42f)};
    dl->AddPolyline(q, 4, c, 0, th);
    return;
  }
  dl->AddPolyline(pts, 33, c, 0, th);
}


}  // namespace gl::skin
