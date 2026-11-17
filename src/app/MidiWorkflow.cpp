// Gridline — keyboard / MIDI workflow (J-17), app side: remote control links,
// learning and multilink, controller templates, recording (count-in, takes,
// automation), step record, transport actions, external clock following.
#include <array>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include "../../third_party/imgui/imgui.h"
#include "MenuParityUi.h"
#include "App.h"
#include "AutomationEditor.h"
#include "PluginUI.h"
#include "ScriptUi.h"
#include "ChordEditorUi.h"
#include "J31Ui.h"
#include "LiveIo.h"
#include "LiveLook.h"
#include "LogicPlugins.h"
#include "MidiArm.h"
#include "RecordMoreUi.h"
#include "../core/Automation.h"
#include "../core/Meter.h"
#include "../packs/PacksUi.h"
#include "MackieUi.h"

namespace gl {

namespace fs = std::filesystem;

namespace {

// R11-03: per MIDI key, the other armed / monitored sounds its note plays in Smooth (they end with it).
std::array<std::vector<ObjectId>, 128> gMidiLayers;

bool learnable(const MidiMessage& m) {
  int k = m.kind();
  return k == 0xB0 ? (m.d1 != 64 && m.d1 < 120) : (k == 0x90 && m.d2 > 0) || k == 0xE0 || k == 0xD0 || k == 0xA0 || k == 0xC0;
}

// Fills a link's source from a message.
void sourceFrom(ControllerLink& l, const MidiMessage& m) {
  switch (m.kind()) {
    case 0x90:
    case 0x80: l.source = RemoteSource::Note; l.cc = m.d1; break;
    case 0xE0: l.source = RemoteSource::PitchBend; break;
    case 0xD0: l.source = RemoteSource::ChannelPressure; break;
    case 0xA0: l.source = RemoteSource::KeyPressure; l.cc = m.d1; break;
    case 0xC0: l.source = RemoteSource::Program; l.cc = m.d1; break;
    default: l.source = RemoteSource::CC; l.cc = m.d1; break;
  }
  l.midiChannel = m.channel();
}

double stepOf(const ParamDesc& d) {
  if (d.kind == ParamKind::Toggle) return 1.0;
  if (d.kind == ParamKind::Choice && d.choices.size() > 1) return 1.0 / static_cast<double>(d.choices.size() - 1);
  return 0.0;
}

}  // namespace

std::string App::templatesUserDir() const { return (fs::path(configDir()) / "midi-templates").string(); }

void App::loadControllerTemplates() {
  std::vector<std::string> dirs;
  if (!assetsDir.empty()) dirs.push_back((fs::path(assetsDir) / "midi" / "controllers").string());
  dirs.push_back(templatesUserDir());
  std::vector<std::string> errs;
  controllerTemplates = gl::loadControllerTemplates(dirs, &errs);
  for (const auto& e : errs) notify("Controller template: " + e, true);
}

void App::publishMidi() {
  if (!midi) return;
  std::vector<uint32_t> rack;
  for (const auto& c : doc.song().channels) rack.push_back(c.id);
  midi->outputLatency = static_cast<double>(audioBuffer) / std::max(1, audioSampleRate);
  midi->publish(rack);
}

void App::openRemoteDialog(ParamId target) {
  const auto& links = doc.song().links;
  for (size_t i = 0; i < links.size(); ++i)
    if (links[i].action == RemoteAction::None && links[i].owner == target.owner && links[i].index == target.index) {
      openRemoteDialogForLink(static_cast<int>(i));
      return;
    }
  remoteEdit = ControllerLink{};
  remoteEdit.owner = target.owner;
  remoteEdit.index = target.index;
  remoteEdit.midiChannel = -1;
  remoteEditIndex = -1;
  remoteListening = true;
  midiLearning = true;
  midiLearnTarget = target;
  showRemoteDialog = true;
}

void App::openRemoteDialog(RemoteAction action) {
  const auto& links = doc.song().links;
  for (size_t i = 0; i < links.size(); ++i)
    if (links[i].action == action) {
      openRemoteDialogForLink(static_cast<int>(i));
      return;
    }
  remoteEdit = ControllerLink{};
  remoteEdit.action = action;
  remoteEdit.button = RemoteButton::Momentary;
  remoteEditIndex = -1;
  remoteListening = true;
  midiLearning = false;
  showRemoteDialog = true;
}

void App::openRemoteDialogForLink(int index) {
  const auto& links = doc.song().links;
  if (index < 0 || index >= static_cast<int>(links.size())) return;
  remoteEdit = links[static_cast<size_t>(index)];
  remoteEditIndex = index;
  remoteListening = false;
  midiLearning = remoteEdit.action == RemoteAction::None;
  midiLearnTarget = {remoteEdit.owner, remoteEdit.index};
  showRemoteDialog = true;
}

void App::startPlayback() {
  if (engine.playing()) return;
  if (recmore::holdPlay(*this)) return;  // K-123 (0.7.7): start on the first note
  if (recordArmed && !audioRecFinishing && engine.mode() != PlayMode::Live) {
    // Armed tracks capture from the transport's first block: the punch range and the recording flag
    // are set before Play, not a UI frame later in processAudioRecording (a slow frame there lost the
    // start of the take, and a first loop pass started late).
    Tick ra = 0, rb = 0;  // checkpoint 56: Clips punches at its loop brace
    const bool sel = livelook::recordRange(*this, ra, rb);
    if (recPunch && sel && engine.mode() == PlayMode::Song)
      engine.setPunch(static_cast<double>(ra), static_cast<double>(rb));
    else
      engine.setPunch(0, 0);
    engine.setRecording(true);
  }
  if (recordArmed && countInBars > 0) {
    // K-179: whole bars of the meter in effect where recording starts (the pattern's own meter in
    // pattern mode).
    const Song& cs = doc.song();
    Tick bar = cs.barTicks();
    if (engine.mode() == PlayMode::Song) bar = MeterMap(cs).fullBarLenAt(static_cast<Tick>(std::max(0.0, engine.position())));
    else if (engine.mode() == PlayMode::Pattern)
      if (const Pattern* cp = cs.pattern(curPattern)) bar = cs.patternBarTicks(*cp);
    engine.playWithCountIn(static_cast<double>(countInBars * bar));
  } else {
    engine.play();
  }
}

void App::runAction(RemoteAction action, bool fromMmc) {
  const Song& s = doc.song();
  switch (action) {
    case RemoteAction::PlayPause:
      if (engine.playing()) {
        if (!fromMmc) engine.pause();
      } else {
        menuparity::playPressed(*this);  // J-112: plugin playback sounding: stopped instead
      }
      break;
    case RemoteAction::Stop: menuparity::stopPressed(*this); break;  // J-109: twice = panic
    case RemoteAction::Record:
      recordArmed = !recordArmed;
      notify(recordArmed ? "Recording armed" : "Recording off");
      break;
    case RemoteAction::SongMode:
      engine.setMode(engine.mode() == PlayMode::Song ? PlayMode::Pattern : PlayMode::Song);
      break;
    case RemoteAction::Metronome: metronome = !metronome; break;
    case RemoteAction::CountIn:
      countInBars = countInBars > 0 ? 0 : std::max(1, countInLength);
      notify(countInBars == 1 ? "Count-in: 1 bar" : countInBars > 1 ? "Count-in: " + std::to_string(countInBars) + " bars" : std::string("Count-in off"));
      break;
    case RemoteAction::StepRecord:
      stepRecord = !stepRecord;
      notify(stepRecord ? "Step record on" : "Step record off");
      break;
    case RemoteAction::LoopRecord:
      recOptions.takes = !recOptions.takes;
      notify(recOptions.takes ? "Loop recording: a new take per pass" : "Loop recording: merge passes");
      break;
    case RemoteAction::NextPattern:
    case RemoteAction::PrevPattern: {
      int i = s.patternIndex(curPattern), n = static_cast<int>(s.patterns.size());
      if (n == 0) break;
      i = (i + (action == RemoteAction::NextPattern ? 1 : n - 1)) % n;
      selectPattern(s.patterns[static_cast<size_t>(i)].id);
      break;
    }
    case RemoteAction::Undo: doc.undo(); break;
    case RemoteAction::Redo: doc.redo(); break;
    case RemoteAction::TapTempo: tapTempo(); break;
    default: break;
  }
}

// ------------------------------------------------------------ tap tempo ----

double App::tapMeasure() {
  const double t = ImGui::GetTime();  // frame time: one clock for the mouse, the keys and controllers
  if (tapper.taps.empty() || t - tapper.taps.back() > tapper.resetAfter) tapRunSerial_ = 0;  // a new run
  else if (t <= tapper.taps.back()) return tapper.bpm();  // a second source in the same frame is one tap
  return tapper.tap(t);
}

void App::tapTempo() {
  const double bpm = tapMeasure();
  if (bpm <= 0 || bpm >= 522.0) return;  // nothing to follow yet, or a bounce rather than a beat
  const uint64_t before = doc.history().stateSerial();
  // The step of this run if it is still the newest; an edit or undo since then starts a new one.
  doc.amendStep(tapRunSerial_, [&] { doc.setParam(songParam(SongParam::Tempo), std::round(bpm * 10.0) / 10.0, false); });
  const uint64_t after = doc.history().stateSerial();
  if (after != before) tapRunSerial_ = after;
}

void App::tapReset() {
  tapper.reset();
  tapRunSerial_ = 0;
}

bool App::tapListening() const { return !tapper.taps.empty() && ImGui::GetTime() - tapper.taps.back() <= tapper.resetAfter; }

double App::tapAge() const { return tapper.taps.empty() ? 1e9 : ImGui::GetTime() - tapper.taps.back(); }

double App::recordTick(double stamp) const {
  if (stamp < 0) stamp = MidiManager::clockNow();
  // What the performer hears left the engine this long ago: the device buffer plus the plugin delay.
  const double sr = engine.sampleRate() > 0 ? engine.sampleRate() : 44100.0;
  const double latency = static_cast<double>(std::max(0, audioBuffer) + std::max(0, engine.outputLatency())) / sr;
  double t = 0;
  const double heard = engine.heardTick(stamp, latency, &t) ? t : engine.position();
  // Checkpoint 48: Clips' Session record places the notes in the playing clip's own ticks.
  ObjectId sp = kNoId;
  Tick sl = 0;
  double so = 0;
  if (livelook::sessionRecordTarget(*this, sp, sl, so)) return heard - so;
  return heard;
}

namespace {
// K-123 (4.2): where a note played while nothing records was heard: the transport playing a pattern or
// the song (not Clips' clip launching), else nowhere (stopped).
CaptureTime captureTimeOf(const App& a, double stamp) {
  CaptureTime t;
  if (!a.engine.playing() || a.engine.countingIn() || a.engine.mode() == PlayMode::Live) return t;
  t.mode = a.engine.mode() == PlayMode::Song ? CaptureTime::Song : CaptureTime::Pattern;
  t.tick = std::max(0.0, a.recordTick(stamp));
  t.pattern = a.curPattern;
  return t;
}
}  // namespace

void App::playedNoteOn(ObjectId channel, int pitch, int velocity, double stamp) {
  j31::chordEditorNote(*this, pitch, true);  // J-31: learn chords from MIDI input
  recmore::noteWhileWaiting(*this, channel, pitch, velocity);  // K-123 (0.7.7): the first note starts the take
  if (stepRecord && !engine.playing()) {
    stepRecorder.press(pitch, velocity);
    return;
  }
  // K-123 (0.7.7): punched out (quick punch), notes play and are not recorded.
  if (recorder.active() && midiarm::records(*this, channel) && (recordArmed || !recmore::quickPunch(*this))) {  // R11-03 / K-123: with tracks or channels armed, only theirs record
    recorder.noteOn(doc, channel, pitch, velocity, recordTick(stamp));
    logicplug::syncRecordSteps(*this);
  } else if (!recorder.active()) {
    capture.noteOn(channel, pitch, velocity, now, captureTimeOf(*this, stamp));  // K-112c: kept for Capture (K-123: and where it was heard)
  }
}

void App::playedNoteOff(ObjectId channel, int pitch, double stamp) {
  j31::chordEditorNote(*this, pitch, false);
  if (stepRecord && !engine.playing()) {
    Tick g = piano.effGrid > 0 ? piano.effGrid : snapTicks(doc.song(), piano.snap);
    stepRecorder.step = std::max<Tick>(1, g);
    stepRecorder.release(doc, curPattern, selChannel, pitch);
    return;
  }
  if (recorder.active()) {
    recorder.noteOff(doc, channel, pitch, recordTick(stamp));
    logicplug::syncRecordSteps(*this);
  } else {
    capture.noteOff(channel, pitch, now, captureTimeOf(*this, stamp).tick);
  }
}

void App::stepRecordKey(int dir) {
  const Tick g = piano.effGrid > 0 ? piano.effGrid : snapTicks(doc.song(), piano.snap);
  stepRecorder.step = std::max<Tick>(1, g);
  if (dir > 0) {
    stepRecorder.right();
  } else if (dir < 0) {
    stepRecorder.left();
  } else {
    stepRecorder.backspace(doc, curPattern, selChannel);
  }
}

void App::processMidi() {
  if (midi && midi->update()) publishMidi();  // the MIDI service answered (or stopped answering)
  if (projectLoadBlocking() && midi) {  // J-167: no input reaches the project behind the loading window
    midi->pollActions();
    midi->poll();
    return;
  }
  // checkpoint 56 (N55-10): Clips' MIDI monitor; R10-03: Smooth's armed / monitored track; K-123: armed channels
  engine.setMidiTarget(livelook::active(*this) && !logicplug::active(*this) ? liveio::midiTarget(*this, selChannel) : midiarm::liveChannel(*this));

  // ---- recording session follows the transport ----
  // K-226: from the count-in on, so notes played in its last beat (pickups) are kept.
  ObjectId sessPattern = kNoId;  // Checkpoint 48: Clips' Session record (the clip launch mode plays on)
  Tick sessLength = 0;
  double sessOrigin = 0;
  const bool sess = livelook::sessionRecordTarget(*this, sessPattern, sessLength, sessOrigin);
  // K-123 (0.7.7): quick punch keeps the take open while the transport plays, punched out or in.
  const bool recNow = ((recordArmed || (recmore::quickPunch(*this) && recorder.active())) && engine.playing() && engine.mode() != PlayMode::Live) || sess;
  if (recNow && !recorder.active()) {
    RecordOptions o = recOptions;
    Tick g = piano.effGrid > 0 ? piano.effGrid : snapTicks(doc.song(), piano.snap);
    o.grid = std::max<Tick>(1, g);
    const Song& s = doc.song();
    RecordSetup st;
    if (sess) {  // the notes join the clip's (the reference's Session record overdubs), pass after pass
      o.overdub = true;
      o.takes = false;
      st.start = std::max<Tick>(0, static_cast<Tick>(std::llround(std::max(0.0, recordTick()))));
      st.pickup = std::max<Tick>(1, s.beatTicks());
      recorder.beginPattern(doc, sessPattern, o, sessLength, st);
    } else {
      // Where it starts: the transport waits there during a count-in; armed while playing, the tick heard now.
      const double at = engine.countingIn() ? engine.position() : recordTick();
      st.start = std::max<Tick>(0, static_cast<Tick>(std::llround(std::max(0.0, at))));
      if (engine.mode() == PlayMode::Song) {
        st.pickup = MeterMap(s).segAt(st.start).beat;
        Tick ra = 0, rb = 0;  // checkpoint 56: Clips loops at its brace while Loop is on, punches at it
        const bool sel = livelook::recordRange(*this, ra, rb);
        if (sel) {
          if (!livelook::active(*this) || livelook::loopOn(*this)) {
            st.loopStart = ra;
            st.loopEnd = rb;
          }
          if (recPunch) {
            st.punchIn = ra;
            st.punchOut = rb;
          }
        }
        if (logicplug::active(*this)) {  // R05-02: onto the selected track (R10-03: the armed one first)
          const int armed = logicplug::armedTrack(*this);
          st.track = armed >= 0 ? armed : logicplug::focusTrack(*this);
          st.takeFolder = settings.getBool("record.midi.takeFolders");  // R14-04: a pass over regions: a take folder
          st.takeLanes = settings.getBool("record.takeLanes");
        }
        recorder.beginSong(doc, curPattern, o, st);
        if (logicplug::active(*this)) logicplug::startLayerTakes(*this, o, st, curPattern);  // R11-03: a take per armed track
      } else if (const Pattern* p = s.pattern(curPattern)) {
        st.pickup = std::max<Tick>(1, s.patternBarTicks(*p) / std::max(1, p->meterNum > 0 ? p->meterNum : s.timeSigNum));
        recorder.beginPattern(doc, curPattern, o, std::max<Tick>(1, s.patternContentLength(*p)), st);
      }
    }
    // K-123 (0.7.7): the note that started the take (start on the first note) records at its start.
    for (const recmore::HeldNote& h : recmore::takeStartNotes()) {
      if (recorder.active() && midiarm::records(*this, h.channel)) recorder.noteOn(doc, h.channel, h.pitch, h.velocity, static_cast<double>(st.start));
    }
  } else if (recorder.active() && (!recNow || recorder.songMode() != (engine.mode() == PlayMode::Song))) {
    recorder.end(doc, lastRecPos_);  // stopped, disarmed, or the play mode changed (a new take starts next frame)
    logicplug::endLayerTakes(*this, lastRecPos_);
  }

  // ---- multilink: remember the controls touched in Gridline ----
  if (multilink && doc.touchSerial() != multilinkSerial_) {
    ParamId t = doc.lastTouchedParam();
    if (std::find(multilinkQueue.begin(), multilinkQueue.end(), t) == multilinkQueue.end() && multilinkQueue.size() < 64)
      multilinkQueue.push_back(t);
  }
  if (!multilink) multilinkQueue.clear();

  if (!midi) {
    multilinkSerial_ = doc.touchSerial();
    followRecording();
    return;
  }
  for (const auto& c : doc.song().channels)  // MIDI Out channels → device (K-129: and Tracker Samplers sending their notes)
    if (c.plugin.type == "gl.midiout" || c.plugin.type == "gl.tracker")
      if (auto rt = builder.channelRuntime(c.id)) {
        MidiOutRing::Msg msg;
        while (rt->midiOut.pop(msg)) midi->send(msg.s, msg.a, msg.b);
      }

  // ---- external sync (MI-02) ----
  if (midi->syncSource == SyncSource::Mtc) {
    const double t = MidiManager::clockNow();
    bool locked = false, located = false;
    double master = 0, locateSec = 0;
    std::lock_guard<std::mutex> lk(midi->followerMutex());
    MtcReceiver& rx = midi->mtc();
    locked = rx.locked(t);
    master = rx.seconds(t);
    located = rx.takeLocate(locateSec);
    SyncChase& chase = midi->chase();
    chase.offset = midi->clockInOffsetMs / 1000.0;
    chase.tolerance = std::max(0.005, midi->chaseToleranceMs / 1000.0);
    TempoMap map;
    map.build(doc.song());
    auto tickOf = [&map](double sec) { return static_cast<Tick>(std::llround(map.tickAtSeconds(std::max(0.0, sec)))); };
    if (located && !locked && !engine.playing()) engine.setPosition(tickOf(locateSec + chase.offset));  // full frame: locate
    const auto act = chase.update(t, locked, master, engine.playing(), map.secondsAt(engine.position()));
    if (act.kind == SyncChase::Action::Start) {
      engine.setPosition(tickOf(act.position));
      engine.play();
    } else if (act.kind == SyncChase::Action::Stop) {
      engine.pause();
    } else if (act.kind == SyncChase::Action::Locate) {
      engine.setPosition(tickOf(act.position));
    }
  } else if (midi->syncSource == SyncSource::Clock) {
    std::vector<MidiClockFollower::Cmd> cmds;
    double bpm = 0, masterTick = 0;
    bool running = false;
    {
      std::lock_guard<std::mutex> lk(midi->followerMutex());
      cmds = midi->follower().takeCommands();
      if (midi->follower().hasTempo()) bpm = midi->follower().bpm();
      running = midi->follower().running() && midi->follower().hasTempo();
      masterTick = static_cast<double>(midi->follower().clocks()) * doc.song().ppq / 24.0;
    }
    {  // drift: the song position the clock count implies (start / stop come with the clock's own commands)
      SyncChase& chase = midi->chase();
      const double ticksPerSec = doc.song().tempo / 60.0 * doc.song().ppq;
      chase.offset = midi->clockInOffsetMs / 1000.0 * ticksPerSec;
      chase.tolerance = std::max(1.0, midi->chaseToleranceMs / 1000.0 * ticksPerSec);
      const auto act = chase.update(MidiManager::clockNow(), running && cmds.empty(), masterTick, engine.playing(), engine.position());
      if (act.kind == SyncChase::Action::Locate) engine.setPosition(static_cast<Tick>(std::llround(std::max(0.0, act.position))));
    }
    for (const auto& c : cmds) {
      if (c.kind == MidiClockFollower::Cmd::Start) {
        engine.setPosition(0);
        engine.play();
      } else if (c.kind == MidiClockFollower::Cmd::Continue) {
        engine.play();
      } else if (c.kind == MidiClockFollower::Cmd::Stop) {
        engine.pause();
      } else {
        engine.setPosition(static_cast<Tick>(std::llround(c.sixteenths * doc.song().ppq / 4.0)));
      }
    }
    static double lastTempoSet = -1;
    if (bpm >= 10 && bpm <= 522 && std::fabs(bpm - doc.song().tempo) > 0.05 && now - lastTempoSet > 0.25) {
      doc.setParam(songParam(SongParam::Tempo), std::round(bpm * 100) / 100, true);
      lastTempoSet = now;
    }
  }
  for (RemoteAction act : midi->pollActions()) runAction(act, true);  // MMC
  controllerScriptsTick(*this);  // J-24: device scripts (load, onIdle, onRefresh)
  mackieTick(*this);             // K-214: Mackie Control / HUI surfaces

  const Song& song = doc.song();
  for (const auto& e : midi->poll()) {
    MidiMessage m{e.status, e.data1, e.data2, e.portNumber};
    const int kind = m.kind();
    menuparity::logMidi(*this, m);  // J-27: score logger

    // The remote control dialog takes the next control moved.
    if (remoteListening && showRemoteDialog && learnable(m)) {
      sourceFrom(remoteEdit, m);
      if (e.portNumber >= 0 && remoteEdit.port >= 0) remoteEdit.port = e.portNumber;
      remoteListening = false;
      continue;
    }

    // J-150: a controller bound to a DJ deck drives the deck (which reads the device itself).
    if (packs::claimMidi(*this, e.port)) continue;

    // Controller script of the device (J-24) sees the message first.
    if (controllerScriptMidi(*this, e.port, m)) {
      lastCcTime_ = now;  // its edits merge until the controller pauses
      continue;
    }
    if (mackieMidi(*this, e.port, m)) {  // K-214
      lastCcTime_ = now;
      continue;
    }

    // K-99: an MPE controller (Settings > MIDI): a member channel's pitch bend, pressure and
    // CC 74 belong to its note (the engine plays them); a recording keeps them in the note's lanes.
    const MidiRouting& mr = midi->routing;
    auto routedChannel = [&](int midiCh) {
      if (mr.byChannel && midiCh != mr.omniChannel && mr.route[midiCh] >= 0 && mr.route[midiCh] < static_cast<int>(song.channels.size()))
        return song.channels[static_cast<size_t>(mr.route[midiCh])].id;
      return selChannel;
    };
    auto mpeFeed = [&](double stamp) {
      MpeInput::Change ch[4];
      mpeIn.bendRange = mr.mpeBendRange;
      const int k = mpeIn.feed(m.status, m.d1, m.d2, ch, 4);
      if (recorder.active())
        for (int i = 0; i < k; ++i)
          recorder.expression(routedChannel(ch[i].midiChannel), j31::incomingPitch(*this, ch[i].key), ch[i].dim, ch[i].value, recordTick(stamp));
    };
    if (mr.mpe && m.channel() != 0 && (kind == 0xE0 || kind == 0xD0 || (kind == 0xB0 && m.d1 == 74))) {
      mpeFeed(e.time);
      continue;
    }

    // Links.
    bool handled = false;
    for (const ControllerLink& l : doc.song().links) {
      if (!remoteMatches(l, m)) continue;
      handled = true;
      if (l.action != RemoteAction::None) {
        if (remote.trigger(l, m)) runAction(l.action);
        continue;
      }
      ParamId pid{l.owner, l.index};
      auto d = describeParam(doc.song(), pid);
      auto v = getParam(doc.song(), pid);
      if (!d || !v) continue;
      double out = 0;
      if (remote.process(l, m, d->toNormalized(*v), &out, stepOf(*d))) {
        const double value = d->fromNormalized(out);
        automationFromController() = true;  // N58-01: recorded below, with the event's time
        if (recorder.active())  // K-226: part of the recording's undo step
          recorder.edit(doc, [&] { doc.setParam(pid, value, true); });
        else
          doc.setParam(pid, value, true);
        automationFromController() = false;
        lastCcTime_ = now;
        if (recorder.active() && !automationCapturing(*this) && recordFilterTakes(recorder.options(), m.status, m.d1)) {  // K-123: the input filter
          recorder.controller(pid, out, recordTick(e.time));  // else captured by the automation recorder
        }
      }
    }

    // K-112D (D5, R11-06): Smooth's Smart Controls in learn mode: a control not linked yet links to
    // the screen control in focus, without a dialog (the pane stays in learn mode for the next one).
    if (!handled && smartLearn.owner != kNoId && ImGui::GetFrameCount() - smartLearnFrame <= 2 && learnable(m) && describeParam(doc.song(), smartLearn)) {
      ControllerLink l;
      sourceFrom(l, m);
      const ParamId t = smartLearn;
      l.owner = t.owner;
      l.index = t.index;
      if (l.source == RemoteSource::Note) l.button = RemoteButton::Toggle;
      doc.editSong("link controller", [&](Song& s) {
        s.links.erase(std::remove_if(s.links.begin(), s.links.end(),
                                     [&](const ControllerLink& x) { return x.action == RemoteAction::None && x.owner == t.owner && x.index == t.index; }),
                      s.links.end());
        s.links.push_back(l);
      });
      remote.forget(l);
      notify("Linked " + remoteSourceLabel(l) + " to " + paramDisplayName(doc.song(), t));
      continue;
    }

    // Multilink: a control not linked yet links to the next touched control.
    if (!handled && multilink && !multilinkQueue.empty() && learnable(m)) {
      ControllerLink l;
      sourceFrom(l, m);
      ParamId t = multilinkQueue.front();
      multilinkQueue.erase(multilinkQueue.begin());
      if (describeParam(doc.song(), t)) {
        l.owner = t.owner;
        l.index = t.index;
        if (l.source == RemoteSource::Note) l.button = RemoteButton::Toggle;
        doc.editSong("link controller", [&](Song& s) {
          s.links.erase(std::remove_if(s.links.begin(), s.links.end(),
                                       [&](const ControllerLink& x) { return x.action == RemoteAction::None && x.owner == t.owner && x.index == t.index; }),
                        s.links.end());
          s.links.push_back(l);
        });
        notify("Linked " + remoteSourceLabel(l) + " to " + paramDisplayName(doc.song(), t));
      }
      continue;
    }

    // Controller template of the device (transport, focused-window knobs, faders).
    if (!handled && (kind == 0xB0 || kind == 0x90 || kind == 0x80)) {
      const ControllerTemplate* tpl = nullptr;
      const MidiInputConfig* cfg = midi->inputConfig(e.port);
      std::string type = cfg ? cfg->controllerType : "";
      if (type.empty())
        tpl = matchControllerTemplate(controllerTemplates, e.port);
      else if (type != "none")
        tpl = findControllerTemplate(controllerTemplates, type);
      if (tpl) {
        TemplateHit h = templateLookup(*tpl, m);
        if (h.kind == TemplateHit::Action) {
          uint64_t key = (static_cast<uint64_t>(m.status & 0xF0) << 16) | (static_cast<uint64_t>(m.channel()) << 8) | m.d1;
          bool down = kind == 0x90 ? m.d2 > 0 : kind == 0xB0 ? m.d2 >= 64 : false;
          bool was = templateKnobDown_[key] != 0;
          templateKnobDown_[key] = down ? 1 : 0;
          if (down && (!was || kind == 0x90)) runAction(h.action);
          handled = true;
        } else if (h.kind == TemplateHit::Knob || h.kind == TemplateHit::Fader) {
          ParamId pid;
          if (h.kind == TemplateHit::Knob) {
            PluginTarget t{focusPluginOwner, focusPluginSlot};
            if (t.owner == kNoId || !t.state(song)) t = PluginTarget{selChannel, -1};
            pid = t.pid(h.index);
          } else if (static_cast<size_t>(h.index) + 1 < song.mixer.size()) {
            pid = mixerParam(song.mixer[static_cast<size_t>(h.index) + 1].id, MixerParam::Volume);
          }
          auto d = describeParam(doc.song(), pid);
          auto v = getParam(doc.song(), pid);
          if (d && v) {
            ControllerLink l;
            sourceFrom(l, m);
            l.owner = pid.owner;
            l.index = pid.index;
            l.takeover = tpl->takeover;
            if (h.kind == TemplateHit::Knob) l.encoder = tpl->knobEncoder;
            double out = 0;
            if (remote.process(l, m, d->toNormalized(*v), &out, stepOf(*d))) {
              const double value = d->fromNormalized(out);
              automationFromController() = true;  // N58-01: recorded below, with the event's time
              if (recorder.active())  // K-226: part of the recording's undo step
                recorder.edit(doc, [&] { doc.setParam(pid, value, true); });
              else
                doc.setParam(pid, value, true);
              automationFromController() = false;
              lastCcTime_ = now;
              if (recorder.active() && !automationCapturing(*this) && recordFilterTakes(recorder.options(), m.status, m.d1)) {  // K-123: the input filter
                recorder.controller(pid, out, recordTick(e.time));  // else captured by the automation recorder
              }
            }
          }
          handled = true;
        }
      }
    }
    if (handled) continue;

    // Notes and the sustain pedal: recording / step record.
    if (kind == 0x90 || kind == 0x80 || (kind == 0xB0 && m.d1 == 64)) {
      const MidiInputConfig* cfg = midi->inputConfig(e.port);
      if (cfg && !cfg->notes) continue;
      ObjectId ch = midiarm::liveChannel(*this);  // R10-03 (Smooth: the armed / monitored track's sound; K-123: an armed channel)
      const MidiRouting& r = midi->routing;
      if (r.byChannel && m.channel() != r.omniChannel && r.route[m.channel()] >= 0 &&
          r.route[m.channel()] < static_cast<int>(song.channels.size()))
        ch = song.channels[static_cast<size_t>(r.route[m.channel()])].id;
      if (kind == 0xB0) {  // the pedal as played (the notes keep the lengths the keys were held)
        if (recorder.active()) recorder.pedal(ch, m.d2 >= 64, recordTick(e.time));
      } else if (m.noteOn()) {
        const int pitch = j31::incomingPitch(*this, m.d1);
        playedNoteOn(ch, pitch, m.d2, e.time);  // J-31: snap incoming MIDI to the scale
        if (r.mpe && m.channel() != 0) mpeFeed(e.time);  // K-99: the note starts with its channel's values
        // R11-03: Smooth: every other armed or monitored track plays (and records) the note too,
        // as the reference layers them (the engine plays the first one from the input itself).
        if (midiarm::layering(*this) && ch == midiarm::liveChannel(*this) && pitch >= 0 && pitch < 128) {
          std::vector<ObjectId>& lay = gMidiLayers[static_cast<size_t>(pitch)];
          for (ObjectId o : lay) {
            engine.noteOff(o, pitch);  // (a retrigger)
          }
          const std::vector<ObjectId> lives = midiarm::liveChannels(*this);
          lay.assign(lives.begin() + 1, lives.end());
          for (ObjectId o : lay) {
            engine.noteOn(o, pitch, static_cast<float>(m.d2) / 127.0f);
            // K-123 (0.7.7): punched out (quick punch), the layered notes play and are not recorded either.
            if (recordArmed || !recmore::quickPunch(*this)) midiarm::layerNote(*this, o, pitch, m.d2, true, recordTick(e.time));
          }
        }
      } else if (!r.ignoreNoteOff) {
        const int pitch = j31::incomingPitch(*this, m.d1);
        playedNoteOff(ch, pitch, e.time);
        if (r.mpe && m.channel() != 0) mpeFeed(e.time);
        if (pitch >= 0 && pitch < 128) {  // R11-03: the layered notes end with it
          std::vector<ObjectId>& lay = gMidiLayers[static_cast<size_t>(pitch)];
          for (ObjectId o : lay) {
            engine.noteOff(o, pitch);
            midiarm::layerNote(*this, o, pitch, 0, false, recordTick(e.time));
          }
          lay.clear();
        }
      }
    }
  }
  // A controller gesture ends after a short pause: one undo step per move.
  if (lastCcTime_ >= 0 && now - lastCcTime_ > 0.4) {
    doc.endGesture();
    lastCcTime_ = -1;
  }
  multilinkSerial_ = doc.touchSerial();
  followRecording();
}

void App::followRecording() {
  if (!recorder.active()) return;
  // After this frame's input: the notes were placed where they were heard, then the position moves on
  // (a loop wraps after the notes played before it).
  const Song& s = doc.song();
  if (recorder.songMode()) {
    if (playlist.selEnd > playlist.selStart)
      recorder.setSongLoop(playlist.selStart, playlist.selEnd);
    else
      recorder.setSongLoop(0, 0);
  } else if (Tick sl = 0; [&] {
               ObjectId sp = kNoId;
               double so = 0;
               return livelook::sessionRecordTarget(*this, sp, sl, so);
             }()) {
    recorder.setLoopLength(sl);  // Clips' Session record: the loop is the clip's length (it grows)
  } else if (const Pattern* p = s.pattern(recorder.pattern())) {
    recorder.setLoopLength(std::max<Tick>(1, s.patternContentLength(*p)));  // the loop follows the pattern
  }
  const double pos = recordTick();
  recorder.position(doc, pos);
  logicplug::syncRecordSteps(*this);
  if (recorder.songMode()) logicplug::followLayerTakes(*this, pos);
  lastRecPos_ = pos;
  // A new take is shown (pattern mode: it is what plays, so it stays selected; song mode: once, the
  // pattern can be changed while recording goes on).
  const ObjectId take = recorder.pattern();
  if (take != kNoId && take != curPattern && (!recorder.songMode() || take != recTakeShown_)) selectPattern(take);
  recTakeShown_ = take;
}

}  // namespace gl
