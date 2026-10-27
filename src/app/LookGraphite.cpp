// Gridline - J-129 direction A, "Graphite": the app icon's material carried
// into every control. Neutral graphite bodies (vertical shade, top rim light,
// soft drop + contact shadow); values are recessed channels with a lit Paper
// floor, exactly like the # cut into the icon. Monochrome by default: Paper is
// the value, the lit-orange role marks modulation and "on", green stays for
// LEDs and meters. Per-plugin colours tint the Paper floor lightly.
#include <algorithm>
#include <cstdio>
#include <cstring>

#include "LookPaint.h"
#include "Widgets.h"

namespace gl::look {
namespace {

Tokens graphiteTokens() {
  Tokens t;
  t.panelTop = rgb(0x2E2E30), t.panelBot = rgb(0x1C1C1E), t.panelLine = rgb(0x0C0C0D);
  t.section = rgb(0x232325), t.sectionLine = rgb(0x0F0F10), t.sectionTitle = rgb(0x8E8E93);
  t.well = rgb(0x111112), t.wellLine = rgb(0x0A0A0B), t.grid = rgb(0xFFFFFF, 0.045f), t.gridStrong = rgb(0xFFFFFF, 0.09f);
  t.face = rgb(0x3A3A3D), t.faceHi = rgb(0x4A4A4E), t.faceLo = rgb(0x262628), t.faceLine = rgb(0x0B0B0C);
  t.field = rgb(0x141415), t.fieldText = rgb(0xF2F2F2);
  t.text = rgb(0xE8E8EA), t.textDim = rgb(0x9A9AA0), t.textFaint = rgb(0x5E5E64);
  t.value = rgb(0xF4F4F4), t.valueHi = rgb(0xFFFFFF);
  const theme::Palette& P = theme::pal();  // functional colours only: hot / modulation, lamps, danger
  t.mod = P.hot, t.hot = P.hot, t.led = P.led, t.danger = P.danger;
  t.meterLow = rgb(0xE6E6E6), t.meterMid = rgb(0xFFC56B), t.meterHigh = rgb(0xFF5B53);
  t.rail = rgb(0x0E0E0F);
  t.radius = 5, t.radiusSm = 3, t.radiusLg = 8;
  t.shadow = 1.0f, t.glow = 0.55f, t.line = 1;
  return t;
}

// Paper floor colours (top bright, bottom cooler) tinted by a role.
ImU32 floorTop(ImU32 role) { return role ? mix(rgb(0xFFFFFF), role, 0.35f) : rgb(0xFFFFFF); }
ImU32 floorBot(ImU32 role) { return role ? mix(rgb(0xD6D6D6), role, 0.45f) : rgb(0xCFCFD2); }

class Graphite : public Painter {
 public:
  Graphite() : Painter(graphiteTokens()) {}
  ImU32 hookTint(ImU32) override { return 0; }  // monochrome: Paper is every value

  // Keycap: graphite body, rim light, chamfer, drop + contact shadow.
  void raised(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float r, int state) override {
    const bool dis = state & Disabled, act = state & Active;
    if (!act) {
      shadowRect(dl, p0, p1, r, 4.0f, 1.5f, dis ? 0.4f : 0.9f);
      dl->AddRectFilled(ImVec2(p0.x, p0.y + 1), ImVec2(p1.x, p1.y + 1), IM_COL32(0, 0, 0, 120), r);  // contact
    }
    ImU32 hi = tk.faceHi, lo = tk.faceLo;
    if (state & Hover) hi = lighten(hi, 0.08f), lo = lighten(lo, 0.06f);
    if (act) hi = darken(tk.faceLo, 0.1f), lo = tk.face;  // pressed in: shade reversed
    if (dis) hi = darken(hi, 0.3f), lo = darken(lo, 0.3f);
    gradRect(dl, p0, p1, hi, lo, r);
    // Top-left sheen like the icon body.
    const int s = dl->VtxBuffer.Size;
    dl->AddRectFilled(p0, ImVec2(p1.x, (p0.y + p1.y) * 0.5f), alpha(IM_COL32_WHITE, 0.05f), r, ImDrawFlags_RoundCornersTop);
    shadeH(dl, s, p0.x, p1.x, alpha(IM_COL32_WHITE, act ? 0.0f : 0.06f), alpha(IM_COL32_WHITE, 0.0f));
    if (!act) dl->AddLine(ImVec2(p0.x + r * 0.7f, p0.y + 0.5f), ImVec2(p1.x - r * 0.7f, p0.y + 0.5f), alpha(IM_COL32_WHITE, dis ? 0.05f : 0.22f), 1.0f);
    else innerShadow(dl, p0, p1, r, 2.5f, 1.0f);
    dl->AddRect(p0, p1, tk.faceLine, r, 0, 1.0f);
  }
  // Recessed channel: near-black, inner shadow under the top edge, light lip at the bottom.
  void sunken(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float r, ImU32 fill) override {
    dl->AddRectFilled(ImVec2(p0.x, p0.y), ImVec2(p1.x, p1.y + 1), alpha(IM_COL32_WHITE, 0.06f), r);  // lip
    dl->AddRectFilled(p0, p1, fill, r);
    innerShadow(dl, p0, p1, r, 3.0f, 1.0f);
  }
  // A Paper floor inside a channel (value fills, lit states).
  void floorRect(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float r, ImU32 role, float bright = 1.0f) {
    gradRect(dl, p0, p1, alpha(floorTop(role), bright), alpha(floorBot(role), bright), r);
    innerShadow(dl, p0, p1, r, 2.0f, 0.55f);
  }

