// Gridline — "dynamics" pack, J-42: Pocket Rack catalogue entry (gl.pocketrack).
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>
#include "../Packs.h"
#include "dynamics.h"

namespace gl::packs::dynamics {

namespace {

struct TypeDef {
  int id;
  const char* name;
  const char* params;  // '|'-separated names of module parameters 1..n
  std::vector<float> defaults;  // parameters 1..n
};

const std::vector<TypeDef>& types() {
  static const std::vector<TypeDef> t = {
      {3, "Four-band EQ",
       "Gain 1|Freq 1|Width 1|Kind 1|Gain 2|Freq 2|Width 2|Kind 2|Gain 3|Freq 3|Width 3|Kind 3|Gain 4|Freq 4|Width 4|Kind 4|Side 1|Side 2|Side 3|Side 4",
       {0.5f, 0.2f, 0.5f, 0.51f, 0.5f, 0.4f, 0.5f, 0.51f, 0.5f, 0.6f, 0.5f, 0.51f, 0.5f, 0.8f, 0.5f, 0.51f, 0, 0, 0, 0}},
      {4, "X/Y Multi", "Kind|X|Y|LFO rate|LFO depth|LFO wave", {0, 0.5f, 0.5f, 0.61f, 0, 0}},
      {6, "Drive", "Drive|Crush|Level|Balance|Curve|Bias", {0.25f, 0, 1, 0.5f, 0, 0.5f}},
      {7, "Eight-band EQ", "80 Hz|160 Hz|375 Hz|750 Hz|1.5 kHz|3 kHz|6 kHz|12 kHz", {0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f}},
      {8, "Compressor", "Threshold|Ratio|Attack|Release|Makeup|Hold|Auto level|Input|RMS", {0.499f, 0, 0.6f, 0.6f, 0.5001f, 0, 1, 0.5f, 0}},
      {9, "Filter", "Cutoff|Resonance|Mode|Slope", {0.7f, 0, 0, 0.5f}},
      {11, "Chorus", "Rate|Depth|Spread|Mix", {0.4f, 0.6f, 1, 0.6f}},
      {13, "Flanger", "Synced rate|Sync|Rate|Wave|Depth|Delay|Feedback|Invert feedback|Spread|Mix|Invert mix",
       {0.4f, 0, 0.55f, 0, 1, 0.25f, 0.75f, 1, 0, 1, 1}},
      {14, "Phaser", "Synced rate|Depth|Feedback|Centre|Mix|Sync|Rate|Wave|Spread", {0.11222f, 0.25f, 0.1f, 0.25f, 1, 1, 0, 0, 1}},
      {15, "Widener", "Amount|Colour|Focus|Haas|Balance", {0.7f, 0.52f, 0.75f, 0.5f, 0.5f}},
      {16, "Bounce Delay", "Time|Feedback|Mix|Tone|Routing|Input balance", {0.775f, 0.4f, 1, 0.5f, 0, 0.5f}},
      {18, "Tape Echo", "Time|Feedback|Offset|Mix", {0.6f, 0.6f, 0.5f, 0.4f}},
      {20, "Gain & Balance", "Gain|Balance", {0.73796f, 0.5f}},
      {24, "Tuner", "", {}},
      {26, "Analyser", "", {}},
      {27, "Gate", "Threshold|Hysteresis|Attack|Attack curve|Release|Release curve|Floor|Hold", {0, 0, 0.01f, 0, 0.25f, 1, 1, 0.25f}},
      {28, "Limiter", "Input|Output|Release|Ceiling", {0.5f, 0.5f, 0.2f, 1}},
      {29, "Beat Duck", "Depth|Attack|Curve|Length|Offset|Hold|Shape", {0.501f, 0.1f, 0.501f, 1, 0.5f, 0, 0}},
      {30, "Key EQ", "Band 1|Band 2|Band 3|Band 4|Band 5|Band 6|Band 7|Band 8|Key|Width",
       {0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0, 0.6f}},
      {34, "Stereo Tools", "L/R delay|Balance|Width|Invert L/R|M/S delay|Invert M/S|Mode", {0.5f, 0.5f, 0.5f, 0, 0.5f, 0, 0}},
      {35, "Hall", "High cut|Low cut|Size|Decay|Diffusion|Damping|Dry|Early|Late|Side input|Pre-delay|Tempo pre-delay|Motion|Bass|Width|Motion rate|Crossover|Balance",
       {0.16f, 0.0187f, 0.5f, 0.068f, 1, 0.16f, 0.8f, 0.8f, 0.56f, 0, 0, 0, 0, 0.28f, 0.5f, 0.33f, 0.2405f, 0.5f}},
      {36, "Three-band Comp",
       "Master|Split 1|Split 2|Listen|Parallel|Low in|Low threshold|Low ratio|Low attack|Low release|Low out|Low state|Mid in|Mid threshold|Mid "
       "ratio|Mid attack|Mid release|Mid out|Mid state|High in|High threshold|High ratio|High attack|High release|High out|High state|Low "
       "width|Mid width|High width|Width mode",
       {0.5f, 0.3f, 0.7f, 0, 0, 0.5f, 1, 0, 0.3f, 0.3f, 0.5f, 0, 0.5f, 1, 0, 0.3f, 0.3f, 0.5f, 0, 0.5f, 1, 0, 0.3f, 0.3f, 0.5f, 0, 0.5f, 0.5f, 0.5f, 0}},
      {40, "Comb", "Source|Pitch|Feedback|Mix|Octave|Glide|Snap|Polarity|Skew|Octave note|Note", {0, 0.33f, 0.8f, 0.75f, 0.5f, 0.25f, 1, 0, 0.5f, 0.5f, 0.4f}},
      {41, "Shaper", "Curve|Auto level|X|Y|Drive|Output|Spread|Mix|High cut|Low cut|Bass|Mid freq|Mid|Treble|LFO wave|LFO sync|LFO synced rate|LFO rate|LFO to X|LFO to Y",
       {0, 0, 0.5f, 0.5f, 0, 0.5f, 0, 1, 1, 0, 0.5f, 0.5f, 0.5f, 0.5f, 0, 0, 0.5f, 0, 0, 0}},
      {42, "Tape Brake", "Stop time|Start time|Stop (sync)|Start (sync)|Running|Curve|Sync", {0.2f, 0.2f, 0.33333f, 0.33333f, 1, 0.5f, 0}},
      {43, "Wobble", "Slow rate|Slow (sync)|Slow depth|Fast rate|Fast (sync)|Fast depth|Sync|Drift rate|Drift (sync)|Drift",
       {0.184564f, 0.2f, 0.25f, 0.066818f, 0.1f, 0.25f, 0, 0.08952f, 0.8f, 0.25f}},
      {44, "Wide Bass", "Width|Split|Mono lows|Separation", {0, 0, 0, 0.5f}},
      {45, "Pitch Fix", "C|C#|D|D#|E|F|F#|G|G#|A|A#|B|Speed|-|-|Formants|Formant shift|Channel|Stability|Humanize|Snap|Mix|Sensitivity|Vibrato|Latency|Sibilants",
       {1, 0, 1, 1, 0, 1, 0, 1, 1, 0, 1, 0, 0.5f, 0, 0, 0, 0.5f, 1, 0.5f, 0, 0, 1, 0.5f, 0, 0, 0}},
      {46, "Pulse Gain", "Rate|Synced rate|Upper|Lower|Phase|Stereo phase|Sync|Shape|Smooth|Skew|Width", {0.04547f, 0, 1, 0, 0, 0, 0, 0.5f, 1, 0.5f, 0.5f}},
  };
  return t;
}
const TypeDef* typeDef(int id) {
  for (const TypeDef& d : types())
    if (d.id == id) return &d;
  return nullptr;
}

}  // namespace

const char* pocketModuleName(int type) {
  if (type == 0) return "Empty";
  const TypeDef* d = typeDef(type);
  return d ? d->name : "";
}
void pocketModuleDefaults(int type, float* out) {
  for (int i = 0; i < kPkParams; ++i) out[i] = 0;
  out[0] = 1;
  if (const TypeDef* d = typeDef(type))
    for (size_t i = 0; i < d->defaults.size() && i + 1 < static_cast<size_t>(kPkParams); ++i) out[i + 1] = d->defaults[i];
}
int pocketModuleParamCount(int type) {
  const TypeDef* d = typeDef(type);
  return d ? std::min(kPkParams, 1 + static_cast<int>(d->defaults.size())) : 1;
}
const char* pocketParamName(int type, int index) {
  static thread_local std::string buf;
  if (index == 0) return "On";
  const TypeDef* d = typeDef(type);
  if (!d) return nullptr;
  const char* p = d->params;
  for (int k = 1; *p; ++k) {
    const char* e = std::strchr(p, '|');
    const size_t n = e ? static_cast<size_t>(e - p) : std::strlen(p);
    if (k == index) {
      buf.assign(p, n);
      return buf == "-" ? nullptr : buf.c_str();
    }
    if (!e) break;
    p = e + 1;
  }
  return nullptr;
}

void appendPocketInfo(std::vector<PluginInfo>& v) {
  PluginInfo p{"gl.pocketrack", "Pocket FX", "Multi FX", PluginKind::Effect, {}, false, {}};
  p.params.resize(PkCount);
  std::vector<std::string> names;
  for (int t = 0; t < kPkTypes; ++t) {
    const char* n = pocketModuleName(t);
    names.push_back(*n ? n : "-");
  }
  for (int s = 0; s < kPkSlots; ++s) {
    const std::string sl = "Slot " + std::to_string(s + 1) + " ";
    p.params[static_cast<size_t>(pkParam(s, PkType))] =
        ParamDesc{sl + "module", "", 0, kPkTypes - 1.0, s == 0 ? 18.0 : 0.0, ParamKind::Choice, ParamScale::Linear, names};
    float def[kPkParams];
    pocketModuleDefaults(s == 0 ? 18 : 0, def);
    for (int i = 0; i < kPkParams; ++i)
      p.params[static_cast<size_t>(pkParam(s, PkP0 + i))] =
          ParamDesc{sl + (i == 0 ? std::string("on") : "control " + std::to_string(i)), "%", 0, 1, def[i], i == 0 ? ParamKind::Toggle : ParamKind::Continuous,
                    ParamScale::Linear, {}};
    p.params[static_cast<size_t>(pkParam(s, PkPost))] = ParamDesc{sl + "post gain", "%", 0, 1, 0.5, ParamKind::Continuous, ParamScale::Linear, {}};
    p.params[static_cast<size_t>(pkParam(s, PkMix))] = ParamDesc{sl + "mix", "%", 0, 1, 1, ParamKind::Continuous, ParamScale::Linear, {}};
    p.params[static_cast<size_t>(pkParam(s, PkPan))] = ParamDesc{sl + "wet balance", "%", 0, 1, 0.5, ParamKind::Continuous, ParamScale::Linear, {}};
  }
  auto slot = [](int s, int type) {
    std::vector<std::pair<int, float>> v;
    float d[kPkParams];
    pocketModuleDefaults(type, d);
    v.push_back({pkParam(s, PkType), static_cast<float>(type)});
    for (int i = 0; i < kPkParams; ++i) v.push_back({pkParam(s, PkP0 + i), d[i]});
    return v;
  };
  auto join = [](std::vector<std::pair<int, float>> a, const std::vector<std::pair<int, float>>& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
  };
  auto tweak = [](std::vector<std::pair<int, float>> a, int k, float v) {
    a.push_back({k, v});
    return a;
  };
  p.presets = {
      {"Tape echo", {}},
      {"Chorus into hall", join(slot(0, 11), slot(1, 35))},
      {"Beat duck", slot(0, 29)},
      {"Warm drive + limiter", join(tweak(slot(0, 6), pkParam(0, PkP0 + 1), 0.45f), slot(1, 28))},
      {"Wobbly tape", join(slot(0, 43), slot(1, 18))},
      {"Filter sweep", tweak(slot(0, 9), pkParam(0, PkP0 + 1), 0.45f)},
  };
  p.editorWidth = 760;
  v.push_back(p);
}

}  // namespace gl::packs::dynamics
