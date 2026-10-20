// Gridline — "synths" pack: runtimes (Analog in analog_dsp.cpp, Wavetable in
// wavetable_dsp.cpp, Matrix FM in matrixfm_dsp.cpp).
#include "../Packs.h"
#include "synths_common.h"

namespace gl::packs::synths {

// J-22 groups (va_dsp.cpp, dco_dsp.cpp).
std::unique_ptr<Generator> createVaGenerator(const std::string& type);
bool prepareVaAux(const PluginState& state, ChannelAux& aux);
std::unique_ptr<Generator> createDcoGenerator(const std::string& type);
bool prepareDcoAux(const PluginState& state, ChannelAux& aux);
// J-33 group "hex" (hexsynth_dsp.cpp).
std::unique_ptr<Generator> makeHexSynth();
bool prepareHexAux(const PluginState& state, ChannelAux& aux);
std::unique_ptr<Generator> makePocketSynth();  // pocket_dsp.cpp (J-33)
bool preparePocketAux(const PluginState& state, ChannelAux& aux);
std::unique_ptr<Generator> makePixelTone();  // pixel_dsp.cpp (J-33)
std::unique_ptr<Generator> makeSampleKeys();  // samplekeys_dsp.cpp (J-33)
bool prepareSampleKeysAux(const PluginState& state, ChannelAux& aux);
bool preparePixelAux(const PluginState& state, ChannelAux& aux);

std::unique_ptr<Generator> createGenerator(const std::string& type) {
  if (type == "gl.analog") return makeAnalog();
  if (type == "gl.wavetable") return makeWavetable();
  if (type == "gl.matrixfm") return makeMatrixFm();
  if (type == "gl.gms") return makeGms();
  if (auto g = createVaGenerator(type)) return g;
  if (auto g = createDcoGenerator(type)) return g;
  if (type == "gl.hexsynth") return makeHexSynth();
  if (type == "gl.pocketsynth") return makePocketSynth();
  if (type == "gl.pixeltone") return makePixelTone();
  if (type == "gl.samplekeys") return makeSampleKeys();
  return nullptr;
}

std::unique_ptr<Effect> createEffect(const std::string& type) {
  (void)type;
  return nullptr;
}

bool prepareAux(const PluginState& state, ChannelAux& aux) {
  if (prepareVaAux(state, aux) || prepareDcoAux(state, aux) || prepareHexAux(state, aux) || preparePocketAux(state, aux) || preparePixelAux(state, aux) ||
      prepareSampleKeysAux(state, aux))
    return true;
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
