// Gridline — "dynamics" pack: reference-layout editors (κ2) for Mastering
// Dynamics (gl.master, the reference maximizer's counterpart) and Enhance
// (gl.enhance, the one-knob counterpart). Laid out on the reference forms'
// geometry with their palette roles, drawn with Gridline's skin kit.
#include "../../core/I18n.h"
#include "maximus_ref.h"
#include <map>
#include "../../app/PluginEditorsDynFl.h"
#include "../../app/PluginSkin.h"
#include "dyn_widgets.h"

namespace gl::packs::dynamics {

using skin::hex;

namespace {

struct MaxUi {
  int band = 3;
  int dragPt = -1, dragTen = -1;
  bool vis[8] = {true, true, true, true, true, true, true, true};
  float speed = 0.5f;
  std::vector<float> spare;
};
MaxUi& maxUi(uint64_t key) {
  static std::map<uint64_t, MaxUi> m;
  return m[key];
}

const char* kBand[4] = {"LOW", "MID", "HIGH", "MASTER"};
const ImU32 kBandCol[4] = {hex(0xB86D68), hex(0xC9B465), hex(0xCED671), hex(0xC9D5D4)};

double curveXForDb(double db) { return std::log(2.0 * std::pow(10.0, db / 20.0) + 1.0) / std::log(9.0); }

// Writes a whole drawn curve (points sorted by x) as one undo step.
void writeCurve(App& a, const PluginTarget& t, const PluginState& st, int band, const DrawnCurve& c, const char* label) {
  PluginState ns = st;
  normalizePluginState(ns);
  ns.params[static_cast<size_t>(msExt(band, MxPoints))] = static_cast<float>(c.n);
  for (int k = 0; k < c.n; ++k) {
    ns.params[static_cast<size_t>(msExt(band, MxPt0 + k * 3))] = static_cast<float>(c.x[k]);
    ns.params[static_cast<size_t>(msExt(band, MxPt0 + k * 3 + 1))] = static_cast<float>(c.y[k]);
    ns.params[static_cast<size_t>(msExt(band, MxPt0 + k * 3 + 2))] = static_cast<float>(c.t[k]);
  }
  setPluginState(a, t, ns, label);
}

// ---------------------------------------------------------------- curve editor
// Reference envelope-editor area (5,4 181x181): x = input, y = output, both on
// the log9(2a + 1) scale (0.5 = 0 dBFS, 1 = +12 dB). Drawn engine: drag points,
// drag a segment's middle handle up / down for its tension, double-click to add
// a point, right-click a point to remove it. Parametric engine: the curve the
// thresholds / ratios give (read only; switch the engine to draw).
void curveArea(App& a, const PluginTarget& t, const PluginState& st, MaxUi& u, const skin::Form& f, float inDb) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const int b = u.band;
  const bool drawn = ui::ival(st, msExt(b, MxCurveMode)) == 1 || ui::ival(st, MsRef0 + MrModel) == 1;
  ImVec2 p0 = f.p(5, 4), p1 = f.p(186, 185);
  dl->AddRectFilled(p0, p1, hex(0x2C3A41));
  const float pad = f.d(6);
  ImVec2 q0(p0.x + pad, p0.y + pad), q1(p1.x - pad, p1.y - pad);
  auto X = [&](double x) { return q0.x + static_cast<float>(x) * (q1.x - q0.x); };
  auto Y = [&](double y) { return q1.y - static_cast<float>(y) * (q1.y - q0.y); };
  auto invX = [&](float x) { return std::clamp((x - q0.x) / (q1.x - q0.x), 0.0f, 1.0f); };
  auto invY = [&](float y) { return std::clamp((q1.y - y) / (q1.y - q0.y), 0.0f, 1.0f); };
  const double grid[5] = {-24, -12, -6, 0, 6};
  for (double g : grid) {
    double x = curveXForDb(g);
    ImU32 c = g == 0 ? hex(0x5D7380) : hex(0x3A4B54);
    dl->AddLine(ImVec2(X(x), q0.y), ImVec2(X(x), q1.y), c);
    dl->AddLine(ImVec2(q0.x, Y(x)), ImVec2(q1.x, Y(x)), c);
    char lb[8];
    std::snprintf(lb, sizeof lb, "%+.0f", g);
    skin::label(dl, f, (X(x) - f.o.x) / f.s - 10, 175, 20, 8, g == 0 ? "0" : lb, hex(0x7F96A3), 0, 6.5f, 1, false);
  }
  dl->AddLine(ImVec2(X(0), Y(0)), ImVec2(X(1), Y(1)), hex(0x4A5E68));
  DrawnCurve c = masterDrawnCurve(st, b);
  ImVec2 pts[121];
  for (int i = 0; i <= 120; ++i) {
    double x = i / 120.0, y;
    if (drawn) {
      y = c.at(x);
    } else {  // parametric: map through the dB curve
      double a01 = (std::pow(9.0, x) - 1.0) * 0.5, inDbv = 20.0 * std::log10(std::max(a01, 1e-6));
      double outDb = masterCurve(st, b, inDbv);
      y = curveXForDb(outDb);
    }
    pts[i] = ImVec2(X(x), Y(std::clamp(y, 0.0, 1.0)));
  }
  dl->AddPolyline(pts, 121, drawn ? kBandCol[b] : hex(0xC9D5D4, 0.6f), 0, f.d(1.5f));
  if (inDb > -90) {
    double x = curveXForDb(inDb);
    double y = drawn ? c.at(x) : curveXForDb(masterCurve(st, b, inDb));
    dl->AddCircleFilled(ImVec2(X(x), Y(y)), f.d(3), hex(0xFFFFFF));
  }
  ImGui::SetCursorScreenPos(p0);
  ImGui::InvisibleButton("##maxcurve", ImVec2(p1.x - p0.x, p1.y - p0.y));
  bool hov = ImGui::IsItemHovered();
  ImGuiIO& io = ImGui::GetIO();
  if (!drawn) {
    skin::label(dl, f, 10, 8, 120, 10, "PARAMETRIC (see detail)", hex(0x9FB3BD), 0, 7.0f, 0, false);
    if (hov) a.setHint("Curve from the thresholds / ratios · choose \"Drawn curve\" in the options to draw it");
    return;
  }
  // Handles.
  auto ptPos = [&](int k) { return ImVec2(X(c.x[k]), Y(c.y[k])); };
  auto tenPos = [&](int k) {  // middle of segment k-1 → k
    double xm = 0.5 * (c.x[k - 1] + c.x[k]);
    return ImVec2(X(xm), Y(c.at(xm)));
  };
  int nearPt = -1, nearTen = -1;
  float best = f.d(7);
  for (int k = 0; k < c.n; ++k) {
    ImVec2 q = ptPos(k);
    float d = std::hypot(io.MousePos.x - q.x, io.MousePos.y - q.y);
    if (d < best) best = d, nearPt = k;
  }
  if (nearPt < 0)
    for (int k = 1; k < c.n; ++k) {
      ImVec2 q = tenPos(k);
      float d = std::hypot(io.MousePos.x - q.x, io.MousePos.y - q.y);
      if (d < best) best = d, nearTen = k;
    }
  if (ImGui::IsItemActivated()) u.dragPt = nearPt, u.dragTen = nearTen;
  if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0)) {
    DrawnCurve e = c;
    if (u.dragPt >= 0) {
      int k = u.dragPt;
      double lo = k == 0 ? 0.0 : e.x[k - 1], hi = k == e.n - 1 ? 1.0 : e.x[k + 1];
      e.x[k] = k == 0 ? 0.0 : std::clamp(static_cast<double>(invX(io.MousePos.x)), lo, hi);
      e.y[k] = invY(io.MousePos.y);
      writeCurve(a, t, st, b, e, "move curve point");
    } else if (u.dragTen > 0) {
      int k = u.dragTen;
      double dir = e.y[k] >= e.y[k - 1] ? 1.0 : -1.0;
      e.t[k] = std::clamp(e.t[k] - dir * io.MouseDelta.y * 0.02, -1.0, 1.0);
      writeCurve(a, t, st, b, e, "curve tension");
    }
  }
  if (ImGui::IsItemDeactivated()) u.dragPt = u.dragTen = -1;
  if (hov && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && nearPt < 0 && c.n < kMsCurvePts) {
    DrawnCurve e = c;
    double x = invX(io.MousePos.x);
    int at = 1;
    while (at < e.n && e.x[at] < x) ++at;
    for (int k = e.n; k > at; --k) e.x[k] = e.x[k - 1], e.y[k] = e.y[k - 1], e.t[k] = e.t[k - 1];
    e.x[at] = x, e.y[at] = invY(io.MousePos.y), e.t[at] = 0;
    ++e.n;
    writeCurve(a, t, st, b, e, "add curve point");
  }
  if (hov && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && nearPt > 0 && c.n > 2) {
    DrawnCurve e = c;
    for (int k = nearPt; k + 1 < e.n; ++k) e.x[k] = e.x[k + 1], e.y[k] = e.y[k + 1], e.t[k] = e.t[k + 1];
    --e.n;
    writeCurve(a, t, st, b, e, "remove curve point");
  }
  for (int k = 1; k < c.n; ++k) {
    ImVec2 q = tenPos(k);
    dl->AddCircle(q, f.d(2.5f), k == nearTen || k == u.dragTen ? hex(0xFFFFFF) : hex(0x9FB3BD, 0.7f), 12, 1.0f);
  }
  for (int k = 0; k < c.n; ++k) {
    ImVec2 q = ptPos(k);
    bool hl = k == nearPt || k == u.dragPt;
    dl->AddCircleFilled(q, f.d(hl ? 4.0f : 3.2f), hl ? hex(0xFFFFFF) : kBandCol[b], 16);
    dl->AddCircle(q, f.d(hl ? 4.0f : 3.2f), hex(0x1D2023), 16, 1.0f);
  }
  if (hov)
    a.setHint(nearPt >= 0 ? "Drag the point · right-click removes it"
              : nearTen > 0 ? "Drag up / down to bend the segment"
                            : "Double-click to add a point (input across, output up; the 0 dB lines cross in the middle)");
}

