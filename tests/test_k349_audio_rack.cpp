// Gridline: K-349, audio clips show in the channel rack. Every way the engine places audio on the
// playlist (a recording, a render in place, a consolidated range, stems) leaves a channel the rack
// lists: in the song, not hidden from the rack, in no display group, and among the rack's audio
// clip channels (ops::audioClipChannels, its "Audio clips" filter). "Select unused channels" leaves
// audio clip channels alone (their clips play from them), and an older project, saved before the
// channel was marked in any way, reads its audio clip channels as such.
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>
#include "Check.h"
#include "Hermetic.h"
#include "core/ClipSources.h"
#include "core/Document.h"
#include "core/MenuOps.h"
#include "core/PluginInfo.h"
#include "core/PluginParams.h"
#include "core/Project.h"
#include "engine/AudioRecord.h"
#include "engine/Chop.h"
#include "engine/ClipRender.h"
#include "engine/Render.h"
#include "engine/TimeStretch.h"
#include "engine/stems/StemPlace.h"

namespace fs = std::filesystem;
using namespace gl;

namespace {

std::string gDir;
void expect(bool ok, const std::string& name) { expect(ok, name.c_str()); }

// A short 16-bit mono WAV holding a decaying tone (so renders are not silent).
std::string writeWav(const std::string& name) {
  const std::string p = gDir + "/" + name + ".wav";
  std::ofstream f(p, std::ios::binary);
  auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
  auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
  const uint32_t frames = 22050;
  f.write("RIFF", 4), u32(36 + frames * 2), f.write("WAVE", 4);
  f.write("fmt ", 4), u32(16), u16(1), u16(1), u32(44100), u32(44100 * 2), u16(2), u16(16);
  f.write("data", 4), u32(frames * 2);
  for (uint32_t i = 0; i < frames; ++i) {
    const double v = std::sin(i * 2.0 * 3.141592653589793 * 220.0 / 44100.0) * std::exp(-3.0 * i / frames);
    u16(static_cast<uint16_t>(static_cast<int16_t>(v * 12000)));
  }
  return p;
}

// A song with one sampler channel playing `wav` from one audio clip on track 0.
Song songWithClip(const std::string& wav, ObjectId* clipOut, ObjectId* chOut) {
  Song s = makeEmptySong();
  s.tempo = 120;
  Channel ch;
  ch.id = s.allocateId();
  ch.name = "Loop";
  ch.plugin = defaultPluginState("gl.sampler");
  ch.plugin.data = wav;
  ch.mixerTrack = s.masterId();
  s.channels.push_back(ch);
  Clip c;
  c.id = s.allocateId();
  c.kind = ClipKind::Audio;
  c.source = ch.id;
  c.track = 0;
  c.start = 0;
  c.length = s.ppq * 2;
  if (s.tracks.size() < 8) s.tracks.resize(8);
  s.playlist.push_back(c);
  if (clipOut) *clipOut = c.id;
  if (chOut) *chOut = ch.id;
  return s;
}

// What the channel rack needs to list a channel as an audio clip channel in every filter that shows it.
bool rackLists(const Song& s, ObjectId ch) {
  const Channel* c = s.channel(ch);
  return c && !c->rackHidden && c->group < 0 && ops::audioClipChannels(s).count(ch) > 0;
}

// Every new audio clip of `after` (not in `before`) plays from a channel the rack lists.
int checkNewClips(const Song& before, const Song& after, const std::string& what) {
  std::set<ObjectId> old;
  for (const auto& c : before.playlist) old.insert(c.id);
  int n = 0;
  for (const auto& c : after.playlist) {
    if (old.count(c.id) || c.kind != ClipKind::Audio) continue;
    ++n;
    expect(rackLists(after, c.source), what + ": the new clip's channel is listed in the rack as an audio clip channel");
  }
  expect(n > 0, what + ": made an audio clip");
  return n;
}

void testDetection() {
  const std::string wav = writeWav("detect");
  ObjectId clip = kNoId, ch = kNoId;
  Song s = songWithClip(wav, &clip, &ch);
  Channel inst;
  inst.id = s.allocateId();
  inst.name = "Synth";
  inst.plugin = defaultPluginState("gl.synth");
  s.channels.push_back(inst);
  Channel flagged;  // an FL audio clip channel no clip uses any more
  flagged.id = s.allocateId();
  flagged.name = "Kept";
  flagged.plugin = defaultPluginState("gl.sampler");
  flagged.audioClip = true;
  s.channels.push_back(flagged);
  const std::set<ObjectId> clips = ops::audioClipChannels(s);
  expect(clips.count(ch) == 1, "a playlist audio clip's channel is an audio clip channel");
  expect(clips.count(flagged.id) == 1, "a channel marked as an audio clip channel stays one without clips");
  expect(clips.count(inst.id) == 0, "an instrument is not an audio clip channel");
  // Another arrangement's clip counts too.
  Song t = s;
  Arrangement other;
  other.name = "B";
  other.playlist = t.playlist;
  t.playlist.clear();
  t.arrangements.push_back(Arrangement{});
  t.arrangements.back().name = "A";
  t.arrangements.push_back(other);
  t.currentArrangement = 0;
  expect(ops::audioClipChannels(t).count(ch) == 1, "a clip in another arrangement makes its channel an audio clip channel");
}

void testUnused() {
  const std::string wav = writeWav("unused");
  ObjectId clip = kNoId, ch = kNoId;
  Document d;
  d.reset(songWithClip(wav, &clip, &ch));
  const ObjectId spare = d.addChannel("Spare", "gl.clap");
  const std::vector<ObjectId> u = ops::unusedChannels(d.song());
  expect(std::find(u.begin(), u.end(), ch) == u.end(), "Select unused channels leaves an audio clip channel alone");
  expect(std::find(u.begin(), u.end(), spare) != u.end(), "a channel nothing uses is still unused");
  // Deleting the channel takes its clips with it, one undo step brings both back.
  const size_t pos = d.history().position();
  expect(ops::removeChannels(d, {ch}), "delete the audio clip channel");
  expect(!d.song().channel(ch) && d.song().playlist.empty(), "its clips go with it");
  expect(d.history().position() == pos + 1, "deleting it is one undo step");
  d.undo();
  expect(d.song().channel(ch) && d.song().playlist.size() == 1 && d.song().playlist[0].source == ch, "one undo brings the channel and its clip back");
}

void testRecording() {
  Song s = makeEmptySong();
  s.tempo = 120;
  s.tracks.resize(8);
  RecordedTake t;
  t.mixer = s.masterId();
  t.startTick = 0;
  t.done = true;
  t.L.assign(22050, 0.0f);
  for (size_t i = 0; i < t.L.size(); ++i) t.L[i] = 0.3f * static_cast<float>(std::sin(i * 0.05));
  t.R = t.L;
  TakeCommitOptions o;
  o.dir = gDir + "/Recorded";
  fs::create_directories(o.dir);
  const Song before = s;
  const TakeCommitResult r = commitTakes(s, {t}, o);
  expect(r.error.empty(), "the take is written");
  checkNewClips(before, s, "recording");
}

void testRenderInPlace() {
  const std::string wav = writeWav("render");
  ObjectId clip = kNoId;
  Song s = songWithClip(wav, &clip, nullptr);
  const std::string dir = gDir + "/Rendered";
  fs::create_directories(dir);
  const Song before = s;
  expect(renderClipInPlace(s, clip, dir, 44100) != kNoId, "render clip to audio");
  checkNewClips(before, s, "render clip to audio");
}

void testConsolidate() {
  const std::string wav = writeWav("chop");
  Song s = songWithClip(wav, nullptr, nullptr);
  const std::string dir = gDir + "/Consolidated";
  fs::create_directories(dir);
  const Song before = s;
  expect(chop::consolidateRange(s, 0, s.ppq * 2, {0}, dir, 44100) != kNoId, "consolidate a range");
  checkNewClips(before, s, "consolidate (chop sending a region)");
}

void testStems() {
  const std::string wav = writeWav("mix");
  ObjectId clip = kNoId;
  Song s = songWithClip(wav, &clip, nullptr);
  const std::vector<stems::StemFile> files = {{"vocals", writeWav("vocals")}, {"drums", writeWav("drums")}};
  const Song before = s;
  expect(stems::placeStems(s, clip, files, "Mix", true).size() == 2, "two stems placed");
  expect(checkNewClips(before, s, "stems") == 2, "one clip per stem");
}

// A project saved by an older version: the audio clip channel is a plain sampler (no mark), placed
// after twenty instruments in a display group. It loads as an audio clip channel the rack lists.
void testOldProject() {
  const std::string wav = writeWav("old");
  ObjectId ch = kNoId;
  Song s = songWithClip(wav, nullptr, &ch);
  s.channelGroups.push_back("Drums");
  for (int i = 0; i < 20; ++i) {
    Channel c;
    c.id = s.allocateId();
    c.name = "Inst " + std::to_string(i + 1);
    c.plugin = defaultPluginState("gl.synth");
    c.group = 0;
    s.channels.insert(s.channels.begin(), c);
  }
  const std::string path = gDir + "/old.gridline";
  std::string err;
  expect(saveProject(path, s, &err), "save the older project");
  std::ifstream in(path);
  const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  expect(text.find("\"audioClip\"") == std::string::npos, "the older project carries no audio clip mark");
  Song back;
  expect(loadProject(path, back, &err), "load the older project");
  expect(rackLists(back, ch), "its audio clip channel loads as one the rack lists");
  expect(back.channel(ch) && !back.channel(ch)->audioClip, "loading changes nothing in the channel (its sound stays)");
}

// An audio clip channel's steps play its sample like any sampler channel's (the rack shows it as a
// row of steps in every layout), and its playlist clip plays exactly as it did without steps.
double rms(const AudioBuffer& b, size_t f0, size_t f1) {
  double e = 0;
  size_t n = 0;
  for (size_t i = f0; i < f1 && i < b.frames; ++i, ++n) e += b.at(0, i) * b.at(0, i) + b.at(1, i) * b.at(1, i);
  return n ? std::sqrt(e / static_cast<double>(n)) : 0.0;
}
void testStepsPlay(bool stretchMode) {
  const std::string tag = stretchMode ? " (stretch mode)" : "";
  const std::string wav = writeWav(stretchMode ? "steps_stretch" : "steps");
  ObjectId ch = kNoId;
  Song s = songWithClip(wav, nullptr, &ch);  // its clip plays the first two beats
  if (stretchMode) {
    Channel* c = s.channel(ch);
    if (c && c->plugin.params.size() > static_cast<size_t>(pp::SmpStretchMode))
      c->plugin.params[pp::SmpStretchMode] = static_cast<float>(stretch::kStretch);
  }
  // A pattern clip on track 1 at bar 3 (four seconds in at 120 BPM): the channel's steps play there.
  Clip pc;
  pc.id = s.allocateId();
  pc.kind = ClipKind::Pattern;
  pc.pattern = s.patterns[0].id;
  pc.track = 1;
  pc.start = s.ppq * 4 * 2;
  pc.length = s.ppq * 4;
  s.playlist.push_back(pc);
  RenderOptions o;
  o.tail = s.ppq * 4;
  const AudioBuffer before = renderSong(s, o);
  Song stepped = s;
  stepped.patterns[0].steps[ch] = std::vector<uint8_t>(16, 0);
  stepped.patterns[0].steps[ch][0] = 100;
  stepped.patterns[0].steps[ch][8] = 100;
  expect(rackLists(stepped, ch), "an audio clip channel with steps is still listed as one" + tag);
  const AudioBuffer after = renderSong(stepped, o);
  const size_t bar3 = 44100 * 4, half = 44100 / 4;
  expect(rms(before, bar3, bar3 + half) < 1e-5, "no steps: silent where the pattern clip is" + tag);
  expect(rms(after, bar3, bar3 + half) > 1e-3, "a step on the audio clip channel plays its sample" + tag);
  expect(rms(after, bar3 + 44100, bar3 + 44100 + half) > 1e-3, "its second step (half a bar on) plays too" + tag);
  // The playlist clip (the first two beats and its tail) is sample for sample what it was.
  bool same = before.frames == after.frames;
  for (size_t i = 0; same && i < bar3 && i < before.frames; ++i) same = before.at(0, i) == after.at(0, i) && before.at(1, i) == after.at(1, i);
  expect(same, "the playlist audio clip plays exactly as before" + tag);
  expect(rms(after, 0, half) > 1e-3, "the playlist audio clip still sounds" + tag);
  // Saved and loaded, its steps and its clip stay.
  const std::string path = gDir + (stretchMode ? "/steps_stretch.gridline" : "/steps.gridline");
  std::string err;
  expect(saveProject(path, stepped, &err), "save a song with steps on an audio clip channel" + tag);
  Song back;
  expect(loadProject(path, back, &err), "load it" + tag);
  expect(back.patterns.size() == stepped.patterns.size() && back.patterns[0].steps.count(ch) && back.patterns[0].steps.at(ch) == stepped.patterns[0].steps.at(ch),
         "the audio clip channel's steps load back" + tag);
  expect(rackLists(back, ch), "and it is still an audio clip channel" + tag);
  const AudioBuffer loaded = renderSong(back, o);
  bool same2 = loaded.frames == after.frames;
  for (size_t i = 0; same2 && i < loaded.frames; ++i) same2 = loaded.at(0, i) == after.at(0, i) && loaded.at(1, i) == after.at(1, i);
  expect(same2, "the loaded song renders sample for sample the same" + tag);
}

}  // namespace

int main() {
  gDir = test::hermeticTestEnv("k349audiorack");
  testDetection();
  testUnused();
  testRecording();
  testRenderInPlace();
  testConsolidate();
  testStems();
  testOldProject();
  testStepsPlay(false);
  testStepsPlay(true);
  return finish();
}
