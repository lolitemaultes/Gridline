// Gridline — "dynamics" pack: catalogue entries.
#include "../Packs.h"
#include "dynamics.h"
#include "modrack_ref.h"

namespace gl::packs::dynamics {

namespace {

ParamDesc lin(std::string n, const char* u, double mn, double mx, double d) {
  return ParamDesc{std::move(n), u, mn, mx, d, ParamKind::Continuous, ParamScale::Linear, {}};
}
ParamDesc lg(std::string n, const char* u, double mn, double mx, double d) {
  return ParamDesc{std::move(n), u, mn, mx, d, ParamKind::Continuous, ParamScale::Log, {}};
}
ParamDesc tog(std::string n, double d) { return ParamDesc{std::move(n), "", 0, 1, d, ParamKind::Toggle, ParamScale::Linear, {}}; }
ParamDesc ch(std::string n, std::vector<std::string> c, double d) {
  double mx = static_cast<double>(c.size() - 1);
  return ParamDesc{std::move(n), "", 0, mx, d, ParamKind::Choice, ParamScale::Linear, std::move(c)};
}

using Vals = std::vector<std::pair<int, float>>;

const std::vector<std::string> kOs = {"Off", "2x", "4x"};

// ------------------------------------------------------------------ master
// J-35: the reference block (raw integers in the reference's own ranges).
void addMaxRef(std::vector<ParamDesc>& ps, int base) {
  ps[static_cast<size_t>(base + MrModel)] = ch("Engine", {"Legacy", "Reference"}, 0);
  static const char* gn[7] = {"Low / mid split", "Low slope", "Mid / high split", "High slope", "Band look-ahead", "Band mix", "Low cut"};
  static const double glo[7] = {0, 0, 0, 0, 0, 0, -1}, ghi[7] = {65536, 1, 65536, 1, 10000, 1000, 65536},
                      gdef[7] = {19800, 1, 48065, 1, 1115, 1000, 32768};
  for (int k = 0; k < 7; ++k) ps[static_cast<size_t>(base + MrRaw0 + k)] = lin(std::string(gn[k]) + " (ref.)", "", glo[k], ghi[k], gdef[k]);
  ps[static_cast<size_t>(base + MrRaw0 + 1)] = ch("Low slope (ref.)", {"12 dB", "24 dB"}, 1);
  ps[static_cast<size_t>(base + MrRaw0 + 3)] = ch("High slope (ref.)", {"12 dB", "24 dB"}, 1);
  static const char* bn[13] = {"mode", "pre gain", "post gain", "separation", "saturation", "ceiling", "attack", "release 2",
                               "attack curve", "release", "release curve", "window", "detector"};
  static const double blo[13] = {0, 0, 0, -1000, -1000, 0, 0, 0, 1, 0, 1, 0, 0}, bhi[13] = {3, 2000, 2000, 1000, 1000, 2000, 10000, 10000, 8, 10000, 8, 10000, 1},
                      bdef[13] = {0, 1000, 1000, 0, 0, 1000, 1115, 0, 2, 5000, 3, 2882, 0};
  static const char* names[kMsBands] = {"Low", "Mid", "High", "Master"};
  for (int b = 0; b < kMsBands; ++b)
    for (int k = 0; k < 13; ++k)
    {
      ps[static_cast<size_t>(base + MrRaw0 + 7 + b * 13 + k)] = lin(std::string(names[b]) + " " + bn[k] + " (ref.)", "", blo[k], bhi[k], bdef[k]);
      if (k == 0) ps[static_cast<size_t>(base + MrRaw0 + 7 + b * 13)] = ch(std::string(names[b]) + " mode (ref.)", {"Compress", "No compression", "Mute", "Off"}, 0);
      if (k == 12) ps[static_cast<size_t>(base + MrRaw0 + 7 + b * 13 + 12)] = ch(std::string(names[b]) + " detector (ref.)", {"Peak", "RMS"}, 0);
    }
  ps[static_cast<size_t>(base + MrSolo)] = tog("Solo (ref.)", 0);
  ps[static_cast<size_t>(base + MrTarget)] = ch("Selected band (ref.)", {"Low", "Mid", "High", "Master"}, 0);
  ps[static_cast<size_t>(base + MrMasterMid)] = tog("Mid on whole input (ref.)", 0);
  ps[static_cast<size_t>(base + MrSafetyOff)] = tog("No ceiling safety (ref.)", 0);
  ps[static_cast<size_t>(base + MrFir)] = tog("Linear-phase split, legacy (ref.)", 0);
  ps[static_cast<size_t>(base + MrLin)] = tog("Linear-phase split (ref.)", 0);
  ps[static_cast<size_t>(base + MrLinQuality)] = ch("Linear-phase quality (ref.)", {"Lowest", "Low", "Normal", "High", "Higher", "Highest"}, 2);
  ps[static_cast<size_t>(base + MrLinSpeed)] = ch("Linear-phase change speed (ref.)", {"Normal", "Fast", "Faster", "Ultra"}, 0);
  for (int b = 0; b < kMsBands; ++b)
    for (int i = 0; i < kMsCurvePts; ++i)
      ps[static_cast<size_t>(base + MrPtType0 + b * kMsCurvePts + i)] = ch(std::string(names[b]) + " point type (ref.)",
                                                                       {"Single", "Double", "Hold", "Stairs", "Smooth stairs", "Pulse", "Wave", "Single 2", "Double 2",
                                                                        "Half sine", "Smooth", "Single 3", "Double 3"},
                                                                       0);
}

void addMaster(std::vector<PluginInfo>& v) {
  PluginInfo p{"gl.master", "Crest", "Dynamics", PluginKind::Effect, {}, false, {}};
  p.params.resize(kMsTotal);
  addMaxRef(p.params, MsRef0);
  p.params[MsLowFreq] = lg("Low / mid split", "Hz", 40, 1000, 160);
  p.params[MsHighFreq] = lg("Mid / high split", "Hz", 800, 16000, 3500);
  p.params[MsLowSlope] = ch("Low split slope", {"6 dB", "12 dB", "24 dB"}, 2);
  p.params[MsHighSlope] = ch("High split slope", {"6 dB", "12 dB", "24 dB"}, 2);
  p.params[MsLookahead] = lin("Look-ahead", "ms", 0, 10, 2);
  p.params[MsBandMix] = lin("Band mix", "%", 0, 1, 1);
  p.params[MsLowCut] = lg("Low cut", "Hz", 10, 300, 10);
  p.params[MsSolo] = ch("Solo", {"Off", "Low", "Mid", "High"}, 0);
  p.params[MsOversample] = ch("Oversampling", kOs, 1);
  static const char* names[kMsBands] = {"Low", "Mid", "High", "Master"};
  for (int b = 0; b < kMsBands; ++b) {
    std::string n = names[b];
    bool m = b == 3;
    p.params[msParam(b, MbMode)] = ch(n + " mode", {"Bypass", "Active", "Mute", "No compression", "Merged"}, 1);
    p.params[msParam(b, MbPre)] = lin(n + " pre gain", "dB", -24, 24, 0);
    p.params[msParam(b, MbPost)] = lin(n + " post gain", "dB", -24, 24, 0);
    p.params[msParam(b, MbAttack)] = lg(n + " attack", "ms", 0.1, 300, m ? 2 : 10);
    p.params[msParam(b, MbRelease)] = lg(n + " release", "ms", 5, 3000, m ? 120 : 180);
    p.params[msParam(b, MbThreshHi)] = lin(n + " threshold", "dB", -60, 0, m ? -3 : -14);
    p.params[msParam(b, MbRatioHi)] = lg(n + " ratio", "x", 1, 30, m ? 10 : 2);
    p.params[msParam(b, MbKnee)] = lin(n + " knee", "dB", 0, 24, 6);
    p.params[msParam(b, MbThreshLo)] = lin(n + " low threshold", "dB", -80, 0, -60);
    p.params[msParam(b, MbRatioLo)] = lg(n + " below ratio", "x", 0.25, 8, 1);
    p.params[msParam(b, MbGate)] = lin(n + " gate", "dB", -90, -20, -90);
    p.params[msParam(b, MbSatThresh)] = lin(n + " saturation", "dB", -24, 0, m ? -1.5 : 0);
    p.params[msParam(b, MbSatCeil)] = lin(n + " ceiling", "dB", -24, 12, m ? -0.1 : 6);
    p.params[msParam(b, MbStereo)] = lin(n + " stereo", "%", -1, 1, 0);
    p.params[msParam(b, MbWindow)] = lin(n + " RMS window", "ms", 0, 100, 0);
    // κ2 extension block (reference maximizer features).
    p.params[msExt(b, MxRelease2)] = lin(n + " release 2", "ms", 0, 2000, 0);
    p.params[msExt(b, MxAtkCurve)] = ch(n + " attack curve", {"1", "2", "3", "4", "5", "6", "7", "8"}, 1);
    p.params[msExt(b, MxRelCurve)] = ch(n + " release curve", {"1", "2", "3", "4", "5", "6", "7", "8"}, 2);
    p.params[msExt(b, MxEnvMode)] = ch(n + " detector", {"Peak window", "RMS window"}, 1);
    p.params[msExt(b, MxSatMode)] = ch(n + " saturation mode", {"Off", "A", "B"}, 1);
    p.params[msExt(b, MxCurveMode)] = ch(n + " engine", {"Parametric", "Drawn curve"}, 0);
    p.params[msExt(b, MxPoints)] = lin(n + " curve points", "", 2, kMsCurvePts, 3);
    const double defX[kMsCurvePts] = {0, 0.5, 1, 1, 1, 1, 1, 1}, defY[kMsCurvePts] = {0, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5};
    for (int k = 0; k < kMsCurvePts; ++k) {
      std::string pn = n + " point " + std::to_string(k + 1);
      p.params[msExt(b, MxPt0 + k * 3)] = lin(pn + " x", "", 0, 1, defX[k]);
      p.params[msExt(b, MxPt0 + k * 3 + 1)] = lin(pn + " y", "", 0, 1, defY[k]);
      p.params[msExt(b, MxPt0 + k * 3 + 2)] = lin(pn + " tension", "", -1, 1, 0);
    }
  }
  auto B = [](int b, int k) { return msParam(b, k); };
  p.presets = {
      {"Gentle glue", {}},
      {"Loud master",
       {{B(0, MbPre), 3.f}, {B(1, MbPre), 3.f}, {B(2, MbPre), 3.f}, {B(0, MbRatioHi), 3.f}, {B(1, MbRatioHi), 3.f},
        {B(2, MbRatioHi), 3.f}, {B(3, MbPre), 4.f}, {B(3, MbRatioHi), 30.f}, {B(3, MbThreshHi), -1.f}}},
      {"De-esser",
       {{MsHighFreq, 5500.f}, {B(0, MbMode), 0.f}, {B(1, MbMode), 0.f}, {B(2, MbThreshHi), -32.f}, {B(2, MbRatioHi), 8.f},
        {B(2, MbAttack), 0.5f}, {B(2, MbRelease), 60.f}, {B(3, MbMode), 0.f}}},
      {"Upward lift",
       {{B(0, MbThreshLo), -40.f}, {B(1, MbThreshLo), -40.f}, {B(2, MbThreshLo), -40.f}, {B(0, MbRatioLo), 0.6f},
        {B(1, MbRatioLo), 0.6f}, {B(2, MbRatioLo), 0.6f}}},
      {"Tight low end",
       {{MsLowCut, 30.f}, {B(0, MbThreshHi), -24.f}, {B(0, MbRatioHi), 5.f}, {B(0, MbAttack), 25.f}, {B(0, MbStereo), -1.f},
        {B(0, MbGate), -55.f}}},
  };
  // κ2: presets for the drawn-curve engine (the curves are Gridline's own).
  auto X = [](int b, int k) { return msExt(b, k); };
  Vals soft;
  for (int b = 0; b < kMsBands; ++b) soft.push_back({X(b, MxCurveMode), 1.f});
  for (int b = 0; b < 3; ++b) {
    soft.push_back({X(b, MxPoints), 4.f});
    soft.push_back({X(b, MxPt0 + 3), 0.38f}), soft.push_back({X(b, MxPt0 + 4), 0.38f});
    soft.push_back({X(b, MxPt0 + 6), 0.62f}), soft.push_back({X(b, MxPt0 + 7), 0.47f}), soft.push_back({X(b, MxPt0 + 8), 0.3f});
    soft.push_back({X(b, MxPt0 + 9), 1.f}), soft.push_back({X(b, MxPt0 + 10), 0.5f});
    soft.push_back({B(b, MbPre), 3.f});
  }
  p.presets.push_back({"Drawn: soft multiband glue", soft});
  p.editorWidth = 860;
  v.push_back(p);
}

// ------------------------------------------------------------------ loudness
void addLoudness(std::vector<PluginInfo>& v) {
  PluginInfo p{"gl.emphasis", "Loudness", "Dynamics", PluginKind::Effect, {}, false, {}};
  p.params.resize(EmCount);
  p.params[EmInput] = lin("Input gain", "dB", 0, 24, 0);
  p.params[EmCeiling] = lin("Ceiling", "dB", -12, 0, -0.3);
  p.params[EmRelease] = lg("Release", "ms", 10, 1000, 120);
  p.params[EmCharacter] = ch("Character", {"Balanced", "Loud", "Punchy", "Smooth"}, 0);
  p.params[EmLowEmph] = lin("Low emphasis", "dB", -6, 6, 0);
  p.params[EmHighEmph] = lin("High emphasis", "dB", -6, 6, 0);
  p.params[EmClip] = lin("Clipper share", "%", 0, 1, 0.3);
  p.params[EmClipHard] = tog("Hard clip", 0);
  p.params[EmLink] = lin("Stereo link", "%", 0, 1, 1);
  p.params[EmStereoMode] = ch("Channels", {"L / R", "M / S"}, 0);
  p.params[EmDcBlock] = tog("DC block", 1);
  p.params[EmOversample] = ch("Oversampling", kOs, 1);
  p.params[EmLevelMatch] = tog("Level match", 0);
  p.params[EmDelta] = tog("Delta", 0);
  p.params[EmModel] = ch("Engine", {"Legacy", "Reference"}, 1);
  p.params[EmBypass] = tog("Bypass", 0);
  p.params[EmOrder] = ch("Envelope order", {"1", "2", "3", "4"}, 0);
  p.params[EmTruePeak] = tog("True peak", 0);
  p.params[EmMode] = ch("Mode", {"Versatile", "Loud", "Transient", "Steady"}, 0);
  p.params[EmAmount] = lin("Amount", "%", 0, 1, 1);
  p.params[EmCeilKnob] = lin("Ceiling", "", 0, 1, 0.9912f);  // gain (11^v - 1) / 10
  p.params[EmHardness] = ch("Hardness", {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9"}, 7);
  p.params[EmOs5] = ch("Oversampling", {"Off", "2x", "4x", "8x", "16x"}, 2);
  p.params[EmLowLatency] = tog("Low latency", 0);
  p.params[EmInstant] = tog("Instant recovery", 1);
  p.legacyModelParam = EmModel;
  p.presets = {
      {"Transparent", {}},
      {"Push it", {{EmInput, 6.f}, {EmMode, 1.f}, {EmHardness, 8.f}}},
      {"Snappy drums", {{EmMode, 2.f}, {EmAmount, 0.6f}, {EmHardness, 6.f}, {EmOs5, 0.f}}},
      {"Round clip", {{EmMode, 3.f}, {EmHardness, 4.f}, {EmInput, 3.f}, {EmOs5, 1.f}}},
      {"Wide bus", {{EmStereoMode, 1.f}, {EmLink, 0.7f}, {EmInput, 2.f}, {EmTruePeak, 1.f}}},
  };
  p.editorWidth = 760;
  v.push_back(p);
}

// ------------------------------------------------------------------ enhance
void addEnhance(std::vector<PluginInfo>& v) {
  PluginInfo p{"gl.enhance", "Enhance", "Dynamics", PluginKind::Effect, {}, false, {}};
  p.params.resize(EnCount);
  p.params[EnAmount] = lin("Amount", "%", 0, 1, 0.6);
  p.params[EnMode] = ch("Voicing", {"A", "B", "C", "D"}, 0);
  p.params[EnCustom] = ch("Program", {"Built-in voicing", "Imported program", "Peak emphasis", "Reference program"}, 0);
  addMaxRef(p.params, EnRef0);
  // Embedded program: the Mastering Dynamics parameter block (same names, prefixed).
  for (const PluginInfo& m : v)
    if (m.type == "gl.master")
      for (size_t k = 0; k < m.params.size() && static_cast<int>(k) < kMsCount; ++k) {
        ParamDesc d = m.params[k];
        d.name = "Program: " + d.name;
        p.params[static_cast<size_t>(EnProg0) + k] = d;
      }
  p.params[EnPeakIn] = lin("Input gain", "dB", 0, 24, 0);
  p.params[EnPeakEmph] = lin("Emphasis", "%", 0, 1, 0.5);
  p.params[EnPeakSoft] = tog("Soft clipping", 0);
  p.params[EnPeakMatch] = tog("Level matching", 0);
  p.params[EnPeakOut] = lin("Output gain", "x", 0, 1, 1);
  p.params[EnPeakBypass] = tog("Bypass", 0);
  p.presets = {{"Default", {}},
               {"Subtle", {{EnAmount, 0.3f}}},
               {"Punch", {{EnMode, 1.f}, {EnAmount, 0.7f}}},
               {"Air", {{EnMode, 2.f}, {EnAmount, 0.6f}}},
               {"Dense", {{EnMode, 3.f}, {EnAmount, 0.8f}}}};
  p.editorWidth = 560;
  v.push_back(p);
}

// ------------------------------------------------------------------ amp rack
void slotVals(Vals& out, int slot, int type, bool on, float mix = 1.f, std::vector<std::pair<int, float>> knobs = {}) {
  out.push_back({rkParam(slot, RsType), static_cast<float>(type)});
  out.push_back({rkParam(slot, RsOn), on ? 1.f : 0.f});
  out.push_back({rkParam(slot, RsMix), mix});
  for (int k = 0; k < kRackKnobs; ++k) out.push_back({rkParam(slot, RsK0 + k), static_cast<float>(rackKnobDefault(type, k))});
  for (auto& kv : knobs) out.push_back({rkParam(slot, RsK0 + kv.first), kv.second});
}

void addRack(std::vector<PluginInfo>& v) {
  PluginInfo p{"gl.amprack", "Pedal Rack", "Distortion", PluginKind::Effect, {}, false, {}};
  p.params.resize(kRkCount);
  p.params[RkInput] = lin("Input", "dB", -24, 24, 0);
  p.params[RkOutput] = lin("Output", "dB", -24, 24, 0);
  p.params[RkSoftClip] = tog("Soft clip output", 1);
  p.params[RkOversample] = ch("Oversampling", kOs, 1);
  std::vector<std::string> types;
  for (int t = 0; t < RmCount; ++t) types.push_back(rackModuleName(t));
  static const int defType[kRackSlots] = {RmOverdrive, RmAmp, RmEq, RmChorus, RmDelay, RmReverb};
  static const bool defOn[kRackSlots] = {true, true, true, false, false, true};
  for (int s = 0; s < kRackSlots; ++s) {
    std::string n = "Slot " + std::to_string(s + 1);
    p.params[rkParam(s, RsType)] = ch(n + " module", types, defType[s]);
    p.params[rkParam(s, RsOn)] = tog(n + " on", defOn[s] ? 1 : 0);
    for (int k = 0; k < kRackKnobs; ++k)
      p.params[rkParam(s, RsK0 + k)] = lin(n + " knob " + std::to_string(k + 1), "%", 0, 1, rackKnobDefault(defType[s], k));
    p.params[rkParam(s, RsMix)] = lin(n + " mix", "%", 0, 1, defType[s] == RmReverb ? 0.35 : 1);
  }
  Vals crunch, clean, fuzz, ambient, funk, surf;
  slotVals(crunch, 0, RmGate, true);
  slotVals(crunch, 1, RmOverdrive, true, 1, {{0, 0.65f}});
  slotVals(crunch, 2, RmAmp, true, 1, {{0, 0.6f}, {2, 0.6f}, {4, 0.6f}});
  slotVals(crunch, 3, RmEq, true);
  slotVals(crunch, 4, RmDelay, false);
  slotVals(crunch, 5, RmReverb, true, 0.25f);
  slotVals(clean, 0, RmCompressor, true);
  slotVals(clean, 1, RmAmp, true, 1, {{0, 0.15f}, {3, 0.6f}, {4, 0.3f}});
  slotVals(clean, 2, RmChorus, true, 0.6f);
  slotVals(clean, 3, RmEmpty, false);
  slotVals(clean, 4, RmDelay, true, 0.3f);
  slotVals(clean, 5, RmReverb, true, 0.3f);
  slotVals(fuzz, 0, RmGate, true, 1, {{0, 0.45f}});
  slotVals(fuzz, 1, RmFuzz, true, 1, {{0, 0.85f}});
  slotVals(fuzz, 2, RmAmp, true, 1, {{0, 0.7f}, {4, 0.7f}});
  slotVals(fuzz, 3, RmEq, true, 1, {{1, 0.65f}, {2, 0.35f}});
  slotVals(fuzz, 4, RmEmpty, false);
  slotVals(fuzz, 5, RmReverb, true, 0.2f);
  slotVals(ambient, 0, RmCompressor, true);
  slotVals(ambient, 1, RmOverdrive, true, 1, {{0, 0.3f}});
  slotVals(ambient, 2, RmAmp, true, 1, {{0, 0.35f}});
  slotVals(ambient, 3, RmPhaser, true, 0.5f);
  slotVals(ambient, 4, RmDelay, true, 0.45f, {{1, 0.55f}, {4, 1.f}});
  slotVals(ambient, 5, RmReverb, true, 0.5f, {{0, 0.85f}, {1, 0.7f}});
  slotVals(funk, 0, RmCompressor, true, 1, {{0, 0.5f}, {1, 0.5f}});
  slotVals(funk, 1, RmWah, true, 1, {{1, 0.7f}});
  slotVals(funk, 2, RmAmp, true, 1, {{0, 0.25f}, {3, 0.65f}});
  slotVals(funk, 3, RmEmpty, false);
  slotVals(funk, 4, RmEmpty, false);
  slotVals(funk, 5, RmReverb, true, 0.2f);
  slotVals(surf, 0, RmAmp, true, 1, {{0, 0.3f}, {3, 0.7f}, {4, 0.1f}});
  slotVals(surf, 1, RmTremolo, true, 1, {{0, 0.55f}, {1, 0.7f}});
  slotVals(surf, 2, RmEmpty, false);
  slotVals(surf, 3, RmEmpty, false);
  slotVals(surf, 4, RmDelay, true, 0.25f, {{0, 0.3f}});
  slotVals(surf, 5, RmReverb, true, 0.45f, {{0, 0.7f}, {2, 0.2f}});
  // J-35: module rack and pedal board reference blocks.
  p.params[RkModel] = ch("Rack model", {"Guitar rack", "Module rack", "Pedal board"}, 0);
  p.params[DrSoftClip] = tog("Soft clip output (modules)", 1);
  p.params[DrCount] = lin("Modules", "", 0, kDrModules, 0);
  for (int k = 0; k < kDrModules; ++k) p.params[DrSlot0 + k] = lin("Module " + std::to_string(k + 1) + " automation block", "", 0, 3, k);
  {
    static const char* mn[DmP0] = {"kind", "algorithm", "oversampling", "remove DC", "disabled", "filter type", "smoothing", "impulse", "chorus mode"};
    static const double mlo[DmP0] = {0, 0, 0, 0, 0, 0, 0, 0, 0}, mhi[DmP0] = {3, 17, 4, 1, 1, 22, 1, 58, 2}, mdef[DmP0] = {0, 0, 0, 0, 0, 0, 1, 0, 0};
    static const double plo[kDrParams] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 64, 1, 0, 0, 0, 1, 0, 0, -500, 0, -1000, 500, 0,
                                          0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -1000, -1000, 0, 0, 0, 0, 0, -3000};
    static const double phi[kDrParams] = {10000, 10000, 10000, 10000, 10000, 1, 10000, 10000, 10000, 10000, 10000, 10000, 10000, 10000, 10000, 1,
                                          192, 10, 128, 128, 1, 127, 160, 1000, 500, 2000, 1000, 1100, 10000, 10000, 10000, 10000, 10000, 1,
                                          10000, 10000, 10000, 10000, 10000, 10000, 10000, 1000, 1000, 768, 6000, 6000, 6000, 6000, 3000};
    drack::ModuleState d;
    drack::moduleDefaults(d);
    for (int m = 0; m < kDrModules; ++m) {
      const std::string pre = "Module " + std::to_string(m + 1) + " ";
      for (int k = 0; k < DmP0; ++k) p.params[drParam(m, k)] = lin(pre + mn[k], "", mlo[k], mhi[k], mdef[k]);
      for (int k = 0; k < kDrParams; ++k) p.params[drParam(m, DmP0 + k)] = lin(pre + "p" + std::to_string(k), "", plo[k], phi[k], d.p[k]);
    }
  }
  {
    static const float pdef[50] = {0.5f, 0.5f, 0, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f,
                                   0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f,
                                   0.5f, 0.5f, 0.5f, 1, 1, 1, 1, 1, 0.5f, 1, 0, 0, 0, 0};
    for (int k = 0; k < 50; ++k) {
      double hi = k == 11 ? 20 : (k >= 45 ? 11 : 1);
      p.params[PbValue0 + k] = lin("Board value " + std::to_string(k), "", k >= 45 ? -1 : 0, hi, pdef[k]);
    }
  }
  p.presets = {{"Crunch stack", crunch}, {"Clean shimmer", clean}, {"Fuzz wall", fuzz},
               {"Ambient lead", ambient}, {"Funk wah", funk},     {"Surf tremolo", surf}};
  p.editorWidth = 1000;
  v.push_back(p);
}

// ------------------------------------------------------------------ distortions
void addDistortions(std::vector<PluginInfo>& v) {
  {
    PluginInfo p{"gl.overdrive", "Overdrive", "Distortion", PluginKind::Effect, {}, false, {}};
    p.params.resize(OdCount);
    p.params[OdPreBand] = lin("Pre band", "%", 0, 1, 0);
    p.params[OdColor] = lin("Colour", "%", 0, 1, 0.5);
    p.params[OdPreAmp] = lin("Pre amp", "%", 0, 1, 0.3);
    p.params[OdBoost] = tog("x100 boost", 0);
    p.params[OdPostFilter] = lin("Post filter", "%", 0, 1, 0);
    p.params[OdPostGain] = lin("Post gain", "dB", -36, 6, -6);
    p.params[OdOversample] = ch("Oversampling", kOs, 1);
    p.params[OdModel] = ch("Engine", {"Legacy", "Reference"}, 1);
    p.params[OdPostGainK] = lin("Post gain", "%", 0, 1, 0.7);
    p.legacyModelParam = OdModel;
    p.presets = {{"Warm push", {}},
                 {"Bite", {{OdPreBand, 0.5f}, {OdPreAmp, 0.6f}, {OdPostGainK, 0.55f}}},
                 {"Fat and dark", {{OdPostFilter, 0.6f}, {OdColor, 0.75f}, {OdPreAmp, 0.5f}, {OdPostGainK, 0.6f}}},
                 {"Screamer", {{OdBoost, 1.f}, {OdPreAmp, 0.4f}, {OdPostFilter, 0.4f}, {OdPreBand, 0.35f}, {OdPostGainK, 0.5f}}},
                 {"Legacy tube", {{OdModel, 0.f}, {OdPreAmp, 0.45f}, {OdPostGain, -9.f}}}};
    p.editorWidth = 560;
    v.push_back(p);
  }
  {
    PluginInfo p{"gl.fastdist", "Clip Dist", "Distortion", PluginKind::Effect, {}, false, {}};
    p.params.resize(FdCount);
    p.params[FdPreAmp] = lin("Pre amp", "dB", -12, 24, 0);
    p.params[FdThreshold] = lin("Threshold", "%", 0.05, 1, 1);
    p.params[FdType] = ch("Type", {"A", "B"}, 0);  // reference A / B (legacy engine: clip / fold)
    p.params[FdMix] = lin("Mix", "%", 0, 1, 1);
    p.params[FdPost] = lin("Post gain", "dB", -24, 12, 0);
    p.params[FdPreK] = lin("Pre amp", "%", 0.5, 1.5, 1);
    p.params[FdDrive] = lin("Threshold", "", 1, 10, 10);
    p.params[FdMixK] = lin("Mix", "%", 0, 1, 1);
    p.params[FdPostK] = lin("Post gain", "%", 0, 1, 1);
    p.params[FdClassic] = tog("Reference engine", 1);
    p.presets = {{"Neutral", {}},
                 {"Warm drive", {{FdPreK, 0.64f}, {FdDrive, 7.f}}},
                 {"Fuzz", {{FdPreK, 1.5f}, {FdType, 1.f}, {FdPostK, 0.62f}}},
                 {"Gentle squash", {{FdPreK, 1.4f}, {FdDrive, 3.f}, {FdMixK, 0.85f}, {FdPostK, 0.8f}}},
                 {"Light clip", {{FdClassic, 0.f}, {FdThreshold, 0.6f}, {FdPreAmp, 4.f}}},
                 {"Parallel grit", {{FdClassic, 0.f}, {FdMix, 0.4f}, {FdPreAmp, 15.f}, {FdThreshold, 0.2f}, {FdPost, 4.f}}}};
    p.editorWidth = 520;
    v.push_back(p);
  }
  {
    PluginInfo p{"gl.squeeze", "Pinch", "Distortion", PluginKind::Effect, {}, false, {}};
    p.params.resize(SqCount);
    p.params[SqGain] = lin("Gain", "dB", -24, 12, 0);
    p.params[SqMix] = lin("Mix", "%", 0, 1, 1);
    p.params[SqSquare] = lin("Squarize", "%", 0, 1, 0);
    p.params[SqSteps] = lg("Steps", "", 2, 4096, 4096);
    p.params[SqPunchAmount] = lin("Punch", "%", 0, 1, 0);
    p.params[SqPreserve] = lg("Preserve", "ms", 0.1, 40, 2);
    p.params[SqImpact] = lin("Impact", "ms", 0, 500, 0);
    p.params[SqRelation] = ch("Relation", {"Flip", "Skip", "Half", "Root"}, 0);
    p.params[SqRingFreq] = lg("Ring frequency", "Hz", 20, 5000, 440);
    p.params[SqRingAmount] = lin("Ring amount", "%", 0, 1, 0);
    p.params[SqRateDiv] = lin("Rate divide", "", 1, 32, 1);
    p.params[SqFilterOn] = tog("Filter", 0);
    p.params[SqFilterType] = ch("Filter type", {"Low pass", "High pass", "Band pass", "Notch", "Peak / LP-HP"}, 0);
    p.params[SqCutoff] = lg("Cutoff", "Hz", 40, 18000, 2000);
    p.params[SqResonance] = lin("Resonance", "%", 0, 1, 0.2);
    p.params[SqFilterPos] = ch("Filter position", {"Pre", "Post"}, 1);
    p.params[SqModel] = ch("Engine", {"Legacy", "Reference"}, 1);
    p.params[SqGainRef] = lin("Gain", "%", 0, 160.0 / 127.0, 1);
    p.params[SqStepsRef] = lg("Steps", "", 1, 32768, 32768);
    p.params[SqCutoffPos] = lin("Cutoff", "%", 0, 1, 0.5);
    p.params[SqImpactMs] = lin("Impact", "ms", 0, 1000, 0);
    p.params[SqPreserveMs] = lin("Preserve", "ms", 0, 1000, 0.117);
    p.params[SqAmountRef] = lin("Amount", "%", 0, 1, 0);
    p.legacyModelParam = SqModel;
    p.presets = {{"Clean", {}},
                 {"Step crunch", {{SqStepsRef, 24.f}, {SqMix, 0.85f}}},
                 {"Gate punch", {{SqImpactMs, 4.f}, {SqPreserveMs, 9.f}, {SqAmountRef, 0.1f}}},
                 {"Squeak", {{SqStepsRef, 6.f}, {SqFilterOn, 1.f}, {SqResonance, 0.7f}, {SqCutoffPos, 0.36f}, {SqMix, 0.5f}}},
                 {"Ring radio", {{SqModel, 0.f}, {SqRingAmount, 0.7f}, {SqRingFreq, 900.f}, {SqFilterOn, 1.f}, {SqFilterType, 2.f}, {SqCutoff, 1500.f}}},
                 {"Low rate", {{SqModel, 0.f}, {SqRateDiv, 8.f}, {SqFilterOn, 1.f}, {SqCutoff, 3000.f}}}};
    p.editorWidth = 560;
    v.push_back(p);
  }
}

// ------------------------------------------------------------------ effector
void addEffector(std::vector<PluginInfo>& v) {
  PluginInfo p{"gl.effector", "Multi FX", "Misc", PluginKind::Effect, {}, false, {}};
  p.params.resize(EfCount);
  std::vector<std::string> kinds;
  for (int k = 0; k < kEfKinds; ++k) kinds.push_back(effectorKindName(k));
  p.params[EfKind] = ch("Effect", kinds, 0);
  p.params[EfX] = lin("X", "%", 0, 1, 0.5);
  p.params[EfY] = lin("Y", "%", 0, 1, 0.5);
  p.params[EfModX] = lin("X modulation", "%", -1, 1, 0);
  p.params[EfModY] = lin("Y modulation", "%", -1, 1, 0);
  p.params[EfModShape] = ch("Mod shape", {"Sine", "Triangle", "Ramp", "Square", "Random", "Smooth"}, 0);
  p.params[EfModRate] = ch("Mod rate", {"1/16", "1/8", "1/4", "1/2", "1 bar", "2 bars", "4 bars", "8 bars"}, 4);
  p.params[EfInput] = lin("Input", "dB", -24, 12, 0);
  p.params[EfOutput] = lin("Output", "dB", -24, 12, 0);
  p.params[EfMix] = lin("Wet", "%", 0, 1, 1);
  p.params[EfBypass] = tog("Bypass effect", 0);
  p.params[EfModel] = ch("Engine", {"Legacy", "Reference"}, 1);
  p.params[EfRawKind] = lin("Effect", "", 0, 1, 0);  // kind = round(11 v)
  p.params[EfRawBypass] = tog("Bypass effect", 0);
  p.params[EfRawMix] = lin("Wet", "%", 0, 1, 1);
  p.params[EfRawX] = lin("X", "%", 0, 1, 0.5);
  p.params[EfRawY] = lin("Y", "%", 0, 1, 0.5);
  p.params[EfRawRate] = lin("Mod rate", "", 0, 1, 0.125);  // choice = round(8 v): 1/8 .. 8 beats
  p.params[EfRawTempoRed] = lin("Tempo reduce", "%", 0, 1, 0);
  p.params[EfRawModX] = lin("X modulation", "%", 0, 1, 0.5);  // amount (2v - 1)|2v - 1|
  p.params[EfRawModY] = lin("Y modulation", "%", 0, 1, 0.5);
  p.params[EfRawShape] = lin("Mod shape", "", 0, 1, 0);  // round(2 v): sine, saw, square
  p.params[EfRawIn] = lin("Input", "%", 0, 1, 1);         // gain v²
  p.params[EfRawOut] = lin("Output", "%", 0, 1, 0.5);     // 10^(2.4 v - 1.2)
  p.legacyModelParam = EfModel;
  auto kd = [](int k) { return static_cast<float>(k / 11.0); };
  p.presets = {{"Grit", {{EfRawX, 0.62f}, {EfRawY, 0.35f}}},
               {"Filter sweep", {{EfRawKind, kd(4)}, {EfRawX, 0.45f}, {EfRawY, 0.4f}, {EfRawModX, 0.9f}, {EfRawRate, 0.625f}}},
               {"Phase drift", {{EfRawKind, kd(3)}, {EfRawY, 0.6f}, {EfRawModX, 0.85f}, {EfRawShape, 0.5f}, {EfRawRate, 0.75f}}},
               {"Stutter quarter", {{EfRawKind, kd(9)}, {EfRawRate, 0.125f}}},
               {"Vowel talk", {{EfRawKind, kd(10)}, {EfRawX, 0.3f}, {EfRawModX, 0.85f}, {EfRawRate, 0.5f}}},
               {"Dub echo", {{EfRawKind, kd(5)}, {EfRawX, 0.75f}, {EfRawY, 0.35f}, {EfRawMix, 0.5f}, {EfRawRate, 0.375f}}}};
  p.editorWidth = 560;
  v.push_back(p);
}

// ------------------------------------------------------------------ transient split
void addTransplit(std::vector<PluginInfo>& v) {
  PluginInfo p{"gl.transplit", "Transient Split", "Dynamics", PluginKind::Effect, {}, false, {}};
  p.params.resize(TsCount);
  p.params[TsSplitFreq] = lg("Split", "Hz", 20, 16000, 20);
  p.params[TsBalance] = lin("Split balance", "%", -1, 1, 0);
  p.params[TsAttack] = lin("Transient level", "%", -1, 1, 0.3);
  p.params[TsAttackShape] = ch("Detection", {"Sharp", "Medium", "Soft"}, 1);
  p.params[TsRelease] = lin("Sustain level", "%", -1, 1, 0);
  p.params[TsReleaseShape] = ch("Release", {"Short", "Medium", "Long"}, 1);
  p.params[TsDrive] = lin("Drive", "%", 0, 1, 0);
  p.params[TsMix] = lin("Mix", "%", 0, 1, 1);
  p.params[TsGain] = lin("Gain", "dB", -18, 18, 0);
  p.params[TsSensitivity] = lin("Sensitivity", "%", 0, 1, 0.5);
  p.params[TsHold] = lin("Hold", "ms", 0, 50, 0);
  p.params[TsTransTilt] = lin("Transient tilt", "%", -1, 1, 0);
  p.params[TsSusTilt] = lin("Sustain tilt", "%", -1, 1, 0);
  const std::vector<std::string> fx = {"Saturate", "Bounce", "Diffuse", "Blur", "Echo", "Off"};
  for (int side = 0; side < 2; ++side) {
    const int o = side * kTsSlotStride;
    const std::string n = side ? "Sustain " : "Transient ";
    p.params[static_cast<size_t>(TsTransFx + o)] = ch(n + "effect", fx, TsFxOff);
    p.params[static_cast<size_t>(TsTransFxMix + o)] = lin(n + "effect mix", "%", 0, 1, 0.5);
    p.params[static_cast<size_t>(TsTransFxA + o)] = lin(n + "effect A", "%", 0, 1, 0.5);
    p.params[static_cast<size_t>(TsTransFxB + o)] = lin(n + "effect B", "%", 0, 1, 0.5);
  }
  p.params[TsLookahead] = tog("Look-ahead", 0);
  p.params[TsStereoLink] = tog("Stereo link", 1);
  p.params[TsTransOn] = tog("Transient on", 1);
  p.params[TsSusOn] = tog("Sustain on", 1);
  p.params[TsTransSync] = tog("Transient echo sync", 0);
  p.params[TsSusSync] = tog("Sustain echo sync", 0);
  {
    static const char* tn[42] = {"Sensitivity", "Hold", "Release", "Transient level", "Sustain level", "Transient tilt", "Sustain tilt",
                                 "T diffusion mix", "T diffusion depth", "T diffusion delay", "S diffusion mix", "S diffusion depth",
                                 "S diffusion delay", "T saturation mix", "T saturation drive", "T saturation warmth", "S saturation mix",
                                 "S saturation drive", "S saturation warmth", "T bounce mix", "T bounce amp curve", "T bounce time curve",
                                 "S bounce mix", "S bounce amp curve", "S bounce time curve", "T delay mix", "T delay time",
                                 "T delay synced time", "T delay feedback", "S delay mix", "S delay time", "S delay synced time",
                                 "S delay feedback", "Look-ahead", "Stereo link", "Detection mode", "Transient on", "Sustain on",
                                 "Transient effect", "Sustain effect", "T delay sync", "S delay sync"};
    for (int k = 0; k < 42; ++k) {
      ParamDesc d = lin(std::string("Ref ") + tn[k], "", kTsRawMin[k], kTsRawMax[k], kTsRawDef[k]);
      if (kTsRawMax[k] <= 5) {
        std::vector<std::string> c;
        if (k == 38 || k == 39) c = {"Saturation", "Bounce", "Diffusion", "Blur", "Delay", "Off"};
        else if (k == 35) c = {"Smooth", "Balanced", "Dense"};
        else c = {"Off", "On"};
        d = ch(std::string("Ref ") + tn[k], c, kTsRawDef[k]);
      }
      p.params[static_cast<size_t>(tsRaw(k))] = d;
    }
  }
  p.params[TsModel] = ch("Engine", {"Gridline", "Reference"}, 0);
  p.params[TsSendT] = lin("Transient send route", "", 0, 500, 0);
  p.params[TsSendS] = lin("Sustain send route", "", 0, 500, 0);
  p.params[TsScIn] = lin("Detection source", "", -1, 500, -1);
  p.sendParams = {TsSendT, TsSendS};
  p.routeModeParam = kRouteIndexAlways;
  p.sideInParam = TsScIn;
  p.legacyModelParam = TsModel;
  p.presets = {{"Snap", {}},
               {"Tighten tails", {{TsAttack, 0.1f}, {TsRelease, -0.6f}}},
               {"Soften hits", {{TsAttack, -0.5f}, {TsAttackShape, 2.f}}},
               {"Crisp highs", {{TsSplitFreq, 2000.f}, {TsBalance, 1.f}, {TsAttack, 0.6f}, {TsTransTilt, 0.4f}}},
               {"Fat drums", {{TsAttack, 0.4f}, {TsRelease, 0.3f}, {TsSusFx, static_cast<float>(TsFxSaturate)}, {TsSusFxMix, 0.6f},
                              {TsSusFxA, 0.4f}, {TsReleaseShape, 2.f}}},
               {"Echo tails", {{TsAttack, 0.f}, {TsSusFx, static_cast<float>(TsFxEcho)}, {TsSusFxMix, 0.35f}, {TsSusFxA, 0.45f},
                               {TsSusFxB, 0.62f}, {TsSusSync, 1.f}}}};
  p.editorWidth = 740;
  v.push_back(p);
}

}  // namespace

namespace {
// J-45 Hit Looper (gl.hitloop).
void addHitLoop(std::vector<PluginInfo>& v) {
  PluginInfo p{"gl.hitloop", "Hit Repeater", "Delay & reverb", PluginKind::Effect, {}, false, {}};
  p.params.resize(HlCount);
  static const char* names[22] = {"Dry level", "Loop A level", "Loop B level", "Reverse chance", "Octave chance", "Octave up chance",
                                  "Spread / auto-pan", "Minimum length", "A / B ratio", "Hit sensitivity", "Bounce", "Hold loops",
                                  "Clear loops", "Length mode", "Bypass", "Retrigger A", "Retrigger B", "Retrigger A + B", "Fixed choices",
                                  "Tempo length", "Tempo trigger", "Skip chance"};
  static const std::vector<std::string> notes = {"1 bar", "1/2", "1/4", "1/8", "1/16", "1/32", "1/2 dot", "1/4 dot", "1/8 dot", "1/16 dot",
                                                 "1/32 dot", "1/2 tri", "1/4 tri", "1/8 tri", "1/16 tri", "1/32 tri", "1/5", "1/10", "1/20",
                                                 "1/40", "1/80"};
  for (int k = 0; k < 22; ++k) p.params[static_cast<size_t>(k)] = lin(names[k], "", kHlRawMin[k], kHlRawMax[k], kHlRawDef[k]);
  p.params[HlFreeze] = tog(names[HlFreeze], 0);
  p.params[HlFlush] = tog(names[HlFlush], 0);
  p.params[HlBypass] = tog(names[HlBypass], 0);
  p.params[HlUpdateA] = tog(names[HlUpdateA], 0);
  p.params[HlUpdateB] = tog(names[HlUpdateB], 0);
  p.params[HlUpdateAB] = tog(names[HlUpdateAB], 0);
  p.params[HlFixed] = ch(names[HlFixed], {"Random", "Fixed"}, 0);
  p.params[HlMode] = ch(names[HlMode], {"Hits", "Bounce", "Adapt", "Tempo"}, 0);
  p.params[HlTempoLength] = ch(names[HlTempoLength], notes, 0);
  std::vector<std::string> trig = notes;
  trig.push_back("Off");
  p.params[HlTempoTrigger] = ch(names[HlTempoTrigger], trig, 21);
  p.params[HlSource] = ch("Detect from", {"Left", "Right", "Both"}, 2);
  p.params[HlStereo] = ch("Capture", {"Mono", "Stereo"}, 1);
  p.params[HlFlushOnStop] = tog("Clear on stop", 1);
  p.params[HlLookahead] = tog("Look-ahead", 1);
  p.params[HlSendTo] = lin("Loops to route", "", 0, 500, 0);
  p.params[HlInput] = lin("Detection route", "", -1, 500, 0);
  p.sendParams = {HlSendTo};
  p.routeModeParam = kRouteIndexAlways;
  p.sideInParam = HlInput;
  p.presets = {{"Hit repeats", {}},
               {"Backwards hits", {{HlDry, 0.f}, {HlLevelB, 0.f}, {HlReverse, 10000.f}, {HlSpread, 768.f}, {HlSensitivity, 1024.f},
                                   {HlMode, 3.f}, {HlFixed, 1.f}}},
               {"Octave scatter", {{HlDry, 0.f}, {HlOctave, 2000.f}, {HlOctaveUp, 5000.f}, {HlSpread, 1024.f}, {HlMode, 3.f},
                                   {HlTempoLength, 1.f}, {HlRatio, 0.f}}},
               {"Drifting echoes", {{HlLevelA, 8000.f}, {HlLevelB, 8000.f}, {HlReverse, 5000.f}, {HlOctave, 10000.f}, {HlMinLength, 2000.f},
                                    {HlRatio, 2.f}, {HlMode, 2.f}, {HlFixed, 1.f}}}};
  p.editorWidth = 640;
  v.push_back(std::move(p));
}
}  // namespace

void appendInfos(std::vector<PluginInfo>& v) {
  addHitLoop(v);
  addMaster(v);
  addLoudness(v);
  addEnhance(v);
  addRack(v);
  addDistortions(v);
  addEffector(v);
  addTransplit(v);
  appendPocketInfo(v);
}

}  // namespace gl::packs::dynamics
