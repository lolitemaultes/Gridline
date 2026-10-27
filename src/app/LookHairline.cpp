// Gridline - J-129 direction B, "Hairline": flat precision. No material, no
// shadows: 1 px hairlines on a near-black slate, values as one clean accent
// stroke, numbers in JetBrains Mono, labels in spaced small caps. Premium by
// restraint and alignment (everything on the pixel grid); live data shows as
// thin second strokes and ticks, never glows.
#include <cstdio>

#include "LookPaint.h"
#include "Widgets.h"

namespace gl::look {
namespace {

Tokens hairlineTokens() {
  Tokens t;
  t.panelTop = rgb(0x17191B), t.panelBot = rgb(0x17191B), t.panelLine = rgb(0x2A2E31);
  t.section = rgb(0x17191B), t.sectionLine = rgb(0x2C3033), t.sectionTitle = rgb(0x7E868B);
  t.well = rgb(0x121415), t.wellLine = rgb(0x2C3033), t.grid = rgb(0xFFFFFF, 0.05f), t.gridStrong = rgb(0xFFFFFF, 0.1f);
  t.face = rgb(0x1E2124), t.faceHi = rgb(0x1E2124), t.faceLo = rgb(0x1E2124), t.faceLine = rgb(0x3A3F43);
  t.field = rgb(0x121415), t.fieldText = rgb(0xECEFF1);
  t.text = rgb(0xE4E8EA), t.textDim = rgb(0x8E979C), t.textFaint = rgb(0x555C61);
  const theme::Palette& P = theme::pal();  // the one accent is the user's accent
  t.value = P.accent, t.valueHi = lighten(P.accent, 0.2f);
  t.mod = rgb(0x4ECCFF), t.hot = P.accent, t.led = P.led, t.danger = P.danger;
  t.meterLow = rgb(0xA8E44A), t.meterMid = rgb(0xF2D04A), t.meterHigh = rgb(0xFF5B53);
  t.rail = rgb(0x33383B);
  t.radius = 3, t.radiusSm = 2, t.radiusLg = 4;
  t.shadow = 0, t.glow = 0, t.line = 1;
  return t;
}

class Hairline : public Painter {
 public:
  Hairline() : Painter(hairlineTokens()) {}
  ImU32 hookTint(ImU32) override { return 0; }  // one accent everywhere

  void raised(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float r, int state) override {
    ImU32 f = tk.face;
    if (state & Hover) f = lighten(f, 0.04f);
    if (state & Active) f = lighten(f, 0.08f);
    dl->AddRectFilled(p0, p1, f, r);
    dl->AddRect(snap(p0), ImVec2(std::floor(p1.x) - 0.5f, std::floor(p1.y) - 0.5f), (state & Disabled) ? alpha(tk.faceLine, 0.5f) : (state & (Hover | Active)) ? lighten(tk.faceLine, 0.2f) : tk.faceLine, r);
  }
  void sunken(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float r, ImU32 fill) override {
    dl->AddRectFilled(p0, p1, fill, r);
    dl->AddRect(snap(p0), ImVec2(std::floor(p1.x) - 0.5f, std::floor(p1.y) - 0.5f), tk.sectionLine, r);
  }

