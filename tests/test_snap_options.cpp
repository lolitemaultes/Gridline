// Gridline — GitHub #58: every grid and snap option (beat fractions, dotted values, quintuplets
// and septuplets, Main, a pattern's own steps) and the fractional grid they need.
#include <set>
#include <string>
#include "../src/core/Meter.h"
#include "../src/core/Song.h"
#include "../src/core/Timeline.h"
#include "Check.h"

using namespace gl;
using namespace gl::timeline;

int main() {
  Song s = makeEmptySong();  // 96 PPQ, 4/4
  const Tick bar = s.barTicks(), beat = s.beatTicks(), step = s.ppq / 4;
  MeterMap m;
  m.setConstant(4, 4, s.ppq);

  // Every mode has a name of its own, and the names round-trip.
  std::set<std::string> names;
  for (int i = 0; i < kSnapCount; ++i) names.insert(snapName(static_cast<Snap>(i)));
  expect(static_cast<int>(names.size()) == kSnapCount, "every snap mode has its own name");
  expect(std::string(snapName(Snap::BeatFifth)) == "1/5 beat" && std::string(snapName(Snap::StepDotted)) == "Dotted step", "new names");

  // Beat fractions and dotted values.
  expect(snapTicks(s, Snap::BeatHalf) == 48 && snapTicks(s, Snap::BeatThird) == 32 && snapTicks(s, Snap::BeatQuarter) == 24 &&
             snapTicks(s, Snap::BeatSixth) == 16,
         "beat fractions at 96 PPQ");
  expect(snapTicks(s, Snap::StepDotted) == 36 && snapTicks(s, Snap::BeatHalfDotted) == 72 && snapTicks(s, Snap::BeatDotted) == 144,
         "dotted values");
  expect(!snapSize(s, Snap::BeatThird).fractional() && !snapSize(s, Snap::BeatDotted).fractional(), "whole-tick sizes are plain");

  // Quintuplets and septuplets: 96 ticks cannot split in 5 or 7, the lines still land on every beat.
  const SnapGrid q5 = snapSize(s, Snap::BeatFifth), q7 = snapSize(s, Snap::BeatSeventh);
  expect(q5.fractional() && q5.num == 96 && q5.den == 5, "1/5 beat is 96/5 ticks");
  expect(q5.at(0) == 0 && q5.at(1) == 19 && q5.at(2) == 38 && q5.at(3) == 58 && q5.at(4) == 77 && q5.at(5) == 96, "quintuplet lines");
  bool onBeats = true;
  for (int64_t k = 0; k <= 5 * 64; k += 5) onBeats = onBeats && q5.at(k) == (k / 5) * beat;
  for (int64_t k = 0; k <= 7 * 64; k += 7) onBeats = onBeats && q7.at(k) == (k / 7) * beat;
  expect(onBeats, "tuplet lines never drift: every 5th (7th) line is a beat, 64 beats on");
  expect(snapNearest(20, q5) == 19 && snapNearest(30, q5) == 38 && snapNearest(95, q5) == 96, "snap nearest on a quintuplet grid");
  expect(snapDown(57, q5) == 38 && snapDown(58, q5) == 58 && snapDown(-1, q5) == -19, "snap down on a quintuplet grid, negatives too");
  expect(q5.indexDown(96 * 3 + 20) == 16, "line index under a tick");
  expect(static_cast<Tick>(q5) == 19 && static_cast<Tick>(q7) == 14, "rounded size for nudges and labels");
  expect(snapTick(40, q5, Snap::BeatFifth) == 38 && snapTick(40, q5, Snap::Cell) == 38, "snapTick with a fractional grid");
  // A drag moves the notes from line to line, so a quintuplet stays on the quintuplet grid.
  expect(moveDistance(19, q5, Snap::BeatFifth, 0) == 19 && moveDistance(38, q5, Snap::BeatFifth, 19) == 39, "move by quintuplet lines");
  expect(moveDistance(-500, q5, Snap::BeatFifth, 38) == -38, "the wall at 0");
  expect(moveDistance(19, q5, Snap::BeatFifth, 5) == 19, "off the grid: the offset from its line is kept");
  // Map versions agree for a constant map and for a map with meter changes.
  expect(snapTick(m, 40, q5, Snap::BeatFifth) == 38 && moveDistance(m, 19, q5, Snap::BeatFifth, 0) == 19, "map: same as plain");
  {
    Song sc = makeEmptySong();
    Marker mk;
    mk.pos = 3 * bar;
    mk.timeSigNum = 3;
    mk.timeSigDen = 4;
    sc.markers.push_back(mk);
    MeterMap mc(sc);
    expect(!mc.constant() && snapTick(mc, 40, q5, Snap::BeatFifth) == 38, "map with meter changes: the absolute tuplet grid");
  }
  std::vector<Tick> lines;
  forEachLine(m, 0, beat + 1, q5, [&](Tick t, int) { lines.push_back(t); });
  expect(lines == std::vector<Tick>({0, 19, 38, 58, 77, 96}), "forEachLine walks a fractional grid");
  expect(gridName(m, q5) == "1/5 beat" && gridName(m, q7) == "1/7 beat", "fractional grid names");

  // A step that does not split evenly: a 12-step pattern (32-tick steps), its thirds and sixths.
  Pattern p12;
  p12.stepsPerBar = 12;
  const MeterMap pm = MeterMap::forPattern(s, &p12);
  expect(pm.stepTicks() == 32, "a 12-step pattern's step is 32 ticks");
  expect(snapTicks(pm, Snap::Step) == 32 && snapTicks(pm, Snap::Half) == 16, "Step follows the pattern's steps");
  expect(snapSize(pm, Snap::Third) == SnapGrid(32, 3) && snapSize(pm, Snap::Third).at(3) == 32, "1/3 of a 32-tick step, exact");
  Pattern p24;
  p24.stepsPerBar = 24;
  expect(snapTicks(MeterMap::forPattern(s, &p24), Snap::Step) == 16, "24 steps: 16-tick steps");
  Pattern p16;
  expect(MeterMap::forPattern(s, &p16).stepTicks() == step && MeterMap::forPattern(s, nullptr).stepTicks() == step, "16 steps: a sixteenth");
  expect(visibleGrid(pm, 0.5) == 32, "the zoom ladder of a 12-step pattern has its steps");
  expect(rulerLabel(pm, 32, 32) == "1.1.2", "ruler steps count the pattern's steps");

  // Main follows the main snap (the playlist's); Main of Main reads as Line.
  setMainSnap(Snap::Beat);
  expect(resolveSnap(Snap::Main) == Snap::Beat && snapTicks(s, Snap::Main) == beat, "Main follows the main snap");
  expect(snapGrid(s, Snap::Main, 1.0, false) == beat, "Main's grid is the main snap's");
  setMainSnap(Snap::Line);
  expect(adaptiveSnap(Snap::Main) && snapGrid(s, Snap::Main, 1.0, false) == step, "Main of an adaptive snap follows the zoom");
  setMainSnap(Snap::Main);
  expect(resolveSnap(Snap::Main) == Snap::Line, "Main of Main is Line");
  setMainSnap(Snap::Cell);
  expect(snapTick(70, SnapGrid(beat), Snap::Main) == 0, "Main of Cell snaps to the cell start");
  setMainSnap(Snap::Line);

  // Which modes draw their own lines: fixed, finer than a beat, far enough apart.
  expect(drawsSnapLines(m, Snap::BeatThird, 0.5) && drawsSnapLines(m, Snap::BeatFifth, 0.5), "triplets and tuplets draw their lines");
  expect(!drawsSnapLines(m, Snap::Line, 0.5) && !drawsSnapLines(m, Snap::Beat, 0.5) && !drawsSnapLines(m, Snap::None, 4.0), "adaptive, beat, none: no");
  expect(!drawsSnapLines(m, Snap::BeatFifth, 0.1), "too dense: no");

  // Alt still inverts every mode.
  expect(snapGrid(s, Snap::BeatFifth, 1.0, true) == 1, "Alt frees a tuplet snap");
  expect(SnapGrid(5, 7) == SnapGrid(1), "never finer than a tick");
  {  // K-255: the Row grid snap is the edited row's own step (5 or 7 a bar, exact)
    Song t = makeEmptySong();
    Pattern p;
    p.id = 900;
    p.bars = 2;
    p.stepsPerBar = 16;
    t.patterns.push_back(p);
    const Pattern* tp = &t.patterns.back();
    const ObjectId ch5 = 11, ch7 = 12, plain = 13;
    t.patterns.back().lanes[ch5].grid = 5;
    t.patterns.back().lanes[ch7].grid = 7;
    const MeterMap m5 = MeterMap::forPattern(t, tp, ch5), m7 = MeterMap::forPattern(t, tp, ch7), mp = MeterMap::forPattern(t, tp, plain);
    expect(snapSize(m5, Snap::RowGrid) == SnapGrid(bar, 5), "row grid of 5 a bar: a fifth of the bar");
    expect(snapSize(m7, Snap::RowGrid) == SnapGrid(bar, 7), "row grid of 7 a bar: a seventh of the bar");
    expect(snapSize(mp, Snap::RowGrid) == SnapGrid(step), "a row on the pattern's grid: the row grid is a step");
    expect(snapSize(m, Snap::RowGrid) == SnapGrid(step), "no row: a step");
    const SnapGrid g5 = snapSize(m5, Snap::RowGrid), g7 = snapSize(m7, Snap::RowGrid);
    expect(g5.at(5) == bar && g7.at(7) == bar && g7.at(14) == 2 * bar, "the lines meet the bar lines");
    expect(g5.at(1) == 77 && g5.at(2) == 154 && g7.at(1) == 55 && g7.at(3) == 165, "lines at the rounded exact places");
    expect(g7.indexDown(166) == 3 && g7.indexDown(164) == 2, "snapping down finds the seventh under it");
    expect(gridName(m7, g7) == "Row grid" && gridName(m5, g5) == "Row grid", "the grid's name");
    expect(std::string(snapName(Snap::RowGrid)) == "Row grid", "name");
    // A grid that divides the bar: whole ticks.
    t.patterns.back().lanes[ch5].grid = 24;
    expect(snapSize(MeterMap::forPattern(t, &t.patterns.back(), ch5), Snap::RowGrid) == SnapGrid(16), "24 a bar: 16 ticks");
  }
  (void)bar;
  return finish();
}
