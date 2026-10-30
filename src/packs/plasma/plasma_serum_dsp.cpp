// Gridline: Serum states read by Plasma: Serum 1 preset chunks (.fxp, VST2
// chunks), the state of hosted Serum / Serum 2 instances (VST3 component
// state, AU class info, VST2 chunk), the single open path for preset files,
// and automation links of the hosted plugins' parameters (stand-ins, see
// core/PluginStandIn.h).
//
// Serum 1 layout (worked out from saved presets and published notes; no Xfer
// code or data is used): the chunk is zlib data holding the 299 VST
// parameters (normalized floats at 0x3460, the last 71 at 0x4AE0), 40-byte
// modulation records ({amount, output, type, source, aux source, destination
// parameter, 0x8080 marker, 0xFF00 | slot}), the effect rack order (0x3BE0),
// wavetable names (0x3C08, 0x3E08), the noise sample (0x4008), preset name
// (0x4972), author (0x49A0), macro names (0x4A60) and LFO shapes. Each value
// is written into the Serum 2 document form Plasma's importer reads, through
// the parameter laws measured on Serum 2 (plasma_serum2_params.inc).
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include "../../core/Json.h"
#include "../../core/Plist.h"
#include "../../core/Presets.h"
#include "../../core/Zip.h"
#include "../../host/Backend.h"
#include "../../host/HostedPlugins.h"
#include "plasma.h"
#include "plasma_serum.h"

