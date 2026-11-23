// Gridline — the side-by-side mixer's zones (MixerLayout.h): the master zone spans every row, the
// strips zone keeps to its content, and the inspector takes the rest or the width it was dragged to.
#include <cmath>
#include "Check.h"
#include "../src/core/MixerLayout.h"

using namespace gl;

int main() {
  // Strips per row.
  expect(mixlayout::perRow(16, 1) == 16, "one row holds every strip");
  expect(mixlayout::perRow(16, 2) == 8, "two rows split the strips");
  expect(mixlayout::perRow(15, 2) == 8, "an odd count rounds up");
  expect(mixlayout::perRow(0, 3) == 0, "no strips, no columns");
  expect(mixlayout::perRow(5, 9) == 2, "rows are at most four");

  // Content width: the master zone, the strips of one row, the + button.
  mixlayout::WideInput in;
  in.left = 1, in.middle = 16, in.rows = 2, in.stripW = 66, in.masterW = 90, in.gap = 2, in.dividerW = 12, in.addW = 40, in.padding = 8, in.tail = 8;
  const float two = mixlayout::wideContentW(in);
  expect(std::fabs(two - (90 + 12 + 8 * 68 - 2 + 2 + 40 + 8 + 16)) < 0.01f, "content width of two rows of eight");
  in.rows = 1;
  const float one = mixlayout::wideContentW(in);
  expect(std::fabs(one - two - 8 * 68) < 0.01f, "one row is eight strips wider");
  in.right = 2;
  expect(std::fabs(mixlayout::wideContentW(in) - one - 2 * 68) < 0.01f, "the right dock adds its strips");
  in.right = 0, in.left = 0;
  expect(std::fabs(one - mixlayout::wideContentW(in) - 102) < 0.01f, "no master zone, no divider");

  // The inspector: automatic width when the strips fill the mixer.
  expect(mixlayout::wideInspectorW(1000, 2000, 0) == 270, "automatic: 27 percent");
  expect(mixlayout::wideInspectorW(2000, 4000, 0) == 320, "automatic: at most 320");
  expect(mixlayout::wideInspectorW(600, 4000, 0) == 240, "automatic: at least 240");
  // Few strips: the inspector takes the rest, nothing empty between them.
  expect(mixlayout::wideInspectorW(1300, 680, 0) == 620, "few strips: the inspector takes the rest");
  expect(mixlayout::wideInspectorW(1300, 680, 0) + 680 == 1300, "strips and inspector fill the mixer");
  // Dragged: its own width, the strips then scroll.
  expect(mixlayout::wideInspectorW(1300, 2000, 500) == 500, "a dragged width is kept");
  expect(mixlayout::wideInspectorW(1300, 2000, 100) == 240, "a dragged width is at least the minimum");
  expect(mixlayout::wideInspectorW(1300, 2000, 1290) == 1300 - mixlayout::kStripsMinW, "a dragged width leaves the strips some room");
  expect(mixlayout::wideInspectorW(1300, 680, 300) == 620, "a narrow drag still leaves no empty space");

  // The master zone's height: every row, band and gap.
  expect(mixlayout::masterZoneH(1, 400, 14, 6) == 400, "one row: one strip high");
  expect(mixlayout::masterZoneH(2, 400, 14, 6) == 820, "two rows: both rows and the band and gap between");
  expect(mixlayout::masterZoneH(9, 100, 0, 6) == 418, "at most four rows");
  return finish();
}
