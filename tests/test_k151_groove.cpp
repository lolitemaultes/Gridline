// Gridline — K-151: swing presets and grooves.
//  * the classic step swing (every project without a preset) places steps and
//    notes exactly as before: the groove warp equals swingWarp to the bit;
//  * every factory preset places steps and notes where its grid, cycle, timing
//    and amount say (compiled and rendered), with its accents;
//  * preset files: save, list, load, folders, bad and short files;
//  * the project keeps the groove (no key while it is the classic swing);
//  * applying a preset is one undo step; collaborators get it (and the
//    "swing piano roll notes" switch);
//  * FLP: FL's one swing amount is written where it says the same, a warning
//    where the groove cannot go;
//  * grooves taken from notes, from an audio clip's transients and from a
//    score file; a groove applied to an audio clip through warp markers;
//  * the piano roll's quantize grooves are the presets; MCP and Lua.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>
#include "Check.h"
#include "Hermetic.h"
#include "collab/CollabSim.h"
#include "core/Document.h"
#include "core/BrowserIndex.h"
#include "core/Groove.h"
#include "core/Meter.h"
#include "core/Mcp.h"
#include "core/NoteTools.h"
#include "core/PluginInfo.h"
#include "core/PluginParams.h"
#include "core/Presets.h"
#include "core/Project.h"
#include "core/ProjectScript.h"
#include "core/WavWriter.h"
#include "core/flp/Flp.h"
#include "core/flp/FlpExport.h"
#include "engine/ClipOps.h"
#include "engine/Freeze.h"
#include "engine/Program.h"
#include "engine/Sample.h"
#include "engine/Render.h"
#include "engine/WarpOps.h"

using namespace gl;
namespace fs = std::filesystem;
namespace gv = gl::groove;

