// Gridline — FL Studio import coverage: what the importer does with every
// project event id, and a tally of FL files (projects, presets) by plugin and
// event id, used by `gridline coverage-json` (analysis/tools/coverage.py).
#pragma once
#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>
#include "Flp.h"

namespace gl::flp {

// What the importer does with one event id.
struct EventInfo {
  enum Status { Decoded, Partial, Ignored };
  uint8_t id = 0;
  const char* name = "";  // short description of the event
  Status status = Ignored;
  const char* note = "";  // Partial / Ignored: what is left out and why
  // BYTE / WORD / DWORD events: the value FL writes when the setting is at its
  // default (-1 = no single default). Lets the coverage report count how often
  // an ignored setting actually differs from its default.
  int64_t dflt = -1;
};

// Every event id seen in FL Studio's shipped projects and presets, in id order.
const std::vector<EventInfo>& eventCatalog();
// nullptr for ids the catalog does not list.
const EventInfo* eventInfo(uint8_t id);
const char* statusName(EventInfo::Status s);

// Per FL plugin: instances found and how they decode.
struct PluginTally {
  int uses = 0, mapped = 0, exact = 0;
  std::map<std::string, int> types;  // Gridline type → instances
};

struct CoverageTally {
  int projects = 0, presets = 0, unreadable = 0;
  std::map<std::string, PluginTally> projectPlugins;  // instances in projects
  std::map<std::string, PluginTally> presetPlugins;   // instances in presets (.fst)
  // FL's plugin database: one .fst per plugin with its default state.
  std::map<std::string, PluginTally> databasePlugins;
  int database = 0;
  // Preset files as `import-fl-presets` converts them, by FL plugin (chains
  // and automation shapes under "(effect chain)" / "(automation shape)").
  std::map<std::string, PluginTally> presetFiles;
  struct EventCount {
    int projectFiles = 0, projectCount = 0, projectNonDefault = 0;
    int presetFiles = 0, presetCount = 0, presetNonDefault = 0;
  };
  std::map<int, EventCount> events;
};

enum class FileKind { Project, Preset, Database };
// Adds one .flp (Project) or .fst (Preset, Database) to the tally.
void tallyFile(const std::string& path, FileKind kind, CoverageTally& t, const std::string& factoryData = "");
// Plugin-native preset files (.gmsynth, .mrp, .tbio): the state payload alone, no FLP events.
bool isNativePresetFile(const std::string& path);
// The files a coverage run reads under an FL Studio install folder (the one
// holding Data/ and Plugins/): shipped projects, the preset library (the
// folders `import-fl-presets` converts) and the plugin database.
struct CoverageFiles {
  std::vector<std::string> projects, presets, database;
};
CoverageFiles coverageFiles(const std::string& flRoot);
// Decodes a plugin with no stored state (only its name) — the type a fresh
// instance maps to; "" when the decoder needs a saved state.
std::string defaultMapping(const std::string& flName, bool* exact = nullptr);
// JSON report: catalog of Gridline plugins, FL decoders, the tally, the event catalog.
std::string coverageJson(const CoverageTally& t);

}  // namespace gl::flp
