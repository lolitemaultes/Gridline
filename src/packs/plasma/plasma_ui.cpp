// Gridline — "plasma" pack: editor frame (see plasma_ui.h): palette, shared
// controls (knobs with modulation rings, value boxes, drop-downs, steppers,
// tabs, curve editor), modulation drag and drop, menus, the header with the
// page tabs and preset row, the bottom strip with wheels, CLIP / ARP, scale and
// keyboard, and the entry point.
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
#include <map>
#include <memory>
#include <set>
#include "../../core/I18n.h"
#include "../../core/Song.h"
#include "../../engine/Program.h"
#include "../PacksUi.h"
#include "../../app/Widgets.h"

namespace gl::packs::plasma {

namespace ui {

namespace fs = std::filesystem;

// ================================================================== palette
namespace {
ImU32 rgb(uint32_t v, float a = 1.0f) { return theme::rgb(v, a); }

Pal makeDark() {
  Pal p;
  p.dark = true;
  p.frame = rgb(0x08090C);
  p.panel = rgb(0x181B21);
  p.panelHi = rgb(0x22262E);
  p.panelLo = rgb(0x14161B);
  p.head = rgb(0x282D36);
  p.headOn = rgb(0x313742);
  p.well = rgb(0x0C0E12);
  p.wellHi = rgb(0x11141A);
  p.grid = rgb(0x1B1F27);
  p.gridStrong = rgb(0x272C36);
  p.line = rgb(0x2A2F39);
  p.lineHi = rgb(0x3A404C);
  p.text = rgb(0xE4E8EF);
  p.textDim = rgb(0x99A2B3);
  p.textFaint = rgb(0x5C6576);
  p.label = rgb(0xBAC2D0);
  p.accent = rgb(0xD963F1);
  p.accentDim = rgb(0x8D4A9E);
  p.accentSoft = rgb(0xD963F1, 0.16f);
  p.mod = rgb(0x4ECCFF);
  p.modSoft = rgb(0x4ECCFF, 0.18f);
  p.warn = rgb(0xFFB84D);
  p.good = rgb(0x7AE4A3);
  p.danger = rgb(0xFF5B53);
  p.capTop = rgb(0x3A3F4A);
  p.capBot = rgb(0x23272F);
  p.rail = rgb(0x0B0D11);
  p.pointer = rgb(0xF2F4F8);
  p.keyWhite = rgb(0xD8DDE6);
  p.keyBlack = rgb(0x1A1D23);
  p.keyDown = rgb(0xD963F1);
  return p;
}
Pal makeLight() {
  Pal p;
  p.dark = false;
  p.frame = rgb(0xB4BAC5);
  p.panel = rgb(0xE6E9EE);
  p.panelHi = rgb(0xF1F3F6);
  p.panelLo = rgb(0xDADEE5);
  p.head = rgb(0xD3D8E0);
  p.headOn = rgb(0xC5CBD6);
  p.well = rgb(0xF8F9FB);
  p.wellHi = rgb(0xFFFFFF);
  p.grid = rgb(0xE6E9EE);
  p.gridStrong = rgb(0xD3D8E0);
  p.line = rgb(0xBFC5CF);
  p.lineHi = rgb(0xA4ABB8);
  p.text = rgb(0x1B1F27);
  p.textDim = rgb(0x4F5768);
  p.textFaint = rgb(0x8C94A3);
  p.label = rgb(0x363C49);
  p.accent = rgb(0xA53BC4);
  p.accentDim = rgb(0xC98ADB);
  p.accentSoft = rgb(0xA53BC4, 0.14f);
  p.mod = rgb(0x1689C7);
  p.modSoft = rgb(0x1689C7, 0.16f);
  p.warn = rgb(0xC77A12);
  p.good = rgb(0x2E9E62);
  p.danger = rgb(0xD23B35);
  p.capTop = rgb(0xFDFDFE);
  p.capBot = rgb(0xD5DAE2);
  p.rail = rgb(0xC3C9D3);
  p.pointer = rgb(0x262A33);
  p.keyWhite = rgb(0xFBFCFD);
  p.keyBlack = rgb(0x2B2F37);
  p.keyDown = rgb(0xC77DE0);
  return p;
}
skin::Skin makeSkin(const Pal& p) {
  skin::Skin s;
  s.panel = p.panel, s.panelHi = p.panelHi, s.panelLo = p.panelLo, s.bevel = p.line;
  s.rail = p.rail, s.fill = p.accent, s.fillPressed = p.accent, s.cap = p.capTop, s.handle = p.capTop;
  s.text = p.text, s.textDim = p.textDim, s.lcdBg = p.well, s.lcdText = p.accent;
  s.graphBg = p.well, s.graphGrid = p.grid, s.graphGridStrong = p.gridStrong, s.graphText = p.textDim, s.graphCurve = p.accent;
  s.select = p.accent, s.well = p.panelLo, s.pillBg = p.head, s.pillText = p.textDim, s.valueBg = p.well, s.valueText = p.accent;
  s.accent2 = p.mod, s.raised = p.panelHi, s.pointer = p.pointer, s.labelBg = p.head;
  return s;
}
}  // namespace

const Pal& pal() {
  static const Pal d = makeDark(), l = makeLight();
  return theme::isDark() ? d : l;
}
const skin::Skin& skin() {
  static const skin::Skin d = makeSkin(makeDark()), l = makeSkin(makeLight());
  return theme::isDark() ? d : l;
}

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
std::string ptext(const Ctx& c, int k) { return paramText(k, val(c, k), &c.st.params); }

// ================================================================== text
float font(const Ctx& c, float px) { return std::max(7.0f, std::round(px * c.F.s * 2.0f) * 0.5f); }
namespace {
ImFont* pick(const Ctx& c, bool bold) { return bold ? (c.a.fontBold ? c.a.fontBold : c.a.fontRegular) : c.a.fontRegular; }
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
};
std::vector<LayoutItem> gLayout;
int gLayer = 0;
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
    layoutAdd(c, ImVec2(at.x + 0.5f, at.y + sz.y * 0.18f), ImVec2(at.x + sz.x - 0.5f, at.y + sz.y * 0.85f), false, s);
  }
}
// Reports intersections (text / text, text / knob) of the frame on stderr, once per pair.
void layoutReport(const Ctx& c) {
  if (!layoutOn()) return;
  static std::set<std::string> seen;
  static size_t most = 0;
  const std::string tag = std::getenv("GL_PLASMA_LAYOUT_TAG") ? std::getenv("GL_PLASMA_LAYOUT_TAG") : "";
  if (gLayout.size() > most) most = gLayout.size(), std::fprintf(stderr, "PLASMA_LAYOUT %s: %zu items\n", tag.c_str(), most);
  for (size_t i = 0; i < gLayout.size(); ++i)
    for (size_t j = i + 1; j < gLayout.size(); ++j) {
      const LayoutItem &p = gLayout[i], &q = gLayout[j];
      if (p.layer != q.layer || (p.knob && q.knob)) continue;
      const float ox = std::min(p.b.x, q.b.x) - std::max(p.a.x, q.a.x), oy = std::min(p.b.y, q.b.y) - std::max(p.a.y, q.a.y);
      if (ox <= c.s(0.6f) || oy <= c.s(0.6f)) continue;
      const std::string key = tag + "|" + p.s + "|" + q.s + "|" + std::to_string(static_cast<int>(p.a.x / c.F.s)) + "," +
                              std::to_string(static_cast<int>(p.a.y / c.F.s));
      if (!seen.insert(key).second) continue;
      std::fprintf(stderr, "PLASMA_OVERLAP %s: '%s' (%.0f,%.0f) x '%s' (%.0f,%.0f) by %.1fx%.1f px\n", tag.c_str(), p.knob ? "<knob>" : p.s.c_str(),
                   (p.a.x - c.F.o.x) / c.F.s, (p.a.y - c.F.o.y) / c.F.s, q.knob ? "<knob>" : q.s.c_str(), (q.a.x - c.F.o.x) / c.F.s,
                   (q.a.y - c.F.o.y) / c.F.s, ox / c.F.s, oy / c.F.s);
    }
  gLayout.clear();
  gLayer = 0;
}
}  // namespace
float textW(const Ctx& c, const char* s, float px, bool bold) { return measure(c, s, px, bold).x / c.F.s; }
void text(const Ctx& c, float x, float y, const char* s, ImU32 col, float px, bool bold) { put(c, c.p(x, y), s, col, px, bold); }
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
void labelBeside(const Ctx& c, float x, float boxY, float boxH, const char* s, ImU32 col, float px) {
  ImFont* f = pick(c, true);
  const float vTop = std::round(c.p(0, boxY + boxH * 0.5f).y - measure(c, "0", 9.0f, true).y * 0.5f);
  put(c, ImVec2(c.p(x, 0).x, vTop + f->GetFontBaked(font(c, 9.0f))->Ascent - f->GetFontBaked(font(c, px))->Ascent), s, col, px, true);
}
void textMid(const Ctx& c, float x0, float x1, float cy, const char* s, ImU32 col, float px, bool bold) {
  const ImVec2 sz = measure(c, s, px, bold);
  const ImVec2 a = c.p(x0, cy), b = c.p(x1, cy);
  const float w = b.x - a.x;
  if (sz.x > w - 2) {  // clip long names to the field
    c.dl->PushClipRect(ImVec2(a.x + 1, a.y - sz.y), ImVec2(b.x - 1, a.y + sz.y), true);
    put(c, ImVec2(a.x + 2, a.y - sz.y * 0.5f), s, col, px, bold);
    c.dl->PopClipRect();
    return;
  }
  put(c, ImVec2(a.x + (w - sz.x) * 0.5f, a.y - sz.y * 0.5f), s, col, px, bold);
}

