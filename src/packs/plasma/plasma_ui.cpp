// Gridline — "plasma" pack: editor basics (see plasma_ui.h): palette, shared
// controls (dials with link bands, value bars, fields, chips, switches, cards,
// drop-downs, steppers, curve editor), links by drag and drop and Link mode,
// menus, preset open / apply, and the entry point. The frame (top bar, rail,
// dock, play bar) is in plasma_frame_ui.cpp.
#include "plasma_ui.h"
#include "../../app/Look.h"
#include "plasma_dsp.h"
#include "../../app/LayoutAudit.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include "../../core/I18n.h"
#include "../../core/Song.h"
#include "../../engine/Program.h"
#include "../PacksUi.h"
#include "../../app/DisplayConfirm.h"
#include "../../app/DisplayConfirmUi.h"
#include "../../app/ThemeSettings.h"
#include "../../app/Widgets.h"
#include "../../third_party/imgui/imgui_internal.h"

namespace gl::packs::plasma {

namespace ui {

namespace fs = std::filesystem;

// ================================================================== palette
// J-202: Plasma's roles are Gridline's slate roles (Theme.h): cells on the
// slate surfaces, displays in the dark field colour, Gridline's text tones,
// the synth family's accent and the modulation role. They follow the dark /
// light theme and the user's theme tweaks (brightness, contrast).
namespace {
ImU32 rgb(uint32_t v, float a = 1.0f) { return theme::rgb(v, a); }
using theme::mix;

// J-202: WCAG contrast of text colour `fg` over `bg` (both opaque ImU32).
float lum(ImU32 c) {
  auto ch = [](uint32_t v) {
    const float s = static_cast<float>(v & 0xFF) / 255.0f;
    return s <= 0.04045f ? s / 12.92f : std::pow((s + 0.055f) / 1.055f, 2.4f);
  };
  return 0.2126f * ch(c >> IM_COL32_R_SHIFT) + 0.7152f * ch(c >> IM_COL32_G_SHIFT) + 0.0722f * ch(c >> IM_COL32_B_SHIFT);
}
float contrast(ImU32 fg, ImU32 bg) {
  const float a = lum(fg), b = lum(bg);
  return (std::max(a, b) + 0.05f) / (std::min(a, b) + 0.05f);
}
// The lowest contrast of `fg` over the surfaces `bg[0..n)`.
float worst(ImU32 fg, const ImU32* bg, int n) {
  float w = 1e9f;
  for (int i = 0; i < n; ++i) w = std::min(w, contrast(fg, bg[i]));
  return w;
}
// Moves `fg` toward `to` in small steps until it reads at `target`:1 on every surface.
ImU32 lift(ImU32 fg, ImU32 to, float target, const ImU32* bg, int n) {
  ImU32 c = fg;
  for (int i = 1; i <= 20 && worst(c, bg, n) < target; ++i) c = mix(fg, to, static_cast<float>(i) * 0.05f);
  return c;
}

Pal makePal(const theme::Palette& T) {
  Pal p;
  const bool d = T.dark;
  p.dark = d;
  p.frame = T.surface900;                                   // the form's ground and the gutters between cells
  p.panel = T.surface800;                                   // a module cell
  p.panelHi = mix(T.surface800, T.surface700, 0.4f);        // knob decks, raised tiles
  p.panelLo = mix(T.surface900, T.surface800, 0.45f);      // recessed strips (lists, slot bodies)
  p.head = mix(T.surface800, T.surface700, d ? 0.55f : 0.35f);  // module header strip
  p.headOn = d ? T.surface700 : T.surface600;               // a lit / hovered header
  p.well = T.well;                                          // displays (graphs, waveforms)
  p.wellHi = mix(T.well, T.surface900, 0.5f);
  p.field = T.field;                                        // value fields, drop-downs, steppers
  p.grid = mix(T.well, T.text, d ? 0.06f : 0.08f);
  p.gridStrong = mix(T.well, T.text, d ? 0.12f : 0.16f);
  p.line = T.line;                                          // dividers and field borders
  p.lineHi = T.lineStrong;
  p.text = T.text;
  p.textDim = T.textDim;
  p.textFaint = T.textFaint;
  p.label = mix(T.text, T.textDim, 0.45f);
  // the synth family's role (PluginUI brandRole: Synth), deepened on light surfaces
  // K-164: Gridline's own roles: the accent is focus and selection only; values ring in the
  // knob fill; lit marks are hot; modulation is drawn in the colour of its source's family.
  p.accent = T.accent;
  p.accentDim = mix(p.accent, T.surface800, 0.45f);
  p.accentSoft = theme::withAlpha(p.accent, d ? 0.2f : 0.14f);
  p.mod = d ? rgb(theme::role::kGraph) : rgb(0x1689C7);
  p.modSoft = theme::withAlpha(p.mod, 0.18f);
  p.hot = T.hot;
  p.knob = T.knobFill;
  {
    const uint32_t fr[FamCount] = {theme::role::kDraw, theme::role::kStamp, theme::role::kSnap, theme::role::kWarp, theme::role::kSlice};
    for (int i = 0; i < FamCount; ++i) p.fam[i] = d ? rgb(fr[i]) : theme::lit(rgb(fr[i]));
  }
  p.warn = d ? rgb(0xFFB84D) : rgb(0xC77A12);
  p.good = T.led;
  p.danger = T.danger;
  p.capTop = d ? T.surface600 : T.surface800;
  p.capBot = d ? T.surface700 : T.surface700;
  p.rail = d ? T.field : T.surface600;
  p.pointer = d ? mix(T.text, T.paper, 0.5f) : T.text;
  p.keyWhite = T.keyWhite;
  p.keyBlack = T.keyBlack;
  p.keyDown = p.accent;
  p.boxEdge = d ? T.lineStrong : mix(T.lineStrong, T.textDim, 0.5f);  // J-202: unchecked boxes, firmer on light panels
  // J-202 contrast pass: text that is meant to be read reaches 4.5:1 on every surface it sits on
  // (frames, cells, decks, strips, headers, wells, fields); disabled text (an off module) 3:1.
  // Lit controls (headOn) carry text or accent text only. Each role moves toward the text colour.
  const Pal raw = p;  // the roles before the pass (for the table)
  {
    const ImU32 body[] = {p.frame, p.panel, p.panelHi, p.panelLo, p.head, p.well, p.field};
    const ImU32 lit[] = {p.panel, p.panelHi, p.head, p.headOn, p.field};
    const int nb = static_cast<int>(sizeof body / sizeof body[0]), nl = static_cast<int>(sizeof lit / sizeof lit[0]);
    const ImU32 dim0 = p.textDim;
    p.textDim = lift(p.textDim, p.text, 4.5f, body, nb);
    p.label = lift(p.label, p.text, 4.5f, body, nb);
    p.textFaint = lift(p.textFaint, dim0, 3.0f, body, nb);
    p.accentText = lift(p.accent, p.text, 4.5f, lit, nl);
    p.modText = lift(p.mod, p.text, 4.5f, body, nb);
    p.warn = lift(p.warn, p.text, 4.5f, body, nb);  // warnings are text too ("sustain loop")
    for (int i = 0; i < FamCount; ++i) p.famText[i] = lift(p.fam[i], p.text, 4.5f, body, nb);
  }
  if (std::getenv("GL_PLASMA_CONTRAST")) {  // J-202: the contrast table (AUDIT.md)
    const struct {
      const char* name;
      ImU32 c;
    } roles[] = {{"text", p.text},         {"label (was)", raw.label}, {"label", p.label},           {"textDim (was)", raw.textDim},
                 {"textDim", p.textDim},   {"textFaint (was)", raw.textFaint}, {"textFaint", p.textFaint}, {"accent (as text, was)", p.accent},
                 {"accentText", p.accentText}, {"mod (as text, was)", p.mod}, {"modText", p.modText},   {"warn (was)", raw.warn},
                 {"warn", p.warn}};
    const struct {
      const char* name;
      ImU32 c;
    } bgs[] = {{"frame", p.frame}, {"panel", p.panel}, {"panelHi", p.panelHi}, {"panelLo", p.panelLo}, {"head", p.head}, {"headOn", p.headOn}, {"well", p.well}, {"field", p.field}};
    std::fprintf(stderr, "PLASMA_CONTRAST %s |", d ? "dark" : "light");
    for (const auto& b : bgs) std::fprintf(stderr, " %s |", b.name);
    std::fprintf(stderr, "\n");
    for (const auto& r : roles) {
      std::fprintf(stderr, "PLASMA_CONTRAST %s |", r.name);
      for (const auto& b : bgs) std::fprintf(stderr, " %.2f |", static_cast<double>(contrast(r.c, b.c)));
      std::fprintf(stderr, "\n");
    }
  }
  return p;
}
skin::Skin makeSkin(const Pal& p) {
  skin::Skin s;
  s.panel = p.panel, s.panelHi = p.panelHi, s.panelLo = p.panelLo, s.bevel = p.line;
  s.rail = p.rail, s.fill = p.accent, s.fillPressed = p.accent, s.cap = p.capTop, s.handle = p.capTop;
  s.text = p.text, s.textDim = p.textDim, s.lcdBg = p.field, s.lcdText = p.text;
  s.graphBg = p.well, s.graphGrid = p.grid, s.graphGridStrong = p.gridStrong, s.graphText = p.textDim, s.graphCurve = p.accent;
  s.select = p.accent, s.well = p.panelLo, s.pillBg = p.head, s.pillText = p.textDim, s.valueBg = p.field, s.valueText = p.text;
  s.accent2 = p.mod, s.raised = p.panelHi, s.pointer = p.pointer, s.labelBg = p.head;
  return s;
}
// The palette of the current theme (rebuilt when the theme or its tweaks change).
struct PalCache {
  uint32_t key[11] = {};
  Pal pal;
  skin::Skin skin;
  bool init = false;
};
PalCache& palCache() {
  static PalCache c;
  const theme::Palette& T = theme::pal();
  // every theme input (mode, brightness, contrast, accent, white text) shows in one of these roles
  const uint32_t k[11] = {T.surface900, T.surface800, T.surface700, T.field, T.text, T.textDim, T.well, T.led, T.accent, static_cast<uint32_t>(T.dark),
                           static_cast<uint32_t>(T.whiteGlyphs)};  // K-177: the white text option
  // J-202: only the app's own palette drives it. Inside Plasma's skin scope theme::pal() is the
  // palette derived from Plasma's skin, which would rebuild the cache twice per frame (and
  // derive the roles from themselves); there the cached palette stays.
  if (!c.init || (!skin::active() && std::memcmp(k, c.key, sizeof k) != 0)) {
    std::memcpy(c.key, k, sizeof k);
    c.pal = makePal(T);
    c.skin = makeSkin(c.pal);
    c.init = true;
  }
  return c;
}
}  // namespace

const Pal& pal() { return palCache().pal; }
const skin::Skin& skin() { return palCache().skin; }

// ================================================================== values
namespace {
const PluginInfo* info() {
  static const PluginInfo* i = findPlugin(kType);
  return i;
}
const ParamDesc* desc(int k) {
  const PluginInfo* i = info();
  return i && k >= 0 && k < static_cast<int>(i->params.size()) ? &i->params[static_cast<size_t>(k)] : nullptr;
}
}  // namespace
const ParamDesc* pdesc(int k) { return desc(k); }
namespace {
uint64_t mergeKey(const PluginTarget& t, int what) { return (t.key() * 0x100000001B3ull) ^ (0x504C0000ull + static_cast<uint64_t>(what)); }
}  // namespace

double val(const Ctx& c, int k) {
  if (k < 0 || k >= static_cast<int>(c.st.params.size())) return desc(k) ? desc(k)->def : 0.0;
  return c.st.params[static_cast<size_t>(k)];
}
float valf(const Ctx& c, int k) { return static_cast<float>(val(c, k)); }
int vali(const Ctx& c, int k) { return static_cast<int>(std::lround(val(c, k))); }
void setv(Ctx& c, int k, double v, bool merge) { pui::setParamValue(c.a, c.t, k, v, merge); }
void commit(Ctx& c, const char* label, int what) {
  const PluginState* cur = c.t.state(c.a.doc.song());
  if (!cur) return;
  PluginState ns = *cur;
  ns.data = modelToData(c.m);
  if (ns.data != cur->data) setPluginState(c.a, c.t, ns, label, what ? mergeKey(c.t, what) : 0);
}
void commitParams(Ctx& c, const char* label, const std::vector<std::pair<int, double>>& ps) {
  const PluginState* cur = c.t.state(c.a.doc.song());
  if (!cur) return;
  PluginState ns = *cur;
  ns.data = modelToData(c.m);
  for (const auto& [k, v] : ps) {
    if (k < 0) continue;
    if (static_cast<int>(ns.params.size()) <= k) ns.params.resize(static_cast<size_t>(k + 1), 0.f);
    ns.params[static_cast<size_t>(k)] = static_cast<float>(v);
  }
  if (!(ns == *cur)) setPluginState(c.a, c.t, ns, label);
}
std::string ptext(const Ctx& c, int k) { return paramText(k, val(c, k), &c.st.params); }

// ================================================================== text
// J-202: nothing smaller than 8 px (the caption size at 100 %).
// The editor's type is set 15 % above its form sizes, so labels and values read at a glance at
// the editor's usual sizes (one place: every text, measure and layout check goes through here).
constexpr float kTypeScale = 1.25f;
float font(const Ctx& c, float px) { return std::max(9.5f, std::round(px * kTypeScale * c.F.s * 2.0f) * 0.5f); }
float ctlPx(float h) { return std::clamp(h * 0.44f, 10.0f, 12.0f); }
namespace {
int gMono = 0;  // MonoText scopes open: values are set in the mono face
ImFont* pick(const Ctx& c, bool bold) {
  if (gMono > 0 && c.a.fontMono) return c.a.fontMono;
  return bold ? (c.a.fontBold ? c.a.fontBold : c.a.fontRegular) : c.a.fontRegular;
}
ImVec2 measure(const Ctx& c, const char* s, float px, bool bold) {
  ImFont* f = pick(c, bold);
  if (!f) return ImVec2(0, 0);
  return f->CalcTextSizeA(font(c, px), 1e9f, 0, ::gl::ui::labelText(s));  // an icon label measures as its text
}
// ---- layout check (dev / test aid): every text and knob rect of a frame, per layer
struct LayoutItem {
  ImVec2 a, b;
  int layer;
  bool knob;
  std::string s;
  bool box = false;  // a control's body (button, bar, field): boxes must not overlap each other or a knob
};
std::vector<LayoutItem> gLayout;
std::vector<ImVec4> gScrollRects;  // clip rects of scrolling areas: text cut at their edges is scrolled, not cut
std::vector<std::string> gClipped;  // text cut by the edge of its field or card
int gLayer = 0;
}  // namespace
// Text drawn by ImGui itself (menus, popups): reported when it runs past the right edge of the
// window it sits in (a popup's edge cuts it).
void layoutWindowText(ImVec2 at, const char* s, float wrap) {
  static const bool on = std::getenv("GL_PLASMA_LAYOUT_CHECK") != nullptr;
  if (!on || !s || !*s) return;
  ImGuiWindow* w = ImGui::GetCurrentWindow();
  const ImVec2 sz = ImGui::CalcTextSize(s, nullptr, false, wrap);
  if (at.x + sz.x > w->InnerClipRect.Max.x + 1.5f) gClipped.push_back(s);
}
namespace {
std::string& layoutViewTag() {
  static std::string t;
  return t;
}
bool layoutOn() {
  static const bool on = std::getenv("GL_PLASMA_LAYOUT_CHECK") != nullptr;
  return on;
}
void layoutAdd(const Ctx& c, ImVec2 a, ImVec2 b, bool knob, const char* s) {
  if (!layoutOn()) return;
  const ImVec2 ca = c.dl->GetClipRectMin(), cb = c.dl->GetClipRectMax();
  a.x = std::max(a.x, ca.x), a.y = std::max(a.y, ca.y), b.x = std::min(b.x, cb.x), b.y = std::min(b.y, cb.y);
  if (b.x - a.x < 1 || b.y - a.y < 1) return;
  gLayout.push_back({a, b, gLayer, knob, s ? s : ""});
}
void put(const Ctx& c, ImVec2 pos, const char* s, ImU32 col, float px, bool bold) {
  ImFont* f = pick(c, bold);
  const ImVec2 at(std::round(pos.x), std::round(pos.y));
  ::gl::ui::drawLabel(c.dl, f, font(c, px), at, col, s);
  if (layoutOn() && s && *s && (col >> 24) > 0x40) {
    const ImVec2 sz = f->CalcTextSizeA(font(c, px), 1e9f, 0, ::gl::ui::labelText(s));
    // glyph box without the line gap: 15 % of the size top and bottom
    const ImVec2 ga(at.x + 0.5f, at.y + sz.y * 0.18f), gb(at.x + sz.x - 0.5f, at.y + sz.y * 0.85f);
    layoutAdd(c, ga, gb, false, s);
    // cut by a clip edge (that is not a scrolling area's edge): a cut word
    const ImVec2 ca = c.dl->GetClipRectMin(), cb = c.dl->GetClipRectMax();
    const bool visible = gb.x > ca.x && ga.x < cb.x && gb.y > ca.y && ga.y < cb.y;
    const float tol = 1.5f;
    if (visible && (ga.x < ca.x - tol || gb.x > cb.x + tol || ga.y < ca.y - tol || gb.y > cb.y + tol)) {
      bool scrolled = false;
      // the edge that cuts it is a scrolling area's edge (also through a field's own clip inside it)
      for (const auto& r : gScrollRects)
        if ((ga.y < ca.y - tol && std::fabs(r.y - ca.y) < 1) || (gb.y > cb.y + tol && std::fabs(r.w - cb.y) < 1) ||
            (ga.x < ca.x - tol && std::fabs(r.x - ca.x) < 1) || (gb.x > cb.x + tol && std::fabs(r.z - cb.x) < 1))
          scrolled = true;
      if (!scrolled) gClipped.push_back(s);
    }
  }
}
// Reports intersections (text / text, text / knob) of the frame on stderr, once per pair.
void layoutReport(const Ctx& c) {
  if (!layoutOn()) return;
  if (gLayoutSettling) {  // the rows settle in a frame: judge the settled layout only
    gLayoutSettling = false;
    gLayout.clear(), gClipped.clear();
    return;
  }
  static std::set<std::string> seen;
  static size_t most = 0;
  const std::string base = std::getenv("GL_PLASMA_LAYOUT_TAG") ? std::getenv("GL_PLASMA_LAYOUT_TAG") : "";
  const std::string tag = layoutViewTag().empty() ? base : layoutViewTag() + "@" + base;
  static std::string lastTag;
  if (tag != lastTag) lastTag = tag, most = 0;
  if (gLayout.size() > most) most = gLayout.size(), std::fprintf(stderr, "PLASMA_LAYOUT %s: %zu items\n", tag.c_str(), most);
  for (const auto& t : gClipped) {
    const std::string key = tag + "|clip|" + t;
    if (seen.insert(key).second) std::fprintf(stderr, "PLASMA_OVERLAP %s: '%s' is cut by the edge of its field or card\n", tag.c_str(), t.c_str());
  }
  gClipped.clear();
  gScrollRects.clear();
  for (size_t i = 0; i < gLayout.size(); ++i)
    for (size_t j = i + 1; j < gLayout.size(); ++j) {
      const LayoutItem &p = gLayout[i], &q = gLayout[j];
      if (p.layer != q.layer || (p.knob && q.knob)) continue;
      // text sits inside its own control's box; only box / box and box / knob count
      if ((p.box && !q.box && !q.knob) || (q.box && !p.box && !p.knob)) continue;
      const float ox = std::min(p.b.x, q.b.x) - std::max(p.a.x, q.a.x), oy = std::min(p.b.y, q.b.y) - std::max(p.a.y, q.a.y);
      if (ox <= c.s(0.6f) || oy <= c.s(0.6f)) continue;
      const std::string key = tag + "|" + p.s + "|" + q.s + "|" + std::to_string(static_cast<int>(p.a.x / c.F.s)) + "," +
                              std::to_string(static_cast<int>(p.a.y / c.F.s));
      if (!seen.insert(key).second) continue;
      std::fprintf(stderr, "PLASMA_OVERLAP %s: '%s' (%.0f,%.0f) x '%s' (%.0f,%.0f) by %.1fx%.1f px\n", tag.c_str(),
                   p.knob ? "<knob>" : p.box ? ("<" + p.s + ">").c_str() : p.s.c_str(),
                   (p.a.x - c.F.o.x) / c.F.s, (p.a.y - c.F.o.y) / c.F.s, q.knob ? "<knob>" : q.box ? ("<" + q.s + ">").c_str() : q.s.c_str(),
                   (q.a.x - c.F.o.x) / c.F.s,
                   (q.a.y - c.F.o.y) / c.F.s, ox / c.F.s, oy / c.F.s);
    }
  gLayout.clear();
  gLayer = 0;
}
}  // namespace
namespace {
// J-202: the baseline of a field centred on screen y `cy`: the mono value's,
// so labels, values and names in any face share one line.
float refBaseline(const Ctx& c, float cy, float px) {
  ImFont* m = c.a.fontMono ? c.a.fontMono : c.a.fontRegular;
  const float fs = font(c, px);
  const float h = m->CalcTextSizeA(fs, 1e9f, 0, "0").y;
  return std::round(cy - h * 0.5f) + m->GetFontBaked(fs)->Ascent;
}
float topFor(const Ctx& c, float baseline, float px, bool bold) { return baseline - pick(c, bold)->GetFontBaked(font(c, px))->Ascent; }
}  // namespace
MonoText::MonoText() { ++gMono; }
MonoText::~MonoText() { --gMono; }
float textW(const Ctx& c, const char* s, float px, bool bold) { return measure(c, s, px, bold).x / c.F.s; }
void text(const Ctx& c, float x, float y, const char* s, ImU32 col, float px, bool bold) { put(c, c.p(x, y), s, col, px, bold); }
void textHeavy(const Ctx& c, float x, float y, const char* s, ImU32 col, float px) {
  put(c, c.p(x, y), s, col, px, true);
  // a heavier weight than the bold face: the same glyphs again a third of a pixel to the right
  const ImVec2 at = c.p(x, y);
  ::gl::ui::drawLabel(c.dl, pick(c, true), font(c, px), ImVec2(std::round(at.x) + 0.35f, std::round(at.y)), col, s);
}
void textC(const Ctx& c, float cx, float y, const char* s, ImU32 col, float px, bool bold) {
  const ImVec2 sz = measure(c, s, px, bold);
  const ImVec2 q = c.p(cx, y);
  put(c, ImVec2(q.x - sz.x * 0.5f, q.y), s, col, px, bold);
}
void textR(const Ctx& c, float rx, float y, const char* s, ImU32 col, float px, bool bold) {
  const ImVec2 sz = measure(c, s, px, bold);
  const ImVec2 q = c.p(rx, y);
  put(c, ImVec2(q.x - sz.x, q.y), s, col, px, bold);
}
void labelBeside(const Ctx& c, float x, float boxY, float boxH, const char* s, ImU32 col, float px, bool bold) {
  const float base = refBaseline(c, c.p(0, boxY + boxH * 0.5f).y, ty::kValue);
  put(c, ImVec2(c.p(x, 0).x, topFor(c, base, px, bold)), s, col, px, bold);
}
void textMid(const Ctx& c, float x0, float x1, float cy, const char* s, ImU32 col, float px, bool bold) {
  const ImVec2 sz = measure(c, s, px, bold);
  const ImVec2 a = c.p(x0, cy), b = c.p(x1, cy);
  const float w = b.x - a.x;
  const float top = topFor(c, refBaseline(c, a.y, px), px, bold);  // J-202: on the field's baseline
  if (sz.x > w - 2) {  // clip long names to the field
    c.dl->PushClipRect(ImVec2(a.x + 1, a.y - sz.y), ImVec2(b.x - 1, a.y + sz.y), true);
    put(c, ImVec2(a.x + 2, top), s, col, px, bold);
    c.dl->PopClipRect();
    return;
  }
  put(c, ImVec2(a.x + (w - sz.x) * 0.5f, top), s, col, px, bold);
}

// ================================================================== surfaces
void panelRect(const Ctx& c, float x, float y, float w, float h, ImU32 col, float r) {
  c.dl->AddRectFilled(c.p(x, y), c.p(x + w, y + h), col, c.s(r));
}
void wellRect(const Ctx& c, float x, float y, float w, float h, float r) {
  const ImVec2 a = c.p(x, y), b = c.p(x + w, y + h);
  gl::audit::group(a, b, "well");  // J-128: rows are checked per panel
  if (look::styled()) return look::hookWell(c.dl, a, b, c.P.field);  // J-129 control style
  c.dl->AddRectFilled(a, b, c.P.field, c.s(r));
  c.dl->AddRect(a, b, c.P.line, c.s(r));
}
void hline(const Ctx& c, float x0, float x1, float y, ImU32 col) {
  const ImVec2 a = c.p(x0, y), b = c.p(x1, y);
  c.dl->AddLine(ImVec2(a.x, std::round(a.y) + 0.5f), ImVec2(b.x, std::round(a.y) + 0.5f), col, 1.0f);
}
void vline(const Ctx& c, float x, float y0, float y1, ImU32 col) {
  const ImVec2 a = c.p(x, y0), b = c.p(x, y1);
  c.dl->AddLine(ImVec2(std::round(a.x) + 0.5f, a.y), ImVec2(std::round(a.x) + 0.5f, b.y), col, 1.0f);
}
void moduleHead(Ctx& c, float x, float y, float w, float h, const char* title, int enable, bool lit) {
  const bool on = enable < 0 || val(c, enable) >= 0.5;
  c.dl->AddRectFilled(c.p(x, y), c.p(x + w, y + h), on && lit ? c.P.headOn : c.P.head);
  hline(c, x, x + w, y + h - 0.5f, c.P.line);
  float tx = x + 7;
  if (enable >= 0) {
    led(c, enable, x + 6, y + (h - 9) * 0.5f, 9);
    tx = x + 20;
  }
  text(c, tx, y + (h - 11) * 0.5f, title, on ? c.P.text : c.P.textFaint, 9.5f, true);
}
void caption(const Ctx& c, float x0, float x1, float y, const char* s) {
  const float w = textW(c, s, 9.0f, true);
  const float cx = (x0 + x1) * 0.5f;
  hline(c, x0, cx - w * 0.5f - 6, y + 5.5f, c.P.lineHi);
  hline(c, cx + w * 0.5f + 6, x1, y + 5.5f, c.P.lineHi);
  textC(c, cx, y, s, c.P.label, 9.0f, true);
}

void layoutLayer(int layer) { gLayer = layer; }
void layoutScroll(const Ctx& c) {
  if (layoutOn()) gScrollRects.push_back(ImVec4(c.dl->GetClipRectMin().x, c.dl->GetClipRectMin().y, c.dl->GetClipRectMax().x, c.dl->GetClipRectMax().y));
}
void layoutBox(const Ctx& c, float x, float y, float w, float h, const char* what) {
  if (!layoutOn()) return;
  const size_t n = gLayout.size();
  layoutAdd(c, c.p(x + 1, y + 1), c.p(x + w - 1, y + h - 1), false, what);
  if (gLayout.size() > n) gLayout.back().box = true;
}

void frameBox(const Ctx& c, float x, float y, float w, float h, ImU32 fill) {
  const float g = kGutter;
  const ImVec2 a = c.p(x + g, y + g), b = c.p(x + w - g, y + h - g);
  gl::audit::group(a, b, "cell");  // J-128: rows are checked per panel
  if (look::styled()) return look::hookSection(c.dl, a, b, fill ? fill : c.P.panel);  // J-129 control style
  // no outline: a cell reads as a cell through the dark gutter around it and its tone
  c.dl->AddRectFilled(a, b, fill ? fill : c.P.panel, c.s(2.0f));
}
float moduleFrame(Ctx& c, float x, float y, float w, float h, float headH, const char* title, int enable, bool lit, ImU32 fill, bool centre) {
  frameBox(c, x, y, w, h, fill);
  const float g = kGutter;
  const bool on = enable < 0 || val(c, enable) >= 0.5;
  const ImVec2 a = c.p(x + g + 1, y + g + 1), b = c.p(x + w - g - 1, y + headH);
  skin::gradientRect(c.dl, a, b, skin::lighten(on && lit ? c.P.headOn : c.P.head, c.P.dark ? 0.04f : 0.03f), on && lit ? c.P.headOn : c.P.head,
                     c.s(2.0f), ImDrawFlags_RoundCornersTop);
  c.dl->AddLine(ImVec2(a.x, b.y), ImVec2(b.x, b.y), c.P.frame, std::max(1.0f, c.s(1.2f)));
  if (title && *title && centre) {
    textC(c, x + w * 0.5f, y + (headH + g - 11) * 0.5f, title, on ? c.P.text : c.P.textFaint, 9.5f, true);
  } else if (title && *title) {
    float tx = x + g + 7;
    if (enable >= 0) {
      led(c, enable, x + g + 6, y + (headH + g - 9) * 0.5f, 9);
      tx = x + g + 20;
    }
    text(c, tx, y + (headH + g - 11) * 0.5f, title, on ? c.P.text : c.P.textFaint, 9.5f, true);
  }
  return y + headH;
}
void deck(const Ctx& c, float x, float y, float w, float h) {
  const float g = kGutter;
  const ImVec2 a = c.p(x + g + 1, y), b = c.p(x + w - g - 1, y + h - g - 1);
  c.dl->AddRectFilled(a, b, c.P.panelHi, c.s(2.0f), ImDrawFlags_RoundCornersBottom);
  c.dl->AddLine(ImVec2(a.x, a.y), ImVec2(b.x, a.y), c.P.frame, std::max(1.0f, c.s(1.5f)));
}
void insetFrame(const Ctx& c, float x, float y, float w, float h, int cols, int rows, bool centre) {
  const ImVec2 a = c.p(x, y), b = c.p(x + w, y + h);
  gl::audit::group(a, b, "inset");  // J-128: rows are checked per panel
  skin::gradientRect(c.dl, a, b, c.P.well, c.P.wellHi, c.s(2));
  if (cols > 0 || rows > 0) gridLines(c, x, y, w, h, cols, rows, centre);
  // inner top shade + border
  c.dl->AddRectFilledMultiColor(a, ImVec2(b.x, a.y + c.s(4)), IM_COL32(0, 0, 0, c.P.dark ? 60 : 18), IM_COL32(0, 0, 0, c.P.dark ? 60 : 18),
                                IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0));
  c.dl->AddRect(a, b, c.P.dark ? c.P.line : c.P.lineHi, c.s(2), 0, 1.0f);
}
void fitLabel(const Ctx& c, float cx, float y, float maxW, const char* s, ImU32 col, float px, bool bold) {
  float p = px;
  while (p > 7.0f && textW(c, s, p, bold) > maxW) p -= 0.5f;
  if (textW(c, s, p, bold) <= maxW) {
    textC(c, cx, y + (px - p) * 0.5f, s, col, p, bold);
    return;
  }
  textMid(c, cx - maxW * 0.5f, cx + maxW * 0.5f, y + px * 0.6f, s, col, p, bold);
}

void cellLabel(const Ctx& c, float cx, float y, const char* s, float maxW) { fitLabel(c, cx, y, maxW, s, c.P.label, 9.0f, true); }

// ================================================================== icons
// Icons: SVG icons (assets/icons/ui) fitted to the old vector glyphs' ink.
void iconArrow(ImDrawList* dl, ImVec2 c, float s, int dir, ImU32 col) {
  // An open chevron, h / 2 deep and s long, of two flat-ended strokes (half width e):
  // their corners reach 0.45 e along the length; across the depth they ink only
  // slivers of pixel, so the depth is taken without them.
  const float h = s * 0.5f, e = std::max(1.2f, s * 0.14f) * 0.5f;
  static const ::gl::ui::Icon kDir[4] = {::gl::ui::Icon::ChevronLeft, ::gl::ui::Icon::Chevron, ::gl::ui::Icon::ChevronUp, ::gl::ui::Icon::ChevronDown};
  const bool horiz = dir == 0 || dir == 1;
  const ImVec2 half = horiz ? ImVec2(h * 0.5f, h + e * 0.45f) : ImVec2(h + e * 0.45f, h * 0.5f);
  ::gl::ui::drawIconFit(dl, kDir[std::clamp(dir, 0, 3)], ImVec2(c.x - half.x, c.y - half.y), ImVec2(c.x + half.x, c.y + half.y), col);
}
void iconGear(ImDrawList* dl, ImVec2 c, float r, ImU32 col) {
  const float e = r;  // flat-ended teeth out to r
  ::gl::ui::drawIconFit(dl, ::gl::ui::Icon::Settings, ImVec2(c.x - e, c.y - e), ImVec2(c.x + e, c.y + e), col);
}
void iconPencil(ImDrawList* dl, ImVec2 c, float s, ImU32 col) {
  const float h = s * 0.5f, e = std::max(1.5f, s * 0.2f) * 0.5f * 0.71f;  // flat-ended shaft to (0.6, -0.6) h, tip at (-0.95, 0.95) h
  ::gl::ui::drawIconFit(dl, ::gl::ui::Icon::Draw, ImVec2(c.x - h * 0.95f, c.y - h * 0.6f - e), ImVec2(c.x + h * 0.6f + e, c.y + h * 0.95f), col);
}
void iconPower(ImDrawList* dl, ImVec2 c, float r, ImU32 col) {
  const float e = std::max(1.3f, r * 0.22f) * 0.5f;  // a ring of r, its stem from 1.15 r above the centre
  ::gl::ui::drawIconFit(dl, ::gl::ui::Icon::Power, ImVec2(c.x - r - e, c.y - r * 1.15f - e), ImVec2(c.x + r + e, c.y + r + e), col);
}
void iconSave(ImDrawList* dl, ImVec2 c, float s, ImU32 col) {
  const float e = s * 0.5f + std::max(1.2f, s * 0.12f) * 0.5f;  // a square of s, outlined
  ::gl::ui::drawIconFit(dl, ::gl::ui::Icon::Save, ImVec2(c.x - e, c.y - e), ImVec2(c.x + e, c.y + e), col);
}
void iconBrowse(ImDrawList* dl, ImVec2 c, float s, ImU32 col) {
  const float h = s * 0.5f, th = std::max(1.2f, s * 0.12f);  // four bulleted lines, 0.75 h above and below
  ::gl::ui::drawIconFit(dl, ::gl::ui::Icon::ListBullets, ImVec2(c.x - h, c.y - h * 0.75f - th), ImVec2(c.x + h + th * 0.5f, c.y + h * 0.75f + th), col);
}
void iconClose(ImDrawList* dl, ImVec2 c, float s, ImU32 col) {
  const float e = s * 0.4f + std::max(1.2f, s * 0.14f) * 0.5f;
  ::gl::ui::drawIconFit(dl, ::gl::ui::Icon::Close, ImVec2(c.x - e, c.y - e), ImVec2(c.x + e, c.y + e), col);
}
void iconSourceMeter(ImDrawList* dl, ImVec2 c, float r, float v, bool bipolar, bool idle, ImU32 track, ImU32 lit) {
  // J-202: a jack (a soft chip with a centre dot) that reads as something to
  // grab, not a spinner; the live arc shows only while the source moves.
  constexpr float a0 = 3.14159265f * 0.75f, a1 = 3.14159265f * 2.25f, mid = (a0 + a1) * 0.5f;
  const float thLit = std::max(1.5f, r * 0.36f);
  dl->AddCircleFilled(c, r * 0.95f, theme::withAlpha(track, 0.28f), 20);
  dl->AddCircleFilled(c, std::max(1.5f, r * 0.34f), track, 12);
  if (idle || (!bipolar && v < 0.005f) || (bipolar && std::fabs(v) < 0.005f)) return;
  const float from = bipolar ? mid : a0;
  const float to = bipolar ? mid + std::clamp(v, -1.f, 1.f) * (a1 - mid) : a0 + std::clamp(v, 0.f, 1.f) * (a1 - a0);
  const float lo = std::min(from, to), hi = std::max(from, to);
  if (hi - lo > 0.02f) {
    dl->PathArcTo(c, r, lo, hi, std::max(2, static_cast<int>((hi - lo) * 6.f)));
    dl->PathStroke(lit, 0, thLit);
  }
  dl->AddCircleFilled(ImVec2(c.x + std::cos(to) * r, c.y + std::sin(to) * r), thLit * 0.75f, lit);  // the value head
}

// ================================================================== controls
namespace {
constexpr float kA0 = 3.14159265f * 0.75f, kA1 = 3.14159265f * 2.25f;

// The standard pui interaction for the last item, with Plasma's own value
// bubble in place of the generic tooltip (which shows the stored control
// value, not the real units).
bool interact(Ctx& c, int k, double& v, float pixels, bool horizontal) {
  const ParamDesc* d = desc(k);
  if (!d) return false;
  const bool menuOpen = ImGui::IsPopupOpen("##pmenu") || ImGui::IsPopupOpen("##type");
  if (!menuOpen) {
    ImGui::PushStyleColor(ImGuiCol_PopupBg, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_BorderShadow, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(0, 0, 0, 0));
  }
  // whole-number parameters step by one on the wheel (the generic step is a fraction of the range)
  ImGuiIO& io = ImGui::GetIO();
  const float wheel = io.MouseWheel;
  const bool intWheel = integerParam(k) && wheel != 0 && ImGui::IsItemHovered();
  if (intWheel) io.MouseWheel = 0;
  bool ch = pui::dragParamItem(c.a, c.t.pid(k), *d, v, pixels, horizontal);
  if (intWheel) {
    io.MouseWheel = wheel;
    const double nv = std::clamp(std::round(v) + (wheel > 0 ? 1.0 : -1.0), d->min, d->max);
    if (nv != v) v = nv, setv(c, k, v), ch = true;
  }
  if (!menuOpen) ImGui::PopStyleColor(4);
  if (ch && integerParam(k) && std::fabs(v - std::round(v)) > 1e-6) {
    v = std::round(v);
    setv(c, k, v, true);
  }
  return ch;
}
// The control's plain sentence, under the pointer once it has rested on control k for a moment.
void helpTip(Ctx& c, int k) {
  static int lastK = -1;
  static double since = 0;
  const double now = ImGui::GetTime();
  if (k != lastK) lastK = k, since = now;
  if (!c.u.prefBubbles || now - since < 0.7 || ImGui::IsMouseDown(0)) return;
  const std::string s = paramSentence(k);
  if (s.empty()) return;
  ImFont* f = c.a.fontRegular;
  const float fs = font(c, 10.5f), wrap = c.s(260), pad = c.s(7);
  const ImVec2 ts = f->CalcTextSizeA(fs, 1e9f, wrap, s.c_str());
  const ImVec2 m = ImGui::GetIO().MousePos;
  const ImVec2 vp = ImGui::GetMainViewport()->Size;
  ImVec2 p0(std::min(m.x + c.s(14), vp.x - ts.x - pad * 2 - 4), m.y + c.s(20));
  if (p0.y + ts.y + pad * 2 > vp.y) p0.y = m.y - c.s(12) - ts.y - pad * 2;
  const ImVec2 p1(p0.x + ts.x + pad * 2, p0.y + ts.y + pad * 2);
  ImDrawList* fg = ImGui::GetForegroundDrawList();
  fg->AddRectFilled(ImVec2(p0.x + 1, p0.y + 2), ImVec2(p1.x + 1, p1.y + 2), IM_COL32(0, 0, 0, 70), c.s(4));
  fg->AddRectFilled(p0, p1, c.P.head, c.s(4));
  fg->AddRect(p0, p1, c.P.lineHi, c.s(4));
  fg->AddText(f, fs, ImVec2(p0.x + pad, p0.y + pad), c.P.text, s.c_str(), nullptr, wrap);
}
void bubble(Ctx& c, ImVec2 anchor, const std::string& s) {
  if (!c.u.prefBubbles) return;
  ImFont* f = c.a.fontMono ? c.a.fontMono : c.a.fontRegular;
  const float fs = font(c, 9.5f);
  const ImVec2 ts = f->CalcTextSizeA(fs, 1e9f, 0, s.c_str());
  const float pad = c.s(5);
  ImVec2 p0(std::round(anchor.x - ts.x * 0.5f - pad), std::round(anchor.y - ts.y - pad * 2.2f));
  ImVec2 p1(p0.x + ts.x + pad * 2, p0.y + ts.y + pad * 1.4f);
  ImDrawList* fg = ImGui::GetForegroundDrawList();
  fg->AddRectFilled(ImVec2(p0.x + 1, p0.y + 2), ImVec2(p1.x + 1, p1.y + 2), IM_COL32(0, 0, 0, 70), c.s(4));
  fg->AddRectFilled(p0, p1, c.P.head, c.s(4));
  fg->AddRect(p0, p1, c.P.lineHi, c.s(4));
  fg->AddText(f, fs, ImVec2(p0.x + pad, p0.y + pad * 0.7f), c.P.text, s.c_str());
}
// Context menu for parameter k: reset and type (J-136), modulation slots, add,
// the standard menu.
void modMenu(Ctx& c, int k, bool& openStd, bool& openType) {
  ImGui::TextDisabled("%s", paramLongName(k).c_str());
  ImGui::Separator();
  if (const ParamDesc* d = desc(k)) {
    double cur = val(c, k);
    gl::ui::ParamResetMenuItem(c.t.pid(k), *d, cur);
    if (ImGui::MenuItem(i18n::tr("Type a value..."))) openType = true;
    ImGui::Separator();
  }
  auto slots = modSlotsFor(c, k);
  for (int s : slots) {
    ImGui::PushID(s);
    const int src = vali(c, modP(s, M_Source));
    float amt = valf(c, modP(s, M_Amount)) * 100.f;
    bool byp = val(c, modP(s, M_Bypass)) >= 0.5;
    ImGui::TextUnformatted(sourceLabel(src).c_str());
    ImGui::SameLine(120);
    ImGui::SetNextItemWidth(120);
    if (ImGui::SliderFloat("##amt", &amt, -100.f, 100.f, "%.0f %%")) setv(c, modP(s, M_Amount), amt / 100.0, true);
    if (ImGui::IsItemDeactivatedAfterEdit()) c.a.doc.endGesture();
    ImGui::SameLine();
    if (ImGui::Checkbox(i18n::tr("Bypass"), &byp)) setv(c, modP(s, M_Bypass), byp ? 1 : 0);
    ImGui::SameLine();
    if (ImGui::SmallButton(i18n::tr("Remove"))) {
      for (int q = 0; q < M_Count; ++q)
        if (const ParamDesc* d = desc(modP(s, q))) setv(c, modP(s, q), d->def);
    }
    ImGui::PopID();
  }
  if (!slots.empty()) ImGui::Separator();
  if (ImGui::BeginMenu(i18n::tr("Link from"))) {
    int src = -1;
    if (sourceMenuItems(c, src) && src > SrcNone) addModulation(c, src, k, 0.25f);
    ImGui::EndMenu();
  }
  if (k >= kMacroBase && k < kMacroBase + kMacros && ImGui::MenuItem(i18n::tr("Rename..."))) macroRenameRequest() = k - kMacroBase;
  if (!slots.empty() && ImGui::MenuItem(i18n::tr("Show in Links"))) c.u.linkSel = slots.front(), openView(c.u, VLinks);
  ImGui::Separator();
  if (ImGui::MenuItem(i18n::tr("Automation, MIDI link..."))) openStd = true;
}
}  // namespace

// ---- links: Link mode, Alt-drag depth, the focused control, the reach record
namespace {
std::vector<char> gReach;  // GL_PLASMA_REACH: parameters a control was drawn for
struct RouteEnd {
  int k;
  ImVec2 at;
  float r;  // the control's half size, so a line ends at its edge
};
std::vector<RouteEnd> gRouteEnds;  // controls drawn this frame
ImVec2 gGrip[SourceCount];         // grips drawn this frame (x < 0: not drawn)
ImVec2 gFamAt[8];                  // a family's header row, for its closed sources
}  // namespace
void reach(int k) {
  if (k < 0) return;
  if (gReach.size() <= static_cast<size_t>(k)) gReach.resize(static_cast<size_t>(k) + 1, 0);
  gReach[static_cast<size_t>(k)] = 1;
}
const std::vector<char>& reachRecord() { return gReach; }
int& macroRenameRequest() {
  static int r = -1;
  return r;
}
void reachClear() { std::fill(gReach.begin(), gReach.end(), 0); }

int familyOf(int src) {
  if (src >= SrcEnv1 && src <= SrcEnv4) return FamEnv;
  if (src >= SrcLfo1 && src <= SrcLfo10Y) return FamLfo;
  if (src >= SrcMacro1 && src <= SrcMacro8) return FamMacro;
  switch (src) {
    case SrcVelo: case SrcNote: case SrcReleaseVelo: case SrcAftertouch: case SrcPolyAT: case SrcModWheel: case SrcPitchBend:
    case SrcExprX: case SrcExprY: case SrcExprZ: return FamKey;
    default: return FamChance;
  }
}
namespace {
ImU32 famCol(const Ctx& c, int src) { return c.P.fam[familyOf(std::max(src, 0))]; }
// The link of `src` into k (-1 = none).
int linkSlot(const Ctx& c, int src, int k) {
  for (int s = 0; s < kModSlots; ++s)
    if (vali(c, modP(s, M_Source)) == src && vali(c, modP(s, M_Dest)) == k + 1) return s;
  return -1;
}
// Link mode on the last item (a dial or a bar of parameter k): a drag up / down (or sideways
// on a bar) sets the depth of the armed source's link into k, made at the press. One pointer
// gesture, one undo step. True while it owns the item.
bool linkGesture(Ctx& c, int k, float pixels) {
  const int src = c.u.linkSrc;
  if (src < 0 || !modulatable(k)) return false;
  pui::devItemHere(c.t.pid(k));  // scripted tests find the control in Link mode too
  static int slot = -1;
  if (ImGui::IsItemActivated()) {
    slot = linkSlot(c, src, k);
    if (slot < 0) slot = addModulation(c, src, k, 0.0f);
  }
  if (ImGui::IsItemActive() && slot >= 0) {
    const ImGuiIO& io = ImGui::GetIO();
    const float d = io.MouseDelta.x - io.MouseDelta.y;
    if (d != 0) {
      const double amt = std::clamp(val(c, modP(slot, M_Amount)) + d / pixels * (io.KeyShift ? 0.15 : 1.0), -1.0, 1.0);
      setv(c, modP(slot, M_Amount), amt, true);
    }
  }
  if (ImGui::IsItemDeactivated()) c.a.doc.endGesture(), slot = -1;
  return true;
}
// Alt + drag on the last item: the depth of its newest link.
void altDepth(Ctx& c, int s, float pixels) {
  const ImGuiIO& io = ImGui::GetIO();
  const float d = io.MouseDelta.x - io.MouseDelta.y;
  if (d != 0) {
    const double amt = std::clamp(val(c, modP(s, M_Amount)) + d / pixels * (io.KeyShift ? 0.15 : 1.0), -1.0, 1.0);
    setv(c, modP(s, M_Amount), amt, true);
  }
  if (ImGui::IsItemDeactivated()) c.a.doc.endGesture();
}
// A control that can take the armed source shows it with an outline in the source's colour.
void linkOutline(const Ctx& c, int k, ImVec2 a, ImVec2 b, float r) {
  if (c.u.linkSrc < 0 || !modulatable(k)) return;
  c.dl->AddRect(ImVec2(a.x - c.s(1.5f), a.y - c.s(1.5f)), ImVec2(b.x + c.s(1.5f), b.y + c.s(1.5f)), famCol(c, c.u.linkSrc), c.s(r), 0,
                std::max(1.0f, c.s(1.2f)));
}
const char* kHintFree = "   (drag \xC2\xB7 Shift: fine \xC2\xB7 double-click: type or reset \xC2\xB7 drop a source here to link it \xC2\xB7 right-click: menu)";
const char* kHintLinked = "   (Alt+drag: depth of the newest link \xC2\xB7 double-click: type or reset \xC2\xB7 right-click: menu)";
}  // namespace

bool knob(Ctx& c, int k, float cx, float cy, float r, const char* label, int flags) {
  const ParamDesc* d = desc(k);
  if (!d) return false;
  reach(k);
  ImGuiIO& io = ImGui::GetIO();
  const ImVec2 C = c.p(cx, cy);
  const float R = c.s(r);
  ImGui::PushID(k);
  ImGui::SetCursorScreenPos(ImVec2(C.x - R - c.s(3), C.y - R - c.s(3)));
  ImGui::InvisibleButton("##k", ImVec2(R * 2 + c.s(6), R * 2 + c.s(6)), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
  const bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
  double v = val(c, k);
  bool changed = false;
  auto slots = modSlotsFor(c, k);
  if (hov && modulatable(k) && (k < kModBase || k >= kFxBase)) c.u.focusK = k;
  const bool rclick = hov && ImGui::IsMouseClicked(ImGuiMouseButton_Right);
  if (linkGesture(c, k, 160.0f)) {
  } else if (act && io.KeyAlt && !slots.empty()) {
    altDepth(c, slots.back(), 160.0f);
  } else if (rclick) {
    ImGui::OpenPopup("##plmod");
  } else {
    changed = interact(c, k, v, 160.0f, false);
  }
  dropTarget(c, k);
  bool openStd = false, openType = false;
  if (ImGui::BeginPopup("##plmod")) {
    modMenu(c, k, openStd, openType);
    ImGui::EndPopup();
  }
  if (openStd) ImGui::OpenPopup("##pmenu");
  if (openType) ImGui::OpenPopup("##type");
  // K-164 look: a track, the value arc in the block's colour (a low glow), a light cap with a
  // dark pointer
  const double n = d->toNormalized(v);
  const bool bip = (flags & KnobBipolar) || (d->min < 0 && d->max > 0);
  const float an = kA0 + static_cast<float>(n) * (kA1 - kA0), from = bip ? (kA0 + kA1) * 0.5f : kA0;
  const float th = std::max(1.5f, c.s(M(3)) * std::min(1.f, r / M(17)));
  const ImU32 vc = gKnobCol ? gKnobCol : c.P.accent;
  c.dl->PathArcTo(C, R, kA0, kA1, 40);
  c.dl->PathStroke(c.P.dark ? theme::mix(c.P.well, c.P.panel, 0.4f) : theme::mix(c.P.line, c.P.panel, 0.5f), 0, th);
  if (std::fabs(an - from) > 0.005f) {
    const float g = glowK(c);
    if (g > 0.01f) {
      c.dl->PathArcTo(C, R, std::min(from, an), std::max(from, an), 40);
      c.dl->PathStroke(theme::withAlpha(vc, 0.18f * g), 0, th + c.s(3) * g);
    }
    c.dl->PathArcTo(C, R, std::min(from, an), std::max(from, an), 40);
    c.dl->PathStroke(vc, 0, th);
  }
  {
    const float dr = R - c.s(M(5.5f)) * std::min(1.f, r / M(17));
    const ImU32 cap = hov || act ? skin::lighten(c.P.knob, 0.06f) : c.P.knob;
    c.dl->AddCircleFilled(C, dr, cap, 40);
    c.dl->AddCircle(C, dr, IM_COL32(0, 0, 0, c.P.dark ? 90 : 64), 40, 1.0f);
    const ImU32 ptr = c.P.dark ? theme::mix(c.P.well, c.P.frame, 0.3f) : c.P.panel;
    c.dl->AddLine(ImVec2(C.x + std::cos(an) * dr * 0.25f, C.y + std::sin(an) * dr * 0.25f),
                  ImVec2(C.x + std::cos(an) * (dr - c.s(2)), C.y + std::sin(an) * (dr - c.s(2))), ptr, std::max(1.4f, c.s(M(2.2f))));
  }
  layoutAdd(c, ImVec2(C.x - R, C.y - R), ImVec2(C.x + R, C.y + R), true, label);
  gRouteEnds.push_back({k, C, R + c.s(4)});
  if (hov || act) c.u.hoverKNext = k;
  if (c.u.hoverSrc >= 0 && linkSlot(c, c.u.hoverSrc, k) >= 0)  // its source is under the pointer: light it up
    c.dl->AddCircle(C, R + c.s(6), famCol(c, c.u.hoverSrc), 36, std::max(1.5f, c.s(2.2f)));
  // link bands: one thin arc per link just outside the ring, in its source's colour
  float ringR = R + c.s(M(3.6f));
  double live = 0;
  for (size_t i = 0; i < slots.size(); ++i) {
    const int s = slots[i];
    const bool byp = val(c, modP(s, M_Bypass)) >= 0.5;
    if (c.hasLive && !byp) live += c.live.mod[s];
    if (i >= 3) continue;
    const float amt = valf(c, modP(s, M_Amount)) * valf(c, modP(s, M_Out));
    const bool cen = val(c, modP(s, M_Bipolar)) >= 0.5;
    float a0 = an, a1 = std::clamp(an + amt * (kA1 - kA0), kA0, kA1);
    if (cen) a0 = std::clamp(an - amt * 0.5f * (kA1 - kA0), kA0, kA1), a1 = std::clamp(an + amt * 0.5f * (kA1 - kA0), kA0, kA1);
    const ImU32 col = byp ? c.P.textFaint : famCol(c, vali(c, modP(s, M_Source)));
    if (std::fabs(a1 - a0) > 0.01f) {
      c.dl->PathArcTo(C, ringR, std::min(a0, a1), std::max(a0, a1), 24);
      c.dl->PathStroke(theme::withAlpha(col, 0.9f), 0, std::max(1.4f, c.s(M(2))));
    }
    ringR += c.s(M(3));
  }
  if (slots.size() > 3) c.dl->AddCircleFilled(ImVec2(C.x + R * 0.9f, C.y - R * 0.9f), c.s(1.6f), c.P.textDim, 8);
  if (!slots.empty() && c.hasLive && c.live.voices > 0) {  // the live value: a dot on the link ring
    const float al = std::clamp(an + static_cast<float>(live) * (kA1 - kA0), kA0, kA1);
    const float rr = R + c.s(M(3.6f));
    glowDot(c, ImVec2(C.x + std::cos(al) * rr, C.y + std::sin(al) * rr), M(2.6f), famCol(c, vali(c, modP(slots.front(), M_Source))));
  }
  linkOutline(c, k, ImVec2(C.x - R - c.s(2), C.y - R - c.s(2)), ImVec2(C.x + R + c.s(2), C.y + R + c.s(2)), r + 2);
  // label (sentence case) under the dial; the value while it moves
  if (!(flags & KnobNoLabel) && label) {
    const float lp = (flags & KnobSmall) ? T(10) : T(11);
    if (flags & KnobValue) {  // the name, and the value under it
      fitLabel(c, cx, cy + r + M(2), std::max(r * 2 + 30, M(62)), label, hov ? c.P.text : c.P.label, lp, false);
      MonoText mono;
      std::string vs = paramText(k, v, &c.st.params);
      if (const size_t br = vs.find(" ["); br != std::string::npos && br > 0) vs.resize(br);  // one value under a dial (the rest on hover)
      fitLabel(c, cx, cy + r + M(2) + M(13), std::max(r * 2 + 30, M(62)), vs.c_str(), act ? c.P.accentText : c.P.text, T(10.5f), false);
    } else {
      const std::string vs = act ? paramText(k, v, &c.st.params) : std::string(label);
      fitLabel(c, cx, cy + r + M(3), std::max(r * 2 + 30, M(62)), vs.c_str(), act ? c.P.accentText : (hov ? c.P.text : c.P.label), lp, false);
    }
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_NoSharedDelay) || act) {
    std::string s = paramText(k, v, &c.st.params);
    if (!slots.empty()) s += "   (" + std::to_string(slots.size()) + (slots.size() == 1 ? " link)" : " links)");
    bubble(c, ImVec2(C.x, C.y - R - c.s(4)), s);
  }
  if (hov) c.a.setHint(paramLongName(k) + ": " + paramText(k, v, &c.st.params) + (slots.empty() ? kHintFree : kHintLinked)), helpTip(c, k);
  ImGui::PopID();
  return changed;
}

bool bar(Ctx& c, int k, float x, float y, float w, float h, const char* label, int flags) {
  layoutBox(c, x, y, w, h, label && *label ? label : "bar");
  const ParamDesc* d = desc(k);
  if (!d) return false;
  reach(k);
  ImGuiIO& io = ImGui::GetIO();
  ImGui::PushID(k);
  ImGui::PushID("bar");
  ImGui::SetCursorScreenPos(c.p(x, y));
  ImGui::InvisibleButton("##b", c.F.sz(w, h), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
  const bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
  double v = val(c, k);
  bool changed = false;
  auto slots = modSlotsFor(c, k);
  if (hov && modulatable(k) && (k < kModBase || k >= kFxBase)) c.u.focusK = k;
  const float px = std::max(60.0f, c.s(w));
  if (linkGesture(c, k, px)) {
  } else if (act && io.KeyAlt && !slots.empty()) {
    altDepth(c, slots.back(), px);
  } else if (hov && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
    ImGui::OpenPopup("##plmod");
  } else {
    changed = interact(c, k, v, px, true);
  }
  dropTarget(c, k);
  bool openStd = false, openType = false;
  if (ImGui::BeginPopup("##plmod")) {
    modMenu(c, k, openStd, openType);
    ImGui::EndPopup();
  }
  if (openStd) ImGui::OpenPopup("##pmenu");
  if (openType) ImGui::OpenPopup("##type");
  const float tp = (flags & BarField) ? T(11.5f) : ctlPx(h);
  const ImVec2 a = c.p(x, y), b = c.p(x + w, y + h);
  c.dl->AddRectFilled(a, b, act ? c.P.panelLo : c.P.field, c.s(3));
  const double n = std::clamp(d->toNormalized(v), 0.0, 1.0);
  const bool cen = (flags & BarCentre) || (d->min < 0 && d->max > 0);
  const float x0 = a.x + c.s(1), x1 = b.x - c.s(1);
  const float vx = x0 + static_cast<float>(n) * (x1 - x0), fx = cen ? (x0 + x1) * 0.5f : x0;
  const bool dim = (flags & BarDim) != 0;
  const bool plain = (flags & BarField) != 0;
  if (!plain && std::fabs(vx - fx) > 0.5f)
    c.dl->AddRectFilled(ImVec2(std::min(fx, vx), a.y + c.s(1)), ImVec2(std::max(fx, vx), b.y - c.s(1)),
                        theme::withAlpha(act ? c.P.accent : c.P.knob, dim ? 0.10f : (c.P.dark ? 0.20f : 0.30f)), c.s(2));
  if (!plain) {  // the value mark: two ticks (top and bottom) that leave the text clear
    const ImU32 mc = act ? c.P.accent : (dim ? c.P.textFaint : c.P.knob);
    const float tk = std::max(c.s(3.0f), (b.y - a.y) * 0.16f);  // short, so a label under the mark stays readable
    const float mx = std::clamp(vx, x0 + c.s(4), x1 - c.s(4));  // kept inside the rounded corners at either end
    c.dl->AddRectFilled(ImVec2(mx - c.s(1), a.y + c.s(2)), ImVec2(mx + c.s(1), a.y + c.s(2) + tk), mc);
    c.dl->AddRectFilled(ImVec2(mx - c.s(1), b.y - c.s(2) - tk), ImVec2(mx + c.s(1), b.y - c.s(2)), mc);
  }
  c.dl->AddRect(a, b, hov || act ? c.P.lineHi : c.P.line, c.s(3));
  gRouteEnds.push_back({k, ImVec2(b.x, (a.y + b.y) * 0.5f), 0.f});
  if (hov || act) c.u.hoverKNext = k;
  if (c.u.hoverSrc >= 0 && linkSlot(c, c.u.hoverSrc, k) >= 0)
    c.dl->AddRect(ImVec2(a.x - c.s(2), a.y - c.s(2)), ImVec2(b.x + c.s(2), b.y + c.s(2)), famCol(c, c.u.hoverSrc), c.s(4), 0,
                  std::max(1.5f, c.s(2.0f)));
  // link bands along the lower edge
  double live = 0;
  for (size_t i = 0; i < slots.size(); ++i) {
    const int s = slots[i];
    const bool byp = val(c, modP(s, M_Bypass)) >= 0.5;
    if (c.hasLive && !byp) live += c.live.mod[s];
    if (i >= 3) continue;
    const float amt = valf(c, modP(s, M_Amount)) * valf(c, modP(s, M_Out));
    float e0 = vx, e1 = std::clamp(vx + amt * (x1 - x0), x0, x1);
    if (val(c, modP(s, M_Bipolar)) >= 0.5) e0 = std::clamp(vx - amt * 0.5f * (x1 - x0), x0, x1), e1 = std::clamp(vx + amt * 0.5f * (x1 - x0), x0, x1);
    const float by = b.y - c.s(2.2f + 2.4f * static_cast<float>(i));
    c.dl->AddRectFilled(ImVec2(std::min(e0, e1), by - c.s(1.6f)), ImVec2(std::max(e0, e1) + 1, by),
                        byp ? c.P.textFaint : famCol(c, vali(c, modP(s, M_Source))));
  }
  if (!slots.empty() && c.hasLive && c.live.voices > 0) {
    const float lx = std::clamp(vx + static_cast<float>(live) * (x1 - x0), x0, x1);
    c.dl->AddTriangleFilled(ImVec2(lx - c.s(3), a.y), ImVec2(lx + c.s(3), a.y), ImVec2(lx, a.y + c.s(3.5f)), c.P.hot);
  }
  linkOutline(c, k, a, b, 3);
  // label left, value right (the label is clipped before the value)
  std::string vs = paramText(k, v, &c.st.params);
  float vw;
  {
    MonoText mono;
    vw = textW(c, vs.c_str(), tp, false);
    // a narrow bar shows the value's first part ("75%" of "75% [-5.0 dB]"); the hint has it all
    const std::string full = vs;
    if (vw > w - 12)
      if (const size_t cut = vs.find(" ["); cut != std::string::npos) vs = vs.substr(0, cut), vw = textW(c, vs.c_str(), tp, false);
    if (hov) c.a.setHint(paramLongName(k) + ": " + full + (slots.empty() ? kHintFree : kHintLinked)), helpTip(c, k);
  }
  // no room for the label as well: the value alone (the hint names it)
  const bool narrow = !label || !*label || vw + textW(c, label, tp, false) + 20 > w;
  if (!(flags & BarNoLabel) && !narrow && label && *label) {
    const ImVec2 c0 = c.p(x + 2, y), c1 = c.p(x + w - vw - 10, y + h);  // left of the text: a glyph's edge is never cut
    c.dl->PushClipRect(c0, ImVec2(std::max(c0.x, c1.x), c1.y), true);
    labelBeside(c, x + 6, y, h, label, dim ? c.P.textFaint : (hov ? c.P.text : c.P.label), tp, false);
    c.dl->PopClipRect();
  }
  {
    MonoText mono;
    const ImVec2 c0 = c.p(x + 2, y), c1 = c.p(x + w - 2, y + h);
    c.dl->PushClipRect(c0, c1, true);
    labelBeside(c, x + w - 6 - vw, y, h, vs.c_str(), act ? c.P.accentText : (dim ? c.P.textFaint : c.P.text), tp, false);
    c.dl->PopClipRect();
  }
  ImGui::PopID();
  ImGui::PopID();
  return changed;
}

bool chip(Ctx& c, int k, float x, float y, float w, float h, const char* label) {
  layoutBox(c, x, y, w, h, label && *label ? label : "toggle");
  reach(k);
  ImGui::PushID(k);
  ImGui::PushID("chip");
  ImGui::SetCursorScreenPos(c.p(x, y));
  const bool click = ImGui::InvisibleButton("##c", c.F.sz(w, h));
  const bool on = val(c, k) >= 0.5, hov = ImGui::IsItemHovered();
  if (click) setv(c, k, on ? 0 : 1);
  const ImVec2 a = c.p(x, y), b = c.p(x + w, y + h);
  c.dl->AddRectFilled(a, b, on ? c.P.headOn : (hov ? c.P.panelHi : c.P.field), c.s(3));
  c.dl->AddRect(a, b, hov ? c.P.lineHi : c.P.line, c.s(3));
  const ImVec2 dc = c.p(x + 8, y + h * 0.5f);
  if (on) c.dl->AddCircleFilled(dc, c.s(3.0f), c.P.good, 12);
  else c.dl->AddCircle(dc, c.s(3.0f), c.P.boxEdge, 12, std::max(1.0f, c.s(1.0f)));
  {
    const ImVec2 c0 = c.p(x + 14, y), c1 = c.p(x + w - 3, y + h);
    c.dl->PushClipRect(c0, c1, true);
    labelBeside(c, x + 15, y, h, label, on ? c.P.text : c.P.label, ctlPx(h), false);
    c.dl->PopClipRect();
  }
  if (hov) c.a.setHint(paramLongName(k) + (on ? ": on (click: off)" : ": off (click: on)")), helpTip(c, k);
  ImGui::PopID();
  ImGui::PopID();
  return click;
}

bool segments(Ctx& c, int k, float x, float y, float w, float h, const std::vector<const char*>& labels) {
  layoutBox(c, x, y, w, h, "segments");
  reach(k);
  ImGui::PushID(k);
  ImGui::PushID("seg");
  const int n = static_cast<int>(labels.size()), cur = vali(c, k);
  const float sw = w / std::max(1, n), pad = std::min(2.f, h * 0.08f);
  c.dl->AddRectFilled(c.p(x, y), c.p(x + w, y + h), c.P.field, c.s(M(4)));
  bool changed = false;
  for (int i = 0; i < n; ++i) {
    ImGui::PushID(i);
    const float sx = x + sw * i;
    ImGui::SetCursorScreenPos(c.p(sx, y));
    if (ImGui::InvisibleButton("##s", c.F.sz(sw, h)) && i != cur) setv(c, k, i), changed = true;
    const bool hov = ImGui::IsItemHovered(), on = i == cur;
    if (on) {
      c.dl->AddRectFilled(c.p(sx + pad, y + pad), c.p(sx + sw - pad, y + h - pad), c.P.accentSoft, c.s(M(3)));
      c.dl->AddRect(c.p(sx + pad, y + pad), c.p(sx + sw - pad, y + h - pad), c.P.accent, c.s(M(3)));
    } else if (hov) {
      c.dl->AddRectFilled(c.p(sx + pad, y + pad), c.p(sx + sw - pad, y + h - pad), c.P.panelHi, c.s(M(3)));
    }
    float px = ctlPx(h);  // a long label in a narrow segment steps down a size rather than clip
    if (const float tw = textW(c, labels[static_cast<size_t>(i)], px, false); tw > sw - 6) px = std::max(9.0f, px * (sw - 6) / tw);
    textMid(c, sx + 1, sx + sw - 1, y + h * 0.5f, labels[static_cast<size_t>(i)], on ? c.P.accentText : hov ? c.P.text : c.P.label, px, false);
    if (hov) c.a.setHint(paramLongName(k) + ": " + labels[static_cast<size_t>(i)]), helpTip(c, k);
    ImGui::PopID();
  }
  c.dl->AddRect(c.p(x, y), c.p(x + w, y + h), c.P.line, c.s(3));
  ImGui::PopID();
  ImGui::PopID();
  return changed;
}

namespace {
// A labelled choice field: label at the left, the choice at the right, a menu on click.
bool choiceField(Ctx& c, int k, float x, float y, float w, float h, const char* label) {
  layoutBox(c, x, y, w, h, label && *label ? label : "choice");
  const ParamDesc* d = desc(k);
  if (!d) return false;
  reach(k);
  ImGui::PushID(k);
  ImGui::PushID("cf");
  const int cur = vali(c, k);
  const int count = static_cast<int>(d->choices.size());
  const std::string shown = cur >= 0 && cur < count ? d->choices[static_cast<size_t>(cur)] : ptext(c, k);
  ImGui::SetCursorScreenPos(c.p(x, y));
  const bool click = ImGui::InvisibleButton("##f", c.F.sz(w, h));
  const bool hov = ImGui::IsItemHovered();
  bool changed = false;
  if (hov && ImGui::GetIO().MouseWheel != 0 && count > 0) {
    setv(c, k, std::clamp(cur + (ImGui::GetIO().MouseWheel > 0 ? -1 : 1), 0, count - 1)), changed = true;
  }
  if (click) ImGui::OpenPopup("##cfm");
  const ImVec2 a = c.p(x, y), b = c.p(x + w, y + h);
  c.dl->AddRectFilled(a, b, hov ? c.P.panelHi : c.P.field, c.s(3));
  c.dl->AddRect(a, b, hov ? c.P.lineHi : c.P.line, c.s(3));
  // label and value whole, never a cut word: a size smaller first, then the value alone (the
  // label stays in the hint), then the value a size smaller again
  float px = ctlPx(h), lw = 0;
  const float room = w - 22;
  const bool hasLabel = label && *label;
  auto need = [&](float p, bool withLabel) { return (withLabel ? textW(c, label, p, false) + 10 : 0.f) + textW(c, shown.c_str(), p, false); };
  bool showLabel = hasLabel;
  if (need(px, showLabel) > room) {
    const float smaller = std::max(9.0f, px - 1.5f);
    if (need(smaller, showLabel) <= room) px = smaller;
    else if (showLabel && need(px, false) <= room) showLabel = false;
    else showLabel = false, px = std::max(8.5f, std::min(px, px * room / std::max(1.f, need(px, false))));
  }
  if (showLabel) {
    lw = textW(c, label, px, false);
    labelBeside(c, x + 6, y, h, label, hov ? c.P.text : c.P.label, px, false);
    lw += 10;
  }
  {
    const float tw = textW(c, shown.c_str(), px, false);
    const float tx = std::max(x + 6 + lw, x + w - 16 - tw);
    const ImVec2 c0 = c.p(x + 4 + lw, y), c1 = c.p(x + w - 14, y + h);
    c.dl->PushClipRect(c0, c1, true);
    labelBeside(c, tx, y, h, shown.c_str(), c.P.text, px, false);
    c.dl->PopClipRect();
  }
  iconArrow(c.dl, c.p(x + w - 8, y + h * 0.5f), c.s(5.5f), 3, c.P.textDim);
  ImGui::SetNextWindowSizeConstraints(ImVec2(c.s(std::max(w, 140.0f)), 0), ImVec2(FLT_MAX, c.s(520)));
  if (ImGui::BeginPopup("##cfm")) {
    for (int i = 0; i < count; ++i)
      if (ImGui::Selectable(d->choices[static_cast<size_t>(i)].c_str(), i == cur)) setv(c, k, i), changed = true;
    ImGui::EndPopup();
  }
  if (hov) c.a.setHint(paramLongName(k) + ": " + shown + "   (click: choose \xC2\xB7 wheel: step)"), helpTip(c, k);
  ImGui::PopID();
  ImGui::PopID();
  return changed;
}
}  // namespace

bool field(Ctx& c, int k, float x, float y, float w, float h, const char* label) {
  const ParamDesc* d = desc(k);
  if (!d) return false;
  if (d->kind == ParamKind::Toggle) return chip(c, k, x, y, w, h, label);
  if (d->kind == ParamKind::Choice) return choiceField(c, k, x, y, w, h, label);
  return bar(c, k, x, y, w, h, label);
}

float fieldGrid(Ctx& c, float x, float y, float w, int cols, float rowH, const std::vector<FieldItem>& items, float h) {
  const float gap = 8, cw = (w - gap * (cols - 1)) / cols;
  int i = 0;
  for (const auto& it : items) {
    if (it.k >= 0) field(c, it.k, x + (cw + gap) * (i % cols), y + rowH * (i / cols), cw, h, it.label);
    ++i;
  }
  return y + rowH * ((i + cols - 1) / cols);
}

float card(Ctx& c, float x, float y, float w, float h, const char* title, int enable) {
  const ImVec2 a = c.p(x + 3, y + 3), b = c.p(x + w - 3, y + h - 3);
  gl::audit::group(a, b, "cell");
  c.dl->AddRectFilled(a, b, c.P.panel, c.s(6));
  c.dl->AddRect(a, b, theme::withAlpha(c.P.line, 0.7f), c.s(6));
  float tx = x + 14;
  if (enable >= 0) {
    reach(enable);
    ImGui::PushID(enable);
    ImGui::PushID("cardon");
    ImGui::SetCursorScreenPos(c.p(x + 9, y + 8));
    const bool on = val(c, enable) >= 0.5;
    if (ImGui::InvisibleButton("##on", c.F.sz(14, 14))) setv(c, enable, on ? 0 : 1);
    if (ImGui::IsItemHovered()) c.a.setHint(paramLongName(enable) + (on ? ": on (click: off)" : ": off (click: on)"));
    const ImVec2 dc = c.p(x + 16, y + 15);
    if (on) c.dl->AddCircleFilled(dc, c.s(4), c.P.good, 14);
    else c.dl->AddCircle(dc, c.s(4), c.P.boxEdge, 14, std::max(1.0f, c.s(1.2f)));
    ImGui::PopID();
    ImGui::PopID();
    tx = x + 26;
  }
  if (title && *title) text(c, tx, y + 8, title, c.P.text, 12.5f, true);
  return y + 28;
}

// The design's advanced panel: a block grown to the whole page. Its frame with the family strip,
// Back, the way back, the part's dot, its name and what it is set to (at kAdvHeadY), a line under
// the head. Returns the x right of the sub text.
float advHead(Ctx& c, const char* title, const char* sub, ImU32 fam, int src) {
  const Pal& P = c.P;
  const float x0 = kAdvX0, x1 = kAdvX1, y0 = kTopH + M(16), y1 = kPlayY - M(16), cy = kAdvHeadY;
  c.dl->AddRectFilled(c.p(x0, y0 + M(2)), c.p(x1, y1 + M(2)), IM_COL32(0, 0, 0, P.dark ? 40 : 14), c.s(M(6)));
  c.dl->AddRectFilled(c.p(x0, y0), c.p(x1, y1), P.panel, c.s(M(6)));
  c.dl->AddRect(c.p(x0, y0), c.p(x1, y1), P.line, c.s(M(6)));
  c.dl->AddRectFilled(c.p(x0 + M(16), y0 - M(0.5f)), c.p(x1 - M(16), y0 + M(1.5f)), theme::withAlpha(fam, 0.9f), c.s(M(1)));
  float x = kAdvIX0;
  {
    const float bw = textW(c, "Back", T(12), false) + M(28);
    if (btnLook(c, "advback", x, cy - M(14), bw, M(28), "", false, false, T(12))) beginClose(c.u);
    if (ImGui::IsItemHovered()) c.a.setHint("Back (Esc)");
    const ImVec2 m = c.p(x + M(12), cy);
    const float d = c.s(M(3));
    c.dl->AddLine(ImVec2(m.x + d * 0.5f, m.y - d), ImVec2(m.x - d * 0.5f, m.y), P.text, std::max(1.f, c.s(M(1.3f))));
    c.dl->AddLine(ImVec2(m.x - d * 0.5f, m.y), ImVec2(m.x + d * 0.5f, m.y + d), P.text, std::max(1.f, c.s(M(1.3f))));
    text(c, x + M(18), cy - T(12) * 0.68f, "Back", P.text, T(12), false);
    x += bw + M(12);
  }
  {
    const int tab = tabOf(c.u.back);
    static const char* const tabs[TabCount] = {"Synth", "Effects", "Links", "Play", "Setup"};
    const char* from = tab >= 0 && tab < TabCount ? tabs[tab] : "Synth";
    text(c, x, cy - T(11.5f) * 0.68f, from, P.textFaint, T(11.5f), false);
    x += textW(c, from, T(11.5f), false) + M(4);
    const ImVec2 m = c.p(x + M(3), cy);
    const float d = c.s(M(2.5f));
    c.dl->AddLine(ImVec2(m.x - d * 0.5f, m.y - d), ImVec2(m.x + d * 0.5f, m.y), P.textFaint, std::max(1.f, c.s(M(1.1f))));
    c.dl->AddLine(ImVec2(m.x + d * 0.5f, m.y), ImVec2(m.x - d * 0.5f, m.y + d), P.textFaint, std::max(1.f, c.s(M(1.1f))));
    x += M(16);
  }
  if (src >= 0) {
    sourceGrip(c, src, x + M(8), cy, M(8));
    x += M(16) + M(10);
  } else {
    glowDot(c, c.p(x + M(4), cy), M(4), P.good);
    x += M(8) + M(10);
  }
  text(c, x, cy - T(16) * 0.68f, title, P.text, T(16), true);
  x += textW(c, title, T(16), true) + M(12);
  if (sub && *sub) {
    text(c, x, cy - T(11.5f) * 0.68f, sub, P.textFaint, T(11.5f), false);
    x += textW(c, sub, T(11.5f), false) + M(12);
  }
  hline(c, x0, x1, kAdvLineY, P.line);
  return x;
}

float stageHead(Ctx& c, const char* title) {
  // a full-width view: Back first, then its title (the Play tab: its pages as a switch)
  panelRect(c, 0, kStageHeadY, kStageW, kStageY - kStageHeadY, c.P.frame);
  const int tab = tabOf(c.u.view);
  if (tab == TabSynth || c.u.large) {  // a block's advanced panel: Back to the blocks
    if (button(c, "back", 8, kStageHeadY + 3, 64, 24, "Back", false, 11.0f)) beginClose(c.u);
    if (ImGui::IsItemHovered()) c.a.setHint("Back to the Synth tab");
    iconArrow(c.dl, c.p(17, kStageHeadY + 15), c.s(6), 0, c.P.textDim);
    text(c, 84, kStageHeadY + 6, title, c.P.text, 14.5f, true);
    return 84 + textW(c, title, 14.5f, true) + 20;
  }
  if (tab != TabPlay) {
    text(c, 12, kStageHeadY + 6, title, c.P.text, 14.5f, true);
    return 12 + textW(c, title, 14.5f, true) + 20;
  }
  static const char* const names[5] = {"Voices", "Key and touch", "Arpeggiator", "Phrases", "Key zones"};
  static const int views[5] = {VVoices, VKeys, VArp, VPhrases, VZones};
  float x = 12;
  for (int i = 0; i < 5; ++i) {
    ImGui::PushID(i + 4400);
    const float bw = textW(c, names[i], 11.5f, true) + 28;
    if (button(c, "pg", x, kStageHeadY + 3, bw, 26, names[i], c.u.view == views[i], 11.5f)) openView(c.u, views[i]);
    ImGui::PopID();
    x += bw + 4;
  }
  return x + 16;
}

void display(const Ctx& c, float x, float y, float w, float h, int cols, int rows) {
  const ImVec2 a = c.p(x, y), b = c.p(x + w, y + h);
  gl::audit::group(a, b, "inset");
  c.dl->AddRectFilled(a, b, c.P.well, c.s(4));
  gridLines(c, x, y, w, h, cols, rows, false);
  c.dl->AddRect(a, b, c.P.line, c.s(4));
}

void sourceGrip(Ctx& c, int src, float cx, float cy, float r) {
  ImGui::PushID(src + 9000);
  const ImVec2 C = c.p(cx, cy);
  const float R = c.s(r);
  ImGui::SetCursorScreenPos(ImVec2(C.x - R - c.s(3), C.y - R - c.s(3)));
  const bool click = ImGui::InvisibleButton("##grip", ImVec2(R * 2 + c.s(6), R * 2 + c.s(6)));
  const bool hov = ImGui::IsItemHovered();
  if (hov || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
  if (hov) c.u.hoverSrcNext = src;
  if (src > SrcNone && src < SourceCount) gGrip[src] = C;
  const ImU32 col = famCol(c, src);
  const bool armed = c.u.linkSrc == src;
  if (click) c.u.linkSrc = armed ? -1 : src;
  float raw = 0;
  bool idle = !c.hasLive;
  if (c.hasLive && src > SrcNone && src < SourceCount) raw = c.live.source[src];
  if (src >= SrcMacro1 && src <= SrcMacro8 && !c.hasLive) raw = valf(c, macroP(src - SrcMacro1)), idle = false;
  bool bip = false;
  const float mv = sourceMeterValue(src, raw, bip);
  // the link dot: a ring in the family's colour and its live value as the centre's brightness
  if (armed) c.dl->AddCircleFilled(C, R + c.s(2.5f), theme::withAlpha(col, 0.35f), 20);
  (void)bip;
  c.dl->AddCircle(C, R, hov || armed ? skin::lighten(col, 0.15f) : col, 24, std::max(1.2f, c.s(M(1.5f))));
  const float lit = idle ? 1.f : 0.55f + 0.45f * std::clamp(std::fabs(mv), 0.f, 1.f);
  c.dl->AddCircleFilled(C, R * 0.4f, theme::withAlpha(col, lit), 16);
  if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoPreviewTooltip)) {
    ImGui::SetDragDropPayload("PLASMA_SRC", &src, sizeof(int));
    const ImVec2 m = ImGui::GetIO().MousePos;
    ImDrawList* fg = ImGui::GetForegroundDrawList();
    fg->AddLine(C, m, col, std::max(1.5f, c.s(1.6f)));
    fg->AddCircleFilled(m, c.s(4), col);
    const std::string s = sourceLabel(src);
    fg->AddText(c.a.fontBold, font(c, 10), ImVec2(m.x + c.s(8), m.y - c.s(14)), col, s.c_str());
    ImGui::EndDragDropSource();
  }
  if (hov) c.a.setHint(sourceLabel(src) + (armed ? ": Link mode is on (click or Esc: off)" : ": drag onto a control to link it \xC2\xB7 click: Link mode"));
  ImGui::PopID();
}

void routesBegin(Ctx& c) {
  c.u.hoverSrc = c.u.hoverSrcNext;
  c.u.hoverSrcNext = -1;
  c.u.hoverK = c.u.hoverKNext;
  c.u.hoverKNext = -1;
  gRouteEnds.clear();
  for (auto& g : gGrip) g = ImVec2(-1, -1);
  for (auto& f : gFamAt) f = ImVec2(-1, -1);
}
void routeFamily(int fam, float cx, float cy, const Ctx& c) {
  if (fam >= 0 && fam < 8) gFamAt[fam] = c.p(cx, cy);
}
void drawRoutes(Ctx& c) {
  const int src = c.u.linkSrc >= 0 ? c.u.linkSrc : c.u.hoverSrc;
  if (src <= SrcNone || src >= SourceCount) return;
  ImVec2 from = gGrip[src];
  if (from.x < 0) from = gFamAt[std::clamp(familyOf(src), 0, 7)];
  if (from.x < 0) return;
  const ImU32 col = famCol(c, src);
  std::vector<int> seen;
  for (const auto& e : gRouteEnds) {
    if (linkSlot(c, src, e.k) < 0 || std::find(seen.begin(), seen.end(), e.k) != seen.end()) continue;
    seen.push_back(e.k);
    const ImVec2 to(e.at.x + e.r, e.at.y);  // the side facing the dock
    const float dx = std::max(c.s(40), std::fabs(to.x - from.x) * 0.45f);
    c.dl->AddBezierCubic(from, ImVec2(from.x - dx, from.y), ImVec2(to.x + dx * 0.3f, to.y), to, theme::withAlpha(col, 0.55f),
                         std::max(1.2f, c.s(1.5f)));
    c.dl->AddCircleFilled(to, c.s(3), col, 10);
  }
}

namespace {
struct Later {
  uint64_t key;
  std::function<void(App&, const PluginTarget&)> fn;
};
std::vector<Later>& laterQueue() {
  static std::vector<Later> q;
  return q;
}
}  // namespace
void later(Ctx& c, std::function<void(App&, const PluginTarget&)> fn) { laterQueue().push_back({c.t.key(), std::move(fn)}); }
void runLater(App& a, const PluginTarget& t) {
  auto& q = laterQueue();
  if (q.empty()) return;
  std::vector<Later> mine;
  for (auto it = q.begin(); it != q.end();) {
    if (it->key == t.key()) mine.push_back(std::move(*it)), it = q.erase(it);
    else ++it;
  }
  for (auto& l : mine) l.fn(a, t);
}

bool knobFree(Ctx& c, const char* id, float& v, float cx, float cy, float r, const char* label, bool bipolar, const char* valueText,
              const ParamDesc& d) {
  const ImVec2 C = c.p(cx, cy);
  const float R = c.s(r);
  ImGui::PushID(id);
  ImGui::SetCursorScreenPos(ImVec2(C.x - R, C.y - R));
  ImGui::InvisibleButton("##kf", ImVec2(R * 2, R * 2));
  const bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
  bool changed = false;
  const float lo = bipolar ? -1.f : 0.f;
  // J-136: the shown value (d), its default and the type-in
  auto toV = [&](double value) { return static_cast<float>(lo + d.toNormalized(value) * (1.0 - lo)); };
  auto fromV = [&](float n) { return d.fromNormalized((n - lo) / (1.f - lo)); };
  const double pressed = gl::ui::ValueAtPress(fromV(v));
  // up or to the right turns it up (as the link depths), Shift for fine steps
  const float kd = ImGui::GetIO().MouseDelta.x - ImGui::GetIO().MouseDelta.y;
  if (act && kd != 0) {
    v = std::clamp(v + kd / 160.f * (1.f - lo) * (ImGui::GetIO().KeyShift ? 0.15f : 1.f), lo, 1.f);
    changed = true;
  }
  bool wantType = false;
  ParamDoubleClick dbl = ParamDoubleClick::Type;
  if (gl::ui::DoubleClicked(d, pressed, &dbl)) {
    if (dbl == ParamDoubleClick::Reset) v = toV(d.def), changed = true;
    else wantType = true;
  }
  if (hov && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup("##kfmenu");
  if (ImGui::BeginPopup("##kfmenu")) {
    ImGui::TextDisabled("%s", d.name.c_str());
    ImGui::Separator();
    if (gl::ui::ResetMenuItem(d, fromV(v))) v = toV(d.def), changed = true;
    if (ImGui::MenuItem(i18n::tr("Type a value..."))) wantType = true;
    ImGui::EndPopup();
  }
  if (wantType) ImGui::OpenPopup("##type");
  double typed = fromV(v);
  if (gl::ui::ValueTypePopup(d, &typed)) v = toV(typed), changed = true;
  if (hov) c.a.setHint(d.name + ": " + d.format(fromV(v)) + "   (" + gl::ui::ParamControlHint() + ")");
  const float n = (v - lo) / (1.f - lo);
  skin::KnobPaint kp;
  kp.rail = c.P.rail, kp.fill = c.P.accent, kp.cap = c.P.capTop, kp.pointer = c.P.pointer;
  kp.a0 = kA0, kp.a1 = kA1, kp.an = kA0 + n * (kA1 - kA0), kp.from = bipolar ? (kA0 + kA1) * 0.5f : kA0;
  kp.hot = hov, kp.active = act, kp.railTh = std::max(2.0f, R * 0.17f);
  skin::knobFace(c.dl, C, R, kp);
  if (label) textC(c, cx, cy + r + 4, act && valueText ? valueText : label, act ? c.P.accentText : c.P.textDim, 8.5f, true);
  if ((hov || act) && valueText) bubble(c, ImVec2(C.x, C.y - R - c.s(4)), valueText);
  ImGui::PopID();
  return changed;
}

namespace {
bool fader(Ctx& c, int k, float x, float y, float w, float h, bool vertical) {
  const ParamDesc* d = desc(k);
  if (!d) return false;
  reach(k);
  ImGui::PushID(k);
  ImGui::SetCursorScreenPos(c.p(x, y));
  ImGui::InvisibleButton("##f", c.F.sz(w, h), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
  const bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
  double v = val(c, k);
  const ImVec2 a = c.p(x, y), b = c.p(x + w, y + h);
  const float knobL = c.s(vertical ? 7 : 9);
  bool ch;
  if (vertical) ch = [&] {
      const ParamDesc& dd = *d;
      (void)dd;
      return pui::dragParamTrack(c.a, c.t.pid(k), *d, v, b.y - knobL * 0.5f, -(b.y - a.y - knobL), false);
    }();
  else
    ch = pui::dragParamTrack(c.a, c.t.pid(k), *d, v, a.x + knobL * 0.5f, b.x - a.x - knobL, true);
  dropTarget(c, k);
  const double n = d->toNormalized(v);
  const float th = c.s(3);
  if (vertical) {
    const float cx = std::round((a.x + b.x) * 0.5f);
    c.dl->AddRectFilled(ImVec2(cx - th * 0.5f, a.y + knobL * 0.5f), ImVec2(cx + th * 0.5f, b.y - knobL * 0.5f), c.P.rail, th);
    const float ky = b.y - knobL * 0.5f - static_cast<float>(n) * (b.y - a.y - knobL);
    c.dl->AddRectFilled(ImVec2(cx - th * 0.5f, ky), ImVec2(cx + th * 0.5f, b.y - knobL * 0.5f), c.P.accent, th);
    const ImVec2 k0(a.x + c.s(1), ky - knobL * 0.5f), k1(b.x - c.s(1), ky + knobL * 0.5f);
    skin::gradientRect(c.dl, k0, k1, c.P.capTop, c.P.capBot, c.s(2));
    c.dl->AddLine(ImVec2(k0.x + c.s(2), ky), ImVec2(k1.x - c.s(2), ky), act || hov ? c.P.accent : c.P.textDim, std::max(1.0f, c.s(1.2f)));
  } else {
    const float cy = std::round((a.y + b.y) * 0.5f);
    c.dl->AddRectFilled(ImVec2(a.x + knobL * 0.5f, cy - th * 0.5f), ImVec2(b.x - knobL * 0.5f, cy + th * 0.5f), c.P.rail, th);
    const float kx = a.x + knobL * 0.5f + static_cast<float>(n) * (b.x - a.x - knobL);
    const bool bip = d->min < 0 && d->max > 0;
    const float from = bip ? (a.x + b.x) * 0.5f : a.x + knobL * 0.5f;
    c.dl->AddRectFilled(ImVec2(std::min(from, kx), cy - th * 0.5f), ImVec2(std::max(from, kx), cy + th * 0.5f), c.P.accent, th);
    const ImVec2 k0(kx - knobL * 0.5f, a.y + c.s(1)), k1(kx + knobL * 0.5f, b.y - c.s(1));
    skin::gradientRect(c.dl, k0, k1, c.P.capTop, c.P.capBot, c.s(2));
    c.dl->AddLine(ImVec2(kx, k0.y + c.s(2)), ImVec2(kx, k1.y - c.s(2)), act || hov ? c.P.accent : c.P.textDim, std::max(1.0f, c.s(1.2f)));
  }
  if (hov || act) {
    bubble(c, vertical ? ImVec2((a.x + b.x) * 0.5f, a.y) : ImVec2((a.x + b.x) * 0.5f, a.y), paramText(k, v, &c.st.params));
    c.a.setHint(paramLongName(k) + ": " + paramText(k, v, &c.st.params));
    helpTip(c, k);
  }
  ImGui::PopID();
  return ch;
}
}  // namespace
bool hfader(Ctx& c, int k, float x, float y, float w, float h) { return fader(c, k, x, y, w, h, false); }
bool vfader(Ctx& c, int k, float x, float y, float w, float h) { return fader(c, k, x, y, w, h, true); }

bool led(Ctx& c, int k, float x, float y, float size) {
  reach(k);
  ImGui::PushID(k);
  ImGui::PushID("led");
  ImGui::SetCursorScreenPos(c.p(x - 2, y - 2));
  const bool click = ImGui::InvisibleButton("##l", c.F.sz(size + 4, size + 4));
  const bool on = val(c, k) >= 0.5;
  if (click) setv(c, k, on ? 0 : 1);
  {  // a round light: lit green with a low glow, off grey
    const ImVec2 C = c.p(x + size * 0.5f, y + size * 0.5f);
    const float rr = c.s(std::min(size * 0.5f, M(4)));
    if (on) {
      const float g = glowK(c);
      if (g > 0.01f) c.dl->AddCircleFilled(C, rr + c.s(3) * g, theme::withAlpha(c.P.good, 0.22f * g), 16);
      c.dl->AddCircleFilled(C, rr, c.P.good, 16);
    } else {
      c.dl->AddCircleFilled(C, rr, c.P.textFaint, 16);
    }
  }
  if (ImGui::IsItemHovered()) c.a.setHint(paramLongName(k) + (on ? ": on (click: off)" : ": off (click: on)")), helpTip(c, k);
  ImGui::PopID();
  ImGui::PopID();
  return click;
}
bool checkBox(Ctx& c, int k, float x, float y, const char* label, float px) {
  reach(k);
  ImGui::PushID(k);
  ImGui::PushID("chk");
  const bool bold = px < 9.0f;  // J-202: caps captions bold, sentence-case labels regular
  const float w = 13 + textW(c, label, px, bold);
  ImGui::SetCursorScreenPos(c.p(x, y - 1));
  const bool click = ImGui::InvisibleButton("##c", c.F.sz(w, 11));
  const bool on = val(c, k) >= 0.5;
  if (click) setv(c, k, on ? 0 : 1);
  const ImVec2 a = c.p(x, y + 0.5f), b = c.p(x + 8.5f, y + 9);
  if (on) skin::gradientRect(c.dl, a, b, skin::lighten(c.P.accent, 0.2f), c.P.accent, c.s(2));
  else c.dl->AddRectFilled(a, b, c.P.field, c.s(2)), c.dl->AddRect(a, b, c.P.boxEdge, c.s(2));
  labelBeside(c, x + 13, y - 4.25f, 18, label, on || !bold ? c.P.text : c.P.textDim, px, bold);  // J-202: on the box's centre line
  if (ImGui::IsItemHovered()) c.a.setHint(paramLongName(k) + (on ? ": on" : ": off")), helpTip(c, k);
  ImGui::PopID();
  ImGui::PopID();
  return click;
}
bool button(Ctx& c, const char* id, float x, float y, float w, float h, const char* label, bool on, float px) {
  layoutBox(c, x, y, w, h, label && *label ? label : id);
  ImGui::PushID(id);
  ImGui::SetCursorScreenPos(c.p(x, y));
  const bool click = ImGui::InvisibleButton("##b", c.F.sz(w, h));
  const bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
  const ImVec2 a = c.p(x, y), b = c.p(x + w, y + h);
  if (on) {  // lit: the accent's soft fill and edge, accent text
    c.dl->AddRectFilled(a, b, c.P.accentSoft, c.s(M(4)));
    c.dl->AddRect(a, b, c.P.accent, c.s(M(4)));
  } else {
    c.dl->AddRectFilled(a, b, act ? c.P.panelLo : (hov ? c.P.headOn : c.P.panelHi), c.s(M(4)));
    c.dl->AddRect(a, b, hov ? c.P.lineHi : c.P.line, c.s(M(4)));
  }
  if (label && *label) textMid(c, x, x + w, y + h * 0.5f, label, on ? c.P.accentText : (hov ? c.P.text : c.P.label), px, false);
  ImGui::PopID();
  return click;
}
bool seg2(Ctx& c, const char* id, float x, float y, float w, float h, const char* a, const char* b, bool second, const char* hint, float px) {
  layoutBox(c, x, y, w, h, id);
  ImGui::PushID(id);
  ImGui::SetCursorScreenPos(c.p(x, y));
  const bool click = ImGui::InvisibleButton("##s2", c.F.sz(w, h));
  const bool hov = ImGui::IsItemHovered();
  c.dl->AddRectFilled(c.p(x, y), c.p(x + w, y + h), c.P.field, c.s(M(4)));
  c.dl->AddRect(c.p(x, y), c.p(x + w, y + h), hov ? c.P.lineHi : c.P.line, c.s(M(4)));
  const float hw = w * 0.5f, sx = x + (second ? hw : 0), pd = M(2);
  c.dl->AddRectFilled(c.p(sx + pd, y + pd), c.p(sx + hw - pd, y + h - pd), c.P.accentSoft, c.s(M(3)));
  c.dl->AddRect(c.p(sx + pd, y + pd), c.p(sx + hw - pd, y + h - pd), c.P.accent, c.s(M(3)));
  textMid(c, x, x + hw, y + h * 0.5f, a, second ? c.P.label : c.P.accentText, px, false);
  textMid(c, x + hw, x + w, y + h * 0.5f, b, second ? c.P.accentText : c.P.label, px, false);
  if (hov && hint) c.a.setHint(hint);
  ImGui::PopID();
  return click;
}
bool toggleBtn(Ctx& c, int k, float x, float y, float w, float h, const char* label, float px) {
  reach(k);
  ImGui::PushID(k);
  const bool on = val(c, k) >= 0.5;
  const bool click = button(c, "tg", x, y, w, h, label, on, px);
  if (click) setv(c, k, on ? 0 : 1);
  if (ImGui::IsItemHovered()) c.a.setHint(paramLongName(k) + (on ? ": on" : ": off")), helpTip(c, k);
  ImGui::PopID();
  return click;
}

bool valueBox(Ctx& c, int k, float x, float y, float w, float h, const char* label, const char* override) {
  reach(k);
  ImGui::PushID(k);
  ImGui::PushID("vb");
  ImGui::SetCursorScreenPos(c.p(x, y));
  ImGui::InvisibleButton("##v", c.F.sz(w, h), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
  const bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
  double v = val(c, k);
  const bool ch = interact(c, k, v, 90.0f, false);
  dropTarget(c, k);
  const ImVec2 a = c.p(x, y), b = c.p(x + w, y + h);
  c.dl->AddRectFilled(a, b, act ? c.P.panelLo : c.P.field, c.s(2));
  c.dl->AddRect(a, b, hov || act ? c.P.lineHi : c.P.line, c.s(2));
  float vx0 = x;
  const std::string s = override ? std::string(override) : paramText(k, v, &c.st.params);
  if (label && *label) {
    // J-128 / J-202: the label sits on the value's baseline
    labelBeside(c, x + 4, y, h, label, c.P.textDim, 8.5f);
    vx0 = x + 6 + textW(c, label, 8.5f, true);
  }
  {
    MonoText mono;
    textMid(c, vx0, x + w - 2, y + h * 0.5f, s.c_str(), act ? c.P.accentText : c.P.text, ty::kValue, false);
  }
  if (!modSlotsFor(c, k).empty()) c.dl->AddRectFilled(ImVec2(b.x - c.s(4), a.y + c.s(2)), ImVec2(b.x - c.s(2), b.y - c.s(2)), c.P.mod, 1);
  if (hov) c.a.setHint(paramLongName(k) + ": " + paramText(k, v, &c.st.params) + "   (drag \xC2\xB7 double-click: type \xC2\xB7 right-click: menu)"), helpTip(c, k);
  ImGui::PopID();
  ImGui::PopID();
  return ch;
}

bool dropField(Ctx& c, const char* id, float x, float y, float w, float h, const char* shown, const std::function<void()>& items, bool arrows,
               int* step, bool placeholder) {
  layoutBox(c, x, y, w, h, shown && *shown ? shown : id);
  ImGui::PushID(id);
  const float aw = arrows ? std::max(13.0f, h * 0.6f) : 0.0f;
  const float fw = w - aw * 2;
  ImGui::SetCursorScreenPos(c.p(x, y));
  const bool click = ImGui::InvisibleButton("##d", c.F.sz(fw, h));
  const bool hov = ImGui::IsItemHovered();
  const ImVec2 a = c.p(x, y), b = c.p(x + fw, y + h);
  c.dl->AddRectFilled(a, b, hov ? c.P.panelHi : c.P.field, c.s(2));
  c.dl->AddRect(a, b, hov ? c.P.lineHi : c.P.line, c.s(2));
  textMid(c, x + 2, x + fw - 12, y + h * 0.5f, shown, placeholder ? c.P.textDim : c.P.text, ctlPx(h), false);
  iconArrow(c.dl, c.p(x + fw - 8, y + h * 0.5f), c.s(std::max(6.f, h * 0.28f)), 3, c.P.textDim);
  if (click) ImGui::OpenPopup("##dd");
  bool changed = false;
  ImGui::SetNextWindowSizeConstraints(ImVec2(c.s(std::max(w, 140.0f)), 0), ImVec2(FLT_MAX, c.s(520)));
  if (ImGui::BeginPopup("##dd")) {
    items();
    ImGui::EndPopup();
  }
  if (arrows && step) {
    // J-202: one joined < > pair, 2 units clear of the field
    const float pw = (w - fw - 2) * 0.5f;
    for (int i = 0; i < 2; ++i) {
      const float bx = x + fw + 2 + pw * i;
      ImGui::SetCursorScreenPos(c.p(bx, y));
      ImGui::PushID(i);
      if (ImGui::InvisibleButton("##ar", c.F.sz(pw, h))) *step = i == 0 ? -1 : 1, changed = true;
      const bool ah = ImGui::IsItemHovered();
      c.dl->AddRectFilled(c.p(bx, y), c.p(bx + pw, y + h), ah ? c.P.headOn : c.P.head, c.s(2),
                          i == 0 ? ImDrawFlags_RoundCornersLeft : ImDrawFlags_RoundCornersRight);
      if (i == 1) vline(c, bx, y + 3, y + h - 3, c.P.line);
      iconArrow(c.dl, c.p(bx + pw * 0.5f, y + h * 0.5f), c.s(std::max(6.5f, h * 0.3f)), i == 0 ? 0 : 1, ah ? c.P.text : c.P.textDim);
      ImGui::PopID();
    }
  }
  ImGui::PopID();
  return changed;
}

bool choiceBox(Ctx& c, int k, float x, float y, float w, float h, bool arrows, const char* override, const std::vector<std::string>* names) {
  const ParamDesc* d = desc(k);
  if (!d) return false;
  reach(k);
  ImGui::PushID(k);
  const int cur = vali(c, k);
  const int count = d->kind == ParamKind::Choice ? static_cast<int>(d->choices.size()) : static_cast<int>(d->max) + 1;
  const std::vector<std::string>& labels = names ? *names : d->choices;
  std::string shown = override ? std::string(override) : (cur >= 0 && cur < static_cast<int>(labels.size()) ? labels[static_cast<size_t>(cur)] : ptext(c, k));
  int step = 0;
  bool changed = false;
  dropField(c, "cb", x, y, w, h, shown.c_str(),
            [&] {
              for (int i = 0; i < count && i < static_cast<int>(labels.size()); ++i)
                if (ImGui::Selectable(labels[static_cast<size_t>(i)].c_str(), i == cur)) setv(c, k, i), changed = true;
            },
            arrows, &step);
  if (step) setv(c, k, std::clamp(cur + step, 0, count - 1)), changed = true;
  if (ImGui::IsItemHovered()) c.a.setHint(paramLongName(k) + ": " + shown), helpTip(c, k);
  ImGui::PopID();
  return changed;
}

bool stepperFree(Ctx& c, const char* id, int& v, int lo, int hi, float x, float y, float w, float h, const char* shown) {
  ImGui::PushID(id);
  ImGui::SetCursorScreenPos(c.p(x, y));
  ImGui::InvisibleButton("##s", c.F.sz(w - 10, h));
  const bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
  bool changed = false;
  static float acc = 0;
  if (ImGui::IsItemActivated()) acc = 0;
  if (act) {
    acc -= ImGui::GetIO().MouseDelta.y / 8.0f;
    while (acc >= 1) acc -= 1, v = std::min(hi, v + 1), changed = true;
    while (acc <= -1) acc += 1, v = std::max(lo, v - 1), changed = true;
  }
  if (hov && ImGui::GetIO().MouseWheel != 0) v = std::clamp(v + (ImGui::GetIO().MouseWheel > 0 ? 1 : -1), lo, hi), changed = true;
  const ImVec2 a = c.p(x, y), b = c.p(x + w, y + h);
  c.dl->AddRectFilled(a, b, c.P.field, c.s(2));
  c.dl->AddRect(a, b, hov || act ? c.P.lineHi : c.P.line, c.s(2));
  char buf[32];
  std::snprintf(buf, sizeof buf, "%d", v);
  {
    MonoText mono;
    textMid(c, x, x + w - 10, y + h * 0.5f, shown ? shown : buf, act ? c.P.accentText : c.P.text, ty::kValue, false);
  }
  for (int i = 0; i < 2; ++i) {
    ImGui::SetCursorScreenPos(c.p(x + w - 10, y + h * 0.5f * i));
    ImGui::PushID(i);
    if (ImGui::InvisibleButton("##ud", c.F.sz(10, h * 0.5f))) v = std::clamp(v + (i == 0 ? 1 : -1), lo, hi), changed = true;
    iconArrow(c.dl, c.p(x + w - 5, y + h * (i == 0 ? 0.3f : 0.7f)), c.s(5.5f), i == 0 ? 2 : 3, ImGui::IsItemHovered() ? c.P.text : c.P.textDim);
    ImGui::PopID();
  }
  ImGui::PopID();
  return changed;
}
bool stepper(Ctx& c, int k, float x, float y, float w, float h, const char* override) {
  layoutBox(c, x, y, w, h, "stepper");
  const ParamDesc* d = desc(k);
  if (!d) return false;
  reach(k);
  ImGui::PushID(k);
  int v = vali(c, k);
  const int lo = static_cast<int>(d->min), hi = d->kind == ParamKind::Choice ? static_cast<int>(d->choices.size()) - 1 : static_cast<int>(d->max);
  std::string shown = override ? override : ptext(c, k);
  const bool ch = stepperFree(c, "st", v, lo, hi, x, y, w, h, shown.c_str());
  if (ch) setv(c, k, v);
  ImGui::PopID();
  return ch;
}

int tabs(Ctx& c, const char* id, float x, float y, float w, float h, const std::vector<std::string>& names, int cur, int srcBase, float px) {
  ImGui::PushID(id);
  const int n = static_cast<int>(names.size());
  const float tw = w / std::max(1, n);
  int clicked = -1;
  for (int i = 0; i < n; ++i) {
    const float tx = x + tw * i;
    ImGui::PushID(i);
    ImGui::SetCursorScreenPos(c.p(tx, y));
    if (ImGui::InvisibleButton("##t", c.F.sz(tw - (srcBase >= 0 ? 18 : 0), h))) clicked = i;  // J-128: clear of the drag handle (it spans tw - 17 .. tw - 3)
    const bool hov = ImGui::IsItemHovered();
    const bool on = i == cur;
    {  // separate boxes with a gap; the current tab is lit and points into its panel
      const ImVec2 a = c.p(tx + kGutter, y + kGutter), b = c.p(tx + tw - kGutter, y + h - 1);
      const ImU32 bg = on ? c.P.headOn : (hov ? skin::lighten(c.P.head, 0.03f) : c.P.head);
      skin::gradientRect(c.dl, a, b, skin::lighten(bg, c.P.dark ? 0.05f : 0.03f), bg, c.s(2.0f), ImDrawFlags_RoundCornersTop);
      if (on) {
        const float nx = (a.x + b.x) * 0.5f - c.s(8);
        c.dl->AddTriangleFilled(ImVec2(nx, b.y), ImVec2(nx + c.s(8), b.y + c.s(5)), ImVec2(nx + c.s(16), b.y), bg);
        c.dl->AddRectFilled(ImVec2(a.x + c.s(3), a.y), ImVec2(b.x - c.s(3), a.y + c.s(1.6f)), c.P.accent, c.s(1));
      }
    }
    text(c, tx + 9, y + (h - px - 2) * 0.5f + 0.5f, names[static_cast<size_t>(i)].c_str(), on ? c.P.text : c.P.label, px, true);
    if (srcBase >= 0) sourceHandle(c, srcBase + i, tx + tw - 10, y + h * 0.5f, 5.0f);
    ImGui::PopID();
  }
  ImGui::PopID();
  return clicked;
}

// Popup rows (inside BeginPopup): one ImGui control per parameter.
void paramRows(Ctx& c, const std::vector<int>& ks) {
  for (int k : ks) {
    const ParamDesc* d = desc(k);
    if (!d) continue;
    ImGui::PushID(k);
    const std::string nm = paramLongName(k);
    double v = val(c, k);
    if (d->kind == ParamKind::Toggle) {
      bool b = v >= 0.5;
      if (ImGui::Checkbox(nm.c_str(), &b)) setv(c, k, b ? 1 : 0);
    } else if (d->kind == ParamKind::Choice) {
      const int cur = static_cast<int>(std::lround(v));
      ImGui::SetNextItemWidth(c.s(150));
      if (ImGui::BeginCombo(nm.c_str(), cur >= 0 && cur < static_cast<int>(d->choices.size()) ? d->choices[static_cast<size_t>(cur)].c_str() : "?")) {
        for (int i = 0; i < static_cast<int>(d->choices.size()); ++i)
          if (ImGui::Selectable(d->choices[static_cast<size_t>(i)].c_str(), i == cur)) setv(c, k, i);
        ImGui::EndCombo();
      }
    } else {
      float n = static_cast<float>(d->toNormalized(v));
      const std::string shown = paramText(k, v, &c.st.params);
      ImGui::SetNextItemWidth(c.s(150));
      if (ImGui::SliderFloat(nm.c_str(), &n, 0.f, 1.f, shown.c_str())) {
        double nv = d->fromNormalized(n);
        if (integerParam(k)) nv = std::round(nv);
        setv(c, k, nv, true);
      }
      if (ImGui::IsItemDeactivatedAfterEdit()) c.a.doc.endGesture();
    }
    ImGui::PopID();
  }
}

void moreButton(Ctx& c, const char* id, float x, float y, const std::vector<int>& ks, const std::function<void()>& extra, const char* hint) {
  for (int k : ks) reach(k);  // its popup holds a control for each
  ImGui::PushID(id);
  ImGui::SetCursorScreenPos(c.p(x, y));
  if (ImGui::InvisibleButton("##more", c.F.sz(18, 16))) ImGui::OpenPopup("##morepop");
  const bool hv = ImGui::IsItemHovered();
  c.dl->AddRectFilled(c.p(x, y), c.p(x + 18, y + 16), hv ? c.P.headOn : c.P.head, c.s(2));
  ::gl::ui::drawIconFit(c.dl, ::gl::ui::Icon::More, c.p(x + 3.7f, y + 6.7f), c.p(x + 14.3f, y + 9.3f), hv ? c.P.text : c.P.textDim);
  if (hv) c.a.setHint(hint ? hint : "More settings");
  if (ImGui::BeginPopup("##morepop")) {
    paramRows(c, ks);
    if (extra) extra();
    ImGui::EndPopup();
  }
  ImGui::PopID();
}

bool moreLink(Ctx& c, const char* id, float x, float y, float w, float h, const char* label, const std::vector<int>& ks,
              const std::function<void()>& extra, const char* hint) {
  for (int k : ks) reach(k);  // its popup holds a control for each
  ImGui::PushID(id);
  const bool click = button(c, "##ml", x, y, w, h, label, false, ctlPx(h));
  if (ImGui::IsItemHovered()) c.a.setHint(hint ? hint : "More settings");
  if (click) ImGui::OpenPopup("##morepop");
  bool open = false;
  if (ImGui::BeginPopup("##morepop")) {
    open = true;
    paramRows(c, ks);
    if (extra) extra();
    ImGui::EndPopup();
  }
  ImGui::PopID();
  return open;
}

bool cardMore(Ctx& c, const char* id, float x, float y, float w, float h, const std::vector<FieldItem>& items, const char* hint) {
  for (const auto& it : items) reach(it.k);
  ImGui::PushID(id);
  const unsigned key = ImGui::GetID("turned");
  bool open = c.u.moreOpen == key;
  if (button(c, "##cm", x + w - 96, y + 6, 90, 22, open ? "Back" : "More...", open, ctlPx(22))) {
    open = !open;
    c.u.moreOpen = open ? key : 0;
  }
  if (ImGui::IsItemHovered()) c.a.setHint(open ? std::string("Back to the main settings") : std::string(hint ? hint : "More settings"));
  if (open) {
    const int cols = w > 420 ? 2 : (items.size() > 3 ? 2 : 1);
    const int rows = (static_cast<int>(items.size()) + cols - 1) / cols;
    fieldGrid(c, x + 12, y + 34, w - 24, cols, std::clamp((h - 44) / static_cast<float>(std::max(1, rows)), 30.f, 40.f), items, 26);
  }
  ImGui::PopID();
  return open;
}

namespace {
bool gBlockMore = false;  // the block being drawn shows its details (More, or Full)
}
float gPicFit = 0;
ImU32 gKnobCol = 0;
bool gLayoutSettling = false, gPicUsed = false;
float gDialBase = 0, gDialPushed = 0;
bool showDetails(const Ctx& c) { return c.u.full || gBlockMore; }
namespace {
std::vector<ImGuiID> gPictures;
}
void markPicture() { gPictures.push_back(ImGui::GetItemID()); }
bool isPicture(unsigned id) { return std::find(gPictures.begin(), gPictures.end(), id) != gPictures.end(); }
void clearPictures() { gPictures.clear(); }
DetailsOn::DetailsOn() : prev(gBlockMore) { gBlockMore = true; }
DetailsOn::~DetailsOn() { gBlockMore = prev; }
// A group inside a block: its name and a short summary. Its details show when the block's More is
// on, or in Full. Moves y below the row.
bool section(Ctx& c, const char* id, float x, float& y, float w, const char* title, const std::string& summary, bool hasDetails) {
  (void)id;
  const bool details = hasDetails && (c.u.full || gBlockMore);
  text(c, x, y + 4, title, c.P.label, 11.5f, true);
  if (!summary.empty()) {  // the summary only when it fits whole (never cut)
    const float tw = textW(c, title, 11.5f, true);
    if (textW(c, summary.c_str(), 11.0f, false) <= w - tw - 12) textR(c, x + w, y + 5, summary.c_str(), c.P.textDim, 11.0f, false);
  }
  y += 24;
  return details;
}

// ---- blocks: one synth part each (a layer, a filter, the amp, an effect, a motion source). The
// same frame and title bar draw the Patch view's boxes.
void blockFrame(const Ctx& c, float x, float y, float w, float h, ImU32 fam, bool sel, bool dim) {
  const ImVec2 a = c.p(x, y), b = c.p(x + w, y + h);
  gl::audit::group(a, b, "cell");
  const float rr = c.s(M(6));
  c.dl->AddRectFilled(ImVec2(a.x, a.y + c.s(1)), ImVec2(b.x, b.y + c.s(2)), IM_COL32(0, 0, 0, c.P.dark ? 52 : 16), rr);  // the shadow
  c.dl->AddRectFilled(a, b, c.P.panel, rr);
  c.dl->AddLine(ImVec2(a.x + rr, a.y + 1), ImVec2(b.x - rr, a.y + 1), IM_COL32(255, 255, 255, c.P.dark ? 9 : 0));  // the top edge's light
  if (sel) {
    const float g = glowK(c);
    c.dl->AddRect(ImVec2(a.x - c.s(1), a.y - c.s(1)), ImVec2(b.x + c.s(1), b.y + c.s(1)), theme::withAlpha(c.P.accent, 0.3f * g + 0.1f), rr, 0, c.s(1.5f));
    c.dl->AddRect(a, b, theme::mix(c.P.accent, c.P.line, 0.3f), rr);
  } else {
    c.dl->AddRect(a, b, theme::mix(c.P.line, c.P.lineHi, 0.35f), rr);
  }
  // the family strip along the top edge (crisp, full strength)
  c.dl->AddRectFilled(ImVec2(a.x + c.s(M(10)), a.y - c.s(M(1))), ImVec2(b.x - c.s(M(10)), a.y + c.s(M(1.6f))), fam, c.s(M(1.5f)),
                      ImDrawFlags_RoundCornersBottom);
  if (dim) c.dl->AddRectFilled(a, b, theme::withAlpha(c.P.frame, 0.45f), rr);
}
namespace {
void moreDots(const Ctx& c, float x, float y, ImU32 col) {  // three dots in a 22 px box
  for (int i = 0; i < 3; ++i) c.dl->AddCircleFilled(c.p(x + M(6) + i * M(5), y + M(11)), c.s(M(1.3f)), col, 8);
}
}  // namespace
namespace {
// The block clipboard: Copy and Paste between blocks of one kind (a layer's table travels with it).
struct BlockClip {
  int kind = BkNone, n = 0;
  std::vector<double> v;
  OscData osc;
  LfoData lfo;
};
BlockClip gBlockClip;
int blockModelIndex(const BlockOpts& o) {
  if (o.pKind == BkLayer) return (o.pLo - kOscBase) / O_Count;
  if (o.pKind == BkLfo) return (o.pLo - kLfoBase) / L_Count;
  return -1;
}
void blockCopy(Ctx& c, const BlockOpts& o) {
  BlockClip& k = gBlockClip;
  k.kind = o.pKind, k.n = o.pN, k.v.assign(static_cast<size_t>(o.pN), 0.0);
  for (int i = 0; i < o.pN; ++i) k.v[static_cast<size_t>(i)] = val(c, o.pLo + i);
  const int mi = blockModelIndex(o);
  if (o.pKind == BkLayer) k.osc = c.m.osc[static_cast<size_t>(mi)];
  if (o.pKind == BkLfo) k.lfo = c.m.lfo[static_cast<size_t>(mi)];
  c.a.notify(i18n::tr("Copied the settings"), false);
}
bool blockCanPaste(const BlockOpts& o) { return o.pKind != BkNone && gBlockClip.kind == o.pKind && gBlockClip.n == o.pN; }
// Pastes the clipboard (or resets to the start) as one undo step; the block stays on or off.
void blockApply(Ctx& c, const BlockOpts& o, bool reset) {
  const BlockClip& k = gBlockClip;
  const int mi = blockModelIndex(o);
  editState(c.a, c.t, reset ? "Reset block" : "Paste settings", [&](PluginState& st, Model& m) {
    for (int i = 0; i < o.pN; ++i) {
      const int p = o.pLo + i;
      if (p == o.enable || p < 0 || p >= static_cast<int>(st.params.size())) continue;
      const ParamDesc* d = pdesc(p);
      st.params[static_cast<size_t>(p)] = reset ? (d ? d->def : st.params[static_cast<size_t>(p)]) : k.v[static_cast<size_t>(i)];
    }
    if (mi < 0) return;
    if (o.pKind == BkLayer) m.osc[static_cast<size_t>(mi)] = reset ? defaultModel().osc[static_cast<size_t>(mi)] : k.osc;
    if (o.pKind == BkLfo) m.lfo[static_cast<size_t>(mi)] = reset ? defaultModel().lfo[static_cast<size_t>(mi)] : k.lfo;
  });
}
}  // namespace

bool blockBegin(Ctx& c, Block& b, const char* id, float x, float& y, float w, const char* title, const BlockOpts& o,
                const std::function<void(float, float, float, float)>& mini) {
  ImGui::PushID(id);
  b.x = x, b.y0 = y, b.w = w, b.key = ImGui::GetID("blk"), b.frameH = o.frameH;
  auto& closed = c.u.blockClosed;
  auto& more = c.u.blockMore;
  const bool isClosed = o.forceOpen ? false : o.forceClosed ? true : std::find(closed.begin(), closed.end(), b.key) != closed.end();
  b.open = !isClosed;
  b.more = std::find(more.begin(), more.end(), b.key) != more.end();
  const auto hIt = c.u.blockH.find(b.key);
  const float headH = M(8 + 24 + 8);
  const float cachedH = std::max(o.frameH, hIt != c.u.blockH.end() ? hIt->second : headH);
  const bool on = o.enable < 0 || val(c, o.enable) >= 0.5;
  blockFrame(c, x, y, w, b.open ? cachedH : headH, o.fam, o.selected, false);
  const float hy = y + M(8), hh = M(24), cy = hy + hh * 0.5f;
  float rx = x + w - M(12);
  // right: "..." (a menu: open large, every setting here, fold, remove), then the block's own tools
  {
    rx -= M(22);
    ImGui::SetCursorScreenPos(c.p(rx, hy + M(1)));
    bool openMenu = ImGui::InvisibleButton("##more", c.F.sz(M(22), M(22)));
    if (c.u.devMenu) {  // dev (screenshots): the first block's menu, open once the editor settled
      static int first = -1;
      if (first < 0) first = ImGui::GetFrameCount();
      if (ImGui::GetFrameCount() - first > 4) openMenu = true, c.u.devMenu = false;
    }  // dev (screenshots): the first block's menu open
    if (openMenu) ImGui::OpenPopup("##blockmenu");
    const bool hv = ImGui::IsItemHovered();
    if (hv) c.a.setHint(std::string(title) + ": open it large, show every setting here, fold or remove it");
    if (hv || b.more) c.dl->AddRectFilled(c.p(rx, hy + M(1)), c.p(rx + M(22), hy + M(23)), b.more ? c.P.accentSoft : c.P.panelHi, c.s(M(3)));
    moreDots(c, rx, hy + M(1), b.more ? c.P.accentText : hv ? c.P.text : c.P.textFaint);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(c.s(M(6)), c.s(M(6))));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, c.s(M(1))));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, c.P.panel);
    ImGui::PushStyleColor(ImGuiCol_Border, c.P.lineHi);
    if (openMenu) ImGui::SetNextWindowPos(c.p(rx - M(10), hy + M(26)));  // under its dots
    if (ImGui::BeginPopup("##blockmenu")) {
      {  // it sizes itself to its widest row over its first frames: judge its layout once it has
        static int menuFrames = 0;
        menuFrames = ImGui::IsWindowAppearing() ? 0 : menuFrames + 1;
        if (menuFrames < 3) gLayoutSettling = true;
      }
      menuHead(c, title);
      if ((o.view >= 0 || o.ownOpen) && menuItemLook(c, "open", "Open advanced panel", "Double-click", 1)) {
        if (o.view == VEnv) c.u.envTab = o.viewTab;
        if (o.view == VLfo) c.u.lfoTab = o.viewTab;
        b.openClicked = true;
        ImGui::CloseCurrentPopup();
      }
      if (o.enable >= 0 && menuItemLook(c, "onoff", val(c, o.enable) >= 0.5 ? "Turn off" : "Turn on", "Shift-click", 2)) {
        setv(c, o.enable, val(c, o.enable) >= 0.5 ? 0 : 1);
        ImGui::CloseCurrentPopup();
      }
      if (o.pKind != BkNone && menuItemLook(c, "reset", "Reset to default", "", 8)) blockApply(c, o, true), ImGui::CloseCurrentPopup();
      if (ImGui::IsItemHovered() && o.pKind != BkNone) c.a.setHint("Its settings go back to the start; Undo brings them back");
      menuRule(c);
      if (o.pKind != BkNone) {
        if (menuItemLook(c, "copy", "Copy settings", "Ctrl C", 9)) blockCopy(c, o), ImGui::CloseCurrentPopup();
        if (menuItemLook(c, "paste", "Paste settings", "Ctrl V", 10, false, blockCanPaste(o))) blockApply(c, o, false), ImGui::CloseCurrentPopup();
        if (ImGui::IsItemHovered() && !blockCanPaste(o)) c.a.setHint("Copy a block of the same kind first");
      }
      if (o.extraMenu) o.extraMenu();
      menuRule(c);
      if (o.canMore && !c.u.full && menuItemLook(c, "more", b.more ? "Show the main controls only" : "Show every setting here", "", 4)) {
        if (b.more) more.erase(std::remove(more.begin(), more.end(), b.key), more.end());
        else more.push_back(b.key);
        b.more = !b.more;
        ImGui::CloseCurrentPopup();
      }
      if (menuItemLook(c, "fold", b.open ? "Fold" : "Unfold", "", 3)) {
        if (b.open) closed.push_back(b.key);
        else closed.erase(std::remove(closed.begin(), closed.end(), b.key), closed.end());
        ImGui::CloseCurrentPopup();
      }
      menuRule(c);
      if (o.patchNode >= 0 && menuItemLook(c, "patch", "Show in Patch view", "Tab", 5)) {
        c.u.patch = true, c.u.pLink = -1, c.u.pSel = o.patchNode;
        ImGui::CloseCurrentPopup();
      }
      if ((o.about || o.hint) && menuItemLook(c, "what", "What this block does", "?", 6)) c.a.notify(o.about ? o.about : o.hint, false);
      if (o.canRemove) {
        menuRule(c);
        char rl[64];
        std::snprintf(rl, sizeof rl, i18n::tr("Remove from %s"), o.section ? i18n::tr(o.section) : i18n::tr("the sound"));
        if (menuItemLook(c, "remove", o.section ? rl : "Remove", "Del", 7, true)) b.removeClicked = true, ImGui::CloseCurrentPopup();
        if (ImGui::IsItemHovered()) c.a.setHint("Its settings go back to the start; Undo brings them back");
      }
      ImGui::EndPopup();
    }
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);
  }
  {  // keys over the block: Copy, Paste, Remove, what it does
    const float bh = b.open ? cachedH : headH;
    const bool over = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::GetIO().WantTextInput &&
                      !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) && !ImGui::IsAnyItemActive() &&
                      ImGui::IsMouseHoveringRect(c.p(x, y), c.p(x + w, y + bh));
    if (over) {
      if (o.pKind != BkNone && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_C)) blockCopy(c, o);
      if (blockCanPaste(o) && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_V)) blockApply(c, o, false);
      if (o.canRemove && (ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false))) b.removeClicked = true;
      if ((o.about || o.hint) && ImGui::GetIO().KeyShift && ImGui::IsKeyPressed(ImGuiKey_Slash, false)) c.a.notify(o.about ? o.about : o.hint, false);
    }
  }
  if (o.tools) rx = o.tools(rx - M(6), hy + M(1));
  // left: the light (or the link dot), the drag grip, the name and what it is
  float tx = x + M(12);
  if (o.enable >= 0) led(c, o.enable, tx, cy - M(4), M(8)), tx += M(8 + 8);
  else if (o.grip >= 0) sourceGrip(c, o.grip, tx + M(8), cy, M(7)), tx += M(16 + 8);
  float tpx = T(13.5f);  // the name fits whole: a step smaller when the block is narrow
  while (tpx > T(11) && textW(c, title, tpx, true) > rx - M(6) - tx) tpx -= 0.25f;
  const float tw = textW(c, title, tpx, true);
  textHeavy(c, tx, cy - tpx * 0.68f, title, on ? c.P.text : c.P.textDim, tpx);
  float subEnd = tx + tw;
  if (o.sub && *o.sub) {
    const float sx = tx + tw + M(8), room = rx - M(6) - sx;
    std::string sub = o.sub;
    while (!sub.empty() && textW(c, sub.c_str(), T(11.5f), false) > room) sub.pop_back();
    if (sub.size() < std::string(o.sub).size() && sub.size() > 2) sub.resize(sub.size() - 1), sub += "...";
    if (room > M(20) && !sub.empty()) {
      text(c, sx, cy - T(11.5f) * 0.68f, sub.c_str(), c.P.textFaint, T(11.5f), false);
      subEnd = sx + textW(c, sub.c_str(), T(11.5f), false);
      if (o.subMenu) {  // what it is: a click chooses (a table, a filter type, a shape)
        ImGui::SetCursorScreenPos(c.p(sx, hy));
        if (ImGui::InvisibleButton("##sub", c.F.sz(std::max(M(10), subEnd - sx), hh))) ImGui::OpenPopup("##submenu");
        if (ImGui::IsItemHovered()) {
          hline(c, sx, subEnd, cy + M(8), c.P.textFaint);
          c.a.setHint(std::string(title) + ": click to choose");
        }
        if (ImGui::BeginPopup("##submenu")) {
          o.subMenu();
          ImGui::EndPopup();
        }
      }
    }
  }
  if (!b.open && mini) {
    const float mx = subEnd + M(10), mw = rx - M(6) - mx;
    if (mw > 30) {
      c.dl->AddRectFilled(c.p(mx, hy + 3), c.p(mx + mw, hy + hh - 3), c.P.well, c.s(3));
      c.dl->PushClipRect(c.p(mx, hy + 3), c.p(mx + mw, hy + hh - 3), true);
      mini(mx + 2, hy + 5, mw - 4, hh - 10);
      c.dl->PopClipRect();
    }
  }
  // the name: the drag handle and the double-click target; last, so a caller can make it a drag source
  ImGui::SetCursorScreenPos(c.p(tx, hy));
  b.titleClicked = ImGui::InvisibleButton("##title", c.F.sz(std::max(10.f, std::min(tw, rx - M(4) - tx)), hh));
  markPicture();
  if (ImGui::IsItemHovered()) c.a.setHint(o.hint ? std::string(o.hint) : std::string(title) + (o.view >= 0 ? ": double-click to open it large" : b.open ? "" : ": click to unfold"));
  if (o.enable >= 0 && ImGui::IsItemClicked() && ImGui::GetIO().KeyShift) setv(c, o.enable, val(c, o.enable) >= 0.5 ? 0 : 1);  // Shift-click: on / off
  if (!b.open && ImGui::IsItemClicked()) {  // a folded block unfolds on a click
    closed.erase(std::remove(closed.begin(), closed.end(), b.key), closed.end());
    b.open = true;
  }
  const bool dbl = ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0);
  if (o.ownOpen) b.titleClicked = false;  // the caller opens it on a double-click
  if (b.openClicked || (dbl && !o.ownOpen && o.view >= 0)) {
    if (o.view == VEnv) c.u.envTab = o.viewTab;
    if (o.view == VLfo) c.u.lfoTab = o.viewTab;
    if (!o.ownOpen) openView(c.u, o.view);
  }
  b.prevMore = gBlockMore;
  gBlockMore = c.u.full || b.more;
  y = y + (b.open ? M(8 + 24 + 4) : headH);
  if (!on && b.open) b.dimFrom = y;
  ImGui::PopID();
  return b.open;
}
void blockEnd(Ctx& c, Block& b, float& y) {
  if (b.open) y += M(8);
  const float h = std::max(b.open ? b.frameH : 0.f, std::max(M(40), y - b.y0));
  c.u.blockH[b.key] = h;
  if (b.dimFrom > 0 && y > b.dimFrom) c.dl->AddRectFilled(c.p(b.x + 4, b.dimFrom), c.p(b.x + b.w, b.y0 + h), theme::withAlpha(c.P.frame, 0.35f), c.s(6));
  gBlockMore = b.prevMore;
  y = b.y0 + h + 8;
}

