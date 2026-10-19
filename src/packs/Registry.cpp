// Gridline — plugin pack registry (engine side). Add a pack here and in RegistryUi.cpp.
#include "Packs.h"

GL_DECLARE_PACK(synths)
GL_DECLARE_PACK(samplers)
GL_DECLARE_PACK(spacefx)
GL_DECLARE_PACK(spectral)
GL_DECLARE_PACK(dynamics)
GL_DECLARE_PACK(modulation)
GL_DECLARE_PACK(models)
GL_DECLARE_PACK(utility)
GL_DECLARE_PACK(creative)
GL_DECLARE_PACK(edison)

#define GL_PACK_LIST X(synths) X(samplers) X(spacefx) X(spectral) X(dynamics) X(modulation) X(models) X(utility) X(creative) X(edison)

namespace gl::packs {

void appendInfos(std::vector<PluginInfo>& v) {
#define X(P) P::appendInfos(v);
  GL_PACK_LIST
#undef X
}

std::unique_ptr<Generator> createGenerator(const std::string& type) {
#define X(P) \
  if (auto g = P::createGenerator(type)) return g;
  GL_PACK_LIST
#undef X
  return nullptr;
}

std::unique_ptr<Effect> createEffect(const std::string& type) {
#define X(P) \
  if (auto fx = P::createEffect(type)) return fx;
  GL_PACK_LIST
#undef X
  return nullptr;
}

bool prepareAux(const PluginState& state, ChannelAux& aux) {
#define X(P) \
  if (P::prepareAux(state, aux)) return true;
  GL_PACK_LIST
#undef X
  return false;
}

}  // namespace gl::packs
