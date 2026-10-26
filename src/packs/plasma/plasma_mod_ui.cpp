// Gridline — "plasma" pack: the lower panel (y 388..655): MACROS | ENV 1-4 |
// LFO 1-10 | VELO / NOTE + VOICING. Every tab carries a drag handle that
// drops its source on a control (modulation).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include "../../core/I18n.h"
#include "plasma_ui.h"

namespace gl::packs::plasma::ui {

namespace {

constexpr float kTabY = kLowerY + 6, kTabH = 24, kBodyY = kTabY + kTabH;  // 394..418, body 418..655
constexpr float kMacX = 0, kEnvX = 165, kEnvW = 350, kLfoX = 517, kLfoW = 521, kVelX = 1040, kVelW = 150;
constexpr float kSecH = kStripY - kLowerY, kKr = 15.0f;  // section frame height, knob radius
// Section frames (x, w): each section sits in a frame; neighbours share the gutter.
constexpr float kFrame[4][2] = {{0, 164}, {164, 352}, {516, 523}, {1039, 151}};
// The tab row of a section and the divider under it.
void tabBand(const Ctx& c, int s) {
  const float x = kFrame[s][0] + kGutter + 1, x1 = kFrame[s][0] + kFrame[s][1] - kGutter - 1;
  hline(c, x, x1, kBodyY - 0.5f, c.P.frame);
}

// ------------------------------------------------------------------ macros
void macros(Ctx& c) {
  moduleFrame(c, kFrame[0][0], kLowerY, kFrame[0][1], kSecH, kBodyY - kLowerY, "MACROS", -1, false);
  const float x0 = kMacX + kGutter + 2, cw = (kFrame[0][1] - 2 * kGutter - 4) / 2, ch = (kStripY - kGutter - 2 - kBodyY - 2) / 4;
  for (int i = 0; i < kMacros; ++i) {
    const int col = i / 4, row = i % 4;
    const float x = x0 + col * cw, y = kBodyY + 2 + row * ch;
    ImGui::PushID(i + 900);
    frameBox(c, x - 1, y - 1, cw + 2, ch + 2, c.P.panelHi);  // a raised tile per macro
    knob(c, macroP(i), x + 25, y + 22, 14.0f, nullptr, KnobNoLabel);
    char num[4];
    std::snprintf(num, sizeof num, "%d", i + 1);
    textR(c, x + cw - 8, y + 5, num, c.P.label, 9, true);
    sourceHandle(c, SrcMacro1 + i, x + cw - 14, y + 31, 6);
    // name (double-click: rename)
    std::string& nm = c.m.macroName[static_cast<size_t>(i)];
    ImGui::SetCursorScreenPos(c.p(x + 4, y + ch - 16));
    ImGui::InvisibleButton("##nm", c.F.sz(cw - 8, 13));
    const bool hv = ImGui::IsItemHovered();
    textMid(c, x + 4, x + cw - 4, y + ch - 10, nm.empty() ? (hv ? "rename\xE2\x80\xA6" : "") : nm.c_str(), nm.empty() ? c.P.textFaint : c.P.label, 8, true);
    if (hv && ImGui::IsMouseDoubleClicked(0)) ImGui::OpenPopup("##ren");
    if (hv) c.a.setHint("Macro " + std::to_string(i + 1) + (nm.empty() ? "" : " (" + nm + ")") + ": double-click the name to rename");
    if (ImGui::BeginPopup("##ren")) {
      char buf[64];
      std::snprintf(buf, sizeof buf, "%s", nm.c_str());
      if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
      ImGui::SetNextItemWidth(c.s(140));
      if (ImGui::InputText("##t", buf, sizeof buf, ImGuiInputTextFlags_EnterReturnsTrue)) {
        nm = buf;
        commit(c, "Rename macro");
        ImGui::CloseCurrentPopup();
      }
      ImGui::EndPopup();
    }
    ImGui::PopID();
  }
}

// ------------------------------------------------------------------ envelopes
double envSec(const Ctx& c, int e, int p) { return envTime(val(c, envP(e, p))) * val(c, G_EnvScale); }

void envelope(Ctx& c) {
  const int e = c.u.envTab;
  std::vector<std::string> names;
  for (int i = 0; i < kEnvs; ++i) names.push_back("ENV " + std::to_string(i + 1));
  if (!c.u.arp) frameBox(c, kFrame[1][0], kLowerY, kFrame[1][1], kSecH), tabBand(c, 1);
  const int t = tabs(c, "envtabs", kEnvX + 1, kTabY, kEnvW - 2, kTabH, names, c.u.arp ? -1 : e, SrcEnv1);
  if (t >= 0) c.u.envTab = t, c.u.arp = false;
  if (c.u.arp) return;
  ImGui::PushID(e + 1200);
  // graph
  const float gx = kEnvX + 4, gy = kBodyY + 7, gw = 320, gh = 134;
  displayWellPublic(c, gx, gy, gw, gh);
  const double win = 4.0 * std::exp2(static_cast<double>(c.u.envZoom));  // visible seconds
  const int sub = win <= 1 ? 10 : win <= 4 ? 4 : win <= 16 ? 1 : 0;
  for (int s = 1; s < static_cast<int>(win) + 1 && sub; ++s) {
    const float x = gx + gw * static_cast<float>(s / win);
    if (x < gx + gw) {
      vline(c, x, gy, gy + gh, c.P.gridStrong);
      char b[12];
      std::snprintf(b, sizeof b, "%ds", s);
      textR(c, x - 3, gy + gh - 12, b, c.P.textFaint, 7.5f, true);
    }
  }
  const double A = envSec(c, e, E_Attack), H = envSec(c, e, E_Hold), D = envSec(c, e, E_Decay), R = envSec(c, e, E_Release);
  const double S = levelGain(val(c, envP(e, E_Sustain))), L0 = val(c, envP(e, E_Start)), L1 = val(c, envP(e, E_End));
  const double cA = (val(c, envP(e, E_CurveA)) - 50) / 50, cD = (val(c, envP(e, E_CurveD)) - 50) / 50, cR = (val(c, envP(e, E_CurveR)) - 50) / 50;
  auto X = [&](double sec) { return gx + gw * static_cast<float>(std::min(sec / win, 1.0)); };
  auto Y = [&](double lv) { return gy + gh - 3 - static_cast<float>(lv) * (gh - 6); };
  std::vector<ImVec2> pts;
  auto seg = [&](double t0, double t1, double v0, double v1, double cv) {
    const int n = 40;
    for (int i = 0; i <= n; ++i) {
      const double f = static_cast<double>(i) / n;
      pts.push_back(c.p(X(t0 + (t1 - t0) * f), Y(v0 + (v1 - v0) * bendCurve(f, cv))));
    }
  };
  seg(0, A, L0, 1, cA);
  seg(A, A + H, 1, 1, 0);
  seg(A + H, A + H + D, 1, S, cD);
  seg(A + H + D, A + H + D + R, S, L1, cR);
  c.dl->PushClipRect(c.p(gx, gy), c.p(gx + gw, gy + gh), true);
  skin::curveArea(c.dl, pts.data(), static_cast<int>(pts.size()), c.p(0, gy + gh).y, c.P.accent, std::max(1.6f, c.s(1.7f)), 0.2f);
  // live level / stage
  if (c.hasLive && c.live.voices > 0 && c.live.envStage[e] > 0) {
    double tx = 0;
    switch (c.live.envStage[e]) {
      case 1: tx = c.live.envTime[e]; break;
      case 2: tx = A + c.live.envTime[e]; break;
      case 3: tx = A + H + c.live.envTime[e]; break;
      case 4: tx = A + H + D; break;
      default: tx = A + H + D + c.live.envTime[e]; break;
    }
    c.dl->AddCircleFilled(c.p(X(tx), Y(c.live.env[e])), c.s(3.5f), c.P.mod);
  }
  // handles: attack, hold, decay / sustain, release; curve handles at segment middles
  struct H_ {
    float x, y;
    int kind;  // 0 A, 1 H, 2 D/S, 3 R, 4 curve A, 5 curve D, 6 curve R
  };
  const H_ hs[] = {{X(A), Y(1), 0},
                   {X(A + H), Y(1), 1},
                   {X(A + H + D), Y(S), 2},
                   {X(A + H + D + R), Y(L1), 3},
                   {X(A * 0.5), Y(L0 + (1 - L0) * bendCurve(0.5, cA)), 4},
                   {X(A + H + D * 0.5), Y(1 + (S - 1) * bendCurve(0.5, cD)), 5},
                   {X(A + H + D + R * 0.5), Y(S + (L1 - S) * bendCurve(0.5, cR)), 6}};
  ImGui::SetCursorScreenPos(c.p(gx, gy));
  ImGui::InvisibleButton("##eg", c.F.sz(gw, gh));
  const bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
  static int drag = -1;
  static float dragY0 = 0, dragC0 = 0;
  const ImVec2 m = ImGui::GetIO().MousePos;
  int hot = -1;
  float best = c.s(8);
  for (const auto& h : hs) {
    const ImVec2 q = c.p(h.x, h.y);
    const float d = std::hypot(q.x - m.x, q.y - m.y);
    if (d < best) best = d, hot = h.kind;
  }
  if (ImGui::IsItemActivated()) {
    drag = hot;
    dragY0 = m.y;
    if (hot >= 4) dragC0 = valf(c, envP(e, hot == 4 ? E_CurveA : hot == 5 ? E_CurveD : E_CurveR));
  }
  if (act && drag >= 0) {
    const double sec = std::max(0.0, static_cast<double>((m.x - c.p(gx, 0).x) / c.s(gw)) * win);
    const double lv = std::clamp(static_cast<double>((c.p(0, gy + gh - 3).y - m.y) / c.s(gh - 6)), 0.0, 1.0);
    const double scale = std::max(1e-6, val(c, G_EnvScale));
    switch (drag) {
      case 0: setv(c, envP(e, E_Attack), envTimeInv(sec / scale), true); break;
      case 1: setv(c, envP(e, E_Hold), envTimeInv(std::max(0.0, sec - A) / scale), true); break;
      case 2:
        setv(c, envP(e, E_Decay), envTimeInv(std::max(0.0, sec - A - H) / scale), true);
        setv(c, envP(e, E_Sustain), std::sqrt(lv), true);
        break;
      case 3: setv(c, envP(e, E_Release), envTimeInv(std::max(0.0, sec - A - H - D) / scale), true); break;
      default: {
        const int k = envP(e, drag == 4 ? E_CurveA : drag == 5 ? E_CurveD : E_CurveR);
        const float dir = drag == 4 ? 1.f : -1.f;  // rising segment bends up with an upward drag
        setv(c, k, std::clamp(dragC0 + (dragY0 - m.y) / c.s(1.2f) * dir, 0.0f, 100.0f), true);
        break;
      }
    }
  }
  if (ImGui::IsItemDeactivated()) drag = -1, c.a.doc.endGesture();
  if (hov && ImGui::IsMouseDoubleClicked(0) && hot >= 4) setv(c, envP(e, hot == 4 ? E_CurveA : hot == 5 ? E_CurveD : E_CurveR), 50);
  for (const auto& h : hs) {
    const bool lit = h.kind == hot || h.kind == drag;
    if (h.kind < 4) c.dl->AddCircleFilled(c.p(h.x, h.y), c.s(lit ? 4.5f : 3.4f), lit ? c.P.text : c.P.accent);
    else c.dl->AddCircle(c.p(h.x, h.y), c.s(lit ? 3.6f : 2.7f), lit ? c.P.text : theme::withAlpha(c.P.accent, 0.8f), 12, std::max(1.f, c.s(1.1f)));
  }
  c.dl->PopClipRect();
  if (hov) c.a.setHint("Drag the points (times, sustain) and the rings (curves) \xC2\xB7 double-click a ring: straight");
  // right column: zoom, fit, more
  {
    const float rx = gx + gw + 6;
    for (int i = 0; i < 2; ++i) {
      ImGui::PushID(i + 30);
      ImGui::SetCursorScreenPos(c.p(rx, gy + 4 + i * 96.f));
      if (ImGui::InvisibleButton("##z", c.F.sz(18, 18))) c.u.envZoom = std::clamp(c.u.envZoom + (i == 0 ? -1 : 1), -3, 3);
      iconArrow(c.dl, c.p(rx + 9, gy + 13 + i * 96.f), c.s(9), i == 0 ? 2 : 3, ImGui::IsItemHovered() ? c.P.text : c.P.textDim);
      if (ImGui::IsItemHovered()) c.a.setHint(i == 0 ? "Zoom in" : "Zoom out");
      ImGui::PopID();
    }
    ImGui::SetCursorScreenPos(c.p(rx, gy + 54));
    if (ImGui::InvisibleButton("##fit", c.F.sz(18, 18))) {
      const double tot = std::max(0.05, A + H + D + R);
      int z = -3;
      while (z < 3 && 4.0 * std::exp2(z) < tot * 1.1) ++z;
      c.u.envZoom = z;
    }
    {
      const ImVec2 q = c.p(rx + 8, gy + 61);
      const ImU32 col = ImGui::IsItemHovered() ? c.P.text : c.P.textDim;
      c.dl->AddCircle(q, c.s(4.5f), col, 14, std::max(1.2f, c.s(1.4f)));
      c.dl->AddLine(ImVec2(q.x + c.s(3.2f), q.y + c.s(3.2f)), ImVec2(q.x + c.s(7), q.y + c.s(7)), col, std::max(1.4f, c.s(1.6f)));
      if (ImGui::IsItemHovered()) c.a.setHint("Fit the envelope in the view");
    }
    moreButton(c, "envmore", rx, gy + 30, {envP(e, E_Start), envP(e, E_End), envP(e, E_LegatoInv), envP(e, E_Steal), G_EnvScale}, {},
               "Envelope options: start / end level, legato retrigger, voice steal, global time scale");
  }
  // values row
  {
    const int ks[5] = {E_Attack, E_Hold, E_Decay, E_Sustain, E_Release};
    wellRect(c, gx, kBodyY + 145, gw, 17, 2);
    for (int i = 0; i < 5; ++i) {
      const std::string s = ptext(c, envP(e, ks[i]));
      textC(c, kEnvX + 38 + i * 52.f, kBodyY + 148.5f, s.c_str(), c.P.accent, 8.5f, true);
    }
  }
  // knobs
  const char* ls[5] = {"ATK", "HOLD", "DEC", "SUS", "REL"};
  const int ks[5] = {E_Attack, E_Hold, E_Decay, E_Sustain, E_Release};
  deck(c, kFrame[1][0], kBodyY + 167, kFrame[1][1], kStripY - kBodyY - 167);
  for (int i = 0; i < 5; ++i) knob(c, envP(e, ks[i]), kEnvX + 38 + i * 52.f, kBodyY + 194, kKr, ls[i]);
  // BPM / MS
  {
    const bool bpm = val(c, envP(e, E_Bpm)) >= 0.5;
    const float bx = kEnvX + 287, by = kBodyY + 184;
    wellRect(c, bx, by, 54, 20, 3);
    ImGui::SetCursorScreenPos(c.p(bx, by));
    if (ImGui::InvisibleButton("##bpm", c.F.sz(54, 20))) setv(c, envP(e, E_Bpm), bpm ? 0 : 1);
    text(c, bx + 7, by + 5, "BPM", bpm ? c.P.accent : c.P.textFaint, 8.5f, true);
    text(c, bx + 32, by + 5, "MS", bpm ? c.P.textFaint : c.P.accent, 8.5f, true);
    if (ImGui::IsItemHovered()) c.a.setHint("Envelope times in milliseconds or in beats");
  }
  ImGui::PopID();
}

// ------------------------------------------------------------------ LFOs
Curve& lfoShapeForEdit(Ctx& c, int l) {
  LfoData& d = c.m.lfo[static_cast<size_t>(l)];
  if (d.pts.empty()) {
    const int sh = vali(c, lfoP(l, L_Shape));
    d.pts = sh >= 1 ? lfoFactoryShape(sh) : linearCurve();
    const auto& n = lfoShapeNames();
    d.name = sh >= 1 && sh < static_cast<int>(n.size()) ? n[static_cast<size_t>(sh)] : "Custom";
    setv(c, lfoP(l, L_Shape), 0);
  }
  return d.pts;
}
Curve lfoShapeOf(const Ctx& c, int l) {
  const LfoData& d = c.m.lfo[static_cast<size_t>(l)];
  if (!d.pts.empty()) return d.pts;
  const int sh = vali(c, lfoP(l, L_Shape));
  if (sh >= 1) return lfoFactoryShape(sh);
  Curve tri = {{0, 0, 0}, {0.5f, 1, 0}, {1, 0, 0}};
  return tri;
}

// Paints one grid cell of a shape: 1 flat, 2 ramp up, 3 ramp down.
void paintCell(Curve& cv, float x0, float x1, int tool, float y) {
  constexpr float eps = 1e-4f;
  const float a = x0 <= 0 ? 0.f : x0 + eps, b = x1 >= 1 ? 1.f : x1 - eps;
  Curve out;
  for (const auto& p : cv)
    if (p.x < a - eps * 0.5f || p.x > b + eps * 0.5f) out.push_back(p);
  float ya = y, yb = y;
  if (tool == 2) ya = 0, yb = y;
  if (tool == 3) ya = y, yb = 0;
  out.push_back({a, ya, 0});
  out.push_back({b, yb, 0});
  std::sort(out.begin(), out.end(), [](const CurvePt& p, const CurvePt& q) { return p.x < q.x; });
  if (out.front().x > 0) out.insert(out.begin(), {0, out.front().y, 0});
  if (out.back().x < 1) out.push_back({1, out.back().y, 0});
  cv = out;
}

void chaosPreview(const Ctx& c, int type, float x, float y, float w, float h) {
  // a short trace of the attractor (display only)
  double px = 0.1, py = 0, pz = 0;
  std::vector<ImVec2> pts;
  const int n = 1600;
  double mnx = 1e9, mxx = -1e9, mny = 1e9, mxy = -1e9;
  std::vector<std::pair<double, double>> raw;
  for (int i = 0; i < n + 200; ++i) {
    double dx, dy, dz, dt;
    if (type == LfoLorenz) {
      dt = 0.01;
      dx = 10 * (py - px), dy = px * (28 - pz) - py, dz = px * py - 8.0 / 3.0 * pz;
    } else {
      dt = 0.03;
      dx = -py - pz, dy = px + 0.2 * py, dz = 0.2 + pz * (px - 5.7);
    }
    px += dx * dt, py += dy * dt, pz += dz * dt;
    if (i < 200) continue;
    raw.push_back({px, type == LfoLorenz ? pz : py});
    mnx = std::min(mnx, px), mxx = std::max(mxx, px), mny = std::min(mny, raw.back().second), mxy = std::max(mxy, raw.back().second);
  }
  for (const auto& [a, b] : raw)
    pts.push_back(c.p(x + 6 + (w - 12) * static_cast<float>((a - mnx) / std::max(1e-9, mxx - mnx)),
                      y + h - 6 - (h - 12) * static_cast<float>((b - mny) / std::max(1e-9, mxy - mny))));
  c.dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), theme::withAlpha(c.P.accent, 0.7f), 0, std::max(1.0f, c.s(1.0f)));
}

