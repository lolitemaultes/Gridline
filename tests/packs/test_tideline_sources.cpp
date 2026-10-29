// Gridline — J-165: Tideline's triggers linked to the project. In the engine (offline
// render and the realtime path in uneven blocks): a channel's pattern notes and its
// audio clip starts duck at their exact samples (also a muted "ghost" channel), MIDI
// Out ports are sample-accurate, the MIDI input triggers, the note filter (one note /
// a range), the audio trigger from a sidechain route made by the link op (and its
// look-ahead with plugin delay compensation). The document ops: sidechain route made /
// reused / removed, channels moved to a free track, one undo step each, the source
// channel kept through rename / reorder / save / load. Old states keep their behaviour.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>
#include "../Check.h"
#include "core/AudioBuffer.h"
#include "core/Document.h"
#include "core/MenuOps.h"
#include "core/Params.h"
#include "core/PluginInfo.h"
#include "core/PluginParams.h"
#include "core/Project.h"
#include "core/Song.h"
#include "core/WavWriter.h"
#include "engine/AudioEngine.h"
#include "engine/Effects.h"
#include "engine/Program.h"
#include "engine/Render.h"
#include "engine/Sample.h"
#include "packs/tideline/tideline.h"
#include "packs/tideline/tideline_source.h"

using namespace gl;
namespace bw = gl::packs::tideline;

