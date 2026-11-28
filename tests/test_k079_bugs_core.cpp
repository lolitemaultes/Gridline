// Gridline - 0.7.9 Track 1 regression tests:
//   * K-395 Part A: Clips track deletion (empty/unnamed sets, starter sets, last-track guard,
//     groups, undo, collab sync);
//   * K-412: Remote controller mapping curves (ALL curves, especially Inverted, with custom/reversed
//     min/max ranges, Pickup & Scale soft takeover, relative encoders, button modes, OSC, internal
//     ModLink + invert, config-change state reset, JSON round-trip);
//   * K-415: Song mode playhead past end of song (default loop-at-end, explicit loop range, song loop
//     marker, Project End marker, Clips arrangement wrap & stop, stop-at-end setting, offline export
//     invariance);
//   * K-409: Tempo type-in parsing/formatting (decimals, clamping, invalid input, 1 undo step, collab).
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>
#include "Check.h"
#include "collab/CollabSim.h"
#include "core/Document.h"
#include "core/MenuParity.h"
#include "core/ParamText.h"
#include "core/Params.h"
#include "core/Project.h"
#include "core/Remote.h"
#include "core/Settings.h"
#include "core/TrackModes.h"
#include "engine/AudioEngine.h"
#include "engine/Program.h"
#include "engine/Render.h"

using namespace gl;
namespace fs = std::filesystem;

namespace {

bool near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) <= eps; }
MidiMessage cc(int n, int v, int ch = 0) { return {static_cast<uint8_t>(0xB0 | ch), static_cast<uint8_t>(n), static_cast<uint8_t>(v), -1}; }

void runEngine(AudioEngine& eng, int64_t total, int block = 256) {
  std::vector<float> L(static_cast<size_t>(block)), R(static_cast<size_t>(block));
  for (int64_t done = 0; done < total;) {
    const int n = static_cast<int>(std::min<int64_t>(block, total - done));
    eng.process(L.data(), R.data(), n);
    done += n;
  }
}

