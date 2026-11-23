// Gridline - K-106 cp18 (K-316 plan 7.3, 8.2): the Tracker Sampler's play tap and the preview of a
// part of a sample.
//   * a played note shows in the tap: its slot, a position that moves on at the note's rate, its note;
//   * nothing is allocated while the generator renders and publishes (operator new counted);
//   * a released note leaves the tap empty once its sound has ended; a generator that goes leaves none;
//   * another instrument's tap is not read as this one's;
//   * the editor's preview of frames from .. to starts at `from` and ends by `to`;
//   * 0.7.8: the tapped position is the frame the voice reads: it walks backwards on a ping-pong loop's
//     backward pass (before: the forward count, so the editor's line ran on to the right), forwards
//     through a reversed sample, inside a cut; checked block by block against the frame the voice
//     reads there and against the way the audio played moves.
//   * 0.7.8 (the Backwards option): a sample played backwards (the audio as it is) reads from its end:
//     with no loop, a forward loop, ping-pong, inside a cut, the tap is the frame read (it walks back),
//     the sound heard is that frame's; a preview of a part plays it from its end; the option survives
//     the instrument text and a text without it reads as before.
//   * 0.7.8: Cut after loop / Minimise follow the direction: on a sample played backwards the part
//     before the loop goes, and the sound is the same before and after (it never reached that part);
//     a forward sample loses the part after its loop and sounds the same, as before.
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <cmath>
#include <cstdlib>
#include <new>
#include <string>
#include <vector>
#include "../../src/core/PluginInfo.h"
#include "../../src/core/TrackerData.h"
#include "../../src/core/WavWriter.h"
#include "../../src/engine/Generators.h"
#include "../../src/engine/Sample.h"
#include "../../src/packs/Packs.h"
#include "../../src/packs/tracker/tracker.h"
#include "../Check.h"

