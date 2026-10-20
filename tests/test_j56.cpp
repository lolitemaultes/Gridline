// Gridline — J-56: playlist track modes (PL-08), MIDI time code / clock
// receive with chase (MI-02), the master chain and loudness normalising
// (RN-09). Synthetic songs and MIDI streams only.
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>
#include "Check.h"
#include "Hermetic.h"
#include "core/Document.h"
#include "core/Mastering.h"
#include "core/MenuOps.h"
#include "core/MidiRecord.h"
#include "core/MidiSync.h"
#include "core/PluginParams.h"
#include "core/Project.h"
#include "core/TrackModes.h"
#include "core/flp/Flp.h"
#include "core/flp/FlpExport.h"
#include "engine/AudioRecord.h"
#include "engine/ExportJob.h"
#include "engine/Loudness.h"
#include "engine/Render.h"

namespace fs = std::filesystem;
using namespace gl;
namespace tmo = gl::trackmodes;

namespace {

std::string gDir;

ObjectId addChannel(Song& s, const std::string& name, const std::string& type, ObjectId mixer = kNoId) {
  Channel c;
  c.id = s.allocateId();
  c.name = name;
  c.plugin = defaultPluginState(type);
  c.mixerTrack = mixer == kNoId ? s.masterId() : mixer;
  s.channels.push_back(c);
  return c.id;
}

// ============================================================ PL-08 track modes

void testAudioTracks() {
  Song s = makeEmptySong();
  s.tracks[2].name = "Vocal";
  std::string why;
  expect(tmo::makeAudioTrack(s, 2, kNoId, &why), "audio track: made on a free insert");
  const MixerTrack* m = tmo::linkedMixer(s, 2);
  expect(m && s.mixerIndex(m->id) == 1, "audio track: the first free insert is linked");
  expect(m && m->name == "Vocal", "audio track: the insert takes the track's name");
  expect(tmo::modeOf(s, 2) == TrackMode::Audio && tmo::audioTrackOf(s, m->id) == 2, "audio track: mode and reverse lookup");
  // A second audio track takes the next free insert; linking the same insert again is refused.
  expect(tmo::makeAudioTrack(s, 3), "audio track: a second one");
  expect(s.mixerIndex(tmo::linkedMixer(s, 3)->id) == 2 && s.tracks[3].name == "Audio 2", "audio track: next insert, named after it");
  expect(!tmo::makeAudioTrack(s, 4, m->id, &why) && !why.empty(), "audio track: an insert belongs to one track");
  expect(!tmo::makeAudioTrack(s, 4, s.masterId(), &why), "audio track: never the master");
  const ObjectId ins5 = s.mixer[5].id;
  addChannel(s, "Synth", "gl.synth", ins5);
  expect(!tmo::mixerLinkable(s, ins5) && !tmo::makeAudioTrack(s, 4, ins5), "audio track: an insert channels play into is refused");
  // Recording controls go to the linked insert.
  expect(tmo::setArmed(s, 2, true), "audio track: arm");
  m = tmo::linkedMixer(s, 2);
  expect(m->armed && m->input == 0 && m->inputStereo && m->records(), "audio track: arming selects the first input pair");
  expect(tmo::setInput(s, 2, 3, false) && m->input == 3 && !m->inputStereo, "audio track: mono input 4");
  expect(tmo::setMonitor(s, 2, InputMonitor::Off) && m->monitor == static_cast<uint8_t>(InputMonitor::Off), "audio track: monitoring");
  expect(tmo::armedTracks(s) == std::vector<int>{2}, "audio track: armed tracks listed");
  expect(!tmo::setArmed(s, 5, true), "ordinary tracks have no arm");
  // Unassigning disarms (FL disarms the mixer track when the track mode changes).
  const ObjectId mid = m->id;
  expect(tmo::unassign(s, 2) && !s.mixerTrack(mid)->armed && tmo::modeOf(s, 2) == TrackMode::Unassigned, "unassign: disarms, ordinary again");
  expect(tmo::mixerLinkable(s, mid), "unassign: the insert is free to link again");
  // Mixer menu: assign to the next free playlist track / drop the link.
  const int t = tmo::assignMixerToNewTrack(s, mid);
  expect(t == 0 && tmo::audioTrackOf(s, mid) == 0, "mixer menu: audio track on the first free playlist track");
  expect(tmo::assignMixerToNewTrack(s, mid) == 0, "mixer menu: assigning twice keeps the one track");
  expect(tmo::unassignMixer(s, mid) && tmo::audioTrackOf(s, mid) < 0, "mixer menu: unassign");
  // Stale links (deleted insert) count as ordinary and are dropped.
  tmo::makeAudioTrack(s, 6);
  const ObjectId gone = s.tracks[6].mixer;
  s.mixer.erase(s.mixer.begin() + s.mixerIndex(gone));
  expect(tmo::modeOf(s, 6) == TrackMode::Unassigned && tmo::linkedMixer(s, 6) == nullptr, "stale link: counts as ordinary");
  expect(tmo::dropStaleLinks(s) == 1 && s.tracks[6].mixer == kNoId, "stale link: dropped");
}

void testInstrumentTracks() {
  Song s = makeEmptySong();
  const ObjectId a = addChannel(s, "Lead", "gl.synth");
  const ObjectId b = addChannel(s, "Bass", "gl.synth", s.mixer[3].id);
  const ObjectId c = addChannel(s, "Pad", "gl.synth", s.mixer[3].id);
  std::string why;
  expect(tmo::makeInstrumentTrack(s, 1, a, &why), "instrument track: made");
  const Channel* ca = s.channel(a);
  expect(ca->mixerTrack != s.masterId() && s.mixerTrack(ca->mixerTrack)->name == "Lead", "instrument track: a master-routed channel gets an insert of its own");
  expect(s.tracks[1].name == "Lead" && s.tracks[1].color == ca->color, "instrument track: named and coloured after the channel");
  expect(tmo::linkedMixer(s, 1) && tmo::linkedMixer(s, 1)->id == ca->mixerTrack, "instrument track: plays into the channel's insert");
  // Bass shares insert 3 with Pad: it moves to a free insert.
  expect(tmo::makeInstrumentTrack(s, 2, b) && s.channel(b)->mixerTrack != s.mixer[3].id, "instrument track: a shared insert is not taken");
  // Pad is now alone on insert 3: it keeps it.
  expect(tmo::makeInstrumentTrack(s, 3, c) && s.channel(c)->mixerTrack == s.mixer[3].id, "instrument track: an own insert is kept");
  expect(!tmo::makeInstrumentTrack(s, 4, a, &why) && !why.empty(), "instrument track: a channel has one track");
  Channel clipCh;
  clipCh.id = s.allocateId();
  clipCh.plugin = defaultPluginState("gl.sampler");
  clipCh.audioClip = true;
  s.channels.push_back(clipCh);
  expect(!tmo::channelLinkable(s, clipCh.id), "instrument track: audio clip channels refused");
  // Clips: pattern clips only.
  expect(tmo::clipAllowed(s, ClipKind::Pattern, 1) && !tmo::clipAllowed(s, ClipKind::Audio, 1) &&
             !tmo::clipAllowed(s, ClipKind::Automation, 1) && tmo::clipAllowed(s, ClipKind::Audio, 5),
         "instrument track: pattern clips only");
  // An audio clip dropped on it goes to the next empty track; on an audio track it is routed there.
  tmo::makeAudioTrack(s, 6);
  const int land = tmo::placeAudioClip(s, 1, clipCh.id);
  expect(land != 1 && tmo::clipAllowed(s, ClipKind::Audio, land), "drop: an audio clip on an instrument track moves on");
  expect(tmo::placeAudioClip(s, 6, clipCh.id) == 6 && s.channel(clipCh.id)->mixerTrack == s.tracks[6].mixer,
         "drop: an audio clip on an audio track plays through its insert");
  // A track with audio clips cannot become an instrument track.
  Clip ac;
  ac.id = s.allocateId();
  ac.kind = ClipKind::Audio;
  ac.source = clipCh.id;
  ac.track = 7;
  s.playlist.push_back(ac);
  const ObjectId d = addChannel(s, "Keys", "gl.synth");
  expect(!tmo::makeInstrumentTrack(s, 7, d, &why), "instrument track: refused while the track holds audio clips");
  expect(tmo::assignChannelToNewTrack(s, d) >= 0 && tmo::instrumentTrackOf(s, d) >= 0, "channel menu: instrument track on a free track");
  expect(tmo::unassignChannel(s, d) && tmo::instrumentTrackOf(s, d) < 0, "channel menu: unassign");
  // Effects cannot be instruments.
  const ObjectId fxCh = addChannel(s, "Odd", "gl.reverb");
  expect(!tmo::channelLinkable(s, fxCh), "instrument track: effects refused");
}

void testUndoAndClone() {
  Document doc(makeEmptySong());
  expect(tmo::makeAudioTrack(doc, 0), "undo: audio track made");
  expect(tmo::setArmed(doc, 0, true), "undo: armed");
  expect(doc.undo() && !tmo::linkedMixer(doc.song(), 0)->armed, "undo: arm undone");
  expect(doc.undo() && tmo::modeOf(doc.song(), 0) == TrackMode::Unassigned, "undo: track mode undone");
  expect(doc.redo() && tmo::modeOf(doc.song(), 0) == TrackMode::Audio, "redo: track mode back");
  std::string why;
  const size_t ch0 = doc.song().channels.size();
  const ObjectId nc = tmo::newInstrumentTrack(doc, 1, "gl.synth", &why);
  expect(nc != kNoId && doc.song().channels.size() == ch0 + 1 && tmo::linkedChannel(doc.song(), 1) &&
             tmo::linkedChannel(doc.song(), 1)->id == nc,
         "new instrument track: a channel and its track");
  expect(doc.undo() && doc.song().channels.size() == ch0 && tmo::modeOf(doc.song(), 1) == TrackMode::Unassigned,
         "new instrument track: one undo step");
  expect(tmo::newInstrumentTrack(doc, 1, "gl.reverb", &why) == kNoId, "new instrument track: effects refused");
  // Failed edits make no undo step.
  const ObjectId master = doc.song().masterId();
  const Song before = doc.song();
  expect(!tmo::makeAudioTrack(doc, 2, master) && doc.song().tracks == before.tracks, "failed edit: refused, nothing changed");
  // Clone: a cloned audio track gets a free insert of its own.
  Document d2(makeEmptySong());
  tmo::makeAudioTrack(d2, 0);
  ops::clonePlaylistTrack(d2, 0);
  const Song& s2 = d2.song();
  expect(tmo::modeOf(s2, 1) == TrackMode::Audio && s2.tracks[1].mixer != s2.tracks[0].mixer, "clone: audio track with its own insert");
}

void testRecordingOnAudioTrack() {
  Song s = makeEmptySong();
  tmo::makeAudioTrack(s, 5);  // insert 1 → track 6
  const ObjectId ins = s.tracks[5].mixer;
  tmo::setArmed(s, 5, true);
  expect(recordTrackFor(s, ins) == 5, "recording: the linked audio track takes the take");
  expect(recordTrackFor(s, s.mixer[2].id) == 1, "recording: other inserts keep insert N → track N");
  // Instrument track on the default lane: the take goes below it.
  const ObjectId ch = addChannel(s, "Lead", "gl.synth");
  tmo::makeInstrumentTrack(s, 2, ch);
  expect(recordTrackFor(s, s.mixer[3].id) == 3, "recording: no audio on an instrument track");
  // Commit synthetic takes: clips land on the audio track, played through its insert.
  std::vector<RecordedTake> takes(2);
  for (int k = 0; k < 2; ++k) {
    takes[static_cast<size_t>(k)].mixer = ins;
    takes[static_cast<size_t>(k)].pass = k;
    takes[static_cast<size_t>(k)].startTick = 0;
    takes[static_cast<size_t>(k)].L.assign(4800, 0.25f);
    takes[static_cast<size_t>(k)].R.assign(4800, 0.25f);
  }
  TakeCommitOptions o;
  o.dir = (fs::path(gDir) / "rec").string();
  o.sampleRate = 48000;
  TakeCommitResult r = commitTakes(s, takes, o);
  expect(r.clips.size() == 2 && s.clip(r.clips[0])->track == 5 && s.clip(r.clips[1])->track == 6, "recording: passes stack below the audio track");
  expect(s.channel(r.channels[0])->mixerTrack == ins, "recording: the take plays through the linked insert");
  expect(tmo::mixerLinkable(s, ins, nullptr, 5), "recording: the take's channel does not block the link");
}

void testSongModeNotes() {
  Song s0 = makeEmptySong();
  const ObjectId lead = addChannel(s0, "Lead", "gl.synth");
  const ObjectId pat = s0.patterns[0].id;
  Document doc(s0);
  tmo::makeInstrumentTrack(doc, 4, lead);
  NoteRecorder rec;
  RecordOptions o;
  rec.beginSong(doc, pat, o);
  rec.position(doc, 0);
  rec.noteOn(doc, lead, 60, 100, 10);
  rec.noteOff(doc, lead, 60, 40);
  rec.end(doc, 96);
  bool onTrack = false;
  for (const auto& c : doc.song().playlist) onTrack = onTrack || (c.kind == ClipKind::Pattern && c.pattern == pat && c.track == 4);
  expect(onTrack, "song-mode recording: the clip goes on the channel's instrument track");
}

void testProjectAndFlp() {
  Song s = makeEmptySong();
  const ObjectId lead = addChannel(s, "Lead", "gl.synth");
  tmo::makeAudioTrack(s, 1);
  tmo::makeInstrumentTrack(s, 3, lead);
  tmo::setArmed(s, 1, true);
  Json j = songToJson(s);
  Song back;
  std::string err;
  expect(songFromJson(j, back, &err), "project: loads");
  expect(back.tracks[1].mode == TrackMode::Audio && back.tracks[1].mixer == s.tracks[1].mixer && back.tracks[3].mode == TrackMode::Instrument &&
             back.tracks[3].channel == lead && back.tracks == s.tracks,
         "project: track modes and links saved");
  expect(tmo::linkedMixer(back, 1)->armed, "project: the arm is the insert's");
  // FL export / import: @49 mode, @53 insert, @57 channel.
  flp::Bytes bytes;
  flp::ExportReport rep;
  flp::ExportOptions eo;
  eo.useOrigin = false;
  expect(flp::exportFlp(s, bytes, rep, eo), "flp: exported");
  Song imp;
  flp::Report ir;
  expect(importFlpBytes(bytes, imp, ir, &err), "flp: re-imported");
  expect(tmo::modeOf(imp, 1) == TrackMode::Audio && imp.mixerIndex(imp.tracks[1].mixer) == s.mixerIndex(s.tracks[1].mixer),
         "flp: audio track and its insert survive");
  const Channel* ic = tmo::linkedChannel(imp, 3);
  expect(tmo::modeOf(imp, 3) == TrackMode::Instrument && ic && ic->name == "Lead", "flp: instrument track and its channel survive");
  expect(tmo::modeOf(imp, 0) == TrackMode::Unassigned && tmo::modeOf(imp, 2) == TrackMode::Unassigned, "flp: ordinary tracks stay ordinary");
  // FL's own projects with track modes (shipped content, when installed).
  const std::string demo = "/Applications/FL Studio 2026.app/Contents/Resources/FL/Data/Demo projects";
  std::error_code ec;
  int audio = 0, instr = 0, files = 0;
  if (fs::is_directory(demo, ec))
    for (auto it = fs::recursive_directory_iterator(demo, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
      if (ec || it->path().extension() != ".flp" || it->path().filename().string().find("Bien Duro") == std::string::npos) continue;
      Song fl;
      flp::Report r2;
      if (!importFlp(it->path().string(), fl, r2, &err)) continue;
      ++files;
      for (int t = 0; t < static_cast<int>(fl.tracks.size()); ++t) {
        audio += tmo::modeOf(fl, t) == TrackMode::Audio;
        instr += tmo::modeOf(fl, t) == TrackMode::Instrument;
      }
    }
  if (files) expect(audio > 0 && instr > 0, "flp: a shipped FL project's audio and instrument tracks import");
}

// ============================================================ MI-02 time code / clock receive

void testTimecodeLabels() {
  bool ok = true;
  for (SyncOut r : {SyncOut::Mtc24, SyncOut::Mtc25, SyncOut::Mtc2997Drop, SyncOut::Mtc30})
    for (int64_t f = 0; f < 200000 && ok; f += 7) ok = mtcFrameOf(r, mtcTimecode(r, f)) == f;
  expect(ok, "mtc: frame ↔ label round trip for every rate (drop frame included)");
  Timecode t;
  t.m = 1, t.s = 0, t.f = 2;
  expect(mtcFrameOf(SyncOut::Mtc2997Drop, t) == 1800, "mtc: 00:01:00;02 is frame 1800 in drop frame");
  for (int c = 0; c < 4; ++c) ok = ok && mtcRateCode(mtcRateFromCode(c)) == c;
  expect(ok, "mtc: rate codes");
}

// Runs an MTC master (Gridline's own generator) from `startSec` for `seconds`
// and feeds every message to `rx`. Calls `tick(now)` every 1/60 s.
void runMaster(SyncOut rate, double startSec, double seconds, MtcReceiver& rx, const std::function<void(double)>& tick,
               const std::function<bool(int64_t)>& drop = {}) {
  const double sr = 48000;
  MidiEventRing ring;
  MidiClockOut gen;
  gen.out = &ring;
  gen.setMode(rate);
  gen.setSampleRate(sr);
  gen.start(0, startSec, 0);
  const int block = 400;  // 1/120 s
  int64_t at = 0, qf = 0;
  double nextTick = 0;
  for (; at < static_cast<int64_t>(seconds * sr); at += block) {
    const double s0 = startSec + at / sr, s1 = startSec + (at + block) / sr;
    gen.segment(0, 0, s0, s1, at, block, [](double) { return 0.0; });
    MidiOutEvent e;
    while (ring.pop(e)) {
      if (e.size == 2 && drop && drop(qf++)) continue;
      rx.message(e.data, e.size, e.sample / sr);
    }
    const double now = (at + block) / sr;
    while (nextTick <= now) {
      if (tick) tick(nextTick);
      nextTick += 1.0 / 60.0;
    }
  }
}

void testMtcReceive() {
  for (SyncOut rate : {SyncOut::Mtc24, SyncOut::Mtc25, SyncOut::Mtc2997Drop, SyncOut::Mtc30}) {
    MtcReceiver rx;
    double worst = 0;
    bool lockedLate = true, lockedEarly = false;
    const double start = 3725.5;  // 01:02:05.5
    runMaster(rate, start, 3.0, rx, [&](double now) {
      if (now < 0.05) lockedEarly = lockedEarly || rx.locked(now);
      if (now > 0.5) {
        lockedLate = lockedLate && rx.locked(now);
        worst = std::max(worst, std::fabs(rx.seconds(now) - (start + now)));
      }
    });
    const std::string n = syncOutName(rate);
    expect(!lockedEarly, ("mtc " + n + ": not locked before a full timecode").c_str());
    expect(lockedLate && rx.rate() == rate, ("mtc " + n + ": locks and detects the rate").c_str());
    expect(worst < 0.5 / mtcFrameRate(rate), ("mtc " + n + ": position within half a frame of the master").c_str());
  }
  // Full frame: a locate, not a lock.
  MtcReceiver rx;
  const uint8_t ff[10] = {0xF0, 0x7F, 0x7F, 0x01, 0x01, static_cast<uint8_t>((1 << 5) | 1), 2, 3, 4, 0xF7};  // 25 fps 01:02:03:04
  expect(rx.message(ff, 10, 10.0) && !rx.locked(10.0), "mtc full frame: understood, not locked");
  double sec = 0;
  expect(rx.takeLocate(sec) && std::fabs(sec - (3723.0 + 4.0 / 25.0)) < 1e-9 && !rx.takeLocate(sec), "mtc full frame: locate once at its time");
  const uint8_t note[3] = {0x90, 60, 100};
  expect(!rx.message(note, 3, 10.0), "mtc: other messages are not time code");
  // Lost quarter frames: unlocked until a clean run; a stopped master unlocks.
  MtcReceiver rx2;
  bool unlockedAfterDrop = false, relocked = false;
  runMaster(SyncOut::Mtc30, 10.0, 2.0, rx2,
            [&](double now) {
              if (now > 1.0 && now < 1.05) unlockedAfterDrop = unlockedAfterDrop || !rx2.locked(now);
              if (now > 1.8) relocked = rx2.locked(now);
            },
            [](int64_t q) { return q == 120; });  // one message lost at 1.0 s
  expect(unlockedAfterDrop && relocked && rx2.dropouts() >= 1, "mtc: a lost quarter frame unlocks, a clean run relocks");
  expect(!rx2.locked(2.0 + 0.5), "mtc: no quarter frames for a while → unlocked (master stopped)");
}

// Local transport model: runs at (1 + speedError) of real time while playing.
struct LocalTransport {
  bool playing = false;
  double pos = 0, last = 0, speedError = 0;
  void advance(double now) {
    if (playing) pos += (now - last) * (1.0 + speedError);
    last = now;
  }
  void apply(const SyncChase::Action& a) {
    if (a.kind == SyncChase::Action::Start) playing = true, pos = a.position;
    if (a.kind == SyncChase::Action::Locate) pos = a.position;
    if (a.kind == SyncChase::Action::Stop) playing = false;
  }
};

void testChase() {
  // MTC master at 25 fps; the local clock runs 0.4 % fast: relocations keep it within tolerance.
  MtcReceiver rx;
  SyncChase chase;
  chase.tolerance = 0.040;
  chase.offset = 0.0;
  LocalTransport local;
  local.speedError = 0.004;
  double worst = 0;
  int starts = 0;
  runMaster(SyncOut::Mtc25, 100.0, 30.0, rx, [&](double now) {
    local.advance(now);
    const auto a = chase.update(now, rx.locked(now), rx.seconds(now), local.playing, local.pos);
    starts += a.kind == SyncChase::Action::Start;
    local.apply(a);
    if (now > 1.0) worst = std::max(worst, std::fabs(local.pos - (100.0 + now)));
  });
  expect(starts == 1 && local.playing, "chase: starts once with the master");
  expect(chase.relocations() >= 1 && chase.relocations() <= 5, "chase: a drifting clock is relocated a few times");
  expect(worst < chase.tolerance + 0.02, "chase: the drift never exceeds the tolerance by more than the confirm time");
  // Jitter below the tolerance never relocates.
  SyncChase calm;
  calm.tolerance = 0.040;
  bool any = false;
  double pos = 0;
  for (int i = 0; i < 600; ++i) {
    const double now = i / 60.0;
    const double jitter = (i % 2 ? 0.01 : -0.01);
    const auto a = calm.update(now, true, now, i > 0, pos + jitter);
    if (i == 0) pos = a.position;
    pos = now;
    any = any || (i > 0 && a.kind != SyncChase::Action::None);
  }
  expect(!any && std::fabs(calm.drift()) < 0.02, "chase: jitter within the tolerance is ignored");
  // A jump of the master (locate while rolling) relocates after the confirm count; offset applies.
  SyncChase jump;
  jump.offset = 0.5;
  jump.tolerance = 0.04;
  LocalTransport lt;
  for (int i = 0; i < 120; ++i) {
    const double now = i / 60.0;
    lt.advance(now);
    const double master = i < 60 ? now : now + 10.0;
    lt.apply(jump.update(now, true, master, lt.playing, lt.pos));
  }
  expect(std::fabs(lt.pos - (120 / 60.0 - 1 / 60.0 + 10.0 + 0.5)) < 0.05 && jump.relocations() == 1, "chase: follows a master jump, with the offset");
  // Master stops → local stops.
  auto stop = jump.update(3.0, false, 0, true, 5.0);
  expect(stop.kind == SyncChase::Action::Stop && !jump.chasing(), "chase: stops with the master");
}

void testClockChaseTicks() {
  // Same, with the local transport counted in ticks.
  SyncChase chase;
  const double ppq = 96, bpm = 120, tps = bpm / 60.0 * ppq;
  chase.tolerance = 0.040 * tps;
  bool playing = false;
  double pos = 0, worst = 0;
  MidiClockFollower f;
  f.message(0xF2, 16, 0, 0.0);
  f.message(0xFB, 0, 0, 0.0);
  const double clockDt = 60.0 / (bpm * 24.0);
  double nextClock = 0;
  for (int i = 0; i < 60 * 20; ++i) {
    const double now = i / 60.0;
    while (nextClock <= now) f.message(0xF8, 0, 0, nextClock), nextClock += clockDt;
    if (playing && i > 0) pos += tps / 60.0 * 0.99;  // 1 % slow
    const double master = static_cast<double>(f.clocks()) * ppq / 24.0;
    const auto a = chase.update(now, f.running() && f.hasTempo(), master, playing, pos);
    if (a.kind == SyncChase::Action::Start || a.kind == SyncChase::Action::Locate) pos = a.position, playing = true;
    if (now > 2.0 && playing) worst = std::max(worst, std::fabs(pos - master));
  }
  expect(playing && pos > 16 * 24, "clock chase: starts from the song position pointer");
  expect(chase.relocations() >= 2 && worst < chase.tolerance + tps * 0.06, "clock chase: a slow transport is pulled back in");
}

// ============================================================ RN-09 master chain + loudness

AudioBuffer sine(double hz, double amp, double seconds, int sr, bool stereo = true) {
  AudioBuffer b(stereo ? 2 : 1, static_cast<size_t>(seconds * sr));
  for (size_t c = 0; c < b.channels; ++c)
    for (size_t i = 0; i < b.frames; ++i) b.at(c, i) = static_cast<float>(amp * std::sin(2 * M_PI * hz * static_cast<double>(i) / sr));
  return b;
}

void testLoudness() {
  // BS.1770: a full-scale 997 Hz sine on one front channel measures -3.01
  // LUFS (on both channels: 0.0 LUFS; a mono buffer counts as both).
  for (int sr : {44100, 48000, 96000}) {
    AudioBuffer left = sine(997, 1.0, 5, sr);
    std::fill(left.plane(1), left.plane(1) + left.frames, 0.0f);
    const double l = integratedLoudness(left, sr);
    expect(std::fabs(l - (-3.01)) < 0.05, ("loudness: 0 dBFS 997 Hz sine on one channel = -3.01 LUFS at " + std::to_string(sr)).c_str());
  }
  expect(std::fabs(integratedLoudness(sine(997, 1.0, 5, 48000), 48000)) < 0.05 &&
             std::fabs(integratedLoudness(sine(997, 1.0, 5, 48000, false), 48000)) < 0.05,
         "loudness: the same sine on both channels / mono = 0.0 LUFS");
  const double l20 = integratedLoudness(sine(997, 0.1, 5, 48000), 48000);
  expect(std::fabs(l20 - (-20.0)) < 0.05, "loudness: -20 dBFS stereo sine = -20 LUFS");
  expect(integratedLoudness(AudioBuffer(2, 48000 * 3), 48000) == kSilentLufs, "loudness: silence");
  // Gating: half the file silent leaves the integrated loudness alone.
  AudioBuffer g = sine(997, 0.1, 10, 48000);
  for (size_t c = 0; c < 2; ++c)
    for (size_t i = g.frames / 2; i < g.frames; ++i) g.at(c, i) = 0;
  expect(std::fabs(integratedLoudness(g, 48000) - l20) < 0.25, "loudness: silence is gated out");
  // Relative gate: a quiet passage 20 dB down does not count.
  AudioBuffer q = sine(997, 0.1, 10, 48000);
  for (size_t c = 0; c < 2; ++c)
    for (size_t i = q.frames / 2; i < q.frames; ++i) q.at(c, i) *= 0.1f;
  expect(std::fabs(integratedLoudness(q, 48000) - l20) < 0.2, "loudness: passages 20 LU down are gated out");
  // K-weighting: lows count less than highs.
  expect(integratedLoudness(sine(50, 0.1, 5, 48000), 48000) < l20 - 0.5 && integratedLoudness(sine(5000, 0.1, 5, 48000), 48000) > l20 + 2,
         "loudness: K-weighting (50 Hz counts less, 5 kHz more)");
  // Normalising.
  AudioBuffer n = sine(997, 0.05, 5, 48000);
  LoudnessNormalization r = normalizeLoudness(n, 48000, -14.0, -1.0);
  expect(!r.ceilingLimited && std::fabs(integratedLoudness(n, 48000) - (-14.0)) < 0.05 && std::fabs(r.result + 14.0) < 1e-6,
         "normalise: reaches -14 LUFS");
  AudioBuffer peaky = sine(997, 0.05, 5, 48000);
  peaky.at(0, 1000) = 0.9f;  // one big peak
  r = normalizeLoudness(peaky, 48000, -6.0, -1.0);
  expect(r.ceilingLimited && samplePeakDb(peaky) <= -1.0 + 1e-4 && r.result < -6.0, "normalise: the peak ceiling wins");
  AudioBuffer quiet(2, 48000);
  r = normalizeLoudness(quiet, 48000, -14, -1);
  expect(r.gainDb == 0 && quiet.peak() == 0, "normalise: silence untouched");
}

Song chainSong() {
  Song s = makeEmptySong();
  s.tempo = 120;
  const ObjectId ch = addChannel(s, "Synth", "gl.synth");
  const ObjectId pat = s.patterns[0].id;
  for (int k = 0; k < 8; ++k) s.pattern(pat)->notes[ch].push_back(Note(k * 96, 90, 48 + (k % 4) * 7, 110));
  Clip c;
  c.id = s.allocateId();
  c.pattern = pat;
  c.track = 0;
  c.start = 0;
  s.playlist.push_back(c);
  return s;
}

void testMasterChain() {
  namespace ms = gl::mastering;
  Song s = chainSong();
  ms::Macros m;
  m.tilt = 2, m.glue = 0.5f, m.width = 1.4f, m.drive = 6, m.ceiling = -1.5f;
  expect(ms::chainSlot(s) < 0, "master chain: none at first");
  expect(ms::installMasterChain(s, m) && ms::chainSlot(s) == 5, "master chain: installed at the end of the master");
  ms::Macros r;
  expect(ms::readMasterChain(s, r) && std::fabs(r.tilt - 2) < 1e-5 && std::fabs(r.glue - 0.5f) < 1e-5 && std::fabs(r.width - 1.4f) < 1e-5 &&
             std::fabs(r.drive - 6) < 1e-5 && std::fabs(r.ceiling + 1.5f) < 1e-5,
         "master chain: macros read back from the effects");
  const auto& types = ms::chainTypes();
  bool typed = true;
  for (int k = 0; k < ms::kChainSlots; ++k) typed = typed && s.mixer[0].slots[static_cast<size_t>(5 + k)].plugin.type == types[static_cast<size_t>(k)];
  expect(typed && types.size() == 5, "master chain: EQ, compressor, stereo shaper, limiter, meter");
  // Out-of-range macros are clamped.
  ms::Macros wild;
  wild.tilt = 40, wild.glue = -3, wild.width = 9, wild.drive = 99, wild.ceiling = 5;
  ms::applyMacros(s, wild);
  ms::readMasterChain(s, r);
  expect(r.tilt == 6 && r.glue == 0 && r.width == 2 && r.drive == 12 && r.ceiling == 0, "master chain: macros clamped");
  // Saved with the project.
  ms::applyMacros(s, m);
  Song back;
  std::string err;
  expect(songFromJson(songToJson(s), back, &err) && ms::readMasterChain(back, r) && std::fabs(r.drive - 6) < 1e-5, "master chain: saved and loaded");
  // A full master refuses; removal frees the slots.
  Song full = chainSong();
  for (int k = 0; k < 8; ++k) full.mixer[0].slots[static_cast<size_t>(k)].plugin = defaultPluginState("gl.delay");
  std::string why;
  expect(!ms::installMasterChain(full, m, &why) && !why.empty(), "master chain: needs five free slots");
  expect(ms::removeMasterChain(s) && ms::chainSlot(s) < 0, "master chain: removed");
  // Presets: original names, sane values.
  bool sane = ms::presets().size() >= 3;
  for (const auto& p : ms::presets()) sane = sane && p.macros.ceiling <= 0 && p.macros.drive >= 0 && p.macros.drive <= 12;
  expect(sane, "master chain: presets");
  // Undo: install, two knob drags merged per gesture.
  Document doc(chainSong());
  expect(ms::installMasterChain(doc, m), "master chain: undoable install");
  ms::Macros k = m;
  k.drive = 7;
  ms::setMacros(doc, k, 42);
  k.drive = 8;
  ms::setMacros(doc, k, 42);
  ms::readMasterChain(doc.song(), r);
  expect(r.drive == 8, "master chain: knob edit");
  expect(doc.undo() && ms::readMasterChain(doc.song(), r) && std::fabs(r.drive - 6) < 1e-5, "master chain: one drag = one undo step");
  expect(doc.undo() && ms::chainSlot(doc.song()) < 0, "master chain: install undone");
  // Drive for a loudness target.
  expect(ms::driveForTarget(m, -20, -14) == 12 && ms::driveForTarget(m, -14, -16) == 4 && ms::driveForTarget(m, -144, -14) == 6,
         "master chain: drive for a target (clamped; no reading = unchanged)");
}

void testMasterChainSound() {
  namespace ms = gl::mastering;
  RenderOptions ro;
  ro.sampleRate = 48000;
  ro.tail = 96;
  Song dry = chainSong();
  const AudioBuffer a = renderSong(dry, ro);
  Song wet = chainSong();
  ms::Macros m = ms::presets()[3].macros;  // loud
  ms::installMasterChain(wet, m);
  const AudioBuffer b = renderSong(wet, ro);
  bool finite = true;
  for (float v : b.data) finite = finite && std::isfinite(v);
  expect(finite && b.peak() > 0.05f, "master chain render: finite, audible");
  expect(samplePeakDb(b) <= m.ceiling + 0.3, "master chain render: the limiter holds the ceiling");
  expect(integratedLoudness(b, 48000) > integratedLoudness(a, 48000) + 2.0, "master chain render: drive makes it louder");
  // Width 0: mono output.
  Song mono = chainSong();
  ms::Macros mm;
  mm.width = 0;
  ms::installMasterChain(mono, mm);
  const AudioBuffer c = renderSong(mono, ro);
  float side = 0;
  for (size_t i = 0; i < c.frames; ++i) side = std::max(side, std::fabs(c.at(0, i) - c.at(1, i)));
  expect(side < 1e-3f && c.peak() > 0.01f, "master chain render: width 0 is mono");
  // Neutral chain (no drive, no glue, no tilt, width 1) stays close to the dry mix level.
  Song neutral = chainSong();
  ms::Macros n;
  n.glue = 0, n.drive = 0, n.ceiling = 0;
  ms::installMasterChain(neutral, n);
  const AudioBuffer d = renderSong(neutral, ro);
  expect(std::fabs(integratedLoudness(d, 48000) - integratedLoudness(a, 48000)) < 1.0, "master chain render: neutral settings keep the level");
}

void testExportNormalize() {
  Song s = chainSong();
  ExportSettings es;
  es.folder = (fs::path(gDir) / "norm").string();
  es.baseName = "Loud";
  es.formats = {ExportFormat::Wav};
  es.wavBits = 32;
  es.render.sampleRate = 48000;
  es.normalize = true;
  es.normalizeLoudness = true;
  es.normalizeLufs = -16.0;
  es.normalizeDb = -1.0;
  ExportOutcome o = runExport(s, es);
  expect(o.errors.empty() && o.files.size() == 1, "export: loudness-normalised file written");
  expect(std::fabs(o.lufs - (-16.0)) < 0.1 || (o.ceilingLimited && o.lufs < -16.0), "export: the file measures the target (or the ceiling held it)");
  es.normalizeLufs = -6.0;  // too loud for a -1 dB ceiling without a limiter
  o = runExport(s, es);
  expect(o.ceilingLimited && o.lufs < -6.0, "export: a target past the ceiling stops at the ceiling");
  es.normalizeLoudness = false;
  es.normalizeDb = -3.0;
  o = runExport(s, es);
  expect(o.errors.empty() && o.lufs > -60, "export: peak normalising still works and reports the loudness");
}

}  // namespace

int main() {
  gDir = test::hermeticTestEnv("j56");
  testAudioTracks();
  testInstrumentTracks();
  testUndoAndClone();
  testRecordingOnAudioTrack();
  testSongModeNotes();
  testProjectAndFlp();
  testTimecodeLabels();
  testMtcReceive();
  testChase();
  testClockChaseTicks();
  testLoudness();
  testMasterChain();
  testMasterChainSound();
  testExportNormalize();
  return finish();
}
