// Gridline: K-248, the shared warp waveform's geometry (app/WarpWaveGeom.h): the marker under the
// pointer at three zooms (the playlist's rule: the nearest within the grab width plus a pixel),
// markers outside the visible part, and the section labels (percent, changed, fits).
#include <string>
#include <vector>
#include "Check.h"
#include "app/WarpWaveGeom.h"

using namespace gl::warpwave;

namespace {

// Markers at 0, 1, 2 and 3 seconds, at `pxPerSec`.
std::vector<Mark> marks(float pxPerSec, float x0) {
  std::vector<Mark> v;
  for (uint32_t i = 0; i < 4; ++i) v.push_back({i + 1, x0 + pxPerSec * static_cast<float>(i), 1.0});
  return v;
}

void testZoom(float pxPerSec) {
  const std::vector<Mark> v = marks(pxPerSec, 100.0f);
  const std::string z = std::to_string(static_cast<int>(pxPerSec)) + " px/s: ";
  const float m2 = 100.0f + pxPerSec;  // the second marker
  const float cx = static_cast<float>(static_cast<int>(m2)) + 0.5f;
  expect(hitTest(v, cx, 0, 2000) == 2, (z + "on the marker").c_str());
  expect(hitTest(v, cx + kGrab, 0, 2000) == 2, (z + "at the grab width").c_str());
  expect(hitTest(v, cx - kGrab - 0.5f, 0, 2000) == 2, (z + "within the grab width plus a pixel").c_str());
  expect(hitTest(v, cx + kGrab + 1.5f, 0, 2000) == 0 || pxPerSec < 2 * (kGrab + 2), (z + "beyond: none").c_str());
  expect(hitTest(v, cx, cx + 50, 2000) == 0, (z + "left of the visible part: none").c_str());
}

void testCrowded() {
  // Two markers 3 px apart: the nearer one wins.
  const std::vector<Mark> v = {{1, 100.0f, 1.0}, {2, 103.0f, 1.0}};
  expect(hitTest(v, 100.6f, 0, 500) == 1 && hitTest(v, 103.4f, 0, 500) == 2, "crowded markers: the nearer one");
}

void testLabels() {
  expect(sectionPercent(2.0) == 50.0 && sectionPercent(0.5) == 200.0, "a section's percent: its length against the original");
  expect(!sectionChanged(1.0) && !sectionChanged(1.0005) && sectionChanged(1.01), "a section changed past a tenth of a percent");
  expect(labelFits(60, 30, 40, 12) && !labelFits(35, 30, 40, 12) && !labelFits(60, 30, 15, 12), "the label shows only where it fits");
}

}  // namespace

int main() {
  testZoom(12.0f);   // zoomed out
  testZoom(120.0f);  // the default
  testZoom(1200.0f); // zoomed in
  testCrowded();
  testLabels();
  return finish();
}