namespace {

bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

// 120 BPM, one 1-bar pattern placed once, one synth channel.
Song grooveSong(int ppq, ObjectId& ch) {
  Song s = makeEmptySong();
  s.tempo = 120;
  s.ppq = ppq;
  s.patterns[0].bars = 1;
  Clip c;
  c.id = s.allocateId();
  c.pattern = s.patterns[0].id;
  s.playlist.push_back(c);
  Channel k;
  k.id = s.allocateId();
  k.name = "synth";
  k.plugin = defaultPluginState("gl.synth");
  k.mixerTrack = s.masterId();
  s.channels.push_back(k);
  ch = k.id;
  return s;
}

long firstSound(const AudioBuffer& b) {
  for (size_t i = 0; i < b.frames; ++i)
    if (std::fabs(b.at(0, i)) > 1e-5f) return static_cast<long>(i);
  return -1;
}

// An independent reading of the groove: cell starts of a cycle at k + timing[k] * amount
// (cells), straight lines between them, cycles from the bar start, a partial cycle straight.
double refMap(double x, double cell, double bar, const std::vector<double>& off) {
  const int n = static_cast<int>(off.size());
  const double cyc = cell * n;
  const double b0 = std::floor(x / bar) * bar;
  const double u = x - b0;
  const double c0 = std::floor(u / cyc) * cyc;
  if (c0 + cyc > bar + 1e-9) return x;
  const double v = (u - c0) / cell;  // in cells
  const int k = std::min(n - 1, static_cast<int>(std::floor(v)));
  const bool nextStraight = c0 + 2 * cyc > bar + 1e-9 && std::fabs(bar - (c0 + cyc)) > 1e-9;
  const double a0 = k + off[static_cast<size_t>(k)];
  const double a1 = k + 1 < n ? k + 1 + off[static_cast<size_t>(k + 1)] : (nextStraight ? n : n + off[0]);
  return b0 + c0 + cell * (a0 + (v - k) * (a1 - a0));
}

// ------------------------------------------------------------------------------------------
void testClassicIsBitIdentical() {
  // The warp of the default shape against the old swing function: every input, exactly.
  std::mt19937_64 rng(151);
  std::uniform_real_distribution<double> ux(0.0, 1.0);
  long cases = 0, same = 0;
  for (int ppq : kPpqChoices)
    for (int spb : {12, 16, 24, 32, 5, 7}) {
      Song s = makeEmptySong();
      s.ppq = ppq;
      Pattern p = s.patterns[0];
      p.stepsPerBar = spb;
      p.bars = 3;
      const Tick st = s.ticksPerStep(p);
      if (st <= 0) continue;
      for (int i = 0; i < 4000; ++i) {
        const double swing = i % 50 == 0 ? (i % 100 == 0 ? 0.0 : 1.0) : ux(rng);
        // grid lines, fractions, ends of pairs and bars, and random positions
        double x = i % 3 == 0 ? static_cast<double>((i / 3) % (3 * spb + 1)) * static_cast<double>(st)
                              : ux(rng) * static_cast<double>(st) * spb * 3;
        if (i % 7 == 0) x = std::nextafter(static_cast<double>(st) * ((i / 7) % (2 * spb)), 0.0);
        const gv::Warp w = gv::makeWarp(s, &p, GrooveShape{}, swing, 1.0);
        const double a = w.active ? std::max(0.0, w.map(x)) : x;
        const double b = swingWarp(x, swing, st, spb);
        ++cases;
        same += a == b;  // bit for bit
      }
    }
  std::printf("# classic swing: %ld of %ld positions identical\n", same, cases);
  expect(same == cases && cases > 100000, "classic: the groove warp of the default shape equals the old swing to the bit (every PPQ, 12/16/24/32/odd steps)");

  // Compiled steps and notes of a busy pattern: the default groove against the old formula.
  ObjectId ch = kNoId;
  Song s = grooveSong(96, ch);
  Pattern& p = s.patterns[0];
  p.bars = 2;
  p.steps[ch] = std::vector<uint8_t>(32, 0);
  for (int i = 0; i < 32; i += 3) p.steps[ch][static_cast<size_t>(i)] = static_cast<uint8_t>(60 + i);
  p.notes[ch] = {Note(7, 30, 60, 100), Note(24, 24, 62, 90), Note(50, 11, 64, 80), Note(200, 90, 65, 70)};
  bool ok = true;
  for (float sw : {0.0f, 0.13f, 0.5f, 0.77f, 1.0f})
    for (int trunc = 0; trunc < 2; ++trunc) {
      s.swing = sw;
      s.channels[0].swingTruncate = trunc != 0;
      const auto ev = compilePatternChannel(s, p, ch);
      const Tick st = s.ticksPerStep(p);
      // Steps: on = swingWarp(grid); velocity = row / 127 (no accents).
      for (int i = 0; i < 32; i += 3) {
        const double want = swingWarp(static_cast<double>(i * st), sw, st, 16);
        const float vel = static_cast<float>(60 + i) / 127.0f;
        bool found = false;
        for (const NoteEv& e : ev) found = found || (noteOnTick(e) == want && e.velocity == vel);
        ok = ok && found;
      }
    }
  expect(ok, "classic: steps start exactly where the old swing put them, velocities untouched");
  Song fresh = makeEmptySong();
  fresh.swing = 0.4f;
  expect(songToJson(fresh).find("groove") == nullptr, "classic: a project without a swing preset writes no groove key");
}

// ------------------------------------------------------------------------------------------
void testFactoryPresetsPlaceNotes() {
  ObjectId ch = kNoId;
  Song s = grooveSong(96, ch);
  Pattern& p = s.patterns[0];
  const Tick st = s.ticksPerStep(p);  // 24 (a sixteenth)
  p.steps[ch] = std::vector<uint8_t>(16, 100);
  bool every = true;
  std::string bad;
  for (const gv::Preset& pr : gv::factoryPresets()) {
    gv::apply(s, pr);
    const auto ev = compilePatternChannel(s, p, ch);
    const double cell = pr.shape.grid == gv::kGrid8th ? 48.0 : 24.0;
    std::vector<double> off;
    for (float t : pr.shape.timing) off.push_back(static_cast<double>(t) * static_cast<double>(pr.amount));
    bool ok = ev.size() == 16;
    for (int i = 0; ok && i < 16; ++i) {
      const double want = std::max(0.0, refMap(static_cast<double>(i * st), cell, 384.0, off));
      double vel = 100.0 / 127.0;
      if (!pr.shape.velocity.empty()) {
        const int c = static_cast<int>(std::floor(static_cast<double>(i * st) / cell + 0.5)) % pr.shape.cycle;
        vel = std::clamp(vel * pr.shape.velocity[static_cast<size_t>(c)], 0.0, 1.0);
      }
      bool found = false;
      for (const NoteEv& e : ev) found = found || (near(noteOnTick(e), want, 1e-6) && near(e.velocity, vel, 1e-5));
      ok = ok && found;
    }
    if (!ok) bad += pr.name + " ";
    every = every && ok;
  }
  if (!every) std::printf("# wrong: %s\n", bad.c_str());
  expect(every, "presets: every factory preset places all 16 steps on its grid, timing and amount, with its accents");

  // Rendered: a step on the off-beat sounds where the preset says (within 2 samples).
  const double spt = 44100.0 * 60.0 / (120.0 * 96.0);
  bool rendered = true;
  for (const char* name : {"Gentle 16ths", "Easy shuffle", "Triplet 16ths", "Lilting 8ths", "Behind the beat", "Ahead of the beat"}) {
    const gv::Preset* pr = gv::findFactory(name);
    if (!pr) {
      rendered = false;
      continue;
    }
    Song r = s;
    gv::apply(r, *pr);
    r.patterns[0].steps[ch] = std::vector<uint8_t>(16, 0);
    const int step = pr->shape.grid == gv::kGrid8th ? 2 : 1;  // the off-beat
    r.patterns[0].steps[ch][static_cast<size_t>(step)] = 100;
    const double cell = pr->shape.grid == gv::kGrid8th ? 48.0 : 24.0;
    std::vector<double> off;
    for (float t : pr->shape.timing) off.push_back(static_cast<double>(t) * static_cast<double>(pr->amount));
    const double want = refMap(static_cast<double>(step) * 24.0, cell, 384.0, off) * spt;
    RenderOptions o;
    const long on = firstSound(renderSong(r, o));
    const bool ok = on >= 0 && std::fabs(static_cast<double>(on) - want) <= 2.0;
    if (!ok) std::printf("# %s: sound at %ld, want %.1f\n", name, on, want);
    rendered = rendered && ok;
  }
  expect(rendered, "render: an off-beat step sounds within 2 samples of where each preset places it");

  // Triplet feel: the off-beat 16th at two thirds of the pair.
  {
    Song r = s;
    gv::apply(r, *gv::findFactory("Triplet 16ths"));
    const auto ev = compilePatternChannel(r, r.patterns[0], ch);
    bool trip = false;
    for (const NoteEv& e : ev) trip = trip || near(noteOnTick(e), 48.0 * 2.0 / 3.0, 1e-4);
    expect(trip, "presets: the triplet preset puts the off-beat 16th at 2/3 of the pair");
  }
  // Piano-roll notes follow the groove (and accents) unless the song swings steps only.
  {
    Song r = s;
    gv::apply(r, *gv::findFactory("Deep pocket"));
    r.patterns[0].steps.clear();
    r.patterns[0].notes[ch] = {Note(24, 12, 60, 100)};
    auto ev = compilePatternChannel(r, r.patterns[0], ch);
    const double want = 24.0 * (1.0 + 0.5 * 0.48);
    expect(ev.size() == 1 && near(noteOnTick(ev[0]), want, 1e-4) && near(ev[0].velocity, 100.0 / 127.0 * 0.9, 1e-5),
           "presets: a piano-roll note on the off-beat moves and takes the accent");
    r.swingNotes = false;
    ev = compilePatternChannel(r, r.patterns[0], ch);
    expect(ev.size() == 1 && noteOnTick(ev[0]) == 24.0 && near(ev[0].velocity, 100.0 / 127.0, 1e-6),
           "presets: steps-only songs leave piano-roll notes straight and unaccented");
    r.swingNotes = true;
    r.channels[0].swingMix = 0.5f;
    ev = compilePatternChannel(r, r.patterns[0], ch);
    expect(ev.size() == 1 && near(noteOnTick(ev[0]), 24.0 * (1.0 + 0.25 * 0.48), 1e-4) &&
               near(ev[0].velocity, 100.0 / 127.0 * 0.95, 1e-5),
           "presets: a channel's swing mix scales the timing and the accents");
  }
  // Block size never changes where grooved notes land.
  {
    Song r = s;
    gv::apply(r, *gv::findFactory("Loose hands"));
    RenderOptions a, b;
    a.blockSize = 512, b.blockSize = 37;
    expect(renderSong(r, a).data == renderSong(r, b).data, "render: a grooved pattern is block-size invariant");
  }
}

// ------------------------------------------------------------------------------------------
void testGrids() {
  ObjectId ch = kNoId;
  Song s = grooveSong(96, ch);
  auto onTicks = [&](const Song& song, const Pattern& p) {
    std::vector<double> v;
    for (const NoteEv& e : compilePatternChannel(song, p, ch)) v.push_back(noteOnTick(e));
    std::sort(v.begin(), v.end());
    return v;
  };
  // 8ths in a 16-step pattern: the off-beat 8th moves, the 16ths between follow.
  Pattern p = s.patterns[0];
  p.steps[ch] = std::vector<uint8_t>(16, 100);
  s.swing = 0.5f;
  s.groove.shape.grid = gv::kGrid8th;
  auto v = onTicks(s, p);
  expect(v.size() == 16 && v[0] == 0 && near(v[1], 24 * 1.25, 1e-9) && near(v[2], 48 * 1.25, 1e-9) && near(v[4], 96, 1e-9),
         "grid 8ths: the off-beat 8th moves a quarter of an 8th, the 16th inside follows, the beat stays");
  // 16ths in a 32-step pattern: the step grid would swing 32nds, the 16th grid swings 16ths.
  p.stepsPerBar = 32;
  p.steps[ch] = std::vector<uint8_t>(32, 100);
  s.groove.shape.grid = gv::kGrid16th;
  v = onTicks(s, p);
  expect(v.size() == 32 && near(v[2], 24 * 1.25, 1e-9) && near(v[1], 12 * 1.25, 1e-9) && v[4] == 48,
         "grid 16ths: a 32-step pattern swings 16ths");
  s.groove.shape.grid = gv::kGridStep;
  v = onTicks(s, p);
  expect(v.size() == 32 && near(v[1], 12 * 1.25, 1e-9) && v[2] == 24, "grid step: a 32-step pattern swings its own steps (as before)");
  // A cycle that does not divide the bar: the last partial cycle stays straight.
  p.stepsPerBar = 16;
  p.steps[ch] = std::vector<uint8_t>(16, 100);
  s.groove.shape = GrooveShape{};
  s.groove.shape.grid = gv::kGrid16th;
  s.groove.shape.cycle = 3;
  s.groove.shape.timing = {0.0f, 0.5f, 0.0f};
  s.swing = 1.0f;
  v = onTicks(s, p);
  expect(v.size() == 16 && near(v[1], 36, 1e-9) && near(v[13], 13 * 24 + 12, 1e-9) && v[15] == 15 * 24,
         "cycle 3: every third 16th group repeats, the bar's last 16th (partial cycle) stays straight");
  // Early cells never go before the pattern start.
  s.groove.shape = GrooveShape{};
  s.groove.shape.grid = gv::kGrid16th;
  s.groove.shape.cycle = 2;
  s.groove.shape.timing = {-0.5f, 0.0f};
  v = onTicks(s, p);
  expect(v.size() == 16 && v[0] == 0 && near(v[2], 48 - 12, 1e-9), "early cells: the first note stays at 0, later downbeats come early");
}

// K-203: without a pattern, the step grid and the swing's bar follow the meter in force.
void testCellTicksMeterInForce() {
  Song s = makeEmptySong();  // ppq 96, 4/4
  expect(gv::cellTicks(s, nullptr, gv::kGridStep) == 24.0 && gv::cellTicks(s, nullptr, gv::kGridStep, 5000) == 24.0,
         "meter in force: no meter changes, the step is a sixteenth as before");
  Marker seven, tiny;
  seven.pos = 4 * 384, seven.timeSigNum = 7, seven.timeSigDen = 8;          // bar 5: 7/8
  tiny.pos = 4 * 384 + 4 * 336, tiny.timeSigNum = 3, tiny.timeSigDen = 32;  // after four bars of 7/8: 3/32
  s.markers = {seven, tiny};
  expect(gv::cellTicks(s, nullptr, gv::kGridStep, 0) == 24.0 && gv::cellTicks(s, nullptr, gv::kGridStep, seven.pos + 10) == 24.0,
         "meter in force: 4/4 and 7/8 steps stay sixteenths");
  // 3/32: a bar of 36 ticks holds 2 steps (a new pattern there gets 2), so a step is 18 ticks.
  expect(gv::cellTicks(s, nullptr, gv::kGridStep, tiny.pos + 1) == 18.0, "meter in force: a 3/32 section steps by its own new-pattern step");
  expect(gv::cellTicks(s, nullptr, gv::kGrid8th, tiny.pos + 1) == 48.0, "meter in force: note grids stay note values");
  const Pattern& p = s.patterns[0];
  expect(gv::cellTicks(s, &p, gv::kGridStep, tiny.pos + 1) == static_cast<double>(s.ticksPerStep(p)), "meter in force: a pattern keeps its own step");
  GrooveShape sh;
  const gv::Warp w0 = gv::makeWarp(s, nullptr, sh, 0.5, 0.0), w7 = gv::makeWarp(s, nullptr, sh, 0.5, 0.0, seven.pos);
  expect(w0.bar == 384.0 && w7.bar == 336.0 && w7.cell == 24.0, "meter in force: the song-time swing's bar is the bar of the meter at its position");
}

// ------------------------------------------------------------------------------------------
void testPresetFiles(const std::string& root) {
  gv::Preset p = *gv::findFactory("Loose hands");
  p.name = "My pocket";
  p.amount = 0.6f;
  p.accent = 0.5f;
  p.factory = false;
  std::string path;
  expect(gv::saveUserPreset(root, p, "My pocket", &path) && fs::exists(fs::u8path(path)), "files: a preset saves into grooves/");
  expect(gv::saveUserPreset(root, p, "Drums/Late/Pocket two"), "files: a preset saves into folders");
  const auto list = gv::listUserPresets(root);
  bool mine = false, folder = false;
  for (const auto& u : list) {
    mine = mine || (u.name == "My pocket" && u.folder.empty() && u.shape == p.shape && u.amount == 0.6f && u.accent == 0.5f);
    folder = folder || (u.name == "Pocket two" && u.folder == "Drums/Late");
  }
  expect(mine && folder, "files: listed with folders, read back exactly");
  gv::Preset found;
  expect(gv::findPreset(root, "Drums/Late/Pocket two", found) && found.name == "Pocket two", "files: found by folder and name");
  expect(gv::findPreset(root, "easy SHUFFLE", found) && found.factory, "files: factory presets by name, any case");
  expect(gv::findPreset(root, "Light swing", found) && found.name == "Gentle 16ths", "files: the quantize tool's older names still find their preset");
  // The library does not mistake the folder for a plugin type.
  expect(browse::libraryTypes(root).empty(), "files: grooves/ is not listed as a plugin's presets");
  // Text format: readable, versioned.
  const std::string text = gv::toJsonText(p);
  expect(text.find("\"format\": \"gridline-groove\"") != std::string::npos && text.find("\"version\": 1") != std::string::npos &&
             text.find("\"grid\": \"1/16\"") != std::string::npos,
         "files: small readable JSON with format and version");
  // Bad and short files.
  gv::Preset q;
  std::string err;
  expect(!gv::fromJsonText("{\"format\":\"gridline-automation-shape\"}", q, &err) && !err.empty(), "files: another kind of file is refused");
  expect(!gv::fromJsonText("{\"format\":\"gridline-groove\"}", q, &err), "files: a preset without timing is refused");
  expect(!gv::fromJsonText("not json", q, &err), "files: not JSON is refused");
  expect(gv::fromJsonText(R"({"format":"gridline-groove","version":7,"future":{"x":1},"grid":"1/8","cycle":4,"timing":[0.2],"velocity":[3,-1],"amount":9})",
                          q) &&
             q.shape.grid == gv::kGrid8th && q.shape.cycle == 4 && q.shape.timing.size() == 4 && q.shape.timing[0] == 0.2f &&
             q.shape.timing[3] == 0.0f && q.shape.velocity.size() == 4 && q.shape.velocity[0] == 2.0f && q.shape.velocity[1] == 0.0f &&
             q.shape.velocity[2] == 1.0f && q.amount == 1.0f,
         "files: newer files load what they can; short lanes are padded, values clamped");
  expect(gv::fromJsonText(R"({"format":"gridline-groove","grid":"1/7","cycle":5,"timing":[9,-9,0,0,0,0]})", q) && q.shape.grid == gv::kGridStep &&
             q.shape.cycle == 4 && q.shape.timing.size() == 4 && q.shape.timing[0] == 0.5f && q.shape.timing[1] == -0.5f,
         "files: an unknown grid and cycle fall back, timing is clamped to half a cell");
  // Delete.
  for (const auto& u : gv::listUserPresets(root))
    if (u.name == "Pocket two") expect(gv::deleteUserPreset(u), "files: a user preset deletes");
  expect(!gv::deleteUserPreset(*gv::findFactory("Straight")), "files: a factory preset cannot be deleted");
}

// ------------------------------------------------------------------------------------------
void testProjectAndUndo() {
  ObjectId ch = kNoId;
  Song s = grooveSong(96, ch);
  gv::apply(s, *gv::findFactory("Late hats"));
  Song back;
  expect(songFromJson(songToJson(s), back) && back.groove == s.groove && back.swing == s.swing && back == s,
         "project: a preset's groove and name survive save and load");
  // A damaged groove key loads as a usable groove.
  Json j = songToJson(s);
  j.object["groove"].object["cycle"] = Json::makeNumber(99);
  j.object["groove"].object["timing"] = Json::makeArray();
  j.object["groove"].object["accent"] = Json::makeNumber(-4);
  expect(songFromJson(j, back) && gv::validCycle(back.groove.shape.cycle) &&
             back.groove.shape.timing.size() == static_cast<size_t>(back.groove.shape.cycle) && back.groove.accent == 0.0f,
         "project: a damaged groove is repaired on load");
  // Undo: one step.
  Document doc(grooveSong(96, ch));
  const Song before = doc.song();
  const gv::Preset& pr = *gv::findFactory("Easy shuffle");
  const size_t pos = doc.history().position();
  doc.editSong("swing preset: " + pr.name, [&pr](Song& x) { gv::apply(x, pr); });
  expect(doc.history().position() == pos + 1 && doc.song().swing == pr.amount && doc.song().groove.preset == pr.name,
         "undo: applying a preset is one step");
  expect(doc.undo() && doc.song() == before, "undo: one undo restores the swing amount and the groove");
  expect(doc.redo() && gv::matches(doc.song(), pr), "undo: redo applies it again");
  // The chip's label.
  std::vector<gv::Preset> all = gv::factoryPresets();
  expect(gv::chipLabel(doc.song(), all) == "Easy shuffle", "label: the preset's name");
  doc.editSong("swing", [](Song& x) { x.swing = 0.5f; });
  expect(gv::chipLabel(doc.song(), all) == "Easy shuffle (edited)", "label: edited after a change");
  expect(gv::chipLabel(before, all) == "Straight", "label: Straight without swing");
  Song sw = before;
  sw.swing = 0.3f;
  expect(gv::chipLabel(sw, all) == "Step swing", "label: the classic swing without a preset");
  expect(gv::feelText(pr.shape, pr.amount) == "16ths, off-beat at 58 % of the pair", "label: the feel of a pair swing");
}

// ------------------------------------------------------------------------------------------
void testCollab() {
  using namespace gl::collab;
  using namespace sim;
  Clock clock;
  Net net(151);
  Peer a, b;
  a.label = "Ann", b.label = "Ben";
  for (Peer* p : {&a, &b}) p->id = crypto::Identity::generate();
  ObjectId ch = kNoId;
  a.doc.reset(grooveSong(96, ch));
  a.folder = tmpDir("k151_a");
  b.folder = tmpDir("k151_b");
  std::string err;
  a.s = Session::create(a.doc, config(a.folder, a.id, a.label, &clock), &err);
  expect(a.s != nullptr, "collab: a session");
  if (!a.s) return;
  net.add(&a);
  b.s = Session::join(b.doc, config(b.folder, b.id, b.label, &clock), ticketOf(*a.s), &err);
  a.s->admitMember(memberOf(b));
  net.add(&b);
  net.connect(&a, &b);
  settle(net, {&a, &b}, clock);
  const gv::Preset& pr = *gv::findFactory("Loose hands");
  a.doc.editSong("swing preset", [&pr](Song& x) { gv::apply(x, pr); });
  settle(net, {&a, &b}, clock);
  expect(b.doc.song().groove == a.doc.song().groove && b.doc.song().swing == pr.amount && gv::matches(b.doc.song(), pr),
         "collab: a preset applied on one computer arrives on the other");
  b.doc.editSong("swing steps only", [](Song& x) { x.swingNotes = false; });
  settle(net, {&a, &b}, clock);
  expect(!a.doc.song().swingNotes, "collab: \"swing piano roll notes\" switched off reaches the collaborator (was never sent)");
  b.doc.editSong("swing groove", [](Song& x) { x.groove.shape.timing[1] = 0.25f; });
  settle(net, {&a, &b}, clock);
  expect(a.doc.song().groove.shape.timing[1] == 0.25f && songText(a) == songText(b), "collab: a groove edit arrives, both songs agree");
}

// ------------------------------------------------------------------------------------------
uint32_t mainSwingOf(const flp::Bytes& bytes) {
  flp::File f;
  std::string err;
  if (!flp::readBytes(bytes, f, &err)) return 9999;
  for (const auto& e : f.events)
    if (e.id == 11) return e.value();
  return 9999;
}

void testFlp() {
  ObjectId ch = kNoId;
  Song s = grooveSong(96, ch);
  s.patterns[0].steps[ch] = std::vector<uint8_t>(16, 100);
  flp::ExportOptions opt;
  opt.renderInstruments = false;
  auto exportOf = [&](const Song& song, flp::ExportReport& rep) {
    flp::Bytes out;
    flp::exportFlp(song, out, rep, opt);
    return out;
  };
  auto warned = [](const flp::ExportReport& rep) {
    for (const auto& w : rep.warnings)
      if (w.find("swing") != std::string::npos) return true;
    return false;
  };
  {  // classic swing: written as before, and back
    s.swing = 0.5f;
    flp::ExportReport rep;
    const auto out = exportOf(s, rep);
    expect(mainSwingOf(out) == 64 && !warned(rep), "flp: the classic swing is FL's main swing (64 of 128), no warning");
    Song back;
    flp::Report r;
    std::string err;
    expect(importFlpBytes(out, back, r, &err) && back.swing == 0.5f && back.groove.isDefault(), "flp: and comes back the same");
  }
  {  // a 16th pair swing on 16-step patterns says the same in FL
    Song t = s;
    gv::apply(t, *gv::findFactory("Easy shuffle"));
    flp::ExportReport rep;
    const auto out = exportOf(t, rep);
    expect(mainSwingOf(out) == static_cast<uint32_t>(std::lround(0.32 * 128)) && !warned(rep), "flp: a 16th pair swing on 16-step patterns maps exactly");
  }
  {  // 8ths: FL swings steps only; the amount goes, with a warning
    Song t = s;
    gv::apply(t, *gv::findFactory("Lilting 8ths"));
    flp::ExportReport rep;
    const auto out = exportOf(t, rep);
    expect(mainSwingOf(out) == static_cast<uint32_t>(std::lround(0.32 * 128)) && warned(rep), "flp: an 8th swing writes its amount and warns");
  }
  {  // a groove template: no FL equivalent
    Song t = s;
    gv::apply(t, *gv::findFactory("Loose hands"));
    flp::ExportReport rep;
    const auto out = exportOf(t, rep);
    expect(mainSwingOf(out) == 0 && warned(rep), "flp: a groove template writes no main swing and warns that it stays in Gridline");
  }
}

// ------------------------------------------------------------------------------------------
void testExtraction(const std::string& dir) {
  // From notes: a played 58 % swing.
  std::vector<Note> notes;
  for (int i = 0; i < 32; ++i) notes.push_back(Note(i * 24 + (i % 2 ? 4 : 0), 6, 60, i % 2 ? 80 : 100));
  gv::Preset p = gv::extract(gv::onsetsOfNotes(notes), 24, 384, gv::kGrid16th, 16, "played");
  expect(p.shape.cycle == 2 && p.shape.timing[0] == 0.0f && p.shape.timing[1] == 0.5f && near(p.amount, 8.0 / 24.0, 1e-6),
         "extract: a played swing becomes a pair at its amount (cycle shortened)");
  expect(p.shape.velocity.size() == 2 && near(p.shape.velocity[1], 0.8, 1e-3), "extract: the accents are kept when the levels vary");
  // Applying the extracted groove places notes where they were played.
  {
    ObjectId ch = kNoId;
    Song s = grooveSong(96, ch);
    gv::apply(s, p);
    s.patterns[0].notes[ch] = {Note(24, 6, 60, 100)};
    const auto ev = compilePatternChannel(s, s.patterns[0], ch);
    expect(ev.size() == 1 && near(noteOnTick(ev[0]), 28, 1e-4), "extract: the groove plays the off-beat where it was played");
  }
  // From an audio clip's transients.
  const double sr = 44100.0, sec16 = 0.125;  // 120 BPM: a 16th is 0.125 s
  auto clicks = [&](const std::string& name, double late) {
    AudioBuffer b(2, static_cast<size_t>(2.2 * sr));
    for (int k = 0; k < 16; ++k) {
      const size_t a = static_cast<size_t>((k * sec16 + (k % 2 ? late : 0)) * sr);
      for (size_t i = 0; i < static_cast<size_t>(0.008 * sr) && a + i < b.frames; ++i)
        b.at(0, a + i) = b.at(1, a + i) = static_cast<float>((k % 2 ? 0.5 : 0.9) * std::sin(2 * 3.14159265358979 * 1000 * static_cast<double>(i) / sr));
    }
    const std::string path = (fs::path(dir) / name).string();
    writeWav(path, b, static_cast<int>(sr), 24);
    return path;
  };
  auto audioSong = [&](const std::string& path, ObjectId& clip) {
    Song s = makeEmptySong();
    s.tempo = 120;
    Channel c;
    c.id = s.allocateId();
    c.name = "Clicks";
    c.plugin = defaultPluginState("gl.sampler");
    c.plugin.data = path;
    c.mixerTrack = s.masterId();
    c.audioClip = true;
    s.channels.push_back(c);
    Clip k;
    k.id = clip = s.allocateId();
    k.kind = ClipKind::Audio;
    k.source = c.id;
    k.start = s.barTicks();
    k.length = s.barTicks();
    s.playlist.push_back(k);
    return s;
  };
  {
    ObjectId clip = kNoId;
    Song s = audioSong(clicks("k151_swung.wav", 0.32 * 0.5 * sec16), clip);
    TempoMap tm;
    tm.build(s);
    const auto on = clipGrooveOnsets(s, *s.clip(clip), tm);
    gv::Preset g = gv::extract(on, 24, 384, gv::kGrid16th, 16, "from audio");
    std::printf("# audio: %zu onsets, cycle %d, amount %.4f\n", on.size(), g.shape.cycle, static_cast<double>(g.amount));
    // (The click on the file's very first sample has no lead-in for the transient detector: 15 or 16.)
    expect(on.size() >= 15 && on.size() <= 16 && g.shape.cycle == 2 && near(g.amount, 0.32, 0.02) && g.shape.timing[1] == 0.5f,
           "extract: an audio clip's transients give its swing (58 %, within 1 %)");
    expect(g.shape.velocity.size() == 2 && g.shape.velocity[1] < 0.7f, "extract: the quieter off-beats become accents");
  }
  {  // A silent hit (velocity 0, as some templates have) never mutes the cell.
    std::vector<gv::Onset> on;
    for (int k = 0; k < 16; ++k) on.push_back({24.0 * k, k % 2 ? 0.0f : 1.0f});
    const gv::Preset g = gv::extract(on, 24, 384, gv::kGrid16th, 2, "silent");
    expect(g.shape.velocity.size() == 2 && g.shape.velocity[1] >= 0.1f && g.shape.velocity[1] < 0.2f,
           "extract: a cell is never silenced (a tenth at least)");
  }
  {  // Applied to a straight clip through warp markers: each transient plays on the swung grid.
    ObjectId clip = kNoId;
    Song s = audioSong(clicks("k151_straight.wav", 0.0), clip);
    gv::apply(s, *gv::findFactory("Easy shuffle"));
    TempoMap tm;
    tm.build(s);
    Document doc(s);
    const size_t pos = doc.history().position();
    Clip edited = *doc.song().clip(clip);
    const int moved = grooveAudioClip(doc.song(), edited, tm, 1.0);
    doc.editClip(clip, "swing the audio clip", [&edited](Clip& k) { k.warp = edited.warp; });
    expect(moved == 8 && doc.history().position() == pos + 1, "audio: the 8 off-beats move, one undo step");
    bool onGrid = true;
    for (int k = 0; k < 16; ++k) {
      const double t = clipTickOfSource(doc.song(), *doc.song().clip(clip), tm, k * sec16 + 0.0005);
      const double want = 384.0 + k * 24 + (k % 2 ? 0.16 * 24 : 0) + 0.0005 * 192.0;
      onGrid = onGrid && near(t, want, 0.6);
      if (!near(t, want, 0.6)) std::printf("# click %d: %.3f want %.3f\n", k, t, want);
    }
    expect(onGrid, "audio: every transient plays on the swung grid (within 3 ms)");
    expect(doc.undo() && doc.song().clip(clip)->warp.empty(), "audio: undo takes the warp back");
  }
  // From a score file (written here: the FLP event format, notes in event 224).
  {
    std::vector<uint8_t> f = {'F', 'L', 'h', 'd', 6, 0, 0, 0, 0x10, 0, 1, 0, 96, 0, 'F', 'L', 'd', 't', 0, 0, 0, 0};
    std::vector<uint8_t> ev = {224};
    std::vector<uint8_t> rec;
    for (int i = 0; i < 16; ++i) {
      const uint32_t pos = static_cast<uint32_t>(i * 24 + (i % 2 ? 6 : 0));
      uint8_t r[24] = {};
      for (int b = 0; b < 4; ++b) r[b] = static_cast<uint8_t>(pos >> (8 * b));
      r[8] = 12;
      r[12] = 60;
      r[21] = 100;
      rec.insert(rec.end(), r, r + 24);
    }
    uint32_t n = static_cast<uint32_t>(rec.size());
    while (n >= 0x80) ev.push_back(static_cast<uint8_t>((n & 0x7F) | 0x80)), n >>= 7;
    ev.push_back(static_cast<uint8_t>(n));
    ev.insert(ev.end(), rec.begin(), rec.end());
    const uint32_t len = static_cast<uint32_t>(ev.size());
    for (int b = 0; b < 4; ++b) f[18 + static_cast<size_t>(b)] = static_cast<uint8_t>(len >> (8 * b));
    f.insert(f.end(), ev.begin(), ev.end());
    const std::string path = (fs::path(dir) / "16_test groove.fsc").string();
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(f.data()), static_cast<std::streamsize>(f.size()));
    gv::Preset g;
    std::string err;
    const bool ok = gv::fromScoreFile(path, g, &err);
    if (!ok) std::printf("# score: %s\n", err.c_str());
    expect(ok && g.shape.grid == gv::kGrid16th && g.shape.cycle == 2 && near(g.amount, 0.5, 1e-6) && g.name == "16_test groove",
           "score file: its notes give the groove (16ths from the name, a 62 % swing)");
  }
}

