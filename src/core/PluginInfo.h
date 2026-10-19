// Gridline — catalogue of built-in instruments and effects: identity and
// parameter descriptors. DSP lives in engine/; the model, undo, automation and
// UI only need these descriptors.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace gl {

enum class ParamKind { Continuous, Toggle, Choice };
enum class ParamScale { Linear, Log };

struct ParamDesc {
  std::string name;
  std::string unit;
  double min = 0.0;
  double max = 1.0;
  double def = 0.0;
  ParamKind kind = ParamKind::Continuous;
  ParamScale scale = ParamScale::Linear;
  std::vector<std::string> choices;  // ParamKind::Choice labels (value = index)

  double clamp(double v) const {
    if (v != v) v = def;  // NaN (corrupt files, broken DSP) -> default
    if (kind == ParamKind::Toggle) return v >= 0.5 ? 1.0 : 0.0;
    if (kind == ParamKind::Choice) {
      double hi = choices.empty() ? max : static_cast<double>(choices.size() - 1);
      if (!(v + 0.5 >= 1.0)) return 0;  // also keeps the cast below in range
      if (v >= hi) return hi;
      double r = static_cast<double>(static_cast<long long>(v + 0.5));
      return r > hi ? hi : r;
    }
    return v < min ? min : (v > max ? max : v);
  }
  // Normalized 0..1 mapping used by knobs, controllers and automation clips.
  double toNormalized(double v) const;
  double fromNormalized(double n) const;
  // Human-readable value, e.g. "1.20 kHz", "-6.0 dB", "Saw".
  std::string format(double v) const;
};

// NoteFx: note processors placed on a channel before its instrument (Channel::noteFx).
enum class PluginKind { Generator, Effect, NoteFx };

struct PluginInfo {
  std::string type;      // stable id stored in projects, e.g. "gl.synth"
  std::string name;      // display name
  std::string category;  // browser grouping
  PluginKind kind = PluginKind::Generator;
  std::vector<ParamDesc> params;
  bool usesSample = false;  // PluginState::data holds a sample path
  // Factory presets: name + parameter overrides on top of the defaults.
  struct Preset {
    std::string name;
    std::vector<std::pair<int, float>> values;
  };
  std::vector<Preset> presets;
  float editorWidth = 0;  // preferred plugin window width in px (0 = default)
  // Controller plugins: names of their control outputs (0..1 each), which
  // internal controller links (ModLink) can route to any parameter.
  std::vector<std::string> outputs = {};
  // Effects with several send outputs (band splitters): parameters holding a
  // destination mixer index each (-1 = none). Up to kMaxSendOutputs; the engine
  // orders the graph accordingly and hands the effect one buffer per output.
  std::vector<int> sendParams = {};
  // Effects that read notes from the engine's MIDI port bus (ProcessContext::
  // ports): the task graph runs every MIDI Out channel before their track.
  bool midiInput = false;
  // Appended "model" choice param (pack plugins): states saved before it existed
  // (shorter param vectors) normalise it to 0 = legacy; new instances use its default.
  int legacyModelParam = -1;
};
inline constexpr int kMaxSendOutputs = 4;

// All built-in plugins (generators first).
const std::vector<PluginInfo>& pluginCatalog();
// Built-in or runtime-registered plugin; nullptr when unknown.
const PluginInfo* findPlugin(const std::string& type);

// --- runtime-registered plugins (hosted VST3 etc.) ---------------------------
// Types with a host prefix ("vst3:<class uid>") are described at runtime, from
// the plugin scanner's cache. Registration is append-only: a later entry for
// the same type supersedes the earlier one, entries are never freed, so the
// pointers findPlugin returns stay valid and lookups need no lock. Thread-safe.
void registerDynamicPlugin(PluginInfo info);
// Latest entry of every registered type, in registration order.
std::vector<const PluginInfo*> dynamicPlugins();
// True for types owned by a plugin host ("vst3:…", "clap:…", "vst2:…",
// "au:…"), known or not (a project
// keeps a missing hosted plugin's state instead of dropping it).
bool isHostedType(const std::string& type);

// A plugin instance's saved state: type, parameter values, opaque data.
struct PluginState {
  std::string type;
  std::vector<float> params;  // one per descriptor, plugin units
  std::string data;           // e.g. sample file path

  bool operator==(const PluginState& o) const { return type == o.type && params == o.params && data == o.data; }
  bool empty() const { return type.empty(); }
};

// State of a factory preset (defaults + overrides).
PluginState presetState(const PluginInfo& info, size_t preset);
// State with every parameter at its default (empty state for unknown types).
PluginState defaultPluginState(const std::string& type);
// Pads/clamps params to the plugin's descriptor list.
void normalizePluginState(PluginState& s);

}  // namespace gl
