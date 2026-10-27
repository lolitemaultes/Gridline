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
  // J-136: true when v sits on the default (within kDefaultEpsilon of the
  // normalized range), so a double-click types instead of resetting.
  static constexpr double kDefaultEpsilon = 1e-4;
  bool isDefault(double v) const;
};

// J-136: what a double-click does on a parameter knob or slider, the same in
// every editor: reset to the default when the value differs from it (one undo
// step), else open the type-in popup.
enum class ParamDoubleClick { Reset, Type };
ParamDoubleClick paramDoubleClick(const ParamDesc& d, double value);

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
  // J-45: FL route numbering (see flRouteOutputs in Song.h). When the
  // parameter routeModeParam is on (>= 0.5), or routeModeParam ==
  // kRouteIndexAlways, the send parameters (sendParams, and gl.send's target)
  // hold an FL route index n of the hosting track instead of a mixer index
  // (0 = none). The engine resolves it on every Program build.
  int routeModeParam = -1;
  // J-45: parameter holding an FL input route index n of the hosting track:
  // < 0 = the track's own sidechain input (default), 0 = none (the effect's
  // own input), n >= 1 = the output of the track's n-th source (unscaled),
  // handed to the effect as its sidechain input.
  int sideInParam = -1;
  // Effects that read notes from the engine's MIDI port bus (ProcessContext::
  // ports): the task graph runs every MIDI Out channel before their track.
  bool midiInput = false;
  // Appended "model" choice param (pack plugins): states saved before it existed
  // (shorter param vectors) normalise it to 0 = legacy; new instances use its default.
  int legacyModelParam = -1;
};
inline constexpr int kMaxSendOutputs = 4;
inline constexpr int kRouteIndexAlways = -2;  // PluginInfo::routeModeParam

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
// "au:…", "lv2:…"), known or not (a project
// keeps a missing hosted plugin's state instead of dropping it).
bool isHostedType(const std::string& type);

// A plugin instance's saved state: type, parameter values, opaque data.
struct PluginState {
  std::string type;
  std::vector<float> params;  // one per descriptor, plugin units
  std::string data;           // e.g. sample file path
  // Channel instrument envelopes / LFOs / filter of FL-style channels (J-33,
  // engine/ChannelIns.h text; empty = none). Kept with the instrument so
  // presets and channel copies carry them.
  std::string ins;
  // Stand-ins (core/PluginStandIn.h): the third-party plugin this built-in
  // state replaces while it is not installed (compact JSON; empty = none).
  std::string original;

  bool operator==(const PluginState& o) const {
    return type == o.type && params == o.params && data == o.data && ins == o.ins && original == o.original;
  }
  bool empty() const { return type.empty(); }
};

// State of a factory preset (defaults + overrides).
PluginState presetState(const PluginInfo& info, size_t preset);
// State with every parameter at its default (empty state for unknown types).
PluginState defaultPluginState(const std::string& type);
// Pads/clamps params to the plugin's descriptor list.
void normalizePluginState(PluginState& s);
// J-104: gl.bitcrush states saved before the Rate control held "Downsample N" (a divider
// of the host rate). They load as Rate = reference / N Hz, the reference being the
// app's audio rate (44.1 kHz by default and for command-line renders).
void setLegacyRateReference(double sampleRate);
double legacyRateReference();
double bitcrushRateOfDivider(double divider);

}  // namespace gl