// ========================================================================= K-395 Part A
void testK395ClipsTrackDeletion() {
  // 1. Default 32-track unnamed song (usedTrackCount == 4): deleting tracks genuinely reduces
  //    usedTrackCount 4 -> 3 -> 2 -> 1, refuses at 1, keeps Studio's trailing tracks, and undoes cleanly.
  {
    Song empty = makeEmptySong();
    empty.tracks.resize(kDefaultPlaylistTracks);
    Document d(empty);
    expect(trackmodes::usedTrackCount(d.song()) == 4, "K-395A: fresh empty song shows 4 tracks");
    std::string why;
    expect(trackmodes::deleteTracks(d, {0}, &why) == 1, "K-395A: deleting track 1 of 4 unnamed tracks succeeds");
    expect(trackmodes::usedTrackCount(d.song()) == 3, "K-395A: shown tracks drops 4 -> 3");
    expect(static_cast<int>(d.song().tracks.size()) == kDefaultPlaylistTracks - 1, "K-395A: Studio trailing tracks preserved");
    expect(trackmodes::deleteTracks(d, {0}, &why) == 1 && trackmodes::usedTrackCount(d.song()) == 2, "K-395A: shown tracks drops 3 -> 2");
    expect(trackmodes::deleteTracks(d, {0}, &why) == 1 && trackmodes::usedTrackCount(d.song()) == 1, "K-395A: shown tracks drops 2 -> 1");
    expect(trackmodes::deleteTracks(d, {0}, &why) == 0 && !why.empty(), "K-395A: cannot delete last shown track");
    expect(trackmodes::usedTrackCount(d.song()) == 1, "K-395A: still 1 track after refused delete");
    expect(d.undo() && trackmodes::usedTrackCount(d.song()) == 2, "K-395A: undo brings back track (1 -> 2)");
    expect(d.undo() && d.undo() && trackmodes::usedTrackCount(d.song()) == 4, "K-395A: undo restores original 4 tracks");
  }

  // 2. Clips starter set (1 MIDI, 2 MIDI, 3 Audio, 4 Audio): delete down to 1, refuse deleting last
  //    (must NOT jump back up to 4!), refuse deleting all 4 at once, undo works in 1 step.
  {
    Song s = makeEmptySong();
    s.tracks.resize(kDefaultPlaylistTracks);
    trackmodes::makeClipsStarterTracks(s);
    Document d(s);
    expect(trackmodes::usedTrackCount(d.song()) == 4, "K-395A: starter set has 4 tracks");
    std::string why;
    expect(trackmodes::deleteTracks(d, {0, 1, 2, 3}, &why) == 0, "K-395A: deleting all 4 shown tracks at once refused");
    expect(trackmodes::usedTrackCount(d.song()) == 4, "K-395A: all 4 starter tracks still intact");
    expect(trackmodes::deleteTracks(d, {3}, &why) == 1 && trackmodes::usedTrackCount(d.song()) == 3, "K-395A: starter 4 -> 3");
    expect(trackmodes::deleteTracks(d, {2}, &why) == 1 && trackmodes::usedTrackCount(d.song()) == 2, "K-395A: starter 3 -> 2");
    expect(trackmodes::deleteTracks(d, {1}, &why) == 1 && trackmodes::usedTrackCount(d.song()) == 1, "K-395A: starter 2 -> 1");
    expect(trackmodes::deleteTracks(d, {0}, &why) == 0, "K-395A: deleting last starter track refused");
    expect(trackmodes::usedTrackCount(d.song()) == 1, "K-395A: count stays 1 (never jumps back to 4)");
    expect(d.undo() && trackmodes::usedTrackCount(d.song()) == 2, "K-395A: starter undo restores track");
  }

  // 3. Track 0 unnamed, Track 1 named ("Bass") -> shown == 2; deleting Track 1 leaves shown == 1 (never jumps to 4).
  {
    Song s = makeEmptySong();
    s.tracks.resize(kDefaultPlaylistTracks);
    s.tracks[1].name = "Bass";
    Document d(s);
    expect(trackmodes::usedTrackCount(d.song()) == 2, "K-395A: unnamed 0 + named 1 -> 2 shown");
    std::string why;
    expect(trackmodes::deleteTracks(d, {1}, &why) == 1 && trackmodes::usedTrackCount(d.song()) == 1,
           "K-395A: deleting sole named track leaves 1 shown track, not 4");
  }

  // 4. Collab sync of Clips track deletion.
  {
    Song s = makeEmptySong();
    s.tracks.resize(kDefaultPlaylistTracks);
    trackmodes::makeClipsStarterTracks(s);
    sim::Clock clock;
    sim::Net net(79);
    sim::Peer a, b;
    a.label = "Alex";
    b.label = "Sam";
    a.id = collab::crypto::Identity::generate();
    b.id = collab::crypto::Identity::generate();
    a.doc.reset(s);
    a.folder = sim::tmpDir("k079_clips_del_a");
    b.folder = sim::tmpDir("k079_clips_del_b");
    std::string err;
    a.s = collab::Session::create(a.doc, sim::config(a.folder, a.id, a.label, &clock), &err);
    net.add(&a);
    b.s = collab::Session::join(b.doc, sim::config(b.folder, b.id, b.label, &clock), sim::ticketOf(*a.s), &err);
    a.s->admitMember(sim::memberOf(b));
    net.add(&b);
    net.connect(&a, &b);
    sim::settle(net, {&a, &b}, clock);
    std::string why;
    expect(trackmodes::deleteTracks(a.doc, {2}, &why) == 1, "K-395A collab: peer 0 deletes track 3");
    sim::settle(net, {&a, &b}, clock);
    expect(trackmodes::usedTrackCount(a.doc.song()) == 3 && trackmodes::usedTrackCount(b.doc.song()) == 3,
           "K-395A collab: both peers agree on 3 tracks after delete");
  }
}

