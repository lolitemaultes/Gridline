// Gridline — rack skin, J-19 kit: skin scope (palette mapping for pui/ImGui
// widgets), backdrop, section wells, caption pills, value strips, wheel
// modules, bound toggles / drop-downs / icon selectors, step grids, X/Y pads,
// envelope editors, keyboard strips, LED meters. All vector-drawn in-house.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <string>
#include <unordered_map>
#include "../core/Params.h"
#include "App.h"
#include "PluginSkin.h"
#include "Theme.h"

namespace gl::skin {

namespace {

ImU32 lerpCol(ImU32 a, ImU32 b, float t) { return theme::mix(a, b, std::clamp(t, 0.0f, 1.0f)); }

struct Bound {
  ParamId pid;
  ParamDesc d;
  double val = 0;
  bool ok = false;
};

Bound bind(App& a, const PluginTarget& t, int param) {
  Bound b;
  if (param < 0) return b;
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

const Skin* gActive[8];
int gDepth = 0;

theme::Palette paletteFor(const Skin& k, const theme::Palette& base) {
  theme::Palette p = base;
  p.surface950 = k.lcdBg;
  p.surface900 = k.panelLo;
  p.surface800 = k.well ? k.well : lerpCol(k.panel, k.bevel, 0.4f);
  p.surface700 = lighten(p.surface800, 0.08f);
  p.surface600 = lighten(p.surface800, 0.16f);
  p.line = darken(k.panel, 0.35f);
  p.lineStrong = lighten(k.panel, 0.18f);
  p.text = k.text;
  p.textDim = k.textDim;
  p.textFaint = lerpCol(k.textDim, k.panel, 0.35f);
  p.paper = lighten(k.text, 0.45f);
  p.ink = darken(k.panel, 0.75f);
  p.accent = k.fill;
  p.stepOff = lighten(k.panelLo, 0.06f);
  p.stepOffAlt = lighten(k.panelLo, 0.12f);
  p.stepOn = k.fill;
  p.gridBar = k.graphGridStrong;
  p.gridBeat = lerpCol(k.graphGrid, k.graphGridStrong, 0.5f);
  p.gridStep = k.graphGrid;
  p.playhead = k.fill;
  return p;
}

std::unordered_map<uint64_t, float>& backdropHeights() {
  static std::unordered_map<uint64_t, float> m;
  return m;
}

}  // namespace

const Skin* active() { return gDepth > 0 && gDepth <= 8 ? gActive[gDepth - 1] : nullptr; }

Scope::Scope(const Skin& k) {
  if (gDepth < 8) gActive[gDepth] = &k;
  ++gDepth;
  theme::pushPalette(paletteFor(k, theme::pal()));
  const theme::Palette& P = theme::pal();
  ImGui::PushStyleColor(ImGuiCol_Text, theme::vec(k.text));
  ImGui::PushStyleColor(ImGuiCol_TextDisabled, theme::vec(k.textDim));
  ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::vec(IM_COL32(0, 0, 0, 0)));
  ImGui::PushStyleColor(ImGuiCol_Border, theme::vec(darken(k.panel, 0.4f)));
  ImGui::PushStyleColor(ImGuiCol_FrameBg, theme::vec(k.lcdBg));
  ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, theme::vec(lighten(k.lcdBg, 0.08f)));
  ImGui::PushStyleColor(ImGuiCol_FrameBgActive, theme::vec(lighten(k.lcdBg, 0.12f)));
  ImGui::PushStyleColor(ImGuiCol_Button, theme::vec(P.surface700));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::vec(P.surface600));
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme::vec(theme::withAlpha(k.fill, 0.5f)));
  ImGui::PushStyleColor(ImGuiCol_Header, theme::vec(theme::withAlpha(k.fill, 0.30f)));
  ImGui::PushStyleColor(ImGuiCol_HeaderHovered, theme::vec(theme::withAlpha(k.fill, 0.22f)));
  ImGui::PushStyleColor(ImGuiCol_HeaderActive, theme::vec(theme::withAlpha(k.fill, 0.40f)));
  ImGui::PushStyleColor(ImGuiCol_SliderGrab, theme::vec(k.fill));
  ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, theme::vec(k.fillPressed));
  ImGui::PushStyleColor(ImGuiCol_CheckMark, theme::vec(k.fill));
}

Scope::~Scope() {
  ImGui::PopStyleColor(16);
  theme::popPalette();
  if (gDepth > 0) --gDepth;
}

