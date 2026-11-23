// Gridline - 0.7.8: the Tracker layout's control faces as a control style (ui::ControlStyle::Tracker).
// Never a setting: the Tracker's Sample editor draws the Audio clip window's parts inside a
// ControlStyleScope of it, so their shared controls (buttons, segmented choices, icon buttons,
// knobs, drop-downs, fields, checks) take the Tracker's own pieces (TrackerLook.h): square corners,
// two-step bevels in the theme's roles (a raised face lit top and left, a sunken field the other way),
// hard pixel edges, the lit state as the band's lit buttons; knobs stay round. Sizes and hit boxes never change.
#include <algorithm>
#include <cmath>

#include "LookPaint.h"
#include "Theme.h"
#include "TrackerLook.h"
#include "Widgets.h"

namespace gl::look {
namespace {

Tokens trackerTokens() {
  const theme::Palette& P = theme::pal();
  Tokens t;
  t.panelTop = t.panelBot = P.surface700;
  t.panelLine = theme::mix(P.surface950, IM_COL32(0, 0, 0, 255), 0.35f);
  t.section = P.surface950, t.sectionLine = t.panelLine, t.sectionTitle = P.textDim;
  t.well = P.surface950, t.wellLine = t.panelLine;
  t.grid = alpha(P.text, 0.07f), t.gridStrong = alpha(P.text, 0.14f);
  t.face = P.surface700, t.faceHi = P.surface700, t.faceLo = P.surface700, t.faceLine = t.panelLine;
  t.field = P.surface950, t.fieldText = trackerview::valueInk();
  t.text = P.text, t.textDim = P.textDim, t.textFaint = P.textFaint;
  t.value = P.hot, t.valueHi = lighten(P.hot, 0.3f);
  t.mod = rgb(0x4ECCFF), t.hot = P.hot, t.led = P.led, t.danger = P.danger;
  t.meterLow = P.meterLow, t.meterMid = P.meterMid, t.meterHigh = P.meterHigh;
  t.rail = mix(P.surface950, P.surface700, 0.5f);
  t.radius = 0, t.radiusSm = 0, t.radiusLg = 0;
  t.shadow = 0, t.glow = 0, t.line = 1;
  return t;
}

class Tracker : public Painter {
 public:
  Tracker() : Painter(trackerTokens()) {}

  // The band's bevel box: raised, or (held) pressed in; hover lightens the face as its buttons do.
  void raised(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float r, int state) override {
    (void)r;
    const theme::Palette& P = theme::pal();
    trackerview::bevelBox(dl, p0, p1, (state & Active) != 0);
    if ((state & Hover) && !(state & Active)) dl->AddRectFilled(ImVec2(p0.x + 2, p0.y + 2), ImVec2(p1.x - 2, p1.y - 2), theme::withAlpha(P.surface600, 0.6f));
    if (state & Disabled) dl->AddRectFilled(p0, p1, theme::withAlpha(P.surface800, 0.45f));
  }
  void sunken(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float r, ImU32 fill) override {
    (void)r;
    trackerview::bevelBox(dl, p0, p1, true);
    if (fill && fill != tk.field) dl->AddRectFilled(ImVec2(p0.x + 1, p0.y + 1), ImVec2(p1.x - 1, p1.y - 1), fill);
  }
  // Every face is a box, at rest too (the band draws no flat buttons). Lit: pressed in with the hot
  // colour under the label (the label keeps its own lit ink, so it stays readable).
  void hookFace(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float r, int state, ImU32 role, bool rest) override {
    (void)rest;
    if (state & Primary) return primary(dl, p0, p1, r, state & ~Primary, role);
    const theme::Palette& P = theme::pal();
    if (state & On) {
      trackerview::bevelBox(dl, p0, p1, true);
      dl->AddRectFilled(ImVec2(p0.x + 1, p0.y + 1), ImVec2(p1.x - 1, p1.y - 1), theme::mix(P.surface950, roleOr(role, P.hot), (state & Hover) ? 0.3f : 0.22f));
      return;
    }
    raised(dl, p0, p1, 0, state);
  }
  void primary(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float r, int state, ImU32 role) override {
    (void)r;
    const theme::Palette& P = theme::pal();
    trackerview::bevelBox(dl, p0, p1, (state & Active) != 0);
    dl->AddRectFilled(ImVec2(p0.x + 1, p0.y + 1), ImVec2(p1.x - 1, p1.y - 1), theme::withAlpha(roleOr(role, P.hot), (state & Disabled) ? 0.4f : 0.85f));
  }
  void hookPanel(ImDrawList* dl, ImVec2 p0, ImVec2 p1, ImU32 hint) override { (void)hint, trackerview::bevelBox(dl, p0, p1, false); }
  void hookSection(ImDrawList* dl, ImVec2 p0, ImVec2 p1, ImU32 hint) override { (void)hint, trackerview::bevelBox(dl, p0, p1, true); }
  void hookWell(ImDrawList* dl, ImVec2 p0, ImVec2 p1, ImU32 hint) override { (void)hint, trackerview::bevelBox(dl, p0, p1, true); }
  void hookInset(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float r, ImU32 hint) override { (void)r, (void)hint, trackerview::bevelBox(dl, p0, p1, true); }
  void hookRaised(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float r, ImU32 hint, int state) override { (void)hint, raised(dl, p0, p1, r, state); }
  void panel(ImDrawList* dl, ImVec2 p0, ImVec2 p1) override { trackerview::bevelBox(dl, p0, p1, false); }
  void section(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* title) override {
    trackerview::bevelBox(dl, p0, p1, false);
    if (title) textIn(dl, Font::Bold, 12, ImVec2(p0.x + 6, p0.y + 2), ImVec2(p1.x, p0.y + 18), tk.text, title, 0);
  }
  void well(ImDrawList* dl, ImVec2 p0, ImVec2 p1) override { trackerview::bevelBox(dl, p0, p1, true); }

