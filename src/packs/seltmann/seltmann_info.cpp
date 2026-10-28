// Gridline — "seltmann" pack: catalogue entry for Seltmann (gl.seltmann).
#include "../Packs.h"
#include "seltmann.h"

namespace gl::packs::seltmann {

namespace {

ParamDesc lin(const std::string& n, const char* u, double mn, double mx, double d) {
  return ParamDesc{n, u, mn, mx, d, ParamKind::Continuous, ParamScale::Linear, {}};
}
ParamDesc lg(const std::string& n, const char* u, double mn, double mx, double d) {
  return ParamDesc{n, u, mn, mx, d, ParamKind::Continuous, ParamScale::Log, {}};
}
ParamDesc tog(const std::string& n, double d) { return ParamDesc{n, "", 0, 1, d, ParamKind::Toggle, ParamScale::Linear, {}}; }
ParamDesc ch(const std::string& n, std::vector<std::string> c, double d) {
  const double mx = static_cast<double>(c.size() - 1);
  return ParamDesc{n, "", 0, mx, d, ParamKind::Choice, ParamScale::Linear, std::move(c)};
}

}  // namespace

void appendInfos(std::vector<PluginInfo>& v) {
  PluginInfo p{kType, "Seltmann", "Sampler", PluginKind::Generator, {}, true, {}};
  p.params.resize(SmCount);
  p.params[SmVolume] = lin("Volume", "%", 0, 1.25, 0.8);
  p.params[SmPan] = lin("Pan", "pan", -1, 1, 0);
  p.params[SmBaseNote] = lin("Base note", "note", 12, 115, 60);
  p.params[SmKeyMap] = ch("Key map", {"Slices", "Slot letters"}, KeySlices);
  p.params[SmSync] = ch("Tempo sync", {"Original", "Resample", "Stretch"}, SyncStretch);
  p.params[SmStretchMode] = ch("Stretch mode", {"Auto", "Transient", "Mono", "Rubber Band"}, 0);  // J-157: 0..2 the stretch, 3 Rubber Band
  p.params[SmLock] = tog("Lock to song", 1);
  p.params[SmMono] = tog("Monophonic", 0);
  p.params[SmAttack] = lin("Attack", "ms", 0, 500, 0);
  p.params[SmDecay] = lin("Decay", "ms", 0, 2000, 0);
  p.params[SmSustain] = lin("Sustain", "%", 0, 1, 1);
  p.params[SmRelease] = lg("Release", "ms", 1, 2000, 10);
  p.params[SmDeclick] = lg("Declick", "ms", 0.3, 50, 3);
  p.params[SmSwing] = lin("Swing", "%", 0, 1, 0);
  p.params[SmPitch] = lin("Pitch", "st", -24, 24, 0);
  p.params[SmVelocity] = lin("Velocity to level", "%", 0, 1, 0.6);
  p.params[SmReverse] = tog("Reverse all", 0);
  p.params[SmGate] = lin("Gate", "%", 0.05, 1, 1);
  p.params[SmSpeed] = ch("Pattern speed", {"Half", "Normal", "Double"}, SpeedNormal);
  p.params[SmCutoff] = lg("Cutoff", "Hz", 20, 20000, 20000);
  p.params[SmReso] = lin("Resonance", "%", 0, 1, 0);
  p.editorWidth = 1120;

  // Factory sounds (original). Chop layouts live in the editor's layout browser.
  p.presets.push_back({"Clean Chops", {}});
  p.presets.push_back({"Vinyl Sync", {{SmSync, SyncResample}}});
  p.presets.push_back({"Dusty Sampler", {{SmCutoff, 4200.f}, {SmReso, 0.15f}, {SmPitch, -1.f}, {SmSwing, 0.3f}}});
  p.presets.push_back({"Tight Gate", {{SmGate, 0.55f}, {SmRelease, 4.f}, {SmMono, 1.f}}});
  p.presets.push_back({"Soft Pads", {{SmAttack, 60.f}, {SmRelease, 400.f}, {SmDeclick, 12.f}}});
  p.presets.push_back({"Half-Time Feel", {{SmSpeed, SpeedHalf}, {SmGate, 0.9f}}});
  v.push_back(std::move(p));
}

}  // namespace gl::packs::seltmann