  void knob(ImDrawList* dl, ImVec2 c, float r, const Knob& k) override {
    const bool dis = k.state & Disabled, act = k.state & Active, hov = k.state & Hover;
    const float from = k.bipolar ? angleOf(0.5f) : kA0, an = angleOf(k.norm);
    const ImU32 val = dis ? tk.textFaint : roleOr(k.tint.value, tk.value);
    const float th = act ? 2.5f : 2.0f;
    const float rr = r - 2;
    arc(dl, c, rr, kA0, kA1, hov || act ? lighten(tk.rail, 0.15f) : tk.rail, 1.0f, false);
    // End ticks of the sweep.
    for (float a : {kA0, kA1}) dl->AddLine(ImVec2(c.x + std::cos(a) * (rr - 3), c.y + std::sin(a) * (rr - 3)), ImVec2(c.x + std::cos(a) * (rr + 2), c.y + std::sin(a) * (rr + 2)), tk.rail, 1.0f);
    if (k.bipolar) dl->AddLine(ImVec2(c.x, c.y - rr - 3), ImVec2(c.x, c.y - rr + 3), tk.textFaint, 1.0f);
    if (std::fabs(an - from) > 0.002f) arc(dl, c, rr, std::min(from, an), std::max(from, an), val, th, false);
    if (k.modDepth != 0) {
      const float m0 = angleOf(k.norm), m1 = angleOf(k.norm + k.modDepth);
      arc(dl, c, rr + 4, std::min(m0, m1), std::max(m0, m1), roleOr(k.tint.mod, tk.mod), 1.0f, false);
    }
    if (k.modNow >= 0) {
      const float a = angleOf(k.modNow);
      dl->AddLine(ImVec2(c.x + std::cos(a) * (rr - 3), c.y + std::sin(a) * (rr - 3)), ImVec2(c.x + std::cos(a) * (rr + 5), c.y + std::sin(a) * (rr + 5)), tk.mod, 2.0f);
    }
    // Body: a quiet disc and the pointer from the centre.
    const float br = rr - 5;
    dl->AddCircleFilled(c, br, hov || act ? rgb(0x24282B) : rgb(0x1E2124), segs(br));
    dl->AddCircle(c, br, alpha(tk.faceLine, dis ? 0.5f : 1.0f), segs(br), 1.0f);
    if (k.level > 0) dl->AddCircle(c, br * 0.45f, alpha(tk.led, 0.3f + 0.7f * k.level), segs(br), 1.0f + k.level);
    const ImVec2 d(std::cos(an), std::sin(an));
    dl->AddLine(ImVec2(c.x + d.x * br * 0.25f, c.y + d.y * br * 0.25f), ImVec2(c.x + d.x * br, c.y + d.y * br), dis ? tk.textFaint : act ? rgb(0xFFFFFF) : tk.text, 1.5f);
  }

  void slider(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const Slider& s) override {
    const bool v = s.vertical;
    const ImVec2 c(std::floor((p0.x + p1.x) * 0.5f) + 0.5f, std::floor((p0.y + p1.y) * 0.5f) + 0.5f);
    const float a = v ? p1.y - 6 : p0.x + 6, b = v ? p0.y + 6 : p1.x - 6;
    auto pos = [&](float n) { return a + (b - a) * std::clamp(n, 0.0f, 1.0f); };
    auto pt = [&](float q) { return v ? ImVec2(c.x, q) : ImVec2(q, c.y); };
    dl->AddLine(pt(a), pt(b), (s.state & Hover) ? lighten(tk.rail, 0.15f) : tk.rail, 1.0f);
    const float o = pos(s.bipolar ? 0.5f : 0.0f), q = pos(s.norm);
    const ImU32 val = (s.state & Disabled) ? tk.textFaint : roleOr(s.tint.value, tk.value);
    dl->AddLine(pt(o), pt(q), val, (s.state & Active) ? 3.0f : 2.0f);
    if (s.bipolar) dl->AddLine(v ? ImVec2(c.x - 4, o) : ImVec2(o, c.y - 4), v ? ImVec2(c.x + 4, o) : ImVec2(o, c.y + 4), tk.textFaint, 1.0f);
    if (s.modDepth != 0) {
      const float m = pos(s.norm + s.modDepth);
      dl->AddLine(v ? ImVec2(c.x + 5, q) : ImVec2(q, c.y + 5), v ? ImVec2(c.x + 5, m) : ImVec2(m, c.y + 5), tk.mod, 1.0f);
    }
    if (s.modNow >= 0) {
      const float m = pos(s.modNow);
      dl->AddLine(v ? ImVec2(c.x + 2, m) : ImVec2(m, c.y + 2), v ? ImVec2(c.x + 8, m) : ImVec2(m, c.y + 8), tk.mod, 2.0f);
    }
    // Handle: a short bar across the line.
    const ImU32 hc = (s.state & Disabled) ? tk.textFaint : (s.state & Active) ? rgb(0xFFFFFF) : tk.text;
    // J-139: 13 px across, centred on the half-pixel line.
    if (v) dl->AddRectFilled(ImVec2(c.x - 6.5f, q - 1), ImVec2(c.x + 6.5f, q + 1), hc);
    else dl->AddRectFilled(ImVec2(q - 1, c.y - 6.5f), ImVec2(q + 1, c.y + 6.5f), hc);
  }

