// Gridline — "modulation" pack: functions shared between the pack's runtime
// files and its editors (all UI-thread safe; none are called per sample
// except tensionCurve / lfoWave).
#pragma once
#include <memory>
#include <string>
#include <vector>
#include "../../core/Expr.h"
#include "../../core/PluginInfo.h"
#include "../../engine/Effects.h"
#include "../../engine/Generators.h"

namespace gl::packs::modulation {

// Bends 0..1 by tension t in -1..1 (0 = straight, > 0 slow start, < 0 fast start).
double tensionCurve(double x, double t);
// LFO shapes 0..4 (sine, triangle, square, saw, random) over a phase in cycles, 0..1.
double lfoWave(int shape, double phase, uint64_t cycle);

// Formula controller: compiles its formula (variables a b c t beat bar tempo in playing).
bool compileFormula(const std::string& text, Expr& out, std::string* err);
// Keyboard controller key table <-> PluginState::data.
std::vector<float> parseKeyValues(const std::string& data);
std::string formatKeyValues(const std::vector<float>& v);

// Chord generator: the chord's intervals (semitones from the played note,
// sorted) for a state; returns the count (<= 8).
int chordVoicing(const PluginState& s, float* notes);

// Note script: compiles its three formulas (pitch, velocity, play) from data.
struct NoteScriptProgram {
  Expr pitch, velocity, play;
  std::string error;  // first compile error, empty when all compiled
};
void compileNoteScript(const std::string& body, NoteScriptProgram& out);
// Default script text and the variable names scripts may use.
const char* defaultNoteScript();
const std::vector<std::string>& noteScriptVars();

// Patch host: a small graph of other plugins kept in PluginState::data.
struct PatchNode {
  PluginState plugin;       // hosted plugin (generator first, then effects)
  bool parallel = false;    // effects: runs alongside the previous effect instead of after it
  float mix = 1.0f;         // effects: dry/wet
  bool bypass = false;
};
struct PatchMacroTarget {
  int node = 0;
  int param = 0;
  float min = 0, max = 1;  // normalized range of the target driven by the macro
};
struct PatchGraph {
  std::vector<PatchNode> nodes;
  std::vector<PatchMacroTarget> macros[8];
  std::string macroNames[8];
};
PatchGraph parsePatchGraph(const std::string& data);
// The graph a host state runs (default graph for empty data, unhostable nodes removed).
PatchGraph patchGraphOf(const PluginState& state);
std::string serializePatchGraph(const PatchGraph& g);
std::unique_ptr<Generator> createPatchHostGenerator();
std::unique_ptr<Effect> createPatchHostEffect();
void preparePatchHost(const PluginState& state, ChannelAux& aux);

}  // namespace gl::packs::modulation
