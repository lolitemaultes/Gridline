// Gridline: built-in stand-ins for a few third-party plugins that are not
// installed (user decision, see AGENTS.md). Serum and Serum 2 open as Plasma
// ("gl.plasma"), Kickstart and Kickstart 2 as Tideline ("gl.tideline"), and
// the Kilohearts single effects as their Gridline Suite Mini counterparts
// (packs/mini/mini_khs.h: one table of identifiers and parameter mappings).
//
// A stand-in is an ordinary built-in plugin state whose PluginState::original
// holds the plugin it replaces: its type, parameters and its own state, never
// altered, plus the automation items that were moved to the stand-in or set
// aside. Projects save the stand-in state with this record ("original" next
// to the plugin); when the real plugin is installed again the project opens
// with the real plugin, its state and automation targets restored. Nothing is
// substituted while the plugin is installed.
//
// Conversion: the plugin's saved state is decoded (Serum 2 state containers,
// Serum 1 chunks, Kickstart 2 value trees) and mapped onto the stand-in's
// parameters; each automated parameter of the original gets a link to one
// stand-in parameter with a measured value curve (see plasma.h, tideline.h).
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "Json.h"
#include "PluginInfo.h"

namespace gl {
struct Song;
}

namespace gl::standin {

enum class Family { None, Serum1, Serum2, Kickstart1, Kickstart2, Kilohearts };

// The family of a hosted type (by its plugin ids, else by name + vendor).
Family familyOf(const std::string& type, const std::string& name = "", const std::string& vendor = "");
const char* familyName(Family f);       // "Serum", "Serum 2", "Kickstart", "Kickstart 2", "Kilohearts"
const char* standInType(Family f);      // "gl.plasma", "gl.tideline" ("" for None and Kilohearts: one per plugin)
const char* standInName(Family f);      // "Plasma", "Tideline", "Gridline Suite Mini"
// The stand-in of one plugin: its built-in type ("gl.minipress") and the
// name the plugin goes by ("kHs Compressor"; the family name for the others).
std::string standInTypeOf(const std::string& type, const std::string& name = "", const std::string& vendor = "");
std::string pluginNameOf(const std::string& type, const std::string& name = "", const std::string& vendor = "");

// Plugins treated as not installed: the environment variable
// GRIDLINE_HIDE_PLUGINS (comma-separated hosted types, plugin names or
// family words: "serum", "kickstart", "kilohearts", "all") plus setHiddenPlugins (tests).
void setHiddenPlugins(const std::vector<std::string>& entries);
bool hidden(const std::string& type, const std::string& name = "");
// The hosted class is registered (scanned) and not hidden.
bool installed(const std::string& type);

// What a stand-in keeps of the plugin it replaces.
struct Original {
  std::string type, name, vendor;
  std::vector<float> params;  // the plugin's parameters as saved
  std::string data;           // the plugin's own state (untouched)
  std::string ins;
  std::string format;               // decoded state, e.g. "Serum 2 (VST3 state)"
  std::vector<std::string> notes;   // what the stand-in only approximates
  // Automation items (project JSON objects, compact text) by list: "automations",
  // "links", "modLinks", "pattern:<id>".
  struct Item {
    std::string list, now, was;  // `now`: the item as moved to the stand-in ("" = set aside)
  };
  std::vector<Item> items;
};
std::string toText(const Original& o);
bool fromText(const std::string& text, Original& o);

// Converted state of one plugin.
struct Conversion {
  PluginState state;
  std::string format, error;
  std::vector<std::string> notes;
};
// Decodes `orig` (a hosted state of family f) into the stand-in's state.
bool convert(Family f, const PluginState& orig, Conversion& out);

// Link of the original's parameter `index` (position in its hosted type's
// parameter list) to a stand-in parameter: `curve` holds the stand-in's
// normalized value at original values i / (curve.size() - 1).
struct ParamLink {
  int standin = -1;
  std::vector<float> curve;
  std::string title;  // the original parameter's name
  float map(float n) const;      // original normalized -> stand-in normalized
  float unmap(float n) const;    // inverse (nearest; curves are monotone or flat)
};
bool paramLink(Family f, const PluginState& orig, int index, ParamLink& out);
// Parameter index (hosted type's list) of a plugin parameter id (VST3 / AU
// parameter id; VST2: the index itself), -1 when unknown.
int paramIndexOfId(Family f, const std::string& type, uint32_t id);

// Builds the stand-in state for `orig` (not installed). False when the
// family is unknown or the state cannot be read at all (the original then
// stays a missing hosted plugin).
bool makeStandIn(const PluginState& orig, const std::string& name, const std::string& vendor, PluginState& out,
                 std::vector<std::string>* notes = nullptr);
bool isStandIn(const PluginState& s);
// The original state kept by a stand-in (empty when none).
PluginState originalState(const PluginState& s);
// UI text: "Serum 2 is not installed: Plasma plays its sound" ("Not installed:
// kHs Compressor, using Gridline Suite Mini Press" for Kilohearts) and the notes.
std::string describe(const PluginState& s);
std::vector<std::string> notes(const PluginState& s);
// One line per stand-in of the song ("Serum 2 (Lead) is not installed: Plasma plays it").
std::vector<std::string> summary(const Song& song);

// Project documents (Project.cpp, after the schema migration): plugins of
// the families above that are not installed become stand-ins, stand-ins whose
// plugin is installed again are restored, and automation targets follow.
// `report` receives one line per change.
void resolveProjectJson(Json& project, std::vector<std::string>* report = nullptr);
// Every stand-in back to its original, whether installed or not (FL export).
void restoreProjectJson(Json& project);
// The same on a Song (through the project document).
void restoreAll(Song& song);
// While alive, songFromJson leaves stand-ins as they are (restoreAll).
struct NoResolve {
  NoResolve();
  ~NoResolve();
  NoResolve(const NoResolve&) = delete;
  NoResolve& operator=(const NoResolve&) = delete;
};
bool resolving();

}  // namespace gl::standin
