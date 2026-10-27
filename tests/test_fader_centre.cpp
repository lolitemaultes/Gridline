// Gridline - J-139: faders and sliders are centred on their track (app/LookGeom.h).
// For every control style's fader, and the Classic one, at odd and even widths,
// whole and fractional origins and the widths the interface scales give them:
// the cap, the slot, the value fill and the ruler share one centre line, that
// line is the control's own centre (within half a pixel, the snap), every edge
// lands on a whole pixel, the ruler is symmetric, the cap's centre is exactly
// the tick line of its value (unity included), and nothing leaves the rect the
// painter is given. Sliders: the slot's centre line, likewise.
#include <cmath>
#include <cstdio>
#include "Check.h"
#include "app/LookGeom.h"
#include "app/ThemeLayout.h"

using namespace gl::look::geom;

namespace {
bool whole(float v) { return std::fabs(v - std::round(v)) < 1e-4f; }
bool same(float a, float b) { return std::fabs(a - b) < 1e-4f; }
}  // namespace

int main() {
  struct Named {
    const char* name;
    FaderSpec spec;
  };
  const Named styles[] = {{"slate", kSlateFader}, {"graphite", kGraphiteFader}, {"hairline", kHairlineFader}, {"lumen", kLumenFader}};
  const float scales[] = {1.0f, 1.25f, 1.5f, 1.75f, 2.0f};
  const float origins[] = {0.0f, 0.25f, 0.5f, 0.75f, 13.0f, 101.5f};
  const float norms[] = {0.0f, 0.13f, 0.5f, 0.72f, 0.8f, 1.0f};
  int checked = 0;
  for (const Named& st : styles) {
    const FaderSpec& s = st.spec;
    bool centred = true, onControl = true, edges = true, capOnTick = true, unity = true, ruler = true, inside = true, parity = true;
    parity = std::fmod(s.capW - s.trackW, 2.0f) == 0.0f && std::fmod(s.capH, 2.0f) == 1.0f;
    for (float sc : scales)
      for (float w0 = 15.0f; w0 <= 28.0f; w0 += 1.0f)  // the mixer's 18 / 22 / 26, the Theme card's 22, plugin faders
        for (float ox : origins) {
          const float w = std::round(w0 * sc), h = std::round(160.0f * sc);  // a scaled control is whole pixels wide
          const float px = ox, py = ox * 0.5f;
          // The painter's rect: the control plus 8 / 4 px (ui::DrawFader).
          const float x0 = px - 8, x1 = px + w + 8, y0 = py - 4, y1 = py + h + 4;
          for (float n : norms) {
            const FaderGeom g = fader(x0, y0, x1, y1, n, s, false);
            ++checked;
            if (!same((g.capX0 + g.capX1) * 0.5f, g.tx) || !same((g.trackX0 + g.trackX1) * 0.5f, g.tx)) centred = false;
            if (std::fabs(g.tx - (px + w * 0.5f)) > 0.5f + 1e-4f) onControl = false;
            if (!whole(g.capX0) || !whole(g.capX1) || !whole(g.trackX0) || !whole(g.trackX1) || !whole(g.capY0) || !whole(g.capY1)) edges = false;
            if (!same(g.y, g.tickY(n)) || !same((g.capY0 + g.capY1) * 0.5f, g.y)) capOnTick = false;
            if (n == 0.8f && !same(g.y, g.tickY(0.8f))) unity = false;
            if (!g.ticksRight || g.tickOut(s.tickOut) <= s.tickIn) ruler = false;
            if (g.tx - g.tickOut(s.tickOut) < x0 || g.tx + g.tickOut(s.tickOut) > x1 || g.capX0 < x0 || g.capX1 > x1 || g.capY0 < y0 - 1 ||
                g.capY1 > y1 + 1)
              inside = false;
          }
          // Beside a meter (the gallery): the ruler moves to the left side only, still centred on the slot.
          const FaderGeom gm = fader(x0, y0, x1 + 40, y1, 0.5f, s, true);
          if (gm.ticksRight || !same((gm.capX0 + gm.capX1) * 0.5f, gm.tx)) ruler = false;
        }
    char b[96];
    std::snprintf(b, sizeof b, "%s fader: cap and slot share one centre line", st.name);
    expect(centred, b);
    std::snprintf(b, sizeof b, "%s fader: centred on its control (within the half-pixel snap)", st.name);
    expect(onControl, b);
    std::snprintf(b, sizeof b, "%s fader: cap and slot edges on whole pixels", st.name);
    expect(edges, b);
    std::snprintf(b, sizeof b, "%s fader: the cap centres on the tick line of its value", st.name);
    expect(capOnTick && unity, b);
    std::snprintf(b, sizeof b, "%s fader: ruler on both sides, left only beside a meter", st.name);
    expect(ruler, b);
    std::snprintf(b, sizeof b, "%s fader: cap and ruler inside the painter's rect", st.name);
    expect(inside, b);
    std::snprintf(b, sizeof b, "%s fader: cap width and height have the parity that keeps them centred", st.name);
    expect(parity, b);
  }

  // Classic (ui::DrawFader without a style).
  {
    bool centred = true, edges = true, capOnTick = true, unity = true;
    for (float sc : scales)
      for (float w0 = 15.0f; w0 <= 28.0f; w0 += 1.0f)
        for (float ox : origins) {
          const float w = std::round(w0 * sc), h = std::round(160.0f * sc);
          for (float n : norms) {
            const ClassicFader g = classicFader(ox, ox, w, h, n);
            if (!same((g.left + g.right) * 0.5f, g.cx) || std::fabs(g.cx - (ox + w * 0.5f)) > 0.5f + 1e-4f) centred = false;
            if (!whole(g.cx) || !whole(g.left) || !whole(g.right)) edges = false;
            if (!same((g.capY0 + g.capY1) * 0.5f, g.yc) || !same(g.yc, g.tickY(n))) capOnTick = false;
            if (n == 0.8f && !same(g.yc, g.tickY(0.8f))) unity = false;
          }
        }
    expect(centred, "classic fader: cap, slot, fill and ticks share the control's centre line");
    expect(edges, "classic fader: centre line and cap edges on whole pixels");
    expect(capOnTick && unity, "classic fader: the cap centres on the tick of its value (unity mark included)");
  }

  // The Theme page's preview cards: the fader each style draws is centred on the card's fader slot.
  {
    bool ok = true;
    for (float cw = gl::themelayout::kCardMinW - 40; cw <= 300; cw += 1) {
      const auto c = gl::themelayout::card(10.5f, 20.0f, cw, gl::themelayout::kCardH);
      const float fw = c.fader.x1 - c.fader.x0;
      for (const Named& st : styles) {
        const FaderGeom g = fader(c.fader.x0 - 8, c.fader.y0 - 4, c.fader.x1 + 8, c.fader.y1 + 4, 0.72f, st.spec, false);
        if (std::fabs(g.tx - (c.fader.x0 + fw * 0.5f)) > 0.5f + 1e-4f || !same((g.capX0 + g.capX1) * 0.5f, g.tx)) ok = false;
      }
    }
    expect(ok, "Theme page cards: every style's fader sits on the card's fader slot");
  }

  // Sliders: the slot's centre line for odd and even rects, whole and fractional origins.
  {
    bool ok = true;
    for (float th : {1.0f, 4.0f, 6.0f})
      for (float a = 0; a <= 2; a += 0.25f)
        for (float w = 9; w <= 40; w += 1) {
          const float c = sliderCentre(a, a + w, th);
          if (std::fabs(c - (a + w * 0.5f)) > 0.5f + 1e-4f || !whole(c - th * 0.5f) || !whole(c + th * 0.5f)) ok = false;
        }
    expect(ok, "sliders: the slot's centre line is the rect's centre, snapped so both edges are whole pixels");
  }
  std::printf("# %d fader layouts checked\n", checked);
  return finish();
}
