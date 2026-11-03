// Gridline - K-45: the unpacked project cache. Every archive unpacks into the cache and its own folder
// stays as it was; what happens to a copy when its project closes, at the sweep after a crash, under
// the size limit and at Clear cache, with real locks and a real second process that dies.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <new>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include "../src/core/PluginInfo.h"
#include "../src/core/Project.h"
#include "../src/core/ProjectArchive.h"
#include "../src/core/ProjectInterop.h"
#include "../src/core/RemoteFs.h"
#include "../src/core/WavWriter.h"
#include "../src/core/Zip.h"
#include "../src/engine/ProjectLoad.h"
#include "../src/engine/Sample.h"
#include "../src/engine/TimeStretch.h"
#include "Check.h"
#include "Hermetic.h"

using namespace gl;
namespace fs = std::filesystem;
using projload::Loader;
using projload::Phase;

namespace {

void expect(bool ok, const std::string& what) { expect(ok, what.c_str()); }

std::set<std::string> listing(const fs::path& dir) {
  std::set<std::string> out;
  std::error_code ec;
  for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) out.insert(it->path().lexically_relative(dir).u8string());
  return out;
}

void writeText(const fs::path& p, const std::string& text) {
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  std::ofstream o(p, std::ios::binary);
  o << text;
}

std::string wavFile(const fs::path& p) {
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  AudioBuffer b(2, 2000);
  for (size_t i = 0; i < b.frames; ++i) b.at(0, i) = b.at(1, i) = static_cast<float>(i % 50) / 100.0f;
  writeWav(p.u8string(), b, 44100, 16);
  return p.u8string();
}

// A Gridline project with one sample, zipped, the sample by its name.
void projectZip(const fs::path& zip, const fs::path& scratch, const std::string& extra = "") {
  std::error_code ec;
  fs::create_directories(zip.parent_path(), ec);
  const std::string wav = wavFile(scratch / "tone.wav");
  Song s = makeEmptySong();
  Channel c;
  c.id = s.allocateId();
  c.name = "Tone";
  c.plugin = defaultPluginState("gl.sampler");
  c.plugin.data = "tone.wav";
  c.mixerTrack = s.masterId();
  s.channels.clear();
  s.channels.push_back(c);
  const std::string text = projectToText(s);
  ZipWriter w;
  expect(w.open(zip.u8string()), "the archive opens for writing");
  w.add("Tune.gridline.json", std::vector<uint8_t>(text.begin(), text.end()));
  w.addFile("tone.wav", wav);
  if (!extra.empty()) w.add("extra.txt", std::vector<uint8_t>(extra.begin(), extra.end()));
  expect(w.close(), "the archive is written");
}

Song songUsing(const std::string& file) {
  Song s = makeEmptySong();
  Channel c;
  c.id = s.allocateId();
  c.plugin = defaultPluginState("gl.sampler");
  c.plugin.data = file;
  c.mixerTrack = s.masterId();
  s.channels.clear();
  s.channels.push_back(c);
  return s;
}

struct Fixture {
  fs::path root, music, cache, recovery, backup, scratch;
  archive::CacheRefs refs;
  archive::UnpackRequest request(const fs::path& zip) const {
    archive::UnpackRequest u;
    u.zipPath = zip.u8string();
    u.cacheRoot = cache.u8string();
    u.refs = &refs;
    return u;
  }
  archive::UnpackResult open(const fs::path& zip) const {
    archive::UnpackResult r;
    std::string err;
    expect(archive::unpackForOpening(request(zip), r, &err), "opens (" + err + ")");
    return r;
  }
  archive::CacheReport sweep() const { return archive::sweepCache(cache.u8string(), archive::kCacheLimit, &refs); }
  archive::CacheReport clear() const { return archive::clearCache(cache.u8string(), &refs); }
};

