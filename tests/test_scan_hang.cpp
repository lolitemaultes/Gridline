// K-71: a plugin scan never holds up the interface. Fake scanner children (a shell script that
// answers like `gridline --scan-plugin`, slowly, or hangs) and fake plugin files in temp folders;
// the user's installed plugins are never scanned.
//   1. discovery: a folder with symlink loops, a deep chain and a wide tree is searched once per real
//      folder, quickly, and finds each plugin once (no copies through the loops);
//   2. the scan status the interface reads every frame (scanStatus, cacheEntries) answers at once
//      while a scan runs over a large plugin cache (the cache is never written while the status lock
//      is held, and not once per plugin);
//   3. one hanging plugin does not hold up the others (scanner children run side by side);
//   4. Rescan of a folder returns at once, even for a folder that takes long to search (the search
//      runs on the scan worker), and the scan shows what it is doing meanwhile.
//   5. a plugin stamped at a second boundary keeps its stamp on every read (no needless rescans).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>

#include "Check.h"  // (Windows skips the whole test: no unused harness there)
#endif
#include "Hermetic.h"
#include "core/Platform.h"
#include "host/HostedPlugins.h"
#include "host/PluginFolders.h"
#include "host/PluginPrefs.h"
#include "host/PluginScan.h"

using namespace gl;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

#if !defined(_WIN32)
double since(Clock::time_point t0) { return std::chrono::duration<double>(Clock::now() - t0).count(); }

void touch(const fs::path& p) {
  fs::create_directories(p.parent_path());
  std::ofstream(p) << "x";
}

// A CLAP "plugin" the CLAP format finds (a file on Linux and Windows, a bundle folder on macOS).
void fakeClap(const fs::path& p) {
#if defined(__APPLE__)
  fs::create_directories(p / "Contents");
#else
  touch(p);
#endif
}

// Answers like `gridline --scan-plugin clap <entry> --out <file>`: after `delay` seconds one class;
// an entry whose name holds "hang" sleeps for `hang` seconds and writes nothing (a hung plugin).
std::string fakeScanner(const std::string& dir, const char* delay, const char* hang) {
  const std::string path = dir + "/fake-scanner.sh";
  std::ofstream f(path);
  f << "#!/bin/sh\n"
       "entry=\"$3\"; out=\"$5\"\n"
       "name=$(basename \"$entry\")\n"
       "case \"$name\" in *hang*) exec sleep "
    << hang
    << ";; esac\n"
       "sleep "
    << delay
    << "\n"
       "printf '{\"format\":\"clap\",\"bundle\":\"%s\",\"ok\":true,\"classes\":[{\"uid\":\"k71.%s\",\"name\":\"%s\",\"vendor\":\"Test\","
       "\"params\":[]}]}' \"$entry\" \"$name\" \"$name\" > \"$out\"\n";
  f.close();
  fs::permissions(path, fs::perms::owner_all, fs::perm_options::add);
  return path;
}

// Only CLAP, only the given folders (never the computer's own plugin folders).
void onlyFolders(const std::vector<std::string>& folders) {
  host::PluginPrefs p = host::pluginPrefs();
  p.disabledFormats = {"vst3", "vst2", "au", "lv2"};
  p.noDefaultFolders = {"vst3", "vst2", "clap", "lv2"};
  p.formatFolders.clear();
  p.formatFolders["clap"] = folders;
  p.extraFolders.clear();
  p.disabledFolders.clear();
  host::setPluginPrefs(p);
  platform::setEnv("CLAP_PATH", "");
}

// A folder that takes long to search the naive way: three symlinks back to itself on every level,
// a wide tree beside them, a deep chain. Returns the ".fix" files a search must find.
std::set<std::string> loopTree(const fs::path& root) {
  std::set<std::string> want;
  fs::create_directories(root);
  for (int i = 0; i < 3; ++i) fs::create_directory_symlink(root, root / ("loop" + std::to_string(i)));
  for (int i = 0; i < 4; ++i) fs::create_symlink(root / "gone", root / ("broken" + std::to_string(i)));  // dangling links never end the search
  for (int i = 0; i < 300; ++i) fs::create_directories(root / "wide" / ("w" + std::to_string(i)));
  touch(root / "wide" / "w7" / "seven.fix");
  want.insert((root / "wide" / "w7" / "seven.fix").generic_string());
  fs::path d = root / "real";
  touch(d / "top.fix");
  want.insert((d / "top.fix").generic_string());
  for (int i = 1; i <= 9; ++i) {
    d /= "d" + std::to_string(i);
    fs::create_directories(d);
    fs::create_directory_symlink(root, d / "up");  // a link back to the top from deep inside
    touch(d / ("level" + std::to_string(i) + ".fix"));
    if (i <= 6) want.insert((d / ("level" + std::to_string(i) + ".fix")).generic_string());  // seven levels deep at most
  }
  return want;
}

