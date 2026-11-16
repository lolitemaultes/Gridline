// Gridline - K-123 professional recording. The audio device is simulated: every block pushes
// generated input frames into the engine's input queue, as the capture callback does.
// Stage 1, input monitoring: input trim and polarity (before the meter, the monitoring and the
// capture), tape-style Auto monitoring around the punch range, Input only monitoring, low-latency
// monitoring (armed tracks skip latency-adding effects while recording), saved and kept local.
#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "../src/collab/LocalState.h"
#include "../src/core/PluginParams.h"
#include "../src/core/Project.h"
#include "../src/core/RecordInfo.h"
#include "../src/core/Song.h"
#include "../src/core/TrackModes.h"
#include "../src/engine/AudioEngine.h"
#include "../src/engine/Program.h"
#include "Check.h"

using namespace gl;

namespace {

struct Rig {
  Song song;
  ProgramBuilder builder;
  AudioEngine eng{48000};
  std::shared_ptr<const Program> prog;
  std::vector<float> outL, outR;
  float inL = 0.25f, inR = -0.5f;
  Rig() {
    song = makeEmptySong();
    builder.setSampleRate(48000);
    eng.setInputChannels(2);
    eng.setMode(PlayMode::Song);
    eng.setLooping(false);
  }
  void build() {
    prog = builder.build(song);
    eng.setProgram(prog);
  }
  void run(int frames, int block = 256) {
    std::vector<float> L(static_cast<size_t>(block)), R(static_cast<size_t>(block)), in(static_cast<size_t>(block) * 2);
    outL.clear();
    outR.clear();
    for (int done = 0; done < frames; done += block) {
      for (int i = 0; i < block; ++i) in[static_cast<size_t>(2 * i)] = inL, in[static_cast<size_t>(2 * i + 1)] = inR;
      eng.pushInput(in.data(), block);
      eng.process(L.data(), R.data(), block);
      outL.insert(outL.end(), L.begin(), L.end());
      outR.insert(outR.end(), R.begin(), R.end());
    }
  }
};

void testTrimAndPolarity() {
  Rig r;
  MixerTrack& m = r.song.mixer[1];
  m.input = 0;
  m.monitor = static_cast<uint8_t>(InputMonitor::On);
  r.build();
  r.run(2048);
  const float base = r.outL.back();
  expect(base > 0.05f, "trim: the input is heard");
  r.song.mixer[1].inputTrim = 6.0206f;
  r.song.mixer[1].inputInvert = true;
  r.build();
  auto rt = r.builder.mixerRuntime(r.song.mixer[1].id);
  if (rt) rt->inPeakL.store(0), rt->inPeakR.store(0);
  r.run(2048);
  expect(std::fabs(r.outL.back() + 2.0f * base) < 1e-3f, "trim +6 dB and inverted polarity: twice as loud, inverted");
  expect(std::fabs(r.outR.back() / r.outL.back() + 2.0f) < 1e-3f, "trim: the pair keeps its balance");
  expect(rt && std::fabs(rt->inPeakL.load() - 0.5f) < 1e-3f && std::fabs(rt->inPeakR.load() - 1.0f) < 1e-3f, "trim: the input meter reads after the trim");
  r.song.mixer[1].inputTrim = -24.0f;
  r.song.mixer[1].inputInvert = false;
  r.build();
  r.run(2048);
  expect(std::fabs(r.outL.back() - base * std::pow(10.0f, -24.0f / 20.0f)) < 1e-4f, "trim -24 dB");
  // The engine reads the device buffers directly when there is nothing to trim.
  r.song.mixer[1].inputTrim = 0.0f;
  r.build();
  r.run(2048);
  expect(std::fabs(r.outL.back() - base) < 1e-6f, "trim 0 dB: unchanged");
}

void testMonitorRules() {
  Rig r;
  AudioEngine& e = r.eng;
  const uint8_t autoMon = static_cast<uint8_t>(InputMonitor::Auto);
  expect(e.hearsInput(autoMon, false, false, 0) && e.hearsInput(autoMon, true, false, 0), "auto: stopped hears the input");
  expect(!e.hearsInput(autoMon, false, true, 0), "auto: an unarmed track playing does not");
  expect(e.hearsInput(autoMon, true, true, 0), "auto: an armed track playing does (no punch)");
  e.setPunch(960, 1920);
  e.setRecording(true);
  expect(!e.hearsInput(autoMon, true, true, 100) && e.hearsInput(autoMon, true, true, 1000) && !e.hearsInput(autoMon, true, true, 2000),
         "tape style: only inside the punch range while recording");
  e.setMonitorInputOnly(true);
  expect(e.hearsInput(autoMon, true, true, 100) && e.hearsInput(autoMon, true, true, 2000), "input only: always, around the punch too");
  e.setMonitorInputOnly(false);
  e.setRecording(false);
  expect(e.hearsInput(autoMon, true, true, 100), "record off: armed tracks hear the input");
  expect(e.hearsInput(static_cast<uint8_t>(InputMonitor::On), false, true, 100) && !e.hearsInput(static_cast<uint8_t>(InputMonitor::Off), true, false, 0),
         "on and off are not changed");
  // In the graph: before the punch in, an armed track in Auto is quiet; Input only hears it.
  r.song.mixer[1].input = 0;
  r.song.mixer[1].armed = true;
  r.build();
  e.setPunch(96.0 * 64, 96.0 * 128);
  e.setRecording(true);
  e.play();
  r.run(4096);
  expect(std::fabs(r.outL.back()) < 1e-6f, "graph: before the punch in, the armed track is quiet");
  e.setMonitorInputOnly(true);
  r.run(4096);
  expect(r.outL.back() > 0.05f, "graph: input only monitoring hears it");
  e.setRecording(false);
  e.stop();
}

void testLowLatency() {
  using namespace pp;
  Rig r;
  MixerTrack& m = r.song.mixer[1];
  m.input = 0;
  m.armed = true;
  m.monitor = static_cast<uint8_t>(InputMonitor::Off);
  m.slots[0].plugin = defaultPluginState("gl.hwinsert");
  m.slots[0].plugin.params[HwSend] = 0;     // none: an audio return of the device input (connected)
  m.slots[0].plugin.params[HwLatency] = 512;  // a latency-adding effect
  r.build();
  r.run(8192);
  expect(r.outL.back() > 0.05f, "low latency off: the latency-adding effect runs");
  r.eng.setLowLatencyMonitoring(true);
  r.run(8192);
  expect(r.outL.back() > 0.05f, "low latency on, not recording: it still runs");
  r.eng.setRecording(true);
  r.run(8192);
  expect(std::fabs(r.outL.back()) < 1e-6f, "low latency while recording: the armed track skips it");
  r.song.mixer[1].armed = false;
  r.build();
  r.run(8192);
  expect(r.outL.back() > 0.05f, "low latency: unarmed tracks keep their effects");
}

// Record safe: saved, shared with collaborators, and a safe track can't be armed.
void testRecordSafe() {
  Song s = makeEmptySong();
  s.mixer[1].recordSafe = true;
  s.mixer[1].armed = true;  // as an older file or a script might leave it
  Song back;
  std::string err;
  expect(songFromJson(songToJson(s), back, &err) && back.mixer[1].recordSafe && !back.mixer[1].armed, "record safe round trips and a safe track loads disarmed");
  MixerTrack cur = s.mixer[1];
  cur.recordSafe = false;
  cur.armed = true;  // armed here
  MixerTrack incoming = cur;
  incoming.recordSafe = true;  // a collaborator made it record safe
  collab::keepMixerSession(incoming, cur);
  expect(incoming.recordSafe && !incoming.armed, "collab: record safe is shared and disarms the track here");
  int t = -1;
  for (size_t i = 0; i < back.tracks.size(); ++i)
    if (trackmodes::modeOf(back, static_cast<int>(i)) == TrackMode::Audio) t = static_cast<int>(i);
  if (t < 0 && !back.tracks.empty()) {
    back.tracks[0].mode = TrackMode::Audio;
    back.tracks[0].mixer = back.mixer[1].id;
    t = 0;
  }
  if (t >= 0) {
    MixerTrack* m = back.mixerTrack(back.tracks[static_cast<size_t>(t)].mixer);
    m->recordSafe = true;
    expect(!trackmodes::setArmed(back, t, true) && !m->armed, "a record safe track can't be armed from its playlist track");
    m->recordSafe = false;
    expect(trackmodes::setArmed(back, t, true) && m->armed, "with record safe off it arms");
  }
}

void testSavedAndLocal() {
  Song s = makeEmptySong();
  s.mixer[1].inputTrim = -7.5f;
  s.mixer[1].inputInvert = true;
  Song back;
  std::string err;
  expect(songFromJson(songToJson(s), back, &err), "save and load");
  expect(back.mixer.size() > 1 && back.mixer[1].inputTrim == -7.5f && back.mixer[1].inputInvert, "trim and polarity round trip");
  MixerTrack incoming = s.mixer[1];
  incoming.inputTrim = 3.0f;
  incoming.inputInvert = false;
  collab::keepMixerSession(incoming, s.mixer[1]);
  expect(incoming.inputTrim == -7.5f && incoming.inputInvert, "collab: the input's trim and polarity stay this computer's");
}

// Stage 2: pre-roll and post-roll around the punch range (song mode, record armed).
void testPrePostRoll() {
  Rig r;
  r.build();
  AudioEngine& e = r.eng;
  const double ppq = r.song.ppq;
  const double pa = ppq * 16, pb = ppq * 24;  // bars 5 to 7 (4/4)
  e.setPunch(pa, pb);
  e.setLoopRange(static_cast<Tick>(pa), static_cast<Tick>(pb));
  e.setRecordRoll(true, ppq * 4, 0);
  e.play();
  r.run(256);
  expect(e.position() < pa && e.position() > pa - ppq * 4 - 1, "pre-roll: playback starts a bar before the punch in");
  r.run(48000 * 3);
  expect(e.playing() && e.position() >= pa && e.position() < pb, "pre-roll: then loops in the punch range (first pass only)");
  e.stop();
  r.run(256);
  // Post-roll: plays on past the punch out and stops there.
  e.setRecordRoll(true, 0, ppq * 2);
  e.setPosition(static_cast<Tick>(pa));
  e.play();
  r.run(256);
  expect(std::fabs(e.position() - pa) < ppq, "no pre-roll: from the punch in");
  const int stops = e.rollStops();
  bool past = false;
  for (int k = 0; k < 400 && e.playing(); ++k) {
    r.run(1024);
    past = past || e.position() > pb;
  }
  expect(past, "post-roll: the transport passes the punch out (no wrap)");
  expect(!e.playing() && e.rollStops() == stops + 1, "post-roll: it stops by itself");
  // Not armed (or no punch): neither applies.
  e.setRecordRoll(false, ppq * 4, ppq * 2);
  e.setPosition(static_cast<Tick>(pa));
  e.play();
  r.run(256);
  expect(e.position() >= pa - 1, "not armed: no pre-roll");
  r.run(48000 * 5);
  expect(e.playing(), "not armed: the selection loops on, no post-roll stop");
  e.stop();
}

}  // namespace

