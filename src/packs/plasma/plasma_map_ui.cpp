// Gridline — "plasma" pack: the Key zones view (on the stage). Each layer has
// a key zone and a velocity zone, the texture a key zone; drag the ends or the
// middle of a zone. Fold and the "keep feeding" switch sit at the left of a row.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include "../../core/I18n.h"
#include "plasma_ui.h"

namespace gl::packs::plasma::ui {

namespace {
constexpr float kLabelW = 236, kRowH = 128;

std::string noteName(int k) {
  static const char* const n[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
  return std::string(n[((k % 12) + 12) % 12]) + std::to_string(k / 12 - 2);
}

// One zone bar: keys (vel false) or velocities over x0..x1.
void zoneBar(Ctx& c, int pMin, int pMax, bool vel, bool on, float x0, float x1, float y0, float h, const char* who) {
  reach(pMin);
  reach(pMax);
  const int lo = vel ? 1 : 0, hi = 127;
  const float bw = x1 - x0;
  auto xOf = [&](float v) { return x0 + bw * (v - lo) / static_cast<float>(hi - lo + 1); };
  const float vmin = std::round(valf(c, pMin)), vmax = std::round(valf(c, pMax));
  display(c, x0, y0, bw, h, 1, 1);
  if (!vel) {
    for (int k = 0; k < 128; ++k) {
      const int pc = k % 12;
      if (pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10)
        c.dl->AddRectFilled(c.p(xOf(static_cast<float>(k)), y0 + 1), c.p(xOf(static_cast<float>(k + 1)), y0 + h * 0.4f), theme::withAlpha(c.P.frame, 0.5f));
      if (pc == 0) vline(c, xOf(static_cast<float>(k)), y0 + 1, y0 + h - 1, c.P.grid);
    }
  } else {
    // a rising wedge: velocity grows to the right
    c.dl->AddTriangleFilled(c.p(x0 + 1, y0 + h - 2), c.p(x1 - 1, y0 + h - 2), c.p(x1 - 1, y0 + 3), theme::withAlpha(c.P.frame, 0.35f));
  }
  const float ax = xOf(vmin), axe = xOf(vmax + 1);
  const ImU32 col = c.P.knob;
  c.dl->AddRectFilled(c.p(ax, y0 + 3), c.p(axe, y0 + h - 3), theme::withAlpha(col, on ? 0.22f : 0.08f), c.s(4));
  c.dl->AddRect(c.p(ax, y0 + 3), c.p(axe, y0 + h - 3), theme::withAlpha(col, on ? 0.8f : 0.3f), c.s(4));
  c.dl->AddCircleFilled(c.p(ax, y0 + h * 0.5f), c.s(4), on ? col : theme::withAlpha(col, 0.4f), 14);
  c.dl->AddCircleFilled(c.p(axe, y0 + h * 0.5f), c.s(4), on ? col : theme::withAlpha(col, 0.4f), 14);
  char t[48];
  if (vel) std::snprintf(t, sizeof t, "%d to %d", static_cast<int>(vmin), static_cast<int>(vmax));
  else std::snprintf(t, sizeof t, "%s to %s", noteName(static_cast<int>(vmin)).c_str(), noteName(static_cast<int>(vmax)).c_str());
  {
    MonoText mono;
    textMid(c, ax, axe, y0 + h * 0.5f, t, on ? c.P.text : c.P.textDim, 11.0f, false);
  }
  // drag: an end moves that end, the middle moves the zone
  ImGui::PushID(vel ? 1 : 0);
  ImGui::SetCursorScreenPos(c.p(x0, y0));
  ImGui::InvisibleButton("##rng", c.F.sz(bw, h));
  static int mode = 0;
  static float grab = 0;
  const float mx = ImGui::GetIO().MousePos.x;
  const float val = std::clamp(lo + (mx - c.p(x0, 0).x) / c.s(bw) * static_cast<float>(hi - lo + 1), static_cast<float>(lo), static_cast<float>(hi) + 0.999f);
  const float edge = 6;
  const bool nearMin = std::fabs(mx - c.p(ax, 0).x) < c.s(edge), nearMax = std::fabs(mx - c.p(axe, 0).x) < c.s(edge);
  if (ImGui::IsItemHovered() && (nearMin || nearMax)) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
  if (ImGui::IsItemActivated()) {
    mode = nearMin ? 1 : nearMax ? 2 : (val >= vmin && val <= vmax + 1) ? 3 : 0;
    grab = val - vmin;
    if (mode == 0) mode = std::fabs(val - vmin) < std::fabs(val - vmax) ? 1 : 2;  // outside: the nearer end jumps there
  }
  if (ImGui::IsItemActive()) {
    const float v = std::floor(val);
    if (mode == 1) setv(c, pMin, std::min(v, vmax), true);
    if (mode == 2) setv(c, pMax, std::max(v, vmin), true);
    if (mode == 3) {
      const float span = vmax - vmin;
      const float nmin = std::clamp(std::floor(val - grab), static_cast<float>(lo), static_cast<float>(hi) - span);
      setv(c, pMin, nmin, true);
      setv(c, pMax, nmin + span, true);
    }
  }
  if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
    setv(c, pMin, lo);
    setv(c, pMax, hi);
  }
  if (ImGui::IsItemHovered())
    c.a.setHint(std::string(vel ? "Velocity zone of " : "Key zone of ") + who + ": drag an end or the middle \xC2\xB7 double-click: everything");
  ImGui::PopID();
}
}  // namespace

void mappingPanel(Ctx& c) {
  const float hx = stageHead(c, "Key zones");
  text(c, hx, kStageHeadY + 8, "Which keys and velocities each layer plays", c.P.textDim, 11.0f, false);
  const float x = 9, w = kStageW - 18;
  const float bx0 = x + kLabelW, bx1 = x + w - 14;
  // the key scale along the top
  {
    const float sy = kStageY + 6, bw = bx1 - bx0;
    for (int k = 0; k <= 120; k += 12) {
      const float kx = bx0 + bw * k / 128.f;
      vline(c, kx, sy + 12, sy + 18, c.P.textFaint);
      MonoText mono;
      text(c, kx + 2, sy, noteName(k).c_str(), c.P.textDim, 10.0f, false);
    }
  }
  for (int o = 0; o <= kOscs; ++o) {  // the three layers, then the texture (key zone only)
    const bool noise = o == kOscs;
    const float ry = kStageY + 30 + o * kRowH;
    ImGui::PushID(o + 9500);
    const char* nm[] = {"Layer 1", "Layer 2", "Layer 3", "Texture"};
    const float top = card(c, x, ry, w, kRowH - 4, nm[o], noise ? noiseP(N_Enable) : oscP(o, O_Enable));
    const bool on = vali(c, noise ? noiseP(N_Enable) : oscP(o, O_Enable)) != 0;
    chip(c, noise ? tailP(T_NKeyFold) : oscP(o, O_MapFold), x + 12, top + 2, 140, 26, "Fold into zone");
    if (ImGui::IsItemHovered())
      c.a.setHint(noise ? "Keys outside the zone wrap into it, its width at a time (otherwise the texture is left out)"
                        : "Keys outside the zone move into it by octaves (otherwise they are left out)");
    if (!noise) {
      chip(c, oscP(o, O_MapWarp), x + 12, top + 34, 140, 26, "Keep feeding");
      if (ImGui::IsItemHovered()) c.a.setHint("Outside its zones the layer stays silent but still feeds the other layers' morphs");
    }
    text(c, x + 162, top + 12, "Keys", c.P.label, 11.0f, false);
    zoneBar(c, noise ? tailP(T_NKeyMin) : oscP(o, O_KeyMin), noise ? tailP(T_NKeyMax) : oscP(o, O_KeyMax), false, on, bx0, bx1, top, 38, nm[o]);
    if (!noise) {
      text(c, x + 162, top + 56, "Velocity", c.P.label, 11.0f, false);
      zoneBar(c, oscP(o, O_VelMin), oscP(o, O_VelMax), true, on, bx0, bx1, top + 44, 40, nm[o]);
    }
    ImGui::PopID();
  }
}

}  // namespace gl::packs::plasma::ui
