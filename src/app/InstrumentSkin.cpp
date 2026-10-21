// Gridline — instrument editor skin (see InstrumentSkin.h).
#include "KeyLight.h"
#include "InstrumentSkin.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include "../../third_party/imgui/imgui_internal.h"
#include "App.h"
#include "Theme.h"

namespace gl::iskin {

namespace {
constexpr float kPi = 3.14159265f;

void arc(ImDrawList* dl, ImVec2 c, float r, float a0, float a1, ImU32 col, float th) {
  if (a1 - a0 < 0.002f) return;
  int segs = std::max(6, static_cast<int>((a1 - a0) * r * 0.5f));
  dl->PathArcTo(c, r, a0, a1, segs);
  dl->PathStroke(col, 0, th);
}

struct ParamView {
  std::optional<ParamDesc> d;
  double v = 0;
  bool ok = false;
};
ParamView view(App& a, const PluginTarget& t, int param) {
  ParamView pv;
  pv.d = describeParam(a.doc.song(), t.pid(param));
  auto v = getParam(a.doc.song(), t.pid(param));
  pv.ok = pv.d.has_value() && v.has_value();
  pv.v = v.value_or(0.0);
  return pv;
}

ImFont* fontFor(bool mono, bool bold) {
  App& a = app();
  if (mono) return bold ? a.fontMonoBold : a.fontMono;
  return bold ? a.fontBold : a.fontRegular;
}
}  // namespace

ImU32 rgb(uint32_t c, float alpha) { return theme::rgb(c, alpha); }
ImU32 lerp(ImU32 a, ImU32 b, float t) { return theme::mix(a, b, t); }

skin::Skin Skin::shared(ImU32 capOverride) const {
  skin::Skin k;
  k.panelHi = c.bgTop, k.panelLo = c.bgBottom, k.panel = lerp(c.bgTop, c.bgBottom, 0.5f);
  k.bevel = c.panelLine;
  k.rail = c.rail;
  k.fill = c.fill;
  k.fillPressed = lerp(c.fill, IM_COL32_WHITE, 0.25f);
  k.cap = capOverride ? capOverride : c.cap;
  k.handle = c.handle;
  k.text = c.text, k.textDim = c.textDim;
  k.lcdBg = c.lcdBg, k.lcdText = c.lcdText;
  k.graphBg = c.lcdBg, k.graphGrid = lerp(c.lcdBg, c.text, 0.1f), k.graphGridStrong = lerp(c.lcdBg, c.text, 0.22f);
  k.graphText = lerp(c.lcdBg, c.text, 0.45f), k.graphCurve = c.fill;
  k.select = c.fill;
  return k;
}

Skin begin(float width, float formW, float formH, const Palette& pal) {
  Skin k;
  k.o = ImGui::GetCursorScreenPos();
  k.s = width / formW;
  k.c = pal;
  (void)formH;
  return k;
}

void end(const Skin& k, float formH) {
  ImGui::SetCursorScreenPos(ImVec2(k.o.x, k.o.y + formH * k.s));
  ImGui::Dummy(ImVec2(1, 1));
}

void background(const Skin& k, float x, float y, float w, float h, ImU32 top, ImU32 bottom, float round) {
  ImDrawList* dl = k.dl();
  ImVec2 a = k.at(x, y), b = k.at(x + w, y + h);
  if (round > 0) {
    // Rounded: solid blend, then the gradient inset by the radius.
    dl->AddRectFilled(a, b, lerp(top, bottom, 0.5f), k.px(round));
    dl->AddRectFilledMultiColor(ImVec2(a.x, a.y + k.px(round)), ImVec2(b.x, b.y - k.px(round)), top, top, bottom, bottom);
  } else {
    dl->AddRectFilledMultiColor(a, b, top, top, bottom, bottom);
  }
}

void bevel(const Skin& k, float x, float y, float w, float h, ImU32 fill, ImU32 edge, float round) {
  ImDrawList* dl = k.dl();
  ImVec2 a = k.at(x, y), b = k.at(x + w, y + h);
  dl->AddRectFilled(a, b, fill, k.px(round));
  if (edge) dl->AddRect(a, b, edge, k.px(round), 0, 1.0f);
}

void text(const Skin& k, float x, float y, float w, float h, const char* s, float size, ImU32 col, int align, bool mono,
          bool bold) {
  ImDrawList* dl = k.dl();
  ImFont* f = fontFor(mono, bold);
  float fs = std::max(8.0f, size * k.s);
  ImVec2 ts = f->CalcTextSizeA(fs, FLT_MAX, 0, s);
  ImVec2 a = k.at(x, y), b = k.at(x + w, y + h);
  float tx = align == 1 ? (a.x + b.x - ts.x) * 0.5f : align == 2 ? b.x - ts.x : a.x;
  float ty = (a.y + b.y - ts.y) * 0.5f;
  dl->PushClipRect(ImVec2(a.x - 1, a.y - 2), ImVec2(b.x + 1, b.y + 2), true);
  dl->AddText(f, fs, ImVec2(std::floor(tx), std::floor(ty)), col, s);
  dl->PopClipRect();
}

void lcd(const Skin& k, float x, float y, float w, float h, const char* s, ImU32 bg, ImU32 fg, float size) {
  bevel(k, x, y, w, h, bg, lerp(bg, IM_COL32(0, 0, 0, 255), 0.5f), 2);
  ImDrawList* dl = k.dl();
  ImVec2 a = k.at(x, y), b = k.at(x + w, y + h);
  dl->AddLine(ImVec2(a.x + 1, b.y - 1), ImVec2(b.x - 1, b.y - 1), theme::withAlpha(fg, 0.10f));
  text(k, x + 2, y, w - 4, h, s, size > 0 ? size : h * 0.66f, fg, 1, true, true);
}

// ---------------------------------------------------------------------------- controls

namespace {
void drawWheel(const Skin& k, ImVec2 p, float S, double n, bool centered, ImU32 fill, int flags, ImU32 cap, bool hov, bool act) {
  ImDrawList* dl = k.dl();
  ImVec2 c(p.x + S * 0.5f, p.y + S * 0.5f);
  float r = S * 0.5f;
  const float a0 = kPi * 0.75f, a1 = kPi * 2.25f;
  float an = a0 + static_cast<float>(n) * (a1 - a0);
  float th = std::max(2.0f, r * 0.16f);
  float rr = r - th * 0.5f - 0.5f;
  // Rail ring (full sweep) + value arc.
  arc(dl, c, rr, a0, a1, k.c.rail, th);
  if (!(flags & WheelNoArc)) {
    float from = centered ? (a0 + a1) * 0.5f : a0;
    ImU32 fc = act ? lerp(fill, IM_COL32_WHITE, 0.25f) : fill;
    arc(dl, c, rr, std::min(from, an), std::max(from, an), fc, th);
  }
  // Cap: soft shadow, body, top highlight.
  float body = rr - th * 0.5f - std::max(1.0f, r * 0.06f);
  ImU32 capc = cap ? cap : k.c.cap;
  dl->AddCircleFilled(ImVec2(c.x, c.y + std::max(1.0f, r * 0.06f)), body, IM_COL32(0, 0, 0, 90), 40);
  dl->AddCircleFilled(c, body, hov ? lerp(capc, IM_COL32_WHITE, 0.08f) : capc, 40);
  dl->AddCircle(c, body, lerp(capc, IM_COL32_WHITE, 0.22f), 40, 1.0f);
  dl->AddCircleFilled(ImVec2(c.x, c.y - body * 0.25f), body * 0.62f, theme::withAlpha(IM_COL32_WHITE, 0.04f), 32);
  // Pointer line from the cap edge inwards.
  ImVec2 dir(std::cos(an), std::sin(an));
  float in = (flags & WheelBigLine) ? 0.18f : 0.42f;
  ImU32 pc = act ? (k.c.pointer ? lerp(k.c.pointer, fill, 0.5f) : IM_COL32_WHITE) : k.c.pointer ? k.c.pointer : lerp(capc, IM_COL32_WHITE, 0.75f);
  dl->AddLine(ImVec2(c.x + dir.x * body * in, c.y + dir.y * body * in), ImVec2(c.x + dir.x * (body - 1.0f), c.y + dir.y * (body - 1.0f)), pc,
              std::max(1.5f, r * ((flags & WheelBigLine) ? 0.13f : 0.09f)));
  if (centered) dl->AddCircleFilled(ImVec2(c.x, c.y - r + 0.5f), std::max(1.0f, th * 0.22f), theme::withAlpha(IM_COL32_WHITE, 0.5f), 8);
}

void drawSlider(const Skin& k, ImVec2 p, ImVec2 sz, float n, float c0, bool centered, ImU32 fill, bool hov, bool act, bool horizontal) {
  ImDrawList* dl = k.dl();
  ImU32 fc = act ? lerp(fill, IM_COL32_WHITE, 0.25f) : fill;
  if (!horizontal) {
    float cx = p.x + sz.x * 0.5f, hh = std::min(sz.x * 0.62f, k.px(12)), travel = sz.y - hh;
    float rw = std::max(2.0f, sz.x * 0.12f);
    dl->AddRectFilled(ImVec2(cx - rw, p.y + hh * 0.5f), ImVec2(cx + rw, p.y + sz.y - hh * 0.5f), k.c.rail, rw);
    float yv = p.y + hh * 0.5f + (1.0f - n) * travel, y0 = p.y + hh * 0.5f + (1.0f - c0) * travel;
    dl->AddRectFilled(ImVec2(cx - rw, std::min(yv, y0)), ImVec2(cx + rw, std::max(yv, y0)), fc, rw);
    if (centered) dl->AddLine(ImVec2(cx - sz.x * 0.36f, y0), ImVec2(cx + sz.x * 0.36f, y0), theme::withAlpha(IM_COL32_WHITE, 0.25f));
    float hw = sz.x * 0.40f;
    ImVec2 h0(cx - hw, yv - hh * 0.5f), h1(cx + hw, yv + hh * 0.5f);
    dl->AddRectFilled(ImVec2(h0.x, h0.y + 1), ImVec2(h1.x, h1.y + 1), IM_COL32(0, 0, 0, 80), k.px(2));
    dl->AddRectFilled(h0, h1, hov || act ? lerp(k.c.handle, IM_COL32_WHITE, 0.1f) : k.c.handle, k.px(2));
    dl->AddRect(h0, h1, lerp(k.c.handle, IM_COL32_WHITE, 0.2f), k.px(2));
    dl->AddLine(ImVec2(h0.x + 2, yv), ImVec2(h1.x - 2, yv), fc, std::max(1.0f, k.px(1.5f)));
  } else {
    float cy = p.y + sz.y * 0.5f, hw = std::min(sz.y * 0.62f, k.px(12)), travel = sz.x - hw;
    float rw = std::max(2.0f, sz.y * 0.12f);
    dl->AddRectFilled(ImVec2(p.x + hw * 0.5f, cy - rw), ImVec2(p.x + sz.x - hw * 0.5f, cy + rw), k.c.rail, rw);
    float xv = p.x + hw * 0.5f + n * travel, x0 = p.x + hw * 0.5f + c0 * travel;
    dl->AddRectFilled(ImVec2(std::min(xv, x0), cy - rw), ImVec2(std::max(xv, x0), cy + rw), fc, rw);
    float hh = sz.y * 0.40f;
    ImVec2 h0(xv - hw * 0.5f, cy - hh), h1(xv + hw * 0.5f, cy + hh);
    dl->AddRectFilled(h0, h1, hov || act ? lerp(k.c.handle, IM_COL32_WHITE, 0.1f) : k.c.handle, k.px(2));
    dl->AddRect(h0, h1, lerp(k.c.handle, IM_COL32_WHITE, 0.2f), k.px(2));
    dl->AddLine(ImVec2(xv, h0.y + 2), ImVec2(xv, h1.y - 2), fc, std::max(1.0f, k.px(1.5f)));
  }
}

// Interaction for a free value: drag (vertical or horizontal), wheel, Ctrl+click
// resets to the default, tooltip with the formatted value. Item = last item.
bool dragValue(App& a, double& v, const ParamDesc& d, float pixels, bool horizontal) {
  ImGuiIO& io = ImGui::GetIO();
  const bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
  bool changed = false;
  ImGuiID id = ImGui::GetItemID();
  static std::map<ImGuiID, double> accum;
  if (ImGui::IsItemActivated()) {
    accum[id] = d.toNormalized(v);
    if (io.KeyCtrl || io.KeySuper) {
      v = d.def;
      return true;
    }
  }
  if (act && !(io.KeyCtrl || io.KeySuper)) {
    float delta = horizontal ? io.MouseDelta.x : -io.MouseDelta.y;
    if (delta != 0) {
      double& acc = accum[id];
      acc = std::clamp(acc + delta * (io.KeyShift ? 0.12 : 1.0) / std::max(40.0f, pixels), 0.0, 1.0);
      double nv = d.fromNormalized(acc);
      if (nv != v) v = nv, changed = true;
    }
  }
  if (hov && !act && io.MouseWheel != 0) {
    double nv = d.fromNormalized(std::clamp(d.toNormalized(v) + io.MouseWheel * (io.KeyShift ? 0.005 : 0.025), 0.0, 1.0));
    if (nv != v) v = nv, changed = true;
  }
  if (hov || act) {
    a.setHint(d.name + ": " + d.format(v) + "   (drag · Shift fine · Ctrl+click reset)");
    if (act || ImGui::GetCurrentContext()->HoveredIdTimer > 0.5f) ImGui::SetTooltip("%s", d.format(v).c_str());
  }
  return changed;
}
}  // namespace

bool wheel(App& a, const PluginTarget& t, const Skin& k, int param, float x, float y, float size, ImU32 fill, int flags,
           ImU32 cap) {
  ImGui::PushID(param);
  const int sf = ((flags & WheelCentered) ? skin::WheelCentered : 0) | ((flags & WheelBigLine) ? skin::WheelBigLine : 0);
  bool changed = skin::wheel(a, t, param, k.at(x, y), k.px(size), k.shared(cap), sf, fill);
  ImGui::PopID();
  return changed;
}

bool wheelValue(App& a, const Skin& k, const char* id, double& v, const ParamDesc& d, float x, float y, float size, ImU32 fill, int flags,
                ImU32 cap) {
  ImGui::PushID(id);
  ImVec2 p = k.at(x, y);
  float S = k.px(size);
  ImGui::SetCursorScreenPos(p);
  ImGui::InvisibleButton("##wheelv", ImVec2(S, S));
  bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
  bool changed = dragValue(a, v, d, 180.0f, false);
  bool centered = (flags & WheelCentered) || (d.min < 0 && d.max > 0);
  drawWheel(k, p, S, d.toNormalized(v), centered, fill, flags, cap, hov, act);
  ImGui::PopID();
  return changed;
}

bool slider(App& a, const PluginTarget& t, const Skin& k, int param, float x, float y, float w, float h, ImU32 fill, bool centered) {
  (void)centered;  // the shared slider fills bipolar ranges from the centre
  ImGui::PushID(param);
  bool changed = skin::vslider(a, t, param, k.at(x, y), ImVec2(k.px(w), k.px(h)), k.shared(), fill, false);
  ImGui::PopID();
  return changed;
}
bool hslider(App& a, const PluginTarget& t, const Skin& k, int param, float x, float y, float w, float h, ImU32 fill, bool centered) {
  (void)centered;
  ImGui::PushID(param);
  bool changed = skin::hslider(a, t, param, k.at(x, y), ImVec2(k.px(w), k.px(h)), k.shared(), fill);
  ImGui::PopID();
  return changed;
}

bool sliderValue(App& a, const Skin& k, const char* id, double& v, const ParamDesc& d, float x, float y, float w, float h, ImU32 fill,
                 bool centered) {
  ImGui::PushID(id);
  ImVec2 p = k.at(x, y);
  ImVec2 sz(k.px(w), k.px(h));
  ImGui::SetCursorScreenPos(p);
  ImGui::InvisibleButton("##sliderv", sz);
  bool act = ImGui::IsItemActive(), hov = ImGui::IsItemHovered();
  bool changed = dragValue(a, v, d, sz.y, false);
  float c0 = centered ? static_cast<float>(d.min < 0 && d.max > 0 ? d.toNormalized(0) : 0.5) : 0.0f;
  drawSlider(k, p, sz, static_cast<float>(d.toNormalized(v)), c0, centered, fill, hov, act, false);
  ImGui::PopID();
  return changed;
}

bool digiValue(App& a, const Skin& k, const char* id, double& v, const ParamDesc& d, float x, float y, float w, float h,
               const std::function<std::string(double)>& fmt, float size) {
  ImGui::PushID(id);
  ImGui::SetCursorScreenPos(k.at(x, y));
  ImGui::InvisibleButton("##digiv", ImVec2(k.px(w), k.px(h)));
  bool act = ImGui::IsItemActive();
  bool changed = dragValue(a, v, d, 140.0f, false);
  std::string s = fmt ? fmt(v) : d.format(v);
  lcd(k, x, y, w, h, s.c_str(), k.c.lcdBg, act ? IM_COL32_WHITE : k.c.lcdText, size);
  ImGui::PopID();
  return changed;
}

bool check(App& a, const PluginTarget& t, const Skin& k, int param, float x, float y, float w, float h, const char* caption,
           float size) {
  ParamView pv = view(a, t, param);
  if (!pv.ok) return false;
  bool on = pv.v >= 0.5;
  ImGui::PushID(param);
  ImVec2 p = k.at(x, y);
  ImGui::SetCursorScreenPos(p);
  bool pressed = ImGui::InvisibleButton("##chk", ImVec2(k.px(w), k.px(h)));
  bool hov = ImGui::IsItemHovered();
  ImDrawList* dl = k.dl();
  float bs = std::min(h * 0.46f, 12.0f);
  float bx = x + 4, by = y + (h - bs) * 0.5f;
  bevel(k, bx, by, bs, bs, on ? k.c.led : k.c.ledOff, lerp(on ? k.c.led : k.c.ledOff, IM_COL32(0, 0, 0, 255), 0.4f), 2);
  if (on) dl->AddRectFilled(k.at(bx - 2, by - 2), k.at(bx + bs + 2, by + bs + 2), theme::withAlpha(k.c.led, 0.18f), k.px(4));
  text(k, bx + bs + 5, y, w - bs - 9, h, caption, size, on || hov ? k.c.text : k.c.textDim, 0, false, true);
  if (pressed) a.doc.setParam(t.pid(param), on ? 0 : 1, false);
  if (hov) a.setHint(pv.d->name + ": " + (on ? "on" : "off") + "   (click to switch)");
  ImGui::PopID();
  return pressed;
}

bool switchBtn(App& a, const PluginTarget& t, const Skin& k, int param, float x, float y, float w, float h,
               const std::function<void(ImDrawList*, ImVec2, ImVec2, bool)>& icon) {
  ParamView pv = view(a, t, param);
  if (!pv.ok) return false;
  bool on = pv.v >= 0.5;
  ImGui::PushID(param);
  ImVec2 p = k.at(x, y), q = k.at(x + w, y + h);
  ImGui::SetCursorScreenPos(p);
  bool pressed = ImGui::InvisibleButton("##sw", ImVec2(q.x - p.x, q.y - p.y));
  bool hov = ImGui::IsItemHovered();
  if (hov) k.dl()->AddRectFilled(p, q, theme::withAlpha(IM_COL32_WHITE, 0.06f), k.px(3));
  icon(k.dl(), p, q, on);
  if (pressed) a.doc.setParam(t.pid(param), on ? 0 : 1, false);
  if (hov) a.setHint(pv.d->name + ": " + (on ? "on" : "off") + "   (click to switch)");
  ImGui::PopID();
  return pressed;
}

bool selector(App& a, const PluginTarget& t, const Skin& k, int param, float x, float y, float w, float h,
              const std::function<void(ImDrawList*, ImVec2, ImVec2, int)>& item, bool endless) {
  ParamView pv = view(a, t, param);
  if (!pv.ok) return false;
  ImGui::PushID(param);
  ImVec2 p = k.at(x, y), q = k.at(x + w, y + h);
  ImGui::SetCursorScreenPos(p);
  ImGui::InvisibleButton("##sel", ImVec2(q.x - p.x, q.y - p.y));
  ImGuiIO& io = ImGui::GetIO();
  bool hov = ImGui::IsItemHovered();
  const int count = std::max<int>(1, static_cast<int>(pv.d->choices.size()));
  int cur = std::clamp(static_cast<int>(std::lround(pv.v)), 0, count - 1);
  bool changed = false;
  if (ImGui::IsItemDeactivated() && io.MouseDragMaxDistanceSqr[0] < 16.0f) {
    int dir = io.MousePos.x >= (p.x + q.x) * 0.5f ? 1 : -1;
    int nx = cur + dir;
    nx = endless ? (nx + count) % count : std::clamp(nx, 0, count - 1);
    if (nx != cur) a.doc.setParam(t.pid(param), nx, false), cur = nx, changed = true;
  } else {
    double val = pv.v;
    if (hov && io.MouseWheel != 0) {
      int nx = cur + (io.MouseWheel < 0 ? 1 : -1);
      nx = endless ? (nx + count) % count : std::clamp(nx, 0, count - 1);
      a.doc.setParam(t.pid(param), nx, false), cur = nx, changed = true;
      io.MouseWheel = 0;
    }
    if (pui::dragParamItem(a, t.pid(param), *pv.d, val, 220.0f, true)) cur = static_cast<int>(std::lround(val)), changed = true;
  }
  item(k.dl(), p, q, cur);
  if (hov) a.setHint(pv.d->name + ": " + pv.d->choices[static_cast<size_t>(cur)] + "   (click left/right half to step · drag · wheel)");
  ImGui::PopID();
  return changed;
}

bool digi(App& a, const PluginTarget& t, const Skin& k, int param, float x, float y, float w, float h,
          const std::function<std::string(double)>& fmt, float size) {
  ParamView pv = view(a, t, param);
  if (!pv.ok) return false;
  ImGui::PushID(param);
  ImVec2 p = k.at(x, y), q = k.at(x + w, y + h);
  ImGui::SetCursorScreenPos(p);
  ImGui::InvisibleButton("##digi", ImVec2(q.x - p.x, q.y - p.y));
  bool act = ImGui::IsItemActive();
  double val = pv.v;
  bool changed = pui::dragParamItem(a, t.pid(param), *pv.d, val, 140.0f, false);
  std::string s = fmt ? fmt(val) : pv.d->format(val);
  lcd(k, x, y, w, h, s.c_str(), k.c.lcdBg, act ? IM_COL32_WHITE : k.c.lcdText, size);
  ImGui::PopID();
  return changed;
}

bool button(const Skin& k, const char* id, float x, float y, float w, float h, const char* caption, bool lit, float size,
            ImU32 litCol) {
  ImVec2 p = k.at(x, y), q = k.at(x + w, y + h);
  ImGui::SetCursorScreenPos(p);
  bool pressed = ImGui::InvisibleButton(id, ImVec2(q.x - p.x, q.y - p.y));
  bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
  ImU32 lc = litCol ? litCol : k.c.led;
  ImU32 bg = lit ? theme::withAlpha(lc, 0.85f) : act ? lerp(k.c.panel, IM_COL32_WHITE, 0.14f) : hov ? lerp(k.c.panel, IM_COL32_WHITE, 0.07f) : k.c.panel;
  bevel(k, x, y, w, h, bg, k.c.panelLine, 2.5f);
  if (caption && *caption) text(k, x + 2, y, w - 4, h, caption, size, lit ? theme::contrastText(lc) : k.c.text, 1, false, true);
  return pressed;
}

int pad(const Skin& k, const char* id, float x, float y, float w, float h, const char* caption, bool selected, float glow, float* vel) {
  ImVec2 p = k.at(x, y), q = k.at(x + w, y + h);
  ImGui::SetCursorScreenPos(p);
  ImGui::InvisibleButton(id, ImVec2(q.x - p.x, q.y - p.y));
  bool hov = ImGui::IsItemHovered();
  int r = 0;
  if (ImGui::IsItemActivated()) {
    float rel = (ImGui::GetIO().MousePos.y - p.y) / std::max(1.0f, q.y - p.y);
    if (vel) *vel = std::clamp(1.0f - rel * 0.85f, 0.1f, 1.0f);  // top of the pad = loud
    r = 1;
  }
  if (ImGui::IsItemDeactivated()) r = 2;
  ImDrawList* dl = k.dl();
  float rd = k.px(4);
  ImU32 base = selected ? lerp(k.c.panel, k.c.fill, 0.22f) : k.c.panel;
  if (hov) base = lerp(base, IM_COL32_WHITE, 0.06f);
  dl->AddRectFilled(ImVec2(p.x, p.y + 2), ImVec2(q.x, q.y + 2), IM_COL32(0, 0, 0, 90), rd);
  dl->AddRectFilledMultiColor(ImVec2(p.x + 1, p.y + 1), ImVec2(q.x - 1, q.y - 1), lerp(base, IM_COL32_WHITE, 0.07f),
                              lerp(base, IM_COL32_WHITE, 0.07f), base, base);
  dl->AddRect(p, q, selected ? k.c.fill : k.c.panelLine, rd, 0, selected ? 1.5f : 1.0f);
  keylight::drawFlash(dl, ImVec2(p.x + 1, p.y + 1), ImVec2(q.x - 1, q.y - 1), k.c.fill2, glow, rd);  // J-77 hit flash
  if (caption && *caption) {
    ImFont* f = app().fontRegular;
    float fs = std::max(8.0f, k.px(10));
    dl->PushClipRect(ImVec2(p.x + 3, p.y), ImVec2(q.x - 3, q.y), true);
    dl->AddText(f, fs, ImVec2(p.x + k.px(5), q.y - fs - k.px(4)), selected ? k.c.text : k.c.textDim, caption);
    dl->PopClipRect();
  }
  return r;
}

int keyboard(App& a, const PluginTarget& t, const Skin& k, float x, float y, float w, float h, int lowNote, int whites,
             const KeyColors& kc, int mark, const bool* lit, bool pickOnly) {
  static const int wOff[7] = {0, 2, 4, 5, 7, 9, 11};
  static const int bAfter[7] = {1, 1, 0, 1, 1, 1, 0};  // black key after this white key
  ImVec2 p = k.at(x, y), q = k.at(x + w, y + h);
  ImGui::SetCursorScreenPos(p);
  ImGui::PushID("kbd");
  ImGui::InvisibleButton("##kbd", ImVec2(q.x - p.x, q.y - p.y));
  bool hov = ImGui::IsItemHovered();
  ImVec2 m = ImGui::GetIO().MousePos;
  const float ww = (q.x - p.x) / static_cast<float>(whites), bh = (q.y - p.y) * 0.62f, bw = ww * 0.62f;
  auto whiteNote = [&](int i) { return lowNote + (i / 7) * 12 + wOff[i % 7]; };
  int hoverNote = -1;
  if (hov) {
    for (int i = 0; i < whites && hoverNote < 0; ++i)
      if (bAfter[i % 7] && i + 1 < whites) {
        float bx = p.x + (i + 1) * ww - bw * 0.5f;
        if (m.x >= bx && m.x < bx + bw && m.y < p.y + bh) hoverNote = whiteNote(i) + 1;
      }
    if (hoverNote < 0) hoverNote = whiteNote(std::clamp(static_cast<int>((m.x - p.x) / ww), 0, whites - 1));
  }
  static int playing = -1;
  static ImGuiID playingId = 0;
  int clicked = -1;
  if (ImGui::IsItemActivated() && hoverNote >= 0) {
    clicked = hoverNote;
    if (!pickOnly && t.isChannel()) {
      a.engine.noteOn(t.owner, hoverNote, 0.85f);
      playing = hoverNote, playingId = ImGui::GetItemID();
    }
  }
  const bool act = ImGui::IsItemActive();
  if (ImGui::IsItemDeactivated() && playing >= 0 && playingId == ImGui::GetItemID()) {
    a.engine.noteOff(t.owner, playing);
    playing = -1;
  }
  ImDrawList* dl = k.dl();
  dl->AddRectFilled(p, q, kc.sep);
  // J-77: without an explicit lit set, the keys sounding on the editor's channel light up.
  const keylight::ChannelLights* chLit = !lit && t.isChannel() ? keylight::lights(a, t.owner) : nullptr;
  auto keyCol = [&](int note, ImU32 base) {
    if (act && note == playing) return kc.pressed;
    if (note == mark) return kc.mark;
    if (lit && note >= 0 && note < 128 && lit[note]) return lerp(base, kc.pressed, 0.45f);
    if (chLit && chLit->key(note) > 0.004f) return lerp(base, kc.pressed, 0.85f * chLit->key(note));
    if (note == hoverNote) return lerp(base, kc.pressed, 0.18f);
    return base;
  };
  for (int i = 0; i < whites; ++i) {
    int note = whiteNote(i);
    float x0 = p.x + i * ww;
    dl->AddRectFilled(ImVec2(x0 + 0.5f, p.y), ImVec2(x0 + ww - 0.5f, q.y), keyCol(note, kc.white), 2.0f, ImDrawFlags_RoundCornersBottom);
    if (note % 12 == 0 && ww > 9) {
      char nm[32];
      std::snprintf(nm, sizeof nm, "C%d", note / 12 - 1);
      ImFont* f = app().fontMono;
      float fs = std::min(9.0f, ww * 0.8f);
      dl->AddText(f, fs, ImVec2(x0 + 2, q.y - fs - 3), lerp(kc.white, IM_COL32_BLACK, 0.55f), nm);
    }
  }
  for (int i = 0; i + 1 < whites; ++i) {
    if (!bAfter[i % 7]) continue;
    int note = whiteNote(i) + 1;
    float bx = p.x + (i + 1) * ww - bw * 0.5f;
    dl->AddRectFilled(ImVec2(bx, p.y), ImVec2(bx + bw, p.y + bh), keyCol(note, kc.black), 1.5f, ImDrawFlags_RoundCornersBottom);
  }
  if (hov && hoverNote >= 0) {
    static const char* nn[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    a.setHint(std::string(nn[hoverNote % 12]) + std::to_string(hoverNote / 12 - 1) + (pickOnly ? "   (click to pick)" : "   (click to play)"));
  }
  ImGui::PopID();
  return clicked;
}

void wave(ImDrawList* dl, const Sample* smp, ImVec2 p0, ImVec2 p1, ImU32 col, double from, double to, ImU32 centre) {
  const float mid = (p0.y + p1.y) * 0.5f, half = (p1.y - p0.y) * 0.46f;
  if (centre) dl->AddLine(ImVec2(p0.x, mid), ImVec2(p1.x, mid), centre);
  if (!smp || smp->frames() == 0) return;
  const size_t n = smp->frames();
  from = std::clamp(from, 0.0, 1.0), to = std::clamp(to, from, 1.0);
  const size_t f0 = static_cast<size_t>(from * static_cast<double>(n)), f1 = std::max(f0 + 1, static_cast<size_t>(to * static_cast<double>(n)));
  const int w = std::max(1, static_cast<int>(p1.x - p0.x));
  for (int x = 0; x < w; ++x) {
    size_t a0 = f0 + (f1 - f0) * static_cast<size_t>(x) / static_cast<size_t>(w);
    size_t a1 = std::max(a0 + 1, f0 + (f1 - f0) * static_cast<size_t>(x + 1) / static_cast<size_t>(w));
    float lo = 0, hi = 0;
    const size_t stepN = std::max<size_t>(1, (a1 - a0) / 48);
    for (size_t i = a0; i < a1 && i < n; i += stepN) {
      float v = (smp->left[i] + smp->right[i]) * 0.5f;
      lo = std::min(lo, v), hi = std::max(hi, v);
    }
    dl->AddLine(ImVec2(p0.x + static_cast<float>(x), mid - hi * half), ImVec2(p0.x + static_cast<float>(x), mid - lo * half + 1), col);
  }
}

// ---------------------------------------------------------------------------- glyphs

void glyphPan(ImDrawList* dl, ImVec2 c, float r, ImU32 col) {
  dl->AddLine(ImVec2(c.x - r, c.y), ImVec2(c.x + r, c.y), col, 1.2f);
  dl->AddTriangleFilled(ImVec2(c.x - r - 1, c.y), ImVec2(c.x - r * 0.45f, c.y - r * 0.45f), ImVec2(c.x - r * 0.45f, c.y + r * 0.45f), col);
  dl->AddTriangleFilled(ImVec2(c.x + r + 1, c.y), ImVec2(c.x + r * 0.45f, c.y - r * 0.45f), ImVec2(c.x + r * 0.45f, c.y + r * 0.45f), col);
}

void glyphInvert(ImDrawList* dl, ImVec2 c, float r, ImU32 col) {
  // A sine cycle mirrored: half-wave up then down, with a flip arrow.
  ImVec2 pts[17];
  for (int i = 0; i < 17; ++i) {
    float u = i / 16.0f;
    pts[i] = ImVec2(c.x - r + 2 * r * u, c.y + std::sin(u * 2 * kPi) * r * 0.55f);
  }
  dl->AddPolyline(pts, 17, col, 0, 1.6f);
  dl->AddLine(ImVec2(c.x - r, c.y), ImVec2(c.x + r, c.y), theme::withAlpha(col, 0.4f), 1.0f);
}

void glyphArrow(ImDrawList* dl, ImVec2 c, float r, ImU32 col, int dir) {
  ImVec2 a, b, d;
  switch (dir) {
    case 0: a = ImVec2(c.x - r * 0.6f, c.y), b = ImVec2(c.x + r * 0.4f, c.y - r * 0.7f), d = ImVec2(c.x + r * 0.4f, c.y + r * 0.7f); break;
    case 1: a = ImVec2(c.x + r * 0.6f, c.y), b = ImVec2(c.x - r * 0.4f, c.y - r * 0.7f), d = ImVec2(c.x - r * 0.4f, c.y + r * 0.7f); break;
    case 2: a = ImVec2(c.x, c.y - r * 0.6f), b = ImVec2(c.x - r * 0.7f, c.y + r * 0.4f), d = ImVec2(c.x + r * 0.7f, c.y + r * 0.4f); break;
    default: a = ImVec2(c.x, c.y + r * 0.6f), b = ImVec2(c.x - r * 0.7f, c.y - r * 0.4f), d = ImVec2(c.x + r * 0.7f, c.y - r * 0.4f); break;
  }
  dl->AddTriangleFilled(a, b, d, col);
}

void glyphFolder(ImDrawList* dl, ImVec2 c, float r, ImU32 col) {
  dl->AddRectFilled(ImVec2(c.x - r, c.y - r * 0.55f), ImVec2(c.x - r * 0.1f, c.y - r * 0.3f), col, 1.0f);
  dl->AddRectFilled(ImVec2(c.x - r, c.y - r * 0.35f), ImVec2(c.x + r, c.y + r * 0.65f), col, 1.5f);
}

}  // namespace gl::iskin
