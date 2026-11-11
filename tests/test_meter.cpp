// Gridline — meter changes across the song (K-179): the meter map, song-time grid and snapping,
// pattern meters (fixed tick lengths, so tracks stay even), pinning on a start meter change,
// project save and load, undo.
#include <cmath>
#include <cstdio>
#include <string>
#include "../src/core/Document.h"
#include "../src/core/Json.h"
#include "../src/core/Meter.h"
#include "../src/core/MeterEdit.h"
#include "../src/core/Project.h"
#include "../src/core/Song.h"
#include "../src/core/Timeline.h"
#include "Check.h"

using namespace gl;

namespace {

Marker meterMarker(Tick pos, int num, int den) {
  Marker m;
  m.pos = pos;
  m.timeSigNum = num;
  m.timeSigDen = den;
  return m;
}

// 8 bars of 4/4, then 6/8 for 4 bars, then 3/4 (at 96 PPQ).
Song threeMeters() {
  Song s = makeEmptySong();
  s.markers.push_back(meterMarker(8 * 384, 6, 8));
  s.markers.push_back(meterMarker(8 * 384 + 4 * 288, 3, 4));
  return s;
}

void constantMatchesOldMaths() {
  bool ok = true;
  for (int ppq : {24, 48, 96, 192, 384, 960})
    for (int den : {2, 4, 8, 16})
      for (int num = 1; num <= 16; ++num) {
        Song s = makeEmptySong();
        s.ppq = ppq;
        s.timeSigNum = num;
        s.timeSigDen = den;
        MeterMap m(s);
        ok = ok && m.constant() && m.baseBar() == s.barTicks() && m.baseBeat() == s.beatTicks();
        const Tick bar = s.barTicks(), beat = s.beatTicks();
        for (Tick t = 0; t < 5 * bar && ok; t += std::max<Tick>(1, beat / 3)) {
          const MeterMap::Bbt b = m.toBbt(t);
          ok = ok && b.bar == t / bar && b.beat == (t % bar) / beat && b.tick == t % beat;
          ok = ok && m.barStartAt(t) == t / bar * bar && m.nextBar(t) == (t / bar + 1) * bar;
          ok = ok && m.beatStartAt(t) == t / beat * beat && m.barLenAt(t) == bar && !m.shortBar(t);
          for (Tick g : {bar, 4 * bar, beat, std::max<Tick>(1, Tick(ppq / 4)), Tick(1)})
            for (Snap mode : {Snap::Line, Snap::Cell})
              ok = ok && timeline::snapTick(m, t, g, mode) == timeline::snapTick(t, g, mode) &&
                   timeline::moveDistance(m, t - 2 * bar, g, mode, 3 * bar) == timeline::moveDistance(t - 2 * bar, g, mode, 3 * bar) &&
                   timeline::rulerLabel(m, t, g) == timeline::rulerLabel(s, t, g);
        }
      }
  expect(ok, "one meter: the map equals the old song-wide maths at every PPQ and meter");
}

void threeMeterMap() {
  const Song s = threeMeters();
  const MeterMap m(s);
  expect(m.segments().size() == 3 && !m.constant(), "4/4, 6/8, 3/4: three sections");
  const Tick c1 = 8 * 384, c2 = c1 + 4 * 288;
  expect(m.barStart(8) == c1 && m.barStart(9) == c1 + 288 && m.barStart(12) == c2 && m.barStart(13) == c2 + 288,
         "bar starts follow each section's bar length");
  expect(m.segAt(c1).group == 3 && m.segAt(0).group == 0 && m.segAt(c2).group == 0, "6/8 is compound (groups of three)");
  bool rt = true;
  for (Tick t = 0; t < m.barStart(40); ++t) {
    const MeterMap::Bbt b = m.toBbt(t);
    const MeterSeg& g = m.segAt(t);
    rt = rt && m.barStart(b.bar) + b.beat * g.beat + b.tick == t && b.tick < g.beat && b.beat < g.num;
  }
  expect(rt, "toBbt and barStart round trip for every tick in 40 bars");
  expect(m.barStartAt(c1 - 1) == c1 - 384 && m.nextBar(c1 - 1) == c1 && m.barStartAt(c1) == c1 && m.nextBar(c1) == c1 + 288,
         "nextBar / barStartAt at and around a change");
  expect(m.nextBeat(c1 - 1) == c1 && m.nextBeat(c1) == c1 + 48 && m.beatLenAt(c1) == 48, "beats switch at the change");
  expect(m.accentAt(c1) == 2 && m.accentAt(c1 + 3 * 48) == 1 && m.accentAt(c1 + 48) == 0 && m.accentAt(c2 + 96) == 0,
         "accents: bar start, compound group, plain beat");
  expect(m.addBars(c1 - 384, 1) == c1 && m.addBars(c1 - 384, 2) == c1 + 288 && m.addBars(c1 + 10, -1) == c1 - 384 + 10,
         "addBars walks real bars, keeping the offset");
  MeterMap copy(s);
  expect(copy.signature() == m.signature() && MeterMap(makeEmptySong()).signature() != m.signature(), "signature tells maps apart");
}

void edgeCases() {
  Song s = makeEmptySong();
  s.markers.push_back(meterMarker(2 * 384 + 192, 6, 8));  // mid-bar: bar 3 is cut short
  MeterMap m(s);
  expect(m.segments().size() == 2, "mid-bar change makes a section");
  expect(m.shortBar(2 * 384 + 10) && m.barLenAt(2 * 384 + 10) == 192 && m.barIndex(2 * 384 + 10) == 2,
         "a mid-bar change leaves a short bar that keeps its number");
  expect(m.barStart(3) == 2 * 384 + 192 && m.barIndex(2 * 384 + 192) == 3, "the next bar starts at the change");
  s.markers = {meterMarker(384, 3, 4), meterMarker(384, 5, 4)};
  m.build(s);
  expect(m.segAt(384).num == 5, "two changes at one tick: the later one wins");
  s.markers = {meterMarker(0, 3, 4)};
  m.build(s);
  expect(m.constant() && m.segAt(0).num == 3, "a change at the start overrides the song's meter");
  s.markers = {meterMarker(384, 40, 6)};
  m.build(s);
  expect(m.segAt(384).num == 32 && m.segAt(384).den == 8, "invalid meters clamp (num to 32, den to a power of two)");
  s.ppq = 24;
  s.markers = {meterMarker(96, 3, 32)};
  m.build(s);
  expect(m.segments().size() == 2 && m.segAt(96).beat == 3, "32nds at 24 PPQ still divide");
  int n = 7, d = 64;
  expect(!normalizeMeter(n, d, 1), "a beat smaller than a tick is refused");
  s.ppq = 96;
  s.markers = {meterMarker(4 * 384, 4, 4)};
  m.build(s);
  expect(m.constant(), "a change to the same meter on a bar line changes nothing");
  expect(sixteenthStepsPerBar(4, 4) == 16 && sixteenthStepsPerBar(6, 8) == 12 && sixteenthStepsPerBar(3, 4) == 12 &&
             sixteenthStepsPerBar(7, 8) == 14,
         "steps per bar that keep a step a sixteenth");
}

void gridAndSnap() {
  const Song s = threeMeters();
  const MeterMap m(s);
  const Tick c1 = 8 * 384, c2 = c1 + 4 * 288;
  expect(timeline::snapTick(m, c1 + 150, 384, Snap::Line) == c1 + 288 && timeline::snapTick(m, c1 + 100, 384, Snap::Line) == c1,
         "Bar snaps to the bar lines of the section");
  expect(timeline::snapTick(m, c1 + 70, 96, Snap::Line) == c1 + 48 && timeline::snapTick(m, c1 + 70, 96, Snap::Cell) == c1 + 48,
         "Beat snaps to the section's beats (eighths in 6/8)");
  expect(timeline::snapTick(m, c1 + 13, 24, Snap::Line) == c1 + 24, "steps stay absolute sixteenths");
  // Bar lines walked per section: 8 in 4/4, 4 in 6/8, then 3/4 bars.
  int bars = 0, groups = 0;
  timeline::forEachLine(m, 0, c2, 384, [&](Tick, int st) { bars += st == timeline::kLineBar; });
  timeline::forEachLine(m, c1, c2, 96, [&](Tick, int st) { groups += st == timeline::kLineGroup; });
  expect(bars == 12, "bar lines: 8 bars of 4/4 and 4 bars of 6/8");
  expect(groups == 4, "beat lines in 6/8 show one group line per bar");
  int beats = 0;
  timeline::forEachLine(m, c2, c2 + 3 * 288, 96, [&](Tick, int) { ++beats; });
  expect(beats == 9, "3/4: three beats per bar");
  Tick prev = -1;
  bool ordered = true;
  timeline::forEachLine(m, 0, c2 + 1000, 24, [&](Tick t, int) { ordered = ordered && t > prev, prev = t; });
  expect(ordered, "lines come in order");
  // A clip on a 4/4 bar line dragged about a bar into 6/8 lands on a 6/8 bar line.
  const Tick from = 7 * 384;
  const Tick d = timeline::moveDistance(m, 384 + 300, 384, Snap::Line, from);
  expect(from + d == c1 + 288, "bar snap moves by bar lines crossed into the next section");
  expect(timeline::rulerLabel(m, c1, 384) == "9" && timeline::rulerLabel(m, c1 + 3 * 48, 96) == "9.4" &&
             timeline::rulerLabel(m, c2, 384) == "13",
         "ruler numbers count on across changes");
  // Short bar at a mid-bar change.
  Song sb = makeEmptySong();
  sb.markers.push_back(meterMarker(384 + 192, 3, 4));
  const MeterMap ms(sb);
  expect(timeline::snapTick(ms, 384 + 150, 384, Snap::Line) == 384 + 192, "Bar snap includes the short bar's end");
}

void patternsKeepTheirLength() {
  Document d{makeEmptySong()};
  const ObjectId p = d.song().patterns.front().id;
  d.setPatternLength(p, 2, 16);
  d.addClip(p, 0, 0);
  d.addClip(p, 1, 9 * 384);
  const Song before = d.song();
  const Tick len = before.clipLength(before.playlist[0]);
  // Meter changes are notation: clips and notes never move or resize.
  meter::add(d, 8 * 384 + 10, 6, 8);
  meter::add(d, 12 * 384, 3, 4);
  bool same = d.song().playlist == before.playlist && d.song().patterns == before.patterns;
  for (const auto& c : d.song().playlist) same = same && d.song().clipLength(c) == len;
  expect(same, "adding meter changes leaves every clip start and length as it was (even tracks)");
  expect(d.song().markers.size() == 2 && d.song().markers[0].pos == 8 * 384, "a new change snaps to the bar line");
  meter::move(d, 0, 10 * 384 + 50);
  meter::remove(d, 1);
  same = d.song().playlist == before.playlist && d.song().patterns == before.patterns;
  expect(same, "moving and removing meter changes leaves clips as they were");
  expect(d.song().markers.size() == 1 && d.song().markers[0].pos == 10 * 384, "a moved change snaps to the nearest bar line");
  // Undo walks back to the exact song.
  while (d.undo()) {
  }
  expect(d.song() == makeEmptySong() || d.song().markers.empty(), "undo removes every meter edit");
}

void startMeterPins() {
  Document d{makeEmptySong()};
  const ObjectId p = d.song().patterns.front().id;
  d.setPatternLength(p, 4, 16);
  d.addClip(p, 0, 0);
  const Tick len = d.song().clipLength(d.song().playlist[0]);
  const Song before = d.song();
  d.setTimeSignature(3, 4);
  const Pattern& pp = *d.song().pattern(p);
  expect(pp.meterNum == 4 && pp.meterDen == 4 && d.song().clipLength(d.song().playlist[0]) == len,
         "changing the start meter pins existing patterns: their length stays");
  d.undo();
  expect(d.song() == before, "pinning and the change undo as one step");
  d.setTimeSignature(3, 4, true);
  expect(!d.song().pattern(p)->ownMeter() && d.song().clipLength(d.song().playlist[0]) == 4 * 288,
         "with 'also change patterns' they follow the new meter, as before");
  d.undo();
  d.setTimeSignature(3, 4);
  d.setTimeSignature(4, 4);
  expect(!d.song().pattern(p)->ownMeter() && d.song().clipLength(d.song().playlist[0]) == len,
         "back to the pinned meter: the pattern follows the song again");
}

void patternMeters() {
  Document d{makeEmptySong()};
  meter::add(d, 4 * 384, 6, 8);
  int num = 0, den = 0, steps = 0;
  meter::newPatternMeter(d.song(), 4 * 384 + 20, num, den, steps);
  expect(num == 6 && den == 8 && steps == 12, "a new pattern in a 6/8 section: 6/8 with 12 steps (a step a sixteenth)");
  meter::newPatternMeter(d.song(), 10, num, den, steps);
  expect(num == 0 && den == 0 && steps == 16, "a new pattern at the start follows the song (16 steps)");
  meter::setForPatterns(d, 0, false);
  meter::newPatternMeter(d.song(), 4 * 384 + 20, num, den, steps);
  expect(num == 0 && steps == 16, "the section's 'new patterns' option off: follows the song");
  const ObjectId np = d.addPattern("x", 2, 12, -1, 6, 8);
  const Pattern& p = *d.song().pattern(np);
  expect(d.song().patternLength(p) == 2 * 288 && d.song().ticksPerStep(p) == 24, "pattern length and step in its own meter");

  // Length never depends on where it sits.
  const ObjectId c0 = d.addClip(np, 0, 0), c1 = d.addClip(np, 1, 4 * 384);
  expect(d.song().clipLength(*d.song().clip(c0)) == d.song().clipLength(*d.song().clip(c1)), "a pattern is as long wherever it sits");
  // Make it match a 4/4 section, keeping its ticks: 576 ticks is 1.5 bars of 4/4, refused; 3/4 fits.
  expect(!meter::matchPatternMeter(d, np, 0), "matching refuses when the length is not whole bars");
  meter::add(d, 8 * 384, 3, 4);
  expect(meter::matchPatternMeter(d, np, 8 * 384) && d.song().pattern(np)->bars == 2 &&
             d.song().patternLength(*d.song().pattern(np)) == 576 && d.song().pattern(np)->stepsPerBar == 12,
         "matching keeps the length and the step size");
  const ObjectId q = d.addPattern("y");
  meter::setPatternMeter(d, q, 7, 8);
  expect(d.song().pattern(q)->meterNum == 7 && d.song().pattern(q)->stepsPerBar == 14, "an empty pattern's steps follow its new meter");
  meter::setPatternMeter(d, q, 4, 4);
  expect(!d.song().pattern(q)->ownMeter() && d.song().pattern(q)->stepsPerBar == 16, "the song's meter again: follows the song");
}

void projectRoundTrip() {
  Song s = threeMeters();
  s.patterns.front().meterNum = 7;
  s.patterns.front().meterDen = 8;
  s.markers[1].meterForPatterns = false;
  Song back;
  std::string err;
  expect(songFromJson(songToJson(s), back, &err) && back == s, "pattern meters and meter changes save and load");
  // Projects without meter features write no new keys.
  const Song plain = makeEmptySong();
  const std::string text = jsonCompact(songToJson(plain));
  expect(text.find("meterNum") == std::string::npos && text.find("meterForPatterns") == std::string::npos,
         "a project without meter changes saves as before (no new keys)");
  // Old projects load as 0/0 (follow the song).
  Json j = songToJson(plain);
  Song old;
  expect(songFromJson(j, old) && !old.patterns.front().ownMeter(), "old patterns follow the song's meter");
  // Bad values are dropped.
  Song bad = makeEmptySong();
  bad.patterns.front().meterNum = 5;
  bad.patterns.front().meterDen = 4;
  Json jb = songToJson(bad);
  for (auto& jp : jb.object["patterns"].array) jp.object["meterDen"].number = 0;
  Song badBack;
  expect(songFromJson(jb, badBack) && !badBack.patterns.front().ownMeter(), "a broken pattern meter loads as 'follows the song'");
}

}  // namespace

int main() {
  constantMatchesOldMaths();
  threeMeterMap();
  edgeCases();
  gridAndSnap();
  patternsKeepTheirLength();
  startMeterPins();
  patternMeters();
  projectRoundTrip();
  return finish();
}
