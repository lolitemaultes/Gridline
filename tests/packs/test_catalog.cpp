// Gridline — whole-catalogue checks across the built-in plugins and every pack.
#include <set>
#include <string>
#include "../../src/core/Params.h"
#include "../../src/core/PluginInfo.h"
#include "../../src/engine/Effects.h"
#include "../../src/engine/Generators.h"
#include "../Check.h"

using namespace gl;

int main() {
  const auto& cat = pluginCatalog();
  std::set<std::string> types, names;
  bool fitsIds = true;
  bool uniqueTypes = true, uniqueNames = true, prefixed = true, runtimes = true, presetsOk = true;
  for (const auto& p : cat) {
    uniqueTypes = uniqueTypes && types.insert(p.type).second;
    uniqueNames = uniqueNames && names.insert(p.name).second;
    prefixed = prefixed && p.type.rfind("gl.", 0) == 0;
    if (p.kind == PluginKind::Generator) runtimes = runtimes && createGenerator(p.type) != nullptr;
    if (p.kind == PluginKind::Effect) runtimes = runtimes && createEffect(p.type) != nullptr;
    for (size_t i = 0; i < p.presets.size(); ++i) {
      PluginState s = presetState(p, i);
      presetsOk = presetsOk && s.params.size() == p.params.size();
    }
    // Every parameter must have its own ParamId (no aliasing between slots / note processors).
    const size_t n = p.params.size();
    bool fit = p.kind == PluginKind::Effect      ? n < kSlotMix
               : p.kind == PluginKind::Generator ? n < kNoteFxBase - kChannelPluginBase
                                                 : n <= kNoteFxStride;
    if (!fit) std::printf("# too many params for its ParamId range: %s (%zu)\n", p.type.c_str(), n);
    fitsIds = fitsIds && fit;
    if (!uniqueTypes || !uniqueNames) std::printf("# duplicate: %s / %s\n", p.type.c_str(), p.name.c_str());
  }
  expect(cat.size() > 100, "catalogue has the full plugin set");
  expect(uniqueTypes, "plugin types are unique");
  expect(uniqueNames, "plugin display names are unique");
  expect(prefixed, "plugin types use the gl. prefix");
  expect(runtimes, "every instrument and effect has a runtime");
  expect(fitsIds, "every parameter has its own ParamId");
  expect(presetsOk, "presets resolve to full parameter vectors");
  std::printf("# %zu plugins\n", cat.size());
  return finish();
}