void sectionEnd(Ctx& c, float x, float& y, float w) {
  y += 6;
  hline(c, x, x + w, y, theme::withAlpha(c.P.line, 0.7f));
  y += 10;
}

// ================================================================== modulation
namespace {
struct SrcGroup {
  const char* name;
  std::vector<int> ids;
};
const std::vector<SrcGroup>& srcGroups() {
  static const std::vector<SrcGroup> g = [] {
    std::vector<SrcGroup> v;
    v.push_back({"Envelopes", {SrcEnv1, SrcEnv2, SrcEnv3, SrcEnv4}});
    SrcGroup l{"LFOs", {}}, ly{"LFO Y outputs", {}}, mc{"Macros", {}};
    for (int i = 0; i < kLfos; ++i) l.ids.push_back(SrcLfo1 + i), ly.ids.push_back(SrcLfo1Y + i);
    for (int i = 0; i < kMacros; ++i) mc.ids.push_back(SrcMacro1 + i);
    v.push_back(l);
    v.push_back(ly);
    v.push_back(mc);
    v.push_back({"Key and touch", {SrcVelo, SrcNote, SrcReleaseVelo, SrcAftertouch, SrcPolyAT, SrcModWheel, SrcPitchBend, SrcExprX, SrcExprY, SrcExprZ}});
    v.push_back({"Chance and voice", {SrcNoise, SrcRand1, SrcRand2, SrcRandDiscrete, SrcAlt1, SrcAlt2, SrcActiveVoices, SrcVoiceIndex, SrcVoiceMod1,
                                      SrcVoiceMod2, SrcFixed}});
    v.push_back({"Layers and filters", {SrcOscA, SrcOscB, SrcOscC, SrcSub, SrcFilter1, SrcFilter2}});
    return v;
  }();
  return g;
}
}  // namespace