Backdrop::Backdrop(const Skin& k, float width, uint64_t key, float pad)
    : scope_(k), o_(ImGui::GetCursorScreenPos()), w_(width), pad_(pad), key_(key) {
  auto it = backdropHeights().find(key);
  float h = it != backdropHeights().end() ? it->second : 0.0f;
  if (h > 0) panel(ImGui::GetWindowDrawList(), ImVec2(o_.x - pad_, o_.y - pad_), ImVec2(o_.x + w_ + pad_, o_.y + h + pad_), k, 4);
}

Backdrop::~Backdrop() {
  float h = ImGui::GetCursorScreenPos().y - o_.y - ImGui::GetStyle().ItemSpacing.y;
  backdropHeights()[key_] = std::max(0.0f, h);
}

void well(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const Skin& k, float rounding) {
  ImU32 w = k.wellC();
  dl->AddRectFilled(p0, p1, w, rounding);
  dl->AddRect(p0, p1, theme::withAlpha(darken(k.panel, 0.45f), 0.6f), rounding, 0, 1.0f);
  dl->AddLine(ImVec2(p0.x + rounding, p0.y + 0.5f), ImVec2(p1.x - rounding, p0.y + 0.5f), theme::withAlpha(lighten(w, 0.14f), 0.8f), 1.0f);
}

float text(ImDrawList* dl, ImVec2 p, ImU32 col, const char* s, float size) {
  ImGui::PushFont(app().fontMonoBold, size);
  float w = ImGui::CalcTextSize(s).x;
  dl->AddText(ImVec2(std::round(p.x), std::round(p.y)), col, s);
  ImGui::PopFont();
  return w;
}

void pill(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const Skin& k, const char* s, float font) {
  float h = p1.y - p0.y;
  dl->AddRectFilled(p0, p1, k.pillBgC(), std::min(h * 0.5f, 6.0f));
  if (!s || !*s) return;
  float fs = font > 0 ? font : std::clamp(h * 0.66f, 9.0f, 13.0f);
  ImGui::PushFont(app().fontMonoBold, fs);
  ImVec2 ts = ImGui::CalcTextSize(s);
  dl->PushClipRect(p0, p1, true);
  dl->AddText(ImVec2(std::round(std::max(p0.x + 3, (p0.x + p1.x - ts.x) * 0.5f)), std::round((p0.y + p1.y - ts.y) * 0.5f)), k.pillTextC(), s);
  dl->PopClipRect();
  ImGui::PopFont();
}

void valueStrip(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const Skin& k, const char* s, float font) {
  float h = p1.y - p0.y;
  dl->AddRectFilled(p0, p1, k.valueBgC(), 2);
  if (!s || !*s) return;
  float fs = font > 0 ? font : std::clamp(h * 0.72f, 9.0f, 13.0f);
  ImGui::PushFont(app().fontMono, fs);
  ImVec2 ts = ImGui::CalcTextSize(s);
  dl->PushClipRect(p0, p1, true);
  dl->AddText(ImVec2(std::round(std::max(p0.x + 3, (p0.x + p1.x - ts.x) * 0.5f)), std::round((p0.y + p1.y - ts.y) * 0.5f)), k.valueTextC(), s);
  dl->PopClipRect();
  ImGui::PopFont();
}

void groupCaption(ImDrawList* dl, const Form& F, float x0, float x1, float y, const Skin& k, const char* s) {
  float fs = F.font(8.0f);
  ImVec2 c = F.p((x0 + x1) * 0.5f, y + 4);
  dl->AddLine(F.p(x0, y + 4), F.p(x1, y + 4), darken(k.panel, 0.25f), 1.0f);
  ImGui::PushFont(app().fontMonoBold, fs);
  ImVec2 ts = ImGui::CalcTextSize(s);
  float top = c.y - ts.y * 0.5f;
  dl->AddRectFilled(ImVec2(c.x - ts.x * 0.5f - 4, top), ImVec2(c.x + ts.x * 0.5f + 4, top + ts.y), k.panel);
  dl->AddText(ImVec2(std::round(c.x - ts.x * 0.5f), std::round(top)), k.textDim, s);
  ImGui::PopFont();
}

bool labeledWheel(App& a, const PluginTarget& t, int param, const Form& F, float x, float y, float size, const Skin& k, const char* lbl, int flags) {
  bool ch = wheel(a, t, param, F.p(x, y), size * F.s, k, flags);
  if (lbl && *lbl) label(ImGui::GetWindowDrawList(), F.p(x + size * 0.5f, 0).x, F.p(0, y + size + 1.5f).y, lbl, k.textDim, F.font(8.0f));
  return ch;
}

