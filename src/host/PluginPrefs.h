// Gridline — plugin manager settings: extra scan folders, enabled formats,
// per-plugin "Run in sandbox", favourites and user categories. Stored in
// <config>/plugin-manager.json. Thread-safe.
#pragma once
#include <map>
#include <set>
#include <string>
#include <vector>

namespace gl::host {

// Wrapper "Processing" options of one hosted plugin type (plugin window →
// Settings), the counterparts of a plugin wrapper's processing switches.
struct HostOptions {
  // Use fixed size buffers: the plugin always processes exactly `fixedSize`
  // frames (adds that much latency, compensated like plugin latency).
  bool fixedBuffers = false;
  int fixedSize = 512;
  // Process maximum size buffers: one process call per engine block, notes
  // placed inside it by sample offset (off: the block is split at notes).
  bool maxBuffers = true;
  // Notify about rendering mode: the plugin learns about offline exports.
  bool notifyRender = true;
  // CLAP: links and automation reach modulatable parameters as
  // non-destructive modulation (the plugin keeps the song's value).
  bool modulation = false;
  // Open the generic parameter editor instead of the plugin's own interface.
  bool genericEditor = false;
  bool operator==(const HostOptions& o) const {
    return fixedBuffers == o.fixedBuffers && fixedSize == o.fixedSize && maxBuffers == o.maxBuffers && notifyRender == o.notifyRender &&
           modulation == o.modulation && genericEditor == o.genericEditor;
  }
  bool operator!=(const HostOptions& o) const { return !(*this == o); }
  // Options that need a new instance when they change.
  bool sameStructure(const HostOptions& o) const {
    return fixedBuffers == o.fixedBuffers && (!fixedBuffers || fixedSize == o.fixedSize) && maxBuffers == o.maxBuffers &&
           modulation == o.modulation;
  }
};

struct PluginPrefs {
  std::vector<std::string> extraFolders;      // searched for every file-based format
  std::set<std::string> disabledFormats;      // format ids not scanned / not listed ("vst2", …)
  std::set<std::string> sandbox;              // hosted types that run in a child process
  std::set<std::string> favourites;           // plugin types (hosted or built-in)
  std::map<std::string, std::string> categories;  // plugin type → user category
  std::map<std::string, HostOptions> options;     // hosted type → processing options (non-default only)
  bool operator==(const PluginPrefs& o) const {
    return extraFolders == o.extraFolders && disabledFormats == o.disabledFormats && sandbox == o.sandbox && favourites == o.favourites &&
           categories == o.categories && options == o.options;
  }
};

// Loads <configDir>/plugin-manager.json (missing file: defaults).
void loadPluginPrefs(const std::string& configDir);
PluginPrefs pluginPrefs();
// Replaces the settings and saves them (when a config dir is known).
void setPluginPrefs(const PluginPrefs& p);
// True when the type runs out of process (also for every type while the
// environment variable GRIDLINE_SANDBOX_ALL is set).
bool runsInSandbox(const std::string& type);
void setRunInSandbox(const std::string& type, bool on);
bool formatEnabled(const std::string& format);
// Processing options of a hosted type (defaults when never set).
HostOptions hostOptions(const std::string& type);
void setHostOptions(const std::string& type, const HostOptions& o);
// Increments whenever the settings change.
uint64_t pluginPrefsGeneration();

}  // namespace gl::host
