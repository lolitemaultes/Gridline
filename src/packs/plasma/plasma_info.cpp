// Gridline — "plasma" pack: catalogue entry for Plasma (gl.plasma), the name
// tables shared by the runtime and the editor (warps, filters, matrix sources,
// effects, scales, divisions), the effect parameter definitions and the
// parameter display text.
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include "../Packs.h"
#include "plasma.h"

namespace gl::packs::plasma {

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

const char* const kOscLetter[kOscs] = {"A", "B", "C"};

}  // namespace

// ------------------------------------------------------------------ warps
const std::vector<std::string>& warpNames() {
  static const std::vector<std::string> v = [] {
    std::vector<std::string> w = {"Off",    "Sync",     "Bend +",   "Bend -",   "Bend +/-", "PWM",      "Asym +",
                                  "Asym -", "Asym +/-", "Flip",     "Mirror",   "Remap 1",  "Remap 2",  "Remap 3",
                                  "Remap 4", "Quantize", "Odd/Even", "Low-pass", "High-pass"};
    const char* dist[] = {"Tube",     "Soft Clip", "Hard Clip", "Diode 1",    "Diode 2",   "Linear Fold", "Sine Fold",
                          "Zero-Square", "Asymmetric", "Rectify", "Sine Shaper", "Stomp Box", "Tape Sat",   "Soft Sat"};
    for (const char* d : dist) w.emplace_back(d);
    const char* src[] = {"Osc 1", "Osc 2", "Noise", "Sub", "Filter 1", "Filter 2"};
    for (const char* kind : {"FM", "FM Exp", "FM Lin"})
      for (const char* s : src) w.push_back(std::string(kind) + " (" + s + ")");
    for (const char* s : src) w.push_back(std::string("PD (") + s + ")");
    w.emplace_back("PD (Self)");
    for (const char* s : src) w.push_back(std::string("AM (") + s + ")");
    for (const char* s : src) w.push_back(std::string("RM (") + s + ")");
    // spectral-only modes (70..103)
    const char* spec[] = {"Detune",        "Smear",          "Spread",          "Add Harmonics",  "Add Subharmonics",
                          "Gate",          "Robotize",       "Shift",           "Spectral Mirror", "Peak Follow",
                          "Peak Oct Up",   "Peak Oct Down",  "Peak Harm Up",    "Peak Harm Down", "Peak Harm Sweep",
                          "Shepard Narrow", "Shepard Filter", "Spectral Comb",  "Pitch Shift",    "Pitch Shift (Formant)",
                          "Phase Twist",   "Formant Shift"};
    for (const char* s : spec) w.emplace_back(s);
    for (const char* s : src) w.push_back(std::string("Mask (") + s + ")");
    for (const char* s : src) w.push_back(std::string("Vocode (") + s + ")");
    return w;
  }();
  return v;
}
int warpCount() { return static_cast<int>(warpNames().size()); }
bool warpAllowed(int oscType, int mode) {
  if (mode <= 0) return true;
  if (mode >= warpCount()) return false;
  const bool alt = mode >= 1 && mode <= 16;  // sync + alternate warps (table phase)
  const bool spectral = mode >= 70;
  switch (oscType) {
    case OscWavetable: return !spectral;
    case OscSpectral: return !alt;
    default: return !alt && !spectral;  // multisample, sample, granular
  }
}

// ------------------------------------------------------------------ filters
namespace {
struct FilterDef {
  const char* name;
  const char* group;
  const char* var;
};
const FilterDef kFilterDefs[] = {
    {"Warm Low 6", "Analog", "BODY"},      {"Warm Low 12", "Analog", "BODY"},     {"Warm Low 18", "Analog", "BODY"},
    {"Warm Low 24", "Analog", "BODY"},     {"Low 6", "Normal", "—"},              {"Low 12", "Normal", "—"},
    {"Low 18", "Normal", "—"},             {"Low 24", "Normal", "—"},             {"High 6", "Normal", "—"},
    {"High 12", "Normal", "—"},            {"High 18", "Normal", "—"},            {"High 24", "Normal", "—"},
    {"Band 12", "Normal", "—"},            {"Band 24", "Normal", "—"},            {"Peak 12", "Normal", "GAIN"},
    {"Peak 24", "Normal", "GAIN"},         {"Notch 12", "Normal", "—"},           {"Notch 24", "Normal", "—"},
    {"Low>High 6", "Multi", "MORPH"},      {"Low>High 12", "Multi", "MORPH"},     {"Low>Band 12", "Multi", "MORPH"},
    {"Low>Peak 12", "Multi", "MORPH"},     {"Low>Notch 12", "Multi", "MORPH"},    {"High>Band 12", "Multi", "MORPH"},
    {"High>Peak 12", "Multi", "MORPH"},    {"High>Notch 12", "Multi", "MORPH"},   {"Band>Peak 12", "Multi", "MORPH"},
    {"Band>Notch 12", "Multi", "MORPH"},   {"Peak>Peak 12", "Multi", "MORPH"},    {"Peak>Notch 12", "Multi", "MORPH"},
    {"Notch>Notch 12", "Multi", "MORPH"},  {"Low>Band>High 12", "Multi", "MORPH"}, {"Low>Band>High 24", "Multi", "MORPH"},
    {"Low>Peak>High 12", "Multi", "MORPH"}, {"Low>Peak>High 24", "Multi", "MORPH"}, {"Low>Notch>High 12", "Multi", "MORPH"},
    {"Low>Notch>High 24", "Multi", "MORPH"}, {"Band>Peak>Notch 12", "Multi", "MORPH"}, {"Band>Peak>Notch 24", "Multi", "MORPH"},
    {"Comb +", "Flanges", "CUT"},          {"Comb -", "Flanges", "CUT"},          {"Comb Low +", "Flanges", "CUT"},
    {"Comb Low -", "Flanges", "CUT"},      {"Comb High +", "Flanges", "CUT"},     {"Comb High -", "Flanges", "CUT"},
    {"Comb Band +", "Flanges", "CUT"},     {"Comb Band -", "Flanges", "CUT"},     {"Flange +", "Flanges", "SPREAD"},
    {"Flange -", "Flanges", "SPREAD"},     {"Flange Low +", "Flanges", "SPREAD"}, {"Flange Low -", "Flanges", "SPREAD"},
    {"Flange High +", "Flanges", "SPREAD"}, {"Flange High -", "Flanges", "SPREAD"}, {"Flange Band +", "Flanges", "SPREAD"},
    {"Flange Band -", "Flanges", "SPREAD"}, {"Phaser 12 +", "Phasers", "SPREAD"}, {"Phaser 12 -", "Phasers", "SPREAD"},
    {"Phaser 24 +", "Phasers", "SPREAD"},  {"Phaser 24 -", "Phasers", "SPREAD"},  {"Phaser 36 +", "Phasers", "SPREAD"},
    {"Phaser 36 -", "Phasers", "SPREAD"},  {"Phaser 48 +", "Phasers", "SPREAD"},  {"Phaser 48 -", "Phasers", "SPREAD"},
    {"Phaser 48 Low +", "Phasers", "SPREAD"}, {"Phaser 48 Low -", "Phasers", "SPREAD"}, {"Phaser 48 High +", "Phasers", "SPREAD"},
    {"Phaser 48 High -", "Phasers", "SPREAD"}, {"Phaser 48 Band +", "Phasers", "SPREAD"}, {"Phaser 48 Band -", "Phasers", "SPREAD"},
    {"Flange-Phase +", "Phasers", "SPREAD"}, {"Flange-Phase -", "Phasers", "SPREAD"}, {"Low Shelf 6", "EQ", "GAIN"},
    {"Low Shelf 12", "EQ", "GAIN"},        {"Bell 12", "EQ", "GAIN"},             {"High Shelf 6", "EQ", "GAIN"},
    {"High Shelf 12", "EQ", "GAIN"},       {"Ring Mod", "Misc", "SPREAD"},        {"Ring Mod x2", "Misc", "SPREAD"},
    {"Sample & Hold", "Misc", "SMOOTH"},   {"Sample & Hold 2", "Misc", "SMOOTH"}, {"Comb Cluster", "Misc", "SPREAD"},
    {"Allpass Cluster", "Misc", "SPREAD"}, {"Small Room", "Misc", "SIZE"},        {"Screamer", "Misc", "TONE"},
    {"Diode Low", "Analog", "BODY"},       {"Bass Lift", "Misc", "BASS"},         {"Formant 1", "Formant", "VOWEL"},
    {"Formant 2", "Formant", "VOWEL"},     {"Formant 3", "Formant", "VOWEL"},     {"Band Reject", "Normal", "WIDTH"},
    {"Dist Comb 1 Low", "Misc", "CUT"},    {"Dist Comb 1 Band", "Misc", "CUT"},   {"Dist Comb 2 Low", "Misc", "CUT"},
    {"Dist Comb 2 Band", "Misc", "CUT"},   {"Screamer 3 Low", "Misc", "TONE"},    {"Screamer 3 Band", "Misc", "TONE"},
    {"Whisper", "Formant", "VOWEL"},       {"DJ Mixer", "Misc", "LOW/HIGH"},      {"Diffuser", "Misc", "SPREAD"},
    {"Ladder", "Analog", "BASS"},          {"Ladder Acid", "Analog", "BASS"},     {"Ladder Bright", "Analog", "BASS"},
    {"Dirty Ladder", "Analog", "BODY"},    {"Pole-Zero", "Misc", "SHAPE"},        {"Comb 2", "Flanges", "CUT"},
    {"Exp Low", "Normal", "SLOPE"},        {"Exp Band", "Normal", "SLOPE"},
};
constexpr int kFilterCount = static_cast<int>(sizeof(kFilterDefs) / sizeof(kFilterDefs[0]));
}  // namespace

const std::vector<std::string>& filterNames() {
  static const std::vector<std::string> v = [] {
    std::vector<std::string> n;
    for (const auto& f : kFilterDefs) n.emplace_back(f.name);
    return n;
  }();
  return v;
}
const std::vector<std::string>& filterGroups() {
  static const std::vector<std::string> v = [] {
    std::vector<std::string> n;
    for (const auto& f : kFilterDefs) n.emplace_back(f.group);
    return n;
  }();
  return v;
}
const char* filterVarLabel(int type) { return type >= 0 && type < kFilterCount ? kFilterDefs[type].var : "VAR"; }
int filterVarKind(int type) { return type; }

// ------------------------------------------------------------------ matrix sources
const std::vector<std::string>& sourceNames() {
  static const std::vector<std::string> v = [] {
    std::vector<std::string> n(SourceCount);
    n[SrcNone] = "—";
    for (int i = 0; i < kEnvs; ++i) n[static_cast<size_t>(SrcEnv1 + i)] = "Env " + std::to_string(i + 1);
    for (int i = 0; i < kLfos; ++i) {
      n[static_cast<size_t>(SrcLfo1 + i)] = "LFO " + std::to_string(i + 1);
      n[static_cast<size_t>(SrcLfo1Y + i)] = "LFO " + std::to_string(i + 1) + " Y";
    }
    n[SrcVelo] = "Velocity";
    n[SrcNote] = "Note #";
    n[SrcActiveVoices] = "Active Voices";
    n[SrcAlt1] = "Note-On Alternate";
    n[SrcAlt2] = "Note-On Alternate 2";
    n[SrcRandDiscrete] = "Note-On Random (Each)";
    n[SrcRand1] = "Note-On Random 1";
    n[SrcRand2] = "Note-On Random 2";
    n[SrcReleaseVelo] = "Release Velocity";
    n[SrcVoiceIndex] = "Voice Index";
    n[SrcVoiceMod1] = "Voice Mod 1";
    n[SrcVoiceMod2] = "Voice Mod 2";
    n[SrcNoise] = "Noise Osc";
    n[SrcOscA] = "Osc A";
    n[SrcOscB] = "Osc B";
    n[SrcOscC] = "Osc C";
    n[SrcSub] = "Sub Osc";
    for (int i = 0; i < kMacros; ++i) n[static_cast<size_t>(SrcMacro1 + i)] = "Macro " + std::to_string(i + 1);
    n[SrcExprX] = "Expression X";
    n[SrcExprY] = "Expression Y";
    n[SrcExprZ] = "Expression Z";
    n[SrcFilter1] = "Filter 1";
    n[SrcFilter2] = "Filter 2";
    n[SrcModWheel] = "Mod Wheel";
    n[SrcAftertouch] = "Aftertouch";
    n[SrcPolyAT] = "Poly Aftertouch";
    n[SrcPitchBend] = "Pitch Bend";
    n[SrcFixed] = "Constant";
    return n;
  }();
  return v;
}
const char* sourceGroup(int s) {
  if (s >= SrcEnv1 && s <= SrcEnv4) return "Envelopes";
  if (s >= SrcLfo1 && s <= SrcLfo10Y) return "LFOs";
  if (s >= SrcVelo && s <= SrcVoiceMod2) return "Note";
  if (s >= SrcNoise && s <= SrcSub) return "Oscillators";
  if (s >= SrcMacro1 && s <= SrcMacro8) return "Macros";
  if (s >= SrcExprX && s <= SrcExprZ) return "Expression";
  if (s == SrcFilter1 || s == SrcFilter2) return "Filters";
  return "";
}