// ------------------------------------------------------------------------------------------
void testQuantize(const std::string& root) {
  using namespace gl::tools;
  const auto& g = grooves();
  bool same = g.size() == gv::factoryPresets().size();
  for (size_t i = 0; same && i < g.size(); ++i) {
    const gv::Preset& p = gv::factoryPresets()[i];
    same = std::string(g[i].name) == p.name && g[i].shift.size() == p.shape.timing.size();
    for (size_t k = 0; same && k < g[i].shift.size(); ++k)
      same = g[i].shift[k] == static_cast<double>(p.shape.timing[k]) * static_cast<double>(p.amount);
  }
  expect(same, "quantize: the piano roll's grooves are the swing presets (shift = timing x amount)");
  // A user preset quantizes with its timing and accents.
  gv::Preset u;
  u.name = "Mine";
  u.shape.grid = gv::kGrid16th;
  u.shape.timing = {0.0f, 0.5f};
  u.shape.velocity = {1.0f, 0.5f};
  u.amount = 0.5f;
  gv::saveUserPreset(root, u, "Mine");
  gv::Preset back;
  expect(gv::findPreset(root, "Mine", back), "quantize: a saved preset is found");
  std::vector<Note> n = {Note(23, 6, 60, 100)};
  NoteSel sel(n.size(), false);
  QuantizeOpts o;
  o.grid = 24;
  o.grooveVelocity = 1.0;
  quantizeWithGroove(n, sel, o, grooveOf(back));
  expect(n[0].start == 30 && n[0].velocity == 50, "quantize: a user preset moves the note to its swung line and applies its accent");
}

