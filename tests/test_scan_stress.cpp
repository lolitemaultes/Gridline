// K-73: plugin scans of a big collection (a report: Windows 10, 1836 plugins in VST2, VST3 and CLAP).
// Fake plugin files in temp folders and a stand-in scanner child (fake_plugin_scanner, argv[1]); the
// computer's own plugins are never scanned. Runs on every system (the scanner is a program, not a script).
//   1. a shell (a plugin file holding many plugins) that takes longer than the timeout is still scanned,
//      since it shows progress; a hung plugin is given up after the timeout;
//   2. VST3 folders are searched like the others: each real folder once (symlink loops), names in any
//      case, and the search stops on quit;
//   3. Windows DLLs that cannot be plugins (32-bit in a 64-bit Gridline, the DLLs plugins need) are told
//      without loading them (the PE check, tested on built files everywhere);
//   4. a first scan of 1836 plugins across the three formats ends by itself, with the hung, crashed and
//      broken ones blacklisted, while what the interface reads every frame stays quick.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "Check.h"
#include "Hermetic.h"
#include "core/Platform.h"
#include "host/HostedPlugins.h"
#include "host/PluginGroups.h"
#include "host/PluginPrefs.h"
#include "host/PluginScan.h"
#include "host/vst3/Vst3Module.h"

using namespace gl;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

std::string gScanner;

double since(Clock::time_point t0) { return std::chrono::duration<double>(Clock::now() - t0).count(); }

void touch(const fs::path& p) {
  fs::create_directories(p.parent_path());
  std::ofstream(p) << "fake";
}

#if defined(_WIN32)
// A minimal 64-bit DLL exporting `entry` (writePe, below).
void writeDll(const fs::path& p, const char* entry);
#endif

// A plugin of `format` as its search finds it: a bundle folder (VST3 everywhere; CLAP and VST2 on
// macOS) or a file.
fs::path fakePlugin(const fs::path& dir, const std::string& name, const std::string& format) {
#if defined(__APPLE__)
  const bool bundle = true;
  const std::string vst2 = ".vst";
#elif defined(_WIN32)
  const bool bundle = format == "vst3";
  const std::string vst2 = ".dll";
#else
  const bool bundle = format == "vst3";
  const std::string vst2 = ".so";
#endif
  const fs::path p = dir / (name + (format == "vst3" ? ".vst3" : format == "clap" ? ".clap" : vst2));
  if (bundle) fs::create_directories(p / "Contents");
#if defined(_WIN32)
  else writeDll(p, format == "vst2" ? "VSTPluginMain" : "clap_entry");  // passes the DLL check, then the stand-in scanner answers
#else
  else touch(p);
#endif
  return p;
}

// ---- 1. shells and hung plugins
void testShellAndHang(const std::string& dir) {
  const fs::path d = fs::path(dir) / "shell";
  const fs::path shell = fakePlugin(d, "bigshell", "clap"), hang = fakePlugin(d, "hangs", "clap");
  auto t0 = Clock::now();
  const host::ScanResult r = host::scanIsolated(gScanner, "clap", shell.generic_string(), 2.0);
  const double took = since(t0);
  std::printf("shell: ok %d, %zu plugins in %.1f s (timeout 2 s), error '%s'\n", r.ok ? 1 : 0, r.classes.size(), took, r.error.c_str());
  expect(r.ok && r.classes.size() == 120, "shell: a plugin file of 120 plugins that takes longer than the timeout is scanned (it shows progress)");
  expect(took > 2.0, "shell: (the shell did take longer than the timeout)");
  t0 = Clock::now();
  const host::ScanResult h = host::scanIsolated(gScanner, "clap", hang.generic_string(), 2.0);
  const double hungFor = since(t0);
  std::printf("hang: ok %d after %.1f s, error '%s'\n", h.ok ? 1 : 0, hungFor, h.error.c_str());
  expect(!h.ok && h.error.find("timed out") != std::string::npos && hungFor < 6, "hang: a plugin that never answers is given up after the timeout");
  std::error_code ec;
  int left = 0;
  for (fs::directory_iterator it(fs::temp_directory_path(ec), ec), end; !ec && it != end; it.increment(ec))
    left += it->path().filename().string().rfind("gridline-plugin-scan-", 0) == 0 ? 1 : 0;
  expect(left == 0, "shell: the scanner's result and progress files are removed");
}

