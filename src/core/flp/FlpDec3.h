// Gridline — FLP import, third-wave decoders (preset coverage): shared helpers.
//
// Each group lives in its own file (FlpDec3<Group>.cpp) and registers from
// registerDecoders3 (FlpDecoders3.cpp). The byte helpers (Blob, begin, set,
// linkParams) are the second wave's (FlpGen2.h). State layouts were worked out
// from FL Studio's shipped presets (observation of stored values against the
// plugins' published controls); no Image-Line code is used.
#pragma once
#include <map>
#include <string>
#include <vector>
#include "FlpGen2.h"

namespace gl::flp::dec3 {

using gen2::Blob;
using gen2::Link;

// Automation range of one FL parameter as recorded in pattern events (the
// plugin's own integer value): raw `lo..hi` ↔ normalized 0..1.
struct RawRange {
  int fl = 0;
  double lo = 0, hi = 65536;
};

// m.param / m.value from `links` (gen2::linkParams) plus m.rawNorm from
// `ranges` (parameters without a range: NaN).
void linkAll(PluginMapping& m, std::vector<Link> links, std::vector<RawRange> ranges);

// Table-driven knobs: FL parameter `fl` (= control tag = automation index)
// stores an integer in lo..hi; `conv` turns it into the Gridline value of
// `gl` (empty: identity).
struct Row {
  int fl = 0;
  double lo = 0, hi = 1;
  int gl = -1;
  std::function<double(double flValue)> conv = {};
};
// Sets every row's Gridline parameter from `flByTag` (indexed by tag; rows
// past its end are skipped) and links param / value / rawNorm for all rows.
void applyRows(PluginMapping& m, const std::vector<Row>& rows, const std::vector<double>& flByTag);

// Adds `dec` under `name` unless an earlier wave registered it.
inline void add(std::map<std::string, PluginDecoder>& d, const std::string& name, PluginDecoder dec) {
  d.emplace(name, std::move(dec));
}

// Groups (one file each).
void registerVfx(std::map<std::string, PluginDecoder>& d);     // FlpDec3Vfx.cpp: VFX note effects
void registerKepler(std::map<std::string, PluginDecoder>& d);  // FlpDec3Kepler.cpp: Kepler Exo
void registerFx(std::map<std::string, PluginDecoder>& d);      // FlpDec3Fx.cpp: effects / controllers
void registerInst(std::map<std::string, PluginDecoder>& d);    // FlpDec3Inst.cpp: Plucked!, Transistor Bass, Slicer 2

}  // namespace gl::flp::dec3
