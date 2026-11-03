// Gridline — "dynamics" pack: the editors of Crest (gl.master; one grid,
// masterfl_layout.h, with the palette taken from the theme's roles) and Enhance
// (gl.enhance, reference-layout), drawn with Gridline's skin kit.
#include "../../core/I18n.h"
#include "maximus_ref.h"
#include <cfloat>
#include <cstdlib>
#include <cstring>
#include <map>
#include "../../app/LayoutAudit.h"
#include "../../app/PluginEditorsDynFl.h"
#include "../../app/PluginSkin.h"
#include "../../app/imgui_hooks/ImGuiItemHook.h"
#include "../../../third_party/imgui/imgui_internal.h"
#include "dyn_widgets.h"
#include "masterfl_layout.h"

namespace gl::packs::dynamics {

using skin::hex;
using i18n::tr;

namespace {

const char* kBand[4] = {"LOW", "MID", "HIGH", "MASTER"};

// ---------------------------------------------------------------- palette
// Crest's roles for the current theme, on the same surfaces as the flagship editors.
constexpr float kCapPx = 10.0f, kTextPx = 10.0f, kTinyPx = 9.0f;  // form px: captions, values on displays, axis figures
struct Pal {
  ImU32 frame, panel, head, well, wellHi, field, grid, gridStrong, line;
  ImU32 text, label, dim;
  ImU32 accent, select, selFace, selText, neutral;
  ImU32 face, faceOn, handle, rail, cap, pointer, plate;
};
float lum(ImU32 c) {
  auto ch = [](uint32_t v) {
    const float q = static_cast<float>(v & 0xFF) / 255.0f;
    return q <= 0.04045f ? q / 12.92f : std::pow((q + 0.055f) / 1.055f, 2.4f);
  };
  return 0.2126f * ch(c >> IM_COL32_R_SHIFT) + 0.7152f * ch(c >> IM_COL32_G_SHIFT) + 0.0722f * ch(c >> IM_COL32_B_SHIFT);
}
float contrast(ImU32 fg, ImU32 bg) {
  const float x = lum(fg), y = lum(bg);
  return (std::max(x, y) + 0.05f) / (std::min(x, y) + 0.05f);
}
// `fg` moved toward `to` until it reads at `target`:1 on the surfaces.
ImU32 lift(ImU32 fg, ImU32 to, float target, std::initializer_list<ImU32> bg) {
  auto worst = [&](ImU32 c) {
    float w = 1e9f;
    for (ImU32 q : bg) w = std::min(w, contrast(c, q));
    return w;
  };
  ImU32 c = fg;
  for (int i = 1; i <= 20 && worst(c) < target; ++i) c = theme::mix(fg, to, static_cast<float>(i) * 0.05f);
  return c;
}
Pal crestPal() {
  const theme::Palette& T = theme::pal();
  const bool d = T.dark;
  Pal p;
  p.frame = T.surface900;
  p.panel = T.surface800;
  p.head = theme::mix(T.surface800, T.surface700, d ? 0.55f : 0.35f);
  p.well = T.well;
  p.wellHi = theme::mix(T.well, T.surface900, 0.5f);
  p.field = T.field;
  p.grid = theme::mix(T.well, T.text, d ? 0.07f : 0.09f);
  p.gridStrong = theme::mix(T.well, T.text, d ? 0.16f : 0.2f);
  p.line = d ? T.line : T.lineStrong;
  p.text = T.text;
  p.label = lift(theme::mix(T.text, T.textDim, 0.45f), T.text, 4.5f, {p.panel, p.head, p.frame});
  p.dim = lift(T.textDim, T.text, 4.5f, {p.panel, p.well});
  p.accent = theme::litRole(theme::role::kMenu);  // Crest's family role, as in its brand mark
  p.select = p.accent;
  p.selFace = theme::mix(T.field, p.accent, d ? 0.26f : 0.2f);
  p.selText = lift(p.accent, T.text, 4.5f, {p.selFace});
  p.neutral = d ? theme::mix(T.knobFill, T.text, 0.2f) : theme::mix(T.textDim, T.text, 0.3f);
  p.face = d ? theme::mix(T.surface800, T.surface700, 0.8f) : T.surface700;
  p.faceOn = d ? T.surface600 : T.surface600;
  p.handle = d ? T.surface600 : T.textDim;
  p.rail = d ? T.field : T.surface600;
  p.cap = d ? T.surface600 : T.surface800;
  p.pointer = d ? theme::mix(T.text, hex(0xFFFFFF), 0.5f) : T.text;
  p.plate = theme::withAlpha(T.well, 0.88f);
  return p;
}
ImU32 bandCol(int b, const Pal& P) {
  static const ImU32 base[4] = {hex(0xCF7A74), hex(0xD9C25F), hex(0xCED671), hex(0xC9D5D4)};
  return b == 3 ? (theme::isDark() ? base[3] : P.text) : theme::lit(base[b]);
}
ImU32 bandText(int b, const Pal& P) { return lift(bandCol(b, P), P.text, 4.5f, {P.panel, P.field}); }

// A display: the well with its inner shade and border.
void wellFrame(ImDrawList* dl, const skin::Form& f, ImVec2 p0, ImVec2 p1, const Pal& P) {
  audit::group(p0, p1, "display");
  skin::gradientRect(dl, p0, p1, P.well, P.wellHi, f.d(theme::kRadiusSm));
  dl->AddRectFilledMultiColor(p0, ImVec2(p1.x, p0.y + f.d(4)), IM_COL32(0, 0, 0, theme::isDark() ? 60 : 18), IM_COL32(0, 0, 0, theme::isDark() ? 60 : 18), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0));
  dl->AddRect(p0, p1, P.line, f.d(theme::kRadiusSm));
}
// A group of the deck: a raised panel, with a header strip when it has a title.
void drawCell(ImDrawList* dl, const skin::Form& f, const crest::Cell& c, const char* title, const Pal& P) {
  const ImVec2 p0 = f.p(c.r.x, c.r.y), p1 = f.p(c.r.x1(), c.r.y1());
  audit::group(p0, p1, "cell");
  dl->AddRectFilled(p0, p1, P.panel, f.d(theme::kRadiusSm));
  if (!title) return;
  const ImVec2 h1 = f.p(c.r.x1(), c.r.y + crest::kHeadH);
  skin::gradientRect(dl, p0, h1, skin::lighten(P.head, theme::isDark() ? 0.04f : 0.03f), P.head, f.d(theme::kRadiusSm), ImDrawFlags_RoundCornersTop);
  dl->AddLine(ImVec2(p0.x, h1.y), h1, P.frame, std::max(1.0f, f.d(1.2f)));
  skin::label(dl, f, c.r.x, c.r.y, c.r.w, crest::kHeadH, title, P.text, 0, kCapPx, 1, true);
}
// The caption of a grid crossing: all of them on the row's one line, in one size.
void captionAt(ImDrawList* dl, const skin::Form& f, const crest::Rect& r, const char* text, const Pal& P) {
  skin::label(dl, f, r.x, r.y, r.w, r.h, text, P.label, 0, kCapPx, 1, true);
}
// A push button of the editor, tinted in `onCol` while lit.
bool flatButton(const skin::Form& f, const char* id, const crest::Rect& r, const char* text, bool on, const Pal& P, ImU32 onCol, const char* hint) {
  const ImU32 bg = on ? theme::mix(P.field, onCol, theme::isDark() ? 0.3f : 0.22f) : P.face;
  const ImU32 fg = on ? lift(onCol, P.text, 4.5f, {bg}) : P.label;
  return skin::button(f, id, r.x, r.y, r.w, r.h, text, bg, fg, hint);
}
// One band tab: its name, a stripe in its colour while selected, and what the band takes off now.
bool bandTabButton(const skin::Form& f, int band, const crest::Rect& r, bool selected, float gain, const Pal& P) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 p0 = f.p(r.x, r.y), p1 = f.p(r.x1(), r.y1());
  ImGui::PushID(band);
  ImGui::SetCursorScreenPos(p0);
  const bool clicked = ImGui::InvisibleButton("##maxband", ImVec2(p1.x - p0.x, p1.y - p0.y));
  const bool hov = ImGui::IsItemHovered();
  ImGui::PopID();
  const float rad = f.d(theme::kRadius);
  if (selected) skin::insetField(dl, p0, p1, P.field, rad);
  else skin::raisedFace(dl, p0, p1, hov ? skin::lighten(P.face, 0.06f) : P.face, rad);
  if (selected) dl->AddRectFilled(ImVec2(p0.x + f.d(1), p0.y + f.d(3)), ImVec2(p0.x + f.d(4), p1.y - f.d(3)), bandCol(band, P), f.d(1));
  const float barH = 4, barPad = 8;
  skin::label(dl, f, r.x, r.y + 2, r.w, r.h - barH - barPad, kBand[band], selected ? bandText(band, P) : P.label, 0, 11.0f, 1, true);
  const ImVec2 g0 = f.p(r.x + barPad, r.y1() - barH - barPad * 0.75f), g1 = f.p(r.x1() - barPad, r.y1() - barPad * 0.75f);
  dl->AddRectFilled(g0, g1, selected ? P.well : P.field, f.d(1));
  const float q = std::clamp(-gain / 18.0f, 0.0f, 1.0f);
  if (q > 0.002f) dl->AddRectFilled(ImVec2(g1.x - (g1.x - g0.x) * q, g0.y), g1, bandCol(band, P), f.d(1));
  return clicked;
}