bool paramBox(App& a, const PluginTarget& t, int param, const Form& F, float x, float y, float w, float h, const Skin& k, const char* caption,
              const char* value, int flags) {
  Bound b = bind(a, t, param);
  if (!b.ok) return false;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  well(dl, F.p(x, y), F.p(x + w, y + h), k, 4 * F.s);
  float ws = h - 5;  // wheel square
  bool ch = wheel(a, t, param, F.p(x + 2, y + 3), ws * F.s, k, flags);
  b = bind(a, t, param);
  float cx = x + ws + 6;
  std::string cap = caption ? std::string(caption) : b.d.name;
  for (char& c : cap) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  float pillH = std::min(16.0f, h * 0.36f);
  pill(dl, F.p(cx, y), F.p(x + w, y + pillH), k, cap.c_str(), F.font(8.5f));
  std::string v = value ? std::string(value) : b.d.format(b.val);
  float vy = y + h * 0.58f;
  valueStrip(dl, F.p(cx + 1, vy), F.p(x + w - 3, std::min(y + h - 3, vy + 14)), k, v.c_str(), F.font(8.5f));
  return ch;
}

bool toggle(App& a, const PluginTarget& t, int param, ImVec2 tl, ImVec2 size, const Skin& k, const char* s, float font) {
  Bound b = bind(a, t, param);
  if (!b.ok) return false;
  ImGui::PushID(param);
  bool isOn = b.val >= 0.5 * (b.d.min + b.d.max) && b.val > b.d.min;
  std::string txt = s ? std::string(s) : b.d.name;
  bool pressed = button("##tg", tl, size, k, txt.c_str(), isOn, font);
  if (ImGui::IsItemHovered()) a.setHint(b.d.name + ": " + (isOn ? "on" : "off") + "   (click to toggle)");
  if (pressed) a.doc.setParam(b.pid, isOn ? b.d.min : b.d.max, false);
  ImGui::PopID();
  return pressed;
}

bool menuBox(App& a, const PluginTarget& t, int param, ImVec2 tl, ImVec2 size, const Skin& k, float font) {
  Bound b = bind(a, t, param);
  if (!b.ok) return false;
  ImGui::PushID(param);
  ImGui::SetCursorScreenPos(tl);
  bool pressed = ImGui::InvisibleButton("##mb", size);
  bool hov = ImGui::IsItemHovered();
  ImGuiIO& io = ImGui::GetIO();
  bool changed = false;
  int n = b.d.choices.empty() ? static_cast<int>(b.d.max - b.d.min) + 1 : static_cast<int>(b.d.choices.size());
  n = std::max(1, n);
  int cur = std::clamp(static_cast<int>(std::lround(b.val - b.d.min)), 0, n - 1);
  if (hov && io.MouseWheel != 0) {
    int sel = std::clamp(cur - static_cast<int>(io.MouseWheel), 0, n - 1);
    if (sel != cur) a.doc.setParam(b.pid, b.d.min + sel, false), cur = sel, changed = true;
  }
  if (hov) a.setHint(b.d.name + ": " + b.d.format(b.d.min + cur) + "   (click for the list · wheel steps)");
  if (pressed) ImGui::OpenPopup("##mbpop");
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 p1(tl.x + size.x, tl.y + size.y);
  dl->AddRectFilled(tl, p1, hov ? lighten(k.lcdBg, 0.06f) : k.lcdBg, 2);
  dl->AddRect(tl, p1, darken(k.panel, 0.6f), 2, 0, 1.0f);
  float fs = font > 0 ? font : std::clamp(size.y * 0.62f, 9.0f, 13.0f);
  std::string v = b.d.format(b.d.min + cur);
  ImGui::PushFont(app().fontMono, fs);
  ImVec2 ts = ImGui::CalcTextSize(v.c_str());
  float aw = std::min(size.y * 0.8f, 14.0f);
  dl->PushClipRect(tl, ImVec2(p1.x - aw, p1.y), true);
  dl->AddText(ImVec2(std::round(tl.x + 5), std::round((tl.y + p1.y - ts.y) * 0.5f)), k.lcdText, v.c_str());
  dl->PopClipRect();
  ImGui::PopFont();
  ImVec2 ac(p1.x - aw * 0.5f - 2, (tl.y + p1.y) * 0.5f);
  float r = std::min(4.0f, size.y * 0.18f);
  dl->AddTriangleFilled(ImVec2(ac.x - r, ac.y - r * 0.5f), ImVec2(ac.x + r, ac.y - r * 0.5f), ImVec2(ac.x, ac.y + r * 0.7f), k.textDim);
  if (ImGui::BeginPopup("##mbpop")) {
    for (int i = 0; i < n; ++i) {
      std::string nm = b.d.format(b.d.min + i);
      if (ImGui::MenuItem(nm.c_str(), nullptr, i == cur) && i != cur) a.doc.setParam(b.pid, b.d.min + i, false), changed = true;
    }
    ImGui::EndPopup();
  }
  ImGui::PopID();
  return changed;
}

