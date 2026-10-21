// Gridline — "utility" pack: editors for the EQ / filter / routing plugins and
// the dispatcher (visual editors live in utility_view_ui.cpp).
#include "../../core/I18n.h"
#include "../../engine/FlFilterRef.h"
#include "../PacksUi.h"
#include <algorithm>
#include <cmath>
#include <functional>
#include <cstdio>
#include <map>
#include <random>
#include "../../core/Params.h"
#include "../../engine/Dsp.h"
#include "utility_widgets.h"

namespace gl::packs::utility {

using theme::pal;

namespace {

uint64_t gestureKey(const PluginTarget& t, uint64_t tag) {
  return tag ^ (t.key() << 4) ^ static_cast<uint64_t>(ImGui::GetFrameCount());
}

// Response graph frame shared by the filter editors: background, spectrum, grid.
void graphFrame(App& a, const PluginTarget& t, ImDrawList* dl, ImVec2 g0, ImVec2 g1, double range) {
  const auto& P = pal();
  dl->AddRectFilled(g0, g1, P.surface950, 8);
  const int N = 160;
  float db[N];
  pui::spectrum(t.tap(a), t.key(), N, db, a.engine.sampleRate());
  float w = g1.x - g0.x, h = g1.y - g0.y;
  for (int k = 0; k + 1 < N; ++k) {
    float x0 = g0.x + w * k / N, x1 = g0.x + w * (k + 1) / N;
    float hh = std::clamp((db[k] + 90.0f) / 90.0f, 0.0f, 1.0f) * h;
    if (hh > 0.5f) dl->AddRectFilled(ImVec2(x0, g1.y - hh), ImVec2(x1, g1.y), theme::withAlpha(P.text, 0.06f));
  }
  pui::drawFreqGrid(dl, g0, g1, range);
}

// Curve through per-pixel dB values.
void drawCurve(ImDrawList* dl, ImVec2 g0, ImVec2 g1, double range, const std::function<double(double)>& dbAt, ImU32 col, float thick,
               bool fill) {
  float w = g1.x - g0.x, h = g1.y - g0.y;
  int W = static_cast<int>(w);
  std::vector<ImVec2> pts(static_cast<size_t>(W));
  float zy = g0.y + h * 0.5f;
  for (int x = 0; x < W; ++x) {
    double db = dbAt(pui::xToFreq(g0.x + x, g0.x, w));
    pts[static_cast<size_t>(x)] = ImVec2(g0.x + x, zy - static_cast<float>(std::clamp(db, -range * 1.15, range * 1.15) / range) * h * 0.5f);
  }
  if (fill)  // one rect per column: no anti-aliased seams between slices
    for (int x = 0; x < W; ++x) {
      float y = pts[static_cast<size_t>(x)].y;
      if (std::fabs(y - zy) > 0.5f) dl->AddRectFilled(ImVec2(g0.x + x, std::min(y, zy)), ImVec2(g0.x + x + 1, std::max(y, zy)), theme::withAlpha(col, 0.08f));
    }
  dl->AddPolyline(pts.data(), W, col, 0, thick);
}

// ================================================================ Curve EQ
enum DrawMode { DmFree, DmLine, DmPoints };
struct CeUi {
  int snap = 0;
  int mode = DmFree;
  ImVec2 last{0, 0}, lineStart{0, 0};
  int point = -1;
  bool erasing = false;
  uint64_t gesture = 0;
  // Cached realised response.
  std::vector<float> respDb;
  std::vector<float> respParams;
  double respSr = 0;
  float respW = 0;
  CeBank bank;
};
std::map<uint64_t, CeUi> ceUis;

void ceResponse(CeUi& ui, const PluginState& st, double sr, float gw, ImVec2 g0) {
  if (ui.respParams == st.params && ui.respSr == sr && ui.respW == gw) return;
  ui.respParams = st.params;
  ui.respSr = sr;
  ui.respW = gw;
  if (ui.bank.sr != sr) ui.bank.prepare(sr);
  float morphed[kCeBands];
  ceMorph(st, val(st, CeMorphX), val(st, CeMorphY), morphed);
  double depth = val(st, CeDepth, 1), shift = val(st, CeShift), out = val(st, CeOutput);
  int W = static_cast<int>(gw);
  ui.respDb.assign(static_cast<size_t>(W), 0.f);
  if (val(st, CePhase) >= 0.5f) {
    for (int x = 0; x < W; ++x) ui.respDb[static_cast<size_t>(x)] = static_cast<float>(ceTargetDb(morphed, depth, shift, pui::xToFreq(g0.x + x, g0.x, gw)) + out);
    return;
  }
  double target[kCeBands], gains[kCeBands];
  for (int b = 0; b < kCeBands; ++b) target[b] = ceTargetDb(morphed, depth, shift, ceBandHz(b));
  ui.bank.solve(target, gains);
  Coefs c[kCeBands];
  for (int b = 0; b < kCeBands; ++b) c[b] = ceBandCoefs(sr, b, gains[b]);
  for (int x = 0; x < W; ++x) {
    double hz = pui::xToFreq(g0.x + x, g0.x, gw), m = 1;
    for (int b = 0; b < kCeBands; ++b)
      if (std::fabs(gains[b]) > 1e-3) m *= c[b].magnitude(hz, sr);
    ui.respDb[static_cast<size_t>(x)] = static_cast<float>(20 * std::log10(std::max(1e-9, m)) + out);
  }
}


// ---------------------------------------------------- reference skin helpers
// EQUO-style panel (curve EQ): slate panel, orange wheels.
const skin::Skin& curveEqSkin() {
  static const skin::Skin k = [] {
    skin::Skin s = skin::Skin::fl(0x3E484F, 0x46515A, 0x3F484D, 0xFFA830, 0x42474A, 0xB9BFC3);
    s.graphBg = skin::hex(0x262E33);
    s.graphGrid = skin::hex(0x313B41);
    s.graphGridStrong = skin::hex(0x44505A);
    s.graphCurve = skin::hex(0xFFC060);
    s.lcdBg = skin::hex(0x262B2F);
    s.lcdText = skin::hex(0xFFC060);
    s.select = skin::hex(0x49B5F5);
    s.accent2 = skin::hex(0x49B5F5);
    return s;
  }();
  return k;
}

// ================================================================ Curve EQ
// Reference panel 565 × 240: band visual on the left (370 × 138 under a mode
// strip), settings strip above it, bank selector underneath, parameter
// column on the right (shift / mix / morph wheels, smooth / volume wheels).
void editorCurveEq(App& a, const PluginTarget& t, float width) {
  const PluginState st = *t.state(a.doc.song());
  const skin::Skin& K = curveEqSkin();
  constexpr float kW = 565, kH = 240;
  skin::Form F = skin::Form::fit(width, kW);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::panel(dl, F.o, F.p(kW, kH), K, 4);
  skin::Scope scope(K);
  const auto& P = pal();
  CeUi& ui = ceUis[t.key()];
  const double sr = a.engine.sampleRate();
  const double range = kCeRange;
  const char* snapNames[kCeSnaps] = {"A", "B", "C", "D"};
  ImGuiIO& io = ImGui::GetIO();
  const float fs = F.font(8.0f);

  // ---- settings strip (above the visual): curve tools, draw mode, latency ----
  skin::well(dl, F.p(10, 5), F.p(392, 27), K, 3 * F.s);
  if (skin::button("##cetools", F.p(14, 7), F.sz(24, 18), K, "=", false, fs)) ImGui::OpenPopup("##cetools");
  if (ImGui::IsItemHovered()) a.setHint("Curve tools: flat, invert, smooth, random, copy between snapshots");
  if (ImGui::BeginPopup("##cetools")) {
    PluginState ns = st;
    auto pt = [&](int b) -> float& { return ns.params[static_cast<size_t>(cePoint(ui.snap, b))]; };
    bool apply = false;
    const char* label = "edit curve";
    ImGui::TextDisabled(i18n::tr("Snapshot %s"), snapNames[ui.snap]);
    if (ImGui::MenuItem(i18n::tr("Flat"))) {
      for (int b = 0; b < kCeBands; ++b) pt(b) = 0;
      apply = true, label = "flatten curve";
    }
    if (ImGui::MenuItem(i18n::tr("Invert"))) {
      for (int b = 0; b < kCeBands; ++b) pt(b) = -pt(b);
      apply = true, label = "invert curve";
    }
    if (ImGui::MenuItem(i18n::tr("Smooth"))) {
      float tmp[kCeBands];
      for (int b = 0; b < kCeBands; ++b) tmp[b] = 0.25f * pt(std::max(0, b - 1)) + 0.5f * pt(b) + 0.25f * pt(std::min(kCeBands - 1, b + 1));
      for (int b = 0; b < kCeBands; ++b) pt(b) = tmp[b];
      apply = true, label = "smooth curve";
    }
    if (ImGui::MenuItem(i18n::tr("Random (gentle)"))) {
      std::mt19937 rng(static_cast<uint32_t>(ImGui::GetFrameCount()) * 2654435761u);
      std::uniform_real_distribution<float> d(-1.f, 1.f);
      float v = 0;
      for (int b = 0; b < kCeBands; ++b) {
        v = std::clamp(v * 0.8f + d(rng) * 2.5f, -9.f, 9.f);
        pt(b) = v;
      }
      apply = true, label = "random curve";
    }
    ImGui::Separator();
    for (int k = 0; k < kCeSnaps; ++k) {
      if (k == ui.snap) continue;
      char item[32];
      std::snprintf(item, sizeof item, "Copy from %s", snapNames[k]);
      if (ImGui::MenuItem(item)) {
        for (int b = 0; b < kCeBands; ++b) pt(b) = st.params[static_cast<size_t>(cePoint(k, b))];
        apply = true, label = "copy snapshot";
      }
    }
    if (ImGui::MenuItem(i18n::tr("Copy to all snapshots"))) {
      for (int k = 0; k < kCeSnaps; ++k)
        for (int b = 0; b < kCeBands; ++b) ns.params[static_cast<size_t>(cePoint(k, b))] = st.params[static_cast<size_t>(cePoint(ui.snap, b))];
      apply = true, label = "copy to all snapshots";
    }
    if (apply) setPluginState(a, t, ns, label);
    ImGui::EndPopup();
  }
  const char* modes[3] = {"DRAW", "LINE", "POINTS"};
  const char* modeHints[3] = {"Freehand: drag to paint the curve, right-drag to erase", "Line: drag a straight ramp across bands",
                              "Points: drag single points, wheel for fine steps, double-click to zero"};
  for (int m = 0; m < 3; ++m) {
    ImGui::PushID(100 + m);
    if (skin::button("##mode", F.p(42 + m * 52.0f, 7), F.sz(50, 18), K, modes[m], ui.mode == m, fs)) ui.mode = m;
    if (ImGui::IsItemHovered()) a.setHint(modeHints[m]);
    ImGui::PopID();
  }
  {
    char lat[64];
    if (val(st, CePhase) >= 0.5f) std::snprintf(lat, sizeof lat, "LATENCY %d SMP (%.0f MS)", kCeLinearLatency, kCeLinearLatency * 1000.0 / sr);
    else std::snprintf(lat, sizeof lat, "ZERO LATENCY");
    ImGui::PushFont(a.fontMono, fs);
    ImVec2 ts = ImGui::CalcTextSize(lat);
    ImGui::PopFont();
    textAt(dl, a.fontMono, fs, ImVec2(F.p(388, 0).x - ts.x, F.p(0, 16).y - ts.y * 0.5f), K.textDim, lat);
  }

  // ---- band visual ----
  ImVec2 g0 = F.p(10, 33), g1 = F.p(392, 200);
  ImVec2 gs(g1.x - g0.x, g1.y - g0.y);
  const float gw = gs.x;
  ImGui::SetCursorScreenPos(g0);
  ImGui::InvisibleButton("##cegraph", gs, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
  bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
  graphFrame(a, t, dl, g0, g1, range);
  dl->PushClipRect(g0, g1, true);
  auto bandX = [&](int b) { return g0.x + gw * static_cast<float>(b) / (kCeBands - 1); };
  auto dbY = [&](double db) { return g0.y + gs.y * 0.5f - static_cast<float>(std::clamp(db, -range * 1.15, range * 1.15) / range) * gs.y * 0.5f; };
  auto yDb = [&](float y) { return std::clamp((g0.y + gs.y * 0.5f - y) / (gs.y * 0.5f) * range, -range, range); };
  for (int k = 0; k < kCeSnaps; ++k) {
    if (k == ui.snap) continue;
    const float* pts = &st.params[static_cast<size_t>(cePoint(k, 0))];
    drawCurve(dl, g0, g1, range, [&](double hz) { return ceCurveAt(pts, hz); }, theme::withAlpha(K.text, 0.16f), 1.0f, false);
  }
  const float* cur = &st.params[static_cast<size_t>(cePoint(ui.snap, 0))];
  drawCurve(dl, g0, g1, range, [&](double hz) { return ceCurveAt(cur, hz); }, theme::withAlpha(K.text, 0.7f), 1.2f, true);
  for (int b = 0; b < kCeBands; ++b) {
    bool hl = ui.mode == DmPoints && b == ui.point;
    dl->AddCircleFilled(ImVec2(bandX(b), dbY(cur[b])), hl ? 5.0f : 2.8f, hl ? K.select : K.textDim, 12);
  }
  ceResponse(ui, st, sr, gw, g0);
  {
    std::vector<ImVec2> pts(ui.respDb.size());
    for (size_t x = 0; x < ui.respDb.size(); ++x) pts[x] = ImVec2(g0.x + static_cast<float>(x), dbY(ui.respDb[x]));
    dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), K.graphCurve, 0, 2.2f);
  }
  textAt(dl, a.fontMonoBold, 12.0f, ImVec2(g1.x - 14, g0.y + 6), K.text, snapNames[ui.snap]);