std::string sourceLabel(int src) {
  if (src <= SrcNone || src >= SourceCount) return "\xE2\x80\x94";
  if (src >= SrcEnv1 && src <= SrcEnv4) return "Envelope " + std::to_string(src - SrcEnv1 + 1);
  if (src >= SrcLfo1 && src <= SrcLfo10) return "LFO " + std::to_string(src - SrcLfo1 + 1);
  if (src >= SrcLfo1Y && src <= SrcLfo10Y) return "LFO " + std::to_string(src - SrcLfo1Y + 1) + " Y";
  if (src >= SrcMacro1 && src <= SrcMacro8) return "Macro " + std::to_string(src - SrcMacro1 + 1);
  switch (src) {
    case SrcVelo: return "Velocity";
    case SrcNote: return "Key";
    case SrcActiveVoices: return "Voices";
    case SrcAlt1: return "Alternate 1";
    case SrcAlt2: return "Alternate 2";
    case SrcRandDiscrete: return "Random steps";
    case SrcRand1: return "Random 1";
    case SrcRand2: return "Random 2";
    case SrcReleaseVelo: return "Release";
    case SrcVoiceIndex: return "Voice number";
    case SrcVoiceMod1: return "Voice mod 1";
    case SrcVoiceMod2: return "Voice mod 2";
    case SrcNoise: return "Noise";
    case SrcOscA: return "Layer 1";
    case SrcOscB: return "Layer 2";
    case SrcOscC: return "Layer 3";
    case SrcSub: return "Root";
    case SrcExprX: return "Slide X";
    case SrcExprY: return "Slide Y";
    case SrcExprZ: return "Press Z";
    case SrcFilter1: return "Filter 1";
    case SrcFilter2: return "Filter 2";
    case SrcModWheel: return "Mod wheel";
    case SrcAftertouch: return "Pressure";
    case SrcPolyAT: return "Key pressure";
    case SrcPitchBend: return "Bend";
    case SrcFixed: return "Constant";
    default: break;
  }
  const auto& n = sourceNames();
  return n[static_cast<size_t>(src)];
}
bool sourceMenuItems(Ctx& c, int& out) {
  (void)c;
  bool chose = false;
  if (ImGui::MenuItem(i18n::tr("None"))) out = SrcNone, chose = true;
  for (const auto& g : srcGroups()) {
    if (ImGui::BeginMenu(i18n::tr(g.name))) {
      for (int id : g.ids)
        if (ImGui::MenuItem(sourceLabel(id).c_str())) out = id, chose = true;
      ImGui::EndMenu();
    }
  }
  return chose;
}
bool destMenuItems(Ctx& c, int& out) {
  (void)c;
  bool chose = false;
  if (ImGui::MenuItem(i18n::tr("None"))) out = -1, chose = true;
  auto group = [&](const char* name, int from, int to) {
    if (ImGui::BeginMenu(i18n::tr(name))) {
      for (int k = from; k < to; ++k)
        if (modulatable(k) && ImGui::MenuItem(paramLongName(k).c_str())) out = k, chose = true;
      ImGui::EndMenu();
    }
  };
  for (int o = 0; o < kOscs; ++o) group(o == 0 ? "Layer 1" : o == 1 ? "Layer 2" : "Layer 3", oscP(o, 0), oscP(o, O_Count));
  group("Root", kSubBase, kNoiseBase);
  group("Texture", kNoiseBase, kFiltBase);
  group("Filter 1", filtP(0, 0), filtP(1, 0));
  group("Filter 2", filtP(1, 0), filtP(2, 0));
  group("Routing", kRouteBase, kEnvBase);
  if (ImGui::BeginMenu(i18n::tr("Envelopes"))) {
    for (int e = 0; e < kEnvs; ++e) {
      char b[16];
      std::snprintf(b, sizeof b, "Envelope %d", e + 1);
      if (ImGui::BeginMenu(b)) {
        for (int k = envP(e, 0); k < envP(e + 1, 0); ++k)
          if (modulatable(k) && ImGui::MenuItem(paramLongName(k).c_str())) out = k, chose = true;
        ImGui::EndMenu();
      }
    }
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu(i18n::tr("LFOs"))) {
    for (int l = 0; l < kLfos; ++l) {
      char b[16];
      std::snprintf(b, sizeof b, "LFO %d", l + 1);
      if (ImGui::BeginMenu(b)) {
        for (int k = lfoP(l, 0); k < lfoP(l + 1, 0); ++k)
          if (modulatable(k) && ImGui::MenuItem(paramLongName(k).c_str())) out = k, chose = true;
        ImGui::EndMenu();
      }
    }
    ImGui::EndMenu();
  }
  group("Macros", kMacroBase, kBusBase);
  group("Point motion", kBusBase, kRetrigBase);
  group("Restarts", kRetrigBase, kModBase);
  if (ImGui::BeginMenu(i18n::tr("Effects"))) {
    for (int s = 0; s < kFxSlots; ++s) {
      const int type = vali(c, fxP(s, X_Type));
      if (type <= FxNone || type >= FxTypeCount) continue;
      const std::string nm = "Effect " + std::to_string(s + 1) + " " + i18n::tr(fxDisplayName(type));
      if (ImGui::BeginMenu(nm.c_str())) {
        const auto& defs = fxParams(type);
        for (int q = 0; q < static_cast<int>(defs.size()); ++q)
          if (defs[static_cast<size_t>(q)].kind == 0 && ImGui::MenuItem(defs[static_cast<size_t>(q)].label)) out = fxP(s, X_P0 + q), chose = true;
        ImGui::EndMenu();
      }
    }
    ImGui::EndMenu();
  }
  group("Voice and play", 0, G_Count);
  return chose;
}

