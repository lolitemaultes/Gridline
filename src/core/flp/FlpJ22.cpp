// Gridline — FLP import, J-22: registry of the tier-C instrument decoders.
#include "FlpJ22.h"

namespace gl::flp {

void registerDecodersJ22(std::map<std::string, PluginDecoder>& d) {
  j22::registerDrums(d);
  j22::registerStrings(d);
  j22::registerMetal(d);
  j22::registerVa(d);
  j22::registerDco(d);
}

}  // namespace gl::flp