  auto setPoints = [&](PluginState& ns, float x0, float db0, float x1, float db1) {
    if (x1 < x0) std::swap(x0, x1), std::swap(db0, db1);
    for (int b = 0; b < kCeBands; ++b) {
      float bx = bandX(b);
      float half = gw / (kCeBands - 1) * 0.5f;
      if (bx < x0 - half || bx > x1 + half) continue;
      float tt = x1 - x0 > 1e-3f ? std::clamp((bx - x0) / (x1 - x0), 0.f, 1.f) : 0.f;
      ns.params[static_cast<size_t>(cePoint(ui.snap, b))] = static_cast<float>(std::clamp(db0 + (db1 - db0) * tt, -static_cast<float>(range), static_cast<float>(range)));
    }
  };
  int nearest = std::clamp(static_cast<int>(std::lround((io.MousePos.x - g0.x) / gw * (kCeBands - 1))), 0, kCeBands - 1);
  if (ImGui::IsItemActivated()) {
    ui.gesture = gestureKey(t, 0x4345510000000000ull);
    ui.erasing = ImGui::IsMouseDown(ImGuiMouseButton_Right);
    ui.last = ui.lineStart = io.MousePos;
    ui.point = nearest;
    if (ui.mode == DmFree) {
      PluginState ns = st;
      setPoints(ns, io.MousePos.x, ui.erasing ? 0.f : static_cast<float>(yDb(io.MousePos.y)), io.MousePos.x,
                ui.erasing ? 0.f : static_cast<float>(yDb(io.MousePos.y)));
      setPluginState(a, t, ns, "draw EQ curve", ui.gesture);
    }
  }
  if (!act && ui.mode == DmPoints && hov) ui.point = nearest;
  if (act) {
    PluginState ns = st;
    bool edit = false;
    if (ui.mode == DmFree && (io.MousePos.x != ui.last.x || io.MousePos.y != ui.last.y)) {
      float d0 = ui.erasing ? 0.f : static_cast<float>(yDb(ui.last.y)), d1 = ui.erasing ? 0.f : static_cast<float>(yDb(io.MousePos.y));
      setPoints(ns, ui.last.x, d0, io.MousePos.x, d1);
      ui.last = io.MousePos;
      edit = true;
    } else if (ui.mode == DmPoints && ui.point >= 0 && io.MouseDelta.y != 0) {
      float& v = ns.params[static_cast<size_t>(cePoint(ui.snap, ui.point))];
      if (io.KeyShift)
        v = std::clamp(v - io.MouseDelta.y * 0.05f, -static_cast<float>(range), static_cast<float>(range));
      else
        v = static_cast<float>(yDb(io.MousePos.y));
      edit = true;
    } else if (ui.mode == DmLine) {
      dl->AddLine(ui.lineStart, io.MousePos, K.select, 1.5f);
    }
    if (edit) setPluginState(a, t, ns, "draw EQ curve", ui.gesture);
  }
  if (ImGui::IsItemDeactivated()) {
    if (ui.mode == DmLine) {
      PluginState ns = st;
      setPoints(ns, ui.lineStart.x, ui.erasing ? 0.f : static_cast<float>(yDb(ui.lineStart.y)), io.MousePos.x,
                ui.erasing ? 0.f : static_cast<float>(yDb(io.MousePos.y)));
      setPluginState(a, t, ns, "draw EQ line", ui.gesture);
    }
    a.doc.endGesture();
  }
  if (hov && io.MouseWheel != 0) {
    PluginState ns = st;
    float& v = ns.params[static_cast<size_t>(cePoint(ui.snap, nearest))];
    v = std::clamp(v + io.MouseWheel * 0.5f, -static_cast<float>(range), static_cast<float>(range));
    setPluginState(a, t, ns, "nudge EQ point", 0x4345575700000000ull ^ (t.key() << 4) ^ static_cast<uint64_t>(nearest));
  }
  if (hov && ui.mode == DmPoints && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
    PluginState ns = st;
    ns.params[static_cast<size_t>(cePoint(ui.snap, nearest))] = 0;
    setPluginState(a, t, ns, "zero EQ point");
  }
  dl->PopClipRect();
  if (hov || act) {
    double hz = pui::xToFreq(io.MousePos.x, g0.x, gw);
    char buf[200];
    std::snprintf(buf, sizeof buf, "%.0f Hz · %+.1f dB · drawing snapshot %s · %s · wheel nudges the nearest point", hz, yDb(io.MousePos.y),
                  snapNames[ui.snap], modeHints[ui.mode]);
    a.setHint(buf);
  }

