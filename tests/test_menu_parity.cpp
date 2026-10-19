// Gridline — J-27 menu parity sweep: every new menu operation as a pure
// function on Document (and its undo), plus the pure note / time helpers.
// Synthetic songs only.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>
#include <string>
#include <vector>
#include "Check.h"
#include "core/Automation.h"
#include "core/Document.h"
#include "core/MenuParity.h"
#include "core/NoteTools.h"
#include "core/PatternAutomation.h"
#include "core/PluginParams.h"
#include "engine/NoteFxBurn.h"

using namespace gl;

namespace {

Document makeDoc() { return Document(makeEmptySong()); }

ObjectId addAudioClip(Document& d, ObjectId channel, int track, Tick start, Tick len) {
  ObjectId id = kNoId;
  d.editSong("add audio clip", [&](Song& s) {
    Clip c;
    c.id = id = s.allocateId();
    c.kind = ClipKind::Audio;
    c.source = channel;
    c.track = track;
    c.start = start;
    c.length = len;
    s.playlist.push_back(c);
  });
  return id;
}

ObjectId addAutoClip(Document& d, ParamId pid, int track, Tick start, Tick len, std::vector<AutoPoint> pts, float mn = 0,
                     float mx = 1) {
  ObjectId clip = kNoId;
  d.editSong("add automation", [&](Song& s) {
    AutomationClip a;
    a.id = s.allocateId();
    a.owner = pid.owner;
    a.index = pid.index;
    a.min = mn;
    a.max = mx;
    a.points = std::move(pts);
    s.automations.push_back(a);
    Clip c;
    c.id = clip = s.allocateId();
    c.kind = ClipKind::Automation;
    c.source = a.id;
    c.track = track;
    c.start = start;
    c.length = len;
    s.playlist.push_back(c);
  });
  return clip;
}

void testPlaylistTracks() {
  Document d = makeDoc();
  const Song before = d.song();
  expect(ops::setAllTracksLocked(d, true) == static_cast<int>(d.song().tracks.size()), "lock all: every track changes");
  bool all = true;
  for (const auto& t : d.song().tracks) all = all && t.locked;
  expect(all, "lock all: all locked");
  expect(ops::setAllTracksLocked(d, true) == 0, "lock all again: nothing to do");
  expect(ops::setAllTracksLocked(d, false) > 0 && !d.song().tracks[3].locked, "unlock all");
  d.undo();
  expect(d.song().tracks[3].locked, "unlock all: undo");
  d.undo();
  expect(d.song() == before, "lock all: undo restores");

  // Groups: track 2 holds 3 tracks below it.
  d.editPlaylistTrack(2, "group", [](PlaylistTrack& t) { t.groupSize = 3, t.color = 0xC08040; });
  d.editPlaylistTrack(10, "group", [](PlaylistTrack& t) { t.groupSize = 1, t.folded = true; });
  expect(ops::trackGroupOf(d.song(), 4) == std::make_pair(2, 3), "group of a member");
  expect(ops::trackGroupOf(d.song(), 2) == std::make_pair(2, 3), "group of the head");
  expect(ops::trackGroupOf(d.song(), 6).first == -1, "track outside groups");
  expect(ops::setAllGroupsFolded(d, true) == 1, "collapse all groups: one unfolded group");
  expect(d.song().tracks[2].folded && d.song().tracks[10].folded, "collapse all groups");
  expect(ops::setAllGroupsFolded(d, false) == 2 && !d.song().tracks[10].folded, "expand all groups");
  expect(ops::autoColorTrackGroup(d, 3), "auto colour group");
  expect(d.song().tracks[2].color == 0xC08040 && d.song().tracks[5].color != 0xC08040 &&
             d.song().tracks[3].color != d.song().tracks[4].color,
         "auto colour group: gradient from the head");
  expect(!ops::autoColorTrackGroup(d, 7), "auto colour: not grouped");
  d.undo();
  expect(d.song().tracks[5].color == PlaylistTrack{}.color, "auto colour: undo");

  // Pattern clips to one track.
  ObjectId pat = d.song().patterns[0].id;
  d.addClip(pat, 0, 0);
  d.addClip(pat, 4, 384);
  ObjectId other = d.addPattern("Other");
  d.addClip(other, 5, 0);
  expect(ops::sendPatternClipsToTrack(d, pat, 7) == 2, "send pattern instances here");
  int on7 = 0;
  for (const auto& c : d.song().playlist) on7 += c.track == 7;
  expect(on7 == 2, "send pattern instances: both on the track, the other pattern stays");
  d.undo();
  on7 = 0;
  for (const auto& c : d.song().playlist) on7 += c.track == 7;
  expect(on7 == 0, "send pattern instances: undo");
  expect(ops::firstEmptyTrack(d.song()) == 1, "first empty track");
}

void testAudioClips() {
  Document d = makeDoc();
  ObjectId ch = d.addChannel("Loop", "gl.sampler");
  ObjectId ch2 = d.addChannel("Hit", "gl.sampler");
  ObjectId a = addAudioClip(d, ch, 0, 0, 384), b = addAudioClip(d, ch2, 1, 0, 384);
  ObjectId pc = d.addClip(d.song().patterns[0].id, 2, 0);
  expect(ops::pitchAudioClips(d, {a, b, pc}, 100.0f) == 2, "pitch audio clips: pattern clips skipped");
  expect(d.song().clip(a)->pitch == 100.0f && d.song().clip(b)->pitch == 100.0f, "pitch up a semitone");
  ops::pitchAudioClips(d, {a}, -200.0f);
  expect(d.song().clip(a)->pitch == -100.0f, "pitch down");
  d.undo();
  expect(d.song().clip(a)->pitch == 100.0f, "pitch: undo");
  expect(ops::reverseAudioClips(d, {a}) == 1 && d.song().clip(a)->reverse, "reverse audio clip");
  ops::reverseAudioClips(d, {a});
  expect(!d.song().clip(a)->reverse, "reverse twice");

  // Make unique as sample.
  ObjectId c2 = addAudioClip(d, ch, 0, 768, 384);
  const size_t nch = d.song().channels.size();
  ObjectId nc = ops::makeAudioClipUnique(d, c2);
  expect(nc != kNoId && d.song().channels.size() == nch + 1, "make unique as sample: a new channel");
  expect(d.song().clip(c2)->source == nc && d.song().clip(a)->source == ch, "make unique: only that clip moves");
  expect(d.song().channel(nc)->plugin == d.song().channel(ch)->plugin, "make unique: same sample and settings");
  expect(d.song().channelIndex(nc) == d.song().channelIndex(ch) + 1, "make unique: right after the original");
  expect(ops::makeAudioClipUnique(d, pc) == kNoId, "make unique: pattern clip refused");
  d.undo();
  expect(d.song().channels.size() == nch && d.song().clip(c2)->source == ch, "make unique: undo");

  // Time mode for every / selected audio clip channels.
  expect(ops::setAudioClipTimeMode(d, {}, 1) == 2, "switch all audio clip channels to stretch");
  expect(d.song().channel(ch)->plugin.params[pp::SmpStretchMode] == 1.0f, "stretch mode set");
  expect(ops::setAudioClipTimeMode(d, {b}, 0) == 1, "switch selected to resample");
  expect(d.song().channel(ch2)->plugin.params[pp::SmpStretchMode] == 0.0f &&
             d.song().channel(ch)->plugin.params[pp::SmpStretchMode] == 1.0f,
         "switch selected: only its channel");
  d.undo();
  d.undo();
  expect(d.song().channel(ch)->plugin.params[pp::SmpStretchMode] == 0.0f, "time mode: undo");

  // Declicking for selected channels (samplers only).
  ObjectId syn = d.addChannel("Synth", "gl.synth");
  expect(ops::setSamplerDeclick(d, {ch, ch2, syn}, 3) == 2, "declick: samplers only");
  expect(d.song().channel(ch)->plugin.params[pp::SmpDeclick] == 3.0f, "declick: generic");
  expect(ops::setSamplerDeclick(d, {ch}, 3) == 0, "declick: unchanged");
  d.undo();
  expect(d.song().channel(ch2)->plugin.params[pp::SmpDeclick] == 0.0f, "declick: undo");
}

void testAutomation() {
  Document d = makeDoc();
  ObjectId ch = d.addChannel("Synth", "gl.synth");
  ParamId vol = channelParam(ch, ChannelParam::Volume);
  ParamId pan = channelParam(ch, ChannelParam::Pan);
  ObjectId a = addAutoClip(d, vol, 3, 0, 384, {{0, 0.0f, 0, AutoShape::Curve}, {384, 1.0f, 0, AutoShape::Curve}});
  ObjectId b = addAutoClip(d, vol, 3, 768, 384, {{0, 0.0f, 0, AutoShape::Curve}, {384, 1.0f, 0, AutoShape::Curve}}, 0.5f, 1.0f);
  ObjectId c = addAutoClip(d, pan, 4, 0, 384, {{0, 0.5f, 0, AutoShape::Curve}});
  const Song before = d.song();
  expect(ops::mergeAutomationClips(d, {a}) == kNoId, "merge automation: needs two clips");
  ObjectId m = ops::mergeAutomationClips(d, {a, b, c});
  expect(m != kNoId, "merge automation clips");
  const Clip* mc = d.song().clip(m);
  expect(mc && mc->start == 0 && mc->length == 1152 && mc->track == 3, "merged clip spans both");
  expect(!d.song().clip(a) && !d.song().clip(b) && d.song().clip(c), "merged clips replaced; other target kept");
  const AutomationClip* ac = mc ? d.song().automation(mc->source) : nullptr;
  expect(ac && ac->owner == vol.owner && ac->index == vol.index, "merged clip drives the same parameter");
  if (ac) {
    expect(std::fabs(automationValue(*ac, 192) - 0.5) < 0.02, "merged: first clip values kept");
    expect(std::fabs(automationValue(*ac, 768) - 0.5) < 0.02, "merged: second clip's range applied (0.5..1)");
    expect(std::fabs(automationValue(*ac, 1151) - 1.0) < 0.02, "merged: end value");
  }
  expect(d.song().automations.size() == 2, "merged: unused contents removed");
  d.undo();
  expect(d.song() == before, "merge automation: undo");

  // Unused automation contents.
  d.editSong("orphan", [&](Song& s) {
    AutomationClip x;
    x.id = s.allocateId();
    s.automations.push_back(x);
  });
  expect(ops::unusedAutomationClips(d.song()).size() == 1, "unused automation clips");
  expect(ops::deleteUnusedAutomationClips(d) == 1 && d.song().automations.size() == 3, "delete unused automation clips");
  d.undo();
  expect(d.song().automations.size() == 4, "delete unused automation: undo");

  // Pattern events → automation clip.
  ObjectId pat = d.song().patterns[0].id;
  d.editPattern(pat, "lane", [&](Pattern& p) {
    setPatternLane(p, pan.owner, pan.index, {{0, 0.2f, 0, AutoShape::Hold}, {192, 0.8f, 0, AutoShape::Hold}});
  });
  d.addClip(pat, 0, 0);
  d.addClip(pat, 0, 1536);
  const size_t clipsBefore = d.song().playlist.size();
  ObjectId id = ops::laneToAutomationClip(d, pat, pan.owner, pan.index);
  expect(id != kNoId && d.song().automation(id), "turn events into automation clip");
  expect(!findPatternLane(*d.song().pattern(pat), pan.owner, pan.index), "the events are removed from the pattern");
  int placed = 0;
  for (const auto& k : d.song().playlist) placed += k.kind == ClipKind::Automation && k.source == id;
  expect(placed == 2 && d.song().playlist.size() == clipsBefore + 2, "one automation clip per pattern clip");
  if (const AutomationClip* x = d.song().automation(id)) expect(x->loop == d.song().patternContentLength(*d.song().pattern(pat)), "events repeat with the pattern");
  expect(ops::laneToAutomationClip(d, pat, pan.owner, pan.index) == kNoId, "no lane: nothing to convert");
  d.undo();
  expect(findPatternLane(*d.song().pattern(pat), pan.owner, pan.index) && !d.song().automation(id), "turn into clip: undo");
}

void testNotes() {
  using ops::NoteSel;
  // Claw: keep the second quarter of every beat (96 ticks).
  std::vector<Note> n;
  for (int i = 0; i < 8; ++i) n.push_back(Note(i * 24, 24, 60, 100));
  NoteSel sel(n.size(), false);
  ops::keepTimeSlot(n, sel, 2, 4, 96);
  expect(n.size() == 2 && n[0].start == 24 && n[1].start == 120, "keep part 2 of 4 per beat");
  expect(sel.size() == n.size(), "keep part: selection aligned");
  n.clear();
  for (int i = 0; i < 8; ++i) n.push_back(Note(i * 24, 24, 60, 100));
  sel.assign(n.size(), false);
  ops::keepTimeSlot(n, sel, 1, 2, 96, true);
  expect(n.size() == 4 && n[0].start == 48, "remove the first half of every beat");
  // Only the selection is affected.
  n.clear();
  for (int i = 0; i < 4; ++i) n.push_back(Note(i * 24, 24, 60, 100));
  sel = {true, false, false, false};
  ops::keepTimeSlot(n, sel, 2, 4, 96);
  expect(n.size() == 3, "keep part: unselected notes stay");

  // Chord inversions.
  std::vector<Note> c = {Note(0, 96, 60, 100), Note(0, 96, 64, 100), Note(0, 96, 67, 100), Note(96, 96, 50, 100)};
  sel.assign(c.size(), false);
  ops::invertChords(c, sel, 1);
  std::multiset<int> ps;
  for (const auto& x : c)
    if (x.start == 0) ps.insert(x.pitch);
  expect(ps == std::multiset<int>({64, 67, 72}), "invert up: lowest note up an octave");
  bool single = false;
  for (const auto& x : c) single = single || (x.start == 96 && x.pitch == 50);
  expect(single, "invert: single notes untouched");
  ops::invertChords(c, sel, -1);
  ps.clear();
  for (const auto& x : c)
    if (x.start == 0) ps.insert(x.pitch);
  expect(ps == std::multiset<int>({60, 64, 67}), "invert down restores the root position");

  // Stacked notes.
  ops::selectStacked(c, sel);
  int count = 0;
  for (bool b : sel) count += b;
  expect(count == 3, "select stacked notes: the chord only");

  // Scale detection: A natural minor notes.
  std::vector<Note> m;
  for (int p : {57, 59, 60, 62, 64, 65, 67, 69, 57, 64}) m.push_back(Note(0, 96, p, 100));
  NoteSel none(m.size(), false);
  auto g = ops::detectScale(m, none);
  expect(g.scale > 0 && g.fit > 0.999, "detect scale: all notes fit");
  bool ok = true;
  for (const auto& x : m) ok = ok && tools::inScale(x.pitch, g.root, g.scale);
  expect(ok, "detect scale: every note is in the guessed scale");
  std::vector<Note> pent = {Note(0, 96, 60, 100), Note(0, 96, 62, 100), Note(0, 96, 64, 100), Note(0, 96, 67, 100), Note(0, 96, 69, 100)};
  NoteSel ps2(pent.size(), false);
  auto g2 = ops::detectScale(pent, ps2);
  expect(g2.fit > 0.999 && tools::scales()[static_cast<size_t>(g2.scale)].steps.size() <= 7, "detect scale: smallest fitting scale");
  expect(ops::detectScale({}, {}).scale == 0, "detect scale: no notes");

  // Scale chords (stamp from the scale): find C major in the scale list.
  int major = -1;
  for (size_t k = 1; k < tools::scales().size(); ++k)
    if (tools::scales()[k].steps == std::vector<int>({0, 2, 4, 5, 7, 9, 11})) major = static_cast<int>(k);
  expect(major > 0, "a major scale exists");
  expect(ops::scaleChord(60, 0, major, 3, false) == std::vector<int>({60, 64, 67}), "scale chord on C: C E G");
  expect(ops::scaleChord(62, 0, major, 3, false) == std::vector<int>({62, 65, 69}), "scale chord on D: D F A (minor)");
  expect(ops::scaleChord(67, 0, major, 3, true) == std::vector<int>({60, 64, 67}), "top down: G on top gives C E G");
  expect(ops::scaleChord(61, 0, major, 3, false).front() != 61, "off-scale pitch moves onto the scale");
  expect(ops::scaleChord(60, 0, 0, 3, false) == std::vector<int>({60}), "no scale: single note");

  // Chord names.
  auto name = [](std::vector<int> pitches) {
    std::vector<Note> v;
    for (int p : pitches) v.push_back(Note(0, 96, p, 100));
    NoteSel s(v.size(), false);
    return ops::chordName(v, s);
  };
  expect(name({60, 64, 67}) == "C", "chord name: C");
  expect(name({57, 60, 64}) == "Am", "chord name: Am");
  expect(name({62, 65, 69, 72}) == "Dm7", "chord name: Dm7");
  expect(name({64, 67, 72}) == "C/E", "chord name: first inversion");
  expect(name({55, 60, 62}) == "Gsus4" || name({55, 60, 62}) == "Csus2/G", "chord name: suspended");
  expect(name({60}).empty() && name({60, 61, 62}).empty(), "chord name: none");
}

void testTimeAndMarkers() {
  Document d = makeDoc();
  ObjectId pat = d.song().patterns[0].id;
  ObjectId a = d.addClip(pat, 0, 384, 384), b = d.addClip(pat, 2, 1536, 192);
  auto r = ops::clipsTimeRange(d.song(), {a, b});
  expect(r.first == 384 && r.second == 1728, "time around the selection");
  expect(ops::clipsTimeRange(d.song(), {}) == std::make_pair<Tick, Tick>(0, 0), "time around: nothing");
  expect(ops::shiftTimeSelection(384, 768, 1) == std::make_pair<Tick, Tick>(768, 1152), "next time");
  expect(ops::shiftTimeSelection(384, 768, -1) == std::make_pair<Tick, Tick>(0, 384), "previous time");
  expect(ops::shiftTimeSelection(100, 500, -1) == std::make_pair<Tick, Tick>(0, 400), "previous time clamps at 0");

  expect(ops::addMarkerPair(d, 384, 1152) == 2 && d.song().markers.size() == 2, "add two markers");
  expect(d.song().markers[0].pos == 384 && d.song().markers[1].pos == 1152, "two markers at the selection edges");
  d.undo();
  expect(d.song().markers.empty(), "add two markers: one undo step");
  int lp = ops::placeLoopMarkers(d, 768, 1536);
  expect(lp >= 0 && d.song().markers[static_cast<size_t>(lp)].action == MarkerAction::Loop, "place loop marker");
  expect(d.song().markers.size() == 2 && ops::markerLoopRange(d.song(), lp) == std::make_pair<Tick, Tick>(768, 1536),
         "loop marker repeats the selection");
  d.undo();
  expect(d.song().markers.empty(), "place loop: undo");
  const Tick bar = d.song().barTicks();
  int j = ops::addJumpToNextBar(d, bar + 96);
  expect(j >= 0 && d.song().markers[static_cast<size_t>(j)].action == MarkerAction::Skip, "jump marker");
  bool atBar = false;
  for (const auto& m : d.song().markers) atBar = atBar || m.pos == 2 * bar;
  expect(atBar, "jump lands on the next bar");

  // Time display.
  Song s = makeEmptySong();
  s.tempo = 120;
  const Tick beat = s.beatTicks();
  expect(ops::formatSongTime(s, 0, ops::TimeFormat::BarBeatTick) == "001:01:000", "bar:beat:tick at 0");
  expect(ops::formatSongTime(s, s.barTicks() + beat * 2 + 5, ops::TimeFormat::BarBeatTick) == "002:03:005", "bar:beat:tick");
  expect(ops::formatSongTime(s, beat + beat / 4 * 2 + 3, ops::TimeFormat::BarBeatTick, true) == "001:07:003", "bar:step:tick");
  expect(ops::formatSongTime(s, beat * 2 * 61 + beat / 4, ops::TimeFormat::MinSecCs) == "1:01:12", "min:sec:centiseconds");
  expect(ops::formatSongTime(s, beat * 2 * 3600, ops::TimeFormat::Smpte, false, 2) == "01:00:00:00", "SMPTE 25 fps one hour");
  expect(ops::formatSongTime(s, beat, ops::TimeFormat::Smpte, false, 2) == "00:00:00:12", "SMPTE 25 fps half a second");
  // 29.97 drop frame: the first minute holds 1800 frames; frame 1800 is labelled 00:01:00;02.
  const double fpsDf = 30000.0 / 1001.0;
  const double t1800 = (1800.0 + 0.5) / fpsDf;  // seconds
  expect(ops::formatSongTime(s, t1800 * 2 * beat, ops::TimeFormat::Smpte, false, 3) == "00:01:00;02", "SMPTE 29.97 drop frame skips ;00 ;01");
  const double t17982 = (17982.0 + 0.5) / fpsDf;  // ten minutes: no drop at minute 10
  expect(ops::formatSongTime(s, t17982 * 2 * beat, ops::TimeFormat::Smpte, false, 3) == "00:10:00;00", "SMPTE drop frame: every tenth minute keeps ;00");
  expect(ops::smpteRates().size() == 13, "13 SMPTE rates");
}

void testScoreLog() {
  ops::ScoreLog log;
  log.noteOn(10.0, 60, 100);
  log.noteOff(10.5, 60);
  log.noteOn(11.0, 64, 80);
  log.noteOff(11.25, 64);
  log.noteOn(11.5, 67, 90);  // still held
  auto n = ops::scoreLogNotes(log, 12.0, 60.0, 120.0, 96);
  expect(n.size() == 3, "score log: three notes");
  expect(n.size() == 3 && n[0].start == 0 && n[0].length == 96 && n[0].pitch == 60, "score log: first note at 0, half a second = a beat at 120");
  expect(n.size() == 3 && n[1].start == 192 && n[1].length == 48 && n[1].velocity == 80, "score log: timing and velocity");
  expect(n.size() == 3 && n[2].start == 288 && n[2].length == 96, "score log: held note ends now");
  auto recent = ops::scoreLogNotes(log, 12.0, 1.2, 120.0, 96);
  expect(recent.size() == 2 && recent[0].pitch == 64, "score log: only the last N seconds");
  log.enabled = false;
  log.noteOn(12.0, 70, 100);
  expect(log.events.size() == 5, "score log: disabled logs nothing");
  log.enabled = true;
  log.keepSeconds = 1.0;
  log.noteOn(20.0, 72, 100);
  expect(log.events.size() == 1, "score log: old events dropped");

  Document d = makeDoc();
  ObjectId ch = d.addChannel("Keys", "gl.synth");
  ObjectId pat = d.song().patterns[0].id;
  d.setNotes(pat, ch, {Note(0, 96, 48, 100)});
  ops::ScoreLog l2;
  for (int i = 0; i < 12; ++i) {
    l2.noteOn(i * 0.5, 60 + i, 100);
    l2.noteOff(i * 0.5 + 0.25, 60 + i);
  }
  d.setParam(songParam(SongParam::Tempo), 120.0, false);
  expect(ops::dumpScoreLog(d, pat, ch, l2, 6.0, 120.0, false) == 12, "dump score log: twelve notes");
  expect(d.song().pattern(pat)->notes.at(ch).size() == 13, "dump: merged with the existing note");
  expect(d.song().pattern(pat)->bars >= 3, "dump: the pattern grows to hold the notes");
  d.undo();
  expect(d.song().pattern(pat)->notes.at(ch).size() == 1 && d.song().pattern(pat)->bars == 1, "dump: one undo step");
  expect(ops::dumpScoreLog(d, pat, ch, l2, 6.0, 120.0, true) == 12 && d.song().pattern(pat)->notes.at(ch).size() == 12, "dump: replace");
  ops::ScoreLog empty;
  expect(ops::dumpScoreLog(d, pat, ch, empty, 6.0, 120.0, false) == 0, "dump: empty log");

  // Note clipboard → MIDI file (round trip through the encoder).
  std::vector<Note> clip = {Note(192, 96, 60, 100), Note(288, 48, 67, 90)};
  clip.back().muted = false;
  MidiFileData mf = ops::notesToMidiFile(clip, 96, 128.0, "Clipboard");
  expect(mf.tracks.size() == 1 && mf.tracks[0].notes.size() == 2 && mf.tracks[0].notes[0].start == 0 &&
             mf.tracks[0].notes[1].start == 96,
         "notes to MIDI: first note at 0");
  MidiFileData back;
  expect(parseMidi(encodeMidi(mf), back) && back.tracks.size() == 1 && back.tracks[0].notes.size() == 2 &&
             back.tracks[0].notes[1].pitch == 67 && std::fabs(back.tempo - 128.0) < 0.01,
         "notes to MIDI: encodes and reads back");
}

void testChannels() {
  Document d = makeDoc();
  ObjectId ch = d.addChannel("Lead", "gl.synth");
  ObjectId pat = d.song().patterns[0].id;
  d.setNotes(pat, ch, {Note(0, 96, 60, 100)});
  d.editSong("links", [&](Song& s) {
    ControllerLink l;
    l.cc = 74;
    l.owner = ch;
    l.index = channelPluginParam(ch, 3).index;
    s.links.push_back(l);
    ControllerLink act;
    act.action = RemoteAction::PlayPause;
    s.links.push_back(act);
  });
  ObjectId c = ops::cloneChannelWithLinks(d, ch);
  expect(c != kNoId && d.song().channels.size() == 2, "clone with links: a clone");
  int forClone = 0, forOrig = 0;
  for (const auto& l : d.song().links) forClone += l.owner == c, forOrig += l.owner == ch;
  expect(forClone == 1 && forOrig == 1 && d.song().links.size() == 3, "clone with links: the controller link is duplicated");
  expect(d.song().pattern(pat)->notes.count(c) == 1, "clone with links: pattern data cloned");
  d.undo();
  expect(d.song().channels.size() == 1 && d.song().links.size() == 2, "clone with links: one undo step");

  // Auto fill: the first bar repeats.
  d.setPatternLength(pat, 2, 16);
  std::vector<uint8_t> row(32, 0);
  row[0] = 100, row[6] = 90;
  d.setSteps(pat, ch, row);
  expect(ops::autoFillSteps(d, pat, ch), "auto fill");
  const auto& r = d.song().pattern(pat)->steps.at(ch);
  expect(r.size() == 32 && r[16] == 100 && r[22] == 90 && r[17] == 0, "auto fill repeats the first bar");
  expect(!ops::autoFillSteps(d, pat, ch), "auto fill: nothing left to fill");
  d.undo();
  expect(d.song().pattern(pat)->steps.at(ch)[16] == 0, "auto fill: undo");

  // Layer helpers.
  ObjectId k1 = d.addChannel("Pad A", "gl.synth"), k2 = d.addChannel("Pad B", "gl.synth");
  ObjectId layer = ops::createLayer(d, {k1, k2});
  expect(layer != kNoId, "layer created");
  int g = ops::groupLayerChildren(d, layer, "Pads");
  expect(g >= 0 && d.song().channel(k1)->group == g && d.song().channel(k2)->group == g && d.song().channelGroups[static_cast<size_t>(g)] == "Pads",
         "group layer children");
  expect(d.song().channel(layer)->group != g, "group children: the layer itself stays out");
  expect(ops::groupLayerChildren(d, ch, "x") == -1, "group children: not a layer");
  d.undo();
  expect(d.song().channelGroups.empty(), "group children: undo");
  const size_t before = d.song().channels.size();
  expect(ops::deleteLayerChildren(d, layer) == 2 && d.song().channels.size() == before - 2, "delete layer children");
  expect(d.song().channel(layer) && d.song().channel(layer)->layer.empty(), "delete children: the layer stays, empty");
  d.undo();
  expect(d.song().channels.size() == before && d.song().channel(layer)->layer.size() == 2, "delete children: undo");
}

void testLive() {
  Document d = makeDoc();
  ObjectId p1 = d.song().patterns[0].id, p2 = d.addPattern("B");
  const Tick bar = d.song().barTicks();
  d.addClip(p1, 0, 0);
  d.addClip(p1, 0, bar);
  d.addClip(p2, 0, bar * 2);
  d.addClip(p2, 1, 0);
  ops::addMarker(d, bar * 2, "Verse");
  const Song before = d.song();
  Tick area = ops::prepareLiveArea(d);
  expect(area == bar * 2, "prepare live: area holds the longest track's sources (two patterns)");
  expect(d.song().live.areaEnd == area, "prepare live: live area set");
  int inArea0 = 0, inArea1 = 0;
  for (const auto& c : d.song().playlist)
    if (c.start < area) (c.track == 0 ? inArea0 : inArea1) += 1;
  expect(inArea0 == 2 && inArea1 == 1, "prepare live: each source once per track");
  bool shifted = false;
  for (const auto& c : d.song().playlist) shifted = shifted || (c.track == 0 && c.start == bar * 2 + area);
  expect(shifted && d.song().markers[0].pos == bar * 2 + area, "prepare live: the arrangement moves right");
  d.undo();
  expect(d.song() == before, "prepare live: undo");
  Document e = makeDoc();
  expect(ops::prepareLiveArea(e) == 0, "prepare live: empty playlist");
}

void testLastTweaked() {
  Document d = makeDoc();
  ObjectId ch = d.addChannel("Lead", "gl.synth");
  expect(d.previousTouchedParam().owner == kNoId, "nothing tweaked yet");
  d.setParam(channelParam(ch, ChannelParam::Volume), 0.5);
  d.setParam(channelParam(ch, ChannelParam::Volume), 0.6);
  expect(d.previousTouchedParam().owner == kNoId, "same parameter twice: no previous");
  d.setParam(channelParam(ch, ChannelParam::Pan), 0.2);
  expect(d.lastTouchedParam() == channelParam(ch, ChannelParam::Pan) &&
             d.previousTouchedParam() == channelParam(ch, ChannelParam::Volume),
         "next to last tweaked parameter");
}

void testBurn() {
  Document d = makeDoc();
  ObjectId ch = d.addChannel("Keys", "gl.synth");
  ObjectId pat = d.song().patterns[0].id;
  d.setNotes(pat, ch, {Note(0, 96, 60, 100), Note(192, 96, 62, 100)});
  // Without processors the notes come back unchanged.
  auto same = burnNoteFx(d.song(), ch, d.song().pattern(pat)->notes.at(ch), 384);
  expect(same == d.song().pattern(pat)->notes.at(ch), "burn: no processors = unchanged");
  expect(!burnNoteFxToPattern(d, pat, ch), "burn to pattern: nothing to burn");
  expect(d.addNoteFx(ch, "gl.chord"), "chord generator added");
  auto out = burnNoteFx(d.song(), ch, d.song().pattern(pat)->notes.at(ch), 384);
  expect(out.size() == 6, "burn: every note becomes a triad");
  std::multiset<int> first;
  for (const auto& n : out)
    if (n.start == 0) first.insert(n.pitch);
  expect(first == std::multiset<int>({60, 64, 67}), "burn: major triad on C");
  bool lengths = true;
  for (const auto& n : out) lengths = lengths && n.length == 96;
  expect(lengths, "burn: note lengths kept");
  // Steps are burned too.
  std::vector<uint8_t> row(16, 0);
  row[8] = 100;
  d.setSteps(pat, ch, row);
  expect(burnNoteFxToPattern(d, pat, ch), "burn to the pattern");
  expect(d.song().channel(ch)->noteFx.empty(), "burn: processors removed");
  expect(d.song().pattern(pat)->notes.at(ch).size() == 9, "burn: notes and steps become chords");
  expect(d.song().pattern(pat)->steps.count(ch) == 0 || d.song().pattern(pat)->steps.at(ch).empty() ||
             std::all_of(d.song().pattern(pat)->steps.at(ch).begin(), d.song().pattern(pat)->steps.at(ch).end(), [](uint8_t v) { return v == 0; }),
         "burn: steps cleared");
  d.undo();
  expect(d.song().channel(ch)->noteFx.size() == 1 && d.song().pattern(pat)->notes.at(ch).size() == 2, "burn: one undo step");
}

}  // namespace

int main() {
  testPlaylistTracks();
  testAudioClips();
  testAutomation();
  testNotes();
  testTimeAndMarkers();
  testScoreLog();
  testChannels();
  testLive();
  testLastTweaked();
  testBurn();
  return finish();
}
