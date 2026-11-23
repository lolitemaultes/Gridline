// Gridline - compact groups on the Stage (core/StageCompact.h): a closed group's chip is the size of one
// token, so a click or a drop beside it reaches the stage (and the place behind the group is free), and
// the focus rules that open and close a group (a click on its chip, choosing its tracks, a drag resting
// on it, Esc or a click elsewhere, the grace after a drag, automation forming and parting groups).
#include <cmath>
#include <vector>
#include "Check.h"
#include "core/StageCompact.h"
#include "core/StageMap.h"

using namespace gl;
namespace st = gl::stage;

namespace {

st::Geo geo(float scale = 1.0f) { return st::layout(4, 114, 1436, 688, scale, 12); }

st::Pt ringOf(const st::Geo& g, double pan, double depth) {
  const st::Pt p = g.at(pan, depth);
  return {p.x, p.y + g.side(depth) * 0.5f};
}

// ---------------------------------------------------------------- the chip and hit-testing
void testChip() {
  for (float scale : {1.0f, 2.0f}) {
    const st::Geo g = geo(scale);
    const st::Rect tok = g.token(0, 0);
    const st::Chip two = st::chipLayout(g, 0, 0, 2), many = st::chipLayout(g, 0, 0, 30);
    expect(two.face.x0 == tok.x0 && two.face.y1 == tok.y1 && std::fabs(two.face.w() - tok.w()) < 0.01f, "the chip's face is the token a lone track has at that spot");
    expect(two.cards == 1 && many.cards == 2, "one card behind for two tracks, two for three or more");
    expect(many.hit.w() <= tok.w() + 2 * st::kStackStep * scale + 0.01f && many.hit.h() <= tok.h() + 2 * st::kStackStep * scale + 0.01f,
           "however many tracks, the chip stays about one token in size");
    const st::Tray tr = st::trayLayout(g, ringOf(g, 0, 0), 30, 4, 1436, 118, 680);
    expect(many.hit.area() * 10 < tr.box.area(), "a closed chip covers far less of the stage than the open tray of 30");
    const st::Chip sh = st::shifted(many, 12, 0);
    expect(sh.face.x0 == many.face.x0 + 12 && sh.hit.x1 == many.hit.x1 + 12, "the chip moves with the display separation");
  }
}

// A drop beside a closed chip, where the open tray would be, reaches the stage: the track stays where it
// was dropped (behind the group). With the old always-open tray there it would have joined the group.
void testDropBeside() {
  const st::Geo g = geo();
  const st::Pt ring = ringOf(g, 0, 0);
  const st::Chip chip = st::chipLayout(g, 0, 0, 8);
  const st::Tray tray = st::trayLayout(g, ring, 8, 4, 1436, 118, 680);
  // Behind the group: one row back (distance 0.25), straight above the spot, inside the old tray.
  const double pan = 0, depth = 0.25;
  const st::Pt at = g.at(pan, depth);
  expect(tray.box.contains(at.x, at.y) && !chip.hit.contains(at.x, at.y), "the place behind the spot is under the open tray but not the chip");
  st::Obstacle closed;
  closed.r = chip.hit, closed.pan = 0, closed.depth = 0, closed.key = 0;
  st::Obstacle open = closed;
  open.r = tray.box;
  const st::Drop dc = st::resolveDrop(g, pan, depth, at, {closed}, false);
  expect(dc.kind == st::Drop::Free && std::fabs(dc.depth - depth) < 1e-9 && std::fabs(dc.pan - pan) < 1e-9, "dropped behind a closed group, the track stays there");
  const st::Drop dopen = st::resolveDrop(g, pan, depth, at, {open}, false);
  expect(dopen.kind == st::Drop::Join, "... where an open tray would have taken it into the group");
  // Right beside the chip at the same distance: eased aside or free, never joined, never overlapping.
  const st::Rect tok = g.token(0, 0);
  const float bx = chip.hit.x1 + tok.w() * 0.5f + st::kGap + 2;
  const double bpan = g.panOfX(bx, 0);
  const st::Drop db = st::resolveDrop(g, bpan, 0, {bx, (tok.y0 + tok.y1) * 0.5f}, {closed}, false);
  expect(db.kind == st::Drop::Free, "a drop just beside the chip stays where it was dropped");
  const st::Rect r = g.token(db.pan, db.depth);
  expect(st::overlapArea(r, chip.hit) <= 0, "... and does not overlap the chip");
  // On the chip itself: joins.
  const st::Pt on{(chip.face.x0 + chip.face.x1) * 0.5f, (chip.face.y0 + chip.face.y1) * 0.5f};
  const st::Drop dj = st::resolveDrop(g, g.panOfX(on.x, 0), 0, on, {closed}, false);
  expect(dj.kind == st::Drop::Join && dj.pan == 0 && dj.depth == 0, "a drop on the chip joins the group at its exact spot");
  // A click beside the chip: neither the chip nor a token there (the stage gets it).
  const st::Pt beside{chip.hit.x1 + 8, (chip.hit.y0 + chip.hit.y1) * 0.5f};
  expect(!chip.hit.grown(2).contains(beside.x, beside.y), "a click 8 px beside the chip misses it (the stage's own)");
  expect(chip.hit.grown(2).contains(chip.hit.x1 + 1, chip.hit.y0 + 1), "... while its stacked cards still count as the chip");
}

// ---------------------------------------------------------------- the focus rules
constexpr double kDt = 1.0 / 60.0;

st::FocusIn frame(const std::vector<std::vector<ObjectId>>& groups, const std::vector<ObjectId>& sel = {}) {
  st::FocusIn in;
  in.groups = groups;
  in.selection = sel;
  in.dt = kDt;
  return in;
}

void run(st::GroupFocus& f, st::FocusIn in, double seconds) {
  for (double t = 0; t < seconds - 1e-9; t += kDt) f.update(in), in.dragFrom = kNoId;
}

void testFocusRules() {
  const std::vector<std::vector<ObjectId>> gs{{10, 11, 12}, {20, 21}};
  {
    st::GroupFocus f;
    expect(f.update(frame(gs)) == -1, "at rest every group is closed");
    f.pin(20);
    expect(f.update(frame(gs)) == 1, "a click on a chip opens its group");
    expect(f.update(frame(gs, {21})) == 1, "choosing a track inside it keeps it open");
    expect(f.update(frame(gs, {5})) == -1, "choosing a track outside it closes it");
    expect(f.pinned() == kNoId, "... and the click is forgotten");
    expect(f.update(frame(gs, {5})) == -1, "it stays closed");
  }
  {
    st::GroupFocus f;
    expect(f.update(frame(gs, {12})) == -1, "the choice a project opens with opens nothing");
    expect(f.update(frame(gs, {11})) == 0, "choosing any member opens its group");
    expect(f.update(frame(gs, {11, 12})) == 0, "... several members of it too");
    expect(f.update(frame(gs, {11, 20})) == -1, "a choice across two groups opens neither");
    expect(f.update(frame(gs, {11, 5})) == -1, "a choice of a member and a lone track opens nothing");
    expect(f.update(frame(gs, {21})) == 1, "choosing a member of another group moves the focus there");
  }
  {
    st::GroupFocus f;
    f.pin(10);
    expect(f.update(frame(gs)) == 0, "open");
    f.close();  // Esc, or a click on the empty floor
    expect(f.update(frame(gs)) == -1, "Esc or a click on the floor closes it");
  }
  {  // Automation: groups form and part while one is open.
    st::GroupFocus f;
    f.pin(12);
    f.update(frame(gs));
    expect(f.update(frame({{20, 21}, {10, 11, 12}})) == 1, "the open group follows its tracks when the groups reorder");
    expect(f.update(frame({{20, 21}, {10, 11, 12, 30}})) == 1, "... and when automation brings another track to it");
    expect(f.update(frame({{20, 21}, {10, 11}})) == -1, "the clicked track drifting away (parted by automation) closes it");
    expect(f.update(frame({{20, 21}, {10, 11, 12}})) == -1, "... and it does not spring open again when it comes back");
  }
}

void testDragRules() {
  const std::vector<std::vector<ObjectId>> gs{{10, 11, 12}, {20, 21}};
  {  // A track dragged onto a closed chip: it opens after resting there, and the tray stays open while over it.
    st::GroupFocus f;
    st::FocusIn in = frame(gs);
    in.dragging = true;
    in.dragOver = 20;
    run(f, in, st::kDwell * 0.6);
    expect(f.open() == -1, "passing over a chip during a drag does not open it");
    run(f, in, st::kDwell * 0.6);
    expect(f.open() == 1, "resting on it a moment opens it (to drop into the tray)");
    run(f, in, 1.0);
    expect(f.open() == 1 && std::fabs(f.openness(1) - 1.0f) < 1e-6f, "it stays open while the drag is over it");
    in.dragOver = kNoId;
    run(f, in, st::kGrace * 0.5);
    expect(f.open() == 1, "leaving it, it stays open a moment (a slip of the pointer)");
    run(f, in, st::kGrace);
    expect(f.open() == -1, "then it closes, so the place behind it is free again");
  }
  {  // A tile lifted out of an open tray: the tray stays open a moment, then closes for the drop behind it.
    st::GroupFocus f;
    f.update(frame(gs));
    f.update(frame(gs, {11}));
    expect(f.open() == 0, "a member chosen: open");
    st::FocusIn in = frame(gs, {11});
    in.dragging = true;
    in.dragFrom = 11;
    in.dragOver = 10;  // still over its own tray
    run(f, in, st::kGrace * 0.5);
    in.dragFrom = kNoId;
    expect(f.open() == 0, "lifting a tile out, its tray stays open a moment");
    run(f, in, st::kGrace);
    expect(f.open() == -1, "then it closes up, even under the pointer, so the tile can go down behind the group");
    run(f, in, st::kDwell);
    expect(f.open() == 0, "resting on its chip a moment opens it again (to put the tile back)");
    in.dragOver = kNoId;
    run(f, in, st::kGrace + 0.05);
    expect(f.open() == -1, "leaving it, it closes after the grace");
    // Dropped behind the group on its own: the choice is that track, outside the group.
    f.update(frame({{10, 12}, {20, 21}}, {11}));
    expect(f.open() == -1, "after the drop it stays closed");
  }
  {  // A drop onto the chip joins: the dropped track (chosen) is a member now, so the group shows it.
    st::GroupFocus f;
    f.update(frame(gs, {5}));
    st::FocusIn in = frame(gs, {5});
    in.dragging = true;
    in.dragOver = 10;  // straight onto the chip, no wait
    f.update(in);
    expect(f.open() == -1, "dropped straight onto a closed chip: it has not opened yet");
    expect(f.update(frame({{10, 11, 12, 5}, {20, 21}}, {5})) == 0, "a track dropped onto a chip opens its group, showing it joined");
    expect(f.update(frame({{10, 11, 12, 5}, {20, 21}}, {5})) == 0, "... and it stays open");
  }
}

void testAnimation() {
  const std::vector<std::vector<ObjectId>> gs{{10, 11, 12}, {20, 21}};
  st::GroupFocus f;
  f.pin(10);
  f.update(frame(gs));
  expect(f.openness(0) > 0 && f.openness(0) < 1, "the tray opens over a few frames (no jump)");
  run(f, frame(gs), st::kOpenTime + 0.02);
  expect(f.openness(0) == 1.0f && f.openness(1) == 0.0f, "after the opening time it is fully open, the others closed");
  f.close();
  f.update(frame(gs));
  expect(f.openness(0) < 1 && f.openness(0) > 0, "closing animates too");
  run(f, frame(gs), st::kOpenTime + 0.02);
  expect(f.openness(0) == 0.0f, "... until it is the chip again");
  // A member lifted out mid-way keeps the group's animation (it is carried by the tracks).
  f.pin(10);
  run(f, frame(gs), st::kOpenTime + 0.02);
  f.update(frame({{10, 12}, {20, 21}}));
  expect(f.openness(0) == 1.0f, "a group that loses a member keeps its openness");
  expect(st::openEase(0) == 0 && st::openEase(1) == 1 && st::openEase(0.5f) > 0.5f, "the ease starts fast and settles");
  const st::Rect a{0, 0, 10, 10}, b{10, 20, 30, 40};
  const st::Rect m = st::lerpRect(a, b, 0.5f);
  expect(m.x0 == 5 && m.y0 == 10 && m.x1 == 20 && m.y1 == 25, "the box grows from the chip to the tray");
}

}  // namespace

int main() {
  testChip();
  testDropBeside();
  testFocusRules();
  testDragRules();
  testAnimation();
  return finish();
}
