// Gridline — preset library on disk: plugin presets (one plugin state) and
// effect-chain presets (a mixer track's slots), organised in folders.
//
//   <root>/<plugin type>/<folder…>/<name>.json      plugin presets
//   <root>/chains/<folder…>/<name>.json             effect chains
//   <root>/automation/<folder…>/<name>.json         automation shapes (J-5)
//
// <root> is "<config dir>/presets". Presets converted from an FL Studio
// installation live under the folder "FL Studio/<FL plugin name>/…" and are
// created on the user's machine only (see flp::importPresetLibrary).
#pragma once
#include <string>
#include <vector>
#include "PluginInfo.h"
#include "Song.h"

namespace gl {

struct PresetEntry {
  std::string name;    // file stem
  std::string folder;  // path below the type folder, "/"-separated ("" = top level)
  std::string file;    // absolute path
};

std::string presetRoot(const std::string& configDir);

// All presets for a plugin type, recursively, sorted by folder then name.
std::vector<PresetEntry> listPresets(const std::string& root, const std::string& type);
bool loadPreset(const std::string& file, const std::string& type, PluginState& out);
// Preset files of other synths that a built-in plugin opens (Serum ".SerumPreset"
// / ".fxp" -> Plasma): the plugin type, "" for others. loadPreset reads them
// for that type.
std::string foreignPresetType(const std::string& file);
bool isForeignPresetFile(const std::string& file);
// `relName` may contain folders ("FL Studio/Fruity Reeverb 2/Large hall").
// Saves params and data. `source` is an optional note (e.g. where it came from).
bool savePreset(const std::string& root, const PluginState& st, const std::string& relName, const std::string& source = "",
                bool exact = true);

// Effect chain: up to 10 slot states (empty type = empty slot) and their mix levels.
struct ChainPreset {
  std::vector<PluginState> slots;
  std::vector<float> mix;
  std::string source;
};
std::vector<PresetEntry> listChains(const std::string& root);
bool loadChain(const std::string& file, ChainPreset& out);
bool saveChain(const std::string& root, const ChainPreset& chain, const std::string& relName);

// Automation shape: a point list saved independently of any target. Files
// store positions in beats, so shapes keep their length at any PPQ.
struct AutomationShape {
  std::vector<AutoPoint> points;  // ticks at the PPQ given to load/save, from 0
  std::string source;             // optional note (e.g. "FL Studio automation preset")
};
std::vector<PresetEntry> listAutomationShapes(const std::string& root);
bool loadAutomationShape(const std::string& file, int ppq, AutomationShape& out);
bool saveAutomationShape(const std::string& root, const AutomationShape& shape, int ppq, const std::string& relName);

// Makes a preset name safe as a file name (keeps letters, digits, spaces, - _ . ( ) &).
std::string safePresetName(const std::string& name);

}  // namespace gl
