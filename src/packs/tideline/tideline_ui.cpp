// Gridline — "tideline" pack: Tideline editor. Big Amount dial on the left, the
// rate / trigger strip on top, a large live curve display (gain envelope over
// one cycle, moving playhead, live gain, output trace folded over the cycle,
// drag to stretch / move the curve, End marker, custom nodes), shift buttons
// and a strip of shape thumbnails, then the band / stereo / trigger panels.
// Everything is drawn here as vectors with Theme roles.
#include "../../core/I18n.h"
#include "../PacksUi.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <unordered_map>
#include "../../core/Params.h"
#include "../../core/PluginBrand.h"
#include "../../engine/Program.h"
#include "../../app/App.h"
#include "../../app/Theme.h"
#include "../../app/Widgets.h"
#include "tideline.h"

namespace gl::packs::tideline {

using theme::pal;

namespace {

constexpr float kPi = 3.14159265f;

// Translatable copies of the shape / rate names (same order as tideline.h).
const char* const kShapeUi[kShapeCount + 1] = {
    i18n::N_("Pump"),     i18n::N_("Snap"),      i18n::N_("Tight"),  i18n::N_("Classic"),     i18n::N_("Round"), i18n::N_("Long"),
    i18n::N_("Breath"),   i18n::N_("Glide"),     i18n::N_("Sub Soft"), i18n::N_("Sub Deep"), i18n::N_("Trim"),  i18n::N_("Trim Fade"),
    i18n::N_("Inhale"),   i18n::N_("Inhale Long"), i18n::N_("Chop"), i18n::N_("Stutter"),     i18n::N_("Custom")};
const char* const kRateUi[kRateCount] = {i18n::N_("1/16"),  i18n::N_("1/8"),   i18n::N_("1/4"),   i18n::N_("1/2"),   i18n::N_("1 bar"),
                                         i18n::N_("2 bars"), i18n::N_("1/8 T"), i18n::N_("1/4 T"), i18n::N_("1/8 D"), i18n::N_("1/4 D")};

struct Live {
  float pos = -1, gain = 1, flash = 0, peak = 0, grDb = 0;
  bool ok = false;
};

Live liveOf(App& a, const PluginTarget& t) {
  Live l;
  if (t.isChannel()) return l;
  if (auto rt = a.builder.effectRuntime(t.owner, t.slot)) {
    l.grDb = rt->meter.load();
    l.pos = rt->bands[0].load();
    l.gain = rt->bands[1].load();
    l.flash = rt->bands[2].load();
    l.peak = rt->bands[3].load();
    l.ok = true;
  }
  return l;
}

// Output level folded over the cycle (one trace per editor), for the display.
struct Trace {
  std::array<float, 192> lvl{};
  float lastPos = -1;
};
Trace& traceFor(const PluginTarget& t) {
  static std::unordered_map<uint64_t, Trace> m;
  return m[t.key()];
}

double pv(App& a, const PluginTarget& t, int k) { return pui::paramValue(a, t, k); }

CustomCurve customOf(App& a, const PluginTarget& t) {
  std::array<float, BwCount> v{};
  for (int i = 0; i < BwCount; ++i) v[static_cast<size_t>(i)] = static_cast<float>(pv(a, t, i));
  CustomCurve c;
  customFrom(v, c);
  return c;
}

// Accent of the Dynamics family (J-94: orange, role::kMenu): curve and lit controls.
ImU32 accent() { return theme::litRole(pui::brandRole(BrandFamily::Dynamics)); }

void textC(ImDrawList* dl, ImFont* f, float size, ImVec2 c, ImU32 col, const char* s) {
  ImVec2 ts = f->CalcTextSizeA(size, 1e9f, 0, s);
  dl->AddText(f, size, ImVec2(std::round(c.x - ts.x * 0.5f), std::round(c.y - ts.y * 0.5f)), col, s);
}

// Shape polyline (gain 0..1 over one cycle) inside a rect.
void shapePath(ImDrawList* dl, ImVec2 p0, ImVec2 p1, int shape, const CustomCurve& cc, double duration, double shift, ImU32 col,
               float thick, ImU32 fill) {
  const int N = std::max(16, static_cast<int>(p1.x - p0.x));
  static std::vector<ImVec2> pts;
  pts.resize(static_cast<size_t>(N + 1));
  const float w = p1.x - p0.x, h = p1.y - p0.y;
  for (int i = 0; i <= N; ++i) {
    double p = static_cast<double>(i) / N;
    double g = curveAt(shape, cc, p + shift, duration);
    pts[static_cast<size_t>(i)] = ImVec2(p0.x + w * static_cast<float>(p), p1.y - h * static_cast<float>(g));
  }
  if (fill)
    for (int i = 0; i < N; ++i) {
      const ImVec2& q = pts[static_cast<size_t>(i)];
      dl->AddRectFilled(ImVec2(q.x, q.y), ImVec2(pts[static_cast<size_t>(i + 1)].x + 0.5f, p1.y), fill);
    }
  dl->AddPolyline(pts.data(), N + 1, col, 0, thick);
}

// ---------------------------------------------------------------- Amount dial

void amountDial(App& a, const PluginTarget& t, ImVec2 c, float r, const Live& live) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ParamId pid = t.pid(BwAmount);
  auto d = describeParam(a.doc.song(), pid);
  if (!d) return;
  double val = pv(a, t, BwAmount);
  ImGui::SetCursorScreenPos(ImVec2(c.x - r, c.y - r));
  ImGui::InvisibleButton("##bwamount", ImVec2(r * 2, r * 2));
  const bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
  pui::dragParamItem(a, pid, *d, val, 220.0f, false);
  const float n = static_cast<float>(std::clamp(val, 0.0, 1.0));
  const float a0 = kPi * 0.75f, a1 = kPi * 2.25f;
  // outer track + value arc
  dl->PathArcTo(c, r - 4, a0, a1, 64);
  dl->PathStroke(P.surface700, ImDrawFlags_None, 7.0f);
  if (n > 0.001f) {
    dl->PathArcTo(c, r - 4, a0, a0 + (a1 - a0) * n, 64);
    dl->PathStroke(act ? theme::mix(accent(), P.paper, 0.25f) : accent(), ImDrawFlags_None, 7.0f);
  }
  // live gain ring (inner): how much the curve is pulling down right now
  const float g = std::clamp(live.gain, 0.0f, 1.0f);
  dl->PathArcTo(c, r - 14, a0, a1, 64);
  dl->PathStroke(P.surface800, ImDrawFlags_None, 3.0f);
  if (live.ok && live.pos >= 0 && g < 0.999f) {
    dl->PathArcTo(c, r - 14, a0, a0 + (a1 - a0) * (1.0f - g), 48);
    dl->PathStroke(theme::withAlpha(P.paper, 0.75f), ImDrawFlags_None, 3.0f);
  }
  // body
  const float br = r - 22;
  dl->AddCircleFilled(c, br, hov || act ? P.surface600 : P.surface700, 64);
  dl->AddCircle(c, br, P.lineStrong, 64, 1.0f);
  dl->AddCircleFilled(ImVec2(c.x, c.y + 2), br * 0.62f, theme::withAlpha(P.surface950, 0.35f), 48);
  dl->AddCircleFilled(c, br * 0.62f, P.surface800, 48);
  // pointer
  const float ang = a0 + (a1 - a0) * n;
  const ImVec2 dir(std::cos(ang), std::sin(ang));
  dl->AddLine(ImVec2(c.x + dir.x * br * 0.68f, c.y + dir.y * br * 0.68f), ImVec2(c.x + dir.x * br * 0.94f, c.y + dir.y * br * 0.94f),
              accent(), 4.0f);
  // value in the centre
  char b[16];
  std::snprintf(b, sizeof b, "%d%%", static_cast<int>(std::lround(val * 100)));
  textC(dl, a.fontBold, theme::kFontLcd, ImVec2(c.x, c.y - 2), P.text, b);
  textC(dl, a.fontMono, theme::kFontLabel, ImVec2(c.x, c.y + 16), P.textFaint, i18n::tr("AMOUNT"));
  if (pui::paramLinked(a, pid)) dl->AddCircleFilled(ImVec2(c.x + r * 0.8f, c.y - r * 0.8f), 3, accent());
}

// ---------------------------------------------------------------- display

void curveDisplay(App& a, const PluginTarget& t, ImVec2 p0, ImVec2 p1, const Live& live) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const int shape = static_cast<int>(std::lround(pv(a, t, BwShape)));
  const int rate = std::clamp(static_cast<int>(std::lround(pv(a, t, BwRate))), 0, kRateCount - 1);
  const int trig = static_cast<int>(std::lround(pv(a, t, BwTrigger)));
  const bool loop = pv(a, t, BwLoop) >= 0.5;
  const double duration = pv(a, t, BwDuration), shift = pv(a, t, BwShift), amount = pv(a, t, BwAmount);
  const double endAt = pv(a, t, BwEnd);
  const CustomCurve cc = customOf(a, t);
  pui::displayFrame(dl, p0, p1);
  const ImVec2 g0(p0.x + 10, p0.y + 22), g1(p1.x - 10, p1.y - 10);
  const float w = g1.x - g0.x, h = g1.y - g0.y;
  // J-98: the display's x is time within the cycle; the curve drawn at x is the
  // curve at phase x + Shift (see curveAt below). The engine reports the phase,
  // so the playhead, the trace, the End marker and the custom points are placed
  // at phase - Shift (wrapped while looping, clamped in one-shot mode).
  const bool wraps = trig == TrigTempo || loop;
  auto phaseToX = [&](double ph) {
    double x = ph - shift;
    if (wraps) x -= std::floor(x);
    return std::clamp(x, 0.0, 1.0);
  };
  const float livePos = live.ok && live.pos >= 0 ? static_cast<float>(phaseToX(live.pos)) : -1.0f;
  // grid: sixteenths of the cycle, quarters stronger, gain lines at 25 %
  for (int i = 0; i <= 16; ++i) {
    float x = std::round(g0.x + w * i / 16.0f) + 0.5f;
    dl->AddLine(ImVec2(x, g0.y), ImVec2(x, g1.y), i % 4 == 0 ? P.gridBeat : theme::withAlpha(P.gridStep, 0.5f), 1);
  }
  for (int i = 0; i <= 4; ++i) {
    float y = std::round(g0.y + h * i / 4.0f) + 0.5f;
    dl->AddLine(ImVec2(g0.x, y), ImVec2(g1.x, y), theme::withAlpha(P.gridStep, i == 0 || i == 4 ? 0.9f : 0.45f), 1);
  }
  // output trace folded over the cycle
  Trace& tr = traceFor(t);
  const int TB = static_cast<int>(tr.lvl.size());
  for (auto& v : tr.lvl) v *= 0.985f;
  if (livePos >= 0) {
    int bi = std::clamp(static_cast<int>(livePos * TB), 0, TB - 1);
    int from = tr.lastPos >= 0 ? std::clamp(static_cast<int>(tr.lastPos * TB), 0, TB - 1) : bi;
    if (from > bi || bi - from > TB / 2) from = bi;
    for (int k = from; k <= bi; ++k) tr.lvl[static_cast<size_t>(k)] = std::max(tr.lvl[static_cast<size_t>(k)], live.peak);
    tr.lastPos = livePos;
  } else {
    tr.lastPos = -1;
  }
  const float midY = g0.y + h * 0.5f;
  for (int k = 0; k < TB; ++k) {
    float v = std::clamp(tr.lvl[static_cast<size_t>(k)], 0.0f, 1.0f) * h * 0.48f;
    if (v < 0.5f) continue;
    float x0 = g0.x + w * k / TB, x1 = g0.x + w * (k + 1) / TB - 1;
    dl->AddRectFilled(ImVec2(x0, midY - v), ImVec2(x1, midY + v), theme::withAlpha(P.paper, 0.13f));
  }
  // curve with amount applied (dim) and the full shape (bright)
  {
    const int N = std::max(16, static_cast<int>(w));
    static std::vector<ImVec2> pts;
    pts.resize(static_cast<size_t>(N + 1));
    for (int i = 0; i <= N; ++i) {
      double p = static_cast<double>(i) / N;
      double gg = applyAmount(curveAt(shape, cc, p + shift, duration), amount);
      pts[static_cast<size_t>(i)] = ImVec2(g0.x + w * static_cast<float>(p), g1.y - h * static_cast<float>(gg));
    }
    for (int i = 0; i < N; ++i)
      dl->AddRectFilled(ImVec2(pts[static_cast<size_t>(i)].x, g0.y), ImVec2(pts[static_cast<size_t>(i + 1)].x, pts[static_cast<size_t>(i)].y),
                        theme::withAlpha(accent(), 0.10f));
    shapePath(dl, g0, g1, shape, cc, duration, shift, theme::withAlpha(accent(), 0.35f), 1.5f, 0);
    dl->AddPolyline(pts.data(), N + 1, accent(), 0, 2.5f);
  }
  // one-shot end marker (triggered modes without loop)
  const bool showEnd = trig != TrigTempo && !loop;
  if (showEnd) {
    float x = g0.x + w * static_cast<float>(std::clamp(endAt - shift, 0.0, 1.0));
    dl->AddRectFilled(ImVec2(x, g0.y), g1, theme::withAlpha(P.surface950, 0.55f));
    dl->AddLine(ImVec2(x, g0.y), ImVec2(x, g1.y), P.paper, 1.5f);
    const ImVec2 hp(x - 7, g0.y), hq(x + 7, g0.y + 16);
    dl->AddRectFilled(hp, hq, P.surface600, 3);
    for (int k = -1; k <= 1; ++k) dl->AddLine(ImVec2(x + k * 3.0f, hp.y + 4), ImVec2(x + k * 3.0f, hq.y - 4), P.text, 1);
    ImGui::SetCursorScreenPos(ImVec2(x - 8, g0.y));
    ImGui::InvisibleButton("##bwend", ImVec2(16, h));
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    static float grabEnd = 0;  // pointer - marker at the click: no jump when grabbed off centre
    if (ImGui::IsItemActivated()) grabEnd = ImGui::GetIO().MousePos.x - x, a.doc.endGesture();
    if (ImGui::IsItemDeactivated()) a.doc.endGesture();
    if (ImGui::IsItemActive() && !ImGui::IsItemActivated()) {
      double v = std::clamp((ImGui::GetIO().MousePos.x - grabEnd - g0.x) / w + shift, 0.0, 1.0);
      pui::setParamValue(a, t, BwEnd, v, true);
    }
    if (ImGui::IsItemHovered()) ui::Tooltip(i18n::tr("One-shot end: the curve holds here until the next trigger"));
  }
  // custom nodes / stretch + move drag on the curve
  if (shape == kShapeCustom) {
    // A point at time x sounds at phase x0 + (x - x0) * f (Duration stretches
    // around the first point x0, see customGain), shown at that phase - Shift.
    const double f = durationFactor(duration);
    int first = 0;
    for (int i = 1; i < kNodes; ++i)
      if (pv(a, t, BwNodeX0 + i) < pv(a, t, BwNodeX0 + first)) first = i;
    const double x0 = pv(a, t, BwNodeX0 + first);
    static ImVec2 grab(0, 0);  // pointer - point centre at the click
    for (int i = 0; i < kNodes; ++i) {
      const double x = pv(a, t, BwNodeX0 + i), y = pv(a, t, BwNodeY0 + i);
      double ph = std::min(1.0, x0 + (x - x0) * f) - shift;
      if (ph < 0 || ph > 1) ph -= std::floor(ph);
      ImVec2 c(g0.x + w * static_cast<float>(ph), g1.y - h * static_cast<float>(y));
      ImGui::PushID(i);
      ImGui::SetCursorScreenPos(ImVec2(c.x - 7, c.y - 7));
      ImGui::InvisibleButton("##node", ImVec2(14, 14));
      const bool hv = ImGui::IsItemHovered(), ac = ImGui::IsItemActive();
      if (ImGui::IsItemActivated()) grab = ImVec2(ImGui::GetIO().MousePos.x - c.x, ImGui::GetIO().MousePos.y - c.y), a.doc.endGesture();
      if (ImGui::IsItemDeactivated()) a.doc.endGesture();
      if (ac && !ImGui::IsItemActivated()) {
        const ImVec2 m(ImGui::GetIO().MousePos.x - grab.x, ImGui::GetIO().MousePos.y - grab.y);
        double p = std::clamp(static_cast<double>((m.x - g0.x) / w), 0.0, 1.0) + shift;
        if (p < 0 || p > 1) p -= std::floor(p);
        const double nx = i == first || p < x0 ? p : x0 + (p - x0) / f;
        pui::setParamValue(a, t, BwNodeX0 + i, std::clamp(nx, 0.0, 1.0), true);
        pui::setParamValue(a, t, BwNodeY0 + i, std::clamp((g1.y - m.y) / h, 0.0f, 1.0f), true);
      }
      if (hv) {
        char tip[64];
        std::snprintf(tip, sizeof tip, "%s %d: %.0f%% / %.0f%%", i18n::tr("Point"), i + 1, x * 100, y * 100);
        ImGui::SetTooltip("%s", tip);
      }
      dl->AddCircleFilled(c, hv || ac ? 6.0f : 4.5f, ac ? P.paper : accent(), 20);
      dl->AddCircle(c, hv || ac ? 6.0f : 4.5f, P.surface950, 20, 1.5f);
      ImGui::PopID();
    }
  } else {
    // drag on the curve area: left/right stretches the duck (Duration), Shift+drag moves it (Shift)
    ImGui::SetCursorScreenPos(g0);
    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("##bwstretch", ImVec2(w, h));
    static double startVal = 0;
    static bool moving = false;
    if (ImGui::IsItemActivated()) {
      moving = ImGui::GetIO().KeyShift;
      startVal = pv(a, t, moving ? BwShift : BwDuration);
      a.doc.endGesture();
    }
    if (ImGui::IsItemDeactivated()) a.doc.endGesture();
    if (ImGui::IsItemActive()) {
      ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
      const float dx = ImGui::GetMouseDragDelta(0, 0).x / w;
      if (moving) {  // the curve follows the pointer; Shift is cyclic, so it wraps instead of stopping at +-50 %
        double v = startVal - dx;
        v -= std::floor(v + 0.5);
        pui::setParamValue(a, t, BwShift, std::clamp(v, -0.5, 0.5), true);
      }
      else pui::setParamValue(a, t, BwDuration, std::clamp(startVal + dx * 2.0, -1.0, 1.0), true);
    }
    if (ImGui::IsItemHovered() && !ImGui::IsItemActive())
      ui::Tooltip(i18n::tr("Drag left / right to shorten or stretch the duck. Shift+drag moves it against the beat."));
  }
  // playhead + live gain
  if (livePos >= 0) {
    float x = g0.x + w * livePos;
    dl->AddLine(ImVec2(x, g0.y), ImVec2(x, g1.y), theme::withAlpha(P.playhead, 0.9f), 1.5f);
    float y = g1.y - h * std::clamp(live.gain, 0.0f, 1.0f);
    dl->AddCircleFilled(ImVec2(x, y), 5.5f, P.paper, 20);
    dl->AddCircle(ImVec2(x, y), 5.5f, accent(), 20, 2);
  }
  if (live.flash > 0.02f) dl->AddRectFilled(ImVec2(g0.x, g0.y - 18), ImVec2(g0.x + 6, g0.y - 6), theme::withAlpha(P.led, live.flash), 2);
  // header line inside the display: shape name, rate, live reduction
  {
    char b[96];
    const char* nm = shape >= 0 && shape <= kShapeCustom ? kShapeUi[shape] : "";
    std::snprintf(b, sizeof b, "%s  ·  %s", i18n::tr(nm), i18n::tr(kRateUi[rate]));
    dl->AddText(a.fontMono, theme::kFontLabel, ImVec2(g0.x + 10, p0.y + 6), P.textDim, b);
    float db = live.ok && live.pos >= 0 ? live.grDb : 0.0f;
    if (db < -0.05f) std::snprintf(b, sizeof b, "%.1f dB", db);
    else std::snprintf(b, sizeof b, "0.0 dB");
    ImVec2 ts = a.fontMono->CalcTextSizeA(theme::kFontLabel, 1e9f, 0, b);
    dl->AddText(a.fontMono, theme::kFontLabel, ImVec2(g1.x - ts.x - 34, p0.y + 6), db < -0.05f ? P.text : P.textFaint, b);
  }
  // reset (duration, shift, custom points)
  ImGui::SetCursorScreenPos(ImVec2(g1.x - 24, p0.y + 2));
  if (ui::IconButton("##bwreset", ui::Icon::Undo, false, 20, i18n::tr("Reset the curve's duration, shift and points"))) {
    const PluginState* sp = t.state(a.doc.song());
    if (sp) {
      PluginState st = *sp;
      st.params[BwDuration] = 0;
      st.params[BwShift] = 0;
      if (shape == kShapeCustom) {
        st.params[BwTension] = 0.3f;
        for (int i = 0; i < kNodes; ++i) {
          st.params[static_cast<size_t>(BwNodeX0 + i)] = kDefNodeX[i];
          st.params[static_cast<size_t>(BwNodeY0 + i)] = kDefNodeY[i];
        }
      }
      setPluginState(a, t, st, i18n::tr("Reset curve"));
    }
  }
}

