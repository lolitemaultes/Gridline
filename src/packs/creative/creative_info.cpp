// Gridline — "creative" pack: catalogue entries.
#include <cmath>
#include <cstdio>
#include <string>
#include "../Packs.h"
#include "creative.h"

namespace gl::packs::creative {

namespace {

ParamDesc lin(const std::string& n, const char* u, double mn, double mx, double d) {
  return ParamDesc{n, u, mn, mx, d, ParamKind::Continuous, ParamScale::Linear, {}};
}
ParamDesc lg(const std::string& n, const char* u, double mn, double mx, double d) {
  return ParamDesc{n, u, mn, mx, d, ParamKind::Continuous, ParamScale::Log, {}};
}
ParamDesc tog(const std::string& n, double d) { return ParamDesc{n, "", 0, 1, d, ParamKind::Toggle, ParamScale::Linear, {}}; }
ParamDesc ch(const std::string& n, std::vector<std::string> c, double d) {
  double mx = static_cast<double>(c.size() - 1);
  return ParamDesc{n, "", 0, mx, d, ParamKind::Choice, ParamScale::Linear, std::move(c)};
}

const char* kTargetNames[kTargets] = {"pan", "volume", "cutoff", "resonance", "low", "band", "high", "shaper mix"};

PluginInfo octaFilter() {
  PluginInfo p{"gl.octafilter", "Octa Filter", "Filter", PluginKind::Effect, {}, false, {}};
  p.params.resize(OfTotal);
  p.params[OfMaster] = lin("Master level", "times", 0, 1.25, 1);
  p.params[OfMasterLfo] = lin("Master LFO level", "%", -1, 1, 0);
  p.params[OfIefLevel] = lin("Follower level", "times", 0, 1.25, 1);
  p.params[OfIefAttack] = lg("Follower attack", "s", 0.0005, 1.0, 0.025);
  p.params[OfIefRelease] = lg("Follower release", "s", 0.0005, 1.0, 0.1);
  {
    std::vector<std::string> pats;
    for (int k = 1; k <= kPatterns; ++k) pats.push_back("Pattern " + std::to_string(k));
    p.params[OfPattern] = ch("Pattern", pats, 0);
  }
  p.params[OfModX] = lin("Modulation X", "%", 0, 1, 0.5);
  p.params[OfModY] = lin("Modulation Y", "%", 0, 1, 0.5);
  p.params[OfSmooth] = tog("Smooth modulation", 0);
  p.params[OfCenter] = tog("Remove DC offset", 0);
  p.params[OfSyncPatterns] = tog("Sync pattern changes", 0);
  p.params[OfHqEnvelopes] = tog("HQ envelopes", 0);
  p.params[OfOversample] = ch("Oversampling", {"Off", "2x", "4x", "8x", "16x"}, 0);
  p.params[OfSolo] = ch("Solo unit", {"None", "1", "2", "3", "4", "5", "6", "7", "8"}, 0);
  p.params[OfIefUse1] = tog("Follower reads unit 1", 0);
  p.params[OfHqShaper] = tog("HQ shaper", 1);
  static const std::vector<std::string> types = {"Multimode", "Low pass 12", "High pass 12", "Low pass", "Band pass", "Band stop",
                                                 "High pass", "All pass", "Low shelf", "Peak", "High shelf", "Low pass 24 drive"};
  for (int u = 0; u < kUnits; ++u) {
    std::string n = "Unit " + std::to_string(u + 1) + " ";
    p.params[unitParam(u, UEnv)] = lin(n + "envelope amount", "%", 0, 2, 1);
    p.params[unitParam(u, UCut)] = lin(n + "cutoff", "%", 0, 1, 0.5);
    p.params[unitParam(u, URes)] = lin(n + "resonance", "%", 0, 1, 0.5);
    p.params[unitParam(u, ULow)] = lin(n + "low / gain / drive", "%", 0, 1, 1);
    p.params[unitParam(u, UBand)] = lin(n + "band", "%", 0, 1, 0);
    p.params[unitParam(u, UHigh)] = lin(n + "high", "%", 0, 1, 0);
    p.params[unitParam(u, UWsPre)] = lin(n + "shaper pre amp", "times", 0.5, 1.5, 1);
    p.params[unitParam(u, UWsMix)] = lin(n + "shaper mix", "%", 0, 1, 1);
    p.params[unitParam(u, UPan)] = lin(n + "pan", "pan", -1, 1, 0);
    p.params[unitParam(u, UOut)] = lin(n + "volume", "times", 0, 1.25, 1);
    p.params[unitParam(u, UIn)] = lin(n + "input", "times", -1.25, 1.25, 1);
    p.params[unitParam(u, UNext)] = lin(n + "to next unit", "times", -1.25, 1.25, 0);
    p.params[unitParam(u, UOrder)] = ch(n + "filter mode", {"Off", "Single", "Dual", "Triple", "Staggered dual", "Staggered triple"}, 1);
    p.params[unitParam(u, UType)] = ch(n + "filter type", types, 0);
    p.params[unitParam(u, UAlt)] = tog(n + "alternate resonance", 0);
    p.params[unitParam(u, UEnabled)] = tog(n + "on", u == 0 ? 1 : 0);
    p.params[unitParam(u, UWsOn)] = tog(n + "shaper on", 0);
    p.params[unitParam(u, UWsBipolar)] = tog(n + "shaper bipolar", 0);
    for (int t = 0; t < kTargets; ++t) {
      std::string m = n + kTargetNames[t] + " LFO ";
      p.params[lfoParam(u, t, LSpeed)] = lin(m + "speed", "%", 0, 1, 40000.0 / 65536.0);
      p.params[lfoParam(u, t, LTension)] = lin(m + "tension", "%", -1, 1, 0);
      p.params[lfoParam(u, t, LSkew)] = lin(m + "skew", "%", -1, 1, 0);
      p.params[lfoParam(u, t, LPulse)] = lin(m + "pulse width", "%", -1, 1, 0);
      p.params[lfoParam(u, t, LPhase)] = lin(m + "phase", "%", 0, 1, 0);
    }
  }
  auto U = [](int u, int f) { return unitParam(u, f); };
  p.presets = {
      {"Multimode low", {}},
      {"Low pass 24 drive", {{U(0, UType), 11.f}, {U(0, UCut), 0.55f}, {U(0, URes), 0.35f}, {U(0, ULow), 0.6f}}},
      {"Two-stage band", {{U(0, UType), 4.f}, {U(0, UOrder), 2.f}, {U(0, UNext), 1.f}, {U(1, UEnabled), 1.f}, {U(1, UType), 4.f},
                          {U(1, UCut), 0.7f}, {U(1, UIn), 0.f}}},
      {"Split highs and lows", {{U(0, UType), 3.f}, {U(0, UCut), 0.45f}, {U(0, URes), 0.1f}, {U(0, UPan), -0.6f}, {U(1, UEnabled), 1.f},
                                {U(1, UType), 6.f}, {U(1, UCut), 0.45f}, {U(1, URes), 0.1f}, {U(1, UPan), 0.6f}}},
      {"Driven shaper", {{U(0, UOrder), 0.f}, {U(0, UWsOn), 1.f}, {U(0, UWsPre), 1.5f}, {U(0, UOut), 0.9f}}},
      {"Peaking comb", {{U(0, UType), 9.f}, {U(0, UOrder), 3.f}, {U(0, UOut), 0.3f}, {U(1, UEnabled), 1.f}, {U(1, UType), 9.f}, {U(1, UOrder), 3.f},
                        {U(1, UCut), 0.65f}, {U(1, UOut), 0.3f}, {OfMaster, 0.5f}}},
  };
  p.params[OfModel] = ch("Engine", {"Legacy", "Reference"}, 1);
  p.legacyModelParam = OfModel;
  p.editorWidth = 1000;
  return p;
}

}  // namespace

void appendInfos(std::vector<PluginInfo>& v) { v.push_back(octaFilter()); }

}  // namespace gl::packs::creative
