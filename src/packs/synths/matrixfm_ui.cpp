// Gridline — "synths" pack: Matrix FM editor.
//
// Reference-form layout (845 × 450 form px): unit tabs along the top (MAIN,
// OP 1-6, F 1-3, FX); the unit panel under them; the articulator target and
// part rows; the curve editor (envelope / LFO / mapping curves, or the
// harmonic editor for operators); the 9 × 9 modulation matrix on the right
// with its FM / RM layer switch; the audition keyboard along the bottom.
// Drawn data edits go through setPluginState (undoable, merged while dragging).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include "../../app/App.h"
#include "../../app/DrawSmoothUi.h"
#include "../../core/PluginBrand.h"
#include "../../app/InstrumentSkin.h"
#include "../../app/Theme.h"
#include "../PacksUi.h"
#include "matrixfm.h"
#include "matrixfm_data.h"
#include "../../app/Widgets.h"

namespace gl::packs::synths {

namespace {

using namespace mfm;
using iskin::rgb;
using iskin::lerp;

constexpr float kW = 845, kH = 450;

iskin::Palette mfmPalette() {
  iskin::Palette p;
  p.bgTop = rgb(0x3A4046);
  p.bgBottom = rgb(0x2C3238);
  p.panel = rgb(0x24282B);
  p.panelLine = rgb(0x171A1C);
  p.rail = rgb(0x4A5157);
  p.fill = rgb(0xF0A43A);   // operators / main values
  p.fill2 = rgb(0x5FC2E8);  // filters
  p.fill3 = rgb(0xB8E05A);  // effects / modulation
  p.cap = rgb(0x5B6268);
  p.handle = rgb(0x1E2326);
  p.label = rgb(0x9AA4AB);
  p.text = rgb(0xDCE3E8);
  p.textDim = rgb(0x8A949B);
  p.lcdBg = rgb(0x15191B);
  p.lcdText = rgb(0xF0A43A);
  p.led = rgb(0xF0A43A);
  p.ledOff = rgb(0x3A4045);
  return p;
}

struct UiState {
  int tab = 1;      // 0 main, 1..6 operators, 7..9 filters, 10 fx
  int target = 1;   // articulator target (operators: kOpArts = harmonics)
  int part = 0;     // 0 envelope, 1 LFO, 2.. mapping curves
  int layer = 0;    // matrix: 0 FM, 1 RM
  int delay = 0;    // FX tab: delay 1..3
  int sel = -1;     // selected curve point
  int harmScroll = 0;
};
std::map<uint64_t, UiState>& uiStates() {
  static std::map<uint64_t, UiState> m;
  return m;
}
UiState& uiState(uint64_t key) {
  auto& m = uiStates();
  auto it = m.find(key);
  if (it != m.end()) return it->second;
  UiState u;
  if (const char* e = std::getenv("GL_MATRIXFM_TAB")) u.tab = std::clamp(std::atoi(e), 0, 10);  // dev: screenshots
  return m[key] = u;
}

int tabUnit(int tab) { return tab == 0 ? kUnits - 1 : tab <= kOps ? tab - 1 : tab <= kOps + kFilters ? tab - 1 : -1; }

uint64_t mkey(const PluginTarget& t, int what) { return (t.key() * 2654435761ull) ^ (0x5F3A0000ull + static_cast<uint64_t>(what)); }

void commit(App& a, const PluginTarget& t, const PluginState& st, const PatchData& d, const char* label, int what) {
  PluginState ns = st;
  ns.data = serializePatch(d);
  if (!(ns == st)) setPluginState(a, t, ns, label, mkey(t, what));
}

void caption(const iskin::Skin& k, float x, float y, float w, const char* s, float size = 9) {
  iskin::text(k, x, y, w, 11, s, size, k.c.textDim, 1, false, true);
}

// ------------------------------------------------------------ curve editor
// Edits `c` in place; returns true when it changed. xLen: visible length
// (envelopes: seconds; LFO: cycle; maps / shaper: 0..1).
bool curveEditor(const iskin::Skin& k, const char* id, float x, float y, float w, float h, Curve& c, bool timeAxis, bool env,
                 double neutralY, int& sel, ImU32 col) {
  ImDrawList* dl = k.dl();
  iskin::bevel(k, x, y, w, h, rgb(0x121517), k.c.panelLine, 3);
  const ImVec2 p0 = k.at(x + 6, y + 6), p1 = k.at(x + w - 6, y + h - 6);
  const float W = p1.x - p0.x, H = p1.y - p0.y;
  // Grid.
  for (int i = 1; i < 4; ++i) {
    const float yy = p0.y + H * i / 4.0f;
    dl->AddLine(ImVec2(p0.x, yy), ImVec2(p1.x, yy), rgb(0x23282B), 1);
  }
  const bool empty = c.pts.empty();
  const double len = empty ? 1.0 : curveLength(c);
  const double view = timeAxis ? std::max(0.05, len * 1.15) : std::max(1.0, len);
  if (timeAxis) {
    // Time ticks every 0.1 / 1 s.
    const double step = view > 4 ? 1.0 : view > 0.8 ? 0.1 : 0.01;
    for (double tt = step; tt < view; tt += step) {
      const float xx = p0.x + static_cast<float>(tt / view) * W;
      dl->AddLine(ImVec2(xx, p0.y), ImVec2(xx, p1.y), rgb(0x1E2326), 1);
    }
  }
  auto sx = [&](double xv) { return p0.x + static_cast<float>(xv / view) * W; };
  auto sy = [&](double yv) { return p1.y - static_cast<float>(std::clamp(yv, 0.0, 1.0)) * H; };
  if (neutralY >= 0) dl->AddLine(ImVec2(p0.x, sy(neutralY)), ImVec2(p1.x, sy(neutralY)), rgb(0x3A4148), 1);
  bool changed = false;
  ImGui::SetCursorScreenPos(p0);
  ImGui::InvisibleButton(id, ImVec2(std::max(1.0f, W), std::max(1.0f, H)));
  const bool hov = ImGui::IsItemHovered();
  const ImVec2 m = ImGui::GetIO().MousePos;
  // Absolute x of each point.
  std::vector<double> ax(c.pts.size(), 0.0);
  for (size_t i = 1; i < c.pts.size(); ++i) ax[i] = ax[i - 1] + c.pts[i].dt;
  int hot = -1;
  for (size_t i = 0; i < c.pts.size(); ++i)
    if (std::fabs(m.x - sx(ax[i])) < 6 && std::fabs(m.y - sy(c.pts[i].y)) < 6) hot = static_cast<int>(i);
  if (empty && hov && ImGui::IsMouseClicked(0)) {
    if (neutralY < 0) c.pts = {{0, 0, 0}, {1, 1, 0}};  // shaper: straight transfer line
    else c.pts = {{0, static_cast<float>(neutralY), 0}, {1, static_cast<float>(neutralY), 0}};
    changed = true;
  }
  if (!empty) {
    if (hov && ImGui::IsMouseClicked(0)) sel = hot;
    if (hov && ImGui::IsMouseDoubleClicked(0) && hot < 0) {
      // Insert a point at the mouse.
      const double xv = std::clamp((m.x - p0.x) / W * view, 0.0, view);
      const float yv = static_cast<float>(std::clamp((p1.y - m.y) / H, 0.0f, 1.0f));
      size_t i = 1;
      while (i < ax.size() && ax[i] < xv) ++i;
      Pt np{static_cast<float>(xv - ax[i - 1]), yv, 0};
      if (i < c.pts.size()) c.pts[i].dt = static_cast<float>(std::max(0.0, ax[i] - xv));
      c.pts.insert(c.pts.begin() + static_cast<long>(i), np);
      const int ii = static_cast<int>(i);
      if (c.sustain >= ii) ++c.sustain;
      if (c.loopStart >= ii) ++c.loopStart;
      if (c.loopEnd >= ii) ++c.loopEnd;
      if (c.decay >= ii) ++c.decay;
      sel = ii;
      changed = true;
    } else if (hov && ImGui::IsMouseClicked(1) && hot > 0 && c.pts.size() > 2) {
      const size_t i = static_cast<size_t>(hot);
      if (i + 1 < c.pts.size()) c.pts[i + 1].dt += c.pts[i].dt;
      c.pts.erase(c.pts.begin() + static_cast<long>(i));
      if (c.sustain == hot) c.sustain = -1;
      else if (c.sustain > hot) --c.sustain;
      if (c.loopStart >= hot) c.loopStart = c.loopStart == hot ? -1 : c.loopStart - 1;
      c.loopEnd = c.loopStart >= 0 ? c.sustain : -1;
      if (c.decay >= hot) c.decay = c.decay == hot ? -1 : c.decay - 1;
      sel = -1;
      changed = true;
    } else if (env && hov && hot > 0 && ImGui::IsMouseClicked(0) && ImGui::GetIO().KeyCtrl) {
      c.sustain = c.sustain == hot ? -1 : hot;  // Ctrl+click: sustain point
      if (c.loopStart >= c.sustain) c.loopStart = -1;
      c.loopEnd = c.loopStart >= 0 ? c.sustain : -1;
      changed = true;
    } else if (env && hov && hot > 0 && ImGui::IsMouseClicked(0) && ImGui::GetIO().KeyAlt) {
      c.decay = c.decay == hot ? -1 : hot;  // Alt+click: decay point (end of the attack part)
      changed = true;
    } else if (env && hov && hot >= 0 && ImGui::IsMouseClicked(0) && ImGui::GetIO().KeyShift && c.sustain > hot) {
      c.loopStart = c.loopStart == hot ? -1 : hot;  // Shift+click: loop start (loops up to the sustain point)
      c.loopEnd = c.loopStart >= 0 ? c.sustain : -1;
      changed = true;
    } else if (ImGui::IsItemActive() && sel >= 0 && sel < static_cast<int>(c.pts.size()) && ImGui::IsMouseDragging(0, 1.0f)) {
      const size_t i = static_cast<size_t>(sel);
      Pt& p = c.pts[i];
      const float yv = std::clamp((p1.y - m.y) / H, 0.0f, 1.0f);
      if (p.y != yv) p.y = yv, changed = true;
      if (i > 0) {
        double xv = std::clamp((m.x - p0.x) / W * view, ax[i - 1], i + 1 < ax.size() ? ax[i + 1] : 1e9);
        if (!timeAxis) xv = std::min(xv, 1.0);
        const float ndt = static_cast<float>(xv - ax[i - 1]);
        if (ndt != p.dt) {
          if (i + 1 < c.pts.size()) c.pts[i + 1].dt = static_cast<float>(std::max(0.0, ax[i + 1] - xv));
          p.dt = ndt;
          changed = true;
        }
      }
    }
    // Mouse wheel over a segment: tension.
    if (hov && hot < 0 && ImGui::GetIO().MouseWheel != 0)
      for (size_t i = 1; i < c.pts.size(); ++i)
        if (m.x >= sx(ax[i - 1]) && m.x < sx(ax[i])) {
          c.pts[i].tension = std::clamp(c.pts[i].tension + ImGui::GetIO().MouseWheel * 0.1f, -1.f, 1.f);
          changed = true;
          break;
        }
  }
  // Recompute after edits and draw.
  ax.assign(c.pts.size(), 0.0);
  for (size_t i = 1; i < c.pts.size(); ++i) ax[i] = ax[i - 1] + c.pts[i].dt;
  if (c.pts.empty()) {
    if (neutralY < 0) dl->AddLine(ImVec2(p0.x, p1.y), ImVec2(p1.x, p0.y), lerp(col, rgb(0x121517), 0.5f), 2);
    else dl->AddLine(ImVec2(p0.x, sy(neutralY)), ImVec2(p1.x, sy(neutralY)), lerp(col, rgb(0x121517), 0.5f), 2);
    iskin::text(k, x, y + h - 20, w, 14, "neutral — click to draw", 9, k.c.textDim, 1);
    return changed;
  }
  if (env && c.loopStart >= 0 && c.sustain > c.loopStart)
    dl->AddRectFilled(ImVec2(sx(ax[static_cast<size_t>(c.loopStart)]), p0.y), ImVec2(sx(ax[static_cast<size_t>(c.sustain)]), p1.y),
                      theme::withAlpha(col, 0.10f));
  // Filled area under the curve.
  std::vector<ImVec2> line;
  const int steps = 240;
  const double total = ax.back();
  for (int i = 0; i <= steps; ++i) {
    const double xv = (timeAxis ? view : std::max(total, 1e-9)) * i / steps;
    if (xv > total + 1e-9 && i > 0) {
      line.push_back(ImVec2(sx(xv), sy(c.pts.back().y)));
      continue;
    }
    line.push_back(ImVec2(sx(xv), sy(curveAt(c, xv))));
  }
  for (size_t i = 1; i < line.size(); ++i)
    dl->AddQuadFilled(ImVec2(line[i - 1].x, p1.y), line[i - 1], line[i], ImVec2(line[i].x, p1.y), theme::withAlpha(col, 0.13f));
  dl->AddPolyline(line.data(), static_cast<int>(line.size()), col, 0, k.px(1.6f));
  for (size_t i = 0; i < c.pts.size(); ++i) {
    const ImVec2 q(sx(ax[i]), sy(c.pts[i].y));
    const bool s = static_cast<int>(i) == sel, hv = static_cast<int>(i) == hot;
    dl->AddCircleFilled(q, s || hv ? 5.0f : 3.6f, s ? IM_COL32_WHITE : col);
    if (env && static_cast<int>(i) == c.sustain) {
      dl->AddLine(ImVec2(q.x, p0.y), ImVec2(q.x, p1.y), theme::withAlpha(col, 0.6f), 1);
      dl->AddText(ImVec2(q.x + 3, p0.y), col, "S");
    }
    if (env && static_cast<int>(i) == c.loopStart) dl->AddText(ImVec2(q.x + 3, p0.y), col, "L");
    if (env && static_cast<int>(i) == c.decay) dl->AddText(ImVec2(q.x + 3, p0.y + 12), theme::withAlpha(col, 0.8f), "D");
  }
  if (hov && hot >= 0) {
    char b[64];
    std::snprintf(b, sizeof b, timeAxis ? "%.3f s  %.2f" : "%.2f  %.2f", ax[static_cast<size_t>(hot)], static_cast<double>(c.pts[static_cast<size_t>(hot)].y));
    ImGui::SetTooltip("%s", b);
  }
  return changed;
}

// ------------------------------------------------------------ harmonics
bool harmonicEditor(App& a, const iskin::Skin& k, float x, float y, float w, float h, PatchData& d, int opk, int& scroll) {
  bool changed = false;
  const int shown = 64;
  scroll = std::clamp(scroll, 0, kHarmonics - shown);
  auto& lv = d.level[static_cast<size_t>(opk)];
  auto& ph = d.phase[static_cast<size_t>(opk)];
  ImDrawList* dl = k.dl();
  const float hl = h * 0.62f, hp = h - hl - 18;
  for (int band = 0; band < 2; ++band) {
    const float by = band == 0 ? y : y + hl + 4, bh = band == 0 ? hl : hp;
    iskin::bevel(k, x, by, w, bh, rgb(0x121517), k.c.panelLine, 3);
    const ImVec2 a0 = k.at(x + 4, by + 4), a1 = k.at(x + w - 4, by + bh - 4);
    const float cw = (a1.x - a0.x) / shown;
    ImGui::SetCursorScreenPos(a0);
    ImGui::InvisibleButton(band == 0 ? "##hlv" : "##hph", ImVec2(a1.x - a0.x, a1.y - a0.y));
    const bool reset = ImGui::GetIO().KeyCtrl || ImGui::IsMouseDown(1);
    const ImVec2 m = ImGui::GetIO().MousePos;
    float* row = band == 0 ? &lv[static_cast<size_t>(scroll)] : &ph[static_cast<size_t>(scroll)];
    // J-99: strokes fill every harmonic between samples. Levels are smoothed
    // (Alt: raw); phases wrap around (0 = 1), so they are filled raw.
    drawui::Spec spec;
    spec.surface = "lattice";
    spec.p0 = a0;
    spec.p1 = a1;
    spec.raw = band == 1;
    if (const std::vector<float>* res = drawui::stroke(a, ImGui::GetItemID(), ImGui::IsItemActive() && !reset, (m.x - a0.x) / (a1.x - a0.x),
                                                       std::clamp((a1.y - m.y) / (a1.y - a0.y), 0.0f, 1.0f), row, shown, spec)) {
      for (int i = 0; i < shown; ++i)
        if (row[i] != (*res)[static_cast<size_t>(i)]) row[i] = (*res)[static_cast<size_t>(i)], changed = true;
    }
    if (ImGui::IsItemActive() && reset) {
      const int hi = scroll + std::clamp(static_cast<int>((m.x - a0.x) / cw), 0, shown - 1);
      const float v = band == 0 ? (hi == 0 ? 1.f : 0.f) : 0.f;
      float& dst = band == 0 ? lv[static_cast<size_t>(hi)] : ph[static_cast<size_t>(hi)];
      if (dst != v) dst = v, changed = true;
    }
    if (ImGui::IsItemDeactivated()) a.doc.endGesture();
    for (int i = 0; i < shown; ++i) {
      const int hi = scroll + i;
      const float v = band == 0 ? lv[static_cast<size_t>(hi)] : ph[static_cast<size_t>(hi)];
      const float bx = a0.x + i * cw;
      const ImU32 c = band == 0 ? k.c.fill : k.c.fill2;
      dl->AddRectFilled(ImVec2(bx + 1, a1.y - v * (a1.y - a0.y)), ImVec2(bx + cw - 1, a1.y), (hi % 2 == 0) ? c : lerp(c, rgb(0x121517), 0.25f));
    }
    caption(k, x, by + bh - 1, 60, band == 0 ? "LEVELS" : "PHASES", 8);
    if (band == 0) drawui::chip(a, "lattice", a0, a1);
  }
  // Scroll + harmonic numbers.
  char b[48];
  std::snprintf(b, sizeof b, "HARMONICS %d-%d", scroll + 1, scroll + shown);
  iskin::text(k, x + 80, y + h - 14, 200, 14, b, 9, k.c.textDim, 0);
  if (iskin::button(k, "##hl", x + w - 70, y + h - 15, 32, 14, GL_ICON_LABEL("chevron-left", "<"), false, 9)) scroll -= 32;
  if (iskin::button(k, "##hr", x + w - 34, y + h - 15, 32, 14, GL_ICON_LABEL("chevron-right", ">"), false, 9)) scroll += 32;
  if (iskin::button(k, "##hsine", x + w - 140, y + h - 15, 64, 14, "SINE", false, 9)) {
    lv.fill(0.f), ph.fill(0.f), lv[0] = 1.f;
    changed = true;
  }
  scroll = std::clamp(scroll, 0, kHarmonics - shown);
  return changed;
}

// Waveform preview of an operator (harmonics + shape blend).
void opScope(const iskin::Skin& k, const PluginState& st, const PatchData& d, int opk, float x, float y, float w, float h) {
  iskin::bevel(k, x, y, w, h, rgb(0x121517), k.c.panelLine, 3);
  const auto& lv = d.level[static_cast<size_t>(opk)];
  const auto& ph = d.phase[static_cast<size_t>(opk)];
  auto P = [&](int k) { return st.params[static_cast<size_t>(op(opk, k))]; };
  const float shape = P(OpShape), tension = P(OpTension), skew = P(OpSkew), sine = P(OpSine);
  const bool half = P(OpHalf) >= 0.5f, absv = P(OpAbs) >= 0.5f;
  ImVec2 pts[96];
  float peak = 1e-6f;
  float v[96];
  for (int i = 0; i < 96; ++i) {
    // J-68: the operator's phase warps and wave shaping in the engine's order
    // (skew, tension, harmonic table, shape blend, sine shaper, half / abs).
    double p = i / 95.0;
    if (skew > 0) {
      const double m = 0.5 + 0.48 * skew;
      p = p < m ? 0.5 * p / m : 0.5 + 0.5 * (p - m) / (1 - m);
    }
    if (tension != 0) p = std::pow(p, std::exp2(tension * 2.0));
    double s = 0;
    for (int hh = 0; hh < 48; ++hh)
      if (lv[static_cast<size_t>(hh)] != 0) s += lv[static_cast<size_t>(hh)] * std::sin(6.283185307 * ((hh + 1) * p + ph[static_cast<size_t>(hh)]));
    const double saw = 2 * p - 1, sq = p < 0.5 ? 1 : -1;
    const double s1 = std::min(1.0f, shape), s2 = std::max(0.0f, shape - 1);
    s = s + (saw - s) * s1;
    s = s + (sq - s) * s2;
    if (sine > 0) s = s + (std::sin(s * 3.14159265358979 * 0.5 * (1.0 + 3.0 * sine)) - s) * sine;
    if (half && p >= 0.5) s = 0;
    if (absv) s = std::fabs(s) * 2.0 - 1.0;
    v[i] = static_cast<float>(s);
    peak = std::max(peak, std::fabs(v[i]));
  }
  const ImVec2 a0 = k.at(x + 3, y + 3), a1 = k.at(x + w - 3, y + h - 3);
  for (int i = 0; i < 96; ++i)
    pts[i] = ImVec2(a0.x + (a1.x - a0.x) * i / 95.0f, (a0.y + a1.y) * 0.5f - v[i] / std::max(1.0f, peak) * (a1.y - a0.y) * 0.45f);
  k.dl()->AddLine(ImVec2(a0.x, (a0.y + a1.y) * 0.5f), ImVec2(a1.x, (a0.y + a1.y) * 0.5f), rgb(0x2A3034), 1);
  k.dl()->AddPolyline(pts, 96, k.c.fill, 0, k.px(1.4f));
}

// ------------------------------------------------------------ unit panels
void wheelCap(App& a, const PluginTarget& t, const iskin::Skin& k, int p, float x, float y, float s, ImU32 col, const char* cap,
              bool centered = false) {
  iskin::wheel(a, t, k, p, x, y, s, col, centered ? iskin::WheelCentered : 0);
  caption(k, x - 8, y + s, s + 16, cap);
}
void sliderCap(App& a, const PluginTarget& t, const iskin::Skin& k, int p, float x, float y, float w, float h, ImU32 col, const char* cap,
               bool centered = false) {
  iskin::slider(a, t, k, p, x, y, w, h, col, centered);
  caption(k, x - 6, y + h + 1, w + 12, cap, 8);
}

void opPanel(App& a, const PluginTarget& t, const iskin::Skin& k, const PluginState& st, const PatchData& d, int o) {
  const float X = 5, Y = 31;
  opScope(k, st, d, o, X + 4, Y + 4, 80, 87);
  iskin::bevel(k, X + 89, Y + 3, 159, 89, k.c.panel, k.c.panelLine, 3);
  sliderCap(a, t, k, op(o, OpShape), X + 96, Y + 8, 16, 62, k.c.fill, "SH");
  sliderCap(a, t, k, op(o, OpTension), X + 120, Y + 8, 16, 62, k.c.fill, "TN", true);
  sliderCap(a, t, k, op(o, OpSkew), X + 144, Y + 8, 16, 62, k.c.fill, "SK");
  sliderCap(a, t, k, op(o, OpSine), X + 168, Y + 8, 16, 62, k.c.fill, "SN");
  sliderCap(a, t, k, op(o, OpFilter), X + 192, Y + 8, 16, 62, k.c.fill, "FL");
  sliderCap(a, t, k, op(o, OpNoise), X + 216, Y + 8, 16, 62, k.c.fill, "NS");
  iskin::check(a, t, k, op(o, OpHalf), X + 252, Y + 6, 62, 14, "HALF", 9);
  iskin::check(a, t, k, op(o, OpEven), X + 252, Y + 22, 62, 14, "EVEN", 9);
  iskin::check(a, t, k, op(o, OpAbs), X + 252, Y + 38, 62, 14, "ABS", 9);
  iskin::check(a, t, k, op(o, OpPluck), X + 252, Y + 54, 62, 14, "PLUCK", 9);
  iskin::check(a, t, k, op(o, OpCenter), X + 252, Y + 70, 62, 14, "CENTER", 9);
  // Sub panel: ratio / offset, phase, pitch range, volume, options.
  const float S = X + 320;
  caption(k, S, Y + 2, 70, "RATIO");
  iskin::digi(a, t, k, op(o, OpRatio), S + 2, Y + 14, 66, 18, [](double v) {
    char b[24];
    std::snprintf(b, sizeof b, "%.4f", v);
    return std::string(b);
  });
  caption(k, S, Y + 34, 70, "OFFSET Hz");
  iskin::digi(a, t, k, op(o, OpFreq), S + 2, Y + 46, 66, 18, [](double v) {
    char b[24];
    std::snprintf(b, sizeof b, "%+.2f", v);
    return std::string(b);
  });
  wheelCap(a, t, k, op(o, OpPhase), S + 76, Y + 10, 34, k.c.fill, "PHASE");
  wheelCap(a, t, k, op(o, OpPitchRange), S + 118, Y + 10, 34, k.c.fill, "PE");
  wheelCap(a, t, k, op(o, OpVol), S + 162, Y + 4, 44, k.c.fill, "VOL", true);
  iskin::check(a, t, k, op(o, OpGlobal), S + 2, Y + 68, 64, 12, "GLOBAL", 8);
  iskin::check(a, t, k, op(o, OpBandLimit), S + 2, Y + 81, 72, 12, "BAND LIMIT", 8);
  iskin::check(a, t, k, op(o, OpDeclick), S + 78, Y + 68, 64, 12, "DECLICK", 8);
}

void filterPanel(App& a, const PluginTarget& t, const iskin::Skin& k, const PluginState& st, int j) {
  const float X = 5, Y = 31;
  iskin::bevel(k, X + 3, Y + 3, 120, 89, k.c.panel, k.c.panelLine, 3);
  caption(k, X + 6, Y + 6, 64, "TYPE");
  iskin::digi(a, t, k, flt(j, FlType), X + 8, Y + 20, 60, 20, [](double v) {
    return std::string(kFltTypeNames[std::clamp(static_cast<int>(std::lround(v)), 0, kFltTypes - 1)]);
  });
  iskin::check(a, t, k, flt(j, FlHq), X + 8, Y + 50, 60, 14, "HQ", 9);
  iskin::check(a, t, k, flt(j, FlFlat), X + 8, Y + 68, 60, 14, "FLAT", 9);
  // Mode selector (stacked).
  const int mode = static_cast<int>(std::lround(st.params[static_cast<size_t>(flt(j, FlMode))]));
  for (int m = 0; m < kFltModes; ++m) {
    char id[16];
    std::snprintf(id, sizeof id, "##fm%d", m);
    if (iskin::button(k, id, X + 76, Y + 6 + m * 14, 42, 13, kFltModeNames[m], m == mode, 8, k.c.fill2)) {
      PluginState ns = st;
      ns.params[static_cast<size_t>(flt(j, FlMode))] = static_cast<float>(m);
      setPluginState(a, t, ns, "Filter mode");
    }
  }
  wheelCap(a, t, k, flt(j, FlEnv), X + 132, Y + 10, 38, k.c.fill2, "ENV");
  wheelCap(a, t, k, flt(j, FlCut), X + 178, Y + 4, 50, k.c.fill2, "CUT");
  wheelCap(a, t, k, flt(j, FlRes), X + 236, Y + 10, 38, k.c.fill2, "RES");
  sliderCap(a, t, k, flt(j, FlLow), X + 284, Y + 8, 16, 62, k.c.fill2, "LOW");
  sliderCap(a, t, k, flt(j, FlBand), X + 308, Y + 8, 16, 62, k.c.fill2, "BAND");
  sliderCap(a, t, k, flt(j, FlHigh), X + 332, Y + 8, 16, 62, k.c.fill2, "HIGH");
  // Waveshaper + to next.
  const float S = X + 360;
  iskin::bevel(k, S + 4, Y + 3, 119, 89, k.c.panel, k.c.panelLine, 3);
  caption(k, S + 6, Y + 5, 60, "SHAPER");
  iskin::check(a, t, k, flt(j, FlWsOn), S + 10, Y + 20, 44, 14, "ON", 9);
  iskin::check(a, t, k, flt(j, FlWsBipolar), S + 10, Y + 38, 50, 14, "BIPOLAR", 8);
  wheelCap(a, t, k, flt(j, FlWsAmp), S + 64, Y + 8, 26, k.c.fill2, "PRE");
  wheelCap(a, t, k, flt(j, FlWsMix), S + 92, Y + 8, 26, k.c.fill2, "MIX");
  wheelCap(a, t, k, flt(j, FlNext), S + 132, Y + 14, 34, k.c.fill2, j < kFilters - 1 ? "NEXT" : "NEXT (-)", true);
}

void fxPanel(App& a, const PluginTarget& t, const iskin::Skin& k, UiState& u) {
  const float X = 5, Y = 31;
  wheelCap(a, t, k, FxPan, X + 6, Y + 26, 30, k.c.fill3, "PAN", true);
  iskin::bevel(k, X + 42, Y + 3, 171, 89, k.c.panel, k.c.panelLine, 3);
  caption(k, X + 44, Y + 4, 50, "CHORUS");
  iskin::digi(a, t, k, ChOrder, X + 48, Y + 18, 26, 18);
  caption(k, X + 44, Y + 38, 34, "ORD", 8);
  const int chP[6] = {ChDepth, ChSpeed, ChDelay, ChSpread, ChCross, ChVol};
  const char* chN[6] = {"DEP", "SPD", "DEL", "SPR", "CRS", "VOL"};
  for (int i = 0; i < 6; ++i) sliderCap(a, t, k, chP[i], X + 84 + i * 21, Y + 8, 14, 66, k.c.fill3, chN[i], chP[i] == ChCross || chP[i] == ChVol);
  // Reverb (between chorus and delay).
  iskin::bevel(k, X + 216, Y + 3, 70, 89, k.c.panel, k.c.panelLine, 3);
  caption(k, X + 218, Y + 4, 66, "REVERB");
  iskin::check(a, t, k, RvOn, X + 222, Y + 17, 40, 12, "ON", 8);
  wheelCap(a, t, k, RvVol, X + 220, Y + 34, 26, k.c.fill3, "WET", true);
  wheelCap(a, t, k, RvDecay, X + 252, Y + 34, 26, k.c.fill3, "DEC");
  // Delays.
  for (int d = 0; d < 3; ++d) {
    char id[16], lb[4];
    std::snprintf(id, sizeof id, "##dn%d", d);
    std::snprintf(lb, sizeof lb, "%d", d + 1);
    if (iskin::button(k, id, X + 289, Y + 9 + d * 25, 19, 23, lb, u.delay == d, 10, k.c.fill3)) u.delay = d;
  }
  iskin::bevel(k, X + 310, Y + 3, 219, 89, k.c.panel, k.c.panelLine, 3);
  const int D = u.delay;
  char cap[32];
  std::snprintf(cap, sizeof cap, "DELAY %d", D + 1);
  caption(k, X + 312, Y + 4, 60, cap);
  iskin::check(a, t, k, dly(D, 0), X + 316, Y + 18, 40, 12, "ON", 8);
  iskin::check(a, t, k, dly(D, 6), X + 316, Y + 34, 52, 12, "TEMPO", 8);
  iskin::check(a, t, k, DlSerial, X + 316, Y + 50, 56, 12, "SERIAL", 8);
  const int dp[4] = {dly(D, 2), dly(D, 3), dly(D, 4), dly(D, 5)};
  const char* dn[4] = {"FB", "TIME", "OFS", "VOL"};
  for (int i = 0; i < 4; ++i) sliderCap(a, t, k, dp[i], X + 378 + i * 22, Y + 8, 14, 66, k.c.fill3, dn[i], i >= 2);
  static const char* modes[3] = {"NORM", "INV", "P-P"};
  iskin::selector(a, t, k, dly(D, 1), X + 470, Y + 8, 52, 20, [&](ImDrawList*, ImVec2, ImVec2, int i) {
    iskin::text(k, X + 470, Y + 8, 52, 20, modes[std::clamp(i, 0, 2)], 9, k.c.text, 1, false, true);
  });
}

void mainPanel(App& a, const PluginTarget& t, const iskin::Skin& k, const PluginState& st) {
  const float X = 5, Y = 31;
  iskin::bevel(k, X + 5, Y + 4, 129, 98, k.c.panel, k.c.panelLine, 3);
  caption(k, X + 5, Y + 5, 129, "MAIN");
  sliderCap(a, t, k, MVolume, X + 20, Y + 18, 18, 64, k.c.fill, "VOL");
  sliderCap(a, t, k, MLfoLevel, X + 60, Y + 18, 18, 64, k.c.fill, "LFO", true);
  sliderCap(a, t, k, MPitch, X + 100, Y + 18, 18, 64, k.c.fill, "PITCH", true);
  iskin::bevel(k, X + 140, Y + 4, 98, 98, k.c.panel, k.c.panelLine, 3);
  caption(k, X + 140, Y + 5, 98, "VOL");
  const int va[4] = {MVolA, MVolD, MVolS, MVolR}, fa[4] = {MFltA, MFltD, MFltS, MFltR};
  const char* adsr[4] = {"A", "D", "S", "R"};
  for (int i = 0; i < 4; ++i) sliderCap(a, t, k, va[i], X + 148 + i * 22, Y + 18, 14, 64, k.c.fill, adsr[i], i == 2);
  iskin::bevel(k, X + 244, Y + 4, 98, 98, k.c.panel, k.c.panelLine, 3);
  caption(k, X + 244, Y + 5, 98, "FILTER");
  for (int i = 0; i < 4; ++i) sliderCap(a, t, k, fa[i], X + 252 + i * 22, Y + 18, 14, 64, k.c.fill2, adsr[i], i == 2);
  iskin::bevel(k, X + 348, Y + 4, 180, 98, k.c.panel, k.c.panelLine, 3);
  caption(k, X + 348, Y + 5, 180, "UNISON");
  iskin::digi(a, t, k, MUniOrder, X + 354, Y + 22, 24, 18);
  caption(k, X + 350, Y + 42, 32, "ORD", 8);
  const int up[6] = {MUniPan, MUniVol, MUniPitch, MUniSub, MUniPhase, MUniEnv};
  const char* un[6] = {"PN", "VL", "PT", "SB", "PH", "EN"};
  for (int i = 0; i < 6; ++i) sliderCap(a, t, k, up[i], X + 388 + i * 22, Y + 18, 14, 64, k.c.fill3, un[i]);
  // Second row: modulation X / Y, voice, EQ, options.
  iskin::bevel(k, X + 5, Y + 108, 129, 92, k.c.panel, k.c.panelLine, 3);
  caption(k, X + 5, Y + 109, 129, "MODULATION");
  wheelCap(a, t, k, MModX, X + 18, Y + 126, 40, k.c.fill3, "X");
  wheelCap(a, t, k, MModY, X + 80, Y + 126, 40, k.c.fill3, "Y");
  iskin::check(a, t, k, MSmooth, X + 18, Y + 182, 70, 12, "SMOOTH", 8);
  iskin::bevel(k, X + 140, Y + 108, 98, 92, k.c.panel, k.c.panelLine, 3);
  caption(k, X + 140, Y + 109, 98, "VOICE");
  iskin::digi(a, t, k, MVoices, X + 148, Y + 124, 30, 16);
  caption(k, X + 144, Y + 141, 38, "POLY", 8);
  wheelCap(a, t, k, MPortaTime, X + 196, Y + 122, 30, k.c.fill, "TIME");
  iskin::check(a, t, k, MPorta, X + 146, Y + 160, 64, 12, "PORTA", 8);
  iskin::check(a, t, k, MMonoKey, X + 146, Y + 175, 80, 12, "MONO KEY", 8);
  iskin::bevel(k, X + 244, Y + 108, 182, 92, k.c.panel, k.c.panelLine, 3);
  caption(k, X + 244, Y + 109, 182, "EQ");
  static const char* tg[4] = {"OFF", "ALL", "OUT", "FX"};
  const int target = static_cast<int>(std::lround(st.params[static_cast<size_t>(MEqTarget)]));
  for (int i = 0; i < 4; ++i) {
    char id[16];
    std::snprintf(id, sizeof id, "##eqt%d", i);
    if (iskin::button(k, id, X + 250, Y + 122 + i * 18, 32, 16, tg[i], i == target, 8)) {
      PluginState ns = st;
      ns.params[static_cast<size_t>(MEqTarget)] = static_cast<float>(i);
      setPluginState(a, t, ns, "EQ target");
    }
  }
  for (int b = 0; b < 3; ++b) {
    const float bx = X + 290 + b * 45;
    iskin::wheel(a, t, k, eq(b, 1), bx + 2, Y + 120, 22, k.c.fill2);
    iskin::wheel(a, t, k, eq(b, 2), bx + 2, Y + 146, 18, k.c.fill2);
    iskin::slider(a, t, k, eq(b, 3), bx + 28, Y + 118, 10, 66, k.c.fill2, true);
  }
  caption(k, X + 286, Y + 186, 140, "FREQ / BW / LEVEL", 8);
  iskin::bevel(k, X + 432, Y + 108, 96, 92, k.c.panel, k.c.panelLine, 3);
  caption(k, X + 432, Y + 109, 96, "OPTIONS");
  iskin::check(a, t, k, MRandom, X + 438, Y + 124, 80, 12, "RANDOM", 8);
  iskin::check(a, t, k, MGlobalPitch, X + 438, Y + 139, 88, 12, "GLOBAL PITCH", 8);
  iskin::check(a, t, k, MCenter, X + 438, Y + 154, 80, 12, "CENTER", 8);
  iskin::check(a, t, k, MGibbs, X + 438, Y + 169, 80, 12, "GIBBS OFF", 8);
  wheelCap(a, t, k, MSoften, X + 498, Y + 154, 22, k.c.fill, "SOFT");
}

// ------------------------------------------------------------ articulator panel
void artPanel(App& a, const PluginTarget& t, const iskin::Skin& k, const PluginState& st, PatchData& d, UiState& u, float Y, float H) {
  const int unit = tabUnit(u.tab);
  if (unit < 0) return;
  const int nArts = unitArts(unit);
  const bool isOp = unit < kOps;
  const int nTargets = nArts + (isOp ? 1 : 0);
  u.target = std::clamp(u.target, 0, nTargets - 1);
  // Target row.
  const float tw = 533.0f / nTargets;
  for (int i = 0; i < nTargets; ++i) {
    const char* nm = i == nArts ? "HARMONICS" : isOp ? kOpArtNames[i] : unit < kOps + kFilters ? kFltArtNames[i] : kMainArtNames[i];
    char id[16];
    std::snprintf(id, sizeof id, "##tg%d", i);
    const bool on = i < nArts && st.params[static_cast<size_t>(art(unit, i, AfEnvOn))] >= 0.5f;
    const bool lon = i < nArts && st.params[static_cast<size_t>(art(unit, i, AfLfoOn))] >= 0.5f;
    if (iskin::button(k, id, 5 + i * tw, Y, tw - 2, 16, nm, u.target == i, 9)) u.target = i, u.sel = -1;
    if (on || lon) k.dl()->AddCircleFilled(k.at(5 + i * tw + 6, Y + 8), k.px(2.5f), on ? k.c.fill : k.c.fill3);
  }
  const float EY = Y + 18;
  if (isOp && u.target == nArts) {
    if (harmonicEditor(a, k, 5, EY, 533, H - 18, d, unit, u.harmScroll)) commit(a, t, st, d, "Edit harmonics", 1);
    return;
  }
  // Part row.
  static const char* parts[2 + kMaps] = {"ENV", "LFO", "KEY", "VEL", "X", "Y", "RAND", "UNI"};
  for (int i = 0; i < 2 + kMaps; ++i) {
    char id[16];
    std::snprintf(id, sizeof id, "##pt%d", i);
    if (iskin::button(k, id, 5 + i * 50, EY, 48, 15, parts[i], u.part == i, 9, k.c.fill3)) u.part = i, u.sel = -1;
  }
  ArtData& ad = d.arts[static_cast<size_t>(unit)][static_cast<size_t>(u.target)];
  const bool bip = artBipolar(unit, u.target);
  const float CY = EY + 18, CH = H - 36;
  const bool compact = CH < 140;
  const float CW = compact ? 300 : 419;
  bool changed = false;
  if (u.part == 0) changed = curveEditor(k, "##env", 5, CY, CW, CH, ad.env, true, true, -1, u.sel, k.c.fill);
  else if (u.part == 1) changed = curveEditor(k, "##lfo", 5, CY, CW, CH, ad.lfo, false, false, 0.5, u.sel, k.c.fill3);
  else {
    Curve& c = ad.maps[static_cast<size_t>(u.part - 2)];
    changed = curveEditor(k, "##map", 5, CY, CW, CH, c, false, false, bip ? 0.5 : 1.0, u.sel, k.c.fill2);
    if (!c.empty() && iskin::button(k, "##mreset", 5 + CW - 52, CY + 4, 48, 14, "RESET", false, 8)) {
      c.pts.clear();
      changed = true;
    }
  }
  if (u.part == 0 && iskin::button(k, "##ereset", 5 + CW - 52, CY + 4, 48, 14, "RESET", false, 8)) {
    ad.env = defaultEnvelope();
    changed = true;
  }
  if (u.part == 1 && iskin::button(k, "##lreset", 5 + CW - 52, CY + 4, 48, 14, "RESET", false, 8)) {
    ad.lfo = defaultLfo();
    changed = true;
  }
  if (changed) commit(a, t, st, d, "Edit articulator curve", 100 + unit * 16 + u.target);
  // Side controls.
  const float SX = 5 + CW + 4, SW = 533 - CW - 4;
  iskin::bevel(k, SX, CY, SW, CH, k.c.panel, k.c.panelLine, 3);
  const int tgt = u.target;
  const int sc[4] = {AfAtt, AfDec, AfSus, AfRel};
  const char* sn[4] = {"A", "D", "S", "R"};
  if (compact) {
    // Compact (main tab): checks on top, one row of wheels.
    iskin::check(a, t, k, art(unit, tgt, AfEnvOn), SX + 6, CY + 5, 50, 12, "ENV", 8);
    iskin::check(a, t, k, art(unit, tgt, AfLfoOn), SX + 60, CY + 5, 50, 12, "LFO", 8);
    iskin::check(a, t, k, art(unit, tgt, AfTempo), SX + 114, CY + 5, 54, 12, "TEMPO", 8);
    iskin::check(a, t, k, art(unit, tgt, AfLfoGlobal), SX + 168, CY + 5, 58, 12, "GLOBAL", 8);
    const int wp[8] = {AfAmount, AfLfoAmt, AfLfoSpeed, AfLfoSkew, AfAtt, AfDec, AfSus, AfRel};
    const char* wn[8] = {"AMT", "LFO", "SPD", "SKW", "A", "D", "S", "R"};
    for (int i = 0; i < 8; ++i)
      wheelCap(a, t, k, art(unit, tgt, wp[i]), SX + 8 + i * 27.5f, CY + 26, 22, i == 0 || i >= 4 ? k.c.fill : k.c.fill3, wn[i],
               i == 0 || i == 3 || i == 6);
  } else {
    iskin::check(a, t, k, art(unit, tgt, AfEnvOn), SX + 6, CY + 5, 50, 12, "ENV", 8);
    iskin::check(a, t, k, art(unit, tgt, AfLfoOn), SX + 58, CY + 5, 50, 12, "LFO", 8);
    wheelCap(a, t, k, art(unit, tgt, AfAmount), SX + 8, CY + 22, 26, k.c.fill, "AMT", true);
    wheelCap(a, t, k, art(unit, tgt, AfLfoAmt), SX + 42, CY + 22, 26, k.c.fill3, "LFO");
    wheelCap(a, t, k, art(unit, tgt, AfLfoSpeed), SX + 76, CY + 22, 26, k.c.fill3, "SPD");
    for (int i = 0; i < 4; ++i)
      sliderCap(a, t, k, art(unit, tgt, sc[i]), SX + 8 + i * 25, CY + 70, 12, std::max(30.f, CH - 118), k.c.fill, sn[i], i == 2);
    const float by = CY + CH - 32;
    wheelCap(a, t, k, art(unit, tgt, AfLfoSkew), SX + 8, by - 6, 20, k.c.fill3, "SKW", true);
    iskin::check(a, t, k, art(unit, tgt, AfTempo), SX + 36, by - 4, 70, 12, "TEMPO", 8);
    iskin::check(a, t, k, art(unit, tgt, AfLfoGlobal), SX + 36, by + 10, 70, 12, "GLOBAL", 8);
  }
  if (u.part == 0) caption(k, 5, CY + CH + 1, CW, "double-click: add · right-click: delete · ctrl: sustain · shift: loop · alt: decay · wheel: tension", 7.5f);
}

// ------------------------------------------------------------ matrix
void matrixPanel(App& a, const PluginTarget& t, const iskin::Skin& k, UiState& u) {
  const float X = 543, Y = 29, W = 298, H = 298;
  iskin::bevel(k, X, Y, W, H + 18, rgb(0x23292E), k.c.panelLine, 3);
  const float lab = 28, cell = (W - lab - 4) / kCols;
  static const char* cols[kCols] = {"1", "2", "3", "4", "5", "6", "PAN", "FX", "OUT"};
  static const char* rows[kRows] = {"OP1", "OP2", "OP3", "OP4", "OP5", "OP6", "F1", "F2", "F3"};
  for (int c = 0; c < kCols; ++c) caption(k, X + lab + c * cell, Y + 4, cell, cols[c], 8);
  const float rh = (H - 22) / kRows;
  for (int r = 0; r < kRows; ++r) {
    const float ry = Y + 18 + r * rh;
    const bool sel = tabUnit(u.tab) == r;
    iskin::text(k, X + 2, ry, lab - 2, rh, rows[r], 8.5f, sel ? k.c.fill : k.c.textDim, 1, false, true);
    if (r % 2 == 0) k.dl()->AddRectFilled(k.at(X + lab, ry), k.at(X + W - 3, ry + rh), IM_COL32(255, 255, 255, 8));
    for (int c = 0; c < kCols; ++c) {
      const bool ring = u.layer == 1;
      if (ring && (r >= kOps || c >= kOps)) continue;
      const int p = ring ? rm(r, c) : fm(r, c);
      const ImU32 col = c >= kOps ? k.c.fill3 : r >= kOps ? k.c.fill2 : ring ? rgb(0xE85F8A) : k.c.fill;
      const float s = std::min(cell, rh) - 5;
      iskin::wheel(a, t, k, p, X + lab + c * cell + (cell - s) * 0.5f, ry + (rh - s) * 0.5f, s, col, iskin::WheelCentered);
    }
  }
  if (iskin::button(k, "##lfm", X, Y + H, 44, 16, "FM", u.layer == 0, 9)) u.layer = 0;
  if (iskin::button(k, "##lrm", X + 46, Y + H, 44, 16, "RM", u.layer == 1, 9, rgb(0xE85F8A))) u.layer = 1;
  caption(k, X + 96, Y + H + 3, W - 100, "row = destination · column = source", 7.5f);
}

}  // namespace

void drawMatrixFmEditor(App& a, const PluginTarget& t, float width) {
  const PluginState* sp = t.state(a.doc.song());
  if (!sp || static_cast<int>(sp->params.size()) < kParamCount) return;
  const PluginState& st = *sp;
  UiState& u = uiState(t.key());
  PatchData d;
  parsePatch(st.data, d);
  iskin::Skin k = iskin::begin(width, kW, kH, mfmPalette());
  iskin::background(k, 0, 0, kW, kH, k.c.bgTop, k.c.bgBottom, 4);
  // Unit tabs.
  static const char* tabs[11] = {"MAIN", "OP 1", "OP 2", "OP 3", "OP 4", "OP 5", "OP 6", "F 1", "F 2", "F 3", "FX"};
  const float tw = 742.0f / 11;
  for (int i = 0; i < 11; ++i) {
    char id[16];
    std::snprintf(id, sizeof id, "##tab%d", i);
    const ImU32 lc = i == 0 ? k.c.fill : i <= kOps ? k.c.fill : i <= kOps + kFilters ? k.c.fill2 : k.c.fill3;
    if (iskin::button(k, id, 5 + i * tw, 4, tw - 3, 22, tabs[i], u.tab == i, 10, lc)) u.tab = i, u.sel = -1;
  }
  if (!pui::titleWordmarkIn(k.dl(), "gl.matrixfm", k.at(752, 4), k.at(752 + 88, 4 + 22), k.px(10), 1, k.c.text))  // J-136: the wordmark
    iskin::text(k, 752, 4, 88, 22, pluginTitle("gl.matrixfm").c_str(), 10, k.c.textDim, 1, false, true);
  if (u.tab == 0) {
    mainPanel(a, t, k, st);
    artPanel(a, t, k, st, d, u, 236, 108);
  } else if (u.tab <= kOps) {
    opPanel(a, t, k, st, d, u.tab - 1);
    artPanel(a, t, k, st, d, u, 130, 214);
  } else if (u.tab <= kOps + kFilters) {
    filterPanel(a, t, k, st, u.tab - kOps - 1);
    artPanel(a, t, k, st, d, u, 130, 214);
  } else {
    fxPanel(a, t, k, u);
    // Waveshaper curves of the three filters are edited here (transfer curve, x = input level).
    for (int j = 0; j < kFilters; ++j) {
      const float x = 5 + j * 178;
      char cap[24];
      std::snprintf(cap, sizeof cap, "F%d SHAPER CURVE", j + 1);
      caption(k, x, 130, 174, cap);
      char id[16];
      std::snprintf(id, sizeof id, "##ws%d", j);
      static int sel[kFilters] = {-1, -1, -1};
      bool ch = curveEditor(k, id, x, 144, 174, 112, d.ws[static_cast<size_t>(j)], false, false, -1, sel[j], k.c.fill2);
      if (!d.ws[static_cast<size_t>(j)].empty() && iskin::button(k, (std::string(id) + "r").c_str(), x + 124, 148, 46, 13, "RESET", false, 8)) {
        d.ws[static_cast<size_t>(j)].pts.clear();
        ch = true;
      }
      if (ch) commit(a, t, st, d, "Edit shaper curve", 50 + j);
    }
    // Reverb detail strip.
    iskin::bevel(k, 5, 262, 533, 84, k.c.panel, k.c.panelLine, 3);
    caption(k, 8, 264, 80, "REVERB");
    const int rp[7] = {RvLowCut, RvHighCut, RvPreDelay, RvRoom, RvDiff, RvHighDamp, RvDecay};
    const char* rn[7] = {"LOW CUT", "HIGH CUT", "PREDELAY", "ROOM", "DIFFUSION", "HI DAMP", "DECAY"};
    for (int i = 0; i < 7; ++i) wheelCap(a, t, k, rp[i], 22 + i * 58, 280, 34, k.c.fill3, rn[i]);
    caption(k, 424, 276, 50, "COLOUR", 8);
    iskin::digi(a, t, k, RvColor, 426, 290, 46, 18, [](double v) {
      static const char* n[5] = {"BRT+", "BRT", "FLAT", "WARM", "WARM+"};
      return std::string(n[std::clamp(static_cast<int>(std::lround(v)), 0, 4)]);
    });
    iskin::check(a, t, k, RvTempo, 482, 292, 52, 12, "TEMPO", 8);
  }
  matrixPanel(a, t, k, u);
  // Audition keyboard.
  iskin::bevel(k, 4, 355, 837, 91, rgb(0x1E2326), k.c.panelLine, 3);
  iskin::KeyColors kc;
  iskin::keyboard(a, t, k, 8, 359, 829, 83, 36, 42, kc);
  iskin::end(k, kH);
}

}  // namespace gl::packs::synths