  // ---- bank strip (under the visual): snapshot to draw ----
  skin::well(dl, F.p(10, 205), F.p(392, 233), K, 3 * F.s);
  textAt(dl, a.fontMonoBold, fs, ImVec2(F.p(18, 0).x, F.p(0, 219).y - fs * 0.6f), K.textDim, "BANK");
  for (int k = 0; k < kCeSnaps; ++k) {
    ImGui::PushID(k);
    if (skin::button("##snap", F.p(56 + k * 40.0f, 209), F.sz(38, 20), K, snapNames[k], ui.snap == k, F.font(9))) {
      ui.snap = k;
      if (io.KeyShift) {
        PluginState ns = st;
        ns.params[CeMorphX] = (k & 1) ? 1.f : 0.f;
        ns.params[CeMorphY] = (k & 2) ? 1.f : 0.f;
        setPluginState(a, t, ns, "morph to snapshot");
      }
    }
    if (ImGui::IsItemHovered()) a.setHint("Snapshot to draw · Shift+click: move the morph here");
    ImGui::PopID();
  }
  {
    static const char* ph[2] = {"MIN PHASE", "LINEAR"};
    skin::switchSel(a, t, CePhase, F.p(232, 209), F.sz(156, 20), K, ph, 2, true);
  }

  // ---- parameter column ----
  skin::well(dl, F.p(398, 5), F.p(560, 128), K, 4 * F.s);
  skin::labeledWheel(a, t, CeShift, F, 406, 14, 40, K, "SHIFT", skin::WheelBigLine | skin::WheelCentered);
  skin::labeledWheel(a, t, CeDepth, F, 459, 14, 40, K, "DEPTH", skin::WheelBigLine | skin::WheelCentered);
  skin::labeledWheel(a, t, CeSmooth, F, 512, 14, 40, K, "GLIDE");
  skin::labeledWheel(a, t, CeOutput, F, 432, 72, 40, K, "VOLUME", skin::WheelBigLine | skin::WheelCentered);
  {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%+.1f dB", val(st, CeOutput));
    skin::valueStrip(dl, F.p(482, 82), F.p(554, 98), K, buf, fs);
  }
  // Morph pad (the reference's morph wheel, as a 4-corner X/Y morph).
  {
    ImVec2 p0 = F.p(398, 134), p1 = F.p(560, 233);
    ImVec2 ps(p1.x - p0.x, p1.y - p0.y);
    ImGui::SetCursorScreenPos(p0);
    ImGui::InvisibleButton("##morph", ps);
    skin::graphWell(dl, p0, p1, K);
    float pad = 14;
    ImVec2 q0(p0.x + pad, p0.y + pad), q1(p1.x - pad, p1.y - pad);
    dl->AddRect(q0, q1, K.graphGridStrong, 3);
    dl->AddLine(ImVec2((q0.x + q1.x) * 0.5f, q0.y), ImVec2((q0.x + q1.x) * 0.5f, q1.y), K.graphGrid);
    dl->AddLine(ImVec2(q0.x, (q0.y + q1.y) * 0.5f), ImVec2(q1.x, (q0.y + q1.y) * 0.5f), K.graphGrid);
    for (int k = 0; k < kCeSnaps; ++k) {
      float qx0 = (k & 1) ? (q0.x + q1.x) * 0.5f + 4 : q0.x + 4, qy0 = (k & 2) ? (q0.y + q1.y) * 0.5f + 4 : q0.y + 4;
      float qw = (q1.x - q0.x) * 0.5f - 8, qh = (q1.y - q0.y) * 0.5f - 8;
      const float* pts = &st.params[static_cast<size_t>(cePoint(k, 0))];
      ImVec2 line[kCeBands];
      for (int b = 0; b < kCeBands; ++b)
        line[b] = ImVec2(qx0 + qw * b / (kCeBands - 1), qy0 + qh * 0.5f - static_cast<float>(pts[b] / range) * qh * 0.5f);
      dl->AddPolyline(line, kCeBands, theme::withAlpha(K.text, k == ui.snap ? 0.6f : 0.25f), 0, 1.0f);
    }
    textAt(dl, a.fontMonoBold, 10.0f, ImVec2(p0.x + 3, p0.y + 1), K.textDim, "A");
    textAt(dl, a.fontMonoBold, 10.0f, ImVec2(p1.x - 10, p0.y + 1), K.textDim, "B");
    textAt(dl, a.fontMonoBold, 10.0f, ImVec2(p0.x + 3, p1.y - 14), K.textDim, "C");
    textAt(dl, a.fontMonoBold, 10.0f, ImVec2(p1.x - 10, p1.y - 14), K.textDim, "D");
    float mx = val(st, CeMorphX), my = val(st, CeMorphY);
    ImVec2 dot(q0.x + mx * (q1.x - q0.x), q0.y + my * (q1.y - q0.y));
    dl->AddCircle(dot, 9, theme::withAlpha(K.fill, 0.5f), 24, 1.5f);
    dl->AddCircleFilled(dot, 5, K.fill, 16);
    static uint64_t morphGesture = 0;
    if (ImGui::IsItemActivated()) morphGesture = gestureKey(t, 0x4D4F525000000000ull);
    if (ImGui::IsItemActive()) {
      PluginState ns = st;
      ns.params[CeMorphX] = std::clamp((io.MousePos.x - q0.x) / (q1.x - q0.x), 0.f, 1.f);
      ns.params[CeMorphY] = std::clamp((io.MousePos.y - q0.y) / (q1.y - q0.y), 0.f, 1.f);
      if (ns.params != st.params) setPluginState(a, t, ns, "morph EQ", morphGesture);
    }
    if (ImGui::IsItemDeactivated()) a.doc.endGesture();
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
      PluginState ns = st;
      ns.params[CeMorphX] = std::round(ns.params[CeMorphX]);
      ns.params[CeMorphY] = std::round(ns.params[CeMorphY]);
      setPluginState(a, t, ns, "snap morph");
    }
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
      char buf[120];
      std::snprintf(buf, sizeof buf, "Morph X %.0f%% · Y %.0f%%   (drag between snapshots A-D · double-click snaps to a corner)", mx * 100, my * 100);
      a.setHint(buf);
    }
  }
  (void)P;
  F.end(kW, kH);
}