void lfo(Ctx& c) {
  int& first = c.u.lfoFirst;
  const int l = c.u.lfoTab;
  // tabs (6 visible) with a scroll arrow pair
  {
    std::vector<std::string> names;
    for (int i = 0; i < 6; ++i) names.push_back("LFO " + std::to_string(first + i + 1));
    if (!c.u.arp) frameBox(c, kFrame[2][0], kLowerY, kFrame[2][1], kSecH), tabBand(c, 2);
    const int t = tabs(c, "lfotabs", kLfoX + 1, kTabY, kLfoW - 25, kTabH, names, c.u.arp ? -1 : l - first, SrcLfo1 + first);
    if (t >= 0) c.u.lfoTab = first + t, c.u.arp = false;
    const float sx = kLfoX + kLfoW - 24;
    ImGui::SetCursorScreenPos(c.p(sx, kTabY));
    if (ImGui::InvisibleButton("##lfoscroll", c.F.sz(20, kTabH))) first = first == 0 ? kLfos - 6 : 0;
    {
      const ImVec2 a = c.p(sx, kTabY + kGutter), b = c.p(sx + 19, kTabY + kTabH - 1);
      c.dl->AddRectFilled(a, b, ImGui::IsItemHovered() ? c.P.headOn : c.P.head, c.s(2.0f));
    }
    iconArrow(c.dl, c.p(sx + 9.5f, kTabY + kTabH * 0.5f + 0.5f), c.s(8), first == 0 ? 1 : 0, c.P.textDim);
    if (ImGui::IsItemHovered()) c.a.setHint(first == 0 ? "LFO 7-10" : "LFO 1-6");
  }
  if (c.u.arp) return;
  ImGui::PushID(l + 1300);
  const int type = vali(c, lfoP(l, L_Type));
  // left column: type, mode, mono
  {
    const float x = kLfoX + 4;
    choiceBox(c, lfoP(l, L_Type), x, kBodyY + 12, 100, 18);
    const char* modes[3] = {"FREE", "RETRIG", "ENVELOPE"};
    const int mode = vali(c, lfoP(l, L_Mode));
    for (int i = 0; i < 3; ++i) {
      const float y = kBodyY + 44 + i * 28.f;
      ImGui::PushID(i + 70);
      ImGui::SetCursorScreenPos(c.p(x, y));
      if (ImGui::InvisibleButton("##m", c.F.sz(100, 24))) setv(c, lfoP(l, L_Mode), i);
      const bool on = mode == i, hv = ImGui::IsItemHovered();
      if (on || hv) panelRect(c, x, y, 100, 24, on ? c.P.accentSoft : c.P.panelHi, 3);
      text(c, x + 8, y + 7, modes[i], on ? c.P.accent : c.P.textDim, 8.5f, true);
      // glyphs: loop, retrigger arrow, one-shot bar
      const ImVec2 g = c.p(x + 86, y + 12);
      const ImU32 gc = on ? c.P.accent : c.P.textFaint;
      if (i == 0) c.dl->AddRect(ImVec2(g.x - c.s(6), g.y - c.s(3)), ImVec2(g.x + c.s(6), g.y + c.s(3)), gc, c.s(3), 0, std::max(1.f, c.s(1.2f)));
      else if (i == 1) {
        c.dl->PathArcTo(g, c.s(4.5f), -1.2f, 3.6f, 12);
        c.dl->PathStroke(gc, 0, std::max(1.f, c.s(1.2f)));
      } else {
        c.dl->AddLine(ImVec2(g.x - c.s(6), g.y), ImVec2(g.x + c.s(4), g.y), gc, std::max(1.f, c.s(1.2f)));
        c.dl->AddLine(ImVec2(g.x + c.s(6), g.y - c.s(4)), ImVec2(g.x + c.s(6), g.y + c.s(4)), gc, std::max(1.f, c.s(1.2f)));
      }
      if (hv)
        c.a.setHint(i == 0 ? "Free: the LFO runs on, shared by every note" : i == 1 ? "Retrig: restarts with each note" : "Envelope: plays once per note (right-click a point: sustain loop)");
      ImGui::PopID();
    }
    checkBox(c, lfoP(l, L_Mono), x + 4, kBodyY + 150, "MONO", 8.5f);
  }
  vline(c, kLfoX + 108, kBodyY + 8, kBodyY + 162, c.P.line);
  vline(c, kLfoX + 473, kBodyY + 8, kBodyY + 162, c.P.line);
  deck(c, kFrame[2][0], kBodyY + 170, kFrame[2][1], kStripY - kBodyY - 170);
  // graph
  const float gx = kLfoX + 112, gy = kBodyY + 7, gw = 357, gh = 134;
  displayWellPublic(c, gx, gy, gw, gh);
  const int gxN = std::max(1, vali(c, lfoP(l, L_GridX))), gyN = std::max(1, vali(c, lfoP(l, L_GridY)));
  gridLines(c, gx, gy, gw, gh, gxN, gyN, false);
  LfoData& ld = c.m.lfo[static_cast<size_t>(l)];
  int& tool = c.u.lfoTool;
  if (type == LfoLorenz || type == LfoRossler) {
    chaosPreview(c, type, gx, gy, gw, gh);
    textC(c, gx + gw * 0.5f, gy + 6, type == LfoLorenz ? "Lorenz attractor" : "R\xC3\xB6ssler attractor", c.P.textFaint, 8, true);
  } else if (type == LfoPath) {
    // X / Y path: points move the X and Y outputs; drag to edit, double-click adds / removes
    if (ld.path.empty() && ld.pathX.empty()) {
      textC(c, gx + gw * 0.5f, gy + gh * 0.5f - 6, "Double-click to add path points", c.P.textFaint, 8.5f, true);
    }
    Curve& pts = ld.path;
    if (ld.pathX.size() != pts.size()) ld.pathX.resize(pts.size(), 0.5f);
    const ImVec2 a = c.p(gx, gy), b = c.p(gx + gw, gy + gh);
    ImGui::SetCursorScreenPos(a);
    ImGui::InvisibleButton("##path", ImVec2(b.x - a.x, b.y - a.y), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const ImVec2 m = ImGui::GetIO().MousePos;
    auto P = [&](size_t i) { return ImVec2(a.x + ld.pathX[i] * (b.x - a.x), b.y - pts[i].y * (b.y - a.y)); };
    int hot = -1;
    for (size_t i = 0; i < pts.size(); ++i)
      if (std::hypot(P(i).x - m.x, P(i).y - m.y) < c.s(6)) hot = static_cast<int>(i);
    static int sel = -1;
    bool changed = false;
    if (ImGui::IsItemActivated()) sel = hot;
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
      if (hot >= 0) pts.erase(pts.begin() + hot), ld.pathX.erase(ld.pathX.begin() + hot), sel = -1;
      else {
        const float nx = std::clamp((m.x - a.x) / (b.x - a.x), 0.f, 1.f), ny = std::clamp((b.y - m.y) / (b.y - a.y), 0.f, 1.f);
        pts.push_back({pts.empty() ? 0.f : std::min(1.f, pts.back().x + 0.1f), ny, 0});
        ld.pathX.push_back(nx);
        float t = 0;  // re-space the time positions evenly
        for (size_t i = 0; i < pts.size(); ++i) pts[i].x = pts.size() > 1 ? t / static_cast<float>(pts.size() - 1) : 0.f, t += 1;
      }
      changed = true;
    }
    if (ImGui::IsItemActive() && sel >= 0 && sel < static_cast<int>(pts.size()) && ImGui::IsMouseDragging(0, 0)) {
      ld.pathX[static_cast<size_t>(sel)] = std::clamp((m.x - a.x) / (b.x - a.x), 0.f, 1.f);
      pts[static_cast<size_t>(sel)].y = std::clamp((b.y - m.y) / (b.y - a.y), 0.f, 1.f);
      changed = true;
    }
    if (ImGui::IsItemDeactivated()) sel = -1;
    for (size_t i = 0; i + 1 < pts.size(); ++i) c.dl->AddLine(P(i), P(i + 1), c.P.accent, std::max(1.4f, c.s(1.6f)));
    if (pts.size() > 2) c.dl->AddLine(P(pts.size() - 1), P(0), theme::withAlpha(c.P.accent, 0.35f), std::max(1.f, c.s(1.2f)));
    for (size_t i = 0; i < pts.size(); ++i) {
      c.dl->AddCircleFilled(P(i), c.s(static_cast<int>(i) == hot ? 4.5f : 3.4f), static_cast<int>(i) == hot ? c.P.text : c.P.accent);
      char b2[16];
      std::snprintf(b2, sizeof b2, "%d", static_cast<int>(i) + 1);
      c.dl->AddText(c.a.fontBold, font(c, 7.5f), ImVec2(P(i).x + c.s(5), P(i).y - c.s(11)), c.P.textDim, b2);
    }
    if (c.hasLive && c.live.voices > 0) {
      // live X / Y position
      c.dl->AddCircleFilled(ImVec2(a.x + (c.live.lfo[l] * 0.5f + 0.5f) * (b.x - a.x), b.y - (c.live.lfo[l] * 0.5f + 0.5f) * (b.y - a.y)), c.s(3),
                            c.P.mod);
    }
    if (changed) commit(c, "Edit LFO path", 1300 + l);
    if (ImGui::IsItemHovered()) c.a.setHint("Path LFO: the X output follows left / right, Y up / down \xC2\xB7 double-click: add / remove a point");
  } else if (type == LfoSH) {
    uint32_t rng = 12345u + static_cast<uint32_t>(l);
    for (int i = 0; i < gxN; ++i) {
      rng = rng * 1664525u + 1013904223u;
      const float v = (rng >> 8) / 16777216.f;
      const float x0 = gx + gw * i / gxN, x1 = gx + gw * (i + 1) / gxN;
      c.dl->AddRectFilled(c.p(x0 + 1, gy + gh - v * gh), c.p(x1 - 1, gy + gh), theme::withAlpha(c.P.accent, 0.25f));
      hline(c, x0, x1, gy + gh - v * gh, c.P.accent);
    }
    textC(c, gx + gw * 0.5f, gy + 6, "Sample & hold: a new random value each step (grid X)", c.P.textFaint, 8, true);
  } else {
    // shape editor (point tool) or cell painting (step / ramp tools)
    Curve view = lfoShapeOf(c, l);
    if (tool == 0) {
      Curve edit = view;
      int loop = ld.loop;
      if (curveEditor(c, "lfoshape", edit, gx, gy, gw, gh, gxN, gyN, c.u.lfoSel, true, c.P.accent, &loop)) {
        Curve& cv = lfoShapeForEdit(c, l);
        cv = edit;
        ld.loop = loop;
        if (ld.name.empty() || ld.name != "Custom") ld.name = "Custom";
        commit(c, "Edit LFO shape", 1300 + l);
      }
    } else {
      drawCurve(c, view, gx, gy, gw, gh, c.P.accent, true);
      ImGui::SetCursorScreenPos(c.p(gx, gy));
      ImGui::InvisibleButton("##paint", c.F.sz(gw, gh));
      if (ImGui::IsItemActive()) {
        const ImVec2 m = ImGui::GetIO().MousePos, a = c.p(gx, gy), b = c.p(gx + gw, gy + gh);
        const float fx = std::clamp((m.x - a.x) / (b.x - a.x), 0.f, 0.9999f), fy = std::clamp((b.y - m.y) / (b.y - a.y), 0.f, 1.f);
        const int cell = static_cast<int>(fx * gxN);
        const float y = std::round(fy * gyN) / gyN;
        Curve& cv = lfoShapeForEdit(c, l);
        paintCell(cv, static_cast<float>(cell) / gxN, static_cast<float>(cell + 1) / gxN, tool, y);
        commit(c, "Paint LFO shape", 1300 + l);
      }
      if (ImGui::IsItemHovered()) c.a.setHint("Paint grid cells (the grid X / Y steppers set the cell size)");
    }
    // live phase
    if (c.hasLive && (c.live.voices > 0 || vali(c, lfoP(l, L_Mode)) == LfoFree)) {
      const float ph = std::clamp(c.live.lfoPhase[l], 0.f, 1.f);
      vline(c, gx + gw * ph, gy, gy + gh, theme::withAlpha(c.P.mod, 0.6f));
      c.dl->AddCircleFilled(c.p(gx + gw * ph, gy + gh - static_cast<float>(curveAt(view, ph)) * gh), c.s(3.2f), c.P.mod);
    }
    if (ld.loop >= 0 && vali(c, lfoP(l, L_Mode)) == LfoEnvelope && ld.loop < static_cast<int>(view.size()))
      textR(c, gx + gw - 4, gy + 4, "sustain loop", c.P.warn, 7.5f, true);
  }
  // right column: options + tools
  {
    const float rx = gx + gw + 9;
    moreButton(c, "lfomore", rx + 5, gy + 2, {lfoP(l, L_Rate10x), lfoP(l, L_Swing), lfoP(l, L_PhaseSnap), G_LfoScale, G_LfoScaleRate},
               [&] {
                 ImGui::Separator();
                 static Curve clip;
                 static int clipLoop = -1;
                 if (ImGui::MenuItem(i18n::tr("Copy shape"))) clip = lfoShapeOf(c, l), clipLoop = ld.loop;
                 if (ImGui::MenuItem(i18n::tr("Paste shape"), nullptr, false, !clip.empty())) {
                   lfoShapeForEdit(c, l) = clip;
                   ld.loop = clipLoop;
                   commit(c, "Paste LFO shape");
                 }
                 if (ImGui::MenuItem(i18n::tr("Flip vertically"))) {
                   for (auto& p : lfoShapeForEdit(c, l)) p.y = 1 - p.y, p.c = -p.c;
                   commit(c, "Flip LFO shape");
                 }
                 if (ImGui::MenuItem(i18n::tr("Reverse"))) {
                   Curve& cv = lfoShapeForEdit(c, l);
                   Curve r;
                   for (auto it = cv.rbegin(); it != cv.rend(); ++it) r.push_back({1 - it->x, it->y, 0});
                   for (size_t i = 1; i < r.size(); ++i) r[i].c = -cv[cv.size() - i].c;
                   cv = r;
                   commit(c, "Reverse LFO shape");
                 }
                 if (ImGui::MenuItem(i18n::tr("Clear"))) {
                   lfoShapeForEdit(c, l) = {{0, 0.5f, 0}, {1, 0.5f, 0}};
                   ld.loop = -1;
                   commit(c, "Clear LFO shape");
                 }
               },
               "LFO options: rate x10, swing, phase snap, global LFO scaling, copy / paste / flip / reverse the shape");
    const char* tips[4] = {"Point tool: drag, add and bend points", "Paint flat steps", "Paint rising ramps", "Paint falling ramps"};
    for (int i = 0; i < 4; ++i) {
      const float by = gy + 28 + i * 26.f;
      ImGui::PushID(i + 80);
      ImGui::SetCursorScreenPos(c.p(rx, by));
      if (ImGui::InvisibleButton("##tool", c.F.sz(30, 22))) tool = i;
      const bool on = tool == i, hv = ImGui::IsItemHovered();
      if (on || hv) panelRect(c, rx, by, 30, 22, on ? c.P.accentSoft : c.P.panelHi, 3);
      const ImU32 col = on ? c.P.accent : c.P.textDim;
      const ImVec2 q = c.p(rx + 15, by + 11);
      if (i == 0) c.dl->AddCircleFilled(q, c.s(3.5f), col);
      else if (i == 1) c.dl->AddLine(ImVec2(q.x - c.s(8), q.y), ImVec2(q.x + c.s(8), q.y), col, std::max(1.4f, c.s(1.6f)));
      else if (i == 2) c.dl->AddLine(ImVec2(q.x - c.s(8), q.y + c.s(4)), ImVec2(q.x + c.s(8), q.y - c.s(4)), col, std::max(1.4f, c.s(1.6f)));
      else c.dl->AddLine(ImVec2(q.x - c.s(8), q.y - c.s(4)), ImVec2(q.x + c.s(8), q.y + c.s(4)), col, std::max(1.4f, c.s(1.6f)));
      if (hv) c.a.setHint(tips[i]);
      ImGui::PopID();
    }
  }
  // row under the graph: shape presets, direction, grid
  {
    const float y = kBodyY + 145;
    const int sh = vali(c, lfoP(l, L_Shape));
    const auto& sn = lfoShapeNames();
    std::string nm = !ld.pts.empty() ? (ld.name.empty() ? std::string("Custom") : ld.name)
                                     : (sh >= 1 && sh < static_cast<int>(sn.size()) ? sn[static_cast<size_t>(sh)] : std::string("Triangle"));
    int step = 0;
    dropField(c, "shape", gx + 4, y, 190, 18, nm.c_str(),
              [&] {
                for (int i = 1; i < static_cast<int>(sn.size()); ++i)
                  if (ImGui::MenuItem(sn[static_cast<size_t>(i)].c_str(), nullptr, ld.pts.empty() && sh == i)) {
                    ld.pts.clear();
                    ld.loop = -1;
                    ld.name.clear();
                    commit(c, "LFO shape");
                    setv(c, lfoP(l, L_Shape), i);
                  }
              },
              true, &step);
    if (step) {
      const int n = static_cast<int>(sn.size()) - 1;
      const int cur = ld.pts.empty() ? std::max(1, sh) : 1;
      if (!ld.pts.empty()) {
        ld.pts.clear();
        ld.name.clear();
        commit(c, "LFO shape");
      }
      setv(c, lfoP(l, L_Shape), ((cur - 1 + step + n) % n) + 1);
    }
    choiceBox(c, lfoP(l, L_Dir), gx + 202, y, 70, 18);
    labelBeside(c, gx + 280, y, 18, "X", c.P.textFaint, 8);
    stepper(c, lfoP(l, L_GridX), gx + 290, y, 34, 18);
    labelBeside(c, gx + 330, y, 18, "Y", c.P.textFaint, 8);
    stepper(c, lfoP(l, L_GridY), gx + 340, y, 34, 18);
  }
  // bottom row: host anchor, rate block, rise / delay / smooth / phase
  {
    const float ky = kBodyY + 194;
    toggleBtn(c, lfoP(l, L_Anchor), kLfoX + 12, ky - 11, 42, 22, "HOST", 8);
    const float bx = kLfoX + 64, bw = 240;
    wellRect(c, bx, ky - 17, bw, 34, 4);
    const bool bpm = val(c, lfoP(l, L_Bpm)) >= 0.5;
    ImGui::SetCursorScreenPos(c.p(bx + 4, ky - 13));  // J-128: the TRIP / DOT row
    if (ImGui::InvisibleButton("##bpm", c.F.sz(44, 20))) setv(c, lfoP(l, L_Bpm), bpm ? 0 : 1);
    text(c, bx + 8, ky - 7, "BPM", bpm ? c.P.accent : c.P.textFaint, 8.5f, true);
    text(c, bx + 32, ky - 7, "HZ", bpm ? c.P.textFaint : c.P.accent, 8.5f, true);
    if (ImGui::IsItemHovered()) c.a.setHint("Rate in tempo divisions or in Hz");
    knob(c, lfoP(l, L_Rate), bx + 76, ky - 3, 12.0f, nullptr, KnobNoLabel);
    cellLabel(c, bx + 76, ky + 20, "RATE");  // J-128: on the RISE / DELAY label line
    const std::string rv = ptext(c, lfoP(l, L_Rate));
    textC(c, bx + 128, ky - 7, rv.c_str(), c.P.accent, 9.5f, true);
    toggleBtn(c, lfoP(l, L_Trip), bx + 160, ky - 13, 36, 20, "TRIP", 7.5f);
    toggleBtn(c, lfoP(l, L_Dot), bx + 200, ky - 13, 36, 20, "DOT", 7.5f);
    knob(c, lfoP(l, L_Rise), kLfoX + 340, ky, kKr, "RISE");
    knob(c, lfoP(l, L_Delay), kLfoX + 390, ky, kKr, "DELAY");
    knob(c, lfoP(l, L_Smooth), kLfoX + 440, ky, kKr, "SMOOTH");
    knob(c, lfoP(l, L_Phase), kLfoX + 490, ky, kKr, "PHASE");
  }
  ImGui::PopID();
}

