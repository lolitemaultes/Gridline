// Gridline — "models" pack, J-22 group "strings": runtimes (Twin String in
// twinstring_dsp.cpp, Plucker in plucker_dsp.cpp, Step Bass in
// stepbass_dsp.cpp).
#include "../Packs.h"
#include "strings.h"

namespace gl::packs::models {

std::unique_ptr<Generator> makeTwinString();  // twinstring_dsp.cpp
std::unique_ptr<Generator> makePlucker();     // plucker_dsp.cpp
std::unique_ptr<Generator> makeStepBass();    // stepbass_dsp.cpp
std::shared_ptr<const AuxData> buildStepBassAux(const PluginState& s);

std::unique_ptr<Generator> createStringsGenerator(const std::string& type) {
  if (type == "gl.twinstring") return makeTwinString();
  if (type == "gl.plucker") return makePlucker();
  if (type == "gl.stepbass") return makeStepBass();
  return nullptr;
}

bool prepareStringsAux(const PluginState& state, ChannelAux& aux) {
  if (state.type == "gl.stepbass") {
    aux = ChannelAux{};
    aux.custom = buildStepBassAux(state);
    return true;
  }
  if (state.type == "gl.twinstring" || state.type == "gl.plucker") {
    aux = ChannelAux{};
    return true;
  }
  return false;
}

}  // namespace gl::packs::models
