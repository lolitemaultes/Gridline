// Gridline - J-137: Settings > Theme control style cards (app/ThemeLayout.h).
// For every page width from very narrow to very wide: the cards of a row fit
// the row, wrap to fewer columns before they get narrower than the minimum, and
// everything a card draws (selection ring, preview, knob, fader with its styled
// margin, meter, name) stays inside the card. The preview group keeps its real
// size and proportions on every card at least the minimum wide, is centred, and
// the geometry is the same every time it is asked for (no frame state).
#include <cmath>
#include <cstdio>
#include "Check.h"
#include "app/ThemeLayout.h"

using namespace gl::themelayout;

int main() {
  const float gap = 12.0f;
  bool rowsFit = true, minKept = true, wraps = true, inside = true, real = true, centred = true, stable = true, aspect = true;
  int lastPerRow = 5;
  for (float avail = 1200.0f; avail >= 120.0f; avail -= 1.0f) {
    const int n = cardsPerRow(avail, gap);
    const float w = cardWidth(avail, gap);
    if (static_cast<float>(n) * w + static_cast<float>(n - 1) * gap > avail + 0.01f) rowsFit = false;
    if (avail >= kCardMinW && w < kCardMinW) minKept = false;
    if (n > lastPerRow) wraps = false;  // narrower never means more columns
    lastPerRow = n;
    const Card c = card(10.0f, 20.0f, w, kCardH);
    const Rect faderDrawn{c.fader.x0 - kFaderMarginX, c.fader.y0 - kFaderMarginY, c.fader.x1 + kFaderMarginX,
                          c.fader.y1 + kFaderMarginY};
    for (const Rect& r : {c.ring, c.preview, c.knob, faderDrawn, c.meter, c.title})
      if (!r.inside(c.card)) inside = false;
    for (const Rect& r : {c.knob, faderDrawn, c.meter})
      if (!r.inside(c.preview)) inside = false;
    if (w >= kCardMinW) {
      if (c.scale != 1.0f) real = false;
      if (std::fabs((c.knob.x1 - c.knob.x0) - 2 * kPrevKnobR) > 0.01f || std::fabs((c.meter.x1 - c.meter.x0) - kPrevMeterW) > 0.01f) real = false;
      const float left = c.knob.x0 - c.preview.x0, right = c.preview.x1 - c.meter.x1;
      if (std::fabs(left - right) > 1.01f) centred = false;
    }
    // Uniform scale: the knob stays round and the fader / meter keep their width ratio.
    if (std::fabs((c.knob.x1 - c.knob.x0) - (c.knob.y1 - c.knob.y0)) > 0.01f) aspect = false;
    if (std::fabs((c.fader.x1 - c.fader.x0) * kPrevMeterW - (c.meter.x1 - c.meter.x0) * kPrevFaderW) > 0.05f) aspect = false;
    const Card again = card(10.0f, 20.0f, w, kCardH);
    if (again.knob.x0 != c.knob.x0 || again.meter.x1 != c.meter.x1 || again.ring.y1 != c.ring.y1) stable = false;
  }
  expect(rowsFit, "a row of cards never exceeds the page width");
  expect(minKept, "cards stay at least the minimum width (they wrap instead)");
  expect(wraps, "a narrower page never shows more columns");
  expect(cardsPerRow(5 * kCardMinW + 4 * gap, gap) == 5 && cardsPerRow(5 * kCardMinW + 4 * gap - 1, gap) == 3 &&
             cardsPerRow(2 * kCardMinW + gap - 1, gap) == 1,
         "5, then 3, 2 and 1 per row");
  expect(inside, "ring, preview, knob, fader (with its styled margin), meter and name stay inside the card");
  expect(real, "previews show the controls at their real size on cards at least the minimum wide");
  expect(centred, "the preview group is centred in the card");
  expect(aspect, "a narrow card scales the preview uniformly (round knob, same proportions)");
  expect(stable, "the geometry depends on the card only (no frame state)");
  return finish();
}
