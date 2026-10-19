// Gridline — "synths" pack, J-22 group "dco": catalogue entries and factory
// presets of DCO Synth (gl.dcosynth), Warm Trio (gl.warmsynth) and FM Duo
// (gl.fmduo). Continuous controls keep the reference's 0..1 positions (shown
// as %); the editors and runtimes apply the value laws (see the *_dsp.cpp
// headers).
#include <string>
#include <vector>
#include "../Packs.h"
#include "dco.h"

namespace gl::packs::synths {

namespace {

using namespace dco;

ParamDesc pct(const std::string& n, double d) { return ParamDesc{n, "%", 0, 1, d, ParamKind::Continuous, ParamScale::Linear, {}}; }
ParamDesc lin(const std::string& n, const char* u, double mn, double mx, double d) {
  return ParamDesc{n, u, mn, mx, d, ParamKind::Continuous, ParamScale::Linear, {}};
}
ParamDesc tog(const std::string& n, bool d) { return ParamDesc{n, "", 0, 1, d ? 1.0 : 0.0, ParamKind::Toggle, ParamScale::Linear, {}}; }
ParamDesc ch(const std::string& n, std::vector<std::string> c, int d) {
  const double hi = static_cast<double>(c.size() - 1);
  return ParamDesc{n, "", 0, hi, static_cast<double>(d), ParamKind::Choice, ParamScale::Linear, std::move(c)};
}

// ------------------------------------------------------------ DCO Synth
// Defaults: the reference's own initial patch (one saw, gate VCA, open filter).
constexpr float kDcDefaults[151] = {
    0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 1, 0.5f, 0, 0.5f, 0.33f,                              // 0..14
    1, 0, 1, 0.5f, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0,                                     // 15..29
    0, 0, 1, 1, 0, 0, 0, 1, 0, 0, 1, 0, 0, 0, 0,                                        // 30..44
    0, 0, 0.5f, 0.5369f, 0, 0, 0, 0, 0, 0.5f, 0, 0, 0, 0, 0,                            // 45..59
    0, 0, 0.5f, 0.5f, 0, 0, 0.5f, 0.5f, 0, 0, 0, 1, 0.6311f, 0.7504f, 0.5f,             // 60..74
    0, 0, 0, 0, 0, 0, 0, 0, 0.5f, 0, 0, 0.5f, 1, 0, 0,                                  // 75..89
    1, 0, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0, 0.5f, 0.5f, 0.5f, 0.5f,    // 90..104
    0.5f, 0.5f, 0.2376f, 0.5f, 0.7823f, 0, 0, 0.5f, 0.6072f, 0.5003f, 0.507f, 0.7498f, 0, 0.5f, 0,  // 105..119
    0.75f, 0, 0.2013f, 0.7865f, 0, 0.5231f, 0.5231f, 0.7762f, 0.382f, 0.2256f, 0.5231f, 0.8003f, 0.8003f,
    0.5231f, 0,                                                                         // 120..134
    0, 0, 0, 0, 0, 0.4977f, 0.4977f, 0.4977f, 0.4977f, 0.5f, 0, 0.8058f, 0.16f, 0.5023f, 1,  // 135..149
    0.5f};                                                                               // 150

}  // namespace

std::vector<std::string> dcoSourceNames() {
  return {"None",          "LFO 1 bi (tri)", "LFO 2 bi",     "Env 1",         "Env 2",          "Velocity",
          "Rel. velocity", "Mod X bi",       "Mod Y bi",     "Mod X uni",     "Mod Y uni",      "Gate",
          "Gate inverted", "LFO 1 bi (sine)", "LFO 1 uni",   "LFO 2 uni",     "Key bi",         "LFO 1 bi fast",
          "LFO 2 bi fast", "LFO 1 sine fast", "LFO 1 uni fast", "LFO 2 uni fast", "Alternate",   "Key uni",
          "Random"};
}

PluginInfo dcoSynthInfo() {
  PluginInfo p{"gl.dcosynth", "DCO Synth", "Synth", PluginKind::Generator, {}, false, {}};
  auto& q = p.params;
  q.resize(DcCount);
  const std::vector<std::string> pwSrc = {"LFO", "Manual", "Env"};
  const std::vector<std::string> xShapes = {"Sine", "Triangle", "Ramp up", "Ramp down", "Square", "Osc out"};
  const std::vector<std::string> lfo2Shapes = {"Triangle", "Sine", "Square", "Ramp up", "Ramp down", "Random"};
  auto def = [&](int i) { return static_cast<double>(kDcDefaults[i]); };
  auto P = [&](int i, const char* n) { q[static_cast<size_t>(i)] = pct(n, def(i)); };
  auto T = [&](int i, const char* n) { q[static_cast<size_t>(i)] = tog(n, def(i) >= 0.5); };
  auto C = [&](int i, const char* n, std::vector<std::string> c) {
    const int k = static_cast<int>(c.size());
    q[static_cast<size_t>(i)] = ch(n, std::move(c), static_cast<int>(def(i) * (k - 1) + 0.5));
  };
  P(DcLfo1ToDco1, "DCO 1 LFO 1");
  T(DcLfo1ToDco1Wide, "DCO 1 LFO 1 wide");
  P(DcLfo2ToDco1, "DCO 1 LFO 2");
  T(DcLfo2ToDco1Wide, "DCO 1 LFO 2 wide");
  P(DcEnv2ToDco1, "DCO 1 env 2");
  P(DcDco1Pw, "DCO 1 PW");
  C(DcDco1PwSrc, "DCO 1 PW source", pwSrc);
  T(DcDco1Pulse, "DCO 1 pulse");
  T(DcDco1Saw, "DCO 1 saw");
  T(DcDco1Sub, "DCO 1 sub");
  P(DcDco1SubLevel, "DCO 1 sub level");
  P(DcDco1SubPhase, "DCO 1 sub phase");
  T(DcDco2On, "DCO 2 on");
  P(DcDco2Semi, "DCO 2 semitones");
  P(DcDco2Detune, "DCO 2 detune");
  T(DcDco2Saw, "DCO 2 saw");
  T(DcDco2Sub, "DCO 2 sub");
  P(DcDco2SubLevel, "DCO 2 sub level");
  P(DcDco2SubPhase, "DCO 2 sub phase");
  P(DcDco2Pw, "DCO 2 PW");
  P(DcLfo1ToDco2, "DCO 2 LFO 1");
  T(DcLfo1ToDco2Wide, "DCO 2 LFO 1 wide");
  P(DcLfo2ToDco2, "DCO 2 LFO 2");
  T(DcLfo2ToDco2Wide, "DCO 2 LFO 2 wide");
  P(DcEnv2ToDco2, "DCO 2 env 2");
  C(DcDco2PwSrc, "DCO 2 PW source", pwSrc);
  T(DcDco2Pulse, "DCO 2 pulse");
  T(DcSync, "Hard sync");
  P(DcXmod12, "Cross mod 1>2");
  P(DcXmod21, "Cross mod 2>1");
  C(DcXmod12Shape, "Cross mod 1>2 shape", xShapes);
  C(DcXmod21Shape, "Cross mod 2>1 shape", xShapes);
  P(DcDco1Level, "DCO 1 level");
  P(DcDco2Level, "DCO 2 level");
  P(DcNoise, "Noise");
  P(DcAm, "DCO 1/2 AM");
  P(DcHpf, "High pass");
  P(DcCutoff, "Cutoff");
  P(DcReso, "Resonance");
  P(DcKeyFollow, "Key follow");
  C(DcSlope, "Slope", {"6 dB", "12 dB", "18 dB", "24 dB"});
  P(DcFilterFm, "Filter FM");
  C(DcFilterFmShape, "Filter FM source", xShapes);
  C(DcEnvPolarity, "Env polarity", {"Normal", "Inverted"});
  P(DcEnv1ToCut, "Env 1 > cutoff");
  P(DcLfo1ToCut, "LFO 1 > cutoff");
  P(DcLfo2ToCut, "LFO 2 > cutoff");
  C(DcVcaSrc, "VCA", {"Env 1", "Gate", "Env 2"});
  P(DcLfo1Rate, "LFO 1 rate");
  T(DcLfo1Sync, "LFO 1 tempo sync");
  P(DcLfo1Delay, "LFO 1 delay");
  C(DcLfo1Trig, "LFO 1 trigger", {"Free", "Key"});
  T(DcLfo1PhaseOn, "LFO 1 retrig phase");
  P(DcLfo1Phase, "LFO 1 phase");
  P(DcLfo2Rate, "LFO 2 rate");
  T(DcLfo2Sync, "LFO 2 tempo sync");
  P(DcLfo2Delay, "LFO 2 delay");
  C(DcLfo2Trig, "LFO 2 trigger", {"Free", "Key"});
  C(DcLfo2Shape, "LFO 2 shape", lfo2Shapes);
  T(DcLfo2PhaseOn, "LFO 2 retrig phase");
  P(DcLfo2Phase, "LFO 2 phase");
  const char* envN[8] = {"Env 1 attack", "Env 1 decay", "Env 1 sustain", "Env 1 release",
                         "Env 2 attack", "Env 2 decay", "Env 2 sustain", "Env 2 release"};
  for (int i = 0; i < 8; ++i) P(DcEnv1A + i, envN[i]);
  C(DcChorus, "Chorus", {"Off", "I", "II", "I+II"});
  P(DcChorusNoise, "Chorus noise");
  T(DcChorusGate, "Chorus noise gate");
  P(DcVolume, "Volume");
  P(DcVelVol, "Velocity > volume");
  P(DcPan, "Pan");
  T(DcMono, "Mono");
  T(DcLegato, "Legato");
  T(DcRetrig, "Retrigger envelopes");
  T(DcRelease, "Restart envelopes on release");
  C(DcPortaMode, "Portamento mode", {"Exponential", "Linear"});
  P(DcPorta, "Portamento");
  T(DcLfoTrigger, "LFO trigger");
  T(DcHold, "Hold");
  C(DcOctave, "Octave", {"-1", "0", "+1"});
  P(DcBend, "Bend range");
  T(DcArpOn, "Arpeggiator");
  P(DcArpRate, "Arp rate");
  T(DcArpSync, "Arp tempo sync");
  C(DcArpRange, "Arp range", {"1 oct", "2 oct", "3 oct"});
  C(DcArpMode, "Arp mode",
    {"Up", "Down", "Up & down", "Random 1", "Random 2", "Random 3", "As played up", "As played down", "As played up & down"});
  P(DcArpGate, "Arp gate");
  T(DcArpVariant, "Arp classic order");
  for (int r = 0; r < kDcMatrix; ++r) P(DcMxAmt1 + r, ("Matrix " + std::to_string(r + 1) + " amount").c_str());
  T(DcEqOn, "EQ");
  const char* eqN[9] = {"EQ low Q", "EQ mid Q", "EQ high Q", "EQ low gain", "EQ mid gain", "EQ high gain",
                        "EQ low freq", "EQ mid freq", "EQ high freq"};
  for (int i = 0; i < 9; ++i) P(DcEqQLo + i, eqN[i]);
  T(DcDlyOn, "Delay");
  T(DcDlySync, "Delay tempo sync");
  P(DcDlyInput, "Delay input");
  P(DcDlyTime, "Delay time");
  P(DcDlyRightTap, "Delay right tap");
  P(DcDlyFb, "Delay feedback");
  P(DcDlyFbCut, "Delay feedback cutoff");
  C(DcDlyFbType, "Delay feedback filter", {"Low pass", "High pass", "Band pass", "Off"});
  P(DcDlyStereo, "Delay stereo");
  P(DcDlyBlur, "Delay blur");
  P(DcDlyTone, "Delay tone");
  T(DcRevOn, "Reverb");
  const char* revN[12] = {"Reverb low cut", "Reverb high cut", "Reverb predelay", "Reverb predelay feedback",
                          "Reverb size", "Reverb diffusion", "Reverb decay", "Reverb damping",
                          "Reverb width", "Reverb dry", "Reverb early", "Reverb wet"};
  for (int i = 0; i < 12; ++i) P(DcRevLowCut + i, revN[i]);
  T(DcSatOn, "Saturation");
  P(DcSatAmount, "Saturation amount");
  C(DcSatPost, "Saturation position", {"Pre", "Post"});
  T(DcEnsOn, "Ensemble");
  const char* ensN[11] = {"Ensemble delay", "Ensemble feedback", "Ensemble rate", "Ensemble depth",
                          "Ensemble movement", "Ensemble movement rate", "", "Ensemble low pass",
                          "Ensemble high pass", "Ensemble mix", "Ensemble phase"};
  for (int i = 0; i < 11; ++i)
    if (i != 6) P(DcEnsDelay + i, ensN[i]);
  C(DcEnsLines, "Ensemble lines", {"2", "4"});
  P(DcXOffset, "X offset");
  P(DcYOffset, "Y offset");
  q[DcVcaSmooth] = tog("VCA smooth", false);
  q[DcSatHq] = tog("Saturation HQ", false);
  q[DcMasterTune] = pct("Master tune", 0.5);
  q[DcHpfOn] = tog("High pass on", true);
  std::vector<std::string> dst;
  dst.reserve(kDcDestCount);
  dst.push_back("None");
  for (int t = 0; t < kDcTags; ++t) dst.push_back(q[static_cast<size_t>(t)].name);
  dst.push_back("DCO 1 pitch");
  dst.push_back("DCO 2 pitch");
  for (int r = 0; r < kDcMatrix; ++r) {
    q[static_cast<size_t>(DcMxSrc1 + r)] = ch("Matrix " + std::to_string(r + 1) + " source", dcoSourceNames(), 0);
    q[static_cast<size_t>(DcMxDst1 + r)] = ch("Matrix " + std::to_string(r + 1) + " target", dst, 0);
  }
  q[DcPanel] = ch("Panel", {"Two DCO", "One DCO"}, PanelTwo);
  q[DcArpQuantize] = tog("Arp quantize first step", false);

  using V = std::vector<std::pair<int, float>>;
  p.presets = {
      {"Init", {}},
      {"Polar Strings",
       V{{DcDco1Pulse, 1}, {DcDco1Saw, 1}, {DcDco1Pw, 0.55f}, {DcDco1PwSrc, 0}, {DcDco2On, 1}, {DcDco2Saw, 1},
         {DcDco2Detune, 0.42f}, {DcCutoff, 0.62f}, {DcReso, 0.12f}, {DcEnv1ToCut, 0.18f}, {DcVcaSrc, 0},
         {DcEnv1A, 0.32f}, {DcEnv1D, 0.6f}, {DcEnv1S, 0.8f}, {DcEnv1R, 0.48f}, {DcChorus, 3}, {DcLfo1Rate, 0.42f},
         {DcRevOn, 1}, {DcRevWet, 0.42f}}},
      {"Rubber Bass",
       V{{DcDco1Pulse, 1}, {DcDco1Saw, 1}, {DcDco1Sub, 1}, {DcDco1SubLevel, 0.8f}, {DcCutoff, 0.28f}, {DcReso, 0.45f},
         {DcEnv1ToCut, 0.62f}, {DcVcaSrc, 0}, {DcEnv1A, 0}, {DcEnv1D, 0.38f}, {DcEnv1S, 0.15f}, {DcEnv1R, 0.18f},
         {DcMono, 1}, {DcLegato, 1}, {DcPorta, 0.12f}, {DcOctave, 0}, {DcEqOn, 0}}},
      {"Glass Sync Lead",
       V{{DcDco2On, 1}, {DcDco2Saw, 1}, {DcDco2Semi, 0.62f}, {DcSync, 1}, {DcEnv2ToDco2, 0.78f}, {DcEnv2A, 0},
         {DcEnv2D, 0.45f}, {DcEnv2S, 0}, {DcDco1Level, 0.4f}, {DcCutoff, 0.8f}, {DcVcaSrc, 1}, {DcMono, 1},
         {DcPorta, 0.08f}, {DcDlyOn, 1}, {DcDlyInput, 0.35f}}},
      {"Night Arp",
       V{{DcDco1Pulse, 1}, {DcDco1Saw, 0}, {DcDco1Pw, 0.3f}, {DcDco1PwSrc, 1}, {DcCutoff, 0.45f}, {DcReso, 0.35f},
         {DcEnv1ToCut, 0.5f}, {DcVcaSrc, 0}, {DcEnv1D, 0.3f}, {DcEnv1S, 0}, {DcArpOn, 1}, {DcArpMode, 2},
         {DcArpRange, 1}, {DcArpSync, 1}, {DcArpRate, 0.75f}, {DcChorus, 1}, {DcDlyOn, 1}}},
      {"Warm Pad One",
       V{{DcPanel, PanelOne}, {DcDco1Saw, 1}, {DcDco1Sub, 1}, {DcDco1SubLevel, 0.5f}, {DcCutoff, 0.5f},
         {DcEnv1ToCut, 0.2f}, {DcVcaSrc, 0}, {DcEnv1A, 0.45f}, {DcEnv1D, 0.6f}, {DcEnv1S, 0.7f}, {DcEnv1R, 0.55f},
         {DcChorus, 2}}},
  };
  p.editorWidth = 1080;
  return p;
}

// ------------------------------------------------------------ Warm Trio
PluginInfo warmSynthInfo() {
  PluginInfo p{"gl.warmsynth", "Warm Trio", "Synth", PluginKind::Generator, {}, false, {}};
  auto& q = p.params;
  q.resize(WmCount);
  for (int k = 0; k < 3; ++k) {
    const std::string o = "Osc " + std::to_string(k + 1) + " ";
    q[static_cast<size_t>(WmOsc1Level + 3 * k)] = pct(o + "level", 0.5);
    q[static_cast<size_t>(WmOsc1Lfo + 3 * k)] = pct(o + "LFO", 0.5);
    q[static_cast<size_t>(WmOsc1Env + 3 * k)] = pct(o + "env", 0.5);
    q[static_cast<size_t>(WmOsc1Pw + 3 * k)] = pct(o + "pulse width", 0.5);
    q[static_cast<size_t>(WmOsc1Coarse + 3 * k)] = lin(o + "coarse", "st", -24, 24, 0);
    q[static_cast<size_t>(WmOsc1Fine + 3 * k)] = pct(o + "fine", 0.5);
    q[static_cast<size_t>(WmOsc1On + k)] = tog(o + "on", k == 0);
    q[static_cast<size_t>(WmOsc1Oct1 + 3 * k)] = tog(o + "+1 octave", false);
    q[static_cast<size_t>(WmOsc1Oct2 + 3 * k)] = tog(o + "+2 octaves", false);
    q[static_cast<size_t>(WmOsc1Warm + 3 * k)] = tog(o + "warm", false);
    q[static_cast<size_t>(WmOsc1Shape + k)] = ch(o + "shape", {"Pulse", "Saw", "Triangle", "Noise", "Sine"}, WsSaw);
  }
  q[WmLfoDelay] = pct("LFO delay", 0);
  q[WmLfoRate] = pct("LFO rate", 0.5);
  q[WmCut] = pct("Cutoff", 0.5);
  q[WmEmph] = pct("Emphasis", 0);
  q[WmSvfEnv] = pct("Filter env", 0.5);
  q[WmSvfLfo] = pct("Filter LFO", 0.5);
  q[WmSvfHigh] = pct("Filter high", 0);
  q[WmSvfBand] = pct("Filter band", 0);
  q[WmAmpA] = pct("Attack", 0);
  q[WmAmpD] = pct("Decay", 0.5);
  q[WmAmpS] = pct("Sustain", 0.5);
  q[WmAmpR] = pct("Release", 10000.0 / 65535.0);
  q[WmVcaVel] = pct("Velocity", 1);
  q[WmSvfA] = pct("Filter attack", 0);
  q[WmSvfD] = pct("Filter decay", 0.5);
  q[WmSvfS] = pct("Filter sustain", 0.5);
  q[WmSvfR] = pct("Filter release", 10000.0 / 65535.0);
  q[WmSvfKb] = pct("Filter key track", 0);
  q[WmChorus] = tog("Chorus", false);
  q[WmRetrig] = tog("Retrigger", false);
  q[WmTrackAmp] = tog("Filter tracks amp", false);
  q[WmLfoOn] = tog("LFO", false);
  q[WmSvfOn] = tog("Filter", false);
  q[WmRing] = tog("Ring 1x2", false);
  q[WmLfoShape] = ch("LFO shape", {"Square", "Ramp", "Triangle", "Random"}, 2);
  using V = std::vector<std::pair<int, float>>;
  p.presets = {
      {"Init", {}},
      {"Soft Brass",
       V{{WmOsc1Shape, WsSaw}, {WmOsc2On, 1}, {WmOsc2Shape, WsSaw}, {WmOsc2Fine, 0.53f}, {WmOsc2Level, 0.5f}, {WmSvfOn, 1},
         {WmCut, 0.3f}, {WmEmph, 0.25f}, {WmSvfEnv, 0.75f}, {WmSvfA, 0.12f}, {WmSvfD, 0.35f}, {WmSvfS, 0.4f},
         {WmAmpA, 0.08f}, {WmAmpS, 0.85f}, {WmChorus, 1}}},
      {"Hollow Organ",
       V{{WmOsc1Shape, WsTriangle}, {WmOsc1Oct1, 1}, {WmOsc1Oct2, 1}, {WmOsc2On, 1}, {WmOsc2Shape, WsSine}, {WmOsc2Coarse, 12},
         {WmAmpS, 1}, {WmAmpD, 0.3f}, {WmChorus, 1}, {WmLfoOn, 1}, {WmOsc1Lfo, 0.53f}, {WmLfoRate, 0.75f}}},
      {"Ring Bell",
       V{{WmOsc1Shape, WsSine}, {WmOsc2On, 1}, {WmOsc2Shape, WsSine}, {WmOsc2Coarse, 19}, {WmRing, 1}, {WmAmpD, 0.62f},
         {WmAmpS, 0}, {WmAmpR, 0.4f}}},
      {"Warm Bass",
       V{{WmOsc1Shape, WsPulse}, {WmOsc1Pw, 0.3f}, {WmOsc1Warm, 1}, {WmOsc1Coarse, -12}, {WmOsc2On, 1}, {WmOsc2Shape, WsSaw},
         {WmOsc2Coarse, -12}, {WmOsc2Fine, 0.47f}, {WmSvfOn, 1}, {WmCut, 0.22f}, {WmEmph, 0.4f}, {WmSvfEnv, 0.8f},
         {WmSvfD, 0.3f}, {WmSvfS, 0.1f}, {WmAmpS, 0.9f}}},
  };
  p.editorWidth = 760;
  return p;
}

// ---------------------------------------------------------------- FM Duo
PluginInfo fmDuoInfo() {
  PluginInfo p{"gl.fmduo", "FM Duo", "Synth", PluginKind::Generator, {}, false, {}};
  auto& q = p.params;
  q.resize(FdCount);
  const double u = 1.0 / 65536.0;
  q[FdAttack] = pct("Attack", 0);
  q[FdDecay] = pct("Decay", 42598 * u);
  q[FdRelease] = pct("Release", 28901 * u);
  for (int k = 0; k < 2; ++k) {
    const int b = k == 0 ? FdM1Coarse : FdM2Coarse;
    const std::string m = "Mod " + std::to_string(k + 1) + " ";
    q[static_cast<size_t>(b)] = pct(m + "coarse", 55706 * u);
    q[static_cast<size_t>(b + 1)] = pct(m + "fine", 21561 * u);
    q[static_cast<size_t>(b + 2)] = pct(m + "init", k == 0 ? 15073 * u : 0.0);
    q[static_cast<size_t>(b + 3)] = pct(m + "time", 52429 * u);
    q[static_cast<size_t>(b + 4)] = pct(m + "sustain", k == 0 ? 3277 * u : 0.0);
    q[static_cast<size_t>(b + 5)] = pct(m + "release", 52429 * u);
    q[static_cast<size_t>(b + 6)] = pct(m + "velocity", 58982 * u);
  }
  q[FdVibrato] = pct("Vibrato", 0);
  q[FdWaveform] = pct("Waveform", 29295 * u);
  q[FdModThru] = pct("Mod thru", 0);
  q[FdLfoRate] = pct("LFO rate", 27132 * u);
  q[FdOctave] = lin("Octave", "", -3, 3, 0);
  using V = std::vector<std::pair<int, float>>;
  p.presets = {
      {"Init", {}},
      {"Tine Piano",
       V{{FdDecay, 0.72f}, {FdRelease, 0.45f}, {FdM1Coarse, 0.0f}, {FdM1Fine, 0.0f}, {FdM1Init, 0.45f},
         {FdM1Time, 0.55f}, {FdM1Sustain, 0.1f}, {FdM2Coarse, 0.62f}, {FdM2Init, 0.3f}, {FdM2Time, 0.3f},
         {FdM2Sustain, 0}, {FdWaveform, 0.2f}}},
      {"Wood Bass",
       V{{FdDecay, 0.55f}, {FdRelease, 0.25f}, {FdM1Coarse, 0.16f}, {FdM1Init, 0.55f}, {FdM1Time, 0.45f},
         {FdM1Sustain, 0.12f}, {FdOctave, -1}}},
      {"Steel Bell",
       V{{FdDecay, 0.85f}, {FdRelease, 0.7f}, {FdM1Coarse, 0.56f}, {FdM1Fine, 0.2f}, {FdM1Init, 0.5f},
         {FdM1Time, 0.75f}, {FdM1Sustain, 0.2f}, {FdM2Coarse, 0.8f}, {FdM2Fine, 0.9f}, {FdM2Init, 0.25f},
         {FdM2Time, 0.6f}, {FdModThru, 0.2f}}},
      {"Vibe Organ",
       V{{FdDecay, 1.0f}, {FdRelease, 0.3f}, {FdM1Coarse, 0.23f}, {FdM1Init, 0.2f}, {FdM1Sustain, 0.2f},
         {FdVibrato, 0.35f}, {FdLfoRate, 0.45f}}},
  };
  p.editorWidth = 640;
  return p;
}

void appendDcoInfos(std::vector<PluginInfo>& v) {
  v.push_back(dcoSynthInfo());
  v.push_back(warmSynthInfo());
  v.push_back(fmDuoInfo());
}

}  // namespace gl::packs::synths
