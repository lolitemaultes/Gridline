// Gridline - K-142 (0.7.8), the engine at rest: what an idle project costs the audio thread.
//  * the demo song, stopped: after its tails every instrument and effect rests (no DSP call),
//    including the instruments whose parameters automation moves and the bus effects that listen
//    to a side input;
//  * an instrument whose parameter automation moves rests in the gaps between its notes and wakes
//    on its note's own sample: the output equals the output with resting off (-120 dB);
//  * a fully resting graph runs no node's per-sample work: its blocks are left out as a whole
//    (counted) and wake on the sample anything arrives (a live note, the transport, an edit);
//  * 0.7.8 follow-up: a graph at rest with nothing to wake it sleeps whole (sleepGraph: one look
//    per block, not every node's) and, stopped, takes the device's block in one piece instead of
//    the control grid's 64-frame steps (counted); live notes and the transport starting wake it on
//    their own sample, as with resting off;
//  * offline renders never rest and are unchanged (their hash is printed for the record);
//  * the audio thread's processor time per second of audio, stopped (empty, demo, 200 channels)
//    and playing the demo, is printed for the record (never judged: it depends on the machine).
// Everything else is counted, never timed.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>
#include <vector>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include "../src/core/Demo.h"
#include "../src/core/PluginInfo.h"
#include "../src/core/PluginParams.h"
#include "../src/core/Params.h"
#include "../src/core/Song.h"
#include "../src/engine/AudioEngine.h"
#include "../src/engine/Program.h"
#include "../src/engine/Render.h"
#include "../src/packs/spacefx/spacefx.h"
#include "Check.h"

using namespace gl;

#define CHECK(x) expect((x), #x)

