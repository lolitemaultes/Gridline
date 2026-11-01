// Gridline - J-219: Track Receipt tests (names, ranges, tiers, report, XML, PDF,
// saved fields, and the coverage measured by playing projects through every sampler).
// Synthetic audio and made-up artist / title names only.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include "../../src/core/Json.h"
#include "../../src/core/PdfWriter.h"
#include "../../src/core/Project.h"
#include "../../src/core/TrackReceipt.h"
#include "../Check.h"
#include "track_receipt_projects.h"

using namespace gl;
using namespace gl::receipt;

namespace {

bool near(double a, double b, double tol = 0.011) { return std::fabs(a - b) <= tol; }
bool loadText(const std::string& t, Song& s, std::string* e) {
  Json j;
  return jsonParse(t, j) && songFromJson(j, s, e);
}

const char* kA = "Neon Harbor-Paper Lanterns-Drums_separated--130.00bpm-Am-[Clearance Required].wav";
const char* kB = "Neon Harbor-Paper Lanterns-Master--130.00bpm-Am-[Clearance Required].wav";
const char* kC = "Jean-Luc Ferro-World-Wide Sunday-Master--109.00bpm--[Clearance Required] - Part_2.wav";

void testNames() {
  NameInfo a = parseName(std::string("/x/y/") + kA);
  expect(a.tracklib, "names: tagged file is Tracklib");
  expect(a.song == "Neon Harbor-Paper Lanterns", "names: song identity before the stem");
  expect(a.artist == "Neon Harbor" && a.title == "Paper Lanterns", "names: artist / title split at the first hyphen");
  expect(a.stem == "Drums_separated", "names: stem");
  expect(near(a.bpm, 130) && a.key == "Am" && a.part == 0, "names: bpm, key, no part");
  NameInfo b = parseName(kB);
  expect(b.song == a.song && b.stem == "Master", "names: two stems of one song share the identity");
  NameInfo c = parseName(kC);
  expect(c.tracklib && c.part == 2 && c.key.empty(), "names: Part_N and empty key");
  expect(c.song == "Jean-Luc Ferro-World-Wide Sunday" && c.stem == "Master", "names: hyphens inside artist and title");
  expect(c.artist == "Jean" && c.title == "Luc Ferro-World-Wide Sunday", "names: the artist split is a best guess (first hyphen)");
  expect(c.ambiguous && !a.ambiguous, "names: more than one hyphen marks the split as a guess");
  const NameInfo c2 = resplit(c, {"jean-luc ferro"});
  expect(c2.artist == "Jean-Luc Ferro" && c2.title == "World-Wide Sunday" && c2.artistKnown, "names: a known artist settles the split");
  expect(resplit(c, {"someone else"}).artist == "Jean", "names: no known artist keeps the first hyphen");
  {
    const char* loopName = "Neo Act-Some Tune-Master--40.60-45.40--100.10bpm-Gm-[Clearance Required].wav";
    const NameInfo l = parseName(loopName);
    expect(l.tracklib && l.song == "Neo Act-Some Tune" && l.stem == "Master" && near(l.bpm, 100.1) && l.key == "Gm",
           "names: a store loop keeps song, stem, bpm and key");
    expect(near(l.loopStart, 40.6) && near(l.loopEnd, 45.4), "names: a store loop names its part of the original");
    Measurement m;
    m.files.push_back(FileIn{std::string("/x/") + loopName, 4.8, true, false, {}});
    m.files.push_back(FileIn{"/x/Neo Act-Some Tune-Drums--100.10bpm-Gm-[Clearance Required].wav", 180, true, false, {}});
    Use u;
    u.file = m.files[0].path, u.where = "loop", u.ranges = {{0, 2}};
    m.uses.push_back(u);
    u.file = m.files[1].path, u.where = "drums", u.ranges = {{41.6, 43.6}};
    m.uses.push_back(u);
    const Report r = build(m, {}, "t");
    expect(r.songs.size() == 1 && near(r.songs[0].seconds, 3.0) && near(r.songs[0].timelines[0].used[0].a, 40.6),
           "report: a store loop maps onto the original (40.6-43.6 = 3.0 s with the drums)");
    m.uses[1].ranges = {{0, 0.004}, {41.6, 43.6}};  // a voice's block at 0 before it jumps
    expect(near(build(m, {}, "t").songs[0].seconds, 3.0), "report: reads shorter than 10 ms are not counted");
  }
  NameInfo d = parseName("/lib/Kick 01.wav");
  expect(!d.tracklib && d.song == "Kick 01" && d.stem.empty(), "names: other files are not Tracklib");
  NameInfo e = parseName("Some-Thing-Vocals--90bpm-C-[Clearance Required] - Part_1.flac");
  expect(e.tracklib && e.part == 1 && near(e.bpm, 90) && e.stem == "Vocals" && e.key == "C", "names: integer bpm, Part_1");
}

void testRanges() {
  Ranges r = unite({{4, 6}, {0, 2}, {1, 3}, {6, 6.0002}});
  expect(r.size() == 2 && near(r[0].a, 0) && near(r[0].b, 3) && near(r[1].b, 6.0002, 1e-6), "ranges: union sorts and merges");
  expect(near(total(r), 5.0002, 1e-6), "ranges: total");
  expect(timeText(65.34) == "1:05.3" && timeText(0) == "0:00.0", "ranges: m:ss.s");
  Ranges p;
  expect(parseRanges("0:01.0-0:03.0, 10-12.5", p) && p.size() == 2 && near(total(p), 4.5), "ranges: parse typed ranges");
  expect(!parseRanges("5-2", p), "ranges: an end before the start is an error");
  expect(tierFor(2.0) == 2 && tierFor(2.04) == 2 && tierFor(2.06) == 15 && tierFor(15) == 15 && tierFor(15.1) == 60 &&
             tierFor(60) == 60 && tierFor(60.1) == 0,
         "tiers: 2 / 15 / 60, over 60 = none");
  expect(parseShare("10") == 10 && parseShare(" 12.5 %") == 12.5 && parseShare("") < 0 && parseShare("x") < 0, "shares: parse");
  auto ds = reportDates("2026-08-31", 3);
  expect(ds.size() == 3 && ds[0] == "2027-02-28" && ds[1] == "2027-08-31" && ds[2] == "2028-02-29", "release: report dates every 6 months");
}

Use use(const std::string& file, const std::string& where, Ranges r, int64_t plays = 1) {
  Use u;
  u.file = file;
  u.where = where;
  u.ranges = std::move(r);
  u.plays = plays;
  u.bar0 = 1, u.bar1 = 8;
  return u;
}

Measurement sampleMeasurement() {
  Measurement m;
  m.mode = "song";
  m.files = {{std::string("/s/") + kA, 180, true, false, {}}, {std::string("/s/") + kB, 180, true, false, {}},
             {std::string("/s/") + kC, 95, true, false, {}}, {"/s/Royalty free kick.wav", 0.4, true, false, {}},
             {"/s/Long Song-Loop-Master--100.00bpm-F-[Clearance Required].wav", 240, true, false, {}}};
  m.uses.push_back(use(m.files[0].path, "Playlist track 2, audio clip", {{10, 12}}, 64));
  m.uses.push_back(use(m.files[1].path, "Channel 3 'Chops' (Sampler)", {{11, 13}, {40, 44}}, 12));
  m.uses.push_back(use(m.files[2].path, "Channel 4 'Keys' (Seltmann)", {{1, 1.5}, {2, 2.25}}, 30));
  m.uses.push_back(use(m.files[3].path, "Channel 1 'Kick' (Sampler)", {{0, 0.4}}, 200));
  m.uses.push_back(use(m.files[4].path, "Playlist track 5, audio clip", {{0, 75}}, 1));
  Use muted = use(m.files[1].path, "Playlist track 9, audio clip", {{100, 110}});
  muted.muted = true;
  muted.note = "muted clip";
  m.uses.push_back(muted);
  return m;
}

void testReport() {
  Saved sv;
  sv.songs["Neon Harbor-Paper Lanterns"].share = "10";
  sv.songs["Neon Harbor-Paper Lanterns"].category = "C";
  sv.songs["Jean-Luc Ferro-World-Wide Sunday"].share = "2";
  sv.songs["Jean-Luc Ferro-World-Wide Sunday"].artist = "Jean-Luc Ferro";
  sv.songs["Jean-Luc Ferro-World-Wide Sunday"].title = "World-Wide Sunday";
  sv.release.date = "2026-11-20";
  sv.release.title = "Night Drive";
  Report r = build(sampleMeasurement(), sv, "Night Drive");
  const SongRow* a = nullptr;
  const SongRow* c = nullptr;
  const SongRow* l = nullptr;
  for (const auto& s : r.songs) {
    if (s.key == "Neon Harbor-Paper Lanterns") a = &s;
    if (s.key == "Jean-Luc Ferro-World-Wide Sunday") c = &s;
    if (s.title == "Loop") l = &s;
  }
  expect(r.songs.size() == 3, "report: three ticked songs (the royalty-free file is not ticked)");
  // Drums 10-12 and Master 11-13 overlap on one timeline -> 10-13 (3 s) + 40-44 (4 s) = 7 s.
  expect(a && near(a->seconds, 7.0) && a->tier == 15, "report: two stems of one song: overlap counts once, sections add up");
  expect(a && a->timelines.size() == 1 && a->timelines[0].parts.size() == 2 && a->timelines[0].parts[0].from.size() == 2,
         "report: each range lists the uses it came from");
  expect(c && near(c->seconds, 0.8) && c->tier == 2 && c->artist == "Jean-Luc Ferro", "report: chopped parts add up, typed artist wins");
  expect(l && l->over && near(l->seconds, 75), "report: over 60 s flagged");
  expect(near(r.totalShare, 12) && !r.sharesComplete, "report: shares add up (one song still blank)");
  bool mutedListed = false;
  for (const auto& f : r.files)
    if (f.path == sampleMeasurement().files[1].path) mutedListed = f.muted.size() == 1;
  expect(mutedListed, "report: muted uses listed, not counted");
  // Ticking the royalty-free file makes it its own song; unticking a Tracklib file drops it.
  Saved t = sv;
  t.files["/s/Royalty free kick.wav"].ticked = 1;
  t.files[std::string("/s/") + kC].ticked = 0;
  Report r2 = build(sampleMeasurement(), t, "Night Drive");
  bool kick = false, ferro = false;
  for (const auto& s : r2.songs) kick = kick || s.title == "Royalty free kick", ferro = ferro || s.key == "Jean-Luc Ferro-World-Wide Sunday";
  expect(kick && !ferro, "report: ticks override the automatic choice");
  // Linking a file to a song
  Saved k = sv;
  k.files["/s/Royalty free kick.wav"] = FileFields{1, "Neon Harbor-Paper Lanterns", ""};
  Report r3 = build(sampleMeasurement(), k, "x");
  for (const auto& s : r3.songs)
    if (s.key == "Neon Harbor-Paper Lanterns") expect(near(s.seconds, 7.4), "report: a linked file joins the song's timeline");
}

void testEstimated() {
  Measurement m;
  m.files = {{std::string("/s/") + kA, 30, true, false, {}}};
  Use u = use(m.files[0].path, "Channel 2 'Lead' (Serum 2, third-party)", {{0, 30}});
  u.estimated = true;
  m.uses.push_back(u);
  Report r = build(m, Saved{}, "p");
  expect(r.songs.size() == 1 && near(r.songs[0].seconds, 30) && r.songs[0].timelines[0].parts[0].estimated,
         "estimated: third-party use counts the whole file, marked");
  Saved s;
  s.files[m.files[0].path].ranges = "0:02-0:04";
  Report r2 = build(m, s, "p");
  expect(r2.songs.size() == 1 && near(r2.songs[0].seconds, 2) && !r2.songs[0].timelines[0].parts[0].estimated,
         "estimated: the typed ranges replace the estimate");
}

void testDerived(const std::string& dir) {
  namespace fs = std::filesystem;
  fs::create_directories(dir);
  const std::string orig = dir + "/" + kB;
  const std::string edit = dir + "/edit.wav";
  const std::string slice = dir + "/edit_slice.wav";
  Origin o1;
  o1.source = orig, o1.start = 20, o1.end = 30, o1.kind = "edit";
  expect(writeOrigin(edit, o1), "origin: written");
  Origin o2;
  o2.source = edit, o2.start = 2, o2.end = 4, o2.reverse = true, o2.kind = "slice";
  writeOrigin(slice, o2);
  auto chain = originChain(slice);
  expect(chain.size() == 2 && chain.back().source == orig, "origin: chains resolve to the original");
  Ranges r{{0, 1}};
  // slice 0..1 reversed -> edit 3..4 -> original 23..24
  expect(mapThrough(chain, 2, r) && r.size() == 1 && near(r[0].a, 23) && near(r[0].b, 24), "origin: ranges map back through the chain");
  Measurement m;
  m.files = {{slice, 2, true, true, chain}, {orig, 180, true, false, {}}};
  m.uses.push_back(use(slice, "Channel 1 (Sampler)", {{0, 1}}));
  m.uses.push_back(use(orig, "Playlist track 1, audio clip", {{23.5, 25}}));
  Report rep = build(m, Saved{}, "p");
  expect(rep.songs.size() == 1 && near(rep.songs[0].seconds, 2.0), "origin: a derived file maps onto the original's timeline (union)");
  Origin o3;
  o3.source = orig, o3.rangeKnown = false, o3.kind = "render";
  writeOrigin(dir + "/render.wav", o3);
  Measurement m2;
  m2.files = {{dir + "/render.wav", 5, true, true, originChain(dir + "/render.wav")}};
  m2.uses.push_back(use(dir + "/render.wav", "Playlist track 3, audio clip", {{0, 5}}));
  Report rep2 = build(m2, Saved{}, "p");
  expect(rep2.songs.size() == 1 && rep2.songs[0].timelines.size() == 1 && rep2.songs[0].timelines[0].part == -1 && near(rep2.songs[0].seconds, 5),
         "origin: range unknown counts on its own (safe side)");
}

void testSaved() {
  Saved s;
  s.songs["A-B"] = SongFields{"A", "B", "C", "10", "$100", "", "Owner Ltd"};
  s.files["/p/x.wav"] = FileFields{0, "A-B", "0:01-0:02"};
  s.release = Release{"T", "P", "L", "ISRC1", "2026-01-02", "D"};
  s.writers.push_back(Writer{"Me", "writer", "PRO", "123", "100"});
  const std::string j = savedToJson(s);
  expect(savedFromJson(j) == s, "saved: JSON round trip");
  expect(savedToJson(Saved{}).empty(), "saved: empty saves nothing");
  expect(fileFields(s, "/other/folder/x.wav").ticked == 0, "saved: matched by file name after a move");
  // Project round trip.
  Song song = makeEmptySong();
  song.trackReceipt = j;
  const std::string text = projectToText(song);
  Song back;
  std::string err;
  expect(loadText(text, back, &err) && back.trackReceipt == j, "saved: project round trip keeps the fields");
  Song old = makeEmptySong();
  Song oldBack;
  expect(loadText(projectToText(old), oldBack, &err) && oldBack.trackReceipt.empty(), "saved: projects without it load");
}

// Minimal XML well-formedness check: tags balance, entities valid.
bool xmlBalanced(const std::string& x, std::string& why) {
  std::vector<std::string> st;
  for (size_t i = 0; i < x.size(); ++i) {
    if (x[i] == '&') {
      const size_t e = x.find(';', i);
      const std::string ent = x.substr(i, e - i + 1);
      if (e == std::string::npos || (ent != "&amp;" && ent != "&lt;" && ent != "&gt;" && ent != "&quot;" && ent != "&#10;")) {
        why = "entity " + ent;
        return false;
      }
    }
    if (x[i] != '<') continue;
    const size_t e = x.find('>', i);
    if (e == std::string::npos) return false;
    std::string tag = x.substr(i + 1, e - i - 1);
    i = e;
    if (tag.empty() || tag[0] == '?' || tag[0] == '!') continue;
    if (tag.back() == '/') continue;
    if (tag[0] == '/') {
      const std::string name = tag.substr(1);
      if (st.empty() || st.back() != name) {
        why = "close " + name;
        return false;
      }
      st.pop_back();
    } else {
      st.push_back(tag.substr(0, tag.find_first_of(" \t\n")));
    }
  }
  if (!st.empty()) why = "open " + st.back();
  return st.empty();
}

bool pdfValid(const std::string& p, std::string& why) {
  if (p.rfind("%PDF-1.4", 0) != 0) return why = "header", false;
  const size_t sx = p.rfind("startxref");
  if (sx == std::string::npos) return why = "startxref", false;
  const size_t xref = std::strtoull(p.c_str() + sx + 10, nullptr, 10);
  if (p.compare(xref, 4, "xref") != 0) return why = "xref offset", false;
  std::istringstream in(p.substr(xref));
  std::string w;
  size_t first = 0, count = 0;
  in >> w >> first >> count;
  for (size_t i = 0; i < count; ++i) {
    size_t off = 0, gen = 0;
    std::string flag;
    in >> off >> gen >> flag;
    if (i == 0) continue;
    const std::string want = std::to_string(i) + " 0 obj";
    if (p.compare(off, want.size(), want) != 0) return why = "object " + std::to_string(i) + " offset", false;
  }
  // stream lengths
  for (size_t at = p.find("/Length "); at != std::string::npos; at = p.find("/Length ", at + 1)) {
    const size_t len = std::strtoull(p.c_str() + at + 8, nullptr, 10);
    const size_t s = p.find("stream\n", at) + 7;
    if (p.compare(s + len, 9, "endstream") != 0) return why = "stream length", false;
  }
  return true;
}

void testOutputs(const std::string& outDir) {
  Saved sv;
  sv.songs["Neon Harbor-Paper Lanterns"].share = "10";
  sv.songs["Neon Harbor-Paper Lanterns"].category = "C";
  sv.songs["Neon Harbor-Paper Lanterns"].holders = "Harbor Records (master), Lantern Songs (publishing)";
  sv.songs["Jean-Luc Ferro-World-Wide Sunday"].share = "2";
  sv.songs["Long Song-Loop"].share = "5";
  sv.release = Release{"Night Drive", "The Example Band", "Example Music Ltd", "XX-A00-26-00001", "2026-11-20", "Example Distribution"};
  sv.writers = {Writer{"Alex Example", "Writer, producer", "PRS", "00000000000", "60"}, Writer{"Sam Example", "Writer", "ASCAP", "", "40"}};
  Report r = build(sampleMeasurement(), sv, "Night Drive <&> \"test\"");
  const std::string xml = spreadsheetXml(r);
  std::string why;
  expect(xml.find("<?mso-application progid=\"Excel.Sheet\"?>") != std::string::npos, "xml: SpreadsheetML processing instruction");
  expect(xmlBalanced(xml, why), ("xml: well-formed " + why).c_str());
  expect(xml.find("Worksheet ss:Name=\"Licenses\"") != std::string::npos && xml.find("ss:Name=\"Usage\"") != std::string::npos &&
             xml.find("ss:Name=\"Release\"") != std::string::npos && xml.find("ss:Name=\"Splits\"") != std::string::npos,
         "xml: four sheets");
  expect(xml.find("ss:Formula=\"=100-(") != std::string::npos, "xml: splits use formulas");
  expect(xml.find("0:10.0-0:13.0 (3.0 s)") != std::string::npos, "xml: per-range timestamps");
  const std::string pdfText = resultsPdf(r);
  expect(pdfValid(pdfText, why), ("pdf: valid header, xref offsets, stream lengths " + why).c_str());
  expect(pdfText.find("...") == std::string::npos, "pdf: nothing is cut off with an ellipsis");
  expect(pdfText.find("Jean-Luc Ferro-World-Wide Sunday-Master") != std::string::npos ||
             pdfText.find("Jean-Luc Ferro-World-Wide Sunday-") != std::string::npos,
         "pdf: the original file name is shown in full");
  for (const SongRow& s : r.songs)
    if (s.key == "Jean-Luc Ferro-World-Wide Sunday") expect(s.checkName, "report: an ambiguous split is marked to check");
  expect(pdf::Doc::wrap("Aaaa-Bbbb-Cccc-Dddd-Eeee-Ffff", 10, false, 40).size() > 1, "pdf: long words wrap");
  pdf::Doc d;
  d.newPage();
  d.text(10, 10, "a(b)c\\", 10);
  expect(d.finish().find("(a\\(b\\)c\\\\)") != std::string::npos, "pdf: text escaped");
  expect(pdf::toWinAnsi("caf\xC3\xA9 \xE2\x80\x94") == "caf\xE9 -", "pdf: UTF-8 to WinAnsi");
  if (!outDir.empty()) {
    std::filesystem::create_directories(outDir);
    std::ofstream(outDir + "/sample_trackreceipt.xml", std::ios::binary) << xml;
    std::ofstream(outDir + "/sample_trackreceipt.pdf", std::ios::binary) << pdfText;
  }
}

}  // namespace

