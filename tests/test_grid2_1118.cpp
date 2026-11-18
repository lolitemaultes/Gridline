// Gridline - grid2-1118: the Clips grid's launch settings, rendered through the engine (no app) and through
// the project file: a clip's own launch quantisation (a triplet grid) and launch mode over its track's, clip
// follow actions (Next, Stop, the chances, Jump, linked to the clip's length), a scene launched as one (its
// empty slots stop their tracks only where the stop button stays), a scene's tempo and time signature at its
// launch point, scene follow actions, the transport moving with the Arrangement when a scene is inserted
// while it plays, and the same follow actions picked on every run. Then the format: version 10 saves and
// loads the new data, a version 9 project with several clips in a slot loads with one clip a slot (nothing
// lost), the scene settings in names, and scene edits that carry the removed stop buttons along.
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include "Check.h"
#include "Hermetic.h"
#include "core/Arrangements.h"
#include "core/Json.h"
#include "core/PluginInfo.h"
#include "core/Project.h"
#include "core/Song.h"
#include "core/TrackModes.h"
#include "engine/AudioEngine.h"
#include "engine/Program.h"

using namespace gl;

namespace {

constexpr int kSr = 44100;
constexpr int kBlock = 64;
constexpr Tick kBar = 384, kScene = 4 * kBar;  // 96 PPQ, 4/4: a scene is four bars
constexpr Tick kArea = 3 * kScene;              // three scenes; the Arrangement starts after them

ObjectId addChannel(Song& s) {
  Channel c;
  c.id = s.allocateId();
  c.name = "synth";
  c.plugin = defaultPluginState("gl.synth");
  c.mixerTrack = s.masterId();
  s.channels.push_back(c);
  return c.id;
}

ObjectId addPattern(Song& s, ObjectId ch) {
  Pattern p;
  p.id = s.allocateId();
  p.name = "P";
  p.bars = 1;
  p.notes[ch].push_back(Note(0, 30, 60, 100));
  s.patterns.push_back(p);
  return p.id;
}

ObjectId addClip(Song& s, ObjectId pat, int track, Tick start, Tick len = kBar) {
  Clip c;
  c.id = s.allocateId();
  c.pattern = pat;
  c.track = track;
  c.start = start;
  c.length = len;
  if (static_cast<int>(s.tracks.size()) <= track) {
    s.tracks.resize(static_cast<size_t>(track) + 1);
  }
  s.playlist.push_back(c);
  return c.id;
}

struct Rig {
  Song s;
  ProgramBuilder b;
  AudioEngine eng{kSr};
  long clock = 0;
  Tick from = kArea;
  ObjectId ch = kNoId, pat = kNoId;
  Rig() {
    s = makeEmptySong();
    s.tempo = 120;
    ch = addChannel(s);
    pat = addPattern(s, ch);
    s.live.areaEnd = kArea;
  }
  void start() {
    eng.setProgram(b.build(s));
    eng.setArrangement(kArea, kArea, false);
    eng.setSessionOverArrangement(true);
    eng.setMode(PlayMode::Song);
    eng.setPosition(from);
    eng.play();
    std::vector<float> L(kBlock), R(kBlock);
    eng.process(L.data(), R.data(), kBlock);  // (the commands are in)
    clock += kBlock;
  }
  // Renders on until the song position reaches `tick` (whatever the tempo).
  void runTo(double tick) {
    std::vector<float> L(kBlock), R(kBlock);
    for (int guard = 0; eng.position() < tick && guard < 2000000; ++guard) {
      eng.process(L.data(), R.data(), kBlock);
      clock += kBlock;
    }
  }
  void runSamples(long n) {
    std::vector<float> L(kBlock), R(kBlock);
    for (long done = 0; done < n; done += kBlock) {
      eng.process(L.data(), R.data(), kBlock);
      clock += kBlock;
    }
  }
};

// A clip's own launch quantisation (1/8 triplets) over its track's 1 Bar; "Global" (-1) keeps the track's.
void testClipQuantisation() {
  auto startOf = [](int quant) {
    Rig r;
    const ObjectId c = addClip(r.s, r.pat, 0, 0);
    r.s.tracks[0].launch = LaunchGrid::Bar;
    r.s.clip(c)->launchSet.quant = static_cast<int8_t>(quant);
    r.start();
    r.runTo(kArea + 40);
    r.eng.launchClip(c);
    double at = -1;
    std::vector<float> L(kBlock), R(kBlock);
    for (int i = 0; i < 4000 && at < 0; ++i) {
      r.eng.process(L.data(), R.data(), kBlock);
      if (r.eng.liveActive(0) == c) {
        at = r.eng.position();
      }
    }
    return at;
  };
  const double trip = startOf(static_cast<int>(LaunchGrid::EighthT));
  expect(trip >= kArea + 64 && trip < kArea + 64 + 4, "a clip on 1/8T starts on the next eighth-triplet line (32 ticks), not the track's bar");
  const double glob = startOf(-1);
  expect(glob >= kArea + kBar && glob < kArea + kBar + 4, "Global: the clip waits for its track's grid (1 Bar)");
  expect(launchGridTicks(makeEmptySong(), LaunchGrid::SixteenthT, 0) == 16, "1/16T is a third of a beat's half (16 ticks at 96 PPQ)");
  expect(launchGridTicks(makeEmptySong(), LaunchGrid::ThirtySecond, 0) == 12, "1/32 is 12 ticks at 96 PPQ");
}

// A clip's own launch mode: Toggle over the track's Trigger (the second launch stops it).
void testClipMode() {
  Rig r;
  const ObjectId c = addClip(r.s, r.pat, 0, 0);
  r.s.tracks[0].launch = LaunchGrid::Now;
  r.s.clip(c)->launchSet.mode = static_cast<int8_t>(PadMode::Toggle);
  r.start();
  r.eng.launchClip(c);
  r.runTo(kArea + 50);
  expect(r.eng.liveActive(0) == c, "the toggle clip plays");
  r.eng.launchClip(c);
  r.runTo(kArea + 100);
  expect(r.eng.liveActive(0) == kNoId, "a second press stops a clip whose own mode is Toggle");
}

// Clip follow actions.
struct Column {
  Rig r;
  ObjectId c[3] = {kNoId, kNoId, kNoId};
  Column() {
    for (int k = 0; k < 3; ++k) {
      c[k] = addClip(r.s, r.pat, 0, static_cast<Tick>(k) * kScene);
    }
    r.s.tracks[0].launch = LaunchGrid::Bar;
  }
  FollowSpec& follow(int k) {
    r.s.clip(c[k])->launchSet.follow.on = true;
    return r.s.clip(c[k])->launchSet.follow;
  }
  void launchFirst() {
    r.start();
    r.runTo(kArea + 40);
    r.eng.launchClip(c[0]);  // starts at kArea + kBar
  }
};

void testFollowActions() {
  {
    Column col;
    col.follow(0).a = FollowAction::Next;  // one bar (time 0)
    col.launchFirst();
    col.r.runTo(kArea + 2 * kBar - 8);
    expect(col.r.eng.liveActive(0) == col.c[0], "the first clip plays until its follow time");
    col.r.runTo(kArea + 2 * kBar + 8);
    expect(col.r.eng.liveActive(0) == col.c[1], "Next: one bar after it started the next clip of the group plays");
  }
  {
    Column col;
    FollowSpec& f = col.follow(0);
    f.a = FollowAction::Stop;
    f.time = 2 * kBar;
    col.launchFirst();
    col.r.runTo(kArea + 3 * kBar - 8);
    expect(col.r.eng.liveActive(0) == col.c[0], "Stop waits for the follow time (2 bars)");
    col.r.runTo(kArea + 3 * kBar + 8);
    expect(col.r.eng.liveActive(0) == kNoId, "Stop: the track stops at the follow time");
  }
  {
    Column col;
    FollowSpec& f = col.follow(0);
    f.a = FollowAction::Stop, f.chanceA = 0;
    f.b = FollowAction::Last, f.chanceB = 100;
    col.launchFirst();
    col.r.runTo(kArea + 2 * kBar + 8);
    expect(col.r.eng.liveActive(0) == col.c[2], "chances: A at 0 % never runs, B (Last) does");
  }
  {
    Column col;
    FollowSpec& f = col.follow(0);
    f.a = FollowAction::Jump, f.jumpA = 2;
    col.launchFirst();
    col.r.runTo(kArea + 2 * kBar + 8);
    expect(col.r.eng.liveActive(0) == col.c[2], "Jump: goes to the clip of the slot it names (scene 3)");
  }
  {
    Column col;
    FollowSpec& f = col.follow(0);
    f.a = FollowAction::Next, f.linked = true, f.loops = 3;
    col.launchFirst();
    col.r.runTo(kArea + 4 * kBar - 8);
    expect(col.r.eng.liveActive(0) == col.c[0], "linked: the clip plays its length three times first");
    col.r.runTo(kArea + 4 * kBar + 8);
    expect(col.r.eng.liveActive(0) == col.c[1], "linked: then the follow action runs");
  }
  {
    Column col;
    col.follow(0).a = FollowAction::Next;
    col.r.start();
    col.r.eng.setFollowActions(false);
    col.r.runTo(kArea + 40);
    col.r.eng.launchClip(col.c[0]);
    col.r.runTo(kArea + 3 * kBar);
    expect(col.r.eng.liveActive(0) == col.c[0], "the global switch off: no follow action runs");
  }
}

// The same follow actions on every run (Any / Other pick by the engine's own seeded generator).
std::vector<ObjectId> anyRun() {
  Column col;
  for (int k = 0; k < 3; ++k) {
    col.follow(k).a = k == 1 ? FollowAction::Other : FollowAction::Any;
  }
  col.launchFirst();
  std::vector<ObjectId> seen;
  for (int b = 2; b < 20; ++b) {
    col.r.runTo(kArea + b * kBar + 8);
    seen.push_back(col.r.eng.liveActive(0));
  }
  return seen;
}
void testDeterministic() {
  const std::vector<ObjectId> a = anyRun(), b = anyRun();
  expect(a == b, "Any / Other follow actions pick the same clips on every run");
  bool moved = false;
  for (size_t i = 1; i < a.size(); ++i) {
    moved = moved || a[i] != a[0];
  }
  expect(moved, "Any / Other do move on to other clips");
}

// A scene launched as one: its empty slots stop their tracks where the stop button stays.
void testSceneLaunch() {
  for (int removed = 0; removed < 2; ++removed) {
    Rig r;
    const ObjectId a0 = addClip(r.s, r.pat, 0, 0);          // track 0: scene 1
    const ObjectId b1 = addClip(r.s, r.pat, 1, kScene);     // track 1: scene 2 only
    for (PlaylistTrack& t : r.s.tracks) {
      t.launch = LaunchGrid::Bar;
    }
    if (removed) {
      trackmodes::setStopButton(r.s, 1, 0, false);
    }
    r.start();
    r.eng.launchClip(b1);
    r.runTo(kArea + kBar + 8);
    expect(r.eng.liveActive(1) == b1, "track 2 plays its clip");
    r.eng.launchScene(0);
    r.runTo(kArea + 2 * kBar + 8);
    expect(r.eng.liveActive(0) == a0, "the scene launches its clips");
    expect(r.eng.liveScene() == 0, "the engine knows the scene launched last");
    if (removed) {
      expect(r.eng.liveActive(1) == b1, "an empty slot without its stop button leaves its track playing");
    } else {
      expect(r.eng.liveActive(1) == kNoId, "an empty slot with its stop button stops its track");
    }
  }
}

// A scene's tempo and time signature apply at its launch point; its follow action launches the next.
void testSceneTempo() {
  Rig r;
  addClip(r.s, r.pat, 0, 0);
  addClip(r.s, r.pat, 0, kScene);
  r.s.tracks[0].launch = LaunchGrid::Bar;
  SceneInfo sc;
  sc.tempo = 150;
  sc.sigNum = 3, sc.sigDen = 4;
  sc.follow.on = true;
  sc.follow.a = FollowAction::Next;
  sc.follow.time = 2 * 288;  // two bars of 3/4
  trackmodes::setSceneSettings(r.s, 0, sc);
  r.start();
  r.runTo(kArea + 40);
  const uint32_t serial0 = r.eng.sceneChangeSerial();
  r.eng.launchScene(0);
  r.runSamples(kBlock * 2);
  expect(std::fabs(r.eng.scenePendingAt() - static_cast<double>(kArea + kBar)) < 0.5, "the scene's tempo waits for its launch point (the next bar)");
  r.runTo(kArea + kBar - 20);
  expect(r.eng.sceneChangeSerial() == serial0, "not before it");
  r.runTo(kArea + kBar + 1);
  expect(r.eng.sceneChangeSerial() != serial0, "the scene's tempo applied at its launch point");
  expect(std::fabs(r.eng.sceneTempoSet() - 150) < 1e-9 && r.eng.sceneSigNumSet() == 3 && r.eng.sceneSigDenSet() == 4, "its tempo and time signature");
  const double p0 = r.eng.position();
  r.runSamples(kSr);  // one second at 150 BPM: 240 ticks (120 BPM would be 192)
  const double moved = r.eng.position() - p0;
  expect(std::fabs(moved - 240.0) < 3.0, "the transport runs at the scene's tempo");
  std::printf("  scene tempo: %.1f ticks in one second\n", moved);
  r.runTo(kArea + kBar + 2 * 288 + 8);
  expect(r.eng.liveScene() == 1, "the scene follow action (Next, two bars of the scene's 3/4) launched the next scene");
  r.eng.stopAllLive();
  r.eng.stop();
  r.runSamples(kBlock * 2);
  expect(r.eng.liveScene() == -1, "stop ends the scene's tempo and follow actions");
}

// A scene inserted while the Arrangement plays: the transport moves with it (the same music goes on).
void testOriginShift() {
  Rig r;
  r.s.live.areaEnd = kScene;
  addClip(r.s, r.pat, 0, 0);
  addClip(r.s, r.pat, 0, kScene + 4 * kBar, 4 * kBar);  // an Arrangement clip
  r.b.setOriginEpoch(7);
  r.from = kScene + 2 * kBar;
  r.eng.setProgram(r.b.build(r.s));
  r.eng.setArrangement(kScene, 0, false);
  r.eng.setSessionOverArrangement(true);
  r.eng.setMode(PlayMode::Song);
  r.eng.setPosition(r.from);
  r.eng.play();
  r.runSamples(kBlock * 4);
  const double before = r.eng.position();
  trackmodes::insertScene(r.s, 0, kScene);
  expect(r.s.live.areaEnd == 2 * kScene, "the live area grew by a scene");
  r.eng.setProgram(r.b.build(r.s));
  r.runSamples(kBlock);
  const double after = r.eng.position();
  expect(after - before > kScene - 2 && after - before < kScene + 4, "the song position moved with the Arrangement (by one scene)");
  std::printf("  origin shift: %.1f -> %.1f\n", before, after);
  ProgramBuilder other;
  other.setOriginEpoch(8);  // another project: nothing moves
  trackmodes::insertScene(r.s, 0, kScene);
  r.eng.setProgram(other.build(r.s));
  const double p1 = r.eng.position();
  r.runSamples(kBlock);
  expect(r.eng.position() - p1 < 10, "a program of another project moves nothing");
}

// ---- the format ----
void testRoundTrip() {
  Song s = makeEmptySong();
  const ObjectId ch = addChannel(s);
  const ObjectId p = addPattern(s, ch);
  s.live.areaEnd = 2 * kScene;
  const ObjectId c = addClip(s, p, 0, 0);
  addClip(s, p, 1, kScene);
  Clip* cp = s.clip(c);
  cp->launchSet.mode = static_cast<int8_t>(PadMode::Gate);
  cp->launchSet.quant = static_cast<int8_t>(LaunchGrid::QuarterT);
  cp->launchSet.legato = true;
  cp->launchSet.follow.on = true;
  cp->launchSet.follow.a = FollowAction::Jump;
  cp->launchSet.follow.jumpA = 1;
  cp->launchSet.follow.b = FollowAction::Other;
  cp->launchSet.follow.chanceA = 30, cp->launchSet.follow.chanceB = 70;
  cp->launchSet.follow.linked = true, cp->launchSet.follow.loops = 4;
  trackmodes::setStopButton(s, 1, 0, false);
  SceneInfo sc;
  sc.name = "Verse";
  sc.tempo = 133.5;
  sc.sigNum = 7, sc.sigDen = 8;
  sc.follow.on = true, sc.follow.a = FollowAction::Previous, sc.follow.time = 3 * kBar;
  trackmodes::setSceneSettings(s, 1, sc);
  trackmodes::setSceneName(s, 1, "Verse");
  const std::string text = projectToText(s);
  expect(text.find("\"version\": 10") != std::string::npos || text.find("\"version\":10") != std::string::npos, "a clip grid saves as version 10");
  Song back;
  std::string err;
  expect(loadProjectText(text, "", back, &err), "it loads");
  const Clip* bc = back.clip(c);
  expect(bc && bc->launchSet == cp->launchSet, "the clip's launch settings and follow actions load back");
  expect(!trackmodes::hasStopButton(back, 1, 0) && trackmodes::hasStopButton(back, 0, 0), "the removed stop button loads back");
  const SceneInfo* bs = trackmodes::sceneInfo(back, 1);
  expect(bs && bs->tempo == 133.5 && bs->sigNum == 7 && bs->sigDen == 8 && bs->follow == sc.follow, "the scene's tempo, time signature and follow actions load back");
  expect(back.playlist.size() == s.playlist.size(), "no clip moved (one clip a slot already)");
  Song plain = makeEmptySong();
  const std::string ptext = projectToText(plain);
  expect(ptext.find("\"version\": 10") == std::string::npos && ptext.find("\"version\":10") == std::string::npos,
         "a song without a clip grid keeps the older version number (older builds open it)");
}

void testMigration() {
  Song s = makeEmptySong();
  const ObjectId ch = addChannel(s);
  const ObjectId p = addPattern(s, ch);
  s.live.areaEnd = 2 * kScene;
  const ObjectId a = addClip(s, p, 0, 0);
  const ObjectId b = addClip(s, p, 0, kBar);       // a second clip in track 1's slot of scene 1
  const ObjectId c = addClip(s, p, 0, 2 * kBar);   // and a third
  const ObjectId d = addClip(s, p, 1, kScene);     // track 2, scene 2
  const ObjectId e = addClip(s, p, 0, 2 * kScene + kBar);  // the Arrangement
  trackmodes::setSceneName(s, 1, "Chorus");
  Json j = songToJson(s);
  j.object["version"] = Json::makeNumber(9);
  Song m;
  std::string err;
  expect(songFromJson(j, m, &err), "a version 9 project loads");
  expect(m.playlist.size() == 5, "no clip is lost");
  auto start = [&](ObjectId id) { return m.clip(id) ? m.clip(id)->start : -1; };
  expect(start(a) == 0, "the first clip stays in its slot");
  expect(start(b) == kScene + kBar, "the second moves to a new scene under it (its place in the scene kept)");
  expect(start(c) == 2 * kScene + 2 * kBar, "the third to the next new scene");
  expect(start(d) == 3 * kScene, "the later scene moves down two rows");
  expect(m.live.areaEnd == 4 * kScene, "the live area grew by the two scenes");
  expect(start(e) == 4 * kScene + kBar, "the Arrangement moves after it (the same place in it)");
  const SceneInfo* chorus = trackmodes::sceneInfo(m, 3);
  expect(chorus && chorus->name == "Chorus", "the later scene's name moves with it");
  for (int k = 0; k < 4; ++k) {
    int n = 0;
    for (const Clip& x : m.playlist) {
      n += x.track == 0 && x.start >= k * kScene && x.start < (k + 1) * kScene ? 1 : 0;
    }
    expect(n <= 1, "one clip a slot after the migration");
  }
  Song again;
  expect(loadProjectText(projectToText(m), "", again, &err) && again.playlist.size() == 5 && again.clip(b)->start == kScene + kBar,
         "saved again (version 10) it loads as it is");
}

void testSceneNames() {
  SceneInfo a;
  a.name = "Verse 128 BPM 6/8";
  int n = 0, d = 0;
  expect(std::fabs(trackmodes::sceneTempo(a) - 128) < 1e-9, "a tempo in the name (\"128 BPM\")");
  expect(trackmodes::sceneSignature(a, n, d) && n == 6 && d == 8, "a time signature in the name (\"6/8\")");
  a.tempo = 90;
  expect(std::fabs(trackmodes::sceneTempo(a) - 90) < 1e-9, "the scene's own tempo wins over its name");
  SceneInfo b;
  b.name = "Intro";
  expect(trackmodes::sceneTempo(b) == 0 && !trackmodes::sceneSignature(b, n, d), "a plain name sets nothing");
}

void testSceneEdits() {
  Song s = makeEmptySong();
  const ObjectId ch = addChannel(s);
  const ObjectId p = addPattern(s, ch);
  s.live.areaEnd = 2 * kScene;
  addClip(s, p, 0, 0);
  addClip(s, p, 0, kScene);
  s.cycleStart = 2 * kScene, s.cycleEnd = 2 * kScene + 4 * kBar;
  trackmodes::setStopButton(s, 0, 1, false);
  trackmodes::insertScene(s, 0, kScene);
  expect(!trackmodes::hasStopButton(s, 0, 2) && trackmodes::hasStopButton(s, 0, 1), "an inserted scene moves the removed stop button down with its scene");
  expect(s.cycleStart == 3 * kScene, "the Arrangement's loop moves with the Arrangement");
  trackmodes::deleteScene(s, 1, kScene);
  expect(!trackmodes::hasStopButton(s, 0, 1), "a deleted scene moves it back up");
  expect(s.cycleStart == 2 * kScene, "and the loop back");
  expect(trackmodes::usesGridV10(s), "a clip grid needs version 10");
}

}  // namespace

int main() {
  gl::test::hermeticTestEnv();
  testClipQuantisation();
  testClipMode();
  testFollowActions();
  testDeterministic();
  testSceneLaunch();
  testSceneTempo();
  testOriginShift();
  testRoundTrip();
  testMigration();
  testSceneNames();
  testSceneEdits();
  return finish();
}
