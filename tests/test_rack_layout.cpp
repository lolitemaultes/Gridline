// Gridline — J-84: channel rack row layout (core/RackLayout.h) and the rack's
// wheel routing (Q-15, core/WheelRoute.h).
// Layout: the pan / volume knobs, mute LED and mixer box keep their place at
// every width and pattern length; the name shrinks (to a minimum) before the
// steps do; long patterns scroll sideways with a consistent range and time
// scroller. Wheel: a rack that scrolls scrolls with the plain wheel; a step's
// velocity changes only after a click on it or with Ctrl / Cmd.
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "Check.h"
#include "../src/core/RackLayout.h"
#include "../src/core/WheelRoute.h"

using namespace gl;
using namespace gl::racklayout;

namespace {

bool near(float a, float b, float eps = 0.01f) { return std::fabs(a - b) <= eps; }

// The rack's steps with a wheel-sensitive active step under the pointer, frame
// by frame as ChannelRack.cpp runs it: the step asks the router; wheel input
// it does not take scrolls the rack (sideways when only the steps scroll).
struct RackSim {
  wheelroute::Router r;
  wheelroute::Mode mode = wheelroute::Mode::Auto;
  float avail = 300;
  int steps = 64;
  bool rowsScroll = false;
  double t = 0;
  int velocity = 100;
  float scroll = 0;
  bool modifier = false;
  uint64_t stepId = 7;

  Layout layout() const { return compute({avail, steps}); }
  void frame(float wheel, bool click = false) {
    t += 1.0 / 60.0;
    const Layout l = layout();
    r.beginFrame(t, wheel);
    const bool scrolls = wheelroute::rackScrolls(mode, rowsScroll, l.scrolls());
    bool took = false;
    r.hovered(stepId);
    if (click && r.mayStartDrag(scrolls)) r.focus(stepId);
    const wheelroute::Query q{scrolls, modifier, r.hasFocus(stepId)};
    if (wheel != 0 && r.route(q) == wheelroute::Route::Adjust) {
      velocity = std::max(1, std::min(127, velocity + static_cast<int>(std::lround(wheel * 8 * wheelroute::Router::scale(q)))));
      r.claim();
      took = true;
    }
    if (wheel != 0 && !took && l.scrolls() && !rowsScroll) scroll = clampScroll(l, scroll - wheel * 4 * (l.stepW + kStepGap));
    r.endFrame();
  }
  void idle(double s) {
    for (int i = 0; i < static_cast<int>(s * 60); ++i) frame(0);
  }
};

}  // namespace