Fixture fixture(const fs::path& base, const char* name) {
  Fixture f;
  f.root = base / name;
  f.music = f.root / "My Music";
  f.cache = f.root / "cache dir" / "unpacked";
  f.recovery = f.root / "recovery";
  f.backup = f.root / "Backup";
  f.scratch = f.root / "scratch";
  f.refs.recovery.push_back(f.recovery.u8string());
  f.refs.backups.push_back(f.backup.u8string());
  std::error_code ec;
  fs::create_directories(f.music, ec);
  return f;
}

void testLocalZip(const fs::path& base) {
  Fixture f = fixture(base, "local");
  const fs::path zip = f.music / "Tune.zip";
  projectZip(zip, f.scratch);
  // A folder an earlier version unpacked beside the archive is neither used nor touched.
  writeText(f.music / "Tune" / "Tune.gridline.json", "old project, saved here");
  writeText(f.music / "Tune" / "tone.wav", "old sample");
  const auto before = listing(f.music);
  archive::UnpackResult r = f.open(zip);
  expect(listing(f.music) == before, "a local archive leaves its folder untouched");
  expect(r.inCache && !r.fromDrive && !r.reused, "it unpacks into the cache");
  expect(r.projectFile.rfind(f.cache.u8string(), 0) == 0 && fs::is_regular_file(fs::u8path(r.folder) / "tone.wav"), "the project and its files are there");
  expect(r.use != nullptr, "and the copy is claimed");
  const fs::path copy = fs::u8path(r.folder);

  // Open: nothing removes it.
  auto s1 = f.sweep();
  expect(fs::exists(copy) && s1.keptOpen == 1 && s1.removed == 0, "the sweep leaves an open copy");
  auto c1 = f.clear();
  expect(fs::exists(copy) && c1.keptOpen == 1 && c1.removed == 0, "Clear cache leaves an open copy and says so");
  expect(archive::trimCache(f.cache.u8string(), 0, "", 0, &f.refs) == 0 && fs::exists(copy), "trimming leaves an open copy");

  // Two users of one copy: the second open reuses it; closing one leaves it for the other.
  archive::UnpackResult r2 = f.open(zip);
  expect(r2.reused && r2.folder == r.folder, "open again while open: the same copy");
  r.use.reset();
  expect(f.sweep().keptOpen == 1 && fs::exists(copy), "closing one of two leaves it");

  // Closed: gone, its lock too; the archive and its folder are as they were.
  r2.use.reset();
  auto s2 = f.sweep();
  expect(!fs::exists(copy) && s2.removed == 1 && s2.freed > 0, "closing the project deletes the copy");
  expect(listing(f.cache).empty(), "nothing is left in the cache");
  expect(listing(fs::path(f.cache.u8string() + ".locks")).empty(), "nor among the locks");
  expect(listing(f.music) == before, "the folder is still untouched");

  // A project whose copy was removed reopens from its zip.
  archive::UnpackResult r3 = f.open(zip);
  expect(!r3.reused && fs::is_regular_file(r3.projectFile) && fs::is_regular_file(fs::u8path(r3.folder) / "tone.wav"), "it reopens from its zip");

  // A new version of the archive is a new copy; the old one goes once closed.
  const fs::path oldCopy = fs::u8path(r3.folder);
  r3.use.reset();
  projectZip(zip, f.scratch, "changed");
  archive::UnpackResult r4 = f.open(zip);
  expect(r4.folder != oldCopy.u8string() && !r4.reused, "a changed archive unpacks anew");
  f.sweep();
  expect(!fs::exists(oldCopy) && fs::exists(fs::u8path(r4.folder)), "the old version goes, the open one stays");
}