namespace gl::packs::plasma {

namespace fs = std::filesystem;

namespace {

// ------------------------------------------------------------------ Serum 2 parameter laws
struct S2Param {
  uint32_t id;
  const char* title;
  const char* mod;  // module path ("Oscillator0/WTOsc0")
  const char* key;
  int law;          // 0 none, 1 lin, 2 pow, 3 stepped, 4 enumeration
  double a, b, c;
  int steps, list, quant;
};
#include "plasma_serum2_params.inc"
constexpr int kS2Count = static_cast<int>(sizeof(kS2Params) / sizeof(kS2Params[0]));
constexpr int kS2AuCount = static_cast<int>(sizeof(kS2AuIds) / sizeof(kS2AuIds[0]));

const char* const kLoopModes[] = {"kNone", "kForward", "kReverse", "kPingPong", "kTailed"};
const char* const kStacks[] = {"kNoUnisonStack", "kOctave1", "kOctave2", "kOctave3", "kOctaveFifth1", "kOctaveFifth2", "kOctaveFifth3", "kCenter12", "kCenter24"};
const char* const kSubShapes[] = {"kSine", "kRoundRect", "kTriangle", "kSaw", "kSquare", "kPulse"};

const char* listToken(int list, int q) {
  switch (list) {
    case 1: return serum::warpKey(std::clamp(q, 0, 69));
    case 2: return serum::filterKey(std::clamp(q, 0, serum::filterKeyCount() - 1));
    case 3: return kLoopModes[std::clamp(q, 0, 4)];
    case 4: return kStacks[std::clamp(q, 0, 8)];
    case 5: return kSubShapes[std::clamp(q, 0, 5)];
    default: return "";
  }
}

int quantize(double n, int steps, int quant) {
  n = std::clamp(n, 0.0, 1.0);
  if (quant >= 2) n = static_cast<double>(static_cast<float>(n));
  const int q = quant % 2 == 0 ? static_cast<int>(std::floor(n * steps + 0.5)) : static_cast<int>(std::floor(n * (steps + 1)));
  return std::clamp(q, 0, steps);
}

// Plain value Serum 2 stores for normalized n.
Json s2Plain(const S2Param& p, double n) {
  n = std::clamp(n, 0.0, 1.0);
  switch (p.law) {
    case 1: return Json::makeNumber(p.a + p.b * n);
    case 2: return Json::makeNumber(p.a + (p.b - p.a) * std::pow(n, p.c));
    case 3: return Json::makeNumber(p.a + (p.b - p.a) * quantize(n, p.steps, p.quant) / std::max(1, p.steps));
    case 4: return Json::makeString(listToken(p.list, quantize(n, p.steps, p.quant)));
    default: return Json{};
  }
}

const S2Param* s2ById(uint32_t id) {
  for (const auto& p : kS2Params)
    if (p.id == id) return &p;
  return nullptr;
}
const S2Param* s2ByKey(const char* mod, const char* key) {
  for (const auto& p : kS2Params)
    if (p.law != 0 && std::strcmp(p.mod, mod) == 0 && std::strcmp(p.key, key) == 0) return &p;
  return nullptr;
}

// root["Oscillator0"]["WTOsc0"]...: created on the way.
Json& module(Json& root, const std::string& path) {
  Json* cur = &root;
  size_t at = 0;
  while (at <= path.size()) {
    const size_t sl = path.find('/', at);
    const std::string seg = path.substr(at, sl == std::string::npos ? std::string::npos : sl - at);
    if (cur->type != Json::Type::Object) *cur = Json::makeObject();
    cur = &cur->object[seg];
    if (sl == std::string::npos) break;
    at = sl + 1;
  }
  if (cur->type != Json::Type::Object) *cur = Json::makeObject();
  return *cur;
}
void putPlain(Json& root, const std::string& path, const std::string& key, Json v) {
  Json& m = module(root, path);
  Json& pp = m.object["plainParams"];
  if (pp.type != Json::Type::Object) pp = Json::makeObject();
  pp.object[key] = std::move(v);
}

// ------------------------------------------------------------------ Serum 1 parameters
enum K : uint8_t { KNone, KS2, KLin, KNorm, KEnum, KSpecial };
struct S1Def {
  std::string name;
  float lo = 0, hi = 1, def = 0;
  K k = KNone;
  std::string mod, key;  // Serum 2 module path + key, or the effect key (fx > 0)
  int fx = 0;            // Plasma effect type
};

// The Serum 1 chunk layout (parameter order, source ids, menu orders) follows the format notes of
// serum2vital (https://github.com/btesser/serum2vital, GPL-3.0, like Gridline), checked against
// presets; the value laws were measured on the plugin.
constexpr int kS1Params = 299;

const std::vector<S1Def>& s1Defs() {
  static const std::vector<S1Def> t = [] {
    std::vector<S1Def> v(kS1Params);
    auto set = [&](int i, const char* name, float lo, float hi, float def, K k, const std::string& mod = "", const std::string& key = "", int fx = 0) {
      S1Def& d = v[static_cast<size_t>(i)];
      d.name = name;
      d.lo = lo;
      d.hi = hi;
      d.def = def;
      d.k = k;
      d.mod = mod;
      d.key = key;
      d.fx = fx;
    };
    set(0, "MasterVol", 0, 100, 0.7f, KS2, "Global0", "kParamMasterVolume");
    for (int o = 0; o < 2; ++o) {
      const int b = o * 13;
      const std::string osc = "Oscillator" + std::to_string(o), wt = osc + "/WTOsc" + std::to_string(o);
      const char* L = o == 0 ? "A" : "B";
      auto nm = [&](const char* s) { return std::string(L) + " " + s; };
      v[static_cast<size_t>(1 + b)] = {nm("Vol"), 0, 100, 0.75f, KS2, osc, "kParamVolume", 0};
      v[static_cast<size_t>(2 + b)] = {nm("Pan"), -50, 50, 0.5f, KS2, osc, "kParamPan", 0};
      v[static_cast<size_t>(3 + b)] = {nm("Octave"), -4, 4, 0.5f, KS2, osc, "kParamOctave", 0};
      v[static_cast<size_t>(4 + b)] = {nm("Semi"), -12, 12, 0.5f, KS2, osc, "kParamPitch", 0};
      v[static_cast<size_t>(5 + b)] = {nm("Fine"), -100, 100, 0.5f, KS2, osc, "kParamFine", 0};
      v[static_cast<size_t>(6 + b)] = {nm("Unison"), 0, 16, 0.0f, KS2, osc, "kParamUnison", 0};
      v[static_cast<size_t>(7 + b)] = {nm("UniDet"), 0, 1, 0.25f, KS2, osc, "kParamDetune", 0};
      v[static_cast<size_t>(8 + b)] = {nm("UniBlend"), 0, 100, 0.75f, KS2, osc, "kParamDetuneWid", 0};
      v[static_cast<size_t>(9 + b)] = {nm("Warp"), 0, 1, 0.0f, KS2, wt, "kParamWarp", 0};
      v[static_cast<size_t>(10 + b)] = {nm("CoarsePit"), 0, 1, 0.5f, KS2, osc, "kParamCoarsePit", 0};
      v[static_cast<size_t>(11 + b)] = {nm("WTPos"), 1, 256, 0.0f, KS2, wt, "kParamTablePos", 0};
      v[static_cast<size_t>(12 + b)] = {nm("RandPhase"), 0, 100, 1.0f, KS2, wt, "kParamRandomPhase", 0};
      v[static_cast<size_t>(13 + b)] = {nm("Phase"), 0, 1, 0.5f, KS2, wt, "kParamInitialPhase", 0};
    }
    set(27, "Noise Level", 0, 100, 0.25f, KS2, "Oscillator3", "kParamVolume");
    set(28, "Noise Pitch", 0, 100, 0.5f, KS2, "Oscillator3/NoiseOsc3", "kParamColor");
    set(29, "Noise Fine", -1, 1, 0.5f, KS2, "Oscillator3/NoiseOsc3", "kParamFine");
    set(30, "Noise Pan", -50, 50, 0.5f, KS2, "Oscillator3", "kParamPan");
    set(31, "Noise RandPhase", 0, 100, 0.0f, KS2, "Oscillator3/NoiseOsc3", "kParamRandomPhase");
    set(32, "Noise Phase", 0, 100, 0.0f, KS2, "Oscillator3/NoiseOsc3", "kParamInitialPhase");
    set(33, "Sub Osc Level", 0, 100, 0.75f, KS2, "Oscillator4", "kParamVolume");
    set(34, "Sub Osc Pan", -50, 50, 0.5f, KS2, "Oscillator4", "kParamPan");
    auto env = [&](int base, int e, const char* nm[5], float sus) {
      const std::string m = "Env" + std::to_string(e);
      set(base, nm[0], 0, 1, 0.11f, KS2, m, "kParamAttack");
      set(base + 1, nm[1], 0, 1, 0.0f, KS2, m, "kParamHold");
      set(base + 2, nm[2], 0, 1, 0.5f, KS2, m, "kParamDecay");
      set(base + 3, nm[3], 0, 1, sus, KS2, m, "kParamSustain");
      set(base + 4, nm[4], 0, 1, 0.215f, KS2, m, "kParamRelease");
    };
    const char* e1[5] = {"Env1 Atk", "Env1 Hold", "Env1 Dec", "Env1 Sus", "Env1 Rel"};
    const char* e2[5] = {"Env2 Atk", "Env2 Hld", "Env2 Dec", "Env2 Sus", "Env2 Rel"};
    const char* e3[5] = {"Env3 Atk", "Env3 Hld", "Env3 Dec", "Env3 Sus", "Env3 Rel"};
    env(35, 0, e1, 1.0f);
    set(40, "OscA>Fil", 0, 1, 1, KSpecial);
    set(41, "OscB>Fil", 0, 1, 0, KSpecial);
    set(42, "OscN>Fil", 0, 1, 0, KSpecial);
    set(43, "OscS>Fil", 0, 1, 0, KSpecial);
    set(44, "Fil Type", 0, 1, 1.0f / 88.0f, KSpecial);
    set(45, "Fil Cutoff", 0, 1, 0.5f, KS2, "VoiceFilter0", "kParamFreq");
    set(46, "Fil Reso", 0, 100, 0.1f, KS2, "VoiceFilter0", "kParamReso");
    set(47, "Fil Driv", 0, 100, 0.0f, KS2, "VoiceFilter0", "kParamDrive");
    set(48, "Fil Var", 0, 100, 0.0f, KS2, "VoiceFilter0", "kParamVar");
    set(49, "Fil Mix", 0, 100, 1.0f, KS2, "VoiceFilter0", "kParamWet");
    set(50, "Fil Stereo", 0, 100, 0.5f, KS2, "VoiceFilter0", "kParamStereo");
    env(51, 1, e2, 1.0f);
    env(56, 2, e3, 1.0f);
    for (int l = 0; l < 4; ++l) set(61 + l, ("LFO" + std::to_string(l + 1) + "Rate").c_str(), 0, 1, 0.5f, KS2, "LFO" + std::to_string(l), "kParamRate");
    set(65, "PortTime", 0, 1, 0.0f, KS2, "Global0", "kParamPortamentoTime");
    set(66, "PortCurve", -100, 100, 0.5f, KS2, "Global0", "kParamPortamentoCurve");
    set(67, "Chaos1 BPM", 0, 1, 0, KNone);
    set(68, "Chaos2 BPM", 0, 1, 0, KNone);
    set(69, "Chaos1 Rate", 0, 1, 0.251189f, KNone);
    set(70, "Chaos2 Rate", 0, 1, 0.251189f, KNone);
    for (int e = 0; e < 3; ++e) {
      const std::string m = "Env" + std::to_string(e), n = std::to_string(e + 1);
      set(71 + 3 * e, ("A curve" + n).c_str(), 0, 100, 0.5f, KS2, m, "kParamCurve1");
      set(72 + 3 * e, ("D curve" + n).c_str(), 0, 100, 0.67f, KS2, m, "kParamCurve2");
      set(73 + 3 * e, ("R curve" + n).c_str(), 0, 100, 0.67f, KS2, m, "kParamCurve3");
    }
    set(80, "Mast.Tun", 0, 1, 0.5f, KS2, "Global0", "kParamMasterTuning");
    set(81, "Verb Wet", 0, 100, 0.33f, KLin, "", "kParamWet", FxReverb);
    set(82, "VerbSize", 0, 100, 0.33f, KLin, "", "kParamSize", FxReverb);
    set(83, "VerbPDly", 0, 1, 0.12f, KNorm, "", "kParamDelay", FxReverb);
    set(84, "VerbLoCt", 0, 100, 0.0f, KLin, "", "kParamFreq", FxReverb);
    set(85, "VerbDamp", 0, 100, 0.28f, KLin, "", "kParamFreqC", FxReverb);
    set(86, "VerbHiCt", 0, 100, 0.5f, KLin, "", "kParamFreqB", FxReverb);
    set(87, "VerbWdth", 0, 100, 1.0f, KLin, "", "kParamWidth", FxReverb);
    set(88, "EQ FrqL", 0, 1, 0.333f, KNorm, "", "kParamFreq1", FxEq);
    set(89, "EQ FrqH", 0, 1, 0.666f, KNorm, "", "kParamFreq2", FxEq);
    set(90, "EQ Q L", 0, 100, 0.6f, KLin, "", "kParamReso1", FxEq);
    set(91, "EQ Q H", 0, 100, 0.6f, KLin, "", "kParamReso2", FxEq);
    set(92, "EQ VolL", -24, 24, 0.5f, KLin, "", "kParamGain1", FxEq);
    set(93, "EQ VolH", -24, 24, 0.5f, KLin, "", "kParamGain2", FxEq);
    set(94, "EQ TypL", 0, 2, 0.0f, KEnum, "", "kParamType1", FxEq);
    set(95, "EQ TypH", 0, 2, 0.0f, KEnum, "", "kParamType2", FxEq);
    set(96, "Dist_Wet", 0, 100, 1.0f, KLin, "", "kParamWet", FxDistortion);
    set(97, "Dist_Drv", 0, 100, 0.25f, KLin, "", "kParamDrive", FxDistortion);
    set(98, "Dist_L/B/H", 0, 100, 0.0f, KLin, "", "kParamLPHP", FxDistortion);
    set(99, "Dist_Mode", 0, 1, 0.0f, KSpecial, "", "kParamMode", FxDistortion);
    set(100, "Dist_Freq", 0, 1, 0.5f, KNorm, "", "kParamFreq", FxDistortion);
    set(101, "Dist_BW", 0, 1, 0.5f, KNorm, "", "kParamBW", FxDistortion);
    set(102, "Dist_PrePost", 0, 2, 0.0f, KEnum, "", "kParamPrePost", FxDistortion);
    set(103, "Flg_Wet", 0, 100, 1.0f, KLin, "", "kParamWet", FxFlanger);
    set(104, "Flg_BPM_Sync", 0, 1, 0.0f, KEnum, "", "kParamBeatSync", FxFlanger);
    set(105, "Flg_Rate", 0, 1, 0.25f, KNorm, "", "kParamRate", FxFlanger);
    set(106, "Flg_Dep", 0, 100, 1.0f, KLin, "", "kParamDepth", FxFlanger);
    set(107, "Flg_Feed", 0, 100, 0.5f, KLin, "", "kParamFeedback", FxFlanger);
    set(108, "Flg_Stereo", 0, 360, 0.5f, KLin, "", "kParamWidth", FxFlanger);
    set(109, "Phs_Wet", 0, 100, 1.0f, KLin, "", "kParamWet", FxPhaser);
    set(110, "Phs_BPM_Sync", 0, 1, 0.0f, KEnum, "", "kParamBeatSync", FxPhaser);
    set(111, "Phs_Rate", 0, 1, 0.25f, KNorm, "", "kParamRate", FxPhaser);
    set(112, "Phs_Dpth", 0, 100, 0.5f, KLin, "", "kParamDepth", FxPhaser);
    set(113, "Phs_Frq", 0, 1, 0.5f, KNorm, "", "kParamFreq", FxPhaser);
    set(114, "Phs_Feed", 0, 100, 0.8f, KLin, "", "kParamFeedback", FxPhaser);
    set(115, "Phs_Stereo", 0, 360, 0.5f, KLin, "", "kParamWidth", FxPhaser);
    set(116, "Cho_Wet", 0, 100, 0.5f, KLin, "", "kParamWet", FxChorus);
    set(117, "Cho_BPM_Sync", 0, 1, 0.0f, KEnum, "", "kParamBeatSync", FxChorus);
    set(118, "Cho_Rate", 0, 1, 0.25f, KNorm, "", "kParamRate", FxChorus);
    set(119, "Cho_Dly", 0, 1, 0.5f, KNorm, "", "kParamDelay", FxChorus);
    set(120, "Cho_Dly2", 0, 1, 0.0f, KNorm, "", "kParamDelay2", FxChorus);
    set(121, "Cho_Dep", 0, 1, 1.0f, KNorm, "", "kParamDepth", FxChorus);
    set(122, "Cho_Feed", 0, 1, 0.1f, KNorm, "", "kParamFeedback", FxChorus);
    set(123, "Cho_Filt", 0, 1, 0.5f, KNorm, "", "kParamFilt", FxChorus);
    set(124, "Dly_Wet", 0, 100, 0.3f, KLin, "", "kParamWet", FxDelay);
    set(125, "Dly_Freq", 0, 1, 0.5f, KNorm, "", "kParamFreq", FxDelay);
    set(126, "Dly_BW", 0.8f, 8.2f, 0.8108f, KLin, "", "kParamBW", FxDelay);
    set(127, "Dly_BPM_Sync", 0, 1, 1.0f, KEnum, "", "kParamBeatSync", FxDelay);
    set(128, "Dly_Link", 0, 1, 0.0f, KEnum, "", "kParamLink", FxDelay);
    set(129, "Dly_TimL", 0, 1, 0.625f, KNorm, "", "kParamTimeL", FxDelay);
    set(130, "Dly_TimR", 0, 1, 0.625f, KNorm, "", "kParamTimeR", FxDelay);
    set(131, "Dly_Mode", 0, 2, 0.0f, KEnum, "", "kParamMode", FxDelay);
    set(132, "Dly_Feed", 0, 100, 0.4f, KLin, "", "kParamFeedback", FxDelay);
    set(133, "Dly_Off L", 0, 1, 0.5f, KNorm, "", "kParamOffsetL", FxDelay);
    set(134, "Dly_Off R", 0, 1, 0.5f, KNorm, "", "kParamOffsetR", FxDelay);
    set(135, "Cmp_Thr", 0, 1, 0.5f, KNorm, "", "kParamThresh", FxComp);
    set(136, "Cmp_Rat", 0, 1, 0.75f, KNorm, "", "kParamRatio", FxComp);
    set(137, "Cmp_Att", 0, 1, 0.3f, KNorm, "", "kParamAttack", FxComp);
    set(138, "Cmp_Rel", 0, 1, 0.3f, KNorm, "", "kParamRelease", FxComp);
    set(139, "CmpGain", 0, 1, 0.0f, KNorm, "", "kParamMakeup", FxComp);
    set(140, "CmpMBnd", 0, 1, 0.0f, KEnum, "", "kParamMultiband", FxComp);
    set(141, "FX Fil Wet", 0, 100, 1.0f, KLin, "", "kParamWet", FxFilter);
    set(142, "FX Fil Type", 0, 1, 0.0f, KSpecial, "", "kParamType", FxFilter);
    set(143, "FX Fil Freq", 0, 1, 0.5f, KNorm, "", "kParamFreq", FxFilter);
    set(144, "FX Fil Reso", 0, 100, 0.0f, KLin, "", "kParamReso", FxFilter);
    set(145, "FX Fil Drive", 0, 100, 0.0f, KLin, "", "kParamDrive", FxFilter);
    set(146, "FX Fil Var", 0, 100, 0.0f, KLin, "", "kParamVar", FxFilter);
    set(147, "Hyp_Wet", 0, 100, 0.5f, KLin, "", "kParamWet", FxHyper);
    set(148, "Hyp_Rate", 0, 100, 0.4f, KLin, "", "kParamRate", FxHyper);
    set(149, "Hyp_Detune", 0, 100, 0.25f, KLin, "", "kParamDetune", FxHyper);
    set(150, "Hyp_Unison", 0, 7, 0.571429f, KEnum, "", "kParamUnison", FxHyper);
    set(151, "Hyp_Retrig", 0, 1, 0.0f, KEnum, "", "kParamRetrig", FxHyper);
    set(152, "HypDim_Size", 0, 100, 0.5f, KLin, "", "kParamDimESize", FxHyper);
    set(153, "HypDim_Mix", 0, 100, 0.0f, KLin, "", "kParamDimEWet", FxHyper);
    const char* en[10] = {"Dist Enable", "Flg Enable", "Phs Enable", "Cho Enable", "Dly Enable",
                          "Comp Enable", "Rev Enable", "EQ Enable", "FX Fil Enable", "Hyp Enable"};
    for (int i = 0; i < 10; ++i) set(154 + i, en[i], 0, 1, 0.0f, KSpecial, "", "kParamEnable", FxDistortion + i);
    set(164, "OscAPitchTrack", 0, 1, 1.0f, KS2, "Oscillator0", "kParamPitchTrack");
    set(165, "OscBPitchTrack", 0, 1, 1.0f, KS2, "Oscillator1", "kParamPitchTrack");
    set(166, "Bend U", -24, 24, 26.0f / 48.0f, KS2, "Global0", "kParamBendRangeUp");
    set(167, "Bend D", -24, 24, 22.0f / 48.0f, KS2, "Global0", "kParamBendRangeDn");
    set(168, "WarpOscA", 0, 1, 0.0f, KSpecial, "Oscillator0/WTOsc0", "kParamWarpMenu");
    set(169, "WarpOscB", 0, 1, 0.0f, KSpecial, "Oscillator1/WTOsc1", "kParamWarpMenu");
    set(170, "SubOscShape", 0, 1, 0.0f, KSpecial, "Oscillator4/SubOsc4", "kParamShape");
    set(171, "SubOscOctave", -4, 4, 0.5f, KS2, "Oscillator4", "kParamOctave");
    set(172, "A Uni LR", 0, 100, 1.0f, KLin, "Oscillator0", "kParamUnisonStereo");
    set(173, "B Uni LR", 0, 100, 1.0f, KLin, "Oscillator1", "kParamUnisonStereo");
    set(174, "A Uni Warp", -100, 100, 0.5f, KLin, "Oscillator0", "kParamUnisonWarp");
    set(175, "B Uni Warp", -100, 100, 0.5f, KLin, "Oscillator1", "kParamUnisonWarp");
    set(176, "A Uni WTPos", -100, 100, 0.5f, KLin, "Oscillator0/WTOsc0", "kParamUnisonWTPos");
    set(177, "B Uni WTPos", -100, 100, 0.5f, KLin, "Oscillator1/WTOsc1", "kParamUnisonWTPos");
    set(178, "A Uni Stack", 0, 1, 0.0f, KSpecial, "Oscillator0", "kParamUnisonStack");
    set(179, "B Uni Stack", 0, 1, 0.0f, KSpecial, "Oscillator1", "kParamUnisonStack");
    for (int s = 0; s < 32; ++s) {
      const int at = s < 16 ? 180 + 2 * s : 228 + 2 * (s - 16);
      const std::string m = "ModSlot" + std::to_string(s), n = "Mod" + std::string(s < 9 ? " " : "") + std::to_string(s + 1);
      set(at, (n + " amt").c_str(), -100, 100, 0.5f, KS2, m, "kParamAmount");
      set(at + 1, (n + " out").c_str(), 0, 100, 1.0f, KS2, m, "kParamOut");
    }
    set(212, "Osc A On", 0, 1, 1.0f, KS2, "Oscillator0", "kParamEnable");
    set(213, "Osc B On", 0, 1, 0.0f, KS2, "Oscillator1", "kParamEnable");
    set(214, "Osc N On", 0, 1, 0.0f, KS2, "Oscillator3", "kParamEnable");
    set(215, "Osc S On", 0, 1, 0.0f, KS2, "Oscillator4", "kParamEnable");
    set(216, "Filter On", 0, 1, 0.0f, KS2, "VoiceFilter0", "kParamEnable");
    set(217, "Mod Wheel", 0, 100, 0.0f, KS2, "Global0", "kParamModWheel");
    for (int k = 0; k < 4; ++k) set(218 + k, ("Macro " + std::to_string(k + 1)).c_str(), 0, 100, 0.0f, KS2, "Macro" + std::to_string(k), "kParamValue");
    set(222, "Amp.", 0, 1, 0.5f, KS2, "Global0", "kParamVoiceAmp");
    for (int l = 0; l < 4; ++l) set(223 + l, ("LFO" + std::to_string(l + 1) + " smooth").c_str(), 0, 100, 0.0f, KS2, "LFO" + std::to_string(l), "kParamSmooth");
    set(227, "Pitch Bend", 0, 1, 0.5f, KNone);
    for (int l = 0; l < 4; ++l) {
      set(260 + l, ("LFO" + std::to_string(l + 5) + "Rate").c_str(), 0, 1, 0.5f, KS2, "LFO" + std::to_string(l + 4), "kParamRate");
      set(264 + l, ("LFO" + std::to_string(l + 5) + " smooth").c_str(), 0, 100, 0.0f, KS2, "LFO" + std::to_string(l + 4), "kParamSmooth");
    }
    set(268, "FXFil Pan", 0, 100, 0.5f, KLin, "", "kParamStereo", FxFilter);
    set(269, "Comp_Wet", 0, 100, 1.0f, KLin, "", "kParamWet", FxComp);
    set(270, "Gain L", 0, 200, 0.5f, KSpecial, "", "kParamGain0", FxComp);
    set(271, "Gain M", 0, 200, 0.5f, KSpecial, "", "kParamGain1", FxComp);
    set(272, "Gain H", 0, 200, 0.5f, KSpecial, "", "kParamGain2", FxComp);
    for (int l = 0; l < 8; ++l) {
      set(273 + l, ("LFO" + std::to_string(l + 1) + " Rise").c_str(), 0, 1, 0.0f, KS2, "LFO" + std::to_string(l), "kParamRise");
      set(281 + l, ("LFO" + std::to_string(l + 1) + " Delay").c_str(), 0, 1, 0.0f, KS2, "LFO" + std::to_string(l), "kParamDelay");
    }
    const char* lv[10] = {"FX Dist Level", "FX Flg Level", "FX Phaser Level", "FX Chorus Level", "FX Delay Level",
                          "FX Comp Level", "FX Reverb Level", "FX DimExp Level", "FX Filter Level", "FX Hyper Level"};
    for (int i = 0; i < 10; ++i) set(289 + i, lv[i], 0, 1, 0.0f, KNone);
    return v;
  }();
  return t;
}

// Serum 1 effect order of the enable parameters = Plasma types FxDistortion .. FxHyper.
constexpr int kS1Fx = 10;

// Serum 1 modulation sources -> Serum 2 source ids (0 = not available).
int s1Source(int s) {
  static const int t[34] = {0, 1, 2, 3, 4, 6, 7, 8, 9, 10, 11, 12, 13, 16, 17, 18, 33, 0, 0, 20, 21, 22, 23, 24, 25, 26, 27, 28, 19, 34, 35, 36, 37, 38};
  return s >= 0 && s < 34 ? t[s] : 0;
}

const char* const kS1Warps[24] = {"kNoWarp", "kSync",      "kSync",     "kSync",     "kBendPos",  "kBendNeg", "kBendPosNeg", "kPWM",
                                  "kASYMPos", "kASYMNeg",  "kASYMPosNeg", "kFlip",   "kFlip",     "kRemap_1", "kRemap_2",    "kRemap_3",
                                  "kRemap_4", "kQuantize", "kFM_OSC",   "kAM_OSC",   "kRM_OSC",   "kFM_NOISE", "kFM_SUB",    "kNoWarp"};
const char* const kS1SubShapes[5] = {"kSine", "kRoundRect", "kSaw", "kSquare", "kPulse"};

float rd32(const std::vector<uint8_t>& d, size_t at) {
  float f = 0;
  if (at + 4 <= d.size()) std::memcpy(&f, d.data() + at, 4);
  return f;
}
double rd64(const std::vector<uint8_t>& d, size_t at) {
  double f = 0;
  if (at + 8 <= d.size()) std::memcpy(&f, d.data() + at, 8);
  return f;
}
uint32_t ru32(const std::vector<uint8_t>& d, size_t at) {
  return at + 4 <= d.size() ? static_cast<uint32_t>(d[at] | d[at + 1] << 8 | d[at + 2] << 16 | static_cast<uint32_t>(d[at + 3]) << 24) : 0;
}
std::string cstr(const std::vector<uint8_t>& d, size_t at, size_t max) {
  std::string s;
  for (size_t i = at; i < d.size() && i < at + max && d[i]; ++i) s += static_cast<char>(d[i]);
  // printable only
  std::string o;
  for (char c : s)
    if (static_cast<unsigned char>(c) >= 32) o += c == '\\' ? '/' : c;
  return o;
}

// Index of a menu value stored as index / (count - 1); several counts across versions.
int menuIndex(float n, std::initializer_list<int> divisors, int& used) {
  for (int d : divisors)
    if (std::fabs(n * d - std::round(n * d)) < 2e-3) {
      used = d;
      return static_cast<int>(std::lround(n * d));
    }
  used = *divisors.begin();
  return static_cast<int>(std::lround(n * used));
}

// Effect object of the rack list for a Plasma type (created when absent).
Json& fxBody(std::map<int, Json>& fx, int type) {
  Json& b = fx[type];
  if (b.type != Json::Type::Object) {
    b = Json::makeObject();
    b.object["plainParams"] = Json::makeObject();
  }
  return b;
}

void addCurve(Json& lfo, const std::vector<double>& xs, const std::vector<double>& ys, const std::vector<double>& ts) {
  Json c = Json::makeObject(), jx = Json::makeArray(), jy = Json::makeArray(), jc = Json::makeArray();
  for (size_t i = 0; i < xs.size(); ++i) {
    jx.array.push_back(Json::makeNumber(std::clamp(xs[i], 0.0, 1.0)));
    jy.array.push_back(Json::makeNumber(std::clamp(ys[i], 0.0, 1.0)));
    if (i + 1 < xs.size()) jc.array.push_back(Json::makeNumber(std::clamp(ts[i], 0.0, 1.0)));
  }
  c.object["xVals"] = jx;
  c.object["yVals"] = jy;
  c.object["curveVals"] = jc;
  c.object["numPoints"] = Json::makeNumber(static_cast<double>(xs.size()) - 1);
  lfo.object["curveData"] = c;
  lfo.object["curveDisplayName"] = Json::makeString("Serum 1 shape");
}

// LFO shape points: arrays of `cap` doubles (tension, x, y); the shape ends at the first x = 1.
bool lfoShape(const std::vector<uint8_t>& d, size_t tens, size_t xs, size_t ys, int cap, int count, Json& lfo) {
  std::vector<double> x, y, t;
  const int n = count > 0 ? std::min(count, cap) : cap;
  for (int i = 0; i < n; ++i) {
    const double xv = rd64(d, xs + 8 * static_cast<size_t>(i));
    if (!std::isfinite(xv) || xv < -1e-6 || xv > 1.0 + 1e-6 || (!x.empty() && xv + 1e-9 < x.back())) return false;
    x.push_back(xv);
    y.push_back(rd64(d, ys + 8 * static_cast<size_t>(i)));
    t.push_back(rd64(d, tens + 8 * static_cast<size_t>(i)));
    if (count <= 0 && xv >= 1.0 - 1e-9) break;
  }
  if (x.size() < 2 || x.back() < 1.0 - 1e-6) return false;
  for (double v : y)
    if (!std::isfinite(v)) return false;
  addCurve(lfo, x, y, t);
  return true;
}

}  // namespace

// ------------------------------------------------------------------ Serum 1
namespace serum {

int serum1ParamCount() { return kS1Params; }
const char* serum1ParamName(int i) { return i >= 0 && i < kS1Params ? s1Defs()[static_cast<size_t>(i)].name.c_str() : ""; }

bool decodeSerum1(const uint8_t* p, size_t n, Serum1& out, std::string& err) {
  out = Serum1{};
  const uint8_t* chunk = p;
  size_t len = n;
  if (n >= 4 && std::memcmp(p, "CcnK", 4) == 0) {
    if (n < 60 || std::memcmp(p + 8, "FPCh", 4) != 0) {
      err = "not a Serum preset (unsupported .fxp kind)";
      return false;
    }
    if (std::memcmp(p + 16, "XfsX", 4) != 0) {
      err = "not a Serum preset";
      return false;
    }
    const uint32_t sz = static_cast<uint32_t>(p[56]) << 24 | static_cast<uint32_t>(p[57]) << 16 | static_cast<uint32_t>(p[58]) << 8 | p[59];
    chunk = p + 60;
    len = std::min<size_t>(sz, n - 60);
  }
  if (len < 3 || (chunk[0] & 0x0F) != 8) {
    err = "damaged Serum 1 data";
    return false;
  }
  if (!inflateRaw(chunk + 2, len - 2, out.state, 180000) || out.state.size() < 0x4AE0 + 4 * 71) {
    err = "damaged Serum 1 data";
    return false;
  }
  const auto& defs = s1Defs();
  out.params.resize(kS1Params);
  for (int i = 0; i < kS1Params; ++i) {
    const size_t at = i < 228 ? 0x3460 + 4 * static_cast<size_t>(i) : 0x4AE0 + 4 * static_cast<size_t>(i - 228);
    float v = rd32(out.state, at);
    if (!std::isfinite(v)) v = defs[static_cast<size_t>(i)].def;
    out.params[static_cast<size_t>(i)] = std::clamp(v, 0.0f, 1.0f);
  }
  out.name = cstr(out.state, 0x4972, 32);
  out.author = cstr(out.state, 0x49A0, 48);
  // Embedded audio: a second zlib stream after the state; the chunk ends with the
  // length of the first stream (u32 LE). It holds oscillator A's table, then B's
  // (their byte sizes are stored in the state), then the noise sample: mono float32.
  if (len >= 8) {
    const size_t first = static_cast<size_t>(chunk[len - 4]) | static_cast<size_t>(chunk[len - 3]) << 8 |
                         static_cast<size_t>(chunk[len - 2]) << 16 | static_cast<size_t>(chunk[len - 1]) << 24;
    std::vector<uint8_t> audio;
    if (first >= 3 && first + 3 <= len - 4 && (chunk[first] & 0x0F) == 8 &&
        inflateRaw(chunk + first + 2, len - 4 - first - 2, audio, size_t(96) << 20)) {
      auto u32 = [&](size_t at) {
        uint32_t v = 0;
        if (at + 4 <= out.state.size()) std::memcpy(&v, out.state.data() + at, 4);
        return static_cast<size_t>(v);
      };
      const size_t na = u32(0x4968) / 4, nb = u32(0x496C) / 4, total = audio.size() / 4;
      auto take = [&](size_t from, size_t count, std::vector<float>& dst) {
        if (from >= total) return;
        count = std::min(count, total - from);
        dst.resize(count);
        std::memcpy(dst.data(), audio.data() + from * 4, count * 4);
        for (float& f : dst)
          if (!std::isfinite(f)) f = 0;
      };
      if (na + nb <= total) {
        take(0, na, out.tableA);
        take(na, nb, out.tableB);
        take(na + nb, total - na - nb, out.noise);
      }
    }
  }
  return true;
}

void serum1Root(const Serum1& s, Json& meta, Json& root, std::vector<std::string>& notes) {
  meta = Json::makeObject();
  root = Json::makeObject();
  if (!s.name.empty()) meta.object["presetName"] = Json::makeString(s.name);
  if (!s.author.empty()) meta.object["presetAuthor"] = Json::makeString(s.author);
  const auto& defs = s1Defs();
  const auto& d = s.state;
  auto P = [&](int i) { return static_cast<double>(s.params[static_cast<size_t>(i)]); };
  std::map<int, Json> fx;  // Plasma effect type -> module body
  const bool modern = d.size() >= 172736;
  // ---- parameters
  for (int i = 0; i < kS1Params; ++i) {
    const S1Def& def = defs[static_cast<size_t>(i)];
    const double n = P(i);
    switch (def.k) {
      case KS2: {
        const S2Param* p2 = s2ByKey(def.mod.c_str(), def.key.c_str());
        if (p2) putPlain(root, def.mod, def.key, s2Plain(*p2, n));
        break;
      }
      case KLin: {
        const double v = def.lo + (def.hi - def.lo) * n;
        if (def.fx) fxBody(fx, def.fx).object["plainParams"].object[def.key] = Json::makeNumber(v);
        else putPlain(root, def.mod, def.key, Json::makeNumber(v));
        break;
      }
      case KNorm: {
        const int q = fxParamIndex(def.fx, def.key.c_str());
        if (q >= 0) fxBody(fx, def.fx).object["plainParams"].object[def.key] = Json::makeNumber(fxPlain(def.fx, q, n));
        break;
      }
      case KEnum: {
        const double v = std::round(n * def.hi);
        if (def.fx) fxBody(fx, def.fx).object["plainParams"].object[def.key] = Json::makeNumber(v);
        else putPlain(root, def.mod, def.key, Json::makeNumber(v));
        break;
      }
      default: break;
    }
  }
  // ---- menus and switches
  static const int routeOf[4] = {0, 1, 3, 4};  // A, B, noise, sub -> Serum 2 routing slots
  for (int k = 0; k < 4; ++k)
    putPlain(root, "RoutingSlot" + std::to_string(routeOf[k]), "kParamRoutingDest",
             Json::makeString(P(40 + k) >= 0.5 ? "kRoutingDestFilter" : "kRoutingDestMaster"));
  int used = 0;
  const int ft = std::clamp(menuIndex(static_cast<float>(P(44)), modern ? std::initializer_list<int>{95, 89, 88} : std::initializer_list<int>{88, 89, 95}, used), 0,
                            serum::filterKeyCount() - 1);
  putPlain(root, "VoiceFilter0", "kParamType", Json::makeString(serum::filterKey(ft)));
  const int fxt = std::clamp(menuIndex(static_cast<float>(P(142)), modern ? std::initializer_list<int>{95, 89, 88} : std::initializer_list<int>{88, 89, 95}, used), 0,
                             serum::filterKeyCount() - 1);
  fxBody(fx, FxFilter).object["plainParams"].object["kParamType"] = Json::makeString(serum::filterKey(fxt));
  const int dm = menuIndex(static_cast<float>(P(99)), {15, 12}, used);
  fxBody(fx, FxDistortion).object["plainParams"].object["kParamMode"] = Json::makeNumber(std::clamp(dm, 0, 15));
  for (int o = 0; o < 2; ++o) {
    const int w = std::clamp(static_cast<int>(std::lround(P(168 + o) * 23)), 0, 23);
    putPlain(root, "Oscillator" + std::to_string(o) + "/WTOsc" + std::to_string(o), "kParamWarpMenu", Json::makeString(kS1Warps[w]));
    if (w == 2 || w == 3 || w == 12) notes.push_back("Serum 1 warp modes Sync Window and Mirror: approximated by Sync and Flip");
    const int st = std::clamp(static_cast<int>(std::lround(P(178 + o) * 8)), 0, 8);
    putPlain(root, "Oscillator" + std::to_string(o), "kParamUnisonStack", Json::makeString(kStacks[st]));
    putPlain(root, "Oscillator" + std::to_string(o), "kParamType", Json::makeString("kOsc_WT"));
  }
  putPlain(root, "Oscillator4/SubOsc4", "kParamShape", Json::makeString(kS1SubShapes[std::clamp(static_cast<int>(std::lround(P(170) * 4)), 0, 4)]));
  putPlain(root, "Oscillator2", "kParamEnable", Json::makeNumber(0));  // Serum 1 has two wavetable oscillators
  putPlain(root, "VoiceFilter1", "kParamEnable", Json::makeNumber(0));
  for (int k = 0; k < 3; ++k) {
    const double pct = P(270 + k) * 200.0;
    fxBody(fx, FxComp).object["plainParams"].object[defs[static_cast<size_t>(270 + k)].key] =
        Json::makeNumber(std::clamp(20.0 * std::log10(std::max(pct, 0.5) / 100.0), -24.0, 24.0));
  }
  // ---- effect rack: all ten modules, in the stored order
  int pos[kS1Fx];
  bool valid = true;
  bool seen[kS1Fx] = {};
  for (int i = 0; i < kS1Fx; ++i) {
    const uint32_t v = ru32(d, 0x3BE0 + 4 * static_cast<size_t>(i));
    pos[i] = v < static_cast<uint32_t>(kS1Fx) ? static_cast<int>(v) : -1;
    if (pos[i] < 0 || seen[pos[i]]) valid = false;
    else seen[pos[i]] = true;
  }
  if (!valid)
    for (int i = 0; i < kS1Fx; ++i) pos[i] = i == kS1Fx - 1 ? 0 : i + 1;  // Serum 1's default: Hyper first
  std::vector<int> order(kS1Fx);
  for (int i = 0; i < kS1Fx; ++i) order[static_cast<size_t>(pos[i])] = i;
  Json list = Json::makeArray();
  std::map<int, int> fxIndex;  // Plasma type -> index in the rack list
  for (int k = 0; k < kS1Fx; ++k) {
    const int i = order[static_cast<size_t>(k)], type = FxDistortion + i;
    Json body = fxBody(fx, type);
    body.object["plainParams"].object["kParamEnable"] = Json::makeNumber(P(154 + i) >= 0.5 ? 1 : 0);
    Json e = Json::makeObject();
    e.object["type"] = Json::makeNumber(type - 1);
    e.object["FXSerum1"] = body;
    fxIndex[type] = static_cast<int>(list.array.size());
    list.array.push_back(e);
  }
  module(root, "FXRack0").object["FX"] = list;
  // ---- names and files
  for (int o = 0; o < 2; ++o) {
    const std::string wt = cstr(d, o == 0 ? 0x3C08 : 0x3E08, 512);
    Json& sub = module(root, "Oscillator" + std::to_string(o) + "/WTOsc" + std::to_string(o));
    if (!wt.empty()) sub.object["relativePathToWT"] = Json::makeString(fs::path(wt).has_extension() ? wt : wt + ".wav");
    const std::vector<float>& t = o == 0 ? s.tableA : s.tableB;
    if (t.size() >= 2048) {
      sub.object["embeddedWTData"] = floatBlob(t.data(), t.size() / 2048 * 2048);
      if (!wt.empty()) sub.object["tableDisplayName"] = Json::makeString(fs::path(wt).stem().string());
    }
  }
  if (const std::string noise = cstr(d, 0x4008, 512); !noise.empty())
    module(root, "Oscillator3/NoiseOsc3").object["relativePathToNoiseSample"] = Json::makeString(noise);
  if (!s.noise.empty()) {
    Json ch = Json::makeArray();
    ch.array.push_back(floatBlob(s.noise.data(), s.noise.size()));
    module(root, "Oscillator3/NoiseOsc3").object["embeddedNoiseData"] = std::move(ch);
  }
  for (int k = 0; k < 4; ++k)
    if (std::string m = cstr(d, 0x4A60 + 0x20 * static_cast<size_t>(k), 32); !m.empty()) module(root, "Macro" + std::to_string(k)).object["name"] = Json::makeString(m);
  // ---- LFO shapes and tempo switches
  for (int l = 0; l < 8; ++l) {
    Json& lfo = module(root, "LFO" + std::to_string(l));
    if (modern) {
      const size_t b = 0x84D8 + 0x2D28 * static_cast<size_t>(l);
      const int count = static_cast<int>(ru32(d, b + 0x2D10));
      if (count >= 2 && count <= 480) lfoShape(d, b + 8, b + 0xF08, b + 0x1E08, 480, count, lfo);
      putPlain(root, "LFO" + std::to_string(l), "kParamBeatSync", Json::makeNumber(1));
    } else {
      const bool hi = l >= 4;
      if (hi && d.size() < 28000) continue;
      const size_t base = hi ? 0x1B70 : 0x280, k = static_cast<size_t>(l % 4);
      lfoShape(d, base + 520 * k, base + 520 * (4 + k), base + 520 * (8 + k), 65, 0, lfo);
      const size_t sw = (hi ? 0x6DB8 : 0x1AE0) + 0x14 + k;  // tempo sync byte per LFO
      if (sw < d.size()) putPlain(root, "LFO" + std::to_string(l), "kParamBeatSync", Json::makeNumber(d[sw] ? 1 : 0));
    }
  }
  notes.push_back("Serum 1 LFO trigger modes are not read: free-running retrigger defaults apply");
  // ---- modulation matrix
  bool slotSeen[32] = {};
  int chaos = 0, unmapped = 0;
  for (size_t at = 0; at + 40 <= std::min<size_t>(d.size(), 0x3460); at += 4) {
    if (d[at + 0x20] != 0x80 || d[at + 0x21] != 0x80 || d[at + 0x23] != 0xFF) continue;
    const int slot = d[at + 0x22];
    if (slot >= 32 || slotSeen[slot]) continue;
    slotSeen[slot] = true;
    const int src = d[at + 0x14] | d[at + 0x15] << 8, aux = d[at + 0x16] | d[at + 0x17] << 8, dest = d[at + 0x1A] | d[at + 0x1B] << 8;
    if (src == 0) continue;
    const int s2 = s1Source(src), a2 = s1Source(aux);
    if (s2 == 0) {
      ++(src == 17 || src == 18 ? chaos : unmapped);
      continue;
    }
    if (dest >= kS1Params) {
      ++unmapped;
      continue;
    }
    const S1Def& dd = defs[static_cast<size_t>(dest)];
    std::string type, key = dd.key;
    int id = 0;
    if (dd.fx) {
      if (dd.key == "kParamEnable" || !fxIndex.count(dd.fx)) {
        ++unmapped;
        continue;
      }
      type = "FXSerum1";
      id = fxIndex[dd.fx];
    } else if (!dd.mod.empty()) {
      const std::string last = dd.mod.substr(dd.mod.rfind('/') == std::string::npos ? 0 : dd.mod.rfind('/') + 1);
      size_t k = last.size();
      while (k > 0 && std::isdigit(static_cast<unsigned char>(last[k - 1]))) --k;
      type = last.substr(0, k);
      id = k < last.size() ? std::atoi(last.c_str() + k) : 0;
    } else {
      ++unmapped;
      continue;
    }
    const int ampAt = slot < 16 ? 180 + 2 * slot : 228 + 2 * (slot - 16);
    Json& ms = module(root, "ModSlot" + std::to_string(slot));
    Json srcArr = Json::makeArray();
    srcArr.array.push_back(Json::makeNumber(s2));
    srcArr.array.push_back(Json::makeNumber(a2));
    ms.object["source"] = srcArr;
    ms.object["destModuleTypeString"] = Json::makeString(type);
    ms.object["destModuleID"] = Json::makeNumber(id);
    ms.object["destModuleParamName"] = Json::makeString(key);
    Json& pp = ms.object["plainParams"];
    if (pp.type != Json::Type::Object) pp = Json::makeObject();
    pp.object["kParamAmount"] = Json::makeNumber(-100.0 + 200.0 * P(ampAt));
    pp.object["kParamOut"] = Json::makeNumber(100.0 * P(ampAt + 1));
    pp.object["kParamBipolar"] = Json::makeNumber(d[at + 0x0C] ? 1 : 0);
  }
  if (chaos) notes.push_back("Serum 1 Chaos modulation sources are not available: their matrix slots were left out");
  if (unmapped) notes.push_back("Some Serum 1 matrix slots have no Plasma destination and were left out");
}

}  // namespace serum

// ------------------------------------------------------------------ hosted states
namespace {

enum class Kind { None, Serum1, Serum2 };

// The Serum document of a hosted instance's state.
bool hostedRoot(const PluginState& hosted, Json& meta, Json& root, serum::Serum1* s1, std::string& format, std::string& err,
                std::vector<std::string>* notesOut = nullptr) {
  const std::string& data = hosted.data;
  std::string bytes;
  const char* fmt = "";
  if (data.compare(0, 7, "vst3s1:") == 0) {
    std::string ctrl;
    if (!host::decodeState(data, bytes, ctrl)) {
      err = "damaged plugin state";
      return false;
    }
    fmt = "VST3 state";
  } else if (data.compare(0, 4, "au1:") == 0) {
    std::string plist;
    if (!host::base64Decode(data.substr(4), plist) || !plist::topValue(plist, "Processor State", bytes)) {
      if (!plist.empty() && plist::topValue(plist, "jucePluginState", bytes)) {
      } else if (!plist.empty() && plist::topValue(plist, "vstdata", bytes)) {
      } else {
        err = "the Audio Unit state holds no Serum data";
        return false;
      }
    }
    fmt = "AU state";
  } else if (data.compare(0, 7, "vst2c1:") == 0) {
    if (!host::base64Decode(data.substr(7), bytes)) {
      err = "damaged plugin state";
      return false;
    }
    fmt = "VST2 chunk";
  } else {
    err = data.empty() ? "no saved state" : "unsupported state format";
    return false;
  }
  const auto* p = reinterpret_cast<const uint8_t*>(bytes.data());
  if (bytes.size() >= 9 && std::memcmp(bytes.data(), "XferJson", 8) == 0) {
    format = std::string("Serum 2 (") + fmt + ")";
    return serum::decodeXferJson(p, bytes.size(), meta, root, err);
  }
  serum::Serum1 local;
  serum::Serum1& s = s1 ? *s1 : local;
  if (!serum::decodeSerum1(p, bytes.size(), s, err)) return false;
  format = std::string("Serum (") + fmt + ")";
  std::vector<std::string> notes;
  serum::serum1Root(s, meta, root, notes);
  if (notesOut) *notesOut = notes;
  return true;
}

// Embedded audio is large and unused by link probes.
void stripEmbedded(Json& j) {
  if (j.type == Json::Type::Object) {
    for (const char* k : {"embeddedWTData", "embedded_audio", "embedded_sfz", "embeddedNoiseData", "embeddedIR", "tuningData"}) j.object.erase(k);
    for (auto& [k, v] : j.object) stripEmbedded(v);
  } else if (j.type == Json::Type::Array) {
    if (j.array.size() > 4096) return;  // numeric blobs
    for (auto& v : j.array) stripEmbedded(v);
  }
}

bool isAu(const std::string& type) { return type.compare(0, 3, "au:") == 0; }
bool isVst2Serum1(const std::string& type) { return type == "vst2:58667358"; }

struct Probe {
  bool ok = false;
  bool s1 = false;
  Json meta, root;
  serum::Serum1 s1state;
};

}  // namespace

bool serumStateToPlasma(const PluginState& hosted, PluginState& out, ImportReport& rep) {
  rep = ImportReport{};
  Json meta, root;
  std::string err, format;
  serum::Serum1 s1;
  std::vector<std::string> notes;
  if (!hostedRoot(hosted, meta, root, &s1, format, err, &notes)) {
    rep.error = err;
    return false;
  }
  rep.format = format;
  rep.approximated = notes;
  serum::runImporter(meta, root, out, rep, {}, true);
  return rep.ok;
}

int serumParamCount(const std::string& type) {
  if (isVst2Serum1(type)) return kS1Params;
  if (isAu(type)) return kS2AuCount;
  return kS2Count;
}

std::string serumParamName(const std::string& type, int index) {
  if (index < 0) return "";
  if (isVst2Serum1(type)) return serum::serum1ParamName(index);
  if (isAu(type)) {
    if (index >= kS2AuCount) return "";
    const S2Param* p = s2ById(kS2AuIds[index]);
    return p ? p->title : "";
  }
  return index < kS2Count ? kS2Params[index].title : "";
}

int serumParamIndexOfId(const std::string& type, uint32_t id) {
  if (isVst2Serum1(type)) return id < static_cast<uint32_t>(kS1Params) ? static_cast<int>(id) : -1;
  if (isAu(type)) {
    for (int i = 0; i < kS2AuCount; ++i)
      if (kS2AuIds[i] == id) return i;
    return -1;
  }
  for (int i = 0; i < kS2Count; ++i)
    if (kS2Params[i].id == id) return i;
  return -1;
}

bool serumParamLink(const PluginState& hosted, int index, int& plasmaParam, std::vector<float>& curve, std::string& title) {
  plasmaParam = -1;
  curve.clear();
  title = serumParamName(hosted.type, index);
  if (title.empty()) return false;
  // decoded documents of recent states (probes run the importer many times)
  static std::mutex mu;
  static std::map<std::string, std::shared_ptr<Probe>> cache;
  std::shared_ptr<Probe> pr;
  {
    std::lock_guard<std::mutex> lk(mu);
    const std::string key = hosted.type + "|" + std::to_string(std::hash<std::string>{}(hosted.data)) + "|" + std::to_string(hosted.data.size());
    auto it = cache.find(key);
    if (it == cache.end()) {
      if (cache.size() > 16) cache.clear();
      auto p = std::make_shared<Probe>();
      std::string fmt, err;
      p->ok = hostedRoot(hosted, p->meta, p->root, &p->s1state, fmt, err);
      if (!p->ok) {  // no readable state: probe from an empty document (the defaults)
        p->meta = Json::makeObject();
        p->root = Json::makeObject();
        p->s1state = serum::Serum1{};
      }
      p->s1 = !p->s1state.params.empty() || isVst2Serum1(hosted.type);
      if (p->s1 && p->s1state.params.empty()) {
        p->s1state.params.resize(kS1Params);
        for (int i = 0; i < kS1Params; ++i) p->s1state.params[static_cast<size_t>(i)] = s1Defs()[static_cast<size_t>(i)].def;
        p->s1state.state.assign(0x4AE0 + 4 * 71, 0);
      }
      stripEmbedded(p->root);
      p->s1state.tableA.clear(), p->s1state.tableB.clear(), p->s1state.noise.clear();  // probes read no audio
      it = cache.emplace(key, p).first;
    }
    pr = it->second;
  }
  const S2Param* row = nullptr;
  if (!pr->s1) {
    if (isAu(hosted.type)) row = index < kS2AuCount ? s2ById(kS2AuIds[index]) : nullptr;
    else row = index < kS2Count ? &kS2Params[index] : nullptr;
    if (!row || row->law == 0) return false;
  } else if (index >= kS1Params || s1Defs()[static_cast<size_t>(index)].k == KNone) {
    return false;
  }
  constexpr int kSamples = 17;
  std::vector<std::vector<float>> runs;
  for (int i = 0; i < kSamples; ++i) {
    const double n = static_cast<double>(i) / (kSamples - 1);
    Json meta = pr->meta, root;
    if (pr->s1) {
      serum::Serum1 s = pr->s1state;
      s.params[static_cast<size_t>(index)] = static_cast<float>(n);
      std::vector<std::string> notes;
      serum::serum1Root(s, meta, root, notes);
      stripEmbedded(root);
    } else {
      root = pr->root;
      putPlain(root, row->mod, row->key, s2Plain(*row, n));
    }
    PluginState ps;
    ImportReport rep;
    serum::runImporter(meta, root, ps, rep, {}, false);
    if (!rep.ok) return false;
    runs.push_back(ps.params);
  }
  const PluginInfo* info = findPlugin(kType);
  if (!info) return false;
  int best = -1;
  double bestSpan = 1e-6;
  for (size_t k = 0; k < runs[0].size() && k < info->params.size(); ++k) {
    double lo = 1e9, hi = -1e9;
    for (const auto& r : runs) {
      const double v = info->params[k].toNormalized(r[k]);
      lo = std::min(lo, v);
      hi = std::max(hi, v);
    }
    if (hi - lo > bestSpan) bestSpan = hi - lo, best = static_cast<int>(k);
  }
  if (best < 0) return false;
  plasmaParam = best;
  for (const auto& r : runs) curve.push_back(static_cast<float>(info->params[static_cast<size_t>(best)].toNormalized(r[static_cast<size_t>(best)])));
  return true;
}

// ------------------------------------------------------------------ the open path
const std::vector<std::string>& openableExtensions() {
  static const std::vector<std::string> e = {".json", ".serumpreset", ".fxp"};
  return e;
}

bool isOpenableName(const std::string& fileName) {
  std::string ext = fs::u8path(fileName).extension().string();
  for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  for (const auto& e : openableExtensions())
    if (ext == e) return true;
  return false;
}

bool openPresetBytes(const std::vector<uint8_t>& bytes, const std::string& fileName, PluginState& out, ImportReport& rep,
                     const std::vector<std::string>& searchRoots, const std::string& presetDir) {
  rep = ImportReport{};
  const size_t n = bytes.size();
  const bool serum2 = n >= 9 && std::memcmp(bytes.data(), "XferJson", 8) == 0;
  const bool serum1 = n >= 4 && std::memcmp(bytes.data(), "CcnK", 4) == 0;
  if (serum2 || serum1) {
    const bool ok = importSerumBytes(bytes, out, rep, searchRoots, presetDir);
    if (ok && rep.presetName.empty()) rep.presetName = fs::u8path(fileName).stem().string();
    return ok;
  }
  // Plasma's own presets: Gridline preset JSON for gl.plasma
  size_t i = 0;
  while (i < n && std::isspace(bytes[i])) ++i;
  if (i < n && bytes[i] == '{') {
    const std::string tmp(bytes.begin(), bytes.end());
    Json j;
    if (!jsonParse(tmp, j) || !j.is(Json::Type::Object)) {
      rep.error = "damaged preset file";
      return false;
    }
    // same reader as the preset library (core/Presets.cpp), through a file in memory
    const std::string type = j.text("type", kType);
    if (type != kType) {
      rep.error = "this preset is for another plugin";
      return false;
    }
    out = defaultPluginState(kType);
    if (const Json* p = j.find("params"); p && p->is(Json::Type::Array))
      for (size_t k = 0; k < p->array.size() && k < out.params.size(); ++k)
        if (p->array[k].is(Json::Type::Number)) out.params[k] = static_cast<float>(p->array[k].number);
    out.data = j.text("data");
    normalizePluginState(out);
    rep.format = "Plasma preset";
    rep.presetName = fs::u8path(fileName).stem().string();
    rep.ok = true;
    return true;
  }
  rep.error = "not a Plasma or Serum preset";
  return false;
}

bool openPresetFile(const std::string& path, PluginState& out, ImportReport& rep, const std::vector<std::string>& searchRoots) {
  rep = ImportReport{};
  std::ifstream f(fs::u8path(path), std::ios::binary);
  if (!f) {
    rep.error = "cannot open the file";
    return false;
  }
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  if (bytes.empty()) {
    rep.error = "the file is empty";
    return false;
  }
  if (bytes.size() > (size_t(256) << 20)) {
    rep.error = "the file is too large";
    return false;
  }
  if (bytes[0] == '{') {  // Plasma presets go through the preset library reader (same fields)
    PluginState st;
    if (loadPreset(path, kType, st)) {
      out = st;
      rep.format = "Plasma preset";
      rep.presetName = fs::u8path(path).stem().string();
      rep.ok = true;
      return true;
    }
  }
  return openPresetBytes(bytes, fs::u8path(path).filename().string(), out, rep, searchRoots, fs::u8path(path).parent_path().u8string());
}

}  // namespace gl::packs::plasma
