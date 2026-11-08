// Gridline — hosted (third-party) plugins as Gridline plugin types.
//
// A scanned plugin class becomes the type "<format>:<id>" (host/PluginScan.h)
// with a runtime PluginInfo (core/PluginInfo.h): its exposed parameters map,
// in plugin order, to PluginState::params (normalized 0..1; toggles 0/1; small
// discrete lists as choice indices), so automation clips, MIDI learn and
// controller links address them through ordinary ParamIds. PluginState::data
// holds the plugin's own state, prefixed by format:
//   vst3s1:<base64 component state>:<base64 controller state>
//   clap1:<base64 state>   vst2c1:<bank|prog>:<base64 chunk>   vst2p1:<base64 floats>
//   au1:<base64 ClassInfo property list>   lv2s1:<base64 port values + state properties>
//
// Runtime instances are Generator / Effect adapters (createHostedGenerator /
// createHostedEffect) over a host::Backend — in this process, or in a sandbox
// child process when the class is set to "Run in sandbox" (PluginPrefs); the
// UI reaches them through hostedInstance().
#pragma once
#include <atomic>
#include <memory>
#include <string>
#include <vector>
#include "../core/PluginInfo.h"
#include "../engine/Effects.h"
#include "../engine/Generators.h"
#include "Backend.h"
#include "PluginPrefs.h"
#include "PluginScan.h"