// ================================================================== surfaces
void panelRect(const Ctx& c, float x, float y, float w, float h, ImU32 col, float r) {
  c.dl->AddRectFilled(c.p(x, y), c.p(x + w, y + h), col, c.s(r));
}
void wellRect(const Ctx& c, float x, float y, float w, float h, float r) {
  const ImVec2 a = c.p(x, y), b = c.p(x + w, y + h);
  gl::audit::group(a, b, "well");  // J-128: rows are checked per panel
  if (look::styled()) return look::hookWell(c.dl, a, b, c.P.well);  // J-129 control style
  c.dl->AddRectFilled(a, b, c.P.well, c.s(r));
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
  constexpr float a0 = 3.14159265f * 0.75f, a1 = 3.14159265f * 2.25f, mid = (a0 + a1) * 0.5f;
  const float th = std::max(1.0f, r * 0.2f), thLit = std::max(1.5f, r * 0.36f);
  dl->PathArcTo(c, r, a0, a1, 24);
  dl->PathStroke(track, 0, th);
  if (bipolar) {  // the zero mark at the top
    dl->AddLine(ImVec2(c.x, c.y - r - th), ImVec2(c.x, c.y - r + th * 1.5f), track, th);
  }
  if (idle || (!bipolar && v < 0.005f)) return;  // a unipolar source at zero reads as the empty track
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
// Modulation context menu for parameter k (slots, add, standard menu).
void modMenu(Ctx& c, int k, bool& openStd) {
  ImGui::TextDisabled("%s", paramLongName(k).c_str());
  ImGui::Separator();
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
  if (ImGui::BeginMenu(i18n::tr("Add modulation"))) {
    int src = -1;
    if (sourceMenuItems(c, src) && src > SrcNone) addModulation(c, src, k, 0.5f);
    ImGui::EndMenu();
  }
  ImGui::Separator();
  if (ImGui::MenuItem(i18n::tr("Automation, MIDI link..."))) openStd = true;
}
}  // namespace

bool knob(Ctx& c, int k, float cx, float cy, float r, const char* label, int flags) {
  const ParamDesc* d = desc(k);
  if (!d) return false;
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
  const bool rclick = hov && ImGui::IsMouseClicked(ImGuiMouseButton_Right);
  if (act && io.KeyAlt && !slots.empty()) {
    // Alt + drag: the amount of the first modulation slot
    const int s = slots.front();
    if (io.MouseDelta.y != 0) {
      const double amt = std::clamp(val(c, modP(s, M_Amount)) - io.MouseDelta.y / 160.0 * (io.KeyShift ? 0.15 : 1.0), -1.0, 1.0);
      setv(c, modP(s, M_Amount), amt, true);
    }
    if (ImGui::IsItemDeactivated()) c.a.doc.endGesture();
  } else if (rclick) {
    ImGui::OpenPopup("##plmod");
  } else {
    changed = interact(c, k, v, 160.0f, false);
  }
  dropTarget(c, k);
  bool openStd = false;
  if (ImGui::BeginPopup("##plmod")) {
    modMenu(c, k, openStd);
    ImGui::EndPopup();
  }
  if (openStd) ImGui::OpenPopup("##pmenu");
  // face
  const double n = d->toNormalized(v);
  const bool bip = (flags & KnobBipolar) || (d->min < 0 && d->max > 0);
  skin::KnobPaint kp;
  kp.rail = c.P.rail;
  kp.fill = act ? skin::lighten(c.P.accent, 0.15f) : c.P.accent;
  kp.cap = c.P.capTop;
  kp.pointer = c.P.pointer;
  kp.a0 = kA0, kp.a1 = kA1;
  kp.an = kA0 + static_cast<float>(n) * (kA1 - kA0);
  kp.from = bip ? (kA0 + kA1) * 0.5f : kA0;
  kp.hot = hov, kp.active = act;
  kp.railTh = std::max(2.0f, R * 0.17f);
  // bezel: a dark socket around the ring, with min / max tick dots
  c.dl->AddCircleFilled(C, R + c.s(1.6f), c.P.dark ? theme::withAlpha(c.P.frame, 0.9f) : theme::withAlpha(c.P.lineHi, 0.45f), 40);
  for (float ta : {kA0, kA1})
    c.dl->AddCircleFilled(ImVec2(C.x + std::cos(ta) * (R + c.s(3.4f)), C.y + std::sin(ta) * (R + c.s(3.4f))), std::max(0.8f, c.s(0.9f)),
                          c.P.textFaint, 8);
  skin::knobFace(c.dl, C, R, kp);
  layoutAdd(c, ImVec2(C.x - R, C.y - R), ImVec2(C.x + R, C.y + R), true, label);
  // modulation rings (amount around the value), live value dot
  float ringR = R + c.s(3.2f);
  double live = 0;
  for (size_t i = 0; i < slots.size() && i < 3; ++i) {
    const int s = slots[i];
    const float amt = valf(c, modP(s, M_Amount)) * valf(c, modP(s, M_Out));
    const bool byp = val(c, modP(s, M_Bypass)) >= 0.5;
    const float a0 = kp.an, a1 = std::clamp(kp.an + amt * (kA1 - kA0), kA0, kA1);
    const ImU32 col = byp ? c.P.textFaint : c.P.mod;
    if (std::fabs(a1 - a0) > 0.01f) {
      c.dl->PathArcTo(C, ringR, std::min(a0, a1), std::max(a0, a1), 24);
      c.dl->PathStroke(col, 0, std::max(1.5f, c.s(2.0f)));
    }
    if (c.hasLive && !byp) live += c.live.mod[s];
    ringR += c.s(2.6f);
  }
  if (!slots.empty() && c.hasLive && c.live.voices > 0) {
    const float al = std::clamp(kp.an + static_cast<float>(live) * (kA1 - kA0), kA0, kA1);
    c.dl->AddCircleFilled(ImVec2(C.x + std::cos(al) * (R - kp.railTh * 0.5f), C.y + std::sin(al) * (R - kp.railTh * 0.5f)), std::max(1.8f, c.s(2.2f)),
                          c.P.mod);
  }
  // label / value
  if (!(flags & KnobNoLabel) && label) {
    const bool showVal = act || (flags & KnobValue);
    const std::string vs = showVal ? paramText(k, v, &c.st.params) : std::string(label);
    fitLabel(c, cx, cy + r + 4.5f, r * 2 + 22, vs.c_str(), showVal ? c.P.accent : (hov ? c.P.text : c.P.label), (flags & KnobSmall) ? 8.0f : 9.0f, true);
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_NoSharedDelay) || act) {
    std::string s = paramText(k, v, &c.st.params);
    if (!slots.empty()) s += "   (" + std::to_string(slots.size()) + (slots.size() == 1 ? " mod)" : " mods)");
    bubble(c, ImVec2(C.x, C.y - R - c.s(4)), s);
  }
  if (hov)
    c.a.setHint(paramLongName(k) + ": " + paramText(k, v, &c.st.params) +
                (slots.empty() ? "   (drag \xC2\xB7 drop a source here to modulate \xC2\xB7 right-click: modulation)"
                               : "   (Alt+drag: modulation amount \xC2\xB7 right-click: modulation)"));
  ImGui::PopID();
  return changed;
}

