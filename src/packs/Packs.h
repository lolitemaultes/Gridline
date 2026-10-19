// Gridline — plugin packs. Each pack lives in src/packs/<pack>/ and adds
// plugins without touching the built-in catalogue, engine or editors:
//   <pack>_info.cpp  catalogue entries          (core)
//   <pack>_dsp.cpp   generator / effect runtime (engine)
//   <pack>_ui.cpp    custom editors             (app)
// The registry calls every pack in turn; a pack returns nullptr / false for
// types it does not own.
#pragma once
#include <memory>
#include <string>
#include <vector>
#include "../core/PluginInfo.h"
#include "../engine/Effects.h"
#include "../engine/Generators.h"

namespace gl::packs {

// Appends every pack's catalogue entries.
void appendInfos(std::vector<PluginInfo>& v);
// Runtime instance for a pack type; nullptr for unknown types.
std::unique_ptr<Generator> createGenerator(const std::string& type);
std::unique_ptr<Effect> createEffect(const std::string& type);
// Prepares data a pack plugin needs besides its parameters (samples, impulse
// responses, zone maps…) on the UI thread when a program is built. Returns
// true when a pack owns the type (aux is then left as the pack filled it).
bool prepareAux(const PluginState& state, ChannelAux& aux);

}  // namespace gl::packs

// Declares one pack's entry points (used by the registry and the packs).
#define GL_DECLARE_PACK(P)                                                     \
  namespace gl::packs::P {                                                     \
  void appendInfos(std::vector<PluginInfo>& v);                                \
  std::unique_ptr<Generator> createGenerator(const std::string& type);         \
  std::unique_ptr<Effect> createEffect(const std::string& type);               \
  bool prepareAux(const PluginState& state, ChannelAux& aux);                  \
  }
