// Gridline: stand-ins for missing Kilohearts effects (J-132, user decision; see
// AGENTS.md and core/PluginStandIn.h). When a project uses one of the
// Kilohearts single effects and it is not installed, the matching Gridline
// Suite Mini effect plays in its place. Installed plugins are always hosted
// as themselves; the original plugin, its parameters and its own state stay
// untouched in the stand-in's PluginState::original.
//
// The table (mini_khs_info.cpp) holds identifiers only: the plugin names as hosts
// and projects store them, AU subtype (type aufx, manufacturer " kHs"), VST3
// class id and VST2 id, the parameter titles in the plugin's own order, and
// a mapping of the parameters whose meaning matches onto the Mini effect.
// Values are carried over from the saved parameters (never by parsing the
// plugin's opaque state); everything else stays at the Mini defaults.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "../../core/PluginInfo.h"

namespace gl::packs::mini::khs {

// A choice without a Mini counterpart: the Mini default stays.
inline constexpr float kKeepDefault = -1e30f;

// How a saved value (the plugin's normalized 0..1) reads as a plain value.
enum class Read : uint8_t {
  Lin,     // v = {lo, hi}: lo + (hi - lo) n
  Exp,     // v = {lo, hi}: lo (hi / lo)^n
  Points,  // v = 11 values the plugin displays at n = 0, 0.1 .. 1 (piecewise linear)
  Index,   // v = the Mini value for each of the plugin's choices (kKeepDefault: none)
  Beats,   // "steps" (1..64) times the product's "Delay Denominator": nearest of `beats`
};

struct Param {
  const char* title;  // as the plugin names it
  int vst3Id;         // its VST3 parameter id; -1: not in the VST3 list ("Enabled")
  int vst3Steps;      // > 1: a VST3 choice (Gridline saves the choice index); else 0 or 1
};

struct Map {
  const char* title;  // the plugin's parameter
  int mini;           // Mini parameter index
  Read read;
  std::vector<float> v;
  float mul = 1, add = 0;  // Mini value = plain * mul + add (Lin, Exp, Points), clamped to the Mini range
  const double* beats = nullptr;  // Read::Beats: the Mini choice list (beats per choice)
  int beatCount = 0;
};

struct Product {
  const char* name;   // "kHs Compressor": as hosts and projects name it
  const char* au;     // AU subtype
  const char* vst3;   // VST3 class id (32 hex digits)
  const char* vst2;   // VST2 unique id (8 hex digits)
  const char* mini;   // Gridline Suite Mini type
  std::vector<Param> params;  // AU / VST2 order; VST3 lists them without "Enabled"
  std::vector<Map> maps;
};

const std::vector<Product>& products();

// The product of a hosted type ("au:aufx/<sub>/ kHs", "vst3:<uid>", "vst2:<id>"),
// else by plugin name (vendor Kilohearts or unknown) when `name` is given.
const Product* find(const std::string& type, const std::string& name = "", const std::string& vendor = "");
// "au:aufx/kscp/ kHs", "vst3:...", "vst2:..." of a product.
std::string auType(const Product& p);
std::string vst3Type(const Product& p);
std::string vst2Type(const Product& p);

// Indices into Product::params of the hosted parameter list of `type`
// (`count`: the saved parameter count, for a type of unknown format).
std::vector<int> hostedList(const Product& p, const std::string& type, size_t count);

// The Mini state for a saved instance (`hosted`: its parameters as saved).
// `notes` lists what the stand-in only approximates.
bool toMini(const Product& p, const std::string& type, const std::vector<float>& hosted, PluginState& out, std::vector<std::string>& notes);
// Automation link of hosted parameter `index`: Mini parameter and its
// normalized value at n = i / (curve.size() - 1).
bool paramLink(const Product& p, const std::string& type, size_t count, int index, int& mini, std::vector<float>& curve, std::string& title);
// Hosted parameter index of a plugin parameter id (VST3 / AU id; VST2: the index), -1 when unknown.
int paramIndexOfId(const Product& p, const std::string& type, uint32_t id);

}  // namespace gl::packs::mini::khs
