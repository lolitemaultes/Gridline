// Gridline - K-76 tests: tracker modules (.xm, .mod) open as projects.
//   * the module reader on synthetic XM / MOD files (written here), truncated and mutated files;
//   * the tracker cell streams notes carry (round trip, run lengths, end marks);
//   * the channel logic (pitch arithmetic, effects) against hand-computed values;
//   * the song built from a module: notes per voice, handovers across patterns, pattern variants,
//     clips for breaks and loops, tempo, global volume, loop marker;
//   * the Tracker Sampler: sound, pitch, volume and pan law sample by sample, block-size
//     invariance, extremes, short parameter vectors; a whole song rendered through the engine.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include "../../src/core/Params.h"
#include "../../src/core/PluginInfo.h"
#include "../../src/core/Project.h"
#include "../../src/core/ProjectArchive.h"
#include "../../src/core/ProjectFiles.h"
#include "../../src/core/TrackerData.h"
#include "../../src/core/TrackerImport.h"
#include "../../src/core/TrackerStream.h"
#include "../../src/engine/Generators.h"
#include "../../src/engine/Render.h"
#include "../../src/packs/Packs.h"
#include "../Check.h"

using namespace gl;
namespace trk = gl::tracker;
namespace fs = std::filesystem;

namespace {

void expect(bool ok, const std::string& name) { expect(ok, name.c_str()); }

// ---------------------------------------------------------------- synthetic XM files

struct XCell {
  uint8_t note = 0, instr = 0, vol = 0, efx = 0, param = 0;
};
struct XSmp {
  std::vector<int16_t> data;  // 8-bit values (-128..127) unless sixteen
  bool sixteen = false;
  uint32_t loopStart = 0, loopLen = 0;
  uint8_t loop = 0, vol = 64, pan = 128;
  int8_t fine = 0, rel = 0;
};
struct XIns {
  std::string name = "ins";
  std::vector<XSmp> smps;
  std::vector<std::pair<int, int>> volEnv;
  uint8_t volType = 0, volSustain = 0;
  uint16_t fadeout = 0;
  uint8_t vibType = 0, vibSweep = 0, vibDepth = 0, vibRate = 0;
};
struct XSong {
  std::string title = "Test song";
  int channels = 2, speed = 6, bpm = 125;
  bool linear = true;
  std::vector<int> orders{0};
  int restart = 0;
  std::vector<std::vector<std::vector<XCell>>> patterns;  // [pattern][row][channel]
  std::vector<XIns> ins;
  // Rows x channels of empty cells.
  std::vector<std::vector<XCell>>& pattern(int rows) {
    patterns.emplace_back(static_cast<size_t>(rows), std::vector<XCell>(static_cast<size_t>(channels)));
    return patterns.back();
  }
};

struct W {
  std::vector<uint8_t> b;
  void u8(int v) { b.push_back(static_cast<uint8_t>(v)); }
  void u16(int v) {
    u8(v & 0xFF);
    u8((v >> 8) & 0xFF);
  }
  void u32(uint32_t v) {
    u16(static_cast<int>(v & 0xFFFF));
    u16(static_cast<int>(v >> 16));
  }
  void text(const std::string& s, size_t n) {
    for (size_t i = 0; i < n; ++i) u8(i < s.size() ? s[i] : 0);
  }
};

std::vector<uint8_t> writeXm(const XSong& s) {
  W w;
  w.text("Extended Module: ", 17);
  w.text(s.title, 20);
  w.u8(0x1A);
  w.text("Gridline test", 20);
  w.u16(0x0104);
  w.u32(276);
  w.u16(static_cast<int>(s.orders.size()));
  w.u16(s.restart);
  w.u16(s.channels);
  w.u16(static_cast<int>(s.patterns.size()));
  w.u16(static_cast<int>(s.ins.size()));
  w.u16(s.linear ? 1 : 0);
  w.u16(s.speed);
  w.u16(s.bpm);
  for (int i = 0; i < 256; ++i) w.u8(i < static_cast<int>(s.orders.size()) ? s.orders[static_cast<size_t>(i)] : 0);
  for (const auto& p : s.patterns) {
    W d;
    for (const auto& row : p)
      for (const auto& c : row) {
        d.u8(c.note);
        d.u8(c.instr);
        d.u8(c.vol);
        d.u8(c.efx);
        d.u8(c.param);
      }
    w.u32(9);
    w.u8(0);
    w.u16(static_cast<int>(p.size()));
    w.u16(static_cast<int>(d.b.size()));
    w.b.insert(w.b.end(), d.b.begin(), d.b.end());
  }
  for (const auto& in : s.ins) {
    const size_t start = w.b.size();
    w.u32(in.smps.empty() ? 29 : 263);
    w.text(in.name, 22);
    w.u8(0);
    w.u16(static_cast<int>(in.smps.size()));
    if (!in.smps.empty()) {
      w.u32(40);
      for (int i = 0; i < 96; ++i) w.u8(0);
      for (int i = 0; i < 12; ++i) {
        const bool has = i < static_cast<int>(in.volEnv.size());
        w.u16(has ? in.volEnv[static_cast<size_t>(i)].first : 0);
        w.u16(has ? in.volEnv[static_cast<size_t>(i)].second : 0);
      }
      for (int i = 0; i < 24; ++i) w.u16(0);
      w.u8(static_cast<int>(in.volEnv.size()));
      w.u8(0);
      w.u8(in.volSustain);
      w.u8(0);
      w.u8(0);
      w.u8(0);
      w.u8(0);
      w.u8(0);
      w.u8(in.volType);
      w.u8(0);
      w.u8(in.vibType);
      w.u8(in.vibSweep);
      w.u8(in.vibDepth);
      w.u8(in.vibRate);
      w.u16(in.fadeout);
      while (w.b.size() < start + 263) w.u8(0);
      for (const auto& sm : in.smps) {
        const uint32_t bytes = static_cast<uint32_t>(sm.data.size() * (sm.sixteen ? 2 : 1));
        const uint32_t mul = sm.sixteen ? 2 : 1;
        w.u32(bytes);
        w.u32(sm.loopStart * mul);
        w.u32(sm.loopLen * mul);
        w.u8(sm.vol);
        w.u8(static_cast<uint8_t>(sm.fine));
        w.u8(sm.loop | (sm.sixteen ? 16 : 0));
        w.u8(sm.pan);
        w.u8(static_cast<uint8_t>(sm.rel));
        w.u8(0);
        w.text("smp", 22);
      }
      for (const auto& sm : in.smps) {
        int prev = 0;
        for (int16_t v : sm.data) {
          const int dlt = v - prev;
          prev = v;
          if (sm.sixteen) w.u16(dlt & 0xFFFF);
          else w.u8(dlt & 0xFF);
        }
      }
    }
  }
  return w.b;
}

// A sample: a ramp of 8-bit values, `len` frames.
XSmp rampSample(int len, uint8_t loop = 0) {
  XSmp s;
  for (int i = 0; i < len; ++i) s.data.push_back(static_cast<int16_t>(((i * 7) % 200) - 100));
  s.loop = loop;
  if (loop) {
    s.loopStart = 0;
    s.loopLen = static_cast<uint32_t>(len);
  }
  return s;
}

XSong basicSong(int rows = 16) {
  XSong s;
  s.channels = 2;
  XIns in;
  in.name = "Lead";
  in.smps.push_back(rampSample(2000));
  s.ins.push_back(in);
  s.pattern(rows);
  return s;
}

std::string tempDir(const std::string& tag) {
  const fs::path d = fs::temp_directory_path() / ("gridline-k76-" + tag);
  std::error_code ec;
  fs::remove_all(d, ec);
  fs::create_directories(d, ec);
  return d.u8string();
}

trk::Module parse(const XSong& s, bool* ok = nullptr) {
  trk::Module m;
  std::string err;
  const bool r = trk::parseModule(writeXm(s), m, &err);
  if (ok) *ok = r;
  return m;
}

bool build(const XSong& s, Song& song, trk::ImportReport& rep, const std::string& folder = "") {
  trk::ImportOptions o;
  o.sampleFolder = folder;
  std::string err;
  return trk::importModule(writeXm(s), "test", o, song, rep, &err);
}

const std::vector<Note>& notesOf(const Song& song, size_t pattern, size_t channel = 0) {
  static const std::vector<Note> none;
  if (pattern >= song.patterns.size() || channel >= song.channels.size()) return none;
  auto it = song.patterns[pattern].notes.find(song.channels[channel].id);
  return it == song.patterns[pattern].notes.end() ? none : it->second;
}

trk::StreamHeader headerOf(const Note& n) {
  trk::StreamHeader h;
  trk::StreamReader r;
  r.open(n.tracker.data(), n.tracker.size(), h);
  return h;
}

std::vector<trk::StreamRow> rowsOf(const Note& n) {
  std::vector<trk::StreamRow> v;
  trk::StreamHeader h;
  trk::StreamReader r;
  if (!r.open(n.tracker.data(), n.tracker.size(), h)) return v;
  for (int g = 0; g < 10000; ++g) {
    const trk::StreamRow row = r.next();
    if (r.done()) break;
    v.push_back(row);
  }
  return v;
}

bool sj0(const Song& s) { return s.markers.size() == 1 && s.markers[0].action == MarkerAction::SongLoop && s.markers[0].pos == 0; }

// ---------------------------------------------------------------- tests

void testStream() {
  trk::StreamHeader h;
  h.flags = trk::kHasState | trk::kHandOver;
  h.lane = 5;
  h.startTick = 3;
  h.state.realVol = 33;
  h.state.vibPos = 200;
  trk::StreamWriter w(h);
  std::vector<trk::StreamRow> in;
  for (int i = 0; i < 300; ++i) {
    trk::StreamRow r;
    r.speed = i < 100 ? 6 : 3;
    r.passes = i == 150 ? 4 : 1;
    if (i % 37 == 0) r.cell = trk::Cell{static_cast<uint8_t>(1 + i % 96), 1, 0x30, 0x0A, static_cast<uint8_t>(i)};
    in.push_back(r);
    w.row(r);
  }
  w.end(4);
  const std::vector<uint8_t> bytes = w.finish();
  trk::StreamReader rd;
  trk::StreamHeader got;
  expect(rd.open(bytes.data(), bytes.size(), got), "stream: opens");
  expect(got.flags == h.flags && got.lane == 5 && got.startTick == 3 && got.state.realVol == 33 && got.state.vibPos == 200, "stream: header and state");
  bool same = true;
  int endAt = -1;
  for (size_t i = 0; i < in.size(); ++i) {
    const trk::StreamRow r = rd.next();
    same = same && r.cell == in[i].cell && r.speed == in[i].speed && r.passes == in[i].passes;
    if (r.endTick >= 0) endAt = static_cast<int>(i);
  }
  expect(same, "stream: every row comes back (300 rows, run lengths over 127, speed and delay changes)");
  expect(endAt == 299, "stream: the end mark belongs to the last row");
  rd.next();
  expect(rd.done(), "stream: done past the last row");
  expect(bytes.size() < 4 + sizeof(trk::State) + 120, "stream: empty rows are run-length coded");
  // Garbage never reads out of range.
  std::vector<uint8_t> junk(64, 0x02);
  junk[0] = trk::kStreamVersion;
  junk[1] = 0;
  trk::StreamHeader jh;
  trk::StreamReader jr;
  bool fine = jr.open(junk.data(), junk.size(), jh);
  for (int i = 0; i < 100; ++i) jr.next();
  expect(fine && jr.done(), "stream: a truncated row op ends the stream");
  trk::StreamHeader bad;
  expect(!jr.open(junk.data(), 2, bad), "stream: too short is refused");
}

void testChannelMath() {
  // C-4 (note 49, finetune 0): period index 48 * 16 + 16; 8363 Hz in both tables.
  const uint16_t lp = trk::periodAt(48 * 16 + 16, true), ap = trk::periodAt(48 * 16 + 16, false);
  expect(lp == 4608, "linear period of C-4 is 4608");
  expect(ap == 1712, "Amiga period of C-4 is 1712");
  expect(std::abs(trk::periodHz(lp, true) - 8363.0) < 1.0, "linear C-4 plays at 8363 Hz");
  expect(std::abs(trk::periodHz(ap, false) - 8363.0) < 1.0, "Amiga C-4 plays at 8363 Hz");
  expect(std::abs(trk::periodHz(lp - 768, true) / trk::periodHz(lp, true) - 2.0) < 1e-3, "linear: 768 period units are an octave");
  // A note, a volume column set and a slide on the following ticks.
  trk::InstrumentDef def;
  def.samples[0].length = 1000;
  def.samples[0].volume = 40;
  trk::Channel c;
  c.reset();
  c.fixed = &def;
  trk::SongFx fx;
  c.row(trk::Cell{49, 1, 0, 0, 0}, 6, fx);
  c.updateEnvelopes();
  uint8_t st = c.takeStatus();
  expect((st & trk::kTrigger) && c.s.realVol == 40 && c.s.realPeriod == 4608, "note triggers with the sample volume");
  expect(std::abs(c.s.finalVol - 40.0f / 64.0f) < 1e-6f, "final volume = volume / 64");
  c.row(trk::Cell{0, 0, 0x10 + 20, 0x0A, 0x02}, 6, fx);  // volume 20, slide down 2 per tick
  for (int t = 5; t >= 1; --t) c.tick(6, t, fx);
  expect(c.s.realVol == 10, "volume column 20, then 5 ticks of slide down by 2 = 10");
  c.row(trk::Cell{0, 0, 0, 0x01, 0x04}, 6, fx);  // portamento up 4 per tick (period - 16)
  for (int t = 5; t >= 1; --t) c.tick(6, t, fx);
  expect(c.s.realPeriod == 4608 - 5 * 16, "portamento up: 5 ticks of 4 * 4 period units");
  c.row(trk::Cell{97, 0, 0, 0, 0}, 6, fx);
  expect(c.s.keyOff == 1 && c.s.realVol == 0, "key off without a volume envelope silences");
  // Fxx and Bxx reach the song.
  trk::SongFx f2;
  c.row(trk::Cell{0, 0, 0, 0x0F, 0x03}, 6, f2);
  expect(f2.speed == 3 && f2.bpm < 0, "F03 sets the speed");
  trk::SongFx f3;
  c.row(trk::Cell{0, 0, 0, 0x0F, 0x90}, 6, f3);
  expect(f3.bpm == 0x90, "F90 sets the tempo");
  trk::SongFx f4;
  c.row(trk::Cell{0, 0, 0, 0x0D, 0x16}, 6, f4);
  expect(f4.breakRow == 16, "D16 breaks to row 16 (decimal digits)");
}

void testParser() {
  XSong s = basicSong(32);
  s.ins[0].smps[0].fine = -16;
  s.ins[0].smps[0].rel = 12;
  XSmp s16;
  for (int i = 0; i < 500; ++i) s16.data.push_back(static_cast<int16_t>(std::lround(20000 * std::sin(i * 0.1))));
  s16.sixteen = true;
  s16.loop = 2;
  s16.loopStart = 100;
  s16.loopLen = 300;
  s.ins[0].smps.push_back(s16);
  s.patterns[0][3][1] = XCell{61, 1, 0x40, 0x04, 0x37};
  s.orders = {0, 0};
  bool ok = false;
  const trk::Module m = parse(s, &ok);
  expect(ok, "XM: parses");
  expect(m.title == "Test song" && m.channels == 2 && m.orders.size() == 2 && m.speed == 6 && m.bpm == 125 && m.linear, "XM: header");
  expect(m.patterns[0].rows == 32 && m.patterns[0].at(3, 1) == trk::Cell{61, 1, 0x40, 0x04, 0x37}, "XM: pattern cell");
  const trk::Instrument& in = m.instruments[1];
  expect(in.name == "Lead" && in.samples[0].data.size() == 2000 && in.samples[0].data[1] == (7 - 100) * 256, "XM: 8-bit delta samples");
  expect(in.def.samples[0].finetune == -16 && in.def.samples[0].relNote == 12, "XM: tuning");
  expect(in.samples[1].data.size() == 500 && in.samples[1].data[10] == static_cast<int16_t>(std::lround(20000 * std::sin(1.0))), "XM: 16-bit delta samples");
  expect(in.def.samples[1].loop == trk::kLoopPingPong && in.def.samples[1].loopStart == 100 && in.def.samples[1].loopLength == 300, "XM: 16-bit loop in frames");
  trk::Module bad;
  std::string err;
  expect(!trk::parseModule(std::vector<uint8_t>{1, 2, 3}, bad, &err) && !err.empty(), "not a module: refused with a reason");
  expect(trk::isModulePath("/a/B.XM") && trk::isModulePath("/a/b.mod") && !trk::isModulePath("/a/b.wav"), "module file names");
}

void testMod() {
  std::vector<uint8_t> b(1084, 0);
  std::memcpy(b.data(), "mod test", 8);
  // Sample 1: 400 bytes, volume 48, looped from 100 (words: 50) for 200 (100).
  uint8_t* sh = b.data() + 20;
  sh[22] = 0;
  sh[23] = 200;  // length in words
  sh[24] = 0;
  sh[25] = 48;
  sh[26] = 0;
  sh[27] = 50;
  sh[28] = 0;
  sh[29] = 100;
  b[950] = 1;    // song length
  b[951] = 127;
  b[952] = 0;    // order 0
  std::memcpy(b.data() + 1080, "M.K.", 4);
  std::vector<uint8_t> pat(64 * 4 * 4, 0);
  // Row 0, channel 0: period 428 (C-4 in the 8-octave table), sample 1, effect C20.
  pat[0] = 0x01;
  pat[1] = 0xAC;
  pat[2] = 0x1C;
  pat[3] = 0x20;
  b.insert(b.end(), pat.begin(), pat.end());
  for (int i = 0; i < 400; ++i) b.push_back(static_cast<uint8_t>(i & 0x7F));
  trk::Module m;
  std::string err;
  expect(trk::parseModule(b, m, &err), "MOD: parses");
  expect(m.format == trk::Format::Mod && m.channels == 4 && !m.linear, "MOD: four channels, Amiga periods");
  expect(m.patterns[0].at(0, 0) == trk::Cell{49, 1, 0, 0x0C, 0x20}, "MOD: period 428 is C-4, sample 1, C20");
  expect(m.instruments[1].def.samples[0].volume == 48 && m.instruments[1].def.samples[0].loopStart == 100 &&
             m.instruments[1].def.samples[0].loopLength == 200 && m.instruments[1].def.samples[0].loop == trk::kLoopForward,
         "MOD: sample volume and loop");
  Song song;
  trk::ImportReport rep;
  trk::ImportOptions o;
  expect(trk::buildSong(m, "mod", o, song, rep, &err) && rep.notes == 1 && song.channels.size() == 1, "MOD: builds a song with its note");
}

void testFuzz() {
  XSong s = basicSong(16);
  s.ins[0].volEnv = {{0, 64}, {10, 32}, {40, 0}};
  s.ins[0].volType = 3;
  s.patterns[0][0][0] = XCell{49, 1, 0, 0, 0};
  s.patterns[0][4][1] = XCell{52, 1, 0x30, 0x0E, 0xD2};
  s.patterns[0][8][0] = XCell{97, 0, 0, 0x0E, 0x62};
  s.patterns[0][9][1] = XCell{0, 0, 0, 0x0B, 0x00};
  const std::vector<uint8_t> good = writeXm(s);
  int parsed = 0, built = 0;
  // Every truncation.
  for (size_t n = 0; n < good.size(); n += 3) {
    trk::Module m;
    std::string err;
    std::vector<uint8_t> cut(good.begin(), good.begin() + static_cast<long>(n));
    if (trk::parseModule(cut, m, &err)) {
      ++parsed;
      Song song;
      trk::ImportReport rep;
      trk::ImportOptions o;
      o.maxSeconds = 20;
      if (trk::buildSong(m, "f", o, song, rep, &err)) ++built;
    }
  }
  // Random bytes changed (a fixed seed).
  uint32_t seed = 12345;
  auto rnd = [&] {
    seed = seed * 1664525u + 1013904223u;
    return seed >> 8;
  };
  for (int iter = 0; iter < 400; ++iter) {
    std::vector<uint8_t> v = good;
    const int edits = 1 + static_cast<int>(rnd() % 12);
    for (int e = 0; e < edits; ++e) v[rnd() % v.size()] = static_cast<uint8_t>(rnd());
    trk::Module m;
    std::string err;
    if (trk::parseModule(v, m, &err)) {
      ++parsed;
      Song song;
      trk::ImportReport rep;
      trk::ImportOptions o;
      o.maxSeconds = 20;
      if (trk::buildSong(m, "f", o, song, rep, &err)) ++built;
    }
  }
  expect(parsed > 100 && built > 0, "fuzz: truncated and mutated files read or refuse, never crash (" + std::to_string(parsed) + " read)");
}

void testVoicesAndNotes() {
  // Channel 0: C-4 at row 0, E-4 at row 8 (the first voice ends there); channel 1: a delayed note.
  XSong s = basicSong(16);
  s.ins[0].smps[0] = rampSample(4000, 1);  // looped: it sounds until something ends it
  s.patterns[0][0][0] = XCell{49, 1, 0, 0, 0};
  s.patterns[0][8][0] = XCell{53, 1, 0, 0, 0};
  s.patterns[0][2][1] = XCell{49, 1, 0, 0, 0};
  s.patterns[0][6][1] = XCell{51, 1, 0, 0x0E, 0xD3};  // starts on tick 3 of row 6
  Song song;
  trk::ImportReport rep;
  expect(build(s, song, rep), "song: builds");
  expect(song.channels.size() == 1 && song.channels[0].plugin.type == "gl.tracker" && song.channels[0].name == "Lead", "song: one channel per instrument");
  expect(song.patterns.size() == 1 && song.playlist.size() == 1, "song: one pattern, one clip");
  expect(std::abs(song.tempo - 125.0) < 1e-6 && rep.rowsPerBeat == 4, "song: speed 6 at 125 = 125 BPM, 4 rows per beat");
  const auto& ns = notesOf(song, 0);
  expect(ns.size() == 4, "song: four voices = four notes");
  if (ns.size() == 4) {
    const Tick row = song.ppq / 4;
    // Sorted by start: lane 0 row 0, lane 1 row 2, lane 1 row 6, lane 0 row 8.
    expect(ns[0].start == 0 && ns[0].length == 8 * row && ns[0].pitch == 60, "voice 1: rows 0..7, C-4 = middle C");
    expect(ns[1].start == 2 * row && ns[1].length == 5 * row, "voice 2: rows 2..6 (it sounds until the delayed note's tick)");
    const auto r1 = rowsOf(ns[1]);
    expect(!r1.empty() && r1.back().endTick == 3, "voice 2 ends on tick 3 of row 6");
    expect(ns[2].start == 6 * row && headerOf(ns[2]).startTick == 3 && ns[2].pitch == 62, "voice 3: starts on row 6, tick 3");
    expect(ns[3].start == 8 * row && ns[3].length == 8 * row && ns[3].pitch == 64, "voice 4: rows 8..15 (to the song end)");
    const auto r0 = rowsOf(ns[0]);
    expect(r0.size() == 8 && r0.front().cell.note == 49 && r0.back().endTick == 255, "voice 1 carries its 8 rows and ends with its last row");
    expect((headerOf(ns[0]).flags & trk::kHasState) && headerOf(ns[0]).lane == 0 && headerOf(ns[1]).lane == 1, "voices keep their channel");
  }
  // A one-shot sample that ends: the note ends after its row.
  XSong t = basicSong(16);
  t.ins[0].smps[0] = rampSample(200);  // 200 frames at 8363 Hz: gone within row 0
  t.patterns[0][0][0] = XCell{49, 1, 0, 0, 0};
  Song s2;
  expect(build(t, s2, rep) && notesOf(s2, 0).size() == 1 && notesOf(s2, 0)[0].length == s2.ppq / 4, "a sample that ended ends its note");
  // ... unless a retrigger later brings it back.
  t.patterns[0][5][0] = XCell{0, 0, 0, 0x0E, 0x93};
  Song s3;
  expect(build(t, s3, rep) && notesOf(s3, 0).size() == 1 && notesOf(s3, 0)[0].length == 6 * s3.ppq / 4, "a retrigger keeps the voice until it ended again");
}

void testPatternsAndOrder() {
  // Pattern 0 played twice: the second time a voice from pattern 1 carries in.
  XSong s = basicSong(8);
  s.ins[0].smps[0] = rampSample(4000, 1);
  s.pattern(8);
  s.patterns[0][7][0] = XCell{0, 0, 0, 0x0C, 0x40};  // a cell: a pattern with none plays as 64 rows
  s.patterns[1][2][1] = XCell{56, 1, 0, 0, 0};  // sounds on into the next pattern
  s.orders = {0, 0, 1, 0};
  Song song;
  trk::ImportReport rep;
  expect(build(s, song, rep), "order: builds");
  expect(song.playlist.size() == 4, "order: one clip per order entry");
  // Patterns: 0 (alone), 1, 0 with the carried voice.
  expect(rep.patterns == 2 && rep.variants == 3, "order: pattern 0 gets a second variant where a voice carries in");
  expect(rep.loops && sj0(song), "order: the song plays on from its start (FT2 songs loop)");
  bool tieFound = false, handFound = false;
  for (const auto& p : song.patterns)
    for (const auto& [cid, notes] : p.notes)
      for (const auto& n : notes) {
        const trk::StreamHeader h = headerOf(n);
        tieFound = tieFound || ((h.flags & trk::kTie) && n.start == 0 && h.lane == 1);
        handFound = handFound || (h.flags & trk::kHandOver);
      }
  expect(tieFound && handFound, "order: the voice hands over to a continuation on the next pattern's first row");
  // The first two clips share their pattern.
  expect(song.playlist.size() >= 2 && song.playlist[0].pattern == song.playlist[1].pattern, "order: equal placements share a pattern");
  const Tick row = song.ppq / 4;
  expect(song.playlist[3].start == 24 * row && song.playlist[3].length == 8 * row, "order: clips follow each other row by row");

  // The same cells from another channel state that sounds the same: one pattern.
  XSong d = basicSong(8);
  d.ins[0].smps[0] = rampSample(4000, 1);
  d.patterns[0][0][0] = XCell{49, 1, 0, 0x04, 0x37};  // vibrato from its own parameter
  d.patterns[0][4][0] = XCell{97, 0, 0, 0, 0};        // key off: silent (no envelope); the voice ends
  d.orders = {0, 0, 0};
  Song sd;
  expect(build(d, sd, rep) && rep.variants == 1 && sd.playlist.size() == 3, "order: a pattern repeated after its voice ended stays one pattern");
  // ... but not when the memory the next pass uses differs (vibrato speed and depth from memory).
  d.patterns[0][2][0] = XCell{0, 0, 0, 0x04, 0x00};
  d.patterns[0][0][0] = XCell{49, 1, 0, 0, 0};
  d.patterns[0][3][0] = XCell{0, 0, 0, 0x04, 0x52};  // sets the memory for the next pass's row 2
  Song sd2;
  expect(build(d, sd2, rep) && rep.variants == 2, "order: a pass that plays differently gets its own pattern");

  // An empty pattern plays as 64 empty rows, as the tracker loads it.
  XSong e = basicSong(8);
  e.pattern(8);
  e.patterns[1][0][0] = XCell{49, 1, 0, 0, 0};
  e.orders = {0, 1};
  Song se;
  expect(build(e, se, rep) && rep.rows == 64 + 8, "order: an empty pattern is 64 rows long");

  // A pattern break to row 4 and a pattern loop.
  XSong b = basicSong(8);
  b.pattern(8);
  b.patterns[0][5][0] = XCell{0, 0, 0, 0x0D, 0x04};  // break to row 4 of the next
  b.patterns[1][1][0] = XCell{0, 0, 0, 0x0E, 0x60};  // loop start
  b.patterns[1][2][0] = XCell{0, 0, 0, 0x0E, 0x62};  // back twice
  b.orders = {0, 1, 1};
  Song sb;
  expect(build(b, sb, rep), "break / loop: builds");
  // Clips: p0 rows 0..5, p1 rows 4..7, p1 rows 0..2, p1 rows 1..2, p1 rows 1..7.
  bool ok = sb.playlist.size() == 5;
  if (ok) {
    ok = sb.playlist[0].offset == 0 && sb.playlist[0].length == 6 * row && sb.playlist[1].offset == 4 * row && sb.playlist[1].length == 4 * row &&
         sb.playlist[2].offset == 0 && sb.playlist[2].length == 3 * row && sb.playlist[3].offset == row && sb.playlist[3].length == 2 * row &&
         sb.playlist[4].offset == row && sb.playlist[4].length == 7 * row;
  }
  expect(ok, "break / loop: one clip per run of rows, with the pattern offset");
  expect(rep.rows == 6 + 4 + 3 + 2 + 7, "break / loop: every row once as played");

  // B01 at the end: the song loops from order 1.
  XSong j = basicSong(4);
  j.pattern(4);
  j.pattern(4);
  j.patterns[0][0][0] = XCell{0, 0, 0, 0x0C, 0x40};
  j.patterns[1][0][0] = XCell{0, 0, 0, 0x0C, 0x40};
  j.patterns[2][3][0] = XCell{0, 0, 0, 0x0B, 0x01};
  j.orders = {0, 1, 2};
  Song sj;
  expect(build(j, sj, rep) && rep.loops && rep.rows == 12 && sj.markers.size() == 1 && sj.markers[0].action == MarkerAction::SongLoop &&
             sj.markers[0].pos == 4 * row,
         "B01 at the end: a loop marker where order 1 starts");
}

void testTempoAndVolume() {
  XSong s = basicSong(8);
  s.patterns[0][2][0] = XCell{0, 0, 0, 0x0F, 0x03};  // speed 3 from row 2
  s.patterns[0][4][0] = XCell{0, 0, 0, 0x0F, 0xFA};  // 250 from row 4
  s.patterns[0][6][0] = XCell{0, 0, 0, 0x10, 0x20};  // global volume 32
  s.patterns[0][7][1] = XCell{0, 0, 0, 0x0E, 0xE1};  // row 7 twice
  Song song;
  trk::ImportReport rep;
  expect(build(s, song, rep), "tempo: builds");
  const AutomationClip* tempo = nullptr;
  const AutomationClip* gv = nullptr;
  for (const auto& a : song.automations) {
    if (a.owner == kSongId) tempo = &a;
    if (a.owner == song.masterId()) gv = &a;
  }
  expect(tempo && tempo->points.size() == 4, "tempo: an automation point where the row's length changes");
  if (tempo && tempo->points.size() == 4) {
    auto desc = describeParam(song, songParam(SongParam::Tempo));
    auto at = [&](size_t i) { return desc->fromNormalized(tempo->points[i].value); };
    const double tick125 = 880.0 / 44000.0, tick250 = 440.0 / 44000.0;
    expect(std::abs(at(0) - 125) < 1e-3 && std::abs(at(1) - 60.0 / (4 * 3 * tick125)) < 1e-3, "tempo: speed 6 then 3 at 125");
    expect(std::abs(at(2) - 60.0 / (4 * 3 * tick250)) < 1e-2 && std::abs(at(3) - 60.0 / (4 * 6 * tick250)) < 1e-2, "tempo: 250, then the delayed row twice as long");
    expect(tempo->points[1].pos == 2 * song.ppq / 4 && tempo->points[3].pos == 7 * song.ppq / 4, "tempo: points on their rows");
  }
  bool half = false;
  if (gv && gv->points.size() == 2) {
    auto md = describeParam(song, {song.masterId(), static_cast<uint32_t>(MixerParam::Volume)});
    auto gain = [&](float n) { const double v = md->fromNormalized(n) / 0.8; return v * v; };  // the fader law
    half = md && std::abs(gain(gv->points[1].value) - 0.5) < 1e-4 && std::abs(gain(gv->points[0].value) - 1.0) < 1e-4;
  }
  expect(half, "global volume 32 = master at half");
}

// Renders one note of the generator directly.
std::vector<float> renderNote(const PluginState& st, const std::vector<uint8_t>& stream, int pitch, int total, int block, std::vector<float>* right = nullptr,
                              int offAt = -1, double tempo = 125) {
  std::vector<float> L(static_cast<size_t>(total), 0.f), R(static_cast<size_t>(total), 0.f);
  auto g = packs::createGenerator(st.type);
  ChannelAux aux;
  packs::prepareAux(st, aux);
  ProcessContext ctx;
  ctx.sampleRate = 44100;
  ctx.tempo = tempo;
  ctx.ppq = 96;
  ctx.playing = true;
  NoteProps np;
  np.tracker = stream.empty() ? nullptr : stream.data();
  np.trackerLen = static_cast<uint32_t>(stream.size());
  double tick = 0;
  for (int pos = 0; pos < total; pos += block) {
    const int n = std::min(block, total - pos);
    if (pos == 0) g->noteOn(1, pitch, 100.0f / 127.0f, 0, np);
    if (offAt >= pos && offAt < pos + n) g->noteOff(1, offAt - pos);
    ctx.tick = tick;
    g->process(st, aux, ctx, L.data() + pos, R.data() + pos, n);
    tick += n * ctx.tempo * ctx.ppq / (60.0 * ctx.sampleRate);
  }
  if (right) *right = R;
  return L;
}

void testSampler() {
  const std::string dir = tempDir("sampler");
  XSong s = basicSong(16);
  s.patterns[0][0][0] = XCell{49, 1, 0, 0, 0};
  Song song;
  trk::ImportReport rep;
  expect(build(s, song, rep, dir) && rep.files.size() == 1 && fs::exists(fs::u8path(dir) / rep.files[0]), "sampler: the sample file is written");
  PluginState st = song.channels[0].plugin;
  expect(st.params.size() == trk::PCount && st.params[trk::PSound] == 0.0f, "import: module instruments play their own samples only (no built-in sound)");
  st.params[trk::PInterp] = trk::InterpNone;
  st.params[trk::PRamp] = 0;
  const Note& n = notesOf(song, 0)[0];
  std::vector<float> R;
  const int total = 8000;
  const std::vector<float> L = renderNote(st, n.tracker, n.pitch, total, 512, &R);
  // Expected: the sample stepped at 8363 Hz in 32.32 fixed point, volume 64, centre pan.
  const double hz = trk::periodHz(4608, true);
  const uint64_t delta = static_cast<uint64_t>(hz / 44100.0 * 4294967296.0 + 0.5);
  const float pan = static_cast<float>(std::lround(65536.0 * std::sqrt(0.5)) / 65536.0);
  double err = 0;
  uint64_t p = 0;
  for (int i = 0; i < total; ++i) {
    const size_t idx = static_cast<size_t>(p >> 32);
    const float v = idx < 2000 ? static_cast<float>(((static_cast<int>(idx) * 7) % 200) - 100) / 128.0f : 0.0f;
    err = std::max(err, static_cast<double>(std::abs(L[static_cast<size_t>(i)] - v * pan)) + std::abs(R[static_cast<size_t>(i)] - v * pan));
    p += delta;
  }
  expect(err < 1e-5, "sampler: sample by sample as the tracker steps it (pitch, volume, square-root pan law)");
  // Linear interpolation up to the end of a sample without a loop: past its last point the
  // tracker repeats that point, so the sound does not dip towards silence before it stops.
  {
    PluginState sl = st;
    sl.params[trk::PInterp] = trk::InterpLinear;
    const int span = 12000;  // past the sample's end at this pitch
    const std::vector<float> Ll = renderNote(sl, n.tracker, n.pitch, span, 512);
    auto v = [](int64_t i) { return static_cast<float>(((static_cast<int>(std::min<int64_t>(i, 1999)) * 7) % 200) - 100) / 128.0f; };
    double errL = 0, tailLevel = 0;
    uint64_t q = 0;
    for (int i = 0; i < span; ++i) {
      const int64_t idx = static_cast<int64_t>(q >> 32);
      const float x = static_cast<float>(static_cast<uint32_t>(q) >> 1) * (1.0f / 2147483648.0f);
      const float want = idx < 2000 ? (v(idx) + (v(idx + 1) - v(idx)) * x) * pan : 0.0f;
      errL = std::max(errL, static_cast<double>(std::abs(Ll[static_cast<size_t>(i)] - want)));
      if (idx == 1999) tailLevel = std::max(tailLevel, static_cast<double>(std::abs(want)));
      q += delta;
    }
    expect(errL < 1e-5 && tailLevel > 0.1, "sampler: the last point of a sample without a loop holds to its end (linear)");
  }
  // Block-size invariance with sinc and ramps.
  PluginState sn = song.channels[0].plugin;
  const std::vector<float> a = renderNote(sn, n.tracker, n.pitch, 6000, 512);
  const std::vector<float> b = renderNote(sn, n.tracker, n.pitch, 6000, 37);
  double d = 0, e2 = 0;
  for (size_t i = 0; i < a.size(); ++i) {
    d = std::max(d, static_cast<double>(std::abs(a[i] - b[i])));
    e2 += a[i] * a[i];
  }
  expect(d < 1e-6 && e2 > 1.0, "sampler: the same output for any block size");
  // A plain note (no cells): sounds, and its key off fades it with the volume envelope.
  XSong env = basicSong(4);
  env.ins[0].smps[0] = rampSample(4000, 1);
  env.ins[0].volEnv = {{0, 64}, {4, 64}, {20, 0}};
  env.ins[0].volType = 1 | 2;  // on, sustain at point 1
  env.ins[0].volSustain = 1;
  env.patterns[0][0][0] = XCell{49, 1, 0, 0, 0};
  Song se;
  const std::string dir2 = tempDir("env");
  expect(build(env, se, rep, dir2), "sampler: envelope song builds");
  const std::vector<float> plain = renderNote(se.channels[0].plugin, {}, 60, 44100, 256, nullptr, 22050);
  double early = 0, late = 0, after = 0;
  for (int i = 1000; i < 20000; ++i) early = std::max(early, static_cast<double>(std::abs(plain[static_cast<size_t>(i)])));
  for (int i = 22050 + 9000; i < 22050 + 12000; ++i) late = std::max(late, static_cast<double>(std::abs(plain[static_cast<size_t>(i)])));
  for (int i = 40000; i < 44100; ++i) after = std::max(after, static_cast<double>(std::abs(plain[static_cast<size_t>(i)])));
  expect(early > 0.3 && early < 0.8, "plain note: sounds at the sample's volume while held (sustain point)");
  expect(after < 1e-6 && late < early, "plain note: released through the envelope to silence");
  // Extremes stay finite.
  PluginState ex = sn;
  ex.params[trk::PLevel] = 2;
  ex.params[trk::PInterp] = trk::InterpCubic;
  ex.params[trk::PRowsPerBeat] = 4;
  ex.params[trk::PTicks] = 1;
  const std::vector<float> xs = renderNote(ex, {}, 127, 20000, 300, nullptr, 5000);
  bool finite = true;
  for (float v : xs) finite = finite && std::isfinite(v);
  const std::vector<float> lo = renderNote(ex, {}, 0, 20000, 300, nullptr, 5000);
  for (float v : lo) finite = finite && std::isfinite(v);
  expect(finite, "extremes: highest / lowest notes, level 2, 1 tick per row stay finite");
  // Short parameter vectors (saved before a parameter existed) take the defaults.
  PluginState shortSt = sn;
  shortSt.params.resize(2);
  normalizePluginState(shortSt);
  expect(shortSt.params.size() == trk::PCount && shortSt.params[trk::PLevel] == 1.0f && shortSt.params[trk::PRamp] == 1.0f, "normalizePluginState pads to the defaults");
  // The instrument text round trip.
  trk::InstrumentText t;
  expect(trk::decodeInstrument(sn.data, t) && t.name == "Lead" && t.def.samples[0].length == 2000 && t.files.size() == 1, "instrument text reads back");
  std::error_code ec;
  fs::remove_all(fs::u8path(dir), ec);
  fs::remove_all(fs::u8path(dir2), ec);
}

// A tempo change on the row a sliding note starts on, set by another channel: the song changes
// tempo inside an audio block, and the note's ticks still follow the new tempo from that row.
void testTempoInBlock() {
  const std::string dir = tempDir("tempoblock");
  XSong s = basicSong(8);
  s.ins[0].smps[0] = rampSample(4000, 1);
  s.patterns[0][0][0] = XCell{0, 0, 0, 0x0F, 0xFA};  // 250: 441-frame ticks, shorter than an audio block
  s.patterns[0][0][1] = XCell{0, 0, 0, 0x0F, 0x07};  // speed 7: the next row starts 15 frames into a block
  s.patterns[0][1][0] = XCell{49, 1, 0, 0x01, 0x08};  // a note sliding up
  s.patterns[0][1][1] = XCell{0, 0, 0, 0x0F, 0x0C};  // speed 12 on the same row
  Song song;
  trk::ImportReport rep;
  expect(build(s, song, rep, dir) && notesOf(song, 0).size() == 1, "tempo in a block: builds");
  for (auto& c : song.channels) {
    c.plugin.params[trk::PInterp] = trk::InterpNone;
    c.plugin.params[trk::PRamp] = 0;
  }
  RenderOptions ro;
  ro.sampleRate = 44100;
  const AudioBuffer buf = renderSong(song, ro);
  const Note& n = notesOf(song, 0)[0];
  const double tick250 = 440 / 44000.0;
  const std::vector<float> alone = renderNote(song.channels[0].plugin, n.tracker, n.pitch, 9000, 512, nullptr, -1, 60.0 / (4 * 12 * tick250));
  const size_t at = static_cast<size_t>(std::lround(7 * tick250 * 44100));
  double best = 1e9;
  for (size_t o = at - 1; o <= at + 1; ++o) {
    double worst = 0;
    for (size_t i = 0; i < alone.size() && o + i < buf.frames; ++i)
      worst = std::max(worst, static_cast<double>(std::abs(buf.data[o + i] - alone[i])));
    best = std::min(best, worst);
  }
  expect(best < 1e-4, "tempo in a block: the sliding note keeps the new row's tick rate (" + std::to_string(best) + ")");
  std::error_code ec;
  fs::remove_all(fs::u8path(dir), ec);
}

void testWholeSongRender() {
  // A song through the engine: two notes on two channels, the second delayed by a pattern.
  const std::string dir = tempDir("render");
  XSong s = basicSong(16);
  s.pattern(16);
  s.patterns[0][0][0] = XCell{49, 1, 0, 0, 0};
  s.patterns[1][4][1] = XCell{61, 1, 0x30, 0, 0};
  s.orders = {0, 1};
  Song song;
  trk::ImportReport rep;
  expect(build(s, song, rep, dir), "render: builds");
  for (auto& c : song.channels) {
    c.plugin.params[trk::PInterp] = trk::InterpNone;
    c.plugin.params[trk::PRamp] = 0;
  }
  RenderOptions ro;
  ro.sampleRate = 44100;
  const AudioBuffer buf = renderSong(song, ro);
  // The tracker: 2000 frames at 8363 Hz from 0 s; then 2000 frames an octave up at 0.48 + 4 rows.
  const double rowSec = 6 * 880.0 / 44000.0;
  const size_t second = static_cast<size_t>(std::lround((16 + 4) * rowSec * 44100));
  auto rms = [&](size_t a, size_t b) {
    double acc = 0;
    for (size_t i = a; i < b && i < buf.frames; ++i) acc += buf.data[i] * buf.data[i];
    return std::sqrt(acc / std::max<size_t>(1, b - a));
  };
  expect(buf.frames >= second + 6000, "render: the whole song");
  expect(rms(100, 9000) > 0.1 && rms(12000, second - 50) < 1e-6, "render: the first note, then silence until the second");
  expect(rms(second + 10, second + 4000) > 0.05 && rms(second - 400, second - 5) < 1e-6, "render: the second note on its row");
  // At the level the instrument plays: channel, insert and master faders at 0 dB.
  {
    const Note& n0 = notesOf(song, 0, 0)[0];
    const std::vector<float> alone = renderNote(song.channels[0].plugin, n0.tracker, n0.pitch, 9000, 512);
    double worst = 0;
    for (size_t i = 0; i < alone.size(); ++i) worst = std::max(worst, static_cast<double>(std::abs(buf.data[i] - alone[i])));
    expect(worst < 1e-4, "render: the song plays at the level of the tracker's mix (" + std::to_string(worst) + ")");
  }
  std::error_code ec;
  fs::remove_all(fs::u8path(dir), ec);
  testTempoInBlock();
}

void testProjectAndFiles() {
  // Notes keep their cells through a save.
  XSong s = basicSong(8);
  s.patterns[0][0][0] = XCell{49, 1, 0x20, 0x04, 0x44};
  Song song;
  trk::ImportReport rep;
  build(s, song, rep);
  const std::string json = projectToText(song);
  Song back;
  std::string err;
  expect(loadProjectText(json, "x.gridline.json", back, &err), "project: saves and loads");
  expect(!notesOf(back, 0).empty() && notesOf(back, 0)[0].tracker == notesOf(song, 0)[0].tracker, "project: a note's tracker cells survive a save");
  expect(back.channels.size() == song.channels.size() && back.channels[0].plugin.data == song.channels[0].plugin.data, "project: the instrument survives a save");
  // Opening.
  expect(openKindOf("/a/Song.XM") == OpenKind::TrackerModule && openKindOf("/a/b.mod") == OpenKind::TrackerModule && opensAsProject("/a/b.xm"),
         "open: modules open as projects");
  bool listed = false;
  for (const auto& ext : openFileTypes()[0].exts) listed = listed || ext == ".xm";
  expect(listed && openFileTypes().back().exts == std::vector<std::string>{".xm", ".mod"}, "open dialog: modules among all projects and on their own");
  // The cache copy: written once, found again.
  const std::string root = tempDir("cache");
  const std::string src = (fs::u8path(root) / "song.xm").u8string();
  {
    const auto bytes = writeXm(s);
    std::ofstream o(fs::u8path(src), std::ios::binary);
    o.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  }
  trk::Module m;
  trk::parseModule(writeXm(s), m, &err);
  int writes = 0;
  archive::MakeRequest mr;
  mr.sourcePath = src;
  mr.cacheRoot = (fs::u8path(root) / "cache").u8string();
  mr.size = fs::file_size(fs::u8path(src));
  mr.write = [&](const std::string& folder, std::vector<std::string>& names, std::string* e) {
    ++writes;
    return trk::writeSamples(m, folder, names, e);
  };
  archive::UnpackResult u1, u2;
  expect(archive::makeCopy(mr, u1, &err) && !u1.reused && fs::exists(fs::u8path(u1.folder) / trk::sampleFileName(1, 0)), "cache: the samples are written into the cache");
  expect(archive::makeCopy(mr, u2, &err) && u2.reused && u2.folder == u1.folder && writes == 1, "cache: the same module finds its copy");
  std::error_code ec;
  fs::remove_all(fs::u8path(root), ec);
}

}  // namespace

int main() {
  testStream();
  testChannelMath();
  testParser();
  testMod();
  testFuzz();
  testVoicesAndNotes();
  testPatternsAndOrder();
  testTempoAndVolume();
  testSampler();
  testWholeSongRender();
  testProjectAndFiles();
  return finish();
}
