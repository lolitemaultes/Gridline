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
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "../core/Params.h"

namespace gl {
class App;
}

namespace gl::plugin {

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
  int macro = 0;
  float value = 0;  // Macro: normalized
};

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
  ~Instance();
  Instance(const Instance&) = delete;
  Instance& operator=(const Instance&) = delete;

  Variant variant() const { return variant_; }
  App& app() { return *app_; }
  // Makes this instance the one gl::app() returns (main-thread entry points).
  void makeCurrent();

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

  // ---- audio thread ----
  void process(const ProcessArgs& a);
  // The engine follows the host transport (last block).
  bool followingHost() const;

 private:
  void applyMidiRouting();
  void publishMacros();  // links → program builder (rebuild)
  void runSlice(const ProcessArgs& a, int from, int len, bool first);

  static constexpr int kChunk = 1024;  // frames per engine call (interleaved input buffer)
  Variant variant_;
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
};

// Finds the assets folder for a plugin binary (bundle Resources, then the
// source tree for builds that run from the build directory).
std::string findPluginAssets();

}  // namespace gl::plugin
