// Gridline — catalogue of built-in instruments and effects: identity and
// parameter descriptors. DSP lives in engine/; the model, undo, automation and
// UI only need these descriptors.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace gl {

enum class ParamKind { Continuous, Toggle, Choice };
// Detent (J-160): the default sits at the centre of the travel, like a mixer knob's
// centre detent: min..def over the lower half, def..max over the upper (each linear).
// Curve501 (K-37): min..max along (501^n - 1) / 500, which reaches a true min of 0, unlike Log.
enum class ParamScale { Linear, Log, Detent, Curve501 };

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
    // K-257c: the preset's own PluginState::data (Terrace's steps); empty = the preset leaves the
    // current data alone (a loaded sample stays).
    std::string data = {};
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
  // J-151: the channel window shows the note processor panel under the editor
  // (instruments whose notes are commands, like a DJ setup's cue keys, leave it
  // out while the channel has none).
  bool noteFxPanel = true;
  // > 0: the editor reflows down to this width when the main window is narrower
  // than editorWidth (the channel window follows it instead of overflowing).
  float editorMinWidth = 0;
  // J-159: names of keys whose notes are commands (index = MIDI note; empty or
  // missing = a plain note). The piano roll labels those rows and notes.
  std::vector<std::string> keyNames = {};
  // 0.7.8: names that depend on the instance (Seltmann: its regions from the base note, the pattern
  // octave below); when set, it wins over keyNames. Called on the UI thread; may cache.
  std::vector<std::string> (*instanceKeyNames)(const struct PluginState&) = nullptr;
  // K-23: kept so old projects open and play the same, but no longer offered
  // (pickers, browser, menus leave it out; projects and FL imports still load it).
  bool hidden = false;
  // K-164: the editor has its own name, preset picker and save in its top bar: the plugin
  // window's header leaves out its wordmark, preset arrows, picker and save (A/B stays).
  bool ownPresetBar = false;
  // K-223: a large editor whose type is designed for 100 %: its window opens at full size
  // whatever the global plugin window size (Settings > Interface), smaller only when the screen
  // has no room for it. A size picked for the plugin in its window menu still wins.
  bool editorFullSize = false;
  // K-2: defaults a parameter had before it moved to the reference plugin's default
  // (setReferenceDefaults). A saved state too short to hold the parameter predates the
  // change and keeps the old default (normalizePluginState).
  std::vector<std::pair<int, float>> legacyDefaults = {};
  // K-46: PluginState::data of a new instance (states saved earlier keep theirs).
  std::string defaultData = {};
  // Installed plugins: the plugin's own stable id of each parameter, in `params` order (VST3
  // ParamID, CLAP id, AU parameter id, VST2 index, LV2 port index). Collaborators whose builds of
  // the plugin list parameters in another order map them by these ids. Empty for built-in plugins.
  std::vector<uint32_t> hostParamIds = {};
};

// K-2: sets parameter defaults to the reference plugin's (index, new default). Factory
// presets that relied on the old default get it written out, so they sound as before;
// the old defaults are kept in legacyDefaults for states saved before the change.
void setReferenceDefaults(PluginInfo& p, const std::vector<std::pair<int, double>>& defs);
inline constexpr int kMaxSendOutputs = 4;
inline constexpr int kRouteIndexAlways = -2;  // PluginInfo::routeModeParam

// All built-in plugins (generators first).
const std::vector<PluginInfo>& pluginCatalog();
// The built-in plugins of one kind (hidden ones left out) by category, for every picker that lists
// them under category headings: each category once, in the order it first appears in the catalogue,
// its plugins in catalogue order. (The catalogue itself is not grouped: pack plugins come after the
// core ones, so walking it and starting a heading whenever the category changes shows a category twice.)
struct CatalogGroup {
  std::string category;
  std::vector<const PluginInfo*> items;
};
const std::vector<CatalogGroup>& catalogGroups(PluginKind kind);
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

// Collaboration: an installed plugin's state put in the song from a collaborator's edit (the session
// marks each one as it arrives). The instance that loads exactly this state then takes the song's
// parameter values as the truth (they hold the parameter edits made after that state) instead of
// reading the plugin's values back. Thread-safe; bounded.
void noteRemotePluginData(const std::string& data);
bool takeRemotePluginData(const std::string& data);
// A state that is still arriving from a collaborator ("collab-state:Audio/<sha256>.glstate"): an
// instance keeps what it has until the real state replaces it.
inline constexpr const char* kPendingStatePrefix = "collab-state:";
inline bool isPendingPluginData(const std::string& data) { return data.compare(0, 13, kPendingStatePrefix) == 0; }

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
  // Third-party plugins: the identity record kept from the project file (compact JSON, host/CrossFormat.h),
  // so a plugin that is not installed here can be found again in another format. Descriptive only: it is
  // rebuilt on every save, so it takes no part in comparing states.
  std::string ident;
  // K-112c: the parameters a device row shows for this instance (the Live layout's installed-plugin
  // panel, chosen with Configure), in order; empty = the default choice. Saved with the project only
  // when set, so older projects save byte-equal. {-1}: none chosen (the panel is empty).
  std::vector<int> shown;
  // The sample's warp markers (core/SampleWarp.h, compact JSON; empty = none): the sampler, Kerf,
  // Pads and Seltmann play their sample warped. Saved only when set.
  std::string warp;
  // Loupe's warp markers on this instrument's sample (or this Loupe slot's audio), placed and not
  // applied yet (core/SampleWarp.h's text, markers only; empty = none). Saved only when set.
  std::string loupeWarp = {};  // (a default: the older aggregate initialisers leave it out)

  bool operator==(const PluginState& o) const {
    return type == o.type && params == o.params && data == o.data && ins == o.ins && original == o.original && shown == o.shown &&
           warp == o.warp && loupeWarp == o.loupeWarp;
  }
  bool empty() const { return type.empty(); }
};

// State of a factory preset (defaults + overrides, and the preset's data when it has its own).
PluginState presetState(const PluginInfo& info, size_t preset);
// K-257c: the factory preset with the current data kept unless the preset brings its own
// (loading a factory preset keeps a loaded sample, but takes Terrace's steps).
PluginState presetStateKeepingData(const PluginInfo& info, size_t preset, const std::string& currentData);
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
