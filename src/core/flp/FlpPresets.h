// Gridline — FL Studio preset (.fst) import into the local Gridline preset library.
#pragma once
#include <functional>
#include <map>
#include <string>
#include <vector>
#include "../Presets.h"
#include "Flp.h"

namespace gl::flp {

struct PresetResult {
  enum Kind { Plugin, Chain, Shape } kind = Plugin;
  std::string flPlugin;     // FL plugin internal name
  PluginState state;        // Plugin
  ChainPreset chain;        // Chain
  AutomationShape shape;    // Shape: automation-clip channel presets (points at kShapePpq)
  bool exact = false;
  std::vector<std::string> unmapped;  // FL plugins without a Gridline equivalent
  std::vector<std::string> notes;     // approximation notes from the decoders
  std::string error;
};

// Ticks per beat of PresetResult::shape (shape files store beats).
inline constexpr int kShapePpq = 960;

// Converts one .fst (plugin, channel or mixer preset).
bool importFlPreset(const std::string& path, PresetResult& out, const std::string& factoryData = "");

struct PresetLibraryReport {
  size_t total = 0, converted = 0, exact = 0, chains = 0, shapes = 0, failed = 0;
  bool cancelled = false;
  std::string error;
  std::map<std::string, int> perPlugin;        // FL plugin → presets converted
  std::map<std::string, int> unmappedPlugins;  // FL plugin → presets skipped (no equivalent)
  std::map<std::string, int> errors;           // other failures by reason
};

// Converts every preset of an FL Studio installation (plugin presets, channel
// presets, packs, mixer presets, presets inside plugin folders) into
// `presetRootDir` (see Presets.h), under "FL Studio/…". Automation-clip
// channel presets become automation shapes ("automation/FL Studio/…"). `progress` may cancel
// by returning false.
PresetLibraryReport importPresetLibrary(const std::string& factoryData, const std::string& presetRootDir,
                                        const std::function<bool(size_t done, size_t total, const std::string& file)>& progress = {});

}  // namespace gl::flp
