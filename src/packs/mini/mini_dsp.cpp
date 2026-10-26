// Gridline — "mini" pack: runtime entry points (effects only; each family
// lives in its own mini_<family>_dsp.cpp).
#include "../Packs.h"
#include "mini_rt.h"

namespace gl::packs::mini {

std::unique_ptr<Generator> createGenerator(const std::string&) { return nullptr; }

std::unique_ptr<Effect> createEffect(const std::string& type) {
  if (type.compare(0, 7, "gl.mini") != 0 || !findEntry(type)) return nullptr;
  if (auto e = rt::createUtility(type)) return e;
  if (auto e = rt::createTone(type)) return e;
  if (auto e = rt::createDynamics(type)) return e;
  if (auto e = rt::createDrive(type)) return e;
  if (auto e = rt::createMotion(type)) return e;
  return rt::createTime(type);
}

bool prepareAux(const PluginState&, ChannelAux&) { return false; }

}  // namespace gl::packs::mini
