// Gridline — "spacefx" pack: catalogue entries.
#include <string>
#include "../Packs.h"
#include "spacefx.h"

namespace gl::packs::spacefx {

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

const std::vector<std::string> kFilterTypes = {"Off", "Low pass", "High pass", "Band pass"};
// Multi Delay: the first four types are the legacy set; the reference model adds the rest.
const std::vector<std::string> kMdFilterTypes = {"Off", "Low pass", "High pass", "Band pass", "Notch", "Low shelf", "Peaking", "High shelf"};
const std::vector<std::string> kSyncNames = {"1/32", "1/16", "1/16.", "1/8", "1/8.", "1/4", "1/4.", "1/2", "1 bar"};
const std::vector<std::string> kLfoSyncNames = {"1/64", "1/32", "1/16", "1/8", "1/4", "1/2", "1 bar", "2 bars", "4 bars"};

PluginInfo convolver() {
  PluginInfo p{"gl.convolver", "Convolver", "Delay & reverb", PluginKind::Effect, {}, true, {}};
  p.params.resize(CvCount);
  p.params[CvSource] = ch("Impulse", {"File", "Room", "Hall", "Plate", "Spring", "Chamber", "Cabinet", "Gated", "Reverse"}, SrcHall);
  p.params[CvSize] = lin("Space size", "%", 0, 1, 0.5);
  p.params[CvTone] = lin("Space tone", "%", 0, 1, 0.6);
  p.params[CvStart] = lin("IR start", "ms", 0, 1000, 0);
  p.params[CvLength] = lg("IR length", "s", 0.02, 60, 12);
  p.params[CvSync] = tog("Tempo length", 0);
  p.params[CvSyncLen] = ch("Synced length", {"1/16", "1/8", "1/4", "1/2", "1 bar", "2 bars", "4 bars"}, 4);
  p.params[CvFadeIn] = lin("Fade in", "ms", 0, 500, 0);
  p.params[CvFadeOut] = lin("Fade out", "%", 0, 1, 0.1);
  p.params[CvTighten] = lin("Tighten", "%", 0, 1, 0);
  p.params[CvReverse] = tog("Reverse", 0);
  p.params[CvStretch] = lg("Stretch", "\xC3\x97", 0.5, 2, 1);
  p.params[CvPreDelay] = lin("Pre-delay", "ms", 0, 500, 0);
  p.params[CvLowCut] = lg("IR low cut", "Hz", 20, 2000, 20);
  p.params[CvHighCut] = lg("IR high cut", "Hz", 1000, 20000, 20000);
  p.params[CvSelfConv] = lin("Self-convolution", "%", 0, 1, 0);
  p.params[CvNormalize] = tog("Normalize", 1);
  p.params[CvStereo] = ch("Stereo mode", {"Stereo", "True stereo", "Mono"}, 1);
  p.params[CvInWidth] = lin("Input width", "%", 0, 2, 1);
  p.params[CvWidth] = lin("Wet width", "%", 0, 2, 1);
  p.params[CvDry] = lin("Dry", "%", 0, 1, 1);
  p.params[CvWet] = lin("Wet", "%", 0, 1, 0.35);
  p.params[CvEqMix] = lin("EQ mix", "%", 0, 1, 1);
  p.params[CvEnvOn] = tog("IR envelope", 0);
  p.params[CvStretchMode] = ch("Stretch mode", {"Resample", "Keep pitch"}, 0);
  p.params[CvLatency] = lin("Reported latency", "smp", 0, 65536, 0);
  p.params[CvModel] = ch("IR build", {"Editing chain", "Reference"}, 0);
  p.legacyModelParam = CvModel;
  p.presets = {{"Concert space", {}},
               {"Tight room", {{CvSource, SrcRoom}, {CvSize, 0.35f}, {CvWet, 0.3f}}},
               {"Bright plate", {{CvSource, SrcPlate}, {CvSize, 0.55f}, {CvTone, 0.85f}, {CvLowCut, 180.f}, {CvWet, 0.3f}}},
               {"Spring tank", {{CvSource, SrcSpring}, {CvSize, 0.5f}, {CvWet, 0.4f}, {CvStereo, 0.f}}},
               {"Speaker box", {{CvSource, SrcCabinet}, {CvSize, 0.4f}, {CvDry, 0.f}, {CvWet, 1.f}, {CvStereo, 2.f}}},
               {"Backwards swell", {{CvSource, SrcReverse}, {CvSize, 0.6f}, {CvPreDelay, 0.f}, {CvWet, 0.45f}}}};
  p.editorWidth = 780;
  return p;
}

PluginInfo multiDelay() {
  PluginInfo p{"gl.multidelay", "Multi Delay", "Delay & reverb", PluginKind::Effect, {}, false, {}};
  p.params.resize(MdTotal);
  p.params[MdSync] = tog("Tempo sync", 1);
  p.params[MdDry] = lin("Dry", "%", 0, 1, 1);
  p.params[MdWet] = lin("Wet", "%", 0, 1, 0.5);
  p.params[MdInput] = lin("Input", "%", 0, 1, 1);
  p.params[MdFbScale] = lin("Feedback scale", "%", 0, 1.25, 1);
  static const float steps[kTaps] = {3, 4, 6, 8, 12, 16, 2, 24};
  static const float ms[kTaps] = {150, 220, 330, 450, 600, 800, 90, 1200};
  for (int k = 0; k < kTaps; ++k) {
    std::string t = "Tap " + std::to_string(k + 1) + " ";
    auto P = [&](int f) -> ParamDesc& { return p.params[static_cast<size_t>(mdParam(k, f))]; };
    P(MdOn) = tog(t + "on", k < 2 ? 1 : 0);
    P(MdSteps) = lg(t + "time (steps)", "", 0.002, 64, steps[k]);
    P(MdTimeMs) = lg(t + "time", "ms", 0.02, 5000, ms[k]);
    P(MdOffset) = lin(t + "stereo offset", "ms", -500, 500, 0);
    P(MdInLevel) = lin(t + "input", "%", -1.91, 1.91, 1);
    P(MdInPan) = lin(t + "input pan", "pan", -1, 1, 0);
    P(MdInFType) = ch(t + "input filter", kMdFilterTypes, 0);
    P(MdInCut) = lg(t + "input cutoff", "Hz", 2, 44200, 2000);
    P(MdInRes) = lin(t + "input resonance", "%", 0, 1, 0.1);
    P(MdFeedback) = lin(t + "feedback", "%", -1, 1, 0.5);
    P(MdFbMode) = ch(t + "feedback mode", {"Normal", "Ping-pong", "Invert", "Off"}, 0);
    P(MdFbPan) = lin(t + "feedback pan", "pan", -1, 1, 0);
    P(MdFbFType) = ch(t + "feedback filter", kMdFilterTypes, 1);
    P(MdFbCut) = lg(t + "feedback cutoff", "Hz", 2, 44200, 6000);
    P(MdFbRes) = lin(t + "feedback resonance", "%", 0, 1, 0.1);
    P(MdSep) = lin(t + "stereo separation", "%", -1, 1, 0);
    P(MdModRate) = lg(t + "mod rate", "Hz ", 0.05, 8, 0.4);
    P(MdModDepth) = lin(t + "mod depth", "%", 0, 1, 0);
    P(MdDiffuse) = lin(t + "diffusion", "%", 0, 1, 0);
    P(MdGateDiv) = lin(t + "gate division", "", 0, 16, 0);
    P(MdGateShape) = lin(t + "gate shape", "%", -1, 1, 0);
    P(MdOutLevel) = lin(t + "output", "%", -1.91, 1.91, 0.8);
    P(MdOutPan) = lin(t + "output pan", "pan", -1, 1, k == 0 ? -0.5 : (k == 1 ? 0.5 : 0.0));
    P(MdToNext) = lin(t + "to next", "%", -1.91, 1.91, 0);
    auto X = [&](int e) -> ParamDesc& { return p.params[static_cast<size_t>(mdExt(k, e))]; };
    X(MdxTempo) = tog(t + "tempo time", 1);
    X(MdxInStages) = lin(t + "input filter stages", "", 1, 3, 1);
    X(MdxInGain) = lin(t + "input filter gain", "\xC3\x97", 0.1, 2, 1);
    X(MdxInPost) = tog(t + "input filter post", 0);
    X(MdxFbStages) = lin(t + "feedback filter stages", "", 1, 3, 1);
    X(MdxFbGain) = lin(t + "feedback filter gain", "\xC3\x97", 0.1, 2, 1);
    X(MdxLeaveFirst) = tog(t + "leave first echo", 0);
  }
  // J-1 reference model (new instances); older projects load as legacy.
  p.params[MdModel] = ch("Delay model", {"Legacy", "Reference"}, 1);
  p.params[MdOversample] = tog("Oversample", 0);
  auto F = [](int tap, int f, float v) { return std::pair<int, float>{mdParam(tap, f), v}; };
  p.presets = {
      {"Wide pair", {}},
      {"Cascade", {F(0, MdToNext, 0.6f), F(1, MdToNext, 0.6f), F(2, MdOn, 1), F(2, MdToNext, 0.5f), F(3, MdOn, 1), F(0, MdFeedback, 0.2f),
                   F(1, MdFeedback, 0.2f), F(2, MdFeedback, 0.25f), F(3, MdFeedback, 0.3f), F(2, MdOutPan, -0.8f), F(3, MdOutPan, 0.8f)}},
      {"Ping-pong dub", {F(0, MdFbMode, 1), F(0, MdFeedback, 0.7f), F(0, MdFbCut, 1800.f), F(0, MdOutPan, 0.f), F(1, MdOn, 0),
                         F(0, MdDiffuse, 0.35f), F(0, MdModDepth, 0.2f)}},
      {"Stutter grains", {F(0, MdGateDiv, 4.f), F(0, MdGateShape, -0.6f), F(1, MdGateDiv, 3.f), F(1, MdGateShape, 0.5f), F(0, MdFeedback, 0.5f),
                          F(1, MdFeedback, 0.5f)}},
      {"Rhythm bank", {F(2, MdOn, 1), F(3, MdOn, 1), F(4, MdOn, 1), F(0, MdFeedback, 0.f), F(1, MdFeedback, 0.f), F(2, MdFeedback, 0.f),
                       F(3, MdFeedback, 0.f), F(4, MdFeedback, 0.f), F(2, MdOutLevel, 0.6f), F(3, MdOutLevel, 0.45f), F(4, MdOutLevel, 0.3f),
                       F(2, MdOutPan, -0.3f), F(3, MdOutPan, 0.3f), {mdExt(0, MdxLeaveFirst), 1.f}, {mdExt(1, MdxLeaveFirst), 1.f},
                       {mdExt(2, MdxLeaveFirst), 1.f}, {mdExt(3, MdxLeaveFirst), 1.f}, {mdExt(4, MdxLeaveFirst), 1.f}}},
      {"Filtered bank", {F(0, MdFbMode, 1), F(0, MdFbFType, 3), F(0, MdFbCut, 1200.f), F(0, MdFbRes, 0.45f), {mdExt(0, MdxFbStages), 2.f},
                         F(1, MdFbFType, 6), F(1, MdFbCut, 3000.f), {mdExt(1, MdxFbGain), 1.6f}, F(1, MdSep, -0.5f), {MdOversample, 1.f}}}};
  p.editorWidth = 840;
  return p;
}

PluginInfo bandDelay() {
  PluginInfo p{"gl.banddelay", "Band Delay", "Delay & reverb", PluginKind::Effect, {}, false, {}};
  p.params.resize(BdTotal);
  p.params[BdMaxTime] = lg("Max time", "s", 0.05, 2, 0.8);
  p.params[BdSync] = tog("Tempo sync", 0);
  p.params[BdSyncTime] = ch("Synced time", {"1/16", "1/8", "1/8.", "1/4", "1/4.", "1/2", "1/2.", "1 bar"}, 3);
  p.params[BdFeedback] = lin("Feedback", "%", 0, 1, 0.6);
  p.params[BdSmooth] = lg("Smoothing", "s", 0.001, 2, 0.15);
  p.params[BdKeepPitch] = tog("Keep pitch", 1);
  p.params[BdMorph] = lin("Morph A/B", "%", 0, 1, 0);
  p.params[BdLowCut] = lg("Wet low cut", "Hz", 20, 2000, 40);
  p.params[BdSat] = ch("Saturation", {"Off", "Soft", "Hard"}, 0);
  p.params[BdDry] = lin("Dry", "%", 0, 1, 1);
  p.params[BdWet] = lin("Wet", "%", 0, 1, 0.5);
  p.params[BdModel] = ch("Engine", {"Spectral", "Band delays"}, 0);
  p.legacyModelParam = BdModel;
  p.params[BdRScale] = lin("Time scale (bands)", "%", 0, 1, 0.5);
  p.params[BdRTenth] = tog("Time x 0.1 (bands)", 0);
  p.params[BdRFeedback] = lin("Feedback (bands)", "%", 0, 1, 0);
  p.params[BdRSmooth] = lin("Smoothing (bands)", "s", 0, 1, 0.25);
  p.params[BdRMode] = ch("Filter bank", {"Gentle", "Steep", "Linear phase"}, 0);
  p.params[BdRKeep] = tog("Keep pitch (bands)", 0);
  p.params[BdRMorph] = lin("Bank morph", "%", 0, 1, 0);
  p.params[BdRMix] = lin("Dry / wet", "%", 0, 1, 1);
  p.params[BdRHarmonic] = tog("Harmonic scale", 0);
  p.params[BdRWeight] = tog("Weighted feedback", 0);
  p.params[BdRHp] = lin("Wet high pass", "%", 0, 1, 0);
  p.params[BdRSat] = ch("Wet saturation", {"Limit", "Soft", "Clip"}, 0);
  p.presets = {{"Spectral spread", {}},
               {"Bank B", {{BdMorph, 1.f}}},
               {"Halfway morph", {{BdMorph, 0.5f}, {BdFeedback, 0.75f}}},
               {"Synced shimmer", {{BdSync, 1.f}, {BdSyncTime, 5.f}, {BdFeedback, 0.8f}, {BdSmooth, 0.6f}, {BdKeepPitch, 0.f}}},
               {"Short smear", {{BdMaxTime, 0.12f}, {BdFeedback, 0.4f}, {BdWet, 0.6f}}}};
  p.editorWidth = 760;
  return p;
}

PluginInfo hall() {
  PluginInfo p{"gl.hall", "Hall", "Delay & reverb", PluginKind::Effect, {}, false, {}};
  p.params.resize(HvCount);
  p.params[HvInGain] = lin("Input gain", "dB", -60, 15, 0);
  p.params[HvInLowCut] = lg("Input low cut", "Hz", 20, 5000, 20);
  p.params[HvInHighCut] = lg("Input high cut", "Hz", 40, 20000, 13000);
  p.params[HvOn] = tog("Reverb on", 1);
  p.params[HvPreDelay] = lin("Pre-delay", "ms", 0, 500, 12);
  p.params[HvPreSync] = tog("Pre-delay sync", 0);
  p.params[HvPreSyncTime] = ch("Pre-delay synced", kSyncNames, 1);
  p.params[HvSize] = lin("Size", "%", 0, 1, 0.55);
  p.params[HvDecay] = lg("Decay", "s", 0.2, 30, 2.6);
  p.params[HvCharacter] = lin("Character", "%", 0, 1, 0.5);
  p.params[HvDiffusion] = lin("Diffusion", "%", 0, 1, 0.85);
  p.params[HvModRate] = lg("Mod rate", "Hz ", 0.05, 10, 0.7);
  p.params[HvModDepth] = lin("Mod depth", "%", 0, 1, 0.2);
  p.params[HvHQ] = tog("High density", 1);
  p.params[HvFreeze] = ch("Freeze", {"Off", "Freeze", "Infinite", "Sustain"}, 0);
  p.params[HvLowMult] = lg("Low decay", "\xC3\x97", 0.25, 4, 1.2);
  p.params[HvLowXover] = lg("Low crossover", "Hz", 50, 1500, 250);
  p.params[HvHighMult] = lg("High decay", "\xC3\x97", 0.05, 2, 0.55);
  p.params[HvHighXover] = lg("High crossover", "Hz", 1000, 16000, 4500);
  p.params[HvFbOn] = tog("Shimmer on", 0);
  p.params[HvFbMode] = ch("Shimmer mode", {"Feedback", "Feed-forward"}, 0);
  p.params[HvFbPitch] = lin("Shimmer pitch", "st", -24, 24, 12);
  p.params[HvFbGain] = lin("Shimmer gain", "%", 0, 1, 0.45);
  p.params[HvFbDelay] = lg("Shimmer delay", "ms", 1, 2000, 120);
  p.params[HvFbLowCut] = lg("Shimmer low cut", "Hz", 20, 5000, 300);
  p.params[HvFbHighCut] = lg("Shimmer high cut", "Hz", 20, 20000, 7000);
  p.params[HvFbMix] = lin("Shimmer into reverb", "%", 0, 1, 1);
  p.params[HvEnvMode] = ch("Envelope", {"Off", "Duck", "Gate", "Swell"}, 0);
  p.params[HvEnvThresh] = lin("Env threshold", "dB", -60, 0, -30);
  p.params[HvEnvAmount] = lin("Env amount", "%", 0, 1, 0.7);
  p.params[HvEnvAttack] = lg("Env attack", "ms", 1, 1000, 15);
  p.params[HvEnvRelease] = lg("Env release", "ms", 10, 5000, 350);
  p.params[HvDry] = lin("Dry level", "dB", -60, 15, 0);
  p.params[HvWet] = lin("Wet level", "dB", -60, 15, -9);
  p.params[HvEqFreq] = lg("Wet EQ freq", "Hz", 40, 12000, 3000);
  p.params[HvEqGain] = lin("Wet EQ gain", "dB", -18, 18, 0);
  p.params[HvEqQ] = lg("Wet EQ Q", "", 0.25, 6, 0.8);
  p.params[HvWidth] = lin("Width", "%", 0, 2, 1.2);
  p.params[HvLimiter] = tog("Limiter", 0);
  p.params[HvModel] = ch("Reverb model", {"Legacy", "Reference"}, 1);
  p.params[HvBright] = lin("Brightness", "%", 0, 1, 0.85);
  p.params[HvModFreq] = lin("Mod frequency", "Hz ", 0, 1, 0.1);
  p.params[HvPreTicks] = lin("Pre-delay ticks", "", 1, 768, 192);
  p.params[HvFbSync] = tog("Shimmer delay sync", 0);
  p.params[HvFbTicks] = lin("Shimmer delay ticks", "", 1, 768, 192);
  p.params[HvEnvRoute] = ch("Envelope target", {"Off", "Wet", "Decay"}, 0);
  p.params[HvEnvScale] = lin("Env scale", "", -1, 1, 0);
  p.params[HvEnvOffset] = lin("Env offset", "", -0.5, 0.5, 0);
  p.params[HvEnvSmooth] = lg("Env smoothing", "ms", 1, 100, 1);
  p.params[HvEnvLowCut] = lg("Env low cut", "Hz", 20, 2000, 60);
  p.params[HvEnvSidechain] = tog("Env sidechain", 0);
  p.params[HvScIn] = lin("Sidechain source", "", -1, 500, -1);
  p.sideInParam = HvScIn;
  p.params[HvFfDirect] = lin("Unpitched level", "%", 0, 1, 0);
  p.params[HvFfPitch1] = lin("Voice 1 pitch", "st", -24, 24, 0);
  p.params[HvFfPitch2] = lin("Voice 2 pitch", "st", -24, 24, 20);
  p.params[HvFfAmp1] = lin("Voice 1 level", "%", 0, 1, 0);
  p.params[HvFfAmp2] = lin("Voice 2 level", "%", 0, 1, 0.02);
  p.params[HvFfDelay1] = lg("Voice 1 delay", "ms", 1, 500, 100);
  p.params[HvFfDelay2] = lg("Voice 2 delay", "ms", 1, 500, 100);
  p.params[HvFfSync1] = tog("Voice 1 delay sync", 0);
  p.params[HvFfSync2] = tog("Voice 2 delay sync", 0);
  p.params[HvFfTicks1] = lin("Voice 1 delay ticks", "", 1, 768, 192);
  p.params[HvFfTicks2] = lin("Voice 2 delay ticks", "", 1, 768, 192);
  p.legacyModelParam = HvModel;
  p.presets = {{"Grand hall", {}},
               {"Vocal plate", {{HvSize, 0.3f}, {HvDecay, 1.8f}, {HvCharacter, 0.15f}, {HvBright, 0.95f}, {HvInLowCut, 150.f}, {HvEnvRoute, HeWet},
                                {HvEnvScale, -0.5f}, {HvEnvThresh, -30.f}}},
               {"Shimmer cloud", {{HvSize, 0.9f}, {HvDecay, 9.f}, {HvFbOn, 1.f}, {HvFbGain, 0.55f}, {HvModDepth, 0.45f}, {HvWet, -6.f}}},
               {"Gated burst", {{HvSize, 0.5f}, {HvDecay, 0.3f}, {HvEnvRoute, HeDecay}, {HvEnvScale, 0.12f}, {HvEnvThresh, -36.f}, {HvWet, -4.f}}},
               {"Frozen pad", {{HvSize, 1.f}, {HvDecay, 20.f}, {HvFreeze, HfSustain}, {HvModDepth, 0.5f}, {HvWet, -10.f}}},
               {"Choir voices", {{HvFbOn, 1.f}, {HvFbMode, 1.f}, {HvFfDirect, 0.6f}, {HvFfPitch1, 12.f}, {HvFfAmp1, 0.35f}, {HvFfPitch2, 7.f},
                                 {HvFfAmp2, 0.25f}, {HvDecay, 5.f}, {HvSize, 0.7f}}}};
  p.editorWidth = 860;
  return p;
}

PluginInfo ensemble() {
  PluginInfo p{"gl.ensemble", "Ensemble", "Modulation", PluginKind::Effect, {}, false, {}};
  p.params.resize(EnCount);
  p.params[EnDelay] = lin("Delay", "ms", 1, 40, 14);
  p.params[EnFeedback] = lin("Feedback", "%", -0.9, 0.9, 0);
  p.params[EnModAmount] = lin("Mod amount", "%", 0, 1, 0.4);
  p.params[EnModRate] = lg("Mod rate", "Hz ", 0.05, 10, 0.7);
  p.params[EnMoveAmount] = lin("Movement", "%", 0, 1, 0.3);
  p.params[EnMoveRate] = lg("Movement rate", "Hz ", 0.02, 4, 0.25);
  p.params[EnLines] = ch("Delay lines", {"4", "8"}, 0);
  p.params[EnHighPass] = lg("High pass", "Hz", 20, 10900, 120);
  p.params[EnLowPass] = lg("Low pass", "Hz", 1500, 22000, 16000);
  p.params[EnPhase] = lin("LFO phase spread", "%", 0, 1, 0.5);
  p.params[EnSpread] = lin("Stereo spread", "%", 0, 1, 1);
  p.params[EnMix] = lin("Mix", "%", 0, 1, 0.5);
  p.params[EnModel] = ch("Engine", {"Legacy", "Reference"}, 1);
  {
    static const char* names[11] = {"Delay", "Feedback", "Mod rate", "Mod amount", "Movement", "Movement rate", "8 lines", "Low pass", "High pass",
                                    "Mix", "LFO phase difference"};
    static const float defs[11] = {0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0, 1, 0, 0.5f, 0.5f};
    for (int k = 0; k < 11; ++k) p.params[EnK0 + k] = k == 6 ? ch(names[k], {"4", "8"}, 0) : lin(names[k], "%", 0, 1, defs[k]);
  }
  p.legacyModelParam = EnModel;
  p.presets = {{"Lush", {}},
               {"Wide doubler", {{EnK0, 0.2f}, {EnK1, 0.f}, {EnK3, 0.3f}, {EnK4, 0.8f}, {EnK9, 0.8f}, {EnK10, 0.8f}}},
               {"Deep ensemble", {{EnK6, 1.f}, {EnK0, 0.9f}, {EnK3, 0.4f}, {EnK2, 0.2f}, {EnK9, 0.4f}, {EnK10, 0.75f}}},
               {"Bright shimmer", {{EnK8, 0.55f}, {EnK1, 0.7f}, {EnK2, 0.3f}, {EnK9, 0.5f}}},
               {"Legacy metal swirl", {{EnModel, 0.f}, {EnDelay, 3.f}, {EnFeedback, 0.7f}, {EnModAmount, 0.5f}, {EnModRate, 0.15f}}}};
  p.editorWidth = 760;
  return p;
}

PluginInfo bbdChorus() {
  PluginInfo p{"gl.bbdchorus", "Bucket Chorus", "Modulation", PluginKind::Effect, {}, false, {}};
  p.params.resize(BcCount);
  p.params[BcMode] = ch("Mode", {"I", "II", "I+II", "Custom"}, 0);
  p.params[BcTime1] = lin("Time 1", "ms", 0.2, 25, 1.7);
  p.params[BcTime2] = lin("Time 2", "ms", 0.2, 25, 5.4);
  p.params[BcRate] = lin("Speed", "Hz ", 0, 12, 0.5);  // 0 = frozen LFO (reference model)
  p.params[BcSync] = tog("Tempo sync", 0);
  p.params[BcSyncRate] = ch("Synced speed", kLfoSyncNames, 4);
  p.params[BcWave] = ch("LFO shape", {"Sine", "Triangle"}, 1);
  p.params[BcPhaseLR] = lin("L/R phase", "%", 0, 1, 0.5);
  p.params[BcStartPhase] = lin("Start phase", "%", 0, 1, 0);
  p.params[BcFeedback] = lin("Feedback", "%", 0, 0.9, 0);
  p.params[BcHighPass] = lg("High pass", "Hz", 10, 1000, 50);
  p.params[BcHiss] = lin("Hiss", "%", 0, 1, 0.5);
  p.params[BcGain] = lin("Gain", "dB", -12, 12, 0);
  p.params[BcMonoIn] = tog("Mono input", 1);
  p.params[BcInvertWet] = tog("Invert wet", 0);
  p.params[BcNoiseGate] = tog("Noise gate", 1);
  p.params[BcMix] = lin("Mix", "%", 0, 1, 0.5);
  // J-1 reference model (new instances); older projects load as legacy.
  p.params[BcModel] = ch("Chorus model", {"Legacy", "Reference"}, 1);
  p.params[BcSyncTicks] = lin("Synced period", "ticks", 1, 6144, 192);
  p.presets = {{"Mode I", {}},
               {"Mode II", {{BcMode, 1.f}}},
               {"Fast shimmer", {{BcMode, 2.f}}},
               {"Slow wide", {{BcMode, 3.f}, {BcRate, 0.18f}, {BcTime1, 3.f}, {BcTime2, 12.f}, {BcWave, 0.f}, {BcMonoIn, 0.f}}},
               {"Clean", {{BcHiss, 0.f}, {BcMode, 1.f}}},
               {"Static doubler", {{BcMode, 3.f}, {BcRate, 0.f}, {BcTime1, 7.5f}, {BcTime2, 7.5f}, {BcMix, 0.5f}, {BcMonoIn, 0.f}}}};
  p.editorWidth = 760;
  return p;
}

PluginInfo swirl() {
  PluginInfo p{"gl.swirl", "Swirl", "Modulation", PluginKind::Effect, {}, false, {}};
  p.params.resize(SwCount);
  p.params[SwOrder] = lin("Voices", "", 1, 9, 4);
  p.params[SwDepth] = lin("Depth", "%", 0, 1, 0.5);
  p.params[SwSpeed] = lg("Speed", "Hz ", 0.02, 10, 0.4);
  p.params[SwDelay] = lin("Delay", "ms", 0.1, 25, 3);
  p.params[SwSpread] = lin("Spread", "%", 0, 1, 1);
  p.params[SwCross] = lin("Cross", "%", -0.95, 0.95, 0);
  p.params[SwDry] = lin("Dry", "%", -1, 1, 1);
  p.params[SwWet] = lin("Wet", "%", -1, 1, 0.7);
  p.params[SwDepthK] = lin("Depth", "%", 0, 1, 0.5);
  p.params[SwSpeedK] = lin("Speed", "%", 0, 1, 0.75);
  p.params[SwDelayK] = lin("Delay", "%", 0, 1, 0.0625);
  p.params[SwSpreadK] = lin("Spread", "%", 0, 1, 1);
  p.params[SwCrossK] = lin("Cross", "%", -1, 1, -0.25);
  p.params[SwDryK] = lin("Dry", "%", -1, 1, 0.75);
  p.params[SwWetK] = lin("Wet", "%", -1, 1, 0.75);
  p.params[SwModel] = tog("Reference engine", 1);
  p.presets = {{"Smooth", {}},
               {"Wide flange", {{SwOrder, 6.f}, {SwDepthK, 0.55f}, {SwSpeedK, 0.83f}, {SwDelayK, 0.f}, {SwSpreadK, 0.f}, {SwCrossK, 0.3f},
                                {SwDryK, 0.f}, {SwWetK, 0.9f}}},
               {"Deep ensemble", {{SwOrder, 7.f}, {SwSpeedK, 0.5f}, {SwDelayK, 1.f}, {SwCrossK, 0.4f}, {SwDryK, 0.95f}, {SwWetK, 0.7f}}},
               {"Wobble", {{SwOrder, 1.f}, {SwDepthK, 0.05f}, {SwSpeedK, 0.95f}, {SwDelayK, 0.f}, {SwSpreadK, 0.f}, {SwDryK, 0.f}, {SwWetK, 1.f}}},
               {"Legacy swirl", {{SwModel, 0.f}}}};
  p.editorWidth = 600;
  return p;
}

PluginInfo phaser() {
  PluginInfo p{"gl.vphaser", "Retro Phaser", "Modulation", PluginKind::Effect, {}, false, {}};
  p.params.resize(VpCount);
  p.params[VpStages] = ch("Stages", {"4", "6", "8", "10", "12"}, 0);
  p.params[VpFreqMin] = lg("Min frequency", "Hz", 1.25, 20480, 180);
  p.params[VpFreqMax] = lg("Max frequency", "Hz", 1.25, 20480, 2800);
  p.params[VpManual] = tog("Manual", 0);
  p.params[VpManualFreq] = lg("Manual frequency", "Hz", 1.25, 20480, 700);
  p.params[VpFeedback] = lin("Feedback", "%", 0, 0.95, 0.35);
  p.params[VpInvertFb] = tog("Invert feedback", 0);
  p.params[VpWave] = ch("LFO shape", {"Sine", "Triangle", "Square", "Ramp up", "Ramp down", "Random"}, 0);
  p.params[VpRate] = lg("Speed", "Hz ", 0.02, 150, 0.35);
  p.params[VpSync] = tog("Tempo sync", 0);
  p.params[VpSyncRate] = ch("Synced speed", kLfoSyncNames, 6);
  p.params[VpTension] = lin("Tension", "%", 0, 1, 0.5);
  p.params[VpPhaseLR] = lin("L/R phase", "%", 0, 1, 0.25);
  p.params[VpStartPhase] = lin("Start phase", "%", 0, 1, 0);
  p.params[VpColorGain] = lin("Color gain", "dB", -12, 30, 0);
  p.params[VpColorFreq] = lg("Color frequency", "Hz", 40, 1200, 200);
  p.params[VpInputHp] = lg("Input high pass", "Hz", 5, 800, 10);
  p.params[VpFbHp] = lg("Feedback high pass", "Hz", 5, 800, 10);
  p.params[VpDrive] = lin("Drive", "dB", -15, 24, 0);
  p.params[VpNoise] = lin("Noise", "%", 0, 1, 0);
  p.params[VpNoiseGate] = tog("Noise gate", 1);
  p.params[VpMonoIn] = tog("Mono input", 0);
  p.params[VpMix] = lin("Mix", "%", 0, 1, 0.5);
  p.params[VpDelayOn] = tog("Echo on", 0);
  p.params[VpDelayTime] = lin("Echo time", "ms", 0, 1500, 280);
  p.params[VpDelaySync] = tog("Echo sync", 1);
  p.params[VpDelaySyncTime] = ch("Echo synced", kSyncNames, 4);
  p.params[VpDelayFb] = lin("Echo feedback", "%", 0, 1, 0.35);
  p.params[VpDelayMix] = lin("Echo mix", "%", 0, 1, 0.3);
  p.params[VpDelayStereo] = lin("Echo stereo", "%", 0, 1, 0.5);
  p.params[VpDelayLp] = lg("Echo low pass", "Hz", 320, 20480, 7000);
  p.params[VpDelayHp] = lg("Echo high pass", "Hz", 20, 20480, 100);
  // J-1 reference model (new instances); older projects load as legacy.
  p.params[VpModel] = ch("Phaser model", {"Legacy", "Reference"}, 1);
  p.params[VpHq] = tog("High quality", 0);
  p.params[VpEngine] = ch("Engine", {"Classic", "Analog"}, 1);
  p.params[VpRateLo] = lg("Speed range low", "Hz ", 0.025, 150, 0.025);
  p.params[VpRateHi] = lg("Speed range high", "Hz ", 0.025, 150, 150);
  p.params[VpDepthScale] = lin("Depth at speed", "%", 0, 1, 1);
  p.params[VpDepthPull] = lin("Depth pull-down", "%", 0, 1, 0);
  p.params[VpSyncTicks] = lin("Synced period", "ticks", 1, 6144, 192);
  p.params[VpDelayKeep] = tog("Echo keep pitch", 0);
  p.params[VpDelayTicks] = lin("Echo synced time", "ticks", 1, 768, 48);
  p.presets = {{"Classic four", {}},
               {"Deep twelve", {{VpModel, 0.f}, {VpStages, 4.f}, {VpFeedback, 0.6f}, {VpRate, 0.12f}}},
               {"Jet sweep", {{VpModel, 0.f}, {VpStages, 3.f}, {VpFeedback, 0.85f}, {VpWave, 1.f}, {VpRate, 0.08f}, {VpFreqMin, 120.f}, {VpFreqMax, 6000.f}}},
               {"Synced pulse", {{VpModel, 0.f}, {VpSync, 1.f}, {VpSyncRate, 4.f}, {VpWave, 2.f}, {VpTension, 0.7f}}},
               {"Echo phase", {{VpDelayOn, 1.f}, {VpFeedback, 0.5f}, {VpDelayKeep, 1.f}}},
               {"Hot analog", {{VpEngine, 1.f}, {VpHq, 1.f}, {VpDrive, 9.f}, {VpFeedback, 0.7f}, {VpColorGain, 6.f}, {VpRate, 0.2f}}}};
  p.editorWidth = 820;
  return p;
}

}  // namespace

void appendInfos(std::vector<PluginInfo>& v) {
  v.push_back(convolver());
  v.push_back(multiDelay());
  v.push_back(bandDelay());
  v.push_back(hall());
  v.push_back(ensemble());
  v.push_back(bbdChorus());
  v.push_back(swirl());
  v.push_back(phaser());
}

}  // namespace gl::packs::spacefx