namespace {
// |H(f)| of the reference resonant low-pass kernel (a, b per sample).
double quickKernelMag(double a, double b, double f, double sr) {
  const double w = 2 * 3.14159265358979323846 * f / sr;
  const double cr = std::cos(w), ci = -std::sin(w), c2r = std::cos(2 * w), c2i = -std::sin(2 * w);
  // (1 - z^-1)(1 - b z^-1) + a z^-1 = 1 - (1 + b - a) z^-1 + b z^-2
  double re = 1 - (1 + b - a) * cr + b * c2r, im = -(1 + b - a) * ci + b * c2i;
  return a / std::max(1e-12, std::sqrt(re * re + im * im));
}
// |H(f)| of a direct-form biquad.
double biquadMag(const flref::FixedBiquad& q, double f, double sr) {
  const double w = 2 * 3.14159265358979323846 * f / sr;
  const double cr = std::cos(w), ci = -std::sin(w), c2r = std::cos(2 * w), c2i = -std::sin(2 * w);
  double nr = q.b0 + q.b1 * cr + q.b2 * c2r, ni = q.b1 * ci + q.b2 * c2i;
  double dr = 1 + q.a1 * cr + q.a2 * c2r, di = q.a1 * ci + q.a2 * c2i;
  return std::sqrt((nr * nr + ni * ni) / std::max(1e-24, dr * dr + di * di));
}
}  // namespace

