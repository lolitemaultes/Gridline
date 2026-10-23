// Gridline — Resynth (gl.harmor): catalogue entry and factory presets.
#include <string>
#include <vector>
#include "../Packs.h"
#include "harmor.h"
#include "models.h"

namespace gl::packs::models {

namespace {

using namespace hm;

ParamDesc lin(const std::string& n, const char* u, double mn, double mx, double d) {
  return ParamDesc{n, u, mn, mx, d, ParamKind::Continuous, ParamScale::Linear, {}};
}
ParamDesc lg(const std::string& n, const char* u, double mn, double mx, double d) {
  return ParamDesc{n, u, mn, mx, d, ParamKind::Continuous, ParamScale::Log, {}};
}
ParamDesc tog(const std::string& n, bool d) { return ParamDesc{n, "", 0, 1, d ? 1.0 : 0.0, ParamKind::Toggle, ParamScale::Linear, {}}; }
ParamDesc ch(const std::string& n, std::vector<std::string> c, int d) {
  const double hi = static_cast<double>(c.size() - 1);
  return ParamDesc{n, "", 0, hi, static_cast<double>(d), ParamKind::Choice, ParamScale::Linear, std::move(c)};
}

}  // namespace

PluginInfo harmorInfo() {
  PluginInfo p{"gl.harmor", "Halftone", "Synth", PluginKind::Generator, {}, false, {}};
  auto& q = p.params;
  q.resize(HmParamCount);
  const std::vector<std::string> fltTypes = {"Low pass", "Band pass", "Band stop", "High pass", "Soft LP",  "Soft BP",   "Soft BS",
                                             "Soft HP",  "Hill LP",   "Slope LP",  "Blend LP",  "Ripple LP", "Edge LP",   "Leak LP",
                                             "Comb",     "Band comb", "Flat BP",   "Off",       "Curve 1",   "Curve 2"};
  const std::vector<std::string> resTypes = {"Peak", "Spring", "Plateau", "Hat", "Bump", "Twin", "Dip", "Ramp", "Shelf", "Noise", "Curve 1", "Curve 2"};
  for (int pt = 0; pt < kParts; ++pt) {
    const std::string P = pt == 0 ? "A " : "B ";
    auto at = [&](int f) -> ParamDesc& { return q[static_cast<size_t>(pf(pt, f))]; };
    at(PfEnabled) = tog(P + "on", pt == 0);
    at(PfPhase) = lin(P + "start phase", "cyc", 0, 1, 0.5);
    at(PfPhaseRand) = lin(P + "phase random", "", -1, 1, 0);
    at(PfTimbreMix) = lin(P + "timbre mix", "%", 0, 1, 0);
    at(PfBlend) = ch(P + "blend", {"Fade", "Subtract", "Multiply", "Maximum", "Minimum", "Pluck"}, 0);
    at(PfSub1) = lin(P + "sub 1", "", 0, 2, 0);
    at(PfSub3) = lin(P + "sub 3", "", 0, 2, 0);
    at(PfSub4) = lin(P + "sub 4", "", 0, 2, 0);
    at(PfSubMode) = ch(P + "sub mode", {"Around", "Below"}, 0);
    at(PfProtect) = lin(P + "protection", "", 0, 1, 0);
    at(PfClip) = lin(P + "clip", "", 0, 1, 1);
    at(PfClipMode) = ch(P + "clip mode", {"High", "Soft low", "Sharp low", "Hard low", "Subtract"}, 0);
    at(PfFxMix) = lin(P + "FX / dry", "", -1, 1, 0);
    at(PfVolume) = lin(P + "volume", "", -2, 2, 1);
    at(PfEnvVolume) = lin(P + "env volume", "", 0, 4, 1);
    at(PfAutoGain) = lin(P + "auto-gain", "%", 0, 1, 0);
    at(PfVelAttTime) = tog(P + "vel > attack", false);
    at(PfVelAttLen) = tog(P + "vel > attack scale", false);
    at(PfVelRelLen) = tog(P + "vel > release", false);
    at(PfTremolo) = lin(P + "tremolo", "%", 0, 1, 0);
    at(PfTremHz) = lg(P + "tremolo speed", "Hz", 0.0156, 64, 4);
    at(PfTremGap) = lin(P + "tremolo gap", "", 0, 1, 0.5);
    at(PfBlurMix) = lin(P + "blur mix", "%", 0, 1, 1);
    at(PfTBlurAtt) = lin(P + "time blur att", "s", 0, 10, 0);
    at(PfTBlurDec) = lin(P + "time blur dec", "s", 0, 10, 0);
    at(PfHBlur) = lg(P + "harmonic blur", "", 1, 256, 1);
    at(PfHBlurLo) = lin(P + "blur low", "", 0, 1, 0.5);
    at(PfHBlurHi) = lin(P + "blur high", "", 0, 1, 0.5);
    at(PfPrism) = lin(P + "prism", "", -1, 1, 0);
    at(PfPrismMul) = tog(P + "prism multiply", false);
    at(PfHzMix) = lin(P + "harmonizer", "%", 0, 1, 0);
    at(PfHzWidth) = lin(P + "harm width", "%", 0, 1, 0.5);
    at(PfHzStrength) = lin(P + "harm strength", "%", 0, 1, 0);
    at(PfHzShift) = lin(P + "harm shift", "", -2, 7, -1);
    at(PfHzShiftStep) = lin(P + "harm shift step", "", -7, 8, -1);
    at(PfHzGap) = lin(P + "harm gap", "", -1, 8, 0);
    at(PfHzGapStep) = lin(P + "harm gap step", "", -7, 8, -1);
    at(PfUniOrder) = lin(P + "unison", "", 1, 9, 1);
    at(PfUniType) = ch(P + "unison type", {"Spread", "Smeared", "Even", "Random", "Hz"}, 0);
    at(PfUniAlt) = tog(P + "unison alternate", false);
    at(PfUniPan) = lin(P + "unison pan", "", 0, 4, 1);
    at(PfUniPitch) = lin(P + "unison pitch", "ct", 0, 75, 11);
    at(PfUniPhase) = lin(P + "unison phase", "", -1, 1, 0.5);
    at(PfFreqMul) = lin(P + "freq x", "", 1, 16, 1);
    at(PfFreqDiv) = lin(P + "freq /", "", 1, 9, 1);
    at(PfDetMul) = lin(P + "detune x", "", 1, 16, 1);
    at(PfDetDiv) = lin(P + "detune /", "", 1, 9, 1);
    at(PfPitchArt) = lin(P + "pitch env range", "ct", -6000, 6000, 1200);
    at(PfVibrato) = lin(P + "vibrato", "ct", 0, 100, 0);
    at(PfVibHz) = lg(P + "vibrato speed", "Hz", 0.125, 8, 5.66);
    at(PfPortaTime) = lin(P + "glide", "s", 0, 1.5, 0.08);
    at(PfPortaLimit) = lin(P + "glide limit", "ct", 0, 1200, 1200);
    at(PfPortaLog) = tog(P + "glide curve", true);
    at(PfPortaVar) = tog(P + "glide variable", false);
    for (int k = 0; k < 2; ++k) {
      const std::string F = P + "F" + std::to_string(k + 1) + " ";
      auto fl = [&](int f) -> ParamDesc& { return q[static_cast<size_t>(flt(pt, k, f))]; };
      fl(PfF1Type) = ch(F + "shape", fltTypes, k == 0 ? 4 : 17);
      fl(PfF1ResType) = ch(F + "reso shape", resTypes, 0);
      fl(PfF1Cut) = lin(F + "freq", "", 0, 1, 1);
      fl(PfF1Width) = lin(F + "width", "", 0, 1, 0.5);
      fl(PfF1Key) = lin(F + "key", "", -2, 2, 0);
      fl(PfF1Env) = lin(F + "env", "", -1, 1, 1);
      fl(PfF1Res) = lin(F + "reso", "%", 0, 1, 0);
      fl(PfF1ResWidth) = lin(F + "reso width", "", 0, 1, 0.5);
      fl(PfF1ResOfs) = lin(F + "reso offset", "ct", -2400, 2400, 0);
      fl(PfF1Osc) = lin(F + "self-osc", "%", 0, 1, 0);
      fl(PfF1Adapt) = tog(F + "adaptive env", false);
      fl(PfF1ResAdapt) = tog(F + "adaptive reso", false);
    }
    at(PfFMix) = lin(P + "filter mix", "", -1, 1, 1);
    at(PfPluck) = lin(P + "pluck decay", "", 0, 1, 1);
    at(PfPhType) = ch(P + "phaser shape", {"Notch", "Triangle", "Round", "Deep", "Deeper", "Needle", "Twin", "Cascade", "Block", "Linear", "Curve"}, 0);
    at(PfPhMix) = lin(P + "phaser", "%", 0, 1, 0);
    at(PfPhWidth) = lin(P + "phaser width", "", 0, 1, 0.5);
    at(PfPhOfs) = lin(P + "phaser offset", "", 0, 1, 0.5);
    at(PfPhMotion) = lin(P + "phaser motion", "", -8, 8, 3);
    at(PfPhKey) = lin(P + "phaser key", "", -2, 2, 0);
    at(PfPhScale) = ch(P + "phaser scale", {"Octaves", "Hz", "Wide"}, 0);
    at(PfImgGainMix) = lin(P + "image mix", "%", 0, 1, 1);
    at(PfImgGainScale) = lin(P + "image gain", "", 0, 1, 0.5);
    at(PfImgTime) = lin(P + "image start", "%", 0, 1, 0);
    at(PfImgTimeSmooth) = lin(P + "image smoothing", "s", 0, 2, 0.5);
    at(PfImgSpeed) = lin(P + "image speed", "", -2, 2, 1);
    at(PfImgSpeedOct) = lin(P + "image speed oct", "oct", -6, 6, 0);
    at(PfImgSharp) = lin(P + "image sharpen", "", -1, 1, 0.75);
    at(PfImgFreqMode) = ch(P + "image freq mode", {"Octaves", "Hz", "Wide Hz", "Unison"}, 0);
    at(PfImgFreqScale) = lin(P + "image freq scale", "", -2, 2, 1);
    at(PfImgFormant) = lin(P + "formant", "st", -6, 6, 0);
    at(PfImgFormantMix) = lin(P + "formant mix", "%", 0, 1, 1);
    at(PfImgLoop) = ch(P + "image loop", {"One-shot", "Loop", "Ping-pong"}, 1);
    at(PfPredelay) = lin(P + "predelay", "s", 0, 1, 0);
    at(PfRelMul) = lin(P + "poly release", "", 0, 1, 1);
  }
  q[GmModX] = lin("Mod X", "", 0, 1, 0.5);
  q[GmModY] = lin("Mod Y", "", 0, 1, 0.5);
  q[GmModZ] = lin("Mod Z", "", 0, 1, 0.5);
  q[GmLegato] = tog("Legato", false);
  q[GmPorta] = tog("Glide", false);
  q[GmVelPorta] = tog("Vel > glide", false);
  q[GmLegatoLimit] = lin("Legato limit", "ct", 0, 2400, 0);
  q[GmStrumMode] = ch("Strum mode", {"Up", "Down", "Alternate", "Random"}, 0);
  q[GmStrumTime] = lin("Strum time", "%", 0, 1, 0);
  q[GmStrumTension] = lin("Strum tension", "", -1, 1, 0);
  q[GmPartMix] = lin("Part mix", "", 0, 1, 0.5);
  q[GmVolume] = lin("Volume", "", 0, 4, 1);
  q[GmPostVolume] = lin("Post volume", "", 0, 4, 1);
  q[GmVelVol] = tog("Vel > volume", true);
  q[GmPitch] = lin("Pitch", "ct", -2400, 2400, 0);
  q[GmLfoAmt] = lin("LFO amount", "", -1, 1, 1);
  q[GmMono] = ch("Mono", {"Off", "Cut same", "Cut all", "Release all"}, 0);
  q[GmVoices] = lin("Voices", "", 1, 16, 12);
  q[FxDistType] = ch("Drive type", {"Off", "Soft", "Hill", "Flat", "Cube", "Log", "Saturate", "Rubber", "Ribbon", "Sine fold", "Crush", "Jam"}, 0);
  q[FxDistDrive] = lin("Drive", "%", 0, 1, 0.5);
  q[FxDistAsym] = lin("Drive asym", "", -0.5, 0.5, 0);
  q[FxDistOut] = lin("Drive out", "", 0, 2, 1);
  q[FxDistMix] = lin("Drive mix", "", -1, 1, 0);
  q[FxDistTone] = lg("Drive tone", "Hz", 500, 20000, 12000);
  q[FxChOrder] = lin("Chorus voices", "", 0, 9, 0);
  q[FxChDepth] = lin("Chorus depth", "%", 0, 1, 0.5);
  q[FxChSpeed] = lin("Chorus speed", "%", 0, 1, 0.75);
  q[FxChDelay] = lin("Chorus delay", "%", 0, 1, 0.0625);
  q[FxChSpread] = lin("Chorus spread", "%", 0, 1, 1);
  q[FxChCross] = lin("Chorus cross", "", -1, 1, -0.25);
  q[FxChMix] = lin("Chorus mix", "", -1, 1, 0.5);
  q[FxDlOn] = tog("Delay", false);
  q[FxDlMode] = ch("Delay mode", {"Straight", "Ping-pong", "Wide"}, 1);
  q[FxDlFeedback] = lin("Delay feedback", "%", 0, 1, 0.3);
  q[FxDlTime] = lin("Delay time", "beats", 0.01, 4, 1.5);
  q[FxDlOffset] = lin("Delay offset", "", -1, 1, 0);
  q[FxDlVol] = lin("Delay level", "", -1, 1, 0.5);
  q[FxDlPan] = lin("Delay pan", "", -1, 1, 0);
  q[FxDlLowCut] = lg("Delay low cut", "Hz", 16, 20000, 16);
  q[FxDlHighCut] = lg("Delay high cut", "Hz", 300, 20000, 20000);
  q[FxDlDamp] = tog("Delay damping", false);
  q[FxRvOn] = tog("Reverb", false);
  q[FxRvLowCut] = lg("Reverb low cut", "Hz", 16, 20000, 60);
  q[FxRvHighCut] = lg("Reverb high cut", "Hz", 300, 20000, 6000);
  q[FxRvPredelay] = lin("Reverb predelay", "s", 0, 0.5, 0);
  q[FxRvRoom] = lin("Reverb room", "%", 0, 1, 0.4);
  q[FxRvDiffusion] = lin("Reverb diffusion", "%", 0, 1, 1);
  q[FxRvDecay] = lin("Reverb decay", "s", 0.1, 9, 3.9);
  q[FxRvDamp] = lg("Reverb damping", "Hz", 300, 20000, 6000);
  q[FxRvColor] = lin("Reverb colour", "", 0, 4, 2);
  q[FxRvMix] = lin("Reverb mix", "", -1, 1, 0.5);
  q[FxCompType] = ch("Comp type", {"Off", "Limit", "Warm", "Hot", "Burn"}, 0);
  q[FxCompAmount] = lin("Comp amount", "%", 0, 1, 0.5);
  q[FxCompLow] = lin("Comp low", "", 0, 2, 1);
  q[FxCompMid] = lin("Comp mid", "", 0, 2, 1);
  q[FxCompHigh] = lin("Comp high", "", 0, 2, 1);
  q[FxEqMix] = lin("EQ curve mix", "%", 0, 1, 1);
  using Ov = std::vector<std::pair<int, float>>;
  p.presets = {
      {"Init saw", {}},
      {"Phased pad", Ov{{pf(0, PfPhMix), 0.8f}, {pf(0, PfPhMotion), 1.5f}, {pf(0, PfUniOrder), 5}, {pf(0, PfUniPitch), 18}, {pf(0, PfHBlur), 3},
                        {pf(0, PfTBlurAtt), 0.6f}, {pf(0, PfTBlurDec), 1.2f}, {flt(0, 0, PfF1Cut), 0.75f}, {FxRvOn, 1}, {FxChOrder, 3}}},
      {"Prism keys", Ov{{pf(0, PfPrism), 0.35f}, {pf(0, PfPluck), 0.45f}, {flt(0, 0, PfF1Cut), 0.85f}, {flt(0, 0, PfF1Key), 1},
                        {pf(0, PfTremolo), 0.15f}, {FxDlOn, 1}, {FxDlFeedback, 0.25f}}},
      {"Two-part stack", Ov{{pf(1, PfEnabled), 1}, {pf(1, PfFreqMul), 2}, {pf(1, PfTimbreMix), 1}, {pf(1, PfHzMix), 0.6f},
                            {flt(1, 0, PfF1Cut), 0.6f}, {flt(1, 0, PfF1Res), 0.5f}, {GmPartMix, 0.45f}, {FxDistType, 1}, {FxDistDrive, 0.3f}}},
      {"Swept band", Ov{{flt(0, 0, PfF1Type), 1}, {flt(0, 0, PfF1Cut), 0.55f}, {flt(0, 0, PfF1Res), 0.6f}, {flt(0, 0, PfF1Width), 0.3f},
                        {pf(0, PfVibrato), 15}, {pf(0, PfUniOrder), 3}, {FxRvOn, 1}, {FxRvMix, 0.3f}}},
  };
  p.editorWidth = 1100;
  return p;
}

}  // namespace gl::packs::models
