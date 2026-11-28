// Gridline - Track Receipt per marker area: the shared area helper (core/MarkerAreas.h), the
// report's areas from timed reads, rough reads and reads whose time is not known, the outputs
// (spreadsheet, PDF), and areas measured by playing projects through. Synthetic audio only.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>
#include "../../src/core/MarkerAreas.h"
#include "../../src/core/PluginInfo.h"
#include "../../src/core/Project.h"
#include "../../src/core/Song.h"
#include "../../src/core/TrackReceipt.h"
#include "../../src/core/WavWriter.h"
#include "../../src/engine/TrackReceiptPass.h"
#include "../Check.h"

using namespace gl;
using namespace gl::receipt;
namespace fs = std::filesystem;

namespace {

bool near(double a, double b, double tol = 0.051) { return std::fabs(a - b) <= tol; }

Marker marker(Tick pos, const std::string& name) {
  Marker m;
  m.pos = pos, m.name = name;
  return m;
}

void testHelper() {
  using markers::areas;
  expect(areas({}, 0, 1000).empty(), "areas: no markers, no areas (one whole section)");
  {
    const auto a = areas({marker(0, "One"), marker(400, "Two"), marker(700, "Three")}, 0, 1000);
    expect(a.size() == 3 && a[0].start == 0 && a[0].end == 400 && a[1].end == 700 && a[2].end == 1000, "areas: marker to marker, the last to the end");
    expect(a[0].name == "One" && a[2].name == "Three" && a[1].marker == 1, "areas: named by their markers");
  }
  {
    const auto a = areas({marker(700, "B"), marker(300, "A")}, 0, 1000);
    expect(a.size() == 3 && a[0].marker == -1 && a[0].name.empty() && a[0].end == 300 && a[1].name == "A" && a[2].name == "B",
           "areas: unsorted markers; the stretch before the first is an area of its own");
  }
  {
    const auto a = areas({marker(300, "A"), marker(300, "A2"), marker(1000, "End"), marker(1500, "After")}, 0, 1000);
    expect(a.size() == 2 && a[1].name == "A", "areas: markers at one place make one area; at or after the end none");
  }
  {
    const auto a = areas({marker(50, "Early"), marker(100, "Start"), marker(600, "Later")}, 200, 1000);
    expect(a.size() == 2 && a[0].start == 200 && a[0].name == "Start" && a[1].start == 600, "areas: a marker before the start names the first area");
  }
  expect(areas({marker(10, "x")}, 500, 500).empty(), "areas: an empty range has none");
  {
    // Meter and key markers change the meter or key inside an area: they cut nothing, as in the
    // Export audio dialog's marker areas (one helper for both).
    Marker meter = marker(300, "");
    meter.timeSigNum = 3, meter.timeSigDen = 4;
    Marker key = marker(600, "D minor");
    key.keyRoot = 2;
    const auto a = areas({marker(0, "One"), meter, key}, 0, 1000);
    expect(a.size() == 1 && a[0].start == 0 && a[0].end == 1000 && a[0].name == "One", "areas: meter and key markers do not split areas");
    const auto b = areas({marker(300, ""), marker(300, "Named"), marker(300, "Later")}, 0, 1000);
    expect(b.size() == 2 && b[1].name == "Named" && b[1].marker == 1, "areas: markers at one place are named by the first named one");
  }
}

// Two areas [0, 10) and [10, 20) s; file A read 0-2 s at song 0-2 s and again at 12-14 s; file B
// only at 15-16 s.
Measurement timedMeasurement() {
  Measurement m;
  m.mode = "song";
  m.files.push_back(FileIn{"/s/Night Garden vocals 90bpm.wav", 30, true, false, {}});
  m.files.push_back(FileIn{"/s/Night Garden drums 90bpm.wav", 30, true, false, {}});
  m.files.push_back(FileIn{"/s/Paper Moon keys.wav", 30, true, false, {}});
  Area a0, a1;
  a0.name = "Song one", a0.a = 0, a0.b = 10, a0.bar0 = 1, a0.bar1 = 5;
  a1.name = "Song two", a1.a = 10, a1.b = 20, a1.bar0 = 6, a1.bar1 = 10;
  m.areas = {a0, a1};
  Use u;
  u.measured = true;
  u.file = m.files[0].path, u.where = "vocals", u.ranges = {{0, 2}};
  u.timed = {{0, 2, 0, 2}, {12, 14, 0, 2}};
  m.uses.push_back(u);
  u.file = m.files[1].path, u.where = "drums", u.ranges = {{5, 6}};
  u.timed = {{15, 16, 5, 6}};
  m.uses.push_back(u);
  return m;
}

Saved tickAll(const Measurement& m) {
  Saved sv;
  for (const FileIn& f : m.files) sv.files[f.path].ticked = 1;
  return sv;
}

const AreaSong* areaSong(const AreaRow& ar, const std::string& key) {
  for (const AreaSong& s : ar.songs)
    if (s.key == key) return &s;
  return nullptr;
}

void testTimed() {
  const Measurement m = timedMeasurement();
  const Report r = build(m, tickAll(m), "t");
  expect(r.songs.size() == 2 && r.areas.size() == 2, "report: two songs, two areas");
  const SongRow* ng = nullptr;
  for (const SongRow& s : r.songs)
    if (s.key == "night garden") ng = &s;
  expect(ng && near(ng->seconds, 3.0), "report: over the whole song, a part used twice counts once (2 + 1 s)");
  if (r.areas.size() != 2) return;
  const AreaSong* s0 = areaSong(r.areas[0], "night garden");
  const AreaSong* s1 = areaSong(r.areas[1], "night garden");
  expect(s0 && near(s0->seconds, 2.0) && near(s0->playSeconds, 2.0) && s0->tier == 2 && !s0->rough,
         "area 1: the vocals' 2.0 s, sounding 2.0 s, up to 2 s");
  expect(s1 && near(s1->seconds, 3.0) && near(s1->playSeconds, 3.0) && s1->tier == 15, "area 2: the vocals again and the drums: 3.0 s");
  expect(s1 && s1->files.size() == 2 && s0 && s0->files.size() == 1, "areas: each lists the files used in it");
  if (s1 && s1->files.size() == 2) {
    double vocals = -1, drums = -1;
    for (const AreaFile& f : s1->files) (f.name.find("vocals") != std::string::npos ? vocals : drums) = f.seconds;
    expect(near(vocals, 2.0) && near(drums, 1.0), "area 2: the seconds of each file");
  }
  expect(!areaSong(r.areas[0], "paper moon") && !areaSong(r.areas[1], "paper moon"), "areas: a song never played is in no area");
  expect(r.areas[0].area.name == "Song one" && r.areas[1].area.bar0 == 6, "areas: name and bars kept");
  // A read across the marker: split where the marker is.
  Measurement x = m;
  x.uses[0].timed = {{8, 12, 0, 4}};
  x.uses[0].ranges = {{0, 4}};
  const Report rx = build(x, tickAll(x), "t");
  const AreaSong* x0 = areaSong(rx.areas[0], "night garden");
  const AreaSong* x1 = areaSong(rx.areas[1], "night garden");
  expect(x0 && near(x0->seconds, 2.0) && x1 && near(x1->seconds, 3.0), "areas: a read across a marker splits there (0-2 s | 2-4 s and the drums)");
  // Backwards reads map the same way.
  x.uses[0].timed = {{8, 12, 4, 0}};
  const Report rb = build(x, tickAll(x), "t");
  expect(areaSong(rb.areas[0], "night garden") && near(areaSong(rb.areas[0], "night garden")->seconds, 2.0), "areas: a backwards read splits too");
}

void testRough() {
  Measurement m = timedMeasurement();
  // The drums without segments, heard roughly from 5 to 15 s (both areas): counted in each.
  m.uses[1].timed.clear();
  m.uses[1].spans = {{5, 15}};
  Report r = build(m, tickAll(m), "t");
  const AreaSong* s0 = areaSong(r.areas[0], "night garden");
  const AreaSong* s1 = areaSong(r.areas[1], "night garden");
  expect(s0 && s0->rough && near(s0->seconds, 3.0) && s1 && s1->rough, "rough: a read known only roughly counts in every area it may play in");
  // Heard only inside the second area: not rough, only there.
  m.uses[1].spans = {{14, 16}};
  r = build(m, tickAll(m), "t");
  s0 = areaSong(r.areas[0], "night garden");
  s1 = areaSong(r.areas[1], "night garden");
  expect(s0 && !s0->rough && near(s0->seconds, 2.0) && s1 && !s1->rough && near(s1->seconds, 3.0), "rough: a span inside one area is placed there");
  // Not known at all (inside a third-party plugin, nothing placed): every area, marked.
  m.uses[1].spans.clear();
  m.uses[1].estimated = true;
  m.uses[1].measured = false;
  r = build(m, tickAll(m), "t");
  s0 = areaSong(r.areas[0], "night garden");
  expect(s0 && s0->rough && s0->estimated, "rough: a use whose time is not known counts in every area, marked");
  // Parts typed by hand for a file nothing measured: every area.
  Measurement t = timedMeasurement();
  Saved sv = tickAll(t);
  sv.files[t.files[2].path].ranges = "0:01-0:03";
  r = build(t, sv, "t");
  expect(areaSong(r.areas[0], "paper moon") && areaSong(r.areas[1], "paper moon") && areaSong(r.areas[1], "paper moon")->rough,
         "rough: typed parts of a file nothing measured count in every area");
}

void testNoAreas() {
  Measurement m = timedMeasurement();
  const Report with = build(m, tickAll(m), "t");
  m.areas.clear();
  const Report none = build(m, tickAll(m), "t");
  expect(none.areas.empty(), "no markers: no areas");
  m.areas.resize(1);
  expect(build(m, tickAll(m), "t").areas.empty(), "one area is the whole song: no areas");
  bool same = with.songs.size() == none.songs.size();
  for (size_t i = 0; same && i < with.songs.size(); ++i) same = with.songs[i].seconds == none.songs[i].seconds && with.songs[i].timelines == none.songs[i].timelines;
  expect(same, "areas never change the whole-song figures");
  const std::string xml = spreadsheetXml(none);
  expect(xml.find("ss:Name=\"Areas\"") == std::string::npos, "xml: no Areas sheet without markers");
  expect(resultsPdf(none).find("By marker area") == std::string::npos, "pdf: no area section without markers");
}

void testOutputs() {
  const Measurement m = timedMeasurement();
  const Report r = build(m, tickAll(m), "Areas <test>");
  const std::string xml = spreadsheetXml(r);
  expect(xml.find("ss:Name=\"Areas\"") != std::string::npos, "xml: an Areas sheet");
  expect(xml.find("Song one") != std::string::npos && xml.find("Song two") != std::string::npos && xml.find("bars 6-10") != std::string::npos,
         "xml: each area by name and bars");
  expect(xml.find("Night Garden drums 90bpm.wav") != std::string::npos, "xml: the files of each area");
  int opens = 0, closes = 0;
  for (size_t at = xml.find("<Worksheet"); at != std::string::npos; at = xml.find("<Worksheet", at + 1)) ++opens;
  for (size_t at = xml.find("</Worksheet>"); at != std::string::npos; at = xml.find("</Worksheet>", at + 1)) ++closes;
  expect(opens == 5 && closes == 5, "xml: five sheets, each closed");
  const std::string pdf = resultsPdf(r);
  expect(pdf.rfind("%PDF-", 0) == 0 && pdf.find("%%EOF") != std::string::npos, "pdf: a whole document");
  expect(pdf.find("By marker area") != std::string::npos && pdf.find("Song two") != std::string::npos, "pdf: the area section names each area");
  expect(pdf.find("...") == std::string::npos, "pdf: nothing is cut off with an ellipsis");
  // An area with nothing in it and an unnamed one.
  Measurement e = m;
  Area a2;
  a2.a = 20, a2.b = 30, a2.bar0 = 11, a2.bar1 = 15;
  e.areas.push_back(a2);
  e.areas[0].name.clear();
  e.areas[0].beforeFirst = true;
  const Report re = build(e, tickAll(e), "t");
  expect(re.areas.size() == 3 && re.areas[2].songs.empty(), "areas: an area using nothing is listed empty");
  expect(areaName(re.areas[0].area, 0) == "Before the first marker" && areaName(re.areas[2].area, 2) == "Area 3", "areas: names of unnamed areas");
  expect(spreadsheetXml(re).find("No licensed sample is used here") != std::string::npos, "xml: an empty area says so");
  expect(resultsPdf(re).find("No licensed sample is used here") != std::string::npos, "pdf: an empty area says so");
}

// ---- measured: a project played through

constexpr int kRate = 44100;

std::string writeFile(const std::string& dir, const std::string& name, double seconds) {
  fs::create_directories(dir);
  const size_t n = static_cast<size_t>(seconds * kRate);
  AudioBuffer b(2, n);
  for (size_t i = 0; i < n; ++i) {
    const double t = static_cast<double>(i) / kRate;
    b.at(0, i) = b.at(1, i) = static_cast<float>(0.3 * std::sin(2 * 3.141592653589793 * (110.0 + 30.0 * t) * t));
  }
  const std::string path = (fs::path(dir) / name).string();
  writeWav(path, b, kRate, 16);
  return path;
}

Tick ticksPerSecond(const Song& s) { return static_cast<Tick>(s.beatTicks() * 2); }  // 120 bpm

ObjectId audioChannel(Song& s, const std::string& path, const std::string& name) {
  Channel ch;
  ch.id = s.allocateId();
  ch.name = name;
  ch.plugin = defaultPluginState("gl.sampler");
  ch.plugin.data = path;
  ch.mixerTrack = s.masterId();
  ch.audioClip = true;
  s.channels.push_back(ch);
  return ch.id;
}

void addClip(Song& s, ObjectId channel, double a, double b, double startSec, int track) {
  Clip k;
  k.id = s.allocateId(), k.kind = ClipKind::Audio, k.source = channel, k.track = track;
  k.start = static_cast<Tick>(startSec * static_cast<double>(ticksPerSecond(s)));
  k.length = static_cast<Tick>((b - a) * static_cast<double>(ticksPerSecond(s)));
  k.audioStart = a, k.audioEnd = b;
  s.playlist.push_back(k);
}

void testMeasured(const std::string& dir) {
  fs::remove_all(dir);
  const std::string vox = writeFile(dir, "Night Garden vocals 120bpm.wav", 10.0);
  const std::string drums = writeFile(dir, "Night Garden drums 120bpm.wav", 10.0);
  const std::string keys = writeFile(dir, "Paper Moon keys clearance required.wav", 10.0);
  Song s = makeEmptySong();
  s.tempo = 120.0;
  const ObjectId cv = audioChannel(s, vox, "Vocals"), cd = audioChannel(s, drums, "Drums"), ck = audioChannel(s, keys, "Keys");
  // Song one (0-8 s): vocals 1-3 s. Song two (8-16 s): the vocals' same 1-3 s again, drums 4-5 s.
  // Song three (16-24 s): keys 2-6 s, and the vocals 1-2 s.
  addClip(s, cv, 1.0, 3.0, 1.0, 0);
  addClip(s, cv, 1.0, 3.0, 9.0, 0);
  addClip(s, cd, 4.0, 5.0, 10.0, 1);
  addClip(s, ck, 2.0, 6.0, 17.0, 2);
  addClip(s, cv, 1.0, 2.0, 22.0, 0);
  const Tick tps = ticksPerSecond(s);
  s.markers = {marker(0, "Song one"), marker(8 * tps, "Song two"), marker(16 * tps, "Song three")};
  PassOptions o;
  o.sampleRate = kRate;
  const Measurement m = measure(s, o);
  expect(m.mode == "song" && m.areas.size() == 3, "measured: three marker areas");
  if (m.areas.size() == 3)
    expect(near(m.areas[1].a, 8.0, 0.001) && m.areas[2].b >= 22.99 && m.areas[2].b <= 24.01, "measured: areas in song seconds, the last to the song end");
  Saved sv;
  for (const FileIn& f : m.files) sv.files[f.path].ticked = 1;
  const Report r = build(m, sv, "t");
  double whole = -1;
  for (const SongRow& x : r.songs)
    if (x.key == "night garden") whole = x.seconds;
  expect(near(whole, 3.0, 0.06), "measured: the whole song uses 3.0 s of Night Garden (1-3 s and the drums' 4-5 s)");
  if (r.areas.size() != 3) return;
  const AreaSong* a0 = areaSong(r.areas[0], "night garden");
  const AreaSong* a1 = areaSong(r.areas[1], "night garden");
  const AreaSong* a2 = areaSong(r.areas[2], "night garden");
  const AreaSong* k2 = areaSong(r.areas[2], "paper moon");
  expect(a0 && near(a0->seconds, 2.0, 0.06) && !a0->rough, "measured: song one uses 2.0 s of Night Garden");
  expect(a1 && near(a1->seconds, 3.0, 0.06) && !a1->rough, "measured: song two uses 3.0 s (the vocals again, the drums)");
  expect(a2 && near(a2->seconds, 1.0, 0.06), "measured: song three uses 1.0 s of Night Garden");
  expect(k2 && near(k2->seconds, 4.0, 0.06) && near(k2->playSeconds, 4.0, 0.06), "measured: song three uses 4.0 s of the keys, sounding 4.0 s");
  expect(!areaSong(r.areas[0], "paper moon") && !areaSong(r.areas[1], "paper moon"), "measured: the keys are only in song three");
  if (a1) expect(near(a1->playSeconds, 2.0, 0.06), "measured: in song two Night Garden sounds 2.0 s (the drums under the vocals)");
  // The same project without markers: no areas, same figures.
  Song plain = s;
  plain.markers.clear();
  const Report rp = build(measure(plain, o), sv, "t");
  double plainWhole = -1;
  for (const SongRow& x : rp.songs)
    if (x.key == "night garden") plainWhole = x.seconds;
  expect(rp.areas.empty() && plainWhole == whole, "measured: without markers, no areas and the same totals");
  // The reference pass (every effect, one thread) places the reads in the same areas.
  PassOptions ref = o;
  ref.reference = true;
  const Report rr = build(measure(s, ref), sv, "t");
  bool same = rr.areas.size() == r.areas.size();
  for (size_t k = 0; same && k < r.areas.size(); ++k) {
    same = rr.areas[k].songs.size() == r.areas[k].songs.size();
    for (size_t i = 0; same && i < r.areas[k].songs.size(); ++i) same = near(rr.areas[k].songs[i].seconds, r.areas[k].songs[i].seconds, 0.06);
  }
  expect(same, "measured: the reference pass gives the same areas");
  fs::remove_all(dir);
}

// A project for the window's screenshots (GL_RECEIPT_AREAS_DEMO=<dir>): three songs split by
// markers that share samples, pack-style names of one song and of another, a store download.
void saveDemo(const std::string& dir) {
  const std::string v = writeFile(dir, "VOCALS_86BPM_GMinor_VBV3_bless_ya_life.wav", 12.0);
  const std::string d = writeFile(dir, "DRUMS_86BPM_GMinor_VBV3_bless_ya_life.wav", 12.0);
  const std::string k = writeFile(dir, "VOCALS_98BPM_DMajor_VBV3_mike_ransom_world_wide_master__98.00bpm_bm__clearance_required_.wav", 12.0);
  const std::string t = writeFile(dir, "Example Artist-Night Garden-Keys--96.00bpm-C-[Clearance Required].wav", 30.0);
  Song s = makeEmptySong();
  s.tempo = 120.0;
  const ObjectId cv = audioChannel(s, v, "Vocals"), cd = audioChannel(s, d, "Drums"), ck = audioChannel(s, k, "Hook"), ct = audioChannel(s, t, "Keys");
  addClip(s, cv, 0.0, 4.0, 0.0, 0);
  addClip(s, cd, 2.0, 6.0, 2.0, 1);
  addClip(s, cv, 0.0, 4.0, 16.0, 0);
  addClip(s, ck, 1.0, 9.0, 18.0, 2);
  addClip(s, ct, 10.0, 22.0, 32.0, 3);
  addClip(s, cd, 2.0, 4.0, 40.0, 1);
  const Tick tps = ticksPerSecond(s);
  s.markers = {marker(0, "First song"), marker(16 * tps, "Second song"), marker(32 * tps, "Third song: a much longer marker name to check the layout")};
  std::string err;
  saveProject(dir + "/Receipt areas demo.json", s, &err);
}

}  // namespace

int main() {
  if (const char* demo = std::getenv("GL_RECEIPT_AREAS_DEMO")) saveDemo(demo);
  const std::string scratch = (fs::temp_directory_path() / "gl_track_receipt_areas_test").generic_string();
  testHelper();
  testTimed();
  testRough();
  testNoAreas();
  testOutputs();
  testMeasured(scratch);
  return finish();
}
