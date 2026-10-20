// Gridline — browser content model tests (J-16): kinds, smart search
// filters, tempo / key detection, the content indexer and its cache, user
// tags, preset trees, the drag & drop target table and the PNG codec used by
// plugin picker thumbnails.
#include <cmath>
#include <cstring>
#include <iterator>
#include <filesystem>
#include <fstream>
#include "Check.h"
#include "Hermetic.h"
#include "core/BrowserIndex.h"
#include "core/Png.h"
#include "core/WavWriter.h"

using namespace gl;
using namespace gl::browse;
namespace fs = std::filesystem;

namespace {

void writeTone(const std::string& path, double seconds, int rate = 44100, size_t channels = 1) {
  AudioBuffer b(channels, static_cast<size_t>(seconds * rate));
  for (size_t c = 0; c < channels; ++c)
    for (size_t i = 0; i < b.frames; ++i)
      b.plane(c)[i] = 0.5f * static_cast<float>(std::sin(2 * 3.14159265358979 * 220.0 * static_cast<double>(i) / rate)) * (c ? 0.5f : 1.0f);
  writeWav(path, b, rate);
}

void touch(const std::string& path, const std::string& text = "x") {
  fs::create_directories(fs::path(path).parent_path());
  std::ofstream(path) << text;
}

void testKinds() {
  expect(kindOf("/a/b/Kick.wav") == Kind::Audio, "kind: wav is audio");
  expect(kindOf("/a/b/Loop.WV") == Kind::Audio, "kind: WavPack is audio (case-insensitive)");
  expect(kindOf("/a/song.mid") == Kind::Midi && kindOf("/a/song.MIDI") == Kind::Midi, "kind: midi");
  expect(kindOf("/a/demo.flp") == Kind::Flp, "kind: flp");
  expect(kindOf("/a/Bass.fst") == Kind::Fst, "kind: fst");
  expect(kindOf("/a/My Song.gridline.json") == Kind::Project, "kind: gridline project");
  expect(kindOf("/cfg/presets/gl.synth/Lead.json") == Kind::Preset, "kind: library preset");
  expect(kindOf("/cfg/presets/chains/Vocal.json") == Kind::Chain, "kind: chain");
  expect(kindOf("/cfg/presets/automation/Rise.json") == Kind::Shape, "kind: automation shape");
  expect(kindOf("/a/notes.txt") == Kind::Other && kindOf("/a/data.json") == Kind::Other, "kind: other files");
  Kind k;
  expect(kindFromName("sample", k) && k == Kind::Audio && kindFromName("MIDI", k) && k == Kind::Midi && !kindFromName("zzz", k),
         "kindFromName aliases");
}

void testTempoKey() {
  expect(bpmFromName("Funk Drums 120bpm.wav") == 120, "bpm: 120bpm");
  expect(bpmFromName("Loop_95_BPM_Am.wav") == 95, "bpm: 95 BPM token");
  expect(bpmFromName("bpm128 groove.wav") == 128, "bpm: bpm128");
  expect(bpmFromName("Break loop 87.wav") == 87, "bpm: bare number in a loop name");
  expect(bpmFromName("Kick 808.wav") == 0 && bpmFromName("Snare 120.wav") == 0, "bpm: numbers in one-shot names are not tempos");
  int pc = -1;
  bool mn = false;
  expect(keyFromName("Bass_Am_120.wav", pc, mn) && pc == 9 && mn, "key: Am");
  expect(keyFromName("Pad F#m.wav", pc, mn) && pc == 6 && mn, "key: F#m");
  expect(keyFromName("Lead Bb.wav", pc, mn) && pc == 10 && !mn, "key: Bb");
  expect(keyFromName("Chords C minor.wav", pc, mn) && pc == 0 && mn, "key: C minor (two tokens)");
  expect(keyFromName("synth_ebmin.wav", pc, mn) && pc == 3 && mn, "key: ebmin");
  expect(!keyFromName("Amen Break.wav", pc, mn), "key: 'Amen' is not a key");
  expect(!keyFromName("Kick A.wav", pc, mn), "key: lone letter is not a key");
  expect(!keyFromName("Cmaj7chord.wav", pc, mn), "key: chord names with extensions are not keys");
  expect(keyName(9, true) == "A minor" && keyName(1, false) == "C# major", "keyName");
  int k, m;
  expect(parseKey("Am", k, m) && k == 9 && m == 1, "parseKey Am");
  expect(parseKey("c#", k, m) && k == 1 && m == -1, "parseKey c# (any mode)");
  expect(parseKey("Ebmaj", k, m) && k == 3 && m == 0, "parseKey Ebmaj");
  expect(parseKey("bb", k, m) && k == 10, "parseKey bb = B flat");
  expect(!parseKey("h", k, m) && !parseKey("Axe", k, m), "parseKey rejects nonsense");
  expect(std::fabs(bpmFromLength(2.0) - 120) < 1e-9, "bpm from length: 1 bar of 2 s = 120");
  expect(std::fabs(bpmFromLength(240.0 * 4 / 128.0) - 128) < 1e-9, "bpm from length: 4 bars at 128");
  expect(std::fabs(bpmFromLength(240.0 * 2 / 92.5) - 92.5) < 1e-9, "bpm from length: half BPM");
  expect(bpmFromLength(0.37) == 0 && bpmFromLength(1.2345) == 0, "bpm from length: one-shots get none");
  expect(std::fabs(syncRatio(120, 120) - 1) < 1e-12, "sync ratio: same tempo");
  expect(std::fabs(syncRatio(70, 140) - 1) < 1e-12, "sync ratio: folds octaves (70 vs 140)");
  expect(std::fabs(syncRatio(100, 120) - 1.2) < 1e-12, "sync ratio: 100 -> 120 plays 1.2x");
  expect(syncRatio(0, 120) == 1 && syncRatio(120, 0) == 1, "sync ratio: unknown tempo plays 1x");
  auto t = autoTags("/Packs/Drums/Kicks/Hard kick 03.wav");
  expect(std::find(t.begin(), t.end(), "kick") != t.end() && std::find(t.begin(), t.end(), "drums") != t.end(), "autoTags from name and folders");
  auto t2 = autoTags("/x/Vox_Chop_Loop.wav");
  expect(std::find(t2.begin(), t2.end(), "vocal") != t2.end() && std::find(t2.begin(), t2.end(), "loop") != t2.end(), "autoTags vox/loop");
  auto t3 = autoTags("/x/subtle.wav");
  expect(std::find(t3.begin(), t3.end(), "bass") == t3.end(), "autoTags: short keywords match whole tokens only");
}

void testPeaks() {
  std::vector<float> p = {0, 0.5f, 1.0f, 2.0f, -1};
  std::string s = encodePeaks(p);
  auto d = decodePeaks(s);
  expect(s.size() == 5 && d.size() == 5 && d[0] == 0 && std::fabs(d[1] - 0.5f) < 0.02f && d[2] == 1 && d[3] == 1 && d[4] == 0,
         "peaks encode/decode (clamped)");
  std::vector<float> l(1000, 0.f), r(1000, 0.f);
  l[10] = 0.8f;
  r[990] = -0.6f;
  auto c = computePeaks(l, r, 10);
  expect(c.size() == 10 && std::fabs(c[0] - 0.8f) < 1e-6f && std::fabs(c[9] - 0.6f) < 1e-6f && c[5] == 0, "computePeaks buckets");
}

void testQuery() {
  Query q = parseQuery("kick type:audio tag:808 bpm:120-130 key:Am");
  expect(q.words.size() == 1 && q.words[0] == "kick", "query: free word");
  expect(q.kinds.count(Kind::Audio) == 1 && q.kinds.size() == 1, "query: type filter");
  expect(q.tags.size() == 1 && q.tags[0] == "808", "query: tag filter");
  expect(q.bpmLo == 120 && q.bpmHi == 130, "query: bpm range");
  expect(q.key == 9 && q.minor == 1, "query: key filter");
  Query q2 = parseQuery("bpm:128 len:<2 type:preset #warm \"deep bass\"");
  expect(q2.bpmLo == 127 && q2.bpmHi == 129, "query: single bpm is +-1");
  expect(q2.lenLo == 0 && q2.lenHi == 2, "query: len upper bound");
  expect(q2.kinds.count(Kind::Preset) && q2.kinds.count(Kind::Fst), "query: type:preset includes FL presets");
  expect(q2.tags.size() == 1 && q2.tags[0] == "warm", "query: #tag shorthand");
  expect(q2.words.size() == 1 && q2.words[0] == "deep bass", "query: quoted phrase");
  Query q3 = parseQuery("type:effect,instrument reverb");
  expect(q3.pluginKinds.size() == 2 && q3.kinds.empty(), "query: plugin kinds");
  expect(parseQuery("").empty() && !parseQuery("bpm:>100").empty(), "query: empty");
  Query q4 = parseQuery("bpm:>100");
  expect(q4.bpmLo == 100 && q4.bpmHi > 1000, "query: bpm lower bound");

  FileMeta m;
  m.path = "/Packs/Drums/Kicks/Big Kick 808 Am 125bpm.wav";
  m.kind = Kind::Audio;
  m.bpm = 125;
  m.key = 9;
  m.minor = true;
  m.seconds = 1.5;
  expect(matches(m, q), "match: all filters");
  expect(matches(m, parseQuery("drums kick")), "match: words in folders");
  expect(matches(m, parseQuery("tag:kick")), "match: automatic tag");
  expect(!matches(m, parseQuery("tag:snare")), "no match: other tag");
  expect(!matches(m, parseQuery("bpm:90-100")), "no match: bpm outside");
  expect(!matches(m, parseQuery("key:C")), "no match: other key");
  expect(matches(m, parseQuery("key:a")), "match: key without mode");
  expect(matches(m, parseQuery("len:<2")) && !matches(m, parseQuery("len:>2")), "match: length");
  expect(!matches(m, parseQuery("type:midi")), "no match: type");
  expect(!matches(m, parseQuery("type:plugin")), "no match: files are not plugins");
  m.tags = {"favourite-kick"};
  expect(matches(m, parseQuery("tag:favourite")), "match: user tag prefix");
  FileMeta unknown;
  unknown.path = "/x/Pad.wav";
  unknown.kind = Kind::Audio;
  expect(!matches(unknown, parseQuery("bpm:120")) && !matches(unknown, parseQuery("key:Am")), "files without tempo/key fail those filters");

  PluginInfo p{"gl.reverbx", "Hall Reverb", "Delay & reverb", PluginKind::Effect, {}, false, {}};
  expect(matchesPlugin(p, parseQuery("reverb"), false), "plugin: word");
  expect(matchesPlugin(p, parseQuery("type:effect"), false) && !matchesPlugin(p, parseQuery("type:instrument"), false), "plugin: kind");
  expect(matchesPlugin(p, parseQuery("tag:delay"), false), "plugin: category tag");
  expect(!matchesPlugin(p, parseQuery("tag:fav"), false) && matchesPlugin(p, parseQuery("tag:fav"), true), "plugin: favourite tag");
  expect(!matchesPlugin(p, parseQuery("reverb bpm:120")), "plugin: file filters exclude plugins");
}

void testPresetTree() {
  std::vector<PresetEntry> e = {
      {"Zeta", "", "/z"},
      {"Alpha", "", "/a"},
      {"Hall", "FL Studio/Reverb/Large", "/h"},
      {"Room", "FL Studio/Reverb", "/r"},
      {"Pad", "Mine", "/p"},
  };
  PresetNode t = buildPresetTree(e);
  expect(t.presets.size() == 2 && e[t.presets[0]].name == "Alpha", "tree: top-level presets sorted");
  expect(t.folders.size() == 2 && t.folders[0].name == "FL Studio" && t.folders[1].name == "Mine", "tree: folders sorted");
  const PresetNode& fl = t.folders[0];
  expect(fl.folders.size() == 1 && fl.folders[0].name == "Reverb" && fl.folders[0].presets.size() == 1 &&
             fl.folders[0].folders.size() == 1 && fl.folders[0].folders[0].name == "Large",
         "tree: nested folders");
  expect(t.count() == 5 && fl.count() == 2, "tree: counts");
  const std::string root = gl::test::hermeticTestEnv("browser") + "/presets";
  touch(root + "/gl.synth/A.json");
  touch(root + "/chains/C.json");
  touch(root + "/automation/S.json");
  touch(root + "/gl.delay/B.json");
  auto types = libraryTypes(root);
  expect(types.size() == 2 && types[0] == "gl.delay" && types[1] == "gl.synth", "libraryTypes skips chains/automation");
}

void testDrop() {
  DropItem audio{Kind::Audio, "", PluginKind::Generator};
  expect(dropAction(audio, DropTarget::Rack) == DropAction::NewSamplerChannel, "drop audio -> rack: sampler");
  expect(dropAction(audio, DropTarget::Playlist) == DropAction::AudioClip, "drop audio -> playlist: audio clip");
  expect(dropAction(audio, DropTarget::MixerSlot) == DropAction::ConvolverIR, "drop audio -> mixer slot: convolver IR");
  expect(dropAction(audio, DropTarget::MixerTrack) == DropAction::ConvolverIR, "drop audio -> mixer strip: convolver IR");
  expect(dropAction(audio, DropTarget::PluginWindow, "gl.sampler", true) == DropAction::LoadSample, "drop audio -> sampler window");
  expect(dropAction(audio, DropTarget::PluginWindow, "gl.synth", false) == DropAction::None, "drop audio -> synth window: nothing");
  expect(dropAction(audio, DropTarget::PianoRoll) == DropAction::None, "drop audio -> piano roll: nothing");
  DropItem midi{Kind::Midi, "", PluginKind::Generator};
  expect(dropAction(midi, DropTarget::PianoRoll) == DropAction::ImportMidiNotes, "drop midi -> piano roll: notes");
  expect(dropAction(midi, DropTarget::Rack) == DropAction::ImportMidiChannels, "drop midi -> rack: channels");
  expect(dropAction(midi, DropTarget::MixerTrack) == DropAction::None, "drop midi -> mixer: nothing");
  DropItem gen{Kind::Preset, "gl.synth", PluginKind::Generator};
  expect(dropAction(gen, DropTarget::PluginWindow, "gl.synth") == DropAction::ApplyPreset, "drop preset -> same plugin window");
  expect(dropAction(gen, DropTarget::PluginWindow, "gl.fm") == DropAction::None, "drop preset -> other plugin window: nothing");
  expect(dropAction(gen, DropTarget::Rack) == DropAction::NewChannelFromPreset, "drop channel preset -> rack");
  expect(dropAction(gen, DropTarget::MixerSlot) == DropAction::None, "drop generator preset -> mixer: nothing");
  DropItem fx{Kind::Preset, "gl.reverb", PluginKind::Effect};
  expect(dropAction(fx, DropTarget::MixerSlot) == DropAction::InsertEffectPreset, "drop effect preset -> slot");
  expect(dropAction(fx, DropTarget::Rack) == DropAction::None, "drop effect preset -> rack: nothing");
  DropItem chain{Kind::Chain, "", PluginKind::Effect};
  expect(dropAction(chain, DropTarget::MixerTrack) == DropAction::ApplyChain, "drop chain -> mixer track");
  expect(dropAction(chain, DropTarget::Playlist) == DropAction::None, "drop chain -> playlist: nothing");
  expect(dropAction(DropItem{Kind::Flp, "", PluginKind::Generator}, DropTarget::Workspace) == DropAction::OpenProject, "drop flp -> open (J-52)");
  expect(dropAction(DropItem{Kind::Project, "", PluginKind::Generator}, DropTarget::Playlist) == DropAction::OpenProject, "drop project -> open");
  expect(dropAction(DropItem{Kind::Score, "", PluginKind::Generator}, DropTarget::Rack) == DropAction::None, "drop score: nothing");
  expect(std::string(dropActionLabel(DropAction::AudioClip)) == "Audio clip", "drop labels");
}

void testIndex() {
  const std::string base = gl::test::hermeticTestEnv("browser");
  const std::string root = base + "/lib";
  fs::create_directories(root + "/Drums/Kicks");
  fs::create_directories(root + "/Loops");
  fs::create_directories(root + "/.hidden");
  writeTone(root + "/Drums/Kicks/Kick Am.wav", 0.25);
  writeTone(root + "/Loops/Groove 2bars.wav", 4.0, 44100, 2);  // 2 bars at 120 (loop folder: tempo from length)
  writeTone(root + "/Loops/Funk 100bpm.wav", 1.0);
  writeTone(root + "/.hidden/Secret.wav", 0.1);
  touch(root + "/song.mid", "MThd");
  touch(root + "/readme.txt");
  const std::string cache = base + "/config/browser-index.json";
  {
    Index ix;
    ix.setCacheFile(cache);
    ix.setRoots({{root, true}});
    ix.start();
    ix.waitIdle();
    expect(ix.size() == 4, "index: 4 listable files (hidden and other skipped)");
    FileMeta m;
    expect(ix.lookup(root + "/Loops/Groove 2bars.wav", m) && m.analysed && std::fabs(m.seconds - 4.0) < 0.01 && m.channels == 2 &&
               std::fabs(m.bpm - 120) < 1e-9 && m.peaks.size() == 64,
           "index: loop analysed (length, stereo, tempo from length, peaks)");
    expect(ix.lookup(root + "/Drums/Kicks/Kick Am.wav", m) && m.key == 9 && m.minor && m.bpm == 0 && m.channels == 1,
           "index: one-shot (key from name, no tempo, mono)");
    expect(ix.lookup(root + "/Loops/Funk 100bpm.wav", m) && m.bpm == 100, "index: tempo from name");
    expect(ix.lookup(root + "/song.mid", m) && m.kind == Kind::Midi && !m.analysed, "index: midi listed, not analysed");
    auto r = ix.search(parseQuery("type:audio bpm:95-125"));
    expect(r.size() == 2 && fs::path(r[0].path).filename() == "Funk 100bpm.wav", "index search: bpm filter, sorted by name");
    expect(ix.search(parseQuery("kick key:Am")).size() == 1, "index search: words + key");
    expect(ix.search(parseQuery("tag:loop")).size() == 2, "index search: folder tag");
    ix.setTags(root + "/song.mid", {"Idea", "idea", " Chords "});
    expect(ix.tagsOf(root + "/song.mid") == std::vector<std::string>({"chords", "idea"}), "tags normalised (lower, unique, sorted)");
    expect(ix.search(parseQuery("tag:idea")).size() == 1, "index search: user tag");
    expect(ix.allTags().size() == 2, "allTags");
    expect(fs::exists(cache), "index cache written");
  }
  {
    Index ix;
    ix.setCacheFile(cache);
    expect(ix.load(), "index cache loads");
    FileMeta m;
    expect(ix.lookup(root + "/Loops/Groove 2bars.wav", m) && m.analysed && std::fabs(m.bpm - 120) < 1e-9, "cache keeps analysis");
    expect(ix.tagsOf(root + "/song.mid").size() == 2, "cache keeps tags");
    // Refresh: a removed file disappears, a new one appears, unchanged ones are not re-analysed.
    fs::remove(root + "/Loops/Funk 100bpm.wav");
    writeTone(root + "/Loops/New 90bpm loop.wav", 0.5);
    ix.setRoots({{root, true}});
    ix.start();
    ix.waitIdle();
    expect(!ix.lookup(root + "/Loops/Funk 100bpm.wav", m), "refresh: removed file dropped");
    expect(ix.lookup(root + "/Loops/New 90bpm loop.wav", m) && m.analysed && m.bpm == 90, "refresh: new file analysed");
    expect(ix.done() == 1, "refresh: only the new file was decoded");
    expect(ix.lookup(root + "/song.mid", m) && m.tags.size() == 2, "refresh keeps tags on entries");
  }
}

void testAcid() {
  const std::string dir = gl::test::hermeticTestEnv("browser") + "/acid";
  fs::create_directories(dir);
  const std::string plain = dir + "/plain.wav";
  writeTone(plain, 0.1);
  expect(wavAcidTempo(plain) == 0, "acid: none in a plain WAV");
  // Append an acid chunk (tempo 62.5 -> folded to 125) and fix the RIFF size.
  std::string bytes;
  {
    std::ifstream in(plain, std::ios::binary);
    bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }
  std::string acid = "acid";
  auto put32 = [](std::string& s, uint32_t v) {
    for (int i = 0; i < 4; ++i) s += static_cast<char>((v >> (8 * i)) & 0xFF);
  };
  put32(acid, 24);
  acid.append(20, '\0');
  float t = 62.5f;
  uint32_t bits;
  std::memcpy(&bits, &t, 4);
  put32(acid, bits);
  bytes += acid;
  std::string riff;
  put32(riff, static_cast<uint32_t>(bytes.size() - 8));
  bytes.replace(4, 4, riff);
  const std::string tagged = dir + "/Groove.wav";
  std::ofstream(tagged, std::ios::binary) << bytes;
  expect(std::fabs(wavAcidTempo(tagged) - 125) < 1e-9, "acid: tempo read and folded into 70..180");
  FileMeta m = Index::describe(tagged, true);
  expect(m.analysed && std::fabs(m.bpm - 125) < 1e-9, "describe uses the acid tempo");
  expect(wavAcidTempo(dir + "/missing.wav") == 0, "acid: missing file");
}

void testPng() {
  png::Image img;
  img.width = 37;
  img.height = 21;
  img.channels = 3;
  for (int y = 0; y < img.height; ++y)
    for (int x = 0; x < img.width; ++x) {
      img.pixels.push_back(static_cast<uint8_t>(x * 7));
      img.pixels.push_back(static_cast<uint8_t>(y * 11));
      img.pixels.push_back(static_cast<uint8_t>(x < 18 ? 40 : 200));
    }
  auto bytes = png::encode(img);
  png::Image back;
  expect(!bytes.empty() && png::decode(bytes.data(), bytes.size(), back), "png encodes and decodes");
  expect(back.width == 37 && back.height == 21 && back.channels == 3 && back.pixels == img.pixels, "png round trip is lossless");
  png::Image flat;
  flat.width = 200;
  flat.height = 120;
  flat.channels = 4;
  flat.pixels.assign(200 * 120 * 4, 90);
  auto fb = png::encode(flat);
  expect(fb.size() < 2000, "png compresses flat areas");
  png::Image fb2;
  expect(png::decode(fb.data(), fb.size(), fb2) && fb2.pixels == flat.pixels, "png RGBA round trip");
  auto small = png::resize(img, 10, 5);
  expect(small.width == 10 && small.height == 5 && small.pixels.size() == 150, "png resize");
  expect(!png::decode(bytes.data(), 20, back), "png rejects truncated data");
}

}  // namespace

int main() {
  gl::test::hermeticTestEnv("browser");
  testKinds();
  testTempoKey();
  testPeaks();
  testQuery();
  testPresetTree();
  testDrop();
  testIndex();
  testAcid();
  testPng();
  return finish();
}