struct MaxUi {
  int band = 3;
  bool init = false;
  int dragPt = -1, dragTen = -1, menuPt = -1;
  bool vis[8] = {true, true, true, true, true, true, true, true};
  float speed = 0.5f;
  std::vector<float> spare;
  bool lockSpare = false;
  // Monitor (reference engine).
  int view = 0;  // 0 monitor, 1 bands
  bool paused = false;
  uint32_t lastHead = 0;
  double headF = 0;
  std::vector<float> cache;
  // Spectrum and heatmap.
  bool hist = true, heat = false;
  int histRange = 1, histSlope = 3, histSmooth = 1, histSource = 0, heatPos = 2;
  std::vector<float> spec, heatMap;
  int heatHead = 0;
  double heatLast = 0;
  // Motion.
  float grShow[kMsBands] = {0, 0, 0, 0}, dot = 0;
  // Dev (GL_CREST_DEV, screenshots and tests): a menu opened on the first frames (1 options, 2 point, 3 linear phase).
  int devMenu = 0, devMenuFrames = 0;
  bool devDemo = false;  // a frozen picture of traces and gains (the offscreen runs have no signal)
};
MaxUi& maxUi(uint64_t key) {
  static std::map<uint64_t, MaxUi> m;
  return m[key];
}

// Dev (screenshots and tests, never set in normal use): "view=1", "pause=1", "menu=1|2|3", "demo=1", separated by commas.
void applyDevState(MaxUi& u) {
  const char* e = std::getenv("GL_CREST_DEV");
  if (!e) return;
  const std::string s(e);
  auto num = [&](const char* key, int fallback) {
    const size_t at = s.find(key);
    return at == std::string::npos ? fallback : std::atoi(s.c_str() + at + std::strlen(key));
  };
  u.view = num("view=", u.view);
  u.paused = num("pause=", 0) == 1;
  u.devMenu = num("menu=", 0);
  if (num("demo=", 0) == 1) {
    constexpr int K = MasterScope::kFrames, S = kMsBands * 4 + 1;
    u.devDemo = u.paused = true;
    u.cache.assign(static_cast<size_t>(K * S), 0.f);
    for (int i = 0; i < K; ++i) {
      const float beat = std::pow(1.0f - std::fmod(static_cast<float>(i) / 90.0f, 1.0f), 2.0f);
      for (int b = 0; b < kMsBands; ++b) {
        const float level = (0.25f + 0.55f * beat) * (b == 1 ? 0.7f : 1.0f);
        float* fr = u.cache.data() + static_cast<size_t>(i) * S + b * 4;
        fr[0] = level, fr[1] = level * 0.7f, fr[2] = level * 0.85f, fr[3] = -9.0f * beat * (b == 3 ? 0.5f : 1.0f);
      }
      u.cache[static_cast<size_t>(i) * S + S - 1] = 0.9f * (0.3f + 0.6f * beat);
    }
    u.lastHead = K - 1;
    u.headF = K - 1;
    u.spec.resize(160);
    for (int i = 0; i < 160; ++i) u.spec[static_cast<size_t>(i)] = -24.f - static_cast<float>(i) * 0.3f + 6.f * std::sin(static_cast<float>(i) * 0.2f);
  }
}
// Opens menu `which` for the dev state, at a fixed place inside the editor.
bool devOpens(MaxUi& u, int which, ImVec2 at) {
  if (u.devMenu != which || u.devMenuFrames > 2) return false;
  ++u.devMenuFrames;
  ImGui::SetNextWindowPos(at);
  return u.devMenuFrames == 2;
}

const ImU32 kBandCol[4] = {hex(0xB86D68), hex(0xC9B465), hex(0xCED671), hex(0xC9D5D4)};

double curveXForDb(double db) { return std::log(2.0 * std::pow(10.0, db / 20.0) + 1.0) / std::log(9.0); }

// A band's points (all of them: a long curve has up to 64 on the reference engine).
struct Pts {
  int n = 3;
  double x[kMsLongCurve] = {0, 0.5, 1}, y[kMsLongCurve] = {0, 0.5, 0.5}, t[kMsLongCurve] = {0, 0, 0};
  int type[kMsLongCurve] = {0, 0, 0};
};
Pts readPts(const PluginState& st, int band, bool ref) {
  Pts p;
  if (ref) {
    const maxref::Curve c = masterRefCurve(st, band);
    p.n = c.n;
    for (int k = 0; k < c.n; ++k) p.x[k] = c.x[k], p.y[k] = c.y[k], p.t[k] = c.t[k], p.type[k] = c.type[k];
  } else {
    const DrawnCurve c = masterDrawnCurve(st, band);
    p.n = c.n;
    for (int k = 0; k < c.n; ++k) p.x[k] = c.x[k], p.y[k] = c.y[k], p.t[k] = c.t[k], p.type[k] = 0;
  }
  return p;
}

// Writes a whole drawn curve (points sorted by x) as one undo step.
void writeCurve(App& a, const PluginTarget& t, const PluginState& st, int band, const Pts& c, bool ref, const char* label) {
  PluginState ns = st;
  normalizePluginState(ns);
  const int np = std::min(c.n, kMsCurvePts);
  ns.params[static_cast<size_t>(msExt(band, MxPoints))] = static_cast<float>(np);
  for (int k = 0; k < np; ++k) {
    const int src = k == np - 1 ? c.n - 1 : k;  // a long curve's parameters keep the first points and the last
    ns.params[static_cast<size_t>(msExt(band, MxPt0 + k * 3))] = static_cast<float>(c.x[src]);
    ns.params[static_cast<size_t>(msExt(band, MxPt0 + k * 3 + 1))] = static_cast<float>(c.y[src]);
    ns.params[static_cast<size_t>(msExt(band, MxPt0 + k * 3 + 2))] = static_cast<float>(c.t[src]);
    if (ref) ns.params[static_cast<size_t>(MsRef0 + MrPtType0 + band * kMsCurvePts + k)] = static_cast<float>(c.type[src]);
  }
  if (ref) {
    if (c.n > kMsCurvePts) {
      LongCurve lc;
      lc.n = c.n;
      for (int k = 0; k < c.n; ++k)
        lc.x[k] = static_cast<float>(c.x[k]), lc.y[k] = static_cast<float>(c.y[k]), lc.t[k] = static_cast<float>(c.t[k]), lc.type[k] = c.type[k];
      ns.data = masterLongCurvesWith(st.data, band, &lc);
    } else {
      ns.data = masterLongCurvesWith(st.data, band, nullptr);
    }
  }
  setPluginState(a, t, ns, label);
}

double curveDb(double x) { return 20.0 * std::log10(std::max((std::pow(9.0, x) - 1.0) * 0.5, 1e-6)); }

// Settings that need other buffers go through a Program build: they are prepared on the UI thread first.
void setBuilt(App& a, const PluginTarget& t, const PluginState& st, int param, float v) {
  PluginState ns = st;
  normalizePluginState(ns);
  ns.params[static_cast<size_t>(param)] = v;
  setPluginState(a, t, ns, "plugin option");
}

void selectBand(App& a, const PluginTarget& t, MaxUi& u, int b, bool ref) {
  u.band = b;
  if (ref) pui::setParamValue(a, t, MsRef0 + MrTarget, b);  // stored as the reference stores it; solo follows it
}

