// Gridline — LV2 plugin descriptions: bundle discovery and what a plugin's
// Turtle data says about it (ports, features, UIs, presets, default state).
// Read with Gridline's own Turtle reader (Turtle.h).
#pragma once
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include "Turtle.h"

namespace gl::host::lv2 {

struct Port {
  enum Type { Audio, Control, Atom, CV, Other } type = Other;
  uint32_t index = 0;
  bool input = true;
  std::string symbol, name;
  bool hasMin = false, hasMax = false, hasDef = false;
  float min = 0, max = 1, def = 0;
  bool toggled = false, integer = false, enumeration = false, logarithmic = false, sampleRate = false;
  bool notOnGui = false, notAutomatic = false, sideChain = false, optional = false, reportsLatency = false;
  std::string designation;                             // lv2:designation IRI
  std::vector<std::pair<float, std::string>> points;   // scale points, by value
  std::string unit;                                    // display symbol ("dB", "Hz", …)
  bool midi = false, timePosition = false;             // atom ports: supports midi:MidiEvent / time:Position
  bool controlDesignated = false;                      // atom port designated lv2:control (main event port)
  uint32_t minimumSize = 0;                            // rsz:minimumSize (atom ports)
  std::string group;                                   // pg:group IRI
};

struct Ui {
  enum Kind { Cocoa, X11, External, Other } kind = Other;
  std::string uri, typeUri, binary, bundle;
  std::vector<std::string> required, optional;
};

struct PresetInfo {
  std::string uri, label;
};

struct PluginDesc {
  std::string uri, name, author, bundle, binary, version;
  std::vector<std::string> types;  // rdf:type IRIs
  std::vector<std::string> required, optional, extensionData;
  std::vector<Port> ports;  // by index
  std::vector<Ui> uis;
  std::vector<PresetInfo> presets;  // sorted by label
  bool instrumentClass = false;     // lv2:InstrumentPlugin
  bool defaultState = false;        // the plugin's own description carries state:state
  // The Turtle data of everything above (presets' port values and state,
  // default state): kept for applying presets later.
  std::shared_ptr<Model> model;
};

// Default LV2 folders of this platform plus $LV2_PATH (may not exist).
std::vector<std::string> defaultPaths();
// LV2 bundles (directories ending in ".lv2") below `folders` whose manifest
// declares at least one plugin.
std::vector<std::string> pluginBundles(const std::vector<std::string>& folders);
// Every bundle below the folders (plugins, presets, UIs).
std::vector<std::string> allBundles(const std::vector<std::string>& folders);
// Plugins declared by a bundle's manifest.
std::vector<std::string> pluginUris(const std::string& bundle, std::string* err = nullptr);
// Full description of `uri` from `bundle` plus every other bundle on the
// default paths that adds UIs or presets to it. Cached per URI.
std::shared_ptr<const PluginDesc> describe(const std::string& bundle, const std::string& uri, std::string* err);
// Newest modification time (seconds) of a bundle's files.
int64_t bundleMtime(const std::string& bundle);

// Port values (symbol → value) and state properties of a preset or of the
// plugin's default state, read from the model.
struct StateValue {
  std::string key;   // property IRI
  std::string type;  // atom type IRI (atom:String, atom:Int, atom:Path, …)
  std::string data;  // the atom body bytes
};
struct StateData {
  std::vector<std::pair<std::string, float>> ports;
  std::vector<StateValue> props;
};
bool readState(const Model& m, const std::string& subject, StateData& out);

// Namespace prefixes.
inline constexpr const char* kLv2 = "http://lv2plug.in/ns/lv2core#";
inline constexpr const char* kAtom = "http://lv2plug.in/ns/ext/atom#";
inline constexpr const char* kUi = "http://lv2plug.in/ns/extensions/ui#";
inline constexpr const char* kRdfType = "http://www.w3.org/1999/02/22-rdf-syntax-ns#type";
inline constexpr const char* kRdfs = "http://www.w3.org/2000/01/rdf-schema#";
inline constexpr const char* kPset = "http://lv2plug.in/ns/ext/presets#";
inline constexpr const char* kState = "http://lv2plug.in/ns/ext/state#";
inline constexpr const char* kXsd = "http://www.w3.org/2001/XMLSchema#";
inline constexpr const char* kKxExternalUi = "http://kxstudio.sf.net/ns/lv2ext/external-ui#Widget";
inline constexpr const char* kKxExternalHost = "http://kxstudio.sf.net/ns/lv2ext/external-ui#Host";
inline constexpr const char* kOldExternalUi = "http://lv2plug.in/ns/extensions/ui#external";

}  // namespace gl::host::lv2