  void knob(ImDrawList* dl, ImVec2 c, float r, const Knob& k) override {
    const bool dis = k.state & Disabled, act = k.state & Active, hov = k.state & Hover;
    const float gw = std::max(3.0f, r * 0.2f);  // groove width
    const float gr = r - gw * 0.5f;             // groove centre radius
    const float from = k.bipolar ? angleOf(0.5f) : kA0, an = angleOf(k.norm);
    const ImU32 role = k.tint.value;
    // The groove: a dark recessed channel around the puck.
    arc(dl, ImVec2(c.x, c.y + 0.8f), gr, kA0, kA1, alpha(IM_COL32_WHITE, 0.06f), gw + 1.0f);  // lower lip
    arc(dl, c, gr, kA0, kA1, tk.rail, gw);
    arc(dl, ImVec2(c.x, c.y - gw * 0.18f), gr, kA0, kA1, IM_COL32(0, 0, 0, 90), gw * 0.45f, false);  // inner shade
    // Value: Paper floor in the groove (soft bloom while touched).
    if (std::fabs(an - from) > 0.002f) {
      const float a0 = std::min(from, an), a1 = std::max(from, an);
      const ImU32 fl = dis ? rgb(0x6A6A70) : floorTop(role);
      if ((hov || act) && !dis) arc(dl, c, gr, a0, a1, alpha(fl, act ? 0.14f : 0.07f), gw * 1.9f);
      arcGrad(dl, c, gr, a0, a1, dis ? fl : floorBot(role), fl, gw - 1.2f);
    }
    if (k.bipolar) {  // centre notch
      const ImVec2 d(std::cos(from), std::sin(from));
      dl->AddLine(ImVec2(c.x + d.x * (r + 1.5f), c.y + d.y * (r + 1.5f)), ImVec2(c.x + d.x * (r + 4), c.y + d.y * (r + 4)), tk.textFaint, 1.0f);
    }
    // Modulation: an orange hairline ring just outside, live dot riding the groove.
    if (k.modDepth != 0) {
      const float m0 = angleOf(k.norm), m1 = angleOf(k.norm + k.modDepth);
      arc(dl, c, r + 2.6f, std::min(m0, m1), std::max(m0, m1), alpha(tk.mod, 0.9f), 2.0f);
    }
    if (k.modNow >= 0) {
      const float a = angleOf(k.modNow);
      glowDot(dl, ImVec2(c.x + std::cos(a) * gr, c.y + std::sin(a) * gr), gw * 0.42f, tk.mod, 0.8f);
    }
    // Puck.
    const float pr = r - gw - std::max(1.5f, r * 0.07f);
    shadowCircle(dl, c, pr, std::max(3.0f, pr * 0.35f), pr * 0.12f, 1.0f);
    dl->AddCircleFilled(ImVec2(c.x, c.y + 1), pr, IM_COL32(0, 0, 0, 140), segs(pr));  // contact
    ImU32 hi = rgb(0x55555A), lo = rgb(0x1E1E20);
    if (hov) hi = lighten(hi, 0.07f);
    if (act) hi = lighten(hi, 0.12f);
    if (dis) hi = darken(hi, 0.35f), lo = darken(lo, 0.3f);
    gradCircle(dl, c, pr, hi, lo);
    // Machined face: slightly smaller disc with reversed shade (turned top).
    if (k.size >= 1 && pr > 9) {
      const float fr = pr * 0.8f;
      gradCircle(dl, c, fr, darken(hi, 0.12f), lighten(lo, 0.12f));
      // Concentric turning marks.
      for (int i = 1; i <= 3; ++i) dl->AddCircle(c, fr * (0.3f + 0.22f * static_cast<float>(i)), alpha(IM_COL32_WHITE, 0.022f), segs(fr), 1.0f);
    }
    // Live level: a faint green inner glow on the face.
    if (k.level > 0) {  // live level: a green light ring set into the face
      dl->AddCircle(c, pr * 0.8f, alpha(tk.led, 0.18f * k.level), segs(pr), 4.0f);
      dl->AddCircle(c, pr * 0.8f, alpha(mix(darken(tk.led, 0.5f), lighten(tk.led, 0.3f), k.level), 0.4f + 0.6f * k.level), segs(pr), 1.5f);
    }
    dl->PathArcTo(c, pr - 0.7f, kPi * 1.1f, kPi * 1.9f, segs(pr, kPi));
    dl->PathStroke(alpha(IM_COL32_WHITE, dis ? 0.08f : 0.28f), 0, 1.0f);  // rim light
    dl->AddCircle(c, pr, tk.faceLine, segs(pr), 1.0f);
    // Pointer: a channel cut into the puck with a Paper floor.
    const ImVec2 d(std::cos(an), std::sin(an));
    const float lw = std::clamp(pr * 0.16f, 2.0f, 4.0f);
    const ImVec2 q0(c.x + d.x * pr * 0.34f, c.y + d.y * pr * 0.34f), q1(c.x + d.x * (pr - lw * 0.9f), c.y + d.y * (pr - lw * 0.9f));
    dl->PathLineTo(q0), dl->PathLineTo(q1), dl->PathStroke(IM_COL32(0, 0, 0, 200), 0, lw + 1.4f);
    const ImU32 pc = dis ? rgb(0x707076) : act ? rgb(0xFFFFFF) : rgb(0xEDEDED);
    dl->PathLineTo(q0), dl->PathLineTo(q1), dl->PathStroke(pc, 0, lw);
    dl->AddCircleFilled(q0, lw * 0.5f, pc, 10), dl->AddCircleFilled(q1, lw * 0.5f, pc, 10);
  }

