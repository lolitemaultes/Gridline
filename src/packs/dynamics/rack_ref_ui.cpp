// Gridline — "dynamics" pack, J-35: editors of the Amp Rack's reference
// models. Module rack (RkModel 1): four module panels side by side (kind,
// processor, oversampling, the processor's own controls, input / mix / output),
// rack strip with the soft clip. Pedal board (RkModel 2): five boxes (type,
// power, four knobs), input / output, cabinet and the eight-band EQ. Names
// and wording are Gridline's own.
#include "../flc_kit.h"
#include "dyn_widgets.h"
#include "modrack_ref.h"
#include "pedal_ref.h"

namespace gl::packs::dynamics {

using namespace ui;

namespace {

const skin::Skin& refRackSkin() {
  static const skin::Skin k = [] {
    skin::Skin s = skin::Skin::fl(0x2E3134, 0x3B4044, 0x23272A, 0xE8743B, 0x4B5155, 0xC8CDD0);
    s.select = skin::hex(0xE8743B);
    s.lcdText = skin::hex(0xFFB38A);
    s.pillBg = skin::hex(0x23272A);
    return s;
  }();
  return k;
}

const char* const kKinds[4] = {"Distortion", "Filter", "Chorus", "Cabinet"};
const char* const kAlgos[18] = {"Tube drive", "Hard clip", "Soft clip", "Tanh",       "Exp curve", "Flat top", "Cubic",     "Log",      "Soft sat",
                                "Elastic",    "Band",      "Sine crush", "Bit crush", "Bit jam",   "Folder",   "Aperture",  "Abrasive", "Crusher"};
const char* const kFilters[22] = {"LP 6",  "HP 6",     "LP 12",    "HP 12",    "BP 12",     "LP 24",      "HP 24", "Notch",
                                  "LP 12 B", "BP 12 B", "HP 12 B", "LP 24 B", "Peak",     "Comb +",     "Comb -", "Vowel",
                                  "Phase 1", "Phase 2", "Phase 3", "Low shelf", "High shelf", "All pass"};
struct KnobSpec {
  int p;
  const char* label;
};
// Controls of a distortion algorithm (raw parameter, label).
int algoKnobs(int algo, KnobSpec* out) {
  static const KnobSpec od[6] = {{0, "Band"}, {1, "Colour"}, {2, "Drive"}, {3, "Level"}, {4, "Tone"}, {5, "Boost"}};
  static const KnobSpec fd[5] = {{16, "Pre"}, {17, "Thres"}, {18, "Post"}, {19, "Mix"}, {20, "Type"}};
  static const KnobSpec sc[2] = {{21, "Thres"}, {22, "Post"}};
  static const KnobSpec sh[5] = {{23, "Drive"}, {24, "Asym"}, {25, "Wet"}, {26, "Dry"}, {27, "Filter"}};
  static const KnobSpec wf[4] = {{6, "Drive"}, {7, "Split"}, {8, "Asym"}, {9, "Thres"}};
  static const KnobSpec ap[6] = {{10, "Drive"}, {11, "Split"}, {12, "Asym"}, {13, "Thres"}, {14, "Warp"}, {15, "Mode"}};
  static const KnobSpec ab[6] = {{28, "Drive"}, {29, "Gate"}, {30, "Mix"}, {31, "Level"}, {32, "Noise"}, {33, "Filter"}};
  static const KnobSpec cr[2] = {{34, "Rate"}, {35, "Bits"}};
  const KnobSpec* src = sh;
  int n = 5;
  if (algo == 0) src = od, n = 6;
  else if (algo == 1) src = fd, n = 5;
  else if (algo == 2) src = sc, n = 2;
  else if (algo == 14) src = wf, n = 4;
  else if (algo == 15) src = ap, n = 6;
  else if (algo == 16) src = ab, n = 6;
  else if (algo == 17) src = cr, n = 2;
  for (int i = 0; i < n; ++i) out[i] = src[i];
  return n;
}

void menuInt(App& a, const PluginTarget& t, const flk::Ctx& c, const char* id, int param, const char* const* names, int count, float x, float y,
             float w, float h) {
  const skin::Skin& K = c.K;
  const skin::Form& F = c.F;
  const PluginState st = *t.state(a.doc.song());
  const int v = std::clamp(ival(st, param), 0, count - 1);
  ImVec2 p0 = F.p(x, y), p1 = F.p(x + w, y + h);
  ImGui::SetCursorScreenPos(p0);
  ImGui::PushID(id);
  if (ImGui::InvisibleButton("##m", ImVec2(p1.x - p0.x, p1.y - p0.y))) ImGui::OpenPopup("##list");
  const bool hov = ImGui::IsItemHovered();
  c.dl->AddRectFilled(p0, p1, hov ? skin::lighten(K.lcdBg, 0.06f) : K.lcdBg, 2);
  std::string nm = flk::upper(names[v]);
  skin::label(c.dl, (p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f - F.font(8) * 0.6f, nm.c_str(), K.lcdText, F.font(8));
  if (ImGui::BeginPopup("##list")) {
    for (int k = 0; k < count; ++k)
      if (ImGui::MenuItem(names[k], nullptr, k == v) && k != v) setParam(a, t, param, k);
    ImGui::EndPopup();
  }
  ImGui::PopID();
}

void knobGrid(App& a, const PluginTarget& t, const flk::Ctx& c, const KnobSpec* ks, int n, int base, float x, float y, float w) {
  const float cw = w / 3, ws = 30;
  for (int i = 0; i < n; ++i) {
    const float cx = x + (i % 3) * cw + cw * 0.5f, cy = y + (i / 3) * 58;
    skin::wheel(a, t, base + ks[i].p, c.F.p(cx - ws * 0.5f, cy), ws * c.F.s, c.K, skin::WheelBigLine);
    std::string L = flk::upper(ks[i].label);
    flk::textC(c, cx, cy + ws + 2, L.c_str());
  }
}

void modulePanel(App& a, const PluginTarget& t, const PluginState& st, const flk::Ctx& c, int m, float x, float y, float w, float h) {
  const skin::Skin& K = c.K;
  ImDrawList* dl = c.dl;
  const int count = std::clamp(ival(st, DrCount), 0, kDrModules);
  ImGui::PushID(m);
  skin::well(dl, c.F.p(x, y), c.F.p(x + w, y + h), K, 5 * c.F.s);
  char num[16];
  std::snprintf(num, sizeof num, "MODULE %d", m + 1);
  flk::text(c, x + 8, y + 7, num);
  if (m >= count) {
    flk::textC(c, x + w * 0.5f, y + h * 0.4f, "EMPTY");
    if (flk::btn(c, "##add", x + w * 0.5f - 30, y + h * 0.5f, 60, 20, "ADD", false) && m == count) setParam(a, t, DrCount, count + 1);
    ImGui::PopID();
    return;
  }
  const int kind = std::clamp(ival(st, drParam(m, DmKind)), 0, 3);
  const bool off = ival(st, drParam(m, DmDisabled)) != 0;
  {  // power
    ImVec2 ctr = c.F.p(x + w - 12, y + 12);
    ImGui::SetCursorScreenPos(ImVec2(ctr.x - 9 * c.F.s, ctr.y - 9 * c.F.s));
    if (ImGui::InvisibleButton("##power", c.F.sz(18, 18))) setParam(a, t, drParam(m, DmDisabled), off ? 0 : 1);
    if (ImGui::IsItemHovered()) a.setHint(off ? "Module bypassed (click to enable)" : "Module on (click to bypass)");
    skin::ledDot(dl, ctr, 4.5f * c.F.s, !off, K);
  }
  menuInt(a, t, c, "kind", drParam(m, DmKind), kKinds, 4, x + 8, y + 24, w - 16, 18);
  const int base = drParam(m, DmP0);
  float ky = y + 92;
  if (kind == drack::KDist) {
    menuInt(a, t, c, "algo", drParam(m, DmAlgo), kAlgos, 18, x + 8, y + 46, w - 16, 18);
    static const char* os[5] = {"1X", "2X", "4X", "8X", "16X"};
    menuInt(a, t, c, "os", drParam(m, DmOs), os, 5, x + 8, y + 68, (w - 16) * 0.5f, 16);
    flk::tog(c, drParam(m, DmRemoveDc), x + w * 0.5f + 4, y + 68, (w - 16) * 0.5f - 4, 16, "DC");
    KnobSpec ks[6];
    const int n = algoKnobs(std::clamp(ival(st, drParam(m, DmAlgo)), 0, 17), ks);
    knobGrid(a, t, c, ks, n, base, x + 4, ky, w - 8);
  } else if (kind == drack::KFilter) {
    menuInt(a, t, c, "ftype", drParam(m, DmFilterType), kFilters, 22, x + 8, y + 46, w - 16, 18);
    static const char* os[3] = {"1X", "2X", "4X"};
    menuInt(a, t, c, "os", drParam(m, DmOs), os, 3, x + 8, y + 68, (w - 16) * 0.5f, 16);
    flk::tog(c, drParam(m, DmFilterOpt), x + w * 0.5f + 4, y + 68, (w - 16) * 0.5f - 4, 16, "SMOOTH");
    static const KnobSpec ks[2] = {{39, "Cutoff"}, {40, "Reso"}};
    knobGrid(a, t, c, ks, 2, base, x + 4, ky, w - 8);
  } else if (kind == drack::KChorus) {
    static const char* modes[3] = {"STEREO", "WIDE", "CROSS"};
    menuInt(a, t, c, "cmode", drParam(m, DmChorusMode), modes, 3, x + 8, y + 46, w - 16, 18);
    static const KnobSpec ks[6] = {{43, "Delay"}, {44, "Feedback"}, {45, "Speed"}, {46, "Depth"}, {47, "Blur"}, {48, "Colour"}};
    knobGrid(a, t, c, ks, 6, base, x + 4, ky, w - 8);
  } else {
    static const char* const cabs[] = {"Cabinet 1", "Cabinet 2", "Cabinet 3", "Cabinet 4", "Cabinet 5", "Cabinet 6", "Cabinet 7", "Cabinet 8"};
    static std::vector<std::string> names;
    static std::vector<const char*> ptr;
    if (names.empty()) {
      for (int i = 0; i <= 58; ++i) names.push_back("Cabinet " + std::to_string(i));
      for (auto& s : names) ptr.push_back(s.c_str());
    }
    (void)cabs;
    menuInt(a, t, c, "ir", drParam(m, DmImpulse), ptr.data(), 59, x + 8, y + 46, w - 16, 18);
    static const KnobSpec ks[2] = {{41, "Sep in"}, {42, "Sep out"}};
    knobGrid(a, t, c, ks, 2, base, x + 4, ky, w - 8);
    flk::textC(c, x + w * 0.5f, ky + 70, "MODELLED CABINET", K.textDim, 7.0f);
  }
  // Footer: input / mix / output.
  const float fy = y + h - 64;
  dl->AddLine(c.F.p(x + 6, fy), c.F.p(x + w - 6, fy), skin::darken(K.wellC(), 0.3f));
  static const KnobSpec io[3] = {{36, "In"}, {37, "Mix"}, {38, "Out"}};
  knobGrid(a, t, c, io, 3, base, x + 4, fy + 6, w - 8);
  ImGui::PopID();
}

const char* const kPedals[pedal::TCount] = {"Empty",  "Drive",   "Chorus", "Flanger", "Phaser", "Reverb",
                                             "Echo",   "Gate",    "EQ",     "Comp",    "Wobble", "Tube"};
int pedalKnobs(int type, const char** out) {
  static const char* L[pedal::TCount][4] = {{nullptr, nullptr, nullptr, nullptr},
                                            {"Drive", "Comb", "Bass", "Bite"},
                                            {"Width", "Speed", "Depth", nullptr},
                                            {"Width", "Speed", "Depth", nullptr},
                                            {"Speed", "Tune", "Width", "Depth"},
                                            {"Decay", "Damp", "Mix", nullptr},
                                            {"Time", "Repeats", "Mix", nullptr},
                                            {"Attack", "Release", "Thres", nullptr},
                                            {"Low", "Mid", "Treble", "High"},
                                            {"Thres", "Ratio", "Attack", "Release"},
                                            {"Pitch", "Amp", "Pan", "Rate"},
                                            {"Low", "High", "Gain", nullptr}};
  int n = 0;
  for (int k = 0; k < 4; ++k)
    if (L[std::clamp(type, 0, pedal::TCount - 1)][k]) out[n++] = L[std::clamp(type, 0, pedal::TCount - 1)][k];
  return n;
}

}  // namespace

void editorModuleRack(App& a, const PluginTarget& t, float width) {
  const PluginState st = *t.state(a.doc.song());
  const skin::Skin& K = refRackSkin();
  constexpr float kW = 900, kH = 400;
  skin::Form F = skin::Form::fit(width, kW);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::panel(dl, F.o, F.p(kW, kH), K, 4);
  skin::Scope scope(K);
  flk::Ctx c{a, t, F, K, dl};
  const float x0 = 8, gap = 8, pw = (kW - 2 * x0 - 3 * gap) / 4;
  for (int m = 0; m < kDrModules; ++m) modulePanel(a, t, st, c, m, x0 + m * (pw + gap), 6, pw, 340);
  skin::well(dl, F.p(6, 352), F.p(kW - 6, kH - 6), K, 4 * F.s);
  flk::tog(c, DrSoftClip, 16, 362, 110, 22, "SOFT CLIP OUT");
  const int count = std::clamp(ival(st, DrCount), 0, kDrModules);
  if (flk::btn(c, "##remove", 140, 362, 120, 22, "REMOVE LAST", false) && count > 0) setParam(a, t, DrCount, count - 1);
  static const char* models[3] = {"GUITAR RACK", "MODULE RACK", "PEDAL BOARD"};
  flk::text(c, 300, 368, "MODEL");
  flk::sw(c, RkModel, 350, 362, 330, 22, models, 3, true);
  F.end(kW, kH);
}

void editorPedalBoard(App& a, const PluginTarget& t, float width) {
  const PluginState st = *t.state(a.doc.song());
  const skin::Skin& K = refRackSkin();
  constexpr float kW = 900, kH = 400;
  skin::Form F = skin::Form::fit(width, kW);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::panel(dl, F.o, F.p(kW, kH), K, 4);
  skin::Scope scope(K);
  flk::Ctx c{a, t, F, K, dl};
  const float x0 = 8, gap = 8, bw = (kW - 2 * x0 - 4 * gap) / 5;
  for (int b = 0; b < pedal::kBoxes; ++b) {
    const float x = x0 + b * (bw + gap), y = 6, h = 230;
    ImGui::PushID(b);
    skin::well(dl, F.p(x, y), F.p(x + bw, y + h), K, 5 * F.s);
    const int type = std::clamp(pedal::boxType(val(st, PbValue0 + 45 + b)), 0, pedal::TCount - 1);
    const bool on = val(st, PbValue0 + 39 + b) != 0;
    menuInt(a, t, c, "type", PbValue0 + 45 + b, kPedals, pedal::TCount, x + 8, y + 8, bw - 36, 18);
    {
      ImVec2 ctr = F.p(x + bw - 14, y + 17);
      ImGui::SetCursorScreenPos(ImVec2(ctr.x - 9 * F.s, ctr.y - 9 * F.s));
      if (ImGui::InvisibleButton("##power", F.sz(18, 18))) setParam(a, t, PbValue0 + 39 + b, on ? 0 : 1);
      skin::ledDot(dl, ctr, 4.5f * F.s, on && type > 0, K);
    }
    const char* labels[4];
    const int n = pedalKnobs(type, labels);
    for (int k = 0; k < n; ++k) {
      const float cx = x + (k % 2) * bw * 0.5f + bw * 0.25f, cy = y + 40 + (k / 2) * 92;
      skin::wheel(a, t, PbValue0 + 13 + 5 * b + k, F.p(cx - 20, cy), 40 * F.s, K, skin::WheelBigLine);
      std::string L = flk::upper(labels[k]);
      flk::textC(c, cx, cy + 44, L.c_str());
    }
    if (n == 0) flk::textC(c, x + bw * 0.5f, y + h * 0.45f, "EMPTY");
    ImGui::PopID();
  }
  skin::well(dl, F.p(6, 242), F.p(kW - 6, kH - 6), K, 4 * F.s);
  flk::wheel(c, PbValue0 + 0, 16, 252, 36, "Input");
  flk::wheel(c, PbValue0 + 1, 70, 252, 36, "Output");
  {
    static std::vector<std::string> names;
    static std::vector<const char*> ptr;
    if (names.empty()) {
      names.push_back("No cabinet");
      for (int i = 1; i <= 20; ++i) names.push_back("Cabinet " + std::to_string(i));
      for (auto& s : names) ptr.push_back(s.c_str());
    }
    flk::text(c, 130, 254, "CABINET (MODELLED)");
    menuInt(a, t, c, "cab", PbValue0 + 11, ptr.data(), 21, 130, 268, 130, 18);
  }
  flk::tog(c, PbValue0 + 2, 280, 252, 60, 18, "EQ");
  static const char* bands[8] = {"125", "250", "500", "1K", "2K", "4K", "8K", "16K"};
  for (int k = 0; k < 8; ++k) {
    const float x = 350 + k * 44;
    skin::vslider(a, t, PbValue0 + 3 + k, F.p(x, 250), ImVec2(20 * F.s, 110 * F.s), K);
    flk::textC(c, x + 10, 364, bands[k]);
  }
  static const char* models[3] = {"GUITAR", "MODULES", "PEDALS"};
  flk::sw(c, RkModel, 720, 252, 170, 22, models, 3, true);
  F.end(kW, kH);
}

}  // namespace gl::packs::dynamics
