// Gridline — J-82: deleting playlist clip sources (picker panel). Used and
// unused audio clip sources, automation clip sources and patterns: the
// source and every clip of it go (every arrangement), links to the channel
// go, the mixer track and the sample file stay, one undo step restores the
// song exactly, and the project / FLP round trip after a delete is clean.
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include "Check.h"
#include "Hermetic.h"
#include "core/Arrangements.h"
#include "core/ClipSources.h"
#include "core/Document.h"
#include "core/MenuParity.h"
#include "core/PatternAutomation.h"
#include "core/PluginParams.h"
#include "core/Project.h"
#include "core/flp/Flp.h"
#include "core/flp/FlpExport.h"

namespace fs = std::filesystem;
using namespace gl;

namespace {

std::string gDir;
void expect(bool ok, const std::string& name) { expect(ok, name.c_str()); }

// A small but valid 16-bit mono WAV (silence), so the sampler has a file.
std::string writeWav(const std::string& name) {
  const std::string p = gDir + "/" + name + ".wav";
  std::ofstream f(p, std::ios::binary);
  auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
  auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
  const uint32_t frames = 4410;
  f.write("RIFF", 4), u32(36 + frames * 2), f.write("WAVE", 4);
  f.write("fmt ", 4), u32(16), u16(1), u16(1), u32(44100), u32(44100 * 2), u16(2), u16(16);
  f.write("data", 4), u32(frames * 2);
  for (uint32_t i = 0; i < frames; ++i) u16(static_cast<uint16_t>((i % 64) * 100));
  return p;
}

ObjectId addSource(Song& s, const std::string& name, const std::string& path, ObjectId mixer) {
  Channel ch;
  ch.id = s.allocateId();
  ch.name = name;
  ch.plugin = defaultPluginState("gl.sampler");
  ch.plugin.data = path;
  ch.mixerTrack = mixer;
  ch.audioClip = true;
  s.channels.push_back(ch);
  return ch.id;
}

Clip audioClip(Song& s, ObjectId src, Tick start, int track) {
  Clip c;
  c.id = s.allocateId();
  c.kind = ClipKind::Audio;
  c.source = src;
  c.track = track;
  c.start = start;
  c.length = s.barTicks();
  return c;
}

struct Fixture {
  Song song;
  ObjectId drum = kNoId, vox = kNoId, unusedAudio = kNoId, synth = kNoId, autoUsed = kNoId, autoUnused = kNoId, pat2 = kNoId;
  ObjectId drumMixer = kNoId;
  std::string drumPath, voxPath;
};

Fixture makeFixture() {
  Fixture f;
  Song& s = f.song;
  s = makeEmptySong();
  f.drumPath = writeWav("drum_loop");
  f.voxPath = writeWav("vox");
  f.drumMixer = s.mixer.size() > 2 ? s.mixer[2].id : s.masterId();
  // An instrument channel playing a pattern.
  Channel syn;
  syn.id = s.allocateId();
  syn.name = "Synth";
  syn.plugin = defaultPluginState("gl.synth");
  syn.mixerTrack = s.masterId();
  s.channels.push_back(syn);
  f.synth = syn.id;
  s.patterns[0].notes[syn.id] = {Note{}};
  f.drum = addSource(s, "Drum loop", f.drumPath, f.drumMixer);
  f.vox = addSource(s, "Vox", f.voxPath, s.masterId());
  f.unusedAudio = addSource(s, "Spare hit", f.voxPath, s.masterId());
  // Pattern clips, 5 drum clips + 1 vox clip in arrangement 1.
  Clip pc;
  pc.id = s.allocateId();
  pc.pattern = s.patterns[0].id;
  pc.start = 0;
  s.playlist.push_back(pc);
  for (int i = 0; i < 5; ++i) s.playlist.push_back(audioClip(s, f.drum, s.barTicks() * i, 1));
  s.playlist.push_back(audioClip(s, f.vox, 0, 2));
  // Automation clips: one placed (volume of the drum channel), one not.
  AutomationClip au;
  au.id = s.allocateId();
  au.name = "Drum volume";
  au.owner = f.drum;
  au.index = 0;
  au.points = {AutoPoint{}, AutoPoint{}};
  au.points[1].pos = s.barTicks();
  s.automations.push_back(au);
  f.autoUsed = au.id;
  au.id = s.allocateId();
  au.name = "Spare automation";
  s.automations.push_back(au);
  f.autoUnused = au.id;
  Clip ac;
  ac.id = s.allocateId();
  ac.kind = ClipKind::Automation;
  ac.source = f.autoUsed;
  ac.track = 3;
  ac.length = s.barTicks();
  s.playlist.push_back(ac);
  // A second (unused) pattern.
  Pattern p2;
  p2.id = s.allocateId();
  p2.name = "Pattern 2";
  s.patterns.push_back(p2);
  f.pat2 = p2.id;
  // Links to the drum channel: an internal controller link, a MIDI link and a pattern automation lane.
  ModLink ml;
  ml.sourceOwner = f.synth;
  ml.owner = f.drum;
  ml.index = 0;
  s.modLinks.push_back(ml);
  ControllerLink cl;
  cl.cc = 7;
  cl.owner = f.drum;
  s.links.push_back(cl);
  PatternAutomation lane = patternLane(kNoId, 0, {});
  lane.owner = f.drum;
  lane.index = 0;
  s.patterns[0].automation.push_back(lane);
  // A second arrangement holding the 6th drum clip.
  ensureArrangementList(s);
  const int second = addArrangement(s, "Two", false);
  selectArrangement(s, second);
  s.playlist.push_back(audioClip(s, f.drum, 0, 0));
  selectArrangement(s, 0);
  return f;
}

int clipsOf(const Song& s, ClipKind k, ObjectId src) {
  int n = 0;
  forEachArrangementClip(s, [&](const Clip& c) { n += (c.kind == k && (k == ClipKind::Pattern ? c.pattern : c.source) == src) ? 1 : 0; });
  return n;
}

void testUsage() {
  Fixture f = makeFixture();
  const Song& s = f.song;
  expect(ops::clipSourceUses(s, ClipKind::Audio, f.drum) == 6, "uses: the drum loop has 6 clips over two arrangements");
  expect(ops::clipSourceUses(s, ClipKind::Audio, f.vox) == 1, "uses: vox has 1 clip");
  expect(ops::clipSourceUses(s, ClipKind::Audio, f.unusedAudio) == 0, "uses: the spare hit has none");
  expect(ops::clipSourceUses(s, ClipKind::Automation, f.autoUsed) == 1, "uses: placed automation clip");
  auto ua = ops::unusedClipSources(s, ClipKind::Audio);
  expect(ua == std::vector<ObjectId>{f.unusedAudio}, "unused audio: only the spare hit (the synth is no audio source)");
  expect(ops::unusedClipSources(s, ClipKind::Automation) == std::vector<ObjectId>{f.autoUnused}, "unused automation clips");
  expect(ops::unusedClipSources(s, ClipKind::Pattern) == std::vector<ObjectId>{f.pat2}, "unused patterns");
  expect(!ops::isAudioClipSource(*s.channel(f.synth)) && ops::isAudioClipSource(*s.channel(f.drum)), "audio clip source: samplers with a sample");
  // A sampler that also plays steps is not "unused".
  Song t = s;
  t.patterns[0].steps[f.unusedAudio] = {100, 0, 0, 0};
  expect(ops::unusedClipSources(t, ClipKind::Audio).empty() && ops::channelHasPatternData(t, f.unusedAudio),
         "unused audio: a sampler playing steps is kept");
}

void testDeleteUsedAudio() {
  Fixture f = makeFixture();
  Document d(f.song);
  const Song before = d.song();
  const auto fileSize = fs::file_size(f.drumPath);
  const size_t undoBefore = d.history().position();
  const int n = ops::deleteClipSources(d, ClipKind::Audio, {f.drum});
  const Song& s = d.song();
  expect(n == 1, "used audio: one source deleted");
  expect(!s.channel(f.drum), "used audio: the channel is gone");
  expect(clipsOf(s, ClipKind::Audio, f.drum) == 0, "used audio: every clip is gone (both arrangements)");
  expect(clipsOf(s, ClipKind::Audio, f.vox) == 1 && s.channel(f.vox), "used audio: other sources untouched");
  expect(clipsOf(s, ClipKind::Pattern, s.patterns[0].id) == 1, "used audio: pattern clips untouched");
  expect(std::none_of(s.modLinks.begin(), s.modLinks.end(), [&](const ModLink& l) { return l.owner == f.drum; }), "used audio: controller links to it go");
  expect(std::none_of(s.links.begin(), s.links.end(), [&](const ControllerLink& l) { return l.owner == f.drum; }), "used audio: MIDI links to it go");
  expect(s.patterns[0].automation.empty(), "used audio: pattern automation lanes of it go");
  expect(s.mixerTrack(f.drumMixer) != nullptr && s.mixer.size() == before.mixer.size(), "used audio: its mixer track stays");
  expect(s.automation(f.autoUsed) != nullptr, "used audio: automation clips stay (their target is gone)");
  expect(fs::exists(f.drumPath) && fs::file_size(f.drumPath) == fileSize, "used audio: the sample file stays on disk");
  expect(d.history().position() == undoBefore + 1, "used audio: one undo step");
  expect(d.undo() && d.song() == before, "used audio: undo restores the song exactly");
  expect(d.redo() && !d.song().channel(f.drum) && clipsOf(d.song(), ClipKind::Audio, f.drum) == 0, "used audio: redo deletes again");
  expect(fs::exists(f.drumPath), "used audio: the sample file is still there after redo");
}

void testDeleteUnusedAndMulti() {
  Fixture f = makeFixture();
  Document d(f.song);
  const Song before = d.song();
  expect(ops::deleteClipSources(d, ClipKind::Audio, ops::unusedClipSources(d.song(), ClipKind::Audio)) == 1 && !d.song().channel(f.unusedAudio),
         "unused audio: deleted");
  expect(d.song().playlist == before.playlist, "unused audio: the playlist is unchanged");
  expect(d.undo() && d.song() == before, "unused audio: undo restores it");
  // Multi-select: drum + vox in one step (duplicates ignored).
  const size_t u0 = d.history().position();
  expect(ops::deleteClipSources(d, ClipKind::Audio, {f.drum, f.vox, f.drum}) == 2, "multi: two sources deleted");
  expect(clipsOf(d.song(), ClipKind::Audio, f.drum) + clipsOf(d.song(), ClipKind::Audio, f.vox) == 0, "multi: all their clips gone");
  expect(d.history().position() == u0 + 1, "multi: one undo step");
  expect(d.undo() && d.song() == before, "multi: undo restores both and their clips");
  expect(fs::exists(f.drumPath) && fs::exists(f.voxPath), "multi: sample files untouched");
}

void testAutomationAndPatterns() {
  Fixture f = makeFixture();
  Document d(f.song);
  const Song before = d.song();
  expect(ops::deleteClipSources(d, ClipKind::Automation, {f.autoUsed}) == 1, "automation: deleted");
  expect(!d.song().automation(f.autoUsed) && clipsOf(d.song(), ClipKind::Automation, f.autoUsed) == 0, "automation: source and clips gone");
  expect(d.undo() && d.song() == before, "automation: undo restores it");
  expect(ops::deleteUnusedAutomationClips(d) == 1 && !d.song().automation(f.autoUnused) && d.song().automation(f.autoUsed),
         "automation: delete unused keeps the placed one");
  d.undo();
  // A clip in another arrangement counts as a use.
  {
    Song t = before;
    selectArrangement(t, 1);
    Clip ac;
    ac.id = t.allocateId();
    ac.kind = ClipKind::Automation;
    ac.source = f.autoUnused;
    t.playlist.push_back(ac);
    selectArrangement(t, 0);
    expect(ops::unusedAutomationClips(t).empty(), "automation: placed in another arrangement = used");
  }
  // Patterns: the used one with its clip; the last one is kept.
  const ObjectId p1 = before.patterns[0].id;
  expect(ops::deleteClipSources(d, ClipKind::Pattern, {p1}) == 1 && !d.song().pattern(p1) && clipsOf(d.song(), ClipKind::Pattern, p1) == 0,
         "pattern: deleted with its clips");
  expect(ops::deleteClipSources(d, ClipKind::Pattern, {f.pat2}) == 0 && d.song().patterns.size() == 1, "pattern: the last pattern stays");
  expect(d.undo() && d.song() == before, "pattern: undo restores it");
}

void testRoundTrips() {
  Fixture f = makeFixture();
  Document d(f.song);
  ops::deleteClipSources(d, ClipKind::Audio, {f.drum});
  ops::deleteClipSources(d, ClipKind::Automation, {f.autoUnused});
  const Song& s = d.song();
  std::string err;
  const std::string path = gDir + "/after_delete.gridline";
  Song back;
  expect(saveProject(path, s, &err) && loadProject(path, back, &err), "project: saves and loads after a delete (" + err + ")");
  expect(back.channels == s.channels && back.playlist == s.playlist && back.automations == s.automations && back.arrangements == s.arrangements,
         "project: channels, clips, automation and arrangements round-trip");
  expect(clipsOf(back, ClipKind::Audio, f.drum) == 0 && !back.channel(f.drum), "project: no dangling clip of the deleted source");
  flp::Bytes bytes;
  flp::ExportReport rep;
  flp::ExportOptions eo;
  eo.useOrigin = false;
  expect(flp::exportFlp(s, bytes, rep, eo), "flp: exported after a delete");
  Song imp;
  flp::Report ir;
  expect(importFlpBytes(bytes, imp, ir, &err), "flp: re-imported (" + err + ")");
  int audio = 0, autos = 0;
  for (const auto& c : imp.playlist) audio += c.kind == ClipKind::Audio, autos += c.kind == ClipKind::Automation;
  int audioWant = 0, autosWant = 0;
  for (const auto& c : s.playlist) audioWant += c.kind == ClipKind::Audio, autosWant += c.kind == ClipKind::Automation;
  expect(audio == audioWant && autos == autosWant, "flp: the remaining audio and automation clips come back");
  bool drumBack = false;
  for (const auto& ch : imp.channels) drumBack = drumBack || ch.name == "Drum loop";
  expect(!drumBack, "flp: the deleted source does not come back");
  for (const auto& c : imp.playlist)
    if (c.kind == ClipKind::Audio && !imp.channel(c.source)) expect(false, "flp: an audio clip without its source");
}

}  // namespace

int main() {
  gDir = test::hermeticTestEnv("clipsources");
  if (const char* out = std::getenv("GRIDLINE_J82_FIXTURE")) {  // screenshots: the fixture project (and its samples) in `out`
    gDir = out;
    fs::create_directories(gDir);
    std::string err;
    return saveProject(gDir + "/sources.gridline", makeFixture().song, &err) ? 0 : 1;
  }
  testUsage();
  testDeleteUsedAudio();
  testDeleteUnusedAndMulti();
  testAutomationAndPatterns();
  testRoundTrips();
  return finish();
}