// ---------------------------------------------------------------- monitor
void monitorArea(App& a, const PluginTarget& t, const PluginState& st, MaxUi& u, const skin::Form& f, const float* gr) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 p0 = f.p(189, 4), p1 = f.p(623, 185);
  dl->AddRectFilled(p0, p1, hex(0x1D2326));
  const int N = 160;
  float db[N];
  pui::spectrum(t.tap(a), t.key(), N, db, a.engine.sampleRate());
  if (u.vis[2]) {
    ImVec2 pts[N];
    for (int k = 0; k < N; ++k)
      pts[k] = ImVec2(p0.x + (p1.x - p0.x) * (k + 0.5f) / N, p1.y - std::clamp((db[k] + 90.0f) / 96.0f, 0.0f, 1.0f) * (p1.y - p0.y));
    for (int k = 0; k + 1 < N; ++k) {
      ImVec2 q[4] = {pts[k], pts[k + 1], ImVec2(pts[k + 1].x, p1.y), ImVec2(pts[k].x, p1.y)};
      dl->AddConvexPolyFilled(q, 4, hex(0x8BFC56, 0.10f));
    }
    dl->AddPolyline(pts, N, hex(0x8BFC56, 0.55f), 0, 1.0f);
  }
  pui::drawFreqGrid(dl, p0, p1, 24, false);
  const bool refm = ui::ival(st, MsRef0 + MrModel) == 1;  // J-35: reference engine stores the raw split values
  double fl = refm ? maxref::splitHz(ui::val(st, MsRef0 + MrRaw0)) : ui::val(st, MsLowFreq);
  double fh = refm ? maxref::splitHz(ui::val(st, MsRef0 + MrRaw0 + 2)) : ui::val(st, MsHighFreq);
  float x1 = pui::freqToX(fl, p0.x, p1.x - p0.x), x2 = pui::freqToX(fh, p0.x, p1.x - p0.x);
  float xs[4] = {p0.x, x1, x2, p1.x};
  for (int b = 0; b < 3; ++b) {
    if (b == u.band) dl->AddRectFilled(ImVec2(xs[b], p0.y), ImVec2(xs[b + 1], p1.y), theme::withAlpha(kBandCol[b], 0.08f));
    if (u.vis[4]) {
      float hh = std::min(1.0f, -gr[b] / 24.0f) * (p1.y - p0.y - 24);
      if (hh > 1) dl->AddRectFilled(ImVec2(xs[b] + 5, p0.y + 4), ImVec2(xs[b + 1] - 5, p0.y + 4 + hh), theme::withAlpha(kBandCol[b], 0.35f), 2);
    }
    skin::label(dl, f, (xs[b] - f.o.x) / f.s + 4, 170, 50, 10, kBand[b], b == u.band ? kBandCol[b] : hex(0x7F8A8E), 0, 7.5f, 0);
  }
  // J-68: scrolling gain envelopes of all four bands in their colours (the
  // reference's main monitor layer): newest at the right, 0 dB at the top,
  // −24 dB at the bottom; sampled from the engine's per-block reduction.
  if (u.vis[5]) {
    struct GrHist {
      enum { N = 512 };
      float v[4][N] = {};
      int head = 0;
      double last = -1;
    };
    static std::map<uint64_t, GrHist> hists;
    GrHist& h = hists[t.key()];
    const double now = ImGui::GetTime();
    if (h.last < 0) h.last = now;
    for (int guard = 0; now - h.last >= 1.0 / 60.0 && guard < 8; ++guard) {
      h.last += 1.0 / 60.0;
      h.head = (h.head + 1) % static_cast<int>(GrHist::N);
      for (int b = 0; b < 4; ++b) h.v[b][h.head] = std::min(0.0f, gr[b]);
    }
    if (now - h.last > 1.0) h.last = now;
    const float w = p1.x - p0.x;
    const int cols = std::min(static_cast<int>(GrHist::N), static_cast<int>(w / 1.5f));
    const float dx = w / static_cast<float>(cols);
    for (int b = 0; b < 4; ++b) {
      ImVec2 prev;
      for (int i = 0; i < cols; ++i) {
        const float v = h.v[b][(h.head - i + static_cast<int>(GrHist::N)) % static_cast<int>(GrHist::N)];
        const ImVec2 q(p1.x - static_cast<float>(i) * dx, p0.y + 2 + std::clamp(-v / 24.0f, 0.0f, 1.0f) * (p1.y - p0.y - 16));
        if (i) dl->AddLine(prev, q, theme::withAlpha(kBandCol[b], b == u.band ? 0.95f : 0.55f), f.d(b == u.band ? 1.4f : 1.0f));
        prev = q;
      }
    }
  }
  // Master reduction strip on the right edge.
  float hm = std::min(1.0f, -gr[3] / 24.0f) * (p1.y - p0.y);
  if (hm > 1) dl->AddRectFilled(ImVec2(p1.x - f.d(6), p0.y), ImVec2(p1.x - f.d(1), p0.y + hm), theme::withAlpha(kBandCol[3], 0.7f));
  ImGui::SetCursorScreenPos(p0);
  ImGui::InvisibleButton("##maxmon", ImVec2(p1.x - p0.x, p1.y - p0.y));
  ImGuiIO& io = ImGui::GetIO();
  static int drag = -1;
  if (ImGui::IsItemActivated()) {
    drag = std::fabs(io.MousePos.x - x1) < f.d(6) ? 0 : std::fabs(io.MousePos.x - x2) < f.d(6) ? 1 : -1;
    if (drag < 0) u.band = io.MousePos.x < x1 ? 0 : io.MousePos.x < x2 ? 1 : 2;
  }
  if (ImGui::IsItemActive() && drag >= 0 && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 1))
  {
    const double hz = pui::xToFreq(io.MousePos.x, p0.x, p1.x - p0.x);
    if (refm)
      a.doc.setParam(t.pid(MsRef0 + MrRaw0 + (drag ? 2 : 0)), std::clamp(std::round(std::log2(hz / 30.0) / 0.000138227748), 0.0, 65536.0), true);
    else
      a.doc.setParam(t.pid(drag ? MsHighFreq : MsLowFreq), hz, true);
  }
  if (ImGui::IsItemDeactivated()) a.doc.endGesture(), drag = -1;
  for (float x : {x1, x2}) dl->AddLine(ImVec2(x, p0.y), ImVec2(x, p1.y), hex(0xFFFFFF, 0.5f), f.d(1.0f));
  skin::hint(a, "Output spectrum and bands · click a band to edit it · drag a split line to move it");
}

