// Gridline — "models" pack, J-22 group "metal": catalogue entries (Alloy,
// Spectra Morph, Grain Scanner, Spline Scratcher) and their factory presets.
// Parameters of the engines keep the reference controls' stored units.
#include <string>
#include <vector>
#include "../Packs.h"
#include "metal.h"

namespace gl::packs::models {

namespace {

using namespace metal;

ParamDesc lin(const std::string& n, const char* u, double mn, double mx, double d) {
  return ParamDesc{n, u, mn, mx, d, ParamKind::Continuous, ParamScale::Linear, {}};
}
ParamDesc tog(const std::string& n, bool d) { return ParamDesc{n, "", 0, 1, d ? 1.0 : 0.0, ParamKind::Toggle, ParamScale::Linear, {}}; }
ParamDesc ch(const std::string& n, std::vector<std::string> c, int d) {
  const double hi = static_cast<double>(c.size() - 1);
  return ParamDesc{n, "", 0, hi, static_cast<double>(d), ParamKind::Choice, ParamScale::Linear, std::move(c)};
}
using V = std::vector<std::pair<int, float>>;

// ------------------------------------------------------------------ Alloy
PluginInfo alloyInfo() {
  PluginInfo p{"gl.alloy", "Alloy", "Synth", PluginKind::Generator, {}, false, {}};
  auto& q = p.params;
  q.resize(AlCount);
  q[AlVolume] = lin("Volume", "", 0, 256, 128);
  q[AlCoarse] = lin("Coarse", "st", -24, 24, 0);
  q[AlFine] = lin("Fine", "ct", -50, 50, 0);
  q[AlModX] = lin("Mod X", "", 0, 512, 256);
  q[AlModY] = lin("Mod Y", "", 0, 512, 256);
  q[AlPreDecay] = lin("Pre-decay", "", 0, 1000, 100);
  q[AlDecay] = lin("Decay", "", 0, 10000, 4000);
  q[AlRelease] = lin("Release", "", 0, 10000, 2000);
  q[AlFullness] = lin("Fullness", "", -1000, 1000, 0);
  q[AlMorph] = lin("Morph", "", 0, 1000, 300);
  q[AlSeed1] = lin("Seed 1", "", 0, 9999, 1);
  q[AlSeed2] = lin("Seed 2", "", 0, 9999, 0);
  q[AlUniOrder] = lin("Unison order", "", 1, 9, 1);
  q[AlUniPan] = lin("Unison pan", "", 0, 128, 128);
  q[AlUniVol] = lin("Unison volume", "", 16, 128, 128);
  q[AlUniPitch] = lin("Unison pitch", "", 0, 256, 128);
  q[AlUniPhase] = lin("Unison phase", "", 0, 128, 64);
  q[AlCutoff] = lin("Cutoff", "", 0, 256, 128);
  q[AlReso] = lin("Resonance", "", 0, 256, 128);
  q[AlChOrder] = lin("Chorus order", "", 0, 9, 4);
  q[AlChDepth] = lin("Chorus depth", "", 0, 1024, 512);
  q[AlChSpeed] = lin("Chorus speed", "", 0, 1024, 768);
  q[AlChDelay] = lin("Chorus delay", "", 0, 128, 8);
  q[AlChSpread] = lin("Chorus spread", "", 0, 128, 128);
  q[AlChCross] = lin("Chorus cross", "", -128, 128, -32);
  q[AlChMix] = lin("Chorus mix", "", -128, 128, 64);
  q[AlDlFeedback] = lin("Delay feedback", "", 0, 127, 64);
  q[AlDlTime] = lin("Delay time", "", 0, 384, 144);
  q[AlDlOffset] = lin("Delay offset", "", -512, 512, 0);
  q[AlDlVol] = lin("Delay volume", "", -128, 128, 64);
  q[AlDlMode] = ch("Delay mode", {"Normal", "Ping-pong", "Inverted", "Inverted ping-pong"}, 0);
  q[AlRvLowCut] = lin("Reverb low cut", "Hz", 19, 3000, 75);
  q[AlRvHighCut] = lin("Reverb high cut", "", 5, 221, 40);
  q[AlRvPredelay] = lin("Reverb predelay", "", 0, 384, 0);
  q[AlRvRoom] = lin("Reverb room", "", 1, 100, 40);
  q[AlRvDiffusion] = lin("Reverb diffusion", "", 0, 100, 100);
  q[AlRvDecay] = lin("Reverb decay", "", 1, 200, 15);
  q[AlRvDamp] = lin("Reverb damping", "", 5, 221, 40);
  q[AlRvColor] = lin("Reverb colour", "", 0, 4, 2);
  q[AlRvWet] = lin("Reverb wet", "", -128, 128, 25);
  for (int k = 0; k < 3; ++k) {
    const std::string b = "EQ " + std::to_string(k + 1) + " ";
    q[static_cast<size_t>(AlEqLevel1 + k)] = lin(b + "level", "", -1800, 1800, 0);
    q[static_cast<size_t>(AlEqFreq1 + k)] = lin(b + "freq", "", 0, 65536, k == 0 ? 8950 : k == 1 ? 33145 : 49305);
    q[static_cast<size_t>(AlEqWidth1 + k)] = lin(b + "width", "", 0, 65536, 17500);
    q[static_cast<size_t>(AlEqType1 + k)] = ch(b + "type", {"Low shelf", "Peaking", "High shelf"}, k);
  }
  q[AlDust] = lin("Magic dust", "", 0, 1000, 430);
  q[AlRichness] = lin("Richness", "", 1, 11, 9);
  q[AlComb] = ch("Harmonic comb", {"Off", "Remove every 2", "Remove every 3", "Remove every 4", "Remove every 5", "Keep every 3",
                                    "Keep every 4", "Keep every 5"}, 0);
  q[AlFltType] = ch("Filter type", {"Low pass", "Band pass", "Band stop", "High pass"}, 0);
  q[AlFltMode] = ch("Filter slope", {"Off", "1x", "2x", "3x", "4x"}, 1);
  q[AlFltFlat] = tog("Flat", false);
  q[AlDeclick] = tog("Declick", true);
  q[AlAltDecay] = tog("Alt decay", false);
  q[AlRvOn] = tog("Reverb on", false);
  q[AlRvTempo] = tog("Reverb tempo", false);
  q[AlDlOn] = tog("Delay on", false);
  q[AlDlTempo] = tog("Delay tempo", true);
  q[AlModSmooth] = tog("Mod smooth", false);
  q[AlPorta] = tog("Portamento", false);
  q[AlPortaVar] = tog("Porta variable", true);
  q[AlPortaLimit] = ch("Porta limit", {"None", "Octave wrap", "4 st", "3 st", "2 st", "1 st"}, 0);
  q[AlKeyPitch] = tog("Key to pitch", true);
  q[AlKeySeed] = tog("Key to seed", false);
  q[AlOneVoicePerKey] = tog("One voice per key", false);
  q[AlVelVol] = tog("Velocity to volume", true);
  q[AlVelAttTime] = tog("Velocity to attack time", false);
  q[AlVelAttLen] = tog("Velocity to attack length", false);
  q[AlQuality] = ch("Quality", {"Draft", "Average", "High", "Pristine", "Maximum"}, 2);
  q[AlRelLink] = tog("Release follows volume", false);
  q[AlNatScale] = tog("Natural EQ scale", false);
  q[AlNatWeight] = tog("Natural EQ weighting", false);
  q[AlAutoLoud] = tog("Auto loudness", false);
  static const char* const artShort[kAlloyEnvArts] = {"Vol", "Cut", "Res", "Pitch", "Uni pitch"};
  for (int a = 0; a < kAlloyEnvArts; ++a) {
    const std::string b = std::string(artShort[a]) + " ";
    q[static_cast<size_t>(alKnob(a, AkAttack))] = lin(b + "env attack", "", 0, 512, 128);
    q[static_cast<size_t>(alKnob(a, AkDecay))] = lin(b + "env decay", "", 0, 512, 128);
    q[static_cast<size_t>(alKnob(a, AkSustain))] = lin(b + "env sustain", "", -128, 128, 0);
    q[static_cast<size_t>(alKnob(a, AkRelease))] = lin(b + "env release", "", 0, 512, 128);
    q[static_cast<size_t>(alKnob(a, AkLfoSpeed))] = lin(b + "LFO speed", "", 0, 65536, 40000);
    q[static_cast<size_t>(alKnob(a, AkLfoTension))] = lin(b + "LFO tension", "", -128, 128, 0);
    q[static_cast<size_t>(alKnob(a, AkLfoSkew))] = lin(b + "LFO skew", "", -128, 128, 0);
    q[static_cast<size_t>(alKnob(a, AkLfoWidth))] = lin(b + "LFO width", "", -128, 128, 0);
  }
  p.presets = {
      {"Temple Bell", V{{AlPreDecay, 60}, {AlDecay, 6200}, {AlRelease, 4200}, {AlFullness, -260}, {AlMorph, 120}, {AlSeed1, 4120},
                        {AlSeed2, 77}, {AlCutoff, 200}, {AlRvOn, 1}, {AlRvWet, 50}, {AlRvDecay, 40}}},
      {"Glass Pad", V{{AlPreDecay, 0}, {AlDecay, 10000}, {AlRelease, 5600}, {AlFullness, 320}, {AlMorph, 520}, {AlSeed1, 812},
                      {AlUniOrder, 5}, {AlUniPitch, 150}, {AlChOrder, 4}, {AlRvOn, 1}, {AlRvWet, 70}, {AlRvRoom, 70}}},
      {"Steel Pluck", V{{AlPreDecay, 420}, {AlDecay, 1400}, {AlRelease, 700}, {AlFullness, -520}, {AlMorph, 40}, {AlSeed1, 3303},
                        {AlCutoff, 150}, {AlReso, 90}}},
      {"Gong Swell", V{{AlPreDecay, 0}, {AlDecay, 9000}, {AlRelease, 7000}, {AlFullness, 700}, {AlMorph, 800}, {AlSeed1, 57},
                       {AlSeed2, 9001}, {AlRichness, 11}, {AlDlOn, 1}, {AlDlVol, 30}, {AlRvOn, 1}, {AlRvWet, 80}}},
      {"Hollow Keys", V{{AlPreDecay, 200}, {AlDecay, 3000}, {AlRelease, 900}, {AlFullness, -100}, {AlComb, 1}, {AlSeed1, 1234},
                        {AlRichness, 7}}},
  };
  p.editorWidth = 760;
  return p;
}

// ----------------------------------------------------------- Spectra Morph
PluginInfo spectraMorphInfo() {
  PluginInfo p{"gl.spectramorph", "Spectra Morph", "Synth", PluginKind::Generator, {}, false, {}};
  auto& q = p.params;
  q.resize(SmCount);
  q[SmVolume] = lin("Volume", "", 0, 1, 0.8);
  q[SmTranspose] = lin("Transpose", "st", -48, 48, 0);
  q[SmFine] = lin("Fine", "ct", -100, 100, 0);
  q[SmGlide] = lin("Glide", "s", 0, 2, 0);
  q[SmVelSens] = lin("Velocity", "", 0, 1, 0.7);
  q[SmMorphX] = lin("Morph X", "", 0, 1, 0);
  q[SmMorphY] = lin("Morph Y", "", 0, 1, 0);
  q[SmMorphRate] = lin("Morph rate", "Hz", 0, 10, 0);
  q[SmMorphMode] = ch("Morph motion", {"Static", "Circle", "Line X", "Line Y", "Figure eight"}, 0);
  q[SmSpeed] = lin("Speed", "", 0, 4, 1);
  q[SmLoop] = ch("Loop", {"One-shot", "Sustain loop", "Ping-pong loop", "Hold last"}, 1);
  q[SmAttack] = lin("Attack", "s", 0, 10, 0.002);
  q[SmDecay] = lin("Decay", "s", 0, 10, 0.5);
  q[SmSustain] = lin("Sustain", "", 0, 1, 1);
  q[SmRelease] = lin("Release", "s", 0, 10, 0.3);
  q[SmNoise] = lin("Noise", "", 0, 1, 0);
  q[SmNoiseColor] = lin("Noise colour", "", 0, 1, 0.5);
  q[SmChorusOn] = tog("Chorus", false);
  q[SmChorusDepth] = lin("Chorus depth", "", 0, 1, 0.5);
  q[SmChorusRate] = lin("Chorus rate", "Hz", 0.05, 5, 0.5);
  q[SmDelayOn] = tog("Delay", false);
  q[SmDelayTime] = lin("Delay time", "s", 0.01, 1.5, 0.3);
  q[SmDelayFb] = lin("Delay feedback", "", 0, 0.95, 0.4);
  q[SmReverbOn] = tog("Reverb", false);
  q[SmReverbDecay] = lin("Reverb decay", "", 0, 1, 0.5);
  q[SmReverbMix] = lin("Reverb mix", "", 0, 1, 0.3);
  q[SmDrive] = lin("Drive", "", 0, 1, 0);
  q[SmVoices] = lin("Voices", "", 1, 16, 12);
  p.presets = {
      {"Breathing Organ", V{{SmMorphRate, 0.2f}, {SmMorphMode, 1}, {SmChorusOn, 1}}},
      {"Slow Sweep", V{{SmMorphRate, 0.05f}, {SmMorphMode, 2}, {SmReverbOn, 1}, {SmAttack, 0.4f}, {SmRelease, 1.5f}}},
      {"Vowel Drift", V{{SmMorphX, 0.5f}, {SmMorphY, 0.5f}, {SmMorphRate, 0.6f}, {SmMorphMode, 4}}},
  };
  p.editorWidth = 760;
  return p;
}

// ----------------------------------------------------------- Grain Scanner
PluginInfo grainScanInfo() {
  PluginInfo p{"gl.grainscan", "Grain Scanner", "Sampler", PluginKind::Generator, {}, true, {}};
  auto& q = p.params;
  q.resize(GsCount);
  q[GsAttack] = lin("Grain attack", "", 50, 500, 204);
  q[GsHold] = lin("Grain hold", "", 0, 500, 50);
  q[GsGrainSpace] = lin("Grain spacing", "", 125, 1000, 611);
  q[GsWaveSpace] = lin("Wave spacing", "", -1000, 1000, 611);
  q[GsFxDepth] = lin("Effect depth", "", 0, 384, 0);
  q[GsFxSpeed] = lin("Effect speed", "", 4, 200, 180);
  q[GsRandom] = lin("Randomness", "", 0, 256, 0);
  q[GsStereo] = lin("Stereo", "", 0, 64, 0);
  q[GsLoop] = tog("Loop", false);
  q[GsHoldOn] = tog("Hold", false);
  q[GsStart] = lin("Start", "", 0, 4096, 0);
  q[GsTransHold] = lin("Transient hold", "", 0, 500, 228);
  q[GsTrigger] = ch("Key to", {"Pitch", "Percent", "Step", "Transient"}, 0);
  q[GsTransSource] = ch("Transient source", {"Auto", "Slices", "Markers"}, 0);
  q[GsTempoLock] = tog("Tempo locked", false);
  q[GsEnvOn] = tog("Envelope", false);
  q[GsEnvAttack] = lin("Env attack", "s", 0, 10, 0);
  q[GsEnvHold] = lin("Env hold", "s", 0, 10, 0);
  q[GsEnvDecay] = lin("Env decay", "s", 0, 10, 0.5);
  q[GsEnvSustain] = lin("Env sustain", "", 0, 1, 1);
  q[GsEnvRelease] = lin("Env release", "s", 0, 10, 0.05);
  q[GsVolume] = lin("Volume", "", 0, 1, 0.8);
  q[GsRoot] = lin("Root note", "", 0, 127, 60);
  p.presets = {
      {"Half Speed", V{{GsWaveSpace, 411}}},
      {"Reverse Cloud", V{{GsWaveSpace, -611}, {GsRandom, 90}, {GsStereo, 40}}},
      {"Stutter Steps", V{{GsGrainSpace, 400}, {GsWaveSpace, 107}, {GsHold, 120}, {GsLoop, 1}}},
      {"Shimmer Freeze", V{{GsWaveSpace, 0}, {GsFxDepth, 200}, {GsFxSpeed, 60}, {GsStereo, 64}, {GsHoldOn, 1}}},
  };
  p.editorWidth = 520;
  return p;
}

// ------------------------------------------------------- Spline Scratcher
PluginInfo splineScratchInfo() {
  PluginInfo p{"gl.splinescratch", "Spline Scratch", "Sampler", PluginKind::Generator, {}, true, {}};
  auto& q = p.params;
  q.resize(SsCount);
  q[SsSpeed] = lin("Speed", "", 0, 1024, 512);
  q[SsAttack] = lin("Attack", "", 0, 100, 25);
  q[SsRelease] = lin("Release", "", 0, 5000, 2500);
  q[SsStartWave] = lin("Start", "", 0, 10000, 0);
  q[SsEndWave] = lin("End", "", 0, 10000, 10000);
  q[SsSplineMode] = ch("Spline type", {"Position", "Speed"}, 0);
  q[SsVolume] = lin("Volume", "", 0, 1, 0.8);
  q[SsContinuous] = tog("Continuous output", false);
  p.presets = {
      {"Default", V{}},
      {"Fast Cuts", V{{SsSpeed, 768}, {SsAttack, 5}, {SsRelease, 600}}},
      {"Slow Drag", V{{SsSpeed, 256}, {SsAttack, 60}}},
  };
  p.editorWidth = 760;
  return p;
}

}  // namespace

void appendMetalInfos(std::vector<PluginInfo>& v) {
  v.push_back(alloyInfo());
  v.push_back(spectraMorphInfo());
  v.push_back(grainScanInfo());
  v.push_back(splineScratchInfo());
}

}  // namespace gl::packs::models