std::vector<int> modSlotsFor(const Ctx& c, int k) {
  std::vector<int> out;
  for (int s = 0; s < kModSlots; ++s)
    if (vali(c, modP(s, M_Dest)) == k + 1 && vali(c, modP(s, M_Source)) > SrcNone) out.push_back(s);
  return out;
}
int addModulation(Ctx& c, int src, int k, float amount) {
  if (!modulatable(k)) return -1;
  for (int s = 0; s < kModSlots; ++s) {
    if (vali(c, modP(s, M_Source)) > SrcNone || vali(c, modP(s, M_Dest)) > 0) continue;
    commitParams(c, "Add link", {{modP(s, M_Source), src}, {modP(s, M_Dest), k + 1}, {modP(s, M_Amount), amount}});  // one undo step
    return s;
  }
  return -1;
}
void sourceHandle(Ctx& c, int src, float cx, float cy, float r) { sourceGrip(c, src, cx, cy, r); }
void dropTarget(Ctx& c, int k) {
  if (!modulatable(k)) return;
  if (ImGui::BeginDragDropTarget()) {
    const ImGuiPayload* pl = ImGui::GetDragDropPayload();
    if (pl && pl->IsDataType("PLASMA_SRC")) {
      const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
      int ps = 0;
      std::memcpy(&ps, pl->Data, sizeof(int));
      ImGui::GetForegroundDrawList()->AddRect(a, b, c.P.fam[familyOf(ps)], c.s(3), 0, std::max(1.5f, c.s(1.5f)));
    }
    if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("PLASMA_SRC", ImGuiDragDropFlags_AcceptNoDrawDefaultRect)) {
      int src = 0;
      std::memcpy(&src, p->Data, sizeof(int));
      addModulation(c, src, k, 0.25f);
    }
    ImGui::EndDragDropTarget();
  }
}