int main() {
  // ---- layout at every width / pattern length ----
  const float widths[] = {180, 200, 230, 260, 300, 340, 420, 520, 640, 700, 760, 900, 1100, 1300, 1600, 2400};
  const int lengths[] = {1, 4, 12, 16, 24, 32, 48, 64, 96, 128, 256};
  bool leftFixed = true, inRow = true, stepRange = true, nameRange = true, scrollRange = true, lampEnd = true, viewFits = true;
  bool stepsLeftOfLamp = true, minimalFits = true;
  for (int n : lengths) {
    float prevName = 0;
    for (float w : widths) {
      const Layout l = compute({w, n});
      // The left section (LED, pan, volume, mixer box) has a fixed place: nothing is ever dropped.
      leftFixed = leftFixed && kXLed + 16 <= kXPan && kXPan + kKnobW <= kXVol + 0.01f && kXVol + kKnobW <= kXMix && kXMix + kMixW <= kNameX;
      inRow = inRow && l.stepsX >= kNameX + l.nameW + kNameGap - 0.01f;
      stepRange = stepRange && l.stepW >= kStepMin && l.stepW <= kStepMax;
      nameRange = nameRange && l.nameW >= kNameFloor && l.nameW <= kNameMax && l.nameW >= prevName - 0.01f;  // never narrower in a wider rack
      prevName = l.nameW;
      scrollRange = scrollRange && near(l.contentW, contentWidth(n, l.stepW)) && near(l.scrollMax, std::max(0.0f, l.contentW - l.viewW)) &&
                    (l.scrolls() || near(l.viewW, l.contentW));
      lampEnd = lampEnd && near(l.lampX, l.stepsX + l.viewW) && near(l.rowW, l.lampX + kSelLampW);
      stepsLeftOfLamp = stepsLeftOfLamp && l.viewW >= std::min(l.contentW, kMinView) - 0.01f;
      // Down to the smallest rack (left section + minimum name + a few steps) a row fits the pane.
      if (w >= kNameX + kNameFloor + kNameGap + kMinView + kSelLampW) viewFits = viewFits && l.rowW <= w + 0.01f;
      else minimalFits = minimalFits && l.nameW >= kNameFloor;
    }
  }
  expect(leftFixed, "left section: LED, pan, volume and mixer box have fixed, non-overlapping places at every width");
  expect(inRow, "the step area starts right of the name button");
  expect(stepRange, "step width stays between the minimum and maximum");
  expect(nameRange, "name width stays in range and never shrinks as the rack widens");
  expect(scrollRange, "scroll range = content width - view width (0 when everything fits)");
  expect(lampEnd, "select lamp sits at the end of the step viewport");
  expect(stepsLeftOfLamp, "a few steps always stay visible");
  expect(viewFits, "rows fit the pane from the smallest rack up (no clipped knobs, no outer scrollbar)");
  expect(minimalFits, "below the smallest rack the name keeps its floor width");

  {  // Default floating rack (J-28, ~700 px rows pane): 16 and 32 steps fit, 64 scroll; knobs never go.
    const Layout l16 = compute({700, 16}), l32 = compute({700, 32}), l64 = compute({700, 64});
    expect(!l16.scrolls() && near(l16.nameW, kNameMax) && l16.stepW >= kStepComfort, "default rack, 16 steps: full name, comfortable steps, no scroll");
    expect(!l32.scrolls(), "default rack, 32 steps: all steps visible");
    expect(l64.scrolls() && l64.scrollMax > 0, "default rack, 64 steps: the steps scroll sideways");
  }
  {  // Wide rack: a 16-step pattern keeps full names and big steps.
    const Layout l = compute({1300, 16});
    expect(!l.scrolls() && near(l.nameW, kNameMax) && near(l.stepW, kStepMax) && !l.nameShrunk, "wide rack: full names, largest steps");
  }
  {  // Narrow rack: names shrink before the steps get small.
    const float room16 = kNameX + kNameGap + kSelLampW + contentWidth(16, kStepComfort);
    const Layout l = compute({room16 + 100, 16});
    expect(l.nameShrunk && l.nameW > kNameMin && l.nameW < kNameMax && l.stepW >= kStepComfort && !l.scrolls(),
           "narrow rack: the name shrinks first while 16 steps keep a comfortable size");
    const Layout n = compute({300, 16});
    expect(n.nameW >= kNameSqueeze && n.scrolls(), "very narrow rack: the name stops at its minimum and the steps scroll");
    const Layout n64 = compute({300, 64});
    expect(n64.nameW >= kNameMin && n64.scrolls(), "very narrow rack, long pattern (scrolls anyway): the name keeps its minimum");
    const Layout d = compute({190, 64});
    expect(d.nameW >= kNameFloor && d.viewW >= kMinView - 0.01f && d.scrolls(), "docked-narrow rack: name floor, a few steps, scroll");
  }
  {  // Steps: positions, hit test, groups of four.
    const Layout l = compute({500, 64});
    bool hit = true, order = true;
    for (int k = 0; k < 64; ++k) {
      hit = hit && l.stepAt(l.stepPos(k) + l.stepW * 0.5f) == k;
      if (k > 0) order = order && l.stepPos(k) >= l.stepPos(k - 1) + l.stepW + kStepGap - 0.01f;
    }
    expect(hit, "stepAt finds every step");
    expect(order, "steps are spaced by the step gap (plus the group gap every four)");
    expect(near(l.stepPos(4) - l.stepPos(3), l.stepW + kStepGap + kGroupGap), "group gap between beats");
    expect(l.stepAt(-1) == -1 && l.stepAt(l.contentW + 1) == -1, "stepAt outside the content is -1");
  }
  {  // Scroll helpers.
    const Layout l = compute({500, 64});
    expect(near(clampScroll(l, -20), 0) && near(clampScroll(l, 1e6f), l.scrollMax), "scroll clamps to [0, scrollMax]");
    const float s63 = revealStep(l, 0, 63);
    expect(near(s63, l.scrollMax) || near(l.stepPos(63) + l.stepW, s63 + l.viewW), "reveal the last step: it ends at the view's right edge");
    expect(near(revealStep(l, s63, 0), 0), "reveal the first step scrolls back to 0");
    expect(near(revealStep(l, 0, 2), 0), "a visible step needs no scroll");
    float x0 = 0, w = 0;
    expect(thumb(l, 0, 200, &x0, &w) && near(x0, 0) && w > 0 && w < 200, "thumb at the left end, sized by the visible part");
    expect(thumb(l, l.scrollMax, 200, &x0, &w) && near(x0 + w, 200), "thumb at the right end");
    expect(near(scrollForTrack(l, 0, 200), 0) && near(scrollForTrack(l, 200, 200), l.scrollMax), "track ends map to the scroll ends");
    const Layout fits = compute({1300, 16});
    expect(!thumb(fits, 0, 200, &x0, &w), "no thumb when the steps fit");
  }

  // ---- Q-15: wheel routing in the rack ----
  expect(wheelroute::rackScrolls(wheelroute::Mode::Auto, false, true) && wheelroute::rackScrolls(wheelroute::Mode::Auto, true, false) &&
             !wheelroute::rackScrolls(wheelroute::Mode::Auto, false, false),
         "auto: the rack scrolls when its list or its steps scroll");
  expect(!wheelroute::rackScrolls(wheelroute::Mode::Adjust, true, true) && wheelroute::rackScrolls(wheelroute::Mode::Scroll, false, false),
         "adjust / scroll settings ignore the layout");
  {  // Narrow rack, 64 steps: the wheel over an active step scrolls the steps, the velocity stays.
    RackSim s;
    for (int i = 0; i < 4; ++i) s.frame(-1.0f);
    expect(s.velocity == 100, "narrow rack: wheel over an active step leaves its velocity");
    expect(s.scroll > 0, "narrow rack: the wheel scrolls the steps instead");
  }
  {  // Ctrl / Cmd + wheel adjusts in fine steps.
    RackSim s;
    s.modifier = true;
    s.frame(1.0f);
    expect(s.velocity == 102 && s.scroll == 0, "narrow rack: Ctrl+wheel changes the velocity (fine), no scroll");
  }
  {  // A click on the step gives it the wheel.
    RackSim s;
    s.frame(0, true);
    s.frame(1.0f);
    expect(s.velocity == 108 && s.scroll == 0, "narrow rack: after a click the wheel changes the step's velocity");
  }
  {  // A flick's momentum never changes a step, Ctrl pressed mid-flick or not.
    RackSim s;
    float v = -1.5f;
    for (int i = 0; i < 3; ++i, v *= 0.9f) s.frame(v);
    s.modifier = true;
    for (int i = 0; i < 20; ++i, v *= 0.9f) s.frame(v);
    expect(s.velocity == 100, "narrow rack: momentum of a scroll never changes a velocity");
  }
  {  // Wide rack, 16 steps, few channels: nothing scrolls, the wheel adjusts as before.
    RackSim s;
    s.avail = 1300;
    s.steps = 16;
    s.frame(1.0f);
    expect(s.velocity == 108, "wide rack (nothing scrolls): the wheel adjusts the velocity as before");
    s.idle(0.5);
    s.rowsScroll = true;  // many channels: the list scrolls vertically
    s.frame(1.0f);
    expect(s.velocity == 108, "a rack whose channel list scrolls: the wheel scrolls (knobs and steps stay)");
  }
  {  // Setting: adjust keeps the old behaviour everywhere.
    RackSim s;
    s.mode = wheelroute::Mode::Adjust;
    s.frame(1.0f);
    expect(s.velocity == 108 && s.scroll == 0, "setting adjust: the wheel changes the step even in a scrolling rack");
  }
  return finish();
}
