// Gridline — project-level features found in FL Studio's supplied projects:
// pre-12.9 playlists, placed-but-empty patterns, audio-clip channels with
// notes, pattern events (pattern automation), several arrangements, live
// (performance) settings, track groups — imported from synthetic FLP data —
// plus their model, engine and project-file support.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <set>
#include <string>
#include <vector>
#include "../src/core/Arrangements.h"
#include "../src/core/Document.h"
#include "../src/core/Json.h"
#include "../src/core/Params.h"
#include "../src/core/PatternAutomation.h"
#include "../src/core/PluginParams.h"
#include "../src/core/Project.h"
#include "../src/core/WavWriter.h"
#include "../src/core/flp/Flp.h"
#include "../src/engine/AudioEngine.h"
#include "../src/engine/Sample.h"
#include "Check.h"

using namespace gl;

namespace {

// --- FLP builder ---------------------------------------------------------------
struct Builder {
  std::vector<uint8_t> ev;
  void b(uint8_t id, uint8_t v) { ev.push_back(id), ev.push_back(v); }
  void w(uint8_t id, uint16_t v) { ev.push_back(id), ev.push_back(v & 0xFF), ev.push_back(v >> 8); }
  void d(uint8_t id, uint32_t v) {
    ev.push_back(id);
    for (int i = 0; i < 4; ++i) ev.push_back(static_cast<uint8_t>(v >> (8 * i)));
  }
  void data(uint8_t id, const std::vector<uint8_t>& p) {
    ev.push_back(id);
    size_t n = p.size();
    do {
      uint8_t x = n & 0x7F;
      n >>= 7;
      ev.push_back(static_cast<uint8_t>(x | (n ? 0x80 : 0)));
    } while (n);
    ev.insert(ev.end(), p.begin(), p.end());
  }
  void text(uint8_t id, const std::string& s) {
    std::vector<uint8_t> p;
    for (char ch : s) p.push_back(static_cast<uint8_t>(ch)), p.push_back(0);
    p.push_back(0), p.push_back(0);
    data(id, p);
  }
  void mixer(int inserts) {
    for (int ins = 0; ins < inserts; ++ins) {
      data(236, std::vector<uint8_t>(12, 0));
      for (int k = 0; k < 10; ++k) w(98, static_cast<uint16_t>(k));
      d(147, 0);
    }
  }
  std::vector<uint8_t> file(int ppq) const {
    std::vector<uint8_t> f = {'F', 'L', 'h', 'd', 6, 0, 0, 0, 0, 0, 1, 0, static_cast<uint8_t>(ppq), 0, 'F', 'L', 'd', 't'};
    uint32_t n = static_cast<uint32_t>(ev.size());
    for (int i = 0; i < 4; ++i) f.push_back(static_cast<uint8_t>(n >> (8 * i)));
    f.insert(f.end(), ev.begin(), ev.end());
    return f;
  }
};
struct Bytes {
  std::vector<uint8_t> v;
  explicit Bytes(size_t n) : v(n, 0) {}
  void u16(size_t at, uint16_t x) { v[at] = x & 0xFF, v[at + 1] = x >> 8; }
  void u32(size_t at, uint32_t x) { for (int i = 0; i < 4; ++i) v[at + static_cast<size_t>(i)] = static_cast<uint8_t>(x >> (8 * i)); }
  void f32(size_t at, float x) { uint32_t b; std::memcpy(&b, &x, 4); u32(at, b); }
  void f64(size_t at, double x) { uint64_t b; std::memcpy(&b, &x, 8); u32(at, static_cast<uint32_t>(b)), u32(at + 4, static_cast<uint32_t>(b >> 32)); }
};
// One playlist record (`stride` 32 = FL ≤ 20, 80 = FL 21+); `rv` = reversed track field.
std::vector<uint8_t> item(uint32_t stride, uint32_t pos, uint16_t idx, uint32_t len, uint16_t rv, uint16_t flags = 0x40) {
  Bytes r(stride);
  r.u32(0, pos);
  r.u16(4, 0x5000);
  r.u16(6, idx);
  r.u32(8, len);
  r.u16(12, rv);
  r.u16(18, flags);
  r.f32(24, -1.0f), r.f32(28, -1.0f);
  return r.v;
}
// A 24-byte note record.
std::vector<uint8_t> note(uint32_t pos, uint16_t ch, uint32_t len, uint16_t key) {
  Bytes n(24);
  n.u32(0, pos), n.u16(6, ch), n.u32(8, len), n.u16(12, key);
  n.v[16] = 120, n.v[18] = 64, n.v[20] = 64, n.v[21] = 100, n.v[22] = 128, n.v[23] = 128;
  return n.v;
}
// Track record (238): index (1-based), live settings, grouped flag.
std::vector<uint8_t> track(uint32_t index, uint32_t motion = 0, uint32_t press = 0, uint32_t sync = 5, uint32_t psync = 0, bool grouped = false) {
  Bytes t(66);
  t.u32(0, index), t.v[12] = 1, t.f32(13, 1.0f);
  t.u32(22, motion), t.u32(26, press), t.u32(30, sync), t.u32(34, 0), t.u32(38, 1), t.u32(42, psync);
  t.v[46] = grouped ? 1 : 0;
  return t.v;
}

bool importBytes(const Builder& B, Song& s, flp::Report& rep, const char* name) {
  std::string path = (std::filesystem::temp_directory_path() / name).string();
  if (FILE* fp = std::fopen(path.c_str(), "wb")) {
    auto bytes = B.file(96);
    std::fwrite(bytes.data(), 1, bytes.size(), fp);
    std::fclose(fp);
  }
  std::string err;
  bool ok = importFlp(path, s, rep, &err, "/nonexistent");
  std::remove(path.c_str());
  return ok;
}

std::string writeDcWav(const std::string& name, size_t frames) {
  AudioBuffer b(2, frames);
  for (size_t i = 0; i < frames; ++i) b.at(0, i) = b.at(1, i) = 0.9f;
  std::string path = (std::filesystem::temp_directory_path() / name).string();
  writeWav(path, b, 44100, 16);
  sampleCache().clear();
  return path;
}

// A song with one sampler channel (a DC sample: any note makes the output jump).
Song dcSong(const std::string& wav) {
  Song s = makeEmptySong();
  s.tempo = 120;
  s.channels.clear();
  Channel ch;
  ch.id = s.allocateId();
  ch.name = "DC";
  ch.plugin = defaultPluginState("gl.sampler");
  ch.plugin.data = wav;
  ch.mixerTrack = s.masterId();
  ch.gain = 1.0f;
  s.channels.push_back(ch);
  for (auto& m : s.mixer) m.volume = 1.0f;
  return s;
}

ObjectId addPatternWithNote(Song& s, const std::string& name, Tick at, Tick len) {
  Pattern p;
  p.id = s.allocateId();
  p.name = name;
  p.notes[s.channels[0].id] = {Note(at, len, 60, 100)};
  s.patterns.push_back(p);
  return p.id;
}

ObjectId addClip(Song& s, ObjectId pattern, int trk, Tick start, Tick len) {
  Clip c;
  c.id = s.allocateId();
  c.pattern = pattern;
  c.track = trk;
  c.start = start;
  c.length = len;
  s.playlist.push_back(c);
  return c.id;
}

// Runs the engine in blocks of 64 samples until `samples` have been rendered;
// returns the first sample index whose output exceeds `thr` (or -1).
long runUntil(AudioEngine& eng, long& clock, long samples, float thr = 0.05f) {
  std::vector<float> L(64), R(64);
  long first = -1;
  for (; clock < samples; clock += 64) {
    eng.process(L.data(), R.data(), 64);
    for (int i = 0; i < 64 && first < 0; ++i)
      if (std::fabs(L[static_cast<size_t>(i)]) > thr) first = clock + i;
  }
  return first;
}

double samplesPerTick(double bpm, int ppq) { return 44100.0 * 60.0 / (bpm * ppq); }

}  // namespace