bool knobFree(Ctx& c, const char* id, float& v, float cx, float cy, float r, const char* label, bool bipolar, const char* valueText) {
  const ImVec2 C = c.p(cx, cy);
  const float R = c.s(r);
  ImGui::PushID(id);
  ImGui::SetCursorScreenPos(ImVec2(C.x - R, C.y - R));
  ImGui::InvisibleButton("##kf", ImVec2(R * 2, R * 2));
  const bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
  bool changed = false;
  const float lo = bipolar ? -1.f : 0.f;
  if (act && ImGui::GetIO().MouseDelta.y != 0) {
    v = std::clamp(v - ImGui::GetIO().MouseDelta.y / 160.f * (1.f - lo) * (ImGui::GetIO().KeyShift ? 0.15f : 1.f), lo, 1.f);
    changed = true;
  }
  if (hov && ImGui::IsMouseDoubleClicked(0)) v = bipolar ? 0.f : 0.5f, changed = true;
  const float n = (v - lo) / (1.f - lo);
  skin::KnobPaint kp;
  kp.rail = c.P.rail, kp.fill = c.P.accent, kp.cap = c.P.capTop, kp.pointer = c.P.pointer;
  kp.a0 = kA0, kp.a1 = kA1, kp.an = kA0 + n * (kA1 - kA0), kp.from = bipolar ? (kA0 + kA1) * 0.5f : kA0;
  kp.hot = hov, kp.active = act, kp.railTh = std::max(2.0f, R * 0.17f);
  skin::knobFace(c.dl, C, R, kp);
  if (label) textC(c, cx, cy + r + 4, act && valueText ? valueText : label, act ? c.P.accent : c.P.textDim, 8.5f, true);
  if ((hov || act) && valueText) bubble(c, ImVec2(C.x, C.y - R - c.s(4)), valueText);
  ImGui::PopID();
  return changed;
}

namespace {
bool fader(Ctx& c, int k, float x, float y, float w, float h, bool vertical) {
  const ParamDesc* d = desc(k);
  if (!d) return false;
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
  }
  ImGui::PopID();
  return ch;
}
}  // namespace
bool hfader(Ctx& c, int k, float x, float y, float w, float h) { return fader(c, k, x, y, w, h, false); }
bool vfader(Ctx& c, int k, float x, float y, float w, float h) { return fader(c, k, x, y, w, h, true); }

bool led(Ctx& c, int k, float x, float y, float size) {
  ImGui::PushID(k);
  ImGui::PushID("led");
  ImGui::SetCursorScreenPos(c.p(x - 2, y - 2));
  const bool click = ImGui::InvisibleButton("##l", c.F.sz(size + 4, size + 4));
  const bool on = val(c, k) >= 0.5;
  if (click) setv(c, k, on ? 0 : 1);
  const ImVec2 a = c.p(x, y), b = c.p(x + size, y + size);
  if (on) {
    c.dl->AddRectFilled(ImVec2(a.x - c.s(1.5f), a.y - c.s(1.5f)), ImVec2(b.x + c.s(1.5f), b.y + c.s(1.5f)), theme::withAlpha(c.P.accent, 0.22f), c.s(3));
    skin::gradientRect(c.dl, a, b, skin::lighten(c.P.accent, 0.25f), c.P.accent, c.s(2));
  } else {
    c.dl->AddRectFilled(a, b, c.P.well, c.s(2));
    c.dl->AddRect(a, b, c.P.lineHi, c.s(2));
  }
  if (ImGui::IsItemHovered()) c.a.setHint(paramLongName(k) + (on ? ": on (click: off)" : ": off (click: on)"));
  ImGui::PopID();
  ImGui::PopID();
  return click;
}
bool checkBox(Ctx& c, int k, float x, float y, const char* label, float px) {
  ImGui::PushID(k);
  ImGui::PushID("chk");
  const float w = 13 + textW(c, label, px, true);
  ImGui::SetCursorScreenPos(c.p(x, y - 1));
  const bool click = ImGui::InvisibleButton("##c", c.F.sz(w, 11));
  const bool on = val(c, k) >= 0.5;
  if (click) setv(c, k, on ? 0 : 1);
  const ImVec2 a = c.p(x, y + 0.5f), b = c.p(x + 8.5f, y + 9);
  if (on) skin::gradientRect(c.dl, a, b, skin::lighten(c.P.accent, 0.2f), c.P.accent, c.s(2));
  else c.dl->AddRectFilled(a, b, c.P.well, c.s(2)), c.dl->AddRect(a, b, c.P.lineHi, c.s(2));
  text(c, x + 13, y, label, on ? c.P.text : c.P.textDim, px, true);
  if (ImGui::IsItemHovered()) c.a.setHint(paramLongName(k) + (on ? ": on" : ": off"));
  ImGui::PopID();
  ImGui::PopID();
  return click;
}
bool button(Ctx& c, const char* id, float x, float y, float w, float h, const char* label, bool on, float px) {
  ImGui::PushID(id);
  ImGui::SetCursorScreenPos(c.p(x, y));
  const bool click = ImGui::InvisibleButton("##b", c.F.sz(w, h));
  const bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
  const ImVec2 a = c.p(x, y), b = c.p(x + w, y + h);
  if (on) {
    c.dl->AddRectFilled(a, b, c.P.accentSoft, c.s(3));
    c.dl->AddRect(a, b, theme::withAlpha(c.P.accent, 0.7f), c.s(3));
  } else {
    skin::gradientRect(c.dl, a, b, act ? c.P.panelLo : (hov ? c.P.headOn : c.P.panelHi), act ? c.P.panelLo : c.P.head, c.s(3));
    c.dl->AddRect(a, b, hov ? c.P.lineHi : c.P.line, c.s(3));
  }
  if (label && *label) textMid(c, x, x + w, y + h * 0.5f, label, on ? c.P.accent : (hov ? c.P.text : c.P.textDim), px, true);
  ImGui::PopID();
  return click;
}
bool toggleBtn(Ctx& c, int k, float x, float y, float w, float h, const char* label, float px) {
  ImGui::PushID(k);
  const bool on = val(c, k) >= 0.5;
  const bool click = button(c, "tg", x, y, w, h, label, on, px);
  if (click) setv(c, k, on ? 0 : 1);
  if (ImGui::IsItemHovered()) c.a.setHint(paramLongName(k) + (on ? ": on" : ": off"));
  ImGui::PopID();
  return click;
}