void warpMenuItems(Ctx& c, int o, int slot) { morphPicker(c, o, slot); }
void filterMenuItems(Ctx& c, int k) { filterPicker(c, k); }

// ================================================================== graphs
void gridLines(const Ctx& c, float x, float y, float w, float h, int cols, int rows, bool centre) {
  for (int i = 1; i < cols; ++i) vline(c, x + w * i / cols, y, y + h, c.P.grid);
  for (int i = 1; i < rows; ++i) hline(c, x, x + w, y + h * i / rows, centre && i * 2 == rows ? c.P.gridStrong : c.P.grid);
}
void drawCurve(const Ctx& c, const Curve& cv, float x, float y, float w, float h, ImU32 col, bool fill, bool bipolar) {
  if (cv.size() < 2) return;
  const int n = std::max(32, static_cast<int>(c.s(w)));
  std::vector<ImVec2> pts(static_cast<size_t>(n + 1));
  for (int i = 0; i <= n; ++i) {
    const double xv = static_cast<double>(i) / n;
    const double yv = std::clamp(curveAt(cv, xv), 0.0, 1.0);
    pts[static_cast<size_t>(i)] = c.p(x + static_cast<float>(xv) * w, y + h - static_cast<float>(yv) * h);
  }
  const float base = bipolar ? c.p(0, y + h * 0.5f).y : c.p(0, y + h).y;
  if (fill) skin::curveArea(c.dl, pts.data(), static_cast<int>(pts.size()), base, col, std::max(1.5f, c.s(1.6f)), 0.25f);
  else c.dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), col, 0, std::max(1.5f, c.s(1.6f)));
}
bool curveEditor(Ctx& c, const char* id, Curve& cv, float x, float y, float w, float h, int gridX, int gridY, int& sel, bool lockEnds, ImU32 col,
                 int* loopPoint) {
  ImGui::PushID(id);
  const ImVec2 a = c.p(x, y), b = c.p(x + w, y + h);
  {  // the grab area reaches a handle's radius past the plot, so points on its edges are whole to grab
    const float pad = c.s(6);
    ImGui::SetCursorScreenPos(ImVec2(a.x - pad, a.y - pad));
    ImGui::InvisibleButton("##ce", ImVec2(b.x - a.x + 2 * pad, b.y - a.y + 2 * pad), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
  }
  const bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
  ImGuiIO& io = ImGui::GetIO();
  const ImVec2 m = io.MousePos;
  auto toX = [&](float xv) { return a.x + xv * (b.x - a.x); };
  auto toY = [&](float yv) { return b.y - yv * (b.y - a.y); };
  auto fromX = [&](float px) { return std::clamp((px - a.x) / (b.x - a.x), 0.f, 1.f); };
  auto fromY = [&](float py) { return std::clamp((b.y - py) / (b.y - a.y), 0.f, 1.f); };
  auto snapX = [&](float v) { return gridX > 0 && io.KeyShift == false && io.KeyCtrl ? std::round(v * gridX) / gridX : v; };
  auto snapY = [&](float v) { return gridY > 0 && io.KeyShift == false && io.KeyCtrl ? std::round(v * gridY) / gridY : v; };
  static int dragHandle = -1;  // curve handle (segment ending at this point) being bent
  static float dragStartY = 0, dragStartC = 0;
  int hot = -1, hotHandle = -1;
  const float hr = c.s(6);
  for (size_t i = 0; i < cv.size(); ++i)
    if (std::fabs(m.x - toX(cv[i].x)) < hr && std::fabs(m.y - toY(cv[i].y)) < hr) hot = static_cast<int>(i);
  // segment bend handles at the curve midpoint of each segment
  std::vector<ImVec2> mids(cv.size());
  for (size_t i = 1; i < cv.size(); ++i) {
    const float mx = (cv[i - 1].x + cv[i].x) * 0.5f;
    mids[i] = ImVec2(toX(mx), toY(static_cast<float>(curveAt(cv, mx))));
    if (hot < 0 && std::fabs(m.x - mids[i].x) < hr && std::fabs(m.y - mids[i].y) < hr) hotHandle = static_cast<int>(i);
  }
  bool changed = false;
  if (hov && ImGui::IsMouseClicked(0)) {
    sel = hot;
    dragHandle = hot < 0 ? hotHandle : -1;
    if (dragHandle > 0) dragStartY = m.y, dragStartC = cv[static_cast<size_t>(dragHandle)].c;
  }
  if (hov && ImGui::IsMouseDoubleClicked(0)) {
    if (hot >= 0 && !(lockEnds && (hot == 0 || hot + 1 == static_cast<int>(cv.size()))) && cv.size() > 2) {
      cv.erase(cv.begin() + hot);
      sel = -1;
      changed = true;
    } else if (hot < 0 && hotHandle > 0) {
      cv[static_cast<size_t>(hotHandle)].c = 0;
      changed = true;
    } else if (hot < 0 && cv.size() < 120) {
      const float nx = snapX(fromX(m.x)), ny = snapY(fromY(m.y));
      size_t i = 0;
      while (i < cv.size() && cv[i].x <= nx) ++i;
      cv.insert(cv.begin() + static_cast<long>(i), CurvePt{nx, ny, 0});
      sel = static_cast<int>(i);
      changed = true;
    }
  }
  if (hov && ImGui::IsMouseClicked(1) && hot >= 0 && !(lockEnds && (hot == 0 || hot + 1 == static_cast<int>(cv.size()))) && cv.size() > 2) {
    cv.erase(cv.begin() + hot);
    sel = -1;
    changed = true;
  } else if (hov && ImGui::IsMouseClicked(1) && hot >= 0 && loopPoint) {
    *loopPoint = *loopPoint == hot ? -1 : hot;
    changed = true;
  }
  if (act && ImGui::IsMouseDragging(0, 0)) {
    if (dragHandle > 0 && dragHandle < static_cast<int>(cv.size())) {
      CurvePt& p = cv[static_cast<size_t>(dragHandle)];
      const float dir = cv[static_cast<size_t>(dragHandle)].y >= cv[static_cast<size_t>(dragHandle) - 1].y ? 1.f : -1.f;
      p.c = std::clamp(dragStartC + (m.y - dragStartY) / c.s(80) * dir, -1.f, 1.f);
      changed = true;
    } else if (sel >= 0 && sel < static_cast<int>(cv.size())) {
      CurvePt& p = cv[static_cast<size_t>(sel)];
      const bool first = sel == 0, last = sel + 1 == static_cast<int>(cv.size());
      float nx = snapX(fromX(m.x));
      const float lo = first ? 0.f : cv[static_cast<size_t>(sel) - 1].x, hi = last ? 1.f : cv[static_cast<size_t>(sel) + 1].x;
      if (lockEnds && (first || last)) nx = first ? 0.f : 1.f;
      p.x = std::clamp(nx, lo, hi);
      p.y = snapY(fromY(m.y));
      changed = true;
    }
  }
  if (ImGui::IsItemDeactivated()) dragHandle = -1;
  // draw
  std::vector<ImVec2> pts;
  const int n = std::max(48, static_cast<int>(b.x - a.x));
  pts.reserve(static_cast<size_t>(n) + 1);
  for (int i = 0; i <= n; ++i) {
    const float xv = static_cast<float>(i) / n;
    pts.push_back(ImVec2(toX(xv), toY(static_cast<float>(std::clamp(curveAt(cv, xv), 0.0, 1.0)))));
  }
  skin::curveArea(c.dl, pts.data(), static_cast<int>(pts.size()), b.y, col, std::max(1.5f, c.s(1.7f)), 0.22f);
  for (size_t i = 1; i < cv.size(); ++i) {
    const bool hh = static_cast<int>(i) == hotHandle || static_cast<int>(i) == dragHandle;
    c.dl->AddCircle(mids[i], c.s(hh ? 3.5f : 2.6f), hh ? c.P.text : theme::withAlpha(col, 0.75f), 12, std::max(1.0f, c.s(1.1f)));
  }
  for (size_t i = 0; i < cv.size(); ++i) {
    const ImVec2 q(toX(cv[i].x), toY(cv[i].y));
    const bool s = static_cast<int>(i) == sel, hh = static_cast<int>(i) == hot;
    const bool lp = loopPoint && *loopPoint == static_cast<int>(i);
    c.dl->AddCircleFilled(q, c.s(s || hh ? 4.2f : 3.2f), lp ? c.P.warn : (s ? c.P.text : col));
  }
  if (hov)
    c.a.setHint("Drag points \xC2\xB7 drag the small rings to bend \xC2\xB7 double-click: add / remove \xC2\xB7 right-click: remove"
                " \xC2\xB7 Ctrl: snap to the grid");
  ImGui::PopID();
  return changed;
}

// ================================================================== presets / open
void previewKey(Ctx& c, int key, bool on) {
  if (!c.t.isChannel()) return;
  if (on) c.a.engine.noteOn(c.t.owner, key, 0.8f);
  else c.a.engine.noteOff(c.t.owner, key);
}

// "Lock effects": a loaded preset keeps the current effects.
void keepLockedFx(const Ctx& c, PluginState& ns, Model& m) {
  if (!c.m.lockFx) return;
  for (int k = kFxBase; k < kTailBase && k < static_cast<int>(ns.params.size()) && k < static_cast<int>(c.st.params.size()); ++k)
    ns.params[static_cast<size_t>(k)] = c.st.params[static_cast<size_t>(k)];
  m.fx = c.m.fx;
  m.fxExtra = c.m.fxExtra;
  m.fxProxy = c.m.fxProxy;
  m.lockFx = true;
}

void applyPreset(Ctx& c, const std::vector<std::pair<int, float>>& values, const std::string& data, const std::string& name) {
  PluginState ns = defaultPluginState(kType);
  for (const auto& [k, v] : values)
    if (k >= 0 && k < static_cast<int>(ns.params.size())) ns.params[static_cast<size_t>(k)] = v;
  Model m = defaultModel();
  if (!data.empty()) modelFromData(data, m);
  if (m.presetName.empty()) m.presetName = name;
  keepLockedFx(c, ns, m);
  ns.data = modelToData(m);
  setPluginState(c.a, c.t, ns, "Load preset");
}

namespace {
// Report text of a sound opened from another format, in neutral words (older
// saves can hold the source format's own wording).
std::string neutralWords(std::string t) {
  static const std::pair<const char*, const char*> kSwap[] = {
      {"Serum 1 preset (.fxp)", "Imported preset (.fxp)"}, {"Serum 2 preset", "Imported preset (newer format)"},
      {"Serum preset", "Imported preset"}, {"Serum 2 (", "Imported preset, newer format ("}, {"Serum (", "Imported preset, older format ("},
      {"Serum 1 shape", "Imported shape"}, {"Serum 1 ", "older-format "}, {"Serum 2 ", ""}, {"Serum", "the imported preset"},
      {"Xfer Records", ""}, {"Xfer", ""}};
  for (const auto& [from, to] : kSwap)
    for (size_t at = t.find(from); at != std::string::npos; at = t.find(from, at + std::strlen(to)))
      t.replace(at, std::strlen(from), to);
  return t;
}
}  // namespace

std::string openReportText(const Model& m) {
  if (m.openFormat.empty()) return "";
  std::string t = std::string(i18n::tr("Opened from")) + ": " + neutralWords(m.openFormat);
  if (!m.openNotes.empty()) {
    t += std::string("\n\n") + i18n::tr("Approximated") + ":";
    for (const auto& n : m.openNotes) t += "\n  - " + neutralWords(n);
  }
  if (!m.openMissing.empty()) {
    t += std::string("\n\n") + i18n::tr("Files not found (install the content or add its folder in the Library)") + ":";
    for (const auto& n : m.openMissing) t += "\n  - " + neutralWords(fs_stem(n));
  }
  return t;
}

bool openPreset(Ctx& c, const std::string& path) {
  PluginState ns;
  ImportReport rep;
  if (!openPresetFile(path, ns, rep)) {
    c.a.notify(std::string(i18n::tr("Could not open the preset")) + ": " + rep.error, true);
    return false;
  }
  Model m = defaultModel();
  modelFromData(ns.data, m);
  if (m.presetName.empty()) m.presetName = rep.presetName.empty() ? fs::u8path(path).stem().string() : rep.presetName;
  keepLockedFx(c, ns, m);
  ns.data = modelToData(m);
  setPluginState(c.a, c.t, ns, "Open preset");
  if (!m.openNotes.empty() || !m.openMissing.empty())
    c.a.notify(std::string(i18n::tr("Opened")) + " " + m.presetName + ": " + i18n::tr("some settings are approximated (see the info mark)"), false,
               openReportText(m));
  return true;
}

void editState(App& a, const PluginTarget& t, const char* label, const std::function<void(PluginState&, Model&)>& fn) {
  const PluginState* cur = t.state(a.doc.song());
  if (!cur) return;
  PluginState ns = *cur;
  Model m = defaultModel();
  modelFromData(ns.data, m);
  fn(ns, m);
  ns.data = modelToData(m);
  setPluginState(a, t, ns, label);
}

void openFile(Ctx& c, const char* title, const std::vector<std::string>& exts,
              const std::function<void(App&, const PluginTarget&, const std::string&)>& ok) {
  App& a = c.a;
  const PluginTarget t = c.t;
  a.fileDialog = {};
  a.fileDialog.open = true;
  a.fileDialog.title = title;
  static std::string lastDir;
  std::error_code ec;
  if (lastDir.empty() || !fs::is_directory(lastDir, ec)) {
    const char* h = std::getenv("HOME");
    lastDir = h ? h : ".";
  }
  a.fileDialog.dir = lastDir;
  a.fileDialog.exts = exts;
  a.fileDialog.onOk = [&a, t, ok](const std::string& p) {
    lastDir = fs::path(p).parent_path().string();
    ok(a, t, p);
  };
}

std::shared_ptr<const Aux> auxOf(const Ctx& c) { return buildAux(c.st); }

void openPresetDialog(Ctx& c) {
  App& a = c.a;
  const PluginTarget t = c.t;
  a.fileDialog = {};
  a.fileDialog.open = true;
  a.fileDialog.title = i18n::tr("Open preset");
  static std::string lastDir;
  std::error_code ec;
  if (lastDir.empty() || !fs::is_directory(lastDir, ec)) {
    // the home folder first (the library folders are reached through the Library)
    const char* h = std::getenv("HOME");
    lastDir = h ? h : ".";
  }
  a.fileDialog.dir = lastDir;
  a.fileDialog.exts = openableExtensions();  // every type Plasma opens, in one filter
  Ui* u = &c.u;
  a.fileDialog.onOk = [&a, t, u](const std::string& p) {
    lastDir = fs::u8path(p).parent_path().string();
    const PluginState* st = t.state(a.doc.song());
    if (!st) return;
    Model m;
    modelFromData(st->data, m);
    PluginState copy = *st;
    const skin::Form F;
    Ctx c2{a, t, F, ImGui::GetWindowDrawList(), copy, m, *u, pal(), {}, false};
    openPreset(c2, p);
  };
}

bool inputField(Ctx& c, const char* id, float x, float y, float w, float h, char* buf, size_t size, const char* hint, int flags, float px) {
  ImGui::SetCursorScreenPos(c.p(x, y));
  ImGui::PushID(id);
  ImGui::PushStyleColor(ImGuiCol_FrameBg, theme::vec(c.P.field));
  ImGui::PushStyleColor(ImGuiCol_Text, theme::vec(c.P.text));
  ImGui::PushStyleColor(ImGuiCol_TextDisabled, theme::vec(c.P.textDim));
  const float fs = font(c, px);
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(c.s(6), std::max(0.f, (c.s(h) - fs) * 0.5f)));
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, c.s(3));
  ImGui::PushFont(c.a.fontRegular, fs);
  ImGui::SetNextItemWidth(c.s(w));
  const bool r = hint ? ImGui::InputTextWithHint("##in", hint, buf, size, flags) : ImGui::InputText("##in", buf, size, flags);
  ImGui::PopFont();
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor(3);
  ImGui::PopID();
  return r;
}