// ---------------------------------------------------------------- curve editor
// x = input, y = output, both on the log9(2a + 1) scale (0.5 = 0 dBFS, 1 = +12 dB). Drawn engine: drag points,
// drag a segment's middle handle up / down for its tension, double-click to add
// a point, right-click a point for its shape. Parametric engine: the curve the
// thresholds / ratios give (read only; switch the engine to draw).
void curveArea(App& a, const PluginTarget& t, const PluginState& st, MaxUi& u, const skin::Form& f, const crest::Rect& area, float inDb, const Pal& P) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const int b = u.band;
  const bool ref = ui::ival(st, MsRef0 + MrModel) == 1;
  const bool drawn = ui::ival(st, msExt(b, MxCurveMode)) == 1 || ref;
  const int rev = ui::ival(st, MsRef0 + MrRevision);
  const int maxPts = ref ? kMsLongCurve : kMsCurvePts;
  const ImVec2 p0 = f.p(area.x, area.y), p1 = f.p(area.x1(), area.y1());
  wellFrame(dl, f, p0, p1, P);
  const float pad = f.d(10);
  const ImU32 band = bandCol(b, P);
  ImVec2 q0(p0.x + pad, p0.y + pad), q1(p1.x - pad, p1.y - pad);
  auto X = [&](double x) { return q0.x + static_cast<float>(x) * (q1.x - q0.x); };
  auto Y = [&](double y) { return q1.y - static_cast<float>(y) * (q1.y - q0.y); };
  auto invX = [&](float x) { return std::clamp((x - q0.x) / (q1.x - q0.x), 0.0f, 1.0f); };
  auto invY = [&](float y) { return std::clamp((q1.y - y) / (q1.y - q0.y), 0.0f, 1.0f); };
  const double grid[5] = {-24, -12, -6, 0, 6};
  for (double g : grid) {
    double x = curveXForDb(g);
    ImU32 c = g == 0 ? P.gridStrong : P.grid;
    dl->AddLine(ImVec2(X(x), q0.y), ImVec2(X(x), q1.y), c);
    dl->AddLine(ImVec2(q0.x, Y(x)), ImVec2(q1.x, Y(x)), c);
    char lb[8];
    std::snprintf(lb, sizeof lb, "%+.0f", g);
    skin::label(dl, f, (X(x) - f.o.x) / f.s - 14, area.y1() - 12, 28, 10, g == 0 ? "0" : lb, P.dim, 0, kTinyPx, 1, false);
  }
  dl->AddLine(ImVec2(X(0), Y(0)), ImVec2(X(1), Y(1)), P.gridStrong);
  const maxref::Curve refC = ref ? masterRefCurve(st, b) : maxref::Curve{};
  const DrawnCurve oldC = ref ? DrawnCurve{} : masterDrawnCurve(st, b);
  const Pts c = readPts(st, b, ref);
  auto curveY = [&](double x) {
    if (ref) return maxref::curveAt(refC, x, rev);
    if (drawn) return oldC.at(x);
    const double a01 = (std::pow(9.0, x) - 1.0) * 0.5;
    return curveXForDb(masterCurve(st, b, 20.0 * std::log10(std::max(a01, 1e-6))));
  };
  constexpr int kSeg = 240;  // fine enough for the stepped point shapes
  ImVec2 pts[kSeg + 1];
  for (int i = 0; i <= kSeg; ++i) {
    const double x = i / static_cast<double>(kSeg);
    pts[i] = ImVec2(X(x), Y(std::clamp(curveY(x), 0.0, 1.0)));
  }
  // Reduction shading between the unity line and the curve above the knee.
  for (int i = 0; i < kSeg; ++i) {
    const double x0 = i / static_cast<double>(kSeg), x1 = (i + 1) / static_cast<double>(kSeg);
    if (pts[i].y <= Y(x0) && pts[i + 1].y <= Y(x1)) continue;
    ImVec2 q[4] = {ImVec2(pts[i].x, Y(x0)), ImVec2(pts[i + 1].x, Y(x1)), pts[i + 1], pts[i]};
    dl->AddConvexPolyFilled(q, 4, theme::withAlpha(band, 0.12f));
  }
  dl->AddPolyline(pts, kSeg + 1, drawn ? band : theme::withAlpha(P.text, 0.6f), 0, f.d(2.0f));
  if (inDb > -90) {
    const double x = curveXForDb(inDb), y = std::clamp(curveY(x), 0.0, 1.0);
    dl->AddLine(ImVec2(X(x), q1.y), ImVec2(X(x), Y(y)), theme::withAlpha(P.text, 0.25f));
    dl->AddCircleFilled(ImVec2(X(x), Y(y)), f.d(4), P.text);
  }
  ImGui::SetCursorScreenPos(p0);
  ImGui::InvisibleButton("##maxcurve", ImVec2(p1.x - p0.x, p1.y - p0.y));
  bool hov = ImGui::IsItemHovered();
  ImGuiIO& io = ImGui::GetIO();
  if (!drawn) {
    skin::label(dl, f, area.x + 8, area.y + 6, area.w - 16, 12, tr("Parametric curve"), P.dim, 0, kTextPx, 0, true);
    skin::label(dl, f, area.x + 8, area.y + 19, area.w - 16, 12, tr("Curves, Release 2 and the detector"), P.dim, 0, kTinyPx, 0, false);
    skin::label(dl, f, area.x + 8, area.y + 31, area.w - 16, 12, tr("apply to drawn curves"), P.dim, 0, kTinyPx, 0, false);
    if (hov) a.setHint("Curve from the thresholds / ratios · choose \"Drawn curve\" in the options to draw it");
    return;
  }
  // Handles.
  auto ptPos = [&](int k) { return ImVec2(X(c.x[k]), Y(c.y[k])); };
  auto tenPos = [&](int k) {  // middle of segment k-1 → k
    double xm = 0.5 * (c.x[k - 1] + c.x[k]);
    return ImVec2(X(xm), Y(std::clamp(curveY(xm), 0.0, 1.0)));
  };
  int nearPt = -1, nearTen = -1;
  float best = f.d(10);
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
    Pts e = c;
    if (u.dragPt >= 0 && u.dragPt < e.n) {
      int k = u.dragPt;
      double lo = k == 0 ? 0.0 : e.x[k - 1], hi = k == e.n - 1 ? 1.0 : e.x[k + 1];
      e.x[k] = k == 0 ? 0.0 : std::clamp(static_cast<double>(invX(io.MousePos.x)), lo, hi);
      e.y[k] = invY(io.MousePos.y);
      writeCurve(a, t, st, b, e, ref, "move curve point");
    } else if (u.dragTen > 0 && u.dragTen < e.n) {
      int k = u.dragTen;
      double dir = e.y[k] >= e.y[k - 1] ? 1.0 : -1.0;
      e.t[k] = std::clamp(e.t[k] - dir * io.MouseDelta.y * 0.02, -1.0, 1.0);
      writeCurve(a, t, st, b, e, ref, "curve tension");
    }
  }
  if (ImGui::IsItemDeactivated()) u.dragPt = u.dragTen = -1;
  if (hov && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && nearPt < 0 && c.n < maxPts) {
    Pts e = c;
    double x = invX(io.MousePos.x);
    int at = 1;
    while (at < e.n && e.x[at] < x) ++at;
    for (int k = e.n; k > at; --k) e.x[k] = e.x[k - 1], e.y[k] = e.y[k - 1], e.t[k] = e.t[k - 1], e.type[k] = e.type[k - 1];
    e.x[at] = x, e.y[at] = invY(io.MousePos.y), e.t[at] = 0, e.type[at] = 0;
    ++e.n;
    writeCurve(a, t, st, b, e, ref, "add curve point");
  }
  auto removePt = [&](int k) {
    Pts e = c;
    for (int i = k; i + 1 < e.n; ++i) e.x[i] = e.x[i + 1], e.y[i] = e.y[i + 1], e.t[i] = e.t[i + 1], e.type[i] = e.type[i + 1];
    --e.n;
    writeCurve(a, t, st, b, e, ref, "remove curve point");
  };
  if (hov && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && nearPt > 0) {
    if (ref) {  // the segment's shape, or remove the point
      u.menuPt = nearPt;
      ImGui::OpenPopup("##mxpt");
    } else if (c.n > 2) {
      removePt(nearPt);
    }
  }
  if (devOpens(u, 2, ImVec2(X(c.x[1]), Y(c.y[1])))) u.menuPt = 1, ImGui::OpenPopup("##mxpt");
  if (ImGui::BeginPopup("##mxpt")) {
    const int k = u.menuPt;
    if (k > 0 && k < c.n) {
      static const char* shapes[13] = {i18n::N_("Single bend"), i18n::N_("Double bend"), i18n::N_("Hold"), i18n::N_("Stairs"), i18n::N_("Smooth stairs"), i18n::N_("Pulse"), i18n::N_("Wave"), i18n::N_("Single bend 2"), i18n::N_("Double bend 2"), i18n::N_("Half sine"), i18n::N_("Smooth"), i18n::N_("Single bend 3"), i18n::N_("Double bend 3")};
      for (int i = 0; i < 13; ++i)
        if (ImGui::MenuItem(i18n::tr(shapes[i]), nullptr, c.type[k] == i)) {
          Pts e = c;
          e.type[k] = i;
          writeCurve(a, t, st, b, e, ref, "curve point shape");
        }
      ImGui::Separator();
      if (ImGui::MenuItem(i18n::tr("Remove point"), nullptr, false, c.n > 2)) removePt(k);
    }
    ImGui::EndPopup();
  }
  for (int k = 1; k < c.n; ++k) {
    ImVec2 q = tenPos(k);
    dl->AddCircle(q, f.d(3.4f), k == nearTen || k == u.dragTen ? P.text : theme::withAlpha(P.dim, 0.8f), 12, 1.0f);
  }
  for (int k = 0; k < c.n; ++k) {
    ImVec2 q = ptPos(k);
    bool hl = k == nearPt || k == u.dragPt;
    dl->AddCircleFilled(q, f.d(hl ? 5.5f : 4.4f), hl ? P.text : band, 16);
    dl->AddCircle(q, f.d(hl ? 5.5f : 4.4f), P.well, 16, 1.0f);
  }
  // Exact values of the point under the pointer.
  const int shown = u.dragPt >= 0 ? u.dragPt : nearPt;
  if (shown >= 0 && shown < c.n) {
    char lb[48];
    std::snprintf(lb, sizeof lb, "IN %+.1f  OUT %+.1f dB", curveDb(c.x[shown]), curveDb(c.y[shown]));
    skin::label(dl, f, area.x + 8, area.y + 6, area.w - 16, 12, lb, P.text, 0, kTextPx, 0, false);
  }
  if (hov)
    a.setHint(nearPt >= 0 ? "Drag the point · right-click for its shape or to remove it"
              : nearTen > 0 ? "Drag up / down to bend the segment"
                            : "Double-click to add a point (input across, output up; the 0 dB lines cross in the middle)");
}

// A monitor label over the traces: on its own dark plate, so it reads whatever runs behind it.
void monitorTag(App& a, ImDrawList* dl, const skin::Form& f, float x, float y, const char* text, ImU32 col, const Pal& P, bool rightAligned = false) {
  const float size = std::max(10.f, f.d(kTextPx));
  const ImVec2 ts = a.fontMono->CalcTextSizeA(size, FLT_MAX, 0.f, text);
  const float padX = f.d(3.5f), padY = f.d(0.7f);
  const float x0 = rightAligned ? x - ts.x - 2 * padX : x;
  dl->AddRectFilled(ImVec2(x0, y), ImVec2(x0 + ts.x + 2 * padX, y + ts.y + 2 * padY), P.plate, f.d(theme::kRadiusSm));
  dl->AddText(a.fontMono, size, ImVec2(x0 + padX, y + padY), col, text);
}

