// Gridline - K-255: lanes play exactly.
//  * Steps of a row on 5 or 7 a bar start at s x bar / grid (a fraction of a tick) and sound on
//    that sample at 44.1 and 48 kHz.
//  * A run-on 7-step lane over a 4-bar looping clip plays its steps 0..6 on and on; restart
//    lanes play as 0.7.7's step loops (the lane code gives the same events as the 0.7.7 code).
//  * Clips' launched slots run a lane on from the launch.
//  * Burn: lanes written out as steps and notes play the same; MIDI and FL exports burn first, so
//    the exported notes are the notes that play.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include "Check.h"
#include "Hermetic.h"
#include "core/ClipLoop.h"
#include "core/Document.h"
#include "core/Grouping.h"
#include "core/Lanes.h"
#include "core/MidiFile.h"
#include "core/PluginInfo.h"
#include "core/Project.h"
#include "core/flp/FlpExport.h"
#include "engine/AudioEngine.h"
#include "engine/Program.h"
#include "engine/Render.h"

using namespace gl;

namespace {

bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

// 120 BPM, one 1-bar pattern, one synth channel; `clip`: placed once at 0.
Song oneChannel(ObjectId& ch, bool clip = true) {
  Song s = makeEmptySong();
  s.tempo = 120;
  s.swing = 0;
  s.patterns[0].bars = 1;
  Channel k;
  k.id = s.allocateId();
  k.name = "synth";
  k.plugin = defaultPluginState("gl.synth");
  k.mixerTrack = s.masterId();
  s.channels.push_back(k);
  ch = k.id;
  if (clip) {
    Clip c;
    c.id = s.allocateId();
    c.pattern = s.patterns[0].id;
    s.playlist.push_back(c);
  }
  return s;
}

long firstSound(const AudioBuffer& b) {
  for (size_t i = 0; i < b.frames; ++i)
    if (std::fabs(b.at(0, i)) > 1e-5f) return static_cast<long>(i);
  return -1;
}

void testCompiledTimes() {
  ObjectId ch = kNoId;
  Song s = oneChannel(ch);
  Pattern& p = s.patterns[0];
  for (int g : {5, 7, 3, 20, 9, 11}) {
    p.lanes[ch].grid = g;
    p.steps[ch] = std::vector<uint8_t>(static_cast<size_t>(g), 100);
    const auto ev = compilePatternChannel(s, p, ch);
    bool ok = static_cast<int>(ev.size()) == g;
    for (int k = 0; ok && k < g; ++k)
      ok = near(noteOnTick(ev[static_cast<size_t>(k)]), 384.0 * k / g, 1e-4) && near(noteOffTick(ev[static_cast<size_t>(k)]), 384.0 * (k + 1) / g, 1e-4);
    char name[96];
    std::snprintf(name, sizeof name, "%d a bar: every step starts and ends at k x bar / %d exactly", g, g);
    expect(ok, name);
  }
  // A two-bar pattern: bar 2's steps exact too (no drift).
  p.bars = 2;
  p.lanes[ch].grid = 7;
  p.steps[ch] = std::vector<uint8_t>(14, 100);
  const auto ev = compilePatternChannel(s, p, ch);
  expect(ev.size() == 14 && near(noteOnTick(ev[13]), 384.0 + 384.0 * 6 / 7, 1e-4), "7 a bar over two bars: the last step exact");
  // Step properties on an odd grid: shift a share of the row's own step, pitch.
  StepProps sp;
  sp.shift = 50;
  sp.pitch = 72;
  p.stepProps[ch] = std::vector<StepProps>(14);
  p.stepProps[ch][1] = sp;
  const auto ev2 = compilePatternChannel(s, p, ch);
  expect(ev2.size() == 14 && near(noteOnTick(ev2[1]), 384.0 / 7 * 1.5, 1e-4) && near(ev2[1].pitch, 72.0, 1e-6),
         "a step's shift is a share of its own step; its pitch plays");
  // A loop of 5 steps on 7 a bar: steps 0..4 again from step 5.
  p.stepProps.erase(ch);
  p.steps[ch] = std::vector<uint8_t>(14, 0);
  p.steps[ch][0] = 100;
  p.steps[ch][3] = 90;
  p.stepLoop[ch] = 5;
  const auto ev3 = compilePatternChannel(s, p, ch);
  expect(ev3.size() == 6 && near(noteOnTick(ev3[2]), 384.0 * 5 / 7, 1e-4) && near(noteOnTick(ev3[3]), 384.0 * 8 / 7, 1e-4),
         "a length of 5 on 7 a bar repeats every 5 of its steps");
  // Notes in a 5-step loop on 7 a bar repeat a fraction of a tick apart.
  p.steps.erase(ch);
  p.notes[ch] = {Note(10, 20, 64, 100)};
  const auto ev4 = compilePatternChannel(s, p, ch);
  expect(ev4.size() == 3 && near(noteOnTick(ev4[1]), 10.0 + 384.0 * 5 / 7, 1e-3) && near(noteOnTick(ev4[2]), 10.0 + 384.0 * 10 / 7, 1e-3),
         "notes inside the lane's length repeat at its exact length");
}

void testRenderedTimes() {
  bool all = true;
  for (int sr : {44100, 48000})
    for (int g : {5, 7}) {
      ObjectId ch = kNoId;
      Song s = oneChannel(ch);
      Pattern& p = s.patterns[0];
      p.lanes[ch].grid = g;
      RenderOptions o;
      o.sampleRate = sr;
      p.steps[ch] = std::vector<uint8_t>(static_cast<size_t>(g), 0);
      p.steps[ch][0] = 100;
      const long base = firstSound(renderSong(s, o));  // the instrument's own start (step 0 on sample 0)
      const double spt = sr * 60.0 / (120.0 * 96.0);
      for (int k = 1; k < g; ++k) {
        p.steps[ch] = std::vector<uint8_t>(static_cast<size_t>(g), 0);
        p.steps[ch][static_cast<size_t>(k)] = 100;
        const long at = firstSound(renderSong(s, o));
        const double want = 384.0 * k / g * spt + static_cast<double>(base);
        const bool ok = at >= 0 && std::fabs(static_cast<double>(at) - want) <= 1.0;
        if (!ok) std::printf("# %d Hz, %d a bar, step %d: sound at %ld, want %.2f\n", sr, g, k, at, want);
        all = all && ok;
      }
    }
  expect(all, "render: every step of 5 and 7 a bar sounds on its exact sample (within one) at 44.1 and 48 kHz");
}

// The compiled song events of channel 0.
std::vector<NoteEv> songEvents(const Song& s) {
  ProgramBuilder pb;
  auto prog = pb.build(s);
  return prog && !prog->channels.empty() ? prog->channels[0].song : std::vector<NoteEv>{};
}

void testRunOn() {
  ObjectId ch = kNoId;
  Song s = oneChannel(ch);
  Pattern& p = s.patterns[0];
  // Steps 0..6 with velocities 1..7 (the velocity says which step played), a length of 7.
  p.steps[ch] = std::vector<uint8_t>(16, 0);
  for (int k = 0; k < 7; ++k) p.steps[ch][static_cast<size_t>(k)] = static_cast<uint8_t>(10 * (k + 1));
  p.stepLoop[ch] = 7;
  Clip& c = s.playlist[0];
  c.loop = true;
  c.length = 4 * 384;
  // Restarting (0.7.7): each pass plays 0..6, 0..6, 0, 1 again.
  {
    const auto ev = songEvents(s);
    bool ok = ev.size() == 64;
    for (size_t j = 0; ok && j < ev.size(); ++j)
      ok = ev[j].on == static_cast<Tick>(j) * 24 && std::lround(ev[j].velocity * 127.0f) == 10 * static_cast<long>((j % 16) % 7 + 1);
    expect(ok, "restart: the lane starts again with every pass of the pattern");
  }
  p.lanes[ch].mode = LaneMode::RunOn;
  {
    const auto ev = songEvents(s);
    bool ok = ev.size() == 64;
    for (size_t j = 0; ok && j < ev.size(); ++j)
      ok = ev[j].on == static_cast<Tick>(j) * 24 && std::lround(ev[j].velocity * 127.0f) == 10 * static_cast<long>(j % 7 + 1);
    expect(ok, "run on: a 7-step lane over a 4-bar clip plays its steps 0..6 on and on");
  }
  expect(lanes::lineUpPasses(s, p) == 7 && lanes::lineUpBars(s, p) == 7, "a 7-step lane on 16 lines up again after 7 bars");
  // A clip starting mid-pattern: the phase still counts from the clip's content (pass 0 at its offset).
  {
    Song t = s;
    t.playlist[0].offset = 8 * 24;  // starts on step 9
    const auto ev = songEvents(t);
    // pass 0 plays steps 8..15 (sources 1..6, 0, 1), pass 1 starts the lane 16 steps on: source 2.
    bool ok = !ev.empty() && std::lround(ev[0].velocity * 127.0f) == 20;
    const auto it = std::find_if(ev.begin(), ev.end(), [](const NoteEv& e) { return e.on == 8 * 24; });
    ok = ok && it != ev.end() && std::lround(it->velocity * 127.0f) == 30;
    expect(ok, "a clip that starts mid-pattern: the lane runs on from the pattern's own start");
  }
  // A clip with a loop of its own (cut short) restarts the lane with its loop.
  {
    Song t = s;
    t.playlist[0].loopLength = 8 * 24;
    t.playlist[0].loopStart = 0;
    const auto a = songEvents(t);
    p.lanes[ch].mode = LaneMode::Restart;
    t.patterns[0] = p;
    const auto b = songEvents(t);
    p.lanes[ch].mode = LaneMode::RunOn;
    bool same = a.size() == b.size();
    for (size_t j = 0; same && j < a.size(); ++j) same = a[j].on == b[j].on && a[j].velocity == b[j].velocity;
    expect(same, "a clip's own loop window restarts the lane (only the whole pattern runs on)");
  }
  // Pattern mode plays the pattern itself: the lane starts again with each loop of it.
  {
    const auto ev = compilePatternChannel(s, p, ch);
    expect(ev.size() == 16 && std::lround(ev[7].velocity * 127.0f) == 10, "pattern mode: one pass of the pattern");
  }
  // An odd grid that runs on: 5 a bar, length 3, over 3 bars.
  {
    Song t = s;
    Pattern& q = t.patterns[0];
    q.lanes[ch].grid = 5;
    q.steps[ch] = {10, 20, 30, 0, 0};
    q.stepLoop[ch] = 3;
    t.playlist[0].length = 3 * 384;
    const auto ev = songEvents(t);
    bool ok = ev.size() == 15;
    for (size_t j = 0; ok && j < ev.size(); ++j)
      ok = near(noteOnTick(ev[j]), 384.0 * static_cast<double>(j) / 5.0, 1e-3) && std::lround(ev[j].velocity * 127.0f) == 10 * static_cast<long>(j % 3 + 1);
    expect(ok, "5 a bar with a length of 3 runs on exactly over 3 bars");
  }
}

// The lane code gives exactly the 0.7.7 events for a lane that plays as 0.7.7 does.
void testSameAsBefore() {
  ObjectId ch = kNoId;
  Song s = oneChannel(ch);
  s.swing = 0.6f;
  Pattern& p = s.patterns[0];
  p.steps[ch] = {100, 0, 90, 80, 0, 70, 0, 60, 100, 0, 90, 80, 0, 70, 0, 60};
  StepProps sp;
  sp.shift = 30;
  sp.pan = 20;
  sp.pitch = 67;
  p.stepProps[ch] = std::vector<StepProps>(16);
  p.stepProps[ch][3] = sp;
  p.notes[ch] = {Note(5, 30, 64, 100), Note(100, 10, 70, 90)};
  p.stepLoop[ch] = 6;
  const auto plain = compilePatternChannel(s, p, ch);
  // A run-on lane in the pattern's first pass, where the pass is the row's length: the lane path.
  p.lanes[ch].mode = LaneMode::RunOn;
  expect(!lanes::plain(p, ch), "the run-on lane takes the lane path");
  const auto lane = compilePatternChannel(s, p, ch);
  bool same = plain.size() == lane.size();
  for (size_t j = 0; same && j < plain.size(); ++j)
    same = plain[j].on == lane[j].on && plain[j].onFrac == lane[j].onFrac && plain[j].off == lane[j].off && plain[j].offFrac == lane[j].offFrac &&
           plain[j].pitch == lane[j].pitch && plain[j].velocity == lane[j].velocity && plain[j].props.pan == lane[j].props.pan;
  expect(same, "the lane path plays a first pass exactly as the 0.7.7 code (swing, shift, properties, notes)");
  // And a song: restart is the 0.7.7 code itself, unchanged.
  p.lanes.erase(ch);
  expect(lanes::plain(p, ch) && compilePatternChannel(s, p, ch).size() == plain.size(), "restart lanes stay on the 0.7.7 code");
}

void testLive() {
  ObjectId ch = kNoId;
  Song s = oneChannel(ch);
  Pattern& p = s.patterns[0];
  p.steps[ch] = std::vector<uint8_t>(16, 0);
  for (int k = 0; k < 7; ++k) p.steps[ch][static_cast<size_t>(k)] = static_cast<uint8_t>(10 * (k + 1));
  p.stepLoop[ch] = 7;
  p.lanes[ch].mode = LaneMode::RunOn;
  s.live.areaEnd = 8 * 384;
  Clip& c = s.playlist[0];
  c.loop = true;
  c.length = 2 * 384;
  ProgramBuilder pb;
  auto prog = pb.build(s);
  bool ok = prog && prog->liveClips.size() == 1 && prog->liveClips[0].notes.size() == 32;
  if (ok) {
    const auto& n = prog->liveClips[0].notes;
    for (size_t j = 0; ok && j < n.size(); ++j) ok = std::lround(n[j].e.velocity * 127.0f) == 10 * static_cast<long>(j % 7 + 1);
  }
  expect(ok, "Clips: a launched slot runs the lane on from its launch (2 bars: steps 0..6 on and on)");
}

void testBurn() {
  ObjectId ch = kNoId;
  Song s = oneChannel(ch);
  // A second channel on 5 a bar and a third restarting on the pattern grid.
  Channel b = s.channels[0];
  b.id = s.allocateId();
  b.name = "five";
  s.channels.push_back(b);
  Channel r = s.channels[0];
  r.id = s.allocateId();
  r.name = "restart";
  s.channels.push_back(r);
  Pattern& p = s.patterns[0];
  p.steps[ch] = std::vector<uint8_t>(16, 0);
  for (int k = 0; k < 7; ++k) p.steps[ch][static_cast<size_t>(k)] = static_cast<uint8_t>(10 * (k + 1));
  p.stepLoop[ch] = 7;
  p.lanes[ch].mode = LaneMode::RunOn;
  p.lanes[b.id].grid = 5;
  p.steps[b.id] = {100, 0, 90, 0, 80};
  p.steps[r.id] = {100, 0, 0, 90, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  p.stepLoop[r.id] = 4;
  p.notes[r.id] = {Note(6, 12, 70, 100)};
  Clip& c = s.playlist[0];
  c.loop = true;
  c.length = 4 * 384;
  // The playback of every channel, as on/off ticks (exact) and velocity.
  auto play = [](const Song& x) {
    ProgramBuilder pb;
    auto prog = pb.build(x);
    std::vector<std::vector<std::pair<double, long>>> out;
    for (const auto& pc : prog->channels) {
      std::vector<std::pair<double, long>> v;
      for (const NoteEv& e : pc.song) v.push_back({noteOnTick(e), std::lround(e.velocity * 127.0f)});
      std::sort(v.begin(), v.end());
      out.push_back(v);
    }
    return out;
  };
  const auto before = play(s);
  Document d(s);
  bool cut = true;
  const int n = lanes::burn(d, d.song().patterns[0].id, {}, &cut);
  expect(n == 3 && !cut, "burn: three lanes written out (they line up after 7 bars)");
  const Pattern& q = d.song().patterns[0];
  expect(q.lanes.empty() && q.stepLoop.empty() && q.bars == 7, "no lanes or lengths left; the pattern runs to where they line up");
  expect(d.song().playlist[0].length == 4 * 384, "the clip keeps its length");
  const auto after = play(d.song());
  bool same = before.size() == after.size();
  for (size_t i = 0; same && i < before.size(); ++i) {
    same = before[i].size() == after[i].size();
    for (size_t j = 0; same && j < before[i].size(); ++j)
      same = near(before[i][j].first, after[i][j].first, 2e-3) && before[i][j].second == after[i][j].second;
  }
  expect(same, "the burnt pattern plays exactly what the lanes played");
  d.undo();
  expect(lanes::runsOn(d.song().patterns[0], ch) && d.song().patterns[0].bars == 1, "undo: the lanes are back");

  // MIDI exports burn first: the exported notes are what plays (to the tick).
  const MidiFileData m = songToMidi(s);
  bool midi = true;
  size_t total = 0;
  for (const auto& v : before) total += v.size();
  size_t got = 0;
  for (const auto& t : m.tracks) got += t.notes.size();
  midi = got == total;
  for (size_t i = 0; midi && i < s.channels.size(); ++i) {
    const auto it = std::find_if(m.tracks.begin(), m.tracks.end(), [&](const MidiTrack& t) { return t.name == s.channels[i].name; });
    if (it == m.tracks.end()) {
      midi = before[i].empty();
      continue;
    }
    std::vector<std::pair<long, long>> mv, pv;
    for (const auto& nn : it->notes) mv.push_back({static_cast<long>(nn.start), nn.velocity});
    for (const auto& e : before[i]) pv.push_back({static_cast<long>(std::floor(e.first + 1e-6)), e.second});
    std::sort(mv.begin(), mv.end());
    std::sort(pv.begin(), pv.end());
    midi = mv == pv;
  }
  expect(midi, "MIDI export: the burnt lanes' notes equal playback (5 a bar on the tick below)");
  const MidiFileData pm = patternToMidi(s, s.patterns[0].id);
  size_t pn = 0;
  for (const auto& t : pm.tracks) pn += t.notes.size();
  // One pass: the run-on lane's 16 steps (7, 7, then 2), the 3 steps of 5 a bar, and the restart
  // lane's loop of 4 four times (its 2 steps and its note each time).
  expect(pn == 16 + 3 + 4 * (2 + 1), ("pattern MIDI export: one pass of the pattern, lanes written out (" + std::to_string(pn) + ")").c_str());

  // FL export burns first and says so.
  flp::ExportReport rep;
  flp::Bytes bytes;
  flp::ExportOptions eo;
  eo.renderInstruments = false;
  const bool ok = flp::exportFlp(s, bytes, rep, eo);
  bool said = false;
  for (const auto& w : rep.warnings) said = said || w.find("written out") != std::string::npos;
  expect(ok && said, "FL export writes the lanes out and says so");
  expect(!lanes::needsBurn(makeEmptySong()), "a song without lanes is not burnt");
}

}  // namespace

int main() {
  gl::test::hermeticTestEnv("k255_timing");
  testCompiledTimes();
  testRenderedTimes();
  testRunOn();
  testSameAsBefore();
  testLive();
  testBurn();
  return finish();
}
