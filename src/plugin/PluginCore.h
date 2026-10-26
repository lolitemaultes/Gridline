// Gridline — Gridline itself as a plugin (PH-09): one Instance per plugin
// instance in the host, independent of the plugin format. The CLAP and VST3
// entry points (ClapEntry.cpp, Vst3Entry.cpp) translate their APIs to this.
//
// An Instance owns a whole Gridline (App: song, engine, program builder,
// interface state). Threads, as the formats define them:
//   main thread  create / destroy, activate, state, idle(), the editor
//   audio thread process() only: no allocation, locks or I/O
//
// Host parameters are 16 macros: each can be linked to any Gridline
// parameter (right-click a control, "Link to host macro"); the host
// automates the macro and the engine applies it every 64-sample control
// block. The plugin state is the whole project (JSON, sample references by
// path) plus the macro links and the plugin settings.
//
// J-131 (Gridline Suite): an Instance can also be one built-in plugin on its
// own (Instance(variant, "gl.<name>")): the App then runs a minimal project
// (the instrument on one channel, or the effect in insert 1's first slot with
// the host input on insert 1 and the sidechain on insert 2), the host
// parameters are the plugin's own parameters (+ Mix and Bypass for effects),
// the state is the plugin's state as Gridline stores it, and the editor is the
// plugin's editor alone.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "../core/Params.h"
#include "../core/PluginInfo.h"

namespace gl {
class App;
}

namespace gl::plugin {

class View;

enum class Variant { Instrument, Effect };

inline constexpr int kMacros = 16;
// Instrument: main stereo output + 15 extra stereo outputs carrying mixer
// inserts 1..15 (post fader, a copy: the inserts still feed the master).
inline constexpr int kExtraOutputs = 15;

// MIDI input: notes play the selected channel, or MIDI channel k plays the
// k-th channel of the rack (k = 1..16).
enum class MidiMode { Selected = 0, PerChannel = 1 };

struct Event {
  enum Kind : uint8_t { Midi, Macro } kind = Midi;
  int offset = 0;  // frame in the block
  uint8_t midi[3] = {0, 0, 0};
  int macro = 0;    // Macro: host parameter index (macro, or the plugin's parameter)
  float value = 0;  // Macro: normalized
};

// A host parameter as the formats describe it.
struct HostParam {
  std::string name, shortName, unit;
  double def = 0;        // normalized default
  int steps = 0;         // 0 = continuous, else number of steps (choices - 1)
  bool bypass = false;   // the effect's bypass switch
  bool toggle = false, choice = false, logScale = false;
  double min = 0, max = 1;  // plain range (Audio Units show plain values)
  std::vector<std::string> choices;
};

// J-131: parameter ids of the single-plugin effects' extra parameters (the
// plugin's own parameters use their index as id).
inline constexpr uint32_t kMixParamId = 0x00100000;
inline constexpr uint32_t kBypassParamId = 0x00100001;

struct Transport {
  bool valid = false;  // the host sent transport information
  bool playing = false;
  bool hasPosition = false, hasTempo = false;
  double beats = 0;  // song position in quarter notes
  double bpm = 0;
};

struct ProcessArgs {
  int frames = 0;
  const float* inL = nullptr;  // effect: main input (nullptr = silence)
  const float* inR = nullptr;
  const float* scL = nullptr;  // sidechain input (nullptr = silence)
  const float* scR = nullptr;
  float* outL = nullptr;  // main output
  float* outR = nullptr;
  int extraCount = 0;  // extra outputs (instrument), at most kExtraOutputs
  float* const* extraL = nullptr;
  float* const* extraR = nullptr;
  const Event* events = nullptr;  // sorted by offset
  int eventCount = 0;
  Transport transport;
};

class Instance {
 public:
  explicit Instance(Variant v);
  // J-131: one built-in plugin (`type` "gl.<name>"); variant Instrument for generators.
  Instance(Variant v, const std::string& type);
  ~Instance();
  Instance(const Instance&) = delete;
  Instance& operator=(const Instance&) = delete;

  Variant variant() const { return variant_; }
  App& app() { return *app_; }
  // Makes this instance the one gl::app() returns (main-thread entry points).
  void makeCurrent();
  // The instance whose App is `a` (main thread), nullptr when none.
  static Instance* of(const App* a);
  // The view showing Gridline's editor (nullptr while closed): hosted
  // plugins' windows belong to it.
  View* editorView() const { return view_; }
  void setEditorView(View* v) { view_ = v; }

  // ---- main thread ----
  bool activate(double sampleRate, int maxFrames);
  void deactivate();
  bool active() const { return active_; }
  double sampleRate() const { return sampleRate_; }
  void setOffline(bool on);
  // Housekeeping: program rebuilds after edits, macro links, MIDI routing,
  // latency changes. Called by timers, the editor and other main-thread calls.
  void idle();
  // Samples between input and output (plugin delay compensation of the project).
  int latency() const { return latency_.load(std::memory_order_relaxed); }
  // The host is told when the latency or the macro texts change.
  std::function<void()> onLatencyChanged;
  std::function<void()> onMacrosChanged;
  // A macro's value changed here (linking takes the parameter's value): the
  // host is told (main thread).
  std::function<void(int macro, double normalized)> onMacroEdited;

  std::string saveState();
  bool loadState(const std::string& data, std::string* err = nullptr);
  // J-131: factory preset i of the single plugin (PluginInfo::presets), as the
  // editor's preset menu loads it (the loaded sample stays). Main thread.
  bool loadFactoryPreset(int i);