// Paths inside plugin data: Windows drive roots with either slash, not a "file:" prefix.
void testChunkPaths() {
  auto has = [](const std::vector<std::string>& v, const std::string& x) { return std::find(v.begin(), v.end(), x) != v.end(); };
  expect(has(receipt::chunkFileNames("{\"s\":\"C:/Users/me/My Stems/Stem.wav\"}"), "C:/Users/me/My Stems/Stem.wav"), "chunk paths: a drive root with forward slashes");
  expect(has(receipt::chunkFileNames("{\"s\":\"D:\\x\\Stem.wav\"}"), "D:\\x\\Stem.wav"), "chunk paths: a drive root with backslashes");
  expect(has(receipt::chunkFileNames("table=file:/Users/me/Loop.wav;"), "/Users/me/Loop.wav"), "chunk paths: a file: prefix is not a drive");
}

int main() {
  const char* tmp = std::getenv("TMPDIR");
  const std::string scratch = std::string(tmp ? tmp : "/tmp") + "/gl_track_receipt_test";
  testNames();
  testChunkPaths();
  testRanges();
  testReport();
  testEstimated();
  testDerived(scratch + "/derived");
  testSaved();
  const char* draft = std::getenv("GL_RECEIPT_DRAFT");
  testOutputs(draft ? draft : "");
  runProjectTests(scratch + "/projects");
  return finish();
}
