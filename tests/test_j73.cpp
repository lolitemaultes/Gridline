// Gridline — J-73 tests: Q-6 eye toggle, Q-7 shared minimum inset, Q-8 effect
// reorder by wheel (order, links, automation, routing, undo), Q-9 reverb room
// view drag mapping (monotonic, undo, reset), Q-10 event-lane bar glides.
#include <cmath>
#include <string>
#include "../src/app/Inset.h"
#include "../src/app/Widgets.h"
#include "../src/core/Document.h"
#include "../src/core/MixerOps.h"
#include "../src/core/Params.h"
#include "../src/core/PluginParams.h"
#include "../src/core/Project.h"
#include "../src/core/SmoothSnap.h"
#include "../src/core/Song.h"
#include "../src/engine/ReverbRoomViz.h"
#include "Check.h"

using namespace gl;

int main() {
  // ---- Q-6: eye icon follows the window state ----
  {
    bool shown = false;
    expect(ui::eyeIcon(shown) == ui::Icon::EyeOff, "hidden window: slashed eye");
    shown = !shown;
    expect(ui::eyeIcon(shown) == ui::Icon::Eye, "shown window: open eye");
    expect(std::string(ui::eyeHint(true)) != ui::eyeHint(false), "tooltip says what a click does");
    expect(std::string(ui::eyeHint(true)).find("Hide") == 0 && std::string(ui::eyeHint(false)).find("Show") == 0, "hide when shown, show when hidden");
    // Q-12: the tool family is its own icon set (the playback tool is not the transport play icon).
    expect(ui::isToolIcon(ui::Icon::ToolDraw) && ui::isToolIcon(ui::Icon::ToolSnap) && ui::isToolIcon(ui::Icon::ToolPlay), "tool icons in the family");
    expect(!ui::isToolIcon(ui::Icon::Play) && !ui::isToolIcon(ui::Icon::Draw) && !ui::isToolIcon(ui::Icon::Eye), "other icons are not");
  }

  // ---- Q-7: shared minimum inset ----
  {
    using inset::Rect;
    const Rect panel{0, 0, 100, 50};
    const Rect slider{80, 1, 90, 40};  // the reverb case: touching the top
    const Rect f = inset::fit(panel, slider, 4);
    expect(f.y0 == 4 && f.y1 == 40 && f.x0 == 80 && f.x1 == 90, "fit shortens only the side that is too close");
    expect(inset::gap(panel, f) >= 4 - 1e-4f, "fitted control clears the inset");
    const Rect lbl{-2, 46, 18, 56};
    const Rect p = inset::place(panel, lbl, 3);
    expect(p.w() == lbl.w() && p.h() == lbl.h() && inset::gap(panel, p) >= 3 - 1e-4f, "place moves a label inside without resizing it");
    const Rect wide{-10, 10, 120, 20};
    const Rect pw = inset::place(panel, wide, 3);
    expect(pw.x0 == 3 && pw.x1 == 97, "place shrinks what cannot fit");
    // check: innermost frame by centre; frames the control fills are not its panel
    std::vector<Rect> frames = {panel, Rect{10, 10, 60, 45}, Rect{70, 5, 80, 15}};
    std::vector<Rect> ctls = {Rect{12, 11, 30, 30}, Rect{20, 20, 40, 40}, Rect{70, 5, 80, 15}, Rect{95, 20, 99, 30}};
    auto issues = inset::check(frames, ctls, 3);
    bool c0 = false, c1 = false, c2 = false, c3 = false;
    for (const auto& is : issues) {
      if (is.control == 0) c0 = is.frame == 1 && is.side == 1;
      if (is.control == 1) c1 = true;
      if (is.control == 2) c2 = true;
      if (is.control == 3) c3 = is.frame == 0 && is.side == 2 && std::fabs(is.gap - 1) < 1e-4f;
    }
    expect(c0, "control 1 px under its well's top is reported against the well");
    expect(!c1, "well-inset control passes");
    expect(!c2, "a control filling a box is that box, not inside it");
    expect(c3, "control 1 px from the panel's right edge is reported");
    expect(inset::minFor(1.0f) >= inset::kMinInset && inset::minFor(2.0f) == 2.0f * inset::kMinInsetUnits, "inset scales with the form");
  }

  // ---- Q-8: wheel reorder in a track's effect chain ----
  {
    Document d(makeEmptySong());
    const Song& s = d.song();
    const ObjectId i1 = s.mixer[1].id, i2 = s.mixer[2].id;
    d.setSlotPlugin(i1, 0, "gl.delay");
    d.setSlotPlugin(i1, 1, "gl.reverb");
    d.setSlotPlugin(i1, 2, "gl.eq");
    d.setParam(slotMixParam(i1, 1), 0.4, false);
    d.setSend(i1, i2, 0.7f);
    d.setSendSidechain(i1, i2, true);
    Song withRefs = s;
    ModLink ml;  // controller link to the reverb's first parameter
    ml.sourceOwner = i2;
    ml.sourceSlot = 5;
    ml.owner = i1;
    ml.index = slotParam(i1, 1, 0).index;
    withRefs.modLinks.push_back(ml);
    AutomationClip ac;  // automation clip on the reverb's mix
    ac.id = withRefs.allocateId();
    ac.owner = i1;
    ac.index = slotMixParam(i1, 1).index;
    ac.points = {AutoPoint{}};
    withRefs.automations.push_back(ac);
    d.reset(withRefs);
    const Song before = s;
    const size_t undoDepth = d.history().size();
    const int now = mixops::stepSlot(d, i1, 1, -1);  // wheel up over the reverb
    const MixerTrack& t = *s.mixerTrack(i1);
    expect(now == 0 && t.slots[0].plugin.type == "gl.reverb" && t.slots[1].plugin.type == "gl.delay" && t.slots[2].plugin.type == "gl.eq",
           "wheel up: the effect trades places with the one above");
    expect(t.slots[0].mix == 0.4f, "mix travels with the effect");
    expect(s.modLinks.size() == 1 && s.modLinks[0].index == slotParam(i1, 0, 0).index, "controller link follows the effect");
    expect(s.automations.size() == 1 && s.automations[0].index == slotMixParam(i1, 0).index, "automation clip follows the effect");
    bool scKept = false;
    for (const auto& sd : t.sends) scKept = scKept || (sd.to == i2 && sd.sidechain && sd.level == 0.7f);
    expect(t.sends == before.mixerTrack(i1)->sends && scKept, "routing and sidechain untouched");
    expect(d.history().size() == undoDepth + 1, "one undo step per notch");
    d.undo();
    expect(s == before, "undo restores order, links and automation");
    expect(mixops::stepSlot(d, i1, 0, -1) == 0 && s == before, "top slot cannot move up");
    expect(mixops::stepSlot(d, i1, 2, +1) == 3 && s.mixerTrack(i1)->slots[3].plugin.type == "gl.eq" && s.mixerTrack(i1)->slots[2].plugin.empty(),
           "wheel down into an empty slot");
    expect(mixops::stepSlot(d, i1, 5, +1) == 5, "empty slot does not move");
    expect(mixops::stepSlot(d, i1, 3, +100) == kEffectSlots - 1, "clamped to the last slot");
  }

  // ---- Q-9: reverb room view drives the room size ----
  {
    expect(roomviz::roomDragParam(true) == pp::RvbFRoom && roomviz::roomDragParam(false) == pp::RvbSize, "room view drives the size control");
    double prev = -1;
    bool mono = true;
    for (int px = -200; px <= 200; px += 5) {
      const double n = roomviz::roomDragStep(0.5, static_cast<float>(px), 100, false);
      mono = mono && n >= prev;
      prev = n;
    }
    expect(mono, "drag up raises the size monotonically");
    expect(roomviz::roomDragStep(0.5, 10, 100, false) > 0.5 && roomviz::roomDragStep(0.5, -10, 100, false) < 0.5, "up larger, down smaller");
    const double coarse = roomviz::roomDragStep(0.5, 20, 100, false) - 0.5, fine = roomviz::roomDragStep(0.5, 20, 100, true) - 0.5;
    expect(fine > 0 && fine < coarse * 0.2, "Shift is fine adjust");
    expect(roomviz::roomDragStep(0.99, 1000, 100, false) == 1.0 && roomviz::roomDragStep(0.01, -1000, 100, false) == 0.0, "clamped to the range");
    // A drag gesture through the document: one undo step, reset to default.
    Document d(makeEmptySong());
    const ObjectId i1 = d.song().mixer[1].id;
    d.setSlotPlugin(i1, 0, "gl.reverb");
    const ParamId pid = slotParam(i1, 0, roomviz::roomDragParam(false));
    const auto desc = describeParam(d.song(), pid);
    const double v0 = *getParam(d.song(), pid);
    const size_t depth = d.history().size();
    double n = desc->toNormalized(v0);
    for (int k = 0; k < 10; ++k) {
      n = roomviz::roomDragStep(n, 8, 120, false);
      d.setParam(pid, desc->fromNormalized(n), true);
    }
    d.endGesture();
    const double v1 = *getParam(d.song(), pid);
    expect(v1 > v0, "dragging up grows the room");
    expect(d.history().size() == depth + 1, "one undo step per drag");
    d.undo();
    expect(*getParam(d.song(), pid) == v0, "undo returns the size");
    d.setParam(pid, desc->fromNormalized(0.9), false);
    d.setParam(pid, desc->def, false);  // double-click reset
    expect(*getParam(d.song(), pid) == desc->def, "reset to the default");
  }

  // ---- Q-10: event-lane bars glide quickly and land exactly ----
  {
    smooth::Glides g;
    double t = 0;
    g.follow(1, 0.2, t, true, 2e-3, smooth::kBarTau);
    double v = 0.2;
    int frames = 0;
    while (v != 0.8 && frames < 200) {
      t += 1.0 / 60;
      v = g.follow(1, 0.8, t, true, 2e-3, smooth::kBarTau);
      ++frames;
    }
    expect(v == 0.8, "bar lands exactly on the value");
    expect(frames >= 3 && frames <= 8, "settles in ~50-130 ms at 60 Hz");
    const double settle = smooth::settleTime(smooth::kBarTau);
    expect(settle > 0.06 && settle < 0.08, "1 % settle time 60-80 ms");
    smooth::Glides a, b;  // frame-rate independence
    a.follow(2, 0, 0, true, 0, smooth::kBarTau);
    b.follow(2, 0, 0, true, 0, smooth::kBarTau);
    const double va = a.follow(2, 1, 0.032, true, 0, smooth::kBarTau);
    double vb = 0;
    for (int k = 1; k <= 4; ++k) vb = b.follow(2, 1, 0.008 * k, true, 0, smooth::kBarTau);
    expect(std::fabs(va - vb) < 1e-12, "same position at 30 and 120 fps");
    smooth::Glides off;
    off.follow(3, 0.1, 0, false, 1e-4, smooth::kBarTau);
    expect(off.follow(3, 0.9, 0.001, false, 1e-4, smooth::kBarTau) == 0.9 && off.size() == 0, "motion off: no lag, nothing kept");
  }
  return finish();
}
