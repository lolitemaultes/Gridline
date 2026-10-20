// Gridline — "models" pack, J-22 group "strings": catalogue (Twin String,
// Plucker, Step Bass). Knob parameters keep the reference's 0..1 positions
// (shown in %); the runtimes apply the value laws (strings_dsp.cpp).
#include <string>
#include "../Packs.h"
#include "strings.h"

namespace gl::packs::models {

using namespace strs;

namespace {

ParamDesc pct(const std::string& n, double d) { return ParamDesc{n, "%", 0, 1, d, ParamKind::Continuous, ParamScale::Linear, {}}; }
ParamDesc lin(const std::string& n, const char* u, double mn, double mx, double d) {
  return ParamDesc{n, u, mn, mx, d, ParamKind::Continuous, ParamScale::Linear, {}};
}
ParamDesc tog(const std::string& n, bool d) { return ParamDesc{n, "", 0, 1, d ? 1.0 : 0.0, ParamKind::Toggle, ParamScale::Linear, {}}; }
ParamDesc ch(const std::string& n, std::vector<std::string> c, int d) {
  double hi = static_cast<double>(c.size() - 1);
  return ParamDesc{n, "", 0, hi, static_cast<double>(d), ParamKind::Choice, ParamScale::Linear, std::move(c)};
}
using Ov = std::vector<std::pair<int, float>>;

// ------------------------------------------------------------ Twin String
PluginInfo twinString() {
  PluginInfo p{"gl.twinstring", "Twin String", "Synth", PluginKind::Generator, {}, false, {}};
  auto& q = p.params;
  q.resize(TsParamCount);
  q[TsLevel] = pct("Level", 0.353);
  q[TsTune] = lin("Fine tune", "", 0, 1, 0.5);
  q[TsAmpA] = pct("Amp attack", 0);
  q[TsAmpD] = pct("Amp decay", 1);
  q[TsAmpS] = pct("Amp sustain", 1);
  q[TsAmpR] = pct("Amp release", 0);
  const char* sn[2] = {"String A ", "String B "};
  for (int s = 0; s < 2; ++s) {
    const int b = s ? TsS2Decay : TsS1Decay;
    std::string n = sn[s];
    q[static_cast<size_t>(b)] = pct(n + "decay", s ? 1 : 1);
    q[static_cast<size_t>(b + 1)] = tog(n + "invert decay", false);
    q[static_cast<size_t>(b + 2)] = pct(n + "damp offset", 0);
    q[static_cast<size_t>(b + 3)] = pct(n + "damping", 0);
    q[static_cast<size_t>(b + 4)] = pct(n + "damp shape", 0);
  }
  q[TsSaturation] = pct("Saturation", 0);
  q[TsMix] = pct("String mix", 0);
  q[TsStereo] = pct("Stereo spread", 0.5);
  q[TsCombRatio] = pct("Comb ratio", 0.5);
  q[TsCombFb] = pct("Comb feedback", 0);
  q[TsCombFbInv] = tog("Comb feedback invert", false);
  q[TsCombLevel] = pct("Comb level", 0);
  q[TsCombLevelInv] = tog("Comb level invert", false);
  q[TsLoCut] = pct("Low cut", 0);
  q[TsLoRes] = pct("Low cut reso", 0);
  q[TsHiAtk] = pct("High cut attack", 0);
  q[TsHiDec] = pct("High cut decay", 0.2);
  q[TsHiSus] = pct("High cut sustain", 0);
  q[TsHiCut] = pct("High cut", 0.5);
  q[TsHiRes] = pct("High cut reso", 0);
  q[TsNoiseLevel] = pct("Noise level", 1);
  q[TsNoiseRate] = pct("Noise rate", 0);
  q[TsDrive] = pct("Exciter drive", 0);
  q[TsEgA] = pct("Env attack", 0);
  q[TsEgD] = pct("Env decay", 1);
  q[TsEgS] = pct("Env sustain", 1);
  q[TsEgR] = pct("Env release", 1);
  q[TsLfoRate] = pct("LFO rate", 0);
  for (int k = 0; k < kTsRes; ++k) {
    std::string n = "Band " + std::to_string(k + 1);
    q[static_cast<size_t>(TsResFreq1 + k)] = pct(n + " freq", 0);
    q[static_cast<size_t>(TsResFb1 + k)] = pct(n + " feedback", 0);
    q[static_cast<size_t>(TsResOn1 + k)] = tog(n + " on", false);
  }
  q[TsDlyOn] = tog("Delay", false);
  q[TsDlyTime] = pct("Delay time", 0.5);
  q[TsDlyFb] = pct("Delay feedback", 0.6);
  q[TsDlySpeed] = pct("Delay mod rate", 0.24);
  q[TsDlyDepth] = pct("Delay mod depth", 0.2);
  q[TsDlyBlur] = pct("Delay blur", 0.5);
  q[TsDlyWet] = pct("Delay wet", 0.35);
  q[TsChoOn] = tog("Chorus", false);
  q[TsChoRate] = pct("Chorus rate", 0.5);
  q[TsChoDepth] = pct("Chorus depth", 0.5);
  q[TsChoWet] = pct("Chorus wet", 0.4);
  q[TsRevOn] = tog("Reverb", false);
  q[TsRevMix] = pct("Reverb wet", 0.5);
  q[TsRevDecay] = pct("Reverb decay", 0.5);
  q[TsRevColor] = pct("Reverb tone", 0.5);
  q[TsSpare] = pct("(unused)", 0);
  std::vector<std::string> srcs = {"None", "Envelope", "LFO", "Mod wheel", "Pitch bend", "Pressure", "Velocity", "Key", "Release vel"};
  std::vector<std::string> dsts = {"LFO depth",     "Level",          "Pan",           "Pitch",      "Low cut",
                                   "High cut",      "HC attack",      "HC decay",      "A decay",    "B decay",
                                   "A + B decay",   "A damping",      "B damping",     "A + B damp", "Noise rate"};
  for (int k = 0; k < kTsMods; ++k) {
    std::string n = "Mod " + std::to_string(k + 1);
    q[static_cast<size_t>(tsMod(k, TmSource))] = ch(n + " source", srcs, k == 0 ? MsVelocity : k == 1 ? MsPitchWheel : MsNone);
    q[static_cast<size_t>(tsMod(k, TmAmount))] = pct(n + " amount", k < 2 ? 1 : 0);
    q[static_cast<size_t>(tsMod(k, TmNegative))] = tog(n + " negative", false);
    q[static_cast<size_t>(tsMod(k, TmDest))] = ch(n + " target", dsts, k == 0 ? MdLevel : k == 1 ? MdPitch : MdLfoDepth);
  }
  q[TsS2Ratio] = ch("String B ratio", {"B /4", "B /3", "B /2", "B = A", "B x2", "B x3", "B x4"}, 3);
  q[TsDlySync] = ch("Delay sync", {"Free", "1/16", "1/8", "3/16", "1/4", "1/2", "3/4", "1 bar", "2 bars", "4 bars"}, 0);
  q[TsTranspose] = ch("Octave", {"-3 oct", "-2 oct", "-1 oct", "0 oct", "+1 oct", "+2 oct", "+3 oct"}, 3);
  auto mod = [](int slot, int src, float amt, int dst) {
    return Ov{{tsMod(slot, TmSource), static_cast<float>(src)}, {tsMod(slot, TmAmount), amt}, {tsMod(slot, TmDest), static_cast<float>(dst)}};
  };
  auto cat = [](Ov a, const Ov& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
  };
  p.presets = {
      {"Init", {}},
      {"Nylon pluck", {{TsLevel, 0.5f}, {TsHiCut, 0.62f}, {TsHiDec, 0.16f}, {TsS1Decay, 0.82f}, {TsS1Damp, 0.45f}, {TsS1DampOfs, 0.3f},
                       {TsMix, 0.3f}, {TsS2Decay, 0.7f}, {TsS2Damp, 0.6f}, {TsS2Ratio, 4.f}, {TsResOn1, 1.f}, {TsResFreq1, 0.3f},
                       {TsResFb1, 0.45f}, {TsResOn1 + 1, 1.f}, {TsResFreq1 + 1, 0.42f}, {TsResFb1 + 1, 0.35f}, {TsRevOn, 1.f},
                       {TsRevMix, 0.25f}}},
      {"Bowed glass", cat(Ov{{TsLevel, 0.45f}, {TsHiAtk, 0.35f}, {TsHiDec, 0.6f}, {TsHiSus, 0.55f}, {TsHiCut, 0.75f}, {TsNoiseLevel, 0.6f},
                            {TsNoiseRate, 0.8f}, {TsS1Decay, 0.97f}, {TsS1Damp, 0.25f}, {TsMix, 0.5f}, {TsS2Ratio, 4.f},
                            {TsS2Decay, 0.95f}, {TsAmpA, 0.3f}, {TsAmpR, 0.45f}, {TsChoOn, 1.f}, {TsRevOn, 1.f}, {TsRevMix, 0.45f},
                            {TsRevDecay, 0.7f}},
                          mod(2, MsLfo, 0.3f, MdHiCut))},
      {"Hammered steel", {{TsLevel, 0.5f}, {TsHiCut, 0.85f}, {TsHiDec, 0.08f}, {TsNoiseLevel, 0.2f}, {TsDrive, 0.3f}, {TsS1Decay, 0.9f},
                          {TsS1Damp, 0.15f}, {TsS1Sharp, 0.5f}, {TsMix, 0.5f}, {TsS2Ratio, 3.f}, {TsS2Decay, 0.88f}, {TsS2DampOfs, 0.1f},
                          {TsStereo, 0.85f}, {TsCombLevel, 0.4f}, {TsCombFb, 0.5f}, {TsCombRatio, 0.3f}, {TsDlyOn, 1.f}, {TsDlyWet, 0.2f}}},
      {"Reed pipe", cat(Ov{{TsLevel, 0.4f}, {TsHiAtk, 0.2f}, {TsHiSus, 0.7f}, {TsHiCut, 0.55f}, {TsHiRes, 0.4f}, {TsNoiseRate, 0.5f},
                          {TsS1Decay, 1.f}, {TsS1Inv, 1.f}, {TsS1Damp, 0.5f}, {TsSaturation, 0.35f}, {TsAmpA, 0.15f}, {TsAmpR, 0.25f},
                          {TsLfoRate, 0.6f}, {TsRevOn, 1.f}, {TsRevMix, 0.3f}},
                        mod(2, MsLfo, 0.08f, MdPitch))},
      {"Muted bass", {{TsLevel, 0.6f}, {TsTranspose, 2.f}, {TsHiCut, 0.4f}, {TsHiDec, 0.1f}, {TsS1Decay, 0.6f}, {TsS1Damp, 0.75f},
                      {TsS1DampOfs, 0.05f}, {TsSaturation, 0.2f}, {TsAmpR, 0.1f}}},
  };
  q[TsEngine] = ch("Engine", {"Waveguide (J-22)", "Reference"}, 1);
  for (auto& pr : p.presets) pr.values.push_back({TsEngine, 0.f});  // voiced for the waveguide engine
  p.legacyModelParam = TsEngine;
  p.editorWidth = 900;
  return p;
}

// --------------------------------------------------------------- Plucker
PluginInfo plucker() {
  PluginInfo p{"gl.plucker", "Plucker", "Synth", PluginKind::Generator, {}, false, {}};
  auto& q = p.params;
  q.resize(PkCount);
  q[PkDecay] = lin("Decay", "", 0, 256, 128);
  q[PkColor] = lin("Colour", "", 0, 128, 64);
  q[PkNormalize] = tog("Even decay", true);
  q[PkGate] = tog("Gate", false);
  q[PkWiden] = tog("Widen", true);
  p.presets = {
      {"Init", {}},
      {"Soft harp", {{PkDecay, 190.f}, {PkColor, 40.f}}},
      {"Bright pick", {{PkDecay, 110.f}, {PkColor, 112.f}, {PkWiden, 0.f}}},
      {"Staccato", {{PkDecay, 150.f}, {PkColor, 80.f}, {PkGate, 1.f}}},
      {"Long bell", {{PkDecay, 250.f}, {PkColor, 96.f}, {PkNormalize, 0.f}}},
  };
  p.editorWidth = 420;
  return p;
}

// ------------------------------------------------------------- Step Bass
PluginInfo stepBass() {
  PluginInfo p{"gl.stepbass", "Step Bass", "Synth", PluginKind::Generator, {}, false, {}};
  auto& q = p.params;
  q.resize(SbCount);
  // Defaults: the reference's empty-program patch.
  static const float d[SbPatchCount] = {0.5f, 0.0f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.0f, 0.1f, 0.5f, 0.0f,
                                        0.0f, 0.2f, 1.0f, 0.25f, 0.0f, 0.497f, 1.0f, 0.303f, 0.01f, 0.496f, 0.834f, 0.496f, 0.695f,
                                        0.0f, 0.5f, 0.5f, 1.0f, 0.5f, 0.0f, 1.0f, 0.444f, 0.605f, 0.045f, 0.253f, 0.762f, 0.5f, 0.0f,
                                        0.0f};
  static const char* n[SbPatchCount] = {
      "Tuning",         "Saw / pulse",     "Cutoff",          "Key follow",      "Resonance",       "Env mod",     "Decay",
      "Accent",         "Volume",          "High pass",       "Min decay",       "Pulse width",     "PWM rate",    "PWM depth",
      "Reverb",         "Reverb low cut",  "Reverb high cut", "Reverb predelay", "Reverb pre fdbk", "Reverb size", "Reverb diffusion",
      "Reverb decay",   "Reverb damping",  "Reverb width",    "Reverb dry",      "Reverb early",    "Reverb wet",  "Distortion",
      "Dist high pass", "Dist drive",      "Dist tone",       "Dist volume",     "Delay",           "Delay sync",  "Delay amount",
      "Delay time",     "Delay right tap", "Delay feedback",  "Delay tone",      "Delay stereo",    "Clicks",
      "Classic pulse"};
  for (int k = 0; k < SbPatchCount; ++k) {
    const bool sw = k == SbRevOn || k == SbDistOn || k == SbDlyOn || k == SbDlySync || k == SbClassicPulse;
    q[static_cast<size_t>(k)] = sw ? tog(n[k], d[k] >= 0.5f) : pct(n[k], d[k]);
  }
  q[SbProgram] = lin("Program", "", 0, kSbPrograms - 1, 0);
  q[SbSeqOn] = tog("Sequencer", false);
  q[SbRoot] = lin("Sequence key", "", 0, 127, 60);
  q[SbTempoMode] = ch("Tempo", {"Song", "Own"}, 0);
  q[SbBpm] = lin("Own tempo", "", 10, 522, 125);
  p.presets = {
      {"Init", {}},
      {"Squelch", {{SbCutoff, 0.25f}, {SbReso, 0.9f}, {SbEnvMod, 0.85f}, {SbDecay, 0.35f}, {SbAccent, 0.8f}}},
      {"Round square", {{SbWave, 1.f}, {SbCutoff, 0.45f}, {SbReso, 0.3f}, {SbEnvMod, 0.35f}, {SbDecay, 0.6f}}},
      {"Driven line", {{SbCutoff, 0.35f}, {SbReso, 0.7f}, {SbEnvMod, 0.65f}, {SbDistOn, 1.f}, {SbDistDrive, 0.7f}, {SbDistVolume, 0.4f}}},
      {"Dub echo", {{SbCutoff, 0.3f}, {SbReso, 0.6f}, {SbDlyOn, 1.f}, {SbDlyAmount, 0.45f}, {SbDlyFeedback, 0.55f}, {SbRevOn, 1.f}}},
  };
  p.editorWidth = 760;
  return p;
}

}  // namespace

void appendStringsInfos(std::vector<PluginInfo>& v) {
  v.push_back(twinString());
  v.push_back(plucker());
  v.push_back(stepBass());
}

}  // namespace gl::packs::models
