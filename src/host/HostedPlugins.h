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
PluginInfo makePluginInfo(const HostedClass& c);
HostedClass makeHostedClass(const ScannedClass& c, const std::string& bundle, const std::string& format = "vst3");

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
void rescanEntry(const std::string& exe, const std::string& format, const std::string& entry);
struct ScanStatus {
  bool running = false;
  int done = 0, total = 0;
  std::string current;  // entry being scanned
  int classes = 0;      // registered hosted classes
  int failed = 0;       // blacklisted entries
  uint64_t generation = 0;  // increments whenever the registered set changes
};
ScanStatus scanStatus();
// Blacklisted entries with the reason (cache entries with ok == false).
std::vector<std::pair<std::string, std::string>> blacklist();
struct CacheEntry {
  std::string format, entry, error;
  bool ok = false;
  int classes = 0;
};
std::vector<CacheEntry> cacheEntries();
// Waits for a running scan to finish (shutdown).
void stopScan();

}  // namespace gl::host
