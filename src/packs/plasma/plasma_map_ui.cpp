// Gridline — "plasma" pack: the oscillator mapping editor (key and velocity
// zone of each main oscillator, with FOLD and WARP), opened from the strip.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include "../../core/I18n.h"
#include "plasma_ui.h"

namespace gl::packs::plasma::ui {

namespace {
constexpr float kPx = 300, kPy = 420, kPw = 740, kPh = kStripY - 6 - kPy;
constexpr float kLabelW = 150, kRowH = 52;

std::string noteName(int k) {
  static const char* const n[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
  return std::string(n[((k % 12) + 12) % 12]) + std::to_string(k / 12 - 2);
}
}  // namespace

namespace {
void mappingBody(Ctx& c);
}

void mappingPanel(Ctx& c) {
  // a child window: above the page in z-order, so it gets the pointer first
  for (int i = 3; i >= 1; --i)  // soft drop shadow
    c.dl->AddRectFilled(c.p(kPx - i * 2.f, kPy - i * 1.f), c.p(kPx + kPw + i * 2.f, kPy + kPh + i * 3.f), theme::withAlpha(IM_COL32(0, 0, 0, 255), 0.07f), c.s(6 + i * 2.f));
  ImGui::SetCursorScreenPos(c.p(kPx, kPy));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 0));
  bool closeOutside = false;
  if (ImGui::BeginChild("##plmap", c.F.sz(kPw, kPh), false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
    Ctx cc = c;
    cc.dl = ImGui::GetWindowDrawList();
    mappingBody(cc);
  }
  ImGui::EndChild();
  ImGui::PopStyleColor();
  ImGui::PopStyleVar();
  // a click outside closes the panel
  if (ImGui::IsMouseClicked(0)) {
    const ImVec2 m = ImGui::GetIO().MousePos, a = c.p(kPx, kPy), b = c.p(kPx + kPw, kPy + kPh);
    closeOutside = m.x < a.x || m.x > b.x || m.y < a.y || m.y > b.y;
  }
  if (closeOutside && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) c.u.mapping = false;
}

namespace {
void mappingBody(Ctx& c) {
  Ui& u = c.u;
  panelRect(c, kPx, kPy, kPw, kPh, c.P.panel, 6);
  c.dl->AddRectFilled(c.p(kPx + 1, kPy + 1), c.p(kPx + kPw - 1, kPy + 29), c.P.head, c.s(5), ImDrawFlags_RoundCornersTop);
  hline(c, kPx + 1, kPx + kPw - 1, kPy + 29.5f, c.P.frame);
  c.dl->AddRect(c.p(kPx, kPy), c.p(kPx + kPw, kPy + kPh), c.P.lineHi, c.s(6));
  // swallow clicks on the panel background
  ImGui::SetCursorScreenPos(c.p(kPx, kPy));
  ImGui::InvisibleButton("##mapbg", c.F.sz(kPw, kPh));
  text(c, kPx + 12, kPy + 10, "OSCILLATOR MAPPING", c.P.text, 9.5f, true);
  text(c, kPx + 180, kPy + 10.5f, "MAPPING", c.P.textDim, 8, true);
  if (button(c, "mkey", kPx + 226, kPy + 6, 44, 18, "KEY", !u.mapVel, 8)) u.mapVel = false;
  if (button(c, "mvel", kPx + 272, kPy + 6, 44, 18, "VEL", u.mapVel, 8)) u.mapVel = true;
  ImGui::SetCursorScreenPos(c.p(kPx + kPw - 26, kPy + 6));
  if (ImGui::InvisibleButton("##mclose", c.F.sz(20, 20))) u.mapping = false;
  iconClose(c.dl, c.p(kPx + kPw - 16, kPy + 16), c.s(8), ImGui::IsItemHovered() ? c.P.text : c.P.textDim);
  const float bx0 = kPx + kLabelW, bx1 = kPx + kPw - 12, bw = bx1 - bx0;
  const int lo = u.mapVel ? 1 : 0, hi = 127;
  auto xOf = [&](float v) { return bx0 + bw * (v - lo) / static_cast<float>(hi - lo + 1); };
  // scale: octaves (key) or quarters (velocity)
  {
    const float sy = kPy + 32;
    if (!u.mapVel) {
      for (int k = 0; k <= 120; k += 12) {
        vline(c, xOf(static_cast<float>(k)), sy + 10, sy + 16, c.P.textFaint);
        text(c, xOf(static_cast<float>(k)) + 2, sy, noteName(k).c_str(), c.P.textFaint, 7.5f, true);
      }
    } else {
      for (int v : {1, 32, 64, 96, 127}) {
        vline(c, xOf(static_cast<float>(v)), sy + 10, sy + 16, c.P.textFaint);
        text(c, xOf(static_cast<float>(v)) + 2, sy, std::to_string(v).c_str(), c.P.textFaint, 7.5f, true);
      }
    }
  }
  for (int o = 0; o < kOscs; ++o) {
    const float ry = kPy + 52 + o * kRowH;
    ImGui::PushID(o + 9500);
    hline(c, kPx + 8, kPx + kPw - 8, ry - 3, c.P.line);
    const char* nm[] = {"OSC A", "OSC B", "OSC C"};
    const bool on = vali(c, oscP(o, O_Enable)) != 0;
    text(c, kPx + 12, ry + 6, nm[o], on ? c.P.text : c.P.textFaint, 9.5f, true);
    checkBox(c, oscP(o, O_MapFold), kPx + 12, ry + 26, "FOLD", 7.5f);
    if (ImGui::IsItemHovered()) c.a.setHint("Fold: keys outside the range move into it by octaves (otherwise they are left out)");
    checkBox(c, oscP(o, O_MapWarp), kPx + 70, ry + 26, "WARP", 7.5f);
    if (ImGui::IsItemHovered()) c.a.setHint("Warp: outside its range the oscillator stays silent but still feeds FM / PD / AM / RM warps");
    // range bar
    const int kMin = u.mapVel ? O_VelMin : O_KeyMin, kMax = u.mapVel ? O_VelMax : O_KeyMax;
    const float vmin = std::round(valf(c, oscP(o, kMin))), vmax = std::round(valf(c, oscP(o, kMax)));
    const float y0 = ry + 4, h = kRowH - 14;
    wellRect(c, bx0, y0, bw, h, 3);
    if (!u.mapVel) {
      for (int k = 0; k < 128; ++k) {
        const int pc = k % 12;
        if (pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10) c.dl->AddRectFilled(c.p(xOf(static_cast<float>(k)), y0 + 1), c.p(xOf(static_cast<float>(k + 1)), y0 + h * 0.45f), theme::withAlpha(c.P.frame, 0.6f));
        if (pc == 0) vline(c, xOf(static_cast<float>(k)), y0 + 1, y0 + h - 1, c.P.grid);
      }
    }
    const float ax = xOf(vmin), axe = xOf(vmax + 1);
    skin::gradientRect(c.dl, c.p(ax, y0 + 3), c.p(axe, y0 + h - 3), theme::withAlpha(c.P.accent, on ? 0.55f : 0.25f), theme::withAlpha(c.P.accent, on ? 0.3f : 0.12f), c.s(3));
    c.dl->AddRect(c.p(ax, y0 + 3), c.p(axe, y0 + h - 3), on ? c.P.accent : c.P.accentDim, c.s(3));
    char t[48];
    if (u.mapVel) std::snprintf(t, sizeof t, "%d - %d", static_cast<int>(vmin), static_cast<int>(vmax));
    else std::snprintf(t, sizeof t, "%s - %s", noteName(static_cast<int>(vmin)).c_str(), noteName(static_cast<int>(vmax)).c_str());
    textMid(c, ax, axe, y0 + h * 0.5f, t, c.P.text, 8, true);
    // drag: edges move one end, the middle moves the range
    ImGui::SetCursorScreenPos(c.p(bx0, y0));
    ImGui::InvisibleButton("##rng", c.F.sz(bw, h));
    static int mode = 0;
    static float grab = 0;
    const float mx = ImGui::GetIO().MousePos.x;
    const float val = std::clamp(lo + (mx - c.p(bx0, 0).x) / c.s(bw) * static_cast<float>(hi - lo + 1), static_cast<float>(lo), static_cast<float>(hi) + 0.999f);
    const float edge = 6;
    const bool nearMin = std::fabs(mx - c.p(ax, 0).x) < c.s(edge), nearMax = std::fabs(mx - c.p(axe, 0).x) < c.s(edge);
    if (ImGui::IsItemHovered() && (nearMin || nearMax)) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    if (ImGui::IsItemActivated()) {
      mode = nearMin ? 1 : nearMax ? 2 : (val >= vmin && val <= vmax + 1) ? 3 : 0;
      grab = val - vmin;
      if (mode == 0) {  // click outside the range: the nearer end jumps there
        mode = std::fabs(val - vmin) < std::fabs(val - vmax) ? 1 : 2;
      }
    }
    if (ImGui::IsItemActive()) {
      const float v = std::floor(val);
      if (mode == 1) setv(c, oscP(o, kMin), std::min(v, vmax), true);
      if (mode == 2) setv(c, oscP(o, kMax), std::max(v, vmin), true);
      if (mode == 3) {
        const float span = vmax - vmin;
        const float nmin = std::clamp(std::floor(val - grab), static_cast<float>(lo), static_cast<float>(hi) - span);
        setv(c, oscP(o, kMin), nmin, true);
        setv(c, oscP(o, kMax), nmin + span, true);
      }
    }
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
      setv(c, oscP(o, kMin), lo);
      setv(c, oscP(o, kMax), hi);
    }
    if (ImGui::IsItemHovered())
      c.a.setHint(std::string(u.mapVel ? "Velocity" : "Key") + " range of " + nm[o] + ": drag the ends or the range \xC2\xB7 double-click: full range");
    ImGui::PopID();
  }
}
}  // namespace

}  // namespace gl::packs::plasma::ui