// ============================================================= Free Filter
struct NodeUi {
  uint64_t gesture = 0;
};
std::map<uint64_t, NodeUi> nodeUis;

// Response graph strip shared by the small filter panels (form units).
constexpr float kGraphH = 96;

// Reference panel 419 × 105: TYPE / FREQ wheel modules over Q (value slider) /
// GAIN, and the CENTER selector module to the right; Gridline's response graph
// sits above the panel.
void editorFreeFilter(App& a, const PluginTarget& t, float width) {
  const PluginState st = *t.state(a.doc.song());
  const skin::Skin& K = fruitySkin(0xD9B3E5);
  constexpr float kW = 419;
  const float kH = kGraphH + 110;
  skin::Form F = skin::Form::fit(width, kW);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::panel(dl, F.o, F.p(kW, kH), K, 4);
  skin::Scope scope(K);
  const double sr = a.engine.sampleRate(), range = 24;
  ImGuiIO& io = ImGui::GetIO();
  int type = std::clamp(static_cast<int>(val(st, FfType) + 0.5f), 0, FtCount - 1);
  double hz = val(st, FfFreq, 1000), q = val(st, FfQ, 0.707), gain = val(st, FfGain);
  ImVec2 g0 = F.p(5, 5), g1 = F.p(kW - 5, kGraphH);
  ImVec2 gs(g1.x - g0.x, g1.y - g0.y);
  ImGui::SetCursorScreenPos(g0);
  ImGui::InvisibleButton("##ffgraph", gs);
  graphFrame(a, t, dl, g0, g1, range);
  dl->PushClipRect(g0, g1, true);
  const bool ffRef = val(st, FfModel, 1) >= 0.5f;  // J-26 reference: fixed 44.1 kHz design
  Coefs c = freeFilterCoefs(type, sr, hz, q, gain);
  flref::FixedBiquad fb;
  if (ffRef) fb.design(std::clamp(type, 0, 6), hz, val(st, FfQK, 0.165f), gain);
  drawCurve(dl, g0, g1, range, [&](double f) -> double {
    return 20 * std::log10(std::max(1e-9, ffRef ? biquadMag(fb, f, sr) : c.magnitude(f, sr)));
  }, K.graphCurve, 2.0f, true);
  bool hasGain = ffHasGain(type);
  float ny = hasGain ? g0.y + gs.y * 0.5f - static_cast<float>(gain / range) * gs.y * 0.5f
                     : g0.y + gs.y * 0.5f - static_cast<float>(std::log(q / 0.707) / std::log(20 / 0.707)) * gs.y * 0.45f;
  ImVec2 node(pui::freqToX(hz, g0.x, gs.x), ny);
  dl->AddCircleFilled(node, 7, K.fill, 20);
  dl->AddCircle(node, 11, theme::withAlpha(K.fill, 0.45f), 24, 1.5f);
  dl->PopClipRect();
  NodeUi& ui = nodeUis[t.key()];
  if (ImGui::IsItemActivated()) ui.gesture = gestureKey(t, 0x4646000000000000ull);
  if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 1)) {
    PluginState ns = st;
    float fine = io.KeyShift ? 0.2f : 1.0f;
    double fx = node.x + io.MouseDelta.x * fine;
    ns.params[FfFreq] = static_cast<float>(std::clamp(pui::xToFreq(static_cast<float>(fx), g0.x, gs.x), 10.0, 22000.0));
    if (hasGain)
      ns.params[FfGain] = static_cast<float>(std::clamp(gain - io.MouseDelta.y * fine * range * 2 / gs.y, -24.0, 24.0));
    else if (ffRef)
      ns.params[FfQK] = static_cast<float>(std::clamp(static_cast<double>(val(st, FfQK, 0.165f)) - io.MouseDelta.y * fine / gs.y, 0.0, 1.0));
    else
      ns.params[FfQ] = static_cast<float>(std::clamp(q * std::pow(1.02, -io.MouseDelta.y * fine), 0.1, 20.0));
    setPluginState(a, t, ns, "move filter", ui.gesture);
  }
  if (ImGui::IsItemDeactivated()) a.doc.endGesture();
  if (ImGui::IsItemHovered() && io.MouseWheel != 0) {
    PluginState ns = st;
    if (ffRef) ns.params[FfQK] = static_cast<float>(std::clamp(static_cast<double>(val(st, FfQK, 0.165f)) + 0.02 * io.MouseWheel, 0.0, 1.0));
    else ns.params[FfQ] = static_cast<float>(std::clamp(q * std::pow(1.15, io.MouseWheel), 0.1, 20.0));
    setPluginState(a, t, ns, "filter Q", 0x4646510000000000ull ^ t.key());
  }
  if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
    char buf[160];
    std::snprintf(buf, sizeof buf, "%.0f Hz · Q %.2f%s   (drag: frequency / %s · wheel: Q · Shift: fine)", hz, q,
                  hasGain ? (" · " + fmtDb(static_cast<float>(gain), " dB")).c_str() : "", hasGain ? "gain" : "Q");
    a.setHint(buf);
  }
  const float y0 = kGraphH + 5;
  menuParamBox(a, t, FfType, F, 5, y0, 160, 45, K, "TYPE");
  skin::paramBox(a, t, FfFreq, F, 170, y0, 160, 45, K, "FREQ");
  sliderBox(a, t, ffRef ? FfQK : FfQ, F, 5, y0 + 50, 160, 45, K, "Q");
  {
    skin::Skin kg = K;  // gain only acts on the shelf / peak types: dim it otherwise
    if (!hasGain) kg.fill = skin::darken(K.fill, 0.45f), kg.valueText = skin::darken(K.fill, 0.35f);
    skin::paramBox(a, t, FfGain, F, 170, y0 + 50, 160, 45, kg, hasGain ? "GAIN" : "GAIN (SHELF/PEAK)");
  }
  // CENTER module: DC removal selector.
  skin::well(dl, F.p(336, y0), F.p(413, y0 + 45), K, 4 * F.s);
  skin::pill(dl, F.p(353, y0), F.p(413, y0 + 16), K, "CENTER", F.font(8.5f));
  {
    static const char* dc[2] = {"OFF", "ON"};
    skin::switchSel(a, t, FfDc, F.p(340, y0 + 19), F.sz(69, 22), K, dc, 2, true);
  }
  skin::well(dl, F.p(336, y0 + 50), F.p(413, y0 + 95), K, 4 * F.s);
  skin::pill(dl, F.p(353, y0 + 50), F.p(413, y0 + 66), K, "ENGINE", F.font(8.5f));
  {
    static const char* eng[2] = {"OLD", "REF"};
    skin::switchSel(a, t, FfModel, F.p(340, y0 + 69), F.sz(69, 22), K, eng, 2, true);
  }
  F.end(kW, kH);
}

