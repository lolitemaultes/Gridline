// Gridline: K-64, playback that stays smooth on a large project, and solos that do not freeze.
//  * a clip's realtime stretch does its start-up work over the blocks before the clip, not in the
//    block it starts in (a 96 kHz source at 44.1 kHz: 17 engine steps), with the same output;
//  * muting and bringing back a playlist track (a solo) makes no new voice and keeps every render;
//  * an edit that cannot move the tempo does no tempo map work (a channel solo edits every channel);
//  * in realtime playback, an idle sampler channel and a settled effect are left out of the block,
//    and the output stays what a render makes.
// Everything is synthetic, and it counts work, never time.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>
#include "../src/core/Document.h"
#include "../src/core/PluginInfo.h"
#include "../src/core/PluginParams.h"
#include "../src/core/Song.h"
#include "../src/core/WavWriter.h"
#include "../src/engine/AudioEngine.h"
#include "../src/engine/Program.h"
#include "../src/engine/Render.h"
#include "../src/engine/Sample.h"
#include "../src/engine/TimeStretch.h"
#include "Check.h"

namespace fs = std::filesystem;
using namespace gl;

namespace {

constexpr int kSr = 44100;

std::string writeTone(const std::string& name, int rate, double seconds) {
  const size_t n = static_cast<size_t>(seconds * rate);
  AudioBuffer b(2, n);
  for (size_t i = 0; i < n; ++i) {
    const double t = static_cast<double>(i) / rate;
    b.at(0, i) = b.at(1, i) = static_cast<float>(0.4 * std::sin(2 * 3.141592653589793 * 110.0 * t) * (1.0 + 0.3 * std::sin(t * 5.0)));
  }
  const std::string path = (fs::temp_directory_path() / name).string();
  writeWav(path, b, rate, 24);
  return path;
}

struct Tone {
  std::vector<float> x;
  int64_t pos = 0;
};
void readTone(void* ctx, float* const* dst, int frames) {
  auto* t = static_cast<Tone*>(ctx);
  for (int i = 0; i < frames; ++i) {
    const int64_t k = t->pos + i;
    const float v = k >= 0 && k < static_cast<int64_t>(t->x.size()) ? t->x[static_cast<size_t>(k)] : 0.f;
    dst[0][i] = v, dst[1][i] = v;
  }
  t->pos += frames;
}

// start() against startLater() + prime() in small pieces: the same stream, and the work really is in pieces.
void primedStretchIsTheSame() {
  constexpr double kSrc = 96000;
  Tone a, b;
  a.x.resize(400000);
  for (size_t i = 0; i < a.x.size(); ++i) a.x[i] = static_cast<float>(0.4 * std::sin(2 * 3.141592653589793 * 110.0 * static_cast<double>(i) / kSrc));
  b.x = a.x;
  const double toFrames = kSrc / kSr, speed = 0.8 * toFrames, pitch = toFrames;
  stretch::Voice va, vb;
  va.prepare(2, kSr, 8 * toFrames, 8 * toFrames, false, stretch::Engine::Stretch, toFrames / 8, kSrc);
  vb.prepare(2, kSr, 8 * toFrames, 8 * toFrames, false, stretch::Engine::Stretch, toFrames / 8, kSrc);
  const int pre = va.prerollFor(speed);
  a.pos = b.pos = 20000 - pre;
  va.start(&readTone, &a, speed, pitch, pre);
  const int64_t whole = va.fedFrames();
  vb.startLater(&readTone, &b, speed, pitch, pre);
  const int64_t atStart = vb.fedFrames();
  expect(vb.primeLeft(speed) > 0, "startLater leaves the stretch's pre-roll to do");
  expect(atStart * 2 < whole, "startLater does less than half of the start-up work (the input it seeks to, no engine step)");
  int calls = 0;
  int64_t biggest = 0;
  while (!vb.prime(&readTone, &b, speed, pitch, 512) && calls < 100000) {
    biggest = std::max(biggest, vb.fedFrames() - atStart);
    ++calls;
    if (calls == 1) biggest = 0;
  }
  expect(calls >= 10, "the pre-roll runs over many calls");
  expect(vb.primeLeft(speed) == 0, "nothing left to prime");
  expect(vb.fedFrames() == whole, "the same input as start()");
  std::vector<float> la(512), ra(512), lb(512), rb(512);
  float* oa[2] = {la.data(), ra.data()};
  float* ob[2] = {lb.data(), rb.data()};
  bool same = true, sound = false;
  for (int k = 0; k < 40; ++k) {
    va.render(oa, 512, speed, pitch, &readTone, &a);
    vb.render(ob, 512, speed, pitch, &readTone, &b);
    for (int i = 0; i < 512; ++i) same = same && la[static_cast<size_t>(i)] == lb[static_cast<size_t>(i)], sound = sound || std::fabs(la[static_cast<size_t>(i)]) > 0.05f;
  }
  expect(same && sound, "a primed stream's output is identical, sample for sample");
}

// A voice that shares out its steps: no block does the work of a whole hop, and the frames stay the same frames.
void spreadVoiceIsTheSameAndEven() {
  constexpr double kSrc = 96000;
  Tone a, b;
  a.x.resize(600000);
  for (size_t i = 0; i < a.x.size(); ++i) {
    const double t = static_cast<double>(i) / kSrc;
    a.x[i] = static_cast<float>(0.3 * std::sin(2 * 3.141592653589793 * 55.0 * t) + 0.1 * std::sin(2 * 3.141592653589793 * 221.0 * t) * std::sin(t * 7.0));
  }
  b.x = a.x;
  const double toFrames = kSrc / kSr, speed = 0.8 * toFrames, pitch = toFrames;
  stretch::Voice va, vb;
  va.prepare(2, kSr, 8 * toFrames, 8 * toFrames, false, stretch::Engine::Stretch, toFrames / 8, kSrc);
  vb.prepare(2, kSr, 8 * toFrames, 8 * toFrames, false, stretch::Engine::Stretch, toFrames / 8, kSrc, true);
  const int pre = va.prerollFor(speed);
  a.pos = b.pos = 20000 - pre;
  va.start(&readTone, &a, speed, pitch, pre);
  vb.start(&readTone, &b, speed, pitch, pre);
  constexpr int kBlock = 256, kBlocks = 600;
  std::vector<float> la(kBlock * kBlocks), ra(kBlock * kBlocks), lb(kBlock * kBlocks), rb(kBlock * kBlocks);
  int64_t worstA = 0, worstB = 0, totalA = 0, totalB = 0;
  for (int k = 0; k < kBlocks; ++k) {
    float* oa[2] = {la.data() + k * kBlock, ra.data() + k * kBlock};
    float* ob[2] = {lb.data() + k * kBlock, rb.data() + k * kBlock};
    int64_t s0 = stretch::spectralStepsRun();
    va.render(oa, kBlock, speed, pitch, &readTone, &a);
    int64_t s1 = stretch::spectralStepsRun();
    vb.render(ob, kBlock, speed, pitch, &readTone, &b);
    const int64_t s2 = stretch::spectralStepsRun();
    if (k >= 20) worstA = std::max(worstA, s1 - s0), worstB = std::max(worstB, s2 - s1), totalA += s1 - s0, totalB += s2 - s1;
  }
  std::printf("# steps per %d-frame block: whole hops: worst %lld of %lld; shared out: worst %lld of %lld\n", kBlock, static_cast<long long>(worstA), static_cast<long long>(totalA), static_cast<long long>(worstB), static_cast<long long>(totalB));
  expect(totalB > 0 && totalB >= totalA * 9 / 10 && totalB <= totalA * 11 / 10, "the same work in all");
  expect(worstB * 3 <= worstA, "no block does more than a third of what one does with whole hops");
  // Its start: the seek is not in the block that begins the stream, and start() and prime() make the same stream.
  Tone tc, td, te;
  tc.x = td.x = te.x = a.x;
  stretch::Voice vc, vd, ve;
  vc.prepare(2, kSr, 8 * toFrames, 8 * toFrames, false, stretch::Engine::Stretch, toFrames / 8, kSrc, true);
  vd.prepare(2, kSr, 8 * toFrames, 8 * toFrames, false, stretch::Engine::Stretch, toFrames / 8, kSrc, true);
  ve.prepare(2, kSr, 8 * toFrames, 8 * toFrames, false, stretch::Engine::Stretch, toFrames / 8, kSrc);
  tc.pos = td.pos = te.pos = 20000 - pre;
  int64_t s0 = stretch::spectralStepsRun();
  ve.startLater(&readTone, &te, speed, pitch, pre);
  const int64_t plainSeek = stretch::spectralStepsRun() - s0;
  s0 = stretch::spectralStepsRun();
  vc.start(&readTone, &tc, speed, pitch, pre);
  const int64_t startAll = stretch::spectralStepsRun() - s0;
  s0 = stretch::spectralStepsRun();
  vd.startLater(&readTone, &td, speed, pitch, pre);
  const int64_t spreadSeek = stretch::spectralStepsRun() - s0;
  std::printf("# steps in the block a stream begins in: whole seek %lld, in pieces %lld; all its start-up %lld\n", static_cast<long long>(plainSeek), static_cast<long long>(spreadSeek), static_cast<long long>(startAll));
  expect(plainSeek > 10, "a whole seek runs engine steps in the block it begins in");
  expect(spreadSeek == 0, "in pieces, beginning a stream runs no engine step");
  int64_t worstPrime = 0;
  int primes = 0;
  for (bool ready = false; !ready && primes < 100000; ++primes) {
    s0 = stretch::spectralStepsRun();
    ready = vd.prime(&readTone, &td, speed, pitch, 512);
    worstPrime = std::max(worstPrime, stretch::spectralStepsRun() - s0);
  }
  expect(vd.primeLeft(speed) == 0, "the start-up is done");
  expect(primes >= 10 && worstPrime * 3 <= startAll, "it runs in many small pieces");
  std::vector<float> lc(512), rc(512), ld(512), rd(512);
  float* oc[2] = {lc.data(), rc.data()};
  float* od[2] = {ld.data(), rd.data()};
  bool sameStream = true;
  for (int k = 0; k < 40; ++k) {
    vc.render(oc, 512, speed, pitch, &readTone, &tc);
    vd.render(od, 512, speed, pitch, &readTone, &td);
    sameStream = sameStream && lc == ld && rc == rd;
  }
  expect(sameStream, "a primed spreading stream is identical to one started at once");
  double e = 0, d = 0;
  double ea = 0;
  for (size_t i = 4000; i < la.size(); ++i) {
    e += static_cast<double>(lb[i]) * lb[i];
    ea += static_cast<double>(la[i]) * la[i];
    d += static_cast<double>(la[i] - lb[i]) * (la[i] - lb[i]);
  }
  const double residualDb = 10 * std::log10((d + 1e-20) / (ea + 1e-20));
  std::printf("# shared out against whole hops: level %.3f dB, residual %.1f dB\n", 10 * std::log10((e + 1e-20) / (ea + 1e-20)), residualDb);
  expect(ea > 1, "there is sound");
  expect(std::fabs(10 * std::log10((e + 1e-20) / (ea + 1e-20))) < 0.05, "the same level");
  expect(residualDb < -40, "the same frames at the same place");
}

struct Fix {
  Song song;
  ObjectId chan = kNoId;
};

Fix make(const std::string& wav, int clips, double stretchBy, Tick firstAt = 0) {
  Fix f;
  f.song = makeEmptySong();
  Song& s = f.song;
  Channel ch;
  ch.id = s.allocateId();
  ch.name = "take";
  ch.plugin = defaultPluginState("gl.sampler");
  ch.plugin.data = wav;
  ch.plugin.params[static_cast<size_t>(pp::SmpStretchMode)] = static_cast<float>(stretch::kStretch);
  ch.mixerTrack = s.masterId();
  ch.audioClip = true;
  s.channels.push_back(ch);
  f.chan = ch.id;
  s.tracks.resize(std::max<size_t>(s.tracks.size(), 4));
  const Tick bar = s.barTicks();
  for (int i = 0; i < clips; ++i) {
    Clip k;
    k.id = s.allocateId(), k.kind = ClipKind::Audio, k.source = f.chan, k.track = i % 4;
    k.start = firstAt + i * bar, k.length = bar, k.stretch = stretchBy;
    s.playlist.push_back(k);
  }
  return f;
}

// The block a clip's realtime stretch starts in holds a small part of its start-up work.
void clipStartsWithoutASpike(const std::string& wav96) {
  sampleCache().clear();
  Fix f = make(wav96, 1, 1.25, makeEmptySong().barTicks() * 2);
  ProgramBuilder b;
  b.setSampleRate(kSr);
  b.setAsyncSamples(true);
  auto prog = b.build(f.song);
  while (sampleCache().pending() > 0) stretch::waitForPendingRenders();
  stretch::waitForPendingRenders();
  prog = b.build(f.song);
  const ProgChannel* pc = nullptr;
  for (const ProgChannel& c : prog->channels)
    if (!c.audio.empty()) pc = &c;
  expect(pc != nullptr && pc->audio.size() == 1 && pc->audio[0].rt, "the clip has a realtime voice");
  if (!pc || pc->audio.empty() || !pc->audio[0].rt) return;
  // As if its render were still on its way: a copy of the clip without it.
  std::vector<ProgAudioClip> clips;
  ProgAudioClip c = ProgAudioClip();
  const ProgAudioClip& src = pc->audio[0];
  c.start = src.start, c.end = src.end, c.startSec = src.startSec, c.durSec = src.durSec, c.sample = src.sample;
  c.srcStart = src.srcStart, c.srcEnd = src.srcEnd, c.speed = src.speed, c.secPerTick = src.secPerTick, c.pitch = src.pitch;
  c.granular = src.granular, c.engine = src.engine, c.clip = src.clip, c.rt = src.rt, c.rtGen = src.rtGen;
  c.gain = 1.f;
  clips.push_back(std::move(c));
  ClipVoice* v = clips[0].rt.get();
  constexpr int kBlock = 512;
  std::vector<float> L(kBlock), R(kBlock);
  int64_t worst = 0, before = 0, total = 0;
  bool startedAhead = false, sound = false;
  const int64_t first = static_cast<int64_t>(std::ceil(prog->tempoMap.samplesBetween(0, static_cast<double>(clips[0].start), kSr)));
  for (int64_t clock = first - 30 * kBlock; clock < first + 60 * kBlock; clock += kBlock) {
    std::fill(L.begin(), L.end(), 0.f), std::fill(R.begin(), R.end(), 0.f);
    const int64_t was = v->voice.fedFrames();
    renderAudioClips(clips, prog->tempoMap, kSr, 0.0, clock, 0, kBlock, L.data(), R.data(), 0.0);
    const int64_t now = v->voice.fedFrames();
    const int64_t did = now >= was ? now - was : now;
    worst = std::max(worst, did);
    total += did;
    if (clock + kBlock <= first) before = total;
    startedAhead = startedAhead || (clock + kBlock <= first && did > 0);
    for (float x : L) sound = sound || std::fabs(x) > 0.02f;
  }
  const int pre = v->voice.prerollFor(clips[0].speed * 96000.0 / kSr);
  expect(sound, "the clip plays through its realtime stretch");
  expect(startedAhead, "its stream starts before the clip");
  expect(before > pre / 2, "most of its start-up work is done ahead of the clip's first block");
  expect(worst < pre, "no block does the whole pre-roll");
}

size_t countVoices(const Program& p) {
  size_t n = 0;
  for (const ProgChannel& c : p.channels)
    for (const ProgAudioClip& a : c.audio) n += a.rt ? 1 : 0;
  return n;
}

// A playlist solo and its undoing, again and again: no new voices, the same renders.
void soloKeepsVoicesAndRenders(const std::string& wav) {
  sampleCache().clear();
  Fix f = make(wav, 16, 1.25);
  Document doc(f.song);
  ProgramBuilder b;
  b.setSampleRate(kSr);
  b.setAsyncSamples(true);
  auto prog = b.build(doc.song());
  while (sampleCache().pending() > 0) stretch::waitForPendingRenders();
  prog = b.build(doc.song());
  stretch::waitForPendingRenders();
  prog = b.build(doc.song());
  expect(countVoices(*prog) == 16, "sixteen clips with a voice each");
  std::vector<const stretch::RenderedClip*> renders;
  for (const ProgChannel& c : prog->channels)
    for (const ProgAudioClip& a : c.audio) renders.push_back(a.hq.get());
  expect(renders.size() == 16 && renders[0] != nullptr, "... and a render each");
  const uint64_t made = b.voicesMade();
  for (int round = 0; round < 6; ++round) {
    const bool on = round % 2 == 0;
    doc.editSong(on ? "solo playlist track" : "unsolo playlist track", [on](Song& x) {
      for (size_t k = 0; k < x.tracks.size(); ++k) x.tracks[k].muted = on && k != 0;
    });
    prog.reset();  // (the old program is retired: only what the builder keeps stays alive)
    prog = b.build(doc.song());
    size_t clips = 0;
    for (const ProgChannel& c : prog->channels) clips += c.audio.size();
    expect(clips == (on ? 4u : 16u), "the solo plays the clips of one track");
  }
  expect(b.voicesMade() == made, "soloing and unsoloing makes no new realtime voice");
  std::vector<const stretch::RenderedClip*> again;
  for (const ProgChannel& c : prog->channels)
    for (const ProgAudioClip& a : c.audio) again.push_back(a.hq.get());
  std::sort(renders.begin(), renders.end()), std::sort(again.begin(), again.end());
  expect(renders == again, "... and every clip has the same render back");
  // A clip deleted while muted lets go of what it held.
  doc.editSong("solo playlist track", [](Song& x) {
    for (size_t k = 1; k < x.tracks.size(); ++k) x.tracks[k].muted = true;
  });
  prog = b.build(doc.song());
  const ObjectId gone = doc.song().playlist.back().id;
  const stretch::RenderedClipPtr unused;
  doc.editSong("delete clip", [gone](Song& x) {
    x.playlist.erase(std::remove_if(x.playlist.begin(), x.playlist.end(), [gone](const Clip& c) { return c.id == gone; }), x.playlist.end());
  });
  prog = b.build(doc.song());
  doc.editSong("unsolo playlist track", [](Song& x) {
    for (auto& t : x.tracks) t.muted = false;
  });
  prog = b.build(doc.song());
  expect(countVoices(*prog) == 15, "a deleted clip is gone");
}

// Background renders on several workers, in any order, against the same renders made one by one.
void workersRenderTheSame(const std::string& wav) {
  sampleCache().clear();
  stretch::trimClipCache();
  const SamplePtr smp = sampleCache().get(wav);
  expect(smp != nullptr, "the source loads");
  if (!smp) return;
  auto request = [&](int k) {
    stretch::ClipRequest r;
    r.sample = smp;
    r.srcStart = 0.1 * k, r.srcEnd = 0.1 * k + 1.0;
    r.mode = stretch::kStretch;
    r.speed = 0.5 + 0.05 * k, r.pitch = 1.0 + 0.01 * k;
    r.rate = kSr;
    r.frames = static_cast<int64_t>((r.srcEnd - r.srcStart) / r.speed * kSr);
    r.atTick = 100.0 * (12 - k);
    return r;
  };
  stretch::setRenderFocus(500);
  std::vector<stretch::RenderedClipPtr> async;
  for (int k = 0; k < 12; ++k) async.push_back(stretch::renderClip(request(k), true));
  stretch::waitForPendingRenders();
  std::vector<std::vector<float>> copies;
  for (const auto& a : async) copies.push_back(a->L);
  async.clear();
  stretch::trimClipCache();
  stretch::setRenderFocus(0);
  bool same = true, sound = false;
  for (int k = 0; k < 12; ++k) {
    const stretch::RenderedClipPtr one = stretch::renderClip(request(k), false);
    same = same && one->L == copies[static_cast<size_t>(k)];
    for (float x : one->L) sound = sound || std::fabs(x) > 0.05f;
  }
  expect(same && sound, "renders on the workers equal renders made one at a time");
}

// A channel solo edits every channel: no tempo map work for any of them.
void editsThatCannotMoveTheTempo(const std::string& wav) {
  Fix f = make(wav, 2, 1.0);
  Document doc(f.song);
  const uint64_t t0 = doc.tempoMapBuilds();
  doc.beginGroup("solo channel");
  doc.setParam(channelParam(f.chan, ChannelParam::Mute), 1, false);
  doc.setParam(channelParam(f.chan, ChannelParam::Mute), 0, false);
  doc.endGroup();
  doc.editChannel(f.chan, "name", [](Channel& c) { c.name = "x"; });
  doc.editMixerTrack(doc.song().masterId(), "solo mixer track", [](MixerTrack& m) { m.solo = !m.solo; });
  expect(doc.tempoMapBuilds() == t0, "a channel or mixer edit does no tempo map work");
  doc.setParam(songParam(SongParam::Tempo), 150, false);
  expect(doc.tempoMapBuilds() > t0, "a tempo edit still does");
  const double beatSecBefore = doc.song().clipLength(doc.song().playlist[0]);
  expect(beatSecBefore > 0, "... and the clips still fit it");
}

// Realtime playback with the idle channel and settled effects left out sounds as a render does.
void restingKeepsTheOutput(const std::string& wav) {
  sampleCache().clear();
  // 30 seconds of nothing, then a clip through EQ, delay and reverb on an insert.
  Song s = makeEmptySong();
  Channel ch;
  ch.id = s.allocateId();
  ch.name = "take";
  ch.plugin = defaultPluginState("gl.sampler");
  ch.plugin.data = wav;
  ch.audioClip = true;
  MixerTrack& m = s.mixer[1];
  ch.mixerTrack = m.id;
  m.slots[0].plugin = defaultPluginState("gl.eq");
  m.slots[0].enabled = true;
  m.slots[1].plugin = defaultPluginState("gl.delay");
  m.slots[1].enabled = true;
  m.slots[2].plugin = defaultPluginState("gl.reverb");
  m.slots[2].enabled = true;
  const ObjectId track = m.id;
  s.channels.push_back(ch);
  const Tick bar = s.barTicks();
  Clip k;
  k.id = s.allocateId(), k.kind = ClipKind::Audio, k.source = ch.id, k.track = 0;
  k.start = 20 * bar, k.length = 2 * bar;
  s.playlist.push_back(k);
  const double total = s.tempo > 0 ? 26.0 * 4 * 60.0 / s.tempo : 60.0;
  const int frames = static_cast<int>(total * kSr);

  auto run = [&](bool offline, std::vector<float>& outL, std::vector<float>& outR, ProgramBuilder& b, AudioEngine& eng) {
    b.setSampleRate(kSr);
    auto prog = b.build(s);
    eng.setProgram(prog);
    eng.setOffline(offline);
    eng.setFixedBlock(AudioEngine::kOfflineBlock);
    eng.setLooping(false);
    eng.setMode(PlayMode::Song);
    eng.play();
    outL.assign(static_cast<size_t>(frames), 0.f), outR.assign(static_cast<size_t>(frames), 0.f);
    for (int at = 0; at < frames; at += 512) {
      const int n = std::min(512, frames - at);
      eng.process(outL.data() + at, outR.data() + at, n);
    }
  };
  std::vector<float> aL, aR, bL, bR;
  ProgramBuilder ba, bb;
  AudioEngine ea(kSr), eb(kSr);
  run(true, aL, aR, ba, ea);
  // Halfway through the silence the realtime engine's effects must have stopped running.
  run(false, bL, bR, bb, eb);
  double worst = 0, peak = 0;
  for (size_t i = 0; i < aL.size(); ++i) {
    worst = std::max(worst, std::fabs(static_cast<double>(aL[i]) - bL[i]));
    worst = std::max(worst, std::fabs(static_cast<double>(aR[i]) - bR[i]));
    peak = std::max(peak, std::fabs(static_cast<double>(aL[i])));
  }
  expect(peak > 0.05, "the clip sounds");
  expect(worst < 1e-5, "realtime with resting channels and effects equals the render");
  // With nothing to play, their counters stop growing at the rest value only if they are left out.
  ProgramBuilder bc;
  AudioEngine ec(kSr);
  Song quiet = s;
  quiet.playlist.clear();
  bc.setSampleRate(kSr);
  ec.setProgram(bc.build(quiet));
  ec.setMode(PlayMode::Song);
  ec.setLooping(false);
  ec.play();
  std::vector<float> L(512), R(512);
  for (int i = 0; i < 2000; ++i) ec.process(L.data(), R.data(), 512);  // 23 seconds
  auto qe = bc.effectRuntime(track, 0);
  auto qd = bc.effectRuntime(track, 1);
  auto qr = bc.effectRuntime(track, 2);
  auto qc = bc.channelRuntime(ch.id);
  const int64_t limit = 15 * kSr;
  expect(qe && qe->quiet > 0 && qe->quiet <= 3 * kSr + 512, "an EQ with no sound rests after 3 seconds");
  expect(qd && qd->quiet > 0 && qd->quiet <= limit, "a delay with no sound rests");
  expect(qr && qr->quiet > 0 && qr->quiet <= 6 * kSr + 512, "a reverb with no sound rests");
  expect(qc && qc->outZero, "an idle sampler channel is left out of the block");
  // A modulated delay, or a track where something plays, keeps running.
  PluginState mod = defaultPluginState("gl.delay");
  mod.params[static_cast<size_t>(pp::DlyModel)] = 0.f;
  mod.params[static_cast<size_t>(pp::DlyModDepth)] = 0.5f;
  Song modded = quiet;
  modded.mixerTrack(track)->slots[1].plugin = mod;
  ProgramBuilder bd;
  AudioEngine ed(kSr);
  bd.setSampleRate(kSr);
  ed.setProgram(bd.build(modded));
  ed.setMode(PlayMode::Song);
  ed.setLooping(false);
  ed.play();
  for (int i = 0; i < 2000; ++i) ed.process(L.data(), R.data(), 512);
  auto dm = bd.effectRuntime(track, 1);
  expect(dm && dm->quiet == 0, "a delay with free-running modulation never rests");
  // Offline renders run everything.
  ProgramBuilder be;
  AudioEngine ee(kSr);
  be.setSampleRate(kSr);
  ee.setOffline(true);
  ee.setProgram(be.build(quiet));
  ee.setFixedBlock(AudioEngine::kOfflineBlock);
  ee.setMode(PlayMode::Song);
  ee.setLooping(false);
  ee.play();
  for (int i = 0; i < 2000; ++i) ee.process(L.data(), R.data(), 512);
  auto oe = be.effectRuntime(track, 0);
  expect(oe && oe->quiet == 0, "offline renders leave nothing out");
}

}  // namespace

int main() {
  const std::string wav = writeTone("k64_tone.wav", kSr, 3.0);
  const std::string wav96 = writeTone("k64_tone96.wav", 96000, 3.0);
  primedStretchIsTheSame();
  clipStartsWithoutASpike(wav96);
  spreadVoiceIsTheSameAndEven();
  soloKeepsVoicesAndRenders(wav);
  workersRenderTheSame(wav);
  editsThatCannotMoveTheTempo(wav);
  restingKeepsTheOutput(wav);
  stretch::shutdownClipRenderer();
  return finish();
}