// ========================================================================= K-412
void testK412RemoteCurves() {
  // 1. Audit ALL curve types on absolute CC.
  {
    RemoteMapper m;
    double out = -1;
    const double xs[] = {0.0, 0.25, 0.5, 0.75, 1.0};
    for (int ci = 0; ci < static_cast<int>(ModCurve::Count); ++ci) {
      ControllerLink l;
      l.cc = 74;
      l.curve = static_cast<ModCurve>(ci);
      if (l.curve == ModCurve::Formula) l.formula = "1 - input";
      for (double u : xs) {
        const int d2 = static_cast<int>(std::lround(u * 127.0));
        const double uq = d2 / 127.0;
        expect(m.process(l, cc(74, d2), 0.5, &out), "K-412: absolute curve produces output");
        const double expected = l.curve == ModCurve::Formula ? (1.0 - uq) : applyModCurve(l.curve, uq);
        expect(near(out, expected, 1e-6), "K-412: curve matches expected transfer function");
      }
    }
  }

  // 2. Inverted curve with full range, custom sub-range [0.2, 0.8], reversed range [0.8, 0.2], and double invert.
  {
    RemoteMapper m;
    double out = -1;
    ControllerLink l;
    l.cc = 1;
    l.curve = ModCurve::Inverted;
    expect(m.process(l, cc(1, 0), 0.5, &out) && near(out, 1.0), "K-412: Inverted CC 0 -> 1");
    expect(m.process(l, cc(1, 127), 0.5, &out) && near(out, 0.0), "K-412: Inverted CC 127 -> 0");
    expect(m.process(l, cc(1, 64), 0.5, &out) && near(out, 1.0 - 64.0 / 127.0), "K-412: Inverted CC 64 -> ~0.496");

    l.min = 0.2f;
    l.max = 0.8f;
    expect(m.process(l, cc(1, 0), 0.5, &out) && near(out, 0.8, 1e-5), "K-412: Inverted [0.2,0.8] CC 0 -> 0.8");
    expect(m.process(l, cc(1, 127), 0.5, &out) && near(out, 0.2, 1e-5), "K-412: Inverted [0.2,0.8] CC 127 -> 0.2");

    l.min = 0.8f;
    l.max = 0.2f;
    expect(m.process(l, cc(1, 0), 0.5, &out) && near(out, 0.2, 1e-5), "K-412: Inverted reversed [0.8,0.2] CC 0 -> 0.2");
    expect(m.process(l, cc(1, 127), 0.5, &out) && near(out, 0.8, 1e-5), "K-412: Inverted reversed [0.8,0.2] CC 127 -> 0.8");

    l.min = 0.0f;
    l.max = 1.0f;
    l.invert = true;  // double invert cancels
    expect(m.process(l, cc(1, 0), 0.5, &out) && near(out, 0.0), "K-412: Inverted + invert flag cancels cleanly (CC 0 -> 0)");
    expect(m.process(l, cc(1, 127), 0.5, &out) && near(out, 1.0), "K-412: Inverted + invert flag cancels cleanly (CC 127 -> 1)");
  }

  // 3. Inverted curve with Pickup and Scale soft takeover (plus config change invalidation).
  {
    RemoteMapper m;
    ControllerLink l;
    l.cc = 10;
    l.curve = ModCurve::Inverted;
    l.min = 0.2f;
    l.max = 0.8f;
    l.takeover = RemoteTakeover::Pickup;
    double out = -1, cur = 0.5;
    expect(!m.process(l, cc(10, 0), cur, &out) && m.waiting(l), "K-412 Pickup+Inverted: CC 0 (mapped 0.8) waits above cur=0.5");
    expect(m.process(l, cc(10, 80), cur, &out) && !m.waiting(l), "K-412 Pickup+Inverted: crossing cur=0.5 picks up");
    const double expected80 = 0.2 + 0.6 * (1.0 - 80.0 / 127.0);
    expect(near(out, expected80, 1e-5), "K-412 Pickup+Inverted: picked-up value matches inverted sub-range");

    // Switching curve back to Linear on same mapper resets Pickup state instead of using stale lastIn.
    l.curve = ModCurve::Linear;
    expect(!m.process(l, cc(10, 0), 0.5, &out) && m.waiting(l), "K-412: changing curve resets Pickup state cleanly");

    // Scale takeover with Inverted curve reaches both endpoints smoothly.
    ControllerLink sc;
    sc.cc = 11;
    sc.curve = ModCurve::Inverted;
    sc.min = 0.2f;
    sc.max = 0.8f;
    sc.takeover = RemoteTakeover::Scale;
    RemoteMapper ms;
    cur = 0.3;
    expect(!ms.process(sc, cc(11, 10), cur, &out), "K-412 Scale+Inverted: first message establishes reference");
    for (int v = 11; v <= 127; ++v)
      if (ms.process(sc, cc(11, v), cur, &out)) cur = out;
    expect(near(cur, 0.2, 1e-5), "K-412 Scale+Inverted: sweeping CC up to 127 converges to min (0.2)");
    for (int v = 126; v >= 0; --v)
      if (ms.process(sc, cc(11, v), cur, &out)) cur = out;
    expect(near(cur, 0.8, 1e-5), "K-412 Scale+Inverted: sweeping CC down to 0 converges to max (0.8)");
  }

  // 4. Relative encoders (all 3 encodings + OSC) and Increment buttons honour ModCurve::Inverted!
  {
    RemoteMapper m;
    double out = -1;
    for (RemoteEncoder enc : {RemoteEncoder::TwosComplement, RemoteEncoder::BinaryOffset, RemoteEncoder::SignBit}) {
      ControllerLink l;
      l.cc = 20;
      l.encoder = enc;
      l.curve = ModCurve::Inverted;
      expect(m.process(l, cc(20, encodeRelative(enc, +2)), 0.5, &out) && near(out, 0.5 - 2.0 / 128.0),
             "K-412: relative encoder with ModCurve::Inverted flips +detent downward");
      expect(m.process(l, cc(20, encodeRelative(enc, -2)), 0.5, &out) && near(out, 0.5 + 2.0 / 128.0),
             "K-412: relative encoder with ModCurve::Inverted flips -detent upward");
    }
    ControllerLink oscRel;
    oscRel.source = RemoteSource::Osc;
    oscRel.oscAddress = "/enc";
    oscRel.encoder = RemoteEncoder::TwosComplement;
    oscRel.curve = ModCurve::Inverted;
    expect(m.processOsc(oscRel, +4.0, false, 0.5, &out) && near(out, 0.5 - 4.0 / 128.0),
           "K-412: OSC relative encoder honours ModCurve::Inverted");

    ControllerLink inc;
    inc.cc = 50;
    inc.button = RemoteButton::Increment;
    inc.curve = ModCurve::Inverted;
    double cur = 0.5;
    expect(m.process(inc, cc(50, 127), cur, &out, 0.25) && near(out, 0.25), "K-412: Increment with Inverted steps down");
    m.process(inc, cc(50, 0), out, &cur, 0.25);
    cur = out;
    expect(m.process(inc, cc(50, 127), cur, &out, 0.25) && near(out, 0.0), "K-412: Increment with Inverted reaches 0");
    m.process(inc, cc(50, 0), out, &cur, 0.25);
    cur = out;
    expect(m.process(inc, cc(50, 127), cur, &out, 0.25) && near(out, 1.0), "K-412: Increment with Inverted wraps 0 -> 1");
  }

  // 5. Internal controller ModLink (ProgramBuilder + AudioEngine): ModCurve::Inverted and ml.invert both work!
  {
    Song s = makeEmptySong();
    const ObjectId ch = s.allocateId();
    Channel c;
    c.id = ch;
    c.name = "Target";
    c.plugin = defaultPluginState("gl.synth");
    c.gain = 0.5f;
    c.macros[0].value = 0.25f;
    s.channels.push_back(c);
    ModLink ml;
    ml.sourceOwner = ch;
    ml.sourceSlot = kLayerMacroSlot;
    ml.sourceOutput = 0;
    ml.owner = ch;
    ml.index = static_cast<uint32_t>(ChannelParam::Pan);
    ml.curve = ModCurve::Inverted;
    ml.min = 0.0f;
    ml.max = 1.0f;
    s.modLinks.push_back(ml);

    ProgramBuilder pb;
    pb.setSampleRate(44100);
    AudioEngine eng(44100);
    auto prog = pb.build(s);
    eng.setProgram(prog);
    runEngine(eng, 256, 64);
    double panVal = -9.0;  // ChannelParam::Pan is [-1, +1]; normalized 0.75 -> +0.5
    expect(pb.modulatedValue({ch, static_cast<uint32_t>(ChannelParam::Pan)}, panVal) && near(panVal, 0.5, 1e-3),
           "K-412: internal ModLink with ModCurve::Inverted maps 0.25 -> norm 0.75 (pan +0.5)");

    s.modLinks[0].curve = ModCurve::Linear;
    s.modLinks[0].invert = true;
    eng.setProgram(pb.build(s));
    runEngine(eng, 256, 64);
    expect(pb.modulatedValue({ch, static_cast<uint32_t>(ChannelParam::Pan)}, panVal) && near(panVal, 0.5, 1e-3),
           "K-412: internal ModLink with invert=true maps 0.25 -> norm 0.75 (pan +0.5)");
  }

  // 6. JSON save/load round-trip preserves Inverted curve & all remote settings.
  {
    Song s = makeEmptySong();
    ControllerLink l;
    l.cc = 74;
    l.midiChannel = 2;
    l.owner = kSongId;
    l.index = static_cast<uint32_t>(SongParam::Tempo);
    l.curve = ModCurve::Inverted;
    l.invert = false;
    l.min = 0.15f;
    l.max = 0.85f;
    l.takeover = RemoteTakeover::Pickup;
    l.encoder = RemoteEncoder::BinaryOffset;
    l.sensitivity = 2.0f;
    l.button = RemoteButton::Toggle;
    s.links.push_back(l);
    const std::string text = projectToText(s);
    Song back;
    std::string err;
    expect(loadProjectText(text, "/tmp/k412_rt.gridline.json", back, &err) && back.links.size() == 1 && back.links[0] == l,
           "K-412: ControllerLink with Inverted curve round-trips identically through JSON");
  }
}