// ================================================================== header
// J-202: one stereo meter (two bars in one well): the header and the MIX page.
void drawMeter(Ctx& c, float x, float y, float w, float h) {
  wellRect(c, x, y, w, h, 2);
  const float bw = (w - 5) * 0.5f;
  for (int ch = 0; ch < 2; ++ch) {
    const float bx = x + 2 + ch * (bw + 1);
    c.dl->AddRectFilled(c.p(bx, y + 2), c.p(bx + bw, y + h - 2), c.P.grid, c.s(1));
    const float pk = c.hasLive ? (ch == 0 ? c.live.outL : c.live.outR) : 0.f;
    const float lv = std::clamp(pk, 0.f, 1.5f);
    const float db = lv > 1e-5f ? 20.f * std::log10(lv) : -90.f;
    const float n = std::clamp((db + 48.f) / 51.f, 0.f, 1.f);
    if (n > 0) {
      const ImVec2 a = c.p(bx, y + h - 2 - n * (h - 4)), b = c.p(bx + bw, y + h - 2);
      skin::gradientRect(c.dl, a, b, db > -1.f ? c.P.danger : c.P.good, theme::withAlpha(c.P.good, 0.6f), c.s(1));
    }
  }
}

namespace {
// Per-instance editor state.
Ui& uiFor(const PluginTarget& t) {
  static std::map<uint64_t, Ui> m;
  auto it = m.find(t.key());
  if (it != m.end()) return it->second;
  Ui u;
  // dev (screenshots, tests): GL_PLASMA_VIEW names a view (the old page / view names map to the
  // new views); GL_PLASMA_PAGE takes the old page numbers
  if (const char* e = std::getenv("GL_PLASMA_PAGE")) {
    static const int kPageView[5] = {VLayer, VRouting, VEffects, VLinks, VSetup};
    u.view = kPageView[std::clamp(std::atoi(e), 0, 4)];
  }
  if (const char* e = std::getenv("GL_PLASMA_PRESET")) u.devPreset = e;
  if (const char* e = std::getenv("GL_PLASMA_VIEW")) devView(u, e);
  if (const char* e = std::getenv("GL_PLASMA_ENGINE")) u.layerCard = std::atoi(e);  // dev: Grains / Spectral card
  return m[t.key()] = u;
}
}  // namespace
// dev (screenshots, tests): puts the editor on a named view (the old page / view names map to the new views)
void devView(Ui& u, const std::string& name) {
  // suffixes: "+full" (Full), "+simple", "+drawer" (the keyboard drawer out)
  std::string v = name;
  if (const size_t plus = v.find('+'); plus != std::string::npos) {
    const std::string sfx = v.substr(plus);
    u.full = sfx.find("+full") != std::string::npos, u.fullSet = true;
    if (sfx.find("+drawer") != std::string::npos) u.drawer = true;
    if (sfx.find("+play") != std::string::npos) u.devPlay = true;
    if (sfx.find("+3d") != std::string::npos) u.wt3d[0] = u.wt3d[1] = u.wt3d[2] = true;
    v.resize(plus);
  }
  {
    static const std::map<std::string, int> names = {
        {"synth", VLayer}, {"voices", VVoices}, {"play", VVoices}, {"layer", VLayer}, {"layer1", VLayer}, {"layer2", VLayer}, {"layer3", VLayer}, {"roottex", VRootTex}, {"filters", VFilters},
        {"amp", VAmp},     {"routing", VRouting}, {"mix", VRouting}, {"effects", VEffects}, {"fx", VEffects}, {"links", VLinks},
        {"matrix", VLinks}, {"keys", VKeys},  {"setup", VSetup}, {"global", VSetup}, {"arp", VArp}, {"phrases", VPhrases},
        {"clip", VPhrases}, {"wt", VTable},   {"table", VTable}, {"browser", VLibrary}, {"library", VLibrary}, {"map", VZones},
        {"zones", VZones}};
    if (auto f = names.find(v); f != names.end()) u.view = f->second;
    if (v.rfind("layer", 0) == 0 && v.size() == 6) u.layer = std::clamp(v[5] - '1', 0, kOscs - 1);
    if (v.rfind("lfo", 0) == 0) u.view = VLfo, u.lfoTab = std::clamp(std::atoi(v.c_str() + 3), 0, kLfos - 1);
    if (v.rfind("env", 0) == 0) u.view = VEnv, u.envTab = std::clamp(std::atoi(v.c_str() + 3), 0, kEnvs - 1);
    if (v == "linkmode") u.linkSrc = SrcLfo1, u.devLinkArm = SrcLfo1;
    if (v == "drawer") u.drawer = true;
    if (v == "menu") u.view = VLayer, u.devMenu = true;
    if (v == "library" || v == "libshapes" || v == "libsounds" || v == "browser") u.view = VLibrary, u.sbLib = v == "libshapes" ? 1 : v == "libsounds" || v == "browser" ? 2 : 0;
    // the Synth tab with n layers, a part dragged from the sidebar, the Modulation section
    if (v.rfind("synth", 0) == 0 && v.size() == 6) u.view = VLayer, u.devLayers = std::clamp(v[5] - '0', 1, 3);
    if (v.rfind("drag", 0) == 0) u.view = v == "dragfx" ? VEffects : VLayer, u.devDrag = v == "dragfx" ? 100 + FxReverb : v == "dragwt" ? 200 + 1 : 0;
    if (v == "modulation") u.view = VLayer, u.devScrollMod = true;
    // a block's advanced panel opened by a double-click: "open:layer1", "open:filter1", "open:lfo1", "open:root"
    if (v == "open:layer1") u.view = VTable, u.wtOsc = 0, u.back = VLayer;
    if (v == "open:filter1") u.view = VFilters, u.large = true;
    if (v == "open:root") u.view = VRootTex, u.large = true, u.src = 3;
    if (v == "open:lfo1") u.view = VLfo, u.lfoTab = 0;
    if (v == "open:fx") u.view = VEffects, u.large = true;
    // Panels / Patch, Simple / Full ("patch", "patchlink", "patchadd", "layer1+full", "src4", ...)
    if (v.rfind("patch", 0) == 0) u.patch = true, u.devPatch = v == "patchlink" ? 1 : v == "patchadd" ? 2 : v == "patchhover" ? 3 : v == "patchsearch" ? 4 : v == "patch" ? 5 : v == "patchsends" ? 6 : 0;
    if (v.rfind("src", 0) == 0 && v.size() == 4) u.src = std::clamp(v[3] - '1', 0, 4), u.view = u.src < 3 ? VLayer : VRootTex, u.layer = std::min(u.src, 2);
  }
}
namespace {

// Data model cache (parsed once per data change).
Model& modelFor(const PluginTarget& t, const std::string& data) {
  struct Entry {
    size_t hash = 0, size = 0;
    Model m;
    bool init = false;
  };
  static std::map<uint64_t, Entry> cache;
  Entry& e = cache[t.key()];
  const size_t h = std::hash<std::string>{}(data);
  if (!e.init || h != e.hash || data.size() != e.size) {
    e.m = defaultModel();
    modelFromData(data, e.m);
    e.hash = h, e.size = data.size(), e.init = true;
  }
  return e.m;
}

}  // namespace

}  // namespace ui