const std::vector<std::string>& fxTypeNames() {
  static const std::vector<std::string> v = {"Empty",    "Distortion", "Flanger",    "Phaser",       "Chorus",      "Delay",
                                             "Compressor", "Reverb",   "Equalizer",  "Filter",       "Hyper/Dimension",
                                             "Frequency Shifter", "Convolve", "Utility", "Split Low/High",
                                             "Split Low/Mid/High", "Split Mid/Side"};
  return v;
}

const std::vector<std::string>& lfoShapeNames() {
  static const std::vector<std::string> v = {"Custom",      "Triangle",   "Sine",       "Ramp Up",    "Ramp Down",  "Square",
                                             "Pulse 25",    "Exp Down",   "Exp Up",     "Bounce",     "Steps 4",    "Steps 8",
                                             "Stairs Up",   "Half Sine",  "Pump",       "Pump Soft",  "Gate 1/2",   "Gate Dotted",
                                             "Trance Gate", "Wobble",     "Shark Fin",  "Double Hump", "Random Curved", "Random Flat"};
  return v;
}

const std::vector<std::string>& noiseNames() {
  static const std::vector<std::string> v = {"User File", "White",     "Pink",       "Brown",     "Crackle",   "Geiger",
                                             "Vinyl",     "Hiss Air",  "Rumble",     "Digital Grit", "Bit Rain", "Breath",
                                             "Wind",      "Metal Ring", "Tape Hum",  "Electric Hum", "Rain",     "Click Train",
                                             "Bandpass Swirl", "Shaker Loop"};
  return v;
}

// ------------------------------------------------------------------ scales
namespace {
struct ScaleDef {
  const char* name;
  std::vector<int> steps;
};
const std::vector<ScaleDef>& scales() {
  static const std::vector<ScaleDef> v = {
      {"Major", {0, 2, 4, 5, 7, 9, 11}},          {"Dorian", {0, 2, 3, 5, 7, 9, 10}},
      {"Phrygian", {0, 1, 3, 5, 7, 8, 10}},       {"Lydian", {0, 2, 4, 6, 7, 9, 11}},
      {"Mixolydian", {0, 2, 4, 5, 7, 9, 10}},     {"Minor", {0, 2, 3, 5, 7, 8, 10}},
      {"Locrian", {0, 1, 3, 5, 6, 8, 10}},        {"Harmonic Minor", {0, 2, 3, 5, 7, 8, 11}},
      {"Melodic Minor", {0, 2, 3, 5, 7, 9, 11}},  {"Romanian Minor", {0, 2, 3, 6, 7, 9, 10}},
      {"Hungarian Minor", {0, 2, 3, 6, 7, 8, 11}}, {"Double Harmonic", {0, 1, 4, 5, 7, 8, 11}},
      {"Phrygian Dominant", {0, 1, 4, 5, 7, 8, 10}}, {"Lydian Dominant", {0, 2, 4, 6, 7, 9, 10}},
      {"Neapolitan Major", {0, 1, 3, 5, 7, 9, 11}}, {"Neapolitan Minor", {0, 1, 3, 5, 7, 8, 11}},
      {"Persian", {0, 1, 4, 5, 6, 8, 11}},        {"Enigmatic", {0, 1, 4, 6, 8, 10, 11}},
      {"Super Locrian", {0, 1, 3, 4, 6, 8, 10}},  {"Major Pentatonic", {0, 2, 4, 7, 9}},
      {"Minor Pentatonic", {0, 3, 5, 7, 10}},     {"Major Blues", {0, 2, 3, 4, 7, 9}},
      {"Minor Blues", {0, 3, 5, 6, 7, 10}},       {"Egyptian", {0, 2, 5, 7, 10}},
      {"Hirajoshi", {0, 2, 3, 7, 8}},             {"In Sen", {0, 1, 5, 7, 10}},
      {"Iwato", {0, 1, 5, 6, 10}},                {"Kumoi", {0, 2, 3, 7, 9}},
      {"Pelog", {0, 1, 3, 7, 8}},                 {"Prometheus", {0, 2, 4, 6, 9, 10}},
      {"Whole Tone", {0, 2, 4, 6, 8, 10}},        {"Augmented", {0, 3, 4, 7, 8, 11}},
      {"Hexatonic Blues", {0, 3, 5, 6, 7, 10}},   {"Diminished (Whole-Half)", {0, 2, 3, 5, 6, 8, 9, 11}},
      {"Diminished (Half-Whole)", {0, 1, 3, 4, 6, 7, 9, 10}}, {"Bebop Major", {0, 2, 4, 5, 7, 8, 9, 11}},
      {"Bebop Minor", {0, 2, 3, 4, 5, 7, 9, 10}}, {"Bebop Dominant", {0, 2, 4, 5, 7, 9, 10, 11}},
      {"Major + b7", {0, 2, 4, 5, 7, 9, 10, 11}}, {"Nine Tone", {0, 2, 3, 4, 6, 7, 8, 9, 11}},
      {"Spanish 8 Tone", {0, 1, 3, 4, 5, 6, 8, 10}}, {"Arabic", {0, 2, 4, 5, 6, 8, 10}},
      {"Klezmer", {0, 1, 4, 5, 7, 8, 10}},        {"Gypsy", {0, 2, 3, 6, 7, 8, 10}},
      {"Algerian", {0, 2, 3, 6, 7, 8, 11}},       {"Balinese", {0, 1, 3, 7, 8}},
      {"Chinese", {0, 4, 6, 7, 11}},              {"Mongolian", {0, 2, 4, 7, 9}},
      {"Raga Bhairav", {0, 1, 4, 5, 7, 8, 11}},   {"Raga Todi", {0, 1, 3, 6, 7, 8, 11}},
      {"Raga Purvi", {0, 1, 4, 6, 7, 8, 11}},     {"Raga Marwa", {0, 1, 4, 6, 7, 9, 11}},
      {"Major Triad", {0, 4, 7}},                 {"Minor Triad", {0, 3, 7}},
      {"Fifths", {0, 7}},                         {"Octaves", {0}},
      {"Chromatic", {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}},
  };
  return v;
}
}  // namespace
const std::vector<std::string>& scaleNames() {
  static const std::vector<std::string> v = [] {
    std::vector<std::string> n;
    for (const auto& s : scales()) n.emplace_back(s.name);
    return n;
  }();
  return v;
}
const std::vector<int>& scaleSteps(int scale) {
  const auto& s = scales();
  return s[static_cast<size_t>(std::clamp(scale, 0, static_cast<int>(s.size()) - 1))].steps;
}

// ------------------------------------------------------------------ divisions
namespace {
struct Div {
  const char* name;
  double beats;
};
const Div kDivs[] = {{"8 bars", 32},  {"4 bars", 16},  {"2 bars", 8},     {"1 bar", 4},     {"1/2", 2},       {"1/4", 1},
                     {"1/8", 0.5},    {"1/16", 0.25},  {"1/32", 0.125},   {"1/64", 0.0625}, {"1/4 T", 2.0 / 3}, {"1/8 T", 1.0 / 3},
                     {"1/16 T", 1.0 / 6}, {"1/4 D", 1.5}, {"1/8 D", 0.75}, {"1/16 D", 0.375}};
constexpr int kDivCount = static_cast<int>(sizeof(kDivs) / sizeof(kDivs[0]));
const Div kLfoDivs[] = {{"32 bars", 128}, {"16 bars", 64}, {"8 bars", 32},   {"4 bars", 16},    {"2 bars", 8},
                        {"1 bar", 4},     {"1/2", 2},      {"1/4", 1},       {"1/8", 0.5},      {"1/16", 0.25},
                        {"1/32", 0.125},  {"1/64", 0.0625}, {"1/128", 0.03125}, {"1/256", 0.015625}, {"fast", 0.0078125}};
constexpr int kLfoDivCount = static_cast<int>(sizeof(kLfoDivs) / sizeof(kLfoDivs[0]));
}  // namespace
const std::vector<std::string>& divisionNames() {
  static const std::vector<std::string> v = [] {
    std::vector<std::string> n;
    for (const auto& d : kDivs) n.emplace_back(d.name);
    return n;
  }();
  return v;
}
double divisionBeats(int div) { return kDivs[std::clamp(div, 0, kDivCount - 1)].beats; }
int lfoRateDivisionCount() { return kLfoDivCount; }
const char* lfoRateDivisionName(int i) { return kLfoDivs[std::clamp(i, 0, kLfoDivCount - 1)].name; }
double lfoRateDivisionBeats(int i) { return kLfoDivs[std::clamp(i, 0, kLfoDivCount - 1)].beats; }

const std::vector<std::string>& arpShapeNames() {
  static const std::vector<std::string> v = {"Pattern",   "Up",        "Down",      "Up/Down",   "Down/Up",    "Up & Down",
                                             "Down & Up", "Thumb Up",  "Thumb Up/Down", "Pinky Up", "Pinky Up/Down", "Converge",
                                             "Diverge",   "Converge & Diverge", "As Played", "Chord", "Random", "Random No Repeat",
                                             "Random Drift", "Random Once"};
  return v;
}

