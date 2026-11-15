// Gridline: K-277, an idle project costs close to nothing.
//  * Rest silent instruments: in realtime playback an instrument that is silent with nothing to
//    play is left out of the blocks (counted), wakes in the block its note arrives in and plays it
//    on the same sample, and the output equals the output with resting off (the demo song, and a
//    song with long gaps between its notes): every sample within -120 dB;
//  * a muted channel rests while its notes go on, a kept-running channel and an instrument with its
//    editor open never rest, an instrument that sounds on its own is not eligible;
//  * the realtime graph wakes the worker threads only when the block has several heavy tasks;
//  * idle frame pacing: full rate while something happens, a few frames a second once nothing has;
//  * with MIDI Out routing (one and four threads, 64 and 512 frame blocks): a listener rests and
//    wakes on its routed note's own sample, routed CC / pitch bend / program change reach it at
//    rest (muted too), a note the sustain pedal holds keeps it awake, the envelope and keyboard
//    controllers fed by MIDI Out never rest, and the output equals the output with resting off.
// Everything is synthetic, and it counts work, never time.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "../src/app/IdlePace.h"
#include "../src/core/Demo.h"
#include "../src/core/Document.h"
#include "../src/core/PatternAutomation.h"
#include "../src/core/PluginInfo.h"
#include "../src/core/PluginParams.h"
#include "../src/core/Song.h"
#include "../src/engine/AudioEngine.h"
#include "../src/engine/Program.h"
#include "Check.h"

using namespace gl;