// ------------------------------------------------------------------------------------------
void testScripts(const std::string& root) {
  {  // MCP
    using namespace gl::mcp;
    ObjectId ch = kNoId;
    Document doc(grooveSong(96, ch));
    script::SimpleProjectHost host{doc};
    Environment env{doc, host};
    env.presetRoot = root;
    double t = 1000;
    env.now = [&t] { return t += 0.5; };
    Session session{env};
    int id = 1;
    auto rpc = [&](const std::string& method, Json params) {
      Json m = jobj({{"jsonrpc", jstr("2.0")}, {"id", jnum(id++)}, {"method", jstr(method)}, {"params", params}});
      Json r;
      parse(session.handle(dump(m)), r);
      return r;
    };
    rpc("initialize", jobj({{"protocolVersion", jstr("2025-06-18")}, {"capabilities", Json::makeObject()},
                            {"clientInfo", jobj({{"name", jstr("t")}, {"version", jstr("1")}})}}));
    session.handle(R"({"jsonrpc":"2.0","method":"notifications/initialized"})");
    const size_t pos = doc.history().position();
    Json r = rpc("tools/call", jobj({{"name", jstr("set_song")}, {"arguments", jobj({{"swingPreset", jstr("Triplet 8ths")}})}}));
    const gv::Preset& pr = *gv::findFactory("Triplet 8ths");
    expect(gv::matches(doc.song(), pr) && doc.history().position() == pos + 1, "mcp: set_song applies a swing preset in one undo step");
    r = rpc("tools/call", jobj({{"name", jstr("list_swing_presets")}, {"arguments", Json::makeObject()}}));
    const std::string txt = dump(r);
    expect(txt.find("Easy shuffle") != std::string::npos && txt.find("Mine") != std::string::npos && txt.find("Triplet 8ths") != std::string::npos,
           "mcp: list_swing_presets lists factory and user presets and the current one");
    r = rpc("tools/call", jobj({{"name", jstr("set_song")}, {"arguments", jobj({{"swingPreset", jstr("No such feel")}})}}));
    expect(dump(r).find("isError") != std::string::npos, "mcp: an unknown preset is an error");
  }
  {  // Lua
    ObjectId ch = kNoId;
    Document doc(grooveSong(96, ch));
    script::SimpleProjectHost host{doc};
    host.presetRoot = root;
    script::Vm vm;
    script::openProjectApi(vm, host);
    const size_t pos = doc.history().position();
    auto r = script::runProject(vm, host,
                                "assert(project.song.applySwingPreset('Easy shuffle'))\n"
                                "assert(project.song.swingPreset() == 'Easy shuffle')\n"
                                "local g = project.song.groove()\n"
                                "assert(g.grid == '1/16' and g.cycle == 2 and g.timing[2] == 0.5)\n"
                                "project.song.setGroove({grid = '1/8', cycle = 4, timing = {0, 0.1, 0, 0.2}})\n"
                                "assert(#project.song.swingPresets() >= 12)\n",
                                "t", "script: groove");
    if (!r.ok) std::printf("# lua: %s\n", r.error.c_str());
    expect(r.ok && doc.history().position() == pos + 1 && doc.song().groove.shape.grid == gv::kGrid8th && doc.song().groove.shape.cycle == 4 &&
               doc.song().groove.shape.timing[3] == 0.2f && doc.song().swing == 0.32f,
           "lua: swing presets and the groove from scripts, one undo step");
  }
}

}  // namespace

