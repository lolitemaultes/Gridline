// Gridline — J-52: project files. File > Open routing (Gridline projects, FL
// Studio projects and zipped projects open as projects; MIDI and audio are
// imported), the open / save dialog types, and the FL Studio zipped loop
// package: archive layout, relative sample paths inside the .flp, and the
// round trip (package → unpack → open) against a plain FLP export. The FL
// demo-project checks run when FL Studio is installed and are skipped otherwise.
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "Check.h"
#include "Hermetic.h"
#include "core/AudioBuffer.h"
#include "core/BrowserIndex.h"
#include "core/Demo.h"
#include "core/PluginInfo.h"
#include "core/Project.h"
#include "core/ProjectFiles.h"
#include "core/ProjectInterop.h"
#include "core/WavWriter.h"
#include "core/Zip.h"
#include "core/flp/Flp.h"
#include "core/flp/FlpExport.h"
#include "core/flp/FlpPackage.h"

using namespace gl;
namespace fs = std::filesystem;

namespace {

std::string scratch;

void expect(bool ok, const std::string& name) { expect(ok, name.c_str()); }

std::string writeTone(const std::string& path, int frames, float level) {
  fs::create_directories(fs::path(path).parent_path());
  AudioBuffer b(2, static_cast<size_t>(frames));
  for (int i = 0; i < frames; ++i) b.at(0, static_cast<size_t>(i)) = b.at(1, static_cast<size_t>(i)) = level * ((i / 50) % 2 ? 1.0f : -1.0f);
  writeWav(path, b, 44100, 16);
  return path;
}

bool has(const std::vector<std::string>& v, const std::string& x) { return std::find(v.begin(), v.end(), x) != v.end(); }

// ---------------------------------------------------------------- routing

void testRouting() {
  expect(openKindOf("/a/Song.gridline.json") == OpenKind::GridlineProject, "open: gridline project");
  expect(openKindOf("/a/Song.FLP") == OpenKind::FlProject, "open: flp (any case)");
  expect(openKindOf("/a/Pack.zip") == OpenKind::ZippedProject, "open: zip");
  expect(openKindOf("/a/x.mid") == OpenKind::Midi && openKindOf("/a/x.wav") == OpenKind::Audio, "open: midi / audio import");
  expect(opensAsProject("/a/Song.gridline.json") && opensAsProject("/a/Song.flp") && opensAsProject("/a/Pack.zip"),
         "projects, FL projects and zips open as projects");
  expect(!opensAsProject("/a/x.mid") && !opensAsProject("/a/x.wav"), "MIDI / audio are imports, not projects");

  // File > Open: all projects first, then one type per format.
  const auto& open = openFileTypes();
  expect(open.size() == 4, "open dialog: 4 types");
  expect(has(open[0].exts, ".json") && has(open[0].exts, ".flp") && has(open[0].exts, ".zip"), "open: all projects lists json, flp, zip");
  expect(open[2].exts == std::vector<std::string>{".flp"} && open[3].exts == std::vector<std::string>{".zip"}, "open: per-type filters");
  // File > Import no longer offers projects.
  for (const auto* t : {&importMidiTypes(), &importAudioTypes()})
    for (const auto& ft : *t) expect(!has(ft.exts, ".flp") && !has(ft.exts, ".zip") && !has(ft.exts, ".json"), "import types hold no projects");
  // Browser / drag and drop: FL projects and zips open.
  using namespace browse;
  expect(kindOf("/a/Pack.zip") == Kind::Project, "browser: zip is a project");
  expect(dropAction(DropItem{Kind::Flp, "", PluginKind::Generator}, DropTarget::Workspace) == DropAction::OpenProject, "drop flp opens it");
  expect(dropAction(DropItem{Kind::Project, "", PluginKind::Generator}, DropTarget::Rack) == DropAction::OpenProject, "drop zip/project opens it");
  expect(std::string(dropActionLabel(DropAction::OpenProject)) == "Open project", "drop label");

  // Save as: Gridline, Gridline zipped (J-78), FL Studio project, FL Studio zipped loop package.
  const auto& save = saveFileTypes();
  expect(save.size() == 4 && save[1].exts[0] == ".gridline.zip" && save[2].exts[0] == ".flp" && save[3].exts[0] == ".zip", "save types");
  expect(saveFormatOf("/a/x.flp") == SaveFormat::Flp && saveFormatOf("/a/x.zip") == SaveFormat::FlZip &&
             saveFormatOf("/a/x.gridline.json") == SaveFormat::Gridline,
         "save format by name");
  expect(withSaveExtension("Song.flp", SaveFormat::Gridline) == "Song.gridline.json", "flp -> gridline name (never the .flp)");
  expect(withSaveExtension("Song.gridline.json", SaveFormat::Flp) == "Song.flp", "gridline -> flp name");
  expect(withSaveExtension("/d/Song.zip", SaveFormat::Flp) == "/d/Song.flp", "zip -> flp name");
  expect(withSaveExtension("My.Song", SaveFormat::FlZip) == "My.Song.zip", "dots kept");
  expect(withSaveExtension("Song", SaveFormat::Gridline) == "Song.gridline.json", "no extension");
  expect(projectStemOf("/d/Song.gridline.json") == "Song" && projectStemOf("/d/Song.flp") == "Song" && projectStemOf("Pack.zip") == "Pack",
         "project stems");
}

// ------------------------------------------------------------- packaging

// Paths normalised (the same file compares equal however it was spelled).
std::string canon(const std::string& p) {
  std::error_code ec;
  const fs::path c = fs::weakly_canonical(p, ec);
  return (ec ? fs::path(p).lexically_normal() : c).generic_string();
}
void normalisePaths(Song& s) {
  forEachFileRef(s, [](std::string& p) {
    if (!p.empty() && p[0] == '/') p = canon(p);
  });
}

// Package `song` → unpack → import; compared with a plain FLP export of the
// same song re-imported. Returns the number of differences.
size_t packageRoundTrip(const Song& song, const std::string& tag, const flp::PackageOptions& opt, flp::PackageReport& rep, bool* allResolve) {
  const std::string dir = scratch + "/" + tag;
  fs::create_directories(dir);
  const std::string zip = dir + "/" + tag + ".zip";
  std::string err;
  if (!flp::exportFlpZip(song, zip, tag, opt, rep, &err)) {
    expect(false, tag + ": package written (" + err + ")");
    return 9999;
  }
  // Layout: <tag>.flp first, every file beside it (no folders), unique names.
  std::vector<ZipEntry> entries;
  expect(readZipDirectory(zip, entries), tag + ": zip readable");
  expect(!entries.empty() && entries[0].name == tag + ".flp", tag + ": the project is the first entry, at the root");
  std::set<std::string> names;
  for (const auto& e : entries) {
    expect(e.name.find('/') == std::string::npos, tag + ": flat layout (" + e.name + ")");
    names.insert(e.name);
  }
  expect(names.size() == entries.size() && entries.size() == rep.files.size() + 1, tag + ": every file once");
  // Sample paths inside the .flp: relative names of archive entries.
  std::vector<uint8_t> bytes;
  readZipEntry(zip, entries[0], bytes);
  flp::File f;
  expect(flp::readBytes(bytes, f, nullptr), tag + ": packed flp parses");
  int rel = 0, abs = 0;
  for (const auto& e : f.events)
    if (e.id == 196) {
      const std::string t = e.text();
      if (names.count(t)) ++rel;
      else if (!t.empty() && (t[0] == '/' || t.find(':') != std::string::npos) && t.rfind("%FLStudio", 0) != 0) ++abs;
    }
  expect(rel == rep.rewritten, tag + ": rewritten paths are archive names");
  if (opt.includeFactorySamples) expect(abs == 0, tag + ": no absolute sample paths left");

  // Unpack and open (what File > Open does with a zip).
  std::string proj;
  const std::string unpack = dir + "/" + tag;
  expect(unpackZippedProject(zip, unpack, proj, &err) && fs::path(proj).filename() == tag + ".flp", tag + ": unpacks to its .flp");
  Song opened;
  flp::Report r1;
  expect(importFlp(proj, opened, r1, &err, opt.flp.factoryData), tag + ": packed project opens");
  // Every packed file resolves (references that never named a file, e.g. a
  // plugin's "none", stay as they were).
  if (allResolve) {
    *allResolve = true;
    for (const auto& m : missingFiles(opened))
      if (fs::path(m).is_absolute()) *allResolve = false, std::printf("  %s: missing after unpacking: %s\n", tag.c_str(), m.c_str());
  }
  // The same song through a plain FLP export.
  flp::Bytes plain;
  flp::ExportReport prep;
  flp::exportFlp(song, plain, prep, opt.flp);
  Song direct;
  flp::Report r2;
  importFlpBytes(plain, direct, r2, &err, opt.flp.factoryData, opt.flp.projectDir);
  // Files that came from the archive point back at their sources.
  std::map<std::string, std::string> back;
  for (const auto& [src, entry] : rep.files) back[canon(unpack + "/" + entry)] = canon(src);
  normalisePaths(opened);
  normalisePaths(direct);
  relinkFiles(opened, back);
  const auto diffs = flp::compareSongs(direct, opened);
  for (size_t i = 0; i < diffs.size() && i < 8; ++i) std::printf("  %s: %s\n", tag.c_str(), diffs[i].c_str());
  expect(r1.counts == r2.counts, tag + ": same import counts as the plain export");
  return diffs.size();
}

void testPackage() {
  // The demo song plus samples from two folders, two of them named alike.
  Song s = makeDemoSong();
  const std::string a = writeTone(scratch + "/src/a/kick.wav", 4000, 0.5f);
  const std::string b = writeTone(scratch + "/src/b/kick.wav", 3000, 0.25f);
  const std::string c = writeTone(scratch + "/src/b/Loop One.wav", 8000, 0.3f);
  for (const auto& [name, path] : std::vector<std::pair<std::string, std::string>>{{"Kick A", a}, {"Kick B", b}, {"Loop", c}}) {
    Channel ch;
    ch.id = s.allocateId();
    ch.name = name;
    ch.plugin = defaultPluginState("gl.sampler");
    ch.plugin.data = path;
    ch.mixerTrack = s.masterId();
    s.channels.push_back(ch);
    if (!s.patterns.empty()) s.patterns[0].notes[ch.id].push_back(Note(0, 24, 60, 100));
  }
  flp::PackageOptions opt;
  opt.flp.factoryData = flp::defaultFactoryData();
  // J-80: the demo's Gridline instruments are not rendered here (compared with
  // a plain in-memory export; renders in the package: test_flp_standin).
  opt.flp.renderInstruments = false;
  // Entry names: flat, unique (case-insensitive), never the project's own name.
  const auto entries = flp::packageEntries(s, "kick.flp", opt);
  std::set<std::string> lower;
  for (const auto& [src, n] : entries) {
    std::string l = n;
    for (char& ch : l) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    lower.insert(l);
  }
  expect(entries.size() == 3 && lower.size() == 3, "entries: three unique names");
  expect(lower.count("kick.wav") && lower.count("kick (2).wav") && lower.count("loop one.wav"), "entries: clash renamed 'kick (2).wav'");

  flp::PackageReport rep;
  bool resolve = false;
  const size_t d = packageRoundTrip(s, "demo", opt, rep, &resolve);
  expect(rep.files.size() == 3 && rep.rewritten == 3 && rep.missing.empty(), "demo: three samples packed, three paths relative");
  expect(resolve, "demo: every sample resolves after unpacking");
  expect(d == 0, "demo: packed song equals the plain FLP export (" + std::to_string(d) + " diffs)");

  // A missing sample is reported, not packed.
  Song m = s;
  m.channels.back().plugin.data = scratch + "/nowhere/gone.wav";
  flp::PackageReport mrep;
  std::string err;
  expect(flp::exportFlpZip(m, scratch + "/missing.zip", "missing", opt, mrep, &err) && mrep.missing.size() == 1 && mrep.files.size() == 2,
         "missing sample reported, the rest packed");

  // rewriteSamplePaths keeps an .flp byte for byte when nothing maps.
  flp::Bytes plain;
  flp::ExportReport prep;
  flp::exportFlp(s, plain, prep, opt.flp);
  flp::Bytes same = plain;
  expect(flp::rewriteSamplePaths(same, {}, opt.flp.factoryData, "") == 0 && same == plain, "no mapping: bytes unchanged");
  flp::Bytes notFlp = {1, 2, 3};
  expect(flp::rewriteSamplePaths(notFlp, {}, "", "") == -1, "not an flp: -1");

  // The Gridline zipped project still opens with its samples.
  CollectReport gz = exportZippedProject(s, scratch + "/gl.zip", "gl");
  std::string proj;
  Song back;
  expect(gz.error.empty() && unpackZippedProject(scratch + "/gl.zip", scratch + "/gl", proj, &err) && loadProject(proj, back, &err) &&
             missingFiles(back).empty(),
         "gridline zip: opens with its samples");
}

// FL Studio's own demo projects (when installed): package with every
// sample (factory ones too, so the paths are rewritten), unpack, open.
void testFlDemos() {
  const std::string data = flp::defaultFactoryData();
  if (data.empty()) {
    std::printf("FL Studio not installed: demo-project package checks skipped\n");
    return;
  }
  const std::vector<std::string> demos = {
      "/Data/Demo projects/Demo songs/Bombs Away ft KARRA - I'm Awake.flp",
      "/Data/Demo projects/Product demos/nucleon - Fruity Delay 3.flp",
      "/Data/Demo projects/Performance mode/Drumloop Masher.flp",
  };
  int k = 0;
  for (const auto& rel : demos) {
    const std::string path = data + rel;
    if (!fs::exists(path)) continue;
    Song s;
    flp::Report r;
    std::string err;
    if (!importFlp(path, s, r, &err, data)) continue;
    flp::PackageOptions opt;
    opt.flp.factoryData = data;
    opt.includeFactorySamples = true;
    flp::PackageReport rep;
    bool resolve = false;
    const std::string tag = "fl" + std::to_string(++k);
    const size_t d = packageRoundTrip(s, tag, opt, rep, &resolve);
    std::printf("%s (%s): %zu files, %d paths relative, %zu diffs\n", tag.c_str(), fs::path(path).filename().string().c_str(), rep.files.size(),
                rep.rewritten, d);
    expect(resolve, tag + ": every sample resolves after unpacking");
    expect(d == 0, tag + ": packed song equals the plain FLP export");
  }
}

}  // namespace

int main() {
  scratch = test::hermeticTestEnv("projfiles") + "/work";
  fs::create_directories(scratch);
  testRouting();
  testPackage();
  testFlDemos();
  return finish();
}
