// Gridline - K-319: the Track Receipt's fast measurement gives exactly what the
// full-mix reference pass gives (every effect, one thread), on projects that
// cover audio clips (plain, reversed, stretched, looped, pitched, tempo-synced,
// muted), the sampler (start offset, pitch, loop, reverse, time stretch), the
// slicer and the other sample players, automation, layers, effects that ring
// on, synths without files and the demo song with sample parts added.
// Prints the time of each pass ("# time ...").
//
// GL_RECEIPT_SPEED_LARGE=1 also runs the medium and large projects (the demo
// song with 8 sample channels and 40 audio clips, and with 32 and 320); GL_RECEIPT_SPEED_SAVE=<dir> saves the
// projects there (for timing the command line and the app).
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
#include <vector>
#include "../../src/core/Demo.h"
#include "../../src/core/Params.h"
#include "../../src/core/PluginInfo.h"
#include "../../src/core/PluginParams.h"
#include "../../src/core/Project.h"
#include "../../src/core/Song.h"
#include "../../src/core/TrackReceipt.h"
#include "../../src/core/WavWriter.h"
#include "../../src/engine/AudioEngine.h"
#include "../../src/engine/Program.h"
#include "../../src/engine/SampleTap.h"
#include "../../src/engine/TrackReceiptPass.h"
#include "../Check.h"

using namespace gl;
using namespace gl::receipt;
namespace fs = std::filesystem;