bool valueBox(Ctx& c, int k, float x, float y, float w, float h, const char* label, const char* override) {
  ImGui::PushID(k);
  ImGui::PushID("vb");
  ImGui::SetCursorScreenPos(c.p(x, y));
  ImGui::InvisibleButton("##v", c.F.sz(w, h), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
  const bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
  double v = val(c, k);
  const bool ch = interact(c, k, v, 90.0f, false);
  dropTarget(c, k);
  const ImVec2 a = c.p(x, y), b = c.p(x + w, y + h);
  c.dl->AddRectFilled(a, b, act ? c.P.panelLo : c.P.well, c.s(2));
  c.dl->AddRect(a, b, hov || act ? c.P.lineHi : c.P.line, c.s(2));
  float vx0 = x;
  const std::string s = override ? std::string(override) : paramText(k, v, &c.st.params);
  if (label && *label) {
    // J-128: the label sits on the value's baseline (textMid centres the value)
    ImFont* f = pick(c, true);
    const float vTop = std::round(c.p(0, y + h * 0.5f).y - measure(c, s.c_str(), 9.0f, true).y * 0.5f);
    const float lTop = vTop + f->GetFontBaked(font(c, 9.0f))->Ascent - f->GetFontBaked(font(c, 8.5f))->Ascent;
    put(c, ImVec2(c.p(x + 4, 0).x, lTop), label, c.P.textDim, 8.5f, true);
    vx0 = x + 6 + textW(c, label, 8.5f, true);
  }
  textMid(c, vx0, x + w, y + h * 0.5f, s.c_str(), c.P.accent, 9.0f, true);
  if (!modSlotsFor(c, k).empty()) c.dl->AddRectFilled(ImVec2(b.x - c.s(4), a.y + c.s(2)), ImVec2(b.x - c.s(2), b.y - c.s(2)), c.P.mod, 1);
  if (hov) c.a.setHint(paramLongName(k) + ": " + paramText(k, v, &c.st.params) + "   (drag \xC2\xB7 double-click: type \xC2\xB7 right-click: menu)");
  ImGui::PopID();
  ImGui::PopID();
  return ch;
}

bool dropField(Ctx& c, const char* id, float x, float y, float w, float h, const char* shown, const std::function<void()>& items, bool arrows,
               int* step) {
  ImGui::PushID(id);
  const float aw = arrows ? 13.0f : 0.0f;
  const float fw = w - aw * 2;
  ImGui::SetCursorScreenPos(c.p(x, y));
  const bool click = ImGui::InvisibleButton("##d", c.F.sz(fw, h));
  const bool hov = ImGui::IsItemHovered();
  const ImVec2 a = c.p(x, y), b = c.p(x + fw, y + h);
  c.dl->AddRectFilled(a, b, hov ? c.P.panelHi : c.P.well, c.s(2));
  c.dl->AddRect(a, b, hov ? c.P.lineHi : c.P.line, c.s(2));
  textMid(c, x + 2, x + fw - 12, y + h * 0.5f, shown, c.P.accent, 9.0f, true);
  iconArrow(c.dl, c.p(x + fw - 7, y + h * 0.5f), c.s(6), 3, c.P.textDim);
  if (click) ImGui::OpenPopup("##dd");
  bool changed = false;
  ImGui::SetNextWindowSizeConstraints(ImVec2(c.s(std::max(w, 140.0f)), 0), ImVec2(FLT_MAX, c.s(520)));
  if (ImGui::BeginPopup("##dd")) {
    items();
    ImGui::EndPopup();
  }
  if (arrows && step) {
    for (int i = 0; i < 2; ++i) {
      const float bx = x + fw + aw * i;
      ImGui::SetCursorScreenPos(c.p(bx, y));
      ImGui::PushID(i);
      if (ImGui::InvisibleButton("##ar", c.F.sz(aw, h))) *step = i == 0 ? -1 : 1, changed = true;
      const bool ah = ImGui::IsItemHovered();
      c.dl->AddRectFilled(c.p(bx + 0.5f, y), c.p(bx + aw - 0.5f, y + h), ah ? c.P.headOn : c.P.head, c.s(2));
      iconArrow(c.dl, c.p(bx + aw * 0.5f, y + h * 0.5f), c.s(6.5f), i == 0 ? 0 : 1, ah ? c.P.text : c.P.textDim);
      ImGui::PopID();
    }
  }
  ImGui::PopID();
  return changed;
}

bool choiceBox(Ctx& c, int k, float x, float y, float w, float h, bool arrows, const char* override, const std::vector<std::string>* names) {
  const ParamDesc* d = desc(k);
  if (!d) return false;
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
  if (ImGui::IsItemHovered()) c.a.setHint(paramLongName(k) + ": " + shown);
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
  c.dl->AddRectFilled(a, b, c.P.well, c.s(2));
  c.dl->AddRect(a, b, hov || act ? c.P.lineHi : c.P.line, c.s(2));
  char buf[32];
  std::snprintf(buf, sizeof buf, "%d", v);
  textMid(c, x, x + w - 10, y + h * 0.5f, shown ? shown : buf, c.P.accent, 9.0f, true);
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
  const ParamDesc* d = desc(k);
  if (!d) return false;
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
    v.push_back({"Note", {SrcVelo, SrcNote, SrcReleaseVelo, SrcAftertouch, SrcPolyAT}});
    v.push_back({"Controllers", {SrcModWheel, SrcPitchBend}});
    v.push_back({"MPE", {SrcExprX, SrcExprY, SrcExprZ}});
    v.push_back({"Random", {SrcNoise, SrcRand1, SrcRand2, SrcRandDiscrete, SrcAlt1, SrcAlt2}});
    v.push_back({"Voice", {SrcActiveVoices, SrcVoiceIndex, SrcVoiceMod1, SrcVoiceMod2}});
    v.push_back({"Audio", {SrcOscA, SrcOscB, SrcOscC, SrcSub, SrcFilter1, SrcFilter2}});
    v.push_back({"Other", {SrcFixed}});
    return v;
  }();
  return g;
}
}  // namespace

std::string sourceLabel(int src) {
  const auto& n = sourceNames();
  if (src <= SrcNone || src >= static_cast<int>(n.size())) return "\xE2\x80\x94";
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
  for (int o = 0; o < kOscs; ++o) group(o == 0 ? "Osc A" : o == 1 ? "Osc B" : "Osc C", oscP(o, 0), oscP(o, O_Count));
  group("Sub", kSubBase, kNoiseBase);
  group("Noise", kNoiseBase, kFiltBase);
  group("Filter 1", filtP(0, 0), filtP(1, 0));
  group("Filter 2", filtP(1, 0), filtP(2, 0));
  group("Routing", kRouteBase, kEnvBase);
  if (ImGui::BeginMenu(i18n::tr("Envelopes"))) {
    for (int e = 0; e < kEnvs; ++e) {
      char b[16];
      std::snprintf(b, sizeof b, "Env %d", e + 1);
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
  group("LFO busses", kBusBase, kRetrigBase);
  group("Retriggers", kRetrigBase, kModBase);
  if (ImGui::BeginMenu(i18n::tr("Effects"))) {
    for (int s = 0; s < kFxSlots; ++s) {
      const int type = vali(c, fxP(s, X_Type));
      if (type <= FxNone || type >= FxTypeCount) continue;
      const std::string nm = "FX " + std::to_string(s + 1) + " " + fxTypeNames()[static_cast<size_t>(type)];
      if (ImGui::BeginMenu(nm.c_str())) {
        const auto& defs = fxParams(type);
        for (int q = 0; q < static_cast<int>(defs.size()); ++q)
          if (defs[static_cast<size_t>(q)].kind == 0 && ImGui::MenuItem(defs[static_cast<size_t>(q)].label)) out = fxP(s, X_P0 + q), chose = true;
        ImGui::EndMenu();
      }
    }
    ImGui::EndMenu();
  }
  group("Global", 0, G_Count);
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
    setv(c, modP(s, M_Source), src);
    setv(c, modP(s, M_Dest), k + 1);
    setv(c, modP(s, M_Amount), amount);
    return s;
  }
  return -1;
}
void sourceHandle(Ctx& c, int src, float cx, float cy, float r) {
  ImGui::PushID(src + 7000);
  const ImVec2 C = c.p(cx, cy);
  const float R = c.s(r);
  ImGui::SetCursorScreenPos(ImVec2(C.x - R - c.s(2), C.y - R - c.s(2)));
  ImGui::InvisibleButton("##src", ImVec2(R * 2 + c.s(4), R * 2 + c.s(4)));
  const bool hov = ImGui::IsItemHovered();
  if (hov || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);  // a grip, not a button
  // live meter of the source's output (the lead note, or the global value)
  float raw = 0;
  bool idle = !c.hasLive;
  if (c.hasLive && src > SrcNone && src < SourceCount) raw = c.live.source[src];
  if (src >= SrcMacro1 && src <= SrcMacro8 && !c.hasLive) raw = valf(c, macroP(src - SrcMacro1)), idle = false;
  if (c.hasLive && (src >= SrcLfo1 && src <= SrcLfo10Y)) {  // a per-note LFO with no note sounding rests
    const int l = (src - SrcLfo1) % kLfos;
    if (c.live.voices == 0 && vali(c, lfoP(l, L_Mode)) != LfoFree && val(c, lfoP(l, L_Mono)) < 0.5) idle = true;
  }
  bool bip = false;
  const float mv = sourceMeterValue(src, raw, bip);
  iconSourceMeter(c.dl, C, R, mv, bip, idle, hov ? c.P.textDim : c.P.textFaint, c.P.mod);
  if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoPreviewTooltip)) {
    ImGui::SetDragDropPayload("PLASMA_SRC", &src, sizeof(int));
    const ImVec2 m = ImGui::GetIO().MousePos;
    ImDrawList* fg = ImGui::GetForegroundDrawList();
    fg->AddLine(C, m, c.P.mod, std::max(1.5f, c.s(1.6f)));
    fg->AddCircleFilled(m, c.s(4), c.P.mod);
    const std::string s = sourceLabel(src);
    fg->AddText(c.a.fontBold, font(c, 9), ImVec2(m.x + c.s(8), m.y - c.s(14)), c.P.mod, s.c_str());
    ImGui::EndDragDropSource();
  }
  if (hov) c.a.setHint(sourceLabel(src) + ": drag onto a control to modulate it");
  ImGui::PopID();
}
void dropTarget(Ctx& c, int k) {
  if (!modulatable(k)) return;
  if (ImGui::BeginDragDropTarget()) {
    const ImGuiPayload* pl = ImGui::GetDragDropPayload();
    if (pl && pl->IsDataType("PLASMA_SRC")) {
      const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
      ImGui::GetForegroundDrawList()->AddRect(a, b, c.P.mod, c.s(3), 0, std::max(1.5f, c.s(1.5f)));
    }
    if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("PLASMA_SRC", ImGuiDragDropFlags_AcceptNoDrawDefaultRect)) {
      int src = 0;
      std::memcpy(&src, p->Data, sizeof(int));
      addModulation(c, src, k, 0.5f);
    }
    ImGui::EndDragDropTarget();
  }
}