int main() {
  // =========================================================== FLP: pre-12.9 file
  // No Arrangement.New events: the playlist, markers and tracks follow the last
  // channel; tracks count down from 198; an audio-clip channel holds notes; a
  // pattern is placed without being stored; a "start" marker makes a live area.
  {
    Builder B;
    B.text(199, "11.0.3");
    B.d(156, 120000);
    B.w(65, 1);
    B.data(224, note(0, 1, 96, 60));  // note on the audio-clip channel
    B.w(64, 0), B.b(21, 2), B.text(201, "Unknown synth"), B.text(203, "Lead");
    B.w(64, 1), B.b(21, 4), B.text(203, "Loop");
    B.text(196, "/nonexistent/loop.wav");
    std::vector<uint8_t> items;
    for (auto& r : {item(32, 0, 0x5001, 384, 198), item(32, 384, 0x5007, 384, 196), item(32, 0, 1, 384, 197)})
      items.insert(items.end(), r.begin(), r.end());
    B.data(233, items);
    B.d(148, 0), B.text(205, "");
    B.d(148, 384), B.text(205, "");
    B.d(148, (5u << 24) | 768u), B.text(205, "Start");
    B.data(238, track(1, 3, 3, 3, 6));  // next then hold · toggle · beat · sync on the clip length
    B.data(238, track(2, 0, 0, 5, 0, true));
    B.data(238, track(3, 0, 0, 5, 0, true));
    B.mixer(2);
    Song s;
    flp::Report rep;
    expect(importBytes(B, s, rep, "gl_corpus_legacy.flp"), "legacy: import succeeds");
    expect(s.channels.size() == 2 && s.channels[1].name == "Loop" && s.channels[1].plugin.type == "gl.sampler",
           "legacy: audio-clip channel is in the rack");
    size_t notes = 0;
    for (const auto& p : s.patterns)
      for (const auto& [ch, ns] : p.notes) notes += ns.size();
    expect(notes == 1, "legacy: notes on the audio-clip channel import");
    expect(s.patterns.size() == 2 && s.patterns[1].name == "Pattern 7", "legacy: placed but unstored pattern is created");
    int pat = 0, audio = 0;
    for (const auto& c : s.playlist) (c.kind == ClipKind::Pattern ? pat : audio) += 1;
    expect(pat == 2 && audio == 1, "legacy: playlist of a file without arrangements");
    bool tracksOk = s.playlist.size() == 3 && s.playlist[0].track == 0 && s.playlist[1].track == 2 && s.playlist[2].track == 1;
    expect(tracksOk, "legacy: tracks count down from 198");
    expect(s.markers.size() == 3 && s.live.areaEnd == 768, "legacy: start marker → live area");
    expect(s.tracks[0].follow == FollowMode::NextHold && s.tracks[0].pad == PadMode::Toggle && s.tracks[0].launch == LaunchGrid::Beat &&
               s.tracks[0].syncStart == LaunchGrid::ClipLength,
           "legacy: track live settings");
    expect(s.tracks[0].groupSize == 2 && s.tracks[1].groupSize == 0, "legacy: grouped tracks fold under the track above");
    expect(s.arrangements.empty(), "legacy: one default arrangement");
  }

  // =========================================================== FLP: several arrangements
  {
    Builder B;
    B.text(199, "25.2.3.5164");
    B.d(156, 140000);
    B.w(100, 1);  // current = second
    B.w(65, 1);
    B.data(224, note(0, 0, 96, 60));
    B.w(65, 2);
    B.data(224, note(0, 0, 96, 64));
    B.w(64, 0), B.b(21, 0), B.text(203, "Kick");
    B.w(99, 0), B.text(241, "Verse");
    {
      auto r = item(80, 0, 0x5001, 384, 499);
      B.data(233, r);
    }
    B.d(148, 0), B.text(205, "A");
    B.data(238, track(1));
    B.w(99, 1), B.text(241, "Chorus");
    {
      std::vector<uint8_t> items;
      for (auto& r : {item(80, 0, 0x5002, 384, 499), item(80, 384, 0x5001, 384, 498)}) items.insert(items.end(), r.begin(), r.end());
      B.data(233, items);
    }
    B.data(238, track(1));
    B.mixer(2);
    Song s;
    flp::Report rep;
    expect(importBytes(B, s, rep, "gl_corpus_arr.flp"), "arrangements: import succeeds");
    expect(arrangementCount(s) == 2 && arrangementName(s, 0) == "Verse" && arrangementName(s, 1) == "Chorus" && s.currentArrangement == 1,
           "arrangements: names and current");
    expect(s.playlist.size() == 2 && s.markers.empty(), "arrangements: current content is the song's playlist");
    expect(s.arrangements[0].playlist.size() == 1 && s.arrangements[0].markers.size() == 1 && s.arrangements[1].playlist.empty(),
           "arrangements: the other one is parked");
    expect(rep.counts["pattern clips"] == 2 && rep.counts["arrangements"] == 2, "arrangements: report counts the current one");
    // Switch and back.
    Song t = s;
    expect(selectArrangement(t, 0) && t.playlist.size() == 1 && t.markers.size() == 1 && t.arrangements[1].playlist.size() == 2,
           "arrangements: switching swaps the content");
    selectArrangement(t, 1);
    expect(t == s, "arrangements: switching back restores the song exactly");
  }

  // =========================================================== FLP: pattern events
  {
    Builder B;
    B.text(199, "25.1.0.4858");
    B.d(156, 120000);
    B.w(65, 1);
    {
      Bytes ev(36);  // channel 0 volume at 0 and 48; insert 1 slot 0 mix at 24
      ev.u32(0, 0), ev.u32(4, 0x00000000u), ev.u32(8, 12800);
      ev.u32(12, 24), ev.u32(16, ((0x7000u + 64u) << 16) | 0x1FC0u), ev.u32(20, 6400);
      ev.u32(24, 48), ev.u32(28, 0x00000000u), ev.u32(32, 3200);
      B.data(223, ev.v);
    }
    B.data(224, note(96, 1, 0, 60));  // a note on the automation channel
    B.w(64, 0), B.b(21, 0), B.text(203, "Kick"), B.b(22, 1);
    B.w(64, 1), B.b(21, 5), B.text(203, "Pan auto");
    {
      Bytes a(21 + 48 + 112);
      a.u32(0, 1), a.u32(17, 2);
      a.f64(21, 0.0), a.f64(29, 0.0);
      a.f64(45, 1.0), a.f64(53, 1.0);
      B.data(234, a.v);
      Bytes l(20);
      l.u16(2, 1), l.u32(8, 0x00000001u), l.u32(12, 8);  // → channel 0 pan
      B.data(227, l.v);
    }
    B.w(99, 0);
    B.data(233, item(80, 0, 0x5001, 384, 499));
    B.data(238, track(1));
    B.mixer(2);
    Song s;
    flp::Report rep;
    expect(importBytes(B, s, rep, "gl_corpus_events.flp"), "pattern events: import succeeds");
    const Pattern& p = s.patterns[0];
    const ObjectId kick = s.channels[0].id;
    const PatternAutomation* vol = findPatternLane(p, kick, static_cast<uint32_t>(ChannelParam::Volume));
    const PatternAutomation* pan = findPatternLane(p, kick, static_cast<uint32_t>(ChannelParam::Pan));
    expect(vol && vol->points.size() == 2 && vol->points[0].pos == 0 && vol->points[1].pos == 48 &&
               std::fabs(vol->points[0].value - 0.8f) < 1e-4 && std::fabs(vol->points[1].value - 0.2f) < 1e-4 &&
               vol->points[0].shape == AutoShape::Hold,
           "pattern events: channel volume lane (raw 0..12800)");
    bool mix = false;
    for (const auto& l : p.automation)
      if (s.mixerTrack(l.owner) && l.points.size() == 1 && l.points[0].pos == 24 && std::fabs(l.points[0].value - 0.4f) < 1e-4) mix = true;
    expect(mix, "pattern events: insert volume lane");
    expect(pan && pan->points.size() == 2 && pan->points[0].pos == 96 && pan->points[1].pos == 96 + 96,
           "pattern events: a note on an automation channel copies the curve");
    expect(rep.counts["pattern events imported"] == 3, "pattern events: report counts");
  }
  // Pattern events on a plugin parameter: the recorded integer goes through the
  // decoder's rawNorm (Fruit Kick amp decay, control range 0..256).
  {
    Builder B;
    B.text(199, "25.1.0.4858");
    B.d(156, 120000);
    B.w(65, 1);
    {
      Bytes ev(36);  // channel 0, plugin parameter 3: 64, 192; parameter 50 (not linked)
      ev.u32(0, 0), ev.u32(4, 0x8003u), ev.u32(8, 64);
      ev.u32(12, 48), ev.u32(16, 0x8003u), ev.u32(20, 192);
      ev.u32(24, 96), ev.u32(28, 0x8032u), ev.u32(32, 7);
      B.data(223, ev.v);
    }
    B.w(64, 0), B.b(21, 2), B.text(201, "Fruit Kick");
    {
      Bytes st(32);  // version 0, max / min freq, freq decay, amp decay 128, click, drive, seed
      st.u32(8, static_cast<uint32_t>(-600)), st.u32(12, 64), st.u32(16, 128), st.u32(20, 32);
      B.data(213, st.v);
    }
    B.text(203, "Kick");
    B.w(99, 0);
    B.data(233, item(80, 0, 0x5001, 384, 499));
    B.data(238, track(1));
    B.mixer(2);
    Song s;
    flp::Report rep;
    expect(importBytes(B, s, rep, "gl_corpus_plugin_events.flp"), "plugin pattern events: import succeeds");
    expect(s.channels.size() == 1 && s.channels[0].plugin.type == "gl.kick", "plugin pattern events: Fruit Kick → Kick");
    const ParamId id = channelPluginParam(s.channels[0].id, pp::KickDecay);
    const PatternAutomation* lane = findPatternLane(s.patterns[0], id.owner, id.index);
    auto d = describeParam(s, id);
    auto decay = [](double v) { return std::clamp(0.05 * std::pow(40.0, v / 256.0), 0.05, 2.0); };
    expect(lane && d && lane->points.size() == 2 && lane->points[1].pos == 48 &&
               std::fabs(lane->points[0].value - d->toNormalized(decay(64))) < 1e-4 &&
               std::fabs(lane->points[1].value - d->toNormalized(decay(192))) < 1e-4,
           "plugin pattern events: decay lane from raw 0..256 values");
    expect(rep.counts["pattern events imported"] == 2 && rep.counts["pattern automation lanes"] == 1, "plugin pattern events: counts");
    bool warned = false;
    for (const auto& w : rep.warnings)
      if (w.find("plugin parameter 50 (Fruit Kick) (1)") != std::string::npos) warned = true;
    expect(warned, "plugin pattern events: unlinked parameter reported");
  }

  // =========================================================== arrangements: model + document
  {
    Song s = makeEmptySong();
    ObjectId p = s.patterns[0].id;
    addClip(s, p, 0, 0, 384);
    Document doc(s);
    doc.editSong("new arrangement", [](Song& x) { selectArrangement(x, addArrangement(x, "", false)); });
    expect(arrangementCount(doc.song()) == 2 && doc.song().playlist.empty() && doc.song().currentArrangement == 1,
           "arrangements: add + switch");
    doc.editSong("duplicate", [](Song& x) {
      selectArrangement(x, 0);
      selectArrangement(x, addArrangement(x, "Copy", true));
    });
    const Song& d = doc.song();
    expect(d.playlist.size() == 1 && d.playlist[0].id != d.arrangements[0].playlist[0].id && arrangementName(d, 2) == "Copy",
           "arrangements: duplicate copies the current one");
    std::set<ObjectId> ids;
    size_t total = 0;
    forEachArrangementClip(d, [&](const Clip& c) { ids.insert(c.id), ++total; });
    expect(ids.size() == total && total == 2, "arrangements: duplicated clips get new ids");
    doc.removePattern(p);
    size_t left = 0;
    forEachArrangementClip(doc.song(), [&](const Clip&) { ++left; });
    expect(left == 0, "arrangements: deleting a pattern removes it from every arrangement");
    doc.undo();
    left = 0;
    forEachArrangementClip(doc.song(), [&](const Clip&) { ++left; });
    expect(left == 2, "arrangements: undo restores parked clips");
    doc.editSong("rename", [](Song& x) { renameArrangement(x, 0, "Intro"); });
    doc.editSong("remove", [](Song& x) { removeArrangement(x, 2); });
    expect(arrangementCount(doc.song()) == 2 && arrangementName(doc.song(), 0) == "Intro" && doc.song().currentArrangement == 1,
           "arrangements: rename, remove the current one");
    Song one = makeEmptySong();
    expect(!removeArrangement(one, 0), "arrangements: the last one stays");
    // PPQ change rescales parked arrangements and pattern automation.
    Song q = doc.song();
    q.patterns[0].automation.push_back(PatternAutomation{q.channels.empty() ? q.masterId() : q.channels[0].id, 0, {{48, 0.5f, 0, AutoShape::Hold}}});
    q.live.areaEnd = 96;
    changePpq(q, 192);
    bool scaled = q.patterns[0].automation[0].points[0].pos == 96 && q.live.areaEnd == 192;
    for (const auto& a : q.arrangements)
      for (const auto& c : a.playlist) scaled = scaled && c.length == 768;
    expect(scaled, "PPQ change rescales arrangements, live area, pattern automation");
  }

  // =========================================================== project round-trip
  {
    Song s = makeEmptySong();
    Channel ch;
    ch.id = s.allocateId();
    ch.plugin = defaultPluginState("gl.sampler");
    ch.mixerTrack = s.masterId();
    s.channels.push_back(ch);
    s.patterns[0].automation.push_back(PatternAutomation{ch.id, static_cast<uint32_t>(ChannelParam::Volume),
                                                         {{0, 0.25f, 0, AutoShape::Hold}, {96, 0.75f, 0.5f, AutoShape::Curve}}});
    addClip(s, s.patterns[0].id, 1, 0, 384);
    s.tracks[1].follow = FollowMode::Shuffle, s.tracks[1].pad = PadMode::Gate, s.tracks[1].launch = LaunchGrid::Beat;
    s.tracks[1].syncStart = LaunchGrid::FourBeats, s.tracks[0].groupSize = 3, s.tracks[0].folded = true;
    s.live.areaEnd = 768;
    s.markers = {{0, "A", 0, 0}};
    selectArrangement(s, addArrangement(s, "B", true));
    s.markers.push_back({384, "B only", 0, 0});
    Song back;
    std::string err;
    bool ok = songFromJson(songToJson(s), back, &err);
    expect(ok && back == s, "round-trip: pattern automation, live settings, arrangements");
    Song plain = makeEmptySong();
    Json j = songToJson(plain);
    expect(!j.find("arrangements") && !j.find("live") && !j.find("currentArrangement"), "round-trip: no new keys when unused");
    bool noPatKey = true;
    if (const Json* pats = j.find("patterns"))
      for (const auto& jp : pats->array) noPatKey = noPatKey && !jp.find("automation");
    expect(noPatKey, "round-trip: no pattern automation key when unused");
  }

  std::string wav = writeDcWav("gl_corpus_dc.wav", 44100 * 4);
  const double spt = samplesPerTick(120, 96);  // 229.6875 samples per tick

  // =========================================================== engine: pattern automation
  {
    Song s = dcSong(wav);
    ObjectId p = addPatternWithNote(s, "P", 0, 384);
    const uint32_t vol = static_cast<uint32_t>(ChannelParam::Volume);
    setPatternLane(*s.pattern(p), s.channels[0].id, vol, {{0, 0.1f, 0, AutoShape::Hold}, {192, 0.6f, 0, AutoShape::Hold}});
    ProgramBuilder pb;
    AudioEngine eng(44100);
    eng.setProgram(pb.build(s));
    eng.setMode(PlayMode::Pattern);
    eng.setPattern(p);
    eng.play();
    long clock = 0;
    runUntil(eng, clock, static_cast<long>(100 * spt));
    auto d = describeParam(s, channelParam(s.channels[0].id, ChannelParam::Volume));
    float early = eng.program()->channels[0].modVolume;
    runUntil(eng, clock, static_cast<long>(250 * spt));
    float late = eng.program()->channels[0].modVolume;
    expect(std::fabs(early - d->fromNormalized(0.1)) < 1e-4 && std::fabs(late - d->fromNormalized(0.6)) < 1e-4,
           "pattern automation plays in pattern mode");
    // Song mode: the lane follows the clip (pattern placed at bar 2).
    addClip(s, p, 0, 384, 384);
    AudioEngine eng2(44100);
    ProgramBuilder pb2;
    eng2.setProgram(pb2.build(s));
    eng2.setMode(PlayMode::Song);
    eng2.play();
    clock = 0;
    runUntil(eng2, clock, static_cast<long>((384 + 100) * spt));
    float a = eng2.program()->channels[0].modVolume;
    runUntil(eng2, clock, static_cast<long>((384 + 250) * spt));
    float b = eng2.program()->channels[0].modVolume;
    expect(std::fabs(a - d->fromNormalized(0.1)) < 1e-4 && std::fabs(b - d->fromNormalized(0.6)) < 1e-4,
           "pattern automation follows the pattern clip in song mode");
  }

  // =========================================================== engine: live mode
  {
    Song s = dcSong(wav);
    ObjectId pa = addPatternWithNote(s, "A", 0, 48);
    ObjectId pb = addPatternWithNote(s, "B", 0, 48);
    ObjectId ca = addClip(s, pa, 0, 0, 384);
    ObjectId cb = addClip(s, pb, 0, 384, 384);
    addClip(s, pa, 1, 1536, 384);  // song area
    s.live.areaEnd = 768;
    s.tracks[0].launch = LaunchGrid::FourBeats;
    {
      ProgramBuilder b0;
      auto prog = b0.build(s);
      expect(prog->liveClips.size() == 2 && prog->liveTracks.size() == 1 && prog->channels[0].song.size() == 1,
             "live: live-area clips are compiled for launching, not for the song");
    }
    ProgramBuilder b1;
    AudioEngine eng(44100);
    eng.setProgram(b1.build(s));
    eng.setMode(PlayMode::Live);
    eng.play();
    long clock = 0;
    long first = runUntil(eng, clock, static_cast<long>(100 * spt));
    expect(first < 0, "live: silence before any launch");
    eng.launchClip(ca);
    first = runUntil(eng, clock, static_cast<long>(300 * spt));
    expect(first < 0 && eng.liveQueued(0) == ca && eng.liveActive(0) == kNoId, "live: launch waits for the grid");
    first = runUntil(eng, clock, static_cast<long>(420 * spt));
    expect(eng.liveActive(0) == ca && first >= 0 && std::labs(first - static_cast<long>(std::ceil(384 * spt))) <= 1,
           "live: clip starts on the next 4 beats");
    // One clip per track: launching B replaces A at the next grid; follow = loop.
    eng.launchClip(cb);
    runUntil(eng, clock, static_cast<long>(760 * spt));
    expect(eng.liveActive(0) == ca && eng.liveQueued(0) == cb, "live: replacement waits for the grid");
    runUntil(eng, clock, static_cast<long>(800 * spt));
    expect(eng.liveActive(0) == cb && eng.liveQueued(0) == kNoId, "live: one clip per track");
    eng.stopLiveTrack(0);
    runUntil(eng, clock, static_cast<long>(1160 * spt));
    expect(eng.liveActive(0) == kNoId, "live: stop at the grid");

    // Follow: next clip after the end; toggle pads stop on the second press.
    s.tracks[0].follow = FollowMode::Next;
    s.tracks[0].pad = PadMode::Toggle;
    s.tracks[0].launch = LaunchGrid::Now;
    ProgramBuilder b2;
    AudioEngine e2(44100);
    e2.setProgram(b2.build(s));
    e2.setMode(PlayMode::Live);
    e2.launchClip(ca);  // starts the transport
    long c2 = 0;
    runUntil(e2, c2, static_cast<long>(100 * spt));
    bool startedNow = e2.liveActive(0) == ca && e2.playing();
    runUntil(e2, c2, static_cast<long>(400 * spt));
    bool followed = e2.liveActive(0) == cb;
    runUntil(e2, c2, static_cast<long>(790 * spt));
    bool wrapped = e2.liveActive(0) == ca;
    e2.launchClip(ca);  // toggle: stops
    runUntil(e2, c2, static_cast<long>(800 * spt));
    expect(startedNow && followed && wrapped && e2.liveActive(0) == kNoId, "live: launch starts transport; follow next; toggle stops");

    // Gate pads stop when released; the song mode ignores the live area.
    s.tracks[0].pad = PadMode::Gate;
    s.tracks[0].follow = FollowMode::Loop;
    ProgramBuilder b3;
    AudioEngine e3(44100);
    e3.setProgram(b3.build(s));
    e3.setMode(PlayMode::Live);
    e3.launchClip(cb);
    long c3 = 0;
    runUntil(e3, c3, static_cast<long>(50 * spt));
    bool held = e3.liveActive(0) == cb;
    e3.releaseClip(cb);
    runUntil(e3, c3, static_cast<long>(60 * spt));
    expect(held && e3.liveActive(0) == kNoId, "live: gate pads play while held");

    ProgramBuilder b4;
    AudioEngine e4(44100);
    e4.setProgram(b4.build(s));
    e4.setMode(PlayMode::Song);
    e4.play();
    long c4 = 0;
    long f4 = runUntil(e4, c4, static_cast<long>(1600 * spt));
    expect(f4 >= 0 && std::labs(f4 - static_cast<long>(std::ceil(1536 * spt))) <= 1, "live: song mode skips the live area");
  }
  std::remove(wav.c_str());

  // =========================================================== send buses (templates)
  {
    // Master + inserts 1, 2 (default) + 3, 4: every insert routes to the
    // master and, at level 0, to inserts 3 and 4 → 3 and 4 are send buses.
    Builder B;
    B.text(199, "24.2.99.4720");
    B.d(156, 120000);
    B.w(64, 0), B.b(21, 0), B.text(203, "Kick");
    B.w(99, 0);
    B.data(238, track(1));
    B.w(103, 6);
    std::vector<uint8_t> params;
    auto rec = [&](int ins, int to, int level) {  // FL 25 route level record (kind 32)
      Bytes r(12);
      r.v[4] = static_cast<uint8_t>(to), r.v[5] = 32, r.u16(6, static_cast<uint16_t>(ins << 6)), r.u32(8, static_cast<uint32_t>(level));
      params.insert(params.end(), r.v.begin(), r.v.end());
    };
    for (int ins = 0; ins < 6; ++ins) {
      B.data(236, std::vector<uint8_t>(12, 0));
      if (ins > 0) B.data(235, std::vector<uint8_t>{1, 0, 0, 1, 1});
      for (int k = 0; k < 10; ++k) B.w(98, static_cast<uint16_t>(k));
      B.d(147, ins == 0 ? 0 : 0xFFFFFFFFu);
      if (ins > 0) rec(ins, 3, 0), rec(ins, 4, 0);
    }
    B.data(225, params);
    Song s;
    flp::Report rep;
    expect(importBytes(B, s, rep, "gl_corpus_sends.flp"), "send buses: import succeeds");
    int toBus = 0;
    for (size_t i = 1; i < s.mixer.size(); ++i)
      for (const auto& sd : s.mixer[i].sends)
        if (s.mixerIndex(sd.to) >= 3 && !sd.sidechain && sd.level == 0.0f) ++toBus;
    expect(s.mixer.size() >= 5 && toBus == 4, "send buses: kept, with level-0 sends from the other inserts");
  }

  // =========================================================== text: lone surrogates, JSON control characters
  {
    const uint8_t raw[] = {'A', 0, 0x3D, 0xD8, 'G', 0, 0x3D, 0xD8, 0x48, 0xDC, 0, 0};  // A, lone high, G, pair, NUL
    flp::Event e;
    e.data = raw;
    e.size = sizeof raw;
    expect(e.text() == "A\xEF\xBF\xBDG\xF0\x9F\x91\x88", "UTF-16 text: lone surrogate becomes U+FFFD");
    Json j = Json::makeString(std::string("a\x1d" "b"));
    Json back;
    size_t pos = 0;
    bool ok = jsonParse(jsonStringify(j), back, &pos);
    expect(ok && back.str == std::string("a\x1d" "b"), "JSON escapes control characters");
  }

  // =========================================================== RTF comments
  {
    std::string rtf = "{\\rtf1\\ansi\\deff0{\\fonttbl{\\f0\\fswiss Arial;}}\r\n\\viewkind4\\uc1\\pard\\f0\\fs16 Demo song.\\par\r\n"
                      "\\par\r\n(c) \\'e9t\\'e9 {\\*\\hidden x}done\\par\r\n}\r\n";
    expect(flp::rtfToText(rtf) == "Demo song.\n\n(c) \xC3\xA9t\xC3\xA9 done", "RTF comments become plain text");
  }
  return finish();
}