// ------------------------------------------------------------------ effect parameters
namespace {
std::vector<std::string> distModes() {
  return {"Tube",       "Soft Clip", "Hard Clip",  "Diode 1",      "Diode 2",   "Linear Fold", "Sine Fold",
          "Zero-Square", "Downsample", "Asymmetric", "Rectify",   "X-Shaper",  "X-Shaper Asym", "Sine Shaper",
          "Stomp Box",  "Tape Sat",  "Overdrive",  "Soft Sat"};
}
using D = FxParamDef;
const std::vector<std::vector<FxParamDef>>& fxTable() {
  static const std::vector<std::vector<FxParamDef>> t = [] {
    std::vector<std::vector<FxParamDef>> v(FxTypeCount);
    const D wet{"kParamWet", "MIX", 0, 100, 100, 0, 0, "%", {}};
    const D lvl{"kParamLevelOut", "LEVEL", 0, 1, 0.5, 0, 2, "dB", {}};
    const D sync{"kParamBeatSync", "BPM", 0, 1, 0, 1, 0, "", {}};
    v[FxDistortion] = {wet,
                       {"kParamDrive", "DRIVE", 0, 100, 25, 0, 0, "%", {}},
                       {"kParamLPHP", "TYPE", 0, 100, 50, 0, 0, "%", {}},
                       {"kParamMode", "MODE", 0, 17, 0, 2, 0, "", distModes()},
                       {"kParamFreq", "FREQ", 0, 1, 0.5, 0, 4, "Hz", {}},
                       {"kParamBW", "Q", 0.075, 7.575, 1.9, 0, 0, "", {}},
                       {"kParamPrePost", "FILTER", 0, 2, 0, 2, 0, "", {"Off", "Pre", "Post"}},
                       lvl,
                       {"kParamNumStages", "STAGES", 1, 16, 4, 0, 3, "", {}}};
    v[FxFlanger] = {wet,
                    sync,
                    {"kParamRate", "RATE", 0, 20, 0.15, 0, 5, "Hz", {}},
                    {"kParamDepth", "DEPTH", 0, 100, 50, 0, 0, "%", {}},
                    {"kParamFeedback", "FEEDBACK", 0, 100, 50, 0, 0, "%", {}},
                    {"kParamWidth", "PHASE", 0, 360, 180, 0, 0, "°", {}},
                    lvl};
    v[FxPhaser] = {wet,
                   sync,
                   {"kParamRate", "RATE", 0, 20, 0.3, 0, 5, "Hz", {}},
                   {"kParamDepth", "DEPTH", 0, 100, 50, 0, 0, "%", {}},
                   {"kParamDepth2", "DEPTH 2", 0, 1, 0, 0, 0, "%", {}},
                   {"kParamFreq", "FREQ", 20, 18000, 800, 0, 1, "Hz", {}},
                   {"kParamFeedback", "FEEDBACK", 0, 100, 50, 0, 0, "%", {}},
                   {"kParamWidth", "PHASE", 0, 360, 180, 0, 0, "°", {}},
                   lvl,
                   {"kParamNumPoles", "POLES", 1, 18, 4, 0, 3, "", {}}};
    v[FxChorus] = {wet,
                   sync,
                   {"kParamRate", "RATE", 0, 20, 0.2, 0, 5, "Hz", {}},
                   {"kParamDelay", "DELAY 1", 0, 20, 7, 0, 0, "ms", {}},
                   {"kParamDelay2", "DELAY 2", 0, 20, 10, 0, 0, "ms", {}},
                   {"kParamDepth", "DEPTH", 0, 30, 5, 0, 0, "ms", {}},
                   {"kParamFeedback", "FEEDBACK", 0, 100, 0, 0, 0, "%", {}},
                   {"kParamFilt", "FILTER", 50, 20000, 20000, 0, 1, "Hz", {}},
                   lvl,
                   {"kParamFiltMode", "FILTER TYPE", 0, 1, 0, 2, 0, "", {"Low-pass", "High-pass"}}};
    v[FxDelay] = {{"kParamWet", "MIX", 0, 100, 30, 0, 0, "%", {}},
                  {"kParamFreq", "FREQ", 40, 18000, 2000, 0, 1, "Hz", {}},
                  {"kParamBW", "Q", 0.75, 8.25, 4, 0, 0, "", {}},
                  {"kParamBeatSync", "BPM", 0, 1, 1, 1, 0, "", {}},
                  {"kParamLink", "LINK", 0, 1, 0, 1, 0, "", {}},
                  {"kParamTimeL", "TIME L", 0, 1, 0.25, 0, 6, "", {}},
                  {"kParamTimeR", "TIME R", 0, 1, 0.25, 0, 6, "", {}},
                  {"kParamMode", "MODE", 0, 2, 0, 2, 0, "", {"Normal", "Ping-Pong", "Tap>Delay"}},
                  {"kParamFeedback", "FEEDBACK", 0, 100, 40, 0, 0, "%", {}},
                  {"kParamOffsetL", "MULT L", 0.5, 1.5, 1, 0, 0, "x", {}},
                  {"kParamOffsetR", "MULT R", 0.5, 1.5, 1, 0, 0, "x", {}},
                  lvl,
                  {"kParamHQ", "HQ", 0, 1, 1, 1, 0, "", {}}};
    v[FxComp] = {wet,
                 {"kParamThresh", "THRESH", 0, 1, 0.15, 0, 0, "thr", {}},
                 {"kParamRatio", "RATIO", 1, 1000000, 4, 0, 1, ":1", {}},
                 {"kParamAttack", "ATTACK", 0.1, 1000, 90, 0, 1, "ms", {}},
                 {"kParamRelease", "RELEASE", 0.1, 1000, 90, 0, 1, "ms", {}},
                 {"kParamMakeup", "GAIN", 1, 31.6228, 1, 0, 1, "x", {}},
                 {"kParamMultiband", "MULTIBAND", 0, 1, 0, 1, 0, "", {}},
                 {"kParamThreshUD0", "THRESH L", 0, 200, 100, 0, 0, "%", {}},
                 {"kParamThreshUD1", "THRESH M", 0, 200, 100, 0, 0, "%", {}},
                 {"kParamThreshUD2", "THRESH H", 0, 200, 100, 0, 0, "%", {}},
                 lvl,
                 {"kParamXoverLow", "X-LOW", 40, 5000, 120, 0, 1, "Hz", {}},
                 {"kParamXoverHi", "X-HIGH", 300, 16000, 2500, 0, 1, "Hz", {}},
                 {"kParamGain0", "L", -24, 24, 0, 0, 0, "dB", {}},
                 {"kParamGain1", "M", -24, 24, 0, 0, 0, "dB", {}},
                 {"kParamGain2", "H", -24, 24, 0, 0, 0, "dB", {}},
                 {"kParamRatioBelow", "BELOW", 0, 1, 0.5, 0, 0, "%", {}},
                 {"kParamRatio0", "RATIO L", 0, 1, 0.5, 0, 0, "%", {}},
                 {"kParamRatio1", "RATIO M", 0, 1, 0.5, 0, 0, "%", {}},
                 {"kParamRatio2", "RATIO H", 0, 1, 0.5, 0, 0, "%", {}},
                 {"kParamRatioBelow0", "BELOW L", 0, 1, 0.5, 0, 0, "%", {}},
                 {"kParamRatioBelow1", "BELOW M", 0, 1, 0.5, 0, 0, "%", {}},
                 {"kParamRatioBelow2", "BELOW H", 0, 1, 0.5, 0, 0, "%", {}},
                 {"kParamDeadband0", "DEADBAND L", 0, 24, 7, 0, 0, "dB", {}},
                 {"kParamDeadband1", "DEADBAND M", 0, 24, 11.6, 0, 0, "dB", {}},
                 {"kParamDeadband2", "DEADBAND H", 0, 24, 5.3, 0, 0, "dB", {}},
                 {"kParamLimiterLatencyComp", "LATENCY COMP", 0, 1, 0, 1, 0, "", {}}};
    v[FxReverb] = {{"kParamWet", "MIX", 0, 100, 25, 0, 0, "%", {}},
                   {"kParamSize", "SIZE", 0, 100, 50, 0, 0, "%", {}},
                   {"kParamDelay", "PRE-DLY", 0, 250, 0, 0, 0, "ms", {}},
                   {"kParamFreq", "LO CUT", 0, 100, 0, 0, 0, "%", {}},
                   {"kParamFeedback", "DECAY", 0, 100, 50, 0, 0, "%", {}},
                   {"kParamFreqB", "HI CUT", 0, 100, 35, 0, 0, "%", {}},
                   {"kParamWidth", "WIDTH", 0, 100, 100, 0, 0, "%", {}},
                   {"kParamFreqC", "DAMP", 0, 100, 50, 0, 0, "%", {}},
                   {"kParamMode", "SHAPE", 0, 100, 0, 0, 0, "%", {}},
                   lvl,
                   {"kParamVintageScale", "DIFF A", 0, 100, 50, 0, 0, "%", {}},
                   {"kParamVintageScaleB", "DIFF B", 0, 100, 50, 0, 0, "%", {}},
                   {"kParamPreDelay", "PRE SYNC", 0, 2.5, 0, 0, 0, "beats", {}},
                   {"kParamPreDelayBeatSync", "BPM", 0, 1, 0, 1, 0, "", {}},
                   {"kParamType", "TYPE", 0, 4, 0, 2, 0, "", {"Plate", "Hall", "Vintage", "Abyss", "Space"}},
                   {"kParamSpinRate", "MOD RATE", 0, 100, 30, 0, 0, "%", {}},
                   {"kParamSpinDepth", "MOD DEPTH", 0, 100, 20, 0, 0, "%", {}}};
    v[FxEq] = {{"kParamFreq1", "FREQ", 20, 20000, 210, 0, 1, "Hz", {}},
               {"kParamFreq2", "FREQ", 20, 20000, 2041, 0, 1, "Hz", {}},
               {"kParamReso1", "Q", 0, 100, 60, 0, 0, "%", {}},
               {"kParamReso2", "Q", 0, 100, 60, 0, 0, "%", {}},
               {"kParamGain1", "GAIN", -24, 24, 0, 0, 0, "dB", {}},
               {"kParamGain2", "GAIN", -24, 24, 0, 0, 0, "dB", {}},
               {"kParamType1", "LOW TYPE", 0, 2, 0, 2, 0, "", {"Shelf", "Peak", "High-pass"}},
               {"kParamType2", "HIGH TYPE", 0, 2, 0, 2, 0, "", {"Shelf", "Peak", "Low-pass"}},
               lvl};
    v[FxFilter] = {wet,
                   {"kParamType", "TYPE", 0, 105, 0, 2, 0, "", filterNames()},
                   {"kParamFreq", "CUTOFF", 0, 1, 0.5, 0, 4, "Hz", {}},
                   {"kParamReso", "RES", 0, 100, 10, 0, 0, "%", {}},
                   {"kParamDrive", "DRIVE", 0, 100, 0, 0, 0, "%", {}},
                   {"kParamVar", "VAR", 0, 100, 0, 0, 0, "%", {}},
                   {"kParamStereo", "PAN", 0, 100, 50, 0, 0, "%", {}},
                   lvl,
                   {"kParamX", "X", 0, 1, 0, 0, 0, "", {}},
                   {"kParamY", "Y", 0, 1, 0, 0, 0, "", {}},
                   {"kParamKeyTrack", "KEY TRACK", 0, 1, 0, 1, 0, "", {}},
                   {"kParamPad", "CLEAN", 0, 1, 0, 1, 0, "", {}}};
    v[FxHyper] = {{"kParamWet", "MIX", 0, 100, 50, 0, 0, "%", {}},
                  {"kParamRate", "RATE", 0, 100, 40, 0, 0, "%", {}},
                  {"kParamDetune", "DETUNE", 0, 100, 25, 0, 0, "%", {}},
                  {"kParamUnison", "UNISON", 0, 7, 4, 0, 3, "", {}},
                  {"kParamRetrig", "RETRIG", 0, 1, 0, 1, 0, "", {}},
                  lvl,
                  {"kParamDimESize", "SIZE", 0, 100, 50, 0, 0, "%", {}},
                  {"kParamDimEWet", "DIM MIX", 0, 100, 0, 0, 0, "%", {}},
                  {"kParamDimELevelOut", "DIM LEVEL", 0, 1, 0.5, 0, 2, "dB", {}}};
    v[FxBode] = {{"kParamWet", "MIX", 0, 100, 50, 0, 0, "%", {}},
                 sync,
                 {"kParamShift", "SHIFT", -100, 100, 0, 0, 0, "%", {}},
                 {"kParamRange", "RANGE", 0.1, 5000, 100, 0, 1, "Hz", {}},
                 lvl,
                 {"kParamDelayTime", "DELAY", 0, 2, 0, 0, 0, "s", {}},
                 {"kParamFeedback", "FEED", 0, 100, 0, 0, 0, "%", {}},
                 {"kParamDelayBalance", "BALANCE", -100, 100, 0, 0, 0, "%", {}},
                 {"kParamSwapAB", "SWAP", 0, 1, 0, 1, 0, "", {}},
                 {"kParamOutputMix", "DIR", -100, 100, 100, 0, 0, "%", {}},
                 {"kParamOutputWidth", "WIDTH", 0, 100, 0, 0, 0, "%", {}},
                 {"kParamBlur", "BLUR", 0, 100, 0, 0, 0, "%", {}},
                 {"kParamRetrig", "RETRIG", 0, 1, 0, 1, 0, "", {}},
                 {"kParamMonoInput", "MONO INPUT", 0, 1, 0, 1, 0, "", {}}};
    v[FxConvolve] = {{"kParamWet", "MIX", 0, 100, 50, 0, 0, "%", {}},
                     {"kParamMinPhase", "MIN PHASE", 0, 1, 0, 1, 0, "", {}},
                     lvl,
                     {"kParamSize", "SIZE", 10, 1000, 100, 0, 1, "%", {}},
                     {"kParamAttack", "ATTACK", 0, 1, 0, 0, 0, "s", {}},
                     {"kParamDecay", "DECAY", 0, 40, 10, 0, 0, "s", {}},
                     {"kParamIpTrim", "IR GAIN", -34, 6, 0, 0, 0, "dB", {}},
                     {"kParamTone", "TONE", -100, 100, 0, 0, 0, "%", {}},
                     {"kParamPredelay", "PRE-DLY", 0, 1, 0, 0, 0, "s", {}},
                     {"kParamPredelayBeatSync", "BPM", 0, 1, 0, 1, 0, "", {}},
                     {"kParamDamping", "DAMP", 0, 100, 0, 0, 0, "%", {}},
                     {"kParamIr", "IMPULSE", 0, 11, 0, 2, 0, "",
                      {"File", "Small Room", "Medium Room", "Large Hall", "Plate", "Spring", "Cabinet 1x12",
                       "Cabinet 4x12", "Telephone", "Lo-Fi Radio", "Reverse Swell", "Cathedral"}}};
    v[FxUtility] = {{"kParamWet", "MIX", 0, 100, 100, 0, 0, "%", {}},
                    lvl,
                    {"kParamWidth", "WIDTH", 0, 800, 100, 0, 0, "%", {}},
                    {"kParamBalance", "PAN", -100, 100, 0, 0, 0, "%", {}},
                    {"kParamLFMono", "MONO BASS", 0, 1, 0, 1, 0, "", {}},
                    {"kParamLFXover", "FREQ", 20, 400, 120, 0, 1, "Hz", {}},
                    {"kParamHPF", "HPF", 1, 400, 1, 0, 1, "Hz", {}},
                    {"kParamLPF", "LPF", 50, 20000, 20000, 0, 1, "Hz", {}},
                    {"kParamPolarityL", "INVERT L", 0, 1, 0, 1, 0, "", {}},
                    {"kParamPolarityR", "INVERT R", 0, 1, 0, 1, 0, "", {}}};
    v[FxSplitLH] = {{"kParamFreq", "SPLIT", 20, 20000, 300, 0, 1, "Hz", {}},
                    {"kParamBand1Bypass", "LOW OFF", 0, 1, 0, 1, 0, "", {}},
                    {"kParamBand2Bypass", "HIGH OFF", 0, 1, 0, 1, 0, "", {}},
                    lvl};
    v[FxSplitLMH] = {{"kParamFreq", "SPLIT 1", 20, 20000, 200, 0, 1, "Hz", {}},
                     {"kParamFreq2", "SPLIT 2", 20, 20000, 2000, 0, 1, "Hz", {}},
                     {"kParamBand1Bypass", "LOW OFF", 0, 1, 0, 1, 0, "", {}},
                     {"kParamBand2Bypass", "MID OFF", 0, 1, 0, 1, 0, "", {}},
                     {"kParamBand3Bypass", "HIGH OFF", 0, 1, 0, 1, 0, "", {}},
                     lvl};
    v[FxSplitMS] = {{"kParamBand1Bypass", "MID OFF", 0, 1, 0, 1, 0, "", {}},
                    {"kParamBand2Bypass", "SIDE OFF", 0, 1, 0, 1, 0, "", {}},
                    lvl};
    return v;
  }();
  return t;
}
}  // namespace

