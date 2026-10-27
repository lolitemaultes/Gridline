// Gridline — keyboard / MIDI workflow (J-17), app side: remote control links,
// learning and multilink, controller templates, recording (count-in, takes,
// automation), step record, transport actions, external clock following.
#include <algorithm>
#include <cmath>
#include <filesystem>
#include "MenuParityUi.h"
#include "App.h"
#include "AutomationEditor.h"
#include "PluginUI.h"
#include "ScriptUi.h"
#include "ChordEditorUi.h"
#include "J31Ui.h"
#include "../core/Automation.h"
#include "../packs/PacksUi.h"

namespace gl {

namespace fs = std::filesystem;

namespace {

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
  if (recordArmed && countInBars > 0)
    engine.playWithCountIn(static_cast<double>(countInBars * doc.song().barTicks()));
  else
    engine.play();
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
      countInBars = countInBars > 0 ? 0 : 1;
      notify(countInBars ? "Count-in: 1 bar" : "Count-in off");
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
    case RemoteAction::TapTempo: {
      if (tapCount_ > 0 && now - tapTimes_[(tapCount_ - 1) % 4] > 2.0) tapCount_ = 0;  // start over
      tapTimes_[tapCount_ % 4] = now;
      ++tapCount_;
      int k = std::min(tapCount_, 4);
      if (k >= 2) {
        double first = tapTimes_[(tapCount_ - k) % 4], last = tapTimes_[(tapCount_ - 1) % 4];
        double bpm = 60.0 * (k - 1) / std::max(1e-3, last - first);
        if (bpm >= 10 && bpm <= 522) doc.setParam(songParam(SongParam::Tempo), std::round(bpm * 10) / 10, false);
      }
      break;
    }
    default: break;
  }
}

void App::playedNoteOn(ObjectId channel, int pitch, int velocity) {
  j31::chordEditorNote(*this, pitch, true);  // J-31: learn chords from MIDI input
  if (stepRecord && !engine.playing()) {
    stepRecorder.press(pitch, velocity);
    return;
  }
  if (recorder.active()) recorder.noteOn(doc, channel, pitch, velocity, engine.position());
}

void App::playedNoteOff(ObjectId channel, int pitch) {
  j31::chordEditorNote(*this, pitch, false);
  if (stepRecord && !engine.playing()) {
    Tick g = piano.effGrid > 0 ? piano.effGrid : snapTicks(doc.song(), piano.snap);
    stepRecorder.step = std::max<Tick>(1, g);
    stepRecorder.release(doc, curPattern, selChannel, pitch);
    return;
  }
  if (recorder.active()) recorder.noteOff(doc, channel, pitch, engine.position());
}

void App::processMidi() {
  engine.setMidiTarget(selChannel);
  const double pos = engine.position();

  // ---- recording session follows the transport ----
  const bool recNow = recordArmed && engine.playing() && !engine.countingIn() && engine.mode() != PlayMode::Live;
  if (recNow && !recorder.active()) {
    RecordOptions o = recOptions;
    Tick g = piano.effGrid > 0 ? piano.effGrid : snapTicks(doc.song(), piano.snap);
    o.grid = std::max<Tick>(1, g);
    if (engine.mode() == PlayMode::Song) {
      recorder.beginSong(doc, curPattern, o);
    } else if (const Pattern* p = doc.song().pattern(curPattern)) {
      recorder.beginPattern(doc, curPattern, o, std::max<Tick>(1, doc.song().patternContentLength(*p)));
    }
  } else if (!recNow && recorder.active()) {
    recorder.end(doc, lastRecPos_);
  }
  if (recorder.active()) {
    recorder.position(doc, pos);
    if (!recorder.songMode() && recorder.pattern() != curPattern) selectPattern(recorder.pattern());  // new take
  }
  lastRecPos_ = pos;

  // ---- multilink: remember the controls touched in Gridline ----
  if (multilink && doc.touchSerial() != multilinkSerial_) {
    ParamId t = doc.lastTouchedParam();
    if (std::find(multilinkQueue.begin(), multilinkQueue.end(), t) == multilinkQueue.end() && multilinkQueue.size() < 64)
      multilinkQueue.push_back(t);
  }
  if (!multilink) multilinkQueue.clear();

  if (!midi) {
    multilinkSerial_ = doc.touchSerial();
    return;
  }
  for (const auto& c : doc.song().channels)  // MIDI Out channels → device
    if (c.plugin.type == "gl.midiout")
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
        doc.setParam(pid, d->fromNormalized(out), true);
        lastCcTime_ = now;
        if (recorder.active() && !automationRecording()) recorder.controller(pid, out, engine.position());  // else captured by the automation recorder
      }
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
              doc.setParam(pid, d->fromNormalized(out), true);
              lastCcTime_ = now;
              if (recorder.active() && !automationRecording()) recorder.controller(pid, out, engine.position());  // else captured by the automation recorder
            }
          }
          handled = true;
        }
      }
    }
    if (handled) continue;

    // Notes: recording / step record.
    if (kind == 0x90 || kind == 0x80) {
      const MidiInputConfig* cfg = midi->inputConfig(e.port);
      if (cfg && !cfg->notes) continue;
      ObjectId ch = selChannel;
      const MidiRouting& r = midi->routing;
      if (r.byChannel && m.channel() != r.omniChannel && r.route[m.channel()] >= 0 &&
          r.route[m.channel()] < static_cast<int>(song.channels.size()))
        ch = song.channels[static_cast<size_t>(r.route[m.channel()])].id;
      if (m.noteOn())
        playedNoteOn(ch, j31::incomingPitch(*this, m.d1), m.d2);  // J-31: snap incoming MIDI to the scale
      else if (!r.ignoreNoteOff)
        playedNoteOff(ch, j31::incomingPitch(*this, m.d1));
    }
  }
  // A controller gesture ends after a short pause: one undo step per move.
  if (lastCcTime_ >= 0 && now - lastCcTime_ > 0.4) {
    doc.endGesture();
    lastCcTime_ = -1;
  }
  multilinkSerial_ = doc.touchSerial();
}

}  // namespace gl
