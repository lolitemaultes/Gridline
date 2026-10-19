// Gridline — "synths" pack, J-22 group "dco": runtimes (DCO Synth in
// dcosynth_dsp.cpp, Warm Trio in warmsynth_dsp.cpp, FM Duo in fmduo_dsp.cpp).
#include "../Packs.h"

namespace gl::packs::synths {

std::unique_ptr<Generator> makeDcoSynth();   // dcosynth_dsp.cpp
std::unique_ptr<Generator> makeWarmSynth();  // warmsynth_dsp.cpp
std::unique_ptr<Generator> makeFmDuo();      // fmduo_dsp.cpp

std::unique_ptr<Generator> createDcoGenerator(const std::string& type) {
  if (type == "gl.dcosynth") return makeDcoSynth();
  if (type == "gl.warmsynth") return makeWarmSynth();
  if (type == "gl.fmduo") return makeFmDuo();
  return nullptr;
}

bool prepareDcoAux(const PluginState& state, ChannelAux& aux) {
  if (state.type == "gl.dcosynth" || state.type == "gl.warmsynth" || state.type == "gl.fmduo") {
    aux = ChannelAux{};
    return true;
  }
  return false;
}

}  // namespace gl::packs::synths