bool iconSel(App& a, const PluginTarget& t, int param, ImVec2 tl, ImVec2 size, const Skin& k, int count,
             void (*drawIcon)(ImDrawList*, int, ImVec2, float, float, ImU32), bool horizontal) {
  Bound b = bind(a, t, param);
  if (!b.ok || count <= 0) return false;
  ImGui::PushID(param);
  ImGui::SetCursorScreenPos(tl);
  ImGui::InvisibleButton("##is", size);
  bool hov = ImGui::IsItemHovered();
  ImGuiIO& io = ImGui::GetIO();
  int cur = std::clamp(static_cast<int>(std::lround(b.val - b.d.min)), 0, count - 1);
  float cell = (horizontal ? size.x : size.y) / count;
  bool changed = false;
  int hovCell = -1;
  if (hov) {
    float off = horizontal ? io.MousePos.x - tl.x : io.MousePos.y - tl.y;
    hovCell = std::clamp(static_cast<int>(off / cell), 0, count - 1);
  }
  if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && hovCell >= 0 && hovCell != cur)
    a.doc.setParam(b.pid, b.d.min + hovCell, false), cur = hovCell, changed = true;
  if (hov && io.MouseWheel != 0) {
    int sel = std::clamp(cur - static_cast<int>(io.MouseWheel), 0, count - 1);
    if (sel != cur) a.doc.setParam(b.pid, b.d.min + sel, false), cur = sel, changed = true;
  }
  if (hov && hovCell >= 0) a.setHint(b.d.name + ": " + b.d.format(b.d.min + hovCell));
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 p1(tl.x + size.x, tl.y + size.y);
  dl->AddRectFilled(tl, p1, k.lcdBg, 3);
  dl->AddRect(tl, p1, darken(k.panel, 0.6f), 3, 0, 1.0f);
  for (int i = 0; i < count; ++i) {
    ImVec2 c0 = horizontal ? ImVec2(tl.x + cell * i, tl.y) : ImVec2(tl.x, tl.y + cell * i);
    ImVec2 c1 = horizontal ? ImVec2(c0.x + cell, p1.y) : ImVec2(p1.x, c0.y + cell);
    bool on = i == cur;
    if (on) dl->AddRectFilled(ImVec2(c0.x + 2, c0.y + 2), ImVec2(c1.x - 2, c1.y - 2), theme::withAlpha(k.select, 0.30f), 2);
    else if (i == hovCell) dl->AddRectFilled(ImVec2(c0.x + 2, c0.y + 2), ImVec2(c1.x - 2, c1.y - 2), theme::withAlpha(k.text, 0.08f), 2);
    if (drawIcon)
      drawIcon(dl, i, ImVec2((c0.x + c1.x) * 0.5f, (c0.y + c1.y) * 0.5f), (c1.x - c0.x) * 0.62f, (c1.y - c0.y) * 0.5f,
               on ? lighten(k.select, 0.35f) : k.textDim);
  }
  ImGui::PopID();
  return changed;
}