// ---------------------------------------------------------------- shape strip

void shapeStrip(App& a, const PluginTarget& t, ImVec2 p0, float w, float cellH) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const int cur = static_cast<int>(std::lround(pv(a, t, BwShape)));
  const CustomCurve cc = customOf(a, t);
  const int cols = 9;
  const float gap = 4, cw = (w - gap * (cols - 1)) / cols;
  for (int i = 0; i <= kShapeCustom; ++i) {
    const int row = i / cols, col = i % cols;
    ImVec2 q0(p0.x + col * (cw + gap), p0.y + row * (cellH + gap)), q1(q0.x + cw, q0.y + cellH);
    ImGui::PushID(i);
    ImGui::SetCursorScreenPos(q0);
    if (ImGui::InvisibleButton("##shape", ImVec2(cw, cellH))) pui::setParamValue(a, t, BwShape, i, false);
    const bool hv = ImGui::IsItemHovered();
    if (hv) ui::Tooltip(kShapeUi[i]);
    const bool on = i == cur;
    dl->AddRectFilled(q0, q1, on ? accent() : (hv ? P.surface600 : P.surface700), theme::kRadiusLg);
    if (!on) dl->AddRect(q0, q1, P.line, theme::kRadiusLg);
    const ImVec2 i0(q0.x + 6, q0.y + 6), i1(q1.x - 6, q1.y - 6);
    shapePath(dl, i0, i1, i, cc, 0, 0, on ? P.surface950 : (hv ? P.text : P.textDim), on ? 2.0f : 1.5f, 0);
    if (i == kShapeCustom) dl->AddCircleFilled(ImVec2(q1.x - 6, q0.y + 6), 2.5f, on ? P.surface950 : accent());
    ImGui::PopID();
  }
}