// ========================================================================= K-415
void testK415SongModeEnd() {
  constexpr int kSr = 44100;
  constexpr int64_t kBarSamples = 88200;  // 1 bar of 4/4 at 120 BPM

  Document d(makeEmptySong());
  d.setParam(songParam(SongParam::Tempo), 120.0, false);
  const ObjectId pat = d.song().patterns[0].id;
  const ObjectId lead = d.addChannel("Lead", "gl.synth");
  d.addNote(pat, lead, {0, 96, 60, 110});
  const Tick bar = d.song().barTicks();
  d.addClip(pat, 0, 0);
  d.addClip(pat, 0, bar);  // 2-bar song

  // 1. Default Song mode: loops back to start at end of song (bar 2 -> bar 0), never infinite silence.
  {
    ProgramBuilder pb;
    pb.setSampleRate(kSr);
    AudioEngine eng(kSr);
    eng.setProgram(pb.build(d.song()));
    eng.setMode(PlayMode::Song);
    eng.play();
    runEngine(eng, 2 * kBarSamples + 1000, 256);
    expect(eng.playing() && eng.position() < static_cast<double>(bar),
           "K-415: default Song mode wraps back to start past end of song");
  }

  // 2. Option to stop at end of song (`setStopAtEnd(true)`): stops cleanly at bar 2.
  {
    ProgramBuilder pb;
    pb.setSampleRate(kSr);
    AudioEngine eng(kSr);
    eng.setProgram(pb.build(d.song()));
    eng.setMode(PlayMode::Song);
    eng.setStopAtEnd(true);
    eng.play();
    runEngine(eng, 2 * kBarSamples + 1000, 256);
    expect(!eng.playing() && near(eng.position(), 0.0, 1.0),
           "K-415: stopAtEnd=true stops cleanly at song end and returns to start");
  }

  // 3. Explicit loop region overrides stopAtEnd (keeps looping [0, 1 bar)).
  {
    ProgramBuilder pb;
    pb.setSampleRate(kSr);
    AudioEngine eng(kSr);
    eng.setProgram(pb.build(d.song()));
    eng.setMode(PlayMode::Song);
    eng.setStopAtEnd(true);
    eng.setLoopRange(0, bar);
    eng.play();
    runEngine(eng, 3 * kBarSamples + 500, 256);
    expect(eng.playing() && eng.position() < static_cast<double>(bar),
           "K-415: explicit loop region keeps looping even with stopAtEnd=true");
  }

  // 4. Song loop marker respected when looping at end.
  {
    Document dm(d.song());
    ops::addSongLoopMarker(dm, bar);
    ProgramBuilder pb;
    pb.setSampleRate(kSr);
    AudioEngine eng(kSr);
    eng.setProgram(pb.build(dm.song()));
    eng.setMode(PlayMode::Song);
    eng.play();
    runEngine(eng, 2 * kBarSamples + 500, 256);
    expect(eng.playing() && eng.position() >= static_cast<double>(bar) && eng.position() < static_cast<double>(2 * bar),
           "K-415: song loop marker wraps back to marker at song end");
  }

  // 5. Project End marker (`Song::projectEnd`) respected for both loop and stop-at-end,
  //    while offline export (`renderSong`) remains completely unaffected!
  {
    Song sp = d.song();
    sp.projectEnd = 4 * bar;  // end marker at bar 4 (past the 2-bar clips)
    ProgramBuilder pb;
    pb.setSampleRate(kSr);
    auto prog = pb.build(sp);
    expect(prog->songEnd == 2 * bar, "K-415: prog->songEnd stays at last clip end (export unaffected)");
    expect(prog->songLoopEnd == 4 * bar, "K-415: prog->songLoopEnd follows Project End marker");

    AudioEngine eng(kSr);
    eng.setProgram(prog);
    eng.setMode(PlayMode::Song);
    eng.play();
    runEngine(eng, 3 * kBarSamples, 256);
    expect(eng.playing() && near(eng.position(), 3.0 * bar, 1.0), "K-415: plays past bar 2 up to Project End marker at bar 4");
    runEngine(eng, kBarSamples + 500, 256);
    expect(eng.playing() && eng.position() < static_cast<double>(bar), "K-415: wraps back to start after Project End marker");

    AudioEngine engStop(kSr);
    engStop.setProgram(prog);
    engStop.setMode(PlayMode::Song);
    engStop.setStopAtEnd(true);
    engStop.play();
    runEngine(engStop, 4 * kBarSamples + 500, 256);
    expect(!engStop.playing(), "K-415: stopAtEnd=true stops at Project End marker");

    // Offline export invariance: rendering with and without projectEnd produces identical length and audio!
    RenderOptions opt;
    opt.sampleRate = kSr;
    opt.tail = 0;
    const AudioBuffer rBase = renderSong(d.song(), opt);
    const AudioBuffer rEnd = renderSong(sp, opt);
    expect(rBase.frames == rEnd.frames && rBase.frames == static_cast<size_t>(2 * kBarSamples + 1),
           "K-415: offline export length completely unaffected by Project End / stopAtEnd");
    bool sameAudio = rBase.data.size() == rEnd.data.size();
    for (size_t i = 0; sameAudio && i < rBase.data.size(); ++i)
      sameAudio = std::fabs(rBase.data[i] - rEnd.data[i]) < 1e-6f;
    expect(sameAudio, "K-415: offline export audio sample-for-sample identical");
  }

  // 6. Clips Arrangement (`setArrangement(org, end, stopAtEnd)`): wraps back to `org` (never into
  //    the clip grid `< org`) when stopAtEnd=false, and stops cleanly at `end` when stopAtEnd=true.
  {
    const Tick org = 4 * bar, end = 6 * bar;
    Song sc = d.song();
    sc.live.areaEnd = org;
    for (Clip& c : sc.playlist) c.start += org;
    ProgramBuilder pb;
    pb.setSampleRate(kSr);
    auto prog = pb.build(sc);

    AudioEngine engLoop(kSr);
    engLoop.setProgram(prog);
    engLoop.setArrangement(org, end, false);
    engLoop.setMode(PlayMode::Song);
    engLoop.setPosition(org);
    engLoop.play();
    runEngine(engLoop, 2 * kBarSamples + 1000, 256);
    expect(engLoop.playing() && engLoop.position() >= static_cast<double>(org) && engLoop.position() < static_cast<double>(org + bar),
           "K-415: Clips Arrangement wraps back to arrangement start (never silence, never clip grid)");

    AudioEngine engStop(kSr);
    engStop.setProgram(prog);
    engStop.setArrangement(org, end, true);
    engStop.setMode(PlayMode::Song);
    engStop.setPosition(org);
    engStop.play();
    runEngine(engStop, 2 * kBarSamples + 1000, 256);
    expect(!engStop.playing() && near(engStop.position(), static_cast<double>(org), 1.0),
           "K-415: Clips Arrangement with stopAtEnd=true stops cleanly at arrangement end");
  }

  // 7. Setting default & round-trip (`transport.stopAtEnd`).
  {
    Settings st;
    expect(!st.getBool("transport.stopAtEnd"), "K-415: transport.stopAtEnd defaults to false (loop at end)");
    st.set("transport.stopAtEnd", true);
    const std::string p = (fs::temp_directory_path() / "k415_settings.json").string();
    expect(st.save(p), "K-415: settings saved");
    Settings st2;
    expect(st2.load(p) && st2.getBool("transport.stopAtEnd"), "K-415: transport.stopAtEnd persists");
    std::error_code ec;
    fs::remove(p, ec);
  }
}

