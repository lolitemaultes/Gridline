// Gridline — "dynamics" pack: runtimes (dispatch; the effects live in
// master_dsp.cpp, dist_dsp.cpp and rack_dsp.cpp).
#include "../Packs.h"
#include "dynamics.h"

namespace gl::packs::dynamics {

std::unique_ptr<Generator> createGenerator(const std::string& type) {
  (void)type;
  return nullptr;
}

std::unique_ptr<Effect> createEffect(const std::string& type) {
  if (type.compare(0, 3, "gl.") != 0) return nullptr;
  if (auto fx = createMasterEffect(type)) return fx;
  if (auto fx = createDistEffect(type)) return fx;
  if (auto fx = createRackEffect(type)) return fx;
  return nullptr;
}

bool prepareAux(const PluginState& state, ChannelAux& aux) {
  (void)state;
  (void)aux;
  return false;
}

}  // namespace gl::packs::dynamics