namespace {

constexpr int kRate = 44100;

// A file whose every moment sounds (a sweep with noise); `seed` makes each file differ.
std::string writeFile(const std::string& dir, const std::string& name, double seconds, uint32_t seed) {
  fs::create_directories(dir);
  const auto n = static_cast<size_t>(seconds * kRate);
  AudioBuffer b(2, n);
  for (size_t i = 0; i < n; ++i) {
    const double t = static_cast<double>(i) / kRate;
    seed = seed * 1664525u + 1013904223u;
    const double noise = (static_cast<double>(seed >> 8) / 16777216.0 - 0.5) * 0.1;
    const auto v = static_cast<float>(0.35 * std::sin(2 * 3.141592653589793 * (110.0 + 20.0 * t) * t) + noise);
    b.at(0, i) = b.at(1, i) = v;
  }
  const std::string path = (fs::path(dir) / name).generic_string();
  writeWav(path, b, kRate, 16);
  return path;
}
std::string storeName(const std::string& song, const std::string& stem) {
  return "Speed Test-" + song + "-" + stem + "--120.00bpm-Am-[Clearance Required].wav";
}

Tick ticksPerSecond(const Song& s) { return static_cast<Tick>(std::llround(static_cast<double>(s.beatTicks()) * s.tempo / 60.0)); }
Tick at(const Song& s, double sec) { return static_cast<Tick>(sec * static_cast<double>(ticksPerSecond(s))); }

ObjectId addChannel(Song& s, const std::string& type, const std::string& data, const std::string& name, ObjectId mixer = kNoId) {
  Channel ch;
  ch.id = s.allocateId();
  ch.name = name;
  ch.plugin = defaultPluginState(type);
  ch.plugin.data = data;
  ch.mixerTrack = mixer == kNoId ? s.masterId() : mixer;
  s.channels.push_back(ch);
  return ch.id;
}
Channel& chan(Song& s, ObjectId id) { return *s.channel(id); }
float& param(Song& s, ObjectId c, int k) { return chan(s, c).plugin.params[static_cast<size_t>(k)]; }

ObjectId addClip(Song& s, ObjectId channel, double a, double b, double startSec, int track) {
  Clip k;
  k.id = s.allocateId(), k.kind = ClipKind::Audio, k.source = channel, k.track = track;
  k.start = at(s, startSec);
  k.length = at(s, b - a);
  k.audioStart = a, k.audioEnd = b;
  s.playlist.push_back(k);
  return k.id;
}
ObjectId audioChannel(Song& s, const std::string& path, const std::string& name, ObjectId mixer = kNoId) {
  const ObjectId id = addChannel(s, "gl.sampler", path, name, mixer);
  chan(s, id).audioClip = true;
  return id;
}
// A pattern of notes (start, length in seconds, pitch) placed at each of `places` (seconds).
ObjectId addNotes(Song& s, ObjectId channel, const std::vector<std::tuple<double, double, int>>& notes, const std::vector<double>& places,
                  int track, int bars = 2) {
  Pattern p;
  p.id = s.allocateId();
  p.bars = bars;
  for (const auto& [st, len, pitch] : notes) {
    Note n;
    n.start = at(s, st);
    n.length = std::max<Tick>(1, at(s, len));
    n.pitch = pitch;
    p.notes[channel].push_back(n);
  }
  s.patterns.push_back(p);
  for (double place : places) {
    Clip k;
    k.id = s.allocateId(), k.kind = ClipKind::Pattern, k.pattern = p.id, k.track = track;
    k.start = at(s, place);
    k.length = static_cast<Tick>(bars) * s.barTicks();
    s.playlist.push_back(k);
  }
  return p.id;
}
void automate(Song& s, ParamId id, const std::vector<float>& values, int track) {
  AutomationClip a;
  a.id = s.allocateId(), a.owner = id.owner, a.index = id.index;
  for (size_t i = 0; i < values.size(); ++i) a.points.push_back(AutoPoint{static_cast<Tick>(i) * s.barTicks(), values[i]});
  s.automations.push_back(a);
  Clip k;
  k.id = s.allocateId(), k.kind = ClipKind::Automation, k.source = a.id, k.track = track;
  k.start = 0, k.length = static_cast<Tick>(values.size()) * s.barTicks();
  s.playlist.push_back(k);
}
void effect(Song& s, size_t mixerIndex, int slot, const std::string& type) {
  if (!findPlugin(type) || mixerIndex >= s.mixer.size()) return;
  s.mixer[mixerIndex].slots[static_cast<size_t>(slot)].plugin = defaultPluginState(type);
}

// Every kind of sample use, on a 120 BPM song of about 40 s, with effects that ring on.
Song features(const std::string& dir) {
  Song s = makeEmptySong();
  s.tempo = 120.0;
  const std::string a = writeFile(dir, storeName("Alpha", "Master"), 12.0, 1), b = writeFile(dir, storeName("Alpha", "Drums"), 12.0, 2);
  const std::string c = writeFile(dir, storeName("Beta", "Keys"), 10.0, 3), d = writeFile(dir, storeName("Beta", "Vox"), 8.0, 4);
  const std::string e = writeFile(dir, storeName("Gamma", "Break"), 4.0, 5), f = writeFile(dir, "Royalty free loop.wav", 6.0, 6);
  // Mixer: insert 1 a hall and a compressor, the master a delay and a limiter.
  effect(s, 1, 0, "gl.hall"), effect(s, 1, 1, "gl.comp"), effect(s, 0, 0, "gl.delay"), effect(s, 0, 1, "gl.limiter");
  const ObjectId ins1 = s.mixer.size() > 1 ? s.mixer[1].id : kNoId;
  // Audio clips.
  const ObjectId ca = audioChannel(s, a, "clips a", ins1), cb = audioChannel(s, b, "clips b"), cf = audioChannel(s, f, "free");
  for (int i = 0; i < 8; ++i) addClip(s, ca, 1.0, 3.0, 2.0 * i, 0);              // a loop played 8 times
  s.clip(addClip(s, cb, 2.0, 4.0, 0.0, 1))->reverse = true;                       // reversed
  {
    Clip& k = *s.clip(addClip(s, cb, 5.0, 7.0, 4.0, 1));                          // stretched x2
    k.stretch = 2.0;
    k.length *= 2;
  }
  s.clip(addClip(s, cb, 8.0, 9.0, 9.0, 1))->pitch = 700.0f;                       // pitched
  {
    Clip& k = *s.clip(addClip(s, cb, 9.0, 11.0, 11.0, 1));                        // looped past its window
    k.loop = true;
    k.length *= 3;
  }
  {
    Clip& k = *s.clip(addClip(s, ca, 6.0, 8.0, 18.0, 2));                         // tempo-synced
    k.syncBpm = 100.0;
  }
  s.clip(addClip(s, ca, 10.0, 11.5, 22.0, 2))->muted = true;                      // muted: listed, not counted
  addClip(s, cf, 0.0, 6.0, 24.0, 3);
  addClip(s, ca, 10.5, 12.0, 38.0, 2);                                            // runs to the song end
  // Sampler: start offset, octave up, loop, reverse, stretched.
  const ObjectId sm = addChannel(s, "gl.sampler", c, "sampler", ins1);
  param(s, sm, pp::SmpOneShot) = 0, param(s, sm, pp::SmpRelease) = 0.002f, param(s, sm, pp::SmpStart) = 0.2f;
  addNotes(s, sm, {{0.0, 0.5, 60}, {1.0, 0.5, 72}, {2.0, 0.5, 55}}, {0.0, 8.0}, 4);
  const ObjectId lp = addChannel(s, "gl.sampler", c, "sampler loop");
  param(s, lp, pp::SmpOneShot) = 0, param(s, lp, pp::SmpLoop) = 1, param(s, lp, pp::SmpLoopStart) = 0.4f, param(s, lp, pp::SmpLoopEnd) = 0.5f;
  addNotes(s, lp, {{0.0, 3.0, 60}}, {12.0}, 5);
  const ObjectId rv = addChannel(s, "gl.sampler", d, "sampler reverse");
  param(s, rv, pp::SmpReverse) = 1, param(s, rv, pp::SmpOneShot) = 0;
  addNotes(s, rv, {{0.0, 1.0, 60}}, {16.0}, 5);
  const ObjectId st = addChannel(s, "gl.sampler", d, "sampler stretch");
  param(s, st, pp::SmpOneShot) = 0, param(s, st, pp::SmpTimeStretch) = 2.0f, param(s, st, pp::SmpStretchMode) = 1.0f, param(s, st, pp::SmpStart) = 0.5f;
  addNotes(s, st, {{0.0, 2.0, 60}}, {20.0}, 5);
  // Automation: the sampler's start and its channel pitch move over the song.
  automate(s, channelPluginParam(sm, pp::SmpStart), {0.1f, 0.3f, 0.6f, 0.2f, 0.8f}, 6);
  automate(s, channelParam(sm, ChannelParam::Pitch), {0.5f, 0.7f, 0.3f, 0.5f}, 7);
  // Automation the fast pass leaves out (levels only).
  automate(s, mixerParam(s.mixer[0].id, MixerParam::Volume), {0.8f, 0.4f, 0.8f}, 22);
  automate(s, channelParam(sm, ChannelParam::Volume), {0.8f, 0.6f}, 23);
  // A long release that rings past the song end (the tail).
  const ObjectId tl = addChannel(s, "gl.sampler", c, "tail", ins1);
  param(s, tl, pp::SmpOneShot) = 0, param(s, tl, pp::SmpRelease) = 0.6f;
  addNotes(s, tl, {{1.5, 0.4, 60}}, {38.0}, 8);
  // Slicer and the other sample players.
  const ObjectId sl = addChannel(s, "gl.slicer", e, "slicer");
  addNotes(s, sl, {{0.0, 0.25, 60}, {0.5, 0.25, 62}, {1.0, 0.25, 61}, {1.5, 0.25, 64}}, {4.0, 28.0}, 9);
  const char* types[] = {"gl.pads", "gl.granular", "gl.scratch", "gl.platter", "gl.seltmann", "gl.grainscan", "gl.splinescratch", "gl.turntable"};
  int k = 0;
  for (const char* t : types) {
    if (!findPlugin(t)) {
      continue;
    }
    const std::string file = writeFile(dir, storeName("Delta", std::string("Stem ") + (t + 3)), 4.0, 10u + static_cast<uint32_t>(k));
    const ObjectId ch = addChannel(s, t, file, t);
    addNotes(s, ch, {{0.0, 1.0, 60}, {1.5, 0.5, 64}}, {2.0 * k, 24.0 + k}, 10 + k);
    ++k;
  }
  // A layer (with its own synth) that plays a sampler child, and a synth without files.
  const ObjectId child = addChannel(s, "gl.sampler", e, "layer child");
  param(s, child, pp::SmpOneShot) = 0;
  const ObjectId layer = addChannel(s, "gl.analog", "", "layer");  // its own synth plays too (taken out by the fast pass)
  chan(s, layer).layer = {child};
  addNotes(s, layer, {{0.0, 0.5, 60}, {1.0, 0.5, 67}}, {30.0}, 20);
  if (findPlugin("gl.analog")) {
    const ObjectId syn = addChannel(s, "gl.analog", "", "synth", ins1);
    addNotes(s, syn, {{0.0, 1.0, 48}, {1.0, 1.0, 51}}, {0.0, 4.0, 8.0}, 21);
    automate(s, channelPluginParam(syn, 3), {0.2f, 0.9f, 0.4f}, 25);  // a synth's sound: left out
  }
  return s;
}

// The demo song with `channels` sample channels (sampler / slicer notes, routed to inserts with
// effects) and `clips` audio clips over its length, some stretched, reversed or pitched.
Song demoPlus(const std::string& dir, int channels, int clips) {
  Song s = makeDemoTrack();
  if (channels == 0 && clips == 0) {
    return s;
  }
  const double songSec = static_cast<double>(s.lengthTicks()) / static_cast<double>(ticksPerSecond(s));
  std::vector<std::string> files;
  for (int i = 0; i < 12; ++i) {
    files.push_back(writeFile(dir, storeName("Song " + std::to_string(i % 6), "Stem" + std::to_string(i)), 12.0, 100u + static_cast<uint32_t>(i)));
  }
  const int baseTrack = static_cast<int>(s.tracks.size());
  for (int i = 0; i < channels; ++i) {
    const size_t mi = 28 + static_cast<size_t>(i % 8);
    if (mi < s.mixer.size()) {
      effect(s, mi, 0, "gl.eq"), effect(s, mi, 1, i % 2 ? "gl.hall" : "gl.comp");
    }
    const ObjectId mix = mi < s.mixer.size() ? s.mixer[mi].id : kNoId;
    const std::string& file = files[static_cast<size_t>(i) % files.size()];
    const ObjectId ch = addChannel(s, i % 4 == 3 ? "gl.slicer" : "gl.sampler", file, "smp " + std::to_string(i), mix);
    if (i % 4 != 3) {
      param(s, ch, pp::SmpOneShot) = 0, param(s, ch, pp::SmpStart) = static_cast<float>(i % 5) * 0.15f;
    }
    std::vector<double> places;
    for (double t = 2.0 * (i % 7); t < songSec - 4; t += 16.0) {
      places.push_back(t);
    }
    addNotes(s, ch, {{0.0, 0.3, 60 + i % 5}, {0.5, 0.3, 62}, {1.0, 0.6, 60}, {2.0, 0.3, 64 + i % 3}}, places, baseTrack + i % 16);
  }
  std::vector<ObjectId> aud;
  for (size_t i = 0; i < files.size(); ++i) {
    aud.push_back(audioChannel(s, files[i], "audio " + std::to_string(i), s.mixer.size() > 36 ? s.mixer[36 + i % 4].id : kNoId));
  }
  for (int i = 0; i < clips; ++i) {
    const double start = std::fmod(i * 3.7, songSec - 6);
    const double a = std::fmod(i * 1.3, 9.0);
    Clip& k = *s.clip(addClip(s, aud[static_cast<size_t>(i) % aud.size()], a, a + 1.0 + (i % 3), start, baseTrack + 16 + i % 24));
    if (i % 5 == 1) {
      k.stretch = 1.5, k.length = static_cast<Tick>(static_cast<double>(k.length) * 1.5);
    }
    if (i % 7 == 2) {
      k.reverse = true;
    }
    if (i % 9 == 4) {
      k.pitch = 300.0f;
    }
  }
  return s;
}

// Tempo automation (kept by the fast pass) under audio clips (plain, tempo-synced, stretched),
// a sampler and a slicer, with mixer automation and effects (left out).
Song tempo(const std::string& dir) {
  Song s = makeEmptySong();
  s.tempo = 120.0;
  const std::string a = writeFile(dir, storeName("Zeta", "Master"), 10.0, 50), b = writeFile(dir, storeName("Zeta", "Break"), 4.0, 51);
  effect(s, 0, 0, "gl.hall");
  const ObjectId ca = audioChannel(s, a, "clips");
  addClip(s, ca, 0.0, 2.0, 0.0, 0);
  s.clip(addClip(s, ca, 2.0, 4.0, 3.0, 0))->syncBpm = 110.0;
  {
    Clip& k = *s.clip(addClip(s, ca, 5.0, 7.0, 6.0, 0));
    k.stretch = 1.5;
    k.length = static_cast<Tick>(static_cast<double>(k.length) * 1.5);
  }
  const ObjectId sm = addChannel(s, "gl.sampler", a, "sampler");
  param(s, sm, pp::SmpOneShot) = 0, param(s, sm, pp::SmpStart) = 0.4f;
  addNotes(s, sm, {{0.0, 0.5, 60}, {1.0, 0.5, 67}}, {0.0, 5.0, 10.0}, 1);
  const ObjectId sl = addChannel(s, "gl.slicer", b, "slicer");
  addNotes(s, sl, {{0.0, 0.25, 60}, {0.5, 0.25, 62}}, {2.0, 9.0}, 2);
  automate(s, songParam(SongParam::Tempo), {0.4f, 0.45f, 0.42f, 0.5f, 0.4f, 0.48f, 0.44f}, 3);
  automate(s, mixerParam(s.mixer[0].id, MixerParam::Volume), {0.8f, 0.4f, 0.8f}, 4);
  return s;
}

// Pattern mode (no playlist): each pattern with notes plays once.
Song patterns(const std::string& dir) {
  Song s = makeEmptySong();
  s.tempo = 120.0;
  const std::string a = writeFile(dir, storeName("Epsilon", "Keys"), 8.0, 40), b = writeFile(dir, storeName("Epsilon", "Break"), 4.0, 41);
  effect(s, 0, 0, "gl.hall");
  const ObjectId sm = addChannel(s, "gl.sampler", a, "sampler");
  param(s, sm, pp::SmpOneShot) = 0, param(s, sm, pp::SmpStart) = 0.3f;
  const ObjectId sl = addChannel(s, "gl.slicer", b, "slicer");
  addNotes(s, sm, {{0.0, 0.5, 60}, {1.0, 0.5, 65}}, {}, 0);
  addNotes(s, sl, {{0.0, 0.25, 60}, {0.5, 0.25, 63}}, {}, 0);
  addNotes(s, sm, {{0.5, 1.5, 48}}, {}, 0);
  return s;
}

// Every figure of a measurement, doubles to the last bit (17 significant digits).
std::string usesText(const Measurement& m) {
  std::ostringstream t;
  t << std::setprecision(17) << "mode " << m.mode << (m.cancelled ? " cancelled" : "") << "\n";
  for (const FileIn& f : m.files) {
    t << "file " << f.path << " " << f.seconds << " " << f.exists << " " << f.derived << " " << f.chain.size() << "\n";
  }
  for (const Use& u : m.uses) {
    t << "use " << u.file << " | " << u.owner << " | " << u.where << " | " << u.note << " | plays " << u.plays << " | bars " << u.bar0 << "-"
      << u.bar1 << " | " << (u.muted ? "muted" : "") << (u.estimated ? "estimated" : "") << " |";
    for (const Range& r : u.ranges) {
      t << " " << r.a << "-" << r.b;
    }
    t << "\n";
  }
  for (const std::string& w : m.warnings) {
    t << "warning " << w << "\n";
  }
  return t.str();
}

// K-321: the tap's own figures of every pair: play seconds and pitch (any blocks); with `blocks`,
// the jumps and the segments too (they follow the engine's blocks, which are the same in both
// passes unless the fast pass took automation out).
std::string tapText(const std::vector<tap::Use>& uses, bool blocks) {
  std::ostringstream t;
  t << std::setprecision(17);
  for (const tap::Use& u : uses) {
    t << "pair " << u.file << " " << u.owner << " play " << u.outSeconds << " pitch " << u.pitched << " " << u.semisMin << " " << u.semisMax
      << " " << u.ratioMin << " " << u.ratioMax;
    if (blocks) {
      t << " jumps " << u.jumps << " |";
      for (const tap::Segment& g : u.segments) {
        t << " " << g.outA << "," << g.outB << ">" << g.srcA << "," << g.srcB;
      }
    }
    t << "\n";
  }
  return t.str();
}

double seconds(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

// Both passes on `s`: the measurement and both outputs must be the same. K-321: so must the tap's
// play seconds and pitch, and with `blocks` (no automation the fast pass takes out) its jumps and
// segments.
void compare(const std::string& name, const Song& s, const std::string& saveDir, bool blocks = false) {
  if (!saveDir.empty()) {
    std::string err;
    saveProject((fs::path(saveDir) / (name + ".gridline")).generic_string(), s, &err);
    if (std::getenv("GL_RECEIPT_SPEED_SAVE_ONLY")) {
      return;
    }
  }
  PassOptions ref;
  ref.sampleRate = kRate;
  ref.reference = true;
  PassOptions fast;
  fast.sampleRate = kRate;
  std::vector<tap::Use> tapRef, tapFast;
  ref.tapUses = &tapRef;
  fast.tapUses = &tapFast;
  auto t0 = std::chrono::steady_clock::now();
  const Measurement mf = measure(s, fast);
  const double tf = seconds(t0);
  t0 = std::chrono::steady_clock::now();
  const Measurement mr = measure(s, ref);
  const double tr = seconds(t0);
  std::printf("# time %-10s reference %8.3f s   fast %8.3f s   x%.1f   (%zu files, %zu uses, %zu channels, %zu clips)\n", name.c_str(), tr, tf,
              tf > 0 ? tr / tf : 0.0, mr.files.size(), mr.uses.size(), s.channels.size(), s.playlist.size());
  const std::string ur = usesText(mr), uf = usesText(mf);
  expect(ur == uf, (name + ": the fast pass measures exactly what the full mix measures").c_str());
  if (ur != uf) {
    std::printf("--- reference\n%s--- fast\n%s", ur.c_str(), uf.c_str());
  }
  const std::string tr2 = tapText(tapRef, blocks), tf2 = tapText(tapFast, blocks);
  expect(!tapRef.empty() && tr2 == tf2, (name + (blocks ? ": the same play seconds, pitch, jumps and segments" : ": the same play seconds and pitch")).c_str());
  if (tr2 != tf2) {
    std::printf("--- reference tap\n%s--- fast tap\n%s", tr2.c_str(), tf2.c_str());
  }
  const Report rr = build(mr, {}, name), rf = build(mf, {}, name);
  expect(spreadsheetXml(rr) == spreadsheetXml(rf), (name + ": the same spreadsheet, byte for byte").c_str());
  expect(resultsPdf(rr) == resultsPdf(rf), (name + ": the same PDF, byte for byte").c_str());
  double used = 0;
  for (const SongRow& r : rr.songs) {
    used += r.seconds;
  }
  expect(used > 0, (name + ": something is measured").c_str());
}


// K-322: a live engine (the app playing) that plays the project during a measurement never marks
// into it: one plays the same song (its plays would add to the counts), one a copy with the same
// ids that reads other parts of the files (as the person moves clips while it measures). Both
// passes (fast, reference) give exactly what they give on a quiet computer. The live engines'
// own reads go to a pass of their own (two passes at once), which shows they did read.
void liveDuringPass(const Song& s) {
  PassOptions fast;
  fast.sampleRate = kRate;
  PassOptions ref = fast;
  ref.reference = true;
  const std::string quietFast = usesText(measure(s, fast)), quietRef = usesText(measure(s, ref));
  Song moved = s;  // the same ids, other parts of the files
  for (Clip& k : moved.playlist) {
    if (k.kind == ClipKind::Audio) {
      k.audioStart += 0.37, k.audioEnd += 0.37;
    }
  }
  for (Channel& c : moved.channels) {
    if (c.plugin.type == "gl.sampler" && !c.audioClip) {
      c.plugin.params[static_cast<size_t>(pp::SmpStart)] = 0.71f;
    }
  }
  std::vector<tap::WatchFile> watch;
  for (const FileIn& f : scan(s, true).files) {
    watch.push_back(tap::WatchFile{f.path, f.seconds});
  }
  tap::Pass livePass(watch);
  std::atomic<bool> stop{false};
  std::atomic<int> blocks{0};
  auto live = [&](const Song& song) {
    ProgramBuilder b;
    b.setSampleRate(kRate);
    AudioEngine eng(kRate);
    eng.setTapPass(&livePass);
    eng.setProgram(b.build(song));
    eng.setThreads(2);
    eng.setMode(PlayMode::Song);
    eng.setLooping(true);
    eng.play();
    std::vector<float> L(512), R(512);
    while (!stop.load()) {
      eng.process(L.data(), R.data(), 512);
      blocks.fetch_add(1);
    }
  };
  std::thread same(live, std::cref(s)), other(live, std::cref(moved));
  while (blocks.load() < 64) {
    std::this_thread::yield();
  }
  const std::string busyFast = usesText(measure(s, fast)), busyRef = usesText(measure(s, ref));
  stop.store(true);
  same.join();
  other.join();
  expect(busyFast == quietFast, "live playback: the fast pass measures exactly what it measures on a quiet computer");
  expect(busyRef == quietRef, "live playback: the reference pass measures exactly what it measures on a quiet computer");
  if (busyFast != quietFast) {
    std::printf("--- quiet\n%s--- while playing\n%s", quietFast.c_str(), busyFast.c_str());
  }
  expect(livePass.marks() > 0 && !livePass.uses().empty(), "live playback: the live engines did read (into their own pass)");
}
}  // namespace

int main() {
  const std::string scratch = (fs::temp_directory_path() / "gl_track_receipt_speed").generic_string();
  const char* save = std::getenv("GL_RECEIPT_SPEED_SAVE");
  const std::string saveDir = save ? save : "";
  const std::string media = saveDir.empty() ? scratch : saveDir + "/media";  // saved projects keep their files
  if (saveDir.empty()) {
    fs::remove_all(scratch);
  }
  compare("features", features(media + "/features"), saveDir);
  {  // a modulation link: the mixer's sound may matter, so its effects stay (the instruments without files still go)
    Song s = features(media + "/features");
    for (const Channel& c : s.channels) {
      if (c.name == "synth" || c.name == "sampler") {
        ModLink l;
        l.sourceOwner = s.channels.back().id, l.sourceSlot = -1, l.owner = c.id, l.index = channelPluginParam(c.id, pp::SmpStart).index;
        s.modLinks.push_back(l);
        break;
      }
    }
    compare("modulated", s, saveDir);
  }
  compare("patterns", patterns(media + "/patterns"), saveDir, true);
  compare("tempo", tempo(media + "/tempo"), saveDir, true);
  if (saveDir.empty()) {
    liveDuringPass(features(media + "/live"));
  }
  if (std::getenv("GL_RECEIPT_SPEED_LARGE")) {  // the reference pass takes minutes on these
    compare("medium", demoPlus(media + "/medium", 8, 40), saveDir);
    compare("large", demoPlus(media + "/large", 32, 320), saveDir);
  }
  if (saveDir.empty()) {
    fs::remove_all(scratch);
  }
  return finish();
}
