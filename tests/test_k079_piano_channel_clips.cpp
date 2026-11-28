// Gridline — 0.7.9 Wave 2 Track 6 tests: K-413, K-414, K-422, K-423, K-424, K-425.
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>
#include "Check.h"
#include "collab/CollabSim.h"
#include "collab/Session.h"
#include "core/Automation.h"
#include "core/ClipGain.h"
#include "core/ClipMultiEdit.h"
#include "core/Document.h"
#include "core/MenuOps.h"
#include "core/NoteLaneEdit.h"
#include "core/NoteTools.h"
#include "core/Params.h"
#include "core/PatternAutomation.h"
#include "core/Project.h"
#include "core/Settings.h"
#include "core/Song.h"
#include "engine/ClipFields.h"
#include "engine/Program.h"
#include "engine/Sample.h"

using namespace gl;

namespace {

// Collab sync helper: two peers connected over an in-memory link.
struct CollabPair {
  sim::Clock clock;
  sim::Net net{17};
  sim::Peer a, b;
  Document& docA;
  Document& docB;

  explicit CollabPair(const Song& initial, const std::string& tag = "k079")
      : docA(a.doc), docB(b.doc) {
    a.label = "Alice";
    b.label = "Bob";
    a.id = collab::crypto::Identity::generate();
    b.id = collab::crypto::Identity::generate();
    a.doc.reset(initial);
    a.folder = sim::tmpDir(tag + "_a");
    b.folder = sim::tmpDir(tag + "_b");
    std::string err;
    a.s = collab::Session::create(a.doc, sim::config(a.folder, a.id, a.label, &clock), &err);
    net.add(&a);
    b.s = collab::Session::join(b.doc, sim::config(b.folder, b.id, b.label, &clock), sim::ticketOf(*a.s), &err);
    a.s->admitMember(sim::memberOf(b));
    net.add(&b);
    net.connect(&a, &b);
    pump();
  }

