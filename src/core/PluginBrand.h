// Gridline — plugin brand system (J-94): flagship names, families, taglines
// and search keywords for the built-in plugins.
//
// Two tiers:
//  * Flagships carry an original one-word name (Lattice, Shuttle, Kerf, ...),
//    a family (accent colour role in the UI) and a one-line tagline. Their
//    editors show the name as a wordmark.
//  * Studio tools and utilities keep plain descriptive names (Compressor,
//    Parametric EQ, Arpeggiator); they have no family.
// Every renamed plugin keeps its former name among its search keywords, so the
// picker, the browser and the MCP catalogue still find "time gate" or "reverb".
// Display names themselves live in PluginInfo::name (the catalogue); type ids
// ("gl.*") never change.
#pragma once
#include <string>
#include <vector>

namespace gl {

enum class BrandFamily {
  None,       // descriptive tool: no wordmark
  Synth,      // sound sources
  Sample,     // samplers, slicers, drum machines, the audio editor
  Space,      // time and space: delay, reverb, time shaping
  Dynamics,   // loudness and mastering
  Voice,      // voice processing
  Structure,  // patching and measuring
};

struct PluginBrand {
  const char* type;
  BrandFamily family;
  const char* tagline;   // flagships: one line ("" for tools)
  const char* keywords;  // lower-case search terms, comma separated: former names, functions
};

// Brand entry of a built-in type; nullptr when the type has none.
const PluginBrand* pluginBrand(const std::string& type);
// Every brand entry (flagships and renamed tools).
const std::vector<PluginBrand>& pluginBrands();
// True for flagships (a family other than None).
bool isFlagship(const std::string& type);
// Lower-case search keywords ("" when none).
std::string pluginKeywords(const std::string& type);
// Tagline of a flagship ("" for tools).
std::string pluginTagline(const std::string& type);
// Display name in capitals, for editor title strips drawn by plugin skins
// (falls back to the type id for unknown types).
std::string pluginTitle(const std::string& type);
// Display name of a type (the catalogue name; the type id when unknown).
std::string pluginDisplayName(const std::string& type);

}  // namespace gl