// ========================================================================= K-409
void testK409TempoTypeIn() {
  Document d(makeEmptySong());
  const auto desc = describeParam(d.song(), songParam(SongParam::Tempo));
  expect(desc.has_value(), "K-409: Tempo ParamDesc exists");

  double out = 0;
  expect(paramtext::parse(*desc, "128", out) && near(out, 128.0), "K-409: parses whole BPM");
  expect(paramtext::parse(*desc, "128.75", out) && near(out, 128.75), "K-409: parses decimal BPM");
  expect(paramtext::parse(*desc, "96.125 BPM", out) && near(out, 96.125), "K-409: parses 3-decimal BPM with unit");
  expect(paramtext::parse(*desc, "2", out) && near(out, 10.0), "K-409: clamps low BPM to 10");
  expect(paramtext::parse(*desc, "999", out) && near(out, 522.0), "K-409: clamps high BPM to 522");
  expect(!paramtext::parse(*desc, "abc", out), "K-409: rejects non-numeric text");
  expect(paramtext::initial(*desc, 140.0) == "140", "K-409: formats whole BPM cleanly");
  expect(paramtext::initial(*desc, 128.5) == "128.5", "K-409: formats 1-decimal BPM cleanly");
  expect(paramtext::initial(*desc, 128.125) == "128.125", "K-409: preserves 3-decimal BPM in initial text");

  // One undo step + collab sync.
  sim::Clock clock;
  sim::Net net(80);
  sim::Peer a, b;
  a.label = "Alex";
  b.label = "Sam";
  a.id = collab::crypto::Identity::generate();
  b.id = collab::crypto::Identity::generate();
  a.doc.reset(makeEmptySong());
  a.folder = sim::tmpDir("k079_tempo_a");
  b.folder = sim::tmpDir("k079_tempo_b");
  std::string err;
  a.s = collab::Session::create(a.doc, sim::config(a.folder, a.id, a.label, &clock), &err);
  net.add(&a);
  b.s = collab::Session::join(b.doc, sim::config(b.folder, b.id, b.label, &clock), sim::ticketOf(*a.s), &err);
  a.s->admitMember(sim::memberOf(b));
  net.add(&b);
  net.connect(&a, &b);
  sim::settle(net, {&a, &b}, clock);
  const double before = a.doc.song().tempo;
  const size_t u0 = a.doc.history().position();
  a.doc.endGesture();
  a.doc.setParam(songParam(SongParam::Tempo), 131.375, false);
  a.doc.endGesture();
  expect(a.doc.history().position() == u0 + 1 && near(a.doc.song().tempo, 131.375), "K-409: typed tempo is one undo step");
  sim::settle(net, {&a, &b}, clock);
  expect(near(b.doc.song().tempo, 131.375), "K-409: typed tempo syncs across collab peers");
  expect(a.doc.undo() && near(a.doc.song().tempo, before), "K-409: single undo restores previous tempo");
}

}  // namespace

int main() {
  testK395ClipsTrackDeletion();
  testK412RemoteCurves();
  testK415SongModeEnd();
  testK409TempoTypeIn();
  return finish();
}
