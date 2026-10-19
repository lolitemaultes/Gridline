// Gridline — "synths" pack, J-22 group "va": runtimes (Twin VA in
// twinva_dsp.cpp, Saw Swarm in sawswarm_dsp.cpp, Finger Bass in
// fingerbass_dsp.cpp).
#include "../Packs.h"
#include "va_fx.h"

namespace gl::packs::synths {

namespace va {
std::unique_ptr<Generator> makeTwinVa();
std::unique_ptr<Generator> makeSawSwarm();
std::unique_ptr<Generator> makeFingerBass();
}  // namespace va

std::unique_ptr<Generator> createVaGenerator(const std::string& type) {
  if (type == "gl.twinva") return va::makeTwinVa();
  if (type == "gl.sawswarm") return va::makeSawSwarm();
  if (type == "gl.fingerbass") return va::makeFingerBass();
  return nullptr;
}

bool prepareVaAux(const PluginState& state, ChannelAux& aux) {
  if (state.type == "gl.twinva" || state.type == "gl.sawswarm" || state.type == "gl.fingerbass") {
    aux = ChannelAux{};
    aux.custom = va::parseCcLinks(state.data);
    return true;
  }
  return false;
}

}  // namespace gl::packs::synths