namespace ui {
// ================================================================== reach driver (GL_PLASMA_REACH=1)
// Steps through every view and its sub-states (layers x engines x cards, envelope and LFO
// tabs, each effect slot with the type that has the most controls, each link) two frames a
// step, then prints the parameters no control was drawn for ("PLASMA_REACH_MISSING k name")
// and "PLASMA_REACH reached of total".
namespace {
bool reachOn() {
  static const bool on = std::getenv("GL_PLASMA_REACH") != nullptr;
  return on;
}
int bestFxType() {
  int best = 1;
  for (int t = 1; t < FxTypeCount; ++t)
    if (fxParams(t).size() > fxParams(best).size()) best = t;
  return best;
}
void reachDrive(Ctx& c) {
  using Step = std::function<void(Ctx&)>;
  static std::vector<Step> steps;
  if (steps.empty()) {
    // Full: every control of the columns in place
    steps.push_back([](Ctx& x) { x.u.full = true, x.u.fullSet = true, x.u.reachAll = true; });
    for (int v : {VRouting, VKeys, VSetup, VArp, VZones, VPhrases, VLinks, VVoices})
      steps.push_back([v](Ctx& x) { x.u.view = v; });
    for (int s = 0; s < 5; ++s) steps.push_back([s](Ctx& x) { x.u.view = s < 3 ? VLayer : VRootTex, x.u.src = s, x.u.layer = std::min(s, 2); });
    steps.push_back([](Ctx& x) { x.u.view = VLayer, x.u.drawer = true; });
    steps.push_back([](Ctx& x) { x.u.drawer = false; });
    for (int o = 0; o < kOscs; ++o)
      for (int e = 0; e < OscTypeCount; ++e)
        for (int card = 0; card < 4; ++card)
          steps.push_back([o, e, card](Ctx& x) {
            x.u.view = VLayer, x.u.layer = o, x.u.layerCard = card;
            if (vali(x, oscP(o, O_Type)) != e) setv(x, oscP(o, O_Type), e);
          });
    for (int e = 0; e < kEnvs; ++e) steps.push_back([e](Ctx& x) { x.u.view = VEnv, x.u.envTab = e; });
    for (int l = 0; l < kLfos; ++l) steps.push_back([l](Ctx& x) { x.u.view = VLfo, x.u.lfoTab = l, x.u.lfoAdv = true; });
    for (int s = 0; s < kFxSlots; ++s)
      steps.push_back([s](Ctx& x) {
        x.u.view = VEffects, x.u.fxSel = s;
        if (vali(x, fxP(s, X_Type)) != bestFxType()) setv(x, fxP(s, X_Type), bestFxType());
      });
    for (int s = 0; s < kModSlots; ++s)
      steps.push_back([s](Ctx& x) {  // a "scaled by" source so its own settings show
        x.u.view = VLinks, x.u.linkSel = s;
        if (vali(x, modP(s, M_Aux)) <= SrcNone) setv(x, modP(s, M_Aux), SrcMacro1);
      });
    // the Patch view's own boxes: NFilt, NFilt + 1, NAmp, NOut, NSendA, NSendB, the macros (NMove + SrcMacro1; plasma_patch_ui.cpp)
    for (int id : {10, 11, 20, 21, 22, 23, 200 + SrcMacro1}) steps.push_back([id](Ctx& x) { x.u.view = VLayer, x.u.devPSel = id; });
    steps.push_back([](Ctx& x) { x.u.devPSel = -1; });
  }
  const int frame = ++c.u.reachStep;
  const int i = frame / 2;
  if (i < static_cast<int>(steps.size())) {
    steps[static_cast<size_t>(i)](c);
    // GL_PLASMA_REACH=patch: the same walk in the Patch view, each step's block selected there
    static const bool patch = std::string(std::getenv("GL_PLASMA_REACH")) == "patch";
    if (patch) c.u.full = true, c.u.fullSet = true, c.u.pAll = true, patchSelectFor(c.u);  // every setting of the box unfolded
    return;
  }
  if (i != static_cast<int>(steps.size()) || frame % 2) return;
  // the report (once): effect parameters past the largest effect's controls have no meaning
  const int fxUsed = static_cast<int>(fxParams(bestFxType()).size());
  const auto& r = reachRecord();
  int total = 0, got = 0;
  for (int k = 0; k < kParamCount; ++k) {
    if (k >= kFxBase && k < kFxBase + kFxSlots * X_Count && (k - kFxBase) % X_Count >= X_P0 + fxUsed) continue;
    ++total;
    if (static_cast<size_t>(k) < r.size() && r[static_cast<size_t>(k)]) ++got;
    else std::fprintf(stderr, "PLASMA_REACH_MISSING %d %s\n", k, paramLongName(k).c_str());
  }
  std::fprintf(stderr, "PLASMA_REACH %d of %d\n", got, total);
  // the pickers' display order: every stored index exactly once (morph mode 0, Off, has its own entry)
  const auto perm = [](const std::vector<MenuGroup>& g, int first, int count, const char* what) {
    std::vector<int> seen(static_cast<size_t>(count), 0);
    bool ok = true;
    for (const auto& grp : g)
      for (int i : grp.items) {
        if (i < first || i >= count) ok = false, std::fprintf(stderr, "PLASMA_MENU_BAD %s index %d out of range\n", what, i);
        else if (seen[static_cast<size_t>(i)]++) ok = false, std::fprintf(stderr, "PLASMA_MENU_BAD %s index %d listed twice\n", what, i);
      }
    for (int i = first; i < count; ++i)
      if (!seen[static_cast<size_t>(i)]) ok = false, std::fprintf(stderr, "PLASMA_MENU_BAD %s index %d missing\n", what, i);
    return ok;
  };
  const bool fOk = perm(filterMenuGroups(), 0, static_cast<int>(filterNames().size()), "filter");
  const bool mOk = perm(morphMenuGroups(), 1, warpCount(), "morph");
  std::fprintf(stderr, "PLASMA_MENUS %s\n", fOk && mOk ? "ok" : "bad");
}
}  // namespace

}  // namespace ui