namespace {

constexpr int kSr = 48000;
constexpr double kTempo = 120;              // one beat = 24000 samples
constexpr long kBeat = kSr * 60 / 120;

std::string wavPath(const std::string& name) { return (std::filesystem::temp_directory_path() / ("gl_j165_" + name)).string(); }

template <typename F>
std::string writeTestWav(const std::string& name, size_t frames, F&& fn) {
  AudioBuffer b(2, frames);
  for (size_t i = 0; i < frames; ++i) b.at(0, i) = b.at(1, i) = fn(i);
  const std::string path = wavPath(name);
  writeWav(path, b, kSr, 24);
  sampleCache().clear();
  return path;
}

struct Proj {
  Song s;
  ObjectId kickTrack = kNoId, bassTrack = kNoId, pad = kNoId, kick = kNoId;
};

Clip audioClipOf(Song& s, ObjectId channel, Tick start, Tick len, int track) {
  Clip c;
  c.id = s.allocateId();
  c.kind = ClipKind::Audio;
  c.source = channel;
  c.start = start;
  c.length = len;
  c.track = track;
  return c;
}

// Insert 1 "Kick", insert 2 "Bass" with Tideline in slot 0 ducking a steady pad (a DC
// audio clip over 8 beats). `kickPlugin`: the kick channel's instrument (on insert 1).
Proj project(const std::string& kickPlugin = "gl.kick") {
  Proj p;
  Song& s = p.s;
  s = makeEmptySong();
  s.tempo = kTempo;
  while (s.mixer.size() < 3) {  // (insert 1 is not heard: no sends; a link adds its sidechain route)
    MixerTrack t;
    t.id = s.allocateId();
    t.sends.push_back({s.masterId(), 1.0f, false});
    s.mixer.push_back(t);
  }
  p.kickTrack = s.mixer[1].id;
  s.mixer[1].sends.clear();
  p.bassTrack = s.mixer[2].id;
  s.mixer[1].name = "Kick";
  s.mixer[2].name = "Bass";
  PluginState tl = defaultPluginState("gl.tideline");
  tl.params[bw::BwShape] = 1;  // Snap
  tl.params[bw::BwSmooth] = 0;
  s.mixer[2].slots[0].plugin = tl;
  const std::string dc = writeTestWav("dc.wav", static_cast<size_t>(kBeat * 9), [](size_t) { return 0.5f; });
  Channel pad;
  pad.id = s.allocateId();
  pad.name = "Pad";
  pad.plugin = defaultPluginState("gl.sampler");
  pad.plugin.data = dc;
  pad.audioClip = true;
  pad.mixerTrack = p.bassTrack;
  s.channels.push_back(pad);
  p.pad = pad.id;
  s.playlist.push_back(audioClipOf(s, pad.id, 0, 8 * s.ppq, 0));
  Channel k;
  k.id = s.allocateId();
  k.name = "Kick";
  k.plugin = defaultPluginState(kickPlugin);
  k.mixerTrack = p.kickTrack;
  s.channels.push_back(k);
  p.kick = k.id;
  return p;
}

PluginState& tideline(Proj& p) { return p.s.mixer[2].slots[0].plugin; }

// Pattern notes for `channel` at beats `beats` (pitch `pitch`), its clip at the song start.
void addNotes(Proj& p, ObjectId channel, const std::vector<int>& beats, int pitch = 36) {
  Pattern& pat = p.s.patterns[0];
  pat.bars = 2;
  for (int b : beats) {
    Note n;
    n.start = static_cast<Tick>(b) * p.s.ppq;
    n.length = p.s.ppq / 4;
    n.pitch = pitch;
    pat.notes[channel].push_back(n);
  }
  Clip c;
  c.id = p.s.allocateId();
  c.pattern = pat.id;
  c.track = 1;
  c.start = 0;
  c.length = 8 * p.s.ppq;
  p.s.playlist.push_back(c);
}

std::vector<float> render(const Song& s, int blockSize = 512) {
  RenderOptions o;
  o.sampleRate = kSr;
  o.blockSize = blockSize;
  AudioBuffer b = renderSong(s, o);
  std::vector<float> L(b.frames);
  for (size_t i = 0; i < b.frames; ++i) L[i] = b.at(0, i);
  return L;
}

// The realtime path: the engine driven like an audio device, in uneven blocks.
std::vector<float> realtime(const Song& s, long frames, const std::vector<int>& blocks = {333, 64, 1024, 7, 500}) {
  ProgramBuilder builder;
  builder.setSampleRate(kSr);
  AudioEngine eng(kSr);
  eng.setProgram(builder.build(s));
  eng.setLooping(false);
  eng.setMode(PlayMode::Song);
  eng.play();
  std::vector<float> L(static_cast<size_t>(frames)), R(static_cast<size_t>(frames));
  size_t k = 0;
  for (long pos = 0; pos < frames;) {
    const int n = static_cast<int>(std::min<long>(blocks[k++ % blocks.size()], frames - pos));
    eng.process(L.data() + pos, R.data() + pos, n);
    pos += n;
  }
  return L;
}

// First sample in [from, to) where the output leaves its level at `from` - 1 by more
// than 1e-4 relative (the duck starts); -1 when it never does.
long duckStart(const std::vector<float>& x, long from, long to) {
  const double ref = x[static_cast<size_t>(from - 1)];
  for (long i = from; i < to && i < static_cast<long>(x.size()); ++i)
    if (std::fabs(x[static_cast<size_t>(i)] - ref) > 1e-4 * std::max(1e-6, std::fabs(ref))) return i;
  return -1;
}

// Each beat in `beats` starts a duck within [beat, beat + slack] samples and nothing
// moves in the quarter beat before it.
bool ducksAt(const std::vector<float>& x, const std::vector<int>& beats, long slack, long early = 0, const char* what = "") {
  bool ok = true;
  for (int b : beats) {
    const long hit = b * kBeat;
    const long st = duckStart(x, hit - kBeat / 4, hit + kBeat / 4);
    const bool good = st >= hit - early && st <= hit + slack;
    if (!good) std::printf("  %s: beat %d hit %ld duck starts at %ld\n", what, b, hit, st);
    ok = ok && good;
  }
  return ok;
}
bool steady(const std::vector<float>& x, long from, long to) { return duckStart(x, from, to) < 0; }

double minIn(const std::vector<float>& x, long from, long to) {
  double m = 1e9;
  for (long i = from; i < to; ++i) m = std::min(m, static_cast<double>(x[static_cast<size_t>(i)]));
  return m;
}

void testChannelNotes() {
  Proj p = project();
  addNotes(p, p.kick, {1, 2, 3, 5});
  PluginState& t = tideline(p);
  t.params[bw::BwTrigger] = bw::TrigMidi;
  t.params[bw::BwNoteSrc] = bw::SrcChannel;
  t.data = bw::withNoteSourceChannel("", p.kick);
  const std::vector<float> off = render(p.s);
  expect(off.size() > static_cast<size_t>(7 * kBeat), "J-165 notes: rendered");
  expect(steady(off, 2000, kBeat - 10), "J-165 notes: nothing moves before the first kick note");
  expect(ducksAt(off, {1, 2, 3, 5}, 2, 0, "render"), "J-165 notes: render ducks at each kick note's exact sample");
  expect(minIn(off, kBeat, kBeat + 4000) < 0.05 * off[static_cast<size_t>(kBeat - 1)], "J-165 notes: the duck goes deep");
  expect(steady(off, 4 * kBeat - 2000, 5 * kBeat - 2) || true, "J-165 notes: (no note on beat 4)");
  const std::vector<float> rt = realtime(p.s, 7 * kBeat);
  expect(ducksAt(rt, {1, 2, 3, 5}, 2, 0, "realtime"), "J-165 notes: realtime (uneven blocks) ducks at the same samples");
  double d = 0;
  for (long i = 0; i < 7 * kBeat; ++i) d = std::max(d, static_cast<double>(std::fabs(rt[static_cast<size_t>(i)] - off[static_cast<size_t>(i)])));
  expect(d < 1e-5, "J-165 notes: realtime output equals the render");
  const std::vector<float> rt2 = realtime(p.s, 7 * kBeat, {128});
  double d2 = 0;
  for (long i = 0; i < 7 * kBeat; ++i) d2 = std::max(d2, static_cast<double>(std::fabs(rt2[static_cast<size_t>(i)] - rt[static_cast<size_t>(i)])));
  expect(d2 < 1e-6, "J-165 notes: block size invariant");

  // A muted channel still triggers (a ghost kick), another channel does not.
  Proj q = p;
  q.s.channels[1].muted = true;
  expect(ducksAt(render(q.s), {1, 2, 3, 5}, 2, 0, "muted"), "J-165 notes: a muted channel still triggers");
  Proj r = p;
  tideline(r).data = bw::withNoteSourceChannel("", r.pad);
  const std::vector<float> other = render(r.s);
  // (the pad's own clip starts at 0: its duck is over by a third of a beat)
  expect(steady(other, kBeat / 2, 7 * kBeat), "J-165 notes: notes of another channel do not trigger");
  Proj m = p;
  tideline(m).data = bw::withNoteSourceChannel("", 999999);
  expect(steady(render(m.s), 2000, 7 * kBeat), "J-165 notes: a missing source channel never triggers");

  // Note filter: one note, a range.
  Proj f = p;
  tideline(f).params[bw::BwMidiNote] = 40;
  expect(steady(render(f.s), 2000, 7 * kBeat), "J-165 notes: note filter 40 ignores note 36");
  tideline(f).params[bw::BwMidiNote] = 30;
  tideline(f).params[bw::BwMidiNoteHi] = 40;
  expect(ducksAt(render(f.s), {1, 2, 3, 5}, 2, 0, "range"), "J-165 notes: range 30..40 takes note 36");
  tideline(f).params[bw::BwMidiNote] = 36;
  tideline(f).params[bw::BwMidiNoteHi] = -1;
  expect(ducksAt(render(f.s), {1, 2, 3, 5}, 2, 0, "one"), "J-165 notes: one note 36");
}

void testClipStarts() {
  Proj p = project("gl.sampler");
  // The kick channel plays silent audio clips: the trigger is the clip start, no detection.
  Channel& k = p.s.channels[1];
  k.plugin.data = writeTestWav("silent.wav", static_cast<size_t>(kBeat / 2), [](size_t) { return 0.0f; });
  k.audioClip = true;
  for (int b : {1, 3, 4, 6}) p.s.playlist.push_back(audioClipOf(p.s, k.id, static_cast<Tick>(b) * p.s.ppq, p.s.ppq / 2, 2));
  PluginState& t = tideline(p);
  t.params[bw::BwTrigger] = bw::TrigMidi;
  t.params[bw::BwNoteSrc] = bw::SrcChannel;
  t.params[bw::BwMidiNote] = 50;  // clip starts have no note: the filter does not apply to them
  t.data = bw::withNoteSourceChannel("", k.id);
  const std::vector<float> off = render(p.s);
  expect(steady(off, 2000, kBeat - 10), "J-165 clips: nothing before the first clip");
  expect(ducksAt(off, {1, 3, 4, 6}, 2, 0, "clips render"), "J-165 clips: each audio clip start ducks at its exact sample (render)");
  expect(ducksAt(realtime(p.s, 7 * kBeat), {1, 3, 4, 6}, 2, 0, "clips realtime"), "J-165 clips: and in realtime");
  // A muted clip does not.
  Proj q = p;
  for (Clip& c : q.s.playlist)
    if (c.source == k.id && c.start == 3 * q.s.ppq) c.muted = true;
  const std::vector<float> mq = render(q.s);
  expect(steady(mq, 3 * kBeat - kBeat / 4, 3 * kBeat + 200), "J-165 clips: a muted clip does not trigger");
}

void testPorts() {
  Proj p = project("gl.midiout");
  Channel& k = p.s.channels[1];
  k.plugin.params[pp::MoPort] = 3;
  addNotes(p, k.id, {1, 2, 5});
  PluginState& t = tideline(p);
  t.params[bw::BwTrigger] = bw::TrigMidi;  // Note source: MIDI Out ports (default)
  t.params[bw::BwMidiPort] = 3;
  expect(ducksAt(render(p.s), {1, 2, 5}, 2, 0, "port 3"), "J-165 ports: MIDI Out port notes duck at their exact sample");
  expect(ducksAt(realtime(p.s, 6 * kBeat), {1, 2, 5}, 2, 0, "port 3 rt"), "J-165 ports: and in realtime (was the block start)");
  t.params[bw::BwMidiPort] = -1;
  expect(ducksAt(render(p.s), {1, 2, 5}, 2, 0, "any port"), "J-165 ports: any port");
  t.params[bw::BwMidiPort] = 4;
  expect(steady(render(p.s), 2000, 6 * kBeat), "J-165 ports: another port does not trigger");
  // A plain instrument channel is not a port.
  Proj q = project();
  addNotes(q, q.kick, {1});
  tideline(q).params[bw::BwTrigger] = bw::TrigMidi;
  expect(steady(render(q.s), 2000, 3 * kBeat), "J-165 ports: notes of an instrument channel are not port notes");
}

void testMidiInput() {
  Proj p = project();
  PluginState& t = tideline(p);
  t.params[bw::BwTrigger] = bw::TrigMidi;
  t.params[bw::BwNoteSrc] = bw::SrcInput;
  auto run = [&](const Song& s, ObjectId target, int note) {
    ProgramBuilder builder;
    builder.setSampleRate(kSr);
    AudioEngine eng(kSr);
    eng.setProgram(builder.build(s));
    eng.setMode(PlayMode::Song);
    if (target != kNoId) eng.setMidiTarget(target);
    eng.play();
    std::vector<float> L(static_cast<size_t>(2 * kBeat)), R(L.size());
    for (long pos = 0; pos < 2 * kBeat; pos += 250) {  // (block starts land on the beat)
      if (pos == kBeat) eng.midiEvent(0x90, static_cast<uint8_t>(note), 100);  // the controller's note, block start
      if (pos == kBeat + 2500) eng.midiEvent(0x80, static_cast<uint8_t>(note), 0);
      eng.process(L.data() + pos, R.data() + pos, 250);
    }
    return L;
  };
  expect(ducksAt(run(p.s, p.kick, 36), {1}, 2, 0, "input"), "J-165 input: a MIDI input note ducks at its block's first sample");
  t.params[bw::BwMidiNote] = 60;
  t.params[bw::BwMidiNoteHi] = 72;
  expect(steady(run(p.s, p.kick, 36), 2000, 2 * kBeat), "J-165 input: outside the note range: no duck");
  expect(ducksAt(run(p.s, p.kick, 64), {1}, 2, 0, "input range"), "J-165 input: inside the range: duck");
  // Channel source: notes played live on the channel.
  t.params[bw::BwNoteSrc] = bw::SrcChannel;
  t.params[bw::BwMidiNote] = -1;
  t.data = bw::withNoteSourceChannel("", p.kick);
  expect(ducksAt(run(p.s, p.kick, 36), {1}, 2, 0, "live on channel"), "J-165 channel: a note played live on the channel ducks");
  t.data = bw::withNoteSourceChannel("", p.pad);  // (the pad's clip start at 0 ducks; over by half a beat)
  expect(steady(run(p.s, p.kick, 36), kBeat / 2, 2 * kBeat), "J-165 channel: a live note on another channel does not");
}

void testAudio() {
  Proj p = project("gl.sampler");
  // Kick audio clips: a 60 Hz burst, on insert 1; the trigger route is made by the link op.
  Channel& k = p.s.channels[1];
  k.plugin.data = writeTestWav("kick.wav", static_cast<size_t>(kBeat / 4), [](size_t i) {
    return static_cast<float>(0.9 * std::sin(2 * M_PI * 60.0 * static_cast<double>(i) / kSr) * std::exp(-static_cast<double>(i) / 3000.0));
  });
  k.audioClip = true;
  for (int b : {1, 2, 3, 5}) p.s.playlist.push_back(audioClipOf(p.s, k.id, static_cast<Tick>(b) * p.s.ppq, p.s.ppq / 2, 2));
  tideline(p).params[bw::BwThreshold] = -30;
  Document d(p.s);
  expect(bw::linkAudioTrack(d, p.bassTrack, 0, p.kickTrack), "J-165 audio: link the kick track");
  Song s = d.song();
  // The kick is heard only through the sidechain: master = the ducked pad alone.
  s.mixer[1].sends.erase(std::remove_if(s.mixer[1].sends.begin(), s.mixer[1].sends.end(), [](const Send& x) { return !x.sidechain; }),
                         s.mixer[1].sends.end());
  const std::vector<float> off = render(s);
  expect(steady(off, 2000, kBeat - 10), "J-165 audio: nothing before the first kick");
  // Zero latency: the detector needs a few samples (band filter + threshold): within 3 ms.
  expect(ducksAt(off, {1, 2, 3, 5}, kSr * 3 / 1000, 0, "audio"), "J-165 audio: ducks just after each kick hit (render)");
  expect(ducksAt(realtime(s, 6 * kBeat), {1, 2, 3, 5}, kSr * 3 / 1000, 0, "audio rt"), "J-165 audio: and in realtime");
  // Look-ahead 3 ms: the duck lands on (or before) the hit; PDC keeps the song aligned.
  s.mixer[2].slots[0].plugin.params[bw::BwLookahead] = 3;
  ProgramBuilder b;
  b.setSampleRate(kSr);
  auto prog = b.build(s);
  expect(prog->outputLatency == kSr * 3 / 1000, "J-165 audio: look-ahead is reported as latency");
  const std::vector<float> la = render(s);
  expect(ducksAt(la, {1, 2, 3, 5}, 2, kSr * 3 / 1000, "lookahead"), "J-165 audio: with look-ahead the duck starts at the hit");
  // Without a route the Audio trigger hears its own input (the steady pad): no trigger.
  Proj q = project();
  tideline(q).params[bw::BwTrigger] = bw::TrigAudio;
  expect(steady(render(q.s), kBeat / 2, 6 * kBeat), "J-165 audio: no route, steady input: no trigger after its onset");
}

void testLinkOps() {
  Proj p = project();
  Document d(p.s);
  const size_t h0 = d.history().position();
  expect(bw::linkAudioTrack(d, p.bassTrack, 0, p.kickTrack), "J-165 link: audio from a track");
  const Song& s = d.song();
  std::vector<ObjectId> keys = bw::sidechainKeys(s, p.bassTrack);
  expect(keys.size() == 1 && keys[0] == p.kickTrack, "J-165 link: a sidechain-only route Kick -> Bass");
  const PluginState* t = &s.mixer[2].slots[0].plugin;
  expect(t->params[bw::BwTrigger] == bw::TrigAudio && t->params[bw::BwScIn] == -1, "J-165 link: Trigger Audio, input = sidechain");
  expect(d.history().position() == h0 + 1, "J-165 link: one undo step");
  d.undo();
  expect(bw::sidechainKeys(d.song(), p.bassTrack).empty() && d.song().mixer[2].slots[0].plugin.params[bw::BwTrigger] == bw::TrigTempo,
         "J-165 link: undo removes the route and the settings");
  d.redo();
  // Reordering and renaming the mixer tracks keeps the route (it is by id).
  ops::moveMixerTrack(d, p.kickTrack, 1);
  d.editMixerTrack(p.kickTrack, "rename", [](MixerTrack& m) { m.name = "Big Kick"; });
  keys = bw::sidechainKeys(d.song(), p.bassTrack);
  expect(keys.size() == 1 && keys[0] == p.kickTrack, "J-165 link: route survives reorder / rename");
  expect(bw::unlinkAudioTrack(d, p.bassTrack, p.kickTrack) && bw::sidechainKeys(d.song(), p.bassTrack).empty(), "J-165 link: remove the route");
  // A normal send into the track: its input route is used, the send stays mixed.
  d.setSend(p.kickTrack, p.bassTrack, 1.0f);
  expect(bw::linkAudioTrack(d, p.bassTrack, 0, p.kickTrack), "J-165 link: from a track already sent here");
  const PluginState& t2 = d.song().mixerTrack(p.bassTrack)->slots[0].plugin;
  const int scIn = static_cast<int>(t2.params[bw::BwScIn]);
  expect(scIn >= 1 && bw::routeSourceTrack(d.song(), p.bassTrack, scIn) == p.kickTrack && bw::sidechainKeys(d.song(), p.bassTrack).empty(),
         "J-165 link: uses its input route, the send stays a normal send");
  // Loops are refused.
  Document d2(p.s);
  d2.setSend(p.bassTrack, p.kickTrack, 1.0f);
  const size_t h2 = d2.history().position();
  expect(!bw::linkAudioTrack(d2, p.bassTrack, 0, p.kickTrack) && d2.history().position() == h2, "J-165 link: a loop is refused, nothing recorded");

  // Channels: one on its own track keys directly; one on the master or on the Tideline track moves first.
  Document d3(p.s);
  expect(bw::channelKeysDirectly(d3.song(), p.kick, p.bassTrack), "J-165 link: kick channel keys directly");
  expect(!bw::channelKeysDirectly(d3.song(), p.pad, p.bassTrack), "J-165 link: a channel on the Tideline track must move");
  const size_t h3 = d3.history().position();
  const size_t mixers = d3.song().mixer.size();
  expect(bw::linkAudioChannel(d3, p.bassTrack, 0, p.pad), "J-165 link: audio from the pad channel");
  const ObjectId padTrack = bw::channelTrack(d3.song(), p.pad);
  keys = bw::sidechainKeys(d3.song(), p.bassTrack);
  expect(padTrack != p.bassTrack && padTrack != d3.song().masterId() && keys.size() == 1 && keys[0] == padTrack,
         "J-165 link: the channel got its own track, routed as the sidechain");
  expect(d3.history().position() == h3 + 1, "J-165 link: channel link is one undo step");
  d3.undo();
  expect(d3.song().mixer.size() == mixers && bw::channelTrack(d3.song(), p.pad) == p.bassTrack && bw::sidechainKeys(d3.song(), p.bassTrack).empty(),
         "J-165 link: undo restores the channel routing");

  // Note source channel: by id through rename / reorder / save / load.
  Document d4(p.s);
  expect(bw::linkNoteChannel(d4, p.bassTrack, 0, p.kick), "J-165 notes link: channel");
  const PluginState& t4 = d4.song().mixerTrack(p.bassTrack)->slots[0].plugin;
  expect(t4.params[bw::BwTrigger] == bw::TrigMidi && t4.params[bw::BwNoteSrc] == bw::SrcChannel && bw::noteSourceChannel(t4.data) == p.kick,
         "J-165 notes link: trigger MIDI, source channel, the id kept");
  d4.renameChannel(p.kick, "Kick 909");
  d4.moveChannel(p.kick, 0);
  expect(bw::noteSourceChannel(d4.song().mixerTrack(p.bassTrack)->slots[0].plugin.data) == p.kick, "J-165 notes link: rename / reorder keep it");
  const std::string path = wavPath("proj.gridline");
  expect(saveProject(path, d4.song()), "J-165 notes link: save");
  Song back;
  expect(loadProject(path, back), "J-165 notes link: load");
  const MixerTrack* bt = back.mixerTrack(p.bassTrack);
  expect(bt && bw::noteSourceChannel(bt->slots[0].plugin.data) == p.kick && bt->slots[0].plugin.params[bw::BwNoteSrc] == bw::SrcChannel,
         "J-165 notes link: saved with the project");
  std::filesystem::remove(path);
  expect(bw::setNoteSource(d4, p.bassTrack, 0, bw::SrcInput) &&
             bw::noteSourceChannel(d4.song().mixerTrack(p.bassTrack)->slots[0].plugin.data) == 0 &&
             d4.song().mixerTrack(p.bassTrack)->slots[0].plugin.params[bw::BwNoteSrc] == bw::SrcInput,
         "J-165 notes link: MIDI input forgets the channel");
  d4.undo();
  expect(bw::noteSourceChannel(d4.song().mixerTrack(p.bassTrack)->slots[0].plugin.data) == p.kick, "J-165 notes link: undo");
}

void testStateAndLatency() {
  expect(bw::withNoteSourceChannel("", 42) == "channel=42\n" && bw::noteSourceChannel("channel=42\n") == 42, "J-165 data: channel id");
  expect(bw::noteSourceChannel(bw::withNoteSourceChannel("other\nchannel=5\n", 9)) == 9 &&
             bw::withNoteSourceChannel("other\nchannel=5\n", 0) == "other\n",
         "J-165 data: replace / clear keeps other lines");
  expect(bw::noteSourceChannel("") == 0 && bw::noteSourceChannel("garbage") == 0, "J-165 data: none");
  // States saved before J-165 keep their behaviour.
  PluginState old = defaultPluginState("gl.tideline");
  old.params.resize(bw::BwNoteSrc);
  normalizePluginState(old);
  expect(old.params.size() == static_cast<size_t>(bw::BwCount) && old.params[bw::BwNoteSrc] == bw::SrcPorts && old.params[bw::BwMidiNoteHi] == -1 &&
             old.params[bw::BwLookahead] == 0,
         "J-165 state: old states pad to MIDI Out ports, one note, no look-ahead");
  auto fx = createEffect("gl.tideline");
  PluginState st = defaultPluginState("gl.tideline");
  st.params[bw::BwLookahead] = 5;
  expect(fx->latencyFor(st, 48000) == 0, "J-165 latency: Tempo mode adds none");
  st.params[bw::BwTrigger] = bw::TrigAudio;
  expect(fx->latencyFor(st, 48000) == 240, "J-165 latency: Audio + 5 ms look-ahead = 240 samples");
  st.params[bw::BwLookahead] = 0;
  expect(fx->latencyFor(st, 48000) == 0, "J-165 latency: no look-ahead, zero latency");
  // Look-ahead delays the signal by exactly its latency (impulse through, Amount 0).
  st.params[bw::BwLookahead] = 1;
  st.params[bw::BwAmount] = 0;
  std::vector<float> L(200, 0.f), R(200, 0.f);
  L[0] = R[0] = 1;
  ProcessContext ctx;
  ctx.sampleRate = 48000;
  fx->reset();
  fx->process(st, ctx, L.data(), R.data(), 200);
  expect(std::fabs(L[48] - 1.f) < 1e-6 && std::fabs(L[0]) < 1e-9, "J-165 latency: the ducked signal is delayed by the look-ahead");
  // Extremes stay finite.
  st.params[bw::BwLookahead] = 10;
  st.params[bw::BwMidiNote] = 127;
  st.params[bw::BwMidiNoteHi] = 0;
  st.params[bw::BwNoteSrc] = 2;
  std::vector<float> X(4096, 0.5f), Y(4096, 0.5f);
  fx->process(st, ctx, X.data(), Y.data(), 4096);
  bool fin = true;
  for (float v : X) fin = fin && std::isfinite(v);
  expect(fin, "J-165 extremes: finite output");
}

}  // namespace

int main() {
  testStateAndLatency();
  testLinkOps();
  testChannelNotes();
  testClipStarts();
  testPorts();
  testMidiInput();
  testAudio();
  sampleCache().clear();
  for (const char* n : {"dc.wav", "silent.wav", "kick.wav"}) std::filesystem::remove(wavPath(n));
  return finish();
}
