// Gridline - J-129 direction D, "Slate": the studio slate colour roles the
// interface uses today (slate panels, dark fields, lit orange, green LEDs, light
// knob rings), rebuilt with ONE finish for everything: a single light direction
// (top), 1 px bevels on the pixel grid, the same radius family and the same
// shadow on every raised part. The safe evolution.
// Also the "Current" painter: today's shared drawing functions, for comparison.
#include <cmath>
#include <cstdio>

#include "LookPaint.h"
#include "PluginSkin.h"
#include "Theme.h"
#include "Widgets.h"

namespace gl::look {
namespace {

// Built from the live palette: dark and light themes, the user's accent and
// role colours, contrast and brightness all carry through.
Tokens slateTokens() {
  const theme::Palette& P = theme::pal();
  const bool dk = P.dark;
  Tokens t;
  t.panelTop = lighten(P.surface800, dk ? 0.04f : 0.03f), t.panelBot = mix(P.surface800, P.surface900, 0.6f);
  t.panelLine = dk ? darken(P.surface950, 0.3f) : P.lineStrong;
  t.section = mix(P.surface900, P.surface950, 0.4f), t.sectionLine = dk ? darken(P.surface950, 0.15f) : P.line, t.sectionTitle = P.textDim;
  t.well = P.well, t.wellLine = dk ? darken(P.well, 0.25f) : P.line;
  t.grid = alpha(P.text, 0.07f), t.gridStrong = alpha(P.text, 0.13f);
  t.face = P.surface700, t.faceHi = P.surface600, t.faceLo = mix(P.surface700, P.surface800, 0.5f);
  t.faceLine = dk ? darken(P.surface950, 0.15f) : P.lineStrong;
  t.field = P.field, t.fieldText = P.hot;
  t.text = P.text, t.textDim = P.textDim, t.textFaint = P.textFaint;
  t.value = P.knobFill, t.valueHi = lighten(P.knobFill, 0.4f);
  t.mod = rgb(0x4ECCFF), t.hot = P.hot, t.led = P.led, t.danger = P.danger;
  t.meterLow = P.meterLow, t.meterMid = P.meterMid, t.meterHigh = P.meterHigh;
  t.rail = dk ? mix(P.well, P.surface950, 0.5f) : darken(P.surface800, 0.08f);
  t.radius = 3, t.radiusSm = 2, t.radiusLg = 5;
  t.shadow = dk ? 0.8f : 0.45f, t.glow = 0.35f, t.line = 1;
  return t;
}

class Slate : public Painter {
 public:
  Slate() : Painter(slateTokens()) {}
  // Slate keeps each plugin's own palette and gives it the one finish.
  void hookPanel(ImDrawList* dl, ImVec2 p0, ImVec2 p1, ImU32 hint) override {
    if (!hint) return panel(dl, p0, p1);
    gradRect(dl, p0, p1, lighten(hint, 0.06f), darken(hint, 0.06f), tk.radiusLg);
    dl->AddLine(ImVec2(p0.x + tk.radiusLg, p0.y + 0.5f), ImVec2(p1.x - tk.radiusLg, p0.y + 0.5f), alpha(IM_COL32_WHITE, 0.14f), 1.0f);
    dl->AddRect(p0, p1, darken(hint, 0.5f), tk.radiusLg);
  }
  void hookSection(ImDrawList* dl, ImVec2 p0, ImVec2 p1, ImU32 hint) override { sunken(dl, p0, p1, tk.radius, hint ? hint : tk.section); }
  void hookWell(ImDrawList* dl, ImVec2 p0, ImVec2 p1, ImU32 hint) override { sunken(dl, p0, p1, tk.radius, hint ? hint : tk.well); }
  void hookInset(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float r, ImU32 hint) override { sunken(dl, p0, p1, std::min(r, tk.radius), hint ? hint : tk.field); }
  void hookRaised(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float r, ImU32 hint, int state) override {
    if (!hint) return raised(dl, p0, p1, std::min(r, tk.radius), state);
    const float rr = std::min(r, tk.radius);
    shadowRect(dl, p0, p1, rr, 2.5f, 1.0f, 0.8f);
    gradRect(dl, p0, p1, lighten(hint, 0.09f), darken(hint, 0.07f), rr);
    dl->AddLine(ImVec2(p0.x + rr, p0.y + 0.5f), ImVec2(p1.x - rr, p0.y + 0.5f), alpha(IM_COL32_WHITE, 0.16f), 1.0f);
    dl->AddRect(p0, p1, darken(hint, 0.5f), rr);
  }