// ---------------------------------------------------------------- monitor (reference engine)
// Scrolling level and gain traces from the engine's monitor frames, over the
// output or input spectrum and an optional heatmap. Newest at the right.
void monitorRef(App& a, const PluginTarget& t, const PluginState& st, MaxUi& u, const skin::Form& f, const crest::Rect& area, const MasterScope* sc, const float* gr, const Pal& P) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 p0 = f.p(area.x, area.y), p1 = f.p(area.x1(), area.y1());
  wellFrame(dl, f, p0, p1, P);
  dl->PushClipRect(p0, p1, true);
  const ImU32 spectrumCol = theme::litRole(0x8BFC56), envCol = theme::litRole(0xFFBC40), gainCol = theme::litRole(0xDD4AEB), mainCol = theme::litRole(0x609E98);
  constexpr int K = MasterScope::kFrames, S = kMsBands * 4 + 1;
  ImGuiIO& io = ImGui::GetIO();
  if (u.cache.size() != static_cast<size_t>(K * S)) u.cache.assign(static_cast<size_t>(K * S), 0.f);
  if (sc && !u.paused) {
    const uint32_t h = sc->head.load(std::memory_order_acquire);
    if (h < u.lastHead || static_cast<double>(h) - u.headF > K / 4) u.headF = h;
    u.lastHead = h;
    float* out = u.cache.data();
    for (int i = 0; i < K; ++i) {
      for (int j = 0; j < S - 1; ++j) *out++ = sc->v[i][j].load(std::memory_order_relaxed);
      *out++ = sc->in[i].load(std::memory_order_relaxed);
    }
    u.headF += (static_cast<double>(h) - u.headF) * std::min(1.0, static_cast<double>(io.DeltaTime) * 24.0);
  }
  const uint32_t head = u.lastHead;
  auto frame = [&](int64_t i, int j) {
    if (i < 0 || i >= static_cast<int64_t>(head) || static_cast<int64_t>(head) - i >= K) return 0.f;
    return u.cache[static_cast<size_t>((i % K) * S + j)];
  };
  // Spectrum.
  constexpr int N = 160;
  if (u.spec.size() != N) u.spec.assign(N, -120.f);
  if (!u.paused) {
    float db[N];
    const ScopeTap* tap = u.histSource == 1 ? t.inputTap(a) : t.tap(a);
    pui::spectrum(tap, t.key() ^ (u.histSource == 1 ? 0x51ull : 0ull), N, db, a.engine.sampleRate());
    const float k = u.histSmooth == 0 ? 1.0f : u.histSmooth == 1 ? 0.45f : 0.15f;
    for (int i = 0; i < N; ++i) u.spec[static_cast<size_t>(i)] += (db[i] - u.spec[static_cast<size_t>(i)]) * k;
  }
  const float range = u.histRange == 0 ? 60.f : u.histRange == 1 ? 90.f : 120.f;
  auto specDb = [&](int i) {  // with the pivot slope around 1 kHz
    const float frac = (static_cast<float>(i) + 0.5f) / N;
    const double hz = pui::xToFreq(frac, 0.f, 1.f);
    return u.spec[static_cast<size_t>(i)] + static_cast<float>(u.histSlope) * std::log2(static_cast<float>(hz) / 1000.f);
  };
  // Heatmap.
  constexpr int HC = 128, HB = 40;
  if (u.heat) {
    if (u.heatMap.size() != HC * HB) u.heatMap.assign(HC * HB, -120.f);
    const double now = ImGui::GetTime();
    if (!u.paused && now - u.heatLast > 1.0 / 30.0) {
      u.heatLast = now;
      u.heatHead = (u.heatHead + 1) % HC;
      for (int y = 0; y < HB; ++y) {
        float m = -120.f;
        for (int i = y * N / HB; i < (y + 1) * N / HB; ++i) m = std::max(m, specDb(i));
        u.heatMap[static_cast<size_t>(u.heatHead * HB + y)] = m;
      }
    }
    const float H = p1.y - p0.y;
    const float top = u.heatPos == 1 ? p0.y + H * 0.5f : p0.y, bot = u.heatPos == 0 ? p0.y + H * 0.5f : p1.y;
    const float cw = (p1.x - p0.x) / HC, ch = (bot - top) / HB;
    for (int c = 0; c < HC; ++c) {
      const int col = (u.heatHead - c + HC) % HC;
      const float x1 = p1.x - static_cast<float>(c) * cw;
      for (int y = 0; y < HB; ++y) {
        const float v = (u.heatMap[static_cast<size_t>(col * HB + y)] + range) / range;
        if (v < 0.08f) continue;
        const float q = std::min(v, 1.0f);
        const ImU32 cc = q < 0.6f ? theme::withAlpha(theme::litRole(0x23889D), q * 1.2f) : theme::withAlpha(envCol, 0.5f + (q - 0.6f));
        const float yb = bot - static_cast<float>(y) * ch;
        dl->AddRectFilled(ImVec2(x1 - cw, yb - ch), ImVec2(x1, yb), cc);
      }
    }
  }
  const bool refm = true;
  const double fl = maxref::splitHz(ui::val(st, MsRef0 + MrRaw0)), fh = maxref::splitHz(ui::val(st, MsRef0 + MrRaw0 + 2));
  const float x1 = pui::freqToX(fl, p0.x, p1.x - p0.x), x2 = pui::freqToX(fh, p0.x, p1.x - p0.x);
  (void)refm;
  if (u.view == 0) {
    if (u.hist) {
      ImVec2 pts[N];
      for (int k = 0; k < N; ++k)
        pts[k] = ImVec2(p0.x + (p1.x - p0.x) * (static_cast<float>(k) + 0.5f) / N, p1.y - std::clamp((specDb(k) + range) / (range + 6.f), 0.0f, 1.0f) * (p1.y - p0.y));
      for (int k = 0; k + 1 < N; ++k) {
        ImVec2 q[4] = {pts[k], pts[k + 1], ImVec2(pts[k + 1].x, p1.y), ImVec2(pts[k].x, p1.y)};
        dl->AddConvexPolyFilled(q, 4, theme::withAlpha(spectrumCol, 0.07f));
      }
      dl->AddPolyline(pts, N, theme::withAlpha(spectrumCol, 0.40f), 0, 1.0f);
    }
    const float xs[4] = {p0.x, x1, x2, p1.x};
    for (int b = 0; b < 3; ++b)
      if (b == u.band) dl->AddRectFilled(ImVec2(xs[b], p0.y), ImVec2(xs[b + 1], p1.y), theme::withAlpha(bandCol(b, P), 0.06f));
  }
  // One lane of scrolling traces.
  static std::vector<ImVec2> line, gain;
  auto lane = [&](ImVec2 q0, ImVec2 q1, int band, bool selected) {
    const float w = q1.x - q0.x, h = q1.y - q0.y;
    dl->PushClipRect(q0, q1, true);
    auto yDb = [&](float v) { return q0.y + std::clamp((6.f - 20.f * std::log10(std::max(v, 1e-6f))) / 66.f, 0.f, 1.f) * h; };
    if (u.vis[6]) {  // level markers
      for (float m : {0.f, -6.f, -12.f, -24.f, -48.f}) {
        const float y = q0.y + (6.f - m) / 66.f * h;
        dl->AddLine(ImVec2(q0.x, y), ImVec2(q1.x, y), m == 0 ? P.gridStrong : P.grid);
      }
    }
    const int fpc = std::clamp(static_cast<int>(std::lround(8.f - u.speed * 7.f)), 1, 8);
    const int cols = std::min(static_cast<int>(w / f.d(1.4f)), (K - 2) / fpc);
    const float dx = w / static_cast<float>(cols);
    const int64_t base = static_cast<int64_t>(std::floor(u.headF / fpc)) * fpc;
    const float frac = static_cast<float>((u.headF - static_cast<double>(base)) / fpc);
    line.clear(), gain.clear();
    bool anyEnv = false, anyGain = false;
    for (int i = -1; i <= cols; ++i) {
      const int64_t end = base - static_cast<int64_t>(i) * fpc;
      float inP = 0, outP = 0, env = 0, g = 0, mainP = 0;
      for (int64_t q = end - fpc; q < end; ++q) {
        inP = std::max(inP, frame(q, band * 4)), outP = std::max(outP, frame(q, band * 4 + 1));
        env = std::max(env, frame(q, band * 4 + 2)), g = std::min(g, frame(q, band * 4 + 3));
        mainP = std::max(mainP, frame(q, S - 1));
      }
      const float x = q1.x - (static_cast<float>(i) - frac) * dx;
      if (u.vis[0] && band == 3 && mainP > 0) dl->AddRectFilled(ImVec2(x, yDb(mainP)), ImVec2(x + dx, q1.y), theme::withAlpha(mainCol, 0.22f));
      if (u.vis[1] && inP > 0) dl->AddRectFilled(ImVec2(x, yDb(inP)), ImVec2(x + dx, q1.y), theme::withAlpha(bandCol(band, P), 0.28f));
      if (u.vis[2] && outP > 0) dl->AddRectFilled(ImVec2(x, yDb(outP)), ImVec2(x + dx, q1.y), theme::withAlpha(bandCol(band, P), selected ? 0.55f : 0.42f));
      anyEnv = anyEnv || env > 1e-4f, anyGain = anyGain || g < -0.02f;
      line.push_back(ImVec2(x + dx * 0.5f, env > 1e-4f ? yDb(env) : q1.y + f.d(4)));  // silence leaves no floor line
      gain.push_back(ImVec2(x + dx * 0.5f, q0.y + std::clamp(-g / 24.f, 0.f, 1.f) * h));
    }
    if (u.vis[3] && anyEnv) dl->AddPolyline(line.data(), static_cast<int>(line.size()), theme::withAlpha(envCol, 0.85f), 0, f.d(1.1f));
    if (u.vis[4] && anyGain) {  // nothing taken off: no line, the lane stays clean
      for (size_t i = 0; i + 1 < gain.size(); ++i) {
        ImVec2 q[4] = {ImVec2(gain[i].x, q0.y), ImVec2(gain[i + 1].x, q0.y), gain[i + 1], gain[i]};
        if (gain[i].y > q0.y + 0.5f || gain[i + 1].y > q0.y + 0.5f) dl->AddConvexPolyFilled(q, 4, theme::withAlpha(gainCol, 0.22f));
      }
      dl->AddPolyline(gain.data(), static_cast<int>(gain.size()), theme::withAlpha(gainCol, 0.95f), 0, f.d(1.4f));
    }
    dl->PopClipRect();
  };
  // The dB scale of the full-height view, above everything drawn in the monitor.
  auto dbScale = [&] {
    if (!u.vis[6]) return;
    for (float m : {0.f, -6.f, -12.f, -24.f, -48.f}) {
      char lb[8];
      std::snprintf(lb, sizeof lb, "%.0f", static_cast<double>(m));
      monitorTag(a, dl, f, p1.x - f.d(3), std::max(p0.y + f.d(2), p0.y + (6.f - m) / 66.f * (p1.y - p0.y) - f.d(6.5f)), lb, P.text, P, true);
    }
  };
  if (u.view == 0) {
    lane(p0, p1, u.band, true);
    if (u.vis[5]) {  // all the bands' gain envelopes in their colours
      for (int b = 0; b < kMsBands; ++b) {
        if (b == u.band && u.vis[4]) continue;
        const int fpc = std::clamp(static_cast<int>(std::lround(8.f - u.speed * 7.f)), 1, 8);
        const float w = p1.x - p0.x;
        const int cols = std::min(static_cast<int>(w / f.d(1.4f)), (K - 2) / fpc);
        const float dx = w / static_cast<float>(cols);
        const int64_t base = static_cast<int64_t>(std::floor(u.headF / fpc)) * fpc;
        const float frac = static_cast<float>((u.headF - static_cast<double>(base)) / fpc);
        gain.clear();
        bool anyG = false;
        for (int i = -1; i <= cols; ++i) {
          const int64_t end = base - static_cast<int64_t>(i) * fpc;
          float g = 0;
          for (int64_t q = end - fpc; q < end; ++q) g = std::min(g, frame(q, b * 4 + 3));
          anyG = anyG || g < -0.02f;
          gain.push_back(ImVec2(p1.x - (static_cast<float>(i) - frac) * dx + dx * 0.5f, p0.y + std::clamp(-g / 24.f, 0.f, 1.f) * (p1.y - p0.y)));
        }
        if (anyG) dl->AddPolyline(gain.data(), static_cast<int>(gain.size()), theme::withAlpha(bandCol(b, P), 0.8f), 0, f.d(1.2f));
      }
    }
    for (double dec : {10.0, 100.0, 1000.0, 10000.0})
      for (int m = 1; m <= 9; ++m) {
        const double hz = dec * m;
        if (hz < 20 || hz > 20000) continue;
        const float gx = pui::freqToX(hz, p0.x, p1.x - p0.x);
        dl->AddLine(ImVec2(gx, p0.y), ImVec2(gx, p1.y), m == 1 ? P.grid : theme::withAlpha(P.grid, 0.5f));
      }
    for (int b = 0; b < 3; ++b) {
      const float xl = b == 0 ? p0.x : b == 1 ? x1 : x2;
      monitorTag(a, dl, f, xl + f.d(4), p1.y - f.d(18), kBand[b], b == u.band ? bandText(b, P) : P.dim, P);
    }
    dbScale();
  } else {
    const float H = (p1.y - p0.y) / 4.f;
    for (int b = 0; b < kMsBands; ++b) {
      const ImVec2 q0(p0.x, p0.y + H * static_cast<float>(b)), q1(p1.x, q0.y + H);
      if (b == u.band) dl->AddRectFilled(q0, q1, theme::withAlpha(bandCol(b, P), 0.07f));
      lane(q0, ImVec2(q1.x, q1.y - 1), b, b == u.band);
      dl->AddLine(ImVec2(q0.x, q1.y), q1, P.gridStrong);
      char lb[32];
      std::snprintf(lb, sizeof lb, "%s  %+.1f dB", kBand[b], gr[b] > -0.05f ? 0.0 : static_cast<double>(gr[b]));
      monitorTag(a, dl, f, q0.x + f.d(4), q0.y + f.d(3), lb, bandText(b, P), P);
    }
  }
  if (u.paused && !u.devDemo) monitorTag(a, dl, f, p0.x + (p1.x - p0.x) * 0.5f, p0.y + f.d(4), i18n::tr("PAUSED"), P.selText, P);
  dl->PopClipRect();
  ImGui::SetCursorScreenPos(p0);
  ImGui::InvisibleButton("##maxmon", ImVec2(p1.x - p0.x, p1.y - p0.y));
  static int drag = -1;
  if (u.view == 0) {
    if (ImGui::IsItemActivated()) {
      drag = std::fabs(io.MousePos.x - x1) < f.d(8) ? 0 : std::fabs(io.MousePos.x - x2) < f.d(8) ? 1 : -1;
      if (drag < 0) selectBand(a, t, u, io.MousePos.x < x1 ? 0 : io.MousePos.x < x2 ? 1 : 2, true);
    }
    if (ImGui::IsItemActive() && drag >= 0 && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 1)) {
      const double hz = pui::xToFreq(io.MousePos.x, p0.x, p1.x - p0.x);
      a.doc.setParam(t.pid(MsRef0 + MrRaw0 + (drag ? 2 : 0)), std::clamp(std::round(std::log2(hz / 30.0) / 0.000138227748), 0.0, 65536.0), true);
    }
    if (ImGui::IsItemDeactivated()) a.doc.endGesture(), drag = -1;
    for (float x : {x1, x2}) dl->AddLine(ImVec2(x, p0.y), ImVec2(x, p1.y), theme::withAlpha(P.text, 0.5f), f.d(1.4f));
    skin::hint(a, "Band levels and gain over time on the spectrum · click a band to edit it · drag a split line to move it");
  } else {
    if (ImGui::IsItemActivated()) selectBand(a, t, u, std::clamp(static_cast<int>((io.MousePos.y - p0.y) / ((p1.y - p0.y) / 4.f)), 0, 3), true);
    skin::hint(a, "All four bands over time · click a lane to edit that band");
  }
}