// ============================================================ Quick Lowpass
// Reference panel 335 × 55: CUTOFF and RESONANCE wheel modules with value
// sliders; the response graph sits above.
void editorQuickLp(App& a, const PluginTarget& t, float width) {
  const PluginState st = *t.state(a.doc.song());
  const skin::Skin& K = fruitySkin(0xE5B3E1);
  constexpr float kW = 335;
  const float kH = kGraphH + 60;
  skin::Form F = skin::Form::fit(width, kW);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::panel(dl, F.o, F.p(kW, kH), K, 4);
  skin::Scope scope(K);
  const double sr = a.engine.sampleRate(), range = 24;
  ImGuiIO& io = ImGui::GetIO();
  double hz = val(st, QlCutoff, 20000), reso = val(st, QlReso);
  ImVec2 g0 = F.p(5, 5), g1 = F.p(kW - 5, kGraphH);
  ImVec2 gs(g1.x - g0.x, g1.y - g0.y);
  ImGui::SetCursorScreenPos(g0);
  ImGui::InvisibleButton("##qlgraph", gs);
  graphFrame(a, t, dl, g0, g1, range);
  dl->PushClipRect(g0, g1, true);
  const bool qlRef = val(st, QlModel, 1) >= 0.5f;  // J-26 reference kernel
  const double qa = flref::quickLpCoef(val(st, QlCutK, 1)), qb = flref::quickLpRes(val(st, QlResK));
  if (qlRef) {
    hz = qa >= 1 ? 20000.0 : std::clamp(-sr * std::log(1 - std::min(0.999, qa)) / (2 * 3.14159265358979323846), 20.0, 20000.0);
    reso = val(st, QlResK);
  }
  bool open = qlRef ? qa >= 1 && qb == 0 : hz >= 19900 && reso < 1e-3;
  double k = 2.0 - 1.98 * std::clamp(reso * 0.97, 0.0, 1.0);
  Coefs c = freeFilterCoefs(FtLowPass, sr, std::min(hz, sr * 0.49), 1.0 / k, 0);
  drawCurve(dl, g0, g1, range, [&](double f) -> double {
    if (open) return 0.0;
    return 20 * std::log10(std::max(1e-9, qlRef ? quickKernelMag(qa, qb, f, sr) : c.magnitude(f, sr)));
  }, K.graphCurve, 2.0f, true);
  ImVec2 node(pui::freqToX(hz, g0.x, gs.x), g1.y - 12 - static_cast<float>(reso) * (gs.y - 24));
  dl->AddCircleFilled(node, 7, K.fill, 20);
  dl->PopClipRect();
  NodeUi& ui = nodeUis[t.key()];
  if (ImGui::IsItemActivated()) ui.gesture = gestureKey(t, 0x514C000000000000ull);
  if (!qlRef && ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 1)) {
    PluginState ns = st;
    float fine = io.KeyShift ? 0.2f : 1.0f;
    ns.params[QlCutoff] = static_cast<float>(std::clamp(pui::xToFreq(node.x + io.MouseDelta.x * fine, g0.x, gs.x), 20.0, 20000.0));
    ns.params[QlReso] = static_cast<float>(std::clamp(reso - io.MouseDelta.y * fine / (gs.y - 24), 0.0, 1.0));
    setPluginState(a, t, ns, "move lowpass", ui.gesture);
  }
  if (ImGui::IsItemDeactivated()) a.doc.endGesture();
  if (ImGui::IsItemHovered()) a.setHint("Drag: cutoff (left/right) and resonance (up/down) · Shift: fine");
  sliderBox(a, t, qlRef ? QlCutK : QlCutoff, F, 5, kGraphH + 5, 160, 45, K, "CUTOFF");
  sliderBox(a, t, qlRef ? QlResK : QlReso, F, 170, kGraphH + 5, 160, 45, K, "RESONANCE");
  {
    static const char* eng[2] = {"OLD", "REF"};
    skin::switchSel(a, t, QlModel, F.p(kW - 75, 9), F.sz(64, 14), K, eng, 2, true);
  }
  F.end(kW, kH);
}