// ================================================================== entry point
namespace ui {
void drawFrame(Ctx& c);  // plasma_frame_ui.cpp

namespace {
int gThemeSwitch = -1;  // -1 none, 0 to dark, 1 to light
int gEditorFrames = 0;  // frames Plasma's editor has drawn (dev hooks)
int gLastFrame = -1;
}  // namespace
void requestThemeSwitch(bool toLight) { gThemeSwitch = toLight ? 1 : 0; }
void applyThemeSwitch(App& a) {
  if (ImGui::GetFrameCount() != gLastFrame) gLastFrame = ImGui::GetFrameCount(), ++gEditorFrames;
  if (gThemeSwitch < 0) return;
  const bool toLight = gThemeSwitch == 1;
  gThemeSwitch = -1;
  const dispconf::Snapshot before = dispconf::take(a.settings, dispconf::themeKeys());
  theme::modeToSettings(toLight ? theme::Mode::Light : theme::Mode::Dark, a.settings);
  theme::applyFromSettings(a.settings);
  ::gl::ui::confirmTheme(a, before, "Theme");
}
bool devThemePress() {
  static const char* at = std::getenv("GL_PLASMA_THEME_AT");  // "60,140": editor frames
  static int last = -1;
  if (!at || last == gEditorFrames) return false;
  for (const char* p = at; *p;) {
    if (std::atoi(p) == gEditorFrames) {
      last = gEditorFrames;
      std::fprintf(stderr, "PLASMA_THEME press %d\n", gEditorFrames);
      return true;
    }
    while (*p && *p != ',') ++p;
    if (*p == ',') ++p;
  }
  return false;
}
// dev (GL_PLASMA_THEME_DUMP=<editor frame>): the app palette, Plasma's palette and ImGui's colours
void devThemeDump(App&) {
  static const char* at = std::getenv("GL_PLASMA_THEME_DUMP");
  static bool done = false;
  if (!at || done || gEditorFrames != std::atoi(at)) return;
  done = true;
  const theme::Palette& T = theme::pal();
  const Pal& P = pal();
  std::fprintf(stderr, "PLASMA_THEME_DUMP frame %d dark %d app %08x %08x %08x %08x plasma %08x %08x %08x %08x %08x %08x %08x %08x imgui",
               ImGui::GetFrameCount(), T.dark ? 1 : 0, T.surface900, T.surface800, T.text, T.field, P.frame, P.panel, P.head, P.text, P.label, P.line, P.field, P.well);
  const ImGuiStyle& st = ImGui::GetStyle();
  for (int i = 0; i < ImGuiCol_COUNT; ++i) std::fprintf(stderr, " %08x", ImGui::ColorConvertFloat4ToU32(st.Colors[i]));
  std::fprintf(stderr, "\n");
}
}
bool drawEditor(App& a, const PluginTarget& t, float width) {
  if (!t.state(a.doc.song())) return false;
  ui::applyThemeSwitch(a);  // before the palette is read and the skin scope opens
  ui::devThemeDump(a);
  ui::runLater(a, t);  // menu edits that replace the sound, made last frame
  const PluginState* sp = t.state(a.doc.song());
  if (!sp || sp->type != kType) return false;
  const PluginState st = *sp;
  using namespace ui;
  const skin::Skin& K = ui::skin();
  skin::Form F = skin::Form::fit(width, kW);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::Scope scope(K);
  Ui& u = uiFor(t);
  Model& m = modelFor(t, st.data);
  Ctx c{a, t, F, dl, st, m, u, ui::pal(), {}, false};
  if (t.isChannel())
    if (auto rt = a.builder.channelRuntime(t.owner); rt && rt->gen) c.hasLive = u.prefLive && liveView(rt->gen.get(), c.live);
  ImGui::PushID("gl.plasma");
  layoutScroll(c);  // the plugin window itself (smaller than the editor: it scrolls)
  if (!u.devPreset.empty()) {  // dev (screenshots): load a factory preset by name once
    const std::string want = u.devPreset;
    u.devPreset.clear();
    if (const PluginInfo* in = info())
      for (const auto& p : in->presets)
        if (p.name == want) applyPreset(c, p.values, "", p.name);
    for (const auto& p : factoryDataPresets())
      if (p.name == want) applyPreset(c, p.values, p.data, p.name);
  }
  if (reachOn()) reachDrive(c);
  // dev (tests): GL_PLASMA_VIEWS="a,b,c" shows each named view for eight frames in turn, the
  // layout check tagged with the view's name
  static std::vector<std::string> cycle = [] {
    std::vector<std::string> v;
    if (const char* e = std::getenv("GL_PLASMA_VIEWS")) {
      std::string cur;
      for (const char* q = e;; ++q) {
        if (*q == ',' || !*q) {
          if (!cur.empty()) v.push_back(cur);
          cur.clear();
          if (!*q) break;
        } else {
          cur += *q;
        }
      }
    }
    return v;
  }();
  static int cycleFrame = 0;
  if (!cycle.empty()) {
    const int i = cycleFrame / 8, f = cycleFrame % 8;
    ++cycleFrame;
    if (i < static_cast<int>(cycle.size())) {
      if (f == 0) {
        const Ui keep = u;
        u = Ui{};
        u.prefBubbles = keep.prefBubbles, u.prefLive = keep.prefLive;
        devView(u, cycle[static_cast<size_t>(i)]);
        u.devFrames = 0;
      }
      layoutViewTag() = cycle[static_cast<size_t>(i)];
    } else {
      layoutViewTag() = "done";
    }
  }
  // dev (tests): Link mode armed after six frames, so the controls first draw (and register) as usual
  if (u.devLinkArm >= 0 && ++u.devFrames <= 6) u.linkSrc = u.devFrames == 6 ? u.devLinkArm : -1;
  drawFrame(c);
  layoutReport(c);
  c.dl->AddRect(F.o, F.p(kW, kH), c.P.line, 0);
  F.end(kW, kH);
  ImGui::PopID();
  return true;
}

}  // namespace gl::packs::plasma
