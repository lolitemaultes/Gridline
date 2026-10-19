// Gridline — "models" pack, J-22 group "metal": prepared data (UI thread →
// audio thread) and runtime factories.
#pragma once
#include <array>
#include <memory>
#include <vector>
#include "../../engine/Generators.h"
#include "metal.h"
#include "models_rt.h"

namespace gl::packs::models::metal {

struct SplineScratchAux final : AuxData {
  SsData d;
};

// Alloy: the patch with every curve pre-sampled.
inline constexpr int kAlloyEqPts = 256;  // timbre EQ tables over log2(harmonic) / 15
struct AlloyArtPrep {
  bool bip = false, envOn = false, lfoOn = false, any = false;
  hm::Curve env, lfo;
  double envLen = 0, lfoLen = 1, susX = -1, loopX = -1;
  bool envTempo = false, lfoTempo = false, lfoGlobal = false;
  float lfoPhase = 0;
  std::array<hm::Curve, AmCount> maps;
  std::array<bool, AmCount> mapFlat{};
  std::array<float, AmCount> mapConst{};
};
struct AlloyAux final : AuxData {
  std::array<AlloyArtPrep, AaCount> arts;
  std::array<std::array<float, kAlloyEqPts>, AeCount> eq{};
  std::array<bool, AeCount> eqUsed{};
};
std::shared_ptr<const AuxData> buildAlloyAux(const PluginState& s);
std::shared_ptr<const AuxData> buildSpectraMorphAux(const PluginState& s);

std::unique_ptr<Generator> makeAlloy();
std::unique_ptr<Generator> makeSpectraMorph();
std::unique_ptr<Generator> makeGrainScan();
std::unique_ptr<Generator> makeSplineScratch();

}  // namespace gl::packs::models::metal