// ---------------------------------------------------------------- monitor
void monitorArea(App& a, const PluginTarget& t, const PluginState& st, MaxUi& u, const skin::Form& f, const crest::Rect& area, const float* gr, const Pal& P) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 p0 = f.p(area.x, area.y), p1 = f.p(area.x1(), area.y1());
  wellFrame(dl, f, p0, p1, P);
  const ImU32 spectrumCol = theme::litRole(0x8BFC56);
  const int N = 160;
  float db[N];
  pui::spectrum(t.tap(a), t.key(), N, db, a.engine.sampleRate());
  if (u.vis[2]) {
    ImVec2 pts[N];
    for (int k = 0; k < N; ++k)
      pts[k] = ImVec2(p0.x + (p1.x - p0.x) * (k + 0.5f) / N, p1.y - std::clamp((db[k] + 90.0f) / 96.0f, 0.0f, 1.0f) * (p1.y - p0.y));
    for (int k = 0; k + 1 < N; ++k) {
      ImVec2 q[4] = {pts[k], pts[k + 1], ImVec2(pts[k + 1].x, p1.y), ImVec2(pts[k].x, p1.y)};
      dl->AddConvexPolyFilled(q, 4, theme::withAlpha(spectrumCol, 0.10f));
    }
    dl->AddPolyline(pts, N, theme::withAlpha(spectrumCol, 0.55f), 0, 1.0f);
  }
  pui::drawFreqGrid(dl, p0, p1, 24, false);
  const bool refm = ui::ival(st, MsRef0 + MrModel) == 1;  // J-35: reference engine stores the raw split values
  double fl = refm ? maxref::splitHz(ui::val(st, MsRef0 + MrRaw0)) : ui::val(st, MsLowFreq);
  double fh = refm ? maxref::splitHz(ui::val(st, MsRef0 + MrRaw0 + 2)) : ui::val(st, MsHighFreq);
  float x1 = pui::freqToX(fl, p0.x, p1.x - p0.x), x2 = pui::freqToX(fh, p0.x, p1.x - p0.x);
  float xs[4] = {p0.x, x1, x2, p1.x};
  for (int b = 0; b < 3; ++b) {
    if (b == u.band) dl->AddRectFilled(ImVec2(xs[b], p0.y), ImVec2(xs[b + 1], p1.y), theme::withAlpha(bandCol(b, P), 0.08f));
    if (u.vis[4]) {
      float hh = std::min(1.0f, -gr[b] / 24.0f) * (p1.y - p0.y - 24);
      if (hh > 1) dl->AddRectFilled(ImVec2(xs[b] + 5, p0.y + 4), ImVec2(xs[b + 1] - 5, p0.y + 4 + hh), theme::withAlpha(bandCol(b, P), 0.35f), 2);
    }
    monitorTag(a, dl, f, xs[b] + f.d(4), p1.y - f.d(18), kBand[b], b == u.band ? bandText(b, P) : P.dim, P);
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
        if (i) dl->AddLine(prev, q, theme::withAlpha(bandCol(b, P), b == u.band ? 0.95f : 0.55f), f.d(b == u.band ? 1.8f : 1.3f));
        prev = q;
      }
    }
  }
  // Master reduction strip on the right edge.
  float hm = std::min(1.0f, -gr[3] / 24.0f) * (p1.y - p0.y);
  if (hm > 1) dl->AddRectFilled(ImVec2(p1.x - f.d(8), p0.y), ImVec2(p1.x - f.d(1), p0.y + hm), theme::withAlpha(bandCol(3, P), 0.7f));
  ImGui::SetCursorScreenPos(p0);
  ImGui::InvisibleButton("##maxmon", ImVec2(p1.x - p0.x, p1.y - p0.y));
  ImGuiIO& io = ImGui::GetIO();
  static int drag = -1;
  if (ImGui::IsItemActivated()) {
    drag = std::fabs(io.MousePos.x - x1) < f.d(8) ? 0 : std::fabs(io.MousePos.x - x2) < f.d(8) ? 1 : -1;
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
  for (float x : {x1, x2}) dl->AddLine(ImVec2(x, p0.y), ImVec2(x, p1.y), theme::withAlpha(P.text, 0.5f), f.d(1.4f));
  skin::hint(a, "Output spectrum and bands · click a band to edit it · drag a split line to move it");
}

// Display-layer glyphs: SVG icons on the old glyphs' grid (half the glyph box per unit).
void glyphDots(ImDrawList* dl, ImVec2 c, float s, ImU32 col) { ::gl::ui::drawIconGlyph(dl, ::gl::ui::Icon::DisplayDots, c, s * 0.34f, 0.0f, col); }
void glyphBars(ImDrawList* dl, ImVec2 c, float s, ImU32 col) { ::gl::ui::drawIconGlyph(dl, ::gl::ui::Icon::DisplayBars, c, s * 0.32f, 0.0f, col); }  // a shade inside: the bars sit on part pixels at 1.5x
void glyphLine(ImDrawList* dl, ImVec2 c, float s, ImU32 col) { ::gl::ui::drawIconGlyph(dl, ::gl::ui::Icon::DisplayLine, c, s * 0.34f, 1.3f, col); }

