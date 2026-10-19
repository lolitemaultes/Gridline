// Gridline — "synths" pack: runtimes (Analog in analog_dsp.cpp, Wavetable in
// wavetable_dsp.cpp, Matrix FM in matrixfm_dsp.cpp).
#include "../Packs.h"
#include "synths_common.h"

namespace gl::packs::synths {

std::unique_ptr<Generator> createGenerator(const std::string& type) {
  if (type == "gl.analog") return makeAnalog();
  if (type == "gl.wavetable") return makeWavetable();
  if (type == "gl.matrixfm") return makeMatrixFm();
  if (type == "gl.gms") return makeGms();
  return nullptr;
}

std::unique_ptr<Effect> createEffect(const std::string& type) {
  (void)type;
  return nullptr;
}

bool prepareAux(const PluginState& state, ChannelAux& aux) {
  if (state.type == "gl.matrixfm") {
    aux.custom = mfm::buildMatrixFmAux(state);
    return true;
  }
  if (state.type == "gl.gms") {
    aux = ChannelAux{};
    aux.custom = buildGmsAux(state);
    return true;
  }
  // Analog / Wavetable need no prepared data; claim the types so the core leaves aux empty.
  return state.type == "gl.analog" || state.type == "gl.wavetable";
}

}  // namespace gl::packs::synths