  // Macros.
  std::string macroName(int k) const;    // "Macro 1"
  std::string macroTarget(int k) const;  // linked parameter's name ("" = not linked)
  ParamId macroLink(int k) const;
  // Links macro k to `target` (kNoId owner = unlink). The macro takes the
  // parameter's current value so linking does not change the sound.
  void linkMacro(int k, ParamId target);
  double macroValue(int k) const;               // normalized, last value from host or link
  void setMacroValue(int k, double normalized);  // main thread (host flush / editor)
  std::string macroText(int k, double normalized) const;

  // Settings (saved with the state).
  bool followHost() const { return follow_.load(std::memory_order_relaxed); }
  void setFollowHost(bool on) { follow_.store(on, std::memory_order_relaxed); }
  MidiMode midiMode() const { return midiMode_; }
  void setMidiMode(MidiMode m);

  // ---- host parameters (J-131: both kinds; the Gridline workstation's are the macros) ----
  bool single() const { return info_ != nullptr; }
  const PluginInfo* pluginInfo() const { return info_; }
  int paramCount() const;
  uint32_t paramId(int index) const;
  int paramIndex(uint32_t id) const;  // -1 = no such parameter
  HostParam paramInfo(int index) const;
  // Steps of a stepped parameter (toggle 1, choice count - 1; 0 = continuous):
  // its plain value is the step 0..steps. Any thread (no allocation).
  int paramSteps(int index) const;
  double paramValue(int index) const;                  // normalized
  void setParamValue(int index, double normalized);    // any thread (audio: no allocation)
  std::string paramText(int index, double normalized) const;
  bool paramFromText(int index, const std::string& text, double& normalized) const;
  double paramToPlain(int index, double normalized) const;
  double paramFromPlain(int index, double plain) const;
  // Single plugin: a parameter edit in the editor is a gesture (begin, values
  // through onMacroEdited, end); several parameters changed at once (preset,
  // undo, state) are announced with onMacrosChanged instead.
  std::function<void(int index, bool begin)> onParamGesture;
  // Single plugin: doc <-> host parameters (main thread; idle() and the editor call it).
  void syncParams();
  // The editor has a control under the pointer held (main thread).
  void setUiGesture(bool active) { uiGesture_ = active; }

  // ---- editor size (logical pixels) ----
  void preferredViewSize(int& w, int& h) const;
  void minViewSize(int& w, int& h) const;
  // Single plugin: the editor's content size at zoom 1 (measured when drawn);
  // true when it changed. Zoom is the editor's scale (host resize, menu).
  bool setEditorContent(int w, int h);
  void editorContent(int& w, int& h) const;
  float editorZoom() const { return zoom_; }
  void setEditorZoom(float z);
  int editorTab() const { return editorTab_; }
  void setEditorTab(int t) { editorTab_ = t; }
  // The channel (instrument) or mixer track (effect) holding the plugin.
  ObjectId pluginOwner() const { return owner_; }
  int pluginSlot() const { return info_ && variant_ == Variant::Effect ? 0 : -1; }

  // ---- audio thread ----
  void process(const ProcessArgs& a);
  // A MIDI message outside process() (parameter flush): applies at the next block.
  void queueMidi(const uint8_t m[3]);
  // The engine follows the host transport (last block).
  bool followingHost() const;

 private:
  void init();
  void applyMidiRouting();
  void publishMacros();  // links → program builder (rebuild)
  void setupSingle();    // J-131: the minimal project of a single plugin
  const PluginState* singleState() const;
  std::string saveSingle();
  bool loadSingle(const std::string& data, std::string* err);
  void resetHostValues();  // host values := the project's values
  void runSlice(const ProcessArgs& a, int from, int len, bool first);

  static constexpr int kChunk = 1024;  // frames per engine call (interleaved input buffer)
  Variant variant_;
  View* view_ = nullptr;
  std::unique_ptr<App> app_;
  bool active_ = false;
  double sampleRate_ = 44100.0;
  int maxFrames_ = 0;
  std::vector<float> interleaved_;  // effect input for the engine (activate)
  std::atomic<int> latency_{0};
  std::atomic<bool> follow_{true};
  MidiMode midiMode_ = MidiMode::Selected;
  ParamId links_[kMacros];
  std::atomic<float> macroValues_[kMacros];
  uint64_t routedRevision_ = ~0ull;
  ObjectId routedSel_ = 0;
  // J-131: single plugin.
  const PluginInfo* info_ = nullptr;
  ObjectId owner_ = kNoId;         // channel (instrument) / insert 1 (effect)
  int nParams_ = 0;                // the plugin's parameters (host parameters 0..n-1)
  std::unique_ptr<std::atomic<float>[]> hostNorm_;      // last value of each host parameter (normalized)
  std::unique_ptr<std::atomic<uint64_t>[]> hostDirty_;  // set by the host, not in the project yet
  std::vector<double> synced_;     // value the project and the host agreed on (normalized)
  std::vector<char> gesture_;      // gesture open for the host
  uint64_t syncedRevision_ = ~0ull;
  bool uiGesture_ = false;
  int editorW_ = 0, editorH_ = 0;
  float zoom_ = 1.0f;
  int editorTab_ = 0;
};

// Finds the assets folder for a plugin binary (bundle Resources, then the
// source tree for builds that run from the build directory).
std::string findPluginAssets();

}  // namespace gl::plugin