// Dev (GL_CREST_TRACE, tests): the editor's own items only; an open menu floats above them.
ImGuiWindow*& auditWindow() {
  static ImGuiWindow* w = nullptr;
  return w;
}
void auditItem(ImGuiID id, ImVec2 p0, ImVec2 p1) {
  if (ImGui::GetCurrentWindow() == auditWindow()) audit::item(id, p0, p1);
}
// Dev (GL_CREST_TRACE, tests): the alignment findings inside the form, for the frame as drawn.
void reportAudit(const audit::Report& rep, const skin::Form& f, const crest::Layout& l) {
  static int frame = 0;
  if (++frame % 8 != 0) return;
  const ImVec2 a0 = f.p(0, 0), a1 = f.p(l.W, l.H);
  const ImRect room = ImGui::GetCurrentWindow()->InnerRect;
  const bool scrolls = room.GetWidth() < f.w || room.GetHeight() < f.h || ImGui::GetCurrentWindow()->ScrollMax.x > 0 || ImGui::GetCurrentWindow()->ScrollMax.y > 0;
  int shown = 0;
  for (const align::Finding& x : rep.findings) {
    if (x.a.x0 < a0.x - 1 || x.a.y0 < a0.y - 1 || x.a.x1 > a1.x + 1 || x.a.y1 > a1.y + 1) continue;
    if (scrolls && x.rule != "overlap") continue;  // a small main window: the rest scrolls, cut off at the window's edge
    std::printf("crest-finding: %s\n", align::describe(x).c_str());
    ++shown;
  }
  int texts = 0;
  for (const align::Text& x : rep.scene.texts)
    if (x.r.x0 >= a0.x - 1 && x.r.y0 >= a0.y - 1 && x.r.x1 <= a1.x + 1 && x.r.y1 <= a1.y + 1) ++texts;
  std::printf("crest-audit: controls=%d texts=%d findings=%d inset=%d scale=%.3f form=%.1fx%.1f\n", static_cast<int>(rep.scene.ctls.size()), texts, shown,
              static_cast<int>(rep.issues.size()), static_cast<double>(f.s), static_cast<double>(f.w), static_cast<double>(f.h));
  std::fflush(stdout);
}

}  // namespace

