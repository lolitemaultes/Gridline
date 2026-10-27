// Gridline - J-137: layout of the Settings > Theme page's control style cards
// (SettingsKeysTheme.cpp), pure geometry so tests can check it headless.
//
// Rules: a row holds 5, 3, 2 or 1 cards, the most that keep each card at least
// kCardMinW wide; a card never draws outside its own rectangle (the selection
// ring included); the preview group (knob, fader, meter at their real sizes)
// is centred in the card and keeps its size and proportions; it only shrinks,
// uniformly, if a card is narrower than the group; nothing depends on time,
// scroll position or the previous frame.
#pragma once
#include <algorithm>
#include <cmath>

namespace gl::themelayout {

constexpr float kCardMinW = 150.0f, kCardH = 132.0f, kCardTextH = 40.0f, kCardRing = 3.0f;
constexpr float kPrevKnobR = 18.0f, kPrevFaderW = 22.0f, kPrevMeterW = 11.0f, kPrevGap = 18.0f, kPrevPad = 8.0f;
constexpr float kPrevGroupW = 2 * kPrevKnobR + kPrevGap + kPrevFaderW + kPrevGap + kPrevMeterW;
// A styled fader draws 8 px beside and 4 px above / below the control (ui::DrawFader).
constexpr float kFaderMarginX = 8.0f, kFaderMarginY = 4.0f, kPrevInsetY = 14.0f;

struct Rect {
  float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  bool inside(const Rect& o) const { return x0 >= o.x0 && y0 >= o.y0 && x1 <= o.x1 && y1 <= o.y1; }
};

// Cards per row for a row `avail` wide with `gap` between cards.
inline int cardsPerRow(float avail, float gap) {
  for (int n : {5, 3, 2})
    if (avail >= static_cast<float>(n) * kCardMinW + static_cast<float>(n - 1) * gap) return n;
  return 1;
}
inline float cardWidth(float avail, float gap) {
  const int n = cardsPerRow(avail, gap);
  return std::floor((avail - gap * static_cast<float>(n - 1)) / static_cast<float>(n));
}

// Everything a card draws, for a card at (x, y), w x h.
struct Card {
  Rect card, preview, ring, title, knob, fader, meter;
  float scale = 1;
};
inline Card card(float x, float y, float w, float h) {
  Card c;
  c.card = {x, y, x + w, y + h};
  const float ph = h - kCardTextH;
  c.ring = {x, y, x + w, y + ph};
  c.preview = {x + kCardRing, y + kCardRing, x + w - kCardRing, y + ph - kCardRing};
  const float pw = c.preview.x1 - c.preview.x0;
  c.scale = std::clamp((pw - 2 * kPrevPad) / kPrevGroupW, 0.0f, 1.0f);
  const float s = c.scale;
  const float gx = std::round(c.preview.x0 + (pw - kPrevGroupW * s) * 0.5f);
  const float top = c.preview.y0 + kPrevInsetY, bot = c.preview.y1 - kPrevInsetY;
  const float mid = std::round((c.preview.y0 + c.preview.y1) * 0.5f);
  c.knob = {gx, mid - kPrevKnobR * s, gx + 2 * kPrevKnobR * s, mid + kPrevKnobR * s};
  const float fx = std::round(gx + (2 * kPrevKnobR + kPrevGap) * s);
  c.fader = {fx, top, fx + kPrevFaderW * s, bot};
  const float mx = std::round(fx + (kPrevFaderW + kPrevGap) * s);
  c.meter = {mx, top, mx + kPrevMeterW * s, bot};
  c.title = {x + 2, y + ph + 4, x + w - 2, y + h};
  return c;
}

}  // namespace gl::themelayout
