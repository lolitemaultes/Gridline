// Gridline - K-123 core A (0.7.7 recording rows): quick punch, capture while armed, start on the
// first sound, punch crossfades, and the loopback latency check. The audio device is simulated:
// every block pushes input frames into the engine's input queue before processing, as the capture
// callback does; the loopback cable is the engine's own output fed back that many frames later.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "../src/core/Automation.h"
#include "../src/core/Settings.h"
#include "../src/core/Song.h"
#include "../src/engine/ArmedRing.h"
#include "../src/engine/AudioEngine.h"
#include "../src/engine/AudioRecord.h"
#include "../src/engine/Loopback.h"
#include "../src/engine/Program.h"
#include "Check.h"
#include "Hermetic.h"

using namespace gl;

namespace {

std::string gDir;

float ramp(int64_t f) { return static_cast<float>(f % 100000) / 100000.0f; }

struct Rig {
  double sr;
  Song song;
  ProgramBuilder builder;
  AudioEngine eng;
  AudioRecorder rec;
  std::shared_ptr<const Program> prog;
  int64_t frame = 0;  // frames processed so far
  std::function<float(int64_t, int)> input = [](int64_t, int) { return 0.f; };
  int loopback = 0;         // > 0: the input is the output this many frames earlier
  float loopGain = 1.0f;    // (an interface that turns the polarity over: negative)
  std::vector<float> hist;  // every output frame (left), for the loopback
  std::vector<float> outL;  // output of the last run

