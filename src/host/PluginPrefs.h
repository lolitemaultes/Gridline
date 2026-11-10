// Gridline — plugin manager settings: extra scan folders, enabled formats,
// per-plugin "Run in sandbox", favourites and user categories. Stored in
// <config>/plugin-manager.json. Thread-safe.
#pragma once
#include <algorithm>
#include <map>
#include <cstdint>
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
  // K-144: how per-note pitch (slide notes, porta, bend lines) reaches the plugin.
  //   Auto: per-note tuning where the format has it (CLAP note expressions, VST3 tuning
  //         expression), otherwise pitch bend on the note's channel.
  //   Mpe:  every note on its own MIDI channel (2..16) with its own pitch bend (MPE).
  //   ChannelBend: pitch bend on the note's channel, even where the format tunes notes.
  //   Off:  notes keep their pitch (slides are ignored).
  enum NotePitch : int { PitchAuto = 0, PitchMpe = 1, PitchChannelBend = 2, PitchOff = 3 };
  int notePitch = PitchAuto;
  int mpeBendRange = 48;  // semitones of a full MPE bend (the MPE standard's default)
  bool operator==(const HostOptions& o) const {
    return fixedBuffers == o.fixedBuffers && fixedSize == o.fixedSize && maxBuffers == o.maxBuffers && notifyRender == o.notifyRender &&
           modulation == o.modulation && genericEditor == o.genericEditor && notePitch == o.notePitch && mpeBendRange == o.mpeBendRange;
  }
  bool operator!=(const HostOptions& o) const { return !(*this == o); }
  // Options that need a new instance when they change.
  bool sameStructure(const HostOptions& o) const {
    return fixedBuffers == o.fixedBuffers && (!fixedBuffers || fixedSize == o.fixedSize) && maxBuffers == o.maxBuffers &&
           modulation == o.modulation && notePitch == o.notePitch && mpeBendRange == o.mpeBendRange;
  }
};

struct PluginPrefs {
  std::vector<std::string> extraFolders;      // searched for every file-based format
  // K-13: search folders per format (PluginFolders.h). The user's own folders of each format id,
  // in the order added; folders switched off ("<format id>|<path>", "*|<path>" for an
  // extraFolders entry); formats whose Default entry (the OS's standard folders) is switched off.
  std::map<std::string, std::vector<std::string>> formatFolders;
  std::set<std::string> disabledFolders;
  std::set<std::string> noDefaultFolders;
  std::set<std::string> disabledFormats;      // format ids not scanned / not listed ("vst2", …)
  std::set<std::string> sandbox;              // hosted types that run in a child process
  std::set<std::string> favourites;           // plugin types (hosted or built-in)
  std::map<std::string, std::string> categories;  // plugin type → user category
  std::map<std::string, HostOptions> options;     // hosted type → processing options (non-default only)
  // J-147: the format the browser lists when a plugin is installed in several
  // ("" = the default order, PluginGroups.h), the types picked per plugin
  // instead, and whether the browser lists every format anyway.
  std::string preferredFormat;
  std::set<std::string> preferTypes;
  bool browserAllFormats = false;
  bool operator==(const PluginPrefs& o) const {
    return extraFolders == o.extraFolders && formatFolders == o.formatFolders && disabledFolders == o.disabledFolders &&
           noDefaultFolders == o.noDefaultFolders && disabledFormats == o.disabledFormats && sandbox == o.sandbox && favourites == o.favourites &&
           categories == o.categories && options == o.options && preferredFormat == o.preferredFormat && preferTypes == o.preferTypes &&
           browserAllFormats == o.browserAllFormats;
  }
};

// Loads <configDir>/plugin-manager.json (missing file: defaults).
void loadPluginPrefs(const std::string& configDir);
// K-13: child processes (scanner, sandbox): the parent's settings file, read only (never saved).
void loadPluginPrefsFile(const std::string& file);
// The settings file ("" before loadPluginPrefs), handed to child processes as --plugin-prefs <file>.
std::string pluginPrefsFile();
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

// K-16: the user's favourite plugins (types, built-in or hosted; one list per user,
// saved with the settings). setFavourite returns false when nothing changed.
bool isFavourite(const std::string& type);
bool setFavourite(const std::string& type, bool on);
// Favourites first, each part in its own order (stable). `typeOf(item)` gives the type.
template <class T, class TypeOf>
void favouritesFirst(std::vector<T>& items, const std::set<std::string>& favourites, TypeOf typeOf) {
  std::stable_partition(items.begin(), items.end(), [&](const T& x) { return favourites.count(typeOf(x)) > 0; });
}

}  // namespace gl::host