  void fader(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const Fader& f) override {
    // J-139: centred on the control (moved left only to make room for a meter), one centre line (LookGeom.h).
    const bool hasMeter = f.meterL >= 0;
    const geom::FaderGeom g = geom::fader(p0.x, p0.y, p1.x, p1.y, f.norm, geom::kHairlineFader, hasMeter);
    const float tx = g.tx;  // a half pixel: the 1 px line is crisp
    const float ti = geom::kHairlineFader.tickIn;
    for (int i = 0; i <= 20; ++i) {
      const float y = g.tickY(static_cast<float>(i) / 20.0f);
      const bool major = i % 4 == 0;
      const float to = g.tickOut(major ? geom::kHairlineFader.tickOut : ti + 3);
      const ImU32 c = i == 16 ? tk.text : major ? tk.textDim : tk.textFaint;
      dl->AddLine(ImVec2(tx - to, y), ImVec2(tx - ti, y), c, 1.0f);
      if (g.ticksRight) dl->AddLine(ImVec2(tx + ti, y), ImVec2(tx + to, y), c, 1.0f);
    }
    dl->AddLine(ImVec2(tx, g.y0), ImVec2(tx, g.y1), tk.rail, 1.0f);
    if (hasMeter) {
      Meter m;
      m.l = f.meterL, m.r = f.meterR, m.peakL = f.peakL, m.peakR = f.peakR;
      meter(dl, ImVec2(tx + 12, g.y0), ImVec2(tx + 19, g.y1), m);
    }
    const float y = g.y;
    dl->AddLine(ImVec2(tx, y), ImVec2(tx, g.y1), (f.state & Active) ? tk.value : tk.textDim, 2.0f);
    const ImVec2 h0(g.capX0, g.capY0), h1(g.capX1, g.capY1);
    dl->AddRectFilled(h0, h1, tk.panelBot, 2);
    dl->AddRect(h0, h1, (f.state & (Hover | Active)) ? tk.text : tk.textDim, 2);
    dl->AddLine(ImVec2(h0.x + 3, y), ImVec2(h1.x - 3, y), (f.state & Active) ? tk.value : tk.text, 1.0f);
  }

  void meter(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const Meter& m) override {
    const bool stereo = m.r >= 0;
    const int nch = stereo ? 2 : 1;
    const float W = m.vertical ? p1.x - p0.x : p1.y - p0.y;
    const float gap = 1;
    const float cw = std::max(1.0f, std::floor((W - gap * static_cast<float>(nch - 1)) / static_cast<float>(nch)));
    for (int ch = 0; ch < nch; ++ch) {
      const float lv = std::clamp(ch ? m.r : m.l, 0.0f, 1.0f), pk = ch ? m.peakR : m.peakL, rms = ch ? m.rmsR : m.rmsL;
      ImVec2 a, b;
      if (m.vertical) a = ImVec2(p0.x + (cw + gap) * static_cast<float>(ch), p0.y), b = ImVec2(a.x + cw, p1.y);
      else a = ImVec2(p0.x, p0.y + (cw + gap) * static_cast<float>(ch)), b = ImVec2(p1.x, a.y + cw);
      dl->AddRectFilled(a, b, rgb(0x1E2124));
      const float L = m.vertical ? b.y - a.y : b.x - a.x;
      if (m.reduction) {
        if (m.vertical) dl->AddRectFilled(a, ImVec2(b.x, a.y + L * lv), tk.danger);
        else dl->AddRectFilled(ImVec2(b.x - L * lv, a.y), b, tk.danger);
        continue;
      }
      // Three flat zones; RMS as the solid body, peak as a lighter body behind it.
      auto zone = [&](float z0, float z1, ImU32 col, float v, float al) {
        const float e0 = z0, e1 = std::min(z1, v);
        if (e1 <= e0) return;
        if (m.vertical) dl->AddRectFilled(ImVec2(a.x, b.y - L * e1), ImVec2(b.x, b.y - L * e0), alpha(col, al));
        else dl->AddRectFilled(ImVec2(a.x + L * e0, a.y), ImVec2(a.x + L * e1, b.y), alpha(col, al));
      };
      const float body = rms >= 0 ? rms : lv;
      if (rms >= 0) zone(0, 0.75f, tk.meterLow, lv, 0.35f), zone(0.75f, 0.92f, tk.meterMid, lv, 0.35f), zone(0.92f, 1, tk.meterHigh, lv, 0.35f);
      zone(0, 0.75f, tk.meterLow, body, 1), zone(0.75f, 0.92f, tk.meterMid, body, 1), zone(0.92f, 1, tk.meterHigh, body, 1);
      if (pk >= 0) {
        const float pp = L * std::clamp(pk, 0.0f, 1.0f);
        if (m.vertical) dl->AddLine(ImVec2(a.x, std::round(b.y - pp) + 0.5f), ImVec2(b.x, std::round(b.y - pp) + 0.5f), tk.text, 1.0f);
        else dl->AddLine(ImVec2(std::round(a.x + pp) + 0.5f, a.y), ImVec2(std::round(a.x + pp) + 0.5f, b.y), tk.text, 1.0f);
      }
    }
    if (m.clip) {
      if (m.vertical) dl->AddRectFilled(ImVec2(p0.x, p0.y - 4), ImVec2(p1.x, p0.y - 2), tk.danger);
      else dl->AddRectFilled(ImVec2(p1.x + 2, p0.y), ImVec2(p1.x + 4, p1.y), tk.danger);
    }
  }