  // A bright bevel edge as an arc whose ends fade out: drawn in short pieces, each piece's alpha a
  // smoothstep up from 0 over the first `edge` of the sweep and back down over the last, so the lit
  // line never stops on a hard end.
  static void fadeArc(ImDrawList* dl, ImVec2 c, float r, float a0, float a1, ImU32 col, float th, float edge = 0.35f) {
    const float sweep = a1 - a0;
    if (sweep < 0.0005f) return;
    const int n = std::max(8, static_cast<int>(sweep / 0.05f));
    for (int i = 0; i < n; ++i) {
      const float t0 = static_cast<float>(i) / static_cast<float>(n), t1 = static_cast<float>(i + 1) / static_cast<float>(n);
      const float s = std::clamp(std::min(t0 + t1, 2.0f - t0 - t1) * 0.5f / edge, 0.0f, 1.0f);
      const float a = s * s * (3.0f - 2.0f * s);
      if (a <= 0.004f) continue;
      dl->PathArcTo(c, r, a0 + sweep * t0, a0 + sweep * t1, 2);
      dl->PathStroke(theme::withAlpha(col, a), 0, th);
    }
  }

  // A knob, round as every knob is, old-school: a sunken round well (shaded on the top and left, the
  // shadow fading out round the sides, and a lit crescent at the bottom right whose ends fade out),
  // the travel in it as an arc with the value in the hot colour, and a raised round cap inside it with
  // a dark outline and the band's bevel (shaded all round, lit top and left with the lit edge fading
  // into the shade at both ends) and a pointer line. Anti-aliased: round shapes drawn on hard pixels
  // only look jagged.
  void knob(ImDrawList* dl, ImVec2 c, float r, const Knob& k) override {
    const theme::Palette& P = theme::pal();
    const bool dis = k.state & Disabled, act = k.state & Active, hov = (k.state & Hover) && !act;
    // The bevel's lit edge goes toward white in light (the ink is dark there, and an ink-toned "lit"
    // edge read as a second dark ring round the cap).
    const ImU32 lit = P.dark ? theme::mix(P.surface700, P.text, 0.42f) : theme::mix(P.surface700, P.paper, 0.75f), shade = theme::mix(P.surface950, IM_COL32(0, 0, 0, 255), 0.35f);
    // The bevel ring: the lit colour over the lit half (top-left, or bottom-right when pressed; screen
    // angles, up is 1.5 pi), the shaded colour over the other, each fading out at its ends so the two
    // cross-fade where they meet and neither stops on a hard end.
    auto ring = [&](float rad, ImU32 litCol, ImU32 shadeCol, bool litTopLeft, float w) {
      const float l0 = litTopLeft ? kPi * 0.6f : kPi * 1.6f, s0 = litTopLeft ? kPi * 1.6f : kPi * 0.6f;
      fadeArc(dl, c, rad, s0, s0 + kPi * 1.3f, shadeCol, w, 0.25f);
      fadeArc(dl, c, rad, l0, l0 + kPi * 1.3f, litCol, w, 0.25f);
    };
    // The well: a step darker than the panel in both themes (in light the plain darkest surface sits
    // too close to the panel), its shadow on the top-left half fading to nothing round the sides.
    const ImU32 well = P.dark ? P.surface950 : theme::mix(P.surface950, P.text, 0.14f);
    dl->AddCircleFilled(c, r, well, segs(r));
    const ImU32 wShade = P.dark ? shade : theme::mix(well, IM_COL32(0, 0, 0, 255), 0.3f), clear = theme::withAlpha(wShade, 0.0f);
    arc(dl, c, r - 0.5f, kPi * 0.9f, kPi * 1.6f, wShade, 1.0f, false);
    arcGrad(dl, c, r - 0.5f, kPi * 0.5f, kPi * 0.9f, clear, wShade, 1.0f);
    arcGrad(dl, c, r - 0.5f, kPi * 1.6f, kPi * 2.0f, wShade, clear, 1.0f);
    // The well's lit crescent at the bottom right, brightest at the bottom-right point, its ends fading out.
    const ImU32 wLit = P.dark ? theme::mix(P.surface700, P.text, 0.34f) : theme::mix(well, P.paper, 0.7f);
    fadeArc(dl, c, r - 0.5f, kPi * 1.68f, kPi * 2.82f, wLit, 1.0f, 0.35f);
    const float th = std::max(2.0f, std::round(r * 0.15f)), rr = r - 2.0f - th * 0.5f;
    // The travel shows in the well even where the value has not reached (the rail token is too close to
    // the well's own colour to read there).
    arc(dl, c, rr, kA0, kA1, theme::mix(well, P.text, P.dark ? 0.16f : 0.12f), th, false);
    const float from = k.bipolar ? angleOf(0.5f) : kA0, an = angleOf(k.norm);
    const ImU32 val = dis ? P.textFaint : roleOr(k.tint.value, trackerview::valueInk());
    if (std::fabs(an - from) > 0.002f) arc(dl, c, rr, std::min(from, an), std::max(from, an), act ? lighten(val, 0.15f) : val, th, false);
    if (k.modDepth != 0) {
      const float m0 = angleOf(k.norm), m1 = angleOf(k.norm + k.modDepth);
      arc(dl, c, rr - th * 0.5f - 1.0f, std::min(m0, m1), std::max(m0, m1), roleOr(k.tint.mod, tk.mod), 1.5f, false);
    }
    if (k.modNow >= 0) {
      const float m = angleOf(k.modNow);
      dl->AddCircleFilled(ImVec2(c.x + std::cos(m) * rr, c.y + std::sin(m) * rr), th * 0.6f, roleOr(k.tint.mod, tk.mod), 12);
    }
    // The cap: a raised disc a step inside the travel, its face a touch lighter at the top.
    const float cap = std::max(4.0f, rr - th * 0.5f - 2.0f);
    const ImU32 face = hov ? theme::mix(P.surface700, P.surface600, 0.6f) : P.surface700;
    dl->AddCircleFilled(c, cap + 1.0f, shade, segs(cap + 1.0f));
    gradCircle(dl, c, cap, theme::mix(face, P.text, act ? 0.02f : 0.08f), theme::mix(face, P.surface950, act ? 0.1f : 0.25f));
    ring(cap - 0.5f, lit, theme::mix(P.surface700, P.surface950, 0.55f), !act, 1.0f);
    if (k.level >= 0) dl->AddCircle(c, cap * 0.5f, alpha(tk.meterLow, 0.25f + 0.75f * k.level), segs(cap), 2.0f);
    const ImVec2 d(std::cos(an), std::sin(an));
    dl->AddLine(ImVec2(c.x + d.x * cap * 0.2f, c.y + d.y * cap * 0.2f), ImVec2(c.x + d.x * (cap - 2.0f), c.y + d.y * (cap - 2.0f)), dis ? P.textFaint : P.text,
                std::max(2.0f, std::round(cap * 0.14f)));
  }
  void button(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* lbl, int state, ImU32 role) override {
    const theme::Palette& P = theme::pal();
    const bool on = state & On;
    hookFace(dl, p0, p1, 0, state, role, true);
    const ImVec2 d = (state & Active) ? ImVec2(1, 1) : ImVec2(0, 0);
    textIn(dl, Font::Body, 13, ImVec2(p0.x + d.x, p0.y + d.y), ImVec2(p1.x + d.x, p1.y + d.y),
           (state & Disabled) ? P.textFaint : on ? roleOr(role, P.hot) : P.text, lbl);
  }
  void iconButton(ImDrawList* dl, ImVec2 p0, ImVec2 p1, int icon, int state, ImU32 role) override {
    const theme::Palette& P = theme::pal();
    hookFace(dl, p0, p1, 0, state, role, true);
    const ImU32 col = (state & Disabled) ? P.textFaint : (state & On) ? roleOr(role, P.hot) : P.text;
    ui::drawIcon(dl, static_cast<ui::Icon>(icon), ImVec2((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f), (p1.y - p0.y) * 0.56f, col);
  }
  void toggle(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float t, int state, ImU32 role) override {
    const float s = p1.y - p0.y;
    check(dl, p0, ImVec2(p0.x + s, p1.y), nullptr, (t > 0.5f ? On : 0) | (state & ~On), role);
  }
  // The band's check row: a sunken square, a hard tick when on.
  void check(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* lbl, int state, ImU32 role) override {
    const theme::Palette& P = theme::pal();
    const trackerview::Aliased hard(dl);
    const float s = std::round(std::min(p1.y - p0.y, 14.0f));
    const ImVec2 b0(std::round(p0.x), std::round((p0.y + p1.y - s) * 0.5f)), b1(b0.x + s, b0.y + s);
    trackerview::bevelBox(dl, b0, b1, true);
    if (state & On) {
      const ImU32 c = roleOr(role, trackerview::valueInk());
      const ImVec2 q[3] = {ImVec2(b0.x + s * 0.22f, b0.y + s * 0.5f), ImVec2(b0.x + s * 0.42f, b0.y + s * 0.72f), ImVec2(b0.x + s * 0.8f, b0.y + s * 0.26f)};
      dl->AddPolyline(q, 3, c, 0, 2.0f);
    }
    if (lbl) textIn(dl, Font::Body, 13, ImVec2(b1.x + 6, p0.y), p1, (state & Disabled) ? P.textFaint : P.text, lbl, 0);
  }
  // One of n: a sunken trough, the chosen one a lit box.
  void segmented(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* const* labels, int n, int sel, int hover, int state) override {
    const theme::Palette& P = theme::pal();
    trackerview::bevelBox(dl, p0, p1, true);
    const float w = (p1.x - p0.x) / static_cast<float>(std::max(1, n));
    for (int i = 0; i < n; ++i) {
      const ImVec2 a(std::round(p0.x + w * static_cast<float>(i)), p0.y), b(std::round(a.x + w), p1.y);
      if (i == sel) hookFace(dl, ImVec2(a.x + 1, a.y + 1), ImVec2(b.x - 1, b.y - 1), 0, On | (state & Disabled), 0, true);
      else trackerview::bevelBox(dl, ImVec2(a.x + 1, a.y + 1), ImVec2(b.x - 1, b.y - 1), false);
      textIn(dl, Font::Body, 12, a, b, i == sel ? P.hot : i == hover ? P.text : P.textDim, labels[i]);
    }
  }
  void dropdown(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* txt, int state) override {
    const theme::Palette& P = theme::pal();
    trackerview::bevelBox(dl, p0, p1, true);
    textIn(dl, Font::Mono, 13, ImVec2(p0.x + 6, p0.y), ImVec2(p1.x - 20, p1.y), (state & Disabled) ? P.textFaint : trackerview::valueInk(), txt, 0);
    const float bw = p1.y - p0.y - 2;
    trackerview::bevelBox(dl, ImVec2(p1.x - bw - 1, p0.y + 1), ImVec2(p1.x - 1, p1.y - 1), (state & Active) != 0);
    ui::drawIcon(dl, ui::Icon::ChevronDown, ImVec2(p1.x - 1 - bw * 0.5f, (p0.y + p1.y) * 0.5f), 9, P.text);
  }
  void field(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* txt, int state, int caret, bool lcd) override {
    (void)caret, (void)lcd;
    const theme::Palette& P = theme::pal();
    trackerview::bevelBox(dl, p0, p1, true);
    if (txt) textIn(dl, Font::Mono, 13, ImVec2(p0.x + 4, p0.y), ImVec2(p1.x - 4, p1.y), (state & Disabled) ? P.textFaint : trackerview::valueInk(), txt, 1);
  }
};

}  // namespace

Painter* makeTracker() { return new Tracker(); }

}  // namespace gl::look