// Coarse / fine nudge buttons for Shift (1/16 and 1/256 of the cycle).
void shiftRow(App& a, const PluginTarget& t, ImVec2 p0, float w) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const double s = pv(a, t, BwShift);
  struct B {
    const char* id;
    ui::Icon icon;
    double step;
    const char* hint;
  };
  const B bs[4] = {{"##sh0", ui::Icon::Left, -1.0 / 16, i18n::N_("Move the curve earlier by 1/16")},
                   {"##sh1", ui::Icon::Chevron, -1.0 / 256, i18n::N_("Nudge the curve earlier (fine)")},
                   {"##sh2", ui::Icon::Chevron, 1.0 / 256, i18n::N_("Nudge the curve later (fine)")},
                   {"##sh3", ui::Icon::Right, 1.0 / 16, i18n::N_("Move the curve later by 1/16")}};
  const float bw = 34, bh = 22;
  for (int k = 0; k < 4; ++k) {
    float x = k < 2 ? p0.x + k * (bw + 4) : p0.x + w - (4 - k) * (bw + 4) + 4;
    ImVec2 q0(x, p0.y), q1(x + bw, p0.y + bh);
    ImGui::SetCursorScreenPos(q0);
    if (ImGui::InvisibleButton(bs[k].id, ImVec2(bw, bh))) {
      double v = s - bs[k].step;  // later = curve moves right = earlier phase
      v -= std::floor(v + 0.5);
      pui::setParamValue(a, t, BwShift, std::clamp(v, -0.5, 0.5), false);
    }
    const bool hv = ImGui::IsItemHovered(), ac = ImGui::IsItemActive();
    if (hv) ui::Tooltip(bs[k].hint);
    dl->AddRectFilled(q0, q1, ac ? P.surface600 : (hv ? P.surface700 : P.surface800), theme::kRadius);
    dl->AddRect(q0, q1, P.line, theme::kRadius);
    const ImVec2 c((q0.x + q1.x) * 0.5f, (q0.y + q1.y) * 0.5f);
    if (bs[k].icon == ui::Icon::Chevron) {  // fine: small chevron pointing the way
      const float d = k == 1 ? -1.0f : 1.0f;
      dl->AddLine(ImVec2(c.x - 3 * d, c.y - 5), ImVec2(c.x + 3 * d, c.y), P.text, 1.6f);
      dl->AddLine(ImVec2(c.x + 3 * d, c.y), ImVec2(c.x - 3 * d, c.y + 5), P.text, 1.6f);
    } else {
      ui::drawIcon(dl, bs[k].icon, c, 14, P.text);
    }
  }
  char b[48];
  std::snprintf(b, sizeof b, "%s %+.1f%%", i18n::tr("SHIFT"), s * 100.0);
  textC(dl, a.fontMono, theme::kFontLabel, ImVec2(p0.x + w * 0.5f, p0.y + bh * 0.5f), std::fabs(s) > 1e-6 ? P.text : P.textFaint, b);
}

