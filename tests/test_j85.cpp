// Gridline — J-85: one-step song loop markers (FL's "loop" marker action) and
// marker actions in the audio engine.
//  * a song loop marker: playback reaching the song end goes on at the marker,
//    sample-exactly, for any block size, also when started after the marker;
//  * an explicit time selection takes precedence; pattern mode is unaffected;
//  * marker loop / skip / pause / stop run at their exact sample;
//  * offline renders (export) play the song once, with no marker actions;
//  * the add-loop-marker command (with / without a time selection), undo,
//    move and delete; project and FLP round trips (FL marker kind 4).
#include <cmath>
#include <string>
#include <vector>
#include "../src/core/Document.h"
#include "../src/core/MenuOpsExtra.h"
#include "../src/core/MenuParity.h"
#include "../src/core/NoteTools.h"
#include "../src/core/PluginParams.h"
#include "../src/core/Project.h"
#include "../src/core/Json.h"
#include "../src/engine/AudioEngine.h"
#include "../src/engine/Program.h"
#include "../src/engine/Render.h"
#include "Check.h"
#include "flp_export/core.h"

using namespace gl;

namespace {

constexpr int kRate = 44100;
constexpr int64_t kBar = 88200;  // one 4/4 bar at 120 BPM

// 120 BPM, a synth note every bar, clips on bars 1 and 4: the song is 4 bars.
Document makeSong() {
  Document d(makeEmptySong());
  d.setParam(songParam(SongParam::Tempo), 120.0, false);
  const ObjectId pat = d.song().patterns[0].id;
  const ObjectId lead = d.addChannel("lead", "gl.synth");
  d.addNote(pat, lead, {0, 96, 60, 110});
  d.addClip(pat, 0, 0);
  d.addClip(pat, 0, 3 * d.song().barTicks());
  return d;
}

double barT(const Song& s) { return static_cast<double>(s.barTicks()); }
double tickPerSample(const Song& s) { return barT(s) / static_cast<double>(kBar); }

// Plays `total` samples in blocks of `block` (the last one shorter).
void run(AudioEngine& eng, int64_t total, int block) {
  std::vector<float> L(static_cast<size_t>(block)), R(static_cast<size_t>(block));
  for (int64_t done = 0; done < total;) {
    const int n = static_cast<int>(std::min<int64_t>(block, total - done));
    eng.process(L.data(), R.data(), n);
    done += n;
  }
}

struct Rig {
  ProgramBuilder builder;
  AudioEngine eng{static_cast<double>(kRate)};
  explicit Rig(const Song& s, PlayMode mode = PlayMode::Song) {
    builder.setSampleRate(kRate);
    eng.setProgram(builder.build(s));
    eng.setMode(mode);
  }
};

bool near(double a, double b, double tol = 1e-6) { return std::fabs(a - b) <= tol; }

void testSongLoopEngine() {
  Document d = makeSong();
  ops::addSongLoopMarker(d, d.song().barTicks());  // loop from bar 2
  const Song& s = d.song();
  const double bar = barT(s), tps = tickPerSample(s);
  {
    Rig r(s);
    auto prog = r.builder.build(s);
    expect(prog->songLoopStart == bar && prog->songLoopEnd == 4 * s.barTicks(), "program: the loop runs from the marker to the song end");
  }
  // Sample-exact wrap at the song end, for several block sizes.
  bool exact = true, before = true, after = true;
  for (int block : {1, 7, 64, 333, 512, 4096}) {
    {
      Rig r(s);
      r.eng.play();
      run(r.eng, 4 * kBar - 1, block);  // one sample before the song end
      before = before && near(r.eng.position(), 4 * bar - tps);
    }
    {
      Rig r(s);
      r.eng.play();
      run(r.eng, 4 * kBar, block);  // exactly the song end: back at the marker
      exact = exact && (near(r.eng.position(), bar) || near(r.eng.position(), 4 * bar));  // the wrap lands on the next sample rendered
    }
    {
      Rig r(s);
      r.eng.play();
      run(r.eng, 4 * kBar + 37, block);
      after = after && near(r.eng.position(), bar + 37 * tps);
      run(r.eng, 3 * kBar - 37, block);  // the next pass (3 bars) wraps again on its sample
      after = after && (near(r.eng.position(), bar) || near(r.eng.position(), 4 * bar)) && r.eng.playing();
    }
  }
  expect(before, "song loop: playback runs to the last sample before the song end");
  expect(exact, "song loop: on the song end's sample playback is at the loop marker (any block size)");
  expect(after, "song loop: it continues from the marker sample-exactly, every pass");

  // Started after the marker: still loops back to it at the song end.
  {
    Rig r(s);
    r.eng.setPosition(static_cast<Tick>(2.5 * bar));
    r.eng.play();
    run(r.eng, kBar + kBar / 2 + 100, 256);
    expect(near(r.eng.position(), bar + 100 * tps), "song loop: started after the marker, the song end still loops to it");
  }
  // Started before the marker: plays through it (no jump) up to the song end.
  {
    Rig r(s);
    r.eng.play();
    run(r.eng, kBar + kBar / 2, 512);
    expect(near(r.eng.position(), 1.5 * bar), "song loop: passing the marker does nothing");
  }
  // No marker: the song end wraps to the start (as before).
  {
    Document plain = makeSong();
    Rig r(plain.song());
    r.eng.play();
    run(r.eng, 4 * kBar + 10, 512);
    expect(near(r.eng.position(), 10 * tps), "no loop marker: the song end wraps to the start");
  }
}

void testPrecedenceAndModes() {
  Document d = makeSong();
  ops::addSongLoopMarker(d, d.song().barTicks());
  const Song& s = d.song();
  const double bar = barT(s), tps = tickPerSample(s);
  // A time selection takes precedence: it loops, the loop marker does not apply.
  {
    Rig r(s);
    r.eng.setLoopRange(2 * s.barTicks(), 3 * s.barTicks());
    r.eng.setPosition(2 * s.barTicks());
    r.eng.play();
    run(r.eng, kBar + kBar / 2, 512);
    expect(near(r.eng.position(), 2.5 * bar), "time selection: loops the selection (precedence over the loop marker)");
    r.eng.jumpTo(static_cast<Tick>(3.5 * bar), 0);  // past the selection: the song end wraps into the selection
    run(r.eng, kBar / 2 + 50, 512);
    expect(near(r.eng.position(), 2 * bar + 50 * tps), "time selection: the song end wraps to the selection, not to the marker");
    r.eng.setLoopRange(0, 0);  // cleared: the loop marker applies again
    r.eng.jumpTo(static_cast<Tick>(3.5 * bar), 0);
    run(r.eng, kBar / 2 + 50, 512);
    expect(near(r.eng.position(), bar + 50 * tps), "time selection cleared: the loop marker applies again");
  }
  // Pattern mode is unaffected (the pattern loops over its own length).
  {
    Rig r(s, PlayMode::Pattern);
    r.eng.setPattern(s.patterns[0].id);
    r.eng.play();
    run(r.eng, kBar + kBar / 4, 512);
    expect(r.eng.position() < bar && near(r.eng.position(), 0.25 * bar, 0.01 * bar), "pattern mode: marker actions do not apply");
  }
}

void testOtherActions() {
  const Tick B = makeSong().song().barTicks();
  const double bar = static_cast<double>(B);
  auto withMarkers = [&](std::vector<Marker> ms) {
    Document d = makeSong();
    d.editSong("markers", [&](Song& x) { x.markers = ms; });
    return d;
  };
  // FL marker loop: repeats [marker, next marker) once playback is inside it.
  {
    Document d = withMarkers({{B, "L", 0, 0, MarkerAction::Loop}, {2 * B, "E"}});
    Rig r(d.song());
    r.eng.play();
    run(r.eng, 2 * kBar + kBar / 2, 300);
    expect(near(r.eng.position(), 1.5 * bar), "marker loop: repeats up to the next marker");
    Rig r2(d.song());
    r2.eng.setPosition(static_cast<Tick>(2.5 * bar));
    r2.eng.play();
    run(r2.eng, kBar / 4, 300);
    expect(near(r2.eng.position(), 2.75 * bar), "marker loop: started after it, playback plays on");
  }
  // Skip: jumps to the next marker on the marker's sample.
  {
    Document d = withMarkers({{B, "S", 0, 0, MarkerAction::Skip}, {3 * B, "T"}});
    for (int block : {64, 4096}) {
      Rig r(d.song());
      r.eng.play();
      run(r.eng, kBar + 5, block);
      expect(near(r.eng.position(), 3 * bar + 5 * bar / kBar), ("skip: lands on the next marker sample-exactly, block " + std::to_string(block)).c_str());
    }
  }
  // Pause: holds at the marker; Play continues from it (no second pause).
  {
    Document d = withMarkers({{B, "P", 0, 0, MarkerAction::Pause}});
    Rig r(d.song());
    r.eng.play();
    run(r.eng, 2 * kBar, 512);
    expect(!r.eng.playing() && near(r.eng.position(), bar), "pause: playback pauses on the marker");
    r.eng.play();
    run(r.eng, kBar / 2, 512);
    expect(r.eng.playing() && near(r.eng.position(), 1.5 * bar), "pause: Play continues from the marker");
  }
  // Stop: stops and returns where the Stop button does (the seek position).
  {
    Document d = withMarkers({{2 * B, "X", 0, 0, MarkerAction::Stop}});
    Rig r(d.song());
    r.eng.setPosition(B / 2);  // the seek position
    r.eng.play();
    run(r.eng, 3 * kBar, 512);
    expect(!r.eng.playing() && near(r.eng.position(), 0.5 * bar), "stop: playback stops and returns to the seek position");
  }
  // A stop marker after the loop marker ends playback before the song end (FL: actions run in order).
  {
    Document d = withMarkers({{B, "Loop", 0, 0, MarkerAction::SongLoop}, {2 * B, "X", 0, 0, MarkerAction::Stop}});
    Rig r(d.song());
    r.eng.play();
    run(r.eng, 3 * kBar, 512);
    expect(!r.eng.playing(), "song loop + a later stop marker: the stop wins (reached first)");
  }
}

void testOfflineRender() {
  Document plain = makeSong();
  Document looped = makeSong();
  ops::addSongLoopMarker(looped, looped.song().barTicks());
  RenderOptions o;
  o.sampleRate = kRate;
  const AudioBuffer a = renderSong(plain.song(), o), b = renderSong(looped.song(), o);
  bool same = a.frames == b.frames && a.data.size() == b.data.size();
  for (size_t i = 0; same && i < a.data.size(); ++i) same = a.data[i] == b.data[i];
  expect(same && a.frames > 0, "render: a loop marker does not change the song render (played once)");
  RenderJob j;
  j.sampleRate = kRate;
  const RenderResult ra = renderAudio(plain.song(), j), rb = renderAudio(looped.song(), j);
  same = ra.bodyFrames == rb.bodyFrames && ra.audio.data.size() == rb.audio.data.size();
  for (size_t i = 0; same && i < ra.audio.data.size(); ++i) same = ra.audio.data[i] == rb.audio.data[i];
  expect(same && rb.bodyFrames == 4 * kBar, "export: the song renders once to its end, no loop");
  RenderJob j2 = j;
  j2.loops = 2;
  const RenderResult r2 = renderAudio(looped.song(), j2);
  expect(r2.bodyFrames == 8 * kBar, "export with 2 loops: the whole song twice (the export's own loop, not the marker)");
}

void testCommands() {
  Document d = makeSong();
  const Tick B = d.song().barTicks();
  // Without a time selection: a song loop marker at the position.
  int i = ops::placeLoop(d, 0, 0, 2 * B);
  expect(i >= 0 && d.song().markers.size() == 1 && d.song().markers[0].action == MarkerAction::SongLoop && d.song().markers[0].name == "Loop" &&
             d.song().markers[0].pos == 2 * B,
         "place loop without a selection: one song loop marker named Loop");
  expect(ops::songLoopMarker(d.song()) == 0, "the song loop marker is found");
  d.undo();
  expect(d.song().markers.empty(), "add loop marker: one undo step");
  d.redo();
  // A second one replaces the first (FL: one loop marker per song).
  ops::addSongLoopMarker(d, B);
  expect(d.song().markers.size() == 1 && d.song().markers[0].action == MarkerAction::SongLoop && d.song().markers[0].pos == B,
         "a new loop marker replaces the song's loop marker");
  d.undo();
  expect(d.song().markers.size() == 1 && d.song().markers[0].pos == 2 * B, "undo restores the earlier loop marker");
  // Placing it where it is removes it (FL's toggle).
  expect(ops::addSongLoopMarker(d, 2 * B) < 0 && d.song().markers.empty(), "placing the loop marker on itself removes it");
  d.undo();
  // A plain marker given the loop action by hand keeps the song's single loop start: the last one before the end.
  ops::addMarker(d, 3 * B, "Later");
  d.editSong("action", [](Song& x) { x.markers[1].action = MarkerAction::SongLoop; });
  expect(ops::songLoopMarker(d.song()) == 1, "two loop markers: the later one is the loop start");
  d.undo();
  d.undo();
  // Move: the loop start follows the marker (undoable).
  ops::editMarker(d, 0, "move marker", [B](Marker& m) { m.pos = 3 * B; });
  {
    ProgramBuilder b;
    expect(b.build(d.song())->songLoopStart == static_cast<double>(3 * B), "moving the marker moves the loop start");
  }
  d.undo();
  expect(d.song().markers[0].pos == 2 * B, "move marker: undo");
  // Delete: no loop.
  ops::removeMarker(d, 0);
  {
    ProgramBuilder b;
    expect(b.build(d.song())->songLoopStart < 0 && ops::songLoopMarker(d.song()) < 0, "deleting the marker removes the loop");
  }
  d.undo();
  expect(ops::songLoopMarker(d.song()) == 0, "delete marker: undo");
  // With a time selection: FL's place loop (a marker loop + an end marker).
  Document e = makeSong();
  int k = ops::placeLoop(e, B, 2 * B, 3 * B);
  expect(k >= 0 && e.song().markers[static_cast<size_t>(k)].action == MarkerAction::Loop && e.song().markers.size() == 2 && ops::songLoopMarker(e.song()) < 0,
         "place loop with a selection: loop markers around it (no song loop marker)");
  // A loop marker at / after the song end does nothing.
  Document f = makeSong();
  ops::addSongLoopMarker(f, 4 * B);
  ProgramBuilder b;
  expect(ops::songLoopMarker(f.song()) < 0 && b.build(f.song())->songLoopStart < 0, "a loop marker at the song end is inactive");
}

void testRoundTrips() {
  Document d = makeSong();
  const Tick B = d.song().barTicks();
  ops::addSongLoopMarker(d, B);
  ops::addMarker(d, 2 * B, "Plain");
  // Project (JSON).
  Song back;
  expect(songFromJson(songToJson(d.song()), back) && back.markers.size() == 2 && back.markers[0].action == MarkerAction::SongLoop &&
             back.markers[1].action == MarkerAction::None,
         "project: the song loop marker is saved and loaded");
  // FLP: the song loop marker is FL's marker kind 4 ("loop"), exactly.
  Song s = d.song();
  s.markers.push_back({3 * B, "ML", 0, 0, MarkerAction::Loop});
  Song fb;
  flp::ExportReport rep;
  flp::Bytes bytes;
  expect(coretest::exportImport(s, fb, rep, &bytes), "flp: export / import");
  bool ok = fb.markers.size() == 3;
  for (size_t i = 0; ok && i < 3; ++i) ok = fb.markers[i].pos == s.markers[i].pos && fb.markers[i].action == s.markers[i].action;
  expect(ok, "flp: song loop, plain and marker loop actions come back");
  flp::File file;
  std::string err;
  int kind4 = 0, kind1 = 0;
  if (flp::readBytes(bytes, file, &err))
    for (const auto& ev : file.events)
      if (ev.id == 148) kind4 += (ev.value() >> 24) == 4, kind1 += (ev.value() >> 24) == 1;
  expect(kind4 == 1 && kind1 == 1, "flp: written as marker kind 4 (loop) and kind 1 (marker loop)");
}

}  // namespace

int main() {
  testSongLoopEngine();
  testPrecedenceAndModes();
  testOtherActions();
  testOfflineRender();
  testCommands();
  testRoundTrips();
  return finish();
}