// ---- 2. VST3 discovery
void testVst3Search(const std::string& dir) {
  const fs::path root = fs::path(dir) / "vst3";
  std::set<std::string> want;
  fs::create_directories(root);
  for (int i = 0; i < 300; ++i) fs::create_directories(root / "wide" / ("w" + std::to_string(i)));
  for (int i = 0; i < 40; ++i) want.insert(fakePlugin(root / "Vendor" / ("v" + std::to_string(i % 5)), "plug" + std::to_string(i), "vst3").generic_string());
  const fs::path upper = root / "Old" / "SHOUT.VST3";  // names in any case
  fs::create_directories(upper / "Contents");
  want.insert(upper.generic_string());
  bool links = true;
#if !defined(_WIN32)
  // Links back up on every level (a naive search follows them until its depth limit, 3^7 times).
  std::error_code ec;
  fs::path deep = root / "Vendor";
  for (int lvl = 0; lvl < 6 && links; ++lvl) {
    for (int k = 0; k < 3 && links; ++k) {
      fs::create_directory_symlink(root, deep / ("up" + std::to_string(k)), ec);
      links = !ec;
    }
    deep /= "d";
    fs::create_directories(deep);
  }
#else
  links = false;
#endif
  auto t0 = Clock::now();
  const std::vector<std::string> found = vst3::findBundles({root.string()});
  const double took = since(t0);
  const std::set<std::string> got(found.begin(), found.end());
  std::printf("vst3 search: %zu found (%zu wanted) in %.3f s, links %d\n", found.size(), want.size(), took, links ? 1 : 0);
  expect(got == want && found.size() == want.size(), "vst3 search: every bundle once, in any letter case (none through the links back up)");
  expect(took < 2.0, "vst3 search: a folder with links back up is searched quickly");
  // A quit stops the search.
  std::atomic<bool> cancel{true};
  {
    host::DiscoveryScope scope(&cancel, nullptr);
    t0 = Clock::now();
    const auto cut = vst3::findBundles({root.string()});
    expect(cut.size() < want.size(), "vst3 search: stops at once on quit");
  }
}

// ---- 3. the PE check
struct Pe {
  uint16_t machine = 0x8664;
  std::vector<std::string> exports;
  bool exportTable = true;
};
void put16(std::vector<unsigned char>& b, size_t at, uint32_t v) {
  b[at] = static_cast<unsigned char>(v & 0xFF);
  b[at + 1] = static_cast<unsigned char>((v >> 8) & 0xFF);
}
void put32(std::vector<unsigned char>& b, size_t at, uint32_t v) {
  for (int k = 0; k < 4; ++k) b[at + static_cast<size_t>(k)] = static_cast<unsigned char>((v >> (8 * k)) & 0xFF);
}
// A minimal DLL image: headers, one section at RVA 0x1000 (file 0x400) with the export directory.
fs::path writePe(const fs::path& p, const Pe& pe) {
  std::vector<unsigned char> b(0x1000, 0);
  b[0] = 'M';
  b[1] = 'Z';
  put32(b, 0x3C, 0x80);
  b[0x80] = 'P';
  b[0x81] = 'E';
  const bool plus = pe.machine != 0x14C;
  const uint32_t optSize = plus ? 240 : 224;
  put16(b, 0x84, pe.machine);
  put16(b, 0x86, 1);
  put16(b, 0x94, optSize);
  put16(b, 0x96, 0x2022);
  const size_t opt = 0x98;
  put16(b, opt, plus ? 0x20B : 0x10B);
  put32(b, opt + (plus ? 108 : 92), 16);
  if (pe.exportTable) {
    put32(b, opt + (plus ? 112 : 96), 0x1000);
    put32(b, opt + (plus ? 116 : 100), 0x200);
  }
  const size_t sec = opt + optSize;
  std::memcpy(&b[sec], ".edata", 6);
  put32(b, sec + 8, 0x400);
  put32(b, sec + 12, 0x1000);
  put32(b, sec + 16, 0x400);
  put32(b, sec + 20, 0x400);
  // Export directory at file 0x400: names at RVA 0x1100, name strings from RVA 0x1200.
  put32(b, 0x400 + 24, static_cast<uint32_t>(pe.exports.size()));
  put32(b, 0x400 + 32, 0x1100);
  uint32_t str = 0x1200;
  for (size_t i = 0; i < pe.exports.size(); ++i) {
    put32(b, 0x500 + 4 * i, str);
    std::memcpy(&b[0x400 + (str - 0x1000)], pe.exports[i].c_str(), pe.exports[i].size() + 1);
    str += static_cast<uint32_t>(pe.exports[i].size() + 1);
  }
  fs::create_directories(p.parent_path());
  std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
  return p;
}