bool stepGrid(App& a, const PluginTarget& t, int first, int count, ImVec2 tl, ImVec2 size, const Skin& k, int stride, int current, int groupOf) {
  if (count <= 0) return false;
  ImGui::PushID(first + 77000);
  ImGui::SetCursorScreenPos(tl);
  ImGui::InvisibleButton("##sg", size);
  bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
  ImGuiIO& io = ImGui::GetIO();
  ImGuiStorage* st = ImGui::GetStateStorage();
  const ImGuiID kPaint = ImGui::GetID("paint");
  float cw = size.x / count;
  int hovCell = hov || act ? std::clamp(static_cast<int>((io.MousePos.x - tl.x) / cw), 0, count - 1) : -1;
  bool changed = false;
  Bound b0 = bind(a, t, first);
  bool toggles = b0.ok && b0.d.kind == ParamKind::Toggle;
  if (ImGui::IsItemActivated() && hovCell >= 0 && toggles) {
    Bound bc = bind(a, t, first + hovCell * stride);
    st->SetInt(kPaint, bc.ok && bc.val >= 0.5 ? 0 : 1);
  }
  if (act && hovCell >= 0) {
    Bound bc = bind(a, t, first + hovCell * stride);
    if (bc.ok) {
      double nv;
      if (toggles) {
        nv = st->GetInt(kPaint, 1) ? bc.d.max : bc.d.min;
      } else {
        float n = std::clamp(1.0f - (io.MousePos.y - tl.y) / size.y, 0.0f, 1.0f);
        nv = bc.d.fromNormalized(n);
      }
      if (std::fabs(nv - bc.val) > 1e-9) {
        a.doc.setParam(bc.pid, nv, !ImGui::IsItemActivated());
        changed = true;
      }
    }
  }
  if (hovCell >= 0) {
    Bound bc = bind(a, t, first + hovCell * stride);
    if (bc.ok) a.setHint(bc.d.name + ": " + bc.d.format(bc.val) + (toggles ? "   (click / drag to paint)" : "   (drag to draw levels)"));
  }
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(tl, ImVec2(tl.x + size.x, tl.y + size.y), k.panelLo, 3);
  for (int i = 0; i < count; ++i) {
    Bound bc = bind(a, t, first + i * stride);
    if (!bc.ok) continue;
    ImVec2 c0(tl.x + i * cw + 1.5f, tl.y + 1.5f), c1(tl.x + (i + 1) * cw - 1.5f, tl.y + size.y - 1.5f);
    bool alt = groupOf > 0 && (i / groupOf) % 2 == 1;
    ImU32 off = alt ? lighten(k.panelLo, 0.14f) : lighten(k.panelLo, 0.07f);
    if (toggles) {
      bool on = bc.val >= 0.5;
      dl->AddRectFilled(c0, c1, on ? k.fill : off, 2);
      if (on) dl->AddRectFilled(ImVec2(c0.x + 1, c0.y + 1), ImVec2(c1.x - 1, c0.y + (c1.y - c0.y) * 0.35f), theme::withAlpha(IM_COL32_WHITE, 0.18f), 2);
    } else {
      dl->AddRectFilled(c0, c1, off, 2);
      float n = static_cast<float>(bc.d.toNormalized(bc.val));
      dl->AddRectFilled(ImVec2(c0.x + 1, c1.y - (c1.y - c0.y) * n), ImVec2(c1.x - 1, c1.y), k.fill, 1);
    }
    if (i == current) dl->AddRect(c0, c1, lighten(k.text, 0.4f), 2, 0, 1.5f);
    else if (i == hovCell) dl->AddRect(c0, c1, theme::withAlpha(k.text, 0.5f), 2, 0, 1.0f);
  }
  ImGui::PopID();
  return changed;
}

bool xyPad(App& a, const PluginTarget& t, int px, int py, ImVec2 tl, ImVec2 size, const Skin& k, const char* lbl) {
  Bound bx = bind(a, t, px), by = bind(a, t, py);
  if (!bx.ok || !by.ok) return false;
  ImGui::PushID(px * 131 + py);
  ImGui::SetCursorScreenPos(tl);
  ImGui::InvisibleButton("##xy", size);
  bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
  ImGuiIO& io = ImGui::GetIO();
  bool changed = false;
  if (act) {
    float nx = std::clamp((io.MousePos.x - tl.x) / size.x, 0.0f, 1.0f), ny = std::clamp(1.0f - (io.MousePos.y - tl.y) / size.y, 0.0f, 1.0f);
    bool merge = !ImGui::IsItemActivated();
    double vx = bx.d.fromNormalized(nx), vy = by.d.fromNormalized(ny);
    if (std::fabs(vx - bx.val) > 1e-12) a.doc.setParam(bx.pid, vx, merge), changed = true;
    if (std::fabs(vy - by.val) > 1e-12) a.doc.setParam(by.pid, vy, true), changed = true;
    bx = bind(a, t, px), by = bind(a, t, py);
  }
  if (hov || act) a.setHint(bx.d.name + ": " + bx.d.format(bx.val) + "  ·  " + by.d.name + ": " + by.d.format(by.val) + "   (drag)");
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 p1(tl.x + size.x, tl.y + size.y);
  graphWell(dl, tl, p1, k);
  for (int i = 1; i < 8; ++i) {
    float x = tl.x + size.x * i / 8, y = tl.y + size.y * i / 8;
    ImU32 c = i == 4 ? k.graphGridStrong : k.graphGrid;
    dl->AddLine(ImVec2(x, tl.y + 1), ImVec2(x, p1.y - 1), c);
    dl->AddLine(ImVec2(tl.x + 1, y), ImVec2(p1.x - 1, y), c);
  }
  float x = tl.x + static_cast<float>(bx.d.toNormalized(bx.val)) * size.x;
  float y = tl.y + (1 - static_cast<float>(by.d.toNormalized(by.val))) * size.y;
  dl->AddLine(ImVec2(x, tl.y + 1), ImVec2(x, p1.y - 1), theme::withAlpha(k.fill, 0.45f));
  dl->AddLine(ImVec2(tl.x + 1, y), ImVec2(p1.x - 1, y), theme::withAlpha(k.fill, 0.45f));
  float r = std::clamp(size.x * 0.035f, 5.0f, 9.0f);
  dl->AddCircleFilled(ImVec2(x, y), r + 3, theme::withAlpha(k.fill, act ? 0.35f : 0.2f), 24);
  dl->AddCircleFilled(ImVec2(x, y), r, act ? k.fillPressed : k.fill, 24);
  dl->AddCircle(ImVec2(x, y), r, lighten(k.fill, 0.4f), 24, 1.2f);
  if (lbl && *lbl) text(dl, ImVec2(tl.x + 5, tl.y + 4), k.graphText, lbl, 10.0f);
  ImGui::PopID();
  return changed;
}

