// Gridline — "models" pack, J-22 group "drums": catalogue (Membrane Pad,
// Membrane Kit, Kick Sculptor, Sweep Kick), factory presets and the kit's
// pattern / name data codec.
#include <algorithm>
#include <cstdio>
#include <sstream>
#include "../Packs.h"
#include "drums.h"

namespace gl::packs::models {

namespace dr {

namespace {
const char* const kVelDest[] = {"None",         "Mallet level", "Mallet decay", "Mallet noise", "Noise pattern", "Membrane decay",
                                "Membrane cut", "Tension",      "Strike phase", "Material",     "Size",         "Shape",
                                "Low freq",     "Low reso",     "Sine freq",    "Sine decay",   "Sine level",   "Mid freq",
                                "Mid level",    "Mid reso",     "Mid freq mod", "Mid level mod", "Mid attack",  "High freq",
                                "High level",   "High reso",    "Lo-fi",        "Hold",         "Pitch"};
// Reference file stems (import / export matching) and Gridline's own names of
// the synthesised layers that stand at the same indices.
const char* const kNoise[] = {"Evolving 1", "Evolving 2", "H-Freq Metal", "Hard Clap", "Lo-Fi Crunch", "Low Bit", "Metallic", "Soft Clap"};
const char* const kNoiseLabel[] = {"Swept hiss", "Dark sweep", "Bright metal", "Hard clap", "Crunch", "Low bits", "Ring metal", "Soft clap"};
static_assert(sizeof kNoise == sizeof kNoiseLabel, "one label per noise");
}  // namespace

int velDestCount() { return static_cast<int>(sizeof kVelDest / sizeof kVelDest[0]); }
const char* velDestName(int k) { return kVelDest[std::clamp(k, 0, velDestCount() - 1)]; }
int velDestParam(int k) { return k <= 0 || k >= velDestCount() ? -1 : k + 1; }  // 1 = mallet level (model 2) … 28 = pitch (29)

int kickNoiseCount() { return static_cast<int>(sizeof kNoise / sizeof kNoise[0]); }
const char* kickNoiseName(int k) { return kNoise[std::clamp(k, 0, kickNoiseCount() - 1)]; }
const char* kickNoiseLabel(int k) { return kNoiseLabel[std::clamp(k, 0, kickNoiseCount() - 1)]; }
int kickNoiseIndex(const std::string& path) {
  size_t s = path.find_last_of("/\\");
  std::string file = s == std::string::npos ? path : path.substr(s + 1);
  for (int k = 0; k < kickNoiseCount(); ++k)
    if (file == std::string(kNoise[k]) + ".wav") return k;
  return -1;
}

// Kit data as text lines: "kit <name>", "pad <k> <name>", "pat <k> <length> <key> <hex cells>", "link <hex>".
std::string encodeKitData(const KitData& d) {
  static const char* hx = "0123456789abcdef";
  std::string s = "kit " + d.kitName + "\n";
  for (int p = 0; p < kKitPads; ++p) s += "pad " + std::to_string(p) + " " + d.padName[p] + "\n";
  for (int k = 0; k < kKitPatterns; ++k) {
    const KitPattern& P = d.pattern[k];
    s += "pat " + std::to_string(k) + " " + std::to_string(P.length) + " " + std::to_string(P.key) + " ";
    for (int ch = 0; ch < kKitPads; ++ch)
      for (int st = 0; st < kKitSteps; ++st) {
        s += hx[P.cell[ch][st] >> 4];
        s += hx[P.cell[ch][st] & 15];
      }
    s += "\n";
  }
  s += "link ";
  for (uint8_t b : d.link) {
    s += hx[b >> 4];
    s += hx[b & 15];
  }
  s += "\n";
  return s;
}

namespace {
int hexv(char c) { return c >= '0' && c <= '9' ? c - '0' : (c >= 'a' && c <= 'f' ? c - 'a' + 10 : (c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1)); }
bool hexBytes(const std::string& h, uint8_t* out, size_t n) {
  if (h.size() < 2 * n) return false;
  for (size_t i = 0; i < n; ++i) {
    int a = hexv(h[2 * i]), b = hexv(h[2 * i + 1]);
    if (a < 0 || b < 0) return false;
    out[i] = static_cast<uint8_t>(a * 16 + b);
  }
  return true;
}
}  // namespace

bool decodeKitData(const std::string& s, KitData& d) {
  d = KitData{};
  for (auto& P : d.pattern)
    for (auto& row : P.cell) std::fill(std::begin(row), std::end(row), uint8_t{0x7F});
  std::istringstream in(s);
  std::string line;
  bool any = false;
  while (std::getline(in, line)) {
    if (line.rfind("kit ", 0) == 0) {
      d.kitName = line.substr(4);
      any = true;
    } else if (line.rfind("pad ", 0) == 0) {
      size_t sp = line.find(' ', 4);
      int k = std::atoi(line.c_str() + 4);
      if (k >= 0 && k < kKitPads) d.padName[k] = sp == std::string::npos ? "" : line.substr(sp + 1);
    } else if (line.rfind("pat ", 0) == 0) {
      std::istringstream ls(line.substr(4));
      int k = -1, len = 0, key = -1;
      std::string cells;
      ls >> k >> len >> key >> cells;
      if (k < 0 || k >= kKitPatterns) continue;
      KitPattern& P = d.pattern[k];
      P.length = std::clamp(len, 0, kKitSteps);
      P.key = key < 0 || key > 127 ? -1 : key;
      hexBytes(cells, &P.cell[0][0], kKitPads * kKitSteps);
      any = true;
    } else if (line.rfind("link ", 0) == 0) {
      hexBytes(line.substr(5), d.link, sizeof d.link);
    }
  }
  return any;
}

}  // namespace dr

namespace {

using namespace dr;

ParamDesc lin(const std::string& n, const char* u, double mn, double mx, double d) {
  return ParamDesc{n, u, mn, mx, d, ParamKind::Continuous, ParamScale::Linear, {}};
}
ParamDesc pct(const std::string& n, double d) { return lin(n, "%", 0, 1, d); }
ParamDesc tog(const std::string& n, bool d) { return ParamDesc{n, "", 0, 1, d ? 1.0 : 0.0, ParamKind::Toggle, ParamScale::Linear, {}}; }
ParamDesc ch(const std::string& n, std::vector<std::string> c, int d) {
  const double hi = static_cast<double>(c.size() - 1);
  return ParamDesc{n, "", 0, hi, static_cast<double>(d), ParamKind::Choice, ParamScale::Linear, std::move(c)};
}

// The drum model's 38 descriptors (prefix for the kit's channels). Defaults: a
// mid tom-like membrane.
void modelParams(std::vector<ParamDesc>& q, int base, const std::string& pre) {
  static const double def[MdCount] = {0, 0, 0.35, 0.5, 0.2, 0.2, 0.9, 0.35, 0.4, 0.5, 0.5, 0.35, 1, 0.45, 0.2, 0.5, 0.5, 0,
                                      0.5, 0.5, 0, 0.5, 0.5, 0.5, 0.8, 0.85, 0, 0, 1, 0.5, 1, 0, 0, 0, 0.6, 0, 0, 0};
  static const char* names[MdCount] = {"Mallet retrigger", "Mid repeat", "Mallet level", "Mallet decay", "Mallet noise", "Noise pattern",
                                       "Membrane decay",   "Membrane cutoff", "Tension",    "Strike phase", "Material",     "Size",
                                       "Shape",            "Low freq",      "Low reso",     "Sine freq",    "Sine decay",   "Sine level",
                                       "Mid freq",         "Mid level",     "Mid reso",     "Mid freq mod", "Mid level mod", "Mid attack",
                                       "High freq",        "High level",    "High reso",    "Lo-fi",        "Hold",         "Pitch",
                                       "Velocity 1 to",    "Velocity 2 to", "Velocity 3 to", "Velocity 4 to", "Velocity 1 amount",
                                       "Velocity 2 amount", "Velocity 3 amount", "Velocity 4 amount"};
  std::vector<std::string> dests;
  for (int k = 0; k < velDestCount(); ++k) dests.push_back(velDestName(k));
  for (int k = 0; k < MdCount; ++k) {
    const std::string n = pre + names[k];
    ParamDesc& d = q[static_cast<size_t>(base + k)];
    if (k == MdMalletRetrig)
      d = tog(n, def[k] > 0.5);
    else if (k == MdRetrig)
      d = ch(n, {"Off", "2x", "3x", "4x"}, static_cast<int>(def[k]));
    else if (k >= MdVelSrc1 && k <= MdVelSrc4)
      d = ch(n, dests, static_cast<int>(def[k]));
    else
      d = pct(n, def[k]);
  }
}

PluginInfo membranePad() {
  PluginInfo p{"gl.membranepad", "Membrane Pad", "Drums", PluginKind::Generator, {}, false, {}};
  p.params.resize(PdCount);
  modelParams(p.params, PdModel, "");
  p.params[PdOutput] = pct("Output", 1.0);
  p.params[PdEngine] = ch("Engine", {"Modal (J-22)", "Reference"}, 1);
  p.legacyModelParam = PdEngine;
  using V = std::vector<std::pair<int, float>>;
  auto preset = [&](const char* n, V v) { p.presets.push_back({n, std::move(v)}); };
  preset("Round Kick", {{MdMalletAmp, 0.62f}, {MdMalletDecay, 0.2f}, {MdMalletNoise, 0.3f}, {MdMemDecay, 0.92f}, {MdMemCutoff, 0.25f},
                        {MdTension, 0.3f}, {MdSize, 0.85f}, {MdLowFreq, 0.3f}, {MdLowReso, 0.35f}, {MdMidFreq, 0.35f}, {MdMidLevel, 0.8f},
                        {MdHighLevel, 0.6f}, {MdHold, 0.6f}, {MdPitch, 0.42f}});
  preset("Tight Snare", {{MdMalletAmp, 0.8f}, {MdMalletDecay, 0.6f}, {MdMalletNoise, 0.85f}, {MdMalletNoiseRp, 0.6f}, {MdMemDecay, 0.75f},
                         {MdMemCutoff, 0.45f}, {MdTension, 0.62f}, {MdMaterial, 0.7f}, {MdSize, 0.3f}, {MdLowFreq, 0.6f},
                         {MdMidFreq, 0.55f}, {MdMidLevel, 0.85f}, {MdMidRes, 0.4f}, {MdHighFreq, 0.75f}, {MdHighLevel, 0.95f}, {MdHold, 0.7f}});
  preset("Closed Tick", {{MdMalletAmp, 0.3f}, {MdMalletDecay, 0.55f}, {MdMalletNoise, 1.f}, {MdMalletNoiseRp, 0.3f}, {MdMemDecay, 0.6f},
                         {MdMemCutoff, 0.95f}, {MdTension, 0.9f}, {MdMaterial, 0.65f}, {MdSize, 0.05f}, {MdLowFreq, 0.9f},
                         {MdHighFreq, 0.85f}, {MdHighLevel, 1.f}, {MdHighRes, 0.4f}, {MdHold, 0.35f}});
  preset("Hand Drum", {{MdMalletAmp, 0.5f}, {MdMalletDecay, 0.35f}, {MdMalletNoise, 0.15f}, {MdMemDecay, 0.8f}, {MdMemCutoff, 0.4f},
                       {MdTension, 0.55f}, {MdPhase, 0.7f}, {MdMaterial, 0.4f}, {MdSize, 0.4f}, {MdLowFreq, 0.5f}, {MdMidFreqMod, 0.35f}});
  preset("Bright Bell Plate", {{MdMalletAmp, 0.45f}, {MdMalletDecay, 0.7f}, {MdMalletNoise, 0.4f}, {MdMemDecay, 0.97f},
                               {MdMemCutoff, 1.f}, {MdTension, 0.95f}, {MdMaterial, 0.95f}, {MdSize, 0.15f}, {MdLowFreq, 0.85f},
                               {MdHighLevel, 0.9f}, {MdHold, 1.f}});
  p.editorWidth = 794;
  return p;
}

PluginInfo membraneKit() {
  PluginInfo p{"gl.membranekit", "Membrane Kit", "Drums", PluginKind::Generator, {}, false, {}};
  p.params.resize(KmCount);
  for (int c = 0; c < kKitPads; ++c) {
    const std::string pre = "Ch " + std::to_string(c + 1) + " ";
    p.params[static_cast<size_t>(kitPad(c, KpVolume))] = pct(pre + "volume", 0.7);
    p.params[static_cast<size_t>(kitPad(c, KpPan))] = pct(pre + "pan", 0.5);
    p.params[static_cast<size_t>(kitPad(c, KpMute))] = tog(pre + "mute", c >= 8);
    p.params[static_cast<size_t>(kitPad(c, KpSolo))] = tog(pre + "solo", false);
    ParamDesc key = lin(pre + "key", "note", 0, 127, 60 + c);  // C5 upwards: a fresh kit answers the rack's default note
    p.params[static_cast<size_t>(kitPad(c, KpKey))] = key;
    modelParams(p.params, kitPad(c, KpModel), pre);
    p.params[static_cast<size_t>(kitPad(c, KpSpare))] = pct(pre + "reserved", 0);
  }
  p.params[KmReserved] = pct("Reserved A", 0);
  p.params[KmLevel] = pct("Master level", 0.5);
  p.params[KmSwing] = pct("Swing", 0);
  p.params[KmEqOn] = tog("EQ on", false);
  for (int k = 0; k < 8; ++k) p.params[static_cast<size_t>(KmEq1 + k)] = pct("EQ band " + std::to_string(k + 1), 0.5);
  p.params[KmLimiter] = tog("Limiter on", false);
  p.params[KmLimGain] = pct("Limiter gain", 0);
  p.params[KmLimRelease] = pct("Limiter release", 0.5);
  p.params[KmReserved2] = pct("Reserved B", 0);
  // A basic 8-piece starting kit: kick, snare, closed / open hat, three toms, clap.
  using V = std::vector<std::pair<int, float>>;
  auto pad = [](V& v, int c, std::initializer_list<std::pair<int, float>> m) {
    for (const auto& kv : m) v.push_back({kitPad(c, KpModel + kv.first), kv.second});
  };
  V base;
  pad(base, 0, {{MdMalletDecay, 0.2f}, {MdMemDecay, 0.9f}, {MdMemCutoff, 0.25f}, {MdTension, 0.3f}, {MdSize, 0.85f}, {MdLowFreq, 0.3f}, {MdPitch, 0.42f}});
  pad(base, 1, {{MdMalletAmp, 0.8f}, {MdMalletNoise, 0.85f}, {MdMemDecay, 0.75f}, {MdTension, 0.62f}, {MdSize, 0.3f}, {MdLowFreq, 0.6f}, {MdHold, 0.7f}});
  pad(base, 2, {{MdMalletNoise, 1.f}, {MdMemCutoff, 0.95f}, {MdTension, 0.9f}, {MdSize, 0.05f}, {MdLowFreq, 0.9f}, {MdHold, 0.3f}});
  pad(base, 3, {{MdMalletNoise, 1.f}, {MdMemCutoff, 0.95f}, {MdTension, 0.9f}, {MdSize, 0.06f}, {MdLowFreq, 0.9f}, {MdHold, 0.8f}});
  pad(base, 4, {{MdSize, 0.4f}, {MdTension, 0.5f}});
  pad(base, 5, {{MdSize, 0.5f}, {MdTension, 0.42f}});
  pad(base, 6, {{MdSize, 0.62f}, {MdTension, 0.35f}});
  pad(base, 7, {{MdMalletNoise, 1.f}, {MdMalletDecay, 0.75f}, {MdRetrig, 1.f}, {MdMemDecay, 0.6f}, {MdMemCutoff, 0.7f}, {MdSize, 0.2f}, {MdHold, 0.5f}});
  p.presets.push_back({"Studio Kit", base});
  V tight = base;
  for (int c = 0; c < 8; ++c) tight.push_back({kitPad(c, KpModel + MdHold), 0.45f});
  tight.push_back({KmLimiter, 1.f});
  tight.push_back({KmLimGain, 0.4f});
  p.presets.push_back({"Tight Room Kit", tight});
  V lofi = base;
  for (int c = 0; c < 8; ++c) lofi.push_back({kitPad(c, KpModel + MdLofi), 0.45f});
  lofi.push_back({KmEqOn, 1.f});
  lofi.push_back({KmEq1, 0.7f});
  lofi.push_back({KmEq8, 0.3f});
  p.presets.push_back({"Crushed Kit", lofi});
  p.editorWidth = 762;
  return p;
}

PluginInfo kickSculpt() {
  PluginInfo p{"gl.kicksculpt", "Kick Sculptor", "Drums", PluginKind::Generator, {}, false, {}};
  auto& q = p.params;
  q.resize(KsCount);
  q[KsLevel] = pct("Level", 0.514);
  q[KsSampling] = pct("Sampling ratio", 0.47);
  q[KsDuration] = pct("Duration", 0.65);
  q[KsSub] = pct("Sub bass", 0.7);
  q[KsDrive] = pct("Drive", 0.45);
  q[KsAttack] = pct("Attack", 0);
  q[KsBase] = pct("Base", 0.4);
  q[KsPeak] = pct("Peak", 0.4);
  q[KsSlide] = pct("Slide time", 0.3);
  q[KsVarBase] = pct("Vary base", 0.5);
  q[KsVarPeak] = pct("Vary peak", 0.4);
  q[KsVarSlide] = pct("Vary slide", 0.6);
  q[KsVarPhase] = pct("Vary phase", 0.2);
  q[KsClickCut] = pct("Click cut", 0.75);
  q[KsClickFreq] = pct("Click tone", 0.15);
  q[KsClickLevel] = pct("Click level", 1.0);
  q[KsNoiseA] = pct("Noise attack", 0.25);
  q[KsNoiseD] = pct("Noise decay", 0.55);
  q[KsNoiseS] = pct("Noise sustain", 0.6);
  q[KsNoiseStart] = pct("Noise start", 0.6);
  q[KsNoisePitch] = pct("Noise pitch", 0.3);
  q[KsNoiseMix] = pct("Noise mix", 0.2);
  q[KsNoiseFilter] = pct("Noise filter", 1.0);
  std::vector<std::string> nz;
  for (int k = 0; k < kickNoiseCount(); ++k) nz.push_back(kickNoiseLabel(k));
  q[KsNoiseSample] = ch("Noise sample", nz, 6);
  using V = std::vector<std::pair<int, float>>;
  p.presets.push_back({"Deep Sub Kick", V{{KsDuration, 0.75f}, {KsSub, 1.f}, {KsBase, 0.3f}, {KsPeak, 0.42f}, {KsSlide, 0.35f}, {KsNoiseMix, 0.05f}}});
  p.presets.push_back({"Punch Kick", V{{KsDuration, 0.55f}, {KsDrive, 0.6f}, {KsBase, 0.42f}, {KsPeak, 0.5f}, {KsSlide, 0.25f}, {KsClickFreq, 0.4f}}});
  p.presets.push_back({"Clap Layer Kick", V{{KsNoiseSample, 3.f}, {KsNoiseMix, 0.55f}, {KsNoiseD, 0.4f}, {KsNoiseFilter, 0.8f}}});
  p.presets.push_back({"Long Boom", V{{KsDuration, 0.9f}, {KsBase, 0.2f}, {KsPeak, 0.38f}, {KsSlide, 0.45f}, {KsDrive, 0.3f}}});
  p.editorWidth = 389 * 1.4f;
  return p;
}

PluginInfo sweepKick() {
  PluginInfo p{"gl.sweepkick", "Sweep Kick", "Drums", PluginKind::Generator, {}, false, {}};
  auto& q = p.params;
  q.resize(SkCount);
  q[SkMaxFreq] = lin("Max freq", "ct", -2400, 3600, 0);
  q[SkMinFreq] = lin("Min freq", "ct", -1200, 1200, -600);
  q[SkFreqDecay] = lin("Freq decay", "", 0, 256, 64);
  q[SkAmpDecay] = lin("Amp decay", "", 0, 256, 128);
  q[SkClick] = lin("Click", "", 0, 64, 32);
  q[SkDist] = lin("Distortion", "", 0, 128, 0);
  q[SkSeed] = lin("Seed", "", 0, 16777215, 4479715);
  using V = std::vector<std::pair<int, float>>;
  p.presets.push_back({"Boom", V{{SkMaxFreq, -1800}, {SkMinFreq, -900}, {SkFreqDecay, 150}, {SkAmpDecay, 170}, {SkClick, 20}}});
  p.presets.push_back({"Snap", V{{SkMaxFreq, 900}, {SkMinFreq, -500}, {SkFreqDecay, 40}, {SkAmpDecay, 90}, {SkClick, 60}}});
  p.presets.push_back({"Dirty", V{{SkMaxFreq, 0}, {SkMinFreq, -700}, {SkFreqDecay, 80}, {SkAmpDecay, 140}, {SkClick, 30}, {SkDist, 90}}});
  p.editorWidth = 380 * 1.8f;
  return p;
}

}  // namespace

void appendDrumsInfos(std::vector<PluginInfo>& v) {
  v.push_back(membranePad());
  v.push_back(membraneKit());
  v.push_back(kickSculpt());
  v.push_back(sweepKick());
}

}  // namespace gl::packs::models