  void slider(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const Slider& s) override {
    const bool v = s.vertical;
    const float th = 6.0f;
    // J-139: the slot's centre line on the pixel grid (slot, fill and handle share it).
    const ImVec2 c(v ? geom::sliderCentre(p0.x, p1.x, th) : (p0.x + p1.x) * 0.5f, v ? (p0.y + p1.y) * 0.5f : geom::sliderCentre(p0.y, p1.y, th));
    const ImVec2 a = v ? ImVec2(c.x - th * 0.5f, p0.y + 8) : ImVec2(p0.x + 8, c.y - th * 0.5f);
    const ImVec2 b = v ? ImVec2(c.x + th * 0.5f, p1.y - 8) : ImVec2(p1.x - 8, c.y + th * 0.5f);
    sunken(dl, a, b, th * 0.5f, tk.rail);
    const float len = v ? b.y - a.y : b.x - a.x;
    auto pos = [&](float n) { return v ? b.y - len * std::clamp(n, 0.0f, 1.0f) : a.x + len * std::clamp(n, 0.0f, 1.0f); };
    const float o = pos(s.bipolar ? 0.5f : 0.0f), q = pos(s.norm);
    if (std::fabs(q - o) > 0.5f) {
      if (v) floorRect(dl, ImVec2(a.x + 1, std::min(o, q)), ImVec2(b.x - 1, std::max(o, q)), 2, s.tint.value, (s.state & Disabled) ? 0.4f : 1.0f);
      else floorRect(dl, ImVec2(std::min(o, q), a.y + 1), ImVec2(std::max(o, q), b.y - 1), 2, s.tint.value, (s.state & Disabled) ? 0.4f : 1.0f);
    }
    if (s.modDepth != 0) {
      const float m = pos(s.norm + s.modDepth);
      if (v) dl->AddRectFilled(ImVec2(b.x + 3, std::min(q, m)), ImVec2(b.x + 5, std::max(q, m)), tk.mod, 1);
      else dl->AddRectFilled(ImVec2(std::min(q, m), b.y + 3), ImVec2(std::max(q, m), b.y + 5), tk.mod, 1);
    }
    if (s.modNow >= 0) {
      const float m = pos(s.modNow);
      glowDot(dl, v ? ImVec2(c.x, m) : ImVec2(m, c.y), 2.2f, tk.mod, 0.8f);
    }
    const ImVec2 h0 = v ? ImVec2(c.x - 10, q - 6) : ImVec2(q - 6, c.y - 10), h1 = v ? ImVec2(c.x + 10, q + 6) : ImVec2(q + 6, c.y + 10);
    raised(dl, h0, h1, 3, s.state);
    const ImU32 lc = (s.state & Disabled) ? rgb(0x707076) : rgb(0xF2F2F2);
    if (v) dl->AddRectFilled(ImVec2(h0.x + 3, q - 1), ImVec2(h1.x - 3, q + 1), lc, 1);
    else dl->AddRectFilled(ImVec2(q - 1, h0.y + 3), ImVec2(q + 1, h1.y - 3), lc, 1);
  }

  void fader(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const Fader& f) override {
    // J-139: centred on the control (moved left only to make room for a meter), one centre line (LookGeom.h).
    const bool hasMeter = f.meterL >= 0;
    const geom::FaderGeom g = geom::fader(p0.x, p0.y, p1.x, p1.y, f.norm, geom::kGraphiteFader, hasMeter);
    const float tx = g.tx;
    const float marks[] = {0.0f, 0.2f, 0.4f, 0.6f, 0.8f, 1.0f};
    const float ti = geom::kGraphiteFader.tickIn, to = g.tickOut(geom::kGraphiteFader.tickOut);
    for (float m : marks) {
      const float y = g.tickY(m);
      const ImU32 c = m == 0.8f ? tk.text : tk.textFaint;
      dl->AddLine(ImVec2(tx - to, y), ImVec2(tx - ti, y), c, 1.0f);
      if (g.ticksRight) dl->AddLine(ImVec2(tx + ti, y), ImVec2(tx + to, y), c, 1.0f);
    }
    sunken(dl, ImVec2(g.trackX0, g.y0), ImVec2(g.trackX1, g.y1), 3, tk.rail);
    if (hasMeter) {
      Meter m;
      m.l = f.meterL, m.r = f.meterR, m.peakL = f.peakL, m.peakR = f.peakR;
      meter(dl, ImVec2(tx + 14, g.y0), ImVec2(tx + 26, g.y1), m);
    }
    const float y = g.y;
    const ImVec2 h0(g.capX0, g.capY0), h1(g.capX1, g.capY1);
    raised(dl, h0, h1, 4, f.state);
    // Grip: engraved lines 4 px above and below the Paper index line.
    for (float d : {-4.0f, 4.0f}) {
      dl->AddLine(ImVec2(h0.x + 4, y + d), ImVec2(h1.x - 4, y + d), IM_COL32(0, 0, 0, 150), 1.0f);
      dl->AddLine(ImVec2(h0.x + 4, y + d + 1), ImVec2(h1.x - 4, y + d + 1), alpha(IM_COL32_WHITE, 0.08f), 1.0f);
    }
    dl->AddRectFilled(ImVec2(h0.x + 2, y - 1.5f), ImVec2(h1.x - 2, y + 1.5f), (f.state & Active) ? rgb(0xFFFFFF) : rgb(0xE6E6E6), 1);
  }