// =============================================================== Bass Boost
// Reference panel 335 × 55: FREQ wheel module (value read-out) and AMOUNT
// module (value slider); the response graph sits above.
void editorBassBoost(App& a, const PluginTarget& t, float width) {
  const PluginState st = *t.state(a.doc.song());
  const skin::Skin& K = fruitySkin(0xE5B3B7);
  constexpr float kW = 335;
  const float kH = kGraphH + 60;
  skin::Form F = skin::Form::fit(width, kW);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::panel(dl, F.o, F.p(kW, kH), K, 4);
  skin::Scope scope(K);
  const double sr = a.engine.sampleRate(), range = 24;
  ImGuiIO& io = ImGui::GetIO();
  double hz = val(st, BbFreq, 80), amt = val(st, BbAmount, 0.4f);
  ImVec2 g0 = F.p(5, 5), g1 = F.p(kW - 5, kGraphH);
  ImVec2 gs(g1.x - g0.x, g1.y - g0.y);
  ImGui::SetCursorScreenPos(g0);
  ImGui::InvisibleButton("##bbgraph", gs);
  graphFrame(a, t, dl, g0, g1, range);
  dl->PushClipRect(g0, g1, true);
  const bool bbRef = val(st, BbModel, 1) >= 0.5f;  // J-26 reference: dry + G x two-pole low-pass
  Coefs c = bassBoostCoefs(sr, hz, amt);
  const double bbG = flref::bassBoostGain(amt), bbC = std::cos(2 * 3.14159265358979323846 * std::nearbyint(hz) / sr), bbM = bbC - 1;
  const double bbK1 = 2 - 2 * bbC, bbK2 = (bbM / 2 + std::sqrt(2.0) * std::sqrt(std::max(0.0, -bbM * bbM * bbM))) / (bbM / 2);
  auto bbMag = [&](double f) -> double {
    if (!bbRef) return c.magnitude(f, sr);
    // dry + G·LP: evaluate the complex sum
    const double w = 2 * 3.14159265358979323846 * f / sr, cr = std::cos(w), ci = -std::sin(w), c2r = std::cos(2 * w), c2i = -std::sin(2 * w);
    double dr = 1 - (1 + bbK2 - bbK1) * cr + bbK2 * c2r, di = -(1 + bbK2 - bbK1) * ci + bbK2 * c2i, d2 = std::max(1e-24, dr * dr + di * di);
    double lr = bbK1 * dr / d2, li = -bbK1 * di / d2;
    return std::sqrt((1 + bbG * lr) * (1 + bbG * lr) + (bbG * li) * (bbG * li));
  };
  drawCurve(dl, g0, g1, range, [&](double f) { return 20 * std::log10(std::max(1e-9, bbMag(f))); }, K.graphCurve, 2.0f, true);
  const double bbDb = bbRef ? 20 * std::log10(1 + bbG) : bassBoostDb(amt);
  ImVec2 node(pui::freqToX(hz, g0.x, gs.x), g0.y + gs.y * 0.5f - static_cast<float>(bbDb / range) * gs.y * 0.5f);
  dl->AddCircleFilled(node, 7, K.fill, 20);
  dl->PopClipRect();
  NodeUi& ui = nodeUis[t.key()];
  if (ImGui::IsItemActivated()) ui.gesture = gestureKey(t, 0x4242000000000000ull);
  if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 1)) {
    PluginState ns = st;
    float fine = io.KeyShift ? 0.2f : 1.0f;
    ns.params[BbFreq] = static_cast<float>(std::clamp(pui::xToFreq(node.x + io.MouseDelta.x * fine, g0.x, gs.x), 20.0, 500.0));
    ns.params[BbAmount] = static_cast<float>(std::clamp(amt - io.MouseDelta.y * fine * (range * 2 / gs.y) / bassBoostDb(1.0), 0.0, 1.0));
    setPluginState(a, t, ns, "move bass boost", ui.gesture);
  }
  if (ImGui::IsItemDeactivated()) a.doc.endGesture();
  if (ImGui::IsItemHovered()) {
    char buf[96];
    std::snprintf(buf, sizeof buf, "%.0f Hz · +%.1f dB   (drag: frequency / amount · Shift: fine)", hz, bbDb);
    a.setHint(buf);
  }
  skin::paramBox(a, t, BbFreq, F, 5, kGraphH + 5, 160, 45, K, "FREQ");
  sliderBox(a, t, BbAmount, F, 170, kGraphH + 5, 160, 45, K, "AMOUNT");
  {
    static const char* eng[2] = {"OLD", "REF"};
    skin::switchSel(a, t, BbModel, F.p(kW - 75, 9), F.sz(64, 14), K, eng, 2, true);
  }
  F.end(kW, kH);
}

// ===================================================================== Mute
// Reference panel 335 × 55: MUTE and CHANNEL wheel modules; Gridline adds the
// FADE module and a status pad (click to mute) above them.
void editorMute(App& a, const PluginTarget& t, float width) {
  const PluginState st = *t.state(a.doc.song());
  const skin::Skin& K = fruitySkin(0xE5B3BB);
  constexpr float kW = 500, kH = 125;
  skin::Form F = skin::Form::fit(width, kW);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::panel(dl, F.o, F.p(kW, kH), K, 4);
  skin::Scope scope(K);
  bool muted = val(st, MuMute) >= 0.5f;
  int side = std::clamp(static_cast<int>(val(st, MuSide) + 0.5f), 0, 2);
  {
    ImVec2 p0 = F.p(5, 5), p1 = F.p(kW - 5, 70);
    ImGui::SetCursorScreenPos(p0);
    if (ImGui::InvisibleButton("##mute", ImVec2(p1.x - p0.x, p1.y - p0.y))) a.doc.setParam(t.pid(MuMute), muted ? 0 : 1, false);
    bool hov = ImGui::IsItemHovered();
    ImU32 danger = skin::hex(0xE5484D);
    skin::graphWell(dl, p0, p1, K);
    if (muted) dl->AddRectFilled(p0, p1, theme::withAlpha(danger, 0.18f), 2);
    dl->AddRect(p0, p1, muted ? danger : hov ? K.textDim : K.graphGrid, 2, 0, muted ? 2.0f : 1.0f);
    float cy = (p0.y + p1.y) * 0.5f;
    ui::drawIcon(dl, ui::Icon::Mute, ImVec2(p0.x + F.u(30), cy), F.u(22), muted ? danger : K.textDim);
    static const char* sides[3] = {"BOTH SIDES", "LEFT SIDE", "RIGHT SIDE"};
    textCentred(dl, a.fontBold, std::clamp(F.u(18), 16.0f, 30.0f), ImVec2((p0.x + p1.x) * 0.5f, cy - F.u(7)), muted ? skin::lighten(K.text, 0.5f) : K.text,
                muted ? "MUTED" : "OPEN");
    textCentred(dl, a.fontMono, F.font(7.5f), ImVec2((p0.x + p1.x) * 0.5f, cy + F.u(14)), K.textDim, sides[side]);
    if (hov) a.setHint("Click to mute / unmute (automatable, click-free)");
  }
  static const char* sideNames[3] = {"both", "left", "right"};
  skin::paramBox(a, t, MuMute, F, 5, 75, 160, 45, K, "MUTE", muted ? "muted" : "open");
  skin::paramBox(a, t, MuSide, F, 170, 75, 160, 45, K, "CHANNEL", sideNames[side]);
  skin::paramBox(a, t, MuFade, F, 335, 75, 160, 45, K, "FADE");
  F.end(kW, kH);
}

