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
      // The left section (headphones, pan, volume, mixer box) has a fixed place: nothing is ever dropped.
      leftFixed = leftFixed && kXLeft + kPhonesW + 2 <= kXPan && kXPan + kKnobW <= kXVol + 0.01f && kXVol + kKnobW <= kXMix && kXMix + kMixW <= kNameX;
      inRow = inRow && l.stepsX >= kNameX + l.nameW + kNameGap - 0.01f;
      stepRange = stepRange && l.stepW >= kStepMin && l.stepW <= kStepMax;
      nameRange = nameRange && l.nameW >= kNameFloor && l.nameW <= kNameMax && l.nameW >= prevName - 0.01f;  // never narrower in a wider rack
      prevName = l.nameW;
      scrollRange = scrollRange && near(l.contentW, contentWidth(n, l.stepW)) && near(l.scrollMax, std::max(0.0f, l.contentW - l.viewW)) &&
                    (l.scrolls() || near(l.viewW, l.contentW));
      // K-127: the lamp sits between the name button and the first step, clear of both.
      lampEnd = lampEnd && l.lampX >= kNameX + l.nameW + 2 - 0.01f && l.lampX + kSelLampW <= l.stepsX - 2 + 0.01f &&
                near(l.rowW, l.stepsX + l.viewW);
      stepsLeftOfLamp = stepsLeftOfLamp && l.viewW >= std::min(l.contentW, kMinView) - 0.01f;
      // Down to the smallest rack (left section + minimum name + a few steps) a row fits the pane.
      if (w >= kNameX + kNameFloor + kNameGap + kMinView + kSelLampW) viewFits = viewFits && l.rowW <= w + 0.01f;
      else minimalFits = minimalFits && l.nameW >= kNameFloor;
    }
  }
  expect(leftFixed, "left section: headphones, pan, volume and mixer box have fixed, non-overlapping places at every width");
  expect(inRow, "the step area starts right of the name button");
  expect(stepRange, "step width stays between the minimum and maximum");
  expect(nameRange, "name width stays in range and never shrinks as the rack widens");
  expect(scrollRange, "scroll range = content width - view width (0 when everything fits)");
  expect(lampEnd, "the channel's light sits between the name button and the steps");
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
  {  // K-295: a 32-step pattern in a narrow floating rack (the demo at 1920x1080, scale 2: 674
     // px of rows) used to squeeze every name to about 53 px ("Ope...", "Sha..."). The name
     // button now keeps what the names need, up to kNameRead, and the last steps scroll.
    const Layout old = compute({680, 32});
    expect(old.nameW < 60, "K-295: without a name need the old squeeze stands (names about 53 px, nothing scrolls)");
    const Layout l = compute({674, 32, kStepMax, 300});
    expect(near(l.nameW, kNameRead) && !l.scrolls() && near(l.stepW, kStepTight),
           "K-295: names keep the width they need; the 32 steps tighten a pixel and still fit (no scroll)");
    const Layout n = compute({520, 32, kStepMax, 70});
    expect(n.nameW >= 70 && n.scrolls() && n.stepW >= kStepTight, "K-295: a narrower rack keeps the names and scrolls the steps");
    const Layout cap = compute({680, 32, kStepMax, 300});
    expect(near(cap.nameW, kNameRead), "K-295: a very long name holds the button open only up to kNameRead (tooltip for the rest)");
    const Layout wide = compute({760, 16, kStepMax, 70}), wide0 = compute({760, 16});
    expect(near(wide.nameW, wide0.nameW) && near(wide.stepW, wide0.stepW) && !wide.scrolls(),
           "K-295: where names already fit (16 steps, default rack) nothing changes");
    const Layout tiny = compute({190, 64, kStepMax, kNameRead});
    expect(tiny.nameW >= kNameFloor && tiny.viewW >= kMinView - 0.01f, "K-295: a tiny rack still keeps a few steps visible");
    bool mono = true;
    float prev = 0;
    for (float w = 180; w <= 2400; w += 20) {
      const Layout m = compute({w, 32, kStepMax, kNameRead});
      mono = mono && m.nameW >= prev - 0.01f && m.rowW <= std::max(w, kNameX + kNameFloor + kNameGap + kMinView + kSelLampW) + 0.01f;
      prev = m.nameW;
    }
    expect(mono, "K-295: with a name need the name never narrows in a wider rack and rows fit the pane");
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
  // K-136: the rack's height follows its content.
  {
    const float top = 60, bar = 28, chrome = 1, pitch = 33;
    auto rows = [&](int n) { return 8.0f + 8 + 4 + n * pitch - 4 + 8; };  // paddings, step lamps, n rows
    const float h5 = racklayout::fitHeight(top, rows(5), bar, chrome, false, 140);
    expect(h5 == std::ceil(top + rows(5) + bar + chrome), "fit height: header, rows, add bar and frame (5 channels)");
    expect(racklayout::fitHeight(top, rows(6), bar, chrome, false, 140) - h5 == pitch, "one more channel: one row pitch taller");
    expect(racklayout::fitHeight(top, rows(4), bar, chrome, false, 140) - h5 == -pitch, "one channel fewer: one row pitch shorter");
    expect(racklayout::fitHeight(top, 0, bar, chrome, true, 140) == top + 140 + chrome, "empty rack: room for its hint");
    expect(racklayout::fitHeight(10.2f, 20.1f, 0, 0, false, 0) == 31.0f, "fit height rounds up to a whole pixel (never cuts the add bar)");
    expect(racklayout::rowsRegion(rows(5), 600) == rows(5), "short list: the rows take their content's height (add bar under the last row)");
    expect(racklayout::rowsRegion(rows(30), 600) == 0, "long list: the rows fill the room and scroll");
    expect(racklayout::rowsRegion(0, 600) == 0, "not measured yet: fill the room");
  }
  // K-236: the step groups follow the pattern's meter (one beat a group).
  {
    expect(stepGroup(16, 4, 4) == 4 && stepGroup(12, 3, 4) == 4 && stepGroup(20, 5, 4) == 4, "meter groups: quarter beats of four sixteenth steps");
    expect(stepGroup(14, 7, 8) == 2 && stepGroup(10, 5, 8) == 2, "meter groups: eighth beats of two steps (7/8, 5/8)");
    expect(stepGroup(12, 6, 8) == 6 && stepGroup(18, 9, 8) == 6 && stepGroup(24, 12, 8) == 6, "meter groups: compound meters count dotted beats");
    expect(stepGroup(12, 4, 4) == 3 && stepGroup(24, 4, 4) == 6 && stepGroup(32, 4, 4) == 8, "meter groups: triplet and fine steps group per beat");
    expect(stepGroup(13, 4, 4) == 4 && stepGroup(0, 4, 4) == 4 && stepGroup(64, 2, 4) == 4, "meter groups: steps that do not fit the beats keep fours");
    Input in;
    in.avail = 900, in.steps = 14, in.group = 2;
    const Layout l = compute(in);
    expect(l.group == 2, "layout keeps the meter's group");
    const float pitch = l.stepW + kStepGap;
    expect(near(l.stepPos(2) - l.stepPos(1), pitch + kGroupGap) && near(l.stepPos(1) - l.stepPos(0), pitch), "7/8: a group gap after every second step");
    bool found = true;
    for (int k = 0; k < 14; ++k) found = found && l.stepAt(l.stepPos(k) + 1) == k;
    expect(found, "7/8: every step is found under its own pad");
    expect(near(l.contentW, contentWidth(14, l.stepW, 2)), "7/8: content width counts its six group gaps");
    Input four;
    four.avail = 900;
    expect(compute(four).group == 4, "default input: groups of four as before");
  }
  return finish();
}
