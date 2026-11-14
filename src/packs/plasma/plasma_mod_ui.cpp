// Gridline — "plasma" pack: the motion views of the stage: Envelope 1-4 (a
// full-width graph and every value), LFO 1-10 (kind, tool bar, editor, timing,
// start, ease-in, point motion), Key and touch (response curves, wheels and
// expression, voice variation, restarts), and Amp and voices.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include "../../core/I18n.h"
#include "plasma_ui.h"

namespace gl::packs::plasma::ui {

namespace {

double envSec(const Ctx& c, int e, int p) { return envTime(val(c, envP(e, p))) * val(c, G_EnvScale); }

void envGraph(Ctx& c, int e, float gx, float gy, float gw, float gh) {
  ImGui::PushID(e + 1200);
  display(c, gx, gy, gw, gh, 0, 4);
  const double win = 4.0 * std::exp2(static_cast<double>(c.u.envZoom));  // visible seconds
  const int sub = win <= 1 ? 10 : win <= 4 ? 4 : win <= 16 ? 1 : 0;
  for (int s = 1; s < static_cast<int>(win) + 1 && sub; ++s) {
    const float x = gx + gw * static_cast<float>(s / win);
    if (x < gx + gw) {
      vline(c, x, gy, gy + gh, c.P.gridStrong);
      char b[12];
      std::snprintf(b, sizeof b, "%ds", s);
      textR(c, x - 3, gy + gh - 15, b, c.P.textDim, 9.5f, false);
    }
  }
  const double A = envSec(c, e, E_Attack), H = envSec(c, e, E_Hold), D = envSec(c, e, E_Decay), R = envSec(c, e, E_Release);
  const double S = levelGain(val(c, envP(e, E_Sustain))), L0 = val(c, envP(e, E_Start)), L1 = val(c, envP(e, E_End));
  const double cA = (val(c, envP(e, E_CurveA)) - 50) / 50, cD = (val(c, envP(e, E_CurveD)) - 50) / 50, cR = (val(c, envP(e, E_CurveR)) - 50) / 50;
  // J-202: the curve keeps 6 units clear of the frame so its handles are whole
  constexpr float kIn = 6;
  auto X = [&](double sec) { return gx + kIn + (gw - 2 * kIn) * static_cast<float>(std::min(sec / win, 1.0)); };
  auto Y = [&](double lv) { return gy + gh - kIn - static_cast<float>(lv) * (gh - 2 * kIn); };
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
  skin::curveArea(c.dl, pts.data(), static_cast<int>(pts.size()), c.p(0, gy + gh).y, c.P.fam[FamEnv], std::max(1.6f, c.s(2.0f)), 0.14f);
  // the control under the pointer marks its part of the shape (a dial or a field below)
  int part = -1;
  {
    const int hk = c.u.hoverK;
    const int ks[8] = {E_Attack, E_Hold, E_Decay, E_Sustain, E_Release, E_CurveA, E_CurveD, E_CurveR};
    const int seg[8] = {0, 1, 2, 4, 3, 0, 2, 3};  // 4: the sustain level
    for (int i = 0; i < 8; ++i)
      if (hk == envP(e, ks[i])) part = seg[i];
  }
  if (part >= 0 && part < 4) {
    const size_t i0 = static_cast<size_t>(part) * 41;
    if (i0 + 41 <= pts.size()) c.dl->AddPolyline(pts.data() + i0, 41, c.P.hot, 0, std::max(2.4f, c.s(3.0f)));
  } else if (part == 4) {
    hline(c, gx + kIn, gx + gw - kIn, Y(S), theme::withAlpha(c.P.hot, 0.8f));
  }
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
    c.dl->AddCircleFilled(c.p(X(tx), Y(c.live.env[e])), c.s(3.5f), c.P.hot);
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
    const double sec = std::max(0.0, static_cast<double>((m.x - c.p(gx + kIn, 0).x) / c.s(gw - 2 * kIn)) * win);
    const double lv = std::clamp(static_cast<double>((c.p(0, gy + gh - kIn).y - m.y) / c.s(gh - 2 * kIn)), 0.0, 1.0);
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
  c.dl->PopClipRect();
  for (const auto& h : hs) {
    const bool lit = h.kind == hot || h.kind == drag;
    if (h.kind < 4) c.dl->AddCircleFilled(c.p(h.x, h.y), c.s(lit ? 5.5f : 4.2f), lit ? c.P.hot : c.P.text);
    else c.dl->AddCircle(c.p(h.x, h.y), c.s(lit ? 4.4f : 3.4f), lit ? c.P.hot : theme::withAlpha(c.P.text, 0.8f), 12, std::max(1.f, c.s(1.2f)));
  }
  if (hov) c.a.setHint("Drag the points (times, sustain) and the rings (curves) \xC2\xB7 double-click a ring: straight");
  // right column (J-202): one stack of square buttons: zoom in, zoom out, fit, options
  {
    const float rx = gx + gw - 26, bs = 20;
    const char* tips[3] = {"Zoom in", "Zoom out", "Fit the envelope in the view"};
    for (int i = 0; i < 3; ++i) {
      const float by = gy + 6 + i * (bs + 4);
      ImGui::PushID(i + 30);
      ImGui::SetCursorScreenPos(c.p(rx, by));
      if (ImGui::InvisibleButton("##z", c.F.sz(bs, bs))) {
        if (i < 2) c.u.envZoom = std::clamp(c.u.envZoom + (i == 0 ? -1 : 1), -3, 3);
        else {
          const double tot = std::max(0.05, A + H + D + R);
          int z = -3;
          while (z < 3 && 4.0 * std::exp2(z) < tot * 1.1) ++z;
          c.u.envZoom = z;
        }
      }
      const bool h = ImGui::IsItemHovered();
      c.dl->AddRectFilled(c.p(rx, by), c.p(rx + bs, by + bs), h ? c.P.headOn : c.P.head, c.s(2));
      const ImU32 col = h ? c.P.text : c.P.textDim;
      const ImVec2 q = c.p(rx + bs * 0.5f, by + bs * 0.5f);
      const float t = std::max(1.2f, c.s(1.4f)), r = c.s(4.5f);
      if (i < 2) {
        c.dl->AddLine(ImVec2(q.x - r, q.y), ImVec2(q.x + r, q.y), col, t);
        if (i == 0) c.dl->AddLine(ImVec2(q.x, q.y - r), ImVec2(q.x, q.y + r), col, t);
      } else {  // fit: a span between two bars
        c.dl->AddLine(ImVec2(q.x - r, q.y - r * 0.8f), ImVec2(q.x - r, q.y + r * 0.8f), col, t);
        c.dl->AddLine(ImVec2(q.x + r, q.y - r * 0.8f), ImVec2(q.x + r, q.y + r * 0.8f), col, t);
        c.dl->AddLine(ImVec2(q.x - r * 0.6f, q.y), ImVec2(q.x + r * 0.6f, q.y), col, t);
      }
      if (h) c.a.setHint(tips[i]);
      ImGui::PopID();
    }
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

void lfoGraph(Ctx& c, int l, float gx, float gy, float gw, float gh) {
  const int type = vali(c, lfoP(l, L_Type));
  display(c, gx, gy, gw, gh, 0, 0);
  const int gxN = std::max(1, vali(c, lfoP(l, L_GridX))), gyN = std::max(1, vali(c, lfoP(l, L_GridY)));
  gridLines(c, gx, gy, gw, gh, gxN, gyN, false);
  LfoData& ld = c.m.lfo[static_cast<size_t>(l)];
  int& tool = c.u.lfoTool;
  if (type == LfoLorenz || type == LfoRossler) {
    chaosPreview(c, type, gx, gy, gw, gh);
    textC(c, gx + gw * 0.5f, gy + 6, type == LfoLorenz ? "Lorenz attractor" : "R\xC3\xB6ssler attractor", c.P.textDim, 10.0f, false);
  } else if (type == LfoPath) {
    // X / Y path: points move the X and Y outputs; drag to edit, double-click adds / removes
    if (ld.path.empty() && ld.pathX.empty()) {
      textC(c, gx + gw * 0.5f, gy + gh * 0.5f - 6, "Double-click to add path points", c.P.textDim, 10.0f, false);
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
    for (size_t i = 0; i + 1 < pts.size(); ++i) c.dl->AddLine(P(i), P(i + 1), c.P.fam[FamLfo], std::max(1.4f, c.s(1.6f)));
    if (pts.size() > 2) c.dl->AddLine(P(pts.size() - 1), P(0), theme::withAlpha(c.P.fam[FamLfo], 0.35f), std::max(1.f, c.s(1.2f)));
    for (size_t i = 0; i < pts.size(); ++i) {
      c.dl->AddCircleFilled(P(i), c.s(static_cast<int>(i) == hot ? 4.5f : 3.4f), static_cast<int>(i) == hot ? c.P.text : c.P.fam[FamLfo]);
      char b2[16];
      std::snprintf(b2, sizeof b2, "%d", static_cast<int>(i) + 1);
      c.dl->AddText(c.a.fontBold, font(c, 9.5f), ImVec2(P(i).x + c.s(5), P(i).y - c.s(11)), c.P.textDim, b2);
    }
    if (c.hasLive && c.live.voices > 0) {
      // live X / Y position
      c.dl->AddCircleFilled(ImVec2(a.x + (c.live.lfo[l] * 0.5f + 0.5f) * (b.x - a.x), b.y - (c.live.lfo[l] * 0.5f + 0.5f) * (b.y - a.y)), c.s(3),
                            c.P.hot);
    }
    if (changed) commit(c, "Edit LFO path", 1300 + l);
    if (ImGui::IsItemHovered()) c.a.setHint("Path LFO: the X output follows left / right, Y up / down \xC2\xB7 double-click: add / remove a point");
  } else if (type == LfoSH) {
    uint32_t rng = 12345u + static_cast<uint32_t>(l);
    for (int i = 0; i < gxN; ++i) {
      rng = rng * 1664525u + 1013904223u;
      const float v = (rng >> 8) / 16777216.f;
      const float x0 = gx + gw * i / gxN, x1 = gx + gw * (i + 1) / gxN;
      c.dl->AddRectFilled(c.p(x0 + 1, gy + gh - v * gh), c.p(x1 - 1, gy + gh), theme::withAlpha(c.P.fam[FamLfo], 0.25f));
      hline(c, x0, x1, gy + gh - v * gh, c.P.fam[FamLfo]);
    }
    textC(c, gx + gw * 0.5f, gy + 6, "Sample & hold: a new random value each step (grid X)", c.P.textDim, 10.0f, false);
  } else {
    // shape editor (point tool) or cell painting (step / ramp tools)
    Curve view = lfoShapeOf(c, l);
    if (tool == 0) {
      Curve edit = view;
      int loop = ld.loop;
      if (curveEditor(c, "lfoshape", edit, gx, gy, gw, gh, gxN, gyN, c.u.lfoSel, true, c.P.fam[FamLfo], &loop)) {
        Curve& cv = lfoShapeForEdit(c, l);
        cv = edit;
        ld.loop = loop;
        if (ld.name.empty() || ld.name != "Custom") ld.name = "Custom";
        commit(c, "Edit LFO shape", 1300 + l);
      }
      pui::DevPoints dp;  // dev / tests: the shape's points ("sdlpt plasma.lfo pt <i>")
      const ImVec2 a = c.p(gx, gy), b = c.p(gx + gw, gy + gh);
      dp.x0 = a.x, dp.y0 = a.y, dp.x1 = b.x, dp.y1 = b.y;
      for (const auto& q : view) dp.pts.push_back({static_cast<double>(q.x), static_cast<double>(q.y), 0.0});
      dp.hot = c.u.lfoSel;
      pui::devPointsSet("plasma.lfo", dp);
    } else {
      drawCurve(c, view, gx, gy, gw, gh, c.P.fam[FamLfo], true);
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
      vline(c, gx + gw * ph, gy, gy + gh, theme::withAlpha(c.P.hot, 0.6f));
      c.dl->AddCircleFilled(c.p(gx + gw * ph, gy + gh - static_cast<float>(curveAt(view, ph)) * gh), c.s(3.2f), c.P.hot);
    }
    if (ld.loop >= 0 && vali(c, lfoP(l, L_Mode)) == LfoEnvelope && ld.loop < static_cast<int>(view.size()))
      textR(c, gx + gw - 4, gy + 4, "sustain loop", c.P.warn, 9.5f, true);
  }
}

}  // namespace

void displayWellPublic(const Ctx& c, float x, float y, float w, float h) { display(c, x, y, w, h, 0, 0); }

// ================================================================== envelopes
void envEditor(Ctx& c, int e, float x, float y, float w, float h, bool compact) {
  // the graph, the five times as dials with their values, Time / Beats, then the curves and levels
  ImGui::PushID(e + 1200);
  const float gh = compact ? 210 : std::max(240.f, h - 192);  // the full view gives the graph all spare height
  envGraph(c, e, x, y, w, gh);
  const float vy = y + gh + 46;
  const int ks[5] = {E_Attack, E_Hold, E_Decay, E_Sustain, E_Release};
  const char* ls[5] = {"Attack", "Hold", "Decay", "Sustain", "Release"};
  const float dx = compact ? (w - 120) / 5 : 128, r = compact ? 24 : 28;
  for (int i = 0; i < 5; ++i) knob(c, envP(e, ks[i]), x + dx * (i + 0.5f), vy, r, ls[i], KnobValue);
  segments(c, envP(e, E_Bpm), x + w - (compact ? 110 : 160), vy - 13, compact ? 110 : 160, 26, {"Time", "Beats"});
  fieldGrid(c, x, vy + r + 50, w, compact ? 2 : 4, 34,
            {{envP(e, E_CurveA), "Attack curve"}, {envP(e, E_CurveD), "Decay curve"}, {envP(e, E_CurveR), "Release curve"},
             {envP(e, E_Start), "Start level"},   {envP(e, E_End), "End level"},     {envP(e, E_Steal), "When stolen"},
             {envP(e, E_LegatoInv), "Restarts in legato"}},
            26);
  ImGui::PopID();
}

// ================================================================== LFOs
// A card of the advanced panels: a title, a note at the right; returns the content top.
float advCard(Ctx& c, float x, float y, float w, float h, const char* title, const char* note) {
  c.dl->AddRectFilled(c.p(x, y), c.p(x + w, y + h), barBg(c), c.s(M(6)));
  c.dl->AddRect(c.p(x, y), c.p(x + w, y + h), theme::mix(c.P.line, c.P.lineHi, 0.35f), c.s(M(6)));
  textHeavy(c, x + M(16), y + M(21) - T(13) * 0.68f, title, c.P.text, T(13));
  if (note && *note && textW(c, note, T(11), false) < w - M(32) - textW(c, title, T(13), true) - M(10))
    textR(c, x + w - M(16), y + M(21) - T(11) * 0.68f, note, c.P.textFaint, T(11), false);
  return y + M(40);
}

// The folded Advanced row under an advanced panel's cards: a click opens or folds the rest.
void advRow(Ctx& c, const char* id, float x, float y, float w, bool& open, const char* what, const char* more) {
  const Pal& P = c.P;
  const float h = M(34);
  ImGui::SetCursorScreenPos(c.p(x, y));
  if (ImGui::InvisibleButton(id, c.F.sz(w, h))) open = !open;
  const bool hv = ImGui::IsItemHovered();
  if (hv) c.a.setHint(open ? "Fold the advanced settings" : "Show the advanced settings");
  layoutBox(c, x, y, w, h, "Advanced");
  c.dl->AddRectFilled(c.p(x, y), c.p(x + w, y + h), hv ? P.panelHi : barBg(c), c.s(M(6)));
  c.dl->AddRect(c.p(x, y), c.p(x + w, y + h), theme::mix(P.line, P.lineHi, 0.35f), c.s(M(6)));
  const ImVec2 m = c.p(x + M(16), y + h * 0.5f);
  const float d = c.s(M(2.6f));
  if (open) c.dl->AddTriangleFilled(ImVec2(m.x - d, m.y - d * 0.5f), ImVec2(m.x + d, m.y - d * 0.5f), ImVec2(m.x, m.y + d * 0.7f), P.label);
  else c.dl->AddTriangleFilled(ImVec2(m.x - d * 0.5f, m.y - d), ImVec2(m.x - d * 0.5f, m.y + d), ImVec2(m.x + d * 0.7f, m.y), P.label);
  text(c, x + M(26), y + h * 0.5f - T(12) * 0.68f, "Advanced", P.text, T(12), false);
  text(c, x + M(26) + textW(c, "Advanced", T(12), false) + M(22), y + h * 0.5f - T(11) * 0.68f, what, P.textFaint, T(11), false);
  textR(c, x + w - M(16), y + h * 0.5f - T(11) * 0.68f, more, P.textFaint, T(11), false);
}

// The links a source drives as rows: the target, its amount as a centred bar, the value. A click
// opens the link on the Links tab. Returns the count.
int movesRows(Ctx& c, int src, float x, float y, float w, float h, ImU32 col) {
  int n = 0;
  const float rh = M(32);
  for (int s = 0; s < kModSlots; ++s) {
    if (vali(c, modP(s, M_Source)) != src || vali(c, modP(s, M_Dest)) <= 0) continue;
    if (y + n * rh + rh > y + h + 1) {
      ++n;
      continue;
    }
    const float ry = y + n * rh, cy = ry + rh * 0.5f;
    ++n;
    ImGui::PushID(s + 5300);
    const int dest = vali(c, modP(s, M_Dest)) - 1;
    const std::string nm = paramLongName(dest);
    const float bw = M(110), vw = M(46), bx = x + w - vw - M(10) - bw;
    std::string shown = nm;
    while (shown.size() > 3 && textW(c, shown.c_str(), T(11.5f), false) > bx - x - M(8)) shown.pop_back();
    ImGui::SetCursorScreenPos(c.p(x, ry));
    if (ImGui::InvisibleButton("##mv", c.F.sz(bx - x - M(4), rh))) c.u.linkSel = s, c.u.view = VLinks;
    if (ImGui::IsItemHovered()) c.a.setHint(nm + ": open this link on the Links tab");
    text(c, x, cy - T(11.5f) * 0.68f, shown.c_str(), ImGui::IsItemHovered() ? c.P.text : c.P.label, T(11.5f), false);
    {  // the amount: a centred bar you can drag
      ImGui::SetCursorScreenPos(c.p(bx, cy - M(8)));
      ImGui::InvisibleButton("##amt", c.F.sz(bw, M(16)));
      if (ImGui::IsItemActive() && ImGui::GetIO().MouseDelta.x != 0)
        setv(c, modP(s, M_Amount), std::clamp(val(c, modP(s, M_Amount)) + ImGui::GetIO().MouseDelta.x / c.s(bw) * 2.0, -1.0, 1.0));
      if (ImGui::IsItemHovered()) c.a.setHint("Drag: how far it moves " + nm);
      const float a = std::clamp(valf(c, modP(s, M_Amount)), -1.f, 1.f), mid = bx + bw * 0.5f;
      c.dl->AddRectFilled(c.p(bx, cy - M(2.5f)), c.p(bx + bw, cy + M(2.5f)), c.P.field, c.s(M(2)));
      c.dl->AddRectFilled(c.p(std::min(mid, mid + a * bw * 0.5f), cy - M(2.5f)), c.p(std::max(mid, mid + a * bw * 0.5f), cy + M(2.5f)), col, c.s(M(2)));
    }
    {
      char b[16];
      std::snprintf(b, sizeof b, "%+d%%", static_cast<int>(std::lround(valf(c, modP(s, M_Amount)) * 100)));
      MonoText mono;
      textR(c, x + w, cy - T(10.5f) * 0.68f, b, c.P.text, T(10.5f), false);
    }
    ImGui::PopID();
  }
  if (n == 0) {
    const char* m = "Drag the dot by the name onto any control to link it";
    std::string shown = m;
    while (shown.size() > 3 && textW(c, shown.c_str(), T(11.5f), false) > w) shown.pop_back();
    text(c, x, y + M(4), shown.c_str(), c.P.textFaint, T(11.5f), false);
  }
  return n;
}

void lfoView(Ctx& c) {
  Ui& u = c.u;
  const Pal& P = c.P;
  const int l = std::clamp(u.lfoTab, 0, kLfos - 1);
  const ImU32 fam = P.fam[FamLfo];
  KnobColour kc(fam);
  ImGui::PushID(l + 1300);
  LfoData& ld = c.m.lfo[static_cast<size_t>(l)];
  const bool synced = val(c, lfoP(l, L_Bpm)) >= 0.5;
  const int mode = vali(c, lfoP(l, L_Mode));
  char title[16], sub[96];
  std::snprintf(title, sizeof title, "LFO %d", l + 1);
  std::snprintf(sub, sizeof sub, "%s %s, %s", synced ? "Tempo" : "Free", ptext(c, lfoP(l, L_Rate)).c_str(),
                mode == 0 ? "keeps going" : mode == 1 ? "restarts each note" : "plays once");
  advHead(c, title, sub, fam, SrcLfo1 + l);
  {  // the ten LFOs, each its own shape in small
    const float cw = M(44), ch = M(34), gap = M(4);
    float x = kAdvIX1 - kLfos * cw - (kLfos - 1) * gap;
    for (int i = 0; i < kLfos; ++i, x += cw + gap) {
      ImGui::PushID(i + 1400);
      const float y = kAdvHeadY - ch * 0.5f;
      ImGui::SetCursorScreenPos(c.p(x, y));
      if (ImGui::InvisibleButton("##pick", c.F.sz(cw, ch))) u.lfoTab = i, u.lfoSel = -1;
      const bool hv = ImGui::IsItemHovered(), on = i == l;
      if (hv) c.a.setHint("LFO " + std::to_string(i + 1));
      c.dl->AddRectFilled(c.p(x, y), c.p(x + cw, y + ch), on ? theme::withAlpha(fam, 0.14f) : (hv ? P.panelHi : P.field), c.s(M(4)));
      c.dl->AddRect(c.p(x, y), c.p(x + cw, y + ch), on ? fam : P.line, c.s(M(4)));
      drawCurve(c, lfoShapePublic(c, i), x + M(8), y + M(4), cw - M(16), M(14), on ? fam : P.textFaint, false);
      char b[16];
      std::snprintf(b, sizeof b, "%d", i + 1);
      MonoText mono;
      textMid(c, x, x + cw, y + ch - M(8), b, on ? P.text : P.textFaint, T(9.5f), false);
      ImGui::PopID();
    }
  }
  // the shapes, the grid, points or steps
  const float tx0 = kAdvIX0, ty = kAdvBodyY, th = M(28);
  {
    struct Sh {
      const char* name;
      int shape;
    };
    static const Sh shapes[] = {{"Smooth", 2}, {"Triangle", 1}, {"Ramp up", 3}, {"Ramp down", 4}, {"Square", 5}, {"Steps", 11}, {"Random", 22}};
    const int sh = vali(c, lfoP(l, L_Shape));
    reach(lfoP(l, L_Shape));
    float x = tx0;
    for (const auto& s : shapes) {
      ImGui::PushID(s.shape + 1500);
      const float w = textW(c, s.name, T(11.5f), false) + M(10 + 22 + 8 + 12);
      const bool on = ld.pts.empty() && sh == s.shape;
      if (btnLook(c, "shape", x, ty, w, th, "", false, on, T(11.5f))) {
        ld.pts.clear(), ld.loop = -1, ld.name.clear();
        commitParams(c, "LFO shape", {{lfoP(l, L_Shape), s.shape}});
      }
      if (ImGui::IsItemHovered()) c.a.setHint(std::string(s.name) + ": start from this shape");
      drawCurve(c, lfoFactoryShape(s.shape), x + M(10), ty + M(8), M(22), th - M(16), on ? fam : P.textFaint, false);
      text(c, x + M(10 + 22 + 8), ty + th * 0.5f - T(11.5f) * 0.68f, s.name, on ? P.accentText : P.label, T(11.5f), false);
      x += w + M(6);
      ImGui::PopID();
    }
    {  // every other shape, with pictures
      const auto& sn = lfoShapeNames();
      const float w = textW(c, "More shapes", T(11.5f), false) + M(24);
      if (btnLook(c, "moreshapes", x, ty, w, th, "More shapes", true, false, T(11.5f))) ImGui::OpenPopup("##lfoshapes");
      if (ImGui::IsItemHovered()) c.a.setHint("Every LFO shape, as pictures");
      if (ImGui::BeginPopup("##lfoshapes")) {
        const float tw = c.s(118), tth = c.s(58);
        ImDrawList* pd = ImGui::GetWindowDrawList();
        for (int i = 1; i < static_cast<int>(sn.size()); ++i) {
          ImGui::PushID(i);
          if ((i - 1) % 4) ImGui::SameLine();
          const ImVec2 a = ImGui::GetCursorScreenPos();
          const bool cur = ld.pts.empty() && sh == i;
          if (ImGui::Selectable("##sh", cur, 0, ImVec2(tw, tth))) {
            ld.pts.clear(), ld.loop = -1, ld.name.clear();
            commitParams(c, "LFO shape", {{lfoP(l, L_Shape), i}});
          }
          const Curve cv = lfoFactoryShape(i);
          const float px = a.x + c.s(6), pw = tw - c.s(12), py = a.y + c.s(5), phh = tth - c.s(24);
          pd->AddRectFilled(ImVec2(px, py), ImVec2(px + pw, py + phh), P.well, c.s(3));
          ImVec2 pts[49];
          for (int q = 0; q <= 48; ++q) {
            const double t = q / 48.0;
            pts[q] = ImVec2(px + pw * static_cast<float>(t), py + phh - 2 - static_cast<float>(curveAt(cv, t)) * (phh - 4));
          }
          pd->AddPolyline(pts, 49, fam, 0, std::max(1.2f, c.s(1.4f)));
          const char* nmi = sn[static_cast<size_t>(i)].c_str();
          const ImVec2 ts = ImGui::CalcTextSize(nmi);
          pd->AddText(ImVec2(a.x + (tw - ts.x) * 0.5f, a.y + tth - c.s(17)), cur ? P.accentText : P.text, nmi);
          ImGui::PopID();
        }
        ImGui::EndPopup();
      }
    }
    // right: Edit, Grid, Points / Steps
    float rx = kAdvIX1;
    {
      const int tool = u.lfoTool == 1 ? 1 : 0;
      const float w = textW(c, "Points", T(11.5f), false) + textW(c, "Steps", T(11.5f), false) + M(48);
      rx -= w;
      const int hit = segPick(c, "tool", rx, ty, w, th, {"Points", "Steps"}, tool, false, T(11.5f));
      if (hit >= 0) u.lfoTool = hit;
      if (ImGui::IsMouseHoveringRect(c.p(rx, ty), c.p(rx + w, ty + th))) c.a.setHint("Points: drag, add and bend points. Steps: paint flat steps");
      rx -= M(10);
    }
    {
      const int gx = vali(c, lfoP(l, L_GridX));
      const int cur = gx == 4 ? 0 : gx == 8 ? 1 : gx == 16 ? 2 : -1;
      const float w = M(120);
      rx -= w;
      MonoText mono;
      const int hit = segPick(c, "grid", rx, ty + M(3), w, th - M(6), {"1/4", "1/8", "1/16"}, cur, true, T(10.5f));
      if (hit >= 0) setv(c, lfoP(l, L_GridX), hit == 0 ? 4 : hit == 1 ? 8 : 16);
      if (ImGui::IsMouseHoveringRect(c.p(rx, ty), c.p(rx + w, ty + th))) c.a.setHint("Grid: cells across (More: cells down)");
      rx -= M(8);
      reach(lfoP(l, L_GridX));
    }
    {
      const float w = textW(c, "Grid", T(11.5f), false);
      rx -= w;
      text(c, rx, ty + th * 0.5f - T(11.5f) * 0.68f, "Grid", P.textFaint, T(11.5f), false);
      rx -= M(14);
    }
    {
      const float w = textW(c, "Edit", T(11.5f), false) + M(24);
      rx -= w;
      if (btnLook(c, "edit", rx, ty, w, th, "Edit", true, false, T(11.5f))) ImGui::OpenPopup("##lfoedit");
      if (ImGui::IsItemHovered()) c.a.setHint("Copy, paste, flip, reverse or clear the shape; paint ramps");
      if (ImGui::BeginPopup("##lfoedit")) {
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
        ImGui::Separator();
        if (ImGui::MenuItem(i18n::tr("Paint ramps up"), nullptr, u.lfoTool == 2)) u.lfoTool = 2;
        if (ImGui::MenuItem(i18n::tr("Paint ramps down"), nullptr, u.lfoTool == 3)) u.lfoTool = 3;
        ImGui::EndPopup();
      }
    }
  }
  // the cards at the bottom, the Advanced row under them; the graph takes the rest
  const float advY = kPlayY - M(16) - M(18) - M(34), cardsH = M(152), cardsY = advY - M(12) - cardsH;
  const float advOpenH = u.lfoAdv ? M(150) : 0.f;
  const float gy = ty + th + M(14), gh = cardsY - M(14) - gy - advOpenH, gx = tx0, gw = kAdvIX1 - tx0;
  {
    wellLook(c, gx, gy, gw, gh);
    lfoGraph(c, l, gx + M(10), gy + M(30), gw - M(64), gh - M(52));
    MonoText mono;
    capsText(c, gx + M(12), gy + M(10), "Double-click to add a point, drag a line to bend it", P.textFaint, T(10), M(0.7f));
    // the LFO's level now, at the right
    const float mx = gx + gw - M(36), my = gy + M(44), mh = gh - M(66);
    c.dl->AddRectFilled(c.p(mx, my), c.p(mx + M(8), my + mh), theme::withAlpha(fam, 0.12f), c.s(M(1)));
    const float lv = c.hasLive && c.live.voices > 0 ? std::clamp(c.live.lfo[l], 0.f, 1.f) : 0.f;
    if (lv > 0) c.dl->AddRectFilledMultiColor(c.p(mx, my + mh * (1 - lv)), c.p(mx + M(8), my + mh), fam, fam, theme::withAlpha(fam, 0.35f), theme::withAlpha(fam, 0.35f));
    char b[16];
    std::snprintf(b, sizeof b, "%d%%", static_cast<int>(std::lround(lv * 100)));
    textMid(c, mx - M(12), mx + M(20), gy + M(26), b, P.label, T(10), false);
  }
  if (u.lfoAdv) {  // every other setting
    const float y = cardsY - M(14) - advOpenH + M(6);
    fieldGrid(c, tx0, y, gw, 4, M(34),
              {{lfoP(l, L_Type), "Kind"}, {lfoP(l, L_Anchor), "Song lock"}, {lfoP(l, L_Swing), "Swing"}, {lfoP(l, L_PhaseSnap), "Phase snap"},
               {lfoP(l, L_Dir), "Direction"}, {lfoP(l, L_Phase), "Start phase"}, {lfoP(l, L_Mono), "One for all notes"}, {lfoP(l, L_Dot), "Dotted"},
               {lfoP(l, L_Trip), "Triplet"}, {lfoP(l, L_Rate10x), "Rate x 10"}, {G_LfoScale, "All LFO rates"}, {G_LfoScaleRate, "Scale rates, not times"},
               {lfoP(l, L_GridY), "Grid down"}},
              M(28));
    {
      std::vector<FieldItem> items;
      static const char* const bn[16] = {"Bus 1", "Bus 2", "Bus 3", "Bus 4", "Bus 5", "Bus 6", "Bus 7", "Bus 8",
                                         "Bus 9", "Bus 10", "Bus 11", "Bus 12", "Bus 13", "Bus 14", "Bus 15", "Bus 16"};
      for (int b = 0; b < kLfoBusses; ++b) items.push_back({busP(b), bn[b]});
      fieldGrid(c, tx0, y + M(34) * 4, gw, 8, M(34), items, M(28));
    }
  }
  const float cw = (gw - M(12) * 3) / 4;
  {  // Speed
    const float x = tx0, y = advCard(c, x, cardsY, cw, cardsH, "Speed", "how fast it moves");
    knob(c, lfoP(l, L_Rate), x + M(16 + 25), y + M(25), M(25), "Rate", KnobValue);
    const float sx = x + M(100);
    const int hit = segPick(c, "bpm", sx, y, M(114), M(28), {"Tempo", "Free"}, synced ? 0 : 1, false, T(11.5f));
    if (hit >= 0) setv(c, lfoP(l, L_Bpm), hit == 0 ? 1 : 0);
    reach(lfoP(l, L_Bpm));
    if (synced) {
      static const char* const divs[5] = {"1/1", "1/2", "1/4", "1/8", "1/16"};
      const int n = lfoRateDivisionCount();
      const int curD = static_cast<int>(std::lround(val(c, lfoP(l, L_Rate)) * (n - 1)));
      const float bw = std::min(M(36), (x + cw - M(16) - sx - M(4) * 4) / 5);
      for (int d = 0; d < 5; ++d) {
        int idx = -1;
        for (int i = 0; i < n; ++i)
          if (std::string(lfoRateDivisionName(i)) == divs[d]) idx = i;
        if (idx < 0) continue;
        ImGui::PushID(d + 1600);
        MonoText mono;
        if (btnLook(c, "div", sx + d * (bw + M(4)), y + M(38), bw, M(26), divs[d], false, curD == idx, T(10.5f)))
          setv(c, lfoP(l, L_Rate), static_cast<double>(idx) / (n - 1));
        ImGui::PopID();
      }
    }
  }
  {  // Each note
    const float x = tx0 + (cw + M(12)), y = advCard(c, x, cardsY, cw, cardsH, "Each note", "where it starts");
    const int hit = segPick(c, "mode", x + M(16), y, cw - M(32), M(28), {"Keeps going", "Restarts", "Plays once"}, mode, false, T(11.5f));
    if (hit >= 0) setv(c, lfoP(l, L_Mode), hit);
    reach(lfoP(l, L_Mode));
    const char* d = mode == 0 ? "One shape runs on through every note, so notes catch it wherever it is."
                    : mode == 1 ? "Every new note starts the shape from the beginning, so each note moves the same way."
                                : "Every new note plays the shape once and holds its end.";
    ImGui::PushClipRect(c.p(x, y), c.p(x + cw - M(16), cardsY + cardsH), true);
    const ImVec2 at = c.p(x + M(16), y + M(40));
    ImFont* f = ImGui::GetFont();
    c.dl->AddText(f, c.s(T(11.5f)) * kTypeScale, at, P.textFaint, i18n::tr(d), nullptr, c.s(cw - M(32)));
    layoutWindowText(at, "", 0);
    ImGui::PopClipRect();
  }
  {  // Feel
    const float x = tx0 + (cw + M(12)) * 2, y = advCard(c, x, cardsY, cw, cardsH, "Feel", "soften the motion");
    const float dx = (cw - M(32)) / 3;
    knob(c, lfoP(l, L_Rise), x + M(16) + dx * 0.5f, y + M(20), M(18), "Rise", KnobValue);
    knob(c, lfoP(l, L_Delay), x + M(16) + dx * 1.5f, y + M(20), M(18), "Delay", KnobValue);
    knob(c, lfoP(l, L_Smooth), x + M(16) + dx * 2.5f, y + M(20), M(18), "Smooth", KnobValue);
  }
  {  // Moves
    const float x = tx0 + (cw + M(12)) * 3;
    int links = 0;
    for (int s = 0; s < kModSlots; ++s) links += vali(c, modP(s, M_Source)) == SrcLfo1 + l && vali(c, modP(s, M_Dest)) > 0;
    char note[24];
    std::snprintf(note, sizeof note, links == 1 ? "%d link" : "%d links", links);
    const float y = advCard(c, x, cardsY, cw, cardsH, "Moves", note);
    movesRows(c, SrcLfo1 + l, x + M(16), y - M(8), cw - M(32), cardsY + cardsH - M(10) - (y - M(8)), fam);
  }
  advRow(c, "lfoadv", tx0, advY, gw, u.lfoAdv, "kind and chaos modes, point motion, phase per voice, rate scaling", "29 more");
  ImGui::PopID();
}

// ================================================================== envelopes (the advanced panel)
namespace {
// A quick shape: the four times (seconds) and the sustain gain, set as one undo step.
struct EnvShape {
  const char* name;
  double a, h, d, s, r;
};
constexpr EnvShape kEnvShapes[] = {{"Pluck", 0.002, 0, 0.35, 0, 0.18}, {"Keys", 0.004, 0, 1.2, 0.35, 0.45},
                                   {"Pad", 0.8, 0, 1.0, 0.7, 1.6},     {"Gate", 0.002, 0, 0.05, 1, 0.012},
                                   {"Swell", 2.2, 0, 0.5, 1, 2.0}};
Curve envShapeCurve(const EnvShape& e) {
  const double t = e.a + e.h + e.d + 0.4 + e.r, sus = std::sqrt(e.s);
  auto X = [t](double v) { return static_cast<float>(v / t); };
  return {{0, 0, 0}, {X(e.a), 1, 0}, {X(e.a + e.h + e.d), static_cast<float>(sus), 0}, {X(e.a + e.h + e.d + 0.4), static_cast<float>(sus), 0}, {1, 0, 0}};
}
bool envIs(const Ctx& c, int e, const EnvShape& s) {
  auto near = [&](int p, double v) { return std::fabs(val(c, envP(e, p)) - v) < 0.004; };
  return near(E_Attack, envTimeInv(s.a)) && near(E_Hold, envTimeInv(s.h)) && near(E_Decay, envTimeInv(s.d)) && near(E_Sustain, std::sqrt(s.s)) &&
         near(E_Release, envTimeInv(s.r));
}
}  // namespace

void envView(Ctx& c) {
  Ui& u = c.u;
  const Pal& P = c.P;
  const int e = std::clamp(u.envTab, 0, kEnvs - 1);
  const ImU32 fam = P.fam[FamEnv];
  KnobColour kc(fam);
  ImGui::PushID(e + 1250);
  int links = 0;
  for (int s = 0; s < kModSlots; ++s) links += vali(c, modP(s, M_Source)) == SrcEnv1 + e && vali(c, modP(s, M_Dest)) > 0;
  const bool beats = val(c, envP(e, E_Bpm)) >= 0.5;
  char title[24], sub[96];
  std::snprintf(title, sizeof title, "Envelope %d", e + 1);
  if (e == 0) std::snprintf(sub, sizeof sub, "%s", beats ? "the amp, every note's level, in beats" : "the amp, every note's level");
  else if (links) std::snprintf(sub, sizeof sub, links == 1 ? "%d link%s" : "%d links%s", links, beats ? ", in beats" : "");
  else std::snprintf(sub, sizeof sub, "%s", beats ? "no links yet, in beats" : "no links yet");
  advHead(c, title, sub, fam, SrcEnv1 + e);
  {  // the four envelopes, each its own shape in small
    const float cw = M(56), ch = M(34), gap = M(4);
    float x = kAdvIX1 - kEnvs * cw - (kEnvs - 1) * gap;
    for (int i = 0; i < kEnvs; ++i, x += cw + gap) {
      ImGui::PushID(i + 1450);
      const float y = kAdvHeadY - ch * 0.5f;
      ImGui::SetCursorScreenPos(c.p(x, y));
      if (ImGui::InvisibleButton("##pick", c.F.sz(cw, ch))) u.envTab = i;
      const bool hv = ImGui::IsItemHovered(), on = i == e;
      if (hv) c.a.setHint(i == 0 ? "Envelope 1: the amp" : "Envelope " + std::to_string(i + 1));
      c.dl->AddRectFilled(c.p(x, y), c.p(x + cw, y + ch), on ? theme::withAlpha(fam, 0.14f) : (hv ? P.panelHi : P.field), c.s(M(4)));
      c.dl->AddRect(c.p(x, y), c.p(x + cw, y + ch), on ? fam : P.line, c.s(M(4)));
      envMini(c, i, x + M(8), y + M(4), cw - M(16), M(14));
      char b[16];
      std::snprintf(b, sizeof b, "%d", i + 1);
      MonoText mono;
      textMid(c, x, x + cw, y + ch - M(8), b, on ? P.text : P.textFaint, T(9.5f), false);
      ImGui::PopID();
    }
  }
  // quick shapes at the left; Time / Beats at the right
  const float tx0 = kAdvIX0, ty = kAdvBodyY, th = M(28), gw = kAdvIX1 - tx0;
  {
    float x = tx0;
    for (const auto& s : kEnvShapes) {
      ImGui::PushID(s.name);
      const float w = textW(c, s.name, T(11.5f), false) + M(10 + 22 + 8 + 12);
      const bool on = envIs(c, e, s);
      if (btnLook(c, "shape", x, ty, w, th, "", false, on, T(11.5f))) {
        const EnvShape q = s;
        editState(c.a, c.t, "Envelope shape", [&](PluginState& st, Model&) {
          auto put = [&](int p, double v) {
            if (envP(e, p) < static_cast<int>(st.params.size())) st.params[static_cast<size_t>(envP(e, p))] = v;
          };
          put(E_Attack, envTimeInv(q.a)), put(E_Hold, envTimeInv(q.h)), put(E_Decay, envTimeInv(q.d));
          put(E_Sustain, std::sqrt(q.s)), put(E_Release, envTimeInv(q.r)), put(E_Bpm, 0);
        });
      }
      if (ImGui::IsItemHovered()) c.a.setHint(std::string(s.name) + ": start from this shape");
      drawCurve(c, envShapeCurve(s), x + M(10), ty + M(8), M(22), th - M(16), on ? fam : P.textFaint, false);
      text(c, x + M(10 + 22 + 8), ty + th * 0.5f - T(11.5f) * 0.68f, s.name, on ? P.accentText : P.label, T(11.5f), false);
      x += w + M(6);
      ImGui::PopID();
    }
    const float w = textW(c, "Time", T(11.5f), false) + textW(c, "Beats", T(11.5f), false) + M(48);
    const int hit = segPick(c, "bpm", kAdvIX1 - w, ty, w, th, {"Time", "Beats"}, beats ? 1 : 0, false, T(11.5f));
    if (hit >= 0) setv(c, envP(e, E_Bpm), hit);
    if (ImGui::IsMouseHoveringRect(c.p(kAdvIX1 - w, ty), c.p(kAdvIX1, ty + th))) c.a.setHint("Time: the stages in seconds. Beats: in beats of the song tempo");
    reach(envP(e, E_Bpm));
  }
  // the cards at the bottom, the Advanced row under them; the graph takes the rest
  const float advY = kPlayY - M(16) - M(18) - M(34), cardsH = M(140), cardsY = advY - M(12) - cardsH;
  const float advOpenH = u.envAdv ? M(48) : 0.f;
  const float gy = ty + th + M(14), gh = cardsY - M(14) - gy - advOpenH, gx = tx0;
  {
    wellLook(c, gx, gy, gw, gh);
    envGraph(c, e, gx + M(10), gy + M(30), gw - M(20), gh - M(40));
    MonoText mono;
    capsText(c, gx + M(12), gy + M(10), "Drag a point to change its time and level, a line to bend it", P.textFaint, T(10), M(0.7f));
  }
  if (u.envAdv)
    fieldGrid(c, tx0, cardsY - M(14) - advOpenH + M(8), gw, 4, M(34),
              {{envP(e, E_Steal), "When a note is stolen"}, {envP(e, E_LegatoInv), "Restarts in legato"}, {G_EnvScale, "All envelope times"}}, M(28));
  const float cw = (gw - M(12) * 3) / 4;
  auto dials = [&](float x, float y, float w, std::initializer_list<std::pair<int, const char*>> ks) {
    const float dx = (w - M(24)) / static_cast<float>(ks.size());
    int i = 0;
    for (const auto& k : ks) knob(c, envP(e, k.first), x + M(12) + dx * (i++ + 0.5f), y + M(22), M(20), k.second, KnobValue);
  };
  {
    const float x = tx0, y = advCard(c, x, cardsY, cw, cardsH, "Times", beats ? "in beats" : "how long each stage");
    dials(x, y, cw, {{E_Attack, "Attack"}, {E_Hold, "Hold"}, {E_Decay, "Decay"}, {E_Release, "Release"}});
  }
  {
    const float x = tx0 + (cw + M(12)), y = advCard(c, x, cardsY, cw, cardsH, "Levels", "where it starts, holds, ends");
    dials(x, y, cw, {{E_Start, "Start"}, {E_Sustain, "Sustain"}, {E_End, "End"}});
  }
  {
    const float x = tx0 + (cw + M(12)) * 2, y = advCard(c, x, cardsY, cw, cardsH, "Curves", "bend each stage");
    dials(x, y, cw, {{E_CurveA, "Attack"}, {E_CurveD, "Decay"}, {E_CurveR, "Release"}});
  }
  {
    const float x = tx0 + (cw + M(12)) * 3;
    char note[24];
    std::snprintf(note, sizeof note, links == 1 ? "%d link" : "%d links", links);
    const float y = advCard(c, x, cardsY, cw, cardsH, "Moves", note);
    if (e == 0 && links == 0) {
      const char* m = "Every note's level; drag its dot onto a control to move more";
      std::string shown = m;
      while (shown.size() > 3 && textW(c, shown.c_str(), T(11.5f), false) > cw - M(32)) shown.pop_back();
      text(c, x + M(16), y - M(4), shown.c_str(), P.textFaint, T(11.5f), false);
    } else {
      movesRows(c, SrcEnv1 + e, x + M(16), y - M(8), cw - M(32), cardsY + cardsH - M(10) - (y - M(8)), fam);
    }
  }
  advRow(c, "envadv", tx0, advY, gw, u.envAdv, "when a note is stolen, legato restarts, every envelope's time", "3 more");
  ImGui::PopID();
}

// ================================================================== key and touch
void keysView(Ctx& c) {
  stageHead(c, "Key and touch");
  ImGui::PushID("keys");
  {  // response curves
    const float x = 9, w = 396;
    const float y = card(c, x, kStageY + 10, w, 384, "Response");
    for (int st = 0; st < 2; ++st) {
      const float gx = x + 14 + st * 192, gw = 180, gy = y + 22, gh = 180;
      sourceGrip(c, st == 0 ? SrcVelo : SrcNote, gx + 6, y + 6, 5.0f);
      text(c, gx + 16, y, st == 0 ? "Velocity" : "Key", c.P.text, 12.0f, true);
      display(c, gx, gy, gw, gh, 4, 4);
      Curve& cv = st == 0 ? c.m.velo : c.m.note;
      Curve edit = cv.empty() ? linearCurve() : cv;
      static int sel[2] = {-1, -1};
      if (curveEditor(c, st == 0 ? "velo" : "note", edit, gx, gy, gw, gh, 4, 4, sel[st], true, c.P.fam[FamKey])) {
        cv = edit;
        commit(c, st == 0 ? "Edit velocity curve" : "Edit key curve", 1400 + st);
      }
    }
    text(c, x + 14, y + 210, "Drag points, double-click to add or remove.", c.P.textDim, 11.0f, false);
    fieldGrid(c, x + 14, y + 232, w - 28, 2, 34, {{G_MpeOn, "Per-note expression"}, {G_MpeYBipolar, "Slide Y centred"}, {G_MpeBend, "Per-note bend"}}, 26);
    // the four expression routings (sound data)
    float ry = y + 302;
    auto flag = [&](const char* id, bool& f, const char* label) {
      ImGui::PushID(id);
      ImGui::SetCursorScreenPos(c.p(x + 14, ry));
      if (ImGui::InvisibleButton("##f", c.F.sz(w - 28, 16))) {
        f = !f;
        commit(c, "Expression routing");
      }
      const ImVec2 dc = c.p(x + 20, ry + 8);
      if (f) c.dl->AddCircleFilled(dc, c.s(3.5f), c.P.good, 12);
      else c.dl->AddCircle(dc, c.s(3.5f), c.P.boxEdge, 12, std::max(1.0f, c.s(1.0f)));
      labelBeside(c, x + 30, ry, 16, label, f ? c.P.text : c.P.label, 11.0f, false);
      ImGui::PopID();
      ry += 20;
    };
    flag("m1", c.m.mpeBendToX, "Per-note bend also moves slide X");
    flag("m2", c.m.mpeXYZtoMacros, "Slide X, Y and press Z move macros 1 to 3");
  }
  {  // wheels
    const float x = 409, w = 192;
    const float y = card(c, x, kStageY + 10, w, 384, "Wheels");
    wheel(c, G_PitchWheel, x + 40, y + 10, 26, 200, true);
    wheel(c, G_ModWheel, x + 128, y + 10, 26, 200, false);
    // (each wheel names itself under its slot, and shows its value while it moves)
    fieldGrid(c, x + 12, y + 238, w - 24, 1, 32, {{G_BendUp, "Bend up"}, {G_BendDown, "Bend down"}}, 26);
    float ry = y + 304;
    auto flag = [&](const char* id, bool& f, const char* label) {
      ImGui::PushID(id);
      ImGui::SetCursorScreenPos(c.p(x + 12, ry));
      if (ImGui::InvisibleButton("##f", c.F.sz(w - 24, 16))) {
        f = !f;
        commit(c, "Expression routing");
      }
      const ImVec2 dc = c.p(x + 18, ry + 8);
      if (f) c.dl->AddCircleFilled(dc, c.s(3.5f), c.P.good, 12);
      else c.dl->AddCircle(dc, c.s(3.5f), c.P.boxEdge, 12, std::max(1.0f, c.s(1.0f)));
      float lpx = 10.5f;  // a step smaller before it would run past the card
      while (lpx > 8.0f && textW(c, label, lpx, false) > w - 28 - 6) lpx -= 0.25f;
      labelBeside(c, x + 28, ry, 16, label, f ? c.P.text : c.P.label, lpx, false);
      ImGui::PopID();
      ry += 20;
    };
    flag("m3", c.m.mpeYZtoMacros, "Slide Y, press Z: macros 1, 2");
    flag("m4", c.m.mpeYtoModWheel, "Slide Y moves the mod wheel");
  }
  {  // voice variation: random amounts and the per-voice sequence
    const float x = 605, w = 386;
    const float y = card(c, x, kStageY + 10, w, 384, "Voices and variation");
    fieldGrid(c, x + 12, y, w - 24, 2, 32,
              {{G_Priority, "Steals"}, {G_SameNote, "One per key"}, {G_RndPan, "Random pan"}, {G_RndDetune, "Random detune"}, {G_RndCutoff, "Random cutoff"},
               {G_RndEnv, "Random times"}},
              26);
    float sy = y + 102;
    text(c, x + 12, sy + 5, "Sequence", c.P.text, 12.0f, true);
    text(c, x + 92, sy + 6, "Steps", c.P.label, 11.0f, false);
    stepper(c, G_VcSteps, x + 130, sy, 46, 26);
    const char* on[5] = {"Root", "1", "2", "3", "Tex"};
    const int ok[5] = {G_VcS, G_VcA, G_VcB, G_VcC, G_VcN};
    text(c, x + 182, sy + 6, "On", c.P.label, 11.0f, false);
    for (int i = 0; i < 5; ++i) {
      ImGui::PushID(i + 5000);
      toggleBtn(c, ok[i], x + 204 + i * 34.f, sy, 31, 26, on[i], 10.5f);
      if (ImGui::IsItemHovered()) c.a.setHint(i == 0 ? "The sequence varies the root" : i == 4 ? "The sequence varies the texture" : "The sequence varies this layer");
      ImGui::PopID();
    }
    sy += 36;
    static int seqRow = 0;
    const char* rows[6] = {"Pan", "Detune", "Cutoff", "Times", "Voice mod 1", "Voice mod 2"};
    const float gx = x + 112, gw = w - 124, rh = 32, gh = rh * 6 - 4;
    for (int r = 0; r < 6; ++r) {
      ImGui::PushID(r + 5100);
      if (button(c, "row", x + 12, sy + r * rh, 94, rh - 6, rows[r], seqRow == r, 10.5f)) seqRow = r;
      ImGui::PopID();
    }
    display(c, gx, sy, gw, gh, 8, 2);
    auto fieldOf = [](VoiceStep& v, int r) -> float& {
      switch (r) {
        case 0: return v.pan;
        case 1: return v.detune;
        case 2: return v.cutoff;
        case 3: return v.env;
        case 4: return v.mod1;
        default: return v.mod2;
      }
    };
    for (int i = 0; i < 8; ++i) {
      const float v = fieldOf(c.m.voiceSeq[static_cast<size_t>(i)], seqRow);
      const float cx0 = gx + gw * i / 8 + 4, cx1 = gx + gw * (i + 1) / 8 - 4, mid = sy + gh * 0.5f;
      const bool live = i < vali(c, G_VcSteps);
      c.dl->AddRectFilled(c.p(cx0, std::min(mid, mid - v * gh * 0.5f)), c.p(cx1, std::max(mid, mid - v * gh * 0.5f)),
                          theme::withAlpha(c.P.fam[FamChance], live ? 0.4f : 0.12f), c.s(1));
      hline(c, cx0, cx1, mid - v * gh * 0.5f, live ? c.P.fam[FamChance] : c.P.textFaint);
    }
    ImGui::SetCursorScreenPos(c.p(gx, sy));
    ImGui::InvisibleButton("##seq", c.F.sz(gw, gh), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    if (ImGui::IsItemActive() || (ImGui::IsItemHovered() && ImGui::IsMouseDown(1))) {
      const ImVec2 m = ImGui::GetIO().MousePos, a = c.p(gx, sy), b = c.p(gx + gw, sy + gh);
      const int i = std::clamp(static_cast<int>((m.x - a.x) / (b.x - a.x) * 8), 0, 7);
      const float v = ImGui::IsMouseDown(1) ? 0.f : std::clamp(1.f - 2.f * (m.y - a.y) / (b.y - a.y), -1.f, 1.f);
      float& f = fieldOf(c.m.voiceSeq[static_cast<size_t>(i)], seqRow);
      if (f != v) {
        f = v;
        commit(c, "Edit voice sequence", 5200 + seqRow);
      }
    }
    if (ImGui::IsItemHovered()) c.a.setHint("Each new note takes the next step: drag to draw, right-drag to clear");
  }
  {  // restarts
    const float y0 = kStageY + 404;
    const float y = card(c, 9, y0, 982, kStageB - 12 - y0, "Restarts (link a source here to restart a part when it rises)");
    static const char* const rn[kRetrigs] = {"Note",  "Layer 1", "Layer 2", "Layer 3", "Root",  "Texture", "Envelope 2", "Envelope 3", "Envelope 4", "LFO 1",
                                             "LFO 2", "LFO 3",   "LFO 4",   "LFO 5",   "LFO 6", "LFO 7",   "LFO 8",      "LFO 9",      "LFO 10"};
    std::vector<FieldItem> items;
    for (int r = 0; r < kRetrigs; ++r) items.push_back({retrigP(r), rn[r]});
    fieldGrid(c, 20, y, 960, 5, 32, items, 26);
  }
  ImGui::PopID();
}

// ================================================================== amp and voices
void ampView(Ctx& c) {
  const float hx = stageHead(c, "Amp and voices");
  sourceGrip(c, SrcEnv1, hx + 4, kStageHeadY + 15, 7.0f);
  text(c, hx + 18, kStageHeadY + 8, "Envelope 1: the level of every note", c.P.textDim, 11.0f, false);
  const float T = kStageY + 10;
  {
    const float x = 12, w = 588;
    const float y = card(c, x - 3, T, w + 6, kStageB - 12 - T, "Envelope 1");
    envEditor(c, 0, x + 10, y + 4, w - 20, 0, true);
  }
  const float R = 612, RW = 376;
  {
    const float y = card(c, R - 3, T, RW + 6, 262, "Voices");
    knob(c, G_Amp, R + 56, y + 36, 26, "Amp depth");
    knob(c, G_MasterTune, R + 156, y + 36, 26, "Fine tune", KnobBipolar);
    fieldGrid(c, R + 10, y + 96, RW - 20, 2, 34,
              {{G_Poly, "Notes"}, {G_Priority, "Steal"}, {G_SameNote, "One per key"}, {G_Mono, "One voice"}, {G_Legato, "Glide notes"}}, 26);
    char b[48];
    std::snprintf(b, sizeof b, "Playing now: %d of %d", c.hasLive ? c.live.voices : 0, vali(c, G_Poly));
    text(c, R + 12, y + 206, b, c.P.textDim, 11.0f, false);
  }
  {
    const float y0 = T + 274;
    const float y = card(c, R - 3, y0, RW + 6, kStageB - 12 - y0, "Glide and bend");
    knob(c, G_PortaTime, R + 56, y + 36, 26, "Glide time", KnobValue);
    knob(c, G_Transpose, R + 156, y + 36, 26, "Transpose", KnobValue);
    fieldGrid(c, R + 10, y + 104, RW - 20, 2, 34,
              {{G_PortaCurve, "Curve"}, {G_PortaAlways, "Every note"}, {G_PortaScaled, "Constant rate"}, {G_BendUp, "Bend up"},
               {G_BendDown, "Bend down"}, {G_EnvScale, "All times"}},
              26);
  }
}


// ================================================================== Panels: 3 Volume
float volumeColumn(Ctx& c, float x, float y, float w) {
  ImGui::PushID("volcol");
  {
    const bool det = section(c, "amp", x, y, w, "Envelope 1", "every note's level", true);
    envGraph(c, 0, x, y, w, 132);
    y += 140;
    y = fieldGrid(c, x, y, w, 1, 32,
                  {{envP(0, E_Attack), "Attack"}, {envP(0, E_Decay), "Decay"}, {envP(0, E_Sustain), "Sustain"}, {envP(0, E_Release), "Release"}}, 26);
    segments(c, envP(0, E_Bpm), x, y, w, 24, {"Time", "Beats"});
    y += 32;
    if (det)
      y = fieldGrid(c, x, y, w, 1, 32,
                    {{envP(0, E_Hold), "Hold"}, {envP(0, E_CurveA), "Attack curve"}, {envP(0, E_CurveD), "Decay curve"},
                     {envP(0, E_CurveR), "Release curve"}, {envP(0, E_Start), "Start level"}, {envP(0, E_End), "End level"},
                     {envP(0, E_Steal), "Stolen"}, {envP(0, E_LegatoInv), "Restarts in legato"}},
                    26);
    sectionEnd(c, x, y, w);
  }
  {
    char sum[24];
    std::snprintf(sum, sizeof sum, "%d notes", vali(c, G_Poly));
    const bool det = section(c, "voices", x, y, w, "Voices", sum, true);
    y = fieldGrid(c, x, y, w, 1, 32, {{G_Amp, "Depth"}, {G_MasterTune, "Fine tune"}}, 26);
    if (det)
      y = fieldGrid(c, x, y, w, 1, 32,
                    {{G_Poly, "Notes"}, {G_Priority, "Steal"}, {G_SameNote, "One per key"}, {G_Mono, "One voice"}, {G_Legato, "Glide notes"},
                     {G_PortaTime, "Glide time"}, {G_Transpose, "Transpose"}, {G_PortaCurve, "Glide curve"}, {G_PortaAlways, "Glide every note"},
                     {G_PortaScaled, "Constant glide rate"}, {G_BendUp, "Bend up"}, {G_BendDown, "Bend down"}, {G_EnvScale, "All times"}},
                    26);
  }
  ImGui::PopID();
  return y;
}


Curve lfoShapePublic(const Ctx& c, int l) { return lfoShapeOf(c, l); }
void lfoMini(Ctx& c, int l, float x, float y, float w, float h) { drawCurve(c, lfoShapeOf(c, l), x, y, w, h, c.P.fam[FamLfo], false); }
void envMini(Ctx& c, int e, float x, float y, float w, float h) {
  const float a0 = valf(c, envP(e, E_Attack)), d0 = valf(c, envP(e, E_Decay)), s0 = valf(c, envP(e, E_Sustain)), r0 = valf(c, envP(e, E_Release));
  const float tot = std::max(0.05f, a0 + d0 + r0 + 0.3f);
  const float xa = x + w * a0 / tot, xd = xa + w * d0 / tot, xs = xd + w * 0.3f / tot, xr = x + w;
  const float yb = y + h, yt = y, ys = yb - (yb - yt) * s0;
  const ImVec2 pts[5] = {c.p(x, yb), c.p(xa, yt), c.p(xd, ys), c.p(xs, ys), c.p(xr, yb)};
  c.dl->AddPolyline(pts, 5, c.P.fam[FamEnv], 0, std::max(1.2f, c.s(1.4f)));
}


// The Amp block: Envelope 1 (every note's level) and the depth; the voices live on the Play tab.
float ampColumn(Ctx& c, float x, float y, float w) {
  ImGui::PushID("ampcol");
  const bool det = showDetails(c);
  const float eh = det ? 120 : fitPic(c, 72);
  envGraph(c, 0, x, y, w, eh);
  y += eh + 8;
  const float dx = w / 4;
  const int ks[4] = {E_Attack, E_Decay, E_Sustain, E_Release};
  const char* ls[4] = {"Attack", "Decay", "Sustain", "Release"};
  y = dialRow(y);
  for (int i = 0; i < 4; ++i) knob(c, envP(0, ks[i]), x + dx * (i + 0.5f), y + 20, 16, ls[i], KnobValue);
  y += kDialRowH;
  if (det) {
    segments(c, envP(0, E_Bpm), x, y, w, 24, {"Time", "Beats"});
    y += 32;
    y = fieldGrid(c, x, y, w, 1, 32,
                  {{G_Amp, "Depth"}, {envP(0, E_Hold), "Hold"}, {envP(0, E_CurveA), "Attack curve"}, {envP(0, E_CurveD), "Decay curve"},
                   {envP(0, E_CurveR), "Release curve"}, {envP(0, E_Start), "Start level"}, {envP(0, E_End), "End level"},
                   {envP(0, E_Steal), "Stolen"}, {envP(0, E_LegatoInv), "Restarts in legato"}},
                  26);
  }
  ImGui::PopID();
  return y;
}

// An LFO block: its shape large (a click opens the LFO editor), then rate, start and sync.
float lfoBlockBody(Ctx& c, int l, float x, float y, float w) {
  ImGui::PushID(l + 7300);
  const bool det = showDetails(c);
  const float gh = fitPic(c, 104);
  display(c, x, y, w, gh, 8, 2);
  drawCurve(c, lfoShapeOf(c, l), x + 4, y + 6, w - 8, gh - 12, c.P.fam[FamLfo], true);
  {  // the name of the shape
    const int sh = vali(c, lfoP(l, L_Shape));
    const auto& sn = lfoShapeNames();
    const LfoData& ld = c.m.lfo[static_cast<size_t>(l)];
    const std::string nm = !ld.pts.empty() ? (ld.name.empty() ? std::string("Custom") : ld.name)
                                           : (sh >= 1 && sh < static_cast<int>(sn.size()) ? sn[static_cast<size_t>(sh)] : std::string("Triangle"));
    if (textW(c, nm.c_str(), 10.0f, false) < w - 16) text(c, x + 6, y + 4, nm.c_str(), c.P.textDim, 10.0f, false);
  }
  ImGui::SetCursorScreenPos(c.p(x, y));
  ImGui::InvisibleButton("##open", c.F.sz(w, gh));
  markPicture();
  if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) c.u.lfoTab = l, openView(c.u, VLfo);
  if (ImGui::IsItemHovered()) c.a.setHint("LFO " + std::to_string(l + 1) + ": double-click to draw its shape in the LFO editor");
  y += gh + 8;
  // the dials at the left, how it runs at the right (one row: no spare height)
  const bool wide = w > 380;
  const float kw = wide ? w * 0.6f : w, dx = kw / 4;
  if (!wide) {  // how it runs above the dials, so the dial row is last (one baseline across the row)
    const float half = (w - 8) * 0.5f;
    segments(c, lfoP(l, L_Bpm), x, y, half, 24, {"Free", "Synced"});
    segments(c, lfoP(l, L_Mode), x + half + 8, y, half, 24, {"Free", "Restart", "Once"});
    y += 32;
  }
  y = dialRow(y);
  knob(c, lfoP(l, L_Rate), x + dx * 0.5f, y + 20, 16, "Rate", KnobValue);
  knob(c, lfoP(l, L_Rise), x + dx * 1.5f, y + 20, 16, "Fade in", KnobValue);
  knob(c, lfoP(l, L_Smooth), x + dx * 2.5f, y + 20, 16, "Soften", KnobValue);
  knob(c, lfoP(l, L_Phase), x + dx * 3.5f, y + 20, 16, "Phase", KnobValue);
  if (wide) {
    const float sx = x + kw + 12, sw = w - kw - 12;
    segments(c, lfoP(l, L_Bpm), sx, y + 4, sw, 24, {"Free", "Synced"});
    segments(c, lfoP(l, L_Mode), sx, y + 34, sw, 24, {"Free", "Restart", "Once"});
  }
  y += kDialRowH;
  if (det)
    y = fieldGrid(c, x, y, w, 2, 32,
                  {{lfoP(l, L_Type), "Kind"}, {lfoP(l, L_Delay), "Wait"}, {lfoP(l, L_Dot), "Dotted"}, {lfoP(l, L_Trip), "Triplet"},
                   {lfoP(l, L_Rate10x), "Rate x 10"}, {lfoP(l, L_Mono), "One for all notes"}, {lfoP(l, L_Dir), "Direction"},
                   {lfoP(l, L_Swing), "Swing"}, {lfoP(l, L_Anchor), "Song lock"}, {lfoP(l, L_PhaseSnap), "Phase snap"},
                   {lfoP(l, L_GridX), "Grid across"}, {lfoP(l, L_GridY), "Grid down"}},
                  26);
  ImGui::PopID();
  return y;
}

// An envelope block: its shape (a click opens the envelope editor) and the four main times.
float envBlockBody(Ctx& c, int e, float x, float y, float w) {
  ImGui::PushID(e + 7400);
  const bool det = showDetails(c);
  const float gh = fitPic(c, 64);
  display(c, x, y, w, gh, 4, 2);
  envMini(c, e, x + 6, y + 8, w - 12, gh - 16);
  ImGui::SetCursorScreenPos(c.p(x, y));
  ImGui::InvisibleButton("##open", c.F.sz(w, gh));
  markPicture();
  if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) c.u.envTab = e, openView(c.u, VEnv);
  if (ImGui::IsItemHovered()) c.a.setHint("Envelope " + std::to_string(e + 1) + ": double-click to shape it in the envelope editor");
  y += gh + 8;
  const float dx = w / 4;
  const int ks[4] = {E_Attack, E_Decay, E_Sustain, E_Release};
  const char* ls[4] = {"Attack", "Decay", "Sustain", "Release"};
  const char* sh[4] = {"A", "D", "S", "R"};  // a narrow block: the letters (the value shows on hover)
  const bool narrow = dx < 46;
  y = dialRow(y);
  for (int i = 0; i < 4; ++i) knob(c, envP(e, ks[i]), x + dx * (i + 0.5f), y + 20, narrow ? 13 : 15, narrow ? sh[i] : ls[i], narrow ? 0 : KnobValue);
  y += kDialRowH;
  if (det)
    y = fieldGrid(c, x, y, w, 1, 32,
                  {{envP(e, E_Hold), "Hold"}, {envP(e, E_Bpm), "Times in beats"}, {envP(e, E_CurveA), "Attack curve"},
                   {envP(e, E_CurveD), "Decay curve"}, {envP(e, E_CurveR), "Release curve"}, {envP(e, E_Start), "Start level"},
                   {envP(e, E_End), "End level"}, {envP(e, E_Steal), "Stolen"}, {envP(e, E_LegatoInv), "Restarts in legato"}},
                  26);
  ImGui::PopID();
  return y;
}

// Play: Voices, the notes, glide and bend of the whole sound.
void voicesView(Ctx& c) {
  stageHead(c, "Voices");
  const float gap = 12, cw = (kStageW - 24 - gap * 2) / 3, y0 = kStageY + 10, ch = 262;
  {
    const float y = card(c, 12, y0, cw, ch, "Notes");
    fieldGrid(c, 24, y, cw - 24, 1, 34, {{G_Poly, "Notes at once"}, {G_Priority, "Steals"}, {G_SameNote, "One per key"}, {G_Mono, "One voice"}}, 28);
  }
  {
    const float y = card(c, 12 + cw + gap, y0, cw, ch, "Glide");
    fieldGrid(c, 24 + cw + gap, y, cw - 24, 1, 34,
              {{G_Legato, "Glide notes"}, {G_PortaTime, "Glide time"}, {G_PortaCurve, "Glide curve"}, {G_PortaAlways, "Glide every note"},
               {G_PortaScaled, "Constant glide rate"}},
              28);
  }
  {
    const float x = 12 + (cw + gap) * 2;
    const float y = card(c, x, y0, cw, ch, "Pitch and level");
    fieldGrid(c, x + 12, y, cw - 24, 1, 34,
              {{G_BendUp, "Bend up"}, {G_BendDown, "Bend down"}, {G_Transpose, "Transpose"}, {G_MasterTune, "Fine tune"}, {G_Amp, "Depth"},
               {G_EnvScale, "All envelope times"}},
              28);
  }
}

}  // namespace gl::packs::plasma::ui