bool envelope(App& a, const PluginTarget& t, const EnvParams& e, ImVec2 tl, ImVec2 size, const Skin& k) {
  // Segment widths follow each time knob's normalised position (as in the
  // reference envelope editors), so each handle drives exactly one knob.
  const int segs[5] = {e.del, e.att, e.hold, e.dec, e.rel};
  Bound bs[5];
  for (int i = 0; i < 5; ++i) bs[i] = bind(a, t, segs[i]);
  Bound bsus = bind(a, t, e.sus);
  Bound bamt = bind(a, t, e.amount);
  ImGui::PushID(e.att * 97 + e.rel + 31000);
  ImGui::SetCursorScreenPos(tl);
  ImGui::InvisibleButton("##env", size);
  bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
  ImGuiIO& io = ImGui::GetIO();
  ImGuiStorage* st = ImGui::GetStateStorage();
  const ImGuiID kGrab = ImGui::GetID("grab");
  const ImVec2 p1(tl.x + size.x, tl.y + size.y);
  const float padX = 6, padY = 8;
  const float w = size.x - padX * 2, h = size.y - padY * 2;
  const float x0 = tl.x + padX, yBot = tl.y + padY + h;
  int present = 0;
  for (const Bound& b : bs) present += b.ok ? 1 : 0;
  const float susW = bsus.ok ? w * 0.14f : 0.0f;
  const float segMax = present > 0 ? (w - susW) / static_cast<float>(present) : 0.0f;
  float amt = 1.0f;
  if (bamt.ok) {
    double n = bamt.d.toNormalized(bamt.val);
    amt = static_cast<float>(bamt.d.min < 0 ? std::fabs(n * 2 - 1) : n);
  }
  const float scaleH = h * std::max(0.08f, amt);
  const float susN = bsus.ok ? static_cast<float>(bsus.d.toNormalized(bsus.val)) : 1.0f;
  auto lvl = [&](float n) { return yBot - n * scaleH; };
  auto segW = [&](int i) { return bs[i].ok ? static_cast<float>(bs[i].d.toNormalized(bs[i].val)) * segMax : 0.0f; };
  float xs[7], ys[7];
  float x = x0;
  xs[0] = x, ys[0] = yBot;
  x += segW(0), xs[1] = x, ys[1] = yBot;
  x += segW(1), xs[2] = x, ys[2] = lvl(1);
  x += segW(2), xs[3] = x, ys[3] = lvl(1);
  x += segW(3), xs[4] = x, ys[4] = lvl(susN);
  x += susW, xs[5] = x, ys[5] = lvl(susN);
  x += segW(4), xs[6] = x, ys[6] = yBot;
  struct H {
    int pt, seg;
  };
  const H handles[5] = {{1, 0}, {2, 1}, {3, 2}, {4, 3}, {6, 4}};
  auto nearest = [&](float maxD) {
    float best = maxD;
    int pick = -1;
    for (const H& hh : handles) {
      if (!bs[hh.seg].ok) continue;
      float d = std::hypot(io.MousePos.x - xs[hh.pt], io.MousePos.y - ys[hh.pt]);
      if (d < best) best = d, pick = hh.seg;
    }
    if (pick < 0 && bsus.ok && std::fabs(io.MousePos.y - ys[4]) < 10 && io.MousePos.x >= xs[3] - 4 && io.MousePos.x <= xs[5] + 4) pick = 5;
    return pick;
  };
  int hot = hov && !act ? nearest(12.0f) : -1;
  if (ImGui::IsItemActivated()) st->SetInt(kGrab, nearest(1e9f));
  bool changed = false;
  if (act) {
    int g = st->GetInt(kGrab, -1);
    bool merge = !ImGui::IsItemActivated();
    if (g >= 0 && g < 5 && bs[g].ok && segMax > 0) {
      float prevX = xs[handles[g].pt] - segW(g);
      float n = std::clamp((io.MousePos.x - prevX) / segMax, 0.0f, 1.0f);
      double nv = bs[g].d.fromNormalized(n);
      if (std::fabs(nv - bs[g].val) > 1e-12) a.doc.setParam(bs[g].pid, nv, merge), changed = true;
      if (g == 3 && bsus.ok) {  // the decay handle also sets the sustain level
        double sv = bsus.d.fromNormalized(std::clamp((yBot - io.MousePos.y) / scaleH, 0.0f, 1.0f));
        if (std::fabs(sv - bsus.val) > 1e-12) a.doc.setParam(bsus.pid, sv, true), changed = true;
      }
    } else if (g == 5 && bsus.ok) {
      double sv = bsus.d.fromNormalized(std::clamp((yBot - io.MousePos.y) / scaleH, 0.0f, 1.0f));
      if (std::fabs(sv - bsus.val) > 1e-12) a.doc.setParam(bsus.pid, sv, merge), changed = true;
    }
    hot = g;
  }
  if (hot >= 0 && hot < 5 && bs[hot].ok) a.setHint(bs[hot].d.name + ": " + bs[hot].d.format(bs[hot].val) + "   (drag the handle)");
  else if (hot == 5 && bsus.ok) a.setHint(bsus.d.name + ": " + bsus.d.format(bsus.val) + "   (drag the level)");
  ImDrawList* dl = ImGui::GetWindowDrawList();
  graphWell(dl, tl, p1, k);
  for (int i = 1; i < 4; ++i) dl->AddLine(ImVec2(tl.x + 1, tl.y + size.y * i / 4), ImVec2(p1.x - 1, tl.y + size.y * i / 4), k.graphGrid);
  ImVec2 pts[7];
  for (int i = 0; i < 7; ++i) pts[i] = ImVec2(xs[i], ys[i]);
  for (int i = 0; i < 6; ++i)
    if (pts[i + 1].x > pts[i].x + 0.5f)
      dl->AddQuadFilled(ImVec2(pts[i].x, yBot), pts[i], pts[i + 1], ImVec2(pts[i + 1].x, yBot), theme::withAlpha(k.graphCurve, 0.14f));
  dl->AddPolyline(pts, 7, k.graphCurve, 0, 2.0f);
  for (const H& hh : handles) {
    if (!bs[hh.seg].ok) continue;
    bool on = hot == hh.seg;
    float r = on ? 5.0f : 4.0f;
    dl->AddCircleFilled(pts[hh.pt], r, on ? lighten(k.fill, 0.3f) : k.graphBg, 16);
    dl->AddCircle(pts[hh.pt], r, k.graphCurve, 16, 1.5f);
  }
  if (bsus.ok) {
    ImVec2 c((xs[4] + xs[5]) * 0.5f, ys[4]);
    dl->AddRectFilled(ImVec2(c.x - 3.5f, c.y - 3.5f), ImVec2(c.x + 3.5f, c.y + 3.5f), hot == 5 ? lighten(k.fill, 0.3f) : k.graphBg);
    dl->AddRect(ImVec2(c.x - 3.5f, c.y - 3.5f), ImVec2(c.x + 3.5f, c.y + 3.5f), k.graphCurve, 0, 0, 1.5f);
  }
  ImGui::PopID();
  return changed;
}

