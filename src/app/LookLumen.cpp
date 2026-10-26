// Gridline - J-129 direction C, "Lumen": dark glass surfaces where every value
// is light. Arcs and fills are emitted light (a colour ramp to a bright tip and
// a soft bloom), modulation is a second, cooler light that travels; live data
// (meters, scopes, phases) glows and leaves short trails. The plugin's role
// colour becomes its light, so each plugin keeps its own hue.
#include <cstdio>

#include "LookPaint.h"
#include "Widgets.h"

namespace gl::look {
namespace {

Tokens lumenTokens() {
  Tokens t;
  t.panelTop = rgb(0x171B1F), t.panelBot = rgb(0x0E1114), t.panelLine = rgb(0x05070A);
  t.section = rgb(0xFFFFFF, 0.025f), t.sectionLine = rgb(0xFFFFFF, 0.07f), t.sectionTitle = rgb(0x8A96A0);
  t.well = rgb(0x07090B), t.wellLine = rgb(0xFFFFFF, 0.06f), t.grid = rgb(0x8FB4FF, 0.06f), t.gridStrong = rgb(0x8FB4FF, 0.12f);
  t.face = rgb(0x1E2328), t.faceHi = rgb(0x262C32), t.faceLo = rgb(0x161A1E), t.faceLine = rgb(0xFFFFFF, 0.08f);
  t.field = rgb(0x0A0C0F), t.fieldText = rgb(0xFFC27A);
  t.text = rgb(0xE6EBEF), t.textDim = rgb(0x8B969F), t.textFaint = rgb(0x4E5861);
  const theme::Palette& P = theme::pal();
  t.value = P.accent, t.valueHi = lighten(P.accent, 0.45f);
  t.mod = rgb(0x4ECCFF), t.hot = P.hot, t.led = P.led, t.danger = P.danger;
  t.meterLow = rgb(0x5FE08A), t.meterMid = rgb(0xFFD24A), t.meterHigh = rgb(0xFF5B53);
  t.rail = rgb(0x1F252B);
  t.radius = 6, t.radiusSm = 4, t.radiusLg = 10;
  t.shadow = 0.8f, t.glow = 1.0f, t.line = 1;
  return t;
}

ImU32 dimOf(ImU32 c) { return alpha(darken(c, 0.45f), 0.9f); }
ImU32 tipOf(ImU32 c) { return lighten(c, 0.45f); }

class Lumen : public Painter {
 public:
  Lumen() : Painter(lumenTokens()) {}

  // Glass face: translucent fill, bright top hairline (specular), faint border.
  void raised(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float r, int state) override {
    ImU32 top = rgb(0x2A3138), bot = rgb(0x181C21);
    if (state & Hover) top = lighten(top, 0.06f), bot = lighten(bot, 0.04f);
    if (state & Active) top = rgb(0x14181C), bot = rgb(0x1E2328);
    if (state & Disabled) top = alpha(top, 0.5f), bot = alpha(bot, 0.5f);
    if (!(state & Active)) shadowRect(dl, p0, p1, r, 5, 2, 0.7f);
    gradRect(dl, p0, p1, top, bot, r);
    dl->AddRect(p0, p1, alpha(IM_COL32_WHITE, (state & Hover) ? 0.14f : 0.08f), r);
    if (!(state & Active)) dl->AddLine(ImVec2(p0.x + r, p0.y + 0.5f), ImVec2(p1.x - r, p0.y + 0.5f), alpha(IM_COL32_WHITE, 0.16f), 1.0f);
  }
  void sunken(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float r, ImU32 fill) override {
    dl->AddRectFilled(p0, p1, fill, r);
    innerShadow(dl, p0, p1, r, 2.5f, 0.9f);
    dl->AddRect(p0, p1, alpha(IM_COL32_WHITE, 0.05f), r);
  }
  // A bar of light: dim at its origin, bright at its tip, with bloom.
  void lightBar(ImDrawList* dl, ImVec2 a, ImVec2 b, bool vertical, ImU32 c, float r) {
    dl->AddRectFilled(ImVec2(a.x - 3, a.y - 3), ImVec2(b.x + 3, b.y + 3), alpha(c, 0.08f), r + 3);
    dl->AddRectFilled(ImVec2(a.x - 1.5f, a.y - 1.5f), ImVec2(b.x + 1.5f, b.y + 1.5f), alpha(c, 0.14f), r + 1.5f);
    const int s = dl->VtxBuffer.Size;
    dl->AddRectFilled(a, b, c, r);
    if (vertical) shadeV(dl, s, a.y, b.y, tipOf(c), dimOf(c));
    else shadeH(dl, s, a.x, b.x, dimOf(c), tipOf(c));
  }

