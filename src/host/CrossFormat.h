// Gridline: opening a project whose third-party plugin is installed here in another format (K-78).
//
// A project made where a plugin was an Audio Unit opens on a computer that has only its VST3 (or VST2,
// or CLAP) version with that version, its settings carried over:
//   1. saving: every hosted plugin keeps an identity record next to its state ("ident" in the project
//      file, PluginState::ident): name, vendor, kind, main channels, parameter names, and the types the
//      same plugin had in the other formats installed where it was saved (PluginIdentity.h);
//   2. opening (resolveProjectJson, before the stand-ins): a plugin whose type is not installed is
//      matched against the installed classes; a confident match (PluginIdentity.h autoPick, the
//      preferred format first) takes its place;
//   3. its state: the plugin's own state is moved into the new format's wrapping (StateConvert.h),
//      and every parameter whose name matches gets the saved value; automation follows the parameters;
//   4. loading: the plugin reads the moved state; its parameters are read back and compared with the
//      saved values, and when they disagree the saved values are set one by one (HostedPlugins.cpp);
//   5. the load report says what happened to each plugin; a plugin with no confident match stays a
//      placeholder and its editor offers the possible matches.
// Nothing is replaced while the saved plugin is installed. A plugin that is not installed in any
// format stays as it was (Serum, Kickstart and the Kilohearts effects then get their stand-ins).
#pragma once
#include <string>
#include <vector>
#include "../core/Json.h"
#include "../core/PluginInfo.h"
#include "PluginIdentity.h"

namespace gl {
struct Song;
}

namespace gl::host::xformat {

// What opening did with one saved plugin. Kept in the identity record ("load") while the project is
// open; never saved.
struct LoadNote {
  std::string from;        // the saved type
  std::string to;          // the type it opened as ("" = still missing)
  std::string name;        // the plugin's name
  std::string confidence;  // ident::confidenceName
  std::string via;         // "state" (its own state moved), "parameters" (values by name), "none", "missing"
  std::string how;         // English: how the state was moved ("AU class info (JUCE state) -> VST3 component state")
  int matched = 0, total = 0;     // saved parameters found in the new format
  std::vector<int> verify;        // new parameter indices whose loaded values must equal the saved ones
  std::vector<std::string> suggest;  // a missing plugin: possible matches, best first
};
bool loadNote(const PluginState& st, LoadNote& out);
Json toJson(const LoadNote& n);
bool fromJson(const Json& j, LoadNote& n);

// Installed here: registered and not hidden (tests and checks hide plugins with GRIDLINE_HIDE_PLUGINS).
bool installed(const std::string& type);

// The identity a state is saved with (Json null for built-in plugins): from its registered class, with
// the other formats of the same plugin registered here and the ids the state's own record had.
Json identityForSave(const PluginState& st);
// The identity of a saved state: its record, else its registered class, else what its type says
// (`hintName`, e.g. the channel's name, as a weak name).
ident::Identity savedIdentity(const PluginState& st, const std::string& hintName = {});
// Installed classes that may be the saved plugin, best first (preferred format order of the plugin
// manager, hidden plugins left out).
std::vector<ident::Candidate> candidatesFor(const ident::Identity& id);

// The saved state as registered type `target`: its own state moved when the format allows it, the
// saved parameter values by name, the identity (aliases keep the saved type). `indexMap` gets the
// new index of every saved parameter (-1 = none).
struct Conversion {
  PluginState state;
  LoadNote note;
  std::vector<int> indexMap;
};
bool convert(const PluginState& saved, const ident::Identity& id, const HostedClass& target, ident::Confidence conf, Conversion& out);

// Project documents (Project.cpp, before the stand-ins): plugins that are not installed open as an
// installed format of the same plugin; `report` receives one line per change.
void resolveProjectJson(Json& project, std::vector<std::string>* report = nullptr);
// Load report: one line per plugin that opened in another format or stays missing.
std::vector<std::string> summary(const Song& song);
std::string describe(const LoadNote& n);

// The plugin's loaded parameters were compared with the saved values (HostedPlugins.cpp). Lines for
// the load report, taken once.
void postLoadCheck(const std::string& line);
std::vector<std::string> takeLoadChecks();

}  // namespace gl::host::xformat

namespace gl::host {
// `gridline plugin-xformat <from type> <to type>...` (CrossFormatCheck.cpp).
int crossFormatCheckMain(int argc, char** argv);
}  // namespace gl::host