// Display-layer glyphs: SVG icons on the old glyphs' grid (half the glyph box per unit).
void glyphDots(ImDrawList* dl, ImVec2 c, float s, ImU32 col) { ::gl::ui::drawIconGlyph(dl, ::gl::ui::Icon::DisplayDots, c, s * 0.5f, 0.0f, col); }
void glyphBars(ImDrawList* dl, ImVec2 c, float s, ImU32 col) { ::gl::ui::drawIconGlyph(dl, ::gl::ui::Icon::DisplayBars, c, s * 0.47f, 0.0f, col); }  // a shade inside: the bars sit on part pixels at 1.5x
void glyphLine(ImDrawList* dl, ImVec2 c, float s, ImU32 col) { ::gl::ui::drawIconGlyph(dl, ::gl::ui::Icon::DisplayLine, c, s * 0.5f, 1.3f, col); }

}  // namespace

// ===================================================================== gl.master (reference layout, 626 x 385)
void editorMasterFl(App& a, const PluginTarget& t, float width) {
  const PluginState st = *t.state(a.doc.song());
  ImDrawList* dl = ImGui::GetWindowDrawList();
  MaxUi& u = maxUi(t.key());
  float gr[kMsBands] = {0, 0, 0, 0};
  float inLevel = -120;
  if (auto rt = ui::runtime(a, t)) {
    for (int b = 0; b < kMsBands; ++b) gr[b] = rt->bands[b].load();
    inLevel = rt->input.load();
  }
  const int b = std::clamp(u.band, 0, 3);
  auto P = [&](int k) { return msParam(b, k); };
  auto X = [&](int k) { return msExt(b, k); };
  // J-35: with the reference engine every control edits the stored integers.
  const bool ref = ui::ival(st, MsRef0 + MrModel) == 1;
  auto RB = [&](int tag) { return MsRef0 + MrRaw0 + maxref::rawIndex(b, tag); };
  auto RG = [&](int g) { return MsRef0 + MrRaw0 + g; };
  auto S = [&](int legacy, int refParam) { return ref ? refParam : legacy; };
  const bool drawn = ref || ui::ival(st, X(MxCurveMode)) == 1;
  // Palette roles.
  const ImU32 panel = hex(0x566065), lbl = hex(0xC9D5D4), lblBg = hex(0x31373B);
  skin::WheelLook wPre{hex(0x3F484D), hex(0x6543AA), hex(0x8758C4), hex(0x3F484D)};
  skin::WheelLook wPost{hex(0x3F484D), hex(0x23889D), hex(0x00CAF4), hex(0x3F484D)};
  skin::WheelLook wSat{hex(0x3F484D), hex(0xCE4679), hex(0xE54E87), hex(0x3F484D)};
  skin::WheelLook wEnv{hex(0x3F484D), hex(0x677576), hex(0x829293), hex(0x3F484D)};
  skin::WheelLook wLow{hex(0x3F484D), hex(0xB86D68), hex(0xCF7A74), hex(0x3F484D)};
  skin::WheelLook wHigh{hex(0x3F484D), hex(0xCED671), hex(0xDDE67A), hex(0x3F484D)};
  skin::WheelLook wCut{hex(0x3F484D), hex(0xC17D33), hex(0xD68A38), hex(0x3F484D)};
  skin::DigiLook digi{hex(0x6A787B), hex(0xFFFFFF)};
  skin::Form f = skin::formAt(626, 385, width);
  dl->AddRectFilled(f.p(0, 0), f.p(626, 385), panel);
  skin::gradient(dl, f, 0, 207, 626, 178, skin::lighten(panel, 0.05f), skin::darken(panel, 0.15f));
  skin::bevel(dl, f, 0, 0, 626, 208, hex(0x2B3136), skin::BevelPlain);
  skin::bevel(dl, f, 253, 207, 242, 178, hex(0x697070), skin::BevelPlain);
  skin::bevel(dl, f, 427, 208, 199, 177, hex(0x3E4546), skin::BevelPlain);
  skin::bevel(dl, f, 139, 290, 108, 73, hex(0x4F5859), skin::BevelRounded);
  skin::bevel(dl, f, 6, 328, 66, 35, hex(0x404748), skin::BevelRounded);

  // J-68: compression indicators at both panel edges (arrow shapes): filled
  // with the selected band's colour, deeper with its live gain reduction.
  {
    const float k = std::clamp(-gr[b] / 12.0f, 0.0f, 1.0f);
    for (int side = 0; side < 2; ++side) {
      const float x0 = side ? 616.0f : 0.0f, x1 = x0 + 10, tip = side ? x0 : x1, base = side ? x1 : x0;
      ImVec2 pts[5] = {f.p(base, 260), f.p(base, 383), f.p((x0 + x1) * 0.5f, 383), f.p(tip, 321.5f), f.p((x0 + x1) * 0.5f, 260)};
      dl->AddConvexPolyFilled(pts, 5, hex(0x3A4245));
      if (k > 0.01f) dl->AddConvexPolyFilled(pts, 5, theme::withAlpha(kBandCol[b], 0.25f + 0.6f * k));
    }
  }
  // ---- top: curve editor + monitor + display options
  float inDb = b == 3 ? inLevel : -120.f;
  curveArea(a, t, st, u, f, inDb);
  monitorArea(a, t, st, u, f, gr);
  if (skin::menuButton(f, "mxopt", 191, 188, 15, 17, hex(0xFFBC40))) ImGui::OpenPopup("##maxopt");
  if (ImGui::BeginPopup("##maxopt")) {
    ImGui::TextDisabled(i18n::tr("%s band"), kBand[b]);
    if (ImGui::MenuItem(i18n::tr("Reference engine"), nullptr, ref)) pui::setParamValue(a, t, MsRef0 + MrModel, ref ? 0.0 : 1.0);
    if (!ref && ImGui::MenuItem(i18n::tr("Drawn curve engine"), nullptr, drawn)) pui::setParamValue(a, t, X(MxCurveMode), drawn ? 0.0 : 1.0);
    if (ImGui::MenuItem(i18n::tr("Drawn curve engine on all bands"))) {
      PluginState ns = st;
      normalizePluginState(ns);
      for (int k = 0; k < kMsBands; ++k) ns.params[static_cast<size_t>(msExt(k, MxCurveMode))] = 1.f;
      setPluginState(a, t, ns, "drawn curve engine");
    }
    if (ImGui::MenuItem(i18n::tr("Reset this band's curve"))) {
      DrawnCurve c;
      writeCurve(a, t, st, b, c, "reset curve");
    }
    ImGui::Separator();
    int os = ui::ival(st, MsOversample);
    const char* osn[3] = {"Saturation oversampling: off", "Saturation oversampling: 2x", "Saturation oversampling: 4x"};
    for (int k = 0; k < 3; ++k)
      if (ImGui::MenuItem(osn[k], nullptr, os == k)) pui::setParamValue(a, t, MsOversample, k);
    ImGui::EndPopup();
  }
  skin::glyphToggle(f, "mvi", 206, 188, 16, 17, u.vis[0], hex(0x609E98), glyphDots, "Show input level on the curve");
  skin::glyphToggle(f, "mvs", 222, 188, 16, 17, u.vis[2], hex(0x8BFC56), glyphLine, "Show the output spectrum");
  skin::glyphToggle(f, "mvg", 238, 188, 16, 17, u.vis[4], hex(0xDD4AEB), glyphBars, "Show band gain reduction");
  skin::glyphToggle(f, "mve", 254, 188, 16, 17, u.vis[5], hex(0xC9B465), glyphLine, i18n::tr("Show the bands' gain envelopes over time"));
  skin::label(dl, f, 382, 187, 89, 19, "SPEED", lbl, hex(0x1D2023), 7.5f, 0);
  skin::slider(f, "mxspeed", 413, 190, 58, 12, u.speed, hex(0x474F50), hex(0x8B9294), "Monitor speed");

  // ---- band selector (LOW / MID / HIGH / MASTER) + mode + solo + stereo
  static const char* bands[4] = {"LOW", "MID", "HIGH", "MASTER"};
  skin::TabLook bt{hex(0x4B5457), hex(0x2B3136), hex(0xA9B3B6), hex(0xFFFFFF)};
  skin::tabsUi(f, "maxband", 2, 210, 69, 96, bands, 4, u.band, bt, true);
  if (ref) {  // reference modes: compress / no compression / mute / band off
    static const char* modes4[4] = {"ON", "OFF", "MUTE", "BYP"};
    int cell = std::clamp(ui::ival(st, RB(maxref::TMode)), 0, 3);
    skin::TabLook ml{hex(0x858B8C), hex(0x2F383D), hex(0x1D2023), hex(0xFFFFFF)};
    if (skin::tabsUi(f, "maxmode", 77, 218, 64, 56, modes4, 4, cell, ml, true)) pui::setParamValue(a, t, RB(maxref::TMode), cell);
  } else {
    static const char* modes[3] = {"ON", "OFF", "MUTE"};
    int mv = ui::ival(st, P(MbMode));  // 0 bypass, 1 active, 2 mute, 3 no compression, 4 merged
    int cell = mv == 1 ? 0 : mv == 2 ? 2 : 1;
    skin::TabLook ml{hex(0x858B8C), hex(0x2F383D), hex(0x1D2023), hex(0xFFFFFF)};
    // OFF = compression off (gains and saturation stay), as on the reference.
    if (skin::tabsUi(f, "maxmode", 77, 218, 64, 56, modes, 3, cell, ml, true)) pui::setParamValue(a, t, P(MbMode), cell == 0 ? 1 : cell == 1 ? 3 : 2);
  }
  {
    bool solo = ref ? (b < 3 && ui::ival(st, MsRef0 + MrSolo) == 1 && ui::ival(st, MsRef0 + MrTarget) == b) : (b < 3 && ui::ival(st, MsSolo) == b + 1);
    ImVec2 c0 = f.p(12, 339), c1 = f.p(56, 355);
    ImGui::SetCursorScreenPos(c0);
    if (ImGui::InvisibleButton("##maxsolo", ImVec2(c1.x - c0.x, c1.y - c0.y)) && b < 3) {
      if (ref) {
        PluginState ns = st;
        normalizePluginState(ns);
        ns.params[static_cast<size_t>(MsRef0 + MrSolo)] = solo ? 0.f : 1.f;
        ns.params[static_cast<size_t>(MsRef0 + MrTarget)] = static_cast<float>(b);
        setPluginState(a, t, ns, "solo band");
      } else {
        pui::setParamValue(a, t, MsSolo, solo ? 0 : b + 1);
      }
    }
    skin::hint(a, "Solo the edited band");
    dl->AddRectFilled(ImVec2(c0.x, c0.y + f.d(3)), ImVec2(c0.x + f.d(10), c0.y + f.d(13)), solo ? hex(0xBD4EC7) : hex(0x2B3136), f.d(2));
    skin::label(dl, f, 24, 339, 32, 16, "SOLO", lbl, 0, 8.0f, 0);
  }
  skin::wheel(a, t, S(P(MbStereo), RB(maxref::TSep)), f, 83, 302, 40, 40, wEnv);
  skin::label(dl, f, 79, 343, 48, 10, "STEREO", lbl, 0, 7.0f);
  // ---- gain + saturation
  skin::wheel(a, t, S(P(MbPre), RB(maxref::TPre)), f, 143, 218, 46, 46, wPre);
  skin::wheel(a, t, S(P(MbPost), RB(maxref::TPost)), f, 197, 218, 46, 46, wPost);
  skin::label(dl, f, 151, 264, 30, 15, "PRE", lbl, lblBg, 8.0f);
  skin::label(dl, f, 181, 264, 26, 15, "GAIN", hex(0x1D2023), hex(0xC9D5D4), 7.5f);
  skin::label(dl, f, 207, 264, 30, 15, "POST", lbl, lblBg, 8.0f);
  skin::wheel(a, t, S(P(MbSatThresh), RB(maxref::TSatThr)), f, 143, 296, 46, 44, wSat);
  skin::wheel(a, t, S(P(MbSatCeil), RB(maxref::TSatCeil)), f, 197, 296, 46, 44, wSat);
  skin::label(dl, f, 151, 341, 30, 15, "THRES", lbl, lblBg, 7.5f);
  skin::label(dl, f, 182, 341, 26, 15, "SAT", hex(0x1D2023), hex(0xC9D5D4), 7.5f);
  skin::label(dl, f, 207, 341, 30, 15, "CEIL", lbl, lblBg, 8.0f);
  if (!ref) skin::switchSel(a, t, X(MxSatMode), f, 160, 357, 66, 6, hex(0x272C2E), hex(0xCE4679));
  // ---- envelope
  skin::wheel(a, t, S(P(MbAttack), RB(maxref::TAtt)), f, 258, 218, 46, 46, wEnv);
  skin::wheel(a, t, S(P(MbRelease), RB(maxref::TRel)), f, 318, 218, 46, 46, wEnv);
  skin::wheel(a, t, S(P(MbWindow), RB(maxref::TWindow)), f, 370, 218, 46, 46, wEnv);
  skin::label(dl, f, 266, 264, 30, 15, "ATT", lbl, lblBg, 8.0f);
  skin::label(dl, f, 299, 264, 26, 15, "ENV", hex(0x1D2023), hex(0xC9D5D4), 7.5f);
  skin::label(dl, f, 326, 264, 30, 15, "REL", lbl, lblBg, 8.0f);
  skin::label(dl, f, 368, 264, 50, 15, "SUSTAIN", lbl, lblBg, 7.5f);
  skin::digi(a, t, S(X(MxAtkCurve), RB(maxref::TAttCurve)), f, 267, 291, 28, 23, digi);
  skin::digi(a, t, S(X(MxRelCurve), RB(maxref::TRelCurve)), f, 327, 291, 28, 23, digi);
  skin::label(dl, f, 296, 294, 32, 15, "CURVE", lbl, 0, 7.0f);
  static const char* envModes[2] = {"PEAK", "RMS"};
  skin::switchSel(a, t, S(X(MxEnvMode), RB(maxref::TSusMode)), f, 373, 292, 54, 28, hex(0x272C2E), hex(0xB9BFC3), envModes);
  skin::wheel(a, t, S(X(MxRelease2), RB(maxref::TRel2)), f, 258, 330, 46, 40, wEnv);
  skin::label(dl, f, 264, 369, 34, 12, "REL 2", lbl, lblBg, 7.5f);
  if (!drawn) {  // J-128: three lines in the 110 x 40 box (one line was cut off)
    static const char* note[3] = {"Curve, Rel 2 and", "Peak/RMS apply", "to drawn curves"};
    for (int i = 0; i < 3; ++i) skin::label(dl, f, 312, 331 + i * 12.5f, 110, 12.5f, note[i], hex(0xC9D5D4, 0.6f), 0, 7.0f, 1, false);
  }
  // ---- global
  skin::wheel(a, t, S(MsLookahead, RG(4)), f, 437, 218, 46, 46, wEnv);
  skin::label(dl, f, 435, 264, 50, 15, "LMH DEL", lbl, lblBg, 7.5f);
  skin::wheel(a, t, S(MsBandMix, RG(5)), f, 437, 295, 46, 44, wEnv);
  skin::label(dl, f, 435, 340, 50, 15, "LMH MIX", lbl, lblBg, 7.5f);
  skin::wheel(a, t, S(MsLowFreq, RG(0)), f, 508, 218, 46, 46, wLow);
  skin::wheel(a, t, S(MsHighFreq, RG(2)), f, 568, 218, 46, 46, wHigh);
  skin::label(dl, f, 513, 264, 34, 15, "LOW", lbl, lblBg, 8.0f);
  skin::label(dl, f, 547, 264, 30, 15, "FREQ", hex(0x1D2023), hex(0xC9D5D4), 7.5f);
  skin::label(dl, f, 577, 264, 34, 15, "HIGH", lbl, lblBg, 8.0f);
  static const char* slopes[3] = {"6", "12", "24"};
  static const char* slopes2[2] = {"12", "24"};
  skin::switchSel(a, t, S(MsLowSlope, RG(1)), f, 500, 292, 40, 30, hex(0x272C2E), hex(0xB9BFC3), ref ? slopes2 : slopes);
  skin::switchSel(a, t, S(MsHighSlope, RG(3)), f, 580, 292, 40, 30, hex(0x272C2E), hex(0xB9BFC3), ref ? slopes2 : slopes);
  skin::wheel(a, t, S(MsLowCut, RG(6)), f, 511, 330, 40, 40, wCut);
  skin::label(dl, f, 505, 369, 52, 15, "LOW CUT", lbl, lblBg, 7.5f);
  {  // store / flip spare state
    const ImU32 fg = hex(0xB9C5C4);
    if (skin::button(f, "mxstore", 569, 356, 24, 24, "S", 0, fg, "Store in spare state")) u.spare = st.params;
    if (skin::button(f, "mxflip", 595, 356, 24, 24, "A/B", 0, fg, "Flip with spare state (A/B compare)") && !u.spare.empty()) {
      PluginState ns = st;
      std::vector<float> keep = st.params;
      ns.params = u.spare;
      u.spare = keep;
      normalizePluginState(ns);
      setPluginState(a, t, ns, "flip with spare state");
    }
  }
  skin::reserve(f);
}