namespace {

constexpr int kSr = 44100;
constexpr int kHost = 512;  // the host's block (realtime playback; the engine's own grid is finer)

struct Run {
  std::vector<float> L, R;
  std::shared_ptr<ChannelRuntime> rt;  // the watched channel's runtime
  uint64_t serial = 0, parallel = 0;
};

// Plays `s` in song mode as realtime playback does (not offline: resting applies), `frames` long.
Run play(const Song& s, int frames, bool rest, ObjectId watch = kNoId, int threads = 1, int hostBlock = kHost) {
  ProgramBuilder b;
  b.setSampleRate(kSr);
  AudioEngine e(kSr);
  e.setThreads(threads);
  e.setRestInstruments(rest);
  e.setProgram(b.build(s));
  e.setLooping(false);
  e.setMode(PlayMode::Song);
  e.play();
  Run r;
  r.L.assign(static_cast<size_t>(frames), 0.f);
  r.R.assign(static_cast<size_t>(frames), 0.f);
  for (int at = 0; at < frames; at += hostBlock) {
    const int n = std::min(hostBlock, frames - at);
    e.process(r.L.data() + at, r.R.data() + at, n);
  }
  if (watch != kNoId) r.rt = b.channelRuntime(watch);
  r.serial = e.serialBlocks();
  r.parallel = e.parallelBlocks();
  return r;
}

double worstDiff(const Run& a, const Run& b) {
  double w = 0;
  for (size_t i = 0; i < a.L.size(); ++i) {
    w = std::max(w, std::fabs(static_cast<double>(a.L[i]) - b.L[i]));
    w = std::max(w, std::fabs(static_cast<double>(a.R[i]) - b.R[i]));
  }
  return w;
}
double peak(const Run& a) {
  double p = 0;
  for (size_t i = 0; i < a.L.size(); ++i) p = std::max(p, std::max(std::fabs(static_cast<double>(a.L[i])), std::fabs(static_cast<double>(a.R[i]))));
  return p;
}
double secs(const Song& s, Tick t) { return static_cast<double>(t) / s.ppq * 60.0 / s.tempo; }
int firstSound(const Run& a, int from) {
  for (size_t i = static_cast<size_t>(from); i < a.L.size(); ++i)
    if (a.L[i] != 0.f || a.R[i] != 0.f) return static_cast<int>(i);
  return -1;
}

// A song with one channel of `type` playing a short note at each of `at` (ticks), nothing else.
Song sparseSong(const std::string& type, const std::vector<Tick>& at, ObjectId& chId) {
  Song s = makeEmptySong();
  Channel c;
  c.id = s.allocateId();
  c.name = "Lead";
  c.plugin = defaultPluginState(type);
  c.mixerTrack = s.mixer.size() > 1 ? s.mixer[1].id : s.masterId();
  s.channels.push_back(c);
  chId = c.id;
  Pattern& p = s.patterns[0];
  p.notes[c.id] = {Note{0, s.ppq / 2, 64, 100}};
  int k = 0;
  for (Tick t : at) {
    Clip cl;
    cl.id = s.allocateId();
    cl.pattern = p.id;
    cl.track = k++ % 4;
    cl.start = t;
    cl.length = s.barTicks();
    s.playlist.push_back(cl);
  }
  return s;
}

// The demo song, and gaps: every built-in instrument rests between its notes, and the song sounds
// the same (within -120 dB) as with resting off.
void restKeepsTheOutput() {
  const Song demo = makeDemoTrack();
  const int frames = 24 * kSr;
  ObjectId watch = demo.channels.empty() ? kNoId : demo.channels.back().id;
  const Run off = play(demo, frames, false, watch), on = play(demo, frames, true, watch);
  expect(peak(off) > 0.05, "the demo song sounds");
  const double w = worstDiff(off, on);
  std::printf("demo: worst difference %.3g (%.1f dB)\n", w, w > 0 ? 20 * std::log10(w) : -999.0);
  expect(w <= 1e-6, "the demo song with resting equals it without (within -120 dB)");
  uint64_t rested = 0;
  {  // the demo's channels rested somewhere (its arrangement has gaps)
    ProgramBuilder b;
    b.setSampleRate(kSr);
    AudioEngine e(kSr);
    e.setProgram(b.build(demo));
    e.setLooping(false);
    e.setMode(PlayMode::Song);
    e.play();
    std::vector<float> L(kHost), R(kHost);
    for (int at = 0; at < frames; at += kHost) e.process(L.data(), R.data(), kHost);
    for (const Channel& c : demo.channels)
      if (auto rt = b.channelRuntime(c.id)) rested += rt->restBlocks.load();
    std::printf("demo: %llu channel blocks left out in %d s\n", static_cast<unsigned long long>(rested), frames / kSr);
  }
  expect(rested > 0, "the demo's instruments rest in its gaps");

  for (const char* type : {"gl.synth", "gl.analog", "gl.fm", "gl.kick", "gl.keys"}) {
    ObjectId id = kNoId;
    Song s = makeEmptySong();
    const Tick bar = s.barTicks();
    // Notes 3 bars apart, the last one on an odd tick (not on a block or a control block edge).
    s = sparseSong(type, {0, 3 * bar, 6 * bar + 7}, id);
    const int len = static_cast<int>(secs(s, 8 * bar) * kSr);
    const Run a = play(s, len, false, id), b = play(s, len, true, id);
    const std::string t = type;
    expect(peak(a) > 1e-3, (t + ": sounds").c_str());
    expect(worstDiff(a, b) <= 1e-6, (t + ": resting changes nothing (within -120 dB)").c_str());
    expect(b.rt && b.rt->restBlocks.load() > 0, (t + ": rests between its notes").c_str());
    expect(a.rt && a.rt->restBlocks.load() == 0, (t + ": resting off: always runs").c_str());
    // Sample accurate: the note after a rest starts on the same sample.
    const int from = static_cast<int>(secs(s, 6 * bar) * kSr);
    expect(firstSound(a, from) > 0 && firstSound(a, from) == firstSound(b, from), (t + ": wakes on the note's own sample").c_str());
  }
}

// Counters: a silent channel is left out, block after block, and a note wakes it.
void countersAndWake() {
  Song s = makeEmptySong();
  Channel c;
  c.id = s.allocateId();
  c.plugin = defaultPluginState("gl.synth");
  c.mixerTrack = s.masterId();
  s.channels.push_back(c);
  ProgramBuilder b;
  b.setSampleRate(kSr);
  AudioEngine e(kSr);
  e.setProgram(b.build(s));
  std::vector<float> L(kHost), R(kHost);
  for (int i = 0; i < 60; ++i) e.process(L.data(), R.data(), kHost);  // stopped, nothing plays (over half a second)
  auto rt = b.channelRuntime(c.id);
  expect(rt && rt->resting && rt->restBlocks.load() > 0, "a silent instrument rests while stopped");
  const uint64_t ran = rt ? rt->runBlocks.load() : 0;
  for (int i = 0; i < 50; ++i) e.process(L.data(), R.data(), kHost);
  expect(rt && rt->runBlocks.load() == ran, "... and stays out of every block");
  e.noteOn(c.id, 60, 0.8f);
  e.process(L.data(), R.data(), kHost);
  float pk = 0;
  for (int i = 0; i < kHost; ++i) pk = std::max(pk, std::fabs(L[static_cast<size_t>(i)]));
  expect(rt && !rt->resting && rt->runBlocks.load() == ran + 1, "a note wakes it in the block it comes in");
  expect(pk > 1e-4f, "... and it sounds in that block");
  e.noteOff(c.id, 60);
  for (int i = 0; i < 200; ++i) e.process(L.data(), R.data(), kHost);
  expect(rt && rt->resting, "after its tail it rests again");

  // Its editor open: never rests.
  rt->editorOpen.store(true);
  for (int i = 0; i < 10; ++i) e.process(L.data(), R.data(), kHost);
  expect(!rt->resting, "an instrument with its editor open runs");
  rt->editorOpen.store(false);

  // Resting off: runs every block.
  e.setRestInstruments(false);
  const uint64_t r0 = rt->runBlocks.load(), q0 = rt->restBlocks.load();
  for (int i = 0; i < 10; ++i) e.process(L.data(), R.data(), kHost);
  expect(rt->restBlocks.load() == q0 && !rt->resting, "Rest silent instruments off: nothing rests");
  (void)r0;
  e.setRestInstruments(true);

  // Kept running, an instrument that sounds on its own, and an offline render: not eligible.
  Song k = s;
  k.channels[0].keepRunning = true;
  Channel cv;
  cv.id = k.allocateId();
  cv.plugin = defaultPluginState("gl.cvin");
  k.channels.push_back(cv);
  ProgramBuilder bk;
  bk.setSampleRate(kSr);
  auto pk2 = bk.build(k);
  expect(!pk2->channels[0].restSilent, "a channel kept running never rests");
  expect(pk2->channels.size() > 1 && !pk2->channels[1].restSilent, "an instrument that sounds without notes never rests");
  auto ps = b.build(s);
  expect(ps->channels[0].restSilent, "an instrument channel may rest");
  AudioEngine eo(kSr);
  ProgramBuilder bo;
  bo.setSampleRate(kSr);
  eo.setOffline(true);
  eo.setProgram(bo.build(s));
  for (int i = 0; i < 50; ++i) eo.process(L.data(), R.data(), kHost);
  auto ort = bo.channelRuntime(c.id);
  expect(ort && ort->restBlocks.load() == 0, "offline renders run every instrument");
}

// A muted channel rests while its song notes go on; unmuted it plays its next note.
void mutedRests() {
  ObjectId id = kNoId;
  Song s = makeEmptySong();
  const Tick bar = s.barTicks();
  std::vector<Tick> at;
  for (int i = 0; i < 8; ++i) at.push_back(i * bar / 2);  // a note every half bar
  s = sparseSong("gl.synth", at, id);
  s.channel(id)->muted = true;
  const int len = static_cast<int>(secs(s, 4 * bar) * kSr);
  const Run m = play(s, len, true, id);
  expect(peak(m) == 0, "a muted channel is silent");
  expect(m.rt && m.rt->restBlocks.load() > m.rt->runBlocks.load(), "a muted channel rests while its notes go on");
}

// The realtime graph: a silent project, or light work, runs on the audio thread alone; an offline
// render shares every block out as before. The output is the same.
struct Shared {
  uint64_t parallel = 0, serial = 0;
};
// `s` stopped on 4 threads: the blocks shared out and run on one thread after its first second.
Shared stoppedOnFour(const Song& s) {
  ProgramBuilder b;
  b.setSampleRate(kSr);
  AudioEngine e(kSr);
  e.setThreads(4);
  e.setProgram(b.build(s));
  e.setMode(PlayMode::Song);
  std::vector<float> L(kHost), R(kHost);
  for (int at = 0; at < kSr; at += kHost) e.process(L.data(), R.data(), kHost);
  const uint64_t p0 = e.parallelBlocks(), s0 = e.serialBlocks();
  for (int at = 0; at < 3 * kSr; at += kHost) e.process(L.data(), R.data(), kHost);
  return {e.parallelBlocks() - p0, e.serialBlocks() - s0};
}

void threadsOnlyWhenWorthIt() {
  Song fresh = makeEmptySong();  // a new project's instruments
  for (const char* type : {"gl.kick", "gl.clap", "gl.hat", "gl.snare", "gl.synth"}) {
    Channel c;
    c.id = fresh.allocateId();
    c.plugin = defaultPluginState(type);
    c.mixerTrack = fresh.masterId();
    fresh.channels.push_back(c);
  }
  const Shared a = stoppedOnFour(fresh);
  std::printf("new project stopped on 4 threads: %llu blocks shared out, %llu on one thread\n",
              static_cast<unsigned long long>(a.parallel), static_cast<unsigned long long>(a.serial));
  expect(a.parallel == 0 && a.serial > 0, "a silent new project wakes no worker thread");
  const Song demo = makeDemoTrack();
  const Shared d = stoppedOnFour(demo);  // (its reverbs keep running: the machine's speed decides)
  std::printf("demo stopped on 4 threads: %llu blocks shared out, %llu on one thread\n",
              static_cast<unsigned long long>(d.parallel), static_cast<unsigned long long>(d.serial));
  const int frames = 4 * kSr;
  const Run one = play(demo, frames, true, kNoId, 1), four = play(demo, frames, true, kNoId, 4);
  expect(worstDiff(one, four) == 0, "the demo plays the same on one thread and on four");
}

void idlePacing() {
  idlepace::IdlePace p;
  p.active(0.0);
  int frames = 0;
  double t = 0;
  for (; t < 0.5; t += 1.0 / 60) {  // something happens: every refresh is a frame
    if (p.waitLeft(t) <= 0) ++frames, p.framed(t);
  }
  expect(frames >= 29, "while active the interface draws every refresh");
  frames = 0;
  for (t = 2.0; t < 4.0; t += 1.0 / 60)  // nothing for a while: about 10 frames a second
    if (p.waitLeft(t) <= 0) ++frames, p.framed(t);
  expect(frames >= 15 && frames <= 22, "idle, it draws about ten frames a second");
  p.active(4.0);
  expect(p.waitLeft(4.01) == 0, "input brings the full rate back at once");
  p.setEnabled(false);
  expect(p.waitLeft(100.0) == 0, "pacing off: every refresh");
}

// ---------------------------------------------------------------- MIDI Out routing

struct DocRun {
  std::vector<float> L, R;
  std::vector<std::shared_ptr<ChannelRuntime>> rt;  // the watched channels' runtimes
  uint64_t parallel = 0;
};
using Hook = std::function<void(AudioEngine&, ProgramBuilder&, long)>;

// Plays pattern `pat` of `s` as realtime playback does (resting applies), in blocks of `block`.
// `hook` runs before each block (live input, a program rebuilt with changed settings).
DocRun playPat(const Song& s, ObjectId pat, int frames, int block, bool rest, int threads, const std::vector<ObjectId>& watch,
               const Hook& hook = {}) {
  ProgramBuilder b;
  b.setSampleRate(kSr);
  AudioEngine e(kSr);
  e.setThreads(threads);
  e.setRestInstruments(rest);
  e.setSustainHolds(true);
  e.setProgram(b.build(s));
  e.setMode(PlayMode::Pattern);
  e.setPattern(pat);
  e.setLooping(false);
  e.play();
  DocRun r;
  r.L.assign(static_cast<size_t>(frames), 0.f);
  r.R.assign(static_cast<size_t>(frames), 0.f);
  for (long at = 0; at < frames; at += block) {
    if (hook) hook(e, b, at);
    const int n = std::min(block, frames - static_cast<int>(at));
    e.process(r.L.data() + at, r.R.data() + at, n);
  }
  for (ObjectId w : watch) r.rt.push_back(b.channelRuntime(w));
  r.parallel = e.parallelBlocks();
  return r;
}
double worstDiff(const DocRun& a, const DocRun& b) {
  double w = 0;
  for (size_t i = 0; i < a.L.size(); ++i)
    w = std::max({w, std::fabs(static_cast<double>(a.L[i]) - b.L[i]), std::fabs(static_cast<double>(a.R[i]) - b.R[i])});
  return w;
}
double peakOf(const DocRun& a, size_t from = 0, size_t to = ~size_t(0)) {
  double p = 0;
  for (size_t i = from; i < std::min(to, a.L.size()); ++i)
    p = std::max({p, std::fabs(static_cast<double>(a.L[i])), std::fabs(static_cast<double>(a.R[i]))});
  return p;
}
int firstSound(const DocRun& a, int from) {
  for (size_t i = static_cast<size_t>(from); i < a.L.size(); ++i)
    if (a.L[i] != 0.f || a.R[i] != 0.f) return static_cast<int>(i);
  return -1;
}
void listenOn(Document& d, ObjectId ch, int port) {
  d.editChannel(ch, "port", [port](Channel& c) { c.midiInPort = port; });
}
// A Hold-shaped lane on `id` in pattern `pat`: (tick, value 0..1) points.
void holdLane(Document& d, ObjectId pat, ParamId id, const std::vector<std::pair<Tick, float>>& pts) {
  d.editPattern(pat, "lane", [&](Pattern& p) {
    std::vector<AutoPoint> v;
    for (const auto& [t, x] : pts) {
      AutoPoint a;
      a.pos = t, a.value = x, a.shape = AutoShape::Hold;
      v.push_back(a);
    }
    setPatternLane(p, id.owner, id.index, v);
  });
}
std::string tag(const std::string& what, int threads, int block) {
  return what + " (" + std::to_string(threads) + " thread" + (threads > 1 ? "s" : "") + ", " + std::to_string(block) + " frames)";
}

// 1. Listeners on a MIDI Out channel's port rest between the routed notes and wake on the sample
// the note would start on if it were sequenced on them. Both wake in the same block, so on four
// threads that block is shared out (the MIDI Out channel has delivered before any listener decides
// to rest or run).
void midiOutWake() {
  Document ref(makeEmptySong());
  const Tick bar = ref.song().barTicks();
  const std::vector<Tick> at = {0, 3 * bar, 6 * bar + 7};
  const ObjectId r1 = ref.addChannel("A", "gl.synth"), r2 = ref.addChannel("B", "gl.synth");
  const ObjectId rp = ref.song().patterns[0].id;
  Document d(makeEmptySong());
  const ObjectId out = d.addChannel("out", "gl.midiout");
  const ObjectId a = d.addChannel("A", "gl.synth"), b2 = d.addChannel("B", "gl.synth");
  listenOn(d, a, 0), listenOn(d, b2, 0);
  const ObjectId pat = d.song().patterns[0].id;
  for (Tick t : at) {
    for (ObjectId r : {r1, r2}) ref.addNote(rp, r, {t, 48, 60, 100}), ref.addNote(rp, r, {t, 48, 67, 100});  // (both hear both)
    d.addNote(pat, out, {t, 48, 60, 100}), d.addNote(pat, out, {t, 48, 67, 100});
  }
  const int frames = static_cast<int>(secs(d.song(), 7 * bar) * kSr);
  const int from = static_cast<int>(secs(d.song(), 6 * bar) * kSr);
  for (int threads : {1, 4})
    for (int block : {64, 512}) {
      const DocRun direct = playPat(ref.song(), rp, frames, block, true, threads, {r1});
      const DocRun on = playPat(d.song(), pat, frames, block, true, threads, {a, b2});
      const DocRun off = playPat(d.song(), pat, frames, block, false, threads, {a, b2});
      expect(peakOf(on) > 1e-3, tag("routed: the listeners sound", threads, block).c_str());
      expect(worstDiff(on, off) <= 1e-6, tag("routed: resting changes nothing", threads, block).c_str());
      expect(on.rt[0] && on.rt[0]->restBlocks.load() > 0 && on.rt[1] && on.rt[1]->restBlocks.load() > 0,
             tag("routed: the listeners rest between their notes", threads, block).c_str());
      expect(firstSound(on, from) > 0 && firstSound(on, from) == firstSound(direct, from),
             tag("routed: a resting listener wakes on the note's own sample", threads, block).c_str());
      std::printf("routed %d threads %d frames: wake at %d (sequenced %d), worst difference to sequenced %.3g\n", threads, block,
                  firstSound(on, from), firstSound(direct, from), worstDiff(on, direct));
      expect(worstDiff(on, direct) <= 1e-5 && worstDiff(off, direct) <= 1e-5,
             tag("routed: the listeners sound as the notes sequenced on them (-100 dB), rest on and off", threads, block).c_str());
      if (threads > 1) expect(on.parallel > 0, tag("routed: blocks where both listeners wake are shared out", threads, block).c_str());
    }
}

// 1b. What MIDI carries arrives as if the notes were sequenced on the listener: every velocity
// (1..127), fine pitch (also past half a semitone), and the song's master pitch once (not twice),
// on instruments of several kinds.
// Per-note pan and mod X / Y have no MIDI message: they are left out here.
void midiOutAsSequenced() {
  for (const char* type : {"gl.synth", "gl.pocketsynth", "gl.fm", "gl.drumkit"}) {
    if (!findPlugin(type)) continue;
    for (float master : {0.0f, 2.0f, -0.5f}) {
      Document ref(makeEmptySong()), d(makeEmptySong());
      const Tick beat = ref.song().ppq;
      const ObjectId r = ref.addChannel("L", type), rp = ref.song().patterns[0].id;
      const ObjectId out = d.addChannel("out", "gl.midiout"), l = d.addChannel("L", type), pat = d.song().patterns[0].id;
      listenOn(d, l, 0);
      const int vels[] = {1, 13, 37, 64, 90, 100, 113, 127};
      Tick t = 0;
      for (int v : vels) {
        Note n{t, beat / 2, 48 + (v % 24), v};
        ref.addNote(rp, r, n), d.addNote(pat, out, n);
        t += beat;
      }
      for (int fine : {30, 70, -45}) {
        Note n{t, beat / 2, 60, 100};
        n.fine = fine;
        ref.addNote(rp, r, n), d.addNote(pat, out, n);
        t += beat;
      }
      Song a = ref.song(), b = d.song();
      a.masterPitch = b.masterPitch = master;
      const int frames = static_cast<int>(secs(a, t + beat) * kSr);
      const DocRun direct = playPat(a, rp, frames, kHost, true, 1, {});
      const DocRun routed = playPat(b, pat, frames, kHost, true, 1, {});
      const double w = worstDiff(direct, routed);
      std::printf("as sequenced: %s, master pitch %+.1f: peak %.3f, worst difference %.3g\n", type, static_cast<double>(master), peakOf(direct), w);
      expect(peakOf(direct) > 1e-3 && w <= 1e-5,
             (std::string("routed notes sound as sequenced (-100 dB): ") + type + ", master pitch " + std::to_string(master)).c_str());
    }
  }
}

// 2. CC and pitch bend routed while the listeners rest reach them (they wake), and the next note
// sounds as with resting off; also while they are muted (they rest muted, are unmuted before the
// note). A program change routed to a resting listener wakes it in its block.
void midiOutControllersAtRest() {
  for (bool muted : {false, true}) {
    // Mod wheel (CC 1: a knob of the MIDI Out channel, automated in the gap) to a Pocket Synth
    // with its wheel on the cutoff; pitch bend from the keyboard on the MIDI Out channel to a
    // Hybrid Synth. Notes at 0 and at 4 bars; the controllers arrive at 2 bars.
    Document d(makeEmptySong());
    const Tick bar = d.song().barTicks();
    const ObjectId out = d.addChannel("out", "gl.midiout");
    const ObjectId pk = d.addChannel("pocket", "gl.pocketsynth"), gm = d.addChannel("hybrid", "gl.gms");
    listenOn(d, pk, 0), listenOn(d, gm, 0);
    const ObjectId pat = d.song().patterns[0].id;
    d.addNote(pat, out, {0, 48, 60, 100});
    d.addNote(pat, out, {4 * bar, 96, 60, 100});
    d.setParam(channelPluginParam(pk, 3), 0, false);  // wheel to the cutoff
    d.setParam(channelPluginParam(pk, 4), 1, false);  // all the way
    d.setParam(channelPluginParam(out, pp::moKnob(0, pp::MoKnobNumber)), 1, false);
    const Song plain = d.song();  // (no controllers)
    holdLane(d, pat, channelPluginParam(out, pp::moKnob(0, pp::MoKnobValue)), {{0, 0.f}, {2 * bar, 1.f}});
    Song start = d.song();
    if (muted)
      for (ObjectId c : {pk, gm}) start.channel(c)->muted = true;
    d.setParam(channelPluginParam(out, pp::MoProgram), 5, false);
    const Song later = d.song();  // unmuted, program 5
    const int frames = static_cast<int>(secs(d.song(), 5 * bar) * kSr);
    const long ctlAt = static_cast<long>(secs(d.song(), 2 * bar) * kSr), unmuteAt = static_cast<long>(secs(d.song(), 3 * bar) * kSr);
    for (int threads : {1, 4})
      for (int block : {64, 512}) {
        uint64_t ranBefore[2] = {0, 0}, ranAfter[2] = {0, 0};
        bool restingBefore[2] = {false, false};
        Hook hook = [&](AudioEngine& e, ProgramBuilder& b, long pos) {
          auto p = b.channelRuntime(pk), g = b.channelRuntime(gm);
          if (!p || !g) return;
          if (pos <= ctlAt && pos + block > ctlAt) {
            e.setMidiTarget(out);
            e.midiEvent(0xE0, 0, 127);  // bend up
            restingBefore[0] = p->resting, restingBefore[1] = g->resting;
            ranBefore[0] = p->runBlocks.load(), ranBefore[1] = g->runBlocks.load();
          } else if (pos > ctlAt && ranAfter[0] == 0) {
            ranAfter[0] = p->runBlocks.load(), ranAfter[1] = g->runBlocks.load();
          }
          if (pos <= unmuteAt && pos + block > unmuteAt) e.setProgram(b.build(later));
        };
        const DocRun on = playPat(start, pat, frames, block, true, threads, {pk, gm}, hook);
        const bool rested = restingBefore[0] && restingBefore[1];
        const bool woke = ranAfter[0] > ranBefore[0] && ranAfter[1] > ranBefore[1];
        const DocRun off = playPat(start, pat, frames, block, false, threads, {pk, gm}, hook);
        const DocRun none = playPat(plain, pat, frames, block, false, threads, {pk, gm});
        const std::string m = muted ? "muted listeners: " : "listeners: ";
        expect(rested, tag(m + "both rest before the controllers arrive", threads, block).c_str());
        expect(woke, tag(m + "routed CC and pitch bend reach them at rest (they run)", threads, block).c_str());
        const size_t late = static_cast<size_t>(secs(d.song(), 4 * bar) * kSr);
        expect(peakOf(on, late) > 1e-3 && worstDiff(on, none) > 1e-3, tag(m + "the controllers change the next note", threads, block).c_str());
        const double w = worstDiff(on, off);
        // A muted channel at rest ends its voices and effect tails (it starts clean when heard
        // again): the same project without the controllers shows how much that alone changes.
        double base = 0;
        if (muted) {
          Song plainMuted = plain;
          for (ObjectId c : {pk, gm}) plainMuted.channel(c)->muted = true;
          Hook unmute = [&](AudioEngine& e, ProgramBuilder& b, long pos) {
            if (pos <= unmuteAt && pos + block > unmuteAt) e.setProgram(b.build(plain));
          };
          base = worstDiff(playPat(plainMuted, pat, frames, block, true, threads, {}, unmute),
                           playPat(plainMuted, pat, frames, block, false, threads, {}, unmute));
        }
        std::printf("%s%d threads %d frames: worst difference rest on / off %.3g (without the controllers %.3g)\n", m.c_str(), threads,
                    block, w, base);
        // (where the clean start falls moves with the block size: within twice that, and -60 dB;
        // a lost controller differs by about -7 dB)
        expect(w <= 1e-6 || (muted && w <= 2 * base + 1e-6 && w < 1e-3), tag(m + "resting loses none of the controllers", threads, block).c_str());
      }
  }
  {
    Document d(makeEmptySong());
    const ObjectId out = d.addChannel("out", "gl.midiout"), syn = d.addChannel("syn", "gl.synth");
    listenOn(d, syn, 0);
    ProgramBuilder b;
    b.setSampleRate(kSr);
    AudioEngine e(kSr);
    e.setRestInstruments(true);
    e.setProgram(b.build(d.song()));
    std::vector<float> L(kHost), R(kHost);
    for (int i = 0; i < 80; ++i) e.process(L.data(), R.data(), kHost);
    auto rt = b.channelRuntime(syn);
    const bool rested = rt && rt->resting;
    const uint64_t ran = rt ? rt->runBlocks.load() : 0;
    d.setParam(channelPluginParam(out, pp::MoProgram), 7, false);
    e.setProgram(b.build(d.song()));
    e.process(L.data(), R.data(), kHost);
    expect(rested && rt->runBlocks.load() == ran + 1, "a program change routed to a resting listener wakes it in its block");
  }
}

// 3. A note the sustain pedal holds (CC 64 from the MIDI Out channel, Sustain holds notes on) keeps
// the listener awake until the pedal lifts and the tail ends; it rests after that.
void midiOutPedal() {
  Document d(makeEmptySong());
  const Tick bar = d.song().barTicks();
  const ObjectId out = d.addChannel("out", "gl.midiout"), syn = d.addChannel("syn", "gl.synth");
  listenOn(d, syn, 0);
  const ObjectId pat = d.song().patterns[0].id;
  d.addNote(pat, out, {4, 24, 60, 100});  // a short key press under the pedal
  d.addNote(pat, out, {5 * bar, 24, 64, 100});
  d.setParam(channelPluginParam(out, pp::moKnob(0, pp::MoKnobNumber)), 64, false);
  holdLane(d, pat, channelPluginParam(out, pp::moKnob(0, pp::MoKnobValue)), {{0, 0.f}, {2, 1.f}, {2 * bar, 0.f}});
  const int frames = static_cast<int>(secs(d.song(), 6 * bar) * kSr);
  const long lift = static_cast<long>(secs(d.song(), 2 * bar) * kSr);
  const long gap = static_cast<long>(secs(d.song(), 5 * bar) * kSr);
  for (int threads : {1, 4})
    for (int block : {64, 512}) {
      uint64_t restedBeforeLift = ~uint64_t{0}, restedInGap = 0;
      Hook hook = [&](AudioEngine&, ProgramBuilder& b, long pos) {
        auto rt = b.channelRuntime(syn);
        if (!rt) return;
        if (pos <= lift && pos + block > lift) restedBeforeLift = rt->restBlocks.load();
        if (pos <= gap && pos + block > gap) restedInGap = rt->restBlocks.load();
      };
      const DocRun on = playPat(d.song(), pat, frames, block, true, threads, {syn}, hook);
      const DocRun off = playPat(d.song(), pat, frames, block, false, threads, {syn});
      const size_t held = static_cast<size_t>(secs(d.song(), bar + bar / 2) * kSr);
      expect(peakOf(on, held, held + 4096) > 1e-2, tag("pedal: the held note sounds a bar after its key lifted", threads, block).c_str());
      expect(restedBeforeLift == 0, tag("pedal: the listener never rests while the pedal holds its note", threads, block).c_str());
      expect(restedInGap > 0, tag("pedal: after the pedal lifts and the tail ends it rests", threads, block).c_str());
      expect(worstDiff(on, off) <= 1e-6, tag("pedal: resting changes nothing", threads, block).c_str());
    }
}

// 4. The envelope and keyboard controllers fed by MIDI Out never rest, and follow its notes.
void midiOutControllers() {
  for (const char* type : {"gl.envctl", "gl.keyctl"}) {
    const std::string t = type;
    expect(findPlugin(type) != nullptr, (t + ": exists").c_str());
    Document d(makeEmptySong());
    const ObjectId out = d.addChannel("out", "gl.midiout"), ctl = d.addChannel("ctl", type);
    listenOn(d, ctl, 0);
    const ObjectId pat = d.song().patterns[0].id;
    const Tick at = 2 * d.song().barTicks();
    d.addNote(pat, out, {at, 384, 96, 127});
    ProgramBuilder b;
    b.setSampleRate(kSr);
    auto p = b.build(d.song());
    expect(p->channels.size() > 1 && !p->channels[1].restSilent, (t + ": a controller never rests").c_str());
    for (int threads : {1, 4}) {
      ProgramBuilder b2;
      b2.setSampleRate(kSr);
      AudioEngine e(kSr);
      e.setThreads(threads);
      e.setRestInstruments(true);
      e.setProgram(b2.build(d.song()));
      e.setMode(PlayMode::Pattern);
      e.setPattern(pat);
      e.setLooping(false);
      e.play();
      std::vector<float> L(kHost), R(kHost);
      auto rt = b2.channelRuntime(ctl);
      float before = 0, after = 0;
      const int noteAt = static_cast<int>(secs(d.song(), at) * kSr);
      for (int pos = 0; pos < noteAt + kSr / 2; pos += kHost) {
        e.process(L.data(), R.data(), kHost);
        float& v = pos + kHost <= noteAt ? before : after;
        v = std::max(v, rt->ctlUi[0].load());
      }
      std::printf("%s on %d threads: output %.3f before the note, %.3f after\n", type, threads, before, after);
      expect(rt->restBlocks.load() == 0, (t + ": fed by MIDI Out, it runs every block").c_str());
      expect(after > before + 0.05f, (t + ": the routed note moves its output").c_str());
    }
  }
}

}  // namespace

int main() {
  restKeepsTheOutput();
  countersAndWake();
  mutedRests();
  threadsOnlyWhenWorthIt();
  idlePacing();
  midiOutWake();
  midiOutAsSequenced();
  midiOutControllersAtRest();
  midiOutPedal();
  midiOutControllers();
  return finish();
}