void testDiscovery(const std::string& dir) {
  const fs::path root = fs::path(dir) / "loops";
  const std::set<std::string> want = loopTree(root);
  const auto t0 = Clock::now();
  const std::vector<std::string> found = host::findEntries({root.string()}, ".fix", false, true);
  const double took = since(t0);
  std::printf("discovery: %zu entries in %.3f s\n", found.size(), took);
  const std::set<std::string> got(found.begin(), found.end());
  expect(got == want, "discovery: every plugin of the tree, once (none through the symlink loops)");
  expect(took < 2.0, "discovery: a tree with symlink loops is searched quickly");
}

// A cache of `n` plugins in `folder`, each up to date (not scanned again), with many parameters.
void writeBigCache(const std::string& cfg, const std::string& folder, int n) {
  host::ScanCache c(cfg + "/plugin-cache.json");
  for (int i = 0; i < n; ++i) {
    char name[64];
    std::snprintf(name, sizeof name, "cached%04d.clap", i);
    const fs::path p = fs::path(folder) / name;
    fakeClap(p);
    host::ScanResult r;
    r.format = "clap";
    r.bundle = p.generic_string();
    r.mtime = host::pathMtime(r.bundle);
    r.ok = true;
    host::ScannedClass k;
    k.uid = std::string("k71.") + name;
    k.name = name;
    k.vendor = "Test vendor with a long name";
    for (int j = 0; j < 40; ++j) {
      host::ScannedParam q;
      q.id = static_cast<uint32_t>(j);
      q.title = "Parameter number " + std::to_string(j) + " of a plugin with many of them";
      q.units = "dB";
      q.def = 0.5;
      k.params.push_back(q);
    }
    r.classes.push_back(k);
    c.put(r);
  }
  c.save();
}

// A plugin stamped right at the start or the end of a second keeps that second on every read: the
// cache compares whole seconds, and a stamp that moved into the next second now and then made the
// scan take a cached plugin as changed and scan it again (one extra plugin in a large collection).
void testStampsStayPut(const std::string& dir) {
  const std::string cfg = dir + "/stamps";
  const time_t sec = 1700000000;
  std::vector<std::string> bundles;
  bool stamped = true;
  for (long ns : {0L, 1L, 999999999L}) {
    const fs::path p = fs::path(cfg) / "plugins" / ("edge" + std::to_string(ns) + ".clap");
    fakeClap(p);
    const timespec ts[2] = {{sec, ns}, {sec, ns}};
    stamped = stamped && ::utimensat(AT_FDCWD, p.c_str(), ts, 0) == 0;
    bundles.push_back(p.generic_string());
  }
  expect(stamped, "stamps: the test plugins are stamped at the second boundaries");
  host::initHosting(cfg);
  host::ScanCache c;
  for (const auto& b : bundles) {
    host::ScanResult r;
    r.format = "clap";
    r.bundle = b;
    r.mtime = host::pathMtime(b);
    r.ok = true;
    c.put(r);
  }
  int wrongStamp = 0, wrongSeconds = 0, rescans = 0;
  for (const auto& b : bundles) {
    const auto t = fs::last_write_time(b);
    for (int i = 0; i < 20000; ++i) {
      wrongSeconds += platform::fileTimeSeconds(t) != static_cast<int64_t>(sec) ? 1 : 0;
      wrongStamp += host::pathMtime(b) != static_cast<int64_t>(sec) ? 1 : 0;
    }
  }
  for (int i = 0; i < 5000; ++i) rescans += static_cast<int>(c.stale(bundles, "clap").size());
  std::printf("stamps: %d file times and %d plugin stamps a second off, %d needless rescans\n", wrongSeconds, wrongStamp, rescans);
  expect(wrongSeconds == 0, "stamps: a file time near a second boundary converts to the same second every time");
  expect(wrongStamp == 0 && rescans == 0, "stamps: a cached plugin stamped near a second boundary is never taken as changed");
}