// ===================================================================== gl.enhance (reference layout, 210 x 294)
// A big amount wheel over the A-D selector (the reference's simple view).
void editorEnhanceFl(App& a, const PluginTarget& t, float width) {
  const PluginState st = *t.state(a.doc.song());
  ImDrawList* dl = ImGui::GetWindowDrawList();
  if (ui::ival(st, EnCustom) == 2) {  // J-26 peak emphasis panel (210 x 294)
    const ImVec2 start = ImGui::GetCursorScreenPos();
    skin::Form f = skin::formAt(210, 294, std::min(width, 340.0f));
    f.o.x += std::max(0.0f, (width - f.w) * 0.5f);
    dl->AddRectFilled(f.p(0, 0), f.p(210, 294), hex(0x2E3438), f.d(6));
    skin::gradient(dl, f, 0, 0, 210, 294, hex(0x384046), hex(0x23282B));
    skin::WheelLook w{hex(0x3F484D), hex(0xE8C267), hex(0xF6D27A), hex(0x4A535A)};
    w.railWidth = 0.09f;
    w.dents = 8;
    skin::wheel(a, t, EnPeakEmph, f, 45, 26, 120, 120, w);
    char buf[48];
    std::snprintf(buf, sizeof buf, "EMPHASIS %d%%", static_cast<int>(std::lround(ui::val(st, EnPeakEmph) * 100)));
    skin::label(dl, f, 25, 150, 160, 16, buf, hex(0xE8E2D2), 0, 10.0f);
    skin::wheel(a, t, EnPeakIn, f, 22, 176, 54, 54, w);
    skin::wheel(a, t, EnPeakOut, f, 134, 176, 54, 54, w);
    std::snprintf(buf, sizeof buf, "IN %.1f dB", ui::val(st, EnPeakIn));
    skin::label(dl, f, 10, 232, 78, 12, buf, hex(0xA9B3B6), 0, 7.5f);
    const double og = ui::val(st, EnPeakOut);
    if (og <= 0) std::snprintf(buf, sizeof buf, "OUT -inf");
    else std::snprintf(buf, sizeof buf, "OUT %.1f dB", 20 * std::log10(og));
    skin::label(dl, f, 122, 232, 78, 12, buf, hex(0xA9B3B6), 0, 7.5f);
    auto tog = [&](const char* id, int param, float x, const char* txt) {
      const bool on = ui::val(st, param) >= 0.5f;
      if (skin::button(f, id, x, 252, 60, 16, txt, on ? hex(0xE8C267) : hex(0x3A4247), on ? hex(0x1D2023) : hex(0xA9B3B6), nullptr))
        pui::setParamValue(a, t, param, on ? 0 : 1);
    };
    tog("emsoft", EnPeakSoft, 8, ui::val(st, EnPeakSoft) >= 0.5f ? "SOFT CLIP" : "HARD CLIP");
    tog("emmatch", EnPeakMatch, 75, "LEVEL MATCH");
    tog("embyp", EnPeakBypass, 142, "BYPASS");
    if (skin::button(f, "enbuilt", 75, 274, 60, 14, "VOICINGS", hex(0x3A4247), hex(0xA9B3B6), "Use Gridline's built-in voicings"))
      pui::setParamValue(a, t, EnCustom, 0);
    ImGui::SetCursorScreenPos(start);
    ImGui::Dummy(ImVec2(width, f.h));
    return;
  }
  float gr[kMsBands] = {0, 0, 0, 0};
  if (auto rt = ui::runtime(a, t))
    for (int b = 0; b < kMsBands; ++b) gr[b] = rt->bands[b].load();
  const ImVec2 start = ImGui::GetCursorScreenPos();
  skin::Form f = skin::formAt(210, 294, std::min(width, 340.0f));
  f.o.x += std::max(0.0f, (width - f.w) * 0.5f);  // centre the panel in a wider window
  dl->AddRectFilled(f.p(0, 0), f.p(210, 294), hex(0x2E3438), f.d(6));
  skin::gradient(dl, f, 0, 0, 210, 294, hex(0x384046), hex(0x23282B));
  skin::WheelLook w{hex(0x3F484D), hex(0xE8C267), hex(0xF6D27A), hex(0x4A535A)};
  w.railWidth = 0.09f;
  w.dents = 8;
  skin::wheel(a, t, EnAmount, f, 23, 30, 164, 164, w);
  char buf[32];
  std::snprintf(buf, sizeof buf, "%d%%", static_cast<int>(std::lround(ui::val(st, EnAmount) * 100)));
  skin::label(dl, f, 70, 196, 70, 18, buf, hex(0xE8E2D2), 0, 12.0f);
  static const char* modes[4] = {"A", "B", "C", "D"};
  skin::TabLook tl{hex(0x3A4247), hex(0xE8C267), hex(0xA9B3B6), hex(0x1D2023)};
  skin::tabs(a, t, EnMode, f, 25, 226, 160, 30, modes, 4, tl);
  const bool custom = ui::ival(st, EnCustom) == 1;
  skin::label(dl, f, 25, 260, 160, 12, custom ? "IMPORTED PROGRAM" : "BUILT-IN VOICING", hex(0x8E989D), 0, 7.5f);
  // Small activity strip (band reductions).
  for (int b = 0; b < 4; ++b) {
    ImVec2 g0 = f.p(40 + b * 34, 276), g1 = f.p(64 + b * 34, 282);
    dl->AddRectFilled(g0, g1, hex(0x1D2023), f.d(1));
    float k = std::clamp(-gr[b] / 18.0f, 0.0f, 1.0f);
    if (k > 0) dl->AddRectFilled(g0, ImVec2(g0.x + (g1.x - g0.x) * k, g1.y), kBandCol[b], f.d(1));
  }
  if (custom) {
    if (skin::button(f, "enbuilt", 150, 6, 54, 14, "BUILT-IN", hex(0x3A4247), hex(0xA9B3B6), "Use Gridline's built-in voicings instead of the imported program"))
      pui::setParamValue(a, t, EnCustom, 0);
  }
  ImGui::SetCursorScreenPos(start);
  ImGui::Dummy(ImVec2(width, f.h));
}

}  // namespace gl::packs::dynamics
