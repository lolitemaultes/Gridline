// Gridline - K-321: the origin model of the Track Receipt. Origin sidecars version 2 (segments per
// source, loops, hashes) always carry the exact version 1 view Gridline 0.7.7 reads (checked with
// a frozen copy of 0.7.7's reader); version 1 sidecars read and trace as before; the trail is a
// DAG (several sources, a derived source, a loop of origins, more than 8 steps); the project's
// table by file hash (names and origins) takes over when the sidecars are gone and the files are
// moved or renamed to their hash; the saved receipt keeps its bytes; the hash cache.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>
#include "../../src/core/Json.h"
#include "../../src/core/PluginInfo.h"
#include "../../src/core/ReceiptHashes.h"
#include "../../src/core/ReceiptOrigin.h"
#include "../../src/core/Sha256.h"
#include "../../src/core/Song.h"
#include "../../src/core/TrackReceipt.h"
#include "../../src/core/WavWriter.h"
#include "../../src/engine/TrackReceiptPass.h"
#include "../Check.h"

using namespace gl;
using namespace gl::receipt;
namespace fs = std::filesystem;

namespace {

bool near(double a, double b, double tol = 1e-9) { return std::fabs(a - b) <= tol; }
bool sameRanges(const Ranges& a, const Ranges& b, double tol = 1e-9) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (!near(a[i].a, b[i].a, tol) || !near(a[i].b, b[i].b, tol)) return false;
  return true;
}
std::string slurp(const std::string& p) {
  std::ifstream f(p, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}
std::string tone(const std::string& dir, const std::string& name, double seconds, int seed) {
  fs::create_directories(dir);
  const auto n = static_cast<size_t>(seconds * 8000);
  AudioBuffer b(1, n);
  for (size_t i = 0; i < n; ++i) b.at(0, i) = static_cast<float>(0.3 * std::sin(0.01 * static_cast<double>(i) * (1 + seed)));
  const std::string path = (fs::path(dir) / name).generic_string();
  writeWav(path, b, 8000, 16);
  return path;
}

// ---- a frozen copy of Gridline 0.7.7's readOrigin (TrackReceipt.cpp): never change it.
bool readOrigin077(const std::string& file, Origin& o) {
  std::ifstream f(file + ".origin.json", std::ios::binary);
  if (!f) return false;
  std::stringstream ss;
  ss << f.rdbuf();
  Json j;
  if (!jsonParse(ss.str(), j) || j.type != Json::Type::Object || !j.find("gridlineOrigin")) return false;
  o = Origin{};
  o.source = j.text("source");
  o.start = j.num("start", 0);
  o.end = j.num("end", -1);
  o.speed = j.num("speed", 1);
  if (!(o.speed > 0)) o.speed = 1;
  if (const Json* v = j.find("reverse"); v && v->type == Json::Type::Bool) o.reverse = v->boolean;
  if (const Json* v = j.find("rangeKnown"); v && v->type == Json::Type::Bool) o.rangeKnown = v->boolean;
  o.kind = j.text("kind");
  if (const Json* a = j.find("sources"); a && a->type == Json::Type::Array)
    for (const auto& s : a->array)
      if (s.type == Json::Type::String) o.sources.push_back(s.str);
  auto beside = [&](std::string& p) {
    const std::filesystem::path f = std::filesystem::u8path(p);
    if (!p.empty() && !f.is_absolute()) p = (std::filesystem::u8path(file).parent_path() / f).lexically_normal().u8string();
  };
  beside(o.source);
  for (auto& x : o.sources) beside(x);
  return !o.source.empty();
}
bool sameV1(const Origin& a, const Origin& b) {
  return a.source == b.source && a.start == b.start && a.end == b.end && a.speed == b.speed && a.reverse == b.reverse &&
         a.rangeKnown == b.rangeKnown && a.kind == b.kind && a.sources == b.sources;
}

void testSidecars(const std::string& dir) {
  fs::create_directories(dir);
  const std::string x = dir + "/X.wav", y = dir + "/Y.wav";
  // A version 1 sidecar reads as before, into a version 1 origin without maps.
  {
    const std::string f = dir + "/v1.wav";
    Origin o;
    o.source = x, o.start = 2, o.end = 4, o.speed = 1, o.kind = "stem";
    writeOrigin(f, o);
    OriginV2 v;
    Origin old;
    expect(readOriginV2(f, v) && readOrigin077(f, old) && v.version == 1 && v.maps.empty() && sameV1(v.v1, old), "v1: reads into v2 unchanged");
    expect(!hasMaps(f, sidecarLookup()), "v1: no maps on the trail");
    const std::vector<Origin> a = originChain(f), b = chainOf(f, sidecarLookup());
    expect(a.size() == 1 && b.size() == 1 && sameV1(a[0], b[0]), "v1: the chain through a lookup is originChain's");
    // Written again as v2 without maps it is the same file a v1 writer writes.
    const std::string text = slurp(originPath(f));
    expect(writeOriginV2(f, v) && slurp(originPath(f)) == text, "v1: written back byte for byte");
  }
  // Version 2, round trip.
  OriginV2 two;
  two.version = 2;
  two.v1.kind = "render";
  two.length = 4;
  two.hash = std::string(64, 'a');
  two.complete = true;
  {
    SourceMap a;
    a.source = x, a.sourceHash = std::string(64, 'b'), a.sourceName = "Artist-Song-Drums--90bpm-Am-[Clearance Required].wav";
    a.sourceLength = 30;
    a.segs = {{0, 2, 10, 12}};
    a.pitch = {-2, 0}, a.speed = {1, 1}, a.chops = 1;
    SourceMap b;
    b.source = y;
    b.segs = {{2, 2.5, 5, 4.5}};
    b.rep = {{0, 1, 0.5, 3}};
    two.maps = {a, b};
    two.ops = {"gain", "eq"};
  }
  const std::string f2 = dir + "/two.wav";
  expect(writeOriginV2(f2, two), "v2: written");
  OriginV2 back;
  OriginV2 want = two;
  want.v1 = v1View(two);  // (read back with its v1 view filled in)
  expect(readOriginV2(f2, back) && back == want, "v2: reads back what was written");
  // ... and 0.7.7 reads its v1 view: several sources, range not known.
  Origin old;
  expect(readOrigin077(f2, old) && !old.rangeKnown && old.source == x && old.sources == std::vector<std::string>{x, y} && old.kind == "render",
         "v2: 0.7.7 reads several sources, range unknown");
  // One linear source: 0.7.7 reads its exact part (forward and backward).
  for (bool rev : {false, true}) {
    OriginV2 one;
    one.version = 2, one.v1.kind = "edit", one.length = 2;
    SourceMap m;
    m.source = x;
    m.segs = {rev ? Seg{0, 2, 7, 3} : Seg{0, 2, 3, 7}};
    one.maps = {m};
    const std::string f = dir + (rev ? "/rev.wav" : "/fwd.wav");
    writeOriginV2(f, one);
    Origin o;
    expect(readOrigin077(f, o) && o.rangeKnown && o.source == x && o.start == 3 && o.end == 7 && o.speed == 2 && o.reverse == rev,
           rev ? "v2: 0.7.7 reads a reversed linear part" : "v2: 0.7.7 reads a linear part at double speed");
    // ... the same keys and values a v1 writer would write for that view.
    Origin view = v1View(one);
    const std::string vf = dir + "/view.wav";
    writeOrigin(vf, view);
    Json a, b;
    jsonParse(slurp(originPath(vf)), a);
    jsonParse(slurp(originPath(f)), b);
    bool same = true;
    for (const auto& [k, v] : a.object)
      if (k != "gridlineOrigin") same = same && b.find(k) && jsonCompact(*b.find(k)) == jsonCompact(v);
    expect(same, "v2: its v1 keys are exactly what the v1 writer writes");
    // and both trace the same.
    Ranges r1{{0.5, 1.5}};
    std::vector<Origin> chain{o};
    mapThrough(chain, 2, r1);
    const auto t = trace(f, {{0.5, 1.5}}, 2, sidecarLookup());
    expect(t.size() == 1 && t.count(x) && sameRanges(t.at(x).ranges, r1), "v2: maps trace like the v1 view");
  }
  // A package names sources relative to the file.
  {
    Json j = originToJson(two);
    j.object["maps"].array[0].object["source"] = Json::makeString("sub/X.wav");
    OriginV2 o;
    expect(originFromJson(j, o, dir) && o.maps[0].source == (fs::path(dir) / "sub/X.wav").lexically_normal().u8string(),
           "v2: a relative map source is beside the file");
  }
}

void testLoops() {
  SourceMap m;
  for (int k = 0; k < 8; ++k) m.segs.push_back({2.0 * k, 2.0 * k + 2, 1, 3});
  m.segs.push_back({16, 17, 9, 10});
  const std::vector<Seg> all = m.segs;
  compactLoops(m);
  expect(m.segs.size() == 2 && m.rep.size() == 1 && m.rep[0].count == 7 && near(m.rep[0].period, 2), "loops: 8 passes of a 2 s loop are one segment and a repeat");
  std::vector<Seg> e = expand(m);
  auto key = [](const Seg& g) { return std::round(g[0] * 1e6); };
  std::sort(e.begin(), e.end(), [&](const Seg& a, const Seg& b) { return key(a) < key(b); });
  bool same = e.size() == all.size();
  for (size_t i = 0; same && i < e.size(); ++i)
    for (int k = 0; k < 4; ++k) same = same && near(e[i][static_cast<size_t>(k)], all[i][static_cast<size_t>(k)], 1e-9);
  expect(same, "loops: expanding gives the segments back");
  SourceMap two;  // chops of two segments repeating
  for (int k = 0; k < 4; ++k) two.segs.push_back({1.0 * k, 1.0 * k + 0.5, 0, 0.5}), two.segs.push_back({1.0 * k + 0.5, 1.0 * k + 1, 4, 3.5});
  compactLoops(two);
  expect(two.segs.size() == 2 && two.rep.size() == 1 && two.rep[0].count == 3 && expand(two).size() == 8, "loops: a two-segment pattern repeats too");
}

void testTrace(const std::string& dir) {
  fs::create_directories(dir);
  const std::string x = dir + "/X.wav", y = dir + "/Y.wav";
  // Two sources: [0,2) plays X 10..12, [2,4) plays Y 5..3 backwards.
  const std::string f = dir + "/mix.wav";
  OriginV2 o;
  o.version = 2, o.length = 4;
  SourceMap a, b;
  a.source = x, a.segs = {{0, 2, 10, 12}};
  b.source = y, b.segs = {{2, 4, 5, 3}};
  o.maps = {a, b};
  writeOriginV2(f, o);
  auto t = trace(f, {{1, 3}}, 4, sidecarLookup());
  expect(t.size() == 2 && sameRanges(t[x].ranges, {{11, 12}}) && sameRanges(t[y].ranges, {{4, 5}}), "trace: each source gets its own part");
  expect(hasMaps(f, sidecarLookup()), "trace: the trail has maps");
  // A derived source: X here is a v1 stem of the original O (1..3 s of it) whose... edit E.
  const std::string orig = dir + "/O.wav", stem = dir + "/stem.wav", edit = dir + "/edit.wav", dag = dir + "/dag.wav";
  {
    Origin s1;
    s1.source = orig, s1.start = 20, s1.end = 30, s1.kind = "stem";
    writeOrigin(stem, s1);
    Origin e1;
    e1.source = stem, e1.start = 2, e1.end = 6, e1.kind = "edit";
    writeOrigin(edit, e1);
    OriginV2 d;
    d.version = 2, d.length = 3;
    SourceMap m1, m2;
    m1.source = edit, m1.segs = {{0, 2, 0, 2}};  // edit 0..2 = stem 2..4 = original 22..24
    m2.source = y, m2.segs = {{2, 3, 0, 1}};
    d.maps = {m1, m2};
    writeOriginV2(dag, d);
  }
  t = trace(dag, {{0, 3}}, 3, sidecarLookup());
  expect(t.size() == 2 && t.count(orig) && sameRanges(t[orig].ranges, {{22, 24}}) && t[orig].depth == 3 && sameRanges(t[y].ranges, {{0, 1}}),
         "trace: through an edit of a stem to the original, and the second source");
  // A v1 render (range not known) on the way: its sources get the reads, marked unknown.
  {
    Origin r;
    r.source = x, r.sources = {x, y}, r.rangeKnown = false, r.kind = "render";
    writeOrigin(dir + "/render.wav", r);
    t = trace(dir + "/render.wav", {{0, 1}}, 1, sidecarLookup());
    expect(t.size() == 2 && t[x].unknown && t[y].unknown && sameRanges(t[x].ranges, {{0, 1}}), "trace: a v1 render: every source, range unknown");
  }
  // A loop of origins stops, with a warning.
  {
    const std::string p = dir + "/p.wav", q = dir + "/q.wav";
    OriginV2 op, oq;
    op.version = oq.version = 2;
    SourceMap mp, mq;
    mp.source = q, mp.segs = {{0, 1, 0, 1}};
    mq.source = p, mq.segs = {{0, 1, 0, 1}};
    op.maps = {mp}, oq.maps = {mq};
    writeOriginV2(p, op), writeOriginV2(q, oq);
    std::vector<std::string> w;
    t = trace(p, {{0, 1}}, 1, sidecarLookup(), &w);
    expect(t.size() == 1 && t.count(p) && w.size() == 1 && w[0].find("loop") != std::string::npos, "trace: a loop of origins stops, said once");
  }
  // Nine steps: it stops at 8, with a warning.
  {
    std::vector<std::string> files;
    for (int k = 0; k <= 9; ++k) files.push_back(dir + "/deep" + std::to_string(k) + ".wav");
    for (int k = 0; k < 9; ++k) {
      OriginV2 d;
      d.version = 2;
      SourceMap m;
      m.source = files[static_cast<size_t>(k + 1)], m.segs = {{0, 100, 1, 101}};  // each step: +1 s
      d.maps = {m};
      writeOriginV2(files[static_cast<size_t>(k)], d);
    }
    std::vector<std::string> w;
    t = trace(files[0], {{0, 1}}, 1, sidecarLookup(), &w);
    expect(t.size() == 1 && t.count(files[8]) && t[files[8]].depth == 8 && !w.empty() && w[0].find("8 steps") != std::string::npos,
           "trace: more than 8 steps stops at the 8th, said");
  }
  // An incomplete map: somewhere in what was read.
  {
    OriginV2 d;
    d.version = 2, d.complete = false;
    SourceMap m;
    m.source = x, m.ranges = {{3, 4}, {8, 9}};
    d.maps = {m};
    writeOriginV2(dir + "/inc.wav", d);
    t = trace(dir + "/inc.wav", {{0, 0.1}}, 5, sidecarLookup());
    expect(t.size() == 1 && sameRanges(t[x].ranges, {{3, 4}, {8, 9}}), "trace: an incomplete map gives the source ranges read");
  }
  // The same through the v1 chain functions: trace == mapThrough for v1 steps.
  {
    for (double end : {-1.0, 9.0})
      for (bool rev : {false, true}) {
        Origin s1;
        s1.source = orig, s1.start = 5, s1.end = end, s1.speed = 1.5, s1.reverse = rev;
        writeOrigin(dir + "/c1.wav", s1);
        Origin s2;
        s2.source = dir + "/c1.wav", s2.start = 0.5, s2.end = -1, s2.speed = 1;
        writeOrigin(dir + "/c2.wav", s2);
        Ranges r{{0.25, 1.0}, {1.5, 2.0}};
        mapThrough(originChain(dir + "/c2.wav"), 2.5, r);
        t = trace(dir + "/c2.wav", {{0.25, 1.0}, {1.5, 2.0}}, 2.5, sidecarLookup());
        expect(t.size() == 1 && sameRanges(t[orig].ranges, r, 0), "trace: v1 steps map exactly as mapThrough (end, reverse, speed)");
      }
  }
}

Song songWith(const std::vector<std::string>& files) {
  Song s = makeEmptySong();
  for (const std::string& f : files) {
    Channel ch;
    ch.id = s.allocateId();
    ch.plugin = defaultPluginState("gl.sampler");
    ch.plugin.data = f;
    ch.mixerTrack = s.masterId();
    s.channels.push_back(ch);
  }
  return s;
}
Use useOf(const std::string& file, double a, double b) {
  Use u;
  u.file = file, u.owner = 7, u.where = "Channel 1 (Sampler)", u.plays = 1, u.measured = true;
  u.ranges = {{a, b}};
  return u;
}
std::string songsText(const Report& r) {
  std::ostringstream o;
  for (const SongRow& s : r.songs) {
    o << s.key << " " << s.seconds << " " << s.tier << ":";
    for (const Timeline& t : s.timelines) o << " part " << t.part << " " << rangesText(t.used);
    o << "\n";
  }
  return o.str();
}

// A derived file of two Tracklib songs: each song gets its own part.
void testBuild(const std::string& dir) {
  const std::string a = tone(dir, "Ann-First Song-Drums--90.00bpm-Am-[Clearance Required].wav", 20, 1);
  const std::string b = tone(dir, "Bob-Second Song-Keys--90.00bpm-Am-[Clearance Required].wav", 20, 2);
  const std::string mix = tone(dir, "bounce.wav", 4, 3);
  OriginV2 o;
  o.version = 2, o.length = 4, o.v1.kind = "render";
  SourceMap ma, mb;
  ma.source = a, ma.segs = {{0, 2, 10, 12}};
  mb.source = b, mb.segs = {{2, 4, 5, 3}};
  o.maps = {ma, mb};
  writeOriginV2(mix, o);
  Measurement m = scan(songWith({mix}), true);
  expect(m.files.size() == 1 && m.files[0].derived && !m.files[0].trail.empty(), "build: the trail is read with the files");
  m.uses = {useOf(mix, 0, 4)};
  const Report r = build(m, {}, "p");
  const std::string t = songsText(r);
  std::printf("%s", t.c_str());
  bool first = false, second = false;
  for (const SongRow& s : r.songs) {
    if (s.key.find("First Song") != std::string::npos)
      first = near(s.seconds, 2.0, 0.05) && s.timelines.size() == 1 && sameRanges(s.timelines[0].used, {{10, 12}}, 1e-6);
    if (s.key.find("Second Song") != std::string::npos)
      second = near(s.seconds, 2.0, 0.05) && s.timelines.size() == 1 && sameRanges(s.timelines[0].used, {{3, 5}}, 1e-6);
  }
  expect(r.songs.size() == 2 && first && second, "build: a render of two songs gives each its own part (no range unknown)");
  expect(!r.files.empty() && r.files[0].ticked && !r.files[0].unknownRange, "build: ticked, range known");
}

// The project's table: sidecars deleted, files moved, the original renamed to its hash.
void testTable(const std::string& dir) {
  const std::string d1 = dir + "/one";
  const std::string orig = tone(d1, "Ann-First Song-Drums--90.00bpm-Am-[Clearance Required].wav", 20, 4);
  const std::string other = tone(d1, "Bob-Second Song-Keys--90.00bpm-Am-[Clearance Required].wav", 20, 5);
  const std::string stem = tone(d1, "my stem.wav", 6, 6), mix = tone(d1, "my mix.wav", 4, 7);
  Origin s1;
  s1.source = orig, s1.start = 4, s1.end = 10, s1.kind = "stem";
  writeOrigin(stem, s1);
  OriginV2 o;
  o.version = 2, o.length = 4;
  SourceMap ma, mb;
  ma.source = orig, ma.segs = {{0, 2, 1, 3}};
  mb.source = other, mb.segs = {{2, 4, 6, 8}};
  o.maps = {ma, mb};
  writeOriginV2(mix, o);
  HashCache cache("");
  Song song = songWith({orig, other, stem, mix});
  Measurement m = scan(song, true, &cache);
  expect(m.files.size() == 4 && m.files[0].hash == sha256File(orig), "table: the scan hashes the files");
  Saved saved;
  expect(rememberOrigins(saved, m) && saved.names.size() == 2 && saved.origins.size() == 2, "table: the scan's names and origins are kept");
  Saved again = saved;
  expect(!rememberOrigins(again, m) && again == saved, "table: nothing new, no change (no edit)");
  m.uses = {useOf(stem, 1, 2), useOf(mix, 0, 4)};
  const std::string before = songsText(build(m, saved, "p"));
  // Elsewhere: sidecars gone, files moved, the original named by its hash (a collab copy).
  const std::string d2 = dir + "/two";
  fs::create_directories(d2);
  const std::string h = sha256File(orig);
  const std::string orig2 = d2 + "/" + h + ".wav", other2 = d2 + "/b.wav", stem2 = d2 + "/my stem.wav", mix2 = d2 + "/my mix.wav";
  fs::copy_file(orig, orig2), fs::copy_file(other, other2), fs::copy_file(stem, stem2), fs::copy_file(mix, mix2);
  Song moved = songWith({orig2, other2, stem2, mix2});
  moved.trackReceipt = savedToJson(saved);
  Measurement m2 = scan(moved, true, &cache);
  expect(m2.files.size() == 4 && m2.files[2].derived && m2.files[3].derived && !m2.files[3].trail.empty(), "table: without sidecars the origins come from the project");
  m2.uses = {useOf(stem2, 1, 2), useOf(mix2, 0, 4)};
  const std::string after = songsText(build(m2, savedFromJson(moved.trackReceipt), "p"));
  std::printf("# before:\n%s# after:\n%s", before.c_str(), after.c_str());
  expect(!before.empty() && before == after, "table: the receipt is unchanged after deleting sidecars, moving and renaming to the hash");
  // The renamed file keeps its name, and says so.
  const Report rr = build(m2, savedFromJson(moved.trackReceipt), "p");
  bool named = false;
  for (const FileRow& f : rr.files)
    if (f.path == orig2) named = f.info.tracklib && f.info.song == parseName(orig).song && f.originNote.find("named from the project") != std::string::npos;
  expect(named, "table: a file renamed to its hash is named from the project");
  // Pruning: a file the project no longer uses leaves the table.
  Measurement m3 = scan(songWith({orig}), true, &cache);
  Saved pruned = saved;
  expect(rememberOrigins(pruned, m3) && pruned.names.size() == 1 && pruned.origins.empty(), "table: entries of files no longer used go on a scan");
}

void testSaved() {
  // A 0.7.7 receipt reads and writes back byte for byte.
  const std::string old077 =
      "{\"files\":{\"/a/X.wav\":{\"ticked\":1}},\"release\":{\"title\":\"T\"},\"songs\":{\"A-B\":{\"share\":\"5\"}},\"v\":1,"
      "\"writers\":[{\"name\":\"W\"}]}";
  expect(savedToJson(savedFromJson(old077)) == old077, "saved: a 0.7.7 receipt keeps its bytes");
  // The new keys round trip, and their bytes too.
  Saved s = savedFromJson(old077);
  s.names[std::string(64, 'c')] = "Ann-First Song-Drums--90.00bpm-Am-[Clearance Required].wav";
  s.origins[std::string(64, 'd')] = "{\"gridlineOrigin\":2,\"kind\":\"render\",\"maps\":[{\"segs\":[[0,1,2,3]],\"source\":\"sha256:" + std::string(64, 'c') + "\"}]}";
  s.heardFloor = "-60";
  s.snapshots = {"{\"receiptSnapshot\":1}"};
  s.songs["A-B"].basis = "heard";
  s.files["/a/X.wav"].seconds = "4.5";
  s.files["/a/X.wav"].uses["7:-1"] = "0:01-0:02";
  const std::string j = savedToJson(s);
  expect(savedFromJson(j) == s, "saved: names, origins, heard, snapshots, basis and entries read back");
  expect(savedToJson(savedFromJson(j)) == j, "saved: and write back byte for byte");
  expect(savedFromJson(j).unknown.empty(), "saved: none of them is an unknown key");
  // A newer version's keys inside them survive.
  const std::string newer = "{\"heard\":{\"floor\":\"-50\",\"twin\":true},\"names\":{\"x\":5},\"v\":1}";
  expect(savedToJson(savedFromJson(newer)) == newer, "saved: unknown keys beside the new ones survive");
  // fileFields: the path, then the hash key, then the file name.
  Saved f;
  f.files["sha256:" + std::string(64, 'e')].ticked = 1;
  f.files["/elsewhere/K.wav"].ticked = 0;
  expect(fileFields(f, "/here/K.wav", std::string(64, 'e')).ticked == 1, "fields: the hash key comes before the file name");
  expect(fileFields(f, "/here/K.wav").ticked == 0, "fields: without a hash, the file name");
}

void testHashes(const std::string& dir) {
  const std::string a = tone(dir, "a.wav", 1, 8);
  const std::string cacheFile = dir + "/cache/receipt-hashes.json";
  HashCache c(cacheFile);
  const std::string h = c.get(a);
  expect(h == sha256File(a) && h.size() == 64 && c.hashed() == 1, "hashes: a file's SHA-256");
  expect(c.get(a) == h && c.hashed() == 1, "hashes: the second time from the cache");
  expect(c.save() && fs::exists(cacheFile), "hashes: saved");
  HashCache d(cacheFile);
  expect(d.get(a) == h && d.hashed() == 0, "hashes: another run reads the cache, not the file");
  tone(dir, "a.wav", 2, 9);  // changed: a new length
  expect(d.get(a) == sha256File(a) && d.get(a) != h && d.hashed() == 1, "hashes: a changed file is hashed again");
  const std::string named = dir + "/" + std::string(64, 'A') + ".wav";
  expect(hashFromName(named) == std::string(64, 'a') && hashFromName(a).empty(), "hashes: a collab copy's name is its hash");
  expect(c.get(dir + "/missing.wav").empty(), "hashes: a missing file has none");
}

}  // namespace

int main() {
  const std::string dir = (fs::temp_directory_path() / "gl_track_receipt_origin").generic_string();
  fs::remove_all(dir);
  testSidecars(dir + "/sidecars");
  testLoops();
  testTrace(dir + "/trace");
  if (findPlugin("gl.sampler")) {
    testBuild(dir + "/build");
    testTable(dir + "/table");
  }
  testSaved();
  testHashes(dir + "/hashes");
  fs::remove_all(dir);
  return finish();
}
