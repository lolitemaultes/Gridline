// Gridline — "plasma" pack: factory presets that carry data (drawn tables,
// drawn LFO shapes, clips, arp patterns, scalar curves). Original sounds made
// for Gridline.
#include <cmath>
#include "plasma.h"

namespace gl::packs::plasma {

namespace {
using PV = std::vector<std::pair<int, float>>;

int table(const char* name) {
  const auto& n = factoryTableNames();
  for (size_t i = 0; i < n.size(); ++i)
    if (n[i] == name) return static_cast<int>(i + 1);
  return 1;
}
int filterType(const char* name) {
  const auto& n = filterNames();
  for (size_t i = 0; i < n.size(); ++i)
    if (n[i] == name) return static_cast<int>(i);
  return 1;
}
void envVals(PV& v, int e, double a, double d, double s, double r) {
  v.push_back({envP(e, E_Attack), static_cast<float>(envTimeInv(a))});
  v.push_back({envP(e, E_Decay), static_cast<float>(envTimeInv(d))});
  v.push_back({envP(e, E_Sustain), static_cast<float>(s)});
  v.push_back({envP(e, E_Release), static_cast<float>(envTimeInv(r))});
}
void modVals(PV& v, int slot, int src, int dest, float amount) {
  v.push_back({modP(slot, M_Source), static_cast<float>(src)});
  v.push_back({modP(slot, M_Dest), static_cast<float>(dest + 1)});
  v.push_back({modP(slot, M_Amount), amount});
}
void fxVals(PV& v, Model& m, int slot, int type, std::initializer_list<std::pair<const char*, double>> plain) {
  v.push_back({fxP(slot, X_Type), static_cast<float>(type)});
  for (int q = 0; q < static_cast<int>(fxParams(type).size()); ++q) v.push_back({fxP(slot, X_P0 + q), static_cast<float>(fxDefaultNorm(type, q))});
  for (const auto& [key, val] : plain) {
    const int q = fxParamIndex(type, key);
    if (q >= 0) v.push_back({fxP(slot, X_P0 + q), static_cast<float>(fxNorm(type, q, val))});
  }
  m.fx.push_back({slot, 0, -1, 0});
}

DataPreset clipGroove() {
  DataPreset p{"Clip Groove", {}, {}};
  Model m = defaultModel();
  PV& v = p.values;
  v = {{oscP(0, O_Table), static_cast<float>(table("Analog Morph"))}, {oscP(0, O_WtPos), 0.2f},
       {filtP(0, F_Enable), 1}, {filtP(0, F_Type), static_cast<float>(filterType("Low 24"))}, {filtP(0, F_Freq), 0.45f},
       {filtP(0, F_Res), 0.35f}, {G_ClipOn, 1}, {G_Mono, 1}};
  envVals(v, 0, 0.001, 0.25, 0.4, 0.06);
  envVals(v, 1, 0.001, 0.18, 0.0, 0.08);
  modVals(v, 0, SrcEnv2, filtP(0, F_Freq), 0.4f);
  fxVals(v, m, 0, FxDelay, {{"kParamWet", 18}});
  ClipSlot& c = m.clip[0];
  c.name = "Groove";
  c.lengthBeats = 4;
  const int keys[16] = {36, 0, 48, 36, 0, 43, 36, 0, 39, 0, 36, 48, 0, 41, 43, 0};
  for (int i = 0; i < 16; ++i)
    if (keys[i]) c.notes.push_back({i * 0.25f, 0.2f, keys[i], i % 4 == 0 ? 1.0f : 0.7f, false});
  c.span = 3;  // follows the played key
  p.data = modelToData(m);
  return p;
}

DataPreset drawnWobble() {
  DataPreset p{"Drawn Wobble", {}, {}};
  Model m = defaultModel();
  PV& v = p.values;
  v = {{oscP(0, O_Table), static_cast<float>(table("Digital Growl"))}, {oscP(0, O_Octave), -1}, {oscP(0, O_Unison), 3},
       {oscP(0, O_UniDetune), 0.2f}, {filtP(0, F_Enable), 1}, {filtP(0, F_Type), static_cast<float>(filterType("Low 24"))},
       {filtP(0, F_Freq), 0.25f}, {filtP(0, F_Res), 0.4f}, {subP(S_Enable), 1}, {subP(S_Octave), -1}, {G_Mono, 1},
       {lfoP(0, L_Rate), 0.45f}};
  envVals(v, 0, 0.002, 1.0, 1.0, 0.1);
  modVals(v, 0, SrcLfo1, filtP(0, F_Freq), 0.55f);
  modVals(v, 1, SrcLfo1, oscP(0, O_WtPos), 0.4f);
  fxVals(v, m, 0, FxDistortion, {{"kParamDrive", 35}, {"kParamWet", 60}});
  LfoData& l = m.lfo[0];
  l.name = "Stutter";
  l.pts = {{0, 0, 0}, {0.12f, 1, -0.6f}, {0.25f, 0.1f, 0.5f}, {0.375f, 0.8f, 0}, {0.5f, 0.0f, 0}, {0.75f, 1, 0.7f}, {1, 0, -0.3f}};
  p.data = modelToData(m);
  return p;
}

DataPreset drawnTable() {
  DataPreset p{"Drawn Glass", {}, {}};
  Model m = defaultModel();
  PV& v = p.values;
  v = {{oscP(0, O_Table), 0.0f}, {oscP(0, O_WtPos), 0.0f}, {G_Poly, 8}, {lfoP(0, L_Rate), 0.25f}, {lfoP(0, L_Mode), LfoFree}, {lfoP(0, L_Shape), 2}};
  envVals(v, 0, 0.003, 1.8, 0.25, 1.4);
  modVals(v, 0, SrcEnv2, oscP(0, O_WtPos), 0.8f);
  envVals(v, 1, 0.001, 1.2, 0.0, 1.0);
  fxVals(v, m, 0, FxReverb, {{"kParamWet", 30}, {"kParamSize", 70}});
  OscData& o = m.osc[0];
  o.table = "custom";
  o.tableName = "Glass Partials";
  const int frames = 8;
  const double ratios[] = {1, 2.0, 3.0, 4.2, 5.4, 6.8};
  o.frames.assign(frames, std::vector<float>(kFrameSize, 0.f));
  for (int f = 0; f < frames; ++f) {
    const double bright = static_cast<double>(f) / (frames - 1);
    float peak = 1e-6f;
    for (int i = 0; i < kFrameSize; ++i) {
      double s = 0;
      for (int h = 0; h < 6; ++h) {
        const int harm = static_cast<int>(std::lround(ratios[h]));
        const double a = h == 0 ? 1.0 : bright * 0.7 / (h + 0.5);
        s += a * std::sin(6.283185307179586 * harm * i / kFrameSize);
      }
      o.frames[static_cast<size_t>(f)][static_cast<size_t>(i)] = static_cast<float>(s);
      peak = std::max(peak, static_cast<float>(std::fabs(s)));
    }
    for (float& x : o.frames[static_cast<size_t>(f)]) x /= peak;
  }
  p.data = modelToData(m);
  return p;
}

DataPreset patternArp() {
  DataPreset p{"Pattern Arp", {}, {}};
  Model m = defaultModel();
  PV& v = p.values;
  v = {{oscP(0, O_Table), static_cast<float>(table("Square PWM"))}, {oscP(0, O_WtPos), 0.4f}, {oscP(0, O_Unison), 2},
       {oscP(0, O_UniDetune), 0.15f}, {filtP(0, F_Enable), 1}, {filtP(0, F_Freq), 0.55f}, {G_ArpOn, 1}};
  envVals(v, 0, 0.001, 0.25, 0.3, 0.12);
  fxVals(v, m, 0, FxDelay, {{"kParamWet", 22}});
  fxVals(v, m, 1, FxReverb, {{"kParamWet", 15}});
  ArpSlot& a = m.arp[0];
  a.name = "Pattern";
  a.shape = ArpPattern;
  a.patternBeats = 2;
  const int rows[8] = {0, 2, 1, 2, 0, 3, 1, 2};
  for (int i = 0; i < 8; ++i) a.steps.push_back({i * 0.25f, 0.2f, rows[i], i % 2 == 0 ? 1.0f : 0.75f, i == 0, 0});
  p.data = modelToData(m);
  return p;
}

DataPreset softKeys() {
  DataPreset p{"Soft Touch Keys", {}, {}};
  Model m = defaultModel();
  PV& v = p.values;
  v = {{oscP(0, O_Table), static_cast<float>(table("Electric Keys"))}, {oscP(0, O_WtPos), 0.3f}, {G_Poly, 12}};
  envVals(v, 0, 0.002, 1.5, 0.35, 0.5);
  modVals(v, 0, SrcVelo, oscP(0, O_WtPos), 0.5f);
  fxVals(v, m, 0, FxChorus, {{"kParamWet", 30}});
  m.velo = {{0, 0.15f, 0}, {1, 1, 0.4f}};  // gentle velocity response
  p.data = modelToData(m);
  return p;
}
}  // namespace

const std::vector<DataPreset>& factoryDataPresets() {
  static const std::vector<DataPreset> v = {clipGroove(), drawnWobble(), drawnTable(), patternArp(), softKeys()};
  return v;
}

}  // namespace gl::packs::plasma