// Stage 3: what the Recording card says.
void testCardTexts() {
  expect(recinfo::modeLine(true, true, false, 2) == "punch, pass 2" && recinfo::modeLine(true, false, false, 0) == "pass 1" &&
             recinfo::modeLine(false, false, true, 3) == "loop",
         "card: mode line");
  expect(recinfo::clock(7.46) == "00:07.4" && recinfo::clock(723.0) == "12:03.0" && recinfo::clock(3723.5) == "1:02:03.5", "card: clock");
  // 48 kHz stereo 32-bit float: 384 kB a second per track, so 10 GB is about 434 minutes for one track.
  const double m = recinfo::minutesLeft(10e9, 48000, 1, 4);
  expect(std::fabs(m - 10e9 / 384000.0 / 60.0) < 1e-6 && std::fabs(recinfo::minutesLeft(10e9, 48000, 2, 4) - m / 2) < 1e-6, "card: disk minutes");
  expect(recinfo::durationText(m) == "7 h 14 min" && recinfo::durationText(42.9) == "42 min" && recinfo::durationText(0.5) == "under a minute",
         "card: disk time text");
}

void testRecordView() {
  using namespace recinfo;
  // The card (default): the card, or the pill when folded; the window folds away when a take starts.
  View v = view(kCard, true, false, false, false, false);
  expect(v.card && !v.strip && !v.pill && !v.window, "view: card when ready");
  v = view(kCard, true, false, true, true, true);
  expect(v.card && !v.window, "view: card folds the Record window away when a take starts");
  v = view(kCard, true, false, true, true, false);
  expect(v.card && v.window, "view: the Record window opened again while recording stays");
  v = view(kCard, true, true, false, true, false);
  expect(v.pill && !v.card && !v.strip, "view: H folds the card to the pill");
  // The strip under the top bar replaces the card.
  v = view(kStrip, true, false, true, true, true);
  expect(v.strip && !v.card && !v.window, "view: strip while recording, the window folded into it");
  v = view(kStrip, true, true, false, true, false);
  expect(v.pill && !v.strip, "view: H folds the strip to the pill");
  // Keep the full window while recording: the window stays, the card steps aside while it shows.
  v = view(kKeepWindow, true, false, true, true, true);
  expect(v.window && !v.card && !v.strip, "view: keep the full window while recording");
  v = view(kKeepWindow, true, false, false, true, false);
  expect(v.card && !v.window, "view: keep the window: the card when the window is closed");
  // Record off: nothing but a window the user left open; unknown values act as the card.
  v = view(kStrip, false, false, false, false, false);
  expect(!v.card && !v.strip && !v.pill && !v.window, "view: nothing when record is off");
  v = view(7, true, false, false, false, false);
  expect(v.card, "view: an unknown form is the card");
}

int main() {
  testPrePostRoll();
  testTrimAndPolarity();
  testMonitorRules();
  testLowLatency();
  testSavedAndLocal();
  testRecordSafe();
  testCardTexts();
  testRecordView();
  return finish();
}
