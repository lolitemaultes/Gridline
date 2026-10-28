// Gridline — J-113: detachable windows, the placement rules (core/WindowPlacement.h):
//  * a saved window whose title bar is still on a monitor stays exactly where it was;
//  * one saved on a monitor that is gone (unplugged, resolution lowered) comes back fitted
//    into the nearest monitor, shrunk if it no longer fits;
//  * positions saved relative to the main window are re-anchored when the main window
//    starts elsewhere or the interface scale changed (the OS position is kept);
//  * a window dragged outside keeps a grabbable part of its title bar on some monitor;
//  * remembered positions belong to the monitor holding them (plugin editors);
//  * the layout.ini [GridlineViewports] lines are read strictly;
//  * "Bring all windows back" cascades windows inside the main window's work area.
//  * J-116: when a frame is drawn from SDL's event watch while a window is resized (core/LiveResize.h).
// The real application (detach, resize, re-dock, focus, drops) is tests/j113_detach.cmake.
#include <cmath>
#include <string>
#include <vector>
#include "Check.h"
#include "core/LiveResize.h"
#include "core/WindowPlacement.h"

using gl::place::Rect;
namespace place = gl::place;
namespace live = gl::live;

namespace {
bool same(const Rect& a, const Rect& b) {
  return std::fabs(a.x - b.x) < 0.01f && std::fabs(a.y - b.y) < 0.01f && std::fabs(a.w - b.w) < 0.01f && std::fabs(a.h - b.h) < 0.01f;
}
bool inside(const Rect& r, const Rect& m) { return r.x >= m.x - 0.01f && r.y >= m.y - 0.01f && r.right() <= m.right() + 0.01f && r.bottom() <= m.bottom() + 0.01f; }

// Two monitors side by side (the second taller, offset upwards, as macOS arranges them).
const Rect kLeft{0, 0, 1440, 900};
const Rect kRight{1440, -200, 1920, 1200};
const std::vector<Rect> kTwo{kLeft, kRight};

void testRecover() {
  const Rect onRight{2000, 100, 900, 400};
  expect(same(place::recoverOnScreen(onRight, kTwo), onRight), "a window on the second monitor stays where it was");
  const Rect straddling{1200, 300, 600, 400};
  expect(same(place::recoverOnScreen(straddling, kTwo), straddling), "a window across both monitors stays where it was");
  const Rect partlyOff{-500, 200, 600, 400};  // 100 px of its title bar still on the left monitor
  expect(same(place::recoverOnScreen(partlyOff, kTwo), partlyOff), "a window with a grabbable title bar is left alone");

  // The second monitor unplugged: the window comes back onto the remaining one.
  const std::vector<Rect> one{kLeft};
  const Rect back = place::recoverOnScreen(onRight, one);
  expect(inside(back, kLeft), "monitor gone: the window comes back fully on the remaining monitor");
  expect(back.w == onRight.w && back.h == onRight.h, "monitor gone: its size is kept when it fits");
  const Rect huge{3000, 0, 1800, 1100};
  const Rect shrunk = place::recoverOnScreen(huge, one);
  expect(inside(shrunk, kLeft) && shrunk.w == kLeft.w && shrunk.h == kLeft.h, "monitor gone: a window larger than the monitor is shrunk to it");
  const Rect far{-5000, -5000, 400, 300};
  expect(inside(place::recoverOnScreen(far, kTwo), kLeft), "far off every monitor: fitted into the nearest one");
  const Rect aboveRight{2400, -900, 400, 300};
  expect(inside(place::recoverOnScreen(aboveRight, kTwo), kRight), "above the second monitor: fitted into the second monitor");
  const Rect titleOff{100, -40, 400, 300};  // its title bar above the top edge: not grabbable
  const Rect fixedTitle = place::recoverOnScreen(titleOff, kTwo);
  expect(fixedTitle.y == 0 && fixedTitle.x == 100, "title bar above the monitor: moved down just enough");
  expect(same(place::recoverOnScreen(onRight, {}), onRight), "no monitor information: unchanged");
}

void testKeepTitle() {
  const Rect r{3500, 300, 600, 400};  // dragged past the right edge of the second monitor
  const Rect k = place::keepTitleOnMonitors(r, kTwo);
  expect(k.right() - place::kGrabW >= kRight.x && k.x <= kRight.right() - place::kGrabW + 0.01f, "dragged past the last monitor: a grab width of title bar stays on it");
  expect(k.w == r.w && k.h == r.h, "keeping the title bar never resizes the window");
  const Rect low{200, 2000, 600, 400};
  const Rect kl = place::keepTitleOnMonitors(low, kTwo);
  expect(place::titleReachable(kl, kTwo), "dragged below every monitor: the title bar comes back on one");
  const Rect fine{1300, 100, 600, 400};
  expect(same(place::keepTitleOnMonitors(fine, kTwo), fine), "between monitors with a reachable title bar: unchanged");
}

void testReanchor() {
  // Saved at x=1600 (ImGui units) with the main window at OS (240, 58), scale 1.
  float x = 1600, y = 120;
  place::reanchor(x, y, 240, 58, 1.0f, 240, 58, 1.0f);
  expect(x == 1600 && y == 120, "same main window and scale: unchanged");
  x = 1600, y = 120;
  place::reanchor(x, y, 240, 58, 1.0f, 340, 8, 1.0f);  // main window moved 100 right, 50 up
  expect(x == 1500 && y == 170, "main window started elsewhere: the detached window keeps its OS position");
  x = 1600, y = 120;
  place::reanchor(x, y, 0, 0, 1.0f, 0, 0, 2.0f);  // interface scale doubled
  expect(x == 800 && y == 60, "interface scale doubled: the same OS position in half the units");
  x = 100, y = 100;
  place::reanchor(x, y, 0, 0, 0.0f, 0, 0, -1.0f);
  expect(x == 100 && y == 100, "invalid scales count as 1");
}

void testMonitorFor() {
  expect(place::monitorFor({2000, 100, 300, 200}, kTwo) == 1, "a window on the second monitor belongs to it");
  expect(place::monitorFor({1300, 100, 400, 200}, kTwo) == 1, "a window mostly on the second monitor belongs to it");
  expect(place::monitorFor({-900, 100, 300, 200}, kTwo) == 0, "off every monitor: the nearest");
  expect(place::monitorAt(2000, -100, kTwo) == 1, "a remembered point above the left monitor's top, on the second: the second");
  expect(place::monitorAt(10, 10, kTwo) == 0, "a remembered point on the left monitor: the left");
  expect(place::monitorFor({0, 0, 10, 10}, {}) == -1, "no monitors: none");
}

void testIniLines() {
  place::SavedMain m;
  expect(!m.have && m.scale == 1, "nothing read: no anchor, scale 1");
  expect(place::readSavedMainLine("MainOs=240,58", m) && m.have && m.x == 240 && m.y == 58, "MainOs read");
  expect(place::readSavedMainLine("MainOs=-1920,-200", m) && m.x == -1920 && m.y == -200, "negative positions (a monitor left of the main one)");
  expect(place::readSavedMainLine("Scale=1.2500", m) && m.scale == 1.25f, "Scale read");
  place::SavedMain n;
  expect(!place::readSavedMainLine("Scale=0", n) && n.scale == 1, "a zero scale is ignored");
  expect(!place::readSavedMainLine("Scale=nan", n) && n.scale == 1, "a nan scale is ignored");
  expect(!place::readSavedMainLine("MainOs=12", n) && !n.have, "half a position is ignored");
  expect(!place::readSavedMainLine("MainOs=1,2x", n) && !n.have, "trailing garbage is ignored");
  expect(!place::readSavedMainLine("Future=1", n), "unknown lines are ignored");
}

void testCascade() {
  const Rect area{0, 48, 1440, 826};
  const Rect a = place::cascade(0, 900, 380, area), b = place::cascade(1, 900, 380, area);
  expect(inside(a, area) && inside(b, area), "brought-back windows are inside the work area");
  expect(b.x > a.x && b.y > a.y, "brought-back windows cascade");
  const Rect big = place::cascade(3, 3000, 2000, area);
  expect(same(big, area), "a window larger than the main window is fitted to it");
  expect(same(place::cascade(10, 900, 380, area), a), "the cascade wraps after ten windows");
}
// J-114: "Pop out into its own window" without a remembered place.
void testPopOutPlace() {
  const Rect mainHalf{100, 100, 800, 600};  // room on its right on the same monitor
  const Rect r = place::popOutPlace(400, 500, 150, mainHalf, kTwo);
  expect(r.x == mainHalf.right() + 24 && r.y == 150 && r.w == 400 && r.h == 500, "room on the right: beside the main window, same height");
  const Rect straddle = place::popOutPlace(400, 500, 150, Rect{100, 100, 1000, 700}, kTwo);
  expect(inside(straddle, kRight), "beside it would straddle two monitors: on the other monitor instead");
  const Rect onlyLeft{600, 100, 800, 600};
  const Rect l = place::popOutPlace(300, 400, 150, onlyLeft, {kLeft});
  expect(l.x == onlyLeft.x - 24 - 300 && l.y == 150, "no room on the right: beside it on the left");
  const Rect tall = place::popOutPlace(300, 800, 400, onlyLeft, {kLeft});
  expect(tall.x == onlyLeft.x - 24 - 300 && tall.y >= 32 && tall.bottom() <= kLeft.bottom(), "too low to fit: moved up on the same side");
  const Rect full{0, 0, 1440, 900};  // the main window fills its monitor
  const Rect second = place::popOutPlace(600, 400, 60, full, kTwo);
  expect(inside(second, kRight) && second.y >= kRight.y + 32, "main window fills its monitor: on the second monitor");
  const Rect alone = place::popOutPlace(600, 400, 60, full, {kLeft});
  expect(inside(alone, kLeft) && alone.x == 48 && alone.y == 80, "one monitor, filled: offset from the main window's corner");
}

// J-116: when SDL's event watch draws a frame from inside the event pump (live resize).
void testLiveResizeGate() {
  using live::Kind;
  using live::shouldDraw;
  // macOS live resize / Windows' size loop: every size step that changes the size draws at once.
  expect(shouldDraw(Kind::Resize, true, false, true, 0.0, true, 0.0, 0.0), "modal loop: a new size draws a frame at once");
  expect(!shouldDraw(Kind::Resize, true, false, true, 0.2, false, 0.001, 0.001), "SIZE_CHANGED then RESIZED of one step: one frame, not two");
  expect(!shouldDraw(Kind::Expose, true, false, true, 0.2, false, 0.001, 0.001), "the view's EXPOSED right after the step's frame: no second frame");
  expect(shouldDraw(Kind::Expose, true, false, true, 0.2, false, 0.017, 0.017), "the 60 Hz live-resize timer (held still): keeps drawing");
  expect(shouldDraw(Kind::Move, true, false, true, 0.2, false, 0.020, 0.020), "Windows' modal move loop (10 ms timer): every other tick draws");
  expect(!shouldDraw(Kind::Expose, true, false, true, 0.2, false, 0.008, 0.008), "the live-resize timer between two drag steps: no extra frame");
  // Never from inside a frame, never outside the pump.
  expect(!shouldDraw(Kind::Resize, true, true, true, 0.2, true, 1.0, 1.0), "inside a frame (re-entrancy): never");
  expect(!shouldDraw(Kind::Resize, false, false, true, 0.2, true, 1.0, 1.0), "outside the event pump: never");
  // X11 / Wayland / the BSDs: no modal loop, the pump returns and the main loop draws the new size.
  expect(!shouldDraw(Kind::Resize, true, false, false, 0.001, true, 0.016, 0.016), "non-blocking pump: the main loop draws it (no double frame)");
  expect(!shouldDraw(Kind::Expose, true, false, false, 0.001, false, 1.0, 1.0), "non-blocking pump: an expose waits for the main loop");
  // A pump blocked by a loop the platform does not report still redraws.
  expect(shouldDraw(Kind::Resize, true, false, false, 0.08, true, 0.0, 0.0), "pump blocked for 80 ms: draws");
  expect(!shouldDraw(Kind::Resize, true, false, false, 0.03, true, 0.0, 0.0), "pump busy for 30 ms: not blocked yet");
  // A slow frame (130 ms: a loaded machine, software GL): the step's own later events, arriving as
  // that frame ends, draw nothing more; the timer a moment later (held still) still draws.
  expect(!shouldDraw(Kind::Resize, true, false, true, 0.2, false, 0.130, 0.0002), "slow frame: the step's RESIZED right after it: no second frame");
  expect(!shouldDraw(Kind::Expose, true, false, true, 0.2, false, 0.131, 0.0004), "slow frame: the step's EXPOSED right after it: no second frame");
  expect(shouldDraw(Kind::Expose, true, false, true, 0.2, false, 0.147, 0.017), "slow frame: the 60 Hz timer once it has ended: draws");
  expect(shouldDraw(Kind::Resize, true, false, true, 0.2, true, 0.130, 0.0001), "slow frame: the next step's new size: draws at once");
}

void testSplitAndIni() {
  expect(place::splitRatioFor(0, 0.3f) == 0.3f && place::splitRatioFor(2, 0.3f) == 0.3f, "left / up: its share is the first child's");
  expect(std::fabs(place::splitRatioFor(1, 0.3f) - 0.7f) < 1e-6f && std::fabs(place::splitRatioFor(3, 0.25f) - 0.75f) < 1e-6f,
         "right / down: the first child keeps the rest");
  expect(place::splitRatioFor(1, 2.0f) >= 0.05f && place::splitRatioFor(0, -1.0f) == 0.05f, "shares are clamped");
  place::Home h;
  h.have = h.docked = true, h.node = 0xABCD1234u, h.parent = 0x00000042u, h.dir = 1, h.share = 0.25f;
  place::Home back;
  std::string line = place::homeLines(h);
  line.pop_back();
  expect(place::readHomeLine(line.c_str(), back) && back.docked && back.node == h.node && back.parent == h.parent && back.dir == 1 &&
             std::fabs(back.share - 0.25f) < 1e-4f,
         "a docked home round-trips through layout.ini");
  place::Home f;
  f.have = true, f.floating = Rect{306, 182, 700, 408};
  place::Home fb;
  line = place::homeLines(f);
  line.pop_back();
  expect(place::readHomeLine(line.c_str(), fb) && !fb.docked && same(fb.floating, f.floating), "a floating home round-trips");
  place::Home bad;
  expect(!place::readHomeLine("Dock=0x1,0x2,7,0.5", bad) && !bad.have, "an unknown side is ignored");
  expect(!place::readHomeLine("Float=1,2,0,4", bad) && !place::readHomeLine("Dock=0x1", bad), "half lines are ignored");
  Rect r;
  expect(place::readOutLine("Rect=1700,150,335,810", r) && same(r, Rect{1700, 150, 335, 810}), "Out read");
  expect(!place::readOutLine("Rect=1,2,0,4", r) && !place::readOutLine("Rect=1,2,3,4x", r), "Out: empty or trailing garbage ignored");
  const Rect high{1500, -190, 400, 300};
  expect(place::keepOsTitleOnMonitor(high, kTwo, 32).y == kRight.y + 32, "placed outside: room for the OS title bar above it");
}

// J-158: a plugin editor's window in the work area: natural where there is room, a reflowing
// editor narrows (not below its minimum), the rest is capped and scrolls; never taller.
void testFitEditor() {
  using place::fitEditor;
  const auto f = fitEditor(1120, 0, 38, 1904, 990);
  expect(f.content == 1120 && f.window == 1158 && !f.scrollX && f.maxH == 990, "room: natural width, no scrolling");
  const auto g = fitEditor(1120, 0, 38, 944, 510);
  expect(g.content == 1120 && g.window == 944 && g.scrollX && g.maxH == 510, "fixed layout wider than the area: capped, scrolls sideways");
  const auto r = fitEditor(1240, 940, 38, 1264, 710);
  expect(r.content == 1226 && r.window == 1264 && !r.scrollX, "reflowing editor narrows to fit");
  const auto m = fitEditor(1240, 940, 38, 944, 510);
  expect(m.content == 940 && m.window == 944 && m.scrollX, "reflowing editor stops at its minimum, then scrolls");
  const auto e = fitEditor(1240, 940, 38, 1278, 900);
  expect(e.content == 1240 && e.window == 1278 && !e.scrollX, "exactly fitting: natural");
  const auto n = fitEditor(600, 900, 38, 400, 300);
  expect(n.content == 600 && n.window == 400 && n.scrollX, "a minimum above the natural width never widens it");
}
}  // namespace

int main() {
  testRecover();
  testKeepTitle();
  testReanchor();
  testMonitorFor();
  testIniLines();
  testCascade();
  testPopOutPlace();
  testLiveResizeGate();
  testSplitAndIni();
  testFitEditor();
  return finish();
}