namespace {
std::atomic<long long> gNews{0};
}
void* operator new(std::size_t n) {
  gNews.fetch_add(1, std::memory_order_relaxed);
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) {
  gNews.fetch_add(1, std::memory_order_relaxed);
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

using namespace gl;
namespace trk = gl::tracker;
namespace tp = gl::packs::trackerpack;

namespace {

void expect(bool ok, const std::string& name) { expect(ok, name.c_str()); }

struct Rig {
  PluginState st;
  ChannelAux aux;
  std::unique_ptr<Generator> g;
  ProcessContext ctx;
  std::vector<float> L, R;
  const tp::InstrumentAux* inst = nullptr;
  explicit Rig(int sound) {
    st = defaultPluginState(trk::kInstrumentType);
    st.params[trk::PSound] = static_cast<float>(sound);
    st.params[trk::PRamp] = 0;
    g = packs::createGenerator(st.type);
    packs::prepareAux(st, aux);
    inst = dynamic_cast<const tp::InstrumentAux*>(aux.custom.get());
    ctx.sampleRate = 44100;
    ctx.tempo = 125;
    ctx.ppq = 96;
    L.assign(4096, 0.0f);
    R.assign(4096, 0.0f);
  }
  void run(int n) {
    for (int done = 0; done < n;) {
      const int m = std::min(256, n - done);
      g->process(st, aux, ctx, L.data(), R.data(), m);
      done += m;
    }
  }
};

void testTap() {
  Rig r(1);
  expect(r.inst != nullptr, "rig: the built-in sound's instrument");
  tp::TapVoice v[tp::kTapVoices];
  r.run(512);
  expect(tp::readPlayTap(r.inst, v, tp::kTapVoices) == 0, "idle: no voice in the tap");
  const long long pub0 = tp::playTapPublished();
  r.g->noteOn(1, 60, 100.0f / 127.0f, 0);
  r.run(256);
  int n = tp::readPlayTap(r.inst, v, tp::kTapVoices);
  expect(n == 1, "a note: one voice in the tap");
  expect(tp::playTapPublished() > pub0, "the generator published");
  const int32_t p1 = n == 1 ? v[0].pos : -1;
  expect(n == 1 && v[0].slot == 0, "the voice's slot");
  expect(n == 1 && v[0].note == 60 - trk::kPitchOffset, "the voice's note (C-4 as the tracker numbers it)");
  // C-4 of a built-in sound plays middle C; its position moves on with the frames rendered.
  const long long newsBefore = gNews.load();
  r.run(2048);
  const long long newsAfter = gNews.load();
  expect(newsAfter == newsBefore, "rendering and publishing allocate nothing (" + std::to_string(newsAfter - newsBefore) + " news)");
  n = tp::readPlayTap(r.inst, v, tp::kTapVoices);
  expect(n == 1 && v[0].pos > p1, "the position moves on");
  // cp20: where the voice is on its envelopes (the Instrument editor's play lines): a tick on the volume
  // envelope's axis while it is on, -1 for an envelope that is off.
  {
    const tp::TapVoice before = v[0];
    r.run(4096);
    n = tp::readPlayTap(r.inst, v, tp::kTapVoices);
    expect(n == 1 && before.volTick >= 0, "envelope tap: the volume envelope's tick (" + std::to_string(before.volTick) + ")");
    expect(n == 1 && v[0].volTick >= before.volTick, "envelope tap: it moves on or holds at the sustain point (" + std::to_string(v[0].volTick) + ")");
    expect(n == 1 && v[0].panTick >= -1, "envelope tap: the panning envelope's tick or -1");
  }
  // Another instrument is never read as this one.
  Rig other(2);
  other.g->noteOn(1, 64, 0.8f, 0);
  other.run(256);
  tp::TapVoice w[tp::kTapVoices];
  const int no = tp::readPlayTap(other.inst, w, tp::kTapVoices);
  n = tp::readPlayTap(r.inst, v, tp::kTapVoices);
  expect(no == 1 && n == 1 && w[0].note == 64 - trk::kPitchOffset, "two instruments: each reads its own voice");
  // Released: the voice fades and the tap empties.
  r.g->noteOff(1, 0);
  r.run(44100 * 3);
  expect(tp::readPlayTap(r.inst, v, tp::kTapVoices) == 0, "released and faded: the tap is empty");
  // The generator goes: its tap with it.
  other.g.reset();
  expect(tp::readPlayTap(other.inst, w, tp::kTapVoices) == 0, "a generator gone leaves no voices");
}

void testPartPreview() {
  Rig r(3);  // (the struck tine: 1.6 s, no loop)
  const int pitch = 60;
  const int32_t len = r.inst->text.def.samples[0].length;
  expect(len > 4000, "the built-in sound has frames to play a part of");
  const int32_t from = 1000, to = 1600;
  const uint32_t id = tp::requestAudition(r.inst, 0, pitch, from, to);
  r.g->noteOn(7, pitch, 100.0f / 127.0f, 0);
  r.run(64);
  tp::TapVoice v[tp::kTapVoices];
  int n = tp::readPlayTap(r.inst, v, tp::kTapVoices);
  expect(tp::auditionPlaying(id), "the part's preview took the note");
  expect(n == 1 && v[0].pos >= from && v[0].pos < from + 200, "the preview starts at `from` (" + std::to_string(n ? v[0].pos : -1) + ")");
  r.run(4096);
  n = tp::readPlayTap(r.inst, v, tp::kTapVoices);
  expect(n == 0, "the preview stops by `to`");
  expect(!tp::auditionSounding(id), "the preview's audition ended");
  r.g->noteOff(7, 0);
}

// ---- 0.7.8: the tap's position on backward passes ----

namespace fs = std::filesystem;

// A ramp file (its right channel steady), the sound the walks below play.
std::string rampFile(const std::string& name, int n, bool reversed) {
  const fs::path d = fs::temp_directory_path() / "gridline-k078-playtap";
  std::error_code ec;
  fs::create_directories(d, ec);
  // Stereo: the right channel a steady 0.5, so left / right reads the frame whatever the note's gain.
  AudioBuffer b(2, static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) {
    const int f = reversed ? n - 1 - i : i;
    b.data[static_cast<size_t>(i)] = 0.05f + 0.9f * static_cast<float>(f) / static_cast<float>(n);
    b.data[static_cast<size_t>(n + i)] = 0.5f;
  }
  const std::string p = (d / name).u8string();
  writeWav(p, b, 44100, 24);
  return p;
}

struct Walk {
  int blocks = 0, wrong = 0, backSteps = 0, fwdSteps = 0, worst = 0;
  int heardWrong = 0, heardWorst = 0;  // backwards: the frame heard (from the sound) against the tap
  std::string first;
};

// The rate C-4 plays with no relative note or fine tune: a file at it needs no rounded tuning, so the
// voice's step is known exactly.
constexpr int kRampRate = 8363;

// Plays C-4 (the file at its own rate) block by block; after each block the tapped position plus the
// cut start (file frames) must be within two frames of the frame the voice read last.
// `backwards`: the sample's Backwards option (the model then reads the played part mirrored: frame t of
// the mirrored part is frame len - 1 - t, its loop mirrored with it).
Walk walk(const std::string& path, int n, uint8_t loop, int32_t ls, int32_t ll, int32_t cut0, int32_t cut1, int blocks, bool backwards = false) {
  Walk w;
  tp::Edit e = tp::readEdit("", 0);
  e.text.def.vol.flags = 0;
  e.text.def.fadeout = 0;
  tp::addSample(e, path, n, kRampRate, "ramp");
  e.cuts.start[0] = cut0;
  e.cuts.end[0] = cut1;
  e.text.def.samples[0].loop = loop;
  e.text.def.samples[0].loopStart = ls;
  e.text.def.samples[0].loopLength = ll;
  e.text.def.samples[0].reverse = backwards;
  PluginState st = defaultPluginState(trk::kInstrumentType);
  st.params[trk::PSound] = 0;
  st.params[trk::PInterp] = trk::InterpNone;
  st.params[trk::PRamp] = 0;
  st.data = tp::writeEdit(e);
  auto g = packs::createGenerator(st.type);
  ChannelAux aux;
  packs::prepareAux(st, aux);
  const auto* inst = dynamic_cast<const tp::InstrumentAux*>(aux.custom.get());
  if (!inst || !inst->data[0]) {
    w.first = "no instrument";
    w.wrong = 1;
    return w;
  }
  ProcessContext ctx;
  ctx.sampleRate = 44100;
  ctx.tempo = 125;
  ctx.ppq = 96;
  const int B = 128;
  std::vector<float> L(B), R(B);
  g->noteOn(1, 60, 100.0f / 127.0f, 0);
  // The frame a voice reads after t file frames (C-4 plays the file at its own rate: kRampRate file
  // frames a second, exactly, with no tuning), counted from the cut start: on to the loop's end, then round the loop (forward) or to and
  // fro (ping-pong: the backward pass reads the loop from its end down to its start).
  const int64_t len = (cut1 > cut0 ? cut1 : n) - cut0, s0 = backwards ? len - (ls - cut0 + ll) : ls - cut0;
  auto model0 = [&](int64_t t) -> int {
    if (loop == trk::kLoopOff || ll <= 0) return static_cast<int>(std::min(t, len - 1));
    if (t < s0 + ll) return static_cast<int>(t);
    if (loop == trk::kLoopForward) return static_cast<int>(s0 + (t - s0) % ll);
    const int64_t u = (t - s0) % (2 * static_cast<int64_t>(ll));
    return static_cast<int>(u < ll ? s0 + u : s0 + 2 * ll - 1 - u);
  };
  auto model = [&](int64_t t) -> int { return backwards ? static_cast<int>(len - 1 - model0(t)) : model0(t); };
  int last = -1;
  for (int b = 0; b < blocks; ++b) {
    std::fill(L.begin(), L.end(), 0.0f);  // (the generator adds into the buffers)
    std::fill(R.begin(), R.end(), 0.0f);
    g->process(st, aux, ctx, L.data(), R.data(), B);
    tp::TapVoice v[tp::kTapVoices];
    if (tp::readPlayTap(inst, v, tp::kTapVoices) != 1) break;
    const int tapped = cut0 + v[0].pos;
    const int frame = cut0 + model(static_cast<int64_t>(std::floor(static_cast<double>(b + 1) * B * kRampRate / 44100.0)));
    const int err = std::abs(tapped - frame);
    w.worst = std::max(w.worst, err);
    if (err > 2) {
      if (w.first.empty()) w.first = "block " + std::to_string(b) + ": tapped " + std::to_string(tapped) + ", played " + std::to_string(frame);
      ++w.wrong;
    }
    if (backwards) {
      // The sound of the block's last frame: the ramp's value (left over the steady right) gives the
      // file frame heard; it is the tapped frame, or the one just before it in the walk (one step).
      const double lr = R[B - 1] != 0.0f ? static_cast<double>(L[B - 1]) / R[B - 1] * 0.5 : 0.0;
      const int heard = static_cast<int>(std::lround((lr - 0.05) / 0.9 * n));
      const int herr = std::abs(heard - tapped);
      w.heardWorst = std::max(w.heardWorst, herr);
      if (herr > 3 && !(loop != trk::kLoopOff && herr > ll - 4)) {  // (a loop's seam: the step wraps)
        if (w.first.empty()) w.first = "block " + std::to_string(b) + ": tapped " + std::to_string(tapped) + ", heard " + std::to_string(heard);
        ++w.heardWrong;
      }
    }
    if (last >= 0 && tapped < last) ++w.backSteps;
    if (last >= 0 && tapped > last) ++w.fwdSteps;
    last = tapped;
    ++w.blocks;
  }
  return w;
}

void testBackwards() {
  const int n = 6000;
  const std::string fwd = rampFile("ramp.wav", n, false), rev = rampFile("ramp-reversed.wav", n, true);
  auto report = [](const Walk& w) {
    return " (" + std::to_string(w.blocks) + " blocks, " + std::to_string(w.wrong) + " wrong, worst " + std::to_string(w.worst) + " frames, " +
           std::to_string(w.fwdSteps) + " forward, " + std::to_string(w.backSteps) + " back" + (w.first.empty() ? "" : "; " + w.first) + ")";
  };
  // Forward, no loop: the sample plays once (6000 frames: some 250 blocks).
  Walk w = walk(fwd, n, trk::kLoopOff, 0, 0, 0, 0, 800);
  expect(w.blocks >= 40 && w.wrong == 0 && w.backSteps == 0, "forward: the tap is the frame played" + report(w));
  // A forward loop: the line jumps back to the loop start, then on again.
  w = walk(fwd, n, trk::kLoopForward, 2000, 2000, 0, 0, 800);
  expect(w.blocks == 800 && w.wrong == 0, "forward loop: the tap is the frame played" + report(w));
  // Ping-pong: on every backward pass the tap walks back (before, it ran on forwards).
  w = walk(fwd, n, trk::kLoopPingPong, 2000, 2000, 0, 0, 800);
  expect(w.blocks == 800 && w.wrong == 0, "ping-pong: the tap is the frame played on both passes" + report(w));
  expect(w.backSteps > 40 && w.fwdSteps > 40, "ping-pong: the tap walks back on the backward passes" + report(w));
  // Ping-pong inside a cut (the tap counts from the sample start; file frames = cut start + tap).
  w = walk(fwd, n, trk::kLoopPingPong, 3000, 1500, 1000, 5000, 800);
  expect(w.blocks == 800 && w.wrong == 0 && w.backSteps > 40, "ping-pong in a cut: the tap is the frame played" + report(w));
  // A reversed sample (the Reverse edit writes the audio backwards): it plays from its start, so the
  // tap moves on through the reversed file, at the frame played.
  w = walk(rev, n, trk::kLoopOff, 0, 0, 0, 0, 800);
  expect(w.blocks >= 40 && w.wrong == 0 && w.backSteps == 0, "reversed sample: the tap is the frame played" + report(w));
  // Reversed with a ping-pong loop.
  w = walk(rev, n, trk::kLoopPingPong, 1000, 3000, 0, 0, 800);
  expect(w.blocks == 800 && w.wrong == 0 && w.backSteps > 40, "reversed, ping-pong: the tap is the frame played" + report(w));

  // 0.7.8: the Backwards option (the audio as it is, read from its end).
  auto heard = [](const Walk& x) { return " (heard " + std::to_string(x.heardWrong) + " wrong, worst " + std::to_string(x.heardWorst) + ")"; };
  // No loop: from the last frame down to the first, the line walking left all the way.
  w = walk(fwd, n, trk::kLoopOff, 0, 0, 0, 0, 800, true);
  expect(w.blocks >= 40 && w.blocks < 800 && w.wrong == 0 && w.fwdSteps == 0 && w.backSteps >= 40, "backwards: the tap walks back from the end" + report(w));
  expect(w.heardWrong == 0, "backwards: the sound is the tapped frame's" + heard(w));
  // A forward loop: the tail after the loop, then the loop from its end down to its start, again and again.
  w = walk(fwd, n, trk::kLoopForward, 2000, 2000, 0, 0, 800, true);
  expect(w.blocks == 800 && w.wrong == 0 && w.backSteps > 10 * w.fwdSteps, "backwards, forward loop: the tap is the frame played, walking back" + report(w));
  expect(w.heardWrong == 0, "backwards, forward loop: the sound is the tapped frame's" + heard(w));
  // Ping-pong: down through the loop, then up, then down.
  w = walk(fwd, n, trk::kLoopPingPong, 2000, 2000, 0, 0, 800, true);
  expect(w.blocks == 800 && w.wrong == 0 && w.backSteps > 40 && w.fwdSteps > 40, "backwards, ping-pong: the tap is the frame played on both passes" + report(w));
  expect(w.heardWrong == 0, "backwards, ping-pong: the sound is the tapped frame's" + heard(w));
  // Inside a cut, with a ping-pong loop off centre.
  w = walk(fwd, n, trk::kLoopPingPong, 1500, 1000, 1000, 5000, 800, true);
  expect(w.blocks == 800 && w.wrong == 0 && w.backSteps > 40, "backwards in a cut: the tap is the frame played" + report(w));
  expect(w.heardWrong == 0, "backwards in a cut: the sound is the tapped frame's" + heard(w));
}

// 0.7.8: the option in the instrument text: written as its own line, read back; a text without it is
// the text it was (old projects read and write the same).
void testBackwardsText() {
  tp::Edit e = tp::readEdit("", 1);
  const std::string before = tp::writeEdit(e);
  expect(before.find("\nrev ") == std::string::npos, "text: no line for a sample played forwards");
  const std::string path = rampFile("ramp.wav", 6000, false);
  tp::addSample(e, path, 6000, kRampRate, "ramp");
  tp::addSample(e, path, 6000, kRampRate, "ramp two");
  const std::string plain = tp::writeEdit(e);
  e.text.def.samples[1].reverse = true;
  const std::string text = tp::writeEdit(e);
  expect(text != plain && text.find("\nrev 1\n") != std::string::npos, "text: the option as a line of its own");
  const tp::Edit back = tp::readEdit(text, 1);
  expect(back.text.def.samples[1].reverse && !back.text.def.samples[0].reverse, "text: read back on its sample only");
  expect(tp::writeEdit(back) == text, "text: written again the same");
  e.text.def.samples[1].reverse = false;
  expect(tp::writeEdit(e) == plain, "text: switched off, the text is the one before");
  // The Clean up's trim after the loop: played backwards, what is never heard is before the loop.
  tp::Edit t = back;
  t.text.def.samples[1].loop = trk::kLoopForward;
  t.text.def.samples[1].loopStart = 2000;
  t.text.def.samples[1].loopLength = 1000;
  const tp::SampleTrim tr = tp::trimSamples(t, false, true, false);
  expect(tr.trimmed == 1 && t.cuts.start[1] == 2000 && t.cuts.end[1] == 0, "trim: a backwards sample starts at its loop, its end stays");
}

// 0.7.8: a preview of a part of a sample played backwards starts at the part's end and walks down.
void testBackwardsPreview() {
  const int n = 6000;
  const std::string path = rampFile("ramp.wav", n, false);
  tp::Edit e = tp::readEdit("", 0);
  tp::addSample(e, path, n, kRampRate, "ramp");
  e.text.def.samples[0].reverse = true;
  PluginState st = defaultPluginState(trk::kInstrumentType);
  st.params[trk::PSound] = 0;
  st.params[trk::PRamp] = 0;
  st.data = tp::writeEdit(e);
  auto g = packs::createGenerator(st.type);
  ChannelAux aux;
  packs::prepareAux(st, aux);
  const auto* inst = dynamic_cast<const tp::InstrumentAux*>(aux.custom.get());
  expect(inst && inst->data[0], "preview: the instrument");
  if (!inst) return;
  ProcessContext ctx;
  ctx.sampleRate = 44100;
  ctx.tempo = 125;
  ctx.ppq = 96;
  std::vector<float> L(128), R(128);
  const int pitch = tp::auditionPitch(e, 0, kRampRate);
  const uint32_t id = tp::requestAudition(inst, 0, pitch, 1000, 3000);
  g->noteOn(3, pitch, 0.8f, 0);
  g->process(st, aux, ctx, L.data(), R.data(), 64);
  tp::TapVoice v[tp::kTapVoices];
  int c = tp::readPlayTap(inst, v, tp::kTapVoices);
  expect(tp::auditionPlaying(id) && c == 1 && v[0].pos <= 3000 && v[0].pos > 2900, "preview backwards: from the part's end (" + std::to_string(c ? v[0].pos : -1) + ")");
  int32_t lowest = c ? v[0].pos : 0;
  bool down = true;
  for (int b = 0; b < 200; ++b) {
    g->process(st, aux, ctx, L.data(), R.data(), 128);
    c = tp::readPlayTap(inst, v, tp::kTapVoices);
    if (c == 0) break;
    down = down && v[0].pos <= lowest;
    lowest = v[0].pos;
  }
  expect(down && lowest >= 1000, "preview backwards: down to the part's start, never below (" + std::to_string(lowest) + ")");
  expect(c == 0, "preview backwards: it stops at the part's start");
}

// 0.7.8: what a note sounds like (both sides, `blocks` blocks of 128) on a sample from `path`.
struct Played {
  std::vector<float> out;
  bool ok = false;
  tp::Wave wave;  // the sample's wave as the editor reads it (waveOf)
};
Played play(const std::string& path, int n, uint8_t loop, int32_t ls, int32_t ll, bool backwards, int blocks) {
  Played p;
  tp::Edit e = tp::readEdit("", 0);
  e.text.def.vol.flags = 0;
  e.text.def.fadeout = 0;
  tp::addSample(e, path, n, kRampRate, "ramp");
  e.text.def.samples[0].loop = loop;
  e.text.def.samples[0].loopStart = ls;
  e.text.def.samples[0].loopLength = ll;
  e.text.def.samples[0].reverse = backwards;
  PluginState st = defaultPluginState(trk::kInstrumentType);
  st.params[trk::PSound] = 0;
  st.params[trk::PRamp] = 0;
  st.data = tp::writeEdit(e);
  auto g = packs::createGenerator(st.type);
  ChannelAux aux;
  packs::prepareAux(st, aux);
  const auto* inst = dynamic_cast<const tp::InstrumentAux*>(aux.custom.get());
  if (!inst || !inst->data[0] || !tp::waveOf(e, 0, *inst, p.wave)) return p;
  ProcessContext ctx;
  ctx.sampleRate = 44100;
  ctx.tempo = 125;
  ctx.ppq = 96;
  std::vector<float> L(128), R(128);
  g->noteOn(1, 60, 100.0f / 127.0f, 0);
  for (int b = 0; b < blocks; ++b) {
    std::fill(L.begin(), L.end(), 0.0f);
    std::fill(R.begin(), R.end(), 0.0f);
    g->process(st, aux, ctx, L.data(), R.data(), 128);
    p.out.insert(p.out.end(), L.begin(), L.end());
    p.out.insert(p.out.end(), R.begin(), R.end());
  }
  p.ok = true;
  return p;
}

std::string waveFile(const std::string& name, const tp::Wave& w) {
  const fs::path d = fs::temp_directory_path() / "gridline-k078-playtap";
  AudioBuffer b(2, w.L.size());
  for (size_t i = 0; i < w.L.size(); ++i) {
    b.data[i] = w.L[i];
    b.data[w.L.size() + i] = w.stereo ? w.R[i] : w.L[i];
  }
  const std::string p = (d / name).u8string();
  writeWav(p, b, 44100, 24);
  return p;
}

float maxDiff(const std::vector<float>& a, const std::vector<float>& b) {
  if (a.size() != b.size()) return 1e9f;
  float m = 0;
  for (size_t i = 0; i < a.size(); ++i) m = std::max(m, std::fabs(a[i] - b[i]));
  return m;
}

void testMinimiseKeepsTheSound() {
  const int n = 6000, blocks = 600;  // (some 1.7 s: the loop goes round many times)
  const std::string path = rampFile("ramp.wav", n, false);
  for (uint8_t loop : {trk::kLoopForward, trk::kLoopPingPong}) {
    const std::string kind = loop == trk::kLoopForward ? "forward loop" : "ping-pong";
    // Played backwards: Minimise takes the 2000 frames before the loop; the note sounds the same.
    const Played before = play(path, n, loop, 2000, 1500, true, blocks);
    expect(before.ok && before.wave.reverse, "backwards, " + kind + ": the wave knows its direction");
    tp::Wave w = before.wave;
    expect(tp::waveMinimise(w) && w.frames() == n - 2000 && w.loopStart == 0 && w.loopLength == 1500,
           "backwards, " + kind + ": minimise leaves the loop and what follows it");
    const Played after = play(waveFile("min-back.wav", w), w.frames(), loop, w.loopStart, w.loopLength, true, blocks);
    float d = maxDiff(before.out, after.out);
    expect(after.ok && d < 1e-5f, "backwards, " + kind + ": the note sounds the same after Minimise (max difference " + std::to_string(d) + ")");
    // The forward trim (what the edit did before) would have cut what a backwards note plays first.
    tp::Wave old = before.wave;
    old.reverse = false;
    expect(tp::waveMinimise(old) && old.frames() == 3500, "backwards, " + kind + ": (the forward trim keeps the start)");
    const Played wrong = play(waveFile("min-wrong.wav", old), old.frames(), loop, old.loopStart, old.loopLength, true, blocks);
    d = maxDiff(before.out, wrong.out);
    expect(wrong.ok && d > 0.01f, "backwards, " + kind + ": the forward trim would change the sound (max difference " + std::to_string(d) + ")");
    // Forward, as before: the part after the loop goes and the note sounds the same.
    const Played fwd = play(path, n, loop, 2000, 1500, false, blocks);
    tp::Wave fw = fwd.wave;
    expect(fwd.ok && !fw.reverse && tp::waveMinimise(fw) && fw.frames() == 3500 && fw.loopStart == 2000, "forward, " + kind + ": the sample ends with its loop");
    const Played fafter = play(waveFile("min-fwd.wav", fw), fw.frames(), loop, fw.loopStart, fw.loopLength, false, blocks);
    d = maxDiff(fwd.out, fafter.out);
    expect(fafter.ok && d < 1e-5f, "forward, " + kind + ": the note sounds the same after Minimise (max difference " + std::to_string(d) + ")");
  }
}

}  // namespace

int main() {
  testTap();
  testPartPreview();
  testBackwards();
  testBackwardsText();
  testBackwardsPreview();
  testMinimiseKeepsTheSound();
  return finish();
}
