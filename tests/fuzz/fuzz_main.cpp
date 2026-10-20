// Gridline — mutation fuzzer for the file readers (J-21 QA).
//
// A small libFuzzer-style driver that needs no libFuzzer: it loads a corpus
// (files or directories), mutates inputs (bit flips, interesting integers,
// chunk insert / delete / duplicate, splices, truncation) and feeds each one
// to a reader. Build it with -fsanitize=address,undefined so every memory or
// UB error aborts with a report; the input being processed is always on disk
// at <work>/input<ext>, so a crash leaves its reproducer behind. A watchdog
// thread copies a slow input to <work>/hang-*.bin and exits with code 3.
//
//   gridline_fuzz <target> <seconds> [--work DIR] [--seed N] [--max-len N] [--repro FILE] <corpus file|dir>...
//   targets: flp fst project sample midi browser-index zip lua (J-36: zip, lua, project seeds with J-31 fields)
//            song-flp (J-46: mutated Gridline songs → FLP export with / without origin → re-import)
//            fx-preset (J-46: mutated FL effect presets → decoder → DSP blocks → FLP export → re-import)
//
// With no corpus the target's built-in seeds are used (ctest smoke run).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>
#include "core/BrowserIndex.h"
#include "core/Document.h"
#include "core/Grouping.h"
#include "core/PianoRollScript.h"
#include "core/SongTools.h"
#include "core/Zip.h"
#include "core/Demo.h"
#include "core/Json.h"
#include "core/MidiFile.h"
#include "core/PluginInfo.h"
#include "core/Project.h"
#include "core/WavWriter.h"
#include "core/flp/Flp.h"
#include "core/flp/FlpExport.h"
#include "core/flp/FlpPresets.h"
#include "engine/Effects.h"
#include "engine/Sample.h"
#include "packs/Packs.h"

namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;

