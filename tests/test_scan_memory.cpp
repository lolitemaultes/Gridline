// K-154: a plugin scan never runs the computer out of memory. A plugin that loops while it loads (a
// bridge's host process stuck in a loop, a plugin that allocates without end) took all the RAM in a
// few seconds: the scanner child had no memory limit, only a timeout, and helpers it started
// outlived it. This test is its own fake scanner child (`--scan-plugin clap <entry> --out <file>`):
// the entry's name says how it misbehaves. Only fake plugin files in temp folders are scanned.
//   1. the memory rules: the cap scales with the computer's memory (a quarter, 2 to 8 GB), the reserve
//      (a tenth, 512 MB to 2 GB), the failure text;
//   2. Process::treeMemoryBytes counts the child and what it started (process group / job), and
//      kill() ends both;
//   3. a scan that allocates without end is stopped at the cap, quickly, with "used too much memory
//      while loading";
//   4. a scan whose helper process allocates (the child itself small) is stopped too, and the helper
//      is ended with it;
//   5. a helper left running by a plugin that scanned fine is ended when the scan ends;
//   6. a scan that holds memory while the computer runs low is stopped ("running out of memory");
//   7. in the scan service: the plugin is listed as failed with that reason, skipped by later scans,
//      and scanned again when retried by hand.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <signal.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include "Check.h"
#include "Hermetic.h"
#include "core/Platform.h"
#include "host/HostedPlugins.h"
#include "host/PluginPrefs.h"
#include "host/PluginScan.h"

using namespace gl;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

constexpr std::uint64_t kMB = 1ull << 20;
double since(Clock::time_point t0) { return std::chrono::duration<double>(Clock::now() - t0).count(); }

// ---------------------------------------------------------------- the fake child processes