// Pill selector over a choice parameter (equal cells, lit cell in the accent).
void pills(App& a, const PluginTarget& t, int param, ImVec2 p0, float w, float h, const char* const* labels, int count) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const int cur = static_cast<int>(std::lround(pv(a, t, param)));
  dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), P.surface800, h * 0.5f);
  dl->AddRect(p0, ImVec2(p0.x + w, p0.y + h), P.line, h * 0.5f);
  const float cw = w / count;
  for (int i = 0; i < count; ++i) {
    const float x = p0.x + cw * i;
    ImGui::PushID(param * 64 + i);
    ImGui::SetCursorScreenPos(ImVec2(x, p0.y));
    if (ImGui::InvisibleButton("##pill", ImVec2(cw, h))) pui::setParamValue(a, t, param, i, false);
    const bool hv = ImGui::IsItemHovered();
    if (i == cur) dl->AddRectFilled(ImVec2(x + 2, p0.y + 2), ImVec2(x + cw - 2, p0.y + h - 2), accent(), (h - 4) * 0.5f);
    else if (hv) dl->AddRectFilled(ImVec2(x + 2, p0.y + 2), ImVec2(x + cw - 2, p0.y + h - 2), P.surface700, (h - 4) * 0.5f);
    textC(dl, a.fontRegular, theme::kFontCaption, ImVec2(x + cw * 0.5f, p0.y + h * 0.5f), i == cur ? P.surface950 : (hv ? P.text : P.textDim),
          i18n::tr(labels[i]));
    ImGui::PopID();
  }
}