  void pump() { sim::settle(net, {&a, &b}, clock); }
};

// Creates a temporary stereo WAV file containing a constant-amplitude sine wave
// (same on L and R) so clip gain and pan RMS ratios are exact.
std::string writeSineWav(const std::string& path, int sr = 44100, int frames = 44100) {
  std::ofstream f(path, std::ios::binary);
  const int channels = 2;
  const int bits = 16;
  const uint32_t dataBytes = static_cast<uint32_t>(frames * channels * (bits / 8));
  const uint32_t riffSize = 36 + dataBytes;
  auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
  auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
  f.write("RIFF", 4);
  u32(riffSize);
  f.write("WAVEfmt ", 8);
  u32(16);
  u16(1);  // PCM
  u16(channels);
  u32(static_cast<uint32_t>(sr));
  u32(static_cast<uint32_t>(sr * channels * (bits / 8)));
  u16(static_cast<uint16_t>(channels * (bits / 8)));
  u16(bits);
  f.write("data", 4);
  u32(dataBytes);
  for (int i = 0; i < frames; ++i) {
    const double s = 0.5 * std::sin(2.0 * 3.14159265358979323846 * 440.0 * static_cast<double>(i) / sr);
    const int16_t v = static_cast<int16_t>(std::lround(s * 32767.0));
    f.write(reinterpret_cast<const char*>(&v), 2);
    f.write(reinterpret_cast<const char*>(&v), 2);
  }
  return path;
}

void measureClipRms(const Song& song, double sr, double& rmsL, double& rmsR) {
  ProgramBuilder builder;
  builder.setSampleRate(sr);
  auto prog = builder.build(song);
  std::vector<float> L(8192, 0.0f), R(8192, 0.0f);
  for (const auto& ch : prog->channels) {
    if (!ch.audio.empty()) {
      renderAudioClips(ch.audio, prog->tempoMap, sr, 0.0, 0, 0, 8192, L.data(), R.data(), 0.0, nullptr);
    }
  }
  double sumL = 0.0, sumR = 0.0;
  // Skip first 512 samples (declick fade-in) for steady-state RMS.
  int count = 0;
  for (int i = 1024; i < 7168; ++i) {
    sumL += static_cast<double>(L[static_cast<size_t>(i)]) * L[static_cast<size_t>(i)];
    sumR += static_cast<double>(R[static_cast<size_t>(i)]) * R[static_cast<size_t>(i)];
    ++count;
  }
  rmsL = std::sqrt(sumL / count);
  rmsR = std::sqrt(sumR / count);
}

void testK413ChannelAutoLanes() {
  Document doc(makeEmptySong());
  const ObjectId ch1 = doc.addChannel("gl.sampler", "Lead");
  const ObjectId pat1 = doc.song().patterns[0].id;
  const ObjectId pat2 = doc.addPattern("Pattern 2");
  const ParamId volParam = channelParam(ch1, ChannelParam::Volume);
  const ParamId tempoParam = songParam(SongParam::Tempo);

  // Add automation clip on Pattern 1 and register persistent lanes on channel & song.
  doc.editSong("add auto lanes", [&](Song& s) {
    Pattern* p = s.pattern(pat1);
    PatternAutomation c1 = newPatternClip(s, volParam, 0.5f, 384);
    addPatternClip(*p, c1);
    ensureRackAutoLane(s, volParam.owner, volParam.index, c1.name, 0xFF8800);
    PatternAutomation c2 = newPatternClip(s, tempoParam, 0.4f, 384);
    addPatternClip(*p, c2);
    ensureRackAutoLane(s, tempoParam.owner, tempoParam.index, c2.name, 0x00CCFF);
  });

  // Switching to Pattern 2 shows the exact same automation lanes even though Pattern 2 has no events yet.
  const Pattern* p2 = doc.song().pattern(pat2);
  expect(p2 && p2->automation.empty(), "K-413: Pattern 2 has no automation events yet");
  const auto lanesP2 = effectiveRackAutoLanes(doc.song(), p2);
  expect(lanesP2.size() == 2, "K-413: Pattern 2 displays both persisted rack automation lanes");
  expect(lanesP2[0].owner == ch1 && lanesP2[0].index == volParam.index, "K-413: channel lane persists into Pattern 2");
  expect(lanesP2[1].owner == kSongId && lanesP2[1].index == tempoParam.index, "K-413: song lane persists into Pattern 2");

  // Save/load JSON round-trip preserves channel and song automation lanes.
  const std::string saved = projectToText(doc.song());
  Song loaded;
  std::string err;
  expect(loadProjectText(saved, "k413.gridline.json", loaded, &err), "K-413: JSON round-trip loads");
  expect(loaded.channel(ch1) && loaded.channel(ch1)->automationLanes.size() == 1, "K-413: Channel::automationLanes persisted");
  expect(loaded.rackAutoLanes.size() == 1, "K-413: Song::rackAutoLanes persisted");

  // Old-project migration: clear automationLanes/rackAutoLanes in JSON, reload -> unions existing pattern lanes!
  Json j;
  expect(jsonParse(saved, j), "K-413: parse JSON for legacy migration test");
  j.object.erase("rackAutoLanes");
  if (auto it = j.object.find("channels"); it != j.object.end() && it->second.is(Json::Type::Array)) {
    for (Json& jc : it->second.array) jc.object.erase("automationLanes");
  }
  Song migrated;
  expect(loadProjectText(jsonStringify(j), "legacy.gridline.json", migrated, &err), "K-413: legacy project loads");
  expect(migrated.channel(ch1) && migrated.channel(ch1)->automationLanes.size() == 1,
         "K-413: legacy migration populates Channel::automationLanes");
  expect(migrated.rackAutoLanes.size() == 1, "K-413: legacy migration populates Song::rackAutoLanes");
  expect(effectiveRackAutoLanes(migrated, migrated.pattern(pat2)).size() == 2,
         "K-413: legacy migrated lanes visible in Pattern 2");

  // Removing a lane is undoable and collab-synced.
  CollabPair cp(doc.song());
  cp.docA.editSong("remove automation lane", [&](Song& s) { removeRackAutoLane(s, volParam.owner, volParam.index); });
  cp.pump();
  expect(!hasRackAutoLane(cp.docA.song(), volParam.owner, volParam.index), "K-413: lane removed on peer A");
  expect(!hasRackAutoLane(cp.docB.song(), volParam.owner, volParam.index), "K-413: lane removal synced to peer B");
  expect(cp.docA.undo(), "K-413: lane removal undoable");
  expect(hasRackAutoLane(cp.docA.song(), volParam.owner, volParam.index), "K-413: lane restored after undo");
}

void testK414AudioClipGainPan() {
  const std::string wav = writeSineWav("/tmp/gl_k079_test_sine.wav");
  Document doc(makeEmptySong());
  const ObjectId ch = doc.addChannel("gl.sampler", "Audio");
  doc.editChannel(ch, "load sample", [&](Channel& c) {
    c.audioClip = true;
    c.plugin.data = wav;
  });
  ObjectId clipId = kNoId;
  doc.editSong("add audio clip", [&](Song& s) {
    Clip c;
    c.id = s.allocateId();
    c.kind = ClipKind::Audio;
    c.source = ch;
    c.track = 0;
    c.start = 0;
    c.length = 384;
    c.gain = 1.0f;
    c.pan = 0.0f;
    clipId = c.id;
    s.playlist.push_back(c);
  });

  // 1. Center unity baseline
  double rmsL0 = 0, rmsR0 = 0;
  measureClipRms(doc.song(), 44100.0, rmsL0, rmsR0);
  expect(rmsL0 > 0.2 && std::fabs(rmsL0 - rmsR0) < 1e-4, "K-414: unity center clip has equal L/R RMS");

  // 2. +6.02 dB (gain = 2.0) -> 2x RMS
  const uint64_t rev0 = doc.revision();
  doc.editClip(clipId, "clip gain", [](Clip& c) { c.gain = 2.0f; });
  expect(doc.clipLevelsOnlySince(rev0), "K-414: gain edit is clipLevelsOnly");
  double rmsL2 = 0, rmsR2 = 0;
  measureClipRms(doc.song(), 44100.0, rmsL2, rmsR2);
  expect(std::fabs(rmsL2 / rmsL0 - 2.0) < 0.02 && std::fabs(rmsR2 / rmsR0 - 2.0) < 0.02,
         "K-414: gain 2.0 doubles L and R RMS");

  // 3. Hard left pan (-1.0) + unity gain -> R silent, L full
  const uint64_t rev1 = doc.revision();
  doc.editClip(clipId, "clip pan", [](Clip& c) {
    c.gain = 1.0f;
    c.pan = -1.0f;
  });
  expect(doc.clipLevelsOnlySince(rev1), "K-414: pan edit is clipLevelsOnly (live patchable)");
  double rmsLL = 0, rmsRL = 0;
  measureClipRms(doc.song(), 44100.0, rmsLL, rmsRL);
  expect(std::fabs(rmsLL / rmsL0 - 1.0) < 0.02 && rmsRL < 1e-5, "K-414: hard left pan mutes R and preserves L");

  // 4. Hard right pan (+1.0) -> L silent, R full
  doc.editClip(clipId, "clip pan", [](Clip& c) { c.pan = 1.0f; });
  double rmsLR = 0, rmsRR = 0;
  measureClipRms(doc.song(), 44100.0, rmsLR, rmsRR);
  expect(rmsLR < 1e-5 && std::fabs(rmsRR / rmsR0 - 1.0) < 0.02, "K-414: hard right pan mutes L and preserves R");

  // 5. Partial right pan (+0.5) -> L is 0.5x, R is 1.0x
  doc.editClip(clipId, "clip pan", [](Clip& c) { c.pan = 0.5f; });
  double rmsLP = 0, rmsRP = 0;
  measureClipRms(doc.song(), 44100.0, rmsLP, rmsRP);
  expect(std::fabs(rmsLP / rmsL0 - 0.5) < 0.02 && std::fabs(rmsRP / rmsR0 - 1.0) < 0.02,
         "K-414: +0.5 pan scales L to 0.5x and keeps R at 1.0x");

  // 6. Live ProgramBuilder::patchClipLevels updates pan and gain without rebuild
  ProgramBuilder pb;
  pb.setSampleRate(44100.0);
  auto prog = pb.build(doc.song());
  doc.editClip(clipId, "live pan", [](Clip& c) {
    c.gain = 0.5f;
    c.pan = -0.75f;
  });
  expect(pb.patchClipLevels(doc.song()), "K-414: patchClipLevels patches pan & gain");
  bool foundPatched = false;
  for (const auto& pc : prog->channels)
    for (const auto& ac : pc.audio)
      if (ac.clip == clipId) {
        foundPatched = std::fabs(static_cast<float>(ac.gain) - 0.5f) < 1e-4f && std::fabs(static_cast<float>(ac.pan) - (-0.75f)) < 1e-4f;
      }
  expect(foundPatched, "K-414: live Program carries patched gain & pan");

  // 7. ClipFields + ClipMultiEdit Pan support + JSON + collab sync
  expect(clipfields::set(doc, {clipId}, clipId, clipfields::Field::Pan, -40.0), "K-414: ClipFields sets pan");
  expect(std::fabs(doc.song().clip(clipId)->pan - (-0.4f)) < 1e-4f, "K-414: ClipFields stored -0.4 pan");
  auto snap = clipmulti::capture(doc.song(), {clipId}, clipmulti::Param::Pan, clipId);
  expect(clipmulti::offset(doc, "clip pan", snap, 0.7), "K-414: ClipMultiEdit offsets pan");
  expect(std::fabs(doc.song().clip(clipId)->pan - 0.3f) < 1e-4f, "K-414: ClipMultiEdit resulting pan +0.3");

  Song loaded;
  std::string err;
  expect(loadProjectText(projectToText(doc.song()), "k414.gridline.json", loaded, &err), "K-414: JSON loads");
  expect(loaded.clip(clipId) && std::fabs(loaded.clip(clipId)->pan - 0.3f) < 1e-4f && std::fabs(loaded.clip(clipId)->gain - 0.5f) < 1e-4f,
         "K-414: JSON preserves clip gain and pan");

  CollabPair cp(doc.song());
  cp.docA.editClip(clipId, "collab clip pan/gain", [](Clip& c) {
    c.gain = 1.5f;
    c.pan = -0.6f;
  });
  cp.pump();
  expect(cp.docB.song().clip(clipId) && std::fabs(cp.docB.song().clip(clipId)->gain - 1.5f) < 1e-4f &&
             std::fabs(cp.docB.song().clip(clipId)->pan - (-0.6f)) < 1e-4f,
         "K-414: clip gain & pan sync over collab");

  std::filesystem::remove(wav);
}

void testK422PreviewWhilePlayingSetting() {
  Settings st;
  expect(!st.getBool("piano.previewWhilePlaying"), "K-422: piano.previewWhilePlaying defaults to false");
  expect(st.set("piano.previewWhilePlaying", true), "K-422: setting can be toggled on");
  expect(st.getBool("piano.previewWhilePlaying"), "K-422: setting reads back true");
  const std::string p = "/tmp/gl_k079_settings_k422.json";
  expect(st.save(p), "K-422: settings save");
  Settings st2;
  expect(st2.load(p), "K-422: settings load");
  expect(st2.getBool("piano.previewWhilePlaying"), "K-422: piano.previewWhilePlaying persists across reload");
  std::filesystem::remove(p);
}

void testK423EventLaneSnapGrids() {
  // Fine pitch: mode 1 = 1 semitone (24 steps over -1200..+1200 ct = exact 100 ct increments!)
  expect(notelane::gridSteps(tools::NoteProp::Fine, 1) == 24, "K-423: Fine pitch 1 semitone has 24 steps");
  for (int semi = -12; semi <= 12; ++semi) {
    const double raw = (static_cast<double>(semi) + 12.0 + 0.31) / 24.0;
    const double clampedRaw = std::clamp(raw, 0.0, 1.0);
    const double snapped = notelane::snapValue(tools::NoteProp::Fine, 1, clampedRaw, false);
    Note n;
    tools::setNotePropNorm(n, tools::NoteProp::Fine, snapped);
    const int expectedCents = (semi == 12 ? 12 : semi) * 100;
    expect(n.fine == expectedCents, "K-423: Fine pitch snaps cleanly to exact integer semitone cents");
  }
  // Fine pitch octave grid (mode 5 = 2 steps: -1200, 0, +1200)
  expect(notelane::gridSteps(tools::NoteProp::Fine, 5) == 2, "K-423: Fine pitch octave grid has 2 steps");
  expect(std::fabs(notelane::snapValue(tools::NoteProp::Fine, 5, 0.72, false) - 0.5) < 1e-9, "K-423: Octave grid snaps 0.72 to 0.5 (0 ct)");
  expect(std::fabs(notelane::snapValue(tools::NoteProp::Fine, 5, 0.80, false) - 1.0) < 1e-9, "K-423: Octave grid snaps 0.80 to 1.0 (+1200 ct)");

  // Velocity 1/8ths (mode 2 = 8 steps)
  expect(notelane::gridSteps(tools::NoteProp::Velocity, 2) == 8, "K-423: Velocity 1/8 grid has 8 steps");
  expect(std::fabs(notelane::snapValue(tools::NoteProp::Velocity, 2, 0.36, false) - 0.375) < 1e-9,
         "K-423: Velocity 1/8 snaps 0.36 to 3/8 (0.375)");

  // Pan 1/4 (mode 1 = 4 steps: 0, 0.25, 0.5, 0.75, 1.0)
  expect(notelane::gridSteps(tools::NoteProp::Pan, 1) == 4, "K-423: Pan 1/4 grid has 4 steps");
  expect(std::fabs(notelane::snapValue(tools::NoteProp::Pan, 1, 0.52, false) - 0.5) < 1e-9, "K-423: Pan 1/4 snaps near center to 0.5");

  // Alt bypass passes through unsnapped!
  const double raw = 0.36123;
  expect(std::fabs(notelane::snapValue(tools::NoteProp::Fine, 1, raw, true) - raw) < 1e-9,
         "K-423: Alt bypass skips snapping smoothly");

  // Grid lines generated for active grid, empty for Off
  expect(notelane::gridLines(tools::NoteProp::Fine, 0).empty(), "K-423: Off mode emits no grid lines");
  expect(notelane::gridLines(tools::NoteProp::Fine, 1).size() == 24, "K-423: 1 semitone mode emits 24 lines");

  // Saved independently per lane in Settings
  Settings st;
  expect(st.set(notelane::gridSettingKey(tools::NoteProp::Fine), 1), "K-423: save Fine grid");
  expect(st.set(notelane::gridSettingKey(tools::NoteProp::Velocity), 2), "K-423: save Velocity grid");
  expect(st.set(notelane::gridSettingKey(tools::NoteProp::Pan), 1), "K-423: save Pan grid");
  const std::string p = "/tmp/gl_k079_settings_k423.json";
  expect(st.save(p), "K-423: settings save");
  Settings st2;
  expect(st2.load(p), "K-423: settings reload");
  expect(static_cast<int>(st2.getNumber(notelane::gridSettingKey(tools::NoteProp::Fine))) == 1, "K-423: Fine grid persisted");
  expect(static_cast<int>(st2.getNumber(notelane::gridSettingKey(tools::NoteProp::Velocity))) == 2, "K-423: Velocity grid persisted");
  expect(static_cast<int>(st2.getNumber(notelane::gridSettingKey(tools::NoteProp::Pan))) == 1, "K-423: Pan grid persisted");
  expect(static_cast<int>(st2.getNumber(notelane::gridSettingKey(tools::NoteProp::ModX))) == 0, "K-423: untouched lane remains Off");
  std::filesystem::remove(p);
}

void testK424DoublePattern() {
  Document doc(makeEmptySong());
  const ObjectId ch1 = doc.addChannel("gl.sampler", "Kick");
  const ObjectId ch2 = doc.addChannel("gl.sampler", "Synth");
  const ObjectId pat = doc.song().patterns[0].id;
  const Tick bar = doc.song().barTicks();  // 384 at 96 PPQ 4/4
  const ParamId vol = channelParam(ch2, ChannelParam::Volume);

  doc.editPattern(pat, "seed pattern", [&](Pattern& p) {
    p.bars = 1;
    p.notes[ch2] = {Note{0, 48, 60, 100}, Note{192, 48, 64, 90}};
    p.steps[ch1] = {100, 0, 0, 0, 100, 0, 0, 0, 100, 0, 0, 0, 100, 0, 0, 0};
    PatternAutomation ac = newPatternClip(doc.song(), vol, 0.2f, bar);
    ac.points = {AutoPoint{0, 0.2f}, AutoPoint{192, 0.8f}};
    addPatternClip(p, ac);
  });
  const ObjectId clipId = doc.addClip(pat, 0, 0, bar);

  const Song before = doc.song();
  const size_t undoBefore = doc.history().position();
  expect(ops::doublePattern(doc, pat, clipId), "K-424: doublePattern succeeds (1 -> 2 bars)");
  expect(doc.history().position() == undoBefore + 1, "K-424: doublePattern is exactly 1 undo step");

  const Pattern* p = doc.song().pattern(pat);
  expect(p && p->bars == 2, "K-424: pattern length doubled to 2 bars");
  expect(p->notes.at(ch2).size() == 4, "K-424: notes duplicated into second bar");
  expect(p->notes.at(ch2)[2].start == bar && p->notes.at(ch2)[2].pitch == 60, "K-424: 3rd note at bar 2 start");
  expect(p->notes.at(ch2)[3].start == bar + 192 && p->notes.at(ch2)[3].pitch == 64, "K-424: 4th note at bar 2 beat 3");
  expect(p->steps.at(ch1).size() == 32 && p->steps.at(ch1)[16] == 100 && p->steps.at(ch1)[20] == 100,
         "K-424: steps duplicated into second bar");
  const PatternAutomation* ac = findPatternLane(*p, vol.owner, vol.index);
  expect(ac && ac->points.size() == 4 && ac->points[2].pos == bar && ac->points[3].pos == bar + 192,
         "K-424: pattern automation duplicated into second bar");
  expect(doc.song().clip(clipId) && doc.song().clip(clipId)->length == bar * 2,
         "K-424: active pattern clip resized to 2 bars in same undo step");

  // 1 undo restores exact pre-double state!
  expect(doc.undo(), "K-424: single undo works");
  expect(doc.song() == before, "K-424: single undo restores exact 1-bar pattern & clip");

  // Collab sync test
  CollabPair cp(before);
  expect(ops::doublePattern(cp.docA, pat, clipId), "K-424: doublePattern on peer A");
  cp.pump();
  const Pattern* pb = cp.docB.song().pattern(pat);
  expect(pb && pb->bars == 2 && pb->notes.at(ch2).size() == 4, "K-424: doubled pattern syncs over collab");
}

void testK425MultiChannelDelete() {
  Document doc(makeEmptySong());
  const ObjectId c1 = doc.addChannel("gl.sampler", "Ch1");
  const ObjectId c2 = doc.addChannel("gl.sampler", "Ch2");
  const ObjectId c3 = doc.addChannel("gl.sampler", "Ch3");
  const ObjectId c4 = doc.addChannel("gl.sampler", "Ch4");
  const ObjectId pat = doc.song().patterns[0].id;

  // Populate notes, steps, pattern automation, rackAutoLanes, playlist track link, playlist automation clip, LFO link.
  doc.editSong("setup multi-delete refs", [&](Song& s) {
    Pattern* p = s.pattern(pat);
    p->notes[c1] = {Note{0, 48, 60, 100}};
    p->notes[c2] = {Note{0, 48, 62, 100}};
    p->notes[c3] = {Note{0, 48, 64, 100}};
    p->steps[c2] = {100, 0, 100, 0};
    const ParamId v2 = channelParam(c2, ChannelParam::Volume);
    addPatternClip(*p, newPatternClip(s, v2, 0.5f, 384));
    ensureRackAutoLane(s, v2.owner, v2.index);
    // Playlist instrument track referencing c2
    s.tracks[1].mode = TrackMode::Instrument;
    s.tracks[1].channel = c2;
    s.tracks[1].sound = c2;
    s.tracks[1].laneSet = true;
    s.tracks[1].laneOwner = c2;
    // Playlist automation clip targeting c3
    AutomationClip ac;
    ac.id = s.allocateId();
    ac.owner = c3;
    ac.index = static_cast<uint32_t>(ChannelParam::Volume);
    ac.points = {AutoPoint{0, 0.5f}};
    s.automations.push_back(ac);
    Clip k;
    k.id = s.allocateId();
    k.kind = ClipKind::Automation;
    k.source = ac.id;
    k.track = 2;
    k.start = 0;
    k.length = 384;
    s.playlist.push_back(k);
    // LFO referencing c1
    SongLfo lfo;
    lfo.id = s.allocateId();
    lfo.noteChannel = c1;
    s.lfos.push_back(lfo);
  });

  const Song before = doc.song();
  const size_t undoBefore = doc.history().position();
  expect(ops::removeChannels(doc, {c1, c2, c3}), "K-425: removeChannels deletes 3 channels at once");
  expect(doc.history().position() == undoBefore + 1, "K-425: multi-channel delete is 1 undo step");

  const Song& after = doc.song();
  expect(after.channels.size() == 1 && after.channels[0].id == c4, "K-425: only unselected Ch4 remains");
  const Pattern* pa = after.pattern(pat);
  expect(pa && !pa->notes.count(c1) && !pa->notes.count(c2) && !pa->notes.count(c3), "K-425: pattern notes cleaned up");
  expect(!pa->steps.count(c2), "K-425: pattern steps cleaned up");
  expect(pa->automation.empty(), "K-425: pattern automation cleaned up");
  // (as for one channel: automation clips and their playlist clips stay, with no target; test_clip_sources)
  expect(after.automations.size() == 1 && after.playlist.size() == 1 && after.playlist[0].kind == ClipKind::Automation,
         "K-425: playlist automation clips stay, as when one channel is deleted");
  expect(after.tracks[1].channel == kNoId && after.tracks[1].mode == TrackMode::Unassigned &&
             after.tracks[1].sound == kNoId && !after.tracks[1].laneSet,
         "K-425: playlist track links cleaned up");
  expect(after.lfos[0].noteChannel == kNoId, "K-425: LFO noteChannel cleaned up");

  // 1 undo restores ALL 3 channels and all references!
  expect(doc.undo(), "K-425: single undo succeeds");
  expect(doc.song() == before, "K-425: single undo restores all 3 channels and their refs");

  // Collab sync
  CollabPair cp(before);
  expect(ops::removeChannels(cp.docA, {c1, c2, c3}), "K-425: multi-channel delete on peer A");
  cp.pump();
  expect(cp.docB.song().channels.size() == 1 && cp.docB.song().channels[0].id == c4,
         "K-425: multi-channel delete synced cleanly to peer B");
}

}  // namespace

int main() {
  testK413ChannelAutoLanes();
  testK414AudioClipGainPan();
  testK422PreviewWhilePlayingSetting();
  testK423EventLaneSnapGrids();
  testK424DoublePattern();
  testK425MultiChannelDelete();
  return finish();
}