// Touched memory, so it counts as resident.
std::vector<char*>& kept() {
  static std::vector<char*> v;
  return v;
}
// In 50 MB pieces, a little at a time (a growing plugin: the watcher sees it pass the cap), filled
// with noise so a memory compressor (macOS) cannot shrink it.
void allocate(std::uint64_t bytes) {
  std::uint64_t x = 88172645463325252ull;
  for (std::uint64_t done = 0; done < bytes; done += 50 * kMB) {
    const size_t n = static_cast<size_t>(std::min<std::uint64_t>(50 * kMB, bytes - done));
    auto* p = static_cast<std::uint64_t*>(std::malloc(n));
    if (!p) return;  // (Windows: the job's limit)
    for (size_t i = 0; i < n / 8; ++i) {
      x ^= x << 13, x ^= x >> 7, x ^= x << 17;
      p[i] = x;
    }
    kept().push_back(reinterpret_cast<char*>(p));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
}
void writePid(const std::string& file) { std::ofstream(file) << platform::processId(); }
// Never forever: a helper left behind by a failing check ends by itself.
[[noreturn]] void sleepThenExit(double seconds) {
  std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
  std::_Exit(0);
}

// Starts this program again as a helper (`--k154-alloc <MB> <pidfile>`), outside any new group: it
// stays in the caller's process group / job, like a bridge's host process.
void startHelper(int mb, const std::string& pidFile) {
  platform::Process p;
  p.start(platform::executablePath(), {platform::executablePath(), "--k154-alloc", std::to_string(mb), pidFile});
}

void writeResult(const std::string& out, const std::string& entry, bool ok) {
  const std::string name = fs::path(entry).filename().string();
  std::ofstream(out) << "{\"format\":\"clap\",\"bundle\":\"" << fs::path(entry).generic_string() << "\",\"ok\":" << (ok ? "true" : "false")
                     << ",\"classes\":[{\"uid\":\"k154." << name << "\",\"name\":\"" << name << "\",\"vendor\":\"Test\",\"params\":[]}]}";
}

int fakeScanner(int argc, char** argv) {
  std::string entry, out;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--scan-plugin") == 0 && i + 2 < argc) entry = argv[i + 2];
    if (std::strcmp(argv[i], "--out") == 0 && i + 1 < argc) out = argv[i + 1];
  }
  const std::string name = fs::path(entry).filename().string();
  if (const char* d = std::getenv("K154_DIR"); d && *d) std::ofstream(std::string(d) + "/starts-" + name + ".txt", std::ios::app) << "+";
  const std::string dir = fs::path(out).parent_path().string();
  if (name.find("leak") != std::string::npos) {  // 50 MB every 50 ms (up to 2 GB, then waits)
    for (int i = 0; i < 40; ++i) {
      allocate(50 * kMB);
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    sleepThenExit(60);
  }
  if (name.find("orphan") != std::string::npos) {  // a helper takes the memory, the child hangs
    startHelper(600, std::getenv("K154_DIR") ? std::string(std::getenv("K154_DIR")) + "/orphan.pid" : dir + "/orphan.pid");
    sleepThenExit(60);
  }
  if (name.find("lowmem") != std::string::npos) {  // holds some memory, then waits
    allocate(350 * kMB);
    sleepThenExit(60);
  }
  if (name.find("helper") != std::string::npos) {  // scans fine but leaves a helper running
    startHelper(1, std::getenv("K154_DIR") ? std::string(std::getenv("K154_DIR")) + "/helper.pid" : dir + "/helper.pid");
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
  }
  writeResult(out, entry, true);
  return 0;
}

// ---------------------------------------------------------------- checks

int readPid(const std::string& file, double waitSeconds) {
  const auto t0 = Clock::now();
  for (;;) {
    std::ifstream f(file);
    int pid = 0;
    if (f >> pid && pid > 0) return pid;
    if (since(t0) > waitSeconds) return 0;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
}

bool alive(int pid) {
#if defined(_WIN32)
  HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
  if (!h) return false;
  const bool running = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
  CloseHandle(h);
  return running;
#else
  return ::kill(static_cast<pid_t>(pid), 0) == 0;
#endif
}

// A killed process that was reparented is reaped by the system shortly after.
bool goneSoon(int pid) {
  for (int i = 0; i < 200 && alive(pid); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  return !alive(pid);
}

void killPid(int pid) {
  if (pid <= 0) return;
#if defined(_WIN32)
  if (HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid))) {
    TerminateProcess(h, 1);
    CloseHandle(h);
  }
#else
  ::kill(static_cast<pid_t>(pid), SIGKILL);
#endif
}

void testRules() {
  constexpr std::uint64_t GB = 1ull << 30;
  platform::unsetEnv("GRIDLINE_SCAN_MEMORY_MB");
  platform::unsetEnv("GRIDLINE_SCAN_RESERVE_MB");
  expect(host::scanMemoryCap(4 * GB) == 2 * GB && host::scanMemoryCap(8 * GB) == 2 * GB, "rules: 8 GB of memory or less: 2 GB per plugin");
  expect(host::scanMemoryCap(16 * GB) == 4 * GB && host::scanMemoryCap(32 * GB) == 8 * GB, "rules: a quarter of the memory (16 GB: 4 GB, 32 GB: 8 GB)");
  expect(host::scanMemoryCap(128 * GB) == 8 * GB, "rules: 8 GB per plugin at most");
  expect(host::scanMemoryCap(0) == 4 * GB, "rules: 4 GB when the memory size is unknown");
  expect(host::scanMemoryReserve(4 * GB) == 512 * kMB && host::scanMemoryReserve(16 * GB) == 16 * GB / 10 &&
             host::scanMemoryReserve(64 * GB) == 2 * GB,
         "rules: the reserve is a tenth of the memory, 512 MB to 2 GB");
  expect(host::scanMemoryLow(600 * kMB, 1 * GB, 32 * GB) && !host::scanMemoryLow(600 * kMB, 8 * GB, 32 * GB),
         "rules: a scan holding 512 MB or more is stopped while less than the reserve is free");
  expect(!host::scanMemoryLow(400 * kMB, 100 * kMB, 32 * GB), "rules: a small scan is never blamed for the computer's low memory");
  expect(!host::scanMemoryLow(3 * GB, 0, 32 * GB), "rules: unknown free memory: the cap alone decides");
  expect(host::scanMemoryError(4 * GB, false) == "used too much memory while loading (more than 4.0 GB)", "rules: the failure text");
  expect(host::scanMemoryError(GB, true).rfind("used too much memory while loading (", 0) == 0 &&
             host::scanMemoryError(GB, true).find("running out of memory") != std::string::npos,
         "rules: the low-memory failure text");
  platform::setEnv("GRIDLINE_SCAN_MEMORY_MB", "300");
  expect(host::scanMemoryCap(32 * GB) == 300 * kMB, "rules: GRIDLINE_SCAN_MEMORY_MB sets the cap (tests)");
  platform::unsetEnv("GRIDLINE_SCAN_MEMORY_MB");
  const std::uint64_t phys = platform::physicalMemoryBytes(), avail = platform::availableMemoryBytes();
  std::printf("rules: this computer has %.1f GB, %.1f GB free: cap %.1f GB, reserve %.1f GB\n", static_cast<double>(phys) / GB,
              static_cast<double>(avail) / GB, static_cast<double>(host::scanMemoryCap(phys)) / GB,
              static_cast<double>(host::scanMemoryReserve(phys)) / GB);
  expect(phys >= GB / 2, "rules: the computer's memory size is known");
#if defined(__linux__) || defined(__APPLE__) || defined(_WIN32) || defined(__FreeBSD__)
  expect(avail > 0 && avail <= phys, "rules: the free memory is known and plausible");
#endif
}

void testTreeMemory(const std::string& dir) {
  const std::string pidFile = dir + "/tree-helper.pid";
  platform::Process p;
  // The child takes 200 MB and starts a helper that takes 300 MB more.
  const bool started = p.start(platform::executablePath(), {platform::executablePath(), "--k154-tree", pidFile}, platform::Process::kTree);
  expect(started, "tree: the child starts");
  const int helper = readPid(pidFile, 20);
  expect(helper > 0, "tree: the child starts its helper");
  std::uint64_t most = 0;
  const auto t0 = Clock::now();
  while (since(t0) < 20 && most < 450 * kMB) {
    most = std::max(most, p.treeMemoryBytes());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  std::printf("tree: %.0f MB counted for the child and its helper\n", static_cast<double>(most) / kMB);
  expect(most >= 450 * kMB && most < 2048 * kMB, "tree: the memory of the child and its helper is counted together");
  p.kill();
  p.wait(10);
  expect(goneSoon(helper), "tree: killing the child ends its helper too");
  killPid(helper);
  expect(p.treeMemoryBytes() == 0, "tree: nothing is counted once the child has ended");
}

host::ScanResult scan(const std::string& dir, const std::string& name, double* took) {
  const fs::path entry = fs::path(dir) / name;
  std::ofstream(entry) << "x";
  const auto t0 = Clock::now();
  host::ScanResult r = host::scanIsolated(platform::executablePath(), "clap", entry.string(), 60);
  *took = since(t0);
  std::printf("%s: %s after %.2f s\n", name.c_str(), r.ok ? "ok" : r.error.c_str(), *took);
  return r;
}

void testScanIsolated(const std::string& dir) {
  platform::setEnv("K154_DIR", dir.c_str());
  platform::setEnv("GRIDLINE_SCAN_MEMORY_MB", "300");
  double took = 0;

  host::ScanResult r = scan(dir, "leak.clap", &took);
  expect(!r.ok && r.error == "used too much memory while loading (more than 0.3 GB)", "leak: stopped with \"used too much memory while loading\"");
  expect(took < 15, "leak: stopped soon after it passed the cap (not at the 60 s timeout)");

  r = scan(dir, "orphan.clap", &took);
  const int orphan = readPid(dir + "/orphan.pid", 1);
  expect(!r.ok && r.error.rfind("used too much memory while loading", 0) == 0, "orphan: a helper's memory counts: stopped");
  expect(took < 20, "orphan: stopped soon (not at the timeout)");
  expect(orphan > 0 && goneSoon(orphan), "orphan: the helper is ended with the scan (never left running)");
  killPid(orphan);

  r = scan(dir, "helper.clap", &took);
  const int helper = readPid(dir + "/helper.pid", 1);
  expect(r.ok, "helper: a plugin that scans fine is listed");
  expect(helper > 0 && goneSoon(helper), "helper: the helper it left running is ended when the scan ends");
  killPid(helper);

  platform::setEnv("GRIDLINE_SCAN_MEMORY_MB", "1200");
  platform::setEnv("GRIDLINE_SCAN_RESERVE_MB", "100000000");  // as if the computer had no memory to spare
  r = scan(dir, "lowmem.clap", &took);
  if (platform::availableMemoryBytes() > 0) {
    expect(!r.ok && r.error.find("running out of memory") != std::string::npos, "low memory: a scan holding memory is stopped");
    expect(took < 15, "low memory: stopped soon");
  }
  r = scan(dir, "fine.clap", &took);
  expect(r.ok, "low memory: a plugin that takes little memory still scans");
  platform::unsetEnv("GRIDLINE_SCAN_RESERVE_MB");
  platform::setEnv("GRIDLINE_SCAN_MEMORY_MB", "300");
}

int starts(const std::string& dir, const std::string& name) {
  std::ifstream f(dir + "/starts-" + name + ".txt");
  std::string s;
  f >> s;
  return static_cast<int>(s.size());
}

// The scan service: the failed plugin is listed with its reason, skipped later, retried by hand.
void testService(const std::string& dir) {
  const std::string cfg = dir + "/service", folder = dir + "/service/plugins";
  fs::create_directories(folder);
#if defined(__APPLE__)
  const auto fakeClap = [](const fs::path& p) { fs::create_directories(p / "Contents"); };
#else
  const auto fakeClap = [](const fs::path& p) { std::ofstream(p) << "x"; };
#endif
  const fs::path leak = fs::path(folder) / "svc-leak.clap";
  fakeClap(leak);
  fakeClap(fs::path(folder) / "svc-ok.clap");
  host::initHosting(cfg);
  host::PluginPrefs p = host::pluginPrefs();
  p.disabledFormats = {"vst3", "vst2", "au", "lv2"};
  p.noDefaultFolders = {"vst3", "vst2", "clap", "lv2"};
  p.formatFolders.clear();
  p.formatFolders["clap"] = {folder};
  p.extraFolders.clear();
  p.disabledFolders.clear();
  host::setPluginPrefs(p);
  platform::setEnv("CLAP_PATH", "");
  const std::string exe = platform::executablePath();
  auto run = [&](auto&& start) {
    start();
    for (int i = 0; i < 3000 && !host::scanStatus().running; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    host::stopScan();
  };
  run([&] { host::startScan(exe, false); });
  std::string why;
  for (const auto& [b, w] : host::blacklist())
    if (b.find("svc-leak") != std::string::npos) why = w;
  int ok = 0;
  for (const auto& e : host::cacheEntries()) ok += e.ok ? 1 : 0;
  std::printf("service: svc-leak failed: \"%s\", %d ok\n", why.c_str(), ok);
  expect(why.rfind("used too much memory while loading", 0) == 0, "service: the plugin is listed as failed, with the reason");
  expect(ok == 1, "service: the other plugin is listed");
  expect(starts(dir, "svc-leak.clap") == 1, "service: scanned once");
  run([&] { host::startScan(exe, false); });
  expect(starts(dir, "svc-leak.clap") == 1, "service: a later scan skips it like other broken plugins");
  run([&] { host::rescanEntry(exe, "clap", leak.string()); });
  expect(starts(dir, "svc-leak.clap") == 2, "service: a retry by hand scans it again");
  host::shutdownScan();
}

}  // namespace

int main(int argc, char** argv) {
  // The fake child processes.
  if (argc >= 2 && std::strcmp(argv[1], "--scan-plugin") == 0) return fakeScanner(argc, argv);
  if (argc >= 4 && std::strcmp(argv[1], "--k154-alloc") == 0) {
    writePid(argv[3]);
    allocate(static_cast<std::uint64_t>(std::atoi(argv[2])) * kMB);
    sleepThenExit(60);
  }
  if (argc >= 3 && std::strcmp(argv[1], "--k154-tree") == 0) {
    startHelper(300, argv[2]);
    allocate(200 * kMB);
    sleepThenExit(60);
  }
  const std::string& dir = gl::test::hermeticTestEnv("scanmem");
  testRules();
  testTreeMemory(dir);
  testScanIsolated(dir);
  testService(dir);
  return finish();
}
