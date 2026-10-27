// Gridline: hosted plugins grouped by product (J-147). One installed plugin
// usually registers once per format ("vst3:...", "clap:...", "au:..."): the
// plugin manager shows one row per plugin with its formats, and the browser
// and plugin picker list only the preferred format of each (unless every
// format is asked for). Gridline's own Suite builds (the native plugins as
// VST3 / CLAP / AU for other hosts, J-131) are recognised by their plugin ids:
// the native versions are built in, so the browser never lists them.
//
// The grouping and naming rules are pure functions (tests/test_plugin_groups.cpp);
// pluginGroups() applies them to the registered classes and the plugin
// manager settings, cached until either changes.
#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace gl::host {

struct HostedClass;

// A clean category from a format's category string (VST3 sub-categories
// "Fx|Dynamics", CLAP features "audio-effect|compressor|stereo", LV2 classes
// "CompressorPlugin", AU / VST2 "Fx"), falling back to words in the plugin's
// name ("Peak Limiter"). English, one of categoryNames(); "Instrument" /
// "Effect" when nothing more specific is known.
std::string normaliseCategory(const std::string& subCategories, bool instrument, const std::string& name = {});
// Every name normaliseCategory returns (instrument ones first).
const std::vector<const char*>& categoryNames();
bool isGenericCategory(const std::string& category);

// Gridline's own Suite build of a native plugin (CLAP org.gridline.suite.*,
// VST3 class GRIDLINESUIT..., AU manufacturer GrLn).
bool isOwnSuiteType(const std::string& type);

// Formats from most to least preferred: `preferred` first, then the default
// order (VST3, CLAP, AU, LV2, VST2). Unknown / empty `preferred`: the default.
std::vector<std::string> formatOrder(const std::string& preferred);
// Grouping key parts: lower-case letters and digits; the name without a
// trailing format tag ("Name (VST3)", "Name x64").
std::string groupNameKey(const std::string& name);
std::string groupVendorKey(const std::string& vendor);

struct GroupItem {
  std::string type, format, name, vendor, subCategories;
  bool instrument = false;
};
struct PluginGroup {
  std::string name, vendor, category;  // display name / vendor of the preferred member; normalised category
  bool instrument = false;
  bool ownSuite = false;      // Gridline Suite build
  bool userPicked = false;    // the preferred member was chosen for this plugin (preferTypes)
  std::vector<size_t> members;  // indexes into the items, most preferred format first
  size_t preferred = 0;         // index into the items
};
// Groups items that are the same plugin in different formats: same name key
// and the same vendor key (an empty vendor matches any). `preferTypes` holds
// types picked per plugin; otherwise the first format of formatOrder(preferredFormat)
// wins. Groups come in item order of their first member.
std::vector<PluginGroup> groupPlugins(const std::vector<GroupItem>& items, const std::string& preferredFormat,
                                      const std::set<std::string>& preferTypes);

// ---- the registered classes, grouped (UI thread) ----
struct GroupSnapshot {
  std::vector<std::shared_ptr<const HostedClass>> classes;  // enabled formats only; parallel to items
  std::vector<GroupItem> items;
  std::vector<PluginGroup> groups;
  std::map<std::string, size_t> groupOf;  // type -> group index
};
// Rebuilt when the registered classes or the plugin manager settings change.
std::shared_ptr<const GroupSnapshot> pluginGroups();
// False for a hosted type the browser and plugin picker leave out: a Gridline
// Suite build, or (unless every format is listed) not its plugin's preferred format.
bool listedInBrowser(const std::string& type);

}  // namespace gl::host