void testStatusStaysFree(const std::string& dir) {
  const std::string cfg = dir + "/big", folder = dir + "/big/plugins";
  fs::create_directories(folder);
  const int cached = 2000, fresh = 12;
  writeBigCache(cfg, folder, cached);
  std::error_code ec;
  std::printf("status: cache of %d plugins, %.1f MB\n", cached, static_cast<double>(fs::file_size(cfg + "/plugin-cache.json", ec)) / 1e6);
  for (int i = 0; i < fresh; ++i) fakeClap(fs::path(folder) / ("new" + std::to_string(i) + ".clap"));
  host::initHosting(cfg);
  onlyFolders({folder});
  const std::string exe = fakeScanner(dir + "/big", "0.15", "1");

  const auto t0 = Clock::now();
  host::startScan(exe, false);
  double worst = 0;
  int frames = 0, slow = 0;
  bool sawRunning = false;
  for (;;) {
    const auto f0 = Clock::now();
    const host::ScanStatus st = host::scanStatus();  // what a frame of the interface reads
    const auto entries = host::cacheEntries();
    const double t = since(f0);
    worst = std::max(worst, t);
    slow += t >= 0.025 ? 1 : 0;
    ++frames;
    sawRunning = sawRunning || st.running;
    if (!st.running || since(t0) > 60) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  host::stopScan();
  const double wall = since(t0);
  const host::ScanStatus st = host::scanStatus();
  std::printf("status: %d frames, longest status read %.1f ms, %d of 25 ms or more, scan of %d took %.2f s\n", frames, worst * 1e3, slow, st.total, wall);
  expect(sawRunning && st.total == fresh && st.done == fresh, "status: only the new plugins are scanned");
  // Waiting for the cache to be written would hold up a read for each of the 12 plugins (writing
  // 12 MB takes that long); a busy or virtual machine may hold up one or two reads by itself.
  expect(slow <= 2 && worst < 0.1, "status: the interface's status read never waits for the cache to be written");
  int ok = 0;
  for (const auto& e : host::cacheEntries()) ok += e.ok ? 1 : 0;
  std::printf("status: %d of %zu cached plugins ok\n", ok, host::cacheEntries().size());
  expect(ok == cached + fresh, "status: every plugin is in the cache afterwards");
  host::ScanCache disk(cfg + "/plugin-cache.json");
  expect(disk.load() && static_cast<int>(disk.entries().size()) == cached + fresh, "status: the cache file holds every plugin at the end");
}

void testHangingPlugin(const std::string& dir) {
  const std::string cfg = dir + "/hang", folder = dir + "/hang/plugins";
  fs::create_directories(cfg);
  fakeClap(fs::path(folder) / "aaa-hang.clap");  // first in line
  for (int i = 0; i < 5; ++i) fakeClap(fs::path(folder) / ("ok" + std::to_string(i) + ".clap"));
  host::initHosting(cfg);
  onlyFolders({folder});
  const std::string exe = fakeScanner(dir + "/hang", "0.3", "8");
  const auto t0 = Clock::now();
  host::startScan(exe, false);
  bool othersDone = false;
  host::ScanStatus st;
  while (since(t0) < 5 && !othersDone) {
    st = host::scanStatus();
    othersDone = st.running && st.done == 5;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  std::printf("hang: %d of %d done after %.2f s, current %s\n", st.done, st.total, since(t0), st.current.c_str());
  expect(othersDone, "hang: the other plugins finish while one plugin hangs");
  expect(st.current.find("aaa-hang") != std::string::npos, "hang: the status names the plugin that holds the scan up");
  host::stopScan();
  int failed = 0, ok = 0;
  for (const auto& e : host::cacheEntries()) {
    (e.ok ? ok : failed) += 1;
    if (!e.ok) std::printf("hang: failed %s: %s\n", e.entry.c_str(), e.error.c_str());
  }
  expect(ok == 5 && failed == 1, "hang: the hung plugin is blacklisted, the others are listed");
}

void testRescanFolder(const std::string& dir) {
  const std::string cfg = dir + "/rescan";
  const fs::path root = fs::path(dir) / "rescan" / "slow";
  loopTree(root);
  fakeClap(root / "real" / "d1" / "found.clap");
  host::initHosting(cfg);
  onlyFolders({root.string()});
  const std::string exe = fakeScanner(dir + "/rescan", "0.05", "1");
  auto t0 = Clock::now();
  host::rescanFolder(exe, "clap", root.string());
  const double took = since(t0);
  std::printf("rescan folder: the call took %.3f s\n", took);
  expect(took < 0.05, "rescan folder: returns at once (the folder is searched on the scan worker)");
  bool shown = host::scanStatus().running;
  for (int i = 0; i < 1000 && host::scanStatus().running; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  const host::ScanStatus st = host::scanStatus();
  expect(shown && !st.running && st.total == 1 && st.done == 1, "rescan folder: its one plugin is scanned");
  host::shutdownScan();
}
#endif

}  // namespace

int main() {
#if defined(_WIN32)
  std::printf("1..0 # skipped: shell script scanners (POSIX only)\n");
  return 0;
#else
  const std::string dir = gl::test::hermeticTestEnv("scan-hang");
  testDiscovery(dir);
  testStampsStayPut(dir);
  testStatusStaysFree(dir);
  testHangingPlugin(dir);
  testRescanFolder(dir);
  return finish();
#endif
}