int keyboard(const char* id, ImVec2 tl, ImVec2 size, int lo, int hi, const Skin& k, const bool* lit, int markLo, int markHi, int root) {
  if (hi < lo) return -1;
  auto isBlack = [](int n) {
    int m = ((n % 12) + 12) % 12;
    return m == 1 || m == 3 || m == 6 || m == 8 || m == 10;
  };
  int whites = 0;
  for (int n = lo; n <= hi; ++n) whites += isBlack(n) ? 0 : 1;
  if (whites == 0) return -1;
  ImGui::SetCursorScreenPos(tl);
  ImGui::InvisibleButton(id, size);
  const bool hov = ImGui::IsItemHovered();
  const bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
  ImGuiIO& io = ImGui::GetIO();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float ww = size.x / whites, bw = ww * 0.62f, bh = size.y * 0.6f;
  const ImVec2 p1(tl.x + size.x, tl.y + size.y);
  dl->AddRectFilled(tl, p1, darken(k.panel, 0.6f), 2);
  auto keyRect = [&](int n, ImVec2& a0, ImVec2& a1) {
    int wi = 0;
    for (int m = lo; m < n; ++m) wi += isBlack(m) ? 0 : 1;
    if (!isBlack(n)) {
      a0 = ImVec2(tl.x + wi * ww, tl.y), a1 = ImVec2(tl.x + (wi + 1) * ww, p1.y);
    } else {
      float cx = tl.x + wi * ww;
      a0 = ImVec2(cx - bw * 0.5f, tl.y), a1 = ImVec2(cx + bw * 0.5f, tl.y + bh);
    }
  };
  int hovered = -1;
  if (hov)
    for (int pass = 0; pass < 2 && hovered < 0; ++pass)
      for (int n = lo; n <= hi; ++n) {
        if (isBlack(n) != (pass == 0)) continue;
        ImVec2 a0, a1;
        keyRect(n, a0, a1);
        if (io.MousePos.x >= a0.x && io.MousePos.x < a1.x && io.MousePos.y >= a0.y && io.MousePos.y < a1.y) {
          hovered = n;
          break;
        }
      }
  const ImU32 whiteC = lerpCol(k.text, IM_COL32(255, 255, 255, 255), 0.6f), blackC = darken(k.panel, 0.55f);
  for (int pass = 0; pass < 2; ++pass)
    for (int n = lo; n <= hi; ++n) {
      const bool black = isBlack(n);
      if (black != (pass == 1)) continue;
      ImVec2 a0, a1;
      keyRect(n, a0, a1);
      bool in = markLo >= 0 && n >= markLo && n <= markHi;
      bool on = lit && n >= 0 && n < 128 && lit[n];
      ImU32 c = black ? blackC : whiteC;
      if (in) c = lerpCol(c, k.fill, black ? 0.45f : 0.35f);
      if (on) c = k.fill;
      if (n == hovered) c = lighten(c, 0.15f);
      dl->AddRectFilled(ImVec2(a0.x + (black ? 0 : 0.5f), a0.y), ImVec2(a1.x - (black ? 0 : 0.5f), a1.y), c, black ? 1.5f : 2.0f,
                        ImDrawFlags_RoundCornersBottom);
      if (n == root) dl->AddRect(ImVec2(a0.x + 1, a0.y + 1), ImVec2(a1.x - 1, a1.y - 1), k.accent2C(), 2, 0, 2.0f);
      if (!black && n % 12 == 0 && a1.x - a0.x > 9) {
        char buf[8];
        std::snprintf(buf, sizeof buf, "%d", n / 12 - 1);
        ImGui::PushFont(app().fontMono, std::clamp(ww * 0.7f, 8.0f, 10.0f));
        ImVec2 ts = ImGui::CalcTextSize(buf);
        dl->AddText(ImVec2((a0.x + a1.x - ts.x) * 0.5f, a1.y - ts.y - 2), darken(k.panel, 0.3f), buf);
        ImGui::PopFont();
      }
    }
  return clicked ? hovered : -1;
}