  void led(ImDrawList* dl, ImVec2 c, float r, bool on, float level, ImU32 role, int state) override {
    const ImU32 col = roleOr(role, tk.led);
    if (on) dl->AddCircleFilled(c, r, col, 16);
    else if (level > 0) dl->AddCircleFilled(c, r, alpha(col, 0.25f + 0.75f * level), 16);
    else dl->AddCircle(c, r - 0.5f, alpha(col, 0.55f), 16, 1.0f);
    (void)state;
  }

  void button(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* lbl, int state, ImU32 role) override {
    const bool on = state & On;
    const ImU32 acc = roleOr(role, tk.hot);
    raised(dl, p0, p1, tk.radius, state);
    if (on) {
      dl->AddRectFilled(p0, p1, alpha(acc, 0.12f), tk.radius);
      dl->AddRect(snap(p0), ImVec2(std::floor(p1.x) - 0.5f, std::floor(p1.y) - 0.5f), acc, tk.radius);
    }
    textIn(dl, Font::Body, 13, p0, p1, (state & Disabled) ? tk.textFaint : on ? acc : tk.text, lbl);
  }
  void iconButton(ImDrawList* dl, ImVec2 p0, ImVec2 p1, int icon, int state, ImU32 role) override {
    const bool on = state & On;
    if (state & (Hover | Active)) dl->AddRectFilled(p0, p1, alpha(IM_COL32_WHITE, (state & Active) ? 0.08f : 0.04f), tk.radius);
    const ImU32 col = (state & Disabled) ? tk.textFaint : on ? roleOr(role, tk.hot) : (state & Hover) ? tk.text : tk.textDim;
    ui::drawIcon(dl, static_cast<ui::Icon>(icon), ImVec2((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f), (p1.y - p0.y) * 0.6f, col);
    if (on) dl->AddLine(ImVec2(p0.x + 6, p1.y - 0.5f), ImVec2(p1.x - 6, p1.y - 0.5f), roleOr(role, tk.hot), 1.0f);
  }
  void toggle(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float t, int state, ImU32 role) override {
    const float h = p1.y - p0.y, r = h * 0.5f;
    const ImU32 acc = roleOr(role, tk.hot);
    dl->AddRectFilled(p0, p1, alpha(acc, t), r);
    dl->AddRect(p0, p1, mix((state & Hover) ? lighten(tk.faceLine, 0.25f) : tk.faceLine, acc, t), r);
    const ImVec2 c(p0.x + r + (p1.x - p0.x - h) * t, p0.y + r);
    dl->AddCircleFilled(c, r - 3, mix(tk.textDim, rgb(0x17191B), t), 16);
  }
  void check(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* lbl, int state, ImU32 role) override {
    const float s = p1.y - p0.y;
    const ImVec2 b0 = snap(p0), b1(b0.x + s - 1, b0.y + s - 1);
    const ImU32 acc = roleOr(role, tk.hot);
    if (state & On) {
      dl->AddRectFilled(ImVec2(b0.x - 0.5f, b0.y - 0.5f), ImVec2(b1.x + 0.5f, b1.y + 0.5f), acc, tk.radiusSm);
      const ImVec2 q[3] = {ImVec2(b0.x + s * 0.25f, b0.y + s * 0.5f), ImVec2(b0.x + s * 0.42f, b0.y + s * 0.66f), ImVec2(b0.x + s * 0.72f, b0.y + s * 0.32f)};
      dl->AddPolyline(q, 3, rgb(0x17191B), 1.6f, 0);
    } else {
      dl->AddRect(b0, b1, (state & Hover) ? tk.textDim : tk.faceLine, tk.radiusSm);
    }
    if (lbl) textIn(dl, Font::Body, 13, ImVec2(p0.x + s + 8, p0.y), p1, (state & Disabled) ? tk.textFaint : tk.text, lbl, 0);
  }
  void segmented(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* const* labels, int n, int sel, int hover, int state) override {
    const float w = (p1.x - p0.x) / static_cast<float>(n);
    dl->AddRect(snap(p0), ImVec2(std::floor(p1.x) - 0.5f, std::floor(p1.y) - 0.5f), tk.faceLine, tk.radius);
    for (int i = 0; i < n; ++i) {
      const ImVec2 a(p0.x + w * static_cast<float>(i), p0.y), b(a.x + w, p1.y);
      if (i > 0) dl->AddLine(ImVec2(std::floor(a.x) + 0.5f, a.y + 5), ImVec2(std::floor(a.x) + 0.5f, b.y - 5), tk.faceLine, 1.0f);
      if (i == sel) dl->AddRectFilled(ImVec2(a.x + 1, a.y + 1), ImVec2(b.x - 1, b.y - 1), alpha(IM_COL32_WHITE, 0.07f), tk.radiusSm);
      if (i == sel) dl->AddLine(ImVec2(a.x + 8, b.y - 2.5f), ImVec2(b.x - 8, b.y - 2.5f), tk.hot, 2.0f);
      const ImU32 tc = (state & Disabled) ? tk.textFaint : i == sel ? tk.text : i == hover ? tk.text : tk.textDim;
      textIn(dl, Font::Body, 12, a, b, tc, labels[i]);
    }
  }
  void tabs(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* const* labels, int n, int sel, int hover) override {
    dl->AddLine(ImVec2(p0.x, std::floor(p1.y) - 0.5f), ImVec2(p1.x, std::floor(p1.y) - 0.5f), tk.sectionLine, 1.0f);
    float x = p0.x;
    for (int i = 0; i < n; ++i) {
      const float w = textSize(Font::Body, 13, labels[i]).x + 24;
      if (i == sel) dl->AddRectFilled(ImVec2(x + 6, p1.y - 2), ImVec2(x + w - 6, p1.y), tk.hot);
      textIn(dl, Font::Body, 13, ImVec2(x, p0.y), ImVec2(x + w, p1.y - 2), i == sel ? tk.text : i == hover ? tk.text : tk.textDim, labels[i]);
      x += w;
    }
  }
  void dropdown(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* txt, int state) override {
    const ImU32 line = (state & (Active | Focus)) ? tk.hot : (state & Hover) ? tk.textDim : tk.faceLine;
    dl->AddRect(snap(p0), ImVec2(std::floor(p1.x) - 0.5f, std::floor(p1.y) - 0.5f), line, tk.radius);
    textIn(dl, Font::Body, 13, ImVec2(p0.x + 9, p0.y), ImVec2(p1.x - 22, p1.y), (state & Disabled) ? tk.textFaint : tk.text, txt, 0);
    ui::drawIcon(dl, ui::Icon::ChevronDown, ImVec2(p1.x - 12, (p0.y + p1.y) * 0.5f), 9, tk.textDim);
  }
  void menu(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* const* items, int n, int hover, int checked) override {
    dl->AddRectFilled(p0, p1, rgb(0x1E2124), tk.radius);
    dl->AddRect(snap(p0), ImVec2(std::floor(p1.x) - 0.5f, std::floor(p1.y) - 0.5f), tk.faceLine, tk.radius);
    const float rh = (p1.y - p0.y - 8) / static_cast<float>(n);
    for (int i = 0; i < n; ++i) {
      const ImVec2 a(p0.x + 4, p0.y + 4 + rh * static_cast<float>(i)), b(p1.x - 4, a.y + rh);
      if (i == hover) dl->AddRectFilled(a, b, alpha(IM_COL32_WHITE, 0.06f), tk.radiusSm);
      if (i == checked) dl->AddRectFilled(ImVec2(a.x + 6, (a.y + b.y) * 0.5f - 2), ImVec2(a.x + 10, (a.y + b.y) * 0.5f + 2), tk.hot);
      textIn(dl, Font::Body, 13, ImVec2(a.x + 20, a.y), b, i == hover ? tk.text : tk.textDim, items[i], 0);
    }
  }
  void field(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* txt, int state, int caret, bool lcd) override {
    if (lcd) dl->AddRectFilled(p0, p1, tk.field, tk.radius);
    const ImU32 line = (state & (Active | Focus)) ? tk.hot : (state & Hover) ? tk.textDim : tk.faceLine;
    if (lcd) dl->AddRect(snap(p0), ImVec2(std::floor(p1.x) - 0.5f, std::floor(p1.y) - 0.5f), line, tk.radius);
    else dl->AddLine(ImVec2(p0.x, std::floor(p1.y) - 0.5f), ImVec2(p1.x, std::floor(p1.y) - 0.5f), line, 1.0f);
    const Font f = lcd ? Font::Mono : Font::Mono;
    const float fs = lcd ? std::min(17.0f, (p1.y - p0.y) * 0.6f) : 13;
    textIn(dl, f, fs, p0, p1, (state & Disabled) ? tk.textFaint : tk.text, txt);
    if (caret >= 0) {
      const ImVec2 ts = textSize(f, fs, txt);
      char pre[64];
      std::snprintf(pre, sizeof pre, "%.*s", caret, txt);
      const float x = std::floor((p0.x + p1.x - ts.x) * 0.5f + textSize(f, fs, pre).x) + 1.5f;
      dl->AddLine(ImVec2(x, p0.y + 4), ImVec2(x, p1.y - 4), tk.hot, 1.0f);
    }
  }
  void stepPad(ImDrawList* dl, ImVec2 p0, ImVec2 p1, bool on, bool alt, float vel, bool playing, int state, ImU32 tint) override {
    const ImU32 c = roleOr(tint, tk.text);
    if (on) {
      dl->AddRectFilled(p0, p1, alpha(c, 0.25f + 0.75f * vel), tk.radiusSm);
    } else {
      dl->AddRectFilled(p0, p1, alt ? rgb(0x22262A) : rgb(0x1C1F22), tk.radiusSm);
      if (state & Hover) dl->AddRect(snap(p0), ImVec2(std::floor(p1.x) - 0.5f, std::floor(p1.y) - 0.5f), tk.textDim, tk.radiusSm);
    }
    if (playing) dl->AddRectFilled(ImVec2(p0.x + 2, p1.y - 3), ImVec2(p1.x - 2, p1.y - 1), tk.hot);  // J-137: inside the pad
  }
  void pad(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* lbl, float glow, int state, ImU32 tint) override {
    const ImU32 c = roleOr(tint, tk.hot);
    dl->AddRectFilled(p0, p1, mix(rgb(0x1E2124), c, 0.85f * glow), tk.radius);
    dl->AddRect(snap(p0), ImVec2(std::floor(p1.x) - 0.5f, std::floor(p1.y) - 0.5f), (state & Hover) ? tk.textDim : tk.faceLine, tk.radius);
    caps(dl, Font::MonoBold, 9.5f, ImVec2(p0.x + 8, p1.y - 20), ImVec2(p1.x - 8, p1.y - 6), glow > 0.5f ? rgb(0x17191B) : tk.textDim, lbl, 1.0f, 0);
  }
  void panel(ImDrawList* dl, ImVec2 p0, ImVec2 p1) override {
    dl->AddRectFilled(p0, p1, tk.panelTop, tk.radiusLg);
    dl->AddRect(snap(p0), ImVec2(std::floor(p1.x) - 0.5f, std::floor(p1.y) - 0.5f), tk.panelLine, tk.radiusLg);
  }
  void section(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* title) override {
    dl->AddRect(snap(p0), ImVec2(std::floor(p1.x) - 0.5f, std::floor(p1.y) - 0.5f), tk.sectionLine, tk.radius);
    if (title) {
      const float tw = textSize(Font::MonoBold, 9.5f, title).x * 1.25f + 16;
      dl->AddRectFilled(ImVec2(p0.x + 8, p0.y - 1), ImVec2(p0.x + 8 + tw, p0.y + 2), tk.panelTop);
      caps(dl, Font::MonoBold, 9.5f, ImVec2(p0.x + 14, p0.y - 6), ImVec2(p0.x + tw, p0.y + 6), tk.sectionTitle, title, 1.4f, 0);
    }
  }
  void header(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* title, const char* sub) override {
    dl->AddLine(ImVec2(p0.x, std::floor(p1.y) - 0.5f), ImVec2(p1.x, std::floor(p1.y) - 0.5f), tk.sectionLine, 1.0f);
    const float cy = (p0.y + p1.y) * 0.5f;
    dl->AddRectFilled(ImVec2(p0.x + 14, cy - 7), ImVec2(p0.x + 16, cy + 7), tk.hot);
    text(dl, Font::Bold, 15, ImVec2(p0.x + 24, cy - 9), tk.text, title);
    if (sub) textIn(dl, Font::Body, 12, p0, ImVec2(p1.x - 14, p1.y), tk.textDim, sub, 2);
  }
  void well(ImDrawList* dl, ImVec2 p0, ImVec2 p1) override {
    dl->AddRectFilled(p0, p1, tk.well, tk.radius);
    dl->AddRect(snap(p0), ImVec2(std::floor(p1.x) - 0.5f, std::floor(p1.y) - 0.5f), tk.wellLine, tk.radius);
  }
  void caption(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* t) override { caps(dl, Font::MonoBold, 9.5f, p0, p1, tk.textDim, t, 1.4f, 0); }
  void tooltip(ImDrawList* dl, ImVec2 pos, const char* title, const char* value) override {
    // Inverted chip: Paper on the dark UI, the one bright surface.
    const ImVec2 ts = textSize(Font::Body, 12, title), vs = textSize(Font::Mono, 13, value);
    const ImVec2 p1(pos.x + ts.x + vs.x + 30, pos.y + 24);
    dl->AddRectFilled(pos, p1, rgb(0xECEFF1), tk.radius);
    text(dl, Font::Body, 12, ImVec2(pos.x + 9, pos.y + 5), rgb(0x5A6166), title);
    text(dl, Font::Mono, 13, ImVec2(pos.x + ts.x + 20, pos.y + 4), rgb(0x111315), value);
  }
  void scrollbar(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float pos, float size, int state) override {
    const bool v = (p1.y - p0.y) > (p1.x - p0.x);
    const float L = v ? p1.y - p0.y : p1.x - p0.x;
    const float a = L * pos * (1 - size), b = a + L * size;
    const float w = (state & (Hover | Active)) ? 4.0f : 2.0f;
    const float cx = (p0.x + p1.x) * 0.5f, cy = (p0.y + p1.y) * 0.5f;
    const ImU32 c = (state & Active) ? tk.text : (state & Hover) ? tk.textDim : tk.textFaint;
    if (v) dl->AddRectFilled(ImVec2(cx - w * 0.5f, p0.y + a), ImVec2(cx + w * 0.5f, p0.y + b), c, w * 0.5f);
    else dl->AddRectFilled(ImVec2(p0.x + a, cy - w * 0.5f), ImVec2(p0.x + b, cy + w * 0.5f), c, w * 0.5f);
  }
  void label(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* t, bool dim) override { caps(dl, Font::MonoBold, 9.0f, p0, p1, dim ? tk.textFaint : tk.textDim, t, 1.2f); }
  void valueText(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* t, int state) override { textIn(dl, Font::Mono, 11, p0, p1, (state & Active) ? tk.hot : tk.text, t); }
  void graphGrid(ImDrawList* dl, ImVec2 p0, ImVec2 p1, int cols, int rows) override {
    // Dots at the intersections instead of lines.
    for (int i = 1; i < cols; ++i)
      for (int j = 1; j < rows; ++j) {
        const float x = std::round(p0.x + (p1.x - p0.x) * static_cast<float>(i) / static_cast<float>(cols));
        const float y = std::round(p0.y + (p1.y - p0.y) * static_cast<float>(j) / static_cast<float>(rows));
        dl->AddRectFilled(ImVec2(x, y), ImVec2(x + 1, y + 1), (i * 2 == cols || j * 2 == rows) ? rgb(0xFFFFFF, 0.3f) : rgb(0xFFFFFF, 0.18f));
      }
  }
  void curve(ImDrawList* dl, const ImVec2* pts, int n, float baseY, ImU32 col, bool fill) override {
    if (n < 2) return;
    const ImU32 c = roleOr(col, tk.value);
    if (fill) fillUnder(dl, pts, n, baseY, alpha(c, 0.09f), alpha(c, 0.09f));
    dl->AddPolyline(pts, n, c, 1.5f, 0);
  }
  void envelope(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const ImVec2* pts, int n, int sel, float playX, float playY) override {
    well(dl, p0, p1);
    graphGrid(dl, p0, p1, 8, 4);
    curve(dl, pts, n, p1.y - 1, 0, true);
    for (int i = 0; i < n; ++i) {
      const ImVec2 q = snap(pts[i]);
      const float s = i == sel ? 4.5f : 3.0f;
      dl->AddRectFilled(ImVec2(q.x - s, q.y - s), ImVec2(q.x + s, q.y + s), i == sel ? tk.hot : tk.well);
      dl->AddRect(ImVec2(q.x - s, q.y - s), ImVec2(q.x + s, q.y + s), i == sel ? tk.hot : tk.text);
    }
    if (playX >= 0) {
      dl->AddLine(ImVec2(std::floor(playX) + 0.5f, p0.y + 1), ImVec2(std::floor(playX) + 0.5f, p1.y - 1), tk.mod, 1.0f);
      dl->AddCircleFilled(ImVec2(playX, playY), 3, tk.mod, 12);
    }
  }
  void keyboard(ImDrawList* dl, ImVec2 p0, ImVec2 p1, int lo, int hi, const bool* held, int rangeLo, int rangeHi) override {
    static const bool kBlack[12] = {false, true, false, true, false, false, true, false, true, false, true, false};
    int whites = 0;
    for (int n = lo; n <= hi; ++n) whites += kBlack[n % 12] ? 0 : 1;
    const float ww = (p1.x - p0.x) / static_cast<float>(whites);
    float x = p0.x;
    for (int n = lo; n <= hi; ++n) {
      if (kBlack[n % 12]) continue;
      ImU32 f = rgb(0xD8DCDF);
      if (held && held[n]) f = tk.hot;
      dl->AddRectFilled(ImVec2(x, p0.y), ImVec2(x + ww - 1, p1.y), f, 1.5f, ImDrawFlags_RoundCornersBottom);
      if (n >= rangeLo && n <= rangeHi) dl->AddRectFilled(ImVec2(x, p1.y + 3), ImVec2(x + ww - (n == rangeHi ? 1 : 0), p1.y + 4), tk.mod);
      x += ww;
    }
    x = p0.x;
    for (int n = lo; n <= hi; ++n) {
      if (kBlack[n % 12]) {
        const ImU32 f = (held && held[n]) ? tk.hot : rgb(0x17191B);
        dl->AddRectFilled(ImVec2(x - ww * 0.3f - 0.5f, p0.y), ImVec2(x + ww * 0.3f - 0.5f, p0.y + (p1.y - p0.y) * 0.6f), f, 1.5f, ImDrawFlags_RoundCornersBottom);
      } else {
        x += ww;
      }
    }
  }
  void modChip(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* lbl, float live, int state) override {
    const float h = p1.y - p0.y;
    dl->AddRect(snap(p0), ImVec2(std::floor(p1.x) - 0.5f, std::floor(p1.y) - 0.5f), (state & Hover) ? tk.mod : alpha(tk.mod, 0.6f), h * 0.5f);
    const ImVec2 c(p0.x + h * 0.5f, (p0.y + p1.y) * 0.5f);
    arc(dl, c, h * 0.24f, kA0, kA1, tk.rail, 1.0f, false);
    arc(dl, c, h * 0.24f, kA0, angleOf(live), tk.mod, 1.5f, false);
    textIn(dl, Font::Body, 12, ImVec2(p0.x + h, p0.y), ImVec2(p1.x - 6, p1.y), tk.text, lbl, 0);
  }
  void modAmount(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float amount, float live, int state) override {
    const float cy = std::floor((p0.y + p1.y) * 0.5f) + 0.5f, cx = std::floor((p0.x + p1.x) * 0.5f) + 0.5f, w = (p1.x - p0.x) * 0.5f;
    dl->AddLine(ImVec2(p0.x, cy), ImVec2(p1.x, cy), tk.rail, 1.0f);
    dl->AddLine(ImVec2(cx, p0.y + 3), ImVec2(cx, p1.y - 3), tk.textFaint, 1.0f);
    dl->AddLine(ImVec2(cx, cy), ImVec2(cx + w * amount, cy), (state & Hover) ? lighten(tk.mod, 0.2f) : tk.mod, 3.0f);
    const float lx = std::floor(cx + w * live * amount) + 0.5f;
    dl->AddLine(ImVec2(lx, p0.y + 2), ImVec2(lx, p1.y - 2), tk.text, 1.0f);
  }
};

}  // namespace

Painter* makeHairline() { return new Hairline(); }

}  // namespace gl::look