#if defined(_WIN32)
void writeDll(const fs::path& p, const char* entry) { writePe(p, Pe{0x8664, {entry}}); }
#endif

void testPeCheck(const std::string& dir) {
  const fs::path d = fs::path(dir) / "pe";
  const std::vector<std::string> vst2 = {"VSTPluginMain", "main"};
  auto check = [&](const char* name, const Pe& pe, uint16_t host = 0x8664) { return host::peDllPrecheck(writePe(d / name, pe).string(), vst2, host); };
  expect(check("ok.dll", Pe{0x8664, {"DllMain", "VSTPluginMain"}}).empty(), "pe: a 64-bit VST 2 plugin may be a plugin");
  expect(check("old.dll", Pe{0x8664, {"main"}}).empty(), "pe: an old one exporting main too");
  const std::string x86 = check("x86.dll", Pe{0x14C, {"VSTPluginMain"}});
  std::printf("pe: 32-bit: '%s'\n", x86.c_str());
  expect(x86.find("32-bit") != std::string::npos, "pe: a 32-bit plugin in a 64-bit Gridline is told without loading it");
  expect(check("x86host.dll", Pe{0x14C, {"VSTPluginMain"}}, 0x14C).empty(), "pe: (a 32-bit plugin suits a 32-bit Gridline)");
  const std::string dep = check("dep.dll", Pe{0x8664, {"qInit", "qRun", "qStop"}});
  std::printf("pe: dependency: '%s'\n", dep.c_str());
  expect(dep.find("not a plugin") != std::string::npos, "pe: a DLL a plugin needs (no plugin entry point) is not loaded");
  expect(check("packed.dll", Pe{0x8664, {}, false}).empty(), "pe: a DLL without a readable export table is left to the scan (packed plugins)");
  expect(check("arm.dll", Pe{0xAA64, {"VSTPluginMain"}}).empty(), "pe: other 64-bit kinds are left to the scan (ARM64X)");
  touch(d / "text.dll");
  expect(host::peDllPrecheck((d / "text.dll").string(), vst2, 0x8664).find("not a Windows DLL") != std::string::npos, "pe: a file that is not a DLL");
  expect(host::peDllPrecheck((d / "missing.dll").string(), vst2, 0x8664).empty(), "pe: an unreadable file is left to the scan");
  std::vector<unsigned char> cut(0x90, 0);
  cut[0] = 'M', cut[1] = 'Z', cut[0x3C] = 0x80, cut[0x80] = 'P', cut[0x81] = 'E';
  std::ofstream(d / "cut.dll", std::ios::binary).write(reinterpret_cast<const char*>(cut.data()), static_cast<std::streamsize>(cut.size()));
  expect(!host::peDllPrecheck((d / "cut.dll").string(), vst2, 0x8664).empty(), "pe: a cut-off DLL is not read past its end");
}

// ---- 4. a collection of 1836
void onlyFolders(const std::map<std::string, std::vector<std::string>>& folders) {
  host::PluginPrefs p = host::pluginPrefs();
  p.disabledFormats = {"au", "lv2"};
  p.noDefaultFolders = {"vst3", "vst2", "clap", "lv2"};
  p.formatFolders.clear();
  for (const auto& [f, v] : folders) p.formatFolders[f] = v;
  p.extraFolders.clear();
  p.disabledFolders.clear();
  host::setPluginPrefs(p);
  for (const char* v : {"VST_PATH", "VST3_PATH", "CLAP_PATH", "LV2_PATH"}) platform::setEnv(v, "");
}