// ------------------------------------------------------------------ velo / note + voicing
void scalars(Ctx& c) {
  const int st = c.u.scalarTab;
  // one source handle per tab: VELO -> SrcVelo, NOTE -> SrcNote (consecutive ids)
  if (!c.u.arp) frameBox(c, kFrame[3][0], kLowerY, kFrame[3][1], kSecH), tabBand(c, 3);
  const int t = tabs(c, "scal", kVelX + 1, kTabY, kVelW - 4, kTabH, {"VELO", "NOTE"}, c.u.arp ? -1 : st, SrcVelo);
  if (t >= 0) c.u.scalarTab = t, c.u.arp = false;
  if (c.u.arp) return;
  const float gx = kVelX + 4, gy = kBodyY + 7, gw = kVelW - 10, gh = 134;
  displayWellPublic(c, gx, gy, gw, gh);
  gridLines(c, gx, gy, gw, gh, 4, 4, false);
  Curve& cv = st == 0 ? c.m.velo : c.m.note;
  Curve edit = cv.empty() ? linearCurve() : cv;
  static int sel = -1;
  if (curveEditor(c, st == 0 ? "velo" : "note", edit, gx, gy, gw, gh, 4, 4, sel, true, c.P.accent)) {
    cv = edit;
    commit(c, st == 0 ? "Edit velocity curve" : "Edit note curve", 1400 + st);
  }
  // voicing
  const float vy = kBodyY + 150;
  deck(c, kFrame[3][0], vy - 4, kFrame[3][1], kStripY - vy + 4);
  hline(c, kVelX + 4, kVelX + kVelW - 7, vy + 16.5f, c.P.line);
  text(c, kVelX + 8, vy + 2, "VOICING", c.P.label, 9, true);
  moreButton(c, "voicemore", kVelX + kVelW - 27, vy, {G_Priority, G_SameNote}, {}, "Voice priority and one voice per key");
  checkBox(c, G_Mono, kVelX + 8, vy + 27, "MONO", 8.5f);  // J-128: on the POLY baseline
  labelBeside(c, kVelX + 76, vy + 22, 18, "POLY", c.P.label, 8.5f);  // J-128: on the stepper baseline
  stepper(c, G_Poly, kVelX + 102, vy + 22, 40, 18);
  checkBox(c, G_Legato, kVelX + 8, vy + 52, "LEGATO", 8.5f);
  char b[32];
  std::snprintf(b, sizeof b, "%d / %d", c.hasLive ? c.live.voices : 0, vali(c, G_Poly));
  wellRect(c, kVelX + 76, vy + 47, 66, 18, 2);
  textMid(c, kVelX + 76, kVelX + 142, vy + 56, b, c.P.accent, 9, true);
}

}  // namespace

void displayWellPublic(const Ctx& c, float x, float y, float w, float h) {
  insetFrame(c, x, y, w, h, 0, 0, false);
}

void lowerPanel(Ctx& c) {
  panelRect(c, 0, kLowerY, kW, kStripY - kLowerY, c.P.frame);
  macros(c);
  if (c.u.arp) {  // the arpeggiator spans ENV..NOTE: one frame under all their tabs
    frameBox(c, kFrame[1][0], kLowerY, kW - kFrame[1][0], kSecH);
    hline(c, kFrame[1][0] + kGutter + 1, kW - kGutter - 1, kBodyY - 0.5f, c.P.frame);
  }
  envelope(c);
  lfo(c);
  scalars(c);
  if (c.u.arp) arpPanel(c);
}

}  // namespace gl::packs::plasma::ui