  void knob(ImDrawList* dl, ImVec2 c, float r, const Knob& k) override {
    const bool dis = k.state & Disabled, act = k.state & Active, hov = k.state & Hover;
    const ImU32 col = dis ? rgb(0x5A646C) : roleOr(k.tint.value, tk.value);
    const float th = std::max(2.0f, r * 0.12f);
    const float rr = r - th;
    const float from = k.bipolar ? angleOf(0.5f) : kA0, an = angleOf(k.norm);
    // Glass disc.
    shadowCircle(dl, c, rr - th * 0.5f, 4, 2, 0.8f);
    const float dr = rr - th * 1.3f;
    gradCircle(dl, c, dr, hov || act ? rgb(0x2C343B) : rgb(0x232A30), rgb(0x0F1215));
    // Live level: the disc fills with soft light from the centre.
    if (k.level > 0) {
      const ImU32 lc = tk.led;
      dl->AddCircleFilled(c, dr * (0.35f + 0.6f * k.level), alpha(lc, 0.10f * k.level), segs(dr));
      dl->AddCircleFilled(c, dr * (0.2f + 0.4f * k.level), alpha(lc, 0.14f * k.level), segs(dr));
    }
    dl->PathArcTo(c, dr - 0.6f, kPi * 1.15f, kPi * 1.85f, segs(dr, kPi));
    dl->PathStroke(alpha(IM_COL32_WHITE, 0.18f), 0, 1.0f);
    dl->AddCircle(c, dr, alpha(IM_COL32_WHITE, 0.06f), segs(dr), 1.0f);
    // Track and the light arc.
    arc(dl, c, rr, kA0, kA1, alpha(IM_COL32_WHITE, 0.07f), th);
    if (std::fabs(an - from) > 0.002f) {
      const float g = dis ? 0.0f : act ? 1.3f : hov ? 1.0f : 0.75f;
      const float a0 = std::min(from, an), a1 = std::max(from, an);
      arc(dl, c, rr, a0, a1, alpha(col, 0.06f * g), th * 4.0f);
      arc(dl, c, rr, a0, a1, alpha(col, 0.12f * g), th * 2.4f);
      const bool up = an >= from;
      arcGrad(dl, c, rr, a0, a1, up ? dimOf(col) : tipOf(col), up ? tipOf(col) : dimOf(col), th);
      if (!dis) glowDot(dl, ImVec2(c.x + std::cos(an) * rr, c.y + std::sin(an) * rr), th * 0.55f, tipOf(col), g);
    } else if (k.bipolar) {
      glowDot(dl, ImVec2(c.x + std::cos(an) * rr, c.y + std::sin(an) * rr), th * 0.5f, col, 0.6f);
    }
    // Modulation: a cooler light outside, the live value as a comet with a tail.
    if (k.modDepth != 0) {
      const float m0 = angleOf(k.norm), m1 = angleOf(k.norm + k.modDepth);
      const ImU32 mc = roleOr(k.tint.mod, tk.mod);
      arc(dl, c, r - 0.5f, std::min(m0, m1), std::max(m0, m1), alpha(mc, 0.35f), 1.5f, false);
      if (k.modNow >= 0) {
        const float a = angleOf(k.modNow);
        const float tail = 0.35f * (k.modDepth > 0 ? 1.0f : -1.0f);
        arcGrad(dl, c, r - 0.5f, std::min(a, a - tail), std::max(a, a - tail), tail > 0 ? alpha(mc, 0.0f) : mc, tail > 0 ? mc : alpha(mc, 0.0f), 2.0f);
        glowDot(dl, ImVec2(c.x + std::cos(a) * (r - 0.5f), c.y + std::sin(a) * (r - 0.5f)), 2.0f, lighten(mc, 0.3f), 1.0f);
      }
    } else if (k.modNow >= 0) {
      const float a = angleOf(k.modNow);
      glowDot(dl, ImVec2(c.x + std::cos(a) * (r - 0.5f), c.y + std::sin(a) * (r - 0.5f)), 2.0f, tk.mod, 1.0f);
    }
    // Pointer: a short light line near the rim of the disc.
    const ImVec2 d(std::cos(an), std::sin(an));
    const ImVec2 q0(c.x + d.x * dr * 0.45f, c.y + d.y * dr * 0.45f), q1(c.x + d.x * dr * 0.85f, c.y + d.y * dr * 0.85f);
    const ImU32 pc = dis ? rgb(0x5A646C) : rgb(0xF4F6F8);
    dl->PathLineTo(q0), dl->PathLineTo(q1), dl->PathStroke(alpha(pc, 0.25f), 0, 4.0f);
    dl->PathLineTo(q0), dl->PathLineTo(q1), dl->PathStroke(pc, 0, 1.8f);
  }

