// K-267 / K-268 (GitHub #59, #46): the parts of the Windows plugin fixes that run on every system.
//   1. the scan's timeouts: per plugin, longer for the first one and for a retry, never the batch's;
//   2. the scan log: one entry per failed scan with what the scanner saw, a header per scan, kept
//      under its size limit (the older part kept once);
//   3. the PE check: only what cannot load is told (not a PE file, another processor), never an
//      export table (protected plugins fill theirs while they load), on built files;
//   4. VST3 bundles: the binary found when its name differs from the bundle's, a *.vst3 folder that
//      is not a bundle searched inside, paths with letters outside ASCII;
//   5. which scan failures are tried again alone.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "Check.h"
#include "Hermetic.h"
#include "core/Platform.h"
#include "host/PluginScan.h"
#include "host/ScanLog.h"
#include "host/vst3/Vst3Module.h"

using namespace gl;
namespace fs = std::filesystem;

namespace {

std::string readAll(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

void touch(const fs::path& p, const std::string& text = "x") {
  fs::create_directories(p.parent_path());
  std::ofstream(p, std::ios::binary) << text;
}

// ---- 1. timeouts
void testTimeouts() {
  const host::ScanTimeouts t = host::scanTimeouts(60, 0);
  expect(t.first == 120 && t.each == 60 && t.longest >= 1800, "timeouts: 120 s for the first plugin, 60 s for each next one");
  expect(!host::scanTimedOut(t, 100, 100, 0), "timeouts: a first start that checks a licence for 100 s is waited for");
  expect(host::scanTimedOut(t, 121, 121, 0), "timeouts: no plugin after 120 s: given up");
  expect(!host::scanTimedOut(t, 600, 30, 40), "timeouts: a shell showing a plugin every 30 s goes on (10 minutes in all)");
  expect(host::scanTimedOut(t, 600, 61, 40), "timeouts: a shell stuck for 61 s after its last plugin is given up");
  expect(host::scanTimedOut(t, 1801, 1, 500), "timeouts: 30 minutes at most for one plugin file, whatever it shows");
  const host::ScanTimeouts again = host::scanTimeouts(60, 1);
  expect(again.first == 240 && again.each == 120, "timeouts: a retry (alone) waits twice as long");
  expect(!host::scanTimedOut(again, 200, 200, 0), "timeouts: a retry waits 200 s for a first start");
  const host::ScanTimeouts test = host::scanTimeouts(3, 0);
  expect(test.first == 6 && test.each == 3, "timeouts: follow the base (GRIDLINE_SCAN_TIMEOUT in tests)");
}

// ---- 5. retries
void testRetryable() {
  using O = host::ScanReport::Outcome;
  auto with = [](O o) {
    host::ScanReport r;
    r.outcome = o;
    return host::scanRetryable(r);
  };
  expect(with(O::TimedOut) && with(O::Crashed), "retry: timeouts and crashes are tried again alone");
  expect(!with(O::Failed) && !with(O::NotForThisComputer) && !with(O::TooMuchMemory) && !with(O::Cancelled) && !with(O::Listed),
         "retry: a plugin that said why it failed, one for another computer, one that used too much memory, a cancel: not");
}

// ---- 2. the scan log
void testScanLog(const std::string& dir) {
  host::ScanLogEntry e;
  e.time = "2026-11-15 10:00:00";
  e.format = "vst3";
  e.entry = "C:/Program Files/Common Files/VST3/Big Sampler.vst3";
  e.error = "crashed while loading (exception 0xC0000005)";
  e.report.outcome = host::ScanReport::Outcome::Crashed;
  e.report.exit = "crashed (exception 0xC0000005)";
  e.report.seconds = 12.25;
  e.report.progress = 0;
  e.report.peakMemory = 512ull << 20;
  e.report.childLog = "scan: start: vst3 C:/x.vst3\r\nscan: loading the module C:/x.vst3/Contents/x86_64-win/x.vst3\n";
  e.retried = true;
  const std::string text = host::formatScanLogEntry(e);
  std::printf("%s", text.c_str());
  expect(text.rfind("2026-11-15 10:00:00  vst3  C:/Program Files/Common Files/VST3/Big Sampler.vst3\n", 0) == 0, "log: time, format and file first");
  expect(text.find("result: crashed: crashed while loading (exception 0xC0000005)") != std::string::npos, "log: the outcome and the reason");
  expect(text.find("after 12.2 s, 0 plugins described, peak memory 512 MB") != std::string::npos, "log: run time, progress, peak memory");
  expect(text.find("    scan: loading the module") != std::string::npos && text.find('\r') == std::string::npos, "log: the scanner's steps, indented");
  expect(text.find("tried once more, alone") != std::string::npos, "log: says when the plugin is tried again");
  e.attempt = 1;
  e.retried = false;
  expect(host::formatScanLogEntry(e).find("(retried alone)") != std::string::npos, "log: a retry's failure says so");
  const std::string head = host::scanLogHeader("2026-11-15 10:00:00", 240, 4);
  expect(head.rfind("== 2026-11-15 10:00:00  scan of 240 plugin files, 4 at a time  (Gridline ", 0) == 0 && head.back() == '\n', "log: the scan's header");
  expect(host::scanLogTime().size() == 19, "log: local time as YYYY-MM-DD HH:MM:SS");

  // Written into a folder that does not exist yet (a first start), appended, kept under its limit.
  const fs::path file = fs::path(dir) / "config with space" / fs::u8path(u8"\u043F\u0430\u043F\u043A\u0430") / "plugin-scan.log";
  const std::string name = file.u8string();
  expect(host::appendScanLog(name, head) && host::appendScanLog(name, text), "log: written (folder created, letters outside ASCII)");
  expect(readAll(file) == head + text, "log: appended in order");
  std::string big(3000, 'x');
  big += "\n";
  expect(host::appendScanLog(name, big, 2048), "log: written past the limit");
  expect(readAll(file) == big, "log: a file past the limit starts again ...");
  expect(readAll(fs::u8path(name + ".old")) == head + text, "log: ... with the older part kept once");
  expect(host::appendScanLog(name, "y\n", 2048) && readAll(fs::u8path(name + ".old")) == big && readAll(file) == "y\n",
         "log: the next turn replaces the older part");
  expect(!host::appendScanLog("", "z"), "log: no file before the config folder is known");
}

// ---- 3. the PE check, on built files
void put16(std::vector<unsigned char>& b, size_t at, uint32_t v) {
  b[at] = static_cast<unsigned char>(v & 0xFF);
  b[at + 1] = static_cast<unsigned char>((v >> 8) & 0xFF);
}
void put32(std::vector<unsigned char>& b, size_t at, uint32_t v) {
  for (int k = 0; k < 4; ++k) b[at + static_cast<size_t>(k)] = static_cast<unsigned char>((v >> (8 * k)) & 0xFF);
}
// A DLL's headers: the PE signature at 0x80, `machine`, the optional header's magic; `exportGarbage`:
// an export directory pointing past the file (what a protected plugin's file can look like).
fs::path writePe(const fs::path& p, uint16_t machine, bool exportGarbage = false, uint16_t magic = 0) {
  std::vector<unsigned char> b(0x400, 0);
  b[0] = 'M';
  b[1] = 'Z';
  put32(b, 0x3C, 0x80);
  b[0x80] = 'P';
  b[0x81] = 'E';
  const bool plus = machine != 0x14C;
  put16(b, 0x84, machine);
  put16(b, 0x86, 1);
  put16(b, 0x94, plus ? 240 : 224);
  put16(b, 0x96, 0x2022);
  put16(b, 0x98, magic ? magic : plus ? 0x20B : 0x10B);
  put32(b, 0x98 + (plus ? 108 : 92), 16);
  if (exportGarbage) {
    put32(b, 0x98 + (plus ? 112 : 96), 0x7FFF0000);
    put32(b, 0x98 + (plus ? 116 : 100), 0x100);
  }
  fs::create_directories(p.parent_path());
  std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
  return p;
}

void testPe(const std::string& dir) {
  const fs::path d = fs::path(dir) / "pe";
  const std::vector<std::string> vst2 = {"VSTPluginMain", "main"};
  auto check = [&](const fs::path& p, uint16_t host) { return host::peDllPrecheck(p.u8string(), vst2, host); };
  expect(check(writePe(d / "x64.dll", 0x8664), 0x8664).empty(), "pe: a 64-bit DLL in a 64-bit Gridline is scanned");
  expect(check(writePe(d / "protected.dll", 0x8664, true), 0x8664).empty(), "pe: an export table the file cannot show is left to the scan");
  expect(check(writePe(d / "x86.dll", 0x14C), 0x8664).find("32-bit") != std::string::npos, "pe: a 32-bit DLL in a 64-bit Gridline is told");
  expect(check(writePe(d / "x86b.dll", 0x14C), 0x14C).empty(), "pe: a 32-bit DLL suits a 32-bit Gridline");
  expect(check(writePe(d / "x64b.dll", 0x8664), 0x14C).find("64-bit") != std::string::npos, "pe: a 64-bit DLL in a 32-bit Gridline is told");
  expect(check(writePe(d / "arm64x.dll", 0xAA64), 0x8664).empty(), "pe: an ARM64X DLL (ARM64 machine) is left to the scan on x64");
  expect(check(writePe(d / "arm.dll", 0xAA64), 0xAA64).empty(), "pe: an ARM64 DLL in an ARM64 Gridline is scanned");
  expect(check(writePe(d / "ec.dll", 0x8664), 0xAA64).find("x64") != std::string::npos, "pe: an x64 or ARM64EC DLL in an ARM64 Gridline is told");
  expect(check(writePe(d / "badmagic.dll", 0x8664, false, 0x1234), 0x8664).find("not a Windows DLL") != std::string::npos,
         "pe: an image without a PE optional header is not a DLL");
  touch(d / "text.dll", "hello, not a DLL at all");
  expect(check(d / "text.dll", 0x8664).find("not a Windows DLL") != std::string::npos, "pe: a text file is not a DLL");
  expect(check(d / "missing.dll", 0x8664).empty(), "pe: an unreadable file is left to the scan");
  const fs::path utf = writePe(d / fs::u8path(u8"\u00C9cho \u0434\u0430\u043B\u044C.dll"), 0x14C);
  expect(check(utf, 0x8664).find("32-bit") != std::string::npos, "pe: a file named with letters outside ASCII is read");
  expect(host::peMachineMismatch(0x8664, 0x8664).empty() && host::peMachineMismatch(0x1C4, 0x8664).empty(),
         "pe: the same machine, or another kind: no verdict");
}

// ---- 4. VST3 bundles
void testBundles(const std::string& dir) {
  const fs::path root = fs::path(dir) / fs::u8path(u8"vst3 \u043F\u043B\u0430\u0433\u0438\u043D\u044B");
  const std::vector<std::string> win = {"x86_64-win"};
  // The SDK's layout.
  const fs::path sdk = root / "Standard.vst3";
  touch(sdk / "Contents" / "x86_64-win" / "Standard.vst3");
  touch(sdk / "Contents" / "x86_64-win" / "Another.vst3");
  expect(vst3::findBundleBinary(sdk.u8string(), win, ".vst3") == (sdk / "Contents" / "x86_64-win" / "Standard.vst3").u8string(),
         "bundle: the binary named after the bundle comes first");
  // A bundle whose binary has another name (the issue's "no binary for this platform").
  const fs::path renamed = root / "Sampler Free.vst3";
  touch(renamed / "Contents" / "x86_64-win" / "SamplerFree.VST3");
  touch(renamed / "Contents" / "x86_64-win" / "readme.txt");
  expect(vst3::findBundleBinary(renamed.u8string(), win, ".vst3") == (renamed / "Contents" / "x86_64-win" / "SamplerFree.VST3").u8string(),
         "bundle: the one binary of another name (any letter case) is found");
  // Windows on ARM: the folders in order.
  const fs::path arm = root / "Arm.vst3";
  touch(arm / "Contents" / "arm64x-win" / "Arm.vst3");
  expect(vst3::findBundleBinary(arm.u8string(), {"arm64-win", "arm64x-win", "arm64ec-win"}, ".vst3") ==
             (arm / "Contents" / "arm64x-win" / "Arm.vst3").u8string(),
         "bundle: Windows on ARM falls back to arm64x-win");
  expect(vst3::findBundleBinary(arm.u8string(), win, ".vst3").empty(), "bundle: no binary for this processor: none");
  // Linux naming.
  const fs::path lin = root / "Lin.vst3";
  touch(lin / "Contents" / "x86_64-linux" / "Other.so");
  expect(vst3::findBundleBinary(lin.u8string(), {"x86_64-linux"}, ".so") == (lin / "Contents" / "x86_64-linux" / "Other.so").u8string(),
         "bundle: Linux: the .so of another name");

  // A *.vst3 folder that is a vendor's folder of plugins, not a bundle (the SDK searches inside).
  const fs::path vendor = root / "Vendor.vst3";
  touch(vendor / "One.vst3" / "Contents" / "x86_64-win" / "One.vst3");
  touch(vendor / "Two.vst3");  // an old single-file plugin
  const fs::path empty = root / "Empty.vst3";
  fs::create_directories(empty);
  std::vector<std::string> found = {sdk.u8string(), vendor.u8string(), empty.u8string(), renamed.u8string(), arm.u8string()};
  const std::vector<std::string> got = vst3::packagesOrInside(found, win);
  const std::set<std::string> have(got.begin(), got.end());
  for (const auto& g : got) std::printf("found: %s\n", g.c_str());
  expect(have.count((vendor / "One.vst3").u8string()) && have.count((vendor / "Two.vst3").u8string()) && !have.count(vendor.u8string()),
         "search: a folder of plugins named *.vst3 is searched inside");
  expect(have.count(sdk.u8string()) && have.count(renamed.u8string()) && have.count(arm.u8string()),
         "search: bundles stay as they are (one without this processor's binary too: it fails with the reason)");
  expect(have.count(empty.u8string()), "search: an empty *.vst3 folder stays listed (and fails with the reason)");
}

}  // namespace

int main() {
  const std::string dir = gl::test::hermeticTestEnv("win-plugins");
  testTimeouts();
  testRetryable();
  testScanLog(dir);
  testPe(dir);
  testBundles(dir);
  return finish();
}