namespace {

constexpr int kSr = 44100;
constexpr int kHost = 256;  // the app's default device block

double threadSeconds() {
#if defined(_WIN32)
  FILETIME c, e, k, u;
  if (!GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u)) return 0;
  const auto ticks = [](const FILETIME& f) { return (static_cast<uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
  return static_cast<double>(ticks(k) + ticks(u)) * 1e-7;  // (100 ns units)
#else
  timespec ts{};
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
  return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) * 1e-9;
#endif
}

struct Nodes {
  uint64_t calls = 0;  // DSP calls of every instrument and effect
  int running = 0;     // nodes that made at least one
  std::string names;
};

Nodes callsSince(const Program& p, std::vector<uint64_t>& last) {
  Nodes r;
  size_t k = 0;
  auto one = [&](uint64_t now, const std::string& what) {
    if (k >= last.size()) last.push_back(0);
    const uint64_t d = now - last[k];
    last[k++] = now;
    if (d == 0) return;
    r.calls += d;
    ++r.running;
    if (r.names.size() < 400) r.names += what + " ";
  };
  for (const auto& c : p.channels)
    if (c.rt) one(c.rt->dspCalls.load(), c.rt->type);
  for (const auto& m : p.mixer)
    for (const auto& s : m.slots)
      if (s.rt) one(s.rt->dspCalls.load(), s.rt->type);
  return r;
}

// Copies the song's channels (with their notes) until it has `channels` of them, and repeats its
// arrangement until it has `clips` clips (as tools/cpu/cpurun.py's large project).
Song grow(Song s, size_t channels, size_t clips) {
  const std::vector<Channel> base = s.channels;
  size_t k = 0;
  while (s.channels.size() < channels && !base.empty()) {
    const Channel& src = base[k % base.size()];
    Channel c = src;
    c.id = s.allocateId();
    c.name = src.name + " " + std::to_string(k / base.size() + 2);
    for (auto& p : s.patterns)
      if (auto it = p.notes.find(src.id); it != p.notes.end()) p.notes[c.id] = it->second;
    s.channels.push_back(c);
    ++k;
  }
  const std::vector<Clip> pl = s.playlist;
  Tick span = 0;
  for (const Clip& c : pl) span = std::max(span, c.start + c.length);
  for (int rep = 1; s.playlist.size() < clips && !pl.empty(); ++rep)
    for (const Clip& c : pl) {
      if (s.playlist.size() >= clips) break;
      Clip n = c;
      n.id = s.allocateId();
      n.start = c.start + rep * span;
      s.playlist.push_back(n);
    }
  return s;
}

struct Bench {
  double cpuPercent = 0;  // audio thread time per audio time, percent of one core
  Nodes nodes;            // DSP calls while measured
  uint64_t blocks = 0;
  uint64_t hushed = 0;  // mixer track blocks left out whole
  size_t tracks = 0;    // mixer tracks the graph runs
  uint64_t slept = 0, longRest = 0;  // K-142 (follow-up): blocks the whole graph slept / long blocks
};

// Plays (or leaves stopped) `s` as the app's audio callback does, lets it settle for `settle`
// seconds and then measures `seconds`.
Bench bench(const Song& s, bool playing, double settle, double seconds, int threads = 1) {
  ProgramBuilder b;
  b.setSampleRate(kSr);
  AudioEngine e(kSr);
  e.setThreads(threads);
  e.setProgram(b.build(s));
  e.setCallCounting(true);
  e.setMode(PlayMode::Song);
  e.setLooping(true);
  if (playing) e.play();
  std::vector<float> L(kHost), R(kHost);
  const int settleBlocks = static_cast<int>(settle * kSr / kHost);
  for (int i = 0; i < settleBlocks; ++i) e.process(L.data(), R.data(), kHost);
  std::vector<uint64_t> last;
  callsSince(*e.program(), last);
  const uint64_t b0 = e.countedBlocks(), h0 = e.hushedTracks(), s0 = e.sleptBlocks(), l0 = e.longRestBlocks();
  const int blocks = static_cast<int>(seconds * kSr / kHost);
  const double t0 = threadSeconds();
  for (int i = 0; i < blocks; ++i) e.process(L.data(), R.data(), kHost);
  const double t1 = threadSeconds();
  Bench r;
  r.cpuPercent = 100.0 * (t1 - t0) / (static_cast<double>(blocks) * kHost / kSr);
  r.nodes = callsSince(*e.program(), last);
  r.blocks = e.countedBlocks() - b0;
  r.hushed = e.hushedTracks() - h0;
  r.slept = e.sleptBlocks() - s0;
  r.longRest = e.longRestBlocks() - l0;
  for (int t : e.program()->graph.serial) r.tracks += t >= e.program()->graph.channels ? 1 : 0;
  return r;
}

Song emptyOf(const Song& demo) {
  Song s = demo;
  s.channels.clear();
  for (auto& p : s.patterns) p.notes.clear();
  s.playlist.clear();
  s.automations.clear();
  s.links.clear();
  s.modLinks.clear();
  for (auto& m : s.mixer) {
    m.slots = std::vector<EffectSlot>(kEffectSlots);
    m.sends.clear();
  }
  return s;
}

void report() {
  const Song demo = makeDemoTrack();
  const Song empty = emptyOf(demo);
  const Song large = grow(demo, 200, 2000);
  struct Row {
    const char* name;
    const Song* s;
    bool playing;
    int threads;
  } rows[] = {{"empty, stopped", &empty, false, 1}, {"demo, stopped", &demo, false, 1}, {"large, stopped", &large, false, 1},
              {"demo, playing", &demo, true, 1},   {"demo, stopped", &demo, false, 8}, {"large, stopped", &large, false, 8},
              {"demo, playing", &demo, true, 8}};
  for (const Row& r : rows) {
    const Bench b = bench(*r.s, r.playing, r.playing ? 2.0 : 12.0, 10.0, r.threads);
    std::printf("cost: %-16s %d thr %7.3f %% of a core, %llu engine blocks, tracks left out %.0f %%, %d nodes ran DSP (%llu calls) %s\n", r.name,
                r.threads, b.cpuPercent,
                static_cast<unsigned long long>(b.blocks), b.tracks > 0 && b.blocks > 0 ? 100.0 * static_cast<double>(b.hushed) / static_cast<double>(b.tracks * b.blocks) : 0.0,
                b.nodes.running, static_cast<unsigned long long>(b.nodes.calls),
                b.nodes.names.c_str());
  }
}

// ---- checks ----

struct Run {
  std::vector<float> L, R;
  uint64_t restBlocks = 0;  // of the watched channel
  std::vector<bool> restingAt;  // the watched channel resting at each probe time
};

double worstDiff(const Run& a, const Run& b) {
  double w = 0;
  for (size_t i = 0; i < a.L.size() && i < b.L.size(); ++i) {
    w = std::max(w, std::fabs(static_cast<double>(a.L[i]) - b.L[i]));
    w = std::max(w, std::fabs(static_cast<double>(a.R[i]) - b.R[i]));
  }
  return w;
}
double peakOf(const Run& a, size_t from = 0) {
  double p = 0;
  for (size_t i = from; i < a.L.size(); ++i) p = std::max(p, std::max(std::fabs(static_cast<double>(a.L[i])), std::fabs(static_cast<double>(a.R[i]))));
  return p;
}
int firstSound(const Run& a, size_t from) {
  for (size_t i = from; i < a.L.size(); ++i)
    if (a.L[i] != 0.f || a.R[i] != 0.f) return static_cast<int>(i);
  return -1;
}
double dB(double x) { return x > 0 ? 20 * std::log10(x) : -999.0; }

int paramIndex(const std::string& type, const std::string& name) {
  if (const PluginInfo* info = findPlugin(type))
    for (size_t i = 0; i < info->params.size(); ++i)
      if (info->params[i].name == name) return static_cast<int>(i);
  return -1;
}

// One Analog Synth channel playing a short note at bar 0 and at bar 8, its filter cutoff moved by
// an automation clip over the whole song (the gap between the notes too).
Song automatedSong(ObjectId& chId, int& cutoff) {
  Song s = makeEmptySong();
  s.tempo = 140;
  Channel c;
  c.id = s.allocateId();
  c.name = "Lead";
  c.plugin = defaultPluginState("gl.analog");
  c.mixerTrack = s.mixer.size() > 1 ? s.mixer[1].id : s.masterId();
  s.channels.push_back(c);
  chId = c.id;
  Pattern& p = s.patterns[0];
  p.notes[c.id] = {Note{0, s.ppq / 2, 60, 100}};
  for (int bar : {0, 8}) {
    Clip cl;
    cl.id = s.allocateId();
    cl.pattern = p.id;
    cl.track = 0;
    cl.start = bar * s.barTicks();
    cl.length = s.barTicks();
    s.playlist.push_back(cl);
  }
  cutoff = paramIndex("gl.analog", "Cutoff");
  AutomationClip a;
  a.id = s.allocateId();
  a.owner = c.id;
  a.index = kChannelPluginBase + static_cast<uint32_t>(std::max(0, cutoff));
  // Moving over the first bar, holding still, moving again over bar 7 (in the gap: the resting
  // instrument wakes for it and follows it), holding still to the end.
  const Tick bar = s.barTicks();
  a.points = {{0, 0.3f}, {bar, 0.8f}, {7 * bar, 0.8f}, {7 * bar + bar / 2, 0.5f}, {10 * bar, 0.5f}};
  s.automations.push_back(a);
  Clip ac;
  ac.id = s.allocateId();
  ac.kind = ClipKind::Automation;
  ac.source = a.id;
  ac.track = 1;
  ac.start = 0;
  ac.length = 10 * s.barTicks();
  s.playlist.push_back(ac);
  return s;
}

Run playRealtime(const Song& s, double seconds, bool rest, ObjectId watch, int threads = 1, const std::vector<double>& probes = {}) {
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
  const int frames = static_cast<int>(seconds * kSr);
  r.L.assign(static_cast<size_t>(frames), 0.f);
  r.R.assign(static_cast<size_t>(frames), 0.f);
  const auto rt = b.channelRuntime(watch);
  size_t probe = 0;
  for (int at = 0; at < frames; at += kHost) {
    e.process(r.L.data() + at, r.R.data() + at, std::min(kHost, frames - at));
    for (; probe < probes.size() && probes[probe] * kSr <= at; ++probe) r.restingAt.push_back(rt && rt->resting);
  }
  if (rt) r.restBlocks = rt->restBlocks.load();
  return r;
}

// An instrument whose parameter automation moves rests between its notes and wakes on its note's
// own sample, sounding as it does with resting off.
void automatedInstrumentRests() {
  ObjectId ch = kNoId;
  int cutoff = -1;
  const Song s = automatedSong(ch, cutoff);
  CHECK(cutoff >= 0);
  const double barSec = 4 * 60.0 / s.tempo;
  const double seconds = 9 * barSec + 1.0;
  for (int threads : {1, 4}) {
    const std::vector<double> probes = {6.5 * barSec, 7.25 * barSec};  // cutoff holding still / moving, no note
    const Run off = playRealtime(s, seconds, false, ch, threads), on = playRealtime(s, seconds, true, ch, threads, probes);
    const double w = worstDiff(off, on);
    const size_t second = static_cast<size_t>(8 * barSec * kSr) - 64;
    std::printf("automated (%d threads): rested %llu blocks, worst difference %.3g (%.1f dB), second note at %d / %d\n", threads,
                static_cast<unsigned long long>(on.restBlocks), w, dB(w), firstSound(on, second), firstSound(off, second));
    CHECK(peakOf(off, second) > 0.01);   // the second note sounds
    CHECK(on.restBlocks > 0);            // it rested in the gap
    CHECK(on.restingAt.size() == 2 && on.restingAt[0] && !on.restingAt[1]);  // ... while its cutoff held still, not while it moved
    CHECK(off.restBlocks == 0);
    CHECK(w <= 1e-6);                    // -120 dB
    CHECK(firstSound(on, second) == firstSound(off, second));  // woke on the note's own sample
  }
}

// The demo song stopped: once the tails have gone, no instrument or effect runs its DSP (its
// automated instruments and the bus effects keyed by the kick included).
void demoRestsWhenStopped() {
  const Bench b = bench(makeDemoTrack(), false, 12.0, 4.0);
  std::printf("demo stopped: %d nodes ran DSP (%s); %llu of %llu track blocks left out whole\n", b.nodes.running, b.nodes.names.c_str(),
              static_cast<unsigned long long>(b.hushed), static_cast<unsigned long long>(b.tracks * b.blocks));
  CHECK(b.nodes.running == 0);
  CHECK(b.hushed == b.tracks * b.blocks);  // every track, every block: the graph at rest costs next to nothing
  // K-142 (follow-up): the whole graph sleeps (one look a block), each device block in one piece
  const uint64_t callbacks = static_cast<uint64_t>(4.0 * kSr / kHost);
  std::printf("demo stopped: %llu engine blocks for %llu device blocks, %llu slept whole, %llu long\n",
              static_cast<unsigned long long>(b.blocks), static_cast<unsigned long long>(callbacks),
              static_cast<unsigned long long>(b.slept), static_cast<unsigned long long>(b.longRest));
  CHECK(b.slept == b.blocks);
  CHECK(b.longRest == b.blocks && b.blocks == callbacks);
}

// K-142 (0.7.8 close): a hall just loaded holds nothing but silence, so once it rests it steps only
// its clocks from its first settle check instead of rendering unheard until a whole ring of silence
// had gone through each of its lines (with its shimmer on, twelve seconds after loading here; now
// five, with or without).
void hallSettlesAtLoad() {
  for (const bool shimmer : {false, true}) {
    Song s = makeEmptySong();
    MixerTrack& m = s.mixer[0];
    m.slots[0].plugin = defaultPluginState("gl.hall");
    if (shimmer && m.slots[0].plugin.params.size() > static_cast<size_t>(packs::spacefx::HvFbOn))
      m.slots[0].plugin.params[static_cast<size_t>(packs::spacefx::HvFbOn)] = 1.0f;
    ProgramBuilder b;
    b.setSampleRate(kSr);
    AudioEngine e(kSr);
    e.setProgram(b.build(s));
    std::vector<float> L(kHost), R(kHost);
    const Effect* fx = nullptr;
    for (const auto& pm : e.program()->mixer)
      for (const auto& sl : pm.slots)
        if (sl.rt && sl.rt->type == "gl.hall") fx = sl.rt->fx.get();
    for (int i = 0; i < 6 * kSr / kHost; ++i) e.process(L.data(), R.data(), kHost);  // six seconds, stopped
    std::printf("hall%s, six seconds after loading: %s\n", shimmer ? " (shimmer)" : "", fx && fx->restSettled() ? "clocks only" : "still rendering");
    CHECK(fx != nullptr && fx->restSettled());
  }
}

ObjectId channelOnTrack(const Song& s, const std::string& track) {
  for (const MixerTrack& m : s.mixer)
    if (m.name == track)
      for (const Channel& c : s.channels)
        if (c.mixerTrack == m.id) return c.id;
  return kNoId;
}

struct LiveRun {
  Run out;
  int ranBefore = 0;      // nodes that ran DSP over the second before the notes
  int ranFirst = 0;       // nodes that ran DSP in the block the notes came in
  uint64_t hushed = 0;    // track blocks left out whole before the notes
  uint64_t slept = 0;     // K-142 (follow-up): blocks the graph slept whole before the notes
  uint64_t longRest = 0;  // ... and blocks taken in one piece
};

// `s` stopped for 12 seconds (everything comes to rest), then live notes on `a` and `b`, released
// after half a second; 3 seconds recorded.
LiveRun liveNotesAfterRest(const Song& s, ObjectId a, int pa, ObjectId b, int pb, bool rest, bool sleep = true) {
  ProgramBuilder pb2;
  pb2.setSampleRate(kSr);
  AudioEngine e(kSr);
  e.setRestInstruments(rest);
  e.setGraphSleep(sleep);
  e.setCallCounting(true);
  e.setProgram(pb2.build(s));
  e.setMode(PlayMode::Song);
  std::vector<float> L(kHost), R(kHost);
  for (int i = 0; i < 11 * kSr / kHost; ++i) e.process(L.data(), R.data(), kHost);
  std::vector<uint64_t> last;
  callsSince(*e.program(), last);
  for (int i = 0; i < kSr / kHost; ++i) e.process(L.data(), R.data(), kHost);
  LiveRun r;
  r.ranBefore = callsSince(*e.program(), last).running;
  r.hushed = e.hushedTracks();
  r.slept = e.sleptBlocks();
  r.longRest = e.longRestBlocks();
  const int frames = 3 * kSr;
  r.out.L.assign(static_cast<size_t>(frames), 0.f);
  r.out.R.assign(static_cast<size_t>(frames), 0.f);
  e.noteOn(a, pa, 1.0f);
  e.noteOn(b, pb, 0.9f);
  for (int at = 0; at < frames; at += kHost) {
    if (at == kSr / 2) {
      e.noteOff(a, pa);
      e.noteOff(b, pb);
    }
    e.process(r.out.L.data() + at, r.out.R.data() + at, std::min(kHost, frames - at));
    if (at == 0) r.ranFirst = callsSince(*e.program(), last).running;
  }
  return r;
}

// A kick keying a bus (its side input) whose effects have no modulation of their own (a sidechain
// ducker, a compressor, a filter, a distortion, a stereo shaper), and a bass with an automated
// cutoff playing into that bus.
Song sidechainSong(ObjectId& kick, ObjectId& bass) {
  Song s = makeEmptySong();
  const ObjectId master = s.masterId();
  MixerTrack& kt = s.mixer[1];
  MixerTrack& bus = s.mixer[2];
  kt.name = "Kick";
  bus.name = "Bus";
  kt.sends = {Send{master, 1.0f}, Send{bus.id, 1.0f, true}};
  bus.sends = {Send{master, 1.0f}};
  const char* fx[] = {"gl.scduck", "gl.comp", "gl.filter", "gl.dist", "gl.stereo"};
  for (int k = 0; k < 5; ++k) bus.slots[static_cast<size_t>(k)].plugin = defaultPluginState(fx[k]);
  Channel kc;
  kc.id = s.allocateId();
  kc.name = "Kick";
  kc.plugin = defaultPluginState("gl.kick");
  kc.mixerTrack = kt.id;
  Channel bc;
  bc.id = s.allocateId();
  bc.name = "Bass";
  bc.plugin = defaultPluginState("gl.analog");
  bc.mixerTrack = bus.id;
  s.channels = {kc, bc};
  kick = kc.id;
  bass = bc.id;
  AutomationClip ac;
  ac.id = s.allocateId();
  ac.owner = bc.id;
  ac.index = kChannelPluginBase + static_cast<uint32_t>(std::max(0, paramIndex("gl.analog", "Cutoff")));
  ac.points = {{0, 0.4f}, {4 * s.barTicks(), 0.7f}};
  s.automations.push_back(ac);
  Clip c;
  c.id = s.allocateId();
  c.kind = ClipKind::Automation;
  c.source = ac.id;
  c.start = 0;
  c.length = 4 * s.barTicks();
  s.playlist.push_back(c);
  return s;
}

// Stopped and at rest, live notes on the kick (the bus effects' side input) and the bass: every
// node rested before, the bus effects wake in the block the notes come in, the first sound comes
// on the same sample as with resting off and the output equals it (-90 dB: the effects' rest rule).
// The demo song too (its kick and its automated Reese bass); its output is compared on the first
// sound only, as its chorus and halls stand still while they rest (GitHub #66: stopped only).
void liveNotesWakeTheRestingBus() {
  ObjectId kick = kNoId, bass = kNoId;
  const Song s = sidechainSong(kick, bass);
  const LiveRun off = liveNotesAfterRest(s, kick, 36, bass, 40, false), on = liveNotesAfterRest(s, kick, 36, bass, 40, true);
  const double w = worstDiff(off.out, on.out);
  std::printf("sidechain bus: %d nodes ran before, %d in the first block; peak %.3f, worst difference %.3g (%.1f dB), first sound %d / %d\n",
              on.ranBefore, on.ranFirst, peakOf(off.out), w, dB(w), firstSound(on.out, 0), firstSound(off.out, 0));
  CHECK(on.ranBefore == 0);  // both instruments and the five bus effects rested
  CHECK(on.hushed > 0);      // and the tracks were left out whole
  CHECK(on.slept > 0 && off.slept == 0);  // (follow-up) the graph slept whole; never with resting off
  CHECK(on.ranFirst == 7);       // all woke in the notes' block
  CHECK(peakOf(off.out) > 0.01);
  CHECK(w <= 3.2e-5);  // -90 dB
  CHECK(firstSound(on.out, 0) == firstSound(off.out, 0));

  const Song demo = makeDemoTrack();
  const ObjectId dk = channelOnTrack(demo, "Kick"), reese = channelOnTrack(demo, "Reese");
  CHECK(dk != kNoId && reese != kNoId);
  const LiveRun doff = liveNotesAfterRest(demo, dk, 36, reese, 40, false), don = liveNotesAfterRest(demo, dk, 36, reese, 40, true);
  std::printf("demo: %d nodes ran before, %d in the first block, first sound %d / %d\n", don.ranBefore, don.ranFirst,
              firstSound(don.out, 0), firstSound(doff.out, 0));
  CHECK(don.ranBefore == 0);
  CHECK(don.ranFirst > 0);
  CHECK(don.slept > 0 && don.longRest > 0);  // (follow-up) asleep, in long blocks, until the notes
  const LiveRun dawake = liveNotesAfterRest(demo, dk, 36, reese, 40, true, false);
  std::printf("demo, the graph never asleep: worst difference %.3g\n", worstDiff(dawake.out, don.out));
  CHECK(dawake.slept == 0 && worstDiff(dawake.out, don.out) == 0.0);  // bit for bit the same as each node resting alone
  const LiveRun sawake = liveNotesAfterRest(s, kick, 36, bass, 40, true, false);
  CHECK(worstDiff(sawake.out, on.out) == 0.0);
  CHECK(peakOf(don.out) > 0.01);
  CHECK(firstSound(don.out, 0) == firstSound(doff.out, 0));
}

// K-142 (follow-up): the demo stopped until its graph sleeps in long blocks, then the transport
// starts: the first sound comes on the same sample as with resting off, and so does the first
// second (the song from its start, every node woken).
void playWakesTheSleepingGraph() {
  const Song demo = makeDemoTrack();
  auto run = [&](bool rest, uint64_t& slept, uint64_t& longRest, bool sleep = true) {
    ProgramBuilder b;
    b.setSampleRate(kSr);
    AudioEngine e(kSr);
    e.setRestInstruments(rest);
    e.setGraphSleep(sleep);
    e.setProgram(b.build(demo));
    e.setMode(PlayMode::Song);
    e.setLooping(false);
    std::vector<float> L(kHost), R(kHost);
    for (int i = 0; i < 12 * kSr / kHost; ++i) e.process(L.data(), R.data(), kHost);
    slept = e.sleptBlocks();
    longRest = e.longRestBlocks();
    e.play();
    Run r;
    const int frames = 2 * kSr;
    r.L.assign(static_cast<size_t>(frames), 0.f);
    r.R.assign(static_cast<size_t>(frames), 0.f);
    for (int at = 0; at < frames; at += kHost) e.process(r.L.data() + at, r.R.data() + at, std::min(kHost, frames - at));
    return r;
  };
  uint64_t so = 0, lo = 0, sf = 0, lf = 0, sn = 0, ln = 0;
  const Run on = run(true, so, lo), off = run(false, sf, lf), awake = run(true, sn, ln, false);
  const double same = worstDiff(awake, on);
  const double w = worstDiff(off, on);
  std::printf("play after sleep: %llu blocks slept, %llu long; first sound %d / %d, worst difference %.3g (%.1f dB)\n",
              static_cast<unsigned long long>(so), static_cast<unsigned long long>(lo), firstSound(on, 0), firstSound(off, 0), w, dB(w));
  std::printf("play after sleep, resting with the graph never asleep: %llu slept, worst difference %.3g\n", static_cast<unsigned long long>(sn), same);
  CHECK(so > 0 && lo > 0 && sf == 0 && lf == 0 && sn == 0 && ln == 0);
  CHECK(same == 0.0);  // the sleep and the long blocks change nothing: bit for bit what each node's own rest gives
  CHECK(peakOf(off) > 0.01);
  CHECK(firstSound(on, 0) == firstSound(off, 0));
}

uint64_t hashOf(const AudioBuffer& a) {
  uint64_t h = 1469598103934665603ull;
  auto mix = [&](const std::vector<float>& v) {
    for (float f : v) {
      uint32_t u = 0;
      std::memcpy(&u, &f, sizeof u);
      h = (h ^ u) * 1099511628211ull;
    }
  };
  mix(a.data);
  return h;
}

// Offline renders never rest: the demo and the automated song render the same with Rest silent
// instruments on and off (and their hashes are printed, for comparing builds).
void offlineUnchanged() {
  ObjectId ch = kNoId;
  int cutoff = -1;
  const Song autoSong = automatedSong(ch, cutoff);
  Song demo = makeDemoTrack();
  Song muted = demo;  // muted tracks send silence down their (compensated) routes
  for (MixerTrack& m : muted.mixer)
    if (m.name == "Kick" || m.name == "Felt Keys" || m.name == "Echo") m.muted = true;
  for (const Song* s : {&autoSong, static_cast<const Song*>(&demo), static_cast<const Song*>(&muted)}) {
    RenderOptions o;
    o.threads = 4;
    const AudioBuffer a = renderSong(*s, o);
    const uint64_t h = hashOf(a);
    std::printf("offline render: %zu frames, hash %016llx\n", a.frames, static_cast<unsigned long long>(h));
    CHECK(a.frames > 0);
  }
}

}  // namespace

int main() {
  automatedInstrumentRests();
  demoRestsWhenStopped();
  hallSettlesAtLoad();
  liveNotesWakeTheRestingBus();
  playWakesTheSleepingGraph();
  offlineUnchanged();
  report();
  return finish();
}