// Rate selector: straight values as pills, triplet / dotted ones after a divider.
void ratePills(App& a, const PluginTarget& t, ImVec2 p0, float w, float h) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  static const char* const rl[kRateCount] = {"1/16", "1/8", "1/4", "1/2", "1", "2", "1/8T", "1/4T", "1/8.", "1/4."};
  const int cur = static_cast<int>(std::lround(pv(a, t, BwRate)));
  float tw[kRateCount], total = 0;
  for (int i = 0; i < kRateCount; ++i) total += tw[i] = a.fontMono->CalcTextSizeA(theme::kFontLabel, 1e9f, 0, rl[i]).x;
  const float divider = 10;
  const float pad = std::max(4.0f, (w - total - divider) / kRateCount);
  float x = p0.x;
  dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), P.surface800, h * 0.5f);
  for (int i = 0; i < kRateCount; ++i) {
    if (i == 6) {
      dl->AddLine(ImVec2(x + divider * 0.5f, p0.y + 5), ImVec2(x + divider * 0.5f, p0.y + h - 5), P.line, 1);
      x += divider;
    }
    const float cw = tw[i] + pad;
    ImGui::PushID(i);
    ImGui::SetCursorScreenPos(ImVec2(x, p0.y));
    if (ImGui::InvisibleButton("##rate", ImVec2(cw, h))) pui::setParamValue(a, t, BwRate, i, false);
    const bool hv = ImGui::IsItemHovered();
    if (hv) {
      char tip[64];
      std::snprintf(tip, sizeof tip, "%s: %s", i18n::tr("Cycle"), i18n::tr(kRateUi[i]));
      ui::Tooltip(tip);
    }
    if (i == cur) dl->AddRectFilled(ImVec2(x + 1, p0.y + 2), ImVec2(x + cw - 1, p0.y + h - 2), accent(), (h - 4) * 0.5f);
    else if (hv) dl->AddRectFilled(ImVec2(x + 1, p0.y + 2), ImVec2(x + cw - 1, p0.y + h - 2), P.surface700, (h - 4) * 0.5f);
    textC(dl, a.fontMono, theme::kFontLabel, ImVec2(x + cw * 0.5f, p0.y + h * 0.5f), i == cur ? P.surface950 : (hv ? P.text : P.textDim), rl[i]);
    ImGui::PopID();
    x += cw;
  }
}