  void knob(ImDrawList* dl, ImVec2 c, float r, const Knob& k) override {
    const bool dis = k.state & Disabled, act = k.state & Active, hov = k.state & Hover;
    const float th = std::max(2.5f, r * 0.17f);
    const float rr = r - th * 0.5f;
    const float from = k.bipolar ? angleOf(0.5f) : kA0, an = angleOf(k.norm);
    const ImU32 val = dis ? tk.textFaint : act ? tk.hot : roleOr(k.tint.value, hov ? lighten(tk.value, 0.15f) : tk.value);
    // Ring: a dark rail with a 1 px light lip below (same bevel as every field).
    arc(dl, ImVec2(c.x, c.y + 1), rr, kA0, kA1, alpha(IM_COL32_WHITE, 0.08f), th);
    arc(dl, c, rr, kA0, kA1, tk.rail, th);
    if (std::fabs(an - from) > 0.002f) arc(dl, c, rr, std::min(from, an), std::max(from, an), val, th - 0.5f);
    else if (k.bipolar) dl->AddCircleFilled(ImVec2(c.x + std::cos(an) * rr, c.y + std::sin(an) * rr), th * 0.4f, val, 12);
    if (k.modDepth != 0) {
      const float m0 = angleOf(k.norm), m1 = angleOf(k.norm + k.modDepth);
      arc(dl, c, r + 2.5f, std::min(m0, m1), std::max(m0, m1), tk.mod, 2.0f);
    }
    if (k.modNow >= 0) {
      const float a = angleOf(k.modNow);
      const ImVec2 q(c.x + std::cos(a) * (r + 2.5f), c.y + std::sin(a) * (r + 2.5f));
      dl->AddCircleFilled(q, 3.0f, tk.panelLine, 12);
      dl->AddCircleFilled(q, 2.2f, lighten(tk.mod, 0.3f), 12);
    }
    // Cap: bevelled, light from the top.
    const float capR = r - th - std::max(1.5f, r * 0.08f);
    shadowCircle(dl, c, capR, 2.5f, 1.2f, 0.9f);
    ImU32 hi = lighten(tk.faceHi, 0.1f), lo = darken(tk.faceLo, 0.12f);
    if (hov) hi = lighten(hi, 0.07f);
    if (dis) hi = darken(hi, 0.25f), lo = darken(lo, 0.2f);
    gradCircle(dl, c, capR, hi, lo);
    dl->PathArcTo(c, capR - 0.7f, kPi * 1.1f, kPi * 1.9f, segs(capR, kPi));
    dl->PathStroke(alpha(IM_COL32_WHITE, 0.22f), 0, 1.0f);
    dl->AddCircle(c, capR, tk.faceLine, segs(capR), 1.0f);
    if (k.level > 0) dl->AddCircle(c, capR * 0.5f, alpha(tk.led, 0.3f + 0.7f * k.level), segs(capR), 1.5f);
    const ImVec2 d(std::cos(an), std::sin(an));
    const float lw = std::clamp(capR * 0.14f, 1.5f, 3.0f);
    const ImVec2 q0(c.x + d.x * capR * 0.3f, c.y + d.y * capR * 0.3f), q1(c.x + d.x * capR * 0.86f, c.y + d.y * capR * 0.86f);
    dl->PathLineTo(q0), dl->PathLineTo(q1), dl->PathStroke(dis ? tk.textFaint : act ? rgb(0xFFFFFF) : tk.text, 0, lw);
  }
  void button(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* lbl, int state, ImU32 role) override {
    const bool on = state & On;
    if (on) {
      const ImU32 c = roleOr(role, tk.hot);
      sunken(dl, p0, p1, tk.radius, mix(tk.field, c, 0.22f));
      dl->AddRectFilled(ImVec2(p0.x + 4, p1.y - 3), ImVec2(p1.x - 4, p1.y - 1.5f), c, 1);
      textIn(dl, Font::Bold, 13, p0, p1, c, lbl);
      return;
    }
    raised(dl, p0, p1, tk.radius, state);
    textIn(dl, Font::Body, 13, ImVec2(p0.x, p0.y + ((state & Active) ? 1.0f : 0.0f)), ImVec2(p1.x, p1.y + ((state & Active) ? 1.0f : 0.0f)),
           (state & Disabled) ? tk.textFaint : tk.text, lbl);
  }
  void stepPad(ImDrawList* dl, ImVec2 p0, ImVec2 p1, bool on, bool alt, float vel, bool playing, int state, ImU32 tint) override {
    const theme::Palette& T = theme::pal();  // the step roles of the theme (or the plugin's skin)
    if (on) {
      const ImU32 c = roleOr(tint, T.stepOn);
      shadowRect(dl, p0, p1, tk.radiusSm, 2, 1, 0.8f);
      gradRect(dl, p0, p1, mix(lighten(T.stepOff, 0.08f), lighten(c, 0.1f), 0.35f + 0.65f * vel), mix(T.stepOff, c, 0.25f + 0.6f * vel), tk.radiusSm);
      dl->AddLine(ImVec2(p0.x + 2, p0.y + 0.5f), ImVec2(p1.x - 2, p0.y + 0.5f), alpha(IM_COL32_WHITE, 0.45f), 1.0f);
    } else {
      const ImU32 b = alt ? T.stepOffAlt : T.stepOff;
      shadowRect(dl, p0, p1, tk.radiusSm, 2, 1, 0.6f);
      gradRect(dl, p0, p1, lighten(b, (state & Hover) ? 0.14f : 0.07f), darken(b, 0.08f), tk.radiusSm);
      dl->AddLine(ImVec2(p0.x + 2, p0.y + 0.5f), ImVec2(p1.x - 2, p0.y + 0.5f), alpha(IM_COL32_WHITE, 0.14f), 1.0f);
    }
    dl->AddRect(p0, p1, tk.faceLine, tk.radiusSm);
    if (playing) dl->AddRectFilled(ImVec2(p0.x + 3, p1.y + 2), ImVec2(p1.x - 3, p1.y + 4), lighten(tk.led, 0.2f), 1);
  }
  void section(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* title) override {
    sunken(dl, p0, p1, tk.radius, tk.section);
    if (title) {
      const float tw = textSize(Font::MonoBold, 9.5f, title).x * 1.2f + 18;
      dl->AddRectFilled(ImVec2(p0.x + 6, p0.y + 4), ImVec2(p0.x + 6 + tw, p0.y + 18), darken(tk.section, 0.25f), 7);
      caps(dl, Font::MonoBold, 9.5f, ImVec2(p0.x + 6, p0.y + 4), ImVec2(p0.x + 6 + tw, p0.y + 18), tk.sectionTitle, title, 1.0f, 1);
    }
  }
  void field(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* txt, int state, int caret, bool lcd) override {
    Painter::field(dl, p0, p1, txt, state, caret, lcd);
  }
};

// ------------------------------------------------------------------ Current
// Draws with the shared functions the interface uses today, so the gallery has
// a baseline to compare the directions with.
class Current : public Painter {
 public:
  Current() : Painter(slateTokens()) {
    const theme::Palette& p = theme::pal();
    tk.panelTop = p.surface800, tk.panelBot = p.surface900, tk.section = p.well, tk.field = p.field, tk.well = p.field;
    tk.text = p.text, tk.textDim = p.textDim, tk.textFaint = p.textFaint, tk.hot = p.hot, tk.led = p.led, tk.value = p.knobFill;
    tk.faceHi = p.surface700, tk.faceLo = p.surface800, tk.faceLine = p.line;
  }
  skin::Skin sk() const {
    skin::Skin k = skin::Skin::fl(0x3F484D, 0x30373B, 0x262B2F, 0xCFD8DD, 0x4F595E, 0xD5DBDF);
    return k;
  }
  void raised(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float r, int state) override {
    skin::raisedFace(dl, p0, p1, (state & Hover) ? tk.faceHi : tk.faceLo, r);
  }
  void sunken(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float r, ImU32 fill) override { skin::insetField(dl, p0, p1, fill, r); }
  void knob(ImDrawList* dl, ImVec2 c, float r, const Knob& k) override {
    skin::KnobPaint kp;
    const theme::Palette& p = theme::pal();
    kp.rail = p.surface900, kp.fill = (k.state & (Hover | Active)) ? p.accent : p.paper, kp.cap = p.surface700, kp.pointer = p.text;
    if (k.state & Disabled) kp.fill = p.textFaint;
    kp.a0 = kA0, kp.a1 = kA1, kp.from = k.bipolar ? angleOf(0.5f) : kA0, kp.an = angleOf(k.norm);
    kp.hot = k.state & Hover, kp.active = k.state & Active;
    skin::knobFace(dl, c, r, kp);
    if (k.modNow >= 0) dl->AddCircleFilled(ImVec2(c.x + std::cos(angleOf(k.modNow)) * r, c.y + std::sin(angleOf(k.modNow)) * r), 3, p.accent, 12);
  }
  void meter(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const Meter& m) override {
    if (!m.vertical) {
      Painter::meter(dl, p0, p1, m);
      return;
    }
    const float db = 20.0f * std::log10(std::max(1e-4f, m.l)) * 0.0f + (m.l - 1.0f) * 48.0f;
    skin::meter(dl, p0, p1, sk(), db, 48.0f, m.reduction);
  }
  void led(ImDrawList* dl, ImVec2 c, float r, bool on, float level, ImU32 role, int state) override {
    (void)role, (void)state;
    ui::Led(dl, c, r, on, level);
  }
  void stepPad(ImDrawList* dl, ImVec2 p0, ImVec2 p1, bool on, bool alt, float vel, bool playing, int state, ImU32 tint) override {
    ui::StepPad(dl, p0, p1, on, alt, state & Hover, tint, vel);
    if (playing) dl->AddRect(p0, p1, theme::pal().playhead, 2, 0, 1.5f);
  }
  void field(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* txt, int state, int caret, bool lcd) override {
    if (lcd) ui::Lcd(dl, p0, ImVec2(p1.x - p0.x, p1.y - p0.y), txt, state & Hover, state & Active);
    else Painter::field(dl, p0, p1, txt, state, caret, lcd);
  }
  void curve(ImDrawList* dl, const ImVec2* pts, int n, float baseY, ImU32 col, bool fill) override {
    (void)fill;
    skin::curveArea(dl, pts, n, baseY, roleOr(col, theme::pal().accent));
  }
  void well(ImDrawList* dl, ImVec2 p0, ImVec2 p1) override { skin::graphWell(dl, p0, p1, sk()); }
};

}  // namespace

Painter* makeSlate() { return new Slate(); }
Painter* makeClassic() { return new Current(); }

}  // namespace gl::look