const std::vector<FxParamDef>& fxParams(int type) {
  const auto& t = fxTable();
  return t[static_cast<size_t>(std::clamp(type, 0, FxTypeCount - 1))];
}
int fxParamIndex(int type, const char* key) {
  const auto& d = fxParams(type);
  for (size_t i = 0; i < d.size(); ++i)
    if (std::strcmp(d[i].key, key) == 0) return static_cast<int>(i);
  return -1;
}
double fxPlain(int type, int p, double norm) {
  const auto& d = fxParams(type);
  if (p < 0 || p >= static_cast<int>(d.size())) return 0;
  const FxParamDef& f = d[static_cast<size_t>(p)];
  norm = std::clamp(norm, 0.0, 1.0);
  if (f.kind == 1) return norm >= 0.5 ? 1 : 0;
  if (f.kind == 2) return std::round(norm * static_cast<double>(f.choices.size() - 1));
  if (f.curve == 1 && f.min > 0) return f.min * std::pow(f.max / f.min, norm);
  if (f.curve == 3) return std::round(f.min + norm * (f.max - f.min));
  if (f.curve == 5) return f.min + norm * norm * (f.max - f.min);  // rate knobs: finer at the bottom
  return f.min + norm * (f.max - f.min);
}
double fxNorm(int type, int p, double plain) {
  const auto& d = fxParams(type);
  if (p < 0 || p >= static_cast<int>(d.size())) return 0;
  const FxParamDef& f = d[static_cast<size_t>(p)];
  if (f.kind == 1) return plain >= 0.5 ? 1 : 0;
  if (f.kind == 2) {
    const double n = static_cast<double>(f.choices.size() - 1);
    return n > 0 ? std::clamp(plain, 0.0, n) / n : 0;
  }
  if (f.max <= f.min) return 0;
  plain = std::clamp(plain, f.min, f.max);
  if (f.curve == 1 && f.min > 0) return std::log(plain / f.min) / std::log(f.max / f.min);
  if (f.curve == 5) return std::sqrt((plain - f.min) / (f.max - f.min));
  return (plain - f.min) / (f.max - f.min);
}
float fxDefaultNorm(int type, int p) {
  const auto& d = fxParams(type);
  if (p < 0 || p >= static_cast<int>(d.size())) return 0;
  return static_cast<float>(fxNorm(type, p, d[static_cast<size_t>(p)].def));
}
std::string fxParamText(int type, int p, double norm) {
  const auto& d = fxParams(type);
  if (p < 0 || p >= static_cast<int>(d.size())) return "";
  const FxParamDef& f = d[static_cast<size_t>(p)];
  const double v = fxPlain(type, p, norm);
  char b[48];
  if (f.kind == 1) return v >= 0.5 ? "On" : "Off";
  if (f.kind == 2) {
    const size_t i = static_cast<size_t>(v);
    return i < f.choices.size() ? f.choices[i] : "";
  }
  const std::string u = f.unit;
  if (f.curve == 2) {
    const double g = busGain(v);
    if (g <= 1e-6) return "-inf dB";
    std::snprintf(b, sizeof b, "%.1f dB", 20.0 * std::log10(g));
  } else if (f.curve == 4) {
    const double hz = cutoffHz(v);
    if (hz >= 1000)
      std::snprintf(b, sizeof b, "%.2f kHz", hz / 1000);
    else
      std::snprintf(b, sizeof b, "%.0f Hz", hz);
  } else if (f.curve == 6) {
    return "";  // delay time: the editor formats it (tempo / ms)
  } else if (u == "Hz") {
    if (v >= 1000)
      std::snprintf(b, sizeof b, "%.2f kHz", v / 1000);
    else if (v < 10)
      std::snprintf(b, sizeof b, "%.2f Hz", v);
    else
      std::snprintf(b, sizeof b, "%.0f Hz", v);
  } else if (u == "ms") {
    std::snprintf(b, sizeof b, v < 10 ? "%.2f ms" : "%.1f ms", v);
  } else if (u == "s") {
    if (v < 1)
      std::snprintf(b, sizeof b, "%.0f ms", v * 1000);
    else
      std::snprintf(b, sizeof b, "%.2f s", v);
  } else if (u == "dB") {
    std::snprintf(b, sizeof b, "%.1f dB", v);
  } else if (u == "thr") {  // compressor threshold: 0 = 0 dB .. 1 = -60 dB
    std::snprintf(b, sizeof b, "%.1f dB", -60.0 * v);
  } else if (u == ":1") {
    if (v >= 999999)
      return "Limit";
    std::snprintf(b, sizeof b, "%.1f:1", v);
  } else if (u == "%") {
    std::snprintf(b, sizeof b, "%.0f%%", f.max <= 1.0 ? v * 100 : v);
  } else if (u == "x") {
    if (f.curve == 1)
      std::snprintf(b, sizeof b, "%.1f dB", 20 * std::log10(std::max(1e-6, v)));
    else
      std::snprintf(b, sizeof b, "%.3fx", v);
  } else if (f.curve == 3) {
    std::snprintf(b, sizeof b, "%.0f", v);
  } else {
    std::snprintf(b, sizeof b, "%.2f%s", v, f.unit);
  }
  return b;
}

// ------------------------------------------------------------------ parameter names / text
namespace {
const char* const kRouteNames[kRoutes] = {"A", "B", "C", "Noise", "Sub", "Filter 1", "Filter 2"};
const char* const kRetrigNames[kRetrigs] = {"Note", "Osc A", "Osc B", "Osc C", "Sub", "Noise", "Env 2", "Env 3", "Env 4", "LFO 1",
                                            "LFO 2", "LFO 3", "LFO 4", "LFO 5", "LFO 6", "LFO 7", "LFO 8", "LFO 9", "LFO 10"};
std::string fmtDb(double g) {
  if (g <= 1e-6) return "-inf dB";
  char b[32];
  std::snprintf(b, sizeof b, "%.1f dB", 20.0 * std::log10(g));
  return b;
}
std::string fmtTime(double s) {
  char b[32];
  if (s < 0.001)
    std::snprintf(b, sizeof b, "%.1f ms", s * 1000);
  else if (s < 1)
    std::snprintf(b, sizeof b, "%.0f ms", s * 1000);
  else
    std::snprintf(b, sizeof b, "%.2f s", s);
  return b;
}
std::string fmtHz(double hz) {
  char b[32];
  if (hz >= 1000)
    std::snprintf(b, sizeof b, "%.2f kHz", hz / 1000);
  else if (hz < 10)
    std::snprintf(b, sizeof b, "%.2f Hz", hz);
  else
    std::snprintf(b, sizeof b, "%.0f Hz", hz);
  return b;
}
const PluginInfo* selfInfo() {
  static const PluginInfo* p = findPlugin(kType);
  return p;
}
}  // namespace

std::string paramLongName(int k) {
  const PluginInfo* info = selfInfo();
  if (!info || k < 0 || k >= static_cast<int>(info->params.size())) return "";
  return info->params[static_cast<size_t>(k)].name;
}
std::string paramShortName(int k) {
  std::string n = paramLongName(k);
  // drop the module prefix ("Osc A ", "Filter 1 ", "Env 2 ", "LFO 10 ", "Mod 12 ", "FX 3 ")
  std::string prefix;
  if (k >= kOscBase && k < kSubBase) prefix = std::string(kOscLetter[(k - kOscBase) / O_Count]) + " ";
  else if (k >= kSubBase && k < kNoiseBase) prefix = "Sub ";
  else if (k >= kNoiseBase && k < kFiltBase) prefix = "Noise ";
  else if (k >= kRouteBase && k < kEnvBase) prefix = std::string(kRouteNames[(k - kRouteBase) / R_Count]) + " ";
  else if (k >= kModBase && k < kFxBase) prefix = "Mod " + std::to_string((k - kModBase) / M_Count + 1) + " ";
  else if (k >= kFxBase && k < kTailBase) prefix = "FX " + std::to_string((k - kFxBase) / X_Count + 1) + " ";
  else if (k >= kFiltBase && k < kRouteBase) prefix = "Filter " + std::to_string((k - kFiltBase) / F_Count + 1) + " ";
  else if (k >= kEnvBase && k < kLfoBase) prefix = "Env " + std::to_string((k - kEnvBase) / E_Count + 1) + " ";
  else if (k >= kLfoBase && k < kMacroBase) prefix = "LFO " + std::to_string((k - kLfoBase) / L_Count + 1) + " ";
  if (!prefix.empty() && n.rfind(prefix, 0) == 0) n = n.substr(prefix.size());
  for (char& c : n) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return n;
}

bool modulatable(int k) {
  const PluginInfo* info = selfInfo();
  if (!info || k < 0 || k >= static_cast<int>(info->params.size())) return false;
  if (k >= kModBase && k < kFxBase) {
    const int p = (k - kModBase) % M_Count;
    return p == M_Amount || p == M_Out;
  }
  if (k >= kFxBase && k < kTailBase) {
    const int p = (k - kFxBase) % X_Count;
    return p >= X_P0;
  }
  const ParamDesc& d = info->params[static_cast<size_t>(k)];
  return d.kind == ParamKind::Continuous;
}

bool integerParam(int k) {
  if (k == G_BendUp || k == G_BendDown || k == G_Poly || k == G_Transpose || k == G_MpeBend || k == G_VcSteps) return true;
  if (k == subP(S_Octave)) return true;
  if (k >= kOscBase && k < kSubBase) {
    const int p = (k - kOscBase) % O_Count;
    return p == O_Octave || p == O_Semi || p == O_Unison || p == O_Slice || p == O_GMaxGrains || p == O_KeyMin || p == O_KeyMax ||
           p == O_VelMin || p == O_VelMax;
  }
  if (k >= kLfoBase && k < kMacroBase) {
    const int p = (k - kLfoBase) % L_Count;
    return p == L_GridX || p == L_GridY;
  }
  return false;
}

