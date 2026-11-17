// Gridline: recording notes from MIDI and the typing keyboard (K-226). Each block reproduces one of the
// wrong results found (timing, undo, pattern loops, replace, song-mode takes, loops, quantize, count-in,
// punch, options kept) and checks the fixed behaviour. Everything runs on simulated input.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>
#include "Check.h"
#include "core/Document.h"
#include "core/MidiRecord.h"
#include "core/PatternAutomation.h"
#include "core/Settings.h"
#include "core/TrackModes.h"
#include "engine/TakeLanes.h"
#include "engine/AudioEngine.h"
#include "engine/Program.h"

using namespace gl;

namespace {

constexpr Tick kBar = 384;  // makeEmptySong: 4/4, 96 ticks a beat

std::vector<Note> notesOf(const Document& d, ObjectId pat, ObjectId ch) {
  const Pattern* p = d.song().pattern(pat);
  if (!p) return {};
  auto it = p->notes.find(ch);
  if (it == p->notes.end()) return {};
  std::vector<Note> v = it->second;
  std::sort(v.begin(), v.end(), [](const Note& a, const Note& b) { return a.start < b.start || (a.start == b.start && a.pitch < b.pitch); });
  return v;
}

bool has(const std::vector<Note>& v, Tick start, int pitch, Tick length = -1) {
  for (const Note& n : v)
    if (n.start == start && n.pitch == pitch && (length < 0 || n.length == length)) return true;
  return false;
}

int clipsOf(const Song& s, ObjectId pat) {
  int n = 0;
  for (const auto& c : s.playlist) n += c.kind == ClipKind::Pattern && c.pattern == pat ? 1 : 0;
  return n;
}

const Clip* clipOf(const Song& s, ObjectId pat) {
  for (const auto& c : s.playlist)
    if (c.kind == ClipKind::Pattern && c.pattern == pat) return &c;
  return nullptr;
}

RecordOptions raw() {  // no quantize: positions as played
  RecordOptions o;
  o.quantize = false;
  o.grid = 1;
  return o;
}

struct Rig {
  Document doc;
  ObjectId ch = kNoId, pat = kNoId;
  Rig() : doc(makeEmptySong()) {
    ch = doc.addChannel("Keys", "gl.synth");
    pat = doc.song().patterns[0].id;
  }
};

// ---- 0.1: the tick heard when a key was played ----
void testTiming() {
  // The engine logs when it rendered what; a key played at time t records at the tick heard at t (what
  // the engine rendered one output latency earlier), whatever the buffer size and the UI frame.
  for (int buf : {64, 256, 1024}) {
    Song s = makeEmptySong();
    s.patterns[0].bars = 4;
    const double sr = 48000.0, tps = s.tempo / 60.0 * s.ppq;
    ProgramBuilder b;
    AudioEngine eng(sr);
    eng.setProgram(b.build(s));
    eng.setPattern(s.patterns[0].id);
    eng.setMode(PlayMode::Pattern);
    eng.play();
    std::vector<float> L(static_cast<size_t>(buf)), R(static_cast<size_t>(buf));
    const double lat = buf / sr;  // the device buffer the performer hears through
    double worstNew = 0, worstOld = 0;
    int k = 0;
    // A key every 37 ms for two seconds; the UI sees it at its next 16 ms frame.
    for (double key = 0.1; key < 2.0; key += 0.037) {
      const double frame = std::ceil(key / 0.016) * 0.016;
      while (static_cast<double>(k) * buf / sr <= frame) {
        eng.setTestWallClock(static_cast<double>(k) * buf / sr);
        eng.process(L.data(), R.data(), buf);
        ++k;
      }
      const double want = (key - lat) * tps;
      double got = -1;
      const bool ok = eng.heardTick(key, lat, &got);
      worstNew = std::max(worstNew, ok ? std::fabs(got - want) : 1e9);
      worstOld = std::max(worstOld, std::fabs(eng.position() - want));  // what recording used before
    }
    char name[160];
    std::snprintf(name, sizeof name, "timing, %d-frame buffer: notes record at the tick heard (within 0.01 tick; position() was %.1f ticks off)", buf,
                  worstOld);
    expect(worstNew < 0.01, name);
    if (buf == 1024) expect(worstOld > 4.0, "timing: the transport position runs ahead of what is heard (the old cause)");
  }
  {
    // Over a pattern loop wrap and in a count-in: the stretches keep their own ticks.
    Song s = makeEmptySong();
    const double sr = 48000.0, tps = s.tempo / 60.0 * s.ppq;
    ProgramBuilder b;
    AudioEngine eng(sr);
    eng.setProgram(b.build(s));
    eng.setPattern(s.patterns[0].id);
    eng.setMode(PlayMode::Pattern);
    eng.playWithCountIn(static_cast<double>(kBar));
    const int buf = 256;
    std::vector<float> L(buf), R(buf);
    const double barSec = static_cast<double>(kBar) / tps;
    int k = 0;
    while (static_cast<double>(k) * buf / sr < 2.6 * barSec) {
      eng.setTestWallClock(static_cast<double>(k) * buf / sr);
      eng.process(L.data(), R.data(), buf);
      ++k;
    }
    double t = 0;
    eng.heardTick(0.75 * barSec, 0, &t);  // the count-in's last beat
    expect(std::fabs(t - (-0.25 * kBar)) < 0.5, "count-in: heard as the ticks before the start (a pickup a beat early is at -96)");
    eng.heardTick(1.5 * barSec, 0, &t);
    const bool mid = std::fabs(t - 0.5 * kBar) < 0.5;
    eng.heardTick(2.25 * barSec, 0, &t);  // after the loop wrapped
    expect(mid && std::fabs(t - 0.25 * kBar) < 0.5, "pattern loop: a key after the wrap is heard at the pattern's start again");
  }
  {
    // T2: a note-on and its note-off handled in one UI frame still get their own positions.
    Rig r;
    NoteRecorder rec;
    rec.beginPattern(r.doc, r.pat, raw(), kBar);
    rec.noteOn(r.doc, r.ch, 60, 100, 100.0);  // both stamped, so both have their own heard ticks
    rec.noteOff(r.doc, r.ch, 60, 112.0);
    rec.end(r.doc, 200);
    const auto ns = notesOf(r.doc, r.pat, r.ch);
    expect(ns.size() == 1 && ns[0].start == 100 && ns[0].length == 12, "a short note keeps its length (not one tick)");
  }
}

// ---- 0.2: one recording, one undo step ----
void testUndo() {
  Rig r;
  r.doc.addNote(r.pat, r.ch, Note(300, 24, 40, 90));  // there before
  NoteRecorder rec;
  rec.beginPattern(r.doc, r.pat, raw(), kBar);
  bool visible = true;
  for (int i = 0; i < 4; ++i) {
    rec.noteOn(r.doc, r.ch, 60 + i, 100, 10.0 + 50 * i);
    rec.noteOff(r.doc, r.ch, 60 + i, 30.0 + 50 * i);
    visible = visible && notesOf(r.doc, r.pat, r.ch).size() == static_cast<size_t>(2 + i);  // shown as played
  }
  // A linked control moved during the take: part of the same step.
  const ParamId vol = channelParam(r.ch, ChannelParam::Volume);
  rec.edit(r.doc, [&] { r.doc.setParam(vol, 0.3, true); });
  rec.end(r.doc, 300);
  expect(visible, "undo: recorded notes appear as they are played");
  r.doc.undo();
  const auto ns = notesOf(r.doc, r.pat, r.ch);
  expect(ns.size() == 1 && ns[0].pitch == 40, "undo: one undo removes the whole take (all four notes)");
  r.doc.redo();
  expect(notesOf(r.doc, r.pat, r.ch).size() == 5, "undo: redo brings the take back");
}

// ---- 0.3: a pattern never grows during a take ----
void testPatternLoop() {
  Rig r;
  NoteRecorder rec;
  rec.beginPattern(r.doc, r.pat, raw(), kBar);
  rec.noteOn(r.doc, r.ch, 60, 100, 360.0);
  rec.position(r.doc, 383.0);
  rec.position(r.doc, 6.0);  // wrapped while held
  rec.noteOff(r.doc, r.ch, 60, 84.0);
  const Pattern* p = r.doc.song().pattern(r.pat);
  auto ns = notesOf(r.doc, r.pat, r.ch);
  expect(ns.size() == 1 && ns[0].start == 360 && ns[0].length == 24 && r.doc.song().patternContentLength(*p) == kBar,
         "pattern loop: a note held over the loop end is cut there, the pattern stays one bar (case 1)");
  // The pattern made two bars long meanwhile: the loop follows it, a note in the new bar stays there.
  r.doc.setPatternLength(r.pat, 2, 16);
  rec.setLoopLength(2 * kBar);
  rec.noteOn(r.doc, r.ch, 62, 100, 500.0);
  rec.noteOff(r.doc, r.ch, 62, 520.0);
  rec.end(r.doc, 530);
  ns = notesOf(r.doc, r.pat, r.ch);
  expect(has(ns, 500, 62, 20) && !has(ns, 116, 62), "pattern loop: a note at tick 500 of a longer loop is not folded into bar 1 (case 2)");
  // Quantized onto the loop end, a note lands on the loop's start and never past the end.
  Rig q;
  RecordOptions o;
  o.grid = 96;
  o.quantizeLength = true;
  NoteRecorder rq;
  rq.beginPattern(q.doc, q.pat, o, kBar);
  rq.noteOn(q.doc, q.ch, 64, 100, 340.0);  // to 384: the start of the loop
  rq.noteOff(q.doc, q.ch, 64, 380.0);
  rq.end(q.doc, 380);
  const auto qn = notesOf(q.doc, q.pat, q.ch);
  expect(q.doc.song().patternContentLength(*q.doc.song().pattern(q.pat)) == kBar && has(qn, 0, 64, 96),
         "pattern loop: quantize keeps notes inside the loop");
}

// ---- 0.4: replace clears only what a pass played over ----
void testReplace() {
  {  // case 3: one note in bar 1 of a four-bar pattern
    Rig r;
    r.doc.setPatternLength(r.pat, 4, 16);
    for (Tick t : {Tick(0), Tick(48), kBar + 10, 2 * kBar + 10, 3 * kBar + 10}) r.doc.addNote(r.pat, r.ch, Note(t, 24, 50, 90));
    RecordOptions o = raw();
    o.overdub = false;
    NoteRecorder rec;
    rec.beginPattern(r.doc, r.pat, o, 4 * kBar);
    rec.noteOn(r.doc, r.ch, 60, 100, 20.0);
    rec.noteOff(r.doc, r.ch, 60, 40.0);
    rec.end(r.doc, 100);
    const auto ns = notesOf(r.doc, r.pat, r.ch);
    expect(ns.size() == 5 && has(ns, 0, 50) && !has(ns, 48, 50) && has(ns, kBar + 10, 50) && has(ns, 2 * kBar + 10, 50) &&
               has(ns, 3 * kBar + 10, 50) && has(ns, 20, 60),
           "replace: only the range played over is cleared, bars 2 to 4 stay (case 3)");
  }
  {  // case 4: a chord held over the wrap keeps the notes the pass before played
    Rig r;
    RecordOptions o = raw();
    o.overdub = false;
    NoteRecorder rec;
    rec.beginPattern(r.doc, r.pat, o, kBar);
    rec.noteOn(r.doc, r.ch, 55, 100, 100.0);
    rec.noteOff(r.doc, r.ch, 55, 150.0);
    rec.noteOn(r.doc, r.ch, 60, 100, 300.0);
    rec.noteOn(r.doc, r.ch, 64, 100, 300.0);
    rec.position(r.doc, 380.0);
    rec.position(r.doc, 4.0);  // wrapped, the chord still held
    rec.noteOff(r.doc, r.ch, 60, 10.0);
    rec.noteOff(r.doc, r.ch, 64, 12.0);
    rec.end(r.doc, 20.0);
    const auto ns = notesOf(r.doc, r.pat, r.ch);
    expect(ns.size() == 3 && has(ns, 100, 55, 50) && has(ns, 300, 60, 84) && has(ns, 300, 64, 84),
           "replace: a chord held over the wrap does not delete the pass it was played in (case 4)");
  }
  {  // case 10: song mode, a loop: the second pass replaces the first where it plays over it
    Rig r;
    RecordOptions o = raw();
    o.overdub = false;
    RecordSetup st;
    st.loopStart = 0;
    st.loopEnd = kBar;
    NoteRecorder rec;
    rec.beginSong(r.doc, r.pat, o, st);
    rec.noteOn(r.doc, r.ch, 60, 100, 50.0);
    rec.noteOff(r.doc, r.ch, 60, 60.0);
    rec.noteOn(r.doc, r.ch, 61, 100, 200.0);
    rec.noteOff(r.doc, r.ch, 61, 210.0);
    rec.position(r.doc, 383.0);
    rec.position(r.doc, 2.0);  // pass 2
    rec.noteOn(r.doc, r.ch, 70, 100, 40.0);
    rec.noteOff(r.doc, r.ch, 70, 45.0);
    rec.position(r.doc, 150.0);
    rec.end(r.doc, 150.0);
    const auto ns = notesOf(r.doc, rec.pattern(), r.ch);
    expect(ns.size() == 2 && has(ns, 40, 70) && has(ns, 200, 61), "replace, song loop: a later pass replaces the earlier one where it plays (case 10)");
  }
  {  // song mode: the range played over is cleared from the clip there too
    Rig r;
    const ObjectId old = r.doc.addPattern("Old");
    r.doc.addNote(old, r.ch, Note(30, 24, 50, 90));
    r.doc.addNote(old, r.ch, Note(300, 24, 51, 90));
    r.doc.addClip(old, 0, kBar);
    RecordOptions o = raw();
    o.overdub = false;
    RecordSetup st;
    st.start = kBar;
    NoteRecorder rec;
    rec.beginSong(r.doc, r.pat, o, st);
    rec.noteOn(r.doc, r.ch, 60, 100, kBar + 20.0);
    rec.noteOff(r.doc, r.ch, 60, kBar + 40.0);
    rec.end(r.doc, kBar + 100.0);
    const auto on = notesOf(r.doc, old, r.ch);
    expect(on.size() == 1 && has(on, 300, 51), "replace, song mode: the notes played over in the clip there are cleared, the rest stays");
  }
}

// ---- 0.5 / W1: song mode records one take: a new pattern, one clip that grows ----
void testSongTake() {
  Rig r;
  r.doc.addNote(r.pat, r.ch, Note(0, 24, 40, 90));  // the selected pattern
  RecordSetup st;
  NoteRecorder rec;
  rec.beginSong(r.doc, r.pat, raw(), st);
  const Tick at[] = {10, kBar + 10, 2 * kBar + 10};
  for (int i = 0; i < 3; ++i) {
    rec.noteOn(r.doc, r.ch, 60 + i, 100, static_cast<double>(at[i]));
    rec.noteOff(r.doc, r.ch, 60 + i, static_cast<double>(at[i] + 30));
    rec.position(r.doc, static_cast<double>(at[i] + 60));
  }
  rec.end(r.doc, 2 * kBar + 100.0);
  const Song& s = r.doc.song();
  const ObjectId take = rec.pattern();
  const auto ns = notesOf(r.doc, take, r.ch);
  const Clip* c = clipOf(s, take);
  expect(take != kNoId && take != r.pat && clipsOf(s, take) == 1 && c && c->start == 0 && s.clipLength(*c) == 3 * kBar,
         "song mode: one clip for the take, from the bar recording started, grown to three bars (case 5)");
  expect(ns.size() == 3 && has(ns, 10, 60) && has(ns, kBar + 10, 61) && has(ns, 2 * kBar + 10, 62),
         "song mode: every note at its own bar, nothing repeated (case 5)");
  expect(notesOf(r.doc, r.pat, r.ch).size() == 1 && clipsOf(s, r.pat) == 0, "song mode: the selected pattern is left alone (W1)");
  {
    // case 6: a note held over a bar line: the one clip grows, no second clip doubles it.
    Rig q;
    NoteRecorder rq;
    rq.beginSong(q.doc, q.pat, raw(), {});
    rq.noteOn(q.doc, q.ch, 60, 100, 300.0);
    rq.position(q.doc, 450.0);
    rq.noteOff(q.doc, q.ch, 60, 500.0);
    rq.noteOn(q.doc, q.ch, 62, 100, 520.0);
    rq.noteOff(q.doc, q.ch, 62, 530.0);
    rq.end(q.doc, 540.0);
    int patternClips = 0;
    for (const auto& x : q.doc.song().playlist) patternClips += x.kind == ClipKind::Pattern ? 1 : 0;
    const auto qn = notesOf(q.doc, rq.pattern(), q.ch);
    expect(patternClips == 1 && qn.size() == 2 && has(qn, 300, 60, 200) && has(qn, 520, 62), "song mode: a note over a bar line, one clip, no doubled notes (case 6)");
  }
  {
    // On the channel's instrument track when it has one (PL-08), started where recording started.
    Rig q;
    trackmodes::makeInstrumentTrack(q.doc, 3, q.ch);
    RecordSetup s2;
    s2.start = 2 * kBar + 50;
    NoteRecorder rq;
    rq.beginSong(q.doc, q.pat, raw(), s2);
    rq.noteOn(q.doc, q.ch, 60, 100, 3 * kBar + 10.0);
    rq.noteOff(q.doc, q.ch, 60, 3 * kBar + 20.0);
    rq.end(q.doc, 3 * kBar + 30.0);
    const Clip* qc = clipOf(q.doc.song(), rq.pattern());
    expect(qc && qc->track == 3 && qc->start == 2 * kBar && has(notesOf(q.doc, rq.pattern(), q.ch), kBar + 10, 60),
           "song mode: the take goes on the instrument track, at the bar recording started");
  }
}

// ---- 0.6: song-mode loops ----
void testSongLoop() {
  {  // case 7: a note held over the wrap keeps its real length
    Rig r;
    RecordSetup st;
    st.loopStart = 0;
    st.loopEnd = kBar;
    NoteRecorder rec;
    rec.beginSong(r.doc, r.pat, raw(), st);
    rec.noteOn(r.doc, r.ch, 60, 100, 360.0);
    rec.position(r.doc, 383.0);
    rec.position(r.doc, 3.0);
    rec.noteOff(r.doc, r.ch, 60, 20.0);
    rec.end(r.doc, 30.0);
    const auto ns = notesOf(r.doc, rec.pattern(), r.ch);
    expect(ns.size() == 1 && ns[0].start == 360 && ns[0].length == 44, "song loop: a note held over the wrap keeps its length, not 1 tick (case 7)");
  }
  {  // a new take per pass
    Rig r;
    RecordOptions o = raw();
    o.takes = true;
    RecordSetup st;
    st.loopStart = 0;
    st.loopEnd = kBar;
    NoteRecorder rec;
    rec.beginSong(r.doc, r.pat, o, st);
    rec.noteOn(r.doc, r.ch, 60, 100, 50.0);
    rec.noteOff(r.doc, r.ch, 60, 60.0);
    const ObjectId t1 = rec.pattern();
    rec.position(r.doc, 383.0);
    rec.position(r.doc, 2.0);
    rec.noteOn(r.doc, r.ch, 62, 100, 70.0);
    rec.noteOff(r.doc, r.ch, 62, 80.0);
    rec.end(r.doc, 100.0);
    const ObjectId t2 = rec.pattern();
    const Song& s = r.doc.song();
    const Clip* c1 = clipOf(s, t1);
    const Clip* c2 = clipOf(s, t2);
    expect(t1 != t2 && c1 && c2 && c1->start == 0 && c2->start == 0 && c2->track != c1->track && c1->muted && !c2->muted &&
               notesOf(r.doc, t1, r.ch).size() == 1 && has(notesOf(r.doc, t2, r.ch), 70, 62) && rec.takes() == 2,
           "song loop: Loop passes = new take records each pass into its own clip, earlier ones muted");
  }
}

// ---- 0.7: quantize never moves a note out of its loop or punch range ----
void testQuantizeEdge() {
  {  // case 8
    Rig r;
    RecordOptions o;
    o.grid = 96;
    RecordSetup st;
    st.loopStart = 0;
    st.loopEnd = kBar;
    NoteRecorder rec;
    rec.beginSong(r.doc, r.pat, o, st);
    rec.noteOn(r.doc, r.ch, 60, 100, 370.0);  // nearest grid line: 384, the loop end
    rec.noteOff(r.doc, r.ch, 60, 380.0);
    rec.end(r.doc, 382.0);
    const auto ns = notesOf(r.doc, rec.pattern(), r.ch);
    const Clip* c = clipOf(r.doc.song(), rec.pattern());
    expect(ns.size() == 1 && ns[0].start == 288 && c && r.doc.song().clipLength(*c) == kBar,
           "quantize: a note just before the loop end snaps to the last grid line inside, the clip keeps its bar (case 8)");
  }
  {
    Rig r;
    RecordOptions o;
    o.grid = 96;
    RecordSetup st;
    st.start = 0;
    st.punchIn = 0;
    st.punchOut = 2 * kBar;
    NoteRecorder rec;
    rec.beginSong(r.doc, r.pat, o, st);
    rec.noteOn(r.doc, r.ch, 60, 100, 2.0 * kBar - 10);
    rec.noteOff(r.doc, r.ch, 60, 2.0 * kBar - 2);
    rec.end(r.doc, 2.0 * kBar - 1);
    expect(has(notesOf(r.doc, rec.pattern(), r.ch), 2 * kBar - 96, 60), "quantize: nor past the punch out");
  }
}

// ---- 0.8: notes in the count-in's last beat ----
void testCountIn() {
  Rig r;
  RecordSetup st;
  st.start = 0;
  st.pickup = 96;
  NoteRecorder rec;
  rec.beginPattern(r.doc, r.pat, raw(), kBar, st);
  rec.position(r.doc, -300.0);
  rec.noteOn(r.doc, r.ch, 50, 100, -250.0);  // too early: not part of the take
  rec.noteOff(r.doc, r.ch, 50, -200.0);
  rec.noteOn(r.doc, r.ch, 60, 100, -30.0);  // a pickup
  rec.noteOn(r.doc, r.ch, 64, 100, -10.0);
  rec.position(r.doc, -5.0);
  rec.position(r.doc, 40.0);
  rec.noteOff(r.doc, r.ch, 60, 50.0);
  rec.noteOff(r.doc, r.ch, 64, 60.0);
  rec.end(r.doc, 100.0);
  const auto ns = notesOf(r.doc, r.pat, r.ch);
  expect(ns.size() == 2 && has(ns, 0, 60, 50) && has(ns, 0, 64, 60) && rec.pass() == 0,
         "count-in: notes in its last beat start the take (no false loop pass), earlier ones are not kept");
  // Song mode too, from where the transport waits.
  Rig q;
  RecordSetup s2;
  s2.start = 4 * kBar;
  s2.pickup = 96;
  NoteRecorder rq;
  rq.beginSong(q.doc, q.pat, raw(), s2);
  rq.noteOn(q.doc, q.ch, 60, 100, 4.0 * kBar - 20);
  rq.noteOff(q.doc, q.ch, 60, 4.0 * kBar + 30);
  rq.end(q.doc, 4.0 * kBar + 40);
  const Clip* c = clipOf(q.doc.song(), rq.pattern());
  expect(c && c->start == 4 * kBar && has(notesOf(q.doc, rq.pattern(), q.ch), 0, 60, 30), "count-in, song mode: the pickup lands on the take's first beat");
}

// ---- 0.9: the punch range ----
void testPunch() {
  Rig r;
  RecordSetup st;
  st.start = 0;
  st.punchIn = kBar;
  st.punchOut = 2 * kBar;
  NoteRecorder rec;
  rec.beginSong(r.doc, r.pat, raw(), st);
  rec.noteOn(r.doc, r.ch, 50, 100, 100.0);  // before the punch in
  rec.noteOff(r.doc, r.ch, 50, 120.0);
  rec.noteOn(r.doc, r.ch, 60, 100, kBar + 16.0);
  rec.noteOff(r.doc, r.ch, 60, kBar + 40.0);
  rec.noteOn(r.doc, r.ch, 62, 100, 2.0 * kBar - 50);  // held over the punch out
  rec.position(r.doc, 2.0 * kBar + 10);
  rec.noteOff(r.doc, r.ch, 62, 2.0 * kBar + 60);
  rec.noteOn(r.doc, r.ch, 64, 100, 2.0 * kBar + 80);  // after it
  rec.noteOff(r.doc, r.ch, 64, 2.0 * kBar + 90);
  rec.end(r.doc, 2.0 * kBar + 100);
  const auto ns = notesOf(r.doc, rec.pattern(), r.ch);
  const Clip* c = clipOf(r.doc.song(), rec.pattern());
  expect(c && c->start == kBar && ns.size() == 2 && has(ns, 16, 60, 24) && has(ns, kBar - 50, 62, 50),
         "punch: only notes inside the range record, one held over the punch out ends there");
}

// ---- K-263: the pedal still records with the notes ----
void testPedal() {
  Rig r;
  NoteRecorder rec;
  rec.beginPattern(r.doc, r.pat, raw(), kBar);
  rec.noteOn(r.doc, r.ch, 60, 100, 0);
  rec.pedal(r.ch, true, 10);
  rec.noteOff(r.doc, r.ch, 60, 24);
  rec.pedal(r.ch, false, 96);
  rec.end(r.doc, 120);
  const PatternAutomation* lane = findPatternLane(*r.doc.song().pattern(r.pat), r.ch, static_cast<uint32_t>(ChannelParam::Sustain));
  expect(lane && lane->events && lane->points.size() == 2, "pedal: recorded as a stepped Sustain lane, in the take's undo step");
  r.doc.undo();
  expect(notesOf(r.doc, r.pat, r.ch).empty() && !findPatternLane(*r.doc.song().pattern(r.pat), r.ch, static_cast<uint32_t>(ChannelParam::Sustain)),
         "pedal: one undo removes the notes and the pedal together");
}

// ---- 0.10: the options are kept ----
void testOptionsKept() {
  namespace fs = std::filesystem;
  const fs::path dir = fs::temp_directory_path() / "gridline_k226_settings";
  fs::create_directories(dir);
  const std::string file = (dir / "settings.json").string();
  {
    Settings s;
    RecordOptions o;
    int bars = 0;
    bool punch = false;
    loadRecordOptions(s, o, bars, punch);
    expect(o.overdub && o.quantize && !o.quantizeLength && !o.takes && o.notes && o.automation && bars == 0 && punch,
           "options: the defaults (merge, quantize, no takes, no count-in, punch on)");
    o.overdub = false;
    o.quantize = false;
    o.quantizeLength = true;
    o.takes = true;
    o.automation = false;
    o.autoTarget = AutoRecordTarget::AutomationClips;
    saveRecordOptions(s, o, 2, false);
    s.save(file);
  }
  Settings back;
  back.load(file);
  RecordOptions o;
  int bars = 0;
  bool punch = true;
  loadRecordOptions(back, o, bars, punch);
  expect(!o.overdub && !o.quantize && o.quantizeLength && o.takes && !o.automation && o.autoTarget == AutoRecordTarget::AutomationClips &&
             bars == 2 && !punch,
         "options: replace, quantize, takes, count-in and punch survive a save and load");
  fs::remove_all(dir);
}

// ---- K-112B (R14-04): Smooth's second pass over a region makes a take folder, not a stack ----
void testTakeFolder() {
  auto pass = [](Rig& r, bool folder, bool overdub, Tick from, Tick to) {
    RecordOptions o = raw();
    o.overdub = overdub;
    RecordSetup st;
    st.start = from;
    st.track = 2;
    st.takeFolder = folder;
    NoteRecorder rec;
    rec.beginSong(r.doc, r.pat, o, st);
    rec.noteOn(r.doc, r.ch, 60, 100, static_cast<double>(from + 10));
    rec.noteOff(r.doc, r.ch, 60, static_cast<double>(from + 40));
    rec.position(r.doc, static_cast<double>(to));
    rec.end(r.doc, static_cast<double>(to));
    return rec.takeClip();
  };
  {  // a region four bars long; a pass over its second bar
    Rig r;
    const ObjectId old = r.doc.addPattern("Pad Intro", 4, 16);
    r.doc.addNote(old, r.ch, Note(30, 24, 50, 90));
    const ObjectId oldClip = r.doc.addClip(old, 2, 0);
    const size_t clipsBefore = r.doc.song().playlist.size();
    const ObjectId take = pass(r, true, true, kBar, kBar + 300);
    const Song& s = r.doc.song();
    const Clip* tc = s.clip(take);
    expect(tc && tc->takeGroup != kNoId && tc->take == 1 && !tc->muted && tc->track == 2, "take folder: the new take is take 2 of a folder, playing");
    int pieces = 0, mutedUnder = 0, playingAround = 0;
    bool inFolder = true;
    for (const Clip& c : s.playlist) {
      if (c.kind != ClipKind::Pattern || c.pattern != old || !tc) continue;
      ++pieces;
      inFolder = inFolder && c.takeGroup == tc->takeGroup && c.take == 0 && c.track == 2;
      const bool under = c.start >= tc->start && c.start + s.clipLength(c) <= tc->start + s.clipLength(*tc);
      mutedUnder += under && c.muted ? 1 : 0;
      playingAround += !under && !c.muted ? 1 : 0;
    }
    expect(inFolder, "take folder: the region recorded over is its take 1, on the track");
    expect(pieces == 3 && mutedUnder == 1 && playingAround == 2, "take folder: the region is split at the take; under it muted, around it playing");
    expect(takes::lanesShown(s, 2) && takes::laneCount(s, 2) == 2, "take folder: the track shows two take lanes");
    r.doc.undo();
    const Clip* oc = r.doc.song().clip(oldClip);
    expect(oc && oc->takeGroup == kNoId && !oc->muted && r.doc.song().playlist.size() == clipsBefore,
           "take folder: one undo takes the pass and the folder away");
    // A third pass joins the same folder as take 3.
    const ObjectId t2 = pass(r, true, true, kBar, kBar + 300);
    const ObjectId t3 = pass(r, true, true, 2 * kBar, 2 * kBar + 300);
    const Clip* c2 = r.doc.song().clip(t2);
    const Clip* c3 = r.doc.song().clip(t3);
    expect(c2 && c3 && c2->takeGroup == c3->takeGroup && c3->take == 2, "take folder: a later pass is the folder's next take");
  }
  for (int k = 0; k < 2; ++k) {  // off (or replace): the take stacks on the region, as before
    Rig r;
    const ObjectId old = r.doc.addPattern("Pad Intro", 4, 16);
    r.doc.addClip(old, 2, 0);
    const ObjectId take = pass(r, k == 1, k == 0, kBar, kBar + 300);
    const Clip* tc = r.doc.song().clip(take);
    int grouped = 0;
    for (const Clip& c : r.doc.song().playlist) {
      grouped += c.takeGroup != kNoId ? 1 : 0;
    }
    expect(tc && grouped == 0, k == 0 ? "take folders off: the take stacks" : "replace: no take folder");
  }
  {  // a pass over empty space makes no folder
    Rig r;
    const ObjectId take = pass(r, true, true, kBar, kBar + 300);
    const Clip* tc = r.doc.song().clip(take);
    expect(tc && tc->takeGroup == kNoId, "take folder: a pass over nothing stays a plain region");
  }
}

}  // namespace

int main() {
  testTiming();
  testUndo();
  testPatternLoop();
  testReplace();
  testSongTake();
  testSongLoop();
  testQuantizeEdge();
  testCountIn();
  testPunch();
  testPedal();
  testOptionsKept();
  testTakeFolder();
  return finish();
}
