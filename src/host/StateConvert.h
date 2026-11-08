// Gridline: moving a plugin's own state between its formats (K-78).
//
// Each format wraps the plugin's state its own way (PluginState::data, HostedPlugins.h):
//   VST2  "vst2c1:<chunk>" (the plugin's chunk as is) or "vst2p1:<program, parameter values>"
//   VST3  "vst3s1:<component stream>:<controller stream>"
//   AU    "au1:<ClassInfo property list>": a dictionary with the unit's type, subtype, manufacturer,
//         saved-state version and name, plus what the plugin adds (a JUCE build keeps its state as
//         data under "jucePluginState")
//   CLAP  "clap1:<the plugin's state stream>"
// Most plugins come from one code base per format, and the bytes inside the wrapping are the same in
// every format. unwrap takes the wrapping off (and what a framework adds around the state: a JUCE VST3
// build's trailing private block, the VST2-compatible fxb bank a JUCE VST3 build that replaces its VST2
// writes); rewrap puts the bytes into the wrapping of another format.
//
// Whether the plugin then takes the state is checked when it loads, by reading its parameters back
// (HostedPlugins.cpp); when it does not, the parameters are set by name instead (CrossFormat.h).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace gl::host::xstate {

struct Core {
  bool ok = false;
  std::string bytes;      // the plugin's own state, without its format's wrapping
  std::string framework;  // "juce" when the state is known to be a JUCE plugin's; "" not known
  std::string source;     // English: where the bytes came from ("AU class info, JUCE state")
  uint32_t vst2Id = 0;    // the VST2 id the bytes belong to, when known (fxb / fxp header, VST2-compatible ids)
  int fxCount = 0;        // parameters / programs count of an fxp / fxb header (0 = not known)
  // VST2 plugins without a chunk: the program and every parameter value, by index.
  bool paramList = false;
  int32_t program = -1;
  std::vector<float> values;
};
// The state inside a PluginState::data string. False when there is none or the prefix is unknown.
bool unwrap(const std::string& data, Core& out);
// `core` as PluginState::data for hosted type `targetType` ("vst3:...", "au:aufx/ABCD/Wxyz", ...).
// "" when the format cannot take it (LV2, a VST2 parameter list for another format). `how` (English)
// says what was written.
// `targetParams`: the target's parameter count (written into a VST2 program header).
std::string rewrap(const Core& core, const std::string& targetType, const std::string& targetName, std::string* how = nullptr, int targetParams = 0);

// ---- the pieces (tests) -------------------------------------------------------------------------
// VST2 "fxb" bank holding a chunk ("CcnK" ... "FBCh"), as VST2 hosts and JUCE VST3 builds that replace
// their VST2 write it; with `vstwHeader` the JUCE VST3 "VstW" block first.
std::string fxBankChunk(uint32_t fxId, int32_t fxVersion, const std::string& chunk, bool vstwHeader);
// The chunk inside an fxb bank / fxp program chunk ("VstW" block optional). False when not one.
bool fxChunkPayload(const std::string& bytes, std::string& chunk, uint32_t* fxId = nullptr, int* fxCount = nullptr);
// VST2 "fxp" program holding a chunk ("CcnK" ... "FPCh"), as a VST2-based Audio Unit keeps it under "vstdata".
std::string fxProgramChunk(uint32_t fxId, int32_t fxVersion, int numParams, const std::string& name, const std::string& chunk);
// An Audio Unit ClassInfo property list (XML) of a VST2-based unit: `program` (an fxp) under "vstdata",
// and `pluginState` (when not empty) under "jucePluginState" too.
std::string auVstClassInfoXml(uint32_t type, uint32_t subtype, uint32_t manufacturer, const std::string& program, const std::string& pluginState = {});
// JUCE VST3 component state: the plugin's state followed by a private block and its size, ending
// with the text "JUCEPrivateData". The state without it; false when there is no such trailer.
bool stripJucePrivateData(const std::string& bytes, std::string& state);
// An Audio Unit ClassInfo property list (XML) with `pluginState` under "jucePluginState".
std::string auClassInfoXml(uint32_t type, uint32_t subtype, uint32_t manufacturer, const std::string& name, const std::string& pluginState);

}  // namespace gl::host::xstate
