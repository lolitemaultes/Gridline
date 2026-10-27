// Gridline — J-138 song swing: the swing knob moves swung steps smoothly (to a
// fraction of a tick, at any PPQ), piano-roll notes swing with the steps through
// the same time warp (channel swing mix and truncated swing honoured), the
// engine plays those fractional positions sample-accurately, and the loop
// starter writes its parts straight (the engine swings them).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include "Check.h"
#include "core/LoopStarter.h"
#include "core/PluginInfo.h"
#include "core/Project.h"
#include "core/flp/Flp.h"
#include "core/flp/FlpExport.h"
#include "engine/Program.h"
#include "engine/Render.h"

using namespace gl;
namespace ls = gl::loopstarter;

namespace {

// 120 BPM song, one 1-bar pattern placed once, one synth channel.
Song swingSong(int ppq, ObjectId& ch) {
  Song s = makeEmptySong();
  s.tempo = 120;
  s.ppq = ppq;
  s.patterns[0].bars = 1;
  Clip c;
  c.id = s.allocateId();
  c.pattern = s.patterns[0].id;
  s.playlist.push_back(c);
  Channel k;
  k.id = s.allocateId();
  k.name = "synth";
  k.plugin = defaultPluginState("gl.synth");
  k.mixerTrack = s.masterId();
  s.channels.push_back(k);
  ch = k.id;
  return s;
}

bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

// First sample (left) louder than a whisper.
long firstSound(const AudioBuffer& b) {
  for (size_t i = 0; i < b.frames; ++i)
    if (std::fabs(b.at(0, i)) > 1e-5f) return static_cast<long>(i);
  return -1;
}

void testKnobSweepIsSmooth() {
  for (int ppq : {96, 24}) {
    ObjectId ch = kNoId;
    Song s = swingSong(ppq, ch);
    Pattern& p = s.patterns[0];
    p.steps[ch] = std::vector<uint8_t>(16, 0);
    p.steps[ch][1] = 100;
    const double st = static_cast<double>(s.ticksPerStep(p));
    bool exact = true, rising = true;
    double prev = -1;
    for (int k = 0; k <= 100; ++k) {
      s.swing = static_cast<float>(k) / 100.0f;
      auto ev = compilePatternChannel(s, p, ch);
      const double on = ev.empty() ? -1 : noteOnTick(ev[0]);
      exact = exact && near(on, st + static_cast<double>(s.swing) * st * 0.5, 1e-4);
      rising = rising && on > prev;
      prev = on;
    }
    const std::string at = " (PPQ " + std::to_string(ppq) + ")";
    expect(exact, ("swing: the off-beat step lands at 1 + swing / 2 steps for every 1% of the knob" + at).c_str());
    expect(rising, ("swing: every 1% of the knob moves the step (no rounding plateaus)" + at).c_str());
  }
}

void testRenderedOnsets() {
  // The engine plays the fractional positions: onset of a note on step 2 at PPQ 24
  // (a 16th = 6 ticks, so 1% swing is 0.03 ticks) for several knob settings.
  ObjectId ch = kNoId;
  Song s = swingSong(24, ch);
  s.patterns[0].notes[ch] = {Note(6, 3, 60, 100)};
  const double samplesPerTick = 44100.0 * 60.0 / (120.0 * 24.0);
  bool ok = true;
  long prev = -1;
  bool rising = true;
  for (int k : {0, 1, 2, 3, 10, 33, 50, 77, 100}) {
    s.swing = static_cast<float>(k) / 100.0f;
    RenderOptions o;
    const long on = firstSound(renderSong(s, o));
    const double want = (6.0 + 3.0 * k / 100.0) * samplesPerTick;
    ok = ok && on >= 0 && std::fabs(static_cast<double>(on) - want) <= 2.0;
    rising = rising && on > prev;
    prev = on;
  }
  expect(ok, "render: a swung note sounds within 2 samples of its exact swung time (PPQ 24)");
  expect(rising, "render: 1%, 2%, 3% ... of swing each move the note later");
  // Block size does not change where swung notes land.
  s.swing = 0.37f;
  s.patterns[0].steps[ch] = std::vector<uint8_t>(16, 90);
  RenderOptions a, b;
  a.blockSize = 512, b.blockSize = 37;
  expect(renderSong(s, a).data == renderSong(s, b).data, "render: swung steps and notes are block-size invariant");
}

void testNotesSwing() {
  ObjectId ch = kNoId;
  Song s = swingSong(96, ch);
  Pattern& p = s.patterns[0];
  const Tick st = s.ticksPerStep(p);  // 24
  s.swing = 0.5f;                     // off-beat 16ths move a quarter step later
  p.notes[ch] = {Note(0, st, 60, 100), Note(st, st, 62, 100), Note(st / 2, st / 2, 64, 100), Note(2 * st, 2 * st, 65, 100),
                 Note(4 * st, 16, 67, 100)};
  auto ev = compilePatternChannel(s, p, ch);
  auto find = [&](float pitch) -> const NoteEv* {
    for (const auto& e : ev)
      if (e.pitch == pitch) return &e;
    return nullptr;
  };
  const double mid = st * 1.25;  // where the off-beat step starts
  const NoteEv *n0 = find(60), *n1 = find(62), *n32 = find(64), *nb = find(65), *nt = find(67);
  expect(n0 && n1 && n32 && nb && nt, "notes: all compiled");
  if (!(n0 && n1 && n32 && nb && nt)) return;
  expect(noteOnTick(*n0) == 0 && near(noteOffTick(*n0), mid, 1e-4), "notes: the downbeat stays; its end follows the off-beat (legato kept)");
  expect(near(noteOnTick(*n1), mid, 1e-4) && near(noteOffTick(*n1), mid + st, 1e-4),
         "notes: an off-beat 16th moves like a step and keeps its length");
  expect(near(noteOnTick(*n32), 0.5 * mid, 1e-4), "notes: a 32nd inside the pair moves in proportion");
  expect(noteOnTick(*nb) == 2.0 * st && noteOffTick(*nb) == 4.0 * st, "notes: notes on pair boundaries do not move");
  expect(noteOnTick(*nt) == 4.0 * st && near(noteOffTick(*nt), 4.0 * st + 16.0 * mid / st, 1e-4),
         "notes: a triplet-length note's end is warped, its start stays");
  // Truncated swing: the off-beat note ends where its pair ends.
  s.channels[0].swingTruncate = true;
  ev = compilePatternChannel(s, p, ch);
  n1 = find(62);
  expect(n1 && near(noteOnTick(*n1), mid, 1e-4) && near(noteOffTick(*n1), 2.0 * st, 1e-4), "notes: truncated swing ends on the grid");
  s.channels[0].swingTruncate = false;
  // Channel swing mix scales the warp; 0 leaves the notes straight.
  s.channels[0].swingMix = 0.5f;
  ev = compilePatternChannel(s, p, ch);
  n1 = find(62);
  expect(n1 && near(noteOnTick(*n1), st * 1.125, 1e-4), "notes: channel swing mix scales the song swing");
  s.channels[0].swingMix = 0.0f;
  ev = compilePatternChannel(s, p, ch);
  n1 = find(62);
  expect(n1 && noteOnTick(*n1) == static_cast<double>(st) && n1->onFrac == 0, "notes: swing mix 0 keeps notes straight");
  s.channels[0].swingMix = 1.0f;
  // Steps and notes on the same position play together.
  p.steps[ch] = std::vector<uint8_t>(16, 0);
  p.steps[ch][3] = 100;
  p.notes[ch] = {Note(3 * st, st, 60, 100)};
  ev = compilePatternChannel(s, p, ch);
  expect(ev.size() == 2 && ev[0].on == ev[1].on && ev[0].onFrac == ev[1].onFrac, "notes: a note and a step on step 4 start together");
  // Odd steps per bar: pairs restart at the bar line; the bar's last step stays straight.
  Pattern odd = p;
  odd.bars = 2, odd.stepsPerBar = 5;
  odd.steps.clear();
  const Tick so = s.ticksPerStep(odd);
  odd.notes[ch] = {Note(so, so, 60, 100), Note(4 * so, so, 62, 100), Note(5 * so, so, 64, 100), Note(6 * so, so, 65, 100)};
  ev = compilePatternChannel(s, odd, ch);
  expect(ev.size() == 4 && near(noteOnTick(ev[0]), 1.25 * so, 1e-4) && noteOnTick(ev[1]) == 4.0 * so && noteOnTick(ev[2]) == 5.0 * so &&
             near(noteOnTick(ev[3]), 6.25 * so, 1e-4),
         "notes: with odd steps per bar the pairs restart at each bar line");
  // Steps only (imported projects): notes stay straight, steps still swing; saved with the project.
  {
    Song so2 = s;
    so2.swingNotes = false;
    so2.patterns[0].notes[ch] = {Note(st, st, 60, 100)};
    so2.patterns[0].steps[ch] = std::vector<uint8_t>(16, 0);
    so2.patterns[0].steps[ch][1] = 100;
    auto e2 = compilePatternChannel(so2, so2.patterns[0], ch);
    expect(e2.size() == 2 && noteOnTick(e2[0]) == static_cast<double>(st) && near(noteOnTick(e2[1]), mid, 1e-4),
           "steps only: the note stays straight, the step swings");
    Song back;
    expect(songFromJson(songToJson(so2), back) && !back.swingNotes, "steps only: saved and loaded");
    Song fresh;
    expect(songFromJson(songToJson(s), fresh) && fresh.swingNotes && songToJson(s).find("swingNotes") == nullptr,
           "projects that swing notes write no extra key");
  }
  // Editing positions stay straight: only the compiled events move.
  expect(s.patterns[0].notes[ch][0].start == 3 * st, "notes: the pattern keeps the written (straight) position");
  // The warp itself: fixed ends, monotonic.
  bool mono = true;
  double last = -1;
  for (int i = 0; i <= 96 * 4; ++i) {
    const double w = swingWarp(i * 0.5, 0.8, 24, 16);
    mono = mono && w > last;
    last = w;
  }
  expect(mono && swingWarp(48, 0.8, 24, 16) == 48 && swingWarp(384, 0.8, 24, 16) == 384, "swing warp: rising, pair ends fixed");
}

void testSongArrangementKeepsFractions() {
  // A pattern clip later in the song and a clip cut short keep swung positions.
  ObjectId ch = kNoId;
  Song s = swingSong(24, ch);
  s.swing = 0.33f;
  s.patterns[0].notes[ch] = {Note(6, 6, 60, 100)};
  Clip c = s.playlist[0];
  c.id = s.allocateId();
  c.start = s.barTicks() * 2;
  s.playlist.push_back(c);
  RenderOptions o;
  AudioBuffer b = renderSong(s, o);
  const double samplesPerTick = 44100.0 * 60.0 / (120.0 * 24.0);
  const double want = (static_cast<double>(s.barTicks() * 2) + 6.0 + 0.33 * 3.0) * samplesPerTick;
  long on = -1;
  for (size_t i = static_cast<size_t>(want) - 400; i < b.frames; ++i)
    if (std::fabs(b.at(0, i)) > 1e-5f && std::fabs(b.at(0, i - 1)) <= 1e-5f) {
      on = static_cast<long>(i);
      break;
    }
  expect(on >= 0 && std::fabs(static_cast<double>(on) - want) <= 2.0, "song: a clip later in the song plays its swung note on time");
}

void testLoopStarterWritesStraight() {
  bool any = false, straight = true;
  for (int style = 0; style < static_cast<int>(ls::styles().size()); ++style) {
    ls::Settings set;
    set.style = style;
    Song s = ls::generate(set);
    if (s.swing <= 0) continue;
    any = true;
    const Tick step = s.barTicks() / 16;
    for (const auto& name : {"Bass", "Chords", "Lead"}) {
      ObjectId id = kNoId;
      for (const auto& c : s.channels)
        if (c.name == name) id = c.id;
      for (const auto& p : s.patterns)
        if (auto it = p.notes.find(id); it != p.notes.end())
          for (const auto& n : it->second) straight = straight && n.start % step == 0;
    }
  }
  expect(any && straight, "loop starter: swung styles write their parts straight (the engine swings them)");
}

void testFlpExchange() {
  // .flp export writes straight notes plus the song swing; an imported .flp swings steps only.
  ObjectId ch = kNoId;
  Song s = swingSong(96, ch);
  s.swing = 0.5f;
  s.patterns[0].notes[ch] = {Note(24, 24, 60, 100)};
  flp::ExportOptions opt;
  opt.useOrigin = false;
  std::vector<uint8_t> out;
  flp::ExportReport xr;
  Song back;
  flp::Report rep;
  std::string err;
  const bool ok = flp::exportFlp(s, out, xr, opt) && importFlpBytes(out, back, rep, &err);
  bool straight = false;
  for (const auto& p : back.patterns)
    for (const auto& [id, notes] : p.notes)
      for (const auto& n : notes) straight = straight || (n.start == 24 && n.pitch == 60);
  expect(ok && straight && std::fabs(back.swing - 0.5f) < 0.01f && !back.swingNotes,
         "flp: notes are written straight with the swing setting; the import swings steps only");
}

}  // namespace

int main() {
  testKnobSweepIsSmooth();
  testRenderedOnsets();
  testNotesSwing();
  testSongArrangementKeepsFractions();
  testLoopStarterWritesStraight();
  testFlpExchange();
  return finish();
}