  void meter(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const Meter& m) override {
    const bool stereo = m.r >= 0;
    const int nch = stereo ? 2 : 1;
    const float gap = 2.0f;
    const float W = m.vertical ? p1.x - p0.x : p1.y - p0.y;
    const float cw = (W - gap * static_cast<float>(nch - 1)) / static_cast<float>(nch);
    for (int ch = 0; ch < nch; ++ch) {
      const float lv = std::clamp(ch ? m.r : m.l, 0.0f, 1.0f);
      const float rms = ch ? m.rmsR : m.rmsL;
      const float pk = ch ? m.peakR : m.peakL;
      ImVec2 a, b;
      if (m.vertical) a = ImVec2(p0.x + (cw + gap) * static_cast<float>(ch), p0.y), b = ImVec2(a.x + cw, p1.y);
      else a = ImVec2(p0.x, p0.y + (cw + gap) * static_cast<float>(ch)), b = ImVec2(p1.x, a.y + cw);
      sunken(dl, a, b, 2, tk.rail);
      const float L = m.vertical ? b.y - a.y - 2 : b.x - a.x - 2;
      if (m.reduction) {
        const float e = L * lv;
        if (m.vertical) gradRect(dl, ImVec2(a.x + 1, a.y + 1), ImVec2(b.x - 1, a.y + 1 + e), tk.danger, darken(tk.danger, 0.25f), 1);
        else gradRect(dl, ImVec2(b.x - 1 - e, a.y + 1), ImVec2(b.x - 1, b.y - 1), tk.danger, darken(tk.danger, 0.25f), 1);
        continue;
      }
      // Continuous Paper light; the top 22% warms to amber and red (fine 1 px segment lines engraved).
      // Zones: Paper light, amber over -6 dB, red over -1 dB (each a lit floor).
      auto bar = [&](float v, float al) {
        if (v <= 0) return;
        const float zs[4] = {0.0f, 0.75f, 0.92f, 1.0f};
        const ImU32 zt[3] = {rgb(0xFFFFFF), tk.meterMid, tk.meterHigh}, zb[3] = {rgb(0xC9C9CC), darken(tk.meterMid, 0.2f), darken(tk.meterHigh, 0.2f)};
        for (int z = 0; z < 3; ++z) {
          const float e0 = zs[z], e1 = std::min(zs[z + 1], v);
          if (e1 <= e0) break;
          if (m.vertical) gradRect(dl, ImVec2(a.x + 1, b.y - 1 - L * e1), ImVec2(b.x - 1, b.y - 1 - L * e0), alpha(zt[z], al), alpha(zb[z], al), 0);
          else gradRect(dl, ImVec2(a.x + 1 + L * e0, a.y + 1), ImVec2(a.x + 1 + L * e1, b.y - 1), alpha(zt[z], al), alpha(zb[z], al), 0);
        }
      };
      if (rms >= 0) bar(lv, 0.35f), bar(rms, 1.0f);
      else bar(lv, 1.0f);
      for (float t = 0.1f; t < 1.0f; t += 0.1f) {  // engraved ruler
        if (m.vertical) dl->AddLine(ImVec2(a.x + 1, std::round(b.y - 1 - L * t) + 0.5f), ImVec2(b.x - 1, std::round(b.y - 1 - L * t) + 0.5f), IM_COL32(0, 0, 0, 110), 1.0f);
        else dl->AddLine(ImVec2(std::round(a.x + 1 + L * t) + 0.5f, a.y + 1), ImVec2(std::round(a.x + 1 + L * t) + 0.5f, b.y - 1), IM_COL32(0, 0, 0, 110), 1.0f);
      }
      if (pk >= 0) {
        const float pp = L * std::clamp(pk, 0.0f, 1.0f);
        const ImU32 pc = pk > 0.94f ? tk.danger : rgb(0xFFFFFF);
        if (m.vertical) dl->AddRectFilled(ImVec2(a.x + 1, b.y - 2 - pp), ImVec2(b.x - 1, b.y - pp), pc);
        else dl->AddRectFilled(ImVec2(a.x + pp, a.y + 1), ImVec2(a.x + pp + 2, b.y - 1), pc);
      }
    }
    if (m.clip) {
      const ImVec2 c0 = m.vertical ? ImVec2(p0.x, p0.y - 6) : ImVec2(p1.x + 3, p0.y), c1 = m.vertical ? ImVec2(p1.x, p0.y - 2) : ImVec2(p1.x + 7, p1.y);
      glowDot(dl, ImVec2((c0.x + c1.x) * 0.5f, (c0.y + c1.y) * 0.5f), 0.1f, tk.danger, 0);
      dl->AddRectFilled(c0, c1, tk.danger, 1);
    }
  }

  void led(ImDrawList* dl, ImVec2 c, float r, bool on, float level, ImU32 role, int state) override {
    const ImU32 col = roleOr(role, tk.led);
    // Socket: a recessed hole with a lip.
    dl->AddCircleFilled(ImVec2(c.x, c.y + 0.8f), r + 1.6f, alpha(IM_COL32_WHITE, 0.07f), 16);
    dl->AddCircleFilled(c, r + 1.6f, rgb(0x0A0A0B), 16);
    const float k = on ? 1.0f : std::clamp(level, 0.0f, 1.0f);
    if (k > 0) {
      glowDot(dl, c, r, alpha(mix(darken(col, 0.4f), col, k), 1.0f), 0.9f * k);
      gradCircle(dl, c, r, alpha(lighten(col, 0.45f), 0.35f + 0.65f * k), alpha(col, 0.3f + 0.7f * k));
      dl->AddCircleFilled(ImVec2(c.x - r * 0.3f, c.y - r * 0.38f), r * 0.32f, alpha(IM_COL32_WHITE, 0.75f * k), 10);
    } else {
      gradCircle(dl, c, r, mix(rgb(0x1A1A1C), col, 0.16f), mix(rgb(0x0E0E0F), col, 0.06f));
      dl->AddCircleFilled(ImVec2(c.x - r * 0.3f, c.y - r * 0.38f), r * 0.28f, alpha(IM_COL32_WHITE, 0.10f), 10);
    }
    (void)state;
  }