std::string paramText(int k, double v, const std::vector<float>* params) {
  char b[64];
  if (integerParam(k)) {
    const int i = static_cast<int>(std::lround(v));
    const bool oct = k == subP(S_Octave) || (k >= kOscBase && k < kSubBase && (k - kOscBase) % O_Count == O_Octave);
    const bool st = k == G_BendUp || k == G_BendDown || k == G_Transpose || k == G_MpeBend ||
                    (k >= kOscBase && k < kSubBase && (k - kOscBase) % O_Count == O_Semi);
    if (i == 0 && (oct || st)) std::snprintf(b, sizeof b, "0%s", st ? " st" : "");
    else if (oct || st) std::snprintf(b, sizeof b, "%+d%s", i, st ? " st" : "");
    else std::snprintf(b, sizeof b, "%d", i);
    return b;
  }
  auto pv = [&](int i, double def) { return params && i < static_cast<int>(params->size()) ? static_cast<double>((*params)[static_cast<size_t>(i)]) : def; };
  if (k == G_MasterVol) return fmtDb(masterGain(v));
  if (k == G_DirectVol || k == G_Bus1Vol || k == G_Bus2Vol) return fmtDb(busGain(v));
  if (k == G_PortaTime) return fmtTime(portaTime(v));
  if (k >= kOscBase && k < kSubBase) {
    const int p = (k - kOscBase) % O_Count;
    switch (p) {
      case O_Level: std::snprintf(b, sizeof b, "%.0f%% [%s]", v * 100, fmtDb(levelGain(v)).c_str()); return b;
      case O_Pan: if (std::fabs(v) < 0.005) return "C"; std::snprintf(b, sizeof b, "%.0f %s", std::fabs(v) * 50, v < 0 ? "L" : "R"); return b;
      case O_UniDetune: std::snprintf(b, sizeof b, "%.2f", v * v); return b;
      case O_Phase: std::snprintf(b, sizeof b, "%.0f°", v * 360); return b;
      case O_WtPos: std::snprintf(b, sizeof b, "%.0f%%", v * 100); return b;
      case O_GDensity: std::snprintf(b, sizeof b, "%.1f /s", grainDensity(v)); return b;
      case O_GLength: std::snprintf(b, sizeof b, "%.0f ms", grainLengthMs(v)); return b;
      case O_MDelay: case O_MAttack: case O_MHold: case O_MDecay: case O_MRelease: return fmtTime(envTime(v));
      case O_MSustain: return fmtDb(levelGain(v));
      default: break;
    }
  }
  if (k == subP(S_Level) || k == noiseP(N_Level)) {
    std::snprintf(b, sizeof b, "%.0f%% [%s]", v * 100, fmtDb(levelGain(v)).c_str());
    return b;
  }
  if (k >= kFiltBase && k < kRouteBase) {
    const int p = (k - kFiltBase) % F_Count;
    if (p == F_Freq) return fmtHz(cutoffHz(v));
    if (p == F_Level) return fmtDb(busGain(v));
  }
  if (k >= kEnvBase && k < kLfoBase) {
    const int e = (k - kEnvBase) / E_Count, p = (k - kEnvBase) % E_Count;
    const bool bpm = pv(envP(e, E_Bpm), 0) >= 0.5;
    if (p == E_Attack || p == E_Hold || p == E_Decay || p == E_Release) {
      const double s = envTime(v);
      if (bpm) {
        std::snprintf(b, sizeof b, "%.3g beats", s * 2.0);  // 1 s at 120 BPM = 2 beats (display only)
        return b;
      }
      return fmtTime(s);
    }
    if (p == E_Sustain) return fmtDb(levelGain(v));
  }
  if (k >= kLfoBase && k < kMacroBase) {
    const int l = (k - kLfoBase) / L_Count, p = (k - kLfoBase) % L_Count;
    if (p == L_Rate) {
      if (pv(lfoP(l, L_Bpm), 1) >= 0.5) return lfoRateDivisionName(static_cast<int>(std::lround(v * (lfoRateDivisionCount() - 1))));
      std::snprintf(b, sizeof b, "%.2f Hz", lfoHz(v, pv(lfoP(l, L_Rate10x), 0) >= 0.5));
      return b;
    }
    if (p == L_Rise || p == L_Delay) {
      const int i = static_cast<int>(std::lround(v * lfoRateDivisionCount()));
      if (i <= 0) return "Off";
      return lfoRateDivisionName(lfoRateDivisionCount() - i);
    }
    if (p == L_Phase) {
      std::snprintf(b, sizeof b, "%.0f°", v * 360);
      return b;
    }
  }
  if (k >= kFxBase && k < kTailBase) {
    const int s = (k - kFxBase) / X_Count, p = (k - kFxBase) % X_Count;
    if (p >= X_P0) {
      const int type = static_cast<int>(pv(fxP(s, X_Type), 0));
      return fxParamText(type, p - X_P0, v);
    }
  }
  const PluginInfo* info = selfInfo();
  if (info && k >= 0 && k < static_cast<int>(info->params.size())) return info->params[static_cast<size_t>(k)].format(v);
  return "";
}

