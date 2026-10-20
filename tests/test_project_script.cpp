// Gridline — J-57 tests: the full-model project scripting API (SC-02).
// Coverage per object type (song, channels, patterns, notes, steps, playlist
// tracks and clips of every kind, automation clips and points, mixer tracks,
// routing, sends, effect slots and their parameters, generic parameter
// references, markers, selection, transport), one undo step per run, rollback
// of failed runs, sandbox escapes and budgets, and the shipped example
// scripts. Run from the source tree (assets/ is read).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>
#include "Check.h"
#include "core/ControllerScript.h"
#include "core/Document.h"
#include "core/Params.h"
#include "core/ProjectScript.h"

using namespace gl;
using namespace gl::script;
namespace fs = std::filesystem;

namespace {

bool contains(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

// A small project: 3 channels (Kick, Snare, Lead), 2 patterns, clips, an
// automation clip, a reverb on insert 1.
Song makeSong() {
  Song s = makeEmptySong();
  Document d(s);
  ObjectId kick = d.addChannel("Kick", "gl.kick");
  ObjectId snare = d.addChannel("Snare", "gl.synth");
  ObjectId lead = d.addChannel("Lead", "gl.synth");
  ObjectId p1 = d.song().patterns[0].id;
  d.renamePattern(p1, "Verse");
  d.setSteps(p1, kick, {100, 0, 0, 0, 100, 0, 0, 0, 100, 0, 0, 0, 100, 0, 0, 0});
  Note n;
  n.start = 0, n.length = 48, n.pitch = 60, n.velocity = 90;
  d.addNote(p1, lead, n);
  ObjectId p2 = d.addPattern("Chorus", 2);
  d.addClip(p1, 0, 0);
  d.addClip(p2, 1, d.song().barTicks());
  d.routeChannel(lead, d.song().mixer[1].id);
  d.setSlotPlugin(d.song().mixer[1].id, 0, "gl.reverb");
  (void)snare;
  return d.song();
}

struct Fixture {
  Document doc{makeSong()};
  SimpleProjectHost host{doc};
  Vm vm;
  Fixture() { openProjectApi(vm, host); }
  // Runs code as one script run; returns the error ("" = ok).
  std::string run(const std::string& code) {
    ProjectRunResult r = runProject(vm, host, code, "test", "script: test");
    return r.ok ? std::string() : (r.error.empty() ? std::string("?") : r.error);
  }
  std::string eval(const std::string& expr) {
    vm.clearLog();
    std::string e = run("print(" + expr + ")");
    if (!e.empty()) return "ERR " + e;
    return vm.log().empty() ? "" : vm.log().back();
  }
};

void testSong() {
  Fixture f;
  expect(f.eval("project.song.tempo()") == "140.0", "song: tempo read");
  expect(f.run("project.song.setTempo(128)").empty() && std::fabs(f.doc.song().tempo - 128) < 1e-9, "song: setTempo");
  expect(f.eval("project.song.timeSignature()") == "4\t4", "song: time signature read");
  expect(f.run("project.song.setTimeSignature(3, 4)").empty() && f.doc.song().timeSigNum == 3, "song: setTimeSignature");
  expect(contains(f.run("project.song.setTimeSignature(3, 5)"), "2, 4, 8 or 16"), "song: bad denominator refused");
  expect(f.eval("project.song.ppq()") == std::to_string(f.doc.song().ppq), "song: ppq");
  expect(f.eval("project.song.bar()") == std::to_string(f.doc.song().barTicks()), "song: bar ticks");
  expect(f.run("project.song.setSwing(0.5)").empty() && std::fabs(f.doc.song().swing - 0.5f) < 1e-6, "song: setSwing");
  expect(f.run("project.song.setInfo{title = 'Demo', author = 'Me'}").empty() && f.doc.song().info.title == "Demo" &&
             f.doc.song().info.author == "Me",
         "song: setInfo");
  expect(f.eval("project.song.info().title") == "Demo", "song: info read");
  expect(f.eval("project.song.length()") == std::to_string(f.doc.song().lengthTicks()), "song: length");
  expect(contains(f.run("project.song.setTempo(0/0)"), "finite"), "song: NaN refused");
}

void testChannels() {
  Fixture f;
  const Song& s = f.doc.song();
  expect(f.eval("project.channels.count()") == "3", "channels: count");
  expect(f.eval("project.channels.name(2)") == "Snare", "channels: name");
  expect(f.eval("project.channels.find('Lead')") == "3", "channels: find");
  expect(f.eval("project.channels.find('nope')") == "nil", "channels: find missing");
  expect(f.eval("project.channels.index(project.channels.id(2))") == "2", "channels: id / index");
  expect(f.run("project.channels.setName(1, 'Big kick')").empty() && s.channels[0].name == "Big kick", "channels: setName");
  expect(f.run("project.channels.setColor(1, 0x123456)").empty() && s.channels[0].color == 0x123456, "channels: setColor");
  expect(contains(f.run("project.channels.setColor(1, -5)"), "colour"), "channels: bad colour refused");
  expect(f.run("project.channels.setVolume(2, 0.5)").empty() && std::fabs(s.channels[1].gain - 0.5f) < 1e-6, "channels: setVolume");
  expect(f.run("project.channels.setPan(2, -0.25)").empty() && std::fabs(s.channels[1].pan + 0.25f) < 1e-6, "channels: setPan");
  expect(f.run("project.channels.setMuted(2, true)").empty() && s.channels[1].muted, "channels: setMuted");
  expect(f.eval("project.channels.muted(2)") == "true", "channels: muted read");
  expect(f.run("project.channels.setPitch(2, 7)").empty() && s.channels[1].pitch == 7, "channels: setPitch");
  expect(f.eval("project.channels.plugin(1)") == "gl.kick", "channels: plugin type");
  expect(!f.eval("project.channels.pluginName(1)").empty(), "channels: plugin name");
  expect(f.eval("project.channels.route(3)") == "1", "channels: route read");
  expect(f.run("project.channels.setRoute(1, 2)").empty() && s.channels[0].mixerTrack == s.mixer[2].id, "channels: setRoute");
  // Plugin parameters.
  const int n = static_cast<int>(s.channels[2].plugin.params.size());
  expect(f.eval("project.channels.paramCount(3)") == std::to_string(n) && n > 0, "channels: paramCount");
  expect(!f.eval("project.channels.paramName(3, 1)").empty(), "channels: paramName");
  f.run("lo, hi = project.channels.paramRange(3, 1) project.channels.setParam(3, 1, hi)");
  auto d = describeParam(s, channelPluginParam(s.channels[2].id, 0));
  expect(d && std::fabs(s.channels[2].plugin.params[0] - d->max) < 1e-4, "channels: setParam to the maximum");
  expect(contains(f.run("project.channels.param(3, 99999)"), "no such parameter"), "channels: unknown parameter refused");
  expect(f.eval("project.channels.param(3, 'volume')") == f.eval("project.channels.volume(3)"), "channels: strip parameter by name");
  // Structure.
  expect(f.eval("project.channels.add('Hat', 'gl.synth')") == "4" && s.channels.size() == 4 && s.channels[3].name == "Hat", "channels: add");
  expect(contains(f.run("project.channels.add('X', 'gl.reverb')"), "instrument"), "channels: effect type refused as instrument");
  expect(f.eval("project.channels.clone(4)") == "5" && s.channels.size() == 5, "channels: clone");
  expect(f.run("project.channels.move(5, 1)").empty() && s.channels[0].name == "Hat #2", "channels: move");
  expect(f.run("project.channels.remove(1)").empty() && s.channels.size() == 4, "channels: remove");
  expect(f.run("project.channels.setPlugin(1, 'gl.synth')").empty() && s.channels[0].plugin.type == "gl.synth", "channels: setPlugin");
  expect(contains(f.run("project.channels.name(0)"), "no such channel"), "channels: index 0 refused");
  expect(contains(f.run("project.channels.name(99)"), "no such channel"), "channels: index past the end refused");
  expect(contains(f.run("project.channels.name('x')"), "number expected"), "channels: wrong type refused");
}

void testPatterns() {
  Fixture f;
  const Song& s = f.doc.song();
  expect(f.eval("project.patterns.count()") == "2", "patterns: count");
  expect(f.eval("project.patterns.name(1)") == "Verse", "patterns: name");
  expect(f.eval("project.patterns.find('Chorus')") == "2", "patterns: find");
  expect(f.run("project.patterns.setName(2, 'Hook')").empty() && s.patterns[1].name == "Hook", "patterns: setName");
  expect(f.run("project.patterns.setColor(2, 0xABCDEF)").empty() && s.patterns[1].color == 0xABCDEF, "patterns: setColor");
  expect(f.eval("project.patterns.bars(2)") == "2\t16", "patterns: bars");
  expect(f.run("project.patterns.setBars(2, 4)").empty() && s.patterns[1].bars == 4, "patterns: setBars");
  expect(f.eval("#project.patterns.steps(1, 1)") == "16", "patterns: steps read");
  expect(f.eval("project.patterns.steps(1, 1)[5]") == "100", "patterns: step velocity");
  expect(f.run("project.patterns.setSteps(1, 2, {true, false, 64, 200})").empty() &&
             s.patterns[0].steps.at(s.channels[1].id) == std::vector<uint8_t>({100, 0, 64, 127}),
         "patterns: setSteps (booleans, clamping)");
  expect(f.eval("#project.patterns.notes(1, 3)") == "1", "patterns: notes read");
  expect(f.eval("project.patterns.notes(1, 3)[1].pitch") == "60", "patterns: note fields");
  expect(f.run("local n = project.patterns.notes(1, 3) n[1].pitch = 64 n[2] = {time = 96, length = 24, pitch = 67}"
               " project.patterns.setNotes(1, 3, n)")
                 .empty(),
         "patterns: setNotes runs");
  const auto& notes = s.patterns[0].notes.at(s.channels[2].id);
  expect(notes.size() == 2 && notes[0].pitch == 64 && notes[1].pitch == 67, "patterns: setNotes result");
  expect(f.run("project.patterns.addNote(1, 3, {time = 0, pitch = 72})").empty() && s.patterns[0].notes.at(s.channels[2].id).size() == 3,
         "patterns: addNote");
  expect(contains(f.run("project.patterns.setNotes(1, 3, {{pitch = 'x'}})"), "must be a number"), "patterns: bad note refused");
  expect(f.eval("table.concat(project.patterns.channels(1), ',')") == "1,2,3", "patterns: channels with content");
  expect(f.eval("project.patterns.add('Bridge', 2)") == "3" && s.patterns.size() == 3, "patterns: add");
  expect(f.eval("project.patterns.clone(1)") == "2" && s.patterns.size() == 4 && s.patterns[1].steps == s.patterns[0].steps,
         "patterns: clone (placed after the original)");
  expect(f.run("project.patterns.remove(4)").empty() && s.patterns.size() == 3, "patterns: remove");
  expect(f.eval("project.patterns.length(1)") == std::to_string(s.patternContentLength(s.patterns[0])), "patterns: length");
}

void testPlaylist() {
  Fixture f;
  const Song& s = f.doc.song();
  expect(f.eval("#project.playlist.clips()") == "2", "playlist: clips");
  expect(f.eval("project.playlist.clips()[2].kind .. project.playlist.clips()[2].pattern .. project.playlist.clips()[2].name") ==
             "pattern2Chorus",
         "playlist: clip table");
  expect(f.eval("#project.playlist.clips(2)") == "1", "playlist: clips of one track");
  expect(f.eval("project.playlist.trackCount()") == std::to_string(s.tracks.size()), "playlist: track count");
  expect(f.run("project.playlist.setTrackName(1, 'Drums')").empty() && s.tracks[0].name == "Drums", "playlist: setTrackName");
  expect(f.eval("project.playlist.trackName(2)") == "Track 2", "playlist: default track name");
  expect(f.run("project.playlist.setTrackColor(1, 0x112233)").empty() && s.tracks[0].color == 0x112233, "playlist: setTrackColor");
  expect(f.run("project.playlist.setTrackMuted(3, true)").empty() && s.tracks[2].muted, "playlist: setTrackMuted");
  // Pattern clip.
  f.vm.clearLog();
  expect(f.run("id = project.playlist.addPatternClip(1, 5, 384) print(id)").empty() && s.playlist.size() == 3, "playlist: addPatternClip");
  const ObjectId id = s.playlist.back().id;
  expect(s.playlist.back().track == 4 && s.playlist.back().start == 384, "playlist: new clip position");
  expect(f.run("project.playlist.moveClip(id, 6, 768)").empty() && s.clip(id)->track == 5 && s.clip(id)->start == 768, "playlist: moveClip");
  expect(f.run("project.playlist.setClipLength(id, 96)").empty() && s.clip(id)->length == 96, "playlist: setClipLength");
  expect(f.run("project.playlist.setClip(id, {muted = true, offset = 24})").empty() && s.clip(id)->muted && s.clip(id)->offset == 24,
         "playlist: setClip");
  // Audio clip (the Kick is not a sample channel; add a sampler).
  expect(contains(f.run("project.playlist.addAudioClip(1, 1, 0)"), "no sample"), "playlist: audio clip needs a sample channel");
  expect(f.run("local c = project.channels.add('Loop', 'gl.sampler') aid = project.playlist.addAudioClip(c, 7, 0, 960)").empty() &&
             s.playlist.back().kind == ClipKind::Audio && s.playlist.back().length == 960,
         "playlist: addAudioClip");
  expect(f.run("project.playlist.setClip(aid, {gain = 0.5, reverse = true, fadeIn = 0.1})").empty() &&
             std::fabs(s.playlist.back().gain - 0.5f) < 1e-6 && s.playlist.back().reverse && s.playlist.back().fadeInManual,
         "playlist: audio clip settings");
  expect(f.eval("project.playlist.clip(aid).kind") == "audio", "playlist: clip(id)");
  expect(f.run("project.playlist.removeClip(aid)").empty() && s.playlist.size() == 3, "playlist: removeClip");
  expect(f.eval("project.playlist.clip(aid)") == "nil", "playlist: removed clip is gone");
  expect(contains(f.run("project.playlist.moveClip(123456, 1, 0)"), "no such clip"), "playlist: unknown clip refused");
  expect(contains(f.run("project.playlist.addPatternClip(1, 9999, 0)"), "track 1..500"), "playlist: bad track refused");
  expect(f.run("project.playlist.setTrackCount(40)").empty() && s.tracks.size() == 40, "playlist: setTrackCount");
}

void testAutomation() {
  Fixture f;
  const Song& s = f.doc.song();
  expect(f.eval("project.automation.count()") == "0", "automation: none yet");
  expect(f.run("ref = project.params.channel(3, 'volume')"
               " a = project.automation.add(ref, nil, {{time = 0, value = 0}, {time = 384, value = 1}})")
             .empty() &&
             s.automations.size() == 1,
         "automation: add on a parameter");
  expect(s.automations[0].owner == s.channels[2].id && s.automations[0].points.size() == 2, "automation: target and points");
  expect(f.eval("project.automation.targetName(1)") == paramDisplayName(s, {s.channels[2].id, 0}), "automation: target name");
  expect(f.eval("project.automation.target(1) == ref") == "true", "automation: target reference");
  expect(f.run("project.automation.addPoint(1, {time = 192, value = 0.25, tension = 0.5})").empty() && s.automations[0].points.size() == 3 &&
             s.automations[0].points[1].pos == 192,
         "automation: addPoint keeps order");
  expect(f.eval("project.automation.points(1)[2].value") == "0.25", "automation: points read");
  expect(f.run("project.automation.setPoints(1, {{time = 10, value = 2}})").empty() && s.automations[0].points.size() == 1 &&
             s.automations[0].points[0].value == 1.0f,
         "automation: setPoints clamps values");
  expect(f.run("project.automation.setName(1, 'Fade')").empty() && s.automations[0].name == "Fade", "automation: setName");
  expect(f.run("cid = project.playlist.addAutomationClip(1, 8, 0, 384)").empty() && s.playlist.back().kind == ClipKind::Automation,
         "automation: placed as a playlist clip");
  expect(f.eval("project.playlist.clip(cid).automation") == "1", "automation: clip refers to it");
  expect(f.run("project.automation.remove(1)").empty() && s.automations.empty() && s.playlist.back().kind != ClipKind::Automation,
         "automation: remove also removes its clips");
  expect(contains(f.run("project.automation.add(12345)"), "not a parameter"), "automation: bad reference refused");
}

void testMixer() {
  Fixture f;
  const Song& s = f.doc.song();
  expect(f.eval("project.mixer.count()") == std::to_string(s.mixer.size()), "mixer: count");
  expect(f.eval("project.mixer.name(0)") == "Master", "mixer: master name");
  expect(f.run("project.mixer.setName(1, 'Lead bus')").empty() && s.mixer[1].name == "Lead bus", "mixer: setName");
  expect(f.run("project.mixer.setColor(1, 0x445566)").empty() && s.mixer[1].color == 0x445566, "mixer: setColor");
  expect(f.run("project.mixer.setVolume(1, 0.4)").empty() && std::fabs(s.mixer[1].volume - 0.4f) < 1e-6, "mixer: setVolume");
  expect(f.run("project.mixer.setPan(1, 0.3)").empty() && std::fabs(s.mixer[1].pan - 0.3f) < 1e-6, "mixer: setPan");
  expect(f.run("project.mixer.setMuted(2, true)").empty() && s.mixer[2].muted, "mixer: setMuted");
  expect(f.run("project.mixer.setSolo(2, true)").empty() && s.mixer[2].solo, "mixer: setSolo");
  expect(f.run("project.mixer.setParam(1, 'eqLowGain', 6)").empty() && std::fabs(s.mixer[1].eqGain[0] - 6) < 1e-4, "mixer: strip EQ parameter");
  expect(f.eval("table.concat(project.mixer.channels(1), ',')") == "3", "mixer: channels routed to a track");
  // Sends and routing.
  expect(f.eval("project.mixer.sends(1)[1].to") == "0", "mixer: default send to the master");
  expect(f.run("project.mixer.setSend(1, 3, 0.5)").empty(), "mixer: setSend runs");
  {
    bool found = false;
    for (const auto& sd : s.mixer[1].sends) found = found || (sd.to == s.mixer[3].id && std::fabs(sd.level - 0.5f) < 1e-6);
    expect(found, "mixer: send added");
  }
  expect(f.eval("project.mixer.setSend(3, 1, 1)") == "false", "mixer: cycle refused");
  expect(f.run("project.mixer.routeTo(1, 4)").empty() && s.mixer[1].sends.size() == 1 && s.mixer[1].sends[0].to == s.mixer[4].id,
         "mixer: routeTo replaces the sends");
  expect(f.run("project.mixer.setSidechain(1, 4, true)").empty() && s.mixer[1].sends[0].sidechain, "mixer: setSidechain");
  // Slots.
  expect(f.eval("project.mixer.slot(1, 1).type") == "gl.reverb", "mixer: slot read");
  expect(f.eval("project.mixer.slot(1, 2)") == "nil", "mixer: empty slot");
  expect(f.run("project.mixer.setSlot(1, 2, 'gl.reverb')").empty() && s.mixer[1].slots[1].plugin.type == "gl.reverb", "mixer: setSlot");
  expect(contains(f.run("project.mixer.setSlot(1, 3, 'gl.synth')"), "not an effect"), "mixer: instrument refused in a slot");
  expect(f.run("project.mixer.setSlotEnabled(1, 2, false)").empty() && !s.mixer[1].slots[1].enabled, "mixer: setSlotEnabled");
  expect(f.run("project.mixer.setSlotMix(1, 2, 0.25)").empty() && std::fabs(s.mixer[1].slots[1].mix - 0.25f) < 1e-6, "mixer: setSlotMix");
  const int pc = static_cast<int>(s.mixer[1].slots[0].plugin.params.size());
  expect(f.eval("project.mixer.slotParamCount(1, 1)") == std::to_string(pc) && pc > 0, "mixer: slotParamCount");
  expect(!f.eval("project.mixer.slotParamName(1, 1, 1)").empty(), "mixer: slotParamName");
  f.run("local lo, hi = project.mixer.slotParamRange(1, 1, 1) project.mixer.setSlotParam(1, 1, 1, lo)");
  auto d = describeParam(s, slotParam(s.mixer[1].id, 0, 0));
  expect(d && std::fabs(s.mixer[1].slots[0].plugin.params[0] - d->min) < 1e-4, "mixer: setSlotParam");
  expect(!f.eval("project.mixer.slotParamText(1, 1, 1)").empty(), "mixer: slotParamText");
  expect(contains(f.run("project.mixer.slotParam(1, 5, 1)"), "empty"), "mixer: empty slot parameter refused");
  expect(f.run("project.mixer.setSlot(1, 2)").empty() && s.mixer[1].slots[1].plugin.empty(), "mixer: clear slot");
  // Structure.
  const size_t before = s.mixer.size();
  expect(f.eval("project.mixer.add()") == std::to_string(before) && s.mixer.size() == before + 1, "mixer: add");
  expect(f.run("project.mixer.remove(" + std::to_string(before) + ")").empty() && s.mixer.size() == before, "mixer: remove");
  expect(contains(f.run("project.mixer.remove(0)"), "master"), "mixer: master cannot be removed");
}

void testParamsMarkersSelectionTransport() {
  Fixture f;
  const Song& s = f.doc.song();
  expect(f.run("t = project.params.song('tempo') project.params.set(t, 100)").empty() && std::fabs(s.tempo - 100) < 1e-9, "params: song tempo");
  expect(f.eval("project.params.get(t)") == "100.0", "params: get");
  expect(f.eval("project.params.name(t)") == paramDisplayName(s, songParam(SongParam::Tempo)), "params: name");
  expect(f.eval("select('#', project.params.range(t))") == "3", "params: range");
  expect(f.run("project.params.setNormalized(project.params.mixer(1, 'volume'), 0)").empty() && s.mixer[1].volume == 0.0f,
         "params: setNormalized");
  expect(f.eval("project.params.fromNormalized(t, project.params.toNormalized(t, 123))") == "123.0", "params: normalized round trip");
  expect(f.run("project.params.set(project.params.slotMix(1, 1), 0.5)").empty() && std::fabs(s.mixer[1].slots[0].mix - 0.5f) < 1e-6,
         "params: slot mix");
  expect(f.run("project.params.set(project.params.slot(1, 1, 1), 0)").empty(), "params: slot parameter");
  expect(f.run("project.params.set(project.params.send(1, 0), 0.7)").empty() && std::fabs(s.mixer[1].sends[0].level - 0.7f) < 1e-6,
         "params: send level");
  expect(contains(f.run("project.params.get(-1)"), "not a parameter"), "params: bad reference refused");
  // Markers.
  expect(f.run("project.markers.add{time = 768, name = 'B'} project.markers.add{time = 0, name = 'A'}").empty() && s.markers.size() == 2 &&
             s.markers[0].name == "A",
         "markers: add keeps order");
  expect(f.eval("project.markers.get(2).name") == "B", "markers: get");
  expect(f.run("project.markers.set(1, {name = 'Intro', tsNum = 3, tsDen = 4})").empty() && s.markers[0].name == "Intro" &&
             s.markers[0].timeSigNum == 3,
         "markers: set");
  expect(f.run("project.markers.remove(2)").empty() && s.markers.size() == 1, "markers: remove");
  // Selection (UI state, no undo step).
  const size_t hist = f.doc.history().position();
  expect(f.run("project.selection.setChannels({1, 3})").empty() && f.host.channels.size() == 2 && f.host.channels[1] == s.channels[2].id,
         "selection: setChannels");
  expect(f.eval("table.concat(project.selection.channels(), ',')") == "1,3", "selection: channels");
  f.host.clips = {s.playlist[0].id};
  expect(f.eval("project.selection.clips()[1]") == std::to_string(s.playlist[0].id), "selection: clips");
  expect(f.run("project.selection.setClips({})").empty() && f.host.clips.empty(), "selection: setClips");
  expect(f.run("project.selection.setPattern(2)").empty() && f.host.pattern == s.patterns[1].id, "selection: setPattern");
  expect(f.eval("project.selection.pattern()") == "2", "selection: pattern");
  expect(f.run("project.selection.setMixer(3)").empty() && f.eval("project.selection.mixer()") == "3", "selection: mixer");
  expect(f.eval("project.selection.time()") == "nil", "selection: no time selection");
  expect(f.run("project.selection.setTime(96, 384)").empty() && f.eval("project.selection.time()") == "96\t384", "selection: time");
  expect(f.doc.history().position() == hist, "selection: no undo steps");
  // Transport: read-only unless allowed.
  f.host.pos = 480;
  expect(f.eval("project.transport.position()") == "480", "transport: position");
  expect(f.eval("project.transport.isPlaying()") == "false", "transport: isPlaying");
  expect(contains(f.run("project.transport.play()"), "may not control the transport"), "transport: play refused by default");
  expect(contains(f.run("project.transport.setPosition(0)"), "may not control"), "transport: setPosition refused by default");
  expect(!f.host.isPlaying && f.host.pos == 480, "transport: unchanged");
  f.host.allowTransport = true;
  expect(f.run("project.transport.play() project.transport.setPosition(96)").empty() && f.host.isPlaying && f.host.pos == 96,
         "transport: allowed by the host");
}

void testUndo() {
  Fixture f;
  const Song before = f.doc.song();
  const size_t pos = f.doc.history().position();
  std::string e = f.run(
      "project.song.setTempo(90)\n"
      "project.channels.setName(1, 'K')\n"
      "local p = project.patterns.add('New')\n"
      "project.playlist.addPatternClip(p, 3, 0)\n"
      "project.mixer.setVolume(1, 0.1)\n"
      "project.markers.add{time = 0, name = 'm'}\n"
      "project.automation.add(project.params.song('tempo'))\n");
  expect(e.empty(), "undo: many-edit script runs");
  expect(f.doc.history().position() == pos + 1, "undo: one history step per run");
  expect(f.doc.history().undoLabel() == "script: test", "undo: labelled after the script");
  expect(f.doc.undo() && f.doc.song() == before, "undo: one undo restores everything");
  expect(f.doc.redo() && f.doc.song().tempo == 90 && f.doc.song().markers.size() == 1, "undo: redo reapplies everything");
  // A failing run is rolled back and leaves no step.
  const Song mid = f.doc.song();
  const size_t pos2 = f.doc.history().position();
  e = f.run("project.song.setTempo(60) project.channels.setName(2, 'Z') error('boom')");
  expect(contains(e, "boom"), "undo: error reported");
  expect(f.doc.song() == mid && f.doc.history().position() == pos2, "undo: failed run rolled back, no step");
  expect(f.doc.groupDepth() == 0, "undo: no group left open");
  // A run with no edits adds no step.
  e = f.run("local x = project.channels.count()");
  expect(e.empty() && f.doc.history().position() == pos2, "undo: read-only run adds no step");
}

void testSandbox() {
  Fixture f;
  expect(contains(f.run("project.channels = nil"), "read-only"), "sandbox: modules are read-only");
  expect(contains(f.run("project.channels.count = print"), "read-only"), "sandbox: module functions are read-only");
  expect(f.eval("getmetatable(project)") == "locked" && f.eval("getmetatable(project.mixer)") == "locked", "sandbox: metatables locked");
  expect(contains(f.run("setmetatable(project, {})"), "protected metatable"), "sandbox: cannot replace a module metatable");
  for (const char* g : {"io", "os", "require", "dofile", "loadfile", "load", "package", "debug"})
    expect(f.eval(std::string("type(") + g + ")") == "nil", (std::string("sandbox: no ") + g).c_str());
  expect(f.eval("project.channels.sample(1)") == "nil", "sandbox: no sample path for non-sample channels");
  // Budget: endless loops and edit floods stop.
  {
    Document d(makeSong());
    SimpleProjectHost h(d);
    Limits lim;
    lim.instructions = 200000;
    Vm vm(lim);
    openProjectApi(vm, h);
    ProjectRunResult r = runProject(vm, h, "while true do project.channels.count() end", "t", "t");
    expect(!r.ok && r.limitHit && contains(r.error, "instruction limit"), "budget: endless read loop stopped");
    const Song before = d.song();
    r = runProject(vm, h, "for i = 1, 1e9 do project.channels.setName(1, 'x' .. i) end", "t", "t");
    expect(!r.ok && r.limitHit, "budget: edit flood stopped");
    expect(d.song() == before, "budget: stopped run rolled back");
    r = runProject(vm, h, "while true do pcall(project.markers.add, {time = 0}) end", "t", "t");
    expect(!r.ok && r.limitHit, "budget: pcall cannot swallow the limit");
  }
  {
    Document d(makeSong());
    SimpleProjectHost h(d);
    Limits lim;
    lim.seconds = 0.2;
    lim.instructions = 1'000'000'000'000ull;
    Vm vm(lim);
    openProjectApi(vm, h);
    ProjectRunResult r = runProject(vm, h, "while true do project.patterns.notes(1, 3) end", "t", "t");
    expect(!r.ok && contains(r.error, "time limit"), "budget: time limit");
  }
  // Fuzz-ish argument sweep: every function with odd arguments must fail cleanly or work.
  {
    Fixture g;
    g.doc.setMaxUndoLevels(1000000);
    const char* args[] = {"", "nil", "0", "-1", "1", "2", "1e300", "0/0", "'x'", "{}", "{{}}", "1, 1, 1", "1, 'volume', 1", "1, 1, {1, 2}",
                          "math.maxinteger", "math.mininteger, 1", "true, false", "print"};
    int crashes = 0, calls = 0;
    static const std::vector<std::pair<std::string, std::vector<std::string>>> fns = {
        {"song", {"tempo", "setTempo", "ppq", "timeSignature", "setTimeSignature", "swing", "setSwing", "masterPitch", "setMasterPitch", "bar",
                  "beat", "length", "info", "setInfo"}},
        {"channels", {"count", "id", "index", "find", "name", "setName", "color", "setColor", "volume", "setVolume", "pan", "setPan", "muted",
                      "setMuted", "pitch", "setPitch", "plugin", "pluginName", "setPlugin", "sample", "route", "setRoute", "add", "remove",
                      "clone", "move", "paramCount", "paramName", "paramRange", "paramText", "param", "setParam"}},
        {"patterns", {"count", "id", "index", "find", "name", "setName", "color", "setColor", "add", "remove", "clone", "length", "bars",
                      "setBars", "steps", "setSteps", "notes", "setNotes", "addNote", "channels"}},
        {"playlist", {"trackCount", "setTrackCount", "trackName", "setTrackName", "trackColor", "setTrackColor", "trackMuted", "setTrackMuted",
                      "clips", "clip", "addPatternClip", "addAudioClip", "addAutomationClip", "removeClip", "moveClip", "setClipLength",
                      "setClip"}},
        {"automation", {"count", "id", "name", "setName", "target", "targetName", "points", "setPoints", "addPoint", "range", "add", "remove"}},
        {"mixer", {"count", "id", "name", "setName", "color", "setColor", "volume", "setVolume", "pan", "setPan", "muted", "setMuted", "solo",
                   "setSolo", "param", "setParam", "add", "remove", "sends", "setSend", "routeTo", "setSidechain", "channels", "slotCount",
                   "slot", "setSlot", "setSlotEnabled", "setSlotMix", "slotParamCount", "slotParamName", "slotParamRange", "slotParamText",
                   "slotParam", "setSlotParam"}},
        {"params", {"song", "channel", "mixer", "send", "slot", "slotMix", "get", "set", "name", "range", "text", "normalized",
                    "setNormalized", "toNormalized", "fromNormalized"}},
        {"markers", {"count", "get", "add", "set", "remove"}},
        {"selection", {"channels", "setChannels", "clips", "setClips", "pattern", "setPattern", "mixer", "setMixer", "time", "setTime"}},
        {"transport", {"isPlaying", "isRecording", "songMode", "position", "allowed", "play", "stop", "setPosition"}},
    };
    for (const auto& [m, names] : fns)
      for (const auto& n : names) {
        expect(g.eval("type(project." + m + "." + n + ")") == "function", ("api: project." + m + "." + n + " exists").c_str());
        for (const char* a : args) {
          ++calls;
          std::string e = g.run("project." + m + "." + n + "(" + a + ")");
          if (contains(e, "non-string")) ++crashes;  // every API error is a message
        }
      }
    expect(crashes == 0 && calls > 2000, "fuzz: every API function survives odd arguments");
    expect(g.doc.groupDepth() == 0, "fuzz: no group left open");
    // The document stays consistent: undo everything back to the start.
    while (g.doc.undo()) {
    }
    expect(g.doc.song() == makeSong(), "fuzz: undo returns to the original song");
  }
}

void testExamples() {
  const fs::path dir = fs::path("assets") / "scripts" / "project";
  std::vector<ScriptInfo> list = scanScripts({{dir.string(), false}});
  expect(list.size() >= 4, "examples: shipped project scripts found");
  for (const auto& info : list) {
    ProjectScript s;
    std::string err;
    expect(s.loadFile(info.path, &err), ("examples: " + info.id + " loads " + err).c_str());
    Document d(makeSong());
    SimpleProjectHost h(d);
    h.selStart = 0, h.selEnd = d.song().barTicks() * 4;
    h.clips = {d.song().playlist[0].id, d.song().playlist[1].id};
    const size_t pos = d.history().position();
    ProjectRunResult r = s.run(h);
    expect(r.ok, ("examples: " + info.id + " runs " + r.error).c_str());
    expect(d.history().position() <= pos + 1, ("examples: " + info.id + " is at most one undo step").c_str());
    if (r.changed) {
      d.undo();
      expect(d.song() == makeSong(), ("examples: " + info.id + " undoes cleanly").c_str());
    }
  }
  // Specific results.
  {
    ProjectScript s;
    s.loadFile((dir / "drum_pattern.lua").string());
    Document d(makeSong());
    SimpleProjectHost h(d);
    ProjectRunResult r = s.run(h);
    const Song& g = d.song();
    const Pattern& p = g.patterns.back();
    expect(r.ok && p.name == "Beat" && p.steps.count(g.channels[0].id) && p.steps.at(g.channels[0].id)[0] == 100 &&
               p.steps.at(g.channels[1].id)[4] == 100,
           "drum pattern: kick and snare steps on the named channels");
    expect(h.pattern == p.id && g.playlist.back().pattern == p.id, "drum pattern: selected and placed");
    Document e(makeEmptySong());
    SimpleProjectHost h2(e);
    r = s.run(h2);
    expect(r.ok && e.song().channels.back().plugin.type == "gl.drumkit" &&
               !e.song().patterns.back().notes.at(e.song().channels.back().id).empty(),
           "drum pattern: drum kit notes when there are no drum channels");
  }
  {
    ProjectScript s;
    s.loadFile((dir / "route_drums_to_bus.lua").string());
    Document d(makeSong());
    SimpleProjectHost h(d);
    ProjectRunResult r = s.run(h);
    const Song& g = d.song();
    int bus = -1;
    for (size_t i = 0; i < g.mixer.size(); ++i)
      if (g.mixer[i].name == "Drum bus") bus = static_cast<int>(i);
    bool ok = r.ok && bus > 0;
    for (int ch : {0, 1}) {  // Kick, Snare
      int m = g.mixerIndex(g.channels[static_cast<size_t>(ch)].mixerTrack);
      ok = ok && m > 0 && m != bus && g.mixer[static_cast<size_t>(m)].sends.size() == 1 &&
           g.mixer[static_cast<size_t>(m)].sends[0].to == g.mixer[static_cast<size_t>(bus)].id;
    }
    expect(ok, "drum bus: each drum on its own insert, sent to the bus");
    expect(bus > 0 && g.mixer[static_cast<size_t>(bus)].sends.size() == 1 && g.mixer[static_cast<size_t>(bus)].sends[0].to == g.masterId(),
           "drum bus: the bus goes to the master");
    expect(g.channels[2].mixerTrack == g.mixer[1].id, "drum bus: other channels untouched");
  }
  {
    ProjectScript s;
    s.loadFile((dir / "rename_tracks_by_content.lua").string());
    Document d(makeSong());
    SimpleProjectHost h(d);
    ProjectRunResult r = s.run(h);
    expect(r.ok && d.song().tracks[0].name == "Verse" && d.song().tracks[1].name == "Chorus" && d.song().tracks[2].name.empty(),
           "rename tracks: named after their clips");
  }
  {
    ProjectScript s;
    s.loadFile((dir / "humanize_playlist.lua").string());
    Document d(makeSong());
    SimpleProjectHost h(d);
    h.clips = {d.song().playlist[1].id};
    std::vector<FormParam> v = s.form().params;
    for (auto& p : v)
      if (p.name == "Keep bar starts") p.value = 0;
      else if (p.name == "Timing") p.value = 1;
    ProjectRunResult r = s.run(h, v, {}, "", 7);
    const Tick moved = d.song().playlist[1].start - makeSong().playlist[1].start;
    expect(r.ok && moved != 0 && std::abs(moved) <= d.song().ppq / 4 && d.song().playlist[0].start == 0,
           "humanize: only the selected clip moves, within a step");
  }
  {
    ProjectScript s;
    s.loadFile((dir / "tempo_ramp.lua").string());
    Document d(makeSong());
    SimpleProjectHost h(d);
    ProjectRunResult r = s.run(h);
    const Song& g = d.song();
    expect(r.ok && g.automations.size() == 1 && g.automations[0].owner == kSongId && g.automations[0].points.size() == 2 &&
               g.playlist.back().kind == ClipKind::Automation,
           "tempo ramp: tempo automation clip placed");
  }
  {
    ProjectScript s;
    s.loadFile((dir / "section_markers.lua").string());
    Document d(makeSong());
    SimpleProjectHost h(d);
    ProjectRunResult r = s.run(h);
    expect(r.ok && !d.song().markers.empty() && d.song().markers[0].name == "Section 1", "section markers: added");
  }
}

void testConsoleWithBothApis() {
  Document d(makeSong());
  SimpleHost ch(d);
  SimpleProjectHost ph(d);
  Vm vm;
  openControllerApi(vm, ch);
  openProjectApi(vm, ph);
  ProjectRunResult r = runProject(vm, ph, "mixer.setVolume(1, 0.5) project.channels.setName(1, 'C')", "console", "console");
  expect(r.ok && d.song().channels[0].name == "C", "console: controller and project APIs side by side");
}

}  // namespace

int main() {
  testSong();
  testChannels();
  testPatterns();
  testPlaylist();
  testAutomation();
  testMixer();
  testParamsMarkersSelectionTransport();
  testUndo();
  testSandbox();
  testExamples();
  testConsoleWithBothApis();
  return finish();
}