void warpMenuItems(Ctx& c, int o, int slot) {
  const int type = vali(c, oscP(o, O_Type));
  const int kmode = oscP(o, slot == 0 ? O_Warp1Mode : O_Warp2Mode);
  const int cur = vali(c, kmode);
  const auto& names = warpNames();
  auto item = [&](int i) {
    if (i < 0 || i >= static_cast<int>(names.size()) || !warpAllowed(type, i)) return;
    if (ImGui::MenuItem(names[static_cast<size_t>(i)].c_str(), nullptr, i == cur)) setv(c, kmode, i);
  };
  auto range = [&](const char* group, int a, int b) {
    bool any = false;
    for (int i = a; i < b; ++i) any = any || warpAllowed(type, i);
    if (!any) return;
    if (ImGui::BeginMenu(i18n::tr(group))) {
      for (int i = a; i < b; ++i) item(i);
      ImGui::EndMenu();
    }
  };
  item(0);
  item(1);
  range("Alt warp", 2, 11);
  range("Remap", 11, 15);
  item(15);
  item(16);
  range("Filter", 17, 19);
  range("Distortion", 19, 33);
  range("FM", 33, 51);
  range("Phase distortion", 51, 58);
  range("AM", 58, 64);
  range("Ring mod", 64, 70);
  range("Spectral", 70, 92);
  range("Mask", 92, 98);
  range("Vocode", 98, 104);
  ImGui::Separator();
  if (ImGui::MenuItem(i18n::tr("Swap warps"))) {
    const int a = vali(c, oscP(o, O_Warp1Mode)), b = vali(c, oscP(o, O_Warp2Mode));
    const double va = val(c, oscP(o, O_Warp1)), vb = val(c, oscP(o, O_Warp2));
    setv(c, oscP(o, O_Warp1Mode), b), setv(c, oscP(o, O_Warp2Mode), a);
    setv(c, oscP(o, O_Warp1), vb), setv(c, oscP(o, O_Warp2), va);
  }
}
void filterMenuItems(Ctx& c, int k) {
  const auto& names = filterNames();
  const auto& groups = filterGroups();
  const int cur = vali(c, k);
  std::string open;
  for (size_t i = 0; i < names.size(); ++i) {
    const std::string& g = i < groups.size() ? groups[i] : std::string("Other");
    if (g == open) continue;
    if (ImGui::BeginMenu(g.c_str())) {
      for (size_t j = i; j < names.size(); ++j)
        if ((j < groups.size() ? groups[j] : std::string("Other")) == g)
          if (ImGui::MenuItem(names[j].c_str(), nullptr, static_cast<int>(j) == cur)) setv(c, k, static_cast<double>(j));
      ImGui::EndMenu();
    }
    open = g;
    // skip the rest of this group (types of a group are contiguous)
  }
}

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
  ImGui::SetCursorScreenPos(a);
  ImGui::InvisibleButton("##ce", ImVec2(b.x - a.x, b.y - a.y), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
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
  for (int k = kFxBase; k < kParamCount && k < static_cast<int>(ns.params.size()) && k < static_cast<int>(c.st.params.size()); ++k)
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

std::string openReportText(const Model& m) {
  if (m.openFormat.empty()) return "";
  std::string t = std::string(i18n::tr("Opened from")) + ": " + m.openFormat;
  if (!m.openNotes.empty()) {
    t += std::string("\n\n") + i18n::tr("Approximated") + ":";
    for (const auto& n : m.openNotes) t += "\n  - " + n;
  }
  if (!m.openMissing.empty()) {
    t += std::string("\n\n") + i18n::tr("Files not found (install the content or add its folder in the preset browser)") + ":";
    for (const auto& n : m.openMissing) t += "\n  - " + n;
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
    const auto roots = defaultSerumRoots();
    lastDir = roots.empty() ? "" : roots.front();
    if (lastDir.empty() || !fs::is_directory(lastDir, ec)) {
      const char* h = std::getenv("HOME");
      lastDir = h ? h : ".";
    }
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
  ImGui::PushStyleColor(ImGuiCol_FrameBg, theme::vec(c.P.well));
  ImGui::PushStyleColor(ImGuiCol_Text, theme::vec(c.P.text));
  ImGui::PushStyleColor(ImGuiCol_TextDisabled, theme::vec(c.P.textFaint));
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
namespace {
void drawMeter(Ctx& c, float x, float y, float w, float h) {
  wellRect(c, x, y, w, h, 2);
  const float lv = c.hasLive ? std::clamp(c.live.outL, 0.f, 1.5f) : 0.f;
  const float db = lv > 1e-5f ? 20.f * std::log10(lv) : -90.f;
  const float n = std::clamp((db + 48.f) / 51.f, 0.f, 1.f);
  if (n > 0) {
    const ImVec2 a = c.p(x + 2, y + h - 2 - n * (h - 4)), b = c.p(x + w - 2, y + h - 2);
    skin::gradientRect(c.dl, a, b, db > -1.f ? c.P.danger : c.P.good, theme::withAlpha(c.P.good, 0.6f), c.s(1));
  }
}

void presetMenu(Ctx& c) {
  const PluginInfo* in = info();
  if (ImGui::MenuItem(i18n::tr("Init"))) applyPreset(c, {}, "", "Init");
  ImGui::Separator();
  if (in && ImGui::BeginMenu(i18n::tr("Factory"))) {
    for (const auto& p : in->presets)
      if (p.name != "Init" && ImGui::MenuItem(p.name.c_str())) applyPreset(c, p.values, "", p.name);
    ImGui::Separator();
    for (const auto& p : factoryDataPresets())
      if (ImGui::MenuItem(p.name.c_str())) applyPreset(c, p.values, p.data, p.name);
    ImGui::EndMenu();
  }
  ImGui::Separator();
  if (ImGui::MenuItem(i18n::tr("Open preset..."))) openPresetDialog(c);
  if (ImGui::MenuItem(i18n::tr("Browse presets..."))) c.u.browser = 1;
}

void header(Ctx& c) {
  const Pal& P = c.P;
  panelRect(c, 0, 0, kW, kHeadH, P.frame);
  // wordmark: accent bar + lower-case name
  {
    const ImVec2 a = c.p(14, 22), b = c.p(17, 46);
    c.dl->AddRectFilled(a, b, P.accent, c.s(1));
    ImFont* f = c.a.fontBold ? c.a.fontBold : c.a.fontRegular;
    const float fs = font(c, 25);
    c.dl->AddText(f, fs, ImVec2(std::round(c.p(24, 0).x), std::round(c.p(0, 34).y - fs * 0.56f)), P.text, "plasma");
    text(c, 25, 49, "WAVETABLE \xC2\xB7 HYBRID", P.textFaint, 7.0f, true);
  }
  // page tabs
  static const char* const kTabs[PgCount] = {"OSC", "MIX", "FX", "MATRIX", "GLOBAL"};
  for (int i = 0; i < PgCount; ++i) {
    const float x = 165 + i * 68.0f;
    ImGui::PushID(i);
    ImGui::SetCursorScreenPos(c.p(x, 2));
    if (ImGui::InvisibleButton("##pg", c.F.sz(66, 64))) c.u.page = i, c.u.clip = false, c.u.wtEdit = false;
    const bool hov = ImGui::IsItemHovered();
    const bool on = c.u.page == i && !c.u.clip && !c.u.wtEdit;
    skin::gradientRect(c.dl, c.p(x, 2), c.p(x + 66, 66), on ? P.headOn : (hov ? P.head : P.panelHi), on ? P.panelHi : P.panel, c.s(3));
    if (on) c.dl->AddRect(c.p(x, 2), c.p(x + 66, 66), P.lineHi, c.s(3));
    c.dl->AddCircleFilled(c.p(x + 33, 20), c.s(2.6f), on ? P.accent : P.textFaint);
    if (on) c.dl->AddCircleFilled(c.p(x + 33, 20), c.s(6), theme::withAlpha(P.accent, 0.18f));
    textC(c, x + 33, 40, kTabs[i], on ? P.text : P.textDim, 10.5f, true);
    ImGui::PopID();
  }
  // preset row
  const float px = 512, pw = 470;
  panelRect(c, px - 4, 4, pw + 8, 60, P.panelLo, 3);
  {
    ImGui::SetCursorScreenPos(c.p(px, 8));
    if (ImGui::InvisibleButton("##brw", c.F.sz(20, 22))) c.u.browser = c.u.browser ? 0 : 1;
    if (ImGui::IsItemHovered()) c.a.setHint("Preset browser: Plasma presets and your Serum presets");
    iconBrowse(c.dl, c.p(px + 10, 19), c.s(11), ImGui::IsItemHovered() || c.u.browser ? P.accent : P.textDim);
    std::string name = c.m.presetName.empty() ? std::string("Init") : c.m.presetName;
    ImGui::SetCursorScreenPos(c.p(px + 24, 8));
    if (ImGui::InvisibleButton("##pn", c.F.sz(pw - 70, 22))) ImGui::OpenPopup("##presets");
    const bool hv = ImGui::IsItemHovered();
    c.dl->AddRectFilled(c.p(px + 24, 8), c.p(px + pw - 46, 30), hv ? P.panelHi : P.well, c.s(2));
    text(c, px + 31, 13, name.c_str(), P.accent, 10.5f, true);
    if (!c.m.openFormat.empty()) {  // opened from a Serum preset / state: its report on hover
      const float ix = px + pw - 66;
      const bool approx = !c.m.openNotes.empty() || !c.m.openMissing.empty();
      ImGui::SetCursorScreenPos(c.p(ix - 7, 12));
      ImGui::InvisibleButton("##openinfo", c.F.sz(14, 14));
      const bool ih = ImGui::IsItemHovered();
      const ImU32 col = ih ? P.text : (approx ? P.mod : P.textFaint);
      c.dl->AddCircle(c.p(ix, 19), c.s(5.5f), col, 16, std::max(1.0f, c.s(1.1f)));
      c.dl->AddLine(c.p(ix, 18), c.p(ix, 22.5f), col, std::max(1.0f, c.s(1.3f)));
      c.dl->AddCircleFilled(c.p(ix, 15.8f), c.s(0.9f), col);
      if (ih) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(c.s(420));
        ImGui::TextUnformatted(openReportText(c.m).c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
      }
    }
    iconArrow(c.dl, c.p(px + pw - 54, 19), c.s(7), 3, P.textDim);
    if (ImGui::BeginPopup("##presets")) {
      presetMenu(c);
      ImGui::EndPopup();
    }
    // previous / next factory preset
    for (int i = 0; i < 2; ++i) {
      ImGui::PushID(i);
      const float bx = px + pw - 42 + i * 21.0f;
      ImGui::SetCursorScreenPos(c.p(bx, 8));
      if (ImGui::InvisibleButton("##pv", c.F.sz(20, 22))) {
        const PluginInfo* in = info();
        if (in && !in->presets.empty()) {
          int cur = -1;
          for (size_t q = 0; q < in->presets.size(); ++q)
            if (in->presets[q].name == name) cur = static_cast<int>(q);
          const int n = static_cast<int>(in->presets.size());
          const int nx = ((cur < 0 ? 0 : cur) + (i == 0 ? -1 : 1) + n) % n;
          applyPreset(c, in->presets[static_cast<size_t>(nx)].values, "", in->presets[static_cast<size_t>(nx)].name);
        }
      }
      const bool h = ImGui::IsItemHovered();
      c.dl->AddRectFilled(c.p(bx, 8), c.p(bx + 20, 30), h ? P.headOn : P.head, c.s(2));
      iconArrow(c.dl, c.p(bx + 10, 19), c.s(8), i == 0 ? 0 : 1, h ? P.text : P.textDim);
      ImGui::PopID();
    }
    // author / description
    text(c, px + 2, 41, "AUTHOR", P.textFaint, 7.5f, true);
    text(c, px + 196, 41, "NOTES", P.textFaint, 7.5f, true);
    auto field = [&](const char* id, float x, float w, std::string& s, const char* label) {
      ImGui::SetCursorScreenPos(c.p(x, 36));
      ImGui::PushID(id);
      ImGui::PushStyleColor(ImGuiCol_FrameBg, theme::vec(P.well));
      ImGui::PushStyleColor(ImGuiCol_Text, theme::vec(P.textDim));
      ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(c.s(4), c.s(2)));
      ImGui::PushFont(c.a.fontRegular, font(c, 9.5f));
      ImGui::SetNextItemWidth(c.s(w));
      char buf[256];
      std::snprintf(buf, sizeof buf, "%s", s.c_str());
      if (ImGui::InputText("##f", buf, sizeof buf)) {
        s = buf;
        commit(c, label, 900 + static_cast<int>(id[0]));
      }
      ImGui::PopFont();
      ImGui::PopStyleVar();
      ImGui::PopStyleColor(2);
      ImGui::PopID();
    };
    field("a", px + 38, 150, c.m.author, "Edit author");
    field("d", px + 224, pw - 228, c.m.description, "Edit notes");
  }
  // undo / redo + menu
  {
    const float x = 990;
    for (int i = 0; i < 2; ++i) {
      ImGui::PushID(100 + i);
      ImGui::SetCursorScreenPos(c.p(x + i * 30.0f, 36));
      if (ImGui::InvisibleButton("##ur", c.F.sz(28, 26))) (void)(i == 0 ? c.a.doc.undo() : c.a.doc.redo());
      const bool h = ImGui::IsItemHovered();
      c.dl->AddRectFilled(c.p(x + i * 30.0f, 36), c.p(x + i * 30.0f + 28, 62), h ? P.headOn : P.head, c.s(3));
      // The SVG undo / redo in the old arc-and-arrow glyph's ink (a ring of 5.5 with an arrow head above it).
      const ImVec2 q = c.p(x + i * 30.0f + 14, 49);
      ::gl::ui::drawIconFit(c.dl, i == 0 ? ::gl::ui::Icon::Undo : ::gl::ui::Icon::Redo, ImVec2(q.x - c.s(6.2f), q.y - c.s(7)),
                            ImVec2(q.x + c.s(6.2f), q.y + c.s(6.2f)), h ? P.text : P.textDim);
      if (h) c.a.setHint(i == 0 ? "Undo" : "Redo");
      ImGui::PopID();
    }
    ImGui::SetCursorScreenPos(c.p(x, 6));
    if (ImGui::InvisibleButton("##menu", c.F.sz(58, 26))) ImGui::OpenPopup("##mainmenu");
    const bool h = ImGui::IsItemHovered();
    c.dl->AddRectFilled(c.p(x, 6), c.p(x + 58, 32), h ? P.headOn : P.head, c.s(3));
    textC(c, x + 29, 13, "MENU", h ? P.text : P.textDim, 9, true);
    if (ImGui::BeginPopup("##mainmenu")) {
      presetMenu(c);
      ImGui::Separator();
      if (ImGui::MenuItem(i18n::tr("Edit wavetable (Osc A)"))) c.u.wtEdit = true, c.u.wtOsc = 0;
      if (ImGui::MenuItem(i18n::tr("Clip editor"), nullptr, c.u.clip)) c.u.clip = !c.u.clip;
      if (ImGui::MenuItem(i18n::tr("Arpeggiator panel"), nullptr, c.u.arp)) c.u.arp = !c.u.arp;
      ImGui::Separator();
      if (ImGui::MenuItem(i18n::tr("Clear the modulation matrix"))) {
        for (int s = 0; s < kModSlots; ++s)
          for (int q = 0; q < M_Count; ++q)
            if (const ParamDesc* d = desc(modP(s, q))) setv(c, modP(s, q), d->def);
      }
      if (ImGui::MenuItem(i18n::tr("Remove all effects"))) {
        for (int s = 0; s < kFxSlots; ++s) setv(c, fxP(s, X_Type), FxNone);
        c.m.fx.clear();
        commit(c, "Remove all effects");
      }
      ImGui::EndPopup();
    }
  }
  // main volume + meter
  knob(c, G_MasterVol, 1110, 26, 15, "MAIN");
  drawMeter(c, 1142, 8, 7, 54);
  drawMeter(c, 1152, 8, 7, 54);
  vline(c, 0, 0, kHeadH, P.line);
  hline(c, 0, kW, kHeadH - 0.5f, P.line);
}

// ================================================================== bottom strip
void wheel(Ctx& c, int k, float x, float y, float w, float h, bool spring) {
  ImGui::PushID(k);
  ImGui::SetCursorScreenPos(c.p(x, y));
  ImGui::InvisibleButton("##wh", c.F.sz(w, h));
  const bool act = ImGui::IsItemActive(), hov = ImGui::IsItemHovered();
  const ParamDesc* d = desc(k);
  double v = val(c, k);
  if (act && d) {
    const double n = std::clamp(1.0 - (ImGui::GetIO().MousePos.y - c.p(0, y).y) / c.s(h), 0.0, 1.0);
    v = d->fromNormalized(n);
    setv(c, k, v, true);
  } else if (spring && ImGui::IsItemDeactivated() && d) {
    setv(c, k, d->def, true);
  }
  if (ImGui::IsItemDeactivated()) c.a.doc.endGesture();
  const double n = d ? d->toNormalized(v) : 0;
  const ImVec2 a = c.p(x, y), b = c.p(x + w, y + h);
  skin::gradientRect(c.dl, a, b, c.P.panelLo, c.P.well, c.s(4));
  c.dl->AddRect(a, b, c.P.line, c.s(4));
  const float ky = b.y - c.s(4) - static_cast<float>(n) * (b.y - a.y - c.s(8));
  skin::gradientRect(c.dl, ImVec2(a.x + c.s(2), ky - c.s(4)), ImVec2(b.x - c.s(2), ky + c.s(4)), c.P.capTop, c.P.capBot, c.s(2));
  c.dl->AddLine(ImVec2(a.x + c.s(4), ky), ImVec2(b.x - c.s(4), ky), act || hov ? c.P.accent : c.P.textDim, std::max(1.0f, c.s(1.2f)));
  if (hov) c.a.setHint(paramLongName(k) + ": " + ptext(c, k));
  ImGui::PopID();
}

void keyboard(Ctx& c, float x, float y, float w, float h) {
  const int lo = 36, hi = 96;  // C2 .. C7
  int whites = 0;
  for (int k = lo; k <= hi; ++k) {
    const int pc = k % 12;
    if (pc != 1 && pc != 3 && pc != 6 && pc != 8 && pc != 10) ++whites;
  }
  const float kw = w / whites;
  const ImVec2 m = ImGui::GetIO().MousePos;
  ImGui::SetCursorScreenPos(c.p(x, y));
  ImGui::InvisibleButton("##kb", c.F.sz(w, h));
  const bool act = ImGui::IsItemActive(), hov = ImGui::IsItemHovered();
  int hit = -1;
  // black keys first (on top)
  auto isBlack = [](int k) {
    const int pc = k % 12;
    return pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
  };
  std::vector<std::pair<int, ImVec4>> rects;
  int wi = 0;
  for (int k = lo; k <= hi; ++k) {
    if (isBlack(k)) continue;
    rects.push_back({k, ImVec4(x + wi * kw, y, x + (wi + 1) * kw, y + h)});
    ++wi;
  }
  std::vector<std::pair<int, ImVec4>> blacks;
  wi = 0;
  for (int k = lo; k <= hi; ++k) {
    if (isBlack(k)) {
      const float bx = x + wi * kw - kw * 0.32f;
      blacks.push_back({k, ImVec4(bx, y, bx + kw * 0.64f, y + h * 0.6f)});
    } else {
      ++wi;
    }
  }
  if (hov || act) {
    for (const auto& [k, r] : blacks)
      if (hit < 0 && m.x >= c.p(r.x, 0).x && m.x < c.p(r.z, 0).x && m.y >= c.p(0, r.y).y && m.y < c.p(0, r.w).y) hit = k;
    for (const auto& [k, r] : rects)
      if (hit < 0 && m.x >= c.p(r.x, 0).x && m.x < c.p(r.z, 0).x) hit = k;
  }
  if (act && hit >= 0 && hit != c.u.heldKey) {
    if (c.u.heldKey >= 0) previewKey(c, c.u.heldKey, false);
    previewKey(c, hit, true);
    c.u.heldKey = hit;
  }
  if (!act && c.u.heldKey >= 0) {
    previewKey(c, c.u.heldKey, false);
    c.u.heldKey = -1;
  }
  for (const auto& [k, r] : rects) {
    const bool down = k == c.u.heldKey;
    const ImVec2 a = c.p(r.x, r.y), b = c.p(r.z, r.w);
    skin::gradientRect(c.dl, ImVec2(a.x + 0.5f, a.y), ImVec2(b.x - 0.5f, b.y), down ? c.P.keyDown : c.P.keyWhite,
                       down ? c.P.keyDown : theme::mix(c.P.keyWhite, c.P.textFaint, 0.18f), c.s(2), ImDrawFlags_RoundCornersBottom);
    if (k % 12 == 0 && k == 60) textC(c, (r.x + r.z) * 0.5f, r.w - 12, "C3", c.P.textFaint, 7, true);
  }
  for (const auto& [k, r] : blacks) {
    const bool down = k == c.u.heldKey;
    const ImVec2 a = c.p(r.x, r.y), b = c.p(r.z, r.w);
    skin::gradientRect(c.dl, a, b, down ? c.P.keyDown : skin::lighten(c.P.keyBlack, 0.12f), down ? c.P.keyDown : c.P.keyBlack, c.s(1.5f),
                       ImDrawFlags_RoundCornersBottom);
  }
  if (hov) c.a.setHint("Click to audition");
}

void strip(Ctx& c) {
  const Pal& P = c.P;
  const float y = kStripY;
  panelRect(c, 0, y, kW, kH - y, P.frame);
  // groups, each in its own frame: wheels | CLIP | ARP | transpose + keys | portamento
  const float fh = kH - y;
  frameBox(c, 0, y, 164, fh);
  frameBox(c, 164, y, 113, fh);
  frameBox(c, 277, y, 114, fh);
  frameBox(c, 391, y, 650, fh);
  frameBox(c, 1041, y, 149, fh);
  // wheels + bend ranges
  wheel(c, G_PitchWheel, 14, y + 10, 18, 66, true);
  wheel(c, G_ModWheel, 130, y + 10, 18, 66, false);
  stepper(c, G_BendUp, 48, y + 12, 66, 20);
  stepper(c, G_BendDown, 48, y + 38, 66, 20);
  textC(c, 81, y + 64, "BEND", P.label, 8, true);
  // CLIP / ARP
  auto block = [&](float x, const char* name, int onParam, bool& open, bool isClip) {
    ImGui::PushID(name);
    // power
    ImGui::SetCursorScreenPos(c.p(x + 4, y + 7));
    if (ImGui::InvisibleButton("##pw", c.F.sz(18, 18))) setv(c, onParam, val(c, onParam) >= 0.5 ? 0 : 1);
    const bool on = val(c, onParam) >= 0.5;
    iconPower(c.dl, c.p(x + 13, y + 16), c.s(5), on ? P.accent : P.textDim);
    // title (opens the editor)
    ImGui::SetCursorScreenPos(c.p(x + 24, y + 6));
    if (ImGui::InvisibleButton("##ti", c.F.sz(82, 20))) {
      open = !open;
      if (isClip) c.u.wtEdit = false;
    }
    const bool hv = ImGui::IsItemHovered();
    c.dl->AddRectFilled(c.p(x + 24, y + 6), c.p(x + 105, y + 26), open ? P.accentSoft : (hv ? P.headOn : P.head), c.s(2));
    text(c, x + 30, y + 10, name, open ? P.accent : (on ? P.text : P.textDim), 9.5f, true);
    if (hv) c.a.setHint(std::string(isClip ? "Clip player" : "Arpeggiator") + ": click the name to open its editor, the power button to switch it on");
    // mini keys
    for (int i = 0; i < 7; ++i) {
      const float kx = x + 6 + i * 14.0f;
      c.dl->AddRectFilled(c.p(kx, y + 32), c.p(kx + 12.5f, y + 76), P.keyWhite, c.s(1.5f));
    }
    for (int i : {0, 1, 3, 4, 5}) {
      const float kx = x + 6 + i * 14.0f + 9;
      c.dl->AddRectFilled(c.p(kx, y + 32), c.p(kx + 8, y + 56), P.keyBlack, c.s(1));
    }
    if (on) c.dl->AddRectFilled(c.p(x + 6, y + 29), c.p(x + 103, y + 31), P.accent, c.s(1));
    ImGui::PopID();
  };
  block(165, "CLIP", G_ClipOn, c.u.clip, true);
  block(278, "ARP", G_ArpOn, c.u.arp, false);
  // transpose / key / scale / swing
  {
    const float x = 396;
    text(c, x + 4, y + 10, "TRANSPOSE", P.textDim, 7.5f, true);
    stepper(c, G_Transpose, x + 54, y + 6, 46, 18);
    text(c, x + 108, y + 10, "KEY", P.textDim, 7.5f, true);
    choiceBox(c, G_Key, x + 128, y + 6, 48, 18);
    text(c, x + 184, y + 10, "SCALE", P.textDim, 7.5f, true);
    choiceBox(c, G_Scale, x + 214, y + 6, 98, 18);
    text(c, x + 320, y + 10, "SWING", P.textDim, 7.5f, true);
    hfader(c, G_Swing, x + 352, y + 8, 62, 14);
    choiceBox(c, G_SwingDiv, x + 418, y + 6, 46, 18);
    text(c, x + 472, y + 10, "OCTAVE", P.textDim, 7.5f, true);
    choiceBox(c, G_TrigOctave, x + 506, y + 6, 46, 18);
    if (ImGui::IsItemHovered()) c.a.setHint("Trigger octave: its twelve keys pick the clip / arp slot while playing");
    if (button(c, "oscmap", x + 556, y + 6, 81, 18, "", c.u.mapping, 7.5f)) c.u.mapping = !c.u.mapping;
    if (ImGui::IsItemHovered()) c.a.setHint("Oscillator mapping: the key and velocity range of each oscillator");
    text(c, x + 562, y + 10, "OSC MAPPING", c.u.mapping ? P.accent : P.textDim, 7.5f, true);
    iconPencil(c.dl, c.p(x + 628, y + 15), c.s(8), c.u.mapping ? P.accent : P.textDim);
    keyboard(c, x, y + 29, 641, 51);
  }
  // portamento
  {
    const float x = 1046;
    checkBox(c, G_PortaAlways, x + 6, y + 9, "ALWAYS", 7.5f);
    checkBox(c, G_PortaScaled, x + 74, y + 9, "SCALED", 7.5f);
    knob(c, G_PortaTime, x + 34, y + 45, 14, "PORTA");
    // curve: a small graph that bends with the value
    const float gx = x + 76, gy = y + 28, gw = 50, gh = 32;
    wellRect(c, gx, gy, gw, gh, 2);
    const float cv = valf(c, G_PortaCurve);
    ImVec2 prev;
    for (int i = 0; i <= 24; ++i) {
      const float t = i / 24.f;
      const float yv = static_cast<float>(bendCurve(t, cv));
      const ImVec2 q = c.p(gx + 3 + t * (gw - 6), gy + gh - 3 - yv * (gh - 6));
      if (i) c.dl->AddLine(prev, q, P.accent, std::max(1.2f, c.s(1.4f)));
      prev = q;
    }
    ImGui::SetCursorScreenPos(c.p(gx, gy));
    ImGui::InvisibleButton("##pc", c.F.sz(gw, gh), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    double pv = val(c, G_PortaCurve);
    interact(c, G_PortaCurve, pv, 120, false);
    cellLabel(c, gx + gw * 0.5f, gy + gh + 3.5f, "CURVE");  // J-128: on the PORTA label line
  }
}

// Per-instance editor state.
Ui& uiFor(const PluginTarget& t) {
  static std::map<uint64_t, Ui> m;
  auto it = m.find(t.key());
  if (it != m.end()) return it->second;
  Ui u;
  if (const char* e = std::getenv("GL_PLASMA_PAGE")) u.page = std::clamp(std::atoi(e), 0, PgCount - 1);  // dev: screenshots
  if (const char* e = std::getenv("GL_PLASMA_PRESET")) u.devPreset = e;  // dev: screenshots
  if (const char* e = std::getenv("GL_PLASMA_VIEW")) {
    const std::string v = e;
    u.arp = v == "arp";
    u.clip = v == "clip";
    u.wtEdit = v == "wt";
    u.browser = v == "browser" ? 1 : 0;
    if (v.rfind("lfo", 0) == 0) u.lfoTab = std::clamp(std::atoi(v.c_str() + 3), 0, kLfos - 1);
    if (v.rfind("env", 0) == 0) u.envTab = std::clamp(std::atoi(v.c_str() + 3), 0, kEnvs - 1);
    u.mapping = v == "map";
  }
  return m[t.key()] = u;
}

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

// ================================================================== entry point
bool drawEditor(App& a, const PluginTarget& t, float width) {
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
  if (!u.devPreset.empty()) {  // dev (screenshots): load a factory preset by name once
    const std::string want = u.devPreset;
    u.devPreset.clear();
    if (const PluginInfo* in = info())
      for (const auto& p : in->presets)
        if (p.name == want) applyPreset(c, p.values, "", p.name);
    for (const auto& p : factoryDataPresets())
      if (p.name == want) applyPreset(c, p.values, p.data, p.name);
  }
  panelRect(c, 0, 0, kW, kH, c.P.frame);
  header(c);
  if (u.browser) {
    // the browser covers the page and the lower panel: they are not drawn (nor clickable) under it
  } else if (u.clip) {
    clipEditor(c);
  } else if (u.wtEdit) {
    wtEditor(c);
  } else {
    panelRect(c, 0, kPageY, kW, kLowerY - kPageY, c.P.frame);  // modules sit in frames: the gutters show this
    switch (u.page) {
      case PgOsc: oscPage(c); break;
      case PgMix: mixPage(c); break;
      case PgFx: fxPage(c); break;
      case PgMatrix: matrixPage(c); break;
      default: globalPage(c); break;
    }
    if (!(u.page == PgMatrix && u.mxExpanded)) {
      lowerPanel(c);
    }
  }
  strip(c);
  layoutLayer(1);
  if (u.mapping) mappingPanel(c);
  layoutLayer(2);
  if (u.browser) presetBrowser(c);
  layoutReport(c);
  c.dl->AddRect(F.o, F.p(kW, kH), c.P.line, 0);
  F.end(kW, kH);
  ImGui::PopID();
  return true;
}

}  // namespace gl::packs::plasma