// =============================================================== Send Level
void editorStereoMatrix(App& a, const PluginTarget& t, float width) {
  const PluginState st = *t.state(a.doc.song());
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  // Signal-flow diagram: inputs left, outputs right; line weight = gain, red = inverted.
  ImVec2 p0 = ImGui::GetCursorScreenPos(), ps(width, 120), p1(p0.x + ps.x, p0.y + ps.y);
  ImGui::Dummy(ps);
  dl->AddRectFilled(p0, p1, P.surface950, 8);
  ImVec2 inL(p0.x + 70, p0.y + 34), inR(p0.x + 70, p1.y - 34), outL(p1.x - 70, p0.y + 34), outR(p1.x - 70, p1.y - 34);
  auto wire = [&](ImVec2 from, ImVec2 to, float g) {
    if (std::fabs(g) < 0.005f) return;
    ImU32 col = g < 0 ? P.danger : P.paper;
    dl->AddBezierCubic(from, ImVec2(from.x + 120, from.y), ImVec2(to.x - 120, to.y), to, theme::withAlpha(col, 0.35f + 0.3f * std::min(1.f, std::fabs(g))),
                       1.0f + 2.5f * std::min(2.f, std::fabs(g)));
  };
  wire(inL, outL, val(st, SmLL, 1));
  wire(inR, outL, val(st, SmRL));
  wire(inL, outR, val(st, SmLR));
  wire(inR, outR, val(st, SmRR, 1));
  for (auto [pt, name] : {std::pair<ImVec2, const char*>{inL, "L IN"}, {inR, "R IN"}, {outL, "L OUT"}, {outR, "R OUT"}}) {
    dl->AddCircleFilled(pt, 9, P.surface700, 20);
    dl->AddCircle(pt, 9, P.text, 20, 1.5f);
    bool left = pt.x < (p0.x + p1.x) * 0.5f;
    textAt(dl, a.fontMono, 10.5f, ImVec2(left ? pt.x - 56 : pt.x + 16, pt.y - 7), P.textDim, name);
  }
  double dly = val(st, SmDelay), deph = val(st, SmDephase);
  char info[96];
  std::snprintf(info, sizeof info, "delay %+.1f ms · dephase %+.0f%%", dly, deph * 100);
  textCentred(dl, a.fontMono, 10.5f, ImVec2((p0.x + p1.x) * 0.5f, p1.y - 12), P.textFaint, info);
  ImGui::Dummy(ImVec2(0, 4));
  float mw = (width - 8) * 0.64f, ew = width - 8 - mw;
  pui::beginSection("MATRIX", mw);
  float cell = (mw - 20) / 4;
  pui::knobCell(a, t, SmLL, "L to L", cell);
  ImGui::SameLine(0, 0);
  pui::knobCell(a, t, SmRL, "R to L", cell);
  ImGui::SameLine(0, 0);
  pui::knobCell(a, t, SmLR, "L to R", cell);
  ImGui::SameLine(0, 0);
  pui::knobCell(a, t, SmRR, "R to R", cell);
  pui::endSection();
  ImGui::SameLine(0, 8);
  pui::beginSection("EFFECTS", ew);
  pui::knobCell(a, t, SmDelay, "Delay", (ew - 20) / 2);
  ImGui::SameLine(0, 0);
  pui::knobCell(a, t, SmDephase, "Dephase", (ew - 20) / 2);
  pui::endSection();
}

}  // namespace
// ------------------------------------------------- reference skin helpers

const skin::Skin& fruitySkin(uint32_t fill) {
  static std::map<uint32_t, skin::Skin> cache;
  auto it = cache.find(fill);
  if (it != cache.end()) return it->second;
  skin::Skin s = skin::Skin::fl(0x31373B, 0x3C4346, 0x464E51, fill, 0x3F484D, 0xA2B0B7);
  s.pillBg = skin::hex(0x444D53);
  s.valueBg = skin::hex(0x656F74);
  s.valueText = skin::hex(fill);
  s.rail = skin::hex(0x464E51);
  s.handle = skin::hex(0x30373A);
  s.graphCurve = skin::lighten(skin::hex(fill), 0.2f);
  return cache.emplace(fill, s).first->second;
}

namespace {
// Shared frame of a wheel module: well, wheel on the left, caption pill;
// returns the module geometry (skin::module, Q-7 shared inset).
skin::Module boxFrame(App& a, const PluginTarget& t, int param, const skin::Form& F, float x, float y, float w, float h, const skin::Skin& k,
                      const char* caption, bool& changed) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const skin::Module m = skin::module(F, x, y, w, h);
  skin::well(dl, m.b0, m.b1, k, 4 * F.s);
  changed = skin::wheel(a, t, param, m.wheelTl, m.wheelSize, k, skin::WheelBigLine);
  skin::pill(dl, m.pill0, m.pill1, k, caption, F.font(8.5f));
  return m;
}
}  // namespace

bool sliderBox(App& a, const PluginTarget& t, int param, const skin::Form& F, float x, float y, float w, float h, const skin::Skin& k,
               const char* caption) {
  bool ch = false;
  const skin::Module m = boxFrame(a, t, param, F, x, y, w, h, k, caption, ch);
  ch |= skin::hslider(a, t, param, m.row0, ImVec2(m.row1.x - m.row0.x, m.row1.y - m.row0.y), k);
  return ch;
}

bool menuParamBox(App& a, const PluginTarget& t, int param, const skin::Form& F, float x, float y, float w, float h, const skin::Skin& k,
                  const char* caption) {
  bool ch = false;
  const skin::Module m = boxFrame(a, t, param, F, x, y, w, h, k, caption, ch);
  const float rh = std::max(m.row1.y - m.row0.y, 16.0f * F.s * 0.8f);
  ch |= skin::menuBox(a, t, param, ImVec2(m.row0.x, m.row1.y - rh), ImVec2(m.row1.x - m.row0.x, rh), k, F.font(8.5f));
  return ch;
}

void soberPanel(ImDrawList* dl, const skin::Form& F, float x, float y, float w, float h, const skin::Skin& k, const char* caption) {
  skin::well(dl, F.p(x, y), F.p(x + w, y + h), k, 3 * F.s);
  if (caption && *caption) skin::text(dl, F.p(x + 6, y + 3), k.textDim, caption, F.font(8.0f));
}


bool drawEditor(App& a, const PluginTarget& t, float width) {
  const PluginState* sp = t.state(a.doc.song());
  if (!sp) return false;
  const std::string& type = sp->type;
  if (type.compare(0, 3, "gl.") != 0) return false;
  width = std::max(width, 360.0f);
  if (type == "gl.drawneq") editorCurveEq(a, t, width);
  else if (type == "gl.bankeq") editorBankEq(a, t, width);
  else if (type == "gl.freefilter") editorFreeFilter(a, t, width);
  else if (type == "gl.fastlp") editorQuickLp(a, t, width);
  else if (type == "gl.bassboost") editorBassBoost(a, t, width);
  else if (type == "gl.scope") editorScope(a, t, width);
  else if (type == "gl.spectro") editorSpectro(a, t, width);
  else if (type == "gl.meter") editorMeter(a, t, width);
  else if (type == "gl.clock") editorClock(a, t, width);
  else if (type == "gl.notes") editorNotes(a, t, width);
  else if (type == "gl.mute") editorMute(a, t, width);
  else if (type == "gl.stereomatrix") editorStereoMatrix(a, t, width);
  else return false;
  return true;
}

}  // namespace gl::packs::utility
