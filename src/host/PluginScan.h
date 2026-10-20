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
#include <cstdint>
#include <memory>
#include <string>
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

struct ScanResult {
  std::string format = "vst3";  // "vst3", "clap", "vst2", "au", "lv2"
  std::string bundle;           // the entry (path or AU key)
  int64_t mtime = 0;            // entry modification stamp; rescan when it changes
  bool ok = false;
  std::string error;  // why the entry is blacklisted
  std::vector<ScannedClass> classes;
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
};
// Formats available on this platform (AU on macOS only).
const std::vector<Format>& formats();
const Format* findFormat(const std::string& id);
// "vst3:ABC…" → format "vst3", uid "ABC…"; false when not a hosted type.
bool splitType(const std::string& type, std::string& format, std::string& uid);

// Instantiates a class in this process (format lookup + create).
std::unique_ptr<Backend> createBackend(const std::string& format, const std::string& entry, const std::string& uid, std::string* err);

// Same in a child process running `exe --scan-plugin <format> <entry> --out
// <tmp>`; a crash, error exit or timeout gives ok = false.
ScanResult scanIsolated(const std::string& exe, const std::string& format, const std::string& entry, double timeoutSeconds = 60);
// Entry point of child processes: the scanner (`--scan-plugin`, `--scan-vst3`)
// and the playback sandbox (`--plugin-sandbox`). Returns the exit code, or -1
// when the arguments name no child mode.
int childMain(int argc, char** argv);
// Path of the running executable ("" when unknown).
std::string selfExecutablePath();
// Newest modification time (seconds) of a path and, for bundles, its binaries.
int64_t pathMtime(const std::string& path);
// Every file / bundle with extension `ext` (".clap") below the folders; bundles
// (directories with that extension) are not searched inside.
std::vector<std::string> findEntries(const std::vector<std::string>& folders, const std::string& ext, bool directories, bool files);
// Search folders from an environment variable (":"-separated), appended to v.
void addEnvPaths(std::vector<std::string>& v, const char* var);
// Audio Unit entry key "<type>/<subtype>/<manufacturer>" from four-character
// codes (codes that are not plain text are written as 0xXXXXXXXX).
std::string auKey(uint32_t type, uint32_t subtype, uint32_t manufacturer);
std::string fourCharCode(uint32_t v);
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
  void remove(const std::string& bundle, const std::string& format = {});
  // Entries of `format` that are new or changed since they were cached.
  std::vector<std::string> stale(const std::vector<std::string>& bundles, const std::string& format = "vst3") const;
  const std::string& file() const { return file_; }

 private:
  std::string file_;
  std::vector<ScanResult> entries_;
};

}  // namespace gl::host