namespace gl::host {

inline constexpr int kMaxInstrumentParams = 4800;  // channel ParamIds: kChannelPluginBase .. kNoteFxBase
inline constexpr int kMaxEffectParams = 990;       // slot ParamIds: below kSlotEnable
inline constexpr int kMaxAuxOutputs = 15;          // extra outputs of multi-output instruments

// A registered hosted class.
struct HostedClass {
  std::string type;    // "<format>:<uid>"
  std::string format;  // "vst3", "clap", "vst2", "au", "lv2"
  std::string bundle;  // entry (bundle path / AU key)
  std::string uid;
  std::string name, vendor, subCategories, version;
  bool instrument = false;
  bool sidechain = false;
  bool eventInput = false, eventOutput = false;  // takes / sends notes and MIDI
  int inputChannels = 0, outputChannels = 0;     // K-78: main bus channels (cross-format matching)
  std::vector<std::string> auxOutputs;  // extra output names
  std::vector<std::string> programs;    // program / factory preset names
  std::vector<ScannedParam> params;     // exposed parameters, PluginInfo order
  // Plugin units <-> normalized for parameter k.
  bool isChoice(size_t k) const { return k < params.size() && params[k].stepCount > 1 && params[k].labels.size() == static_cast<size_t>(params[k].stepCount) + 1; }
  double toNormalized(size_t k, double v) const;
  double fromNormalized(size_t k, double n) const;
  const char* formatLabel() const;
};

// Registers every class of a scan result (PluginInfo + HostedClass). Thread-safe.
void registerScanResult(const ScanResult& r);
// Class description for a hosted type; nullptr when not registered.
std::shared_ptr<const HostedClass> hostedClass(const std::string& type);
// Every registered class (latest registration of each type).
std::vector<std::shared_ptr<const HostedClass>> hostedClasses();
// Increments whenever a class is registered (J-147: cached views rebuild).
uint64_t hostedClassesGeneration();
PluginInfo makePluginInfo(const HostedClass& c);
HostedClass makeHostedClass(const ScannedClass& c, const std::string& bundle, const std::string& format = "vst3");

// A render on a worker thread, with hosted plugins in the project. Some plugin formats only allow
// creating, preparing and destroying an instance on the interface thread, while their audio side runs
// on any one thread at a time. A thread inside this scope hands those calls to the interface thread
// and waits for them; the interface thread runs them with runUiThreadTasks (every frame). So the
// window stays alive and Cancel works during the render, whatever the project holds.
class UiThreadRenderScope {
 public:
  UiThreadRenderScope();
  ~UiThreadRenderScope();
  UiThreadRenderScope(const UiThreadRenderScope&) = delete;
  UiThreadRenderScope& operator=(const UiThreadRenderScope&) = delete;
};
void runUiThreadTasks();
// Interface thread: `running` is the worker's own flag; serves its calls until it is false.
void serveUiThreadTasksWhile(const std::atomic<bool>& running);

// Runtime instances (UI thread). nullptr when the type is unknown or the
// plugin cannot be loaded; the reason is kept for lastError().
std::unique_ptr<Generator> createHostedGenerator(const std::string& type);
std::unique_ptr<Effect> createHostedEffect(const std::string& type);
std::string lastError();

std::string encodeState(const std::string& component, const std::string& controller);
bool decodeState(const std::string& data, std::string& component, std::string& controller);

// What a hosted instance asks the UI to apply to the song (see pumpUi).
struct UiUpdate {
  struct Edit {
    int index;    // parameter index (PluginInfo order)
    float value;  // plugin units
  };
  std::vector<Edit> edits;      // made in the plugin's own editor
  bool gestureEnd = false;      // an edit gesture finished (end of an undo step)
  bool readback = false;        // `values` holds every parameter as the plugin now has it
  std::vector<float> values;
  bool captureState = false;    // the plugin's state changed: store it in the song (captureState)
  bool crashed = false;         // the sandboxed plugin process died during this frame
};

// UI-side access to a running hosted plugin.
class HostedInstance {
 public:
  virtual ~HostedInstance() = default;
  virtual Backend* backend() = 0;
  virtual const HostedClass& hostedClass() const = 0;
  // Once per UI frame: forwards controller calls, keeps the plugin's
  // controller in sync with the values the engine sends (automation, links,
  // knob edits) and reports what the song should take over.
  virtual void pumpUi(UiUpdate& u) = 0;
  // Current plugin state as PluginState::data; the instance then treats this
  // data as already loaded (storing it in the song does not reload it).
  virtual bool captureState(std::string& data) = 0;
  // Parameter text as the plugin formats it ("" when unavailable).
  virtual std::string paramText(int index, float value) = 0;
  // Programs (factory presets of the plugin); selecting one reads the
  // parameters back and captures the new state on the next pumpUi.
  virtual int programCount() const = 0;
  virtual std::string programName(int index) const = 0;
  virtual bool selectProgram(int index) = 0;
  // Presets of the plugin's own library (CLAP preset discovery), cached
  // after the first call; loading one reads the parameters back and
  // captures the new state on the next pumpUi.
  virtual const std::vector<PresetRef>& libraryPresets() = 0;
  virtual bool loadLibraryPreset(size_t index) = 0;
  // Out-of-process instances.
  virtual bool sandboxed() const = 0;
  virtual bool crashed() const = 0;
  virtual std::string crashReason() const = 0;
  // The next Program build replaces this instance with a fresh one (reload
  // after a crash, sandbox setting changed).
  virtual void requestReload() = 0;
  // Processing options this instance runs with (see HostOptions).
  virtual const HostOptions& options() const = 0;
  // Total latency the engine compensates: the plugin's own plus the fixed
  // size buffer, in samples.
  virtual int totalLatency() const = 0;
  // Processing load since the previous call: average and peak fraction of
  // real time (1 = the block took as long as it plays).
  virtual void takeCpu(float& average, float& peak) = 0;
  // MIDI messages the plugin sent so far / note events it received ("Detect").
  virtual uint64_t midiOutCount() const = 0;
  virtual bool hasMidiOutput() const = 0;
};
HostedInstance* hostedInstance(Generator* g);
HostedInstance* hostedInstance(Effect* fx);

// Executable started for scanner / sandbox child processes (default: this
// executable). Tests point it at the CLI.
void setChildExecutable(const std::string& exe);
std::string childExecutable();

// ---- scanning service (app) -----------------------------------------------
// Loads <configDir>/plugin-cache.json and the plugin manager settings, and
// registers the cached classes (synchronous, fast). Call once at startup
// before opening projects.
void initHosting(const std::string& configDir);
// Scans new or changed entries of every enabled format in the default and
// user folders on a background thread, each in a child process (`exe
// --scan-plugin …`); `force` rescans everything, including blacklisted
// entries. No-op while a scan runs.
void startScan(const std::string& exe, bool force = false);
// Rescans one entry (plugin manager "retry").
// K-13: the search folders changed: scans what is new and drops the plugins of folders that left
// (now, or right after a running scan). Projects using a dropped plugin keep its placeholder.
void requestScan(const std::string& exe);
// K-13: scans every entry of a format found in one folder again; format "*": of every format that
// is on and searches folders. K-71: the folder is searched on the scan worker (returns at once).
void rescanFolder(const std::string& exe, const std::string& format, const std::string& folder);
// Every (format, entry) the search folders hold now (formats switched off left out).
std::vector<std::pair<std::string, std::string>> discoverPluginEntries();
void rescanEntry(const std::string& exe, const std::string& format, const std::string& entry);
// Rescans several entries ({format, entry}) one after another (plugin manager
// "Retry all", "Rescan" of a plugin installed in several formats).
void rescanEntries(const std::string& exe, const std::vector<std::pair<std::string, std::string>>& entries);
// K-71: a scan runs a few scanner children side by side (GRIDLINE_SCAN_JOBS=1..8 sets how many). The
// status is cheap to read every frame: it is never held while the cache is written or a child is
// waited for.
struct ScanStatus {
  bool running = false;
  int done = 0, total = 0;
  std::string current;          // entry being scanned (of several: the one scanning longest)
  double currentSeconds = 0;    // K-71: how long `current` has been scanning
  int active = 0;               // K-71: entries being scanned now
  std::string searching;        // K-71: folder being searched for plugins (before the entries are scanned)
  int classes = 0;      // registered hosted classes
  int failed = 0;       // blacklisted entries
  uint64_t generation = 0;  // increments whenever the registered set changes
  uint64_t cacheChanges = 0;  // K-73: increments whenever the cache changes (what cacheEntries() lists)
};
ScanStatus scanStatus();
// K-73: scanStatus().running without the copy (no lock): cheap enough to ask per plugin.
bool scanRunning();
// K-71: the entry being scanned as the interface shows it: its file name, and after a few seconds
// how long it has taken ("Big Synth.vst3  12 s"), so a slow plugin never looks like a hang.
std::string scanCurrentLabel(const ScanStatus& st);
// Blacklisted entries with the reason (cache entries with ok == false).
std::vector<std::pair<std::string, std::string>> blacklist();
struct CacheEntry {
  std::string format, entry, error;
  bool ok = false;
  int classes = 0;
};
std::vector<CacheEntry> cacheEntries();
// Waits for a running scan to finish (`gridline scan-plugins`).
void stopScan();
// J-103 app quit: cancels a running scan (its scanner child is killed; the entries
// not reached are scanned at the next start), joins the worker and refuses new
// scans, so no scan thread runs into static destruction. Idempotent.
void shutdownScan();

}  // namespace gl::host
