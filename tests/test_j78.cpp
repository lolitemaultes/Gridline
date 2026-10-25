// Gridline — J-78 tests: FL Studio project export hunt (every plugin type at
// default / minimum / maximum, loop starters, the demo song, long and large
// projects, unicode names, hosted plugins), FLP file writing (paths with
// spaces / unicode, overwrite, read-only folders, error text), save types
// (Gridline zipped project beside the FL Studio zipped loop package) and
// notification clipboard text.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include "Check.h"
#include "core/Demo.h"
#include "core/Document.h"
#include "core/FlexStandin.h"
#include "core/MenuOps.h"
#include "engine/ChannelIns.h"
#include "core/LoopStarter.h"
#include "core/PluginInfo.h"
#include "core/Project.h"
#include "core/ProjectFiles.h"
#include "core/ProjectInterop.h"
#include "core/Song.h"
#include "core/flp/Flp.h"
#include "core/flp/FlpExport.h"
#include "core/flp/FlpPackage.h"
#include "app/ToastText.h"

using namespace gl;
namespace fs = std::filesystem;

namespace {

std::string scratch;
void expect(bool ok, const std::string& name) { expect(ok, name.c_str()); }

// Export → import from bytes; false (with `why`) on any failure or exception.
bool roundTrip(const Song& s, std::string& why, Song* back = nullptr) {
  try {
    flp::ExportOptions o;
    o.useOrigin = false;
    flp::ExportReport rep;
    flp::Bytes out;
    if (!flp::exportFlp(s, out, rep, o)) {
      why = "export returned false";
      return false;
    }
    Song b;
    flp::Report r;
    std::string err;
    if (!importFlpBytes(out, b, r, &err, "", "", false)) {
      why = "re-import failed: " + err;
      return false;
    }
    if (back) *back = std::move(b);
    return true;
  } catch (const std::exception& e) {
    why = std::string("exception: ") + e.what();
  } catch (...) {
    why = "unknown exception";
  }
  return false;
}

Song withChannel(const PluginState& st) {
  Song s = makeEmptySong();
  Channel c;
  c.id = s.allocateId();
  c.name = "Ch";
  c.plugin = st;
  s.channels.push_back(c);
  return s;
}

PluginState at(const PluginInfo& info, int mode) {
  PluginState st = defaultPluginState(info.type);
  for (size_t i = 0; i < info.params.size() && i < st.params.size(); ++i)
    st.params[i] = static_cast<float>(mode == 0 ? info.params[i].min : info.params[i].max);
  return st;
}

void testEveryPlugin() {
  int bad = 0, total = 0;
  for (const auto& info : pluginCatalog()) {
    for (int mode = -1; mode <= 1; ++mode) {
      PluginState st = mode < 0 ? defaultPluginState(info.type) : at(info, mode);
      if (info.usesSample && st.data.empty()) st.data = scratch + "/Sämples dir/kick ü.wav";
      Song s = makeEmptySong();
      if (info.kind == PluginKind::Generator) {
        s = withChannel(st);
      } else if (info.kind == PluginKind::NoteFx) {
        s = withChannel(defaultPluginState("gl.synth"));
        s.channels[0].noteFx.push_back(st);
      } else {
        ensureMixer(s);
        s.mixer[1].slots[0].plugin = st;
        s.mixer[0].slots[0].plugin = st;
      }
      std::string why;
      ++total;
      if (!roundTrip(s, why)) {
        ++bad;
        std::printf("  # %s (%s): %s\n", info.type.c_str(), mode < 0 ? "default" : mode == 0 ? "min" : "max", why.c_str());
      }
    }
  }
  std::printf("  every plugin: %d / %d exported and re-imported\n", total - bad, total);
  expect(bad == 0, "every plugin type at default / min / max exports and re-imports");
}

void testLoopStarters() {
  int bad = 0;
  for (int st = 0; st < static_cast<int>(loopstarter::styles().size()); ++st)
    for (int f = 0; f < static_cast<int>(loopstarter::Form::Count); ++f) {
      auto set = loopstarter::styleDefaults(st, 7);
      set.form = static_cast<loopstarter::Form>(f);
      std::string why;
      if (!roundTrip(loopstarter::generate(set), why)) {
        ++bad;
        std::printf("  # loop starter %d/%d: %s\n", st, f, why.c_str());
      }
    }
  expect(bad == 0, "every loop starter style and form exports");
  std::string why;
  expect(roundTrip(makeDemoSong(), why), "demo song exports (" + why + ")");
  expect(roundTrip(makeEmptySong(), why), "empty project exports (" + why + ")");
  Song bare;  // no mixer at all
  expect(roundTrip(bare, why), "project without a mixer exports (" + why + ")");
}

void testLarge() {
  Song s = makeDemoSong();
  s.info.title = "Ünïcödé 曲 🎵";
  for (auto& c : s.channels) c.name += " — ñ";
  // Very long: clips far into the song.
  if (!s.playlist.empty()) {
    Clip c = s.playlist[0];
    c.id = s.allocateId();
    c.start = static_cast<Tick>(s.barTicks()) * 9000;
    s.playlist.push_back(c);
  }
  s.markers.push_back({static_cast<Tick>(s.barTicks()) * 8000, "Far marker ✓"});
  std::string why;
  Song back;
  expect(roundTrip(s, why, &back), "long project with unicode names exports (" + why + ")");
  expect(back.info.title == s.info.title, "unicode title kept");
  // Many channels / patterns / inserts / tracks.
  Song m = makeEmptySong();
  for (int i = 0; i < 300; ++i) {
    Channel c;
    c.id = m.allocateId();
    c.name = "C" + std::to_string(i);
    c.plugin = defaultPluginState("gl.synth");
    m.channels.push_back(c);
  }
  while (m.patterns.size() < 1100) {
    Pattern p;
    p.id = m.allocateId();
    p.name = "P" + std::to_string(m.patterns.size());
    m.patterns.push_back(p);
  }
  while (m.mixer.size() < 140) {
    MixerTrack t;
    t.id = m.allocateId();
    t.sends.push_back({m.mixer[0].id, 1.0f, false});
    m.mixer.push_back(t);
  }
  m.tracks.resize(600);
  expect(roundTrip(m, why), "large project (300 channels, 1100 patterns, 140 inserts, 600 tracks) exports (" + why + ")");
  // Hosted plugin states (installed or not).
  Song h = withChannel(PluginState{"vst3:0123456789ABCDEF0123456789ABCDEF", {0.5f, 0.25f}, "", "", ""});
  ensureMixer(h);
  h.mixer[1].slots[0].plugin = PluginState{"au:aufx,dely,appl", {}, "", "", ""};
  h.mixer[1].slots[1].plugin = PluginState{"clap:com.example.thing", {1.0f}, "garbage", "", ""};
  expect(roundTrip(h, why), "hosted plugin placeholders export (" + why + ")");
}

// The demo song with what the later tracks added: chopped clips (J-60),
// looping clips with offsets and copies (J-69), smooth-timing channel
// envelopes (J-72), a FLEX stand-in (J-53), automation (looping), clip /
// pattern / channel groups, markers of every kind, track modes (J-56).
Song featureSong() {
  Song s = makeDemoSong();
  Document d;
  d.reset(s);
  std::vector<ObjectId> pat;
  for (const auto& c : d.song().playlist)
    if (c.kind == ClipKind::Pattern) pat.push_back(c.id);
  if (!pat.empty()) ops::chopClip(d, pat[0], d.song().barTicks() / 2);
  s = d.song();
  for (auto& p : s.patterns) p.loop = true;
  for (size_t i = 0; i < s.playlist.size() && i < 4; ++i) {
    Clip c = s.playlist[i];
    c.id = s.allocateId();
    c.start += s.barTicks() * 64;
    c.offset = s.ppq * 3;
    c.length = s.barTicks() * 7 + 5;
    c.group = 77;
    s.playlist.push_back(c);
  }
  InsSettings ins;
  ins.timing = InsTiming::Smooth;
  ins.t[InsVol].on = true;
  ins.t[InsVol].lfoAmount = 64;
  ins.cutoff = 120;
  ins.type = 6;
  for (auto& c : s.channels)
    if (insSupported(c.plugin.type)) c.plugin.ins = formatIns(ins);
  float controls[flex::kControls] = {};
  for (int i = 0; i < flex::kControls; ++i) controls[i] = 0.5f;
  Channel fx;
  fx.id = s.allocateId();
  fx.name = "Flex";
  fx.plugin = flex::makeState("Pack ü", "Preset 1", controls);
  s.channels.push_back(fx);
  AutomationClip a;
  a.id = s.allocateId();
  a.owner = s.mixer.size() > 1 ? s.mixer[1].id : kNoId;
  a.index = 0;
  a.loop = s.ppq * 4;
  a.points = {{0, 0.0f, 0.0f, AutoShape::Curve}, {s.ppq * 2, 1.0f, 0.5f, AutoShape::Curve}};
  s.automations.push_back(a);
  Clip ac;
  ac.id = s.allocateId();
  ac.kind = ClipKind::Automation;
  ac.source = a.id;
  ac.track = 5;
  ac.length = s.barTicks() * 3;
  s.playlist.push_back(ac);
  s.markers.push_back({s.barTicks() * 2, "Loop", 0, 0, MarkerAction::Loop});
  s.markers.push_back({s.barTicks() * 3, "Sig", 7, 8});
  Marker key;
  key.pos = s.barTicks() * 4;
  key.name = "Key";
  key.keyRoot = 3;
  s.markers.push_back(key);
  s.channelGroups = {"Drums ✓"};
  if (!s.channels.empty()) s.channels[0].group = 0;
  s.tracks[0].groupSize = 2;
  s.tracks[0].folded = true;
  s.tracks[3].mode = TrackMode::Audio;
  s.tracks[3].mixer = s.mixer.size() > 2 ? s.mixer[2].id : kNoId;
  s.tracks[4].mode = TrackMode::Instrument;
  s.tracks[4].channel = s.channels.empty() ? kNoId : s.channels[0].id;
  std::sort(s.markers.begin(), s.markers.end(), [](const Marker& x, const Marker& y) { return x.pos < y.pos; });
  return s;
}

void testFeatures() {
  std::string why;
  expect(roundTrip(featureSong(), why), "feature song (chops, loops, smooth envelopes, FLEX, automation, groups, track modes) exports (" + why + ")");
  // The FL zipped loop package and the Gridline zipped project of it.
  const std::string dir = scratch + "/pack dir ü";
  fs::create_directories(dir);
  flp::PackageOptions po;
  flp::PackageReport pr;
  std::string err;
  expect(flp::exportFlpZip(featureSong(), dir + "/Feat.zip", "Feat", po, pr, &err), "feature song: FL zipped loop package (" + err + ")");
  expect(exportZippedProject(featureSong(), dir + "/Feat.gridline.zip", "Feat").error.empty(), "feature song: Gridline zipped project");
}

// ------------------------------------------------------------ save types

void testSaveTypes() {
  const auto& t = saveFileTypes();
  expect(t.size() == static_cast<size_t>(SaveFormat::Count), "one save type per format");
  expect(t.size() == 4 && t[0].label.find("Gridline project") == 0 && t[1].label.find("Gridline zipped project") == 0 &&
             t[2].label.find("FL Studio project") == 0 && t[3].label.find("FL Studio zipped loop package") == 0,
         "save types: Gridline, Gridline zipped, FL Studio project, FL Studio zipped loop package");
  expect(t[static_cast<size_t>(SaveFormat::GridlineZip)].exts == std::vector<std::string>{".gridline.zip"} &&
             t[static_cast<size_t>(SaveFormat::FlZip)].exts == std::vector<std::string>{".zip"},
         "type index = format; the zipped types differ by extension");
  // Extension handling: the two .zip formats never get confused.
  expect(saveFormatOf("/d/Song.gridline.zip") == SaveFormat::GridlineZip && saveFormatOf("/d/SONG.Gridline.ZIP") == SaveFormat::GridlineZip,
         "format by name: .gridline.zip (any case)");
  expect(saveFormatOf("/d/Song.zip") == SaveFormat::FlZip && saveFormatOf("/d/gridline.zip") == SaveFormat::FlZip,
         "format by name: any other .zip is an FL package");
  expect(withSaveExtension("Song.zip", SaveFormat::GridlineZip) == "Song.gridline.zip", "FL zip name -> Gridline zip name");
  expect(withSaveExtension("Song.gridline.zip", SaveFormat::FlZip) == "Song.zip", "Gridline zip name -> FL zip name (no .gridline left)");
  expect(withSaveExtension("Song.gridline.zip", SaveFormat::GridlineZip) == "Song.gridline.zip", "Gridline zip name kept");
  expect(withSaveExtension("Song.gridline.zip", SaveFormat::Gridline) == "Song.gridline.json", "Gridline zip -> Gridline project");
  expect(withSaveExtension("Song.gridline.zip", SaveFormat::Flp) == "Song.flp", "Gridline zip -> FL project");
  expect(withSaveExtension("/d/Song.flp", SaveFormat::GridlineZip) == "/d/Song.gridline.zip", "FL project -> Gridline zip");
  expect(withSaveExtension("Song", SaveFormat::GridlineZip) == "Song.gridline.zip", "no extension -> .gridline.zip");
  expect(withSaveExtension("My.Song", SaveFormat::GridlineZip) == "My.Song.gridline.zip", "dots kept");
  expect(projectStemOf("/d/Song.gridline.zip") == "Song", "stem of a Gridline zip");
  // Every format's name maps back to that format.
  for (int f = 0; f < static_cast<int>(SaveFormat::Count); ++f)
    expect(saveFormatOf(withSaveExtension("Track 1", static_cast<SaveFormat>(f))) == static_cast<SaveFormat>(f),
           "format " + std::to_string(f) + ": name round trip");
  // Opening: both zips open as projects.
  expect(openKindOf("/d/Song.gridline.zip") == OpenKind::ZippedProject && opensAsProject("/d/Song.gridline.zip"), "a Gridline zip opens");
}

std::string tone(const std::string& path) {
  fs::create_directories(fs::path(path).parent_path());
  std::FILE* f = std::fopen(path.c_str(), "wb");
  // A tiny valid WAV: 16-bit mono, 8 frames.
  const unsigned char hdr[44] = {'R', 'I', 'F', 'F', 52, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 1, 0,
                                 0x44, 0xAC, 0, 0, 0x88, 0x58, 1, 0, 2, 0, 16, 0, 'd', 'a', 't', 'a', 16, 0, 0, 0};
  std::fwrite(hdr, 1, sizeof hdr, f);
  const short s[8] = {0, 1000, 2000, 1000, 0, -1000, -2000, -1000};
  std::fwrite(s, 2, 8, f);
  std::fclose(f);
  return path;
}

Song sampleSong(const std::string& wav) {
  Song s = makeEmptySong();
  Channel c;
  c.id = s.allocateId();
  c.name = "Kick";
  c.plugin = defaultPluginState("gl.sampler");
  c.plugin.data = wav;
  s.channels.push_back(c);
  s.info.title = "Zip test";
  return s;
}

void testZipRoundTrip() {
  const std::string dir = scratch + "/zips ✓";
  const std::string wav = tone(scratch + "/lib/kick ü.wav");
  const Song s = sampleSong(wav);
  // Gridline zipped project: written, recognised by name, unpacked, opened.
  const std::string gz = withSaveExtension(dir + "/My Song", SaveFormat::GridlineZip);
  fs::create_directories(dir);
  CollectReport r = exportZippedProject(s, gz, projectStemOf(gz));
  expect(r.error.empty() && r.copied == 1 && fs::exists(gz), "Gridline zip written with its sample");
  expect(saveFormatOf(gz) == SaveFormat::GridlineZip, "the written name re-saves as a Gridline zip");
  std::string proj, err;
  expect(unpackZippedProject(gz, dir + "/My Song", proj, &err) && proj.size() > 14 && proj.compare(proj.size() - 14, 14, ".gridline.json") == 0,
         "Gridline zip unpacks to a Gridline project (" + err + ")");
  Song back;
  expect(loadProject(proj, back, &err) && back.channels.size() == 1 && back.info.title == "Zip test", "Gridline zip opens (" + err + ")");
  expect(!back.channels.empty() && fs::exists(back.channels[0].plugin.data) && missingFiles(back).empty(), "Gridline zip: the sample resolves");
  // Re-saving the opened project as a Gridline zip again (Save keeps the format).
  r = exportZippedProject(back, gz, projectStemOf(gz));
  expect(r.error.empty() && r.copied == 1, "Gridline zip re-saved over itself");
  // The FL zipped loop package beside it still opens as an FL project.
  const std::string fz = withSaveExtension(dir + "/My Song", SaveFormat::FlZip);
  flp::PackageOptions po;
  flp::PackageReport pr;
  expect(fz != gz && flp::exportFlpZip(s, fz, projectStemOf(fz), po, pr, &err), "FL zip written beside it (" + err + ")");
  expect(saveFormatOf(fz) == SaveFormat::FlZip, "FL zip name re-saves as an FL zip");
  expect(unpackZippedProject(fz, dir + "/fl", proj, &err) && openKindOf(proj) == OpenKind::FlProject, "FL zip unpacks to an .flp");
  flp::Report fr;
  expect(importFlp(proj, back, fr, &err) && back.channels.size() == 1, "FL zip opens (" + err + ")");
}

// ------------------------------------------------------------ file writing

bool leftovers(const std::string& dir) {
  std::error_code ec;
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
    if (it->path().filename().string().find(".tmp") != std::string::npos) return true;
  return false;
}

std::string slurp(const std::string& p) {
  std::FILE* f = std::fopen(p.c_str(), "rb");
  if (!f) return "";
  std::string s;
  char b[4096];
  size_t n;
  while ((n = std::fread(b, 1, sizeof b, f)) > 0) s.append(b, n);
  std::fclose(f);
  return s;
}

// Whether file permissions stop this process: not as root (CI containers), and folder / read
// permissions not on Windows. Probed, not assumed.
bool folderPermsEnforced(const std::string& scratch) {
  const std::string d = scratch + "/perm-probe";
  fs::create_directories(d);
  fs::permissions(d, fs::perms::owner_read | fs::perms::owner_exec, fs::perm_options::replace);
  bool enforced;
  {
    std::ofstream os(d + "/x");
    enforced = !os;
  }
  fs::permissions(d, fs::perms::owner_all, fs::perm_options::replace);
  std::error_code ec;
  fs::remove_all(d, ec);
  return enforced;
}
bool filePermsEnforced(const std::string& scratch) {
  const std::string f = scratch + "/perm-probe.txt";
  { std::ofstream(f) << "x"; }
  fs::permissions(f, fs::perms::owner_read, fs::perm_options::replace);
  bool enforced;
  {
    std::ofstream os(f, std::ios::app);
    enforced = !os;
  }
  fs::permissions(f, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace);
  std::error_code ec;
  fs::remove(f, ec);
  return enforced;
}

void testFlpFileWriting() {
  const Song s = makeDemoSong();
  flp::ExportOptions o;
  flp::ExportReport rep;
  std::string err;
  // Spaces + unicode in folder and name; then overwrite (Replace).
  const std::string dir = scratch + "/My Projects ü 曲";
  fs::create_directories(dir);
  const std::string p = dir + "/Song ✓ 1.flp";
  expect(flp::exportFlpFile(s, p, rep, o, &err), "FLP written to a path with spaces / unicode (" + err + ")");
  Song back;
  flp::Report fr;
  expect(importFlp(p, back, fr, &err) && back.channels.size() == s.channels.size(), "that FLP re-imports (" + err + ")");
  expect(flp::exportFlpFile(s, p, rep, o, &err), "FLP overwrites an existing file (" + err + ")");
  expect(!leftovers(dir), "no temporary files left after writing");

  const bool folderPerms = folderPermsEnforced(scratch), filePerms = filePermsEnforced(scratch);
  if (!folderPerms) std::printf("# folder permissions are not enforced here (root / Windows): read-only folder checks skipped\n");
  if (!filePerms) std::printf("# file permissions are not enforced here (root): read-only file checks skipped\n");
  flp::PackageOptions po;
  flp::PackageReport pr;
  CollectReport cr;
  // Read-only folder: clear, actionable error, nothing left behind.
  if (folderPerms) {
  const std::string ro = scratch + "/read only";
  fs::create_directories(ro);
  fs::permissions(ro, fs::perms::owner_read | fs::perms::owner_exec, fs::perm_options::replace);
  err.clear();
  expect(!flp::exportFlpFile(s, ro + "/x.flp", rep, o, &err), "read-only folder: write fails");
  std::printf("  # read-only folder: %s\n", err.c_str());
  expect(err.find("x.flp") != std::string::npos && err.find("no permission") != std::string::npos &&
             err.find("Choose another folder") != std::string::npos,
         "read-only folder: the message names the file, the reason and what to do");
  // Zipped formats in the same folder.
  err.clear();
  expect(!flp::exportFlpZip(s, ro + "/x.zip", "x", po, pr, &err) && err.find("no permission") != std::string::npos,
         "read-only folder: FL zip fails with the reason (" + err + ")");
  cr = exportZippedProject(s, ro + "/x.gridline.zip", "x");
  expect(cr.error.find("no permission") != std::string::npos, "read-only folder: Gridline zip fails with the reason (" + cr.error + ")");
  fs::permissions(ro, fs::perms::owner_all, fs::perm_options::replace);
  expect(!leftovers(ro) && fs::is_empty(ro), "read-only folder: nothing written");
  }

  // A read-only file is not replaced; its content stays.
  if (filePerms) {
  const std::string locked = dir + "/Locked.flp";
  expect(flp::exportFlpFile(s, locked, rep, o, &err), "locked file: first write");
  const std::string before = slurp(locked);
  fs::permissions(locked, fs::perms::owner_read, fs::perm_options::replace);
  Song other = makeEmptySong();
  err.clear();
  expect(!flp::exportFlpFile(other, locked, rep, o, &err) && err.find("read-only") != std::string::npos,
         "read-only file: not replaced, says why (" + err + ")");
  expect(slurp(locked) == before, "read-only file: content unchanged");
  fs::permissions(locked, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace);
  }

  // Missing folder / a folder in the way / a too-long name.
  err.clear();
  expect(!flp::exportFlpFile(s, scratch + "/no such folder/x.flp", rep, o, &err) && err.find("does not exist") != std::string::npos,
         "missing folder: says so (" + err + ")");
  fs::create_directories(dir + "/Taken.flp");
  err.clear();
  expect(!flp::exportFlpFile(s, dir + "/Taken.flp", rep, o, &err) && err.find("a folder has that name") != std::string::npos,
         "folder in the way: says so (" + err + ")");
  err.clear();
  expect(!flp::exportFlpFile(s, dir + "/" + std::string(300, 'a') + ".flp", rep, o, &err) && err.find("too long") != std::string::npos,
         "name too long: says so (" + err + ")");
  expect(!leftovers(dir), "no temporary files left after failures");

  // An archive that fails half-way keeps the previous file (J-78: atomic zips).
  const std::string wav = tone(scratch + "/lib2/snare.wav");
  const Song zs = sampleSong(wav);
  const std::string gz = dir + "/Keep.gridline.zip", fz = dir + "/Keep.zip";
  expect(exportZippedProject(zs, gz, "Keep").error.empty() && flp::exportFlpZip(zs, fz, "Keep", po, pr, &err), "good archives written");
  const std::string gzBefore = slurp(gz), fzBefore = slurp(fz);
  if (folderPerms) {
  fs::permissions(wav, fs::perms::none, fs::perm_options::replace);  // the sample can no longer be read
  cr = exportZippedProject(zs, gz, "Keep");
  err.clear();
  const bool fzOk = flp::exportFlpZip(zs, fz, "Keep", po, pr, &err);
  fs::permissions(wav, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace);
  expect(!cr.error.empty() && cr.error.find("snare.wav") != std::string::npos, "unreadable sample: Gridline zip fails naming it (" + cr.error + ")");
  expect(!fzOk && err.find("snare.wav") != std::string::npos, "unreadable sample: FL zip fails naming it (" + err + ")");
  expect(slurp(gz) == gzBefore && slurp(fz) == fzBefore, "a failed archive leaves the previous file intact");
  }
  expect(!leftovers(dir), "a failed archive leaves no temporary file");
}

// ------------------------------------------------------------ notifications

void testToastText() {
  expect(toast::clipboardText("Saved Song.flp", "/d/Song.flp", false) == "Saved Song.flp\n/d/Song.flp", "copy: message + details");
  expect(toast::clipboardText("Could not save: x", "", true) == "Error: Could not save: x", "copy: errors say so");
  expect(toast::clipboardText("cannot write /d/x.flp", "/d/x.flp", true) == "Error: cannot write /d/x.flp", "copy: details already in the text are not repeated");
  expect(toast::lifetime(true) > toast::lifetime(false) && toast::lifetime(true) >= 8.0, "errors stay up longer");
  expect(toast::kCopiedFeedback >= 0.8 && toast::kCopiedFeedback <= 1.5, "copied feedback ~1 s");
}

}  // namespace

int main() {
  scratch = (fs::temp_directory_path() / "gl_j78").string();
  fs::remove_all(scratch);
  fs::create_directories(scratch);
  testEveryPlugin();
  testLoopStarters();
  testLarge();
  testFeatures();
  testSaveTypes();
  testZipRoundTrip();
  testFlpFileWriting();
  testToastText();
  return finish();
}
