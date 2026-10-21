// Gridline — J-83: wheel routing over mixer controls (core/WheelRoute.h).
// Thin mixer: the wheel scrolls the pane; wide mixer: it adjusts the control
// under the pointer; Ctrl+wheel adjusts in thin; momentum never changes a
// parameter; resizing across the J-38 breakpoint flips the routing live; the
// setting overrides the layout.
#include <cmath>

#include "Check.h"
#include "../src/core/MixerLayout.h"
#include "../src/core/WheelRoute.h"

using namespace gl;
using namespace gl::wheelroute;

namespace {

// A pane with one wheel-sensitive control (a gain fader), run frame by frame
// the way the UI does: the control asks the router, the pane scrolls with any
// wheel input the control did not take.
struct Sim {
  Router r;
  Mode mode = Mode::Auto;
  float width = 300;   // mixer pane width (logical px)
  double t = 0;
  double gain = 0.8;   // the fader's value
  double scroll = 0;   // the pane's scroll (wheel units)
  bool overFader = true;
  bool modifier = false;
  uint64_t faderId = 42;

  bool narrow() const {
    mixlayout::Input in;
    in.width = width, in.height = 800, in.strips = 12;
    return mixlayout::compute(in).column;
  }
  void frame(float wheel, bool click = false) {
    t += 1.0 / 60.0;
    r.beginFrame(t, wheel);
    const bool scrolls = paneScrolls(mode, narrow());
    bool took = false;
    if (overFader) {
      r.hovered(faderId);
      if (click && r.mayStartDrag(scrolls)) r.focus(faderId);  // press + release without a drag
      const Query q{scrolls, modifier, r.hasFocus(faderId)};
      if (wheel != 0 && r.route(q) == Route::Adjust) {
        gain += wheel * 0.02 * Router::scale(q);
        r.claim();
        took = true;
      }
    }
    if (wheel != 0 && !took) scroll += wheel;
    r.endFrame();
  }
  void idle(double seconds) {
    for (int i = 0; i < static_cast<int>(seconds * 60); ++i) frame(0);
  }
};

bool same(double a, double b) { return std::fabs(a - b) < 1e-9; }

}  // namespace

