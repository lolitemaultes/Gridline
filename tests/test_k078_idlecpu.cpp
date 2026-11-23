// Gridline: GitHub #66, idle CPU with chip-level emulations (instruments and effects that run their
// whole emulation in every block and never put out exact silence: a noise floor, a little DC, an
// infinite tail). The in-tree VST3 test plugin plays one (GRIDLINE_TEST_CHIP, read as each
// instance is made):
//  * an instrument whose output stays at or below -90 dBFS with nothing playing it rests (after
//    four seconds), and the plugin's process() is no longer called; at -80 dBFS it keeps running;
//    exact silence still rests after two seconds;
//  * a note after the rest plays on its own sample, and the output equals the output with resting
//    off within -90 dB (only the plugin's own inaudible floor differs);
//  * its editor in use, Keep running (Rest silent instruments off) or an offline render: never rests;
//  * a hosted effect fed silence with the transport stopped rests the same way; the transport
//    playing, sound coming in, its editor in use or a louder floor keep it running; sound coming in
//    wakes it in that block and the output equals the output with resting off within -90 dB;
//  * an open plugin window counts as in use only under the pointer or with the keyboard.
// Everything is synthetic; it counts blocks and calls, never time.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>
#include "Check.h"
#include "Hermetic.h"
#include "app/IdlePace.h"
#include "core/Document.h"
#include "core/PluginParams.h"
#include "core/Song.h"
#include "engine/AudioEngine.h"
#include "engine/Program.h"
#include "host/Dylib.h"
#include "host/HostedPlugins.h"
#include "host/PluginScan.h"
#include "host/vst3/Vst3Module.h"
#include "host/vst3/Vst3Scan.h"
#include "vst3/Probe.h"

using namespace gl;

