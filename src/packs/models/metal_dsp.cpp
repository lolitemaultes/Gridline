// Gridline — "models" pack, J-22 group "metal": runtime dispatch and prepared
// data (Alloy curves, Spectra Morph programs, the samplers' samples).
#include "../Packs.h"
#include <map>
#include <mutex>
#include "../../engine/Slicing.h"
#include "../samplers/samplers.h"
#include "metal_rt.h"

namespace gl::packs::models {

std::unique_ptr<Generator> createMetalGenerator(const std::string& type) {
  if (type == "gl.alloy") return metal::makeAlloy();
  if (type == "gl.spectramorph") return metal::makeSpectraMorph();
  if (type == "gl.grainscan") return metal::makeGrainScan();
  if (type == "gl.splinescratch") return metal::makeSplineScratch();
  return nullptr;
}

namespace {
// First line of the data: the sample path (channel-sample convention).
std::string firstLine(const std::string& d) {
  const size_t nl = d.find('\n');
  std::string s = nl == std::string::npos ? d : d.substr(0, nl);
  if (!s.empty() && s.back() == '\r') s.pop_back();
  return s;
}
}  // namespace

bool prepareMetalAux(const PluginState& state, ChannelAux& aux) {
  if (state.type == "gl.alloy") {
    aux = ChannelAux{};
    aux.custom = metal::buildAlloyAux(state);
    return true;
  }
  if (state.type == "gl.spectramorph") {
    aux = ChannelAux{};
    aux.custom = metal::buildSpectraMorphAux(state);
    return true;
  }
  if (state.type == "gl.grainscan" || state.type == "gl.splinescratch") {
    aux = ChannelAux{};
    const std::string path = firstLine(state.data);
    // No sample yet: the built-in scratch phrase, so a fresh instance plays.
    aux.samples.push_back(path.empty() ? samplers::scratchDefaultSample() : sampleCache().get(path));
    // Grain Scanner's transient key mode: the sample's transients (detected once here).
    if (state.type == "gl.grainscan" && aux.sample(0)) {
      static std::mutex mu;
      static std::map<const Sample*, std::pair<SamplePtr, std::vector<int64_t>>> cache;  // keeps the sample alive: keys stay unique
      std::lock_guard<std::mutex> lk(mu);
      auto it = cache.find(aux.sample(0).get());
      if (it == cache.end()) {
        if (cache.size() > 64) cache.clear();
        it = cache.emplace(aux.sample(0).get(), std::make_pair(aux.sample(0), computeSlices(*aux.sample(0), 0, 0.5))).first;
      }
      aux.slices = it->second.second;
      // J-92: transient hold (ms): transients closer than this to the previous one are skipped.
      const double holdMs = state.params.size() > static_cast<size_t>(metal::GsTransHold) ? state.params[metal::GsTransHold] : 228.0;
      const double gap = std::max(0.0, holdMs) * 0.001 * aux.sample(0)->sampleRate;
      std::vector<int64_t> kept;
      for (int64_t s : aux.slices)
        if (kept.empty() || static_cast<double>(s - kept.back()) >= gap) kept.push_back(s);
      aux.slices = std::move(kept);
    }
    if (state.type == "gl.splinescratch") {
      auto d = std::make_shared<metal::SplineScratchAux>();
      metal::parseScratchSpl(state.data, d->d);
      aux.custom = d;
    }
    return true;
  }
  return false;
}

}  // namespace gl::packs::models
