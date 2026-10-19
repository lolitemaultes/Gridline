// Gridline — FLP import: FL effects plugin states → Gridline equivalents.
// The decoders live in one file per effect family (FlpFx*.cpp).
#include "FlpFx.h"

namespace gl::flp {

void registerEffectDecoders(std::map<std::string, PluginDecoder>& d) {
  fx::registerFxEq(d);
  fx::registerFxTime(d);
  fx::registerFxDyn(d);
  fx::registerFxMisc(d);
  fx::registerFxK3(d);
}

}  // namespace gl::flp