// A frozen channel's render is taken with the song's groove; the freeze goes out of date when the
// groove changes (not when only the preset's name does), and the channel live again swings the same.
void testFreeze() {
  const std::string dir = (fs::path(gl::test::hermeticTestEnv("k151")) / "k151_freeze").string();
  auto firstOf = [&](const Song& s, ObjectId ch, const char* tag) -> long {
    FreezeRecord r;
    std::string why;
    if (!freeze::render(s, freeze::channelUnit(ch), 48000, (fs::path(dir) / tag).string(), r, &why)) {
      std::printf("# freeze render failed: %s\n", why.c_str());
      return -2;
    }
    SamplePtr smp = loadSample(r.render.data);
    if (!smp) return -3;
    for (size_t i = 0; i < smp->frames(); ++i)
      if (std::fabs(smp->left[i]) > 1e-5f) return static_cast<long>(i);
    return -1;
  };
  ObjectId ch = kNoId;
  Song straight = grooveSong(96, ch);
  straight.patterns[0].steps[ch] = std::vector<uint8_t>(16, 0);
  straight.patterns[0].steps[ch][1] = 100;  // the off-beat 16th only
  Song swung = straight;
  gv::apply(swung, *gv::findFactory("Easy shuffle"));
  const long a = firstOf(straight, ch, "straight"), b = firstOf(swung, ch, "swung");
  // 120 BPM: a 16th is 6000 samples; Easy shuffle (0.32) puts the off-beat 0.16 of a 16th late.
  std::printf("# freeze: straight %ld swung %ld\n", a, b);
  expect(a > 0 && b > 0 && std::labs((b - a) - 960) <= 2, "freeze: a frozen channel's render plays the song's groove");
  const std::string f0 = freeze::fingerprint(swung, freeze::channelUnit(ch));
  Song renamed = swung;
  renamed.groove.preset = "Drums/My shuffle";
  Song other = swung;
  other.groove.shape.timing[1] = 0.25f;
  expect(freeze::fingerprint(renamed, freeze::channelUnit(ch)) == f0 && freeze::fingerprint(other, freeze::channelUnit(ch)) != f0 &&
             freeze::fingerprint(straight, freeze::channelUnit(ch)) != f0,
         "freeze: a groove change makes the freeze out of date, a preset's new name does not");
  // Live again (no freeze): the channel plays the same swung notes as before freezing.
  std::vector<NoteEv> live = compileChannelNotes(swung, swung.patterns[0], swung.channels[0]);
  expect(!live.empty() && std::fabs((static_cast<double>(live[0].on) + live[0].onFrac) - (24.0 + 24.0 * 0.16)) < 1e-6, "freeze: live again, the step swings as before");
  std::error_code ec;
  fs::remove_all(dir, ec);
}

int main() {
  const std::string& tmp = gl::test::hermeticTestEnv("k151");
  const std::string root = (fs::path(tmp) / "presets").string();
  testClassicIsBitIdentical();
  testFactoryPresetsPlaceNotes();
  testGrids();
  testCellTicksMeterInForce();
  testPresetFiles(root);
  testProjectAndUndo();
  testCollab();
  testFlp();
  testExtraction(tmp);
  testQuantize(root);
  testScripts(root);
  testFreeze();
  return finish();
}