namespace {

Bytes readAll(const std::string& p) {
  std::ifstream in(p, std::ios::binary);
  return Bytes(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}
void writeAll(const std::string& p, const Bytes& b) {
  std::ofstream o(p, std::ios::binary | std::ios::trunc);
  o.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
}

// ---------------------------------------------------------------- seeds
void put16(Bytes& b, uint32_t v) { b.push_back(v & 0xFF), b.push_back((v >> 8) & 0xFF); }
void put32(Bytes& b, uint32_t v) { put16(b, v & 0xFFFF), put16(b, v >> 16); }
void putVar(Bytes& b, size_t n) {
  do {
    uint8_t x = n & 0x7F;
    n >>= 7;
    b.push_back(static_cast<uint8_t>(x | (n ? 0x80 : 0)));
  } while (n);
}

Bytes seedFlp() {
  // FLhd + FLdt with a handful of byte / word / dword / text / data events.
  Bytes d;
  d.push_back(199), putVar(d, 6), d.insert(d.end(), {'2', '0', '.', '8', 0, 0});  // version
  d.push_back(28), d.push_back(1);                                                // registered
  d.push_back(66), put16(d, 140);                                                 // tempo (word)
  d.push_back(156), put32(d, 140000);                                             // tempo (dword)
  d.push_back(64), put16(d, 0);                                                   // new channel
  d.push_back(21), d.push_back(0);                                                // channel type: sampler
  d.push_back(201), putVar(d, 8), d.insert(d.end(), {'S', 0, 'a', 0, 'm', 0, 0, 0});  // plugin name (utf16)
  d.push_back(65), put16(d, 1);                                                   // new pattern
  d.push_back(224), putVar(d, 24);                                                // pattern notes
  for (int i = 0; i < 24; ++i) d.push_back(static_cast<uint8_t>(i * 7));
  d.push_back(233), putVar(d, 32);                                                // playlist items
  for (int i = 0; i < 32; ++i) d.push_back(static_cast<uint8_t>(i));
  Bytes f{'F', 'L', 'h', 'd'};
  put32(f, 6), put16(f, 0), put16(f, 1), put16(f, 96);
  f.insert(f.end(), {'F', 'L', 'd', 't'});
  put32(f, static_cast<uint32_t>(d.size()));
  f.insert(f.end(), d.begin(), d.end());
  return f;
}

Bytes seedProject() {
  std::string t = gl::projectToText(gl::makeDemoSong());
  return Bytes(t.begin(), t.end());
}

// J-36: a project using the J-31 fields (pattern / clip / note groups, step
// loops, key ranges, key markers, recording point, solo-alone).
Bytes seedProjectGroups() {
  gl::Document d(gl::makeDemoSong());
  const gl::Song& s = d.song();
  std::vector<gl::ObjectId> pats;
  for (const auto& p : s.patterns) pats.push_back(p.id);
  gl::ops::groupPatterns(d, pats, "group a");
  std::set<gl::ObjectId> clips;
  for (const auto& c : s.playlist) clips.insert(c.id);
  gl::ops::groupClips(d, clips);
  std::vector<gl::ObjectId> chans;
  for (const auto& c : s.channels) chans.push_back(c.id);
  if (!pats.empty()) gl::ops::setStepLoop(d, pats[0], chans, 6);
  if (!chans.empty()) gl::ops::setKeyRange(d, chans[0], 36, 72);
  gl::ops::addKeyMarker(d, s.ppq * 8, 9, 2);
  if (s.mixer.size() > 1)
    d.editMixerTrack(s.mixer[1].id, "qa", [](gl::MixerTrack& m) { m.recPoint = 2, m.soloAlone = true; });
  if (!pats.empty() && !chans.empty()) {
    std::vector<gl::Note> notes(3);
    for (size_t i = 0; i < notes.size(); ++i) notes[i].start = static_cast<gl::Tick>(i) * 48, notes[i].group = 1 + static_cast<int>(i % 2);
    d.setNotes(pats[0], chans[0], notes);
  }
  std::string t = gl::projectToText(d.song());
  return Bytes(t.begin(), t.end());
}

// J-36: ZIP archives (stored + fixed-Huffman + dynamic-Huffman entries).
void zipEntry(Bytes& file, Bytes& cd, const std::string& name, uint16_t method, const Bytes& raw, const Bytes& packed) {
  const uint32_t crc = gl::crc32(raw.data(), raw.size());
  const uint32_t off = static_cast<uint32_t>(file.size());
  put32(file, 0x04034b50), put16(file, 20), put16(file, 0), put16(file, method), put32(file, 0);
  put32(file, crc), put32(file, static_cast<uint32_t>(packed.size())), put32(file, static_cast<uint32_t>(raw.size()));
  put16(file, static_cast<uint32_t>(name.size())), put16(file, 0);
  file.insert(file.end(), name.begin(), name.end());
  file.insert(file.end(), packed.begin(), packed.end());
  put32(cd, 0x02014b50), put16(cd, 20), put16(cd, 20), put16(cd, 0), put16(cd, method), put32(cd, 0);
  put32(cd, crc), put32(cd, static_cast<uint32_t>(packed.size())), put32(cd, static_cast<uint32_t>(raw.size()));
  put16(cd, static_cast<uint32_t>(name.size())), put16(cd, 0), put16(cd, 0), put16(cd, 0), put16(cd, 0), put32(cd, 0), put32(cd, off);
  cd.insert(cd.end(), name.begin(), name.end());
}

Bytes seedZip() {
  const std::string a = "hello hello hello hello";
  std::string t;
  for (int i = 0; i < 20; ++i) t += "the quick brown fox jumps over the lazy dog " + std::to_string(i) + "\n";
  const Bytes fixedPacked{0xcb, 0x48, 0xcd, 0xc9, 0xc9, 0x57, 0xc8, 0x40, 0x27, 0x01};
  const Bytes dynPacked{0x95, 0xd1, 0xcb, 0x01, 0x82, 0x30, 0x14, 0x05, 0xd1, 0x3d, 0x55, 0xbc, 0x12, 0xbc, 0x22, 0x6a, 0xca, 0xe1,
                        0x13, 0x05, 0x3f, 0x04, 0x43, 0x22, 0x60, 0xf5, 0xd2, 0xc2, 0xac, 0x67, 0x76, 0x27, 0xf5, 0xde, 0x3e, 0x79,
                        0x68, 0x9f, 0xd6, 0xc4, 0xb0, 0x8c, 0x76, 0x0b, 0xab, 0x3d, 0xf2, 0x7b, 0x9a, 0x2d, 0x7c, 0x7d, 0xb4, 0xb4,
                        0xe7, 0x57, 0xfd, 0xdb, 0xac, 0x0b, 0x77, 0x3b, 0x14, 0x09, 0xdc, 0x42, 0xf7, 0x11, 0xdd, 0x25, 0xba, 0x4f,
                        0xe8, 0xae, 0xd0, 0x7d, 0x46, 0xf7, 0x05, 0xdd, 0x57, 0x74, 0x3b, 0xa6, 0x03, 0x31, 0x99, 0xa6, 0x18, 0xa7,
                        0x98, 0xa7, 0x18, 0xa8, 0x98, 0xa8, 0x18, 0xa9, 0x98, 0xa9, 0x18, 0xaa, 0x5c, 0xf1, 0x07};
  Bytes file, cd;
  const Bytes rawA(a.begin(), a.end()), rawT(t.begin(), t.end());
  zipEntry(file, cd, "song/project.json", 0, rawA, rawA);
  zipEntry(file, cd, "song/fixed.txt", 8, rawA, fixedPacked);
  zipEntry(file, cd, "song/samples/dyn.txt", 8, rawT, dynPacked);
  zipEntry(file, cd, "song/samples/", 0, {}, {});
  const uint32_t cdStart = static_cast<uint32_t>(file.size());
  file.insert(file.end(), cd.begin(), cd.end());
  put32(file, 0x06054b50), put16(file, 0), put16(file, 0), put16(file, 4), put16(file, 4);
  put32(file, static_cast<uint32_t>(cd.size())), put32(file, cdStart), put16(file, 0);
  return file;
}

// J-36: piano roll scripts (the run input is the script source).
std::vector<Bytes> seedLua() {
  const char* src[] = {
      "-- name: qa\nfunction form(f) f:knobInt('Hits', 5, 1, 32) f:combo('Grid', {'a', 'b'}, 1) f:check('On', true) f:text('T', 'x') end\n"
      "function apply(score, p) for i, n in score:notes() do n.velocity = (n.velocity + p['Hits']) % 128 end\n"
      "  score:add{time = 0, length = score.ppq, pitch = score:snap(61)} score:addMarker{time = 96, name = 'm'} score:sort() end\n",
      "local s = string.rep('ab', 100) for k in s:gmatch('(a)(b)') do end print(s:find('b+a', 1), ('x'):rep(3, ','), #s)\n"
      "local t = {} for i = 1, 100 do t[i] = {time = i * 24, pitch = 40 + i % 60} end score._notes = t\n",
      "local co = coroutine.wrap(function() for i = 1, 10 do coroutine.yield(i) end end) for i = 1, 10 do co() end\n"
      "print(pcall(error, {})) print(select('#', table.unpack({1, 2, 3}))) print(utf8.char(72, 228), math.tointeger(3.0))\n"
      "setmetatable(score, {__index = function(t, k) return rawget(t, '_' .. k) end})\n",
  };
  std::vector<Bytes> v;
  for (const char* x : src) v.emplace_back(x, x + std::strlen(x));
  return v;
}

Bytes seedWav(int bits) {
  gl::AudioBuffer buf(2, 256);
  for (size_t i = 0; i < buf.data.size(); ++i) buf.data[i] = static_cast<float>(std::sin(static_cast<double>(i) * 0.1) * 0.5);
  std::string p = (fs::temp_directory_path() / ("gl-fuzz-seed-" + std::to_string(getpid()) + ".wav")).string();
  gl::writeWav(p, buf, 44100, bits);
  Bytes b = readAll(p);
  fs::remove(p);
  return b;
}

Bytes seedMidi() {
  gl::MidiFileData m;
  m.tracks.resize(2);
  for (int i = 0; i < 8; ++i) m.tracks[static_cast<size_t>(i % 2)].notes.push_back({i * 48, 40, 60 + i, 100, i % 2});
  std::string p = (fs::temp_directory_path() / ("gl-fuzz-seed-" + std::to_string(getpid()) + ".mid")).string();
  gl::writeMidiFile(p, m);
  Bytes b = readAll(p);
  fs::remove(p);
  return b;
}

Bytes seedBrowserIndex() {
  std::string t =
      R"({"format":"gridline-browser-index","version":9,"files":{"/x/a.wav":{"s":100,"m":5,"sec":1.5,"rate":44100,"ch":2,"bpm":120,"key":3,"peaks":"AAEC"},)"
      R"("/x/b.flac":{"s":-1,"m":1e300,"sec":-2,"rate":1e40,"ch":3e12,"bpm":-5,"key":99}},"tags":{"/x/a.wav":["kick","loop"],"/y/c.mid":["x"]}})";
  return Bytes(t.begin(), t.end());
}


// J-46: a song with the J-37…J-44 arrangement features: overlapping audio
// clips (automatic crossfades), hand-set fades with shapes / tensions /
// declickers, clip groups, markers with actions, FL icons, insert lock /
// separator, and the J-42 effects in mixer slots.
Bytes seedSongFlp(bool variant) {  // variant: automatic crossfades off, kick channels
  gl::Document d(gl::makeDemoSong());
  const gl::ObjectId ch = d.addChannel("loop", "gl.sampler");
  d.editSong("qa", [&](gl::Song& x) {
    for (auto& c : x.channels)
      if (c.id == ch) c.plugin.data = "sample=/nonexistent/qa-loop.wav\n";
    const gl::Tick bar = x.barTicks();
    for (int i = 0; i < 4; ++i) {
      gl::Clip k;
      k.id = x.allocateId();
      k.kind = gl::ClipKind::Audio;
      k.source = ch;
      k.track = 9 + i / 3;
      k.start = bar * i - (i ? bar / 4 : 0);
      k.length = bar + bar / 2;
      k.audioEnd = 2.0;
      if (i == 1) k.fadeIn = 0.25, k.fadeInManual = true, k.fadeInShape = 1, k.fadeInTension = -3.5f, k.fadeInDeclick = 2;
      if (i == 2) k.fadeOut = 0.1, k.fadeOutManual = true, k.fadeOutTension = 40.f, k.fadeOutDeclick = 1;
      if (i == 3) k.fadeOutManual = true;  // "no fade" by hand
      while (static_cast<int>(x.tracks.size()) <= k.track) x.tracks.push_back(gl::PlaylistTrack{});
      x.playlist.push_back(k);
    }
    if (x.tracks.size() > 2) x.tracks[1].flIcon = 37, x.tracks[2].locked = true;
    if (!x.channels.empty()) x.channels[0].flIcon = 12;
    for (size_t m = 1; m < x.mixer.size() && m < 4; ++m) x.mixer[m].separator = m == 2, x.mixer[m].locked = m == 3, x.mixer[m].flIcon = static_cast<int>(m) * 5;
    gl::Marker mk;
    mk.pos = bar * 2, mk.name = "loop here", mk.action = static_cast<gl::MarkerAction>(1);
    x.markers.push_back(mk);
    mk.pos = bar * 4, mk.name = "sig", mk.timeSigNum = 3, mk.timeSigDen = 4, mk.action = gl::MarkerAction::None;
    x.markers.push_back(mk);
    x.autoCrossfades = !variant;
    // Light plugins (the encoders of the demo's synths take seconds per
    // export) plus the J-42 effects; the variant has kick channels.
    for (auto& c : x.channels)
      if (c.id != ch) c.plugin = gl::defaultPluginState(variant ? "gl.kick" : "gl.sampler");
    for (auto& m : x.mixer)
      for (auto& sl : m.slots) sl = gl::EffectSlot{};
    const std::vector<std::string> fx = {"gl.pocketrack", "gl.timegate", "gl.octafilter", "gl.convolver", "gl.amprack", "gl.splitter"};  // (no band delay: its curve fit takes ~1 s per export)
    for (size_t k = 0; k < fx.size() && 1 + k / 2 < x.mixer.size(); ++k) x.mixer[1 + k / 2].slots[k % 2].plugin = gl::defaultPluginState(fx[k]);
  });
  std::set<gl::ObjectId> clips;
  for (const auto& c : d.song().playlist)
    if (c.kind == gl::ClipKind::Audio) clips.insert(c.id);
  gl::ops::groupClips(d, clips);
  std::string t = gl::projectToText(d.song());
  return Bytes(t.begin(), t.end());
}

// J-46: the FL presets of the J-42 effects (Pocket Rack, Gross Beat, Love
// Philter) when FL Studio is installed; the bare seed otherwise.
std::vector<Bytes> seedFxPresets() {
  std::vector<Bytes> v;
  const std::string root = "/Applications/FL Studio 2026.app/Contents/Resources/FL/Data/Patches/Plugin presets/Effects/";
  for (const char* dir : {"FL Studio Mobile Rack FX", "Gross Beat", "Fruity Love Philter"}) {
    std::error_code ec;
    std::vector<std::string> files;
    for (auto it = fs::recursive_directory_iterator(root + dir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
      if (it->path().extension() == ".fst") files.push_back(it->path().string());
    std::sort(files.begin(), files.end());
    for (const auto& f : files) v.push_back(readAll(f));
  }
  if (v.empty()) v.push_back(seedFlp());
  return v;
}

void exportReimport(const gl::Song& s) {
  for (bool origin : {true, false}) {
    gl::flp::ExportOptions o;
    o.useOrigin = origin;
    o.factoryData = "/nonexistent-fl-data";
    gl::flp::ExportReport rep;
    gl::flp::Bytes out;
    if (!gl::flp::exportFlp(s, out, rep, o)) continue;
    gl::Song back;
    gl::flp::Report r2;
    std::string err;
    if (!gl::importFlpBytes(out, back, r2, &err, "/nonexistent-fl-data", "", false)) {
      std::fprintf(stderr, "re-import of our own export failed: %s\n", err.c_str());
      std::abort();  // our own output must read back
    }
  }
}

// ---------------------------------------------------------------- targets
struct Target {
  const char* name;
  const char* ext;
  std::function<void(const std::string& path)> run;
  std::function<std::vector<Bytes>()> seeds;
};

std::string gWork;

std::vector<Target> targets() {
  return {
      {"flp", ".flp",
       [](const std::string& p) {
         gl::Song s;
         gl::flp::Report r;
         std::string err;
         if (gl::importFlp(p, s, r, &err, "/nonexistent-fl-data")) (void)gl::projectToText(s);
       },
       [] { return std::vector<Bytes>{seedFlp()}; }},
      {"flp-export", ".flp",  // J-34: import → FLP export (with / without the origin) → re-import
       [](const std::string& p) {
         gl::Song s;
         gl::flp::Report r;
         std::string err;
         if (!gl::importFlp(p, s, r, &err, "/nonexistent-fl-data")) return;
         for (bool origin : {true, false}) {
           gl::flp::ExportOptions o;
           o.useOrigin = origin;
           o.factoryData = "/nonexistent-fl-data";
           gl::flp::ExportReport rep;
           gl::flp::Bytes out;
           if (!gl::flp::exportFlp(s, out, rep, o)) continue;
           gl::Song back;
           gl::flp::Report r2;
           if (!gl::importFlpBytes(out, back, r2, &err, "/nonexistent-fl-data", "", false)) std::abort();  // our own output must read back
         }
       },
       [] { return std::vector<Bytes>{seedFlp()}; }},
      {"song-flp", ".json",  // J-46: mutated songs → FLP export (with / without the origin) → re-import
       [](const std::string& p) {
         gl::Song s;
         std::string err;
         if (gl::loadProject(p, s, &err)) exportReimport(s);
       },
       [] { return std::vector<Bytes>{seedSongFlp(false), seedSongFlp(true)}; }},  // (the every-effect seed is slow: ~10 s per export)
      {"fx-preset", ".fst",  // J-46: mutated FL effect presets → decoder → effect DSP → FLP export → re-import
       [](const std::string& p) {
         gl::flp::PresetResult r;
         if (!gl::flp::importFlPreset(p, r, "/nonexistent-fl-data") || r.kind != gl::flp::PresetResult::Plugin) return;
         gl::PluginState st = r.state;
         gl::normalizePluginState(st);
         const gl::PluginInfo* info = gl::findPlugin(st.type);
         if (!info) return;
         if (info->kind == gl::PluginKind::Effect) {
           if (auto fx = gl::createEffect(st.type)) {
             fx->prepareHost(st, 44100.0);
             gl::ChannelAux aux;
             (void)gl::packs::prepareAux(st, aux);
             (void)fx->latencyFor(st, 44100.0);
             gl::ProcessContext ctx;
             ctx.sampleRate = 44100.0, ctx.playing = true;
             std::vector<float> L(512), R(512);
             uint32_t x = 12345;
             for (int b = 0; b < 6; ++b) {
               for (size_t i = 0; i < L.size(); ++i) {
                 x = x * 1664525u + 1013904223u;
                 L[i] = static_cast<float>(x >> 8) / 16777216.0f - 0.5f, R[i] = -L[i];
               }
               fx->processAux(st, aux, ctx, L.data(), R.data(), b == 3 ? 37 : 512);
               ctx.tick += 48;
             }
           }
         }
         // A light song (cheap encoders) around the decoded plugin.
         gl::Song s = gl::makeDemoSong();
         for (auto& ch : s.channels) ch.plugin = gl::defaultPluginState("gl.sampler");
         for (auto& m : s.mixer)
           for (auto& sl : m.slots) sl = gl::EffectSlot{};
         if (info->kind == gl::PluginKind::Effect && s.mixer.size() > 1) s.mixer[1].slots[0].plugin = st;
         else if (info->kind == gl::PluginKind::Generator && !s.channels.empty()) s.channels[0].plugin = st;
         exportReimport(s);
       },
       [] { return seedFxPresets(); }},
      {"fst", ".fst",
       [](const std::string& p) {
         gl::flp::PresetResult r;
         (void)gl::flp::importFlPreset(p, r, "/nonexistent-fl-data");
         // Cross-feed: the same plugin events to a few other decoders (each
         // must cope with any payload, not just its own plugin's).
         gl::flp::File f;
         std::string err;
         if (!gl::flp::readFile(p, f, &err)) return;
         auto& all = gl::flp::decoders();
         uint64_t h = 1469598103934665603ull;
         for (uint8_t x : f.bytes) h = (h ^ x) * 1099511628211ull;
         for (int k = 0; k < 3; ++k) {
           auto it = all.begin();
           std::advance(it, static_cast<long>((h >> (k * 9)) % all.size()));
           gl::Song song;
           gl::flp::Report rep;
           gl::flp::Context c(f, song, rep);
           c.factoryData = "/nonexistent-fl-data";
           gl::flp::PluginMapping m;
           m.flName = it->first;
           if (it->second(c, f.events, m)) gl::normalizePluginState(m.state);
         }
       },
       [] { return std::vector<Bytes>{seedFlp()}; }},
      {"project", ".json",
       [](const std::string& p) {
         gl::Song s;
         std::string err;
         if (gl::loadProject(p, s, &err)) {
           gl::Song s2;
           std::string t = gl::projectToText(s);
           gl::Json j;
           if (gl::jsonParse(t, j)) (void)gl::songFromJson(j, s2, &err);
         }
       },
       [] { return std::vector<Bytes>{seedProject(), seedProjectGroups()}; }},
      {"sample", ".bin",
       [](const std::string& p) { (void)gl::loadSample(p); },
       [] { return std::vector<Bytes>{seedWav(16), seedWav(24)}; }},
      {"midi", ".mid",
       [](const std::string& p) {
         gl::MidiFileData m;
         (void)gl::readMidiFile(p, m);
       },
       [] { return std::vector<Bytes>{seedMidi()}; }},
      {"browser-index", ".json",
       [](const std::string& p) {
         gl::browse::Index idx;
         idx.setCacheFile(p);
         if (idx.load()) {
           (void)idx.search(gl::browse::parseQuery("kick bpm:100-130 #loop key:a"));
           (void)idx.allTags();
           gl::browse::FileMeta m;
           (void)idx.lookup("/x/a.wav", m);
           for (const auto& e : idx.search(gl::browse::Query{}, 50)) (void)gl::browse::decodePeaks(e.peaks);
         }
       },
       [] { return std::vector<Bytes>{seedBrowserIndex()}; }},
      {"zip", ".zip",
       [](const std::string& p) {
         std::vector<gl::ZipEntry> entries;
         if (!gl::readZipDirectory(p, entries)) return;
         size_t n = 0;
         for (const auto& e : entries) {
           if (++n > 64) break;
           std::vector<uint8_t> out;
           (void)gl::readZipEntry(p, e, out);
         }
         const std::string dir = gWork + "/unzip";
         std::error_code ec;
         fs::remove_all(dir, ec);
         std::vector<std::string> files;
         (void)gl::extractZip(p, dir, &files);
         for (const auto& f : files)  // nothing may land outside the folder
           if (fs::weakly_canonical(f, ec).string().rfind(fs::weakly_canonical(dir, ec).string(), 0) != 0) std::abort();
         fs::remove_all(dir, ec);
         // The raw inflater on the whole input too (most of it is not a deflate stream).
         Bytes b = readAll(p);
         std::vector<uint8_t> out;
         (void)gl::inflateRaw(b.data(), b.size(), out, b.size() * 4);
       },
       [] { return std::vector<Bytes>{seedZip()}; }},
      {"lua", ".lua",
       [](const std::string& p) {
         Bytes b = readAll(p);
         const std::string src(b.begin(), b.end());
         gl::script::PianoRollScript s;
         if (!s.loadSource(src, "fuzz")) return;
         gl::script::PianoRollContext ctx;
         for (int i = 0; i < 8; ++i) {
           gl::Note n;
           n.start = i * 48, n.length = 24, n.pitch = 60 + i;
           ctx.notes.push_back(n);
         }
         ctx.sel.assign(ctx.notes.size(), false);
         ctx.scale = 2;
         gl::script::Limits lim;
         lim.instructions = 2'000'000;
         lim.seconds = 2.0;
         lim.memoryBytes = 16u << 20;
         (void)s.run(ctx, {}, lim);
       },
       [] { return seedLua(); }},
  };
}

// ---------------------------------------------------------------- mutator
struct Mutator {
  std::mt19937_64 rng;
  size_t maxLen;
  const std::vector<Bytes>* corpus = nullptr;
  size_t pick(size_t n) { return n ? static_cast<size_t>(rng() % n) : 0; }

  void mutate(Bytes& b) {
    int rounds = 1 + static_cast<int>(pick(6));
    for (int r = 0; r < rounds; ++r) {
      switch (pick(10)) {
        case 0:  // bit flip
          if (!b.empty()) b[pick(b.size())] ^= static_cast<uint8_t>(1u << pick(8));
          break;
        case 1:  // random byte
          if (!b.empty()) b[pick(b.size())] = static_cast<uint8_t>(rng());
          break;
        case 2: {  // interesting byte
          static const uint8_t v[] = {0, 1, 0x7F, 0x80, 0xFF, 0xFE, 0x40, 0x20};
          if (!b.empty()) b[pick(b.size())] = v[pick(sizeof v)];
          break;
        }
        case 3: {  // interesting 16/32-bit integer (little endian)
          static const uint32_t v[] = {0, 1, 0xFFFFFFFFu, 0x7FFFFFFFu, 0x80000000u, 0xFFFFu, 0x8000u, 0x10000u, 0x7FFFu, 256, 4096};
          if (b.size() >= 4) {
            size_t at = pick(b.size() - 3);
            uint32_t x = v[pick(sizeof v / sizeof v[0])];
            int w = pick(2) ? 4 : 2;
            for (int k = 0; k < w; ++k) b[at + static_cast<size_t>(k)] = static_cast<uint8_t>(x >> (8 * k));
          }
          break;
        }
        case 4:  // delete a chunk
          if (b.size() > 1) {
            size_t at = pick(b.size()), n = 1 + pick(std::min<size_t>(64, b.size() - at));
            b.erase(b.begin() + static_cast<long>(at), b.begin() + static_cast<long>(std::min(b.size(), at + n)));
          }
          break;
        case 5: {  // insert random bytes
          size_t at = pick(b.size() + 1), n = 1 + pick(32);
          Bytes ins(n);
          for (auto& x : ins) x = static_cast<uint8_t>(rng());
          b.insert(b.begin() + static_cast<long>(at), ins.begin(), ins.end());
          break;
        }
        case 6:  // duplicate a chunk
          if (!b.empty()) {
            size_t at = pick(b.size()), n = 1 + pick(std::min<size_t>(128, b.size() - at));
            Bytes c(b.begin() + static_cast<long>(at), b.begin() + static_cast<long>(at + n));
            b.insert(b.begin() + static_cast<long>(pick(b.size() + 1)), c.begin(), c.end());
          }
          break;
        case 7:  // truncate
          if (b.size() > 8) b.resize(8 + pick(b.size() - 8));
          break;
        case 8:  // splice with another corpus entry
          if (corpus && !corpus->empty()) {
            const Bytes& o = (*corpus)[pick(corpus->size())];
            if (!o.empty() && !b.empty()) {
              size_t cut = pick(b.size()), from = pick(o.size());
              b.resize(cut);
              b.insert(b.end(), o.begin() + static_cast<long>(from), o.end());
            }
          }
          break;
        default: {  // text-ish: swap a digit / quote / bracket (JSON targets)
          static const char v[] = "0123456789-.eE\"{}[],:tfn\\u";
          if (!b.empty()) b[pick(b.size())] = static_cast<uint8_t>(v[pick(sizeof v - 1)]);
          break;
        }
      }
    }
    if (b.size() > maxLen) b.resize(maxLen);
  }
};

// Structure-aware JSON mutation: parse, change one random node, serialise.
void collectNodes(gl::Json& j, std::vector<gl::Json*>& out) {
  out.push_back(&j);
  for (auto& x : j.array) collectNodes(x, out);
  for (auto& [k, x] : j.object) collectNodes(x, out);
}
bool mutateJson(Bytes& b, Mutator& m) {
  gl::Json root;
  if (!gl::jsonParse(std::string(b.begin(), b.end()), root)) return false;
  std::vector<gl::Json*> nodes;
  collectNodes(root, nodes);
  int rounds = 1 + static_cast<int>(m.pick(4));
  for (int r = 0; r < rounds; ++r) {
    gl::Json& n = *nodes[m.pick(nodes.size())];
    static const double nums[] = {-1, 0, 1, 0.5, -0.5, 1e300, -1e300, 2147483648.0, 4294967296.0, 9.3e18, -9.3e18,
                                  1e-310, 255, 256, 65536, 1e9, -2147483649.0, 127, 128, 1024, 999999};
    switch (m.pick(7)) {
      case 0: n.type = gl::Json::Type::Number, n.number = nums[m.pick(sizeof nums / sizeof nums[0])]; break;
      case 1: if (n.type == gl::Json::Type::Number) n.number = -n.number * static_cast<double>(1 + m.pick(1000)); break;
      case 2: if (!n.array.empty()) n.array.erase(n.array.begin() + static_cast<long>(m.pick(n.array.size()))); break;
      case 3: if (!n.array.empty()) n.array.push_back(n.array[m.pick(n.array.size())]); break;
      case 4: if (!n.object.empty()) { auto it = n.object.begin(); std::advance(it, static_cast<long>(m.pick(n.object.size()))); n.object.erase(it); } break;
      case 5: if (n.type == gl::Json::Type::String) n.str = m.pick(2) ? std::string() : n.str + n.str; break;
      default: {  // type swap
        static const gl::Json::Type ts[] = {gl::Json::Type::Null, gl::Json::Type::Bool, gl::Json::Type::Number,
                                            gl::Json::Type::String, gl::Json::Type::Array, gl::Json::Type::Object};
        n.type = ts[m.pick(6)];
        break;
      }
    }
    nodes.clear();
    collectNodes(root, nodes);
  }
  std::string t = gl::jsonStringify(root, 0);
  b.assign(t.begin(), t.end());
  return true;
}

void collect(const std::string& p, std::vector<Bytes>& out, size_t maxLen, size_t maxFiles) {
  std::error_code ec;
  if (fs::is_directory(p, ec)) {
    std::vector<std::string> files;
    for (auto it = fs::recursive_directory_iterator(p, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
      if (ec) break;
      if (it->is_regular_file(ec) && it->file_size(ec) <= maxLen) files.push_back(it->path().string());
    }
    std::sort(files.begin(), files.end());
    // Spread the pick over the whole tree.
    size_t step = std::max<size_t>(1, files.size() / std::max<size_t>(1, maxFiles));
    for (size_t i = 0; i < files.size() && out.size() < maxFiles; i += step) out.push_back(readAll(files[i]));
  } else if (fs::is_regular_file(p, ec)) {
    out.push_back(readAll(p));
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: gridline_fuzz <target> <seconds> [--work DIR] [--seed N] [--max-len N] [--repro FILE] [corpus...]\n");
    return 2;
  }
  std::string name = argv[1];
  double seconds = std::atof(argv[2]);
  std::string work, repro;
  uint64_t seed = 1;
  size_t maxLen = 1 << 20, maxFiles = 400;
  double hangSeconds = 20.0;
  std::vector<std::string> corpusPaths;
  for (int i = 3; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--work" && i + 1 < argc) work = argv[++i];
    else if (a == "--seed" && i + 1 < argc) seed = std::strtoull(argv[++i], nullptr, 10);
    else if (a == "--max-len" && i + 1 < argc) maxLen = std::strtoull(argv[++i], nullptr, 10);
    else if (a == "--max-files" && i + 1 < argc) maxFiles = std::strtoull(argv[++i], nullptr, 10);
    else if (a == "--hang" && i + 1 < argc) hangSeconds = std::atof(argv[++i]);
    else if (a == "--repro" && i + 1 < argc) repro = argv[++i];
    else corpusPaths.push_back(a);
  }
  const Target* t = nullptr;
  static const std::vector<Target> all = targets();
  for (const auto& x : all)
    if (name == x.name) t = &x;
  if (!t) {
    std::fprintf(stderr, "unknown target %s\n", name.c_str());
    return 2;
  }
  if (work.empty()) work = (fs::temp_directory_path() / ("gl-fuzz-" + name + "-" + std::to_string(getpid()))).string();
  fs::create_directories(work);
  // Readers that cache decoded data (FL samples, IRs) write under the work dir.
  setenv("GRIDLINE_CACHE_DIR", (work + "/cache").c_str(), 1);
  setenv("XDG_CONFIG_HOME", (work + "/config").c_str(), 1);
  const std::string input = work + "/input" + t->ext;

  if (!repro.empty()) {  // run one file (no mutation)
    fs::copy_file(repro, input, fs::copy_options::overwrite_existing);
    t->run(input);
    std::printf("repro ok: %s\n", repro.c_str());
    return 0;
  }

  std::vector<Bytes> corpus;
  for (const auto& p : corpusPaths) collect(p, corpus, maxLen, maxFiles);
  if (corpus.empty()) corpus = t->seeds();
  std::printf("%s: %zu corpus inputs, %.0f s, seed %llu, work %s\n", t->name, corpus.size(), seconds,
              static_cast<unsigned long long>(seed), work.c_str());
  std::fflush(stdout);

  std::atomic<int64_t> startedMs{-1};
  std::atomic<bool> done{false};
  auto nowMs = [] {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
  };
  std::thread watchdog([&] {
    while (!done) {
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      int64_t s = startedMs.load();
      if (s >= 0 && static_cast<double>(nowMs() - s) > hangSeconds * 1000.0) {
        std::string dst = work + "/hang-" + std::to_string(nowMs()) + ".bin";
        std::error_code ec;
        fs::copy_file(input, dst, fs::copy_options::overwrite_existing, ec);
        std::fprintf(stderr, "HANG (> %.0f s): input saved to %s\n", hangSeconds, dst.c_str());
        std::fflush(stderr);
        _exit(3);
      }
    }
  });

  Mutator m{std::mt19937_64(seed), maxLen, &corpus};
  const int64_t t0 = nowMs();
  size_t execs = 0;
  // First pass: every corpus input unchanged; then mutations until time is up.
  for (size_t i = 0; i < corpus.size(); ++i) {
    writeAll(input, corpus[i]);
    startedMs = nowMs();
    t->run(input);
    startedMs = -1;
    ++execs;
    if (static_cast<double>(nowMs() - t0) > seconds * 1000.0) break;
  }
  while (static_cast<double>(nowMs() - t0) < seconds * 1000.0) {
    Bytes b = corpus[m.pick(corpus.size())];
    if (!(std::strcmp(t->ext, ".json") == 0 && m.pick(4) != 0 && mutateJson(b, m))) m.mutate(b);
    writeAll(input, b);
    startedMs = nowMs();
    t->run(input);
    startedMs = -1;
    ++execs;
  }
  done = true;
  watchdog.join();
  std::error_code ec;
  fs::remove(input, ec);
  fs::remove_all(work + "/cache", ec);
  std::printf("%s: %zu execs in %.1f s, no crash\n", t->name, execs, static_cast<double>(nowMs() - t0) / 1000.0);
  return 0;
}