void caption(ImDrawList* dl, App& a, ImVec2 p, const char* s) {
  dl->AddText(a.fontMono, theme::kFontLabel, p, pal().textFaint, i18n::tr(s));
}

}  // namespace

bool drawEditor(App& a, const PluginTarget& t, float width) {
  const PluginState* sp = t.state(a.doc.song());
  if (!sp || sp->type != "gl.tideline") return false;
  const auto& P = pal();
  const Live live = liveOf(a, t);
  const float W = std::max(width, 640.0f);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 o = ImGui::GetCursorScreenPos();
  const int trig = static_cast<int>(std::lround(pv(a, t, BwTrigger)));

  // ---- top strip: title, rate, trigger, loop
  const float topH = 30;
  const float leftW = 210;
  const float rx = o.x + leftW + 12, rw = W - leftW - 12;
  const float rateW = rw * 0.62f;
  ratePills(a, t, ImVec2(rx, o.y + 3), rateW, 24);
  const float trigX = rx + rateW + 10;
  const float trigW = rw - (trigX - rx) - (trig != TrigTempo ? 30 : 0);
  {
    static const char* const tl[3] = {i18n::N_("Tempo"), i18n::N_("Audio"), i18n::N_("MIDI")};
    pills(a, t, BwTrigger, ImVec2(trigX, o.y + 3), trigW, 24, tl, 3);
  }
  if (trig != TrigTempo) {
    const bool loop = pv(a, t, BwLoop) >= 0.5;
    ImGui::SetCursorScreenPos(ImVec2(rx + rw - 26, o.y + 3));
    if (ui::IconButton("##bwloop", ui::Icon::Loop, loop, 24,
                       i18n::tr("Loop: repeat the curve until the next trigger (off = one shot with an end marker)")))
      pui::setParamValue(a, t, BwLoop, loop ? 0.0 : 1.0, false);
  }

  // ---- main row: dial (left) + display (right)
  const float mainY = o.y + topH + 8, dispH = 214;
  amountDial(a, t, ImVec2(o.x + leftW * 0.5f, mainY + 96), 92, live);
  curveDisplay(a, t, ImVec2(rx, mainY), ImVec2(rx + rw, mainY + dispH), live);
  shiftRow(a, t, ImVec2(rx, mainY + dispH + 6), rw);
  const float stripY = mainY + dispH + 34, cellH = 38;
  shapeStrip(a, t, ImVec2(rx, stripY), rw, cellH);

  // left column below the dial: band split
  {
    const float by = mainY + 200;
    caption(dl, a, ImVec2(o.x + 4, by), i18n::N_("BAND"));
    static const char* const bl[3] = {i18n::N_("Full"), i18n::N_("Lows"), i18n::N_("Highs")};
    pills(a, t, BwBand, ImVec2(o.x + 4, by + 16), leftW - 40, 24, bl, 3);
    ImGui::SetCursorScreenPos(ImVec2(o.x + leftW - 30, by + 15));
    const bool listen = pv(a, t, BwBandListen) >= 0.5;
    // J-98: Listen solos what is ducked (the band, of the mid or side when one is
    // picked). Full band in Stereo ducks everything, so there it changes nothing:
    // the button stays usable but is drawn faint and says so.
    const bool solos = static_cast<int>(std::lround(pv(a, t, BwBand))) != BandFull || static_cast<int>(std::lround(pv(a, t, BwStereo))) != StStereo;
    if (ui::IconButton("##bwlisten", ui::Icon::Audio, listen, 24,
                       solos ? i18n::tr("Listen: hear only the part being ducked")
                             : i18n::tr("Listen: Full band in Stereo ducks everything, so there is nothing to solo. Pick Lows / Highs or Mid / Side."),
                       solos ? 0 : P.textFaint, solos ? 0 : P.textFaint))
      pui::setParamValue(a, t, BwBandListen, listen ? 0.0 : 1.0, false);
    ImGui::SetCursorScreenPos(ImVec2(o.x + 4, by + 46));
    pui::hslider(a, t, BwBandFreq, i18n::tr("Split"), leftW - 8);
  }

  // ---- lower panels
  const float lowY = stripY + 2 * cellH + 4 + 12;
  ImGui::SetCursorScreenPos(ImVec2(o.x, lowY));
  const float kh = pui::knobCellHeight(pui::KnobSmall);
  const float kw = pui::knobCellWidth(pui::KnobSmall);
  // Curve panel
  float x = o.x;
  const float panelH = kh + 40;
  auto panel = [&](float px, float pw, const char* title) {
    dl->AddRectFilled(ImVec2(px, lowY), ImVec2(px + pw, lowY + panelH), P.surface800, theme::kRadiusLg);
    dl->AddRect(ImVec2(px, lowY), ImVec2(px + pw, lowY + panelH), P.line, theme::kRadiusLg);
    dl->AddText(a.fontMono, theme::kFontLabel, ImVec2(px + 8, lowY + 5), P.textFaint, i18n::tr(title));
  };
  const bool custom = static_cast<int>(std::lround(pv(a, t, BwShape))) == kShapeCustom;
  const float curveW = kw * (custom ? 4 : 3) + 16;
  panel(x, curveW, i18n::N_("CURVE"));
  pui::knobAt(a, t, BwDuration, ImVec2(x + 8, lowY + 22), i18n::tr("Duration"), pui::KnobSmall);
  pui::knobAt(a, t, BwShift, ImVec2(x + 8 + kw, lowY + 22), i18n::tr("Shift"), pui::KnobSmall);
  pui::knobAt(a, t, BwSmooth, ImVec2(x + 8 + 2 * kw, lowY + 22), i18n::tr("Smooth"), pui::KnobSmall);
  if (custom) pui::knobAt(a, t, BwTension, ImVec2(x + 8 + 3 * kw, lowY + 22), i18n::tr("Tension"), pui::KnobSmall);
  x += curveW + 8;
  // Stereo panel
  const float stW = 150;
  panel(x, stW, i18n::N_("STEREO"));
  {
    static const char* const sl[3] = {i18n::N_("Stereo"), i18n::N_("Mid"), i18n::N_("Side")};
    pills(a, t, BwStereo, ImVec2(x + 8, lowY + 26), stW - 16, 24, sl, 3);
  }
  {
    const int st = static_cast<int>(std::lround(pv(a, t, BwStereo)));
    const char* hint = st == StMid ? i18n::N_("Ducks the centre only") : st == StSide ? i18n::N_("Ducks the sides only") : i18n::N_("Ducks both channels together");
    dl->AddText(a.fontRegular, theme::kFontCaption, ImVec2(x + 8, lowY + 56), P.textDim, i18n::tr(hint));
  }
  x += stW + 8;
  // Trigger panel (contents depend on the mode)
  const float trW = o.x + W - x;
  panel(x, trW, trig == TrigAudio ? i18n::N_("AUDIO TRIGGER") : trig == TrigMidi ? i18n::N_("MIDI TRIGGER") : i18n::N_("TEMPO SYNC"));
  if (trig == TrigAudio) {
    float kx = x + 8;
    const int ks[4] = {BwThreshold, BwRetrigger, BwDetLow, BwDetHigh};
    const char* kl[4] = {i18n::N_("Threshold"), i18n::N_("Gap"), i18n::N_("Low cut"), i18n::N_("High cut")};
    for (int k = 0; k < 4 && kx + kw <= x + trW; ++k, kx += kw) pui::knobAt(a, t, ks[k], ImVec2(kx, lowY + 22), i18n::tr(kl[k]), pui::KnobSmall);
    if (kx + 100 <= x + trW) {
      ImGui::SetCursorScreenPos(ImVec2(kx + 4, lowY + 24));
      pui::stepper(a, t, BwScIn, std::min(96.0f, x + trW - kx - 16), nullptr);
      const int sc = static_cast<int>(std::lround(pv(a, t, BwScIn)));
      const char* what = sc < 0 ? i18n::N_("Trigger: track sidechain") : sc == 0 ? i18n::N_("Trigger: own input") : i18n::N_("Trigger: input route");
      dl->AddText(a.fontRegular, theme::kFontCaption, ImVec2(kx + 4, lowY + 54), P.textDim, i18n::tr(what));
    }
  } else if (trig == TrigMidi) {
    pui::knobAt(a, t, BwMidiPort, ImVec2(x + 8, lowY + 22), i18n::tr("Port"), pui::KnobSmall);
    pui::knobAt(a, t, BwMidiNote, ImVec2(x + 8 + kw, lowY + 22), i18n::tr("Note"), pui::KnobSmall);
    const int port = static_cast<int>(std::lround(pv(a, t, BwMidiPort))), note = static_cast<int>(std::lround(pv(a, t, BwMidiNote)));
    char b[96];
    std::snprintf(b, sizeof b, "%s %s · %s %s", i18n::tr("port"), port < 0 ? i18n::tr("any") : std::to_string(port).c_str(), i18n::tr("note"),
                  note < 0 ? i18n::tr("any") : std::to_string(note).c_str());
    dl->AddText(a.fontRegular, theme::kFontCaption, ImVec2(x + 16 + 2 * kw, lowY + 30), P.textDim, b);
    dl->AddText(a.fontRegular, theme::kFontCaption, ImVec2(x + 16 + 2 * kw, lowY + 48), P.textFaint,
                i18n::tr("Notes from a MIDI Out channel"));
  } else {
    dl->AddText(a.fontRegular, theme::kFontCaption, ImVec2(x + 10, lowY + 28), P.textDim, i18n::tr("Locked to the song position."));
    dl->AddText(a.fontRegular, theme::kFontCaption, ImVec2(x + 10, lowY + 46), P.textFaint,
                i18n::tr("Switch to Audio or MIDI to follow a kick."));
  }
  ImGui::SetCursorScreenPos(ImVec2(o.x, lowY + panelH + 6));
  ImGui::Dummy(ImVec2(W, 1));
  return true;
}

}  // namespace gl::packs::tideline
