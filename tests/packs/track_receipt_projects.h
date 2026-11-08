// Gridline - J-219: Track Receipt measured on real projects: synthetic audio
// files (made-up names in the store's file-name pattern), projects built in
// code, the offline pass, the report.
#pragma once
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>
#include "../../src/core/Params.h"
#include "../../src/core/PluginInfo.h"
#include "../../src/core/PluginParams.h"
#include "../../src/core/Project.h"
#include "../../src/core/Song.h"
#include "../../src/core/TrackReceipt.h"
#include "../../src/core/WavWriter.h"
#include "../../src/engine/TrackReceiptPass.h"
#include "../../src/packs/samplers/dwave.h"
#include "../../src/packs/samplers/samplers.h"
#include "../Check.h"

namespace {

namespace prj {

using namespace gl;
using namespace gl::receipt;
namespace fs = std::filesystem;

constexpr int kRate = 44100;

// A 10 s file whose every moment sounds (a slow sweep with noise), so any read is heard.
std::string writeFile(const std::string& dir, const std::string& name, double seconds = 10.0, int rate = kRate) {
  fs::create_directories(dir);
  const size_t n = static_cast<size_t>(seconds * rate);
  AudioBuffer b(2, n);
  uint32_t seed = 12345;
  for (size_t i = 0; i < n; ++i) {
    const double t = static_cast<double>(i) / rate;
    seed = seed * 1664525u + 1013904223u;
    const double noise = (static_cast<double>(seed >> 8) / 16777216.0 - 0.5) * 0.1;
    const float v = static_cast<float>(0.35 * std::sin(2 * 3.141592653589793 * (110.0 + 20.0 * t) * t) + noise);
    b.at(0, i) = b.at(1, i) = v;
  }
  const std::string path = (fs::path(dir) / name).string();
  writeWav(path, b, rate, 16);
  return path;
}

Song base() {
  Song s = makeEmptySong();
  s.tempo = 120.0;  // a beat is 0.5 s
  return s;
}
Tick ticksPerSecond(const Song& s) { return static_cast<Tick>(s.beatTicks() * 2); }

ObjectId addChannel(Song& s, const std::string& type, const std::string& data, const std::string& name = "ch") {
  Channel ch;
  ch.id = s.allocateId();
  ch.name = name;
  ch.plugin = defaultPluginState(type);
  ch.plugin.data = data;
  ch.mixerTrack = s.masterId();
  s.channels.push_back(ch);
  return ch.id;
}
Channel& chan(Song& s, ObjectId id) { return *s.channel(id); }

// An audio clip playing file seconds [a, b) at `startSec` of the song.
ObjectId addClip(Song& s, ObjectId channel, double a, double b, double startSec, int track = 0) {
  Clip k;
  k.id = s.allocateId(), k.kind = ClipKind::Audio, k.source = channel, k.track = track;
  k.start = static_cast<Tick>(startSec * static_cast<double>(ticksPerSecond(s)));
  k.length = static_cast<Tick>((b - a) * static_cast<double>(ticksPerSecond(s)));
  k.audioStart = a, k.audioEnd = b;
  s.playlist.push_back(k);
  return k.id;
}
ObjectId audioChannel(Song& s, const std::string& path, const std::string& name = "clip") {
  ObjectId id = addChannel(s, "gl.sampler", path, name);
  chan(s, id).audioClip = true;
  return id;
}

// A pattern with notes for `channel` (start / length in seconds), placed once at the song start.
ObjectId addNotes(Song& s, ObjectId channel, const std::vector<std::pair<double, double>>& notes, int pitch = 60, int bars = 4) {
  Pattern p;
  p.id = s.allocateId();
  p.bars = bars;
  for (const auto& [at, len] : notes) {
    Note n;
    n.start = static_cast<Tick>(at * static_cast<double>(ticksPerSecond(s)));
    n.length = static_cast<Tick>(len * static_cast<double>(ticksPerSecond(s)));
    n.pitch = pitch;
    p.notes[channel].push_back(n);
  }
  s.patterns.push_back(p);
  Clip k;
  k.id = s.allocateId(), k.kind = ClipKind::Pattern, k.pattern = p.id, k.track = 1;
  k.start = 0;
  k.length = static_cast<Tick>(bars) * s.barTicks();
  s.playlist.push_back(k);
  return p.id;
}

Report run(const Song& s, const Saved& saved = {}) {
  PassOptions o;
  o.sampleRate = kRate;
  return build(measure(s, o), saved, "test");
}
const SongRow* songOf(const Report& r, const std::string& key) {
  for (const SongRow& s : r.songs)
    if (s.key == key) return &s;
  return nullptr;
}
double secondsOf(const Report& r, const std::string& key) {
  const SongRow* s = songOf(r, key);
  return s ? s->seconds : -1;
}
bool within(double v, double want, double tol) { return std::fabs(v - want) <= tol; }

const char* kSongA = "Test Artist-Made Up Song";
std::string nameA(const std::string& stem, int part = 0) {
  return std::string(kSongA) + "-" + stem + "--120.00bpm-Am-[Clearance Required]" + (part ? " - Part_" + std::to_string(part) : "") + ".wav";
}

void testPlaylist(const std::string& dir) {
  const std::string master = writeFile(dir, nameA("Master")), drums = writeFile(dir, nameA("Drums"));
  {  // 2 s looped for 4 minutes counts 2.0 s
    Song s = base();
    ObjectId c = audioChannel(s, master);
    for (int i = 0; i < 120; ++i) addClip(s, c, 1.0, 3.0, 2.0 * i);
    Report r = run(s);
    expect(within(secondsOf(r, kSongA), 2.0, 0.05), "project: 2 s looped for 4 minutes counts 2.0 s");
    const SongRow* row = songOf(r, kSongA);
    expect(row && row->timelines.size() == 1 && row->timelines[0].used.size() == 1 && within(row->timelines[0].used[0].a, 1.0, 0.02),
           "project: the looped part sits at 0:01.0");
    expect(row && row->tier == 2, "project: 2 s is the 2 s tier");
  }
  {  // the same section from two stems counts once; two sections add
    Song s = base();
    ObjectId m = audioChannel(s, master, "m"), d = audioChannel(s, drums, "d");
    addClip(s, m, 1.0, 3.0, 0.0, 0);
    addClip(s, d, 1.0, 3.0, 0.0, 1);
    addClip(s, d, 5.0, 6.0, 4.0, 1);
    Report r = run(s);
    expect(within(secondsOf(r, kSongA), 3.0, 0.05), "project: same section of two stems once, a second section adds (3.0 s)");
  }
  {  // muted clips and muted channels do not count, and are listed
    Song s = base();
    ObjectId m = audioChannel(s, master, "m");
    addClip(s, m, 0.0, 2.0, 0.0);
    ObjectId muted = addClip(s, m, 4.0, 8.0, 3.0);
    s.clip(muted)->muted = true;
    Report r = run(s);
    expect(within(secondsOf(r, kSongA), 2.0, 0.05), "project: a muted clip is not counted");
    bool listed = false;
    for (const FileRow& f : r.files) listed = listed || !f.muted.empty();
    expect(listed, "project: the muted clip is listed as not counted");
  }
  {  // K-132: silent as set, heard through its automation: counted; kept silent by it: not counted
    const auto automate = [](Song& s, ParamId id, std::vector<float> values) {
      AutomationClip a;
      a.id = s.allocateId(), a.owner = id.owner, a.index = id.index;
      for (size_t i = 0; i < values.size(); ++i) a.points.push_back(AutoPoint{static_cast<Tick>(i) * s.barTicks(), values[i]});
      s.automations.push_back(a);
      Clip k;
      k.id = s.allocateId(), k.kind = ClipKind::Automation, k.source = a.id, k.track = 5;
      k.start = 0, k.length = 16 * s.barTicks();
      s.playlist.push_back(k);
    };
    {  // a channel at volume zero that its automation brings up
      Song s = base();
      ObjectId m = audioChannel(s, master, "m");
      chan(s, m).gain = 0;
      addClip(s, m, 1.0, 3.0, 2.0);
      automate(s, channelParam(m, ChannelParam::Volume), {0.0f, 0.8f});
      expect(within(secondsOf(run(s), kSongA), 2.0, 0.05), "project: a channel at volume zero brought up by its automation counts");
    }
    {  // a muted channel its mute automation switches on
      Song s = base();
      ObjectId m = audioChannel(s, master, "m");
      chan(s, m).muted = true;
      addClip(s, m, 1.0, 3.0, 2.0);
      automate(s, channelParam(m, ChannelParam::Mute), {1.0f, 0.0f});
      expect(within(secondsOf(run(s), kSongA), 2.0, 0.05), "project: a muted channel unmuted by its automation counts");
    }
    {  // a muted mixer track brought back by its automation
      Song s = base();
      ObjectId m = audioChannel(s, master, "m");
      chan(s, m).mixerTrack = s.mixer[1].id;
      s.mixer[1].muted = true;
      addClip(s, m, 1.0, 3.0, 2.0);
      automate(s, mixerParam(s.mixer[1].id, MixerParam::Mute), {0.0f});
      expect(within(secondsOf(run(s), kSongA), 2.0, 0.05), "project: a muted mixer track unmuted by its automation counts");
    }
    {  // automation that keeps it at zero: still silent, still listed
      Song s = base();
      ObjectId m = audioChannel(s, master, "m");
      chan(s, m).gain = 0;
      addClip(s, m, 1.0, 3.0, 2.0);
      automate(s, channelParam(m, ChannelParam::Volume), {0.0f, 0.0f});
      const Report rep = run(s);
      bool listed = false;
      for (const FileRow& f : rep.files) listed = listed || !f.muted.empty();
      expect(secondsOf(rep, kSongA) <= 0.0 && listed, "project: automation that keeps the volume at zero: not counted, listed");
    }
  }
  {  // reverse: the same part of the file counts
    Song s = base();
    ObjectId m = audioChannel(s, master, "m");
    ObjectId k = addClip(s, m, 2.0, 4.0, 0.0);
    s.clip(k)->reverse = true;
    Report r = run(s);
    expect(within(secondsOf(r, kSongA), 2.0, 0.06), "project: a reversed clip counts its part (2.0 s)");
  }
  {  // stretched: twice as long in the song, the same 2 s of the file
    Song s = base();
    ObjectId m = audioChannel(s, master, "m");
    ObjectId k = addClip(s, m, 2.0, 4.0, 0.0);
    s.clip(k)->stretch = 2.0;
    s.clip(k)->length *= 2;
    Report r = run(s);
    expect(within(secondsOf(r, kSongA), 2.0, 0.08), "project: a stretched clip counts the file part once (2.0 s)");
  }
}

void testSampler(const std::string& dir) {
  const std::string path = writeFile(dir, nameA("Keys"));
  auto sampler = [&](Song& s) {
    ObjectId c = addChannel(s, "gl.sampler", path, "smp");
    auto& p = chan(s, c).plugin.params;
    p[static_cast<size_t>(pp::SmpOneShot)] = 0;
    p[static_cast<size_t>(pp::SmpRelease)] = 0.002f;
    return c;
  };
  {  // start offset 20 %, a 0.5 s note: 2.0-2.5 s
    Song s = base();
    ObjectId c = sampler(s);
    chan(s, c).plugin.params[static_cast<size_t>(pp::SmpStart)] = 0.2f;
    addNotes(s, c, {{0.0, 0.5}, {1.0, 0.5}, {2.0, 0.5}});
    Report r = run(s);
    const SongRow* row = songOf(r, kSongA);
    expect(row && within(row->seconds, 0.5, 0.1), "project: sampler start offset, 0.5 s notes repeated count 0.5 s");
    expect(row && !row->timelines.empty() && !row->timelines[0].used.empty() && within(row->timelines[0].used[0].a, 2.0, 0.05),
           "project: sampler start offset maps to 0:02.0");
  }
  {  // an octave up reads twice as fast
    Song s = base();
    ObjectId c = sampler(s);
    addNotes(s, c, {{0.0, 0.5}}, 72);
    Report r = run(s);
    expect(within(secondsOf(r, kSongA), 1.0, 0.1), "project: sampler an octave up reads 1.0 s in 0.5 s");
  }
  {  // a loop 1-2 s held for 4 s: 0-2 s of the file
    Song s = base();
    ObjectId c = sampler(s);
    auto& p = chan(s, c).plugin.params;
    p[static_cast<size_t>(pp::SmpLoop)] = 1;
    p[static_cast<size_t>(pp::SmpLoopStart)] = 0.1f;
    p[static_cast<size_t>(pp::SmpLoopEnd)] = 0.2f;
    addNotes(s, c, {{0.0, 4.0}});
    Report r = run(s);
    expect(within(secondsOf(r, kSongA), 2.0, 0.1), "project: sampler loop held 4 s counts the loop once (2.0 s)");
  }
  {  // reverse
    Song s = base();
    ObjectId c = sampler(s);
    chan(s, c).plugin.params[static_cast<size_t>(pp::SmpReverse)] = 1;
    addNotes(s, c, {{0.0, 1.0}});
    Report r = run(s);
    const SongRow* row = songOf(r, kSongA);
    expect(row && within(row->seconds, 1.0, 0.1), "project: sampler reverse counts 1.0 s");
    expect(row && !row->timelines.empty() && !row->timelines[0].used.empty() && row->timelines[0].used.back().b > 9.8,
           "project: sampler reverse reads the end of the file");
  }
  const std::string hi = writeFile(dir, nameA("Keys96"), 10.0, 96000);
  for (float mode : {1.0f, 7.0f, 101.0f, 107.0f}) {  // time stretch x2 (Gridline's stretch, Rubber Band): a 2 s note reads 1 s (the stretcher reads a little ahead)
    Song s = base();
    ObjectId c = mode > 100 ? addChannel(s, "gl.sampler", hi, "smp96") : sampler(s);
    if (mode > 100) {
      chan(s, c).plugin.params[static_cast<size_t>(pp::SmpOneShot)] = 0;
      chan(s, c).plugin.params[static_cast<size_t>(pp::SmpRelease)] = 0.002f;
      mode -= 100;
    }
    auto& p = chan(s, c).plugin.params;
    p[static_cast<size_t>(pp::SmpTimeStretch)] = 2.0f;
    p[static_cast<size_t>(pp::SmpStretchMode)] = mode;
    p[static_cast<size_t>(pp::SmpStart)] = 0.5f;
    addNotes(s, c, {{1.0, 2.0}});
    Report r = run(s);
    const double got = secondsOf(r, kSongA);
    expect(within(got, 1.0, 0.25), ("project: sampler stretched x2 (mode " + std::to_string(static_cast<int>(mode)) + ") counts 1.0 s, got " + secondsText(got)).c_str());
  }
  {  // a muted channel is not counted
    Song s = base();
    ObjectId c = sampler(s);
    chan(s, c).muted = true;
    addNotes(s, c, {{0.0, 1.0}});
    Report r = run(s);
    expect(secondsOf(r, kSongA) <= 0, "project: a muted sampler channel is not counted");
  }
}

// Every sample-playing instrument: one note of 1 s reads some of the file, never more than it has.
void testEveryType(const std::string& dir, std::vector<std::string>& coverage) {
  const char* types[] = {"gl.sampler", "gl.pads",       "gl.slicer",      "gl.granular",  "gl.multisampler", "gl.scratch",
                         "gl.waveprogram", "gl.platter", "gl.seltmann", "gl.grainscan", "gl.splinescratch", "gl.turntable",
                         "gl.plasma"};
  for (const char* type : types) {
    if (!findPlugin(type)) continue;
    const std::string stem = std::string("Stem ") + (type + 3);
    const std::string path = writeFile(dir, nameA(stem), 4.0);
    Song s = base();
    std::string data = path;
    if (std::string(type) == "gl.multisampler") {
      packs::samplers::ZoneMap m;
      packs::samplers::Zone z;
      z.path = path, z.lo = 0, z.hi = 127, z.root = 60;
      m.zones = {z};
      data = packs::samplers::serializeZoneMap(m);
    } else if (std::string(type) == "gl.waveprogram") {
      packs::samplers::dw::DwProgram p;
      packs::samplers::dw::DwZone z;
      z.path = path;
      p.zones = {z};
      data = packs::samplers::dw::formatProgram(p);
    }
    ObjectId c = addChannel(s, type, data, type);
    addNotes(s, c, {{0.0, 1.0}, {2.0, 1.0}});
    Report r = run(s);
    const double sec = secondsOf(r, kSongA);
    coverage.push_back(std::string(type) + " " + secondsText(sec) + " s");
    expect(sec > 0.05 && sec <= 4.0 + 1e-6, (std::string("project: ") + type + " reports what it plays (" + secondsText(sec) + " s)").c_str());
  }
}

// A small project for the window's screenshots (GL_RECEIPT_PROJECTS=<dir>): two songs, a
// sampler, a loop, a muted clip and a royalty-free file.
void saveDemo(const std::string& dir) {
  const std::string m = writeFile(dir, nameA("Master")), d = writeFile(dir, nameA("Drums"));
  const std::string other = writeFile(dir, "Second Example-Night Garden-Keys--96.00bpm-C-[Clearance Required].wav", 30.0);
  const std::string free = writeFile(dir, "Royalty free kick.wav", 1.0);
  Song s = base();
  ObjectId cm = audioChannel(s, m, "Master"), cd = audioChannel(s, d, "Drums");
  for (int i = 0; i < 16; ++i) addClip(s, cm, 1.0, 3.0, 2.0 * i, 0);
  addClip(s, cd, 1.0, 3.0, 0.0, 1);
  addClip(s, cd, 5.0, 6.5, 8.0, 1);
  ObjectId muted = addClip(s, cd, 7.0, 9.0, 12.0, 1);
  s.clip(muted)->muted = true;
  ObjectId keys = addChannel(s, "gl.sampler", other, "Keys");
  chan(s, keys).plugin.params[static_cast<size_t>(pp::SmpStart)] = 0.25f;
  chan(s, keys).plugin.params[static_cast<size_t>(pp::SmpOneShot)] = 0;
  addNotes(s, keys, {{0.0, 1.5}, {4.0, 1.5}, {8.0, 3.0}});
  ObjectId kick = addChannel(s, "gl.sampler", free, "Kick");
  addNotes(s, kick, {{0.0, 0.2}, {1.0, 0.2}});
  std::string err;
  saveProject(dir + "/Receipt demo.json", s, &err);
}

void runProjectTests(const std::string& dir) {
  if (const char* demo = std::getenv("GL_RECEIPT_PROJECTS")) saveDemo(demo);
  fs::remove_all(dir);
  testPlaylist(dir + "/playlist");
  testSampler(dir + "/sampler");
  std::vector<std::string> coverage;
  testEveryType(dir + "/types", coverage);
  for (const auto& c : coverage) std::printf("# coverage %s\n", c.c_str());
}

}  // namespace prj

void runProjectTests(const std::string& dir) { prj::runProjectTests(dir); }

}  // namespace