namespace {

constexpr int kSr = 44100;
constexpr int kHost = 256;
const std::string kBundle = GRIDLINE_TESTPLUG_BUNDLE;
const std::string kSynth = "vst3:47524944" "4C494E45" "54535931" "00000001";
const std::string kGain = "vst3:47524944" "4C494E45" "54535931" "00000003";

void setChip(const char* v) {
#if defined(_WIN32)
  _putenv_s("GRIDLINE_TEST_CHIP", v ? v : "");
#else
  if (v) setenv("GRIDLINE_TEST_CHIP", v, 1);
  else unsetenv("GRIDLINE_TEST_CHIP");
#endif
}

GridlineVst3Probe* probe() {
  const std::string bin = vst3::bundleBinaryPath(kBundle);
  void* h = host::dl::loaded(bin);
  if (!h) h = host::dl::open(bin, nullptr, true);
  void* f = h ? host::dl::sym(h, "gridline_vst3_probe") : nullptr;
  return f ? host::toFunction<GridlineVst3Probe* (*)()>(f)() : nullptr;
}

struct Run {
  std::vector<float> L, R;
  uint64_t rested = 0;    // blocks the watched channel / slot was left out
  int64_t lastCall = -1;  // frame at which the plugin last processed (the probe's blocks moved)
};

double worstDiff(const Run& a, const Run& b) {
  double w = 0;
  for (size_t i = 0; i < a.L.size(); ++i)
    w = std::max({w, std::fabs(static_cast<double>(a.L[i]) - b.L[i]), std::fabs(static_cast<double>(a.R[i]) - b.R[i])});
  return w;
}
double peakFrom(const Run& a, size_t from) {
  double p = 0;
  for (size_t i = from; i < a.L.size(); ++i) p = std::max({p, std::fabs(static_cast<double>(a.L[i])), std::fabs(static_cast<double>(a.R[i]))});
  return p;
}
int firstAbove(const Run& a, size_t from, double level) {
  for (size_t i = from; i < a.L.size(); ++i)
    if (std::fabs(a.L[i]) > level) return static_cast<int>(i);
  return -1;
}

// A song with the test synth on channel 1 (mixer track 1), its pattern holding `notes`.
Song synthSong(const std::vector<Note>& notes, ObjectId& ch) {
  Song s = makeEmptySong();
  Channel c;
  c.id = s.allocateId();
  c.name = "Chip";
  c.plugin = defaultPluginState(kSynth);
  c.mixerTrack = s.mixer[1].id;
  s.channels.push_back(c);
  ch = c.id;
  for (const Note& n : notes) s.patterns[0].notes[c.id].push_back(n);
  return s;
}

struct Opts {
  bool rest = true;
  bool playing = false;  // pattern playback (else stopped)
  bool editor = false;   // the watched runtime's editor in use
  bool offline = false;
  int liveAt = -1;       // stopped: a live note on the channel from this frame (half a second)
};

// Realtime processing of `s` for `frames` in host blocks; watches channel `ch` or slot (`fxTrack`, 0).
Run run(const Song& s, int frames, ObjectId ch, ObjectId fxTrack, const Opts& o) {
  ProgramBuilder b;
  b.setSampleRate(kSr);
  AudioEngine e(kSr);
  if (o.offline) e.setOffline(true);
  e.setRestInstruments(o.rest);
  e.setProgram(b.build(s));
  e.setLooping(false);
  e.setMode(PlayMode::Pattern);
  e.setPattern(s.patterns[0].id);
  if (o.playing) e.play();
  auto crt = ch != kNoId ? b.channelRuntime(ch) : nullptr;
  auto frt = fxTrack != kNoId ? b.effectRuntime(fxTrack, 0) : nullptr;
  if (o.editor && crt) crt->editorOpen.store(true);
  if (o.editor && frt) frt->editorOpen.store(true);
  GridlineVst3Probe* pr = probe();
  Run r;
  r.L.assign(static_cast<size_t>(frames), 0.f);
  r.R.assign(static_cast<size_t>(frames), 0.f);
  int32_t calls = pr ? pr->blocks : 0;
  for (int at = 0; at < frames; at += kHost) {
    if (o.liveAt >= 0 && at == o.liveAt) e.noteOn(ch, 69, 0.8f);
    if (o.liveAt >= 0 && at == o.liveAt + kSr / 2 / kHost * kHost) e.noteOff(ch, 69);
    e.process(r.L.data() + at, r.R.data() + at, std::min(kHost, frames - at));
    if (pr && pr->blocks != calls) calls = pr->blocks, r.lastCall = at;
  }
  if (crt) r.rested = crt->restBlocks.load();
  if (frt) r.rested = frt->restBlocks.load();
  return r;
}

void instrumentRests() {
  ObjectId ch = kNoId;
  const Song s = synthSong({}, ch);
  const int frames = 10 * kSr;
  auto restedSecs = [](const Run& r) { return static_cast<double>(r.rested) * kHost / kSr; };

  setChip("-100:0");
  const Run faint = run(s, frames, ch, kNoId, Opts{});
  std::printf("floor -100 dBFS: rested %.2f s of 10, last plugin call at %.2f s\n", restedSecs(faint),
              static_cast<double>(faint.lastCall) / kSr);
  expect(restedSecs(faint) > 5.0 && restedSecs(faint) < 6.1, "a chip-like instrument at -100 dBFS rests after about four seconds");
  expect(faint.lastCall >= 0 && faint.lastCall < 5 * kSr, "... and its plugin is not called from then on");
  expect(peakFrom(faint, static_cast<size_t>(6 * kSr)) == 0.0, "... its output at rest is silence");

  setChip("-80:0");
  const Run loud = run(s, frames, ch, kNoId, Opts{});
  expect(loud.rested == 0, "at -80 dBFS (above the inaudible level) it keeps running");

  setChip(nullptr);
  const Run zero = run(s, frames, ch, kNoId, Opts{});
  std::printf("exact silence: rested %.2f s of 10\n", restedSecs(zero));
  expect(restedSecs(zero) > 7.5, "an instrument putting out exact silence still rests after two seconds");

  setChip("-100:0");
  Opts ed;
  ed.editor = true;
  expect(run(s, frames, ch, kNoId, ed).rested == 0, "its editor in use: it keeps running");
  Opts off;
  off.rest = false;
  expect(run(s, frames, ch, kNoId, off).rested == 0, "Rest silent instruments off: it keeps running");
  Opts offl;
  offl.offline = true;
  expect(run(s, frames, ch, kNoId, offl).rested == 0, "an offline render leaves nothing out");
}

void instrumentWakes() {
  setChip("-100:0");
  ObjectId ch = kNoId;
  Song s = synthSong({}, ch);
  const Tick bar = s.barTicks();
  s.patterns[0].notes[ch] = {Note{4 * bar + 7, s.ppq, 69, 100}};  // after the rest (4 bars at 140 BPM: ~6.9 s)
  const int frames = static_cast<int>((4.0 * 4 + 6) * 60.0 / s.tempo * kSr);
  Opts on, off;
  on.playing = off.playing = true;
  off.rest = false;
  const Run a = run(s, frames, ch, kNoId, on), b = run(s, frames, ch, kNoId, off);
  const size_t from = static_cast<size_t>(4.0 * 4 * 60.0 / s.tempo * kSr) - kSr;
  const double w = worstDiff(a, b);
  std::printf("note after rest: rested %llu blocks, worst difference %.3g (%.1f dB), onsets %d / %d\n",
              static_cast<unsigned long long>(a.rested), w, w > 0 ? 20 * std::log10(w) : -999.0, firstAbove(a, from, 1e-3),
              firstAbove(b, from, 1e-3));
  expect(a.rested > 0 && b.rested == 0, "it rests before the note (and never with resting off)");
  expect(peakFrom(a, from) > 0.05, "the note sounds");
  expect(firstAbove(a, from, 1e-3) == firstAbove(b, from, 1e-3), "it wakes on the note's own sample");
  expect(w <= AudioEngine::kRestFaint, "the output equals the output with resting off within -90 dB (its own floor)");
  setChip("-500:0");  // exact silence below the note: resting changes nothing at all
  const Run c = run(s, frames, ch, kNoId, on), d = run(s, frames, ch, kNoId, off);
  expect(c.rested > 0 && worstDiff(c, d) <= 1e-6, "with no floor of its own, resting changes nothing (within -120 dB)");
}

void effectRests() {
  // The gain effect on mixer track 1, nothing sending to it but the (silent) test synth.
  ObjectId ch = kNoId;
  Song s = synthSong({}, ch);
  Document d(s);
  const ObjectId track = d.song().mixer[1].id;
  expect(d.setSlotPlugin(track, 0, kGain), "the gain effect goes on mixer track 1");
  s = d.song();
  s.channels.clear();  // only the effect: silence in
  auto restedSecs = [](const Run& r) { return static_cast<double>(r.rested) * kHost / kSr; };
  const int frames = 10 * kSr;

  setChip("-100:0:fx");
  const Run faint = run(s, frames, kNoId, track, Opts{});
  std::printf("effect, floor -100 dBFS, stopped: rested %.2f s of 10, last plugin call at %.2f s\n", restedSecs(faint),
              static_cast<double>(faint.lastCall) / kSr);
  expect(restedSecs(faint) > 5.0 && restedSecs(faint) < 6.1, "a chip-like effect fed silence rests after about four seconds (stopped)");
  expect(faint.lastCall >= 0 && faint.lastCall < 5 * kSr, "... and its plugin is not called from then on");
  Opts play;
  play.playing = true;
  expect(run(s, frames, kNoId, track, play).rested == 0, "the transport playing: it keeps running");
  Opts ed;
  ed.editor = true;
  expect(run(s, frames, kNoId, track, ed).rested == 0, "its editor in use: it keeps running");
  Opts off;
  off.rest = false;
  expect(run(s, frames, kNoId, track, off).rested == 0, "Rest silent instruments off: it keeps running");
  Opts offl;
  offl.offline = true;
  expect(run(s, frames, kNoId, track, offl).rested == 0, "an offline render leaves it in");
  setChip("-80:0:fx");
  expect(run(s, frames, kNoId, track, Opts{}).rested == 0, "at -80 dBFS it keeps running");

  // Sound coming in wakes it in that block: a live note on the synth (built-in sine below), stopped.
  setChip("-100:0:fx");
  ObjectId ch2 = kNoId;
  Song s2 = synthSong({}, ch2);
  s2.channels[0].plugin = defaultPluginState("gl.synth");
  Document d2(s2);
  d2.setSlotPlugin(d2.song().mixer[1].id, 0, kGain);
  s2 = d2.song();
  Opts live, liveOff;
  live.liveAt = liveOff.liveAt = 7 * kSr / kHost * kHost;
  liveOff.rest = false;
  // (the live note goes to channel ch2; the slot is the one watched)
  auto runLive = [&](const Opts& o) {
    ProgramBuilder bl;
    bl.setSampleRate(kSr);
    AudioEngine e(kSr);
    e.setRestInstruments(o.rest);
    e.setProgram(bl.build(s2));
    Run r;
    r.L.assign(static_cast<size_t>(frames), 0.f);
    r.R.assign(static_cast<size_t>(frames), 0.f);
    for (int at = 0; at < frames; at += kHost) {
      if (at == o.liveAt) e.noteOn(ch2, 69, 0.8f);
      if (at == o.liveAt + kSr / 2 / kHost * kHost) e.noteOff(ch2, 69);
      e.process(r.L.data() + at, r.R.data() + at, kHost);
    }
    if (auto rt = bl.effectRuntime(s2.mixer[1].id, 0)) r.rested = rt->restBlocks.load();
    return r;
  };
  const Run a2 = runLive(live), b2 = runLive(liveOff);
  const size_t at = static_cast<size_t>(live.liveAt);
  const double w = worstDiff(a2, b2);
  std::printf("effect woken by sound: rested %llu blocks, worst difference %.3g (%.1f dB), onsets %d / %d\n",
              static_cast<unsigned long long>(a2.rested), w, w > 0 ? 20 * std::log10(w) : -999.0, firstAbove(a2, at, 1e-3),
              firstAbove(b2, at, 1e-3));
  expect(a2.rested > 0, "the effect rests before the sound comes");
  expect(peakFrom(a2, at) > 0.05 && firstAbove(a2, at, 1e-3) == firstAbove(b2, at, 1e-3), "sound coming in wakes it in that block");
  expect(w <= AudioEngine::kRestFaint, "the output equals the output with resting off within -90 dB (its own floor)");
}

void editorInUse() {
  using idlepace::editorInUse;
  expect(!editorInUse(false, true, false, false), "an open plugin window away from the pointer and the keyboard is not in use");
  expect(editorInUse(false, true, true, false), "under the pointer it is (a click comes after the pointer)");
  expect(editorInUse(false, true, false, true), "holding the keyboard it is");
  expect(!editorInUse(false, false, true, true), "set aside it is not");
  expect(editorInUse(true, false, false, false), "a window Gridline cannot see into (a sandbox's, Wayland's own) always is");
}

}  // namespace

int main() {
  gl::test::hermeticTestEnv("k078idle");
  host::ScanResult r = vst3::scanBundleInProcess(kBundle);
  host::registerScanResult(r);
  expect(findPlugin(kSynth) && findPlugin(kGain), "the VST3 test plugins are registered");
  expect(probe() != nullptr, "the VST3 test plugin's probe is reachable");
  editorInUse();
  instrumentRests();
  instrumentWakes();
  effectRests();
  return finish();
}