void testDriveCopy(const fs::path& base) {
  Fixture f = fixture(base, "drive");
  const fs::path drive = f.root / "drive";
  remotefs::setTestPrefixes({drive.u8string()});
  const fs::path zip = drive / "Music" / "Tune.zip";
  projectZip(zip, f.scratch);
  const auto before = listing(drive);
  archive::UnpackResult r = f.open(zip);
  expect(r.fromDrive && listing(drive) == before, "a drive's archive: nothing on the drive");
  const fs::path copy = fs::u8path(r.folder);
  r.use.reset();
  auto s = f.sweep();
  expect(fs::exists(copy) && s.keptDrive == 1 && s.removed == 0, "a copy from a drive stays after closing");
  archive::UnpackResult again = f.open(zip);
  expect(again.reused, "and opens without reading the drive again");
  again.use.reset();
  // The limit still applies to it.
  fs::last_write_time(copy / ".gridline-unpacked", fs::file_time_type::clock::now() - std::chrono::hours(2));
  archive::sweepCache(f.cache.u8string(), 10, &f.refs);
  expect(!fs::exists(copy), "the limit trims it");
  archive::UnpackResult third = f.open(zip);
  third.use.reset();
  auto c = f.clear();
  expect(!fs::exists(fs::u8path(third.folder)) && c.removed == 1, "Clear cache removes it");
  remotefs::setTestPrefixes({});
}

void testPinnedAndRefs(const fs::path& base) {
  Fixture f = fixture(base, "refs");
  // Pinned.
  {
    const fs::path zip = f.music / "Pinned.zip";
    projectZip(zip, f.scratch);
    archive::UnpackResult r = f.open(zip);
    const fs::path copy = fs::u8path(r.folder);
    const Song s = songUsing((copy / "tone.wav").u8string());
    const auto used = archive::copiesUsedBy(s, f.cache.u8string());
    expect(used.size() == 1 && fs::u8path(used[0]) == copy, "the song's copy is found");
    expect(archive::pinUnpacked(s, f.cache.u8string()) == 1, "a saved project marks it");
    r.use.reset();
    expect(f.sweep().keptSaved == 1 && fs::exists(copy), "a marked copy survives closing and the sweep");
    auto c = f.clear();
    expect(c.keptSaved == 1 && c.removed == 0 && fs::exists(copy), "and Clear cache, which says why");
    archive::trimCache(f.cache.u8string(), 0, "", 0, &f.refs);
    expect(fs::exists(copy), "and the limit");
    fs::remove_all(copy);
  }
  // Crash recovery.
  {
    const fs::path zip = f.music / "Crashed.zip";
    projectZip(zip, f.scratch);
    archive::UnpackResult r = f.open(zip);
    const fs::path copy = fs::u8path(r.folder);
    writeText(f.recovery / "Crashed.gridline.json", "{\"sample\": \"" + (copy / "tone.wav").generic_u8string() + "\"}");
    r.use.reset();
    auto s = f.sweep();
    expect(s.keptRecovery == 1 && fs::exists(copy), "a copy crash recovery points at survives the sweep");
    auto c = f.clear();
    expect(c.keptRecovery == 1 && c.removed == 0 && fs::exists(copy), "and Clear cache, which says why");
    archive::trimCache(f.cache.u8string(), 0, "", 0, &f.refs);
    expect(fs::exists(copy), "and the limit");
    fs::last_write_time(f.recovery / "Crashed.gridline.json", fs::file_time_type::clock::now() - std::chrono::hours(24 * 90));
    expect(f.sweep().keptRecovery == 1 && fs::exists(copy), "however old the recovery file is");
    fs::remove_all(f.recovery);
    expect(f.sweep().removed == 1 && !fs::exists(copy), "once it has been dealt with the copy goes");
  }
  // Timed backups.
  {
    const fs::path zip = f.music / "Backed.zip";
    projectZip(zip, f.scratch);
    archive::UnpackResult r = f.open(zip);
    const fs::path copy = fs::u8path(r.folder);
    const fs::path bak = f.backup / "Untitled (backup 2026-11-04 10-00-00).gridline.json";
    // (the name across two reads of the scan: it starts just before a megabyte)
    writeText(bak, std::string((1u << 20) - 5, ' ') + (copy / "tone.wav").generic_u8string());
    r.use.reset();
    expect(f.sweep().keptRecovery == 1 && fs::exists(copy), "a copy a recent backup points at stays");
    expect(f.clear().keptRecovery == 1 && fs::exists(copy), "Clear cache keeps it too");
    // At the limit a backup's copy goes last.
    const fs::path zip2 = f.music / "Other.zip";
    projectZip(zip2, f.scratch);
    archive::UnpackResult r2 = f.open(zip2);
    const fs::path other = fs::u8path(r2.folder);
    r2.use.reset();
    const auto hours = [](int n) { return fs::file_time_type::clock::now() - std::chrono::hours(n); };
    fs::last_write_time(copy / ".gridline-unpacked", hours(50));
    fs::last_write_time(other / ".gridline-unpacked", hours(10));
    const uint64_t oneCopy = archive::cacheBytes(f.cache.u8string()) / 2 + 2000;
    archive::trimCache(f.cache.u8string(), oneCopy, "", 3600, &f.refs);
    expect(fs::exists(copy) && !fs::exists(other), "at the limit the other copy goes first, though newer");
    archive::trimCache(f.cache.u8string(), 0, "", 3600, &f.refs);
    expect(!fs::exists(copy), "and the backup's copy when the limit demands it");
    // An old backup no longer keeps one.
    archive::UnpackResult r3 = f.open(zip);
    r3.use.reset();
    fs::last_write_time(bak, fs::file_time_type::clock::now() - std::chrono::hours(24 * (archive::kBackupRefDays + 1)));
    expect(f.sweep().removed == 1 && !fs::exists(fs::u8path(r3.folder)), "a backup older than the window does not keep a copy");
  }
#if !defined(_WIN32)
  // What cannot be read cannot rule anything out.
  {
    const fs::path zip = f.music / "Unread.zip";
    projectZip(zip, f.scratch);
    archive::UnpackResult r = f.open(zip);
    const fs::path copy = fs::u8path(r.folder);
    r.use.reset();
    const fs::path shut = f.recovery / "shut.json";
    writeText(shut, "x");
    fs::permissions(shut, fs::perms::none);
    if (!std::ifstream(shut)) {
      auto s = f.sweep();
      expect(s.keptUnknown == 1 && fs::exists(copy), "a recovery file that cannot be read: the copy stays");
    }
    fs::permissions(shut, fs::perms::owner_all);
    fs::remove_all(f.recovery);
    f.sweep();
    expect(!fs::exists(copy), "readable again: it goes");
  }
#endif
}