  void slider(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const Slider& s) override {
    const bool v = s.vertical;
    const float th = 4.0f;
    const ImVec2 c((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f);
    const ImVec2 a = v ? ImVec2(c.x - th * 0.5f, p0.y + 7) : ImVec2(p0.x + 7, c.y - th * 0.5f);
    const ImVec2 b = v ? ImVec2(c.x + th * 0.5f, p1.y - 7) : ImVec2(p1.x - 7, c.y + th * 0.5f);
    sunken(dl, a, b, th * 0.5f, tk.well);
    const float len = v ? b.y - a.y : b.x - a.x;
    auto pos = [&](float n) { return v ? b.y - len * std::clamp(n, 0.0f, 1.0f) : a.x + len * std::clamp(n, 0.0f, 1.0f); };
    const float o = pos(s.bipolar ? 0.5f : 0.0f), q = pos(s.norm);
    const ImU32 col = (s.state & Disabled) ? rgb(0x5A646C) : roleOr(s.tint.value, tk.value);
    if (std::fabs(q - o) > 0.5f) {
      if (v) lightBar(dl, ImVec2(a.x, std::min(o, q)), ImVec2(b.x, std::max(o, q)), true, col, th * 0.5f);
      else lightBar(dl, ImVec2(std::min(o, q), a.y), ImVec2(std::max(o, q), b.y), false, col, th * 0.5f);
    }
    if (s.modDepth != 0) {
      const float m = pos(s.norm + s.modDepth);
      if (v) dl->AddRectFilled(ImVec2(b.x + 3, std::min(q, m)), ImVec2(b.x + 4.5f, std::max(q, m)), alpha(tk.mod, 0.6f));
      else dl->AddRectFilled(ImVec2(std::min(q, m), b.y + 3), ImVec2(std::max(q, m), b.y + 4.5f), alpha(tk.mod, 0.6f));
    }
    if (s.modNow >= 0) {
      const float m = pos(s.modNow);
      glowDot(dl, v ? ImVec2(b.x + 4, m) : ImVec2(m, b.y + 4), 2.0f, tk.mod, 1.0f);
    }
    const ImVec2 hc = v ? ImVec2(c.x, q) : ImVec2(q, c.y);
    const float hr = (s.state & Active) ? 7.0f : 6.0f;
    shadowCircle(dl, hc, hr, 3, 1.5f, 0.9f);
    gradCircle(dl, hc, hr, rgb(0x343C44), rgb(0x1A1F24));
    dl->AddCircle(hc, hr, alpha(IM_COL32_WHITE, 0.14f), 20, 1.0f);
    glowDot(dl, hc, 2.0f, (s.state & Disabled) ? rgb(0x5A646C) : tipOf(col), (s.state & (Hover | Active)) ? 1.0f : 0.5f);
  }

  void fader(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const Fader& f) override {
    const float tx = std::round((p0.x + p1.x) * 0.5f) - 4;
    const float y0 = p0.y + 14, y1 = p1.y - 14;
    for (int i = 0; i <= 10; ++i) {
      const float y = std::round(y1 - (y1 - y0) * static_cast<float>(i) / 10.0f) + 0.5f;
      dl->AddLine(ImVec2(tx - 15, y), ImVec2(tx - (i % 2 ? 11 : 9), y), i == 8 ? tk.text : tk.textFaint, 1.0f);
    }
    sunken(dl, ImVec2(tx - 2, y0), ImVec2(tx + 2, y1), 2, tk.well);
    const float y = std::round(y1 - (y1 - y0) * f.norm);
    // The fader's light comes up while it is touched; at rest the travel is a quiet ember.
    if (f.state & (Hover | Active)) lightBar(dl, ImVec2(tx - 1, y), ImVec2(tx + 1, y1), true, tk.value, 1);
    else dl->AddRectFilled(ImVec2(tx - 1, y), ImVec2(tx + 1, y1), alpha(tk.value, 0.28f), 1);
    if (f.meterL >= 0) {
      Meter m;
      m.l = f.meterL, m.r = f.meterR, m.peakL = f.peakL, m.peakR = f.peakR;
      meter(dl, ImVec2(tx + 13, y0), ImVec2(tx + 23, y1), m);
    }
    const ImVec2 h0(tx - 11, y - 13), h1(tx + 11, y + 13);
    raised(dl, h0, h1, 4, f.state);
    const ImU32 lc = (f.state & Active) ? tipOf(tk.value) : tk.value;
    dl->AddRectFilled(ImVec2(h0.x + 3, y - 4), ImVec2(h1.x - 3, y + 4), alpha(lc, 0.12f), 2);
    dl->AddLine(ImVec2(h0.x + 3, y + 0.5f), ImVec2(h1.x - 3, y + 0.5f), lc, 1.5f);
  }

  void meter(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const Meter& m) override {
    const bool stereo = m.r >= 0;
    const int nch = stereo ? 2 : 1;
    const float gap = 2.0f;
    const float W = m.vertical ? p1.x - p0.x : p1.y - p0.y;
    const float cw = (W - gap * static_cast<float>(nch - 1)) / static_cast<float>(nch);
    for (int ch = 0; ch < nch; ++ch) {
      const float lv = std::clamp(ch ? m.r : m.l, 0.0f, 1.0f), pk = ch ? m.peakR : m.peakL, rms = ch ? m.rmsR : m.rmsL;
      ImVec2 a, b;
      if (m.vertical) a = ImVec2(p0.x + (cw + gap) * static_cast<float>(ch), p0.y), b = ImVec2(a.x + cw, p1.y);
      else a = ImVec2(p0.x, p0.y + (cw + gap) * static_cast<float>(ch)), b = ImVec2(p1.x, a.y + cw);
      dl->AddRectFilled(a, b, tk.well, 2);
      const float L = m.vertical ? b.y - a.y : b.x - a.x;
      if (m.reduction) {
        if (m.vertical) lightBar(dl, a, ImVec2(b.x, a.y + L * lv), true, tk.danger, 2);
        else lightBar(dl, ImVec2(b.x - L * lv, a.y), b, false, tk.danger, 2);
        continue;
      }
      auto bar = [&](float v, float al) {
        if (v <= 0) return;
        const ImVec2 q0 = m.vertical ? ImVec2(a.x, b.y - L * v) : a, q1 = m.vertical ? b : ImVec2(a.x + L * v, b.y);
        const ImU32 tip = meterCol(v);
        dl->AddRectFilled(ImVec2(q0.x - 1.5f, q0.y - 1.5f), ImVec2(q1.x + 1.5f, q1.y + 1.5f), alpha(tip, 0.12f * al), 3);
        const int s = dl->VtxBuffer.Size;
        dl->AddRectFilled(q0, q1, alpha(IM_COL32_WHITE, al), 2);
        // Continuous ramp: green body, amber, red near the top.
        if (m.vertical) shadeAlong(dl, s, ImVec2(0, b.y - L * 0.55f), ImVec2(0, b.y - L), alpha(tk.meterLow, al), alpha(tk.meterHigh, al));
        else shadeAlong(dl, s, ImVec2(a.x + L * 0.55f, 0), ImVec2(a.x + L, 0), alpha(tk.meterLow, al), alpha(tk.meterHigh, al));
      };
      if (rms >= 0) bar(lv, 0.35f), bar(rms, 1.0f);
      else bar(lv, 1.0f);
      if (pk >= 0) {
        const float pp = L * std::clamp(pk, 0.0f, 1.0f);
        const ImVec2 pc = m.vertical ? ImVec2((a.x + b.x) * 0.5f, b.y - pp) : ImVec2(a.x + pp, (a.y + b.y) * 0.5f);
        if (m.vertical) dl->AddLine(ImVec2(a.x, pc.y), ImVec2(b.x, pc.y), tipOf(meterCol(pk)), 1.5f);
        else dl->AddLine(ImVec2(pc.x, a.y), ImVec2(pc.x, b.y), tipOf(meterCol(pk)), 1.5f);
        glowDot(dl, pc, 0.8f, meterCol(pk), 0.8f);
      }
    }
    if (m.clip) glowDot(dl, m.vertical ? ImVec2((p0.x + p1.x) * 0.5f, p0.y - 5) : ImVec2(p1.x + 5, (p0.y + p1.y) * 0.5f), 2.2f, tk.danger, 1.2f);
  }

  void led(ImDrawList* dl, ImVec2 c, float r, bool on, float level, ImU32 role, int state) override {
    const ImU32 col = roleOr(role, tk.led);
    const float k = on ? 1.0f : std::clamp(level, 0.0f, 1.0f);
    dl->AddCircleFilled(c, r + 1, rgb(0x07090B), 16);
    if (k > 0) {
      glowDot(dl, c, r, mix(dimOf(col), tipOf(col), k), 1.3f * k);
      dl->AddCircleFilled(c, r * 0.45f, alpha(IM_COL32_WHITE, 0.7f * k), 10);
    } else {
      dl->AddCircleFilled(c, r, alpha(col, 0.12f), 16);
      dl->AddCircle(c, r, alpha(col, 0.25f), 16, 1.0f);
    }
    (void)state;
  }

  void button(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* lbl, int state, ImU32 role) override {
    const bool on = state & On;
    const ImU32 col = roleOr(role, tk.hot);
    raised(dl, p0, p1, tk.radius, state);
    if (on) {
      // Lit from inside: coloured light rising from the bottom edge.
      const int s = dl->VtxBuffer.Size;
      dl->AddRectFilled(ImVec2(p0.x + 1, p0.y + 1), ImVec2(p1.x - 1, p1.y - 1), alpha(col, 0.3f), tk.radius - 1);
      shadeV(dl, s, p0.y, p1.y, alpha(col, 0.05f), alpha(col, 0.35f));
      dl->AddRect(ImVec2(p0.x - 1, p0.y - 1), ImVec2(p1.x + 1, p1.y + 1), alpha(col, 0.18f), tk.radius + 1, 0, 2.0f);
      dl->AddRect(p0, p1, alpha(col, 0.7f), tk.radius);
      dl->AddLine(ImVec2(p0.x + 8, p1.y - 1), ImVec2(p1.x - 8, p1.y - 1), tipOf(col), 1.5f);
    }
    textIn(dl, Font::Body, 13, p0, p1, (state & Disabled) ? tk.textFaint : on ? tipOf(col) : tk.text, lbl);
  }
  void iconButton(ImDrawList* dl, ImVec2 p0, ImVec2 p1, int icon, int state, ImU32 role) override {
    const bool on = state & On;
    const ImU32 col = roleOr(role, tk.hot);
    if (state & (Hover | Active)) raised(dl, p0, p1, tk.radius, state);
    const ImVec2 c((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f);
    if (on) {
      dl->AddCircleFilled(c, (p1.y - p0.y) * 0.55f, alpha(col, 0.10f), 24);
      dl->AddCircleFilled(c, (p1.y - p0.y) * 0.38f, alpha(col, 0.12f), 24);
    }
    const ImU32 ic = (state & Disabled) ? tk.textFaint : on ? tipOf(col) : (state & Hover) ? tk.text : tk.textDim;
    ui::drawIcon(dl, static_cast<ui::Icon>(icon), c, (p1.y - p0.y) * 0.6f, ic);
  }
  void toggle(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float t, int state, ImU32 role) override {
    const float h = p1.y - p0.y, r = h * 0.5f;
    const ImU32 col = roleOr(role, tk.hot);
    sunken(dl, p0, p1, r, tk.well);
    if (t > 0.01f) lightBar(dl, ImVec2(p0.x + 3, p0.y + 3), ImVec2(p0.x + r + (p1.x - p0.x - h) * t + 2, p1.y - 3), false, alpha(col, t), r - 3);
    const ImVec2 c(p0.x + r + (p1.x - p0.x - h) * t, p0.y + r);
    shadowCircle(dl, c, r - 2.5f, 2, 1, 0.8f);
    gradCircle(dl, c, r - 2.5f, (state & Hover) ? rgb(0x3E4750) : rgb(0x343C44), rgb(0x1A1F24));
    dl->AddCircle(c, r - 2.5f, alpha(IM_COL32_WHITE, 0.14f), 20, 1.0f);
  }
  void check(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* lbl, int state, ImU32 role) override {
    const float s = p1.y - p0.y;
    const ImVec2 b0 = p0, b1(p0.x + s, p1.y);
    const ImU32 col = roleOr(role, tk.hot);
    sunken(dl, b0, b1, tk.radiusSm, tk.well);
    if (state & On) {
      dl->AddRectFilled(ImVec2(b0.x - 2, b0.y - 2), ImVec2(b1.x + 2, b1.y + 2), alpha(col, 0.10f), tk.radiusSm + 2);
      const ImVec2 q[3] = {ImVec2(b0.x + s * 0.26f, b0.y + s * 0.52f), ImVec2(b0.x + s * 0.43f, b0.y + s * 0.68f), ImVec2(b0.x + s * 0.74f, b0.y + s * 0.33f)};
      glowLine(dl, q, 3, tipOf(col), 1.8f, 1.0f);
    } else if (state & Hover) {
      dl->AddRect(b0, b1, alpha(IM_COL32_WHITE, 0.2f), tk.radiusSm);
    }
    if (lbl) textIn(dl, Font::Body, 13, ImVec2(b1.x + 8, p0.y), p1, (state & Disabled) ? tk.textFaint : tk.text, lbl, 0);
  }
  void segmented(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* const* labels, int n, int sel, int hover, int state) override {
    sunken(dl, p0, p1, tk.radius, tk.well);
    const float w = (p1.x - p0.x) / static_cast<float>(n);
    for (int i = 0; i < n; ++i) {
      const ImVec2 a(p0.x + w * static_cast<float>(i), p0.y), b(a.x + w, p1.y);
      if (i == sel) {
        raised(dl, ImVec2(a.x + 2, a.y + 2), ImVec2(b.x - 2, b.y - 2), tk.radiusSm, state & Disabled);
        glowDot(dl, ImVec2(a.x + 10, (a.y + b.y) * 0.5f), 2.0f, tk.hot, 1.0f);
      }
      const ImU32 tc = (state & Disabled) ? tk.textFaint : i == sel ? tk.text : i == hover ? tk.text : tk.textDim;
      textIn(dl, Font::Body, 12, ImVec2(a.x + (i == sel ? 8 : 0), a.y), b, tc, labels[i]);
    }
  }
  void tabs(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* const* labels, int n, int sel, int hover) override {
    dl->AddLine(ImVec2(p0.x, p1.y - 0.5f), ImVec2(p1.x, p1.y - 0.5f), tk.sectionLine, 1.0f);
    float x = p0.x;
    for (int i = 0; i < n; ++i) {
      const float w = textSize(Font::Body, 13, labels[i]).x + 26;
      if (i == sel) {
        const int s = dl->VtxBuffer.Size;
        dl->AddRectFilled(ImVec2(x, p0.y), ImVec2(x + w, p1.y), alpha(tk.hot, 0.12f), tk.radius, ImDrawFlags_RoundCornersTop);
        shadeV(dl, s, p0.y, p1.y, alpha(tk.hot, 0.0f), alpha(tk.hot, 0.16f));
        lightBar(dl, ImVec2(x + 6, p1.y - 2), ImVec2(x + w - 6, p1.y), false, tk.hot, 1);
      }
      textIn(dl, Font::Body, 13, ImVec2(x, p0.y), ImVec2(x + w, p1.y - 2), i == sel ? tk.text : i == hover ? tk.text : tk.textDim, labels[i]);
      x += w;
    }
  }
  void dropdown(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* txt, int state) override {
    sunken(dl, p0, p1, tk.radius, (state & Hover) ? rgb(0x0F1216) : tk.field);
    if (state & (Active | Focus)) dl->AddRect(ImVec2(p0.x - 1, p0.y - 1), ImVec2(p1.x + 1, p1.y + 1), alpha(tk.hot, 0.6f), tk.radius + 1, 0, 1.5f);
    textIn(dl, Font::Body, 13, ImVec2(p0.x + 10, p0.y), ImVec2(p1.x - 22, p1.y), (state & Disabled) ? tk.textFaint : tk.text, txt, 0);
    ui::drawIcon(dl, ui::Icon::ChevronDown, ImVec2(p1.x - 12, (p0.y + p1.y) * 0.5f), 10, (state & (Hover | Active)) ? tk.hot : tk.textDim);
  }
  void menu(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* const* items, int n, int hover, int checked) override {
    shadowRect(dl, p0, p1, tk.radiusLg, 16, 6, 1.4f);
    gradRect(dl, p0, p1, rgb(0x20262C), rgb(0x161A1F), tk.radiusLg);
    dl->AddRect(p0, p1, alpha(IM_COL32_WHITE, 0.09f), tk.radiusLg);
    const float rh = (p1.y - p0.y - 10) / static_cast<float>(n);
    for (int i = 0; i < n; ++i) {
      const ImVec2 a(p0.x + 5, p0.y + 5 + rh * static_cast<float>(i)), b(p1.x - 5, a.y + rh);
      if (i == hover) {
        const int s = dl->VtxBuffer.Size;
        dl->AddRectFilled(a, b, alpha(tk.hot, 0.2f), tk.radiusSm);
        shadeH(dl, s, a.x, b.x, alpha(tk.hot, 0.24f), alpha(tk.hot, 0.04f));
      }
      if (i == checked) glowDot(dl, ImVec2(a.x + 10, (a.y + b.y) * 0.5f), 2.2f, tk.hot, 1.0f);
      textIn(dl, Font::Body, 13, ImVec2(a.x + 22, a.y), b, i == hover ? tk.text : tk.textDim, items[i], 0);
    }
  }
  void field(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* txt, int state, int caret, bool lcd) override {
    sunken(dl, p0, p1, tk.radiusSm, tk.field);
    if (state & (Active | Focus)) dl->AddRect(ImVec2(p0.x - 1, p0.y - 1), ImVec2(p1.x + 1, p1.y + 1), alpha(tk.hot, 0.6f), tk.radiusSm + 1, 0, 1.5f);
    const Font f = lcd ? Font::MonoBold : Font::Mono;
    const float fs = lcd ? std::min(18.0f, (p1.y - p0.y) * 0.62f) : 13;
    const ImU32 tc = (state & Disabled) ? tk.textFaint : lcd ? tk.fieldText : tk.text;
    if (lcd) {  // phosphor glow behind the digits
      for (int i = 0; i < 2; ++i) textIn(dl, f, fs, ImVec2(p0.x + (i ? -1.0f : 1.0f), p0.y), ImVec2(p1.x + (i ? -1.0f : 1.0f), p1.y), alpha(tk.fieldText, 0.18f), txt);
    }
    textIn(dl, f, fs, p0, p1, tc, txt);
    if (caret >= 0) {
      const ImVec2 ts = textSize(f, fs, txt);
      char pre[64];
      std::snprintf(pre, sizeof pre, "%.*s", caret, txt);
      const float x = (p0.x + p1.x - ts.x) * 0.5f + textSize(f, fs, pre).x + 1;
      const ImVec2 q[2] = {ImVec2(x, p0.y + 4), ImVec2(x, p1.y - 4)};
      glowLine(dl, q, 2, tk.hot, 1.2f, 1.0f);
    }
  }
  void stepPad(ImDrawList* dl, ImVec2 p0, ImVec2 p1, bool on, bool alt, float vel, bool playing, int state, ImU32 tint) override {
    const ImU32 col = roleOr(tint, tk.value);
    if (on) {
      dl->AddRectFilled(ImVec2(p0.x - 2, p0.y - 2), ImVec2(p1.x + 2, p1.y + 2), alpha(col, 0.12f * vel), tk.radiusSm + 2);
      const int s = dl->VtxBuffer.Size;
      dl->AddRectFilled(p0, p1, col, tk.radiusSm);
      shadeV(dl, s, p0.y, p1.y, mix(dimOf(col), tipOf(col), vel), mix(darken(col, 0.7f), col, vel * 0.8f));
      dl->AddLine(ImVec2(p0.x + 2, p0.y + 0.5f), ImVec2(p1.x - 2, p0.y + 0.5f), alpha(IM_COL32_WHITE, 0.4f * vel), 1.0f);
    } else {
      gradRect(dl, p0, p1, alt ? rgb(0x2A2629) : rgb(0x22282E), alt ? rgb(0x1A1719) : rgb(0x161A1E), tk.radiusSm);
      dl->AddRect(p0, p1, alpha(IM_COL32_WHITE, (state & Hover) ? 0.16f : 0.06f), tk.radiusSm);
    }
    if (playing) {
      dl->AddRectFilled(ImVec2(p0.x - 3, p0.y - 3), ImVec2(p1.x + 3, p1.y + 3), alpha(IM_COL32_WHITE, 0.08f), tk.radiusSm + 3);
      dl->AddRect(ImVec2(p0.x - 1, p0.y - 1), ImVec2(p1.x + 1, p1.y + 1), alpha(IM_COL32_WHITE, 0.7f), tk.radiusSm + 1, 0, 1.0f);
    }
  }
  void pad(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* lbl, float glow, int state, ImU32 tint) override {
    const ImU32 col = roleOr(tint, tk.hot);
    raised(dl, p0, p1, tk.radius, state);
    if (glow > 0) {
      dl->AddRectFilled(ImVec2(p0.x - 4, p0.y - 4), ImVec2(p1.x + 4, p1.y + 4), alpha(col, 0.10f * glow), tk.radius + 4);
      const int s = dl->VtxBuffer.Size;
      dl->AddRectFilled(ImVec2(p0.x + 1, p0.y + 1), ImVec2(p1.x - 1, p1.y - 1), alpha(col, 0.6f * glow), tk.radius);
      shadeV(dl, s, p0.y, p1.y, alpha(tipOf(col), 0.65f * glow), alpha(col, 0.2f * glow));
      dl->AddRect(p0, p1, alpha(tipOf(col), 0.8f * glow), tk.radius);
    }
    textIn(dl, Font::Body, 12, ImVec2(p0.x + 8, p1.y - 20), ImVec2(p1.x - 8, p1.y - 4), glow > 0.5f ? tk.text : tk.textDim, lbl, 0);
  }
  void panel(ImDrawList* dl, ImVec2 p0, ImVec2 p1) override {
    gradRect(dl, p0, p1, tk.panelTop, tk.panelBot, tk.radiusLg);
    // A faint cool light from the top centre.
    const int s = dl->VtxBuffer.Size;
    dl->AddRectFilled(p0, ImVec2(p1.x, p0.y + 120), alpha(rgb(0x6E8CFF), 0.05f), tk.radiusLg, ImDrawFlags_RoundCornersTop);
    shadeV(dl, s, p0.y, p0.y + 120, alpha(rgb(0x6E8CFF), 0.05f), alpha(rgb(0x6E8CFF), 0.0f));
    dl->AddRect(p0, p1, alpha(IM_COL32_WHITE, 0.07f), tk.radiusLg);
  }
  void section(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* title) override {
    dl->AddRectFilled(p0, p1, alpha(IM_COL32_WHITE, 0.025f), tk.radius);
    dl->AddRect(p0, p1, alpha(IM_COL32_WHITE, 0.06f), tk.radius);
    dl->AddLine(ImVec2(p0.x + tk.radius, p0.y + 0.5f), ImVec2(p1.x - tk.radius, p0.y + 0.5f), alpha(IM_COL32_WHITE, 0.08f), 1.0f);
    if (title) {
      glowDot(dl, ImVec2(p0.x + 12, p0.y + 12), 2.0f, tk.hot, 0.9f);
      caps(dl, Font::MonoBold, 9.5f, ImVec2(p0.x + 20, p0.y + 5), ImVec2(p1.x, p0.y + 19), tk.sectionTitle, title, 1.2f, 0);
    }
  }
  void header(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* title, const char* sub) override {
    gradRect(dl, p0, p1, rgb(0x1C2126), rgb(0x14181C), tk.radiusLg, ImDrawFlags_RoundCornersTop);
    lightBar(dl, ImVec2(p0.x + 1, p1.y - 1), ImVec2(p0.x + (p1.x - p0.x) * 0.35f, p1.y), false, tk.hot, 0);
    const float cy = (p0.y + p1.y) * 0.5f;
    glowDot(dl, ImVec2(p0.x + 16, cy), 3, tk.hot, 1.0f);
    text(dl, Font::Bold, 16, ImVec2(p0.x + 28, cy - 10), tk.text, title);
    if (sub) textIn(dl, Font::Body, 12, p0, ImVec2(p1.x - 14, p1.y), tk.textDim, sub, 2);
  }
  void well(ImDrawList* dl, ImVec2 p0, ImVec2 p1) override { sunken(dl, p0, p1, tk.radius, tk.well); }
  void caption(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* t) override {
    dl->AddRectFilled(p0, p1, alpha(IM_COL32_WHITE, 0.04f), (p1.y - p0.y) * 0.5f);
    caps(dl, Font::MonoBold, 9.5f, p0, p1, tk.textDim, t, 1.0f);
  }
  void tooltip(ImDrawList* dl, ImVec2 pos, const char* title, const char* value) override {
    const ImVec2 ts = textSize(Font::Body, 12, title), vs = textSize(Font::MonoBold, 14, value);
    const ImVec2 p1(pos.x + std::max(ts.x, vs.x) + 24, pos.y + 46);
    shadowRect(dl, pos, p1, 8, 14, 5, 1.2f);
    gradRect(dl, pos, p1, rgb(0x232930), rgb(0x171B20), 8);
    dl->AddRect(pos, p1, alpha(IM_COL32_WHITE, 0.1f), 8);
    lightBar(dl, ImVec2(pos.x + 10, pos.y), ImVec2(pos.x + 40, pos.y + 1.5f), false, tk.hot, 0);
    text(dl, Font::Body, 12, ImVec2(pos.x + 12, pos.y + 7), tk.textDim, title);
    text(dl, Font::MonoBold, 14, ImVec2(pos.x + 12, pos.y + 23), tipOf(tk.hot), value);
  }
  void scrollbar(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float pos, float size, int state) override {
    const bool v = (p1.y - p0.y) > (p1.x - p0.x);
    const float L = v ? p1.y - p0.y : p1.x - p0.x, w = v ? p1.x - p0.x : p1.y - p0.y;
    dl->AddRectFilled(p0, p1, alpha(IM_COL32_WHITE, 0.03f), w * 0.5f);
    const float a = L * pos * (1 - size), b = a + L * size;
    const ImVec2 t0 = v ? ImVec2(p0.x + 2, p0.y + a + 2) : ImVec2(p0.x + a + 2, p0.y + 2);
    const ImVec2 t1 = v ? ImVec2(p1.x - 2, p0.y + b - 2) : ImVec2(p0.x + b - 2, p1.y - 2);
    if (state & Active) lightBar(dl, t0, t1, v, tk.hot, w * 0.5f - 2);
    else dl->AddRectFilled(t0, t1, alpha(IM_COL32_WHITE, (state & Hover) ? 0.3f : 0.16f), w * 0.5f - 2);
  }
  void label(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* t, bool dim) override { caps(dl, Font::Bold, 9.5f, p0, p1, dim ? tk.textFaint : tk.textDim, t, 0.9f); }
  void valueText(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* t, int state) override {
    textIn(dl, Font::Mono, 11, p0, p1, (state & Active) ? tipOf(tk.hot) : tk.text, t);
  }
  void curve(ImDrawList* dl, const ImVec2* pts, int n, float baseY, ImU32 col, bool fill) override {
    if (n < 2) return;
    const ImU32 c = roleOr(col, tk.value);
    if (fill) fillUnder(dl, pts, n, baseY, alpha(c, 0.35f), alpha(c, 0.0f));
    glowLine(dl, pts, n, tipOf(c), 1.6f, 1.0f);
  }
  void scope(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const float* s, int n) override {
    well(dl, p0, p1);
    graphGrid(dl, p0, p1, 8, 4);
    ImVec2 pts[512];
    n = std::min(n, 512);
    const float cy = (p0.y + p1.y) * 0.5f, h = (p1.y - p0.y) * 0.45f;
    // Phosphor persistence: two fading echoes of the trace.
    for (int e = 2; e >= 0; --e) {
      for (int i = 0; i < n; ++i) pts[i] = ImVec2(p0.x + (p1.x - p0.x) * static_cast<float>(i) / static_cast<float>(n - 1), cy - s[i] * h * (1.0f - 0.07f * static_cast<float>(e)));
      if (e) dl->AddPolyline(pts, n, alpha(tk.mod, 0.12f / static_cast<float>(e)), 1.5f, 0);
      else glowLine(dl, pts, n, lighten(tk.mod, 0.3f), 1.4f, 1.0f);
    }
  }
  void spectrum(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const float* db, int n, float peakLine) override {
    well(dl, p0, p1);
    graphGrid(dl, p0, p1, 10, 4);
    ImVec2 pts[512];
    n = std::min(n, 512);
    for (int i = 0; i < n; ++i)
      pts[i] = ImVec2(p0.x + (p1.x - p0.x) * static_cast<float>(i) / static_cast<float>(n - 1), p0.y + (p1.y - p0.y) * std::clamp(-db[i] / 90.0f, 0.0f, 1.0f));
    // Spectrum fill: hue runs low (cool) to high (warm) across the band.
    for (int i = 0; i + 1 < n; ++i) {
      const ImU32 c = mix(tk.mod, tk.value, static_cast<float>(i) / static_cast<float>(n));
      fillUnder(dl, pts + i, 2, p1.y - 1, alpha(c, 0.42f), alpha(c, 0.02f));
    }
    glowLine(dl, pts, n, rgb(0xF2F6FA), 1.2f, 0.6f);
    (void)peakLine;
  }
  void waveform(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const float* mn, const float* mx, int n, float playPos, float selA, float selB) override {
    well(dl, p0, p1);
    const float cy = (p0.y + p1.y) * 0.5f, h = (p1.y - p0.y) * 0.45f;
    if (selB > selA) dl->AddRectFilled(ImVec2(p0.x + (p1.x - p0.x) * selA, p0.y + 1), ImVec2(p0.x + (p1.x - p0.x) * selB, p1.y - 1), alpha(tk.mod, 0.12f));
    const float px = playPos >= 0 ? p0.x + (p1.x - p0.x) * playPos : -1;
    for (int i = 0; i < n; ++i) {
      const float x = p0.x + 1 + (p1.x - p0.x - 2) * static_cast<float>(i) / static_cast<float>(n);
      const bool played = x < px;
      const ImU32 c = played ? tipOf(tk.value) : alpha(tk.value, 0.6f);
      const int s = dl->VtxBuffer.Size;
      dl->AddRectFilled(ImVec2(x, cy - mx[i] * h), ImVec2(x + 1, cy - mn[i] * h + 1), c);
      shadeV(dl, s, cy - h, cy + h, c, alpha(c, 0.55f));
    }
    if (px >= 0) {
      const ImVec2 q[2] = {ImVec2(px, p0.y + 1), ImVec2(px, p1.y - 1)};
      glowLine(dl, q, 2, rgb(0xFFFFFF), 1.2f, 1.0f);
    }
  }
  void wavetable(ImDrawList* dl, ImVec2 p0, ImVec2 p1, int frames, float current, float morph) override {
    well(dl, p0, p1);
    const float W = p1.x - p0.x, H = p1.y - p0.y;
    const float dx = W * 0.22f / static_cast<float>(frames), dy = H * 0.42f / static_cast<float>(frames);
    for (int f = frames - 1; f >= 0; --f) {
      const float ft = static_cast<float>(f) / static_cast<float>(frames - 1);
      const float ox = p0.x + 10 + dx * static_cast<float>(f), oy = p1.y - 16 - dy * static_cast<float>(f);
      ImVec2 pts[65];
      for (int i = 0; i <= 64; ++i) {
        const float t = static_cast<float>(i) / 64.0f;
        const float v = std::sin(t * 2 * kPi) * (1 - ft * morph) + std::sin(t * 6 * kPi) * 0.33f * ft * morph;
        pts[i] = ImVec2(ox + (W * 0.7f) * t, oy - v * H * 0.2f);
      }
      const float d = std::fabs(static_cast<float>(f) - current);
      const ImU32 c = mix(tk.mod, tk.value, ft);
      if (d < 0.5f) glowLine(dl, pts, 65, tipOf(c), 1.8f, 1.0f);
      else dl->AddPolyline(pts, 65, alpha(c, std::max(0.07f, 0.35f - d * 0.06f)), 1.0f, 0);
    }
  }
  void xyPad(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float x, float y, float mx, float my, int state) override {
    well(dl, p0, p1);
    graphGrid(dl, p0, p1, 4, 4);
    const ImVec2 q(p0.x + (p1.x - p0.x) * x, p1.y - (p1.y - p0.y) * y);
    dl->AddLine(ImVec2(q.x, p0.y + 1), ImVec2(q.x, p1.y - 1), alpha(tk.value, 0.18f), 1.0f);
    dl->AddLine(ImVec2(p0.x + 1, q.y), ImVec2(p1.x - 1, q.y), alpha(tk.value, 0.18f), 1.0f);
    if (mx >= 0) {
      const ImVec2 m(p0.x + (p1.x - p0.x) * mx, p1.y - (p1.y - p0.y) * my);
      const ImVec2 l[2] = {q, m};
      glowLine(dl, l, 2, alpha(tk.mod, 0.5f), 1.0f, 0.6f);
      glowDot(dl, m, 2.5f, lighten(tk.mod, 0.3f), 1.2f);
    }
    dl->AddCircle(q, (state & Active) ? 10.0f : 8.0f, alpha(tk.value, 0.5f), 24, 1.0f);
    glowDot(dl, q, 4, tipOf(tk.value), 1.2f);
  }
  void keyboard(ImDrawList* dl, ImVec2 p0, ImVec2 p1, int lo, int hi, const bool* held, int rangeLo, int rangeHi) override {
    static const bool kBlack[12] = {false, true, false, true, false, false, true, false, true, false, true, false};
    int whites = 0;
    for (int n = lo; n <= hi; ++n) whites += kBlack[n % 12] ? 0 : 1;
    const float ww = (p1.x - p0.x) / static_cast<float>(whites);
    float x = p0.x;
    for (int n = lo; n <= hi; ++n) {
      if (kBlack[n % 12]) continue;
      ImU32 top = rgb(0xC9D0D6), bot = rgb(0x8E979F);
      if (n >= rangeLo && n <= rangeHi) top = mix(top, tk.mod, 0.2f);
      gradRect(dl, ImVec2(x, p0.y), ImVec2(x + ww - 1, p1.y), top, bot, 2, ImDrawFlags_RoundCornersBottom);
      if (held && held[n]) {
        gradRect(dl, ImVec2(x, p0.y), ImVec2(x + ww - 1, p1.y), alpha(tk.hot, 0.2f), alpha(tipOf(tk.hot), 1.0f), 2, ImDrawFlags_RoundCornersBottom);
        dl->AddRectFilled(ImVec2(x - 2, p1.y - 6), ImVec2(x + ww + 1, p1.y + 3), alpha(tk.hot, 0.2f), 4);
      }
      x += ww;
    }
    x = p0.x;
    for (int n = lo; n <= hi; ++n) {
      if (kBlack[n % 12]) {
        const ImVec2 a(x - ww * 0.32f, p0.y), b(x + ww * 0.32f - 1, p0.y + (p1.y - p0.y) * 0.6f);
        shadowRect(dl, a, b, 2, 2, 1, 0.8f);
        if (held && held[n]) gradRect(dl, a, b, darken(tk.hot, 0.4f), tipOf(tk.hot), 2, ImDrawFlags_RoundCornersBottom);
        else gradRect(dl, a, b, rgb(0x2A3036), rgb(0x0C0E10), 2, ImDrawFlags_RoundCornersBottom);
      } else {
        x += ww;
      }
    }
  }
  void modChip(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* lbl, float live, int state) override {
    raised(dl, p0, p1, (p1.y - p0.y) * 0.5f, state);
    const float h = p1.y - p0.y;
    const ImVec2 c(p0.x + h * 0.5f, (p0.y + p1.y) * 0.5f);
    glowDot(dl, c, h * 0.16f + h * 0.08f * live, tk.mod, 0.4f + live);
    textIn(dl, Font::Body, 12, ImVec2(p0.x + h, p0.y), ImVec2(p1.x - 6, p1.y), tk.text, lbl, 0);
  }
  void modAmount(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float amount, float live, int state) override {
    sunken(dl, p0, p1, (p1.y - p0.y) * 0.5f, tk.well);
    const float cx = (p0.x + p1.x) * 0.5f, w = (p1.x - p0.x) * 0.5f - 3;
    const float e = cx + w * amount;
    lightBar(dl, ImVec2(std::min(cx, e), p0.y + 3), ImVec2(std::max(cx, e), p1.y - 3), false, (state & Hover) ? lighten(tk.mod, 0.15f) : tk.mod, 2);
    glowDot(dl, ImVec2(cx + w * live * amount, (p0.y + p1.y) * 0.5f), 2.2f, rgb(0xFFFFFF), 1.0f);
  }
};

}  // namespace

Painter* makeLumen() { return new Lumen(); }

}  // namespace gl::look
