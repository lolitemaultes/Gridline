// Gridline - J-139: where a fader's and a slider's parts go, shared by the
// control style painters (Look*.cpp) and ui::DrawFader. Pure geometry, so the
// tests can check it headless (tests/test_fader_centre.cpp).
//
// Rules: the cap, the track, the value fill, the ruler ticks and the unity
// mark share one centre line; every edge of an even-width part falls on a
// whole pixel and an odd-width part is centred on a half pixel, so neither
// side is blurred more than the other; the cap's centre and the ruler's tick
// lines use the same rounding, so at any value the cap sits exactly on the
// tick for that value. Centring holds in interface units, so it holds at
// every interface scale too (everything scales by the same factor).
#pragma once
#include <algorithm>
#include <cmath>

namespace gl::look::geom {

// A centre near `c` for a band `w` wide whose edges fall on whole pixels
// (a whole pixel for an even width, a half pixel for an odd one).
inline float snapCentre(float c, float w) {
  const float h = w * 0.5f;
  return std::round(c - h) + h;
}

// One style's fader. The painter's rect is the control's own plus 8 px each
// side and 4 px above and below (ui::DrawFader, ThemeLayout.h).
struct FaderSpec {
  float trackW;            // the slot
  float capW, capH;        // the cap (capW has trackW's parity, capH is odd: the cap centres on a tick line)
  float insetY;            // the travel's inset from the rect's top and bottom
  float tickIn, tickOut;   // ruler ticks, from the track's centre line
  float meterShift;        // the track moves left by this when a meter is drawn beside it
};
constexpr FaderSpec kSlateFader{4, 16, 23, 11, 9, 14, 0};
constexpr FaderSpec kGraphiteFader{6, 24, 29, 14, 11, 17, 4};
constexpr FaderSpec kHairlineFader{1, 21, 17, 12, 7, 14, 4};
constexpr FaderSpec kLumenFader{4, 22, 25, 14, 9, 15, 4};

struct FaderGeom {
  float tx = 0;                     // the centre line
  float y0 = 0, y1 = 0;             // the travel (top = full, bottom = zero)
  float y = 0;                      // the cap's centre (= tickY(norm))
  float trackX0 = 0, trackX1 = 0;   // the slot (and the value fill)
  float capX0 = 0, capX1 = 0, capY0 = 0, capY1 = 0;
  float tickMax = 0;                // ticks reach at most this far from tx (2 px beside the control)
  bool ticksRight = true;           // ruler on both sides (none when a meter is beside the track)
  // The y of the 1 px ruler line for a value (0..1): the pixel row it falls in, on its half pixel.
  float tickY(float n) const { return std::floor(y1 - (y1 - y0) * std::clamp(n, 0.0f, 1.0f)) + 0.5f; }
  // A tick's outer end, kept inside the rect.
  float tickOut(float want) const { return std::min(want, tickMax); }
};

inline FaderGeom fader(float x0, float top, float x1, float bot, float norm, const FaderSpec& s, bool meter) {
  FaderGeom g;
  g.tx = snapCentre((x0 + x1) * 0.5f - (meter ? s.meterShift : 0.0f), s.trackW);
  g.y0 = top + s.insetY, g.y1 = bot - s.insetY;
  g.y = g.tickY(norm);
  g.trackX0 = g.tx - s.trackW * 0.5f, g.trackX1 = g.tx + s.trackW * 0.5f;
  g.capX0 = g.tx - s.capW * 0.5f, g.capX1 = g.tx + s.capW * 0.5f;
  g.capY0 = g.y - s.capH * 0.5f, g.capY1 = g.y + s.capH * 0.5f;
  // At most 2 px beside the control (the 8 px margin less 6): the mixer's well and the meter next to it stay clear.
  g.tickMax = std::max(s.tickIn + 1.0f, (x1 - x0) * 0.5f - 6.0f);
  g.ticksRight = !meter;
  return g;
}

// ui::DrawFader without a control style (Classic): the control's own rect.
struct ClassicFader {
  float cx = 0;                 // the centre line (a whole pixel: track 4, fill 2, cap even)
  float left = 0, right = 0;    // the control's width made even around cx
  float t0 = 0, t1 = 0;         // the travel of the cap's centre
  float yc = 0;                 // the cap's centre
  float capY0 = 0, capY1 = 0;
  float tickY(float n) const { return std::round(t0 + (t1 - t0) * (1.0f - std::clamp(n, 0.0f, 1.0f))); }
};
inline ClassicFader classicFader(float px, float py, float w, float h, float norm, float capH = 18.0f) {
  ClassicFader g;
  g.cx = std::round(px + w * 0.5f);
  const float half = std::floor(w * 0.5f);
  g.left = g.cx - half, g.right = g.cx + half;
  g.t0 = py + capH * 0.5f, g.t1 = py + h - capH * 0.5f;
  g.yc = g.tickY(norm);
  g.capY0 = g.yc - capH * 0.5f, g.capY1 = g.yc + capH * 0.5f;
  return g;
}

// A slider's centre line across its travel (the rect's middle), snapped for a track `trackW` wide.
inline float sliderCentre(float a, float b, float trackW) { return snapCentre((a + b) * 0.5f, trackW); }

}  // namespace gl::look::geom