  // Off: graphite keycap. On: Paper floor with ink text (the icon's lit channel).
  void button(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* lbl, int state, ImU32 role) override {
    const bool on = state & On, dis = state & Disabled;
    if (on) {
      sunken(dl, p0, p1, tk.radius, tk.rail);
      floorRect(dl, ImVec2(p0.x + 1.5f, p0.y + 1.5f), ImVec2(p1.x - 1.5f, p1.y - 1.5f), tk.radius - 1, role, (state & Hover) ? 1.0f : 0.94f);
      textIn(dl, Font::Bold, 13, p0, p1, rgb(0x161618), lbl);
      return;
    }
    raised(dl, p0, p1, tk.radius, state);
    const float dy = (state & Active) ? 1.0f : 0.0f;
    textIn(dl, Font::Body, 13, ImVec2(p0.x, p0.y + dy), ImVec2(p1.x, p1.y + dy), dis ? tk.textFaint : (state & Hover) ? rgb(0xFFFFFF) : tk.text, lbl);
  }
  void iconButton(ImDrawList* dl, ImVec2 p0, ImVec2 p1, int icon, int state, ImU32 role) override {
    const bool on = state & On;
    if (on) {
      sunken(dl, p0, p1, tk.radius, tk.rail);
      floorRect(dl, ImVec2(p0.x + 1.5f, p0.y + 1.5f), ImVec2(p1.x - 1.5f, p1.y - 1.5f), tk.radius - 1, 0, 0.94f);
    } else if (state & (Hover | Active)) {
      raised(dl, p0, p1, tk.radius, state);
    }
    const ImU32 col = (state & Disabled) ? tk.textFaint : on ? roleOr(role ? darken(role, 0.35f) : 0, rgb(0x161618)) : (state & Hover) ? rgb(0xFFFFFF) : tk.textDim;
    ui::drawIcon(dl, static_cast<ui::Icon>(icon), ImVec2((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f + ((state & Active) ? 1.0f : 0.0f)), (p1.y - p0.y) * 0.6f, col);
  }
  void toggle(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float t, int state, ImU32 role) override {
    const float h = p1.y - p0.y, r = h * 0.5f;
    sunken(dl, p0, p1, r, tk.rail);
    if (t > 0.01f) {
      const ImVec2 f1(p0.x + h + (p1.x - p0.x - h) * t, p1.y - 2);
      floorRect(dl, ImVec2(p0.x + 2, p0.y + 2), f1, r - 2, role, t);
    }
    const ImVec2 c(p0.x + r + (p1.x - p0.x - h) * t, p0.y + r);
    shadowCircle(dl, c, r - 2.5f, 3, 1.2f, 1.0f);
    gradCircle(dl, c, r - 2.5f, (state & Hover) ? rgb(0x606066) : rgb(0x55555A), rgb(0x232325));
    dl->PathArcTo(c, r - 3.2f, kPi * 1.1f, kPi * 1.9f, 16);
    dl->PathStroke(alpha(IM_COL32_WHITE, 0.3f), 0, 1.0f);
    dl->AddCircle(c, r - 2.5f, tk.faceLine, 20, 1.0f);
  }
  void check(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* lbl, int state, ImU32 role) override {
    const float s = p1.y - p0.y;
    const ImVec2 b0 = p0, b1(p0.x + s, p1.y);
    sunken(dl, b0, b1, tk.radiusSm, tk.rail);
    if (state & On) {
      floorRect(dl, ImVec2(b0.x + 2, b0.y + 2), ImVec2(b1.x - 2, b1.y - 2), tk.radiusSm - 1, role);
      const ImVec2 q[3] = {ImVec2(b0.x + s * 0.28f, b0.y + s * 0.52f), ImVec2(b0.x + s * 0.44f, b0.y + s * 0.68f), ImVec2(b0.x + s * 0.73f, b0.y + s * 0.34f)};
      dl->AddPolyline(q, 3, rgb(0x161618), 2.0f, 0);
    } else if (state & Hover) {
      dl->AddRect(b0, b1, alpha(IM_COL32_WHITE, 0.18f), tk.radiusSm);
    }
    if (lbl) textIn(dl, Font::Body, 13, ImVec2(b1.x + 8, p0.y), p1, (state & Disabled) ? tk.textFaint : tk.text, lbl, 0);
  }
  void segmented(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* const* labels, int n, int sel, int hover, int state) override {
    sunken(dl, p0, p1, tk.radius, tk.rail);
    const float w = (p1.x - p0.x - 4) / static_cast<float>(n);
    for (int i = 0; i < n; ++i) {
      const ImVec2 a(p0.x + 2 + w * static_cast<float>(i), p0.y + 2), b(a.x + w, p1.y - 2);
      if (i == sel) raised(dl, ImVec2(a.x + 0.5f, a.y), ImVec2(b.x - 0.5f, b.y), tk.radiusSm, (state & Disabled) ? Disabled : 0);
      const ImU32 tc = (state & Disabled) ? tk.textFaint : i == sel ? rgb(0xFFFFFF) : i == hover ? tk.text : tk.textDim;
      textIn(dl, i == sel ? Font::Bold : Font::Body, 12, a, b, tc, labels[i]);
      if (i == sel) dl->AddRectFilled(ImVec2((a.x + b.x) * 0.5f - 6, b.y - 3), ImVec2((a.x + b.x) * 0.5f + 6, b.y - 1.5f), tk.hot, 1);
    }
  }
  void tabs(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* const* labels, int n, int sel, int hover) override {
    sunken(dl, ImVec2(p0.x, p1.y - 3), ImVec2(p1.x, p1.y), 1, tk.rail);
    float x = p0.x;
    for (int i = 0; i < n; ++i) {
      const float w = textSize(Font::Body, 13, labels[i]).x + 26;
      const ImVec2 a(x, p0.y), b(x + w, p1.y - 3);
      if (i == sel) {
        // The selected tab's channel floor lights up under it.
        floorRect(dl, ImVec2(a.x + 4, p1.y - 2.5f), ImVec2(b.x - 4, p1.y - 0.5f), 1, 0);
        glowDot(dl, ImVec2((a.x + b.x) * 0.5f, p1.y - 1.5f), 0.1f, IM_COL32_WHITE, 0);
      }
      textIn(dl, i == sel ? Font::Bold : Font::Body, 13, a, b, i == sel ? rgb(0xFFFFFF) : i == hover ? tk.text : tk.textDim, labels[i]);
      x += w;
    }
  }
  void dropdown(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* txt, int state) override {
    raised(dl, p0, p1, tk.radius, state & ~Focus);
    const float h = p1.y - p0.y;
    dl->AddLine(ImVec2(p1.x - h - 0.5f, p0.y + 4), ImVec2(p1.x - h - 0.5f, p1.y - 4), IM_COL32(0, 0, 0, 140), 1.0f);
    dl->AddLine(ImVec2(p1.x - h + 0.5f, p0.y + 4), ImVec2(p1.x - h + 0.5f, p1.y - 4), alpha(IM_COL32_WHITE, 0.07f), 1.0f);
    textIn(dl, Font::Body, 13, ImVec2(p0.x + 10, p0.y), ImVec2(p1.x - h - 4, p1.y), (state & Disabled) ? tk.textFaint : tk.text, txt, 0);
    ui::drawIcon(dl, ui::Icon::ChevronDown, ImVec2(p1.x - h * 0.5f, (p0.y + p1.y) * 0.5f), 10, (state & (Hover | Active)) ? rgb(0xFFFFFF) : tk.textDim);
  }
  void menu(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* const* items, int n, int hover, int checked) override {
    shadowRect(dl, p0, p1, tk.radiusLg, 14, 6, 1.4f);
    gradRect(dl, p0, p1, rgb(0x343437), rgb(0x28282A), tk.radiusLg);
    dl->AddLine(ImVec2(p0.x + 6, p0.y + 0.5f), ImVec2(p1.x - 6, p0.y + 0.5f), alpha(IM_COL32_WHITE, 0.16f), 1.0f);
    dl->AddRect(p0, p1, tk.faceLine, tk.radiusLg);
    const float rh = (p1.y - p0.y - 10) / static_cast<float>(n);
    for (int i = 0; i < n; ++i) {
      const ImVec2 a(p0.x + 5, p0.y + 5 + rh * static_cast<float>(i)), b(p1.x - 5, a.y + rh);
      if (i == hover) floorRect(dl, a, b, tk.radiusSm, 0, 0.92f);
      const ImU32 tc = i == hover ? rgb(0x161618) : tk.text;
      if (i == checked) ui::drawIcon(dl, ui::Icon::Check, ImVec2(a.x + 11, (a.y + b.y) * 0.5f), 10, i == hover ? tc : tk.hot);
      textIn(dl, Font::Body, 13, ImVec2(a.x + 24, a.y), b, tc, items[i], 0);
    }
  }
  void field(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* txt, int state, int caret, bool lcd) override {
    sunken(dl, p0, p1, tk.radiusSm, lcd ? rgb(0x0B0B0C) : tk.field);
    if (lcd) {  // faint engraved grid like the icon texture
      dl->PushClipRect(p0, p1, true);
      for (float x = p0.x + 6; x < p1.x; x += 6) dl->AddLine(ImVec2(x, p0.y), ImVec2(x, p1.y), alpha(IM_COL32_WHITE, 0.018f), 1.0f);
      dl->PopClipRect();
    }
    if (state & (Active | Focus)) dl->AddRect(ImVec2(p0.x + 0.75f, p0.y + 0.75f), ImVec2(p1.x - 0.75f, p1.y - 0.75f), alpha(tk.hot, 0.85f), tk.radiusSm, 0, 1.5f);  // J-137: inside
    const Font f = lcd ? Font::MonoBold : Font::Mono;
    const float fs = lcd ? std::min(18.0f, (p1.y - p0.y) * 0.62f) : 13;
    const ImU32 tc = (state & Disabled) ? tk.textFaint : (state & Hover) ? rgb(0xFFFFFF) : lcd ? rgb(0xF2F2F2) : tk.text;
    if (lcd) textIn(dl, f, fs, ImVec2(p0.x, p0.y + 1), ImVec2(p1.x, p1.y + 1), alpha(tk.value, 0.08f), txt);  // soft phosphor
    textIn(dl, f, fs, p0, p1, tc, txt);
    if (caret >= 0) {
      const ImVec2 ts = textSize(f, fs, txt);
      char pre[64];
      std::snprintf(pre, sizeof pre, "%.*s", caret, txt);
      const float x = (p0.x + p1.x - ts.x) * 0.5f + textSize(f, fs, pre).x + 1;
      dl->AddLine(ImVec2(x, p0.y + 4), ImVec2(x, p1.y - 4), tk.hot, 1.5f);
    }
  }
  void stepPad(ImDrawList* dl, ImVec2 p0, ImVec2 p1, bool on, bool alt, float vel, bool playing, int state, ImU32 tint) override {
    if (on) {
      sunken(dl, p0, p1, tk.radiusSm, tk.rail);
      floorRect(dl, ImVec2(p0.x + 1.5f, p0.y + 1.5f), ImVec2(p1.x - 1.5f, p1.y - 1.5f), tk.radiusSm - 1, tint, 0.45f + 0.55f * vel);
    } else {
      // Off steps are keycaps; the alternate group is a warmer graphite.
      const ImU32 hi = alt ? rgb(0x4D4744) : tk.faceHi, lo = alt ? rgb(0x292524) : tk.faceLo;
      shadowRect(dl, p0, p1, tk.radiusSm, 3, 1, 0.8f);
      gradRect(dl, p0, p1, (state & Hover) ? lighten(hi, 0.08f) : hi, lo, tk.radiusSm);
      dl->AddLine(ImVec2(p0.x + 2, p0.y + 0.5f), ImVec2(p1.x - 2, p0.y + 0.5f), alpha(IM_COL32_WHITE, 0.18f), 1.0f);
      dl->AddRect(p0, p1, tk.faceLine, tk.radiusSm);
    }
    if (playing) {  // J-137: the ring sits inside the pad (never clipped by a step viewport or covered by a neighbour)
      dl->AddRect(ImVec2(p0.x + 0.6f, p0.y + 0.6f), ImVec2(p1.x - 0.6f, p1.y - 0.6f), tk.hot, tk.radiusSm, 0, 1.2f);
      dl->AddRect(ImVec2(p0.x + 2.0f, p0.y + 2.0f), ImVec2(p1.x - 2.0f, p1.y - 2.0f), alpha(tk.hot, 0.35f), std::max(0.0f, tk.radiusSm - 1), 0, 1.5f);
    }
  }
  void pad(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* lbl, float glow, int state, ImU32 tint) override {
    raised(dl, p0, p1, tk.radius, state);
    if (glow > 0) {
      const ImVec2 a(p0.x + 5, p0.y + 5), b(p1.x - 5, p1.y - 22);
      sunken(dl, a, b, 3, tk.rail);
      floorRect(dl, ImVec2(a.x + 1.5f, a.y + 1.5f), ImVec2(b.x - 1.5f, b.y - 1.5f), 2, tint, glow);
      dl->AddRect(ImVec2(p0.x + 0.5f, p0.y + 0.5f), ImVec2(p1.x - 0.5f, p1.y - 0.5f), alpha(floorTop(tint), 0.35f * glow), tk.radius);  // J-137: inside
    } else {
      sunken(dl, ImVec2(p0.x + 5, p0.y + 5), ImVec2(p1.x - 5, p1.y - 22), 3, tk.rail);
    }
    textIn(dl, Font::Body, 12, ImVec2(p0.x + 7, p1.y - 20), ImVec2(p1.x - 7, p1.y - 3), tk.textDim, lbl, 0);
  }
  void panel(ImDrawList* dl, ImVec2 p0, ImVec2 p1) override {
    gradRect(dl, p0, p1, tk.panelTop, tk.panelBot, tk.radiusLg);
    // Top-left sheen and the faint engraved grid of the icon.
    const int s = dl->VtxBuffer.Size;
    dl->AddRectFilled(p0, ImVec2(p1.x, p0.y + (p1.y - p0.y) * 0.5f), alpha(IM_COL32_WHITE, 0.035f), tk.radiusLg, ImDrawFlags_RoundCornersTop);
    shadeH(dl, s, p0.x, p0.x + (p1.x - p0.x) * 0.7f, alpha(IM_COL32_WHITE, 0.035f), alpha(IM_COL32_WHITE, 0.0f));
    dl->PushClipRect(p0, p1, true);
    for (float x = p0.x + 24; x < p1.x; x += 24) dl->AddLine(ImVec2(x + 0.5f, p0.y), ImVec2(x + 0.5f, p1.y), alpha(IM_COL32_WHITE, 0.012f), 1.0f);
    for (float y = p0.y + 24; y < p1.y; y += 24) dl->AddLine(ImVec2(p0.x, y + 0.5f), ImVec2(p1.x, y + 0.5f), alpha(IM_COL32_WHITE, 0.012f), 1.0f);
    dl->PopClipRect();
    dl->AddLine(ImVec2(p0.x + tk.radiusLg, p0.y + 0.5f), ImVec2(p1.x - tk.radiusLg, p0.y + 0.5f), alpha(IM_COL32_WHITE, 0.12f), 1.0f);
    dl->AddRect(p0, p1, tk.panelLine, tk.radiusLg);
  }
  void section(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* title) override {
    // A shallow tray milled into the panel.
    dl->AddRectFilled(ImVec2(p0.x, p0.y), ImVec2(p1.x, p1.y + 1), alpha(IM_COL32_WHITE, 0.05f), tk.radius);
    gradRect(dl, p0, p1, rgb(0x1E1E20), rgb(0x232325), tk.radius);
    innerShadow(dl, p0, p1, tk.radius, 3.0f, 0.7f);
    if (title) {
      caps(dl, Font::MonoBold, 9.5f, ImVec2(p0.x + 10, p0.y + 5), ImVec2(p1.x, p0.y + 19), tk.sectionTitle, title, 1.2f, 0);
    }
  }
  void header(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* title, const char* sub) override {
    gradRect(dl, p0, p1, rgb(0x38383B), rgb(0x2A2A2C), tk.radiusLg, ImDrawFlags_RoundCornersTop);
    dl->AddLine(ImVec2(p0.x + tk.radiusLg, p0.y + 0.5f), ImVec2(p1.x - tk.radiusLg, p0.y + 0.5f), alpha(IM_COL32_WHITE, 0.18f), 1.0f);
    dl->AddLine(ImVec2(p0.x, p1.y - 0.5f), ImVec2(p1.x, p1.y - 0.5f), rgb(0x0B0B0C), 1.0f);
    const float cy = (p0.y + p1.y) * 0.5f;
    // A little Paper channel as the brand tick.
    sunken(dl, ImVec2(p0.x + 12, cy - 8), ImVec2(p0.x + 17, cy + 8), 2, tk.rail);
    floorRect(dl, ImVec2(p0.x + 13, cy - 7), ImVec2(p0.x + 16, cy + 7), 1, 0);
    text(dl, Font::Bold, 16, ImVec2(p0.x + 26, cy - 10), rgb(0xF4F4F4), title);
    if (sub) textIn(dl, Font::Body, 12, p0, ImVec2(p1.x - 14, p1.y), tk.textDim, sub, 2);
  }
  void well(ImDrawList* dl, ImVec2 p0, ImVec2 p1) override {
    sunken(dl, p0, p1, tk.radius, rgb(0x0D0D0E));
  }
  void caption(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* t) override {
    caps(dl, Font::MonoBold, 9.5f, p0, p1, tk.sectionTitle, t, 1.2f);
  }
  void tooltip(ImDrawList* dl, ImVec2 pos, const char* title, const char* value) override {
    const ImVec2 ts = textSize(Font::Body, 12, title), vs = textSize(Font::MonoBold, 14, value);
    const ImVec2 p1(pos.x + std::max(ts.x, vs.x) + 24, pos.y + 46);
    shadowRect(dl, pos, p1, 6, 12, 5, 1.3f);
    gradRect(dl, pos, p1, rgb(0x3A3A3D), rgb(0x2B2B2D), 6);
    dl->AddLine(ImVec2(pos.x + 6, pos.y + 0.5f), ImVec2(p1.x - 6, pos.y + 0.5f), alpha(IM_COL32_WHITE, 0.2f), 1.0f);
    dl->AddRect(pos, p1, tk.faceLine, 6);
    text(dl, Font::Body, 12, ImVec2(pos.x + 12, pos.y + 7), tk.textDim, title);
    text(dl, Font::MonoBold, 14, ImVec2(pos.x + 12, pos.y + 23), rgb(0xFFFFFF), value);
  }
  void scrollbar(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float pos, float size, int state) override {
    const bool v = (p1.y - p0.y) > (p1.x - p0.x);
    const float L = v ? p1.y - p0.y : p1.x - p0.x, w = v ? p1.x - p0.x : p1.y - p0.y;
    sunken(dl, p0, p1, w * 0.5f, tk.rail);
    const float a = L * pos * (1 - size), b = a + L * size;
    const ImVec2 t0 = v ? ImVec2(p0.x + 1.5f, p0.y + a + 1.5f) : ImVec2(p0.x + a + 1.5f, p0.y + 1.5f);
    const ImVec2 t1 = v ? ImVec2(p1.x - 1.5f, p0.y + b - 1.5f) : ImVec2(p0.x + b - 1.5f, p1.y - 1.5f);
    if (state & Active) floorRect(dl, t0, t1, w * 0.5f - 1.5f, 0, 0.9f);
    else gradRect(dl, t0, t1, (state & Hover) ? rgb(0x6A6A70) : rgb(0x55555A), rgb(0x38383B), w * 0.5f - 1.5f);
  }
  void label(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* t, bool dim) override {
    caps(dl, Font::Bold, 9.5f, p0, p1, dim ? tk.textFaint : tk.textDim, t, 0.9f);
  }
  void valueText(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* t, int state) override {
    textIn(dl, Font::Mono, 11, p0, p1, (state & Active) ? rgb(0xFFFFFF) : tk.text, t);
  }
  void graphGrid(ImDrawList* dl, ImVec2 p0, ImVec2 p1, int cols, int rows) override {
    // Engraved: a dark line with a faint light line under it.
    for (int i = 1; i < cols; ++i) {
      const float x = std::round(p0.x + (p1.x - p0.x) * static_cast<float>(i) / static_cast<float>(cols)) + 0.5f;
      dl->AddLine(ImVec2(x, p0.y + 2), ImVec2(x, p1.y - 2), i * 2 == cols ? tk.gridStrong : tk.grid, 1.0f);
    }
    for (int i = 1; i < rows; ++i) {
      const float y = std::round(p0.y + (p1.y - p0.y) * static_cast<float>(i) / static_cast<float>(rows)) + 0.5f;
      dl->AddLine(ImVec2(p0.x + 2, y), ImVec2(p1.x - 2, y), i * 2 == rows ? tk.gridStrong : tk.grid, 1.0f);
    }
  }
  void keyboard(ImDrawList* dl, ImVec2 p0, ImVec2 p1, int lo, int hi, const bool* held, int rangeLo, int rangeHi) override {
    sunken(dl, ImVec2(p0.x - 2, p0.y - 2), ImVec2(p1.x + 2, p1.y + 2), 3, tk.rail);
    Painter::keyboard(dl, p0, p1, lo, hi, held, rangeLo, rangeHi);
    dl->AddRectFilled(ImVec2(p0.x, p0.y), ImVec2(p1.x, p0.y + 5), IM_COL32(0, 0, 0, 0), 0);
    const int s = dl->VtxBuffer.Size;
    dl->AddRectFilled(p0, ImVec2(p1.x, p0.y + 6), IM_COL32(0, 0, 0, 90));
    shadeV(dl, s, p0.y, p0.y + 6, IM_COL32(0, 0, 0, 90), IM_COL32(0, 0, 0, 0));
  }
  void modChip(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* lbl, float live, int state) override {
    raised(dl, p0, p1, (p1.y - p0.y) * 0.5f, state);
    const float h = p1.y - p0.y;
    // Live value as a tiny Paper channel filling up.
    const ImVec2 a(p0.x + h * 0.35f, (p0.y + p1.y) * 0.5f - 2), b(p0.x + h * 1.1f, (p0.y + p1.y) * 0.5f + 2);
    sunken(dl, a, b, 2, tk.rail);
    if (live > 0) dl->AddRectFilled(ImVec2(a.x + 1, a.y + 1), ImVec2(a.x + 1 + (b.x - a.x - 2) * live, b.y - 1), tk.mod, 1);
    textIn(dl, Font::Body, 12, ImVec2(b.x + 6, p0.y), ImVec2(p1.x - 6, p1.y), tk.text, lbl, 0);
  }
};

}  // namespace

Painter* makeGraphite() { return new Graphite(); }

}  // namespace gl::look
