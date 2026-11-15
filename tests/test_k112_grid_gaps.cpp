// Gridline - K-112c: the Live layout's session grid keeps one gap between neighbouring cells:
// across columns, between slot rows, under the titles and above the stop row, all kGap made a
// whole number of device pixels, with every edge on a device pixel, whatever the panel's origin
// (fractional window positions at 1280, 1440 and 1920 wide screens, scrolled or not) and at the
// interface scales 100 %, 125 %, 150 % and 200 % (1, 1.25, 1.5 and 2 device px per logical px).
#include <cmath>
#include <cstdio>
#include "Check.h"
#include "app/LiveGrid.h"

using namespace gl::livegrid;

namespace {
float dppNow = 1.0f;
bool near(float a, float b) { return std::fabs(a - b) < 1e-3f; }
bool onDevice(float v) { return near(v * dppNow, std::round(v * dppNow)); }
bool wholeCell(const Cell& c) { return onDevice(c.x0) && onDevice(c.y0) && onDevice(c.x1) && onDevice(c.y1); }
}  // namespace

int main() {
  const float origins[][2] = {{308, 60}, {308.5f, 60.25f}, {307.4f, 59.6f}, {612.75f, 83.5f}, {1.0f / 3.0f, 2.0f / 3.0f}, {955.49f, 401.51f}};
  const float scales[] = {1.0f, 1.25f, 1.5f, 2.0f};
  constexpr int kCols = 12, kRows = 9;
  for (const float dpp : scales) {
    dppNow = dpp;
    const float gap = gl::pixelgrid::whole(kGap, dpp);  // kGap as whole device px
    const float cellW = gl::pixelgrid::whole(kColW, dpp) - gap, cellH = gl::pixelgrid::whole(kSlotH, dpp) - gap;
    for (const auto& o : origins) {
      bool edges = true, cols = true, rows = true, titles = true, stop = true, sizes = true;
      for (int t = 0; t < kCols; ++t) {
        const Cell tt = title(o[0], o[1], t, dpp), first = slot(o[0], o[1], t, 0, dpp);
        edges = edges && wholeCell(tt);
        titles = titles && near(first.y0 - tt.y1, gap) && near(tt.x0, first.x0) && near(tt.x1, first.x1);
        for (int k = 0; k <= kRows; ++k) {  // k == kRows: the stop row
          const Cell c = slot(o[0], o[1], t, k, dpp);
          edges = edges && wholeCell(c);
          sizes = sizes && near(c.x1 - c.x0, cellW) && near(c.y1 - c.y0, cellH);
          if (t + 1 < kCols) cols = cols && near(slot(o[0], o[1], t + 1, k, dpp).x0 - c.x1, gap);
          if (k + 1 < kRows) rows = rows && near(slot(o[0], o[1], t, k + 1, dpp).y0 - c.y1, gap);
          if (k == kRows) stop = stop && near(c.y0 - slot(o[0], o[1], t, k - 1, dpp).y1, gap);
        }
      }
      char name[200];
      const double ox = static_cast<double>(o[0]), oy = static_cast<double>(o[1]), sc = static_cast<double>(dpp);
      std::snprintf(name, sizeof name, "scale %.2f origin %.2f,%.2f: every edge on a device pixel", sc, ox, oy);
      expect(edges, name);
      std::snprintf(name, sizeof name, "scale %.2f origin %.2f,%.2f: column gaps are one width", sc, ox, oy);
      expect(cols, name);
      std::snprintf(name, sizeof name, "scale %.2f origin %.2f,%.2f: row gaps are one width", sc, ox, oy);
      expect(rows, name);
      std::snprintf(name, sizeof name, "scale %.2f origin %.2f,%.2f: the gap under the titles is the same", sc, ox, oy);
      expect(titles, name);
      std::snprintf(name, sizeof name, "scale %.2f origin %.2f,%.2f: the gap above the stop row is the same", sc, ox, oy);
      expect(stop, name);
      std::snprintf(name, sizeof name, "scale %.2f origin %.2f,%.2f: every cell is the same size", sc, ox, oy);
      expect(sizes, name);
    }
    char name[120];
    std::snprintf(name, sizeof name, "scale %.2f: the gap is at least one device pixel", static_cast<double>(dpp));
    expect(gap * dpp >= 1.0f - 1e-3f, name);
  }
  expect(kColW - (kColW - kGap) == kSlotH - kCellH, "the column gap equals the row gap");
  return finish();
}