// ------------------------------------------------------------------ catalogue
void appendInfos(std::vector<PluginInfo>& v) {
  PluginInfo p{kType, "Plasma", "Synth", PluginKind::Generator, {}, false, {}};
  p.params.resize(kParamCount);
  auto& P = p.params;
  auto set = [&](int k, ParamDesc d) { P[static_cast<size_t>(k)] = std::move(d); };

  // ---- global
  set(G_MasterVol, lin("Main Volume", "%", 0, 1, 0.5));
  set(G_MasterTune, lin("Main Tuning", "ct", -100, 100, 0));
  set(G_Amp, lin("Amp", "%", 0, 1, 0.5));
  set(G_PortaTime, lin("Porta Time", "%", 0, 1, 0));
  set(G_PortaCurve, lin("Porta Curve", "%", -1, 1, 0));
  set(G_PortaAlways, tog("Porta Always", 0));
  set(G_PortaScaled, tog("Porta Scaled", 0));
  set(G_BendUp, lin("Bend Up", "st", -48, 48, 2));
  set(G_BendDown, lin("Bend Down", "st", -48, 48, -2));
  set(G_PitchWheel, lin("Pitch Wheel", "%", -1, 1, 0));
  set(G_ModWheel, lin("Mod Wheel", "%", 0, 1, 0));
  set(G_Mono, tog("Mono", 0));
  set(G_Legato, tog("Legato", 0));
  set(G_Poly, lin("Poly", "", 1, 32, 8));
  set(G_Priority, ch("Voice Priority", {"Newest", "Oldest", "Highest", "Lowest", "Velocity"}, 0));
  set(G_SameNote, tog("Limit Same Note", 0));
  set(G_Swing, lin("Swing", "%", 0, 1, 0.5));
  set(G_SwingDiv, ch("Swing Division", {"1/4", "1/8", "1/16", "1/32"}, 2));
  set(G_Transpose, lin("Transpose", "st", -48, 48, 0));
  set(G_DirectVol, lin("Direct Volume", "%", 0, 1, 0.5));
  set(G_Bus1Vol, lin("Bus 1 Volume", "%", 0, 1, 0.5));
  set(G_Bus2Vol, lin("Bus 2 Volume", "%", 0, 1, 0.5));
  set(G_Bus1Dest, ch("Bus 1 Output", {"Main", "Direct"}, 0));
  set(G_Bus2Dest, ch("Bus 2 Output", {"Main", "Direct"}, 0));
  set(G_Quality, ch("Quality", {"Draft", "High", "Ultra"}, 1));
  set(G_S1Mode, tog("Legacy Warp Mode", 0));
  set(G_NoSmooth, tog("Disable Smoothing", 0));
  set(G_ConcertA, lin("Concert A", "Hz", 400, 480, 440));
  set(G_EnvScale, lg("Envelope Scaling", "x", 0.1, 10, 1));
  set(G_LfoScale, lg("LFO Scaling", "x", 0.1, 10, 1));
  set(G_LfoScaleRate, tog("LFO Scale Rate", 0));
  set(G_MpeOn, tog("MPE", 0));
  set(G_MpeBend, lin("MPE Bend Range", "st", 1, 96, 48));
  set(G_MpeYBipolar, tog("MPE Y Bipolar", 0));
  {
    std::vector<std::string> keys = {"Off", "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    set(G_Key, ch("Key", keys, 0));
    set(G_Scale, ch("Scale", scaleNames(), 0));
  }
  set(G_RndPan, lin("Random Pan", "%", 0, 1, 0));
  set(G_RndDetune, lin("Random Detune", "ct", 0, 20, 0));
  set(G_RndCutoff, lin("Random Cutoff", "%", 0, 1, 0));
  set(G_RndEnv, lin("Random Env Time", "%", 0, 1, 0));
  set(G_VcS, tog("Voice Ctrl Sub", 1));
  set(G_VcA, tog("Voice Ctrl A", 1));
  set(G_VcB, tog("Voice Ctrl B", 1));
  set(G_VcC, tog("Voice Ctrl C", 1));
  set(G_VcN, tog("Voice Ctrl Noise", 1));
  set(G_VcSteps, lin("Voice Ctrl Steps", "", 1, 8, 8));
  set(G_ArpOn, tog("Arp", 0));
  set(G_ClipOn, tog("Clip Player", 0));
  set(G_MidiOut, ch("MIDI Out", {"Off", "Clip Player", "Synth"}, 0));
  set(G_NoteLatch, tog("Note Latch", 0));
  const char* arpOff[] = {"Arp Rate", "Arp Shift", "Arp Range", "Arp Offset", "Arp Repeats", "Arp Gate", "Arp Chance",
                          "Arp Retrig Rate", "Arp Velo Decay", "Arp Velo Target", "Arp Transpose", "Arp Wrap Transpose",
                          "Arp Wrap Range", "Arp Wrap Phantom"};
  for (int i = 0; i < 14; ++i) set(G_ArpRate + i, lin(arpOff[i], "%", -1, 1, 0));
  set(G_ClipTranspose, lin("Clip Transpose", "%", -1, 1, 0));
  set(G_ClipRate, lin("Clip Rate", "%", -1, 1, 0));
  set(G_ClipOffset, lin("Clip Offset", "%", -1, 1, 0));
  {
    std::vector<std::string> oct = {"Off"};
    for (int o = -2; o <= 8; ++o) oct.push_back("C" + std::to_string(o));
    set(G_TrigOctave, ch("Trigger Octave", oct, 1));
  }

  // ---- oscillators
  std::vector<std::string> tableChoices = {"Custom"};
  for (const auto& n : factoryTableNames()) tableChoices.push_back(n);
  const std::vector<std::string> warps = warpNames();
  for (int o = 0; o < kOscs; ++o) {
    const std::string n = std::string(kOscLetter[o]) + " ";
    auto O = [&](int q, ParamDesc d) { set(oscP(o, q), std::move(d)); };
    O(O_Enable, tog(n + "Enable", o == 0 ? 1 : 0));
    O(O_Type, ch(n + "Type", {"Wavetable", "Multisample", "Sample", "Granular", "Spectral"}, 0));
    O(O_Table, ch(n + "Table", tableChoices, 1));
    O(O_Level, lin(n + "Level", "%", 0, 1, 0.75));
    O(O_Pan, lin(n + "Pan", "%", -1, 1, 0));
    O(O_Octave, lin(n + "Octave", "oct", -4, 4, 0));
    O(O_Semi, lin(n + "Semi", "st", -12, 12, 0));
    O(O_Fine, lin(n + "Fine", "ct", -100, 100, 0));
    O(O_Coarse, lin(n + "Coarse Pitch", "st", -64, 64, 0));
    O(O_Ratio, lin(n + "Ratio", "x", 0.25, 64, 1));
    O(O_HzOffset, lin(n + "Hz Offset", "Hz", -1000, 1000, 0));
    O(O_PitchTrack, tog(n + "Pitch Track", 1));
    O(O_PitchMode, ch(n + "Pitch Mode", {"Semitones", "Steps", "Harmonics", "Ratio"}, 0));
    O(O_WtPos, lin(n + "WT Pos", "%", 0, 1, 0));
    O(O_Phase, lin(n + "Phase", "%", 0, 1, 0.5));
    O(O_RandPhase, lin(n + "Rand Phase", "%", 0, 1, 1));
    O(O_PhaseMemory, ch(n + "Phase Memory", {"Per Voice", "Contiguous", "Free"}, 0));
    O(O_Smooth, tog(n + "Smooth Interp", 1));
    O(O_Unison, lin(n + "Unison", "", 1, 16, 1));
    O(O_UniStack, ch(n + "Uni Stack", {"Off", "12 x1", "12 x2", "12 x3", "12+7 x1", "12+7 x2", "12+7 x3", "Center -12", "Center -24"}, 0));
    O(O_UniDetune, lin(n + "Uni Detune", "%", 0, 1, 0.5));
    O(O_UniBlend, lin(n + "Uni Blend", "%", 0, 1, 0.75));
    O(O_UniWidth, lin(n + "Uni Width", "%", -1, 1, 1));
    O(O_UniSpan, lin(n + "Uni Span", "%", -1, 1, 0));
    O(O_UniRange, lin(n + "Uni Range", "st", 0, 48, 2));
    O(O_UniRotate, lin(n + "Uni Rotate", "%", 0, 1, 0));
    O(O_UniRandStart, lin(n + "Uni Rand Start", "%", 0, 1, 0));
    O(O_UniWtPos, lin(n + "Uni WT Pos", "%", -1, 1, 0));
    O(O_UniWarp1, lin(n + "Uni Warp", "%", -1, 1, 0));
    O(O_UniWarp2, lin(n + "Uni Warp 2", "%", -1, 1, 0));
    O(O_DetuneMode, ch(n + "Detune Mode", {"Linear", "Super", "Exp", "Inverse", "Random"}, 0));
    O(O_Warp1Mode, ch(n + "Warp Mode", warps, 0));
    O(O_Warp1, lin(n + "Warp", "%", 0, 1, 0));
    O(O_Warp1Var, lin(n + "Warp Var", "%", 0, 1, 0));
    O(O_Warp2Mode, ch(n + "Warp 2 Mode", warps, 0));
    O(O_Warp2, lin(n + "Warp 2", "%", 0, 1, 0));
    O(O_Warp2Var, lin(n + "Warp 2 Var", "%", 0, 1, 0));
    O(O_Start, lin(n + "Start", "%", 0, 1, 0));
    O(O_End, lin(n + "End", "%", 0, 1, 1));
    O(O_Reverse, tog(n + "Reverse", 0));
    O(O_ScanRate, lin(n + "Scan Rate", "x", -2, 2, 1));
    O(O_ScanSync, tog(n + "Scan Sync", 0));
    O(O_ScanBpm, ch(n + "Scan Division", divisionNames(), 3));
    O(O_ScanKey, tog(n + "Scan Key Track", 0));
    O(O_Position, lin(n + "Position", "%", 0, 1, 0));
    O(O_LoopStart, lin(n + "Loop Start", "%", 0, 1, 0));
    O(O_LoopEnd, lin(n + "Loop End", "%", 0, 1, 1));
    O(O_LoopXfade, lin(n + "Loop X-Fade", "%", 0, 1, 0));
    O(O_LoopMode, ch(n + "Loop Mode", {"One-Shot", "Forward", "Reverse", "Ping-Pong", "Tailed"}, 0));
    O(O_RelLoop, tog(n + "Relative Loop", 0));
    O(O_Slice, lin(n + "Slice", "", 1, 100, 1));
    O(O_SliceMode, ch(n + "Slice Play", {"To Next Marker", "To End"}, 0));
    O(O_GDensity, lin(n + "Density", "%", 0, 1, 0.55));
    O(O_GLength, lin(n + "Grain Length", "%", 0, 1, 0.6));
    O(O_GReverse, lin(n + "Grain Reverse", "%", 0, 1, 0));
    O(O_GWinShape, ch(n + "Window Shape", {"Hann", "Welch", "Gaussian", "Blackman-Harris", "Sinc", "Tukey", "Triangle", "Trapezoid", "Exp Decay", "Exp Rise"}, 0));
    O(O_GWinSkew, lin(n + "Window Skew", "%", -1, 1, 0));
    O(O_GWinParam, lin(n + "Window Amount", "%", 0, 1, 0.5));
    O(O_GRndOffset, lin(n + "Rand Offset", "%", 0, 1, 0));
    O(O_GRndDir, lin(n + "Rand Direction", "%", 0, 1, 0));
    O(O_GRndPitch, lin(n + "Rand Pitch", "%", 0, 1, 0));
    O(O_GRndLength, lin(n + "Rand Length", "%", 0, 1, 0));
    O(O_GRndPan, lin(n + "Rand Pan", "%", 0, 1, 0));
    O(O_GRndGain, lin(n + "Rand Gain", "%", 0, 1, 0));
    O(O_GRndWin, lin(n + "Rand Window", "%", 0, 1, 0));
    O(O_GRndSkew, lin(n + "Rand Skew", "%", 0, 1, 0));
    O(O_GRndWarp, lin(n + "Rand Warp", "%", 0, 1, 0));
    O(O_GRndWarp2, lin(n + "Rand Warp 2", "%", 0, 1, 0));
    O(O_GDensMode, ch(n + "Density Mode", {"Free", "Tempo", "Grains"}, 0));
    O(O_GLenMode, ch(n + "Length Mode", {"Free", "Tempo", "Percent"}, 0));
    O(O_GMaxGrains, lin(n + "Max Grains", "", 1, 64, 24));
    O(O_GTrig, ch(n + "Unison Trigger", {"Together", "Even", "Exponential", "Random"}, 0));
    O(O_SCut, lin(n + "Spectral Cut", "%", -1, 1, 0));
    O(O_SMix, lin(n + "Spectral Mix", "%", 0, 1, 1));
    O(O_SLo, lin(n + "Spectral Low", "%", 0, 1, 0));
    O(O_SHi, lin(n + "Spectral High", "%", 0, 1, 1));
    O(O_SPhaseLock, tog(n + "Phase Lock", 0));
    O(O_STransients, tog(n + "Keep Transients", 1));
    O(O_MTimbre, lin(n + "Timbre", "%", -1, 1, 0));
    O(O_MVelTrack, lin(n + "Vel Track", "%", 0, 1, 1));
    O(O_MEnvOn, tog(n + "Sample Env", 0));
    O(O_MDelay, lin(n + "Sample Env Delay", "%", 0, 1, 0));
    O(O_MAttack, lin(n + "Sample Env Attack", "%", 0, 1, 0.1));
    O(O_MHold, lin(n + "Sample Env Hold", "%", 0, 1, 0));
    O(O_MDecay, lin(n + "Sample Env Decay", "%", 0, 1, 0.5));
    O(O_MSustain, lin(n + "Sample Env Sustain", "%", 0, 1, 1));
    O(O_MRelease, lin(n + "Sample Env Release", "%", 0, 1, 0.3));
    O(O_KeyMin, lin(n + "Key Min", "", 0, 127, 0));
    O(O_KeyMax, lin(n + "Key Max", "", 0, 127, 127));
    O(O_VelMin, lin(n + "Vel Min", "", 0, 127, 0));
    O(O_VelMax, lin(n + "Vel Max", "", 0, 127, 127));
    O(O_MapFold, tog(n + "Key Fold", 0));
    O(O_MapWarp, tog(n + "Zone Warp Source", 0));
  }

  // ---- sub / noise
  set(subP(S_Enable), tog("Sub Enable", 0));
  set(subP(S_Level), lin("Sub Level", "%", 0, 1, 0.75));
  set(subP(S_Pan), lin("Sub Pan", "%", -1, 1, 0));
  set(subP(S_Octave), lin("Sub Octave", "oct", -4, 4, 0));
  set(subP(S_Coarse), lin("Sub Coarse Pitch", "st", -64, 64, 0));
  set(subP(S_PitchTrack), tog("Sub Pitch Track", 1));
  set(subP(S_Shape), ch("Sub Shape", {"Sine", "Round Rect", "Triangle", "Saw", "Square", "Pulse"}, 0));
  set(subP(S_Phase), lin("Sub Phase", "%", 0, 1, 0));
  set(subP(S_ContPhase), tog("Sub Continuous Phase", 0));
  set(noiseP(N_Enable), tog("Noise Enable", 0));
  set(noiseP(N_Level), lin("Noise Level", "%", 0, 1, 0.25));
  set(noiseP(N_Pan), lin("Noise Pan", "%", -1, 1, 0));
  set(noiseP(N_PitchTrack), tog("Noise Pitch Track", 0));
  set(noiseP(N_Pitch), lin("Noise Pitch", "st", -64, 64, 0));
  set(noiseP(N_Fine), lin("Noise Fine", "%", -1, 1, 0));
  set(noiseP(N_Phase), lin("Noise Phase", "%", 0, 1, 0));
  set(noiseP(N_RandPhase), lin("Noise Rand Phase", "%", 0, 1, 0));
  set(noiseP(N_OneShot), tog("Noise One-Shot", 0));
  set(noiseP(N_Type), ch("Noise Type", noiseNames(), 1));
  set(noiseP(N_Color), lin("Noise Color", "%", -1, 1, 0));
  // tone filter: below 0 a 12 dB/oct low-pass closes, above 0 a 12 dB/oct high-pass opens; 0 = off
  set(tailP(T_NoiseTone), lin("Noise Tone", "%", -1, 1, 0));
  // unison voice mix (all oscillators): Even spreads the voices evenly across the stereo field;
  // Alternating sends them hard left and right in turn (an odd count keeps a centre voice in
  // both), with its own blend and voice-count level curves. Preset import selects Alternating.
  set(tailP(T_UniMix), ch("Unison Mix", {"Even", "Alternating"}, 0));
  // filter drive (both filters and the filter effect): Leveled keeps the level about even as the
  // drive rises; Boost raises it, up to +24 dB into a soft clip. Preset import selects Boost.
  set(tailP(T_DriveMode), ch("Filter Drive Mode", {"Leveled", "Boost"}, 0));
  // filter key tracking (both filters): the key at which tracking leaves the cutoff where it is.
  // Preset import selects G#1 (J-203a/b: measured, the cutoff moves by key - 32 semitones).
  set(tailP(T_KeyCentre), ch("Filter Key Centre", {"C4", "G#1"}, 0));
  // multisample level: Plasma plays zones at unity with linear velocity; Mapped follows the map's
  // velocity tracking (amp_veltrack, squared curve), its amplitude envelope and +9.1 dB of gain.
  // Preset import selects Mapped (J-203a: measured).
  set(tailP(T_MsLaw), ch("Multisample Level", {"Unity", "Mapped"}, 0));
  // envelope curves (all four envelopes): Gentle bends a stage by 8 c (c = curve -1..1); Steep
  // bends it by 9.5 atanh(c), so the ends of the knob become nearly a step. Preset import selects
  // Steep (J-203a: measured).
  set(tailP(T_EnvLaw), ch("Envelope Curves", {"Gentle", "Steep"}, 0));

  // ---- filters
  for (int f = 0; f < kFilters; ++f) {
    const std::string n = "Filter " + std::to_string(f + 1) + " ";
    set(filtP(f, F_Enable), tog(n + "On", 0));
    set(filtP(f, F_Type), ch(n + "Type", filterNames(), 1));
    set(filtP(f, F_Freq), lin(n + "Freq", "%", 0, 1, 0.5));
    set(filtP(f, F_Res), lin(n + "Res", "%", 0, 1, 0.1));
    set(filtP(f, F_Drive), lin(n + "Drive", "%", 0, 1, 0));
    set(filtP(f, F_Var), lin(n + "Var", "%", 0, 1, 0));
    set(filtP(f, F_Stereo), lin(n + "Pan", "%", 0, 1, 0.5));
    set(filtP(f, F_Wet), lin(n + "Mix", "%", 0, 1, 1));
    set(filtP(f, F_Level), lin(n + "Level", "%", 0, 1, 0.5));
    set(filtP(f, F_KeyTrack), tog(n + "Key Track", 0));
    set(filtP(f, F_X), lin(n + "X", "%", 0, 1, 0));
    set(filtP(f, F_Y), lin(n + "Y", "%", 0, 1, 0));
  }

  // ---- routing
  for (int r = 0; r < kRoutes; ++r) {
    const std::string n = std::string(kRouteNames[r]) + " ";
    std::vector<std::string> dests = r < RtF1 ? std::vector<std::string>{"Filter", "Main", "Direct", "None"}
                                               : std::vector<std::string>{r == RtF1 ? "Filter 2" : "Filter 1", "Main", "Direct", "None"};
    set(routeP(r, R_Dest), ch(n + "Output", dests, r == RtA ? RouteFilter : RouteMain));
    set(routeP(r, R_Balance), lin(n + "Filter Balance", "%", -1, 1, -1));
    set(routeP(r, R_Bus1), lin(n + "Bus 1 Send", "%", 0, 1, 0));
    set(routeP(r, R_Bus2), lin(n + "Bus 2 Send", "%", 0, 1, 0));
  }

  // ---- envelopes
  for (int e = 0; e < kEnvs; ++e) {
    const std::string n = "Env " + std::to_string(e + 1) + " ";
    set(envP(e, E_Attack), lin(n + "Attack", "%", 0, 1, envTimeInv(0.001)));
    set(envP(e, E_Hold), lin(n + "Hold", "%", 0, 1, 0));
    set(envP(e, E_Decay), lin(n + "Decay", "%", 0, 1, envTimeInv(1.0)));
    set(envP(e, E_Sustain), lin(n + "Sustain", "%", 0, 1, 1));
    set(envP(e, E_Release), lin(n + "Release", "%", 0, 1, envTimeInv(0.015)));
    set(envP(e, E_CurveA), lin(n + "Atk Curve", "pct", 0, 100, 50));
    set(envP(e, E_CurveD), lin(n + "Dec Curve", "pct", 0, 100, 66.6));
    set(envP(e, E_CurveR), lin(n + "Rel Curve", "pct", 0, 100, 66.6));
    set(envP(e, E_Start), lin(n + "Start", "%", 0, 1, 0));
    set(envP(e, E_End), lin(n + "End", "%", 0, 1, 0));
    set(envP(e, E_Bpm), tog(n + "Tempo Sync", 0));
    set(envP(e, E_LegatoInv), tog(n + "Legato Inverted", 0));
    set(envP(e, E_Steal), ch(n + "Steal Restart", {"From Current Level", "From Zero"}, 0));
  }

  // ---- LFOs
  for (int l = 0; l < kLfos; ++l) {
    const std::string n = "LFO " + std::to_string(l + 1) + " ";
    set(lfoP(l, L_Rate), lin(n + "Rate", "%", 0, 1, 0.5));
    set(lfoP(l, L_Smooth), lin(n + "Smooth", "%", 0, 1, 0));
    set(lfoP(l, L_Rise), lin(n + "Rise", "%", 0, 1, 0));
    set(lfoP(l, L_Delay), lin(n + "Delay", "%", 0, 1, 0));
    set(lfoP(l, L_Phase), lin(n + "Phase", "%", 0, 1, 0));
    set(lfoP(l, L_Mode), ch(n + "Mode", {"Free", "Retrig", "Envelope"}, LfoRetrig));
    set(lfoP(l, L_Type), ch(n + "Type", {"Normal", "Path", "Chaos Lorenz", "Chaos Rossler", "Sample & Hold"}, 0));
    set(lfoP(l, L_Bpm), tog(n + "Tempo Sync", 1));
    set(lfoP(l, L_Anchor), tog(n + "Host Anchor", 1));
    set(lfoP(l, L_Dot), tog(n + "Dotted", 0));
    set(lfoP(l, L_Trip), tog(n + "Triplet", 0));
    set(lfoP(l, L_GridX), lin(n + "Grid X", "", 1, 32, 8));
    set(lfoP(l, L_GridY), lin(n + "Grid Y", "", 1, 32, 8));
    set(lfoP(l, L_Mono), tog(n + "Mono", 0));
    set(lfoP(l, L_Dir), ch(n + "Direction", {"Forward", "Reverse", "Ping-Pong"}, 0));
    set(lfoP(l, L_Rate10x), tog(n + "Rate x10", 0));
    set(lfoP(l, L_Swing), tog(n + "Swing", 0));
    set(lfoP(l, L_PhaseSnap), tog(n + "Phase Snap", 0));
    set(lfoP(l, L_Shape), ch(n + "Shape", lfoShapeNames(), 1));
  }

  for (int m = 0; m < kMacros; ++m) set(macroP(m), lin("Macro " + std::to_string(m + 1), "%", 0, 1, 0));
  for (int b = 0; b < kLfoBusses; ++b) set(busP(b), lin("LFO Bus " + std::to_string(b + 1), "%", -1, 1, 0));
  for (int r = 0; r < kRetrigs; ++r) set(retrigP(r), lin(std::string("Retrigger ") + kRetrigNames[r], "%", 0, 1, 0));

  // ---- matrix
  for (int s = 0; s < kModSlots; ++s) {
    const std::string n = "Mod " + std::to_string(s + 1) + " ";
    set(modP(s, M_Source), lin(n + "Source", "", 0, SourceCount - 1, 0));
    set(modP(s, M_Amount), lin(n + "Amount", "%", -1, 1, 0));
    set(modP(s, M_Bipolar), tog(n + "Bipolar", 0));
    set(modP(s, M_Dest), lin(n + "Destination", "", 0, kParamCount, 0));
    set(modP(s, M_Aux), lin(n + "Aux Source", "", 0, SourceCount - 1, 0));
    set(modP(s, M_AuxInv), tog(n + "Aux Invert", 0));
    set(modP(s, M_Out), lin(n + "Output", "%", 0, 1, 1));
    set(modP(s, M_Bypass), tog(n + "Bypass", 0));
    set(modP(s, M_Curve), lin(n + "Curve", "%", -1, 1, 0));
    set(modP(s, M_AuxCurve), lin(n + "Aux Curve", "%", -1, 1, 0));
    set(modP(s, M_Rise), lin(n + "Smooth Rise", "%", 0, 1, 0));
    set(modP(s, M_Fall), lin(n + "Smooth Fall", "%", 0, 1, 0));
  }

  // ---- effect slots
  for (int s = 0; s < kFxSlots; ++s) {
    const std::string n = "FX " + std::to_string(s + 1) + " ";
    set(fxP(s, X_Type), ch(n + "Type", fxTypeNames(), 0));
    set(fxP(s, X_On), tog(n + "On", 1));
    for (int q = 0; q < kFxParams; ++q) set(fxP(s, X_P0 + q), lin(n + "Param " + std::to_string(q + 1), "%", 0, 1, 0));
  }

  p.editorWidth = 1190;

  // ---- factory presets (parameter overrides; original sounds)
  auto osc = [](int o, int q) { return oscP(o, q); };
  using PV = std::vector<std::pair<int, float>>;
  auto tbl = [](const char* name) {
    const auto& n = factoryTableNames();
    for (size_t i = 0; i < n.size(); ++i)
      if (n[i] == name) return static_cast<float>(i + 1);
    return 1.0f;
  };
  auto ft = [](const char* name) {
    const auto& n = filterNames();
    for (size_t i = 0; i < n.size(); ++i)
      if (n[i] == name) return static_cast<float>(i);
    return 1.0f;
  };
  auto fxs = [](int s, int type, std::vector<std::pair<const char*, double>> vals) {
    PV v = {{fxP(s, X_Type), static_cast<float>(type)}};
    // defaults of the type, then the overrides (plain values)
    const auto& d = fxParams(type);
    for (size_t i = 0; i < d.size(); ++i) v.push_back({fxP(s, X_P0 + static_cast<int>(i)), fxDefaultNorm(type, static_cast<int>(i))});
    for (const auto& [key, plain] : vals) {
      const int i = fxParamIndex(type, key);
      if (i >= 0) v.push_back({fxP(s, X_P0 + i), static_cast<float>(fxNorm(type, i, plain))});
    }
    return v;
  };
  auto mod = [](int s, int src, int dest, float amt, bool bi = false) {
    return PV{{modP(s, M_Source), static_cast<float>(src)}, {modP(s, M_Dest), static_cast<float>(dest + 1)},
              {modP(s, M_Amount), amt}, {modP(s, M_Bipolar), bi ? 1.f : 0.f}};
  };
  auto join = [](std::initializer_list<PV> parts) {
    PV out;
    for (const auto& p2 : parts) out.insert(out.end(), p2.begin(), p2.end());
    return out;
  };
  auto env = [](int e, double a, double d, double s, double r) {
    return PV{{envP(e, E_Attack), static_cast<float>(envTimeInv(a))}, {envP(e, E_Decay), static_cast<float>(envTimeInv(d))},
              {envP(e, E_Sustain), static_cast<float>(s)}, {envP(e, E_Release), static_cast<float>(envTimeInv(r))}};
  };
  const float lpf24 = ft("Warm Low 24"), lp12 = ft("Low 12");
  p.presets.push_back({"Init", {}});
  p.presets.push_back({"Supersaw Lead",
                       join({{{osc(0, O_Table), tbl("Saw Stack")}, {osc(0, O_Unison), 7}, {osc(0, O_UniDetune), 0.42f},
                              {osc(0, O_UniWidth), 1}, {osc(1, O_Enable), 1}, {osc(1, O_Table), tbl("Saw Stack")},
                              {osc(1, O_Octave), 1}, {osc(1, O_Unison), 5}, {osc(1, O_UniDetune), 0.35f}, {osc(1, O_Level), 0.55f},
                              {filtP(0, F_Enable), 1}, {filtP(0, F_Type), lpf24}, {filtP(0, F_Freq), 0.82f}, {filtP(0, F_Res), 0.15f},
                              {routeP(RtB, R_Dest), RouteFilter}, {G_Poly, 8}},
                             env(0, 0.004, 1.2, 0.8, 0.35), fxs(0, FxChorus, {{"kParamWet", 30}}),
                             fxs(1, FxDelay, {{"kParamWet", 18}}), fxs(2, FxReverb, {{"kParamWet", 20}, {"kParamSize", 60}})})});
  p.presets.push_back({"Reese Growl",
                       join({{{osc(0, O_Table), tbl("Saw Stack")}, {osc(0, O_Unison), 4}, {osc(0, O_UniDetune), 0.28f},
                              {osc(0, O_Octave), -1}, {osc(1, O_Enable), 1}, {osc(1, O_Table), tbl("Saw Stack")},
                              {osc(1, O_Octave), -1}, {osc(1, O_Fine), 12}, {osc(1, O_Unison), 4}, {osc(1, O_UniDetune), 0.3f},
                              {routeP(RtB, R_Dest), RouteFilter}, {filtP(0, F_Enable), 1}, {filtP(0, F_Type), lpf24},
                              {filtP(0, F_Freq), 0.45f}, {filtP(0, F_Res), 0.35f}, {filtP(0, F_Drive), 0.4f}, {G_Mono, 1},
                              {G_Legato, 1}, {G_PortaTime, 0.45f}, {subP(S_Enable), 1}, {subP(S_Octave), -1}, {subP(S_Level), 0.6f},
                              {routeP(RtSub, R_Dest), RouteDirect}},
                             mod(0, SrcLfo1, filtP(0, F_Freq), 0.35f, true),
                             PV{{lfoP(0, L_Rate), 0.57f}, {lfoP(0, L_Shape), 2}},
                             fxs(0, FxDistortion, {{"kParamDrive", 35}, {"kParamWet", 60}}), fxs(1, FxComp, {{"kParamWet", 100}})})});
  p.presets.push_back({"Glass Pluck",
                       join({{{osc(0, O_Table), tbl("Glass Bells")}, {osc(0, O_WtPos), 0.3f}, {filtP(0, F_Enable), 1},
                              {filtP(0, F_Type), lp12}, {filtP(0, F_Freq), 0.55f}},
                             env(0, 0.001, 0.45, 0.0, 0.4), env(1, 0.001, 0.25, 0.0, 0.2),
                             mod(0, SrcEnv2, filtP(0, F_Freq), 0.45f), mod(1, SrcEnv2, oscP(0, O_WtPos), 0.4f),
                             fxs(0, FxDelay, {{"kParamWet", 22}, {"kParamFeedback", 45}}),
                             fxs(1, FxReverb, {{"kParamWet", 28}, {"kParamSize", 70}})})});
  p.presets.push_back({"Warm Pad",
                       join({{{osc(0, O_Table), tbl("Analog Morph")}, {osc(0, O_Unison), 6}, {osc(0, O_UniDetune), 0.3f},
                              {osc(1, O_Enable), 1}, {osc(1, O_Table), tbl("Vocal Formants")}, {osc(1, O_Level), 0.5f},
                              {routeP(RtB, R_Dest), RouteFilter}, {filtP(0, F_Enable), 1}, {filtP(0, F_Type), lpf24},
                              {filtP(0, F_Freq), 0.6f}, {G_Poly, 12}},
                             env(0, 0.9, 2.0, 0.85, 2.5), mod(0, SrcLfo1, oscP(0, O_WtPos), 0.5f),
                             PV{{lfoP(0, L_Rate), 0.25f}, {lfoP(0, L_Shape), 2}, {lfoP(0, L_Mode), LfoFree}},
                             mod(1, SrcLfo1 + 1,filtP(0, F_Freq), 0.15f, true), PV{{lfoP(1, L_Rate), 0.3f}, {lfoP(1, L_Shape), 2}},
                             fxs(0, FxChorus, {{"kParamWet", 45}}), fxs(1, FxReverb, {{"kParamWet", 40}, {"kParamSize", 85}, {"kParamType", 1}})})});
  p.presets.push_back({"Sub Bass",
                       join({{{osc(0, O_Table), tbl("Basic Shapes")}, {osc(0, O_WtPos), 1.0f}, {osc(0, O_Octave), -1},
                              {osc(0, O_Level), 0.55f}, {subP(S_Enable), 1}, {subP(S_Octave), -1}, {subP(S_Level), 0.85f},
                              {routeP(RtSub, R_Dest), RouteMain}, {filtP(0, F_Enable), 1}, {filtP(0, F_Type), lpf24},
                              {filtP(0, F_Freq), 0.42f}, {G_Mono, 1}, {G_Legato, 1}, {G_PortaTime, 0.35f}},
                             env(0, 0.002, 0.6, 0.9, 0.08), fxs(0, FxUtility, {{"kParamLFMono", 1}})})});
  p.presets.push_back({"Acid Line",
                       join({{{osc(0, O_Table), tbl("Basic Shapes")}, {osc(0, O_WtPos), 0.0f}, {filtP(0, F_Enable), 1},
                              {filtP(0, F_Type), ft("Ladder Acid")}, {filtP(0, F_Freq), 0.38f}, {filtP(0, F_Res), 0.72f},
                              {filtP(0, F_Drive), 0.35f}, {G_Mono, 1}, {G_Legato, 1}, {G_PortaTime, 0.3f}},
                             env(0, 0.001, 0.4, 0.6, 0.06), env(1, 0.001, 0.22, 0.0, 0.1), mod(0, SrcEnv2, filtP(0, F_Freq), 0.5f),
                             mod(1, SrcVelo, filtP(0, F_Res), 0.15f),
                             fxs(0, FxDistortion, {{"kParamMode", 16}, {"kParamDrive", 30}, {"kParamWet", 50}}),
                             fxs(1, FxDelay, {{"kParamWet", 20}, {"kParamMode", 1}})})});
  p.presets.push_back({"Bell Keys",
                       join({{{osc(0, O_Table), tbl("Glass Bells")}, {osc(0, O_WtPos), 0.6f}, {osc(1, O_Enable), 1},
                              {osc(1, O_Table), tbl("Basic Shapes")}, {osc(1, O_WtPos), 1.0f}, {osc(1, O_Octave), 2}, {osc(1, O_Level), 0.4f},
                              {osc(0, O_Warp1Mode), 33}, {osc(0, O_Warp1), 0.25f}},
                             env(0, 0.002, 1.6, 0.2, 1.2), mod(0, SrcEnv2, oscP(0, O_Warp1), 0.35f), env(1, 0.001, 0.6, 0.0, 0.5),
                             fxs(0, FxReverb, {{"kParamWet", 30}, {"kParamSize", 60}})})});
  p.presets.push_back({"Wobble Bass",
                       join({{{osc(0, O_Table), tbl("Digital Growl")}, {osc(0, O_Octave), -1}, {osc(0, O_Unison), 2},
                              {osc(0, O_UniDetune), 0.15f}, {filtP(0, F_Enable), 1}, {filtP(0, F_Type), lpf24}, {filtP(0, F_Res), 0.3f},
                              {subP(S_Enable), 1}, {subP(S_Octave), -1}, {routeP(RtSub, R_Dest), RouteMain}, {G_Mono, 1}},
                             mod(0, SrcLfo1, filtP(0, F_Freq), 0.6f), mod(1, SrcLfo1, oscP(0, O_WtPos), 0.5f),
                             PV{{lfoP(0, L_Rate), 0.5f}, {lfoP(0, L_Shape), 2}, {lfoP(0, L_Trip), 1}},
                             fxs(0, FxDistortion, {{"kParamDrive", 40}, {"kParamWet", 70}, {"kParamMode", 4}}),
                             fxs(1, FxComp, {{"kParamWet", 100}, {"kParamMultiband", 1}})})});
  p.presets.push_back({"Noise Riser",
                       join({{{osc(0, O_Enable), 0}, {noiseP(N_Enable), 1}, {noiseP(N_Level), 0.8f}, {noiseP(N_Type), 1},
                              {routeP(RtNoise, R_Dest), RouteFilter}, {filtP(0, F_Enable), 1}, {filtP(0, F_Type), ft("Band 12")},
                              {filtP(0, F_Res), 0.5f}, {filtP(0, F_Freq), 0.3f}},
                             env(0, 4.0, 1.0, 1.0, 1.5), mod(0, SrcEnv1, filtP(0, F_Freq), 0.6f),
                             fxs(0, FxReverb, {{"kParamWet", 45}, {"kParamSize", 90}, {"kParamType", 3}})})});
  p.presets.push_back({"Hollow Organ",
                       join({{{osc(0, O_Table), tbl("Organ Drawbars")}, {osc(0, O_WtPos), 0.4f}, {osc(1, O_Enable), 1},
                              {osc(1, O_Table), tbl("Basic Shapes")}, {osc(1, O_WtPos), 1.0f}, {osc(1, O_Octave), 1}, {osc(1, O_Level), 0.35f}},
                             env(0, 0.005, 0.2, 1.0, 0.08), fxs(0, FxChorus, {{"kParamWet", 50}, {"kParamRate", 5}}),
                             fxs(1, FxReverb, {{"kParamWet", 18}})})});
  p.presets.push_back({"FM Bass",
                       join({{{osc(0, O_Table), tbl("Basic Shapes")}, {osc(0, O_WtPos), 1.0f}, {osc(0, O_Octave), -1},
                              {osc(0, O_Warp1Mode), 33}, {osc(0, O_Warp1), 0.4f}, {osc(1, O_Enable), 1},
                              {osc(1, O_Table), tbl("Basic Shapes")}, {osc(1, O_WtPos), 1.0f}, {osc(1, O_Level), 0.0f}, {routeP(RtB, R_Dest), RouteNone},
                              {G_Mono, 1}},
                             env(0, 0.001, 0.5, 0.5, 0.1), env(1, 0.001, 0.3, 0.1, 0.1), mod(0, SrcEnv2, oscP(0, O_Warp1), 0.5f),
                             fxs(0, FxDistortion, {{"kParamMode", 0}, {"kParamDrive", 20}, {"kParamWet", 40}})})});
  p.presets.push_back({"Granular Cloud",
                       join({{{osc(0, O_Type), OscGranular}, {osc(0, O_Table), tbl("Vocal Formants")}, {osc(0, O_GDensity), 0.6f},
                              {osc(0, O_GLength), 0.65f}, {osc(0, O_GRndOffset), 0.3f}, {osc(0, O_GRndPan), 0.6f},
                              {osc(0, O_GRndPitch), 0.08f}, {G_Poly, 6}},
                             env(0, 1.2, 2.0, 0.9, 3.0), mod(0, SrcLfo1, oscP(0, O_Position), 0.4f),
                             PV{{lfoP(0, L_Rate), 0.2f}, {lfoP(0, L_Mode), LfoFree}, {lfoP(0, L_Shape), 2}},
                             fxs(0, FxReverb, {{"kParamWet", 50}, {"kParamSize", 90}, {"kParamType", 4}})})});
  p.presets.push_back({"Arp Sequence",
                       join({{{osc(0, O_Table), tbl("Saw Stack")}, {osc(0, O_Unison), 3}, {osc(0, O_UniDetune), 0.25f},
                              {filtP(0, F_Enable), 1}, {filtP(0, F_Type), lpf24}, {filtP(0, F_Freq), 0.5f}, {G_ArpOn, 1}},
                             env(0, 0.001, 0.3, 0.2, 0.15), env(1, 0.001, 0.2, 0.0, 0.1), mod(0, SrcEnv2, filtP(0, F_Freq), 0.4f),
                             fxs(0, FxDelay, {{"kParamWet", 25}, {"kParamMode", 1}}), fxs(1, FxReverb, {{"kParamWet", 15}})})});
  p.presets.push_back({"Spectral Choir",
                       join({{{osc(0, O_Type), OscSpectral}, {osc(0, O_Table), tbl("Vocal Formants")}, {osc(0, O_Unison), 3},
                              {osc(0, O_UniDetune), 0.2f}, {osc(0, O_Warp1Mode), 72}, {osc(0, O_Warp1), 0.3f}, {G_Poly, 8}},
                             env(0, 0.6, 1.5, 0.9, 2.0), mod(0, SrcLfo1, oscP(0, O_Position), 0.3f),
                             PV{{lfoP(0, L_Rate), 0.3f}, {lfoP(0, L_Mode), LfoFree}, {lfoP(0, L_Shape), 2}},
                             fxs(0, FxChorus, {{"kParamWet", 35}}), fxs(1, FxReverb, {{"kParamWet", 35}, {"kParamSize", 80}})})});
  p.presets.push_back({"Kick Drum",
                       join({{{osc(0, O_Table), tbl("Basic Shapes")}, {osc(0, O_WtPos), 1.0f}, {osc(0, O_Octave), -2},
                              {osc(0, O_Phase), 0.25f}, {osc(0, O_RandPhase), 0}, {G_Mono, 1}},
                             env(0, 0.0005, 0.35, 0.0, 0.1), env(1, 0.0005, 0.08, 0.0, 0.05),
                             mod(0, SrcEnv2, oscP(0, O_Coarse), 0.5f), fxs(0, FxDistortion, {{"kParamDrive", 30}, {"kParamWet", 50}})})});
  p.presets.push_back({"Snare Noise",
                       join({{{osc(0, O_Table), tbl("Basic Shapes")}, {osc(0, O_WtPos), 1.0f}, {osc(0, O_Level), 0.5f},
                              {noiseP(N_Enable), 1}, {noiseP(N_Level), 0.7f}, {noiseP(N_Type), 1}, {routeP(RtNoise, R_Dest), RouteFilter},
                              {filtP(0, F_Enable), 1}, {filtP(0, F_Type), ft("High 12")}, {filtP(0, F_Freq), 0.55f}},
                             env(0, 0.0005, 0.18, 0.0, 0.12), env(1, 0.0005, 0.04, 0.0, 0.03), mod(0, SrcEnv2, oscP(0, O_Coarse), 0.3f),
                             fxs(0, FxReverb, {{"kParamWet", 15}, {"kParamSize", 30}})})});
  p.presets.push_back({"Hyper Stab",
                       join({{{osc(0, O_Table), tbl("Saw Stack")}, {osc(0, O_Unison), 5}, {osc(0, O_UniDetune), 0.3f},
                              {osc(1, O_Enable), 1}, {osc(1, O_Table), tbl("Square PWM")}, {osc(1, O_Octave), -1},
                              {routeP(RtB, R_Dest), RouteFilter}, {filtP(0, F_Enable), 1}, {filtP(0, F_Type), lpf24},
                              {filtP(0, F_Freq), 0.62f}},
                             env(0, 0.001, 0.35, 0.0, 0.25), env(1, 0.001, 0.2, 0.0, 0.2), mod(0, SrcEnv2, filtP(0, F_Freq), 0.3f),
                             fxs(0, FxHyper, {{"kParamWet", 60}}), fxs(1, FxReverb, {{"kParamWet", 22}})})});
  p.presets.push_back({"Formant Talker",
                       join({{{osc(0, O_Table), tbl("Saw Stack")}, {osc(0, O_Unison), 2}, {osc(0, O_UniDetune), 0.15f},
                              {filtP(0, F_Enable), 1}, {filtP(0, F_Type), ft("Formant 1")}, {filtP(0, F_Res), 0.4f}, {G_Mono, 1}},
                             mod(0, SrcLfo1, filtP(0, F_Var), 0.8f), PV{{lfoP(0, L_Rate), 0.45f}, {lfoP(0, L_Shape), 2}},
                             fxs(0, FxDistortion, {{"kParamDrive", 20}, {"kParamWet", 40}})})});
  p.presets.push_back({"Sync Lead",
                       join({{{osc(0, O_Table), tbl("Basic Shapes")}, {osc(0, O_WtPos), 0.0f}, {osc(0, O_Warp1Mode), 1},
                              {osc(0, O_Warp1), 0.3f}, {G_Mono, 1}, {G_Legato, 1}, {G_PortaTime, 0.35f}},
                             env(1, 0.001, 0.6, 0.2, 0.3), mod(0, SrcEnv2, oscP(0, O_Warp1), 0.5f),
                             mod(1, SrcModWheel, oscP(0, O_Warp1), 0.4f), fxs(0, FxDelay, {{"kParamWet", 22}}),
                             fxs(1, FxReverb, {{"kParamWet", 18}})})});
  p.presets.push_back({"Chaos Texture",
                       join({{{osc(0, O_Table), tbl("Digital Growl")}, {osc(1, O_Enable), 1}, {osc(1, O_Table), tbl("Vocal Formants")},
                              {routeP(RtB, R_Dest), RouteFilter}, {filtP(0, F_Enable), 1}, {filtP(0, F_Type), ft("Comb +")},
                              {filtP(0, F_Res), 0.6f}, {lfoP(0, L_Type), LfoLorenz}, {lfoP(0, L_Bpm), 0}, {lfoP(0, L_Rate), 0.25f},
                              {lfoP(0, L_Mode), LfoFree}},
                             env(0, 0.4, 1.0, 0.8, 1.5), mod(0, SrcLfo1, oscP(0, O_WtPos), 0.6f, true),
                             mod(1, SrcLfo1Y, filtP(0, F_Freq), 0.3f, true),
                             fxs(0, FxReverb, {{"kParamWet", 40}, {"kParamType", 3}, {"kParamSize", 90}})})});
  p.presets.push_back({"Clean Keys",
                       join({{{osc(0, O_Table), tbl("Electric Keys")}, {osc(0, O_WtPos), 0.2f}, {G_Poly, 16}},
                             env(0, 0.002, 1.2, 0.35, 0.6), mod(0, SrcVelo, oscP(0, O_WtPos), 0.3f),
                             fxs(0, FxPhaser, {{"kParamWet", 30}}), fxs(1, FxReverb, {{"kParamWet", 20}})})});
  p.presets.push_back({"Tape Lo-Fi Chords",
                       join({{{osc(0, O_Table), tbl("Analog Morph")}, {osc(0, O_WtPos), 0.5f}, {osc(0, O_Unison), 3},
                              {osc(0, O_UniDetune), 0.2f}, {filtP(0, F_Enable), 1}, {filtP(0, F_Type), lp12}, {filtP(0, F_Freq), 0.62f},
                              {G_RndDetune, 6}, {G_Poly, 10}},
                             env(0, 0.01, 1.0, 0.7, 0.5), mod(0, SrcLfo1, oscP(0, O_Fine), 0.08f, true),
                             PV{{lfoP(0, L_Rate), 0.3f}, {lfoP(0, L_Mode), LfoFree}, {lfoP(0, L_Shape), 2}},
                             fxs(0, FxDistortion, {{"kParamMode", 15}, {"kParamDrive", 25}, {"kParamWet", 50}}),
                             fxs(1, FxEq, {{"kParamGain2", -6}}), fxs(2, FxReverb, {{"kParamWet", 25}, {"kParamType", 2}})})});
  p.presets.push_back({"Frequency Shift FX",
                       join({{{osc(0, O_Table), tbl("Digital Growl")}, {osc(0, O_WtPos), 0.5f}},
                             env(0, 0.01, 0.8, 0.6, 0.8),
                             fxs(0, FxBode, {{"kParamShift", 20}, {"kParamFeedback", 50}, {"kParamDelayTime", 0.12}, {"kParamWet", 60}}),
                             fxs(1, FxReverb, {{"kParamWet", 25}})})});
  v.push_back(std::move(p));
}

}  // namespace gl::packs::plasma
