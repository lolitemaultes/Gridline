// Gridline - tools and engine parts that work in song time use the meter in force at the
// position, not the song's meter at the start: the live launch grid, new spots and pattern
// placement in the playlist, merged, chopped and stuttered patterns, recorded automation, the
// project preview's bar count. A meter stays notation: a song renders the same with or without
// its meter changes.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>
#include <string>
#include <vector>
#include "../Check.h"
#include "../Hermetic.h"
#include "core/Document.h"
#include "core/Meter.h"
#include "core/MeterEdit.h"
#include "core/MenuOps.h"
#include "core/NoteTools.h"
#include "core/PluginInfo.h"
#include "core/ProjectPreview.h"
#include "core/TrackPlus.h"
#include "engine/AudioEngine.h"
#include "engine/Chop.h"
#include "engine/Program.h"
#include "engine/Render.h"

using namespace gl;

namespace {

constexpr int kSr = 44100;

// 4/4 for bar 1 (0..384), 7/8 from tick 384 (bars of 336), 6/8 from 384 + 2 * 336 = 1056.
constexpr Tick kSeven = 384, kSix = 384 + 2 * 336;

void addMeters(Song& s) {
  Marker a;
  a.pos = kSeven, a.timeSigNum = 7, a.timeSigDen = 8;
  Marker b;
  b.pos = kSix, b.timeSigNum = 6, b.timeSigDen = 8;
  s.markers.push_back(a);
  s.markers.push_back(b);
}

ObjectId addChannel(Song& s, const std::string& type) {
  Channel c;
  c.id = s.allocateId();
  c.name = type;
  c.plugin = defaultPluginState(type);
  c.mixerTrack = s.masterId();
  s.channels.push_back(c);
  return c.id;
}

ObjectId addPattern(Song& s, ObjectId ch, Tick noteAt) {
  Pattern p;
  p.id = s.allocateId();
  p.name = "P";
  p.bars = 1;
  p.notes[ch] = {Note(noteAt, 48, 60, 100)};
  s.patterns.push_back(p);
  return p.id;
}

ObjectId addClip(Song& s, ObjectId pat, int track, Tick start, Tick len) {
  Clip c;
  c.id = s.allocateId();
  c.pattern = pat;
  c.track = track;
  c.start = start;
  c.length = len;
  if (static_cast<int>(s.tracks.size()) <= track) s.tracks.resize(static_cast<size_t>(track) + 1);
  s.playlist.push_back(c);
  return c.id;
}

// A meter is notation: the same song with and without meter changes renders bit for bit alike.
void testNotation() {
  Song s = makeEmptySong();
  s.tempo = 120;
  const ObjectId ch = addChannel(s, "gl.synth");
  const ObjectId p = addPattern(s, ch, 0);
  s.pattern(p)->notes[ch] = {Note(0, 96, 60, 100), Note(200, 96, 64, 100), Note(350, 96, 67, 100)};
  for (Tick t = 0; t < 2000; t += 384) addClip(s, p, 0, t, 384);
  Song m = s;
  addMeters(m);
  RenderOptions o;
  o.tail = 192;
  const AudioBuffer a = renderSong(s, o), b = renderSong(m, o);
  bool same = a.frames == b.frames;
  for (size_t i = 0; same && i < a.frames; ++i) same = a.at(0, i) == b.at(0, i) && a.at(1, i) == b.at(1, i);
  expect(same && a.frames > 0, "a song renders bit for bit the same with or without meter changes");
  ProgramBuilder pb;
  expect(!pb.build(m)->meterReaders, "built-in plugins only: blocks are not split at meter changes");
}

// Live mode: the launch grid counts beats of the meter in force.
long firstSound(AudioEngine& eng, long& clock, long until) {
  std::vector<float> L(64), R(64);
  long first = -1;
  while (clock < until) {
    eng.process(L.data(), R.data(), 64);
    for (int i = 0; i < 64 && first < 0; ++i)
      if (std::fabs(L[i]) > 1e-6f) first = clock + i;
    clock += 64;
  }
  return first;
}

void testLiveGrid() {
  const double spt = kSr * 60.0 / (120.0 * 96.0);  // samples per tick
  auto launchAt = [&](LaunchGrid g, Tick when, bool meters) {
    Song s = makeEmptySong();
    s.tempo = 120;
    const ObjectId ch = addChannel(s, "gl.synth");
    const ObjectId p = addPattern(s, ch, 0);
    const ObjectId c = addClip(s, p, 0, 0, 384);
    s.live.areaEnd = 768;
    s.tracks[0].launch = g;
    if (meters) addMeters(s);
    ProgramBuilder b;
    AudioEngine eng(kSr);
    eng.setProgram(b.build(s));
    eng.setMode(PlayMode::Live);
    eng.play();
    long clock = 0;
    firstSound(eng, clock, static_cast<long>(static_cast<double>(when) * spt));
    eng.launchClip(c);
    return firstSound(eng, clock, static_cast<long>(1600 * spt));
  };
  auto near = [&](long got, Tick tick) { return got >= 0 && std::labs(got - static_cast<long>(std::ceil(static_cast<double>(tick) * spt))) <= 2; };
  expect(near(launchAt(LaunchGrid::Beat, 400, false), 480), "live grid without changes: the next quarter-note beat (as before)");
  expect(near(launchAt(LaunchGrid::Beat, 400, true), 432), "live grid in 7/8: the next eighth-note beat of the section");
  expect(near(launchAt(LaunchGrid::FourBeats, 400, true), 384 + 192), "live grid in 7/8: four beats are four eighths from the section start");
  expect(near(launchAt(LaunchGrid::FourBeats, 1000, true), kSix), "live grid: a grid line never steps past the next meter change");
  expect(near(launchAt(LaunchGrid::Step, kSix + 10, true), kSix + 12), "live grid in 6/8: a step is a quarter of an eighth");
}

// Song-time tools: new spots, pattern placement, loop ranges, the preview, merged / stuttered patterns.
void testTools() {
  Song s = makeEmptySong();
  addMeters(s);
  const ObjectId ch = addChannel(s, "gl.synth");
  const ObjectId p = addPattern(s, ch, 0);
  addClip(s, p, 0, kSeven, 200);  // ends at 584, inside the first 7/8 bar (384..720)
  expect(trackplus::freeSpot(s, 0, kSeven, 100) == kSeven + 336, "a free spot after a clip starts on the next bar line of 7/8");
  expect(trackplus::placeTick(s, kSeven + 400, 0, 5000) == kSeven + 336, "a pattern placed at the song position starts on its 7/8 bar line");
  expect(trackplus::placeTick(s, 99999, kSix + 10, 20000) == kSix + 288, "a pattern placed at the view start goes to the next 6/8 bar line");
  {
    Song m = s;
    Marker plain;
    plain.pos = kSix + 288;  // the last marker: its loop runs one bar of the meter in force
    m.markers.push_back(plain);
    m.playlist.clear();
    expect(ops::markerLoopRange(m, 2).second == kSix + 2 * 288, "a marker loop past the song end is one 6/8 bar");
  }
  {
    Song m = s;
    m.playlist.clear();
    addClip(m, p, 0, 0, kSix + 100);  // to 1156: 1 bar of 4/4, 2 of 7/8, 1 started bar of 6/8
    expect(preview::make(m).bars == 4, "the project preview counts bars of the meter in force");
  }
  {
    Song m = s;
    addClip(m, p, 1, kSeven, 168);
    Document d(m);
    std::set<ObjectId> sel;
    for (const auto& c : d.song().playlist) sel.insert(c.id);
    const ObjectId nc = ops::mergeClips(d, sel);  // (the merged clip)
    const Clip* mc = nc != kNoId ? d.song().clip(nc) : nullptr;
    const Pattern* mp = mc ? d.song().pattern(mc->pattern) : nullptr;
    expect(mp && mp->meterNum == 7 && mp->meterDen == 8 && mp->bars == 1, "merged clips in a 7/8 section make a 7/8 pattern of whole 7/8 bars");
  }
  {  // Chopping a clip every bar / every beat cuts on the bar and beat lines of the meter in force.
    Song m = s;
    m.playlist.clear();
    const ObjectId c = addClip(m, p, 0, kSeven, 2 * 336);
    chop::sliceAtGrid(m, {c}, m.barTicks());
    std::vector<Tick> starts;
    for (const auto& x : m.playlist) starts.push_back(x.start);
    std::sort(starts.begin(), starts.end());
    expect(starts == std::vector<Tick>({kSeven, kSeven + 336}), "chop every bar in 7/8 cuts on the 7/8 bar line");
    Song n = s;
    n.playlist.clear();
    const ObjectId c2 = addClip(n, p, 0, kSix, 288);
    chop::sliceAtGrid(n, {c2}, n.beatTicks());
    expect(n.playlist.size() == 6, "chop every beat in 6/8 cuts at each eighth");
  }
  {
    Song m = s;
    m.pattern(p)->meterNum = 7, m.pattern(p)->meterDen = 8;
    std::set<ObjectId> sel = {m.playlist[0].id};
    chop::StutterParams sp;
    const std::vector<ObjectId> out = chop::stutter(m, sel, kSeven, kSeven + 168, sp);
    const Pattern* np = nullptr;
    for (ObjectId id : out)
      if (const Clip* c = m.clip(id))
        if (c->pattern != p) np = m.pattern(c->pattern);
    expect(np && np->meterNum == 7 && np->meterDen == 8, "a stuttered pattern keeps its source pattern's meter");
  }
}

}  // namespace

int main() {
  gl::test::hermeticTestEnv();
  testNotation();
  testLiveGrid();
  testTools();
  return finish();
}