int main() {
  // Setting values.
  expect(modeFromSetting(0) == Mode::Adjust && modeFromSetting(1) == Mode::Scroll && modeFromSetting(2) == Mode::Auto, "setting values map to modes");
  expect(modeFromSetting(7) == Mode::Auto && modeFromSetting(-1) == Mode::Auto, "unknown setting values fall back to auto");
  expect(paneScrolls(Mode::Auto, true) && !paneScrolls(Mode::Auto, false), "auto: narrow scrolls, wide adjusts");
  expect(paneScrolls(Mode::Scroll, false) && !paneScrolls(Mode::Adjust, true), "scroll / adjust ignore the layout");

  {  // Thin layout: the wheel over a fader scrolls the pane, the gain stays.
    Sim s;
    s.width = 300;
    expect(s.narrow(), "300 px is the thin layout");
    for (int i = 0; i < 5; ++i) s.frame(-1.0f);
    expect(same(s.gain, 0.8), "thin: wheel over the fader leaves the gain");
    expect(same(s.scroll, -5.0), "thin: wheel over the fader scrolls the pane");
  }
  {  // Wide layout: the wheel adjusts as before.
    Sim s;
    s.width = 900;
    expect(!s.narrow(), "900 px is the wide layout");
    s.frame(1.0f);
    expect(same(s.gain, 0.82) && same(s.scroll, 0), "wide: wheel over the fader adjusts it (full step), the pane stays");
  }
  {  // Thin + modifier: fine adjustment.
    Sim s;
    s.modifier = true;
    s.frame(1.0f);
    expect(same(s.gain, 0.8 + 0.02 * kFineScale) && same(s.scroll, 0), "thin: Ctrl+wheel adjusts the fader in fine steps");
  }
  {  // Thin: a click gives wheel focus; leaving the fader drops it.
    Sim s;
    s.frame(0, true);
    s.frame(1.0f);
    expect(same(s.gain, 0.82) && same(s.scroll, 0), "thin: after a click the wheel adjusts the fader");
    s.idle(0.5);
    s.overFader = false;
    s.frame(0);
    s.overFader = true;
    s.idle(0.5);
    s.frame(1.0f);
    expect(same(s.gain, 0.82) && same(s.scroll, 1.0), "thin: once the pointer left the fader the wheel scrolls again");
  }
  {  // Momentum: a flick that started over a strip's meter rolls over the fader.
    Sim s;
    s.overFader = false;
    float v = 2.0f;
    for (int i = 0; i < 4; ++i, v *= 0.9f) s.frame(v);
    s.overFader = true;  // the strips scroll under the pointer: now it is on a fader
    s.modifier = true;   // even with Ctrl pressed mid-flick
    double before = s.scroll;
    for (int i = 0; i < 40; ++i, v *= 0.9f) s.frame(i % 3 == 2 ? 0.0f : v);  // OS momentum: events on two of three frames
    expect(same(s.gain, 0.8), "momentum: the tail of a pane scroll never changes the fader (Ctrl held or not)");
    expect(s.scroll > before, "momentum: the tail keeps scrolling the pane");
    expect(!s.r.mayStartDrag(true), "momentum: a press while the pane scrolls starts no drag");
    s.frame(0, true);  // a click during the flick: no wheel focus either
    expect(!s.r.hasFocus(s.faderId), "momentum: a click during the flick does not give wheel focus");
    s.idle(0.5);  // the gesture ends
    expect(s.r.mayStartDrag(true), "after the gesture a press may drag again");
    s.frame(1.0f);
    expect(same(s.gain, 0.8 + 0.02 * kFineScale), "a new gesture with Ctrl adjusts");
  }
  {  // Momentum in the wide layout behaves as before (the reference adjusts).
    Sim s;
    s.width = 900;
    s.overFader = false;
    s.frame(1.0f);
    s.overFader = true;
    s.frame(1.0f);
    expect(same(s.gain, 0.82), "wide: wheel input keeps adjusting (unchanged behaviour)");
  }
  {  // Resizing across the breakpoint flips the routing live.
    Sim s;
    s.width = 900;
    s.frame(1.0f);
    expect(same(s.gain, 0.82), "resize: wide adjusts");
    s.idle(0.5);
    s.width = mixlayout::kColumnBelow - 1;
    s.frame(1.0f);
    expect(same(s.gain, 0.82) && same(s.scroll, 1.0), "resize: narrowed below the breakpoint, the same wheel scrolls");
    s.idle(0.5);
    s.width = mixlayout::kColumnBelow;
    s.frame(1.0f);
    expect(same(s.gain, 0.84), "resize: widened again, the wheel adjusts again");
  }
  {  // The setting overrides the layout.
    Sim s;
    s.mode = Mode::Adjust;
    s.width = 300;
    s.frame(1.0f);
    expect(same(s.gain, 0.82) && same(s.scroll, 0), "setting adjust: thin mixer adjusts like the wide one");
    Sim w;
    w.mode = Mode::Scroll;
    w.width = 900;
    w.frame(1.0f);
    expect(same(w.gain, 0.8) && same(w.scroll, 1.0), "setting scroll: the wide mixer's wheel no longer adjusts");
    w.modifier = true;
    w.idle(0.5);
    w.frame(1.0f);
    expect(same(w.gain, 0.8 + 0.02 * kFineScale), "setting scroll: Ctrl+wheel still adjusts");
    Sim n;
    n.mode = Mode::Auto;
    n.width = 900;
    n.frame(1.0f);
    expect(same(n.gain, 0.82), "setting auto: wide adjusts");
  }
  {  // Gestures: events closer than kGestureGap are one gesture.
    Router r;
    r.beginFrame(0.0, 1.0f);
    r.endFrame();  // unclaimed: a pane scroll
    expect(r.scrolling(), "unclaimed wheel input is a pane scroll gesture");
    r.beginFrame(kGestureGap * 0.5, 0);
    r.endFrame();
    expect(r.scrolling(), "the gesture lasts through short gaps");
    r.beginFrame(kGestureGap + 0.05, 0);
    r.endFrame();
    expect(!r.scrolling(), "the gesture ends after the gap");
    expect(r.route({true, true, false}) == Route::Adjust, "after the gesture Ctrl+wheel adjusts");
    expect(r.route({true, false, false}) == Route::Scroll, "plain wheel scrolls a scrolling pane");
    expect(r.route({false, false, false}) == Route::Adjust, "plain wheel adjusts in an adjusting pane");
  }
  return finish();
}