void meter(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const Skin& k, float db, float rangeDb, bool reduction) {
  dl->AddRectFilled(p0, p1, k.lcdBg, 2);
  const float h = p1.y - p0.y - 2;
  const int segs = std::max(4, static_cast<int>(h / 4));
  const float sh = h / segs;
  const float f = reduction ? std::clamp(-db / rangeDb, 0.0f, 1.0f) : std::clamp((db + rangeDb) / rangeDb, 0.0f, 1.0f);
  const int lit = static_cast<int>(std::round(f * segs));
  for (int i = 0; i < segs; ++i) {
    float y0, y1;
    if (reduction) y0 = p0.y + 1 + i * sh, y1 = y0 + sh - 1;
    else y1 = p1.y - 1 - i * sh, y0 = y1 - sh + 1;
    float lvl = static_cast<float>(i) / segs;
    ImU32 c = reduction ? hex(0xE5484D) : lvl > 0.92f ? hex(0xE5484D) : lvl > 0.75f ? hex(0xE8C547) : k.fill;
    dl->AddRectFilled(ImVec2(p0.x + 1, y0), ImVec2(p1.x - 1, y1), i < lit ? c : theme::withAlpha(c, 0.12f));
  }
}

void ledDot(ImDrawList* dl, ImVec2 c, float r, bool on, const Skin& k, ImU32 onCol) {
  ImU32 col = onCol ? onCol : k.fill;
  if (on) dl->AddCircleFilled(c, r * 1.9f, theme::withAlpha(col, 0.22f), 16);
  dl->AddCircleFilled(c, r, on ? col : k.lcdBg, 16);
  dl->AddCircle(c, r, on ? lighten(col, 0.35f) : darken(k.panel, 0.5f), 16, 1.0f);
}

}  // namespace gl::skin