  explicit Rig(double rate = 48000) : sr(rate), eng(rate) {
    song = makeEmptySong();
    song.tempo = 120;
    builder.setSampleRate(rate);
    eng.setInputChannels(2);
    eng.setMode(PlayMode::Song);
    eng.setLooping(false);
  }
  void build() {
    prog = builder.build(song);
    eng.setProgram(prog);
  }
  void run(int64_t frames, int block = 256) {
    std::vector<float> L(static_cast<size_t>(block)), R(static_cast<size_t>(block)), in(static_cast<size_t>(block) * 2);
    outL.clear();
    for (int64_t done = 0; done < frames;) {
      const int n = static_cast<int>(std::min<int64_t>(block, frames - done));
      for (int i = 0; i < n; ++i) {
        const int64_t f = frame + i;
        float l = 0, r = 0;
        if (loopback > 0) {
          const int64_t at = f - loopback;
          l = r = at >= 0 && at < static_cast<int64_t>(hist.size()) ? loopGain * hist[static_cast<size_t>(at)] : 0.f;
        } else {
          l = input(f, 0);
          r = input(f, 1);
        }
        in[static_cast<size_t>(2 * i)] = l;
        in[static_cast<size_t>(2 * i + 1)] = r;
      }
      eng.pushInput(in.data(), n);
      eng.process(L.data(), R.data(), n);
      hist.insert(hist.end(), L.begin(), L.begin() + n);
      outL.insert(outL.end(), L.begin(), L.begin() + n);
      frame += n;
      done += n;
      if (prog) rec.pump(eng, ringsOf(*prog));
    }
  }
  double spt() const { return sr * 60.0 / (song.tempo * song.ppq); }  // samples per tick
  void finish() {
    eng.setRecording(false);
    eng.stop();
    run(eng.recordLatency() + 1024);
  }
};

// ---------------------------------------------------------------- the ring

void testArmedRing() {
  ArmedRing r(1000);
  expect(r.capacity() == ArmedRing::capacityFor(1000) && r.capacity() >= 65536, "a short ring is the punch ring at least");
  std::vector<float> L(256), R(256);
  std::vector<double> t(256);
  int64_t f = 0;
  auto block = [&](double tick0, double step) {
    for (int i = 0; i < 256; ++i) {
      L[static_cast<size_t>(i)] = ramp(f + i);
      R[static_cast<size_t>(i)] = -ramp(f + i);
      t[static_cast<size_t>(i)] = tick0 < 0 ? -1.0 : tick0 + step * i;
    }
    r.write(L.data(), R.data(), t.data(), 256);
    f += 256;
  };
  block(-1, 0);          // stopped
  block(0, 0.1);         // plays from 0
  block(25.6, 0.1);      // on
  block(0, 0.1);         // the loop came round
  double tk = 0;
  expect(!r.tickAt(100, tk), "stopped frames have no position");
  expect(r.tickAt(256, tk) && std::fabs(tk) < 1e-9 && r.tickAt(300, tk) && std::fabs(tk - 4.4) < 1e-9, "positions of a run");
  size_t a = 0, b = 0;
  expect(r.runOf(300, a, b) && a == 256 && b == 768, "the run before the wrap: frames 256..768");
  expect(r.lastRun(10, a, b) && a == 768 && b == 1024, "the last pass is the one after the wrap");
  std::vector<float> l(10), rr(10);
  expect(r.read(500, 10, l.data(), rr.data()) == 10 && l[0] == ramp(500) && rr[9] == -ramp(509), "frames read back");
  // Wrapping: old frames go.
  for (int k = 0; k < 400; ++k) {
    block(51.2 + 25.6 * k, 0.1);
  }
  expect(r.read(500, 10, l.data(), rr.data()) == 0 && r.oldest() > 500, "overwritten frames are not read");
  expect(r.read(r.written() - 10, 10, l.data(), rr.data()) == 10 && l[9] == ramp(static_cast<int64_t>(r.written()) - 1), "the newest frames are");
  // A tempo change (a new spacing) is no jump: the run goes on.
  ArmedRing q(0);
  f = 0;
  for (int i = 0; i < 256; ++i) {
    t[static_cast<size_t>(i)] = 10.0 + 0.1 * i;
  }
  q.write(L.data(), R.data(), t.data(), 256);
  for (int i = 0; i < 256; ++i) {
    t[static_cast<size_t>(i)] = 10.0 + 25.6 + 0.12 * i;
  }
  q.write(L.data(), R.data(), t.data(), 256);
  expect(q.runOf(400, a, b) && a == 0 && b == 512 && q.tickAt(300, tk) && std::fabs(tk - (35.6 + 0.12 * 44)) < 1e-9, "a tempo change keeps the run");
}

// ---------------------------------------------------------------- capture while armed

void testCaptureAndRecover() {
  Rig r;
  r.song.mixer[1].input = 0;
  r.song.mixer[1].armed = true;
  r.song.mixer[1].monitor = static_cast<uint8_t>(InputMonitor::Off);
  r.builder.setArmedCapture(10);
  r.build();
  const auto& pm = r.prog->mixer[1];
  expect(pm.armRing && pm.armRing->capacity() == static_cast<size_t>(10 * 48000), "an armed track gets a ring of the chosen length");
  const auto first = pm.armRing;
  r.build();
  expect(r.prog->mixer[1].armRing == first, "rebuilds keep the ring (same length)");
  r.input = [](int64_t f, int) { return ramp(f); };
  r.eng.setRecordLatency(100);
  r.run(1000);
  const int64_t f0 = r.frame;  // the transport starts here: tick 0
  r.eng.play();
  r.run(static_cast<int64_t>(2 * r.sr));  // played, never recorded
  r.eng.stop();
  r.run(1024);
  expect(r.rec.takes().empty(), "nothing recorded (Record was off)");
  std::vector<RecordedTake> got = recoverLastPass(armRingsOf(*r.prog), r.sr);
  expect(got.size() == 1, "recover: one take for the armed track");
  if (got.size() == 1) {
    const RecordedTake& t = got[0];
    expect(t.mixer == r.song.mixer[1].id && std::fabs(t.startTick) < 1e-9, "recover: placed at the song position it was played at (tick 0)");
    // Latency compensated as a take is: the input 100 frames after the transport started is what was heard at tick 0.
    expect(t.L[0] == ramp(f0 + 100) && t.L[1000] == ramp(f0 + 1100), "recover: sample exact, latency compensated");
    expect(std::llabs(t.frames() - static_cast<int64_t>(2 * r.sr)) <= 256, "recover: the whole pass");
  }
  // A second pass: recover takes the newest one.
  r.eng.setPosition(960);
  r.eng.play();
  r.run(static_cast<int64_t>(r.sr));
  r.eng.stop();
  r.run(1024);
  got = recoverLastPass(armRingsOf(*r.prog), r.sr);
  expect(got.size() == 1 && std::fabs(got[0].startTick - 960) < 1e-6 && std::llabs(got[0].frames() - static_cast<int64_t>(r.sr)) <= 256,
         "recover: the newest pass (from tick 960)");
  // Committed as ordinary clips (one per track, not take lanes).
  TakeCommitOptions o;
  o.dir = gDir + "/Recovered";
  o.sampleRate = r.sr;
  const TakeCommitResult res = commitTakes(r.song, got, o);
  const Clip* c = res.clips.empty() ? nullptr : r.song.clip(res.clips[0]);
  expect(c && c->start == 960 && c->takeGroup == kNoId, "recover: an audio clip where it was played");
  // Disarmed: no ring.
  r.song.mixer[1].armed = false;
  r.build();
  expect(!r.prog->mixer[1].armRing && armRingsOf(*r.prog).empty(), "a disarmed track keeps no ring");
}

// ---------------------------------------------------------------- quick punch

void testQuickPunch() {
  Rig r;
  r.song.mixer[1].input = 0;
  r.song.mixer[1].armed = true;
  r.song.mixer[1].monitor = static_cast<uint8_t>(InputMonitor::Off);
  r.build();
  r.input = [](int64_t f, int) { return ramp(f); };
  r.rec.begin();
  r.rec.setQuickPunch(true);
  r.rec.setPunchPads(armRingsOf(*r.prog), 0.010, r.sr);
  r.eng.setRecording(true);
  r.eng.play();
  r.run(static_cast<int64_t>(r.sr));
  r.eng.setRecording(false);  // punch out (the transport plays on)
  r.run(static_cast<int64_t>(r.sr / 2));
  const int64_t inAt = r.frame;
  r.eng.setRecording(true);  // punch in
  r.run(static_cast<int64_t>(r.sr / 2));
  r.eng.setRecording(false);  // punch out again
  r.run(static_cast<int64_t>(r.sr / 4));
  r.eng.setRecording(true);  // and in
  r.run(static_cast<int64_t>(r.sr / 4));
  r.finish();
  std::vector<RecordedTake> takes = r.rec.release();
  expect(takes.size() == 3, "quick punch: three punches, three pieces");
  if (takes.size() != 3) return;
  expect(takes[0].pass == 0 && takes[1].pass == 0 && takes[2].pass == 0, "quick punch: the pieces of one pass are one take (same pass)");
  expect(!takes[0].punchIn && takes[0].punchOut && takes[1].punchIn && takes[1].punchOut && takes[2].punchIn && !takes[2].punchOut,
         "the punch points are marked (not the transport start and stop)");
  // The second piece reaches back half the crossfade to a whole tick: the frames before the punch,
  // continuous with it.
  const RecordedTake& t = takes[1];
  const double spt = r.spt();
  expect(t.padBefore >= static_cast<int64_t>(0.005 * r.sr) && t.padBefore < static_cast<int64_t>(0.005 * r.sr + spt + 1), "pre-pad: half the crossfade, to a whole tick");
  expect(std::fabs(t.startTick - std::round(t.startTick)) < 1e-6, "pre-pad: the take starts on a whole tick");
  expect(t.L[static_cast<size_t>(t.padBefore)] == ramp(inAt) && t.L[0] == ramp(inAt - t.padBefore), "pre-pad: the audio before the punch, sample exact");
  expect(takes[0].padAfter >= static_cast<int64_t>(0.005 * r.sr) && takes[0].padAfter < static_cast<int64_t>(0.005 * r.sr + spt + 1),
         "post-pad: half the crossfade after the punch out");
  // Placement: no take lanes for the pieces of one pass.
  TakeCommitOptions o;
  for (size_t i = 0; i < takes.size(); ++i) {
    const TakePlacement p = takePlacement(r.song, takes, i, o);
    expect(!p.grouped && !p.lane && !p.muted, "a quick punch piece is not a loop pass");
  }
  // A punch at or before the last one (the loop came round) is the next pass.
  AudioRecorder q;
  q.setQuickPunch(true);
  Rig r2;
  r2.song.mixer[1].input = 0;
  r2.song.mixer[1].armed = true;
  r2.build();
  r2.eng.setPosition(960);
  r2.eng.setRecording(true);
  r2.eng.play();
  r2.run(4096);
  r2.rec.setQuickPunch(true);
  r2.eng.setPosition(0);  // back to the start: the next pass
  r2.run(4096);
  r2.finish();
  const auto& t2 = r2.rec.takes();
  expect(t2.size() == 2 && t2[1].pass == t2[0].pass + 1, "quick punch: a punch earlier in the song is the next pass");
}

// ---------------------------------------------------------------- punch crossfades (sample exact)

// The old audio on the track: a constant level from tick 0 for 12 beats, recorded on mixer track 1.
void placeOld(Rig& r, float level) {
  RecordedTake old;
  old.mixer = r.song.mixer[1].id;
  old.startTick = 0;
  old.L.assign(static_cast<size_t>(12 * 96 * r.spt()), level);
  old.R = old.L;
  TakeCommitOptions o;
  o.dir = gDir + "/Old";
  o.sampleRate = r.sr;
  commitTakes(r.song, {old}, o);
}

void testPunchCrossfade(float newLevel) {
  Rig r;
  placeOld(r, 0.5f);
  r.song.mixer[1].input = 0;
  r.song.mixer[1].armed = true;
  r.song.mixer[1].monitor = static_cast<uint8_t>(InputMonitor::Off);
  r.build();
  const double pa = 4 * 96, pb = 8 * 96, spt = r.spt();  // 250 samples per tick
  r.eng.setPunch(pa, pb);
  r.input = [newLevel](int64_t, int) { return newLevel; };
  r.rec.begin();
  r.rec.setPunchPads(armRingsOf(*r.prog), 0.010, r.sr);
  r.eng.setRecording(true);
  r.eng.play();
  r.run(static_cast<int64_t>(10 * 96 * spt));  // past the punch out: the transport plays on
  r.finish();
  std::vector<RecordedTake> takes = r.rec.release();
  expect(takes.size() == 1 && takes[0].punchIn && takes[0].punchOut, "punch range: one take, both ends punches");
  if (takes.size() != 1) return;
  expect(std::fabs(takes[0].startTick - 383) < 1e-6 && takes[0].padBefore == 250, "pre-pad: from tick 383 (half of 10 ms before 384, to a whole tick)");
  expect(takes[0].padAfter == 250, "post-pad: to tick 769");
  TakeCommitOptions o;
  o.dir = gDir + "/Punch";
  o.sampleRate = r.sr;
  o.punchFade = 0.010;
  const TakeCommitResult res = commitTakes(r.song, takes, o);
  const Clip* c = res.clips.empty() ? nullptr : r.song.clip(res.clips[0]);
  expect(c && c->start == 383 && c->start + c->length == 769, "the take clip: ticks 383 to 769");
  const double xf = 2 * 250 / r.sr;  // 2 ticks
  expect(c && std::fabs(c->fadeIn - xf) < 1e-9 && std::fabs(c->fadeOut - xf) < 1e-9 && c->fadeInManual && c->fadeOutManual, "the take fades in and out over 2 ticks");
  std::vector<const Clip*> old;
  for (const Clip& k : r.song.playlist) {
    if (c && k.track == c->track && k.id != c->id) old.push_back(&k);
  }
  std::sort(old.begin(), old.end(), [](const Clip* x, const Clip* y) { return x->start < y->start; });
  expect(old.size() == 2, "the old audio: the part before the punch and the part after (the middle went)");
  if (old.size() == 2) {
    expect(old[0]->start == 0 && old[0]->start + old[0]->length == 385 && std::fabs(old[0]->fadeOut - xf) < 1e-9, "before: ends at 385, fades out where the take fades in");
    expect(old[1]->start == 767 && std::fabs(old[1]->fadeIn - xf) < 1e-9, "after: starts at 767, fades in where the take fades out");
  }
  // Play the song back (the input silent, the track disarmed) and listen at every sample.
  r.song.mixer[1].armed = false;
  r.input = [](int64_t, int) { return 0.f; };
  r.build();
  r.run(4096);
  r.eng.setPosition(0);
  r.eng.play();
  r.run(static_cast<int64_t>(11 * 96 * spt), 250);
  const std::vector<float> out = r.outL;
  const auto at = [&](double tick) { return out[static_cast<size_t>(std::llround(tick * spt))]; };
  const float steady = at(100);
  expect(steady > 0.05f, "the old audio plays");
  const float ratio = newLevel / 0.5f;
  bool flat = true, inside = true, mono = true;
  float worstJump = 0;
  for (size_t k = static_cast<size_t>(200 * spt); k < static_cast<size_t>(1000 * spt); ++k) {
    const double tick = static_cast<double>(k) / spt;
    const float want = tick < 383 || tick >= 769 ? steady : (tick >= 385 && tick < 767 ? steady * ratio : 0.f);
    if (tick < 383 || tick >= 769 || (tick >= 385 && tick < 767)) flat = flat && std::fabs(out[k] - want) < 1e-4f * std::max(1.0f, steady);
    if (k > 0) worstJump = std::max(worstJump, std::fabs(out[k] - out[k - 1]));
    if (tick >= 383 && tick < 385) {
      const float lo = std::min(steady, steady * ratio), hi = std::max(steady, steady * ratio);
      inside = inside && out[k] >= lo - 1e-4f && out[k] <= hi + 1e-4f;
      if (ratio < 1 && k > 0 && out[k] > out[k - 1] + 1e-5f) mono = false;  // (float rounding aside)
    }
  }
  if (ratio == 1.0f) {
    bool level = true;
    for (size_t k = static_cast<size_t>(200 * spt); k < static_cast<size_t>(1000 * spt); ++k) {
      level = level && std::fabs(out[k] - steady) < 2e-3f * steady;
    }
    expect(level, "same level in and out: the output never moves through both crossfades (no dip, no bump)");
  } else {
    expect(flat, "the old audio plays exactly until tick 383 and from 769, the take exactly between 385 and 767");
    expect(inside && mono, "inside the crossfade the level moves smoothly from the old to the new, never past either");
    expect(std::fabs(at(384) - 0.5f * (steady + steady * ratio)) < 0.02f * steady, "half way at the punch point");
  }
  expect(worstJump < 0.01f * steady + std::fabs(steady - steady * ratio) * 0.02f, "no click: no sample-to-sample step");
}

// Punch crossfade off: the take is placed as recorded and the old audio is left alone.
void testPunchFadeOff() {
  Rig r;
  placeOld(r, 0.5f);
  r.song.mixer[1].input = 0;
  r.song.mixer[1].armed = true;
  r.build();
  r.eng.setPunch(4 * 96, 8 * 96);
  r.rec.begin();
  r.eng.setRecording(true);
  r.eng.play();
  r.run(static_cast<int64_t>(10 * 96 * r.spt()));
  r.finish();
  std::vector<RecordedTake> takes = r.rec.release();
  TakeCommitOptions o;
  o.dir = gDir + "/NoFade";
  o.sampleRate = r.sr;
  const size_t before = r.song.playlist.size();
  const TakeCommitResult res = commitTakes(r.song, takes, o);
  const Clip* c = res.clips.empty() ? nullptr : r.song.clip(res.clips[0]);
  expect(c && c->start == 384 && c->fadeIn == 0 && c->fadeOut == 0 && r.song.playlist.size() == before + 1,
         "crossfade off: the take as recorded (from the punch in), the old audio untouched");
}

// Loop recording with the punch range on the loop: a pass after the wrap starts at the loop start,
// with nothing from the end of the loop before it (no pad across the wrap), every pass whole.
void testLoopWrapNoPad() {
  Rig r;
  r.song.mixer[1].input = 0;
  r.song.mixer[1].armed = true;
  r.build();
  const Tick bar = r.song.barTicks();
  r.eng.setLooping(true);
  r.eng.setLoopRange(0, bar);
  r.eng.setPunch(0, static_cast<double>(bar));
  r.input = [](int64_t f, int) { return ramp(f); };
  r.rec.begin();
  r.rec.setPunchPads(armRingsOf(*r.prog), 0.010, r.sr);
  r.eng.setRecording(true);
  r.eng.play();
  const double barFrames = static_cast<double>(bar) * r.spt();
  r.run(static_cast<int64_t>(barFrames * 2.5), 512);
  r.finish();
  const auto takes = r.rec.release();
  expect(takes.size() == 3, "loop + punch range: one take per pass");
  if (takes.size() != 3) return;
  expect(takes[1].startTick == 0 && takes[2].startTick == 0 && takes[1].padBefore == 0 && takes[2].padBefore == 0,
         "passes after the wrap start at the loop start: no audio from the loop's end before them");
  expect(takes[0].padAfter == 0 && takes[1].padAfter == 0, "and none from the next pass after them");
  expect(std::llabs(takes[1].frames() - static_cast<int64_t>(barFrames)) <= 1, "every pass is one loop long");
  expect(std::fabs(takes[1].L[0] - takes[0].L.back() - 1e-5f) < 1e-6f, "no frame lost or doubled at the wrap");
}

// ---------------------------------------------------------------- start on the first sound

void testStartOnInput() {
  Rig r;
  r.song.mixer[1].input = 0;
  r.song.mixer[1].armed = true;
  r.song.mixer[1].monitor = static_cast<uint8_t>(InputMonitor::Off);
  r.build();
  const int64_t onset = 10000;
  r.input = [](int64_t f, int) { return f >= 10000 ? 0.5f : 0.001f; };
  r.rec.begin();
  r.eng.armStartOnInput(std::pow(10.0f, -30.0f / 20.0f));
  r.run(8192);
  expect(!r.eng.playing() && r.eng.inputStarts() == 0, "quiet input: the transport waits");
  r.run(8192);
  expect(r.eng.playing() && r.eng.recordingOn() && r.eng.inputStarts() == 1 && r.eng.startOnInputLevel() == 0.0f,
         "the first loud block starts playback and recording (once)");
  r.finish();
  const auto& t = r.rec.takes();
  const int64_t blockStart = onset / 256 * 256;
  expect(t.size() == 1 && std::fabs(t[0].startTick) < 1e-9, "the take starts at the song position");
  if (!t.empty()) {
    const auto k = static_cast<size_t>(onset - blockStart);
    expect(t[0].L.size() > k && t[0].L[k] == 0.5f && t[0].L[k - 1] == 0.001f, "the sound that started it is in the take (sample exact)");
  }
  // Off: nothing starts.
  Rig q;
  q.song.mixer[1].input = 0;
  q.song.mixer[1].armed = true;
  q.build();
  q.input = [](int64_t, int) { return 0.9f; };
  q.eng.armStartOnInput(0);
  q.run(4096);
  expect(!q.eng.playing(), "start on sound off: a loud input starts nothing");
}

// ---------------------------------------------------------------- the loopback latency check

void testFindLag() {
  const std::vector<float> s = loopback::probeSignal(48000);
  expect(s.size() > 1000 && s.size() < 3000, "the burst is about 43 ms");
  std::vector<float> got(loopback::listenFrames(48000), 0.f);
  uint32_t x = 99;
  for (float& v : got) {
    x = x * 1103515245u + 12345u;
    v = 0.01f * (static_cast<float>(x >> 16 & 0x7FFF) / 32768.0f - 0.5f);  // a little hiss
  }
  for (size_t i = 0; i < s.size(); ++i) {
    got[1234 + i] += 0.3f * s[i];
  }
  const loopback::Lag l = loopback::findLag(s, got);
  expect(l.found && l.frames == 1234 && !l.inverted && l.match > 0.9f, "the burst is found at its lag through hiss");
  for (size_t i = 0; i < s.size(); ++i) {
    got[1234 + i] -= 0.6f * s[i];
  }
  const loopback::Lag li = loopback::findLag(s, got);
  expect(li.found && li.frames == 1234 && li.inverted, "and upside down");
  std::vector<float> hiss(got.size(), 0.f);
  for (size_t i = 0; i < hiss.size(); ++i) {
    hiss[i] = got[(i * 7919) % got.size()] * 0.01f;
  }
  expect(!loopback::findLag(s, std::vector<float>(got.size(), 0.f)).found, "silence: nothing found");
  loopback::Lag a, b, c;
  a.found = b.found = c.found = true;
  a.frames = 500, b.frames = 501, c.frames = 900;
  const loopback::Summary sum = loopback::summarise({a, b, c});
  expect(sum.ok && sum.agree == 2 && (sum.frames == 500 || sum.frames == 501), "two of three agree: their median");
  c.frames = 2000;
  b.frames = 1500;
  expect(!loopback::summarise({a, b, c}).ok, "none agree: no result");
  expect(loopback::recordLatency(256, 0, 10, 0) == 266 && loopback::recordLatency(256, 300, 10, 5) == 305 && loopback::recordLatency(10, 0, 0, 50) == 0,
         "record latency: the measured round trip replaces the estimate");
  expect(loopback::setupKey("Out", "In", 48000, 256) != loopback::setupKey("Out", "In", 48000, 512), "a measurement belongs to its buffer size");
}

int measure(Rig& r, int channel) {
  const std::vector<float> burst = loopback::probeSignal(r.sr);
  expect(r.eng.startLatencyProbe(channel, burst, loopback::listenFrames(r.sr)), "the check starts");
  for (int k = 0; k < 400 && !r.eng.latencyProbeDone(); ++k) {
    r.run(256);
  }
  const loopback::Lag l = loopback::findLag(burst, r.eng.latencyProbeCapture());
  r.eng.cancelLatencyProbe();
  return l.found ? l.frames * (l.inverted ? -1 : 1) : 0;
}

void testLoopback() {
  for (int d : {300, 1000, 2731}) {
    Rig r;
    r.build();
    r.loopback = d;
    r.run(1024);
    const int got = measure(r, 0);
    expect(got == d, ("the round trip through the cable, sample exact: " + std::to_string(d)).c_str());
  }
  {
    Rig r;
    r.build();
    r.loopback = 777;
    r.loopGain = -0.3f;
    r.run(1024);
    expect(measure(r, 1) == -777, "an interface that turns the polarity over, quieter, on In 2");
  }
  // The measured round trip applied: a click the song plays comes back through the cable and the
  // take lands where the click is in the song, to the sample. Without it, the take is that late.
  for (bool compensate : {true, false}) {
    Rig r;
    r.song.mixer[1].input = 0;
    r.song.mixer[1].armed = true;
    r.song.mixer[1].monitor = static_cast<uint8_t>(InputMonitor::Off);  // (no feedback through the cable)
    RecordedTake click;
    click.mixer = r.song.mixer[2].id;
    click.startTick = 192;
    click.L.assign(48000, 0.f);
    click.L[1000] = 0.8f;
    click.R = click.L;
    TakeCommitOptions o;
    o.dir = gDir + "/Click";
    o.sampleRate = r.sr;
    commitTakes(r.song, {click}, o);
    r.build();
    r.loopback = 1500;
    r.run(1024);
    const int rt = measure(r, 0);
    expect(rt == 1500, "measured");
    r.eng.setRecordLatency(loopback::recordLatency(0, compensate ? rt : 0, r.eng.outputLatency(), 0));
    r.rec.begin();
    r.eng.setPosition(0);
    r.eng.setRecording(true);
    r.eng.play();
    r.run(static_cast<int64_t>(1.5 * r.sr));
    r.finish();
    const auto& t = r.rec.takes();
    expect(!t.empty(), "a take");
    if (t.empty()) continue;
    size_t peak = 0;
    for (size_t i = 0; i < t[0].L.size(); ++i) {
      if (std::fabs(t[0].L[i]) > std::fabs(t[0].L[peak])) peak = i;
    }
    const double heard = t[0].startTick + static_cast<double>(peak) / r.spt();
    const double want = 192 + 1000 / r.spt();
    if (compensate) {
      expect(std::fabs(heard - want) < 0.5 / r.spt(), "compensated: the click lands on its own sample");
    } else {
      expect(std::fabs(heard - want - 1500 / r.spt()) < 0.5 / r.spt(), "not compensated: the round trip late");
    }
  }
}

void testSettingsDefaults() {
  Settings s;
  expect(!s.getBool("record.quickPunch") && s.getNumber("record.captureSeconds") == 60 && !s.getBool("record.startOnNote") &&
             s.getNumber("record.startLevelDb") == -30 && s.getNumber("record.punchFadeMs") == 10 && s.getNumber("audio.loopbackLatency") == 0,
         "the defaults: quick punch off, 60 s capture, start on note off at -30 dB, 10 ms crossfade, no measurement");
}

}  // namespace

int main() {
  gDir = gl::test::hermeticTestEnv("k123_core_a");
  testArmedRing();
  testCaptureAndRecover();
  testQuickPunch();
  testPunchCrossfade(0.5f);
  testPunchCrossfade(0.25f);
  testPunchFadeOff();
  testLoopWrapNoPad();
  testStartOnInput();
  testFindLag();
  testLoopback();
  testSettingsDefaults();
  return finish();
}
