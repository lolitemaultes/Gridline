// Gridline — "samplers" pack: catalogue entries.
#include "../Packs.h"
#include "samplers.h"

namespace gl::packs::samplers {

namespace {

ParamDesc lin(const char* n, const char* u, double mn, double mx, double d) {
  return ParamDesc{n, u, mn, mx, d, ParamKind::Continuous, ParamScale::Linear, {}};
}
ParamDesc lg(const char* n, const char* u, double mn, double mx, double d) {
  return ParamDesc{n, u, mn, mx, d, ParamKind::Continuous, ParamScale::Log, {}};
}
ParamDesc tog(const char* n, double d) { return ParamDesc{n, "", 0, 1, d, ParamKind::Toggle, ParamScale::Linear, {}}; }
ParamDesc ch(const char* n, std::vector<std::string> c, double d) {
  double mx = static_cast<double>(c.size() - 1);
  return ParamDesc{n, "", 0, mx, d, ParamKind::Choice, ParamScale::Linear, std::move(c)};
}

PluginInfo multisampler() {
  PluginInfo p;
  p.type = "gl.multisampler";
  p.name = "Multisampler";
  p.category = "Samplers";
  p.kind = PluginKind::Generator;
  p.usesSample = false;  // data holds a zone map, not a path
  p.editorWidth = 740;
  p.params.resize(MsCount);
  p.params[MsVolume] = lin("Volume", "%", 0, 1, 0.8);
  p.params[MsTranspose] = lin("Transpose", "st", -24, 24, 0);
  p.params[MsFine] = lin("Fine tune", "ct", -100, 100, 0);
  p.params[MsPolyphony] = lin("Polyphony", "", 1, kMaxVoices, 32);
  p.params[MsPlayMode] = ch("Play mode", {"Poly", "Mono", "Legato"}, 0);
  p.params[MsGlide] = lin("Glide", "s", 0, 2, 0);
  p.params[MsRoundRobin] = ch("Round robin", {"Layer", "Cycle", "Random", "No repeat"}, 1);
  p.params[MsVelAmp] = lin("Velocity", "%", 0, 1, 0.7);
  p.params[MsInterp] = ch("Interpolation", {"Linear", "Cubic", "Sinc"}, 1);
  p.params[MsAttack] = lg("Attack", "s", 0.001, 10, 0.002);
  p.params[MsHold] = lin("Hold", "s", 0, 5, 0);
  p.params[MsDecay] = lg("Decay", "s", 0.01, 20, 1.0);
  p.params[MsSustain] = lin("Sustain", "%", 0, 1, 1.0);
  p.params[MsRelease] = lg("Release", "s", 0.005, 20, 0.3);
  p.params[MsFilterType] = ch("Filter", {"Off", "Low", "High", "Band", "Notch"}, 0);
  p.params[MsCutoff] = lg("Cutoff", "Hz", 20, 20000, 8000);
  p.params[MsResonance] = lin("Resonance", "%", 0, 1, 0.1);
  p.params[MsKeyTrack] = lin("Key track", "%", 0, 1, 0);
  p.params[MsVelCutoff] = lin("Vel > cutoff", "%", 0, 1, 0);
  p.params[MsEnvAmount] = lin("Env > cutoff", "oct", -8, 8, 0);
  p.params[MsModAttack] = lg("Mod attack", "s", 0.001, 10, 0.005);
  p.params[MsModDecay] = lg("Mod decay", "s", 0.01, 20, 0.6);
  p.params[MsModSustain] = lin("Mod sustain", "%", 0, 1, 0);
  p.params[MsModRelease] = lg("Mod release", "s", 0.005, 20, 0.3);
  p.params[MsModPitch] = lin("Env > pitch", "st", -24, 24, 0);
  p.params[MsLfoShape] = ch("LFO shape", {"Sine", "Triangle", "Square", "Saw", "Random"}, 0);
  p.params[MsLfoRate] = lg("LFO rate", "Hz", 0.05, 30, 5);
  p.params[MsLfoDelay] = lin("LFO delay", "s", 0, 5, 0.2);
  p.params[MsLfoPitch] = lin("LFO > pitch", "ct", 0, 1200, 0);
  p.params[MsLfoCutoff] = lin("LFO > cutoff", "oct", 0, 4, 0);
  p.params[MsLfoAmp] = lin("LFO > volume", "%", 0, 1, 0);
  p.params[MsLfoPan] = lin("LFO > pan", "%", 0, 1, 0);
  p.params[MsFactorySet] = ch("Factory set", factorySetNames(), 0);
  p.presets = {
      {"Tine Keys", {{MsFactorySet, 0.f}, {MsRelease, 0.45f}, {MsDecay, 2.5f}, {MsSustain, 0.6f}}},
      {"Glass Bells", {{MsFactorySet, 1.f}, {MsRelease, 2.5f}, {MsLfoPitch, 8.f}, {MsLfoRate, 4.5f}}},
      {"Slow Pad", {{MsFactorySet, 2.f}, {MsAttack, 0.9f}, {MsRelease, 2.2f}, {MsFilterType, 1.f}, {MsCutoff, 2400.f},
                    {MsLfoCutoff, 0.6f}, {MsLfoRate, 0.35f}, {MsLfoPan, 0.35f}}},
      {"Mono Bass", {{MsFactorySet, 3.f}, {MsPlayMode, 2.f}, {MsGlide, 0.07f}, {MsFilterType, 1.f}, {MsCutoff, 500.f},
                     {MsResonance, 0.35f}, {MsEnvAmount, 3.5f}, {MsModDecay, 0.35f}, {MsRelease, 0.08f}}},
      {"Soft Pluck", {{MsFactorySet, 4.f}, {MsSustain, 0.f}, {MsDecay, 0.9f}, {MsFilterType, 1.f}, {MsCutoff, 1200.f},
                      {MsEnvAmount, 3.f}, {MsModDecay, 0.25f}, {MsVelCutoff, 0.5f}}},
  };
  return p;
}

PluginInfo scratch() {
  PluginInfo p;
  p.type = "gl.scratch";
  p.name = "Scratch Sampler";
  p.category = "Samplers";
  p.kind = PluginKind::Generator;
  p.usesSample = true;  // first line of data is the sample path
  p.editorWidth = 900;
  p.params.resize(ScCount);
  p.params[ScVolume] = lin("Volume", "%", 0, 1, 0.8);
  p.params[ScStart] = lin("Start", "%", 0, 1, 0.0);
  p.params[ScEnd] = lin("End", "%", 0, 1, 0.5);
  p.params[ScLength] = lin("Length", "beats", 0.25, 16, 1);
  p.params[ScSync] = tog("Tempo sync", 1);
  p.params[ScKeyMode] = ch("Keys", {"Move per key", "Pitched"}, 0);
  p.params[ScRoot] = lin("Root note", "note", 0, 127, 60);
  p.params[ScPitchFollow] = lin("Pitch follow", "%", 0, 1, 1);
  p.params[ScSmoothing] = lin("Smoothing", "%", 0, 1, 0.3);
  p.params[ScAttack] = lg("Attack", "s", 0.001, 1, 0.003);
  p.params[ScRelease] = lg("Release", "s", 0.005, 5, 0.08);
  p.params[ScLoopMove] = tog("Loop move", 1);
  p.params[ScInterp] = ch("Interpolation", {"Linear", "Cubic"}, 1);
  p.params[ScSpeed] = lg("Speed", "", 0.25, 4, 1);
  p.params[ScMotion] = ch("Curve", {"Position", "Speed"}, 0);
  p.presets = {
      {"Baby Cuts", {}},
      {"Slow Chirps", {{ScLength, 2.f}, {ScSmoothing, 0.6f}, {ScRelease, 0.2f}}},
      {"Pitched Wobble", {{ScKeyMode, 1.f}, {ScLength, 0.5f}, {ScEnd, 0.2f}}},
      {"Tight Stabs", {{ScLength, 0.5f}, {ScSmoothing, 0.1f}, {ScLoopMove, 0.f}}},
      {"Platter Ride", {{ScMotion, 1.f}, {ScLength, 2.f}, {ScEnd, 1.f}, {ScSmoothing, 0.5f}}},
  };
  return p;
}

}  // namespace

PluginInfo waveProgramInfo();  // dwave_info.cpp (J-33)

void appendInfos(std::vector<PluginInfo>& v) {
  v.push_back(multisampler());
  v.push_back(waveProgramInfo());
  v.push_back(scratch());
}

}  // namespace gl::packs::samplers