// ===================================================================== gl.master (one grid: masterfl_layout.h)
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
  const bool refNow = ui::ival(st, MsRef0 + MrModel) == 1;
  if (!u.init) {  // the band the state selects
    u.init = true;
    if (refNow) u.band = std::clamp(ui::ival(st, MsRef0 + MrTarget), 0, 3);
    applyDevState(u);
  }
  const MasterScope* scope = nullptr;
  if (refNow)
    if (auto rt = ui::runtime(a, t)) scope = masterScope(rt->fx.get());
  // Gain readouts: fast down, gentle back (the eye follows them better than the raw value).
  {
    const float dt = ImGui::GetIO().DeltaTime;
    for (int k = 0; k < kMsBands; ++k) {
      const float v = std::min(0.f, gr[k]);
      u.grShow[k] = v < u.grShow[k] ? v : u.grShow[k] + (v - u.grShow[k]) * std::min(1.f, dt * 7.f);
      gr[k] = u.grShow[k];
    }
    if (u.devDemo) gr[0] = -6.2f, gr[1] = -3.1f, gr[2] = -1.4f, gr[3] = -2.4f;
  }
  const int b = std::clamp(u.band, 0, 3);
  auto P_ = [&](int k) { return msParam(b, k); };
  auto X = [&](int k) { return msExt(b, k); };
  // J-35: with the reference engine every control edits the stored integers.
  const bool ref = ui::ival(st, MsRef0 + MrModel) == 1;
  auto RB = [&](int tag) { return MsRef0 + MrRaw0 + maxref::rawIndex(b, tag); };
  auto RG = [&](int g) { return MsRef0 + MrRaw0 + g; };
  auto S = [&](int legacy, int refParam) { return ref ? refParam : legacy; };
  const bool drawn = ref || ui::ival(st, X(MxCurveMode)) == 1;
  const Pal P = crestPal();
  const crest::Layout l = crest::makeLayout(ref);
  skin::Form f = skin::formAt(crest::kW, l.H, width);
  f.o.x += std::max(0.0f, (width - f.w) * 0.5f);  // past the largest scale the form stays centred
  // K-40 layout audit of the frame as drawn (GL_CREST_TRACE: tests).
  static const bool trace = std::getenv("GL_CREST_TRACE") != nullptr;
  const bool tracing = trace && !audit::on();
  if (tracing) {
    audit::begin();
    auditWindow() = ImGui::GetCurrentWindow();
    imhook::setItemHook(&auditItem);
  }
  // Knob looks: the value arc in the role colour of its group, the same rail, cap and pointer everywhere.
  auto wheelLook = [&](ImU32 fill) {
    skin::WheelLook w;
    w.rail = P.rail, w.fill = fill, w.fillPressed = skin::lighten(fill, 0.2f), w.cap = P.cap, w.pointer = P.pointer;
    return w;
  };
  const skin::WheelLook wPre = wheelLook(theme::litRole(0x8758C4)), wPost = wheelLook(theme::litRole(0x00CAF4)), wSat = wheelLook(theme::litRole(0xE54E87));
  const skin::WheelLook wEnv = wheelLook(P.neutral), wLow = wheelLook(bandCol(0, P)), wHigh = wheelLook(bandCol(2, P)), wCut = wheelLook(theme::litRole(0xD68A38));
  // Wheels: with the reference engine the stored words read out in dB, ms, Hz on hover and drag.
  enum Law { kNone, kGain, kTime, kTime2, kSplit, kCut, kMix, kSep, kSat };
  auto wheelV = [&](int param, int row, int col, const skin::WheelLook& look, Law law, const char* caption) {
    const crest::Rect r = l.knobAt(row, col);
    skin::wheel(a, t, param, f, r.x, r.y, r.w, r.h, look);
    const bool show = ref && law != kNone && (ImGui::IsItemHovered() || ImGui::IsItemActive());
    captionAt(dl, f, l.labelAt(row, col), caption, P);
    if (!show) return;
    const double v = ui::val(st, param);
    char buf[32];
    switch (law) {
      case kGain: {
        const double g = maxref::gain7(v);
        if (g <= 0) std::snprintf(buf, sizeof buf, "-inf dB");
        else std::snprintf(buf, sizeof buf, "%+.1f dB", 20.0 * std::log10(g));
        break;
      }
      case kTime: std::snprintf(buf, sizeof buf, "%.1f ms", maxref::timeMs(v)); break;
      case kTime2: std::snprintf(buf, sizeof buf, "%.0f ms", 2.0 * maxref::timeMs(v)); break;
      case kSplit: std::snprintf(buf, sizeof buf, "%.0f Hz", maxref::splitHz(v)); break;
      case kCut:
        if (v < 0) std::snprintf(buf, sizeof buf, "Off");
        else std::snprintf(buf, sizeof buf, "%.0f Hz", maxref::lowCutHz(v));
        break;
      case kMix: std::snprintf(buf, sizeof buf, "%.0f %%", v / 10.0); break;
      case kSep: std::snprintf(buf, sizeof buf, "%+.0f %%", v / 10.0); break;
      case kSat:
        if (v == 0) std::snprintf(buf, sizeof buf, "Off");
        else std::snprintf(buf, sizeof buf, "%s %.0f %%", v < 0 ? "A" : "B", std::fabs(v) / 10.0);
        break;
      default: return;
    }
    ImDrawList* fg = ImGui::GetForegroundDrawList(ImGui::GetWindowViewport());
    const float size = std::max(10.f, f.d(kTextPx));
    const ImVec2 ts = a.fontMono->CalcTextSizeA(size, FLT_MAX, 0.f, buf);
    const ImVec2 c(f.p(r.cx(), r.y).x, f.p(r.x, r.y1()).y);
    // In the gap at the foot of the wheel, so the readout never sits on the caption below it.
    const ImVec2 q0(c.x - ts.x * 0.5f - f.d(4), c.y - ts.y - f.d(3)), q1(c.x + ts.x * 0.5f + f.d(4), c.y);
    fg->AddRectFilled(q0, q1, P.field, f.d(2));
    fg->AddRect(q0, q1, theme::withAlpha(P.accent, 0.7f), f.d(2));
    fg->AddText(a.fontMono, size, ImVec2(q0.x + f.d(4), q0.y + f.d(1.5f)), P.text, buf);
  };
  // A selector or button on the second row's knob line.
  auto selector = [&](int param, const crest::Rect& r, const char* const* labels, int count) {
    skin::Skin k;
    k.panel = P.field, k.lcdBg = P.field, k.select = P.select, k.selectText = lift(P.select, P.text, 4.5f, {theme::mix(P.field, P.select, 0.3f)}), k.textDim = P.label;
    skin::switchSel(a, t, param, f.p(r.x, r.y), ImVec2(f.d(r.w), f.d(r.h)), k, labels, count, true);
  };
  auto Item = [&](const char* id) { return crest::itemRect(l, id); };
  // ---- ground
  dl->AddRectFilled(f.p(0, 0), f.p(l.W, l.H), P.frame, f.d(theme::kRadiusLg));
  drawCell(dl, f, l.mode, tr("MODE"), P);
  drawCell(dl, f, l.gain, tr("GAIN"), P);
  drawCell(dl, f, l.env, tr("ENVELOPE"), P);
  drawCell(dl, f, l.mix, tr("BANDS"), P);
  drawCell(dl, f, l.split, tr("SPLIT"), P);
  drawCell(dl, f, l.tabs, nullptr, P);
  // ---- top: curve editor + monitor + tool strip
  float inDb = b == 3 ? inLevel : -120.f;
  if (scope) {  // the level the band's curve is reading now
    float lv = 0;
    const uint32_t h = scope->head.load(std::memory_order_acquire);
    for (uint32_t i = 1; i <= 6 && i <= h; ++i) lv = std::max(lv, scope->v[(h - i) % MasterScope::kFrames][b * 4 + 2].load(std::memory_order_relaxed));
    u.dot = lv > u.dot ? lv : u.dot + (lv - u.dot) * std::min(1.f, ImGui::GetIO().DeltaTime * 10.f);
    inDb = 20.f * std::log10(std::max(u.dot, 1e-6f));
  }
  curveArea(a, t, st, u, f, l.curve, inDb, P);
  if (ref) monitorRef(a, t, st, u, f, l.monitor, scope, gr, P);
  else monitorArea(a, t, st, u, f, l.monitor, gr, P);
  dl->AddRectFilled(f.p(l.strip.x, l.strip.y), f.p(l.strip.x1(), l.strip.y1()), P.panel, f.d(theme::kRadiusSm));
  {  // readout under the curve: the band and what it takes off now
    char lb[48];
    const crest::Rect& q = l.readout;
    skin::label(dl, f, q.x, q.y, 58, q.h, kBand[b], bandText(b, P), 0, kCapPx, 0, true);
    std::snprintf(lb, sizeof lb, "%+.1f dB", gr[b] > -0.05f ? 0.0 : static_cast<double>(gr[b]));  // never "-0.0"
    skin::label(dl, f, q.x + 58, q.y, 62, q.h, lb, gr[b] < -0.05f ? P.text : P.dim, 0, kCapPx, 2, false);
    const float k = std::clamp(-gr[b] / 18.0f, 0.0f, 1.0f);
    const ImVec2 g0 = f.p(q.x + 128, q.cy() - 3), g1 = f.p(q.x1(), q.cy() + 3);
    skin::insetField(dl, g0, g1, P.field, f.d(1));
    if (k > 0.002f) dl->AddRectFilled(ImVec2(g1.x - (g1.x - g0.x) * k, g0.y), g1, theme::withAlpha(bandCol(b, P), 0.9f), f.d(1));
  }
  skin::raisedFace(dl, f.p(l.menu.x, l.menu.y), f.p(l.menu.x1(), l.menu.y1()), P.face, f.d(theme::kRadius));
  if (skin::menuButton(f, "mxopt", l.menu.x, l.menu.y, l.menu.w, l.menu.h, P.accent)) ImGui::OpenPopup("##maxopt");
  if (devOpens(u, 1, f.p(l.menu.x, l.curve.y + 40))) ImGui::OpenPopup("##maxopt");
  if (ImGui::BeginPopup("##maxopt")) {
    ImGui::TextDisabled(i18n::tr("%s band"), kBand[b]);
    if (ImGui::MenuItem(i18n::tr("Reference engine"), nullptr, ref)) setBuilt(a, t, st, MsRef0 + MrModel, ref ? 0.f : 1.f);
    if (!ref && ImGui::MenuItem(i18n::tr("Drawn curve engine"), nullptr, drawn)) pui::setParamValue(a, t, X(MxCurveMode), drawn ? 0.0 : 1.0);
    if (!ref && ImGui::MenuItem(i18n::tr("Drawn curve engine on all bands"))) {
      PluginState ns = st;
      normalizePluginState(ns);
      for (int k = 0; k < kMsBands; ++k) ns.params[static_cast<size_t>(msExt(k, MxCurveMode))] = 1.f;
      setPluginState(a, t, ns, "drawn curve engine");
    }
    if (ImGui::MenuItem(i18n::tr("Reset this band's curve"))) {
      Pts c;
      writeCurve(a, t, st, b, c, ref, "reset curve");
    }
    ImGui::Separator();
    if (ref) {
      const int stages = ui::ival(st, MsRef0 + MrOversample);
      static const char* osn[4] = {i18n::N_("Oversampling: off"), i18n::N_("Oversampling: 2x"), i18n::N_("Oversampling: 4x"), i18n::N_("Oversampling: 8x")};
      for (int k = 0; k < 4; ++k)
        if (ImGui::MenuItem(i18n::tr(osn[k]), nullptr, stages == k)) setBuilt(a, t, st, MsRef0 + MrOversample, static_cast<float>(k));
      ImGui::Separator();
      auto toggle = [&](const char* label, int param, bool invert = false) {
        const bool on = (ui::ival(st, param) == 1) != invert;
        if (ImGui::MenuItem(i18n::tr(label), nullptr, on)) setBuilt(a, t, st, param, (on != invert) ? 0.f : 1.f);
      };
      toggle("Master band on the whole input", MsRef0 + MrMasterMid);
      toggle("Older linear-phase split filters", MsRef0 + MrFir);
      toggle("Keep 0.2 dB under the ceiling", MsRef0 + MrSafetyOff, true);
      ImGui::MenuItem(i18n::tr("Lock the spare state"), nullptr, &u.lockSpare);
      ImGui::Separator();
      if (ImGui::BeginMenu(i18n::tr("Spectrum"))) {
        ImGui::MenuItem(i18n::tr("Show"), nullptr, &u.hist);
        if (ImGui::MenuItem(i18n::tr("Of the output"), nullptr, u.histSource == 0)) u.histSource = 0;
        if (ImGui::MenuItem(i18n::tr("Of the input"), nullptr, u.histSource == 1)) u.histSource = 1;
        ImGui::Separator();
        static const char* ranges[3] = {i18n::N_("Range 60 dB"), i18n::N_("Range 90 dB"), i18n::N_("Range 120 dB")};
        for (int k = 0; k < 3; ++k)
          if (ImGui::MenuItem(i18n::tr(ranges[k]), nullptr, u.histRange == k)) u.histRange = k;
        ImGui::Separator();
        for (int k = 0; k <= 6; ++k) {
          char lb[32];
          std::snprintf(lb, sizeof lb, i18n::tr("Tilt %d dB per octave"), k);
          if (ImGui::MenuItem(lb, nullptr, u.histSlope == k)) u.histSlope = k;
        }
        ImGui::Separator();
        static const char* smooths[3] = {i18n::N_("No smoothing"), i18n::N_("Some smoothing"), i18n::N_("Long average")};
        for (int k = 0; k < 3; ++k)
          if (ImGui::MenuItem(i18n::tr(smooths[k]), nullptr, u.histSmooth == k)) u.histSmooth = k;
        ImGui::EndMenu();
      }
      if (ImGui::BeginMenu(i18n::tr("Heatmap"))) {
        ImGui::MenuItem(i18n::tr("Show"), nullptr, &u.heat);
        static const char* where[3] = {i18n::N_("Top half"), i18n::N_("Bottom half"), i18n::N_("Whole monitor")};
        for (int k = 0; k < 3; ++k)
          if (ImGui::MenuItem(i18n::tr(where[k]), nullptr, u.heatPos == k)) u.heatPos = k;
        ImGui::EndMenu();
      }
    } else {
      int os = ui::ival(st, MsOversample);
      const char* osn[3] = {"Saturation oversampling: off", "Saturation oversampling: 2x", "Saturation oversampling: 4x"};
      for (int k = 0; k < 3; ++k)
        if (ImGui::MenuItem(osn[k], nullptr, os == k)) pui::setParamValue(a, t, MsOversample, k);
    }
    ImGui::EndPopup();
  }
  // Display options: a glyph on its own face, lit in the colour of what it shows.
  auto glyph = [&](const char* id, const crest::Rect& r, bool& on, uint32_t role, void (*draw)(ImDrawList*, ImVec2, float, ImU32), const char* hint) {
    skin::raisedFace(dl, f.p(r.x, r.y), f.p(r.x1(), r.y1()), on ? P.faceOn : P.face, f.d(theme::kRadius));
    skin::glyphToggle(f, id, r.x, r.y, r.w, r.h, on, theme::litRole(role), draw, hint);
  };
  if (ref) {
    glyph("mvm", l.glyph[0], u.vis[0], 0x609E98, glyphBars, i18n::tr("Main input peaks"));
    glyph("mvi", l.glyph[1], u.vis[1], 0xC9B465, glyphBars, i18n::tr("Band input peaks"));
    glyph("mvo", l.glyph[2], u.vis[2], 0xDDE67A, glyphBars, i18n::tr("Band output peaks"));
    glyph("mva", l.glyph[3], u.vis[3], 0xFFBC40, glyphLine, i18n::tr("Level the curve reads"));
    glyph("mvg", l.glyph[4], u.vis[4], 0xDD4AEB, glyphLine, i18n::tr("Gain of this band"));
    glyph("mve", l.glyph[5], u.vis[5], 0xC9B465, glyphLine, i18n::tr("Gain of all the bands"));
    glyph("mvk", l.glyph[6], u.vis[6], 0x9FB3BD, glyphDots, i18n::tr("Level markers"));
    glyph("mvh", l.heat, u.heat, 0x23889D, glyphBars, i18n::tr("Spectrum heatmap"));
    static const char* views[2] = {"MONITOR", "BANDS"};
    skin::TabLook vl{P.face, P.selFace, P.label, P.selText};
    skin::tabsUi(f, "mxview", l.views.x, l.views.y, l.views.w, l.views.h, views, 2, u.view, vl, false);
    if (flatButton(f, "mxpause", l.pause, "II", u.paused, P, P.accent, i18n::tr("Pause the monitor")))
      u.paused = !u.paused;
  } else {
    glyph("mvi", l.glyph[0], u.vis[0], 0x609E98, glyphDots, "Show input level on the curve");
    glyph("mvs", l.glyph[1], u.vis[2], 0x8BFC56, glyphLine, "Show the output spectrum");
    glyph("mvg", l.glyph[2], u.vis[4], 0xDD4AEB, glyphBars, "Show band gain reduction");
    glyph("mve", l.glyph[3], u.vis[5], 0xC9B465, glyphLine, i18n::tr("Show the bands' gain envelopes over time"));
  }
  skin::label(dl, f, l.speedLabel.x, l.speedLabel.y, l.speedLabel.w, l.speedLabel.h, tr("SPEED"), P.label, 0, kCapPx, 0, true);
  skin::slider(f, "mxspeed", l.speed.x, l.speed.y, l.speed.w, l.speed.h, u.speed, P.field, P.handle, "Monitor speed");
  // Store / flip the spare state (the plugin's own compare, apart from the header's).
  if (flatButton(f, "mxstore", l.store, "S", false, P, P.accent, "Store in spare state") && !u.lockSpare) u.spare = st.params;
  if (flatButton(f, "mxflip", l.flip, "A/B", false, P, P.accent, "Flip with spare state (A/B compare)") && !u.spare.empty()) {
    PluginState ns = st;
    std::vector<float> keep = st.params;
    ns.params = u.spare;
    if (!u.lockSpare) u.spare = keep;
    normalizePluginState(ns);
    setPluginState(a, t, ns, "flip with spare state");
  }

  // ---- band tabs: the name and, under it, what that band takes off now
  for (int k = 0; k < kMsBands; ++k)
    if (bandTabButton(f, k, l.bandTab[k], k == b, gr[k], P) && k != u.band) selectBand(a, t, u, k, ref);
  // ---- mode + solo + stereo
  {
    static const char* modes4[4] = {"COMP", "NO COMP", "MUTE", "OFF"};
    static const char* modes3[3] = {"ON", "OFF", "MUTE"};
    const int mv = ui::ival(st, P_(MbMode));  // legacy: 0 bypass, 1 active, 2 mute, 3 no compression, 4 merged
    const int cur = ref ? std::clamp(ui::ival(st, RB(maxref::TMode)), 0, 3) : (mv == 1 ? 0 : mv == 2 ? 2 : 1);
    for (int k = 0; k < l.modes; ++k) {
      char id[16];
      std::snprintf(id, sizeof id, "maxmode%d", k);
      if (flatButton(f, id, l.modeCell[k], ref ? modes4[k] : modes3[k], cur == k, P, P.accent, nullptr) && cur != k) {
        if (ref) pui::setParamValue(a, t, RB(maxref::TMode), k);
        else pui::setParamValue(a, t, P_(MbMode), k == 0 ? 1 : k == 1 ? 3 : 2);  // OFF = compression off (gains and saturation stay)
      }
    }
    wheelV(S(P_(MbStereo), RB(maxref::TSep)), 1, 0, wEnv, kSep, tr("STEREO"));
    const bool solo = ref ? (b < 3 && ui::ival(st, MsRef0 + MrSolo) == 1 && ui::ival(st, MsRef0 + MrTarget) == b) : (b < 3 && ui::ival(st, MsSolo) == b + 1);
    if (flatButton(f, "maxsolo", Item("solo"), "SOLO", solo, P, theme::litRole(theme::role::kStamp), "Solo the edited band") && b < 3) {
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
  }
  // ---- gain + saturation
  wheelV(S(P_(MbPre), RB(maxref::TPre)), 0, 2, wPre, kGain, tr("PRE"));
  wheelV(S(P_(MbPost), RB(maxref::TPost)), 0, 3, wPost, kGain, tr("POST"));
  wheelV(S(P_(MbSatThresh), RB(maxref::TSatThr)), 1, 2, wSat, kSat, tr("SAT THRES"));
  wheelV(S(P_(MbSatCeil), RB(maxref::TSatCeil)), 1, 3, wSat, kGain, tr("SAT CEIL"));
  // ---- envelope
  wheelV(S(P_(MbAttack), RB(maxref::TAtt)), 0, 4, wEnv, kTime, tr("ATTACK"));
  wheelV(S(P_(MbRelease), RB(maxref::TRel)), 0, 5, wEnv, kTime2, tr("RELEASE"));
  wheelV(S(X(MxRelease2), RB(maxref::TRel2)), 0, 6, wEnv, kTime2, tr("RELEASE 2"));
  wheelV(S(P_(MbWindow), RB(maxref::TWindow)), 0, 7, wEnv, kTime, tr("SUSTAIN"));
  const skin::DigiLook digi{P.field, P.text};
  {
    const crest::Rect r = Item("atkCurve");
    skin::digi(a, t, S(X(MxAtkCurve), RB(maxref::TAttCurve)), f, r.x, r.y, r.w, r.h, digi);
    captionAt(dl, f, Item("atkCurve.label"), tr("ATT CURVE"), P);
  }
  {
    const crest::Rect r = Item("relCurve");
    skin::digi(a, t, S(X(MxRelCurve), RB(maxref::TRelCurve)), f, r.x, r.y, r.w, r.h, digi);
    captionAt(dl, f, Item("relCurve.label"), tr("REL CURVE"), P);
  }
  static const char* envModes[2] = {"PEAK", "RMS"};
  selector(S(X(MxEnvMode), RB(maxref::TSusMode)), Item("detector"), envModes, 2);
  captionAt(dl, f, Item("detector.label"), tr("DETECTOR"), P);
  // ---- bands
  wheelV(S(MsLookahead, RG(4)), 0, 8, wEnv, kTime, tr("DELAY"));
  wheelV(S(MsBandMix, RG(5)), 1, 8, wEnv, kMix, tr("MIX"));
  // ---- split
  wheelV(S(MsLowFreq, RG(0)), 0, 9, wLow, kSplit, tr("LOW"));
  wheelV(S(MsHighFreq, RG(2)), 0, 10, wHigh, kSplit, tr("HIGH"));
  wheelV(S(MsLowCut, RG(6)), 0, 11, wCut, kCut, tr("LOW CUT"));
  static const char* slopes3[3] = {"6", "12", "24"};
  static const char* slopes2[2] = {"12", "24"};
  selector(S(MsLowSlope, RG(1)), Item("lowSlope"), ref ? slopes2 : slopes3, ref ? 2 : 3);
  captionAt(dl, f, Item("lowSlope.label"), tr("SLOPE"), P);
  selector(S(MsHighSlope, RG(3)), Item("highSlope"), ref ? slopes2 : slopes3, ref ? 2 : 3);
  captionAt(dl, f, Item("highSlope.label"), tr("SLOPE"), P);
  const crest::Rect opt = Item("option");
  if (ref) {  // linear-phase split: right-click for its quality and change speed
    const bool on = ui::ival(st, MsRef0 + MrLin) == 1;
    if (flatButton(f, "mxlin", opt, "LIN", on, P, P.accent, i18n::tr("Linear-phase split · right-click for quality and change speed")))
      setBuilt(a, t, st, MsRef0 + MrLin, on ? 0.f : 1.f);
    if (devOpens(u, 3, f.p(opt.x - 120, l.curve.y + 120))) ImGui::OpenPopup("##mxlinmenu");
    if (ImGui::BeginPopupContextItem("##mxlinmenu")) {
      static const char* qn[6] = {i18n::N_("Lowest quality"), i18n::N_("Low quality"), i18n::N_("Normal quality"), i18n::N_("High quality"), i18n::N_("Higher quality"), i18n::N_("Highest quality")};
      static const char* sn[4] = {i18n::N_("Normal change speed"), i18n::N_("Fast change speed"), i18n::N_("Faster change speed"), i18n::N_("Ultra change speed")};
      for (int k = 0; k < 6; ++k)
        if (ImGui::MenuItem(i18n::tr(qn[k]), nullptr, ui::ival(st, MsRef0 + MrLinQuality) == k)) setBuilt(a, t, st, MsRef0 + MrLinQuality, static_cast<float>(k));
      ImGui::Separator();
      for (int k = 0; k < 4; ++k)
        if (ImGui::MenuItem(i18n::tr(sn[k]), nullptr, ui::ival(st, MsRef0 + MrLinSpeed) == k)) pui::setParamValue(a, t, MsRef0 + MrLinSpeed, k);
      ImGui::EndPopup();
    }
    captionAt(dl, f, Item("option.label"), tr("PHASE"), P);
  } else {  // the older engine's saturation shape takes this place
    static const char* satModes[3] = {"-", "A", "B"};
    selector(X(MxSatMode), opt, satModes, 3);
    captionAt(dl, f, Item("option.label"), tr("SAT MODE"), P);
  }
  if (tracing) {
    imhook::setItemHook(nullptr);
    audit::collectText(ImGui::GetCurrentWindow());
    const audit::Report rep = audit::end();
    reportAudit(rep, f, l);
    // What the clicks of the test reach, and where.
    auto at = [&](const crest::Rect& r) {
      const ImVec2 c = f.p(r.cx(), r.cy());
      return std::to_string(std::lround(c.x)) + "," + std::to_string(std::lround(c.y));
    };
    static int uiFrame = 0;
    if (++uiFrame % 8 == 0)
      std::printf("crest-ui: band=%d view=%d paused=%d mode=%d solo=%d lin=%d detector=%d low=%d tab1=%s mode2=%s solo=%s lin=%s det1=%s slope0=%s bandsview=%s pause=%s\n", b, u.view, u.paused ? 1 : 0,
                  ref ? ui::ival(st, RB(maxref::TMode)) : ui::ival(st, P_(MbMode)), ref ? ui::ival(st, MsRef0 + MrSolo) : ui::ival(st, MsSolo), ui::ival(st, MsRef0 + MrLin),
                  ui::ival(st, S(X(MxEnvMode), RB(maxref::TSusMode))), ui::ival(st, S(MsLowSlope, RG(1))), at(l.bandTab[1]).c_str(), at(l.modeCell[2]).c_str(), at(Item("solo")).c_str(), at(opt).c_str(),
                  at(crest::Rect{Item("detector").cx() + 4, Item("detector").y, 8, Item("detector").h}).c_str(), at(crest::Rect{Item("lowSlope").x + 2, Item("lowSlope").y, 8, Item("lowSlope").h}).c_str(),
                  at(crest::Rect{l.views.cx() + 4, l.views.y, 8, l.views.h}).c_str(), at(l.pause).c_str());
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