void testCollection(const std::string& dir) {
  const fs::path root = fs::path(dir) / "collection";
  int failing = 0;
  const std::pair<const char*, int> formats[] = {{"vst3", 700}, {"vst2", 600}, {"clap", 536}};
  for (const auto& [fmt, n] : formats)
    for (int i = 0; i < n; ++i) {
      const int k = i % 97;
      std::string name = k == 5 ? "hang" : k == 11 ? "crash" : k == 17 ? "slow" : (k == 29 || k == 31 || k == 37) ? "junk" : "plug";
      failing += (k == 5 || k == 11 || k == 29 || k == 31 || k == 37) ? 1 : 0;
      if (std::string(fmt) == "vst2" && (i == 100 || i == 300)) name = "shell";
      fakePlugin(root / fmt / ("vendor" + std::to_string(i % 23)), name + std::to_string(i), fmt);
    }
  host::initHosting(dir + "/collection-config");
  onlyFolders({{"vst3", {(root / "vst3").string()}}, {"vst2", {(root / "vst2").string()}}, {"clap", {(root / "clap").string()}}});
  platform::setEnv("GRIDLINE_SCAN_TIMEOUT", "3");
  const auto t0 = Clock::now();
  host::startScan(gScanner, false);
  double worst = 0;
  int frames = 0, slow = 0;
  for (;;) {
    const auto f0 = Clock::now();
    // What a frame of the interface reads (status line, plugin manager, browser).
    const host::ScanStatus st = host::scanStatus();
    const auto entries = host::cacheEntries();
    const auto groups = host::pluginGroups();
    const double t = since(f0);
    worst = std::max(worst, t);
    slow += t >= 0.25 ? 1 : 0;
    ++frames;
    if (!st.running || since(t0) > 400) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(8));
  }
  host::stopScan();
  const double wall = since(t0);
  const host::ScanStatus st = host::scanStatus();
  std::printf("collection: %d of %d scanned in %.1f s, %d failed (%d expected), %d plugins, %d frames, longest read %.1f ms, %d of 250 ms or more\n", st.done,
              st.total, wall, st.failed, failing, st.classes, frames, worst * 1e3, slow);
  expect(!st.running && st.total == 1836 && st.done == 1836, "collection: the first scan of 1836 plugins ends by itself");
  expect(st.failed == failing, "collection: the hung, crashed and broken plugins are blacklisted, no others");
  expect(st.classes == 1836 - failing - 2 + 240, "collection: every plugin is listed, the shells' 120 each included");
  // A busy or virtual machine may hold up one or two reads by themselves; the interface waiting for
  // the scan holds up many, or for seconds (the scan timeout is 3 s).
  expect(slow <= 2 && worst < 1.0, "collection: what the interface reads every frame stays quick during the scan");
  host::ScanCache disk(dir + "/collection-config/plugin-cache.json");
  expect(disk.load() && disk.entries().size() == 1836, "collection: the cache file holds every plugin at the end");
  // An unchanged rescan scans nothing and is quick.
  const auto t1 = Clock::now();
  host::startScan(gScanner, false);
  while (host::scanStatus().running && since(t1) < 60) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  const host::ScanStatus again = host::scanStatus();
  std::printf("collection: unchanged rescan: %d scanned in %.2f s\n", again.total, since(t1));
  expect(!again.running && again.total == 0 && since(t1) < 20, "collection: an unchanged rescan scans nothing");
  host::shutdownScan();
}

}  // namespace

int main(int argc, char** argv) {
  const std::string dir = gl::test::hermeticTestEnv("scan-stress");
  if (argc < 2) {
    std::printf("usage: gridline_tests_scan_stress <fake_plugin_scanner>\n");
    return 2;
  }
  gScanner = argv[1];
  testPeCheck(dir);
  testVst3Search(dir);
  testShellAndHang(dir);
  testCollection(dir);
  return finish();
}