// A second process claims the copy, and is killed without a word.
void testOtherInstance(const fs::path& base) {
  Fixture f = fixture(base, "other");
  const fs::path zip = f.music / "Shared.zip";
  projectZip(zip, f.scratch);
  archive::UnpackResult r = f.open(zip);
  const fs::path copy = fs::u8path(r.folder);
  r.use.reset();
  const fs::path ready = f.root / "child-ready";
  platform::Process child;
  const std::string exe = platform::executablePath();
  expect(child.start(exe, {exe, "--hold", copy.u8string(), f.cache.u8string(), ready.u8string()}), "a second process starts");
  for (int i = 0; i < 1000 && !fs::exists(ready); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  expect(fs::exists(ready), "and claims the copy");
  auto s = f.sweep();
  expect(s.keptOpen == 1 && fs::exists(copy), "a copy open in another Gridline survives the sweep");
  auto c = f.clear();
  expect(c.keptOpen == 1 && fs::exists(copy), "and Clear cache");
  archive::trimCache(f.cache.u8string(), 0, "", 0, &f.refs);
  expect(fs::exists(copy), "and the limit");
  child.kill();
  child.wait(10);
  expect(!child.running(), "the other Gridline is gone, without closing anything");
  auto s2 = f.sweep();
  expect(s2.removed == 1 && !fs::exists(copy), "a dead owner's copy goes");
}

void testLeftovers(const fs::path& base) {
  Fixture f = fixture(base, "left");
  const fs::path locks(f.cache.u8string() + ".locks");
  std::error_code ec;
  auto part = [&](const char* name) {
    const fs::path d = f.cache / name;
    writeText(d / "half.wav", std::string(5000, 'x'));
    return d;
  };
  // A crash mid-unpack: its lock file is there, no one holds it.
  const fs::path crashed = part("Song-0123456789abcdef.part-99-1");
  writeText(locks / crashed.filename(), "");
  // An unpack running in another Gridline.
  const fs::path running = part("Song-0123456789abcdef.part-99-2");
  platform::FileLock holder;
  fs::create_directories(locks, ec);
  expect(holder.tryLock((locks / running.filename()).u8string(), platform::FileLock::Mode::Exclusive), "an unpack holds its lock");
  // Earlier versions took no lock: theirs goes when a day old.
  const fs::path youngOld = part("Song-0123456789abcdef.part-99-3");
  const fs::path oldOld = part("Song-0123456789abcdef.part-99-4");
  fs::last_write_time(oldOld, fs::file_time_type::clock::now() - std::chrono::hours(48));
  // A removal cut short.
  const fs::path trash = part("Song-0123456789abcdef.removing-99-1");
  // Things that are not Gridline's copies.
  const fs::path foreign = f.cache / "not-a-copy";
  writeText(foreign / "a.txt", "keep");
  writeText(f.cache / "stray.txt", "keep");
  const fs::path outside = f.root / "outside";
  writeText(outside / "precious.txt", "keep");
  fs::create_directory_symlink(outside, f.cache / "link-out", ec);
  const bool linked = !ec;
  // A plugin cache beside the unpacked root.
  const fs::path sibling = f.cache.parent_path() / "plasma-media";
  writeText(sibling / "m.bin", "keep");

  auto s = f.sweep();
  expect(!fs::exists(crashed), "a half-unpacked folder goes");
  expect(fs::exists(running), "one still unpacking stays");
  expect(fs::exists(youngOld) && !fs::exists(oldOld), "an earlier version's goes when a day old");
  expect(!fs::exists(trash), "an interrupted removal is finished");
  expect(fs::is_regular_file(foreign / "a.txt") && fs::is_regular_file(f.cache / "stray.txt"), "what is not a copy stays");
  expect(s.keptUnknown >= 1, "and is reported as undecided");
  auto c = f.clear();
  expect(fs::is_regular_file(foreign / "a.txt") && fs::is_regular_file(f.cache / "stray.txt"), "Clear cache too");
  expect(c.keptUnknown >= 1, "and says so");
  if (linked) expect(fs::is_regular_file(outside / "precious.txt") && fs::is_symlink(f.cache / "link-out"), "a link out of the cache is neither followed nor removed");
  expect(fs::is_regular_file(sibling / "m.bin"), "other caches are not touched");
  holder.unlock();
  f.sweep();
  expect(!fs::exists(running), "the unpack that ended without finishing goes");
  // Roots that are no cache.
  auto rr = archive::sweepCache("/", 0, &f.refs);
  expect(rr.removed == 0 && rr.freed == 0 && rr.bytesLeft == 0, "the root of the disk is never swept, nor even measured");
}

// The limit, with copies unpacked for real.
void testLimit(const fs::path& base) {
  Fixture f = fixture(base, "limit");
  std::vector<fs::path> copies;
  const auto hours = [](int n) { return fs::file_time_type::clock::now() - std::chrono::hours(n); };
  remotefs::setTestPrefixes({f.music.u8string()});
  for (int i = 0; i < 3; ++i) {
    const fs::path zip = f.music / ("Z" + std::to_string(i) + ".zip");
    projectZip(zip, f.scratch);
    archive::UnpackResult r = f.open(zip);
    copies.push_back(fs::u8path(r.folder));
    r.use.reset();
    fs::last_write_time(copies.back() / ".gridline-unpacked", hours(30 - i));
  }
  const uint64_t total = archive::cacheBytes(f.cache.u8string());
  archive::sweepCache(f.cache.u8string(), total - 100, &f.refs);
  expect(!fs::exists(copies[0]) && fs::exists(copies[1]) && fs::exists(copies[2]), "the limit trims oldest first");
  remotefs::setTestPrefixes({});
}

void testCancel(const fs::path& base) {
  Fixture f = fixture(base, "cancel");
  const fs::path zip = f.music / "Stop.zip";
  projectZip(zip, f.scratch);
  std::atomic<bool> stop{false};
  archive::UnpackRequest u = f.request(zip);
  u.cancel = &stop;
  u.onEntry = [&](const std::string&, size_t, size_t) { stop = true; };
  archive::UnpackResult r;
  std::string err;
  const bool ok = archive::unpackForOpening(u, r, &err);
  expect(!ok && err == remotefs::kCancelled, "a stopped unpack says so");
  expect(listing(f.cache).empty() && listing(fs::path(f.cache.u8string() + ".locks")).empty(), "and leaves nothing");
}

// Opening while sweeps run: every open gets a whole copy that stays while it is held.
void testRace(const fs::path& base) {
  Fixture f = fixture(base, "race");
  const fs::path zip = f.music / "Race.zip";
  projectZip(zip, f.scratch);
  std::atomic<bool> stop{false};
  std::atomic<int> bad{0}, opens{0};
  std::thread sweeper([&] {
    while (!stop) f.sweep();
  });
  std::vector<std::thread> openers;
  for (int t = 0; t < 3; ++t)
    openers.emplace_back([&] {
      for (int i = 0; i < 25; ++i) {
        archive::UnpackRequest u = f.request(zip);
        archive::UnpackResult r;
        std::string err;
        if (!archive::unpackForOpening(u, r, &err) || !r.use) {
          ++bad;
          continue;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        if (!fs::is_regular_file(fs::u8path(r.folder) / "tone.wav") || !fs::is_regular_file(r.projectFile)) ++bad;
        ++opens;
      }
    });
  for (auto& t : openers) t.join();
  stop = true;
  sweeper.join();
  expect(bad == 0 && opens == 75, "opens during sweeps always find their files (" + std::to_string(bad.load()) + " bad)");
  f.sweep();
  expect(listing(f.cache).empty(), "and nothing is left when all have closed");
}

// One realtime stretch voice per clip that may need one: idle voices must not hold their big scratch.
void testIdleVoices() {
  auto rssNow = [] { return platform::peakRssMb(); };
  std::vector<std::unique_ptr<stretch::Voice>> voices;
  voices.push_back(std::make_unique<stretch::Voice>());
  voices.back()->prepare(2, 44100.0, 8.0, 8.0, false);  // (the shared tables are built once)
  const double before = rssNow();
  int made = 0;
  try {  // (a small virtual machine's memory limit may come first)
    for (; made < 1500; ++made) {
      voices.push_back(std::make_unique<stretch::Voice>());
      voices.back()->prepare(2, 44100.0, 8.0, 8.0, false);
    }
  } catch (const std::bad_alloc&) {
  }
  const double perVoiceKb = (rssNow() - before) * 1024.0 / std::max(made, 1);
  std::printf("# %d idle voices: one holds %.0f KB\n", made, perVoiceKb);
  expect(perVoiceKb < 2500.0, "an idle voice does not hold its big buffers (it held over 4 MB)");
  voices.resize(1);
  // And it works: a tone goes in, sound comes out, all of it finite, at the top of its range too.
  struct Src {
    double ph = 0;
    static void read(void* c, float* const* d, int n) {
      auto* s = static_cast<Src*>(c);
      for (int i = 0; i < n; ++i, s->ph += 0.03) d[0][i] = d[1][i] = static_cast<float>(std::sin(s->ph));
    }
  };
  for (double speed : {1.0, 8.0}) {
    for (double pitch : {1.0, 0.125}) {
      Src src;
      stretch::Voice& v = *voices[0];
      v.reset();
      v.start(&Src::read, &src, speed, pitch);
      std::vector<float> L(4096), R(4096);
      float* out[2] = {L.data(), R.data()};
      double peak = 0;
      bool finite = true;
      for (int b = 0; b < 8; ++b) {
        v.render(out, 4096, speed, pitch, &Src::read, &src);
        for (float x : L) {
          finite = finite && std::isfinite(x);
          peak = std::max(peak, static_cast<double>(std::fabs(x)));
        }
      }
      expect(finite && peak > 0.05, "a voice plays at speed " + std::to_string(speed) + " pitch " + std::to_string(pitch));
    }
  }
}

// A build finds a file missing and queues it, then the editor decodes it on its own thread: the next
// build has to hear of it, or the engine never gets the sample.
void testDecodedOnTheCallersThread(const fs::path& base) {
  fs::create_directories(base / "decoded");
  const std::string wav = wavFile(base / "decoded" / "here.wav");
  sampleCache().pauseBackground(true);
  expect(!sampleCache().request(wav), "a build finds the file missing");
  const uint64_t before = sampleCache().generation();
  expect(sampleCache().get(wav) != nullptr, "the editor decodes it itself");
  expect(sampleCache().generation() != before, "and the next build hears of it");
  sampleCache().pauseBackground(false);
  sampleCache().clear();
}

void testLoader(const fs::path& base) {
  Fixture f = fixture(base, "loader");
  const fs::path zip = f.music / "Tune.zip";
  projectZip(zip, f.scratch);
  const auto before = listing(f.music);
  fs::path copy;
  {
    Loader L;
    projload::Request q;
    q.path = zip.u8string();
    q.unpackCache = f.cache.u8string();
    q.sketch = false;
    L.start(q);
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (L.phase() != Phase::Ready && L.phase() != Phase::Failed && std::chrono::steady_clock::now() < until) {
      L.step(0.004);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    expect(L.phase() == Phase::Ready, "a local archive opens through the loader (" + L.error() + ")");
    expect(L.samplesTotal() == 1, "its sample is found");
    expect(listing(f.music) == before, "its folder is untouched");
    projload::Result& r = L.result();
    expect(r.fromZip && r.inCache && r.copyUse != nullptr, "from the cache, claimed");
    copy = fs::u8path(r.copyUse->folder);
    archive::CopyHold held = std::move(r.copyUse);
    L.clear();
    expect(f.sweep().keptOpen == 1 && fs::exists(copy), "the claim outlives the loader");
    held.reset();
  }
  sampleCache().clear();
  expect(f.sweep().removed == 1 && !fs::exists(copy), "released, the copy goes");
}

}  // namespace

int main(int argc, char** argv) {
  // The second process of testOtherInstance: claims a copy, says so, waits to be killed.
  if (argc == 5 && !std::strcmp(argv[1], "--hold")) {
    archive::CopyHold h = archive::holdCopy(argv[2], argv[3]);
    if (!h) return 3;
    std::ofstream(argv[4]) << "ready";
    for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
  }
  const fs::path base = gl::test::hermeticTestEnv("k45");
  testLocalZip(base);
  testDriveCopy(base);
  testPinnedAndRefs(base);
  testOtherInstance(base);
  testLeftovers(base);
  testLimit(base);
  testCancel(base);
  testRace(base);
  testLoader(base);
  testIdleVoices();
  testDecodedOnTheCallersThread(base);
  return finish();
}
