// Gridline — plugin formats, scanning, the scan cache and the blacklist.
//
// A plugin "entry" is what one scan describes: a bundle / file path (VST3,
// CLAP, VST2) or an AudioUnit component key ("aufx/dely/appl"). Each entry is
// scanned in a child process (`gridline --scan-plugin <format> <entry> --out
// <file>`) so a plugin that crashes or hangs while loading cannot take
// Gridline down; such entries are blacklisted until they change. Results live
// in <config>/plugin-cache.json, shared by every format.
//
// Every scanned class becomes the Gridline plugin type "<format>:<uid>":
//   vst3:<32 hex digits class id>   clap:<plugin id>
//   vst2:<8 hex digits unique id>   au:<type>/<subtype>/<manufacturer>
//   lv2:<plugin URI> (entry: the .lv2 bundle directory)
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include "../core/Json.h"
#include "Backend.h"

namespace gl::host {

struct ScannedParam {
  uint32_t id = 0;
  std::string title, units;
  int stepCount = 0;
  double def = 0;
  int32_t flags = 0;                // ParamFlag
  std::vector<std::string> labels;  // value names of discrete parameters (stepCount + 1), when small
};

struct ScannedClass {
  std::string uid, name, vendor, version, subCategories, sdkVersion;
  bool instrument = false;
  bool eventInput = false;
  bool eventOutput = false;  // sends notes / MIDI (MIDI output port)
  int inputChannels = 0, outputChannels = 0, sidechainChannels = 0;
  int latency = 0;
  std::vector<std::string> auxOutputs;  // names of extra outputs (multi-output instruments)
  std::vector<std::string> programs;    // program / factory preset names
  std::vector<ScannedParam> params;
};

// K-180: one plugin of an entry that did not load while the rest of the entry did (a bundle or
// file of many plugins, a shell).
struct ScanFailure {
  std::string uid, name, error;
};

struct ScanResult {
  std::string format = "vst3";  // "vst3", "clap", "vst2", "au", "lv2"
  std::string bundle;           // the entry (path or AU key)
  int64_t mtime = 0;            // entry modification stamp; rescan when it changes
  bool ok = false;
  std::string error;  // why the entry is blacklisted
  std::vector<ScannedClass> classes;
  std::vector<ScanFailure> failed;  // K-180: its plugins that did not load (each listed as failed on its own)
};

Json toJson(const ScanResult& r);
bool fromJson(const Json& j, ScanResult& r);

// One plugin format.
struct Format {
  const char* id;     // "vst3"
  const char* label;  // "VST3"
  // Default folders (existing or not); AU lists the component folders for display.
  std::vector<std::string> (*defaultPaths)();
  // Entries found below the folders (AU: the system's component registry).
  std::vector<std::string> (*discover)(const std::vector<std::string>& folders);
  // Loads the entry in this process and describes its classes.
  ScanResult (*scanInProcess)(const std::string& entry);
  // Instantiates class `uid` of the entry (UI thread). nullptr + err on failure.
  std::unique_ptr<Backend> (*create)(const std::string& entry, const std::string& uid, std::string* err);
  // Modification stamp of an entry (seconds or a version number).
  int64_t (*mtime)(const std::string& entry);
  // K-73: why the entry cannot be a plugin for this computer, told without loading it ("" = it may
  // be one: scan it). nullptr: every entry is scanned.
  std::string (*precheck)(const std::string& entry) = nullptr;
};
// Formats available on this platform (AU on macOS only).
const std::vector<Format>& formats();
const Format* findFormat(const std::string& id);
// "vst3:ABC…" → format "vst3", uid "ABC…"; false when not a hosted type.
bool splitType(const std::string& type, std::string& format, std::string& uid);

// Instantiates a class in this process (format lookup + create).
std::unique_ptr<Backend> createBackend(const std::string& format, const std::string& entry, const std::string& uid, std::string* err);

// Same in a child process running `exe --scan-plugin <format> <entry> --out
// <tmp>`; a crash, error exit or timeout gives ok = false. When `cancel` turns
// true the child is killed and the result (ok = false) must not be cached (J-103: quit).
// K-73: the timeout counts from the child's last progress (scanProgress), so a shell of hundreds of
// plugins is not given up while it describes them; 30 minutes at most in all.
// K-154: the child runs in its own process group (Windows: a job), and the whole tree (with any
// helper it starts, such as a bridge's host process) is ended when the scan of the entry ends.
// While it loads, the tree's memory is watched: past scanMemoryCap(), or while the computer runs
// low on memory (scanMemoryLow()), it is stopped and the entry fails with "used too much memory
// while loading", like any broken plugin (kept out of later scans until a Rescan of everything).
ScanResult scanIsolated(const std::string& exe, const std::string& format, const std::string& entry, double timeoutSeconds = 60,
                        const std::atomic<bool>* cancel = nullptr);
// K-154: the memory one plugin's scan may use: a quarter of the physical memory, 2 to 8 GB (4 GB
// when the size is unknown; big sample-based instruments take a gigabyte or two while they load).
// What keeps the computer out of swap is the reserve below. GRIDLINE_SCAN_MEMORY_MB overrides it (tests).
std::uint64_t scanMemoryCap(std::uint64_t physicalBytes);
// K-154: the scan's tree is stopped early when it holds 512 MB or more (a quarter of the cap when
// that is less: real use, not a bystander's) while the memory still free for new use is under the
// reserve: a tenth of the physical memory, 512 MB to 2 GB (GRIDLINE_SCAN_RESERVE_MB overrides it).
// So a scan never pushes the computer into swap, whatever the cap allows.
std::uint64_t scanMemoryReserve(std::uint64_t physicalBytes);
bool scanMemoryLow(std::uint64_t treeBytes, std::uint64_t availableBytes, std::uint64_t physicalBytes);
// The failure text for a stopped scan ("used too much memory while loading (...)").
std::string scanMemoryError(std::uint64_t treeBytes, bool low);
// K-154: a scan result file larger than this is not read (a plugin that described itself without end).
constexpr std::uint64_t kScanResultMaxBytes = 256ull << 20;
// Entry point of child processes: the scanner (`--scan-plugin`, `--scan-vst3`)
// and the playback sandbox (`--plugin-sandbox`). Returns the exit code, or -1
// when the arguments name no child mode.
int childMain(int argc, char** argv);
// K-73: in the scanner child, one plugin of the entry is described (the parent's timeout starts
// again). Elsewhere it does nothing.
void scanProgress();
// The file a scanner child writing `outFile` reports its progress in (it grows by a byte a plugin).
std::string scanProgressFile(const std::string& outFile);
// K-73: Windows DLLs (PE files): "" when the file may be a plugin with one of the `entryPoints`
// exported for this computer; else why it is not (not a DLL, built for another processor such as a
// 32-bit DLL in a 64-bit Gridline, no plugin entry point). Unreadable files give "" (the scan decides).
// `hostMachine` is a PE machine code (0x8664 x64, 0xAA64 ARM64, 0x14C x86), 0 for this build's.
std::string peDllPrecheck(const std::string& path, const std::vector<std::string>& entryPoints, uint16_t hostMachine = 0);
// Path of the running executable ("" when unknown).
std::string selfExecutablePath();
// Newest modification time (seconds) of a path and, for bundles, its binaries.
int64_t pathMtime(const std::string& path);
// Every file / bundle with extension `ext` (".clap") below the folders; bundles
// (directories with that extension) are not searched inside. K-71: each real folder is searched
// once (symlink loops, links back up, folders reached twice), seven levels deep at most; /proc,
// /sys and /dev are never searched.
std::vector<std::string> findEntries(const std::vector<std::string>& folders, const std::string& ext, bool directories, bool files);
// K-71: while one lives, findEntries on this thread stops early when `cancel` turns true (what it
// found so far is returned and must not be trusted) and tells `progress` the folder it searches,
// now and then. The scan worker searches with one; scopes do not nest.
class DiscoveryScope {
 public:
  using Progress = std::function<void(const std::string& folder)>;
  DiscoveryScope(const std::atomic<bool>* cancel, Progress progress);
  ~DiscoveryScope();
  DiscoveryScope(const DiscoveryScope&) = delete;
  DiscoveryScope& operator=(const DiscoveryScope&) = delete;
};
// Search folders from an environment variable (":"-separated), appended to v.
void addEnvPaths(std::vector<std::string>& v, const char* var);
// $var/sub, "" when the variable is not set.
std::string envFolder(const char* var, const char* sub);
// The system-wide plugin folders of a format: Windows %LOCALAPPDATA%\Programs\Common\<winCommon>
// and %COMMONPROGRAMFILES%\<winCommon>; elsewhere /usr/lib, /usr/local/lib and (NetBSD) /usr/pkg/lib
// followed by <unixLib>.
void addSystemPluginFolders(std::vector<std::string>& v, const char* winCommon, const char* unixLib);
// Audio Unit entry key "<type>/<subtype>/<manufacturer>" from four-character
// codes (codes that are not plain text are written as 0xXXXXXXXX).
std::string auKey(uint32_t type, uint32_t subtype, uint32_t manufacturer);
std::string fourCharCode(uint32_t v);
#if defined(__APPLE__)
// Tests: a parameter edit made in the Audio Unit itself, as its view makes it. False when `b` is
// not an Audio Unit.
bool auPostParameterForTests(Backend& b, uint32_t id, float plainValue);
#endif
// "vst2:" type id of a VST2 unique id (8 upper-case hex digits).
std::string vst2Type(uint32_t uniqueId);

// Scan cache: one ScanResult per (format, entry).
class ScanCache {
 public:
  explicit ScanCache(std::string file = {}) : file_(std::move(file)) {}
  bool load();
  bool save() const;
  const std::vector<ScanResult>& entries() const { return entries_; }
  const ScanResult* find(const std::string& bundle, const std::string& format = {}) const;
  void put(ScanResult r);
  // K-150: the cached result of an entry, to change in place (nullptr: not cached), and a new
  // entry's result added without a search: a caller that holds a lock while the cache changes finds
  // the entry first, and changes it in constant time.
  ScanResult* slot(const std::string& bundle, const std::string& format);
  void append(ScanResult r) { entries_.push_back(std::move(r)); }
  void remove(const std::string& bundle, const std::string& format = {});
  // K-71: several entries ({format, entry}) in one pass.
  void remove(const std::vector<std::pair<std::string, std::string>>& entries);
  // Entries of `format` that are new or changed since they were cached.
  std::vector<std::string> stale(const std::vector<std::string>& bundles, const std::string& format = "vst3") const;
  const std::string& file() const { return file_; }

 private:
  std::string file_;
  std::vector<ScanResult> entries_;
};

}  // namespace gl::host
