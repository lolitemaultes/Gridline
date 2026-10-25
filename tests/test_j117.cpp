// Gridline — J-117: the elastic ("rubber") boundary of windows floating inside the main window
// (core/WindowRubber.h):
//  * a window rests fully inside the work area; one larger than it keeps its top-left in;
//  * dragged past an edge it follows with growing resistance, never further out than J-67's
//    elastic bound, monotonic in the drag, and 1:1 inside;
//  * let go, it springs back inside, the same way at 30, 60, 144 and 240 frames a second, within
//    a quarter of a second or so, never overshooting the edge;
//  * elastic edges off: a hard edge, no spring.
// The app (dragging the channel rack, the pop-out button, docking) is tests/j117_rubber.cmake.
#include <cmath>
#include <cstdio>
#include "Check.h"
#include "core/WindowRubber.h"

namespace rubber = gl::rubber;

namespace {

// The 1440x900 main window's work area (top bar 0..48, status bar at the bottom).
constexpr float kA0 = 48, kA1 = 876;

void testRange() {
  const rubber::Range r = rubber::range(0, 1440, 700);
  expect(r.lo == 0 && r.hi == 740, "range: the top-left of a 700 px window in 1440 px rests in [0, 740]");
  const rubber::Range big = rubber::range(0, 1440, 1600);
  expect(big.lo == 0 && big.hi == 0, "range: a window larger than the area keeps its start edge on the area's start");
  expect(rubber::rest(-120, r) == 0 && rubber::rest(900, r) == 740 && rubber::rest(300, r) == 300, "rest: clamped into the range, untouched inside");
  expect(rubber::rest(-50, big) == 0 && rubber::rest(50, big) == 0, "rest: an oversized window's title bar and top-left stay reachable");
}

void testDrag() {
  const rubber::Params p;
  const rubber::Range r = rubber::range(kA0, kA1, 400);
  expect(rubber::drag(300, r, p) == 300, "drag inside: 1:1");
  float prev = r.hi;
  bool monotonic = true, bounded = true, resisting = true;
  for (int s = 1; s <= 4000; s += 7) {
    const float d = rubber::drag(r.hi + static_cast<float>(s), r, p);
    if (d < prev) monotonic = false;
    if (d - r.hi >= static_cast<float>(p.maxOver)) bounded = false;
    if (d - r.hi > static_cast<float>(s)) resisting = false;
    prev = d;
  }
  expect(monotonic, "drag past an edge: the window still follows the pointer (monotonic)");
  expect(bounded, "drag past an edge: never further out than the elastic bound (48 px)");
  expect(resisting, "drag past an edge: always less out than the pointer is");
  const float o100 = rubber::drag(r.hi + 100, r, p) - r.hi, o400 = rubber::drag(r.hi + 400, r, p) - r.hi;
  char label[160];
  std::snprintf(label, sizeof label, "growing resistance: 100 px past gives %.1f px, 400 px past only %.1f px", static_cast<double>(o100),
                static_cast<double>(o400));
  expect(o100 > 15 && o100 < 40 && o400 > o100 && o400 - o100 < o100, label);
  const float up = rubber::drag(r.lo - 300, r, p);
  expect(up < r.lo && up > r.lo - static_cast<float>(p.maxOver), "past the start edge (the top bar, the left edge): the same, mirrored");
  rubber::Params hard;
  hard.elastic = false;
  expect(rubber::drag(r.hi + 300, r, hard) == r.hi && rubber::drag(r.lo - 300, r, hard) == r.lo, "elastic edges off: a hard edge");
}

// Frames of `fps` until the spring from `from` snaps onto `to`; the largest step past `to`.
int settle(float from, float to, double fps, float& overshoot) {
  const rubber::Params p;
  float v = from;
  overshoot = 0;
  for (int f = 1; f <= 10000; ++f) {
    v = rubber::spring(v, to, 1.0 / fps, p);
    overshoot = std::fmax(overshoot, (from > to) ? to - v : v - to);
    if (v == to) return f;
  }
  return -1;
}

void testSpring() {
  const rubber::Params p;
  float over = 0;
  const int f60 = settle(740 + 40, 740, 60, over);
  char label[160];
  std::snprintf(label, sizeof label, "spring back: 40 px out settles in %d frames at 60 fps (%.0f ms)", f60, f60 * 1000.0 / 60.0);
  expect(f60 > 3 && f60 <= 20, label);
  expect(over <= 0, "spring back: never overshoots the edge");
  float o2 = 0;
  expect(settle(48 - 40, 48, 60, o2) > 3 && o2 <= 0, "spring back from above the top bar: the same");
  // Frame-rate independence: the time to settle is the same at 30 / 60 / 144 / 240 fps (one frame's slack).
  const double t60 = f60 / 60.0;
  bool same = true;
  for (double fps : {30.0, 144.0, 240.0}) {
    float o = 0;
    const double t = settle(780, 740, fps, o) / fps;
    if (std::fabs(t - t60) > 1.0 / 30.0 + 1e-9) same = false;
  }
  expect(same, "spring back: the same duration at 30, 60, 144 and 240 frames a second");
  // Half way in time is half way in log distance, whatever the frame split.
  float a = 780, b = 780;
  a = rubber::spring(a, 740, 0.05, p);
  for (int i = 0; i < 5; ++i) b = rubber::spring(b, 740, 0.01, p);
  expect(std::fabs(a - b) < 0.01f, "one 50 ms frame moves it exactly as far as five 10 ms frames");
  expect(rubber::spring(780, 740, 0, p) == 780, "no time passed: not moved");
  rubber::Params hard;
  hard.elastic = false;
  expect(rubber::spring(780, 740, 1.0 / 60, hard) == 740, "elastic edges off: back at once");
  const double st = rubber::settleTime(40, p);
  expect(st > 0.15 && st < 0.3, "settle time from 40 px: about a quarter of a second");
}

}  // namespace

int main() {
  testRange();
  testDrag();
  testSpring();
  return finish();
}
