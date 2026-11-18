// Gridline - grid-1118: the Clips layout's clip grid plays over the arrangement, as a session grid does.
// Rendered through the engine (no app): launch quantisation on whole bars of the meter in force, a
// launched clip that takes its track from the arrangement (only that track, only once it starts), a
// track stopped from the grid that stays silent, Back to arrangement (all tracks, one track), the
// Repeat launch mode, a legato launch (a captured scene) and launched clips that keep their phase when
// the song position jumps. Song mode without the grid on top (the other layouts) launches nothing.
#include <cmath>
#include <cstdio>
#include <set>
#include <string>
#include <vector>
#include "Check.h"
#include "Hermetic.h"
#include "core/Arrangements.h"
#include "core/PluginInfo.h"
#include "core/Song.h"
#include "engine/AudioEngine.h"
#include "engine/Program.h"

using namespace gl;

namespace {

constexpr int kSr = 44100;
constexpr int kBlock = 64;
constexpr Tick kBar = 384, kArea = 2 * kBar;  // the live area (the clip grid's storage): bars 1 and 2
const double kSpt = kSr * 60.0 / (120.0 * 96.0);  // samples per tick at 120 BPM, 96 PPQ

ObjectId addChannel(Song& s) {
  Channel c;
  c.id = s.allocateId();
  c.name = "synth";
  c.plugin = defaultPluginState("gl.synth");
  c.mixerTrack = s.masterId();
  s.channels.push_back(c);
  return c.id;
}

ObjectId addPattern(Song& s, ObjectId ch, const std::vector<Tick>& at) {
  Pattern p;
  p.id = s.allocateId();
  p.name = "P";
  p.bars = 1;
  for (Tick t : at) p.notes[ch].push_back(Note(t, 30, 60, 100));
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

struct Rig {
  Song s;
  ProgramBuilder b;
  AudioEngine eng{kSr};
  long clock = 0;
  void start(Tick from, bool over = true) {
    eng.setProgram(b.build(s));
    eng.setArrangement(kArea, kArea, false);  // nothing to wrap at: the transport plays on
    eng.setSessionOverArrangement(over);
    eng.setMode(PlayMode::Song);
    eng.setPosition(from);
    eng.play();
  }
  long sampleOf(double tick) const { return static_cast<long>(std::ceil(tick * kSpt)); }
  // Renders up to song tick `until` (counted from `from` passed to start()); the first audible sample.
  long run(Tick from, double until, long* firstSound = nullptr) {
    std::vector<float> L(kBlock), R(kBlock);
    long first = -1;
    const long end = sampleOf(until - static_cast<double>(from));
    while (clock < end) {
      eng.process(L.data(), R.data(), kBlock);
      for (int i = 0; i < kBlock && first < 0; ++i)
        if (std::fabs(L[i]) > 1e-6f) first = clock + i;
      clock += kBlock;
    }
    if (firstSound) *firstSound = first;
    return first;
  }
  // Peak level of the output while rendering on to `until`.
  float peak(Tick from, double until) {
    std::vector<float> L(kBlock), R(kBlock);
    float pk = 0;
    const long end = sampleOf(until - static_cast<double>(from));
    while (clock < end) {
      eng.process(L.data(), R.data(), kBlock);
      for (int i = 0; i < kBlock; ++i) pk = std::max(pk, std::fabs(L[i]));
      clock += kBlock;
    }
    return pk;
  }
};

// The tick (song time) of a sample counted from `from`.
double tickOf(long sample, Tick from) { return static_cast<double>(from) + static_cast<double>(sample) / kSpt; }

// Launch quantisation: the bar grids wait for whole bars of the meter in force (song position).
void testQuantisation() {
  auto firstAt = [](LaunchGrid g, Tick launchAt) {
    Rig r;
    r.s = makeEmptySong();
    r.s.tempo = 120;
    const ObjectId ch = addChannel(r.s);
    const ObjectId p = addPattern(r.s, ch, {0});
    const ObjectId c = addClip(r.s, p, 0, 0, kBar);
    r.s.live.areaEnd = kArea;
    r.s.tracks[0].launch = g;
    const Tick from = kArea;
    r.start(from);
    r.run(from, static_cast<double>(launchAt));
    r.eng.launchClip(c);
    long first = -1;
    r.run(from, static_cast<double>(launchAt + 10 * kBar), &first);
    return first < 0 ? -1.0 : tickOf(first, from);
  };
  auto near = [](double got, Tick want) { return got >= 0 && std::fabs(got - static_cast<double>(want)) < 1.5; };
  expect(near(firstAt(LaunchGrid::Bar, kArea + 40), kArea + kBar), "1 Bar: a clip launched in a bar starts on the next bar line");
  expect(near(firstAt(LaunchGrid::TwoBars, kArea + 40), 4 * kBar), "2 Bars: on the next line of two bars (counted from the song start)");
  expect(near(firstAt(LaunchGrid::FourBars, kArea + 40), 4 * kBar), "4 Bars: on the next four-bar line");
  expect(near(firstAt(LaunchGrid::EightBars, kArea + 40), 8 * kBar), "8 Bars: on the next eight-bar line");
  expect(near(firstAt(LaunchGrid::Beat, kArea + 40), kArea + 96), "1/4: on the next beat");
  expect(near(firstAt(LaunchGrid::Now, kArea + 40), kArea + 40), "None: at once");
  expect(launchGridTicks(makeEmptySong(), LaunchGrid::TwoBars, 0) == 2 * kBar, "the 2 Bars grid is two bars long");
}

// The arrangement of track 0 (a note every beat) and of track 1 (another channel, every beat too).
struct Two {
  Rig r;
  ObjectId empty0 = kNoId, notes1 = kNoId, ch0 = kNoId, ch1 = kNoId;
  explicit Two(LaunchGrid g = LaunchGrid::Bar) {
    r.s = makeEmptySong();
    r.s.tempo = 120;
    ch0 = addChannel(r.s);
    ch1 = addChannel(r.s);
    const ObjectId p0 = addPattern(r.s, ch0, {0, 96, 192, 288});
    const ObjectId p1 = addPattern(r.s, ch1, {48});
    for (int k = 0; k < 16; ++k) {
      addClip(r.s, p0, 0, kArea + k * kBar, kBar);
      addClip(r.s, p1, 1, kArea + k * kBar, kBar);
    }
    // In the grid: an empty clip on track 0 (its launch silences the arrangement's notes there).
    const ObjectId pe = addPattern(r.s, ch0, {});
    empty0 = addClip(r.s, pe, 0, 0, kBar);
    r.s.live.areaEnd = kArea;
    for (auto& t : r.s.tracks) t.launch = g;
  }
};

// Track 0's arrangement notes start on every beat, track 1's half a beat into each bar: a window at a bar
// line hears track 0 only (track 1's note of the bar before has long faded).
void testOverride() {
  Two t;
  const Tick from = kArea;
  t.r.start(from);
  const float before = t.r.peak(from, kArea + 30);
  expect(before > 1e-3f, "the arrangement plays on track 0 before any launch");
  const float quiet = before * 0.05f;  // what counts as silent (a release tail may linger)
  t.r.run(from, kArea + 100);
  t.r.eng.launchClip(t.empty0);  // 1 Bar: takes track 0 at kArea + kBar
  const float stillPlays = t.r.peak(from, kArea + kBar - 100);
  expect(stillPlays > 1e-3f, "until its launch grid line the track plays its arrangement");
  expect(!t.r.eng.arrangementOverridden(0) && t.r.eng.liveQueued(0) == t.empty0, "the launch waits (queued), the track is not taken yet");
  t.r.run(from, kArea + kBar + 40);  // past the line, (track 0's note there would have started)
  expect(t.r.eng.arrangementOverridden(0), "once the clip starts the grid has the track (Back to arrangement lights)");
  expect(!t.r.eng.arrangementOverridden(1), "the other track still plays its arrangement");
  // From the bar line on: track 0's notes (every beat) are gone; track 1's (half a beat in) still sound.
  t.r.run(from, kArea + 3 * kBar - 10);
  const float onBeat = t.r.peak(from, kArea + 3 * kBar + 40);  // a bar line: track 0 alone would sound here
  expect(onBeat < quiet, "track 0's arrangement notes no longer play (the grid's clip is empty)");
  const float halfBeat = t.r.peak(from, kArea + 3 * kBar + 92);  // track 1's note at +48
  expect(halfBeat > 1e-3f, "track 1's arrangement notes play on");
  // A stop from the grid keeps the track silent; Back to arrangement brings it back at once.
  t.r.eng.stopLiveTrack(0);
  t.r.run(from, kArea + 5 * kBar - 10);
  expect(t.r.eng.liveActive(0) == kNoId && t.r.eng.arrangementOverridden(0), "a stopped track stays off its arrangement");
  const float stopped = t.r.peak(from, kArea + 5 * kBar + 40);
  expect(stopped < quiet, "and stays silent there");
  t.r.eng.backToArrangement(0);
  t.r.run(from, kArea + 6 * kBar - 10);
  expect(!t.r.eng.arrangementOverridden(0) && !t.r.eng.anyArrangementOverridden(), "Back to arrangement for the track gives it back");
  const float back = t.r.peak(from, kArea + 6 * kBar + 40);
  expect(back > 1e-3f, "its arrangement notes play again");
  // A scene's empty slot (a stop on a track with no grid clip playing) takes the track too.
  t.r.eng.stopLiveTrack(1);
  t.r.run(from, kArea + 7 * kBar + 10);
  expect(t.r.eng.arrangementOverridden(1), "a stop on a track playing its arrangement takes it (on the launch grid)");
  t.r.eng.backToArrangement();
  t.r.run(from, kArea + 7 * kBar + 20);
  expect(!t.r.eng.anyArrangementOverridden(), "Back to arrangement gives every track back");
}

// The other layouts: song mode plays no launched clip and never silences the arrangement.
void testOffElsewhere() {
  Two t;
  const Tick from = kArea;
  t.r.start(from, false);
  t.r.run(from, kArea + 100);
  t.r.eng.launchClip(t.empty0);
  t.r.run(from, kArea + 2 * kBar + 50);
  expect(t.r.eng.liveActive(0) == kNoId && !t.r.eng.arrangementOverridden(0), "without the grid on top, song mode launches nothing");
}

// Repeat: held, the clip starts again on every launch grid line; let go, it plays on.
void testRepeat() {
  Rig r;
  r.s = makeEmptySong();
  r.s.tempo = 120;
  const ObjectId ch = addChannel(r.s);
  const ObjectId p = addPattern(r.s, ch, {0});
  const ObjectId c = addClip(r.s, p, 0, 0, kBar);
  r.s.live.areaEnd = kArea;
  r.s.tracks[0].launch = LaunchGrid::Beat;
  r.s.tracks[0].pad = PadMode::Repeat;
  const Tick from = kArea;
  r.start(from);
  r.run(from, kArea + 20);
  r.eng.launchClip(c);
  std::set<Tick> origins;
  for (Tick t = kArea + 30; t < kArea + 330; t += 8) {
    r.run(from, static_cast<double>(t));
    if (r.eng.liveActive(0) == c) origins.insert(r.eng.liveOrigin(0));
  }
  expect(origins.size() >= 3 && origins.count(kArea + 96) && origins.count(kArea + 192) && origins.count(kArea + 288),
         "Repeat held: the clip starts again on each beat");
  r.eng.releaseClip(c);
  std::set<Tick> after;
  for (Tick t = kArea + 330; t < kArea + 380 + 2 * kBar; t += 8) {
    r.run(from, static_cast<double>(t));
    if (r.eng.liveActive(0) == c) after.insert(r.eng.liveOrigin(0));
  }
  expect(r.eng.liveActive(0) == c, "let go, the clip plays on");
  bool loopsOnly = true;  // (it loops at its own length from its last start)
  for (Tick o : after) loopsOnly = loopsOnly && (o - (kArea + 288)) % kBar == 0;
  expect(loopsOnly, "and no longer restarts on every beat");
}

// Legato (a captured scene): the new clip takes over at once, where the playing one is.
void testLegato() {
  Rig r;
  r.s = makeEmptySong();
  r.s.tempo = 120;
  const ObjectId ch = addChannel(r.s);
  const ObjectId p = addPattern(r.s, ch, {0, 192});
  const ObjectId a = addClip(r.s, p, 0, 0, kBar);
  const ObjectId b = addClip(r.s, p, 0, kBar, kBar);
  r.s.live.areaEnd = kArea;
  r.s.tracks[0].launch = LaunchGrid::Bar;
  const Tick from = kArea;
  r.start(from);
  r.eng.launchClip(a);
  r.run(from, kArea + kBar + 100);
  const Tick o = r.eng.liveOrigin(0);
  expect(r.eng.liveActive(0) == a, "the first clip plays");
  r.eng.launchClip(b, true);
  r.run(from, kArea + kBar + 110);
  expect(r.eng.liveActive(0) == b && r.eng.liveOrigin(0) == o, "a legato launch takes over at once, in the same phase");
}

// A jump of the song position (a seek, a loop wrap) moves the launched clips' times along.
void testJump() {
  Rig r;
  r.s = makeEmptySong();
  r.s.tempo = 120;
  const ObjectId ch = addChannel(r.s);
  const ObjectId p = addPattern(r.s, ch, {0});
  const ObjectId c = addClip(r.s, p, 0, 0, kBar);
  r.s.live.areaEnd = kArea;
  r.s.tracks[0].launch = LaunchGrid::Now;
  const Tick from = kArea;
  r.start(from);
  r.run(from, kArea + 40);
  r.eng.launchClip(c);
  r.run(from, kArea + 100);
  const Tick o = r.eng.liveOrigin(0);
  r.eng.setPosition(kArea + 4 * kBar + 100);
  std::vector<float> L(kBlock), R(kBlock);
  for (int i = 0; i < 8; ++i) r.eng.process(L.data(), R.data(), kBlock);
  const Tick moved = r.eng.liveOrigin(0) - o;
  expect(r.eng.liveActive(0) == c && std::llabs(static_cast<long long>(moved - 4 * kBar)) <= 1,
         "after a jump of 4 bars the launched clip's start moved 4 bars: it plays on in its phase");
}

}  // namespace

int main() {
  gl::test::hermeticTestEnv();
  testQuantisation();
  testOverride();
  testOffElsewhere();
  testRepeat();
  testLegato();
  testJump();
  return finish();
}
