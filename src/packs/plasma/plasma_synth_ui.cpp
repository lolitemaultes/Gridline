// Gridline — "plasma" pack: the Synth and Effects tabs (see plasma_ui.h). The sound is a grid of
// blocks, one per part: the layers, Root and Texture, the filters and the amp in signal order,
// the modulation under them. A sidebar at the right holds the parts to drag in (each family
// counts what is free) and a library of tables, LFO shapes and sounds. Adding or removing a block
// switches one of the engine's fixed slots on or off; the block order on screen is the editor's
// own and never changes the sound.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include "../../app/CollabUi.h"
#include "../../app/Widgets.h"
#include "../../core/I18n.h"
#include "../../third_party/imgui/imgui_internal.h"
#include "plasma_dsp.h"
#include "plasma_ui.h"

namespace gl::packs::plasma::ui {
namespace fs = std::filesystem;

int tabOf(int v) {
  switch (v) {
    case VEffects: return TabEffects;
    case VLinks: return TabLinks;
    case VVoices:
    case VKeys:
    case VArp:
    case VPhrases:
    case VZones: return TabPlay;
    case VSetup: return TabSetup;
    default: return TabSynth;
  }
}

float sidebarW(const Ui& u) {
  if (u.patch || isOverlay(u.view) || (u.view != VLayer && u.view != VEffects)) return 0;
  return M(284);  // the design: 248 wide, 20 from the edge, 16 from the content
}

namespace {

// ---- block ids: 0..2 layers, 3 Root, 4 Texture, 10..11 filters, 20 Amp (the sound row);
// 100 + l LFOs, 200 + e envelopes (2..4), 300 Macros, 310 Key and touch, 320 Chance (modulation)
constexpr int kAmpId = 20, kMacroId = 300, kKeysId = 310, kChanceId = 320;
const std::vector<int>& defaultOrder() {
  static const std::vector<int> v = [] {
    std::vector<int> o = {0, 1, 2, 3, 4, 10, 11, kAmpId};
    for (int l = 0; l < kLfos; ++l) o.push_back(100 + l);
    for (int e = 1; e < kEnvs; ++e) o.push_back(200 + e);
    o.push_back(kMacroId), o.push_back(kKeysId), o.push_back(kChanceId);
    return o;
  }();
  return v;
}
void ensureOrder(Ui& u) {
  if (u.order.size() == defaultOrder().size()) return;
  u.order = defaultOrder();
}

// ---- the parts in the sidebar (the payload "PLASMA_PART": a module family, 100 + an effect
// type, 200 + a table number, 1000 + an LFO shape)
enum Part : int { PLayer = 0, PRoot, PTexture, PFilter, PLfo, PEnv, PMacro, PKeys, PChance, PCount };
constexpr int kAmpTile = 90;  // the sidebar's Amp tile (always 1 of 1, not a part to add)
constexpr int kPartFx = 100, kPartTable = 200, kPartShape = 1000;
const char* partName(int p) {
  static const char* const n[PCount] = {"Layer", "Root", "Texture", "Filter", "LFO", "Envelope", "Macro", "Key and touch", "Chance"};
  return p >= 0 && p < PCount ? n[p] : "";
}
const char* partPlural(int p) {
  static const char* const n[PCount] = {"layers", "Root", "Texture", "filters", "LFOs", "envelopes", "macros", "Key and touch", "Chance"};
  return p >= 0 && p < PCount ? n[p] : "";
}

bool differsFromDefault(const Ctx& c, int k0, int n) {
  for (int k = k0; k < k0 + n; ++k)
    if (const ParamDesc* d = pdesc(k); d && std::fabs(val(c, k) - d->def) > 1e-9) return true;
  return false;
}
int linksFrom(const Ctx& c, int fam) {
  int n = 0;
  for (int s = 0; s < kModSlots; ++s) {
    const int src = vali(c, modP(s, M_Source));
    if (src > SrcNone && vali(c, modP(s, M_Dest)) > 0 && familyOf(src) == fam && (fam != FamMacro)) ++n;
  }
  return n;
}
bool lfoUsed(const Ctx& c, int l) {
  return linksOf(c, SrcLfo1 + l) + linksOf(c, SrcLfo1Y + l) > 0 || !c.m.lfo[static_cast<size_t>(l)].pts.empty() || differsFromDefault(c, lfoP(l, 0), L_Count);
}
bool envUsed(const Ctx& c, int e) { return linksOf(c, SrcEnv1 + e) > 0 || differsFromDefault(c, envP(e, 0), E_Count); }
int macrosUsed(const Ctx& c) {
  int hi = 0;
  for (int i = 0; i < kMacros; ++i)
    if (linksOf(c, SrcMacro1 + i) > 0 || !c.m.macroName[static_cast<size_t>(i)].empty() || differsFromDefault(c, macroP(i), 1)) hi = i + 1;
  return hi;
}
int macrosShown(const Ctx& c) {
  if (c.u.reachAll) return kMacros;
  const int n = std::max({4, macrosUsed(c), c.u.macrosShown});
  return n > 4 ? kMacros : 4;
}

bool present(const Ctx& c, int id) {
  if (c.u.reachAll) return true;
  if (id < 3) return val(c, oscP(id, O_Enable)) >= 0.5;
  if (id == 3) return val(c, subP(S_Enable)) >= 0.5;
  if (id == 4) return val(c, noiseP(N_Enable)) >= 0.5;
  if (id == 10 || id == 11) return val(c, filtP(id - 10, F_Enable)) >= 0.5;
  if (id == kAmpId || id == kMacroId) return true;
  if (id >= 100 && id < 100 + kLfos) return id == 100 || ((c.u.added >> (id - 100)) & 1u) || lfoUsed(c, id - 100);
  if (id >= 200 && id < 200 + kEnvs) return ((c.u.added >> (10 + id - 200)) & 1u) || envUsed(c, id - 200);
  if (id == kKeysId) return ((c.u.added >> 22) & 1u) || linksFrom(c, FamKey) > 0;
  if (id == kChanceId) return ((c.u.added >> 23) & 1u) || linksFrom(c, FamChance) > 0;
  return false;
}
// In use / slots of a part family.
std::pair<int, int> partCount(const Ctx& c, int p) {
  int n = 0;
  switch (p) {
    case PLayer:
      for (int o = 0; o < kOscs; ++o) n += present(c, o);
      return {n, kOscs};
    case PRoot: return {present(c, 3) ? 1 : 0, 1};
    case PTexture: return {present(c, 4) ? 1 : 0, 1};
    case PFilter: return {present(c, 10) + present(c, 11), kFilters};
    case PLfo:
      for (int l = 0; l < kLfos; ++l) n += present(c, 100 + l);
      return {n, kLfos};
    case PEnv:
      n = 1;  // Envelope 1 is the Amp's
      for (int e = 1; e < kEnvs; ++e) n += present(c, 200 + e);
      return {n, kEnvs};
    case PMacro: return {macrosShown(c), kMacros};
    case PKeys: return {present(c, kKeysId) ? 1 : 0, 1};
    case PChance: return {present(c, kChanceId) ? 1 : 0, 1};
    default: return {0, 0};
  }
}
// Adds a part: the next free slot of its family. Returns the block id (-1: the family is full).
int addPart(Ctx& c, int p) {
  Ui& u = c.u;
  switch (p) {
    case PLayer:
      for (int o = 0; o < kOscs; ++o)
        if (!present(c, o)) return setv(c, oscP(o, O_Enable), 1), u.layer = o, o;
      return -1;
    case PRoot: return present(c, 3) ? -1 : (setv(c, subP(S_Enable), 1), 3);
    case PTexture: return present(c, 4) ? -1 : (setv(c, noiseP(N_Enable), 1), 4);
    case PFilter:
      for (int f = 0; f < kFilters; ++f)
        if (!present(c, 10 + f)) return setv(c, filtP(f, F_Enable), 1), 10 + f;
      return -1;
    case PLfo:
      for (int l = 0; l < kLfos; ++l)
        if (!present(c, 100 + l)) return u.added |= 1u << l, 100 + l;
      return -1;
    case PEnv:
      for (int e = 1; e < kEnvs; ++e)
        if (!present(c, 200 + e)) return u.added |= 1u << (10 + e), 200 + e;
      return -1;
    case PMacro:
      if (macrosShown(c) >= kMacros) return -1;
      u.macrosShown = kMacros;
      return kMacroId;
    case PKeys: return present(c, kKeysId) ? -1 : (u.added |= 1u << 22, kKeysId);
    case PChance: return present(c, kChanceId) ? -1 : (u.added |= 1u << 23, kChanceId);
    default: return -1;
  }
}
// Removes a block: its slot goes off and back to its start settings, its links go (one undo step).
void removeBlock(Ctx& c, int id) {
  Ui& u = c.u;
  auto resetRange = [](PluginState& st, int k0, int n) {
    for (int k = k0; k < k0 + n; ++k)
      if (const ParamDesc* d = pdesc(k); d && k < static_cast<int>(st.params.size())) st.params[static_cast<size_t>(k)] = d->def;
  };
  auto dropLinks = [&](PluginState& st, int src) {
    for (int s = 0; s < kModSlots; ++s)
      if (static_cast<int>(std::lround(st.params[static_cast<size_t>(modP(s, M_Source))])) == src) resetRange(st, modP(s, 0), M_Count);
  };
  const Model def = defaultModel();
  const char* label = "Remove part";
  if (id < 3)
    editState(c.a, c.t, label, [&](PluginState& st, Model& m) {
      resetRange(st, oscP(id, 0), O_Count);
      st.params[static_cast<size_t>(oscP(id, O_Enable))] = 0;
      m.osc[static_cast<size_t>(id)] = def.osc[static_cast<size_t>(id)];
    });
  else if (id == 3)
    editState(c.a, c.t, label, [&](PluginState& st, Model&) {
      resetRange(st, subP(0), S_Count);
      st.params[static_cast<size_t>(subP(S_Enable))] = 0;
    });
  else if (id == 4)
    editState(c.a, c.t, label, [&](PluginState& st, Model& m) {
      resetRange(st, noiseP(0), N_Count);
      st.params[static_cast<size_t>(noiseP(N_Enable))] = 0;
      m.noiseFile.clear();
    });
  else if (id == 10 || id == 11)
    editState(c.a, c.t, label, [&](PluginState& st, Model&) {
      resetRange(st, filtP(id - 10, 0), F_Count);
      st.params[static_cast<size_t>(filtP(id - 10, F_Enable))] = 0;
    });
  else if (id >= 100 && id < 100 + kLfos) {
    const int l = id - 100;
    editState(c.a, c.t, label, [&](PluginState& st, Model& m) {
      resetRange(st, lfoP(l, 0), L_Count);
      dropLinks(st, SrcLfo1 + l), dropLinks(st, SrcLfo1Y + l);
      m.lfo[static_cast<size_t>(l)] = def.lfo[static_cast<size_t>(l)];
    });
    u.added &= ~(1u << l);
  } else if (id >= 200 && id < 200 + kEnvs) {
    const int e = id - 200;
    editState(c.a, c.t, label, [&](PluginState& st, Model&) {
      resetRange(st, envP(e, 0), E_Count);
      dropLinks(st, SrcEnv1 + e);
    });
    u.added &= ~(1u << (10 + e));
  } else if (id == kKeysId || id == kChanceId) {
    const int fam = id == kKeysId ? FamKey : FamChance;
    editState(c.a, c.t, label, [&](PluginState& st, Model&) {
      for (int s = 0; s < kModSlots; ++s) {
        const int src = static_cast<int>(std::lround(st.params[static_cast<size_t>(modP(s, M_Source))]));
        if (src > SrcNone && familyOf(src) == fam) resetRange(st, modP(s, 0), M_Count);
      }
    });
    u.added &= ~(1u << (id == kKeysId ? 22 : 23));
  }
}
// Loads factory table t (1-based) into layer o.
void loadTable(Ctx& c, int o, int t) {
  auto& d = c.m.osc[static_cast<size_t>(o)];
  d.table.clear(), d.tableName.clear(), d.frames.clear();
  commitParams(c, "Choose table", {{oscP(o, O_Table), t}, {oscP(o, O_Type), OscWavetable}});  // one undo step
}
void loadShape(Ctx& c, int l, int sh) {
  LfoData& ld = c.m.lfo[static_cast<size_t>(l)];
  ld.pts.clear(), ld.loop = -1, ld.name.clear();
  commitParams(c, "LFO shape", {{lfoP(l, L_Shape), sh}});
}

// The family colour of a block.
ImU32 blockFam(const Ctx& c, int id) {
  if (id < 10) return c.P.knob;
  if (id < kAmpId) return c.P.good;
  if (id == kAmpId) return c.P.fam[FamEnv];
  if (id < 200) return c.P.fam[FamLfo];
  if (id < 300) return c.P.fam[FamEnv];
  if (id == kMacroId) return c.P.fam[FamMacro];
  if (id == kKeysId) return c.P.fam[FamKey];
  return c.P.fam[FamChance];
}
std::string blockTitle(const Ctx& c, int id) {
  (void)c;
  if (id < 3) return "Layer " + std::to_string(id + 1);
  if (id == 3) return "Root";
  if (id == 4) return "Texture";
  if (id == 10 || id == 11) return "Filter " + std::to_string(id - 9);
  if (id == kAmpId) return "Amp";
  if (id >= 100 && id < 200) return "LFO " + std::to_string(id - 99);
  if (id >= 200 && id < 300) return "Envelope " + std::to_string(id - 199);
  if (id == kMacroId) return "Macros";
  if (id == kKeysId) return "Key and touch";
  return "Chance";
}
enum Section : int { SecSources = 0, SecShape, SecMove, SecCount };
constexpr int kFreeLayer = -2, kFreeFilter = -3;  // the free slots' ids
int sectionOf(int id) { return id < 10 ? SecSources : id < 100 ? SecShape : SecMove; }
int sectionOfPart(int part) {
  if (part < 0 || (part >= kPartFx && part < kPartTable)) return -1;
  if (part >= kPartShape) return SecMove;
  if (part >= kPartTable) return SecSources;
  return part <= PTexture ? SecSources : part == PFilter ? SecShape : SecMove;
}

const ImGuiPayload* partPayload() {
  const ImGuiPayload* p = ImGui::GetDragDropPayload();
  return p && p->IsDataType("PLASMA_PART") ? p : nullptr;
}
int draggedPart(const Ui& u) {
  if (const ImGuiPayload* p = partPayload()) {
    int v = 0;
    std::memcpy(&v, p->Data, sizeof(int));
    return v;
  }
  return u.devDrag;
}

void partGlyph(Ctx& c, int p, float x, float y, float w, float h, ImU32 col) {
  const float th = std::max(1.2f, c.s(1.4f));
  auto P = [&](float fx, float fy) { return c.p(x + fx * w, y + h - fy * h); };
  switch (p) {
    case PLayer: {
      ImVec2 pts[25];
      for (int i = 0; i <= 24; ++i) {
        const float t = i / 24.f;
        pts[i] = P(t, 0.5f + 0.42f * (std::sin(t * 6.2832f) * 0.7f + std::sin(t * 18.85f) * 0.3f));
      }
      c.dl->AddPolyline(pts, 25, col, 0, th);
      break;
    }
    case PRoot: rootWavePublic(c, 0, x, y, w, h, col); break;
    case PTexture:
      for (int i = 0; i < 18; ++i) {
        const float t = i / 17.f, a = 0.5f + 0.42f * std::sin(i * 2.7f + 1.3f) * std::cos(i * 1.9f);
        c.dl->AddLine(P(t, 0.5f), P(t, a), col, th);
      }
      break;
    case PFilter: {
      const ImVec2 pts[5] = {P(0, 0.62f), P(0.45f, 0.62f), P(0.6f, 0.9f), P(0.72f, 0.3f), P(1, 0.04f)};
      c.dl->AddPolyline(pts, 5, col, 0, th);
      break;
    }
    case PLfo: {
      const ImVec2 pts[5] = {P(0, 0.1f), P(0.25f, 0.9f), P(0.5f, 0.1f), P(0.75f, 0.9f), P(1, 0.1f)};
      c.dl->AddPolyline(pts, 5, col, 0, th);
      break;
    }
    case PEnv: {
      const ImVec2 pts[5] = {P(0, 0.05f), P(0.18f, 0.95f), P(0.42f, 0.55f), P(0.72f, 0.55f), P(1, 0.05f)};
      c.dl->AddPolyline(pts, 5, col, 0, th);
      break;
    }
    case PMacro: {
      const ImVec2 m = c.p(x + w * 0.5f, y + h * 0.55f);
      const float r = c.s(std::min(w, h) * 0.42f);
      c.dl->PathArcTo(m, r, 2.36f, 2.36f + 4.71f, 24);
      c.dl->PathStroke(theme::withAlpha(col, 0.35f), 0, th);
      c.dl->PathArcTo(m, r, 2.36f, 2.36f + 3.0f, 18);
      c.dl->PathStroke(col, 0, th);
      c.dl->AddLine(m, ImVec2(m.x + r * std::cos(5.36f), m.y + r * std::sin(5.36f)), col, th);
      break;
    }
    case PKeys:
      for (int i = 0; i < 5; ++i) c.dl->AddRect(P(i / 5.f, 1), P((i + 1) / 5.f, 0), col, 0, 0, th);
      for (int i : {1, 2, 4}) c.dl->AddRectFilled(P(i / 5.f - 0.07f, 1), P(i / 5.f + 0.07f, 0.42f), col);
      break;
    default:
      for (int i = 0; i < 5; ++i) c.dl->AddCircleFilled(P(0.18f + 0.16f * i, 0.5f + 0.3f * std::sin(i * 2.1f)), c.s(1.8f), col, 8);
      break;
  }
}

// ---- the bodies of the motion blocks
void macroBody(Ctx& c, float x, float& y, float w) {
  const int n = macrosShown(c);
  const int per = 4;
  const float dx = w / per;
  for (int i = 0; i < n; ++i) {
    ImGui::PushID(i + 800);
    const float cx = x + dx * (i % per + 0.5f), cy = y + 26 + (i / per) * 92.f;
    const std::string nm = c.m.macroName[static_cast<size_t>(i)].empty() ? "Macro " + std::to_string(i + 1) : c.m.macroName[static_cast<size_t>(i)];
    knob(c, macroP(i), cx, cy, 18, nm.c_str(), KnobValue);
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) macroRenameRequest() = i;
    sourceGrip(c, SrcMacro1 + i, cx - 26, cy - 18, 5.5f);
    const int links = linksOf(c, SrcMacro1 + i);
    char b[24];
    if (links) std::snprintf(b, sizeof b, links == 1 ? "1 link" : "%d links", links);
    else std::snprintf(b, sizeof b, "no links");
    textC(c, cx, cy + 52, b, links ? c.P.famText[FamMacro] : c.P.textFaint, 9.5f, false);
    macroRenamePopup(c, i);
    ImGui::PopID();
  }
  y += 92.f * ((n + per - 1) / per);
}
void sourceList(Ctx& c, const std::vector<std::pair<int, const char*>>& list, float x, float& y, float w, int view) {
  const float cw = (w - 6) / 2;
  for (size_t i = 0; i < list.size(); ++i) sourceRow(c, list[i].first, x + (i % 2) * (cw + 6), y + (i / 2) * 30.f, cw, false, view, list[i].second);
  y += 30.f * ((list.size() + 1) / 2);
}
void keysBody(Ctx& c, float x, float& y, float w) {
  static const std::vector<std::pair<int, const char*>> main = {{SrcVelo, "Velocity"}, {SrcNote, "Key"}, {SrcModWheel, "Mod wheel"}, {SrcAftertouch, "Pressure"}};
  static const std::vector<std::pair<int, const char*>> more = {{SrcReleaseVelo, "Release"}, {SrcPolyAT, "Key pressure"}, {SrcPitchBend, "Bend"},
                                                                {SrcExprX, "Slide X"},     {SrcExprY, "Slide Y"},       {SrcExprZ, "Press Z"}};
  sourceList(c, main, x, y, w, -1);
  if (showDetails(c)) sourceList(c, more, x, y, w, -1);
  if (button(c, "curves", x, y, w, 24, "Response curves...", false, 11.0f)) openView(c.u, VKeys);
  if (ImGui::IsItemHovered()) c.a.setHint("Key and touch on the Play tab: response curves, wheels, expression");
  y += 30;
}
void chanceBody(Ctx& c, float x, float& y, float w) {
  static const std::vector<std::pair<int, const char*>> main = {{SrcRand1, "Random 1"}, {SrcRand2, "Random 2"}, {SrcNoise, "Noise"}, {SrcAlt1, "Alternate 1"}};
  static const std::vector<std::pair<int, const char*>> more = {
      {SrcRandDiscrete, "Random steps"}, {SrcAlt2, "Alternate 2"}, {SrcActiveVoices, "Voices"}, {SrcVoiceIndex, "Voice number"},
      {SrcVoiceMod1, "Voice mod 1"},     {SrcVoiceMod2, "Voice mod 2"}, {SrcFixed, "Constant"},  {SrcOscA, "Layer 1"},
      {SrcOscB, "Layer 2"},              {SrcOscC, "Layer 3"},         {SrcSub, "Root"},         {SrcFilter1, "Filter 1"},
      {SrcFilter2, "Filter 2"}};
  sourceList(c, main, x, y, w, -1);
  if (showDetails(c)) sourceList(c, more, x, y, w, -1);
}

// ---- one block at (x, y), w wide: returns the y under it
// Opens a block's advanced panel (a double-click), growing from its rectangle.
void openAdvanced(Ctx& c, int id, ImVec2 a, ImVec2 b) {
  Ui& u = c.u;
  u.expandFrom = ImVec4(a.x, a.y, b.x, b.y), u.expandT = 0, u.closing = false;
  if (id < 3) u.wtOsc = id, u.layer = id, openView(u, VTable);
  else if (id == 3 || id == 4) u.src = id, u.large = true, u.view = VRootTex;
  else if (id == 10 || id == 11) u.large = true, u.view = VFilters;
  else if (id == kAmpId) u.envTab = 0, u.view = VEnv;
  else if (id >= 100 && id < 200) u.lfoTab = id - 100, u.view = VLfo;
  else if (id >= 200 && id < 300) u.envTab = id - 200, u.view = VEnv;
  else if (id == kKeysId) u.view = VKeys;
  else u.view = VLinks, u.linkGroup = 0;
}
float drawBlock(Ctx& c, int id, float x, float y, float w, float frameH) {
  Ui& u = c.u;
  ImGui::PushID(id + 20000);
  const std::string title = blockTitle(c, id);
  Block b;
  BlockOpts o;
  o.fam = blockFam(c, id);
  o.drag = true;
  o.canRemove = id != kAmpId && id != kMacroId && id != 100;
  std::function<void(float, float, float, float)> mini;
  if (id < 3) o.enable = oscP(id, O_Enable), o.selected = u.layer == id, mini = [&c, id](float a, float bb, float cc, float d) { cycleMiniPublic(c, id, a, bb, cc, d); };
  else if (id == 3) o.enable = subP(S_Enable), mini = [&c](float a, float bb, float cc, float d) { rootWavePublic(c, vali(c, subP(S_Shape)), a, bb, cc, d, c.P.knob); };
  else if (id == 4) o.enable = noiseP(N_Enable), mini = [&c](float a, float bb, float cc, float d) { textureMiniPublic(c, a, bb, cc, d); };
  else if (id == 10 || id == 11) o.enable = filtP(id - 10, F_Enable), mini = [&c, id](float a, float bb, float cc, float d) { filterCurvePublic(c, id - 10, a, bb, cc, d); };
  else if (id == kAmpId) o.grip = SrcEnv1, o.view = VEnv, o.viewTab = 0, mini = [&c](float a, float bb, float cc, float d) { envMini(c, 0, a, bb, cc, d); };
  else if (id >= 100 && id < 200) o.grip = SrcLfo1 + id - 100, o.view = VLfo, o.viewTab = id - 100, mini = [&c, id](float a, float bb, float cc, float d) { lfoMini(c, id - 100, a, bb, cc, d); };
  else if (id >= 200 && id < 300) o.grip = SrcEnv1 + id - 200, o.view = VEnv, o.viewTab = id - 200, mini = [&c, id](float a, float bb, float cc, float d) { envMini(c, id - 200, a, bb, cc, d); };
  else if (id == kMacroId) o.canMore = false;
  o.frameH = frameH, o.ownOpen = true;
  o.hint = "Double-click for more";
  // its menu: the parameters it owns, its Patch box, its section and what it does
  static const char* const secName[] = {"Sources", "Shape", "Movement"};
  o.section = secName[std::clamp(sectionOf(id), 0, 2)];
  if (id < 3) {
    o.pLo = oscP(id, 0), o.pN = O_Count, o.pKind = BkLayer, o.patchNode = id;
    o.about = "A wavetable layer: it plays a table of single-cycle frames; Position scans through them, Warp bends each one";
    o.extraMenu = [&c, id] {
      if (menuItemLook(c, "loadtbl", "Load wavetable", ">", 11)) ImGui::OpenPopup("##loadtbl");
      if (ImGui::BeginPopup("##loadtbl")) {
        tableMenuPublic(c, id);
        ImGui::EndPopup();
      }
      if (menuItemLook(c, "savetbl", "Save wavetable", "", 12)) exportTablePublic(c, id), ImGui::CloseCurrentPopup();
      if (ImGui::IsItemHovered()) c.a.setHint("Save the table as a WAV of 2048-sample frames");
    };
  } else if (id == 3) {
    o.pLo = subP(0), o.pN = S_Count, o.pKind = BkRoot, o.patchNode = 3;
    o.about = "Root: a plain wave an octave or two under the layers, to hold the low end steady";
  } else if (id == 4) {
    o.pLo = noiseP(0), o.pN = N_Count, o.pKind = BkTexture, o.patchNode = 4;
    o.about = "Texture: noise or a sampled loop under the layers, for air, grit or a strike at the start";
  } else if (id == 10 || id == 11) {
    o.pLo = filtP(id - 10, 0), o.pN = F_Count, o.pKind = BkFilter, o.patchNode = 10 + (id - 10);
    o.about = "A filter: it takes away (or lifts) part of the spectrum of the sources routed into it";
  } else if (id == kAmpId) {
    o.pLo = envP(0, 0), o.pN = E_Count, o.pKind = BkEnv, o.patchNode = 20;
    o.about = "The amp: Envelope 1 shapes the loudness of every note from key down to release";
  } else if (id >= 100 && id < 200) {
    o.pLo = lfoP(id - 100, 0), o.pN = L_Count, o.pKind = BkLfo, o.patchNode = 200 + SrcLfo1 + id - 100;
    o.about = "An LFO: a repeating shape that moves whatever it is linked to; drag its dot onto a knob";
  } else if (id >= 200 && id < 300) {
    o.pLo = envP(id - 200, 0), o.pN = E_Count, o.pKind = BkEnv, o.patchNode = 200 + SrcEnv1 + id - 200;
    o.about = "An envelope: a shape played once per note; drag its dot onto a knob to move it";
  } else if (id == kMacroId) {
    o.patchNode = 200 + SrcMacro1;
    o.about = "Macros: eight knobs you name and link to many settings at once, for playing the sound live";
  } else if (id == kKeysId) {
    o.about = "Key and touch: velocity, key, wheels and pressure as sources to link";
  } else if (id == kChanceId) {
    o.about = "Chance: random values, noise and alternating values, new on every note";
  }
  // what it is, after the name (a click chooses it where there is a choice)
  std::string sub;
  if (id < 3) {
    sub = layerSourceName(c, id);
    o.subMenu = [&c, id] { tableMenuPublic(c, id); };
    if (layerHasTable(c, id))
      o.tools = [&c, id](float rx, float ty) {
        const float sw = M(58), sx = rx - sw;
        const int hit = segPick(c, "dim", sx, ty, sw, M(22), {"2D", "3D"}, c.u.wt3d[id] ? 1 : 0, true, T(10.5f));
        if (hit >= 0) c.u.wt3d[id] = hit == 1;
        return sx - M(6);
      };
  } else if (id == 3) {
    static const char* const names[SubShapeCount] = {"Sine", "Round", "Triangle", "Saw", "Square", "Pulse"};
    sub = names[std::clamp(vali(c, subP(S_Shape)), 0, SubShapeCount - 1)];
    o.subMenu = [&c] {
      for (int i = 0; i < SubShapeCount; ++i)
        if (ImGui::MenuItem(i18n::tr(names[i]), nullptr, vali(c, subP(S_Shape)) == i)) setv(c, subP(S_Shape), i);
    };
  } else if (id == 4) {
    sub = paramText(noiseP(N_Type), val(c, noiseP(N_Type)), &c.st.params);
  } else if (id == 10 || id == 11) {
    sub = filterDisplayName(vali(c, filtP(id - 10, F_Type)));
    o.subMenu = [&c, id] { filterPicker(c, filtP(id - 10, F_Type)); };
  } else if (id == kAmpId) {
    sub = "Envelope 1, every note";
  } else if (id >= 100 && id < 200) {
    const int l = id - 100;
    sub = std::string(val(c, lfoP(l, L_Bpm)) >= 0.5 ? "Tempo, " : "Free, ") + paramText(lfoP(l, L_Rate), val(c, lfoP(l, L_Rate)), &c.st.params);
  } else if (id >= 200 && id < 300) {
    const int src = SrcEnv1 + id - 200;
    int n = 0;
    for (int sl = 0; sl < kModSlots; ++sl)
      if (vali(c, modP(sl, M_Source)) == src && vali(c, modP(sl, M_Dest)) > 0) {
        if (n++ == 0) sub = paramLongName(vali(c, modP(sl, M_Dest)) - 1);
      }
    if (n == 0) sub = "no links";
    else if (n > 1) sub += " and " + std::to_string(n - 1) + " more";
  } else if (id == kMacroId) {
    sub = "1 to 4";
  }
  o.sub = sub.c_str();
  const float y0 = y;
  const bool open = blockBegin(c, b, "b", x, y, w, title.c_str(), o, mini);
  if (ImGui::BeginDragDropSource()) {
    ImGui::SetDragDropPayload("PLASMA_BLK", &id, sizeof(int));
    ImGui::TextUnformatted(title.c_str());
    ImGui::EndDragDropSource();
  }
  if (b.titleClicked && id < 3) u.layer = id;
  {
    auto& mi = u.moreIds;
    const bool had = std::find(mi.begin(), mi.end(), id) != mi.end();
    if (b.more && !had) mi.push_back(id);
    if (!b.more && had) mi.erase(std::remove(mi.begin(), mi.end(), id), mi.end());
  }
  // no-scroll sizing (Simple, in a row of set height): the picture takes the row's slack, found
  // from what the body measured last frame (it settles in one frame)
  const bool grows = u.full || b.more || frameH <= 0;
  float& fit = u.picFit[id];
  fit = 0;
  gPicFit = 0, gPicUsed = false, gDialPushed = 0, gDialBase = 0;
  const ImU32 fam = blockFam(c, id);
  KnobColour kc(fam);
  const bool design = open && !grows && id != kKeysId && id != kChanceId;
  if (design) {  // Simple: the design's body, exactly in the row's height
    const float bx = x + M(12), bw = w - M(24), bh = std::max(M(30), y0 + frameH - M(10) - y);
    if (id < 3) layerBlockBody(c, id, bx, y, bw, bh);
    else if (id == 3) rootBlockBody(c, bx, y, bw, bh);
    else if (id == 4) textureBlockBody(c, bx, y, bw, bh);
    else if (id == 10 || id == 11) filterBlockBody(c, id - 10, bx, y, bw, bh);
    else if (id == kAmpId) envBlockBodyDesign(c, 0, bx, y, bw, bh, 12);
    else if (id >= 100 && id < 200) lfoBlockBodyDesign(c, id - 100, bx, y, bw, bh);
    else if (id >= 200 && id < 300) envBlockBodyDesign(c, id - 200, bx, y, bw, bh, 6);
    else if (id == kMacroId) macroBlockBodyDesign(c, 0, bx, y, bw, bh);
    y += bh;
  } else if (open) {
    const float bx = x + M(12), bw = w - M(24);
    if (id < 3) y = layerColumn(c, id, bx, y, bw);
    else if (id == 3) y = rootColumn(c, bx, y, bw);
    else if (id == 4) y = textureColumn(c, bx, y, bw);
    else if (id == 10 || id == 11) y = filterColumn(c, id - 10, bx, y, bw);
    else if (id == kAmpId) y = ampColumn(c, bx, y, bw);
    else if (id >= 100 && id < 200) y = lfoBlockBody(c, id - 100, bx, y, bw);
    else if (id >= 200 && id < 300) y = envBlockBody(c, id - 200, bx, y, bw);
    else if (id == kMacroId) macroBody(c, bx, y, bw);
    else if (id == kKeysId) keysBody(c, bx, y, bw);
    else chanceBody(c, bx, y, bw);
  }
  gPicFit = 0, gDialBase = 0;
  const float natural = std::max(M(44), y + (open ? M(10) : 0.f) - y0);
  blockEnd(c, b, y);
  {
    const auto it = u.contentH.find(id);
    if (it == u.contentH.end() || std::fabs(it->second - natural) > 0.5f) gLayoutSettling = true;  // its row grows to it next frame
  }
  u.contentH[id] = natural;
  const float bottom = y0 + std::max(frameH, y - 8 - y0);
  // a double-click on its title or a picture (or "Open it large" in its menu) opens its advanced panel
  if (b.openClicked) openAdvanced(c, id, c.p(x, y0), c.p(x + w, bottom));
  else if (ImGui::IsMouseDoubleClicked(0) && ImGui::IsMouseHoveringRect(c.p(x, y0), c.p(x + w, bottom)) &&
      ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && (ImGui::GetHoveredID() == 0 || isPicture(ImGui::GetHoveredID())))
    openAdvanced(c, id, c.p(x, y0), c.p(x + w, bottom));
  // drops on the block: another block (reorder), a table onto a layer, a shape onto an LFO
  if (ImGui::BeginDragDropTargetCustom(ImRect(c.p(x, y0), c.p(x + w, bottom)), ImGui::GetID("drop"))) {
    constexpr ImGuiDragDropFlags fl = ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect;
    auto lit = [&] { c.dl->AddRect(c.p(x - 2, y0 - 2), c.p(x + w + 2, bottom + 2), c.P.accent, c.s(8), 0, std::max(1.5f, c.s(2))); };
    if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("PLASMA_BLK", fl); p && (lit(), p->IsDelivery())) {
      int from = 0;
      std::memcpy(&from, p->Data, sizeof(int));
      if (from != id && sectionOf(from) == sectionOf(id)) {
        auto& ord = u.order;
        ord.erase(std::remove(ord.begin(), ord.end(), from), ord.end());
        ord.insert(std::find(ord.begin(), ord.end(), id), from);
      }
    }
    const int part = draggedPart(u);
    if ((id < 3 && part >= kPartTable && part < kPartShape) || (id >= 100 && id < 200 && part >= kPartShape))
      if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("PLASMA_PART", fl); p && (lit(), p->IsDelivery())) {
        int v = 0;
        std::memcpy(&v, p->Data, sizeof(int));
        if (id < 3) loadTable(c, id, v - kPartTable);
        else loadShape(c, id - 100, v - kPartShape);
      }
    ImGui::EndDragDropTarget();
  }
  if (b.removeClicked) removeBlock(c, id);
  ImGui::PopID();
  return bottom;
}

// ---- the sections: Sources (layers, Root and Texture), Shape (filters, amp), Movement. Each has
// its own slots; a part lands only in its own section, its slot shown as a ghost while it is
// dragged; blocks glide to their slots. Simple: each section has a set height, so the tab fits
// without scrolling (the pictures take what the controls leave); Full and More grow a row.
constexpr float kSecMinH[SecCount] = {M(268), M(190), M(190)};  // Simple: the row heights (the design)
constexpr float kSecHead = M(22 + 8), kSecGap = M(14), kGut = M(14);

// A slot: one block, or (Sources) Root over Texture in one column. -1: the ghost of a dragged part.
struct Slot {
  std::vector<int> ids;
  float w = 0;
};
float slotWidth(const Ui& u, int id, float W) {
  const bool more = u.full || std::find(u.moreIds.begin(), u.moreIds.end(), id) != u.moreIds.end();
  const float unit = (W - 3 * kGut) / 4, two = unit * 2 + kGut;  // four columns
  if (more) return two;  // every setting: two columns wide
  return id == kAmpId ? two : unit;
}
std::vector<Slot> slotsOf(const Ctx& c, int sec, float W, int ghostPart) {
  const Ui& u = c.u;
  std::vector<Slot> v;
  Slot stack;
  for (int id : u.order) {
    if (sectionOf(id) != sec || !present(c, id)) continue;
    if (id == 3 || id == 4) {  // Root and Texture share one column
      stack.ids.push_back(id), stack.w = std::max(stack.w, slotWidth(u, id, W));
      continue;
    }
    v.push_back({{id}, slotWidth(u, id, W)});
  }
  if (ghostPart >= 0) {
    float gw = slotWidth(u, 0, W);
    if (ghostPart == PLayer || ghostPart >= kPartTable) gw = slotWidth(u, 0, W);
    else if (ghostPart == PFilter) gw = slotWidth(u, 10, W);
    else if (ghostPart == PLfo || ghostPart >= kPartShape) gw = slotWidth(u, 100, W);
    else if (ghostPart == PEnv || ghostPart == PKeys || ghostPart == PChance) gw = slotWidth(u, 201, W);
    if (ghostPart == PRoot || ghostPart == PTexture) stack.ids.push_back(-1), stack.w = std::max(stack.w, slotWidth(u, 3, W));
    else v.push_back({{-1}, gw});
  }
  // a family with room shows its next slot as a quiet outline (a click adds it), so a row never
  // ends in a blank area; a dragged part of that family takes its place
  const int gfam = ghostPart >= kPartShape ? PLfo : ghostPart >= kPartTable ? PLayer : ghostPart;
  if (sec == SecSources && gfam != PLayer && partCount(c, PLayer).first < kOscs) v.push_back({{kFreeLayer}, slotWidth(u, 0, W)});
  if (sec == SecShape && gfam != PFilter && partCount(c, PFilter).first < kFilters) v.push_back({{kFreeFilter}, slotWidth(u, 10, W)});
  if (!stack.ids.empty()) v.push_back(stack);
  return v;
}

float gPagerRx = 0;  // the right end of the Movement page dots
ImU32 partFamCol(const Ctx& c, int fam) {
  switch (fam) {
    case PLayer: return partCol(c, 0);
    case PRoot: return partCol(c, 3);
    case PTexture: return partCol(c, 4);
    case PFilter: return partCol(c, 10);
    case PLfo: return partCol(c, 100);
    case PEnv: return partCol(c, 201);
    case PMacro: return partCol(c, 300);
    case PKeys: return partCol(c, 310);
    default: return partCol(c, 320);
  }
}
// A slot that is not a block yet: a dashed outline (tinted), a plus in a ring, the name, a line.
void slotOutline(const Ctx& c, float x, float y, float w, float h, ImU32 col, float tint, const char* name, const char* line, bool strong) {
  const ImVec2 a = c.p(x, y), b = c.p(x + w, y + h);
  if (tint > 0) c.dl->AddRectFilled(a, b, theme::withAlpha(col, tint), c.s(M(6)));
  const ImU32 dc = theme::withAlpha(col, strong ? 0.7f : 0.9f);
  const float th = std::max(1.f, c.s(M(1.5f))), dl = c.s(M(5)), gp = c.s(M(4));
  for (float d = a.x; d < b.x; d += dl + gp) {
    c.dl->AddLine(ImVec2(d, a.y), ImVec2(std::min(b.x, d + dl), a.y), dc, th);
    c.dl->AddLine(ImVec2(d, b.y), ImVec2(std::min(b.x, d + dl), b.y), dc, th);
  }
  for (float d = a.y; d < b.y; d += dl + gp) {
    c.dl->AddLine(ImVec2(a.x, d), ImVec2(a.x, std::min(b.y, d + dl)), dc, th);
    c.dl->AddLine(ImVec2(b.x, d), ImVec2(b.x, std::min(b.y, d + dl)), dc, th);
  }
  const float cy = y + std::min(h * 0.5f - M(16), M(34) + M(15));
  c.dl->AddCircle(c.p(x + w * 0.5f, cy), c.s(M(15)), dc, 24, th);
  c.dl->AddLine(c.p(x + w * 0.5f - M(6), cy), c.p(x + w * 0.5f + M(6), cy), dc, th);
  c.dl->AddLine(c.p(x + w * 0.5f, cy - M(6)), c.p(x + w * 0.5f, cy + M(6)), dc, th);
  textC(c, x + w * 0.5f, cy + M(15 + 6), name, strong ? c.P.text : c.P.label, T(13), true);
  if (line && textW(c, line, T(11), false) < w - M(12)) textC(c, x + w * 0.5f, cy + M(15 + 6 + 20), line, c.P.textFaint, T(11), false);
}
// Where the layers go, in a few words ("Layers 1 and 2 to Filter 1, Layer 3 to Filter 2").
std::string routingSummary(const Ctx& c) {
  std::string to[2];
  int n[2] = {0, 0};
  for (int o = 0; o < kOscs; ++o) {
    if (val(c, oscP(o, O_Enable)) < 0.5 || vali(c, routeP(o, R_Dest)) != RouteFilter) continue;
    const double bal = val(c, routeP(o, R_Balance));
    for (int f = 0; f < 2; ++f)
      if (f == 0 ? bal < 0.99 : bal > -0.99) {
        if (n[f]) to[f] += n[f] == 1 ? " and " : ", ";
        to[f] += n[f] ? std::to_string(o + 1) : "Layer " + std::to_string(o + 1);
        ++n[f];
      }
  }
  std::string s;
  for (int f = 0; f < 2; ++f) {
    if (!n[f]) continue;
    if (n[f] > 1) to[f].replace(0, 5, "Layers");
    if (!s.empty()) s += ", ";
    s += to[f] + " to Filter " + std::to_string(f + 1);
  }
  return s;
}

// One section: its title row, then its slots in rows (Movement: one row, paged). Returns the y
// under it.
float sectionRows(Ctx& c, int sec, float x, float y, float W, float originY, int ghostPart, float forceH) {
  Ui& u = c.u;
  const bool anim = c.a.settings.getBool("ui.animations") && !std::getenv("GL_PLASMA_VIEW") && !std::getenv("GL_PLASMA_VIEWS");
  const float k = anim ? 1.f - std::exp(-ImGui::GetIO().DeltaTime / 0.05f) : 1.f;  // about 150 ms to settle
  const std::vector<Slot> slots = slotsOf(c, sec, W, ghostPart);
  // rows of slots that fit the width
  std::vector<std::vector<size_t>> rows(1);
  float rw = 0;
  for (size_t i = 0; i < slots.size(); ++i) {
    if (!rows.back().empty() && rw + slots[i].w > W + 0.5f) rows.emplace_back(), rw = 0;
    rows.back().push_back(i), rw += slots[i].w + kGut;
  }
  const bool paged = sec == SecMove && !u.full && rows.size() > 1;
  if (paged) {  // the page dots at the right of the title row
    u.movePage = std::clamp(u.movePage, 0, static_cast<int>(rows.size()) - 1);
    for (size_t r = 0; r < rows.size(); ++r)  // a dragged part shows the page its slot is on
      for (size_t i : rows[r])
        if (slots[i].ids.front() < 0) u.movePage = static_cast<int>(r);
    const float dx = gPagerRx - M(11) * rows.size();
    for (size_t i = 0; i < rows.size(); ++i) {
      ImGui::PushID(static_cast<int>(i) + 9100);
      ImGui::SetCursorScreenPos(c.p(dx + i * M(11), y - kSecHead + M(4)));
      if (ImGui::InvisibleButton("##pg", c.F.sz(M(10), M(14)))) u.movePage = static_cast<int>(i);
      if (ImGui::IsItemHovered()) c.a.setHint("Movement page " + std::to_string(i + 1) + " of " + std::to_string(rows.size()));
      c.dl->AddCircleFilled(c.p(dx + i * M(11) + M(3), y - kSecHead + M(11)), c.s(M(3)), static_cast<int>(i) == u.movePage ? c.P.label : c.P.lineHi, 12);
      ImGui::PopID();
    }
  }
  float rowTop = y;
  for (size_t r = 0; r < rows.size(); ++r) {
    if (paged && static_cast<int>(r) != u.movePage) continue;
    // the row's height: the section's set height, or more when a block's content needs it (Full, More)
    float rowH = forceH > 0 ? forceH : kSecMinH[sec];
    for (size_t i : rows[r]) {
      float stackH = 0;
      for (int id : slots[i].ids) {
        const auto it = u.contentH.find(id);
        const bool grows = u.full || std::find(u.moreIds.begin(), u.moreIds.end(), id) != u.moreIds.end();
        const float h = it != u.contentH.end() && grows ? it->second : 0.f;
        stackH += (slots[i].ids.size() > 1 ? std::max(h, (rowH - kGut) / 2) : h) + (stackH > 0 ? kGut : 0);
      }
      rowH = std::max(rowH, stackH);
    }
    float cx = x;
    for (size_t i : rows[r]) {
      const Slot& sl = slots[i];
      const float n = static_cast<float>(sl.ids.size());
      const float each = n > 1 ? (rowH - kGut * (n - 1)) / n : rowH;
      float cy = rowTop;
      for (int id : sl.ids) {
        const ImVec2 target(cx - x, cy - originY);
        auto it = u.blockPos.find(id);
        if (it == u.blockPos.end() || !anim) {
          ImVec2 from = target;
          if (anim && u.dropAt.x >= 0 && id >= 0) from = u.dropAt;  // a dropped part glides in from the pointer
          it = u.blockPos.insert_or_assign(id, from).first;
        }
        ImVec2& pos = it->second;
        pos.x += (target.x - pos.x) * k, pos.y += (target.y - pos.y) * k;
        if (std::fabs(target.x - pos.x) < 0.3f && std::fabs(target.y - pos.y) < 0.3f) pos = target;
        else gLayoutSettling = true;  // gliding to its place: the layout check judges where it lands
        const float bx = x + pos.x, by = originY + pos.y;
        if (id == kFreeLayer || id == kFreeFilter) {  // a free slot: a quiet outline, a click adds the part
          const int fp = id == kFreeLayer ? PLayer : PFilter;
          ImGui::PushID(id);
          ImGui::SetCursorScreenPos(c.p(bx, by));
          const bool hit = ImGui::InvisibleButton("##free", c.F.sz(sl.w, each));
          const bool hv = ImGui::IsItemHovered();
          ImGui::PopID();
          if (hv) c.a.setHint(std::string("Click to add ") + partName(fp) + ", or drag one here from the parts at the right");
          const std::string nm = std::string(partName(fp)) + " " + std::to_string(partCount(c, fp).first + 1);
          slotOutline(c, bx, by, sl.w, each, hv ? c.P.lineHi : c.P.line, 0.f, nm.c_str(), "Click to add it, or drag one here", hv);
          if (hit) {
            u.dropAt = ImVec2(-1, -1);
            const int nid = addPart(c, fp);
            if (nid >= 0) u.blockPos[nid] = ImVec2(bx - x, by - originY);  // it opens where its slot was
          }
        } else if (id < 0) {  // the ghost: where the dragged part will go
          const int gf = ghostPart >= kPartShape ? PLfo : ghostPart >= kPartTable ? PLayer : ghostPart;
          const auto [cnt, of] = partCount(c, gf);
          (void)of;
          std::string msg = partName(gf);
          if (gf == PLayer || gf == PFilter || gf == PLfo || gf == PEnv) msg += " " + std::to_string(cnt + 1 + (gf == PEnv ? 1 : 0));
          slotOutline(c, bx, by, sl.w, each, partFamCol(c, gf), 0.07f, msg.c_str(), "Release to place it here", true);
        } else {
          drawBlock(c, id, bx, by, sl.w, each);
        }
        cy += each + kGut;
      }
      cx += sl.w + kGut;
    }
    rowTop += rowH + kGut;
  }
  if (slots.empty()) {  // an empty section: a quiet line where its parts go
    const char* msg = sec == SecMove ? "Drag an LFO, an envelope or a macro here from the parts at the right" : "Drag a part here from the parts at the right";
    textMid(c, x, x + W, rowTop + M(22), msg, c.P.textFaint, T(11.5f), false);
    rowTop += M(56);
  }
  return rowTop - kGut;
}

// The section a dragged part lands in, if its family has room (-1: none).
int ghostFor(const Ctx& c, int part, int sec) {
  if (part < 0 || sectionOfPart(part) != sec) return -1;
  int fam = part >= kPartShape ? PLfo : part >= kPartTable ? PLayer : part;
  const auto [n, of] = partCount(c, fam);
  if (n >= of || fam == PMacro) return -1;
  return part;
}

float synthGrid(Ctx& c, float x, float y, float w) {
  Ui& u = c.u;
  ensureOrder(u);
  const float originY = y;
  const int part = draggedPart(u);
  static const char* const names[SecCount] = {"Sources", "Shape", "Movement"};
  static const char* const subs[SecCount] = {"three wavetable layers, a root and a texture", "filters, then the amp", "drag a dot onto any knob to link it"};
  // the rows share the tab's height (the drawer slides over it: the tab then scrolls that far)
  const float avail = (kPlayY - M(16)) - (kTopH + M(16)) - SecCount * kSecHead - (SecCount - 1) * kSecGap;
  const float scale = std::max(1.f, avail / (kSecMinH[0] + kSecMinH[1] + kSecMinH[2]));
  for (int sec = 0; sec < SecCount; ++sec) {
    char right[64] = "";
    if (sec == SecSources) std::snprintf(right, sizeof right, "Layers %d of %d", partCount(c, PLayer).first, kOscs);
    if (sec == SecMove) std::snprintf(right, sizeof right, "Links %d of %d", linksUsed(c), kModSlots);
    float rx = sectionHead(c, x, y, w, names[sec], subs[sec], sec == SecShape ? "" : right);
    if (sec == SecShape) {  // where the layers go, and the routing page
      const float bw = textW(c, "Routing", T(11.5f), false) + M(16);
      if (btnLook(c, "routing", rx - bw, y, bw, M(22), "Routing", false, false, T(11.5f))) openView(u, VRouting);
      if (ImGui::IsItemHovered()) c.a.setHint("Routing: which part feeds which filter, the sends, the order of the filters");
      const std::string rs = routingSummary(c);
      const float rw = textW(c, rs.c_str(), T(11), false);
      if (rw < w * 0.5f) text(c, rx - bw - M(8) - rw, y + M(11) - T(11) * 0.68f, rs.c_str(), c.P.textFaint, T(11), false);
    }
    if (sec == SecMove) {  // Links opens the Links tab
      const float lw = textW(c, right, T(11), false);
      ImGui::SetCursorScreenPos(c.p(x + w - lw, y));
      if (ImGui::InvisibleButton("##links", c.F.sz(lw, M(22)))) u.view = VLinks;
      if (ImGui::IsItemHovered()) c.a.setHint("Links: every link, grouped by source or by target");
      gPagerRx = rx;
    }
    y += kSecHead;
    if (sec == SecMove) u.modY = y - kSecHead - originY;
    y = sectionRows(c, sec, x, y, w, originY, ghostFor(c, part, sec), std::floor(kSecMinH[sec] * scale)) + kSecGap;
  }
  u.dropAt = ImVec2(-1, -1);  // used by the block the drop added, this frame
  return y;
}

// The ghost of a part being dragged (screenshots only; a real drag shows the tooltip).
void devGhost(Ctx& c, int part, float x, float y) {
  const float w = 140, h = 44;
  c.dl->AddRectFilled(c.p(x + 3, y + 4), c.p(x + w + 3, y + h + 4), IM_COL32(0, 0, 0, 60), c.s(6));
  c.dl->AddRectFilled(c.p(x, y), c.p(x + w, y + h), c.P.panelHi, c.s(6));
  c.dl->AddRect(c.p(x, y), c.p(x + w, y + h), c.P.accent, c.s(6), 0, std::max(1.f, c.s(1.5f)));
  std::string nm;
  if (part >= kPartFx && part < kPartTable) {
    fxTypeGlyphPublic(c, part - kPartFx, x + 8, y + 10, 30, 24, c.P.knob);
    nm = "Reverb";
  } else if (part >= kPartTable && part < kPartShape) {
    partGlyph(c, PLayer, x + 8, y + 12, 30, 20, c.P.knob);
    const auto& n = factoryTableNames();
    nm = part - kPartTable >= 1 && part - kPartTable <= static_cast<int>(n.size()) ? n[static_cast<size_t>(part - kPartTable - 1)] : "Table";
  } else {
    partGlyph(c, part, x + 8, y + 12, 30, 20, c.P.knob);
    nm = partName(part);
  }
  if (textW(c, nm.c_str(), 11.5f, true) > w - 56) nm = "Part";
  labelBeside(c, x + 46, y, h, nm.c_str(), c.P.text, 11.5f, true);
  // the pointer
  const ImVec2 m = c.p(x + w - 18, y + h - 10);
  const float s = c.s(1);
  const ImVec2 pts[7] = {m, ImVec2(m.x, m.y + 16 * s), ImVec2(m.x + 4 * s, m.y + 12 * s), ImVec2(m.x + 7 * s, m.y + 18 * s),
                         ImVec2(m.x + 9.5f * s, m.y + 17 * s), ImVec2(m.x + 6.5f * s, m.y + 11 * s), ImVec2(m.x + 11.5f * s, m.y + 11 * s)};
  c.dl->AddConvexPolyFilled(pts, 7, IM_COL32(250, 250, 250, 255));
  c.dl->AddPolyline(pts, 7, IM_COL32(20, 20, 24, 255), ImDrawFlags_Closed, std::max(1.f, c.s(1.f)));
}

// ---- sidebar pieces
// A part tile: picture, count, name. Drag it into the synth (or click to add it).
void partTile(Ctx& c, int part, float x, float y, float w, float h) {
  const bool amp = part == kAmpTile;  // the Amp is always there (1 of 1)
  auto [n, of] = amp ? std::pair<int, int>{1, 1} : partCount(c, part);
  if (part == PEnv) n -= 1, of -= 1;  // Envelope 1 belongs to the Amp
  const bool full = n >= of;
  ImGui::PushID(part + 4700);
  ImGui::SetCursorScreenPos(c.p(x, y));
  const bool click = ImGui::InvisibleButton("##tile", c.F.sz(w, h));
  const bool hv = ImGui::IsItemHovered();
  if (!full && ImGui::BeginDragDropSource()) {
    ImGui::SetDragDropPayload("PLASMA_PART", &part, sizeof(int));
    ImGui::Text("%s %s", i18n::tr("Add"), i18n::tr(partName(part)));
    ImGui::EndDragDropSource();
  }
  const char* nm = amp ? "Amp" : partName(part);
  if (hv) {
    if (amp) c.a.setHint("Amp: every sound has one, it is in the Shape section");
    else if (full)
      c.a.setHint(of == 1 ? std::string(nm) + " is already in the synth (remove it there to add it again)"
                          : std::string("All ") + std::to_string(of) + " " + partPlural(part) + " are in use: remove one to add another");
    else c.a.setHint(std::string("Drag ") + nm + " into the synth, or click to add it");
  }
  if (click && !full && !amp) {
    const int id = addPart(c, part);
    if (id >= 0 && id < 3) c.u.layer = id;
  }
  layoutBox(c, x, y, w, h, nm);
  const float a = full ? 0.5f : 1.f;
  auto fade = [&](ImU32 col) { return theme::withAlpha(col, a * ((col >> IM_COL32_A_SHIFT) & 0xFF) / 255.f); };
  if (!full) c.dl->AddRectFilled(c.p(x, y + M(1)), c.p(x + w, y + h + M(1)), IM_COL32(0, 0, 0, c.P.dark ? 50 : 18), c.s(M(4)));
  c.dl->AddRectFilled(c.p(x, y), c.p(x + w, y + h), fade(hv && !full ? c.P.panelHi : c.P.panel), c.s(M(4)));
  c.dl->AddRect(c.p(x, y), c.p(x + w, y + h), fade(hv && !full ? c.P.lineHi : c.P.line), c.s(M(4)));
  const int fam = amp ? 20 : part == PLayer ? 0 : part == PRoot ? 3 : part == PTexture ? 4 : part == PFilter ? 10 : part == PLfo ? 100 : part == PEnv ? 200 : part == PMacro ? 300 : part == PKeys ? 310 : 320;
  partGlyph(c, amp ? PEnv : part, x + M(8), y + h * 0.5f - M(8), M(22), M(16), fade(partCol(c, fam)));
  const float tx = x + M(8 + 22 + 8), tw = x + w - M(6) - tx;
  // the name (two lines when it is long), then in use / slots
  std::string l1 = nm, l2;
  if (textW(c, nm, T(11.5f), false) > tw) {
    const std::string s = nm;
    for (size_t sp = s.rfind(' '); sp != std::string::npos && sp > 0; sp = s.rfind(' ', sp - 1)) {
      if (textW(c, s.substr(0, sp).c_str(), T(11.5f), false) <= tw) {
        l1 = s.substr(0, sp), l2 = s.substr(sp + 1);
        break;
      }
      if (sp == 0) break;
    }
  }
  char cnt[16];
  std::snprintf(cnt, sizeof cnt, "%d of %d", n, of);
  const float lh = M(12.6f), ch = M(13), tot = lh * (l2.empty() ? 1 : 2) + ch;
  float ty = y + (h - tot) * 0.5f;
  text(c, tx, ty + lh * 0.5f - T(11.5f) * 0.68f, l1.c_str(), fade(c.P.text), T(11.5f), false);
  if (!l2.empty()) ty += lh, text(c, tx, ty + lh * 0.5f - T(11.5f) * 0.68f, l2.c_str(), fade(c.P.text), T(11.5f), false);
  ty += lh;
  {
    MonoText mono;
    text(c, tx, ty + ch * 0.5f - T(10) * 0.68f, cnt, fade(c.P.textFaint), T(10), false);
  }
  ImGui::PopID();
}
void fxTile(Ctx& c, int type, float x, float y, float w, float h, bool full) {
  ImGui::PushID(type + 4800);
  ImGui::SetCursorScreenPos(c.p(x, y));
  const bool click = ImGui::InvisibleButton("##fxt", c.F.sz(w, h));
  const bool hv = ImGui::IsItemHovered();
  const int part = kPartFx + type;
  const char* nm = i18n::tr(fxName(type));
  if (!full && ImGui::BeginDragDropSource()) {
    ImGui::SetDragDropPayload("PLASMA_PART", &part, sizeof(int));
    ImGui::Text("%s %s", i18n::tr("Add"), nm);
    ImGui::EndDragDropSource();
  }
  if (hv) c.a.setHint(full ? std::string("All effect places are in use: remove an effect to add another") : std::string("Drag ") + nm + " into a lane, or click to add it to the open lane");
  if (click && !full) addFxAt(c, type, c.u.fxRack, -1);
  layoutBox(c, x, y, w, h, nm);
  const float a = full ? 0.5f : 1.f;
  auto fade = [&](ImU32 col) { return theme::withAlpha(col, a * ((col >> IM_COL32_A_SHIFT) & 0xFF) / 255.f); };
  if (!full) c.dl->AddRectFilled(c.p(x, y + M(1)), c.p(x + w, y + h + M(1)), IM_COL32(0, 0, 0, c.P.dark ? 50 : 18), c.s(M(4)));
  c.dl->AddRectFilled(c.p(x, y), c.p(x + w, y + h), fade(hv && !full ? c.P.panelHi : c.P.panel), c.s(M(4)));
  c.dl->AddRect(c.p(x, y), c.p(x + w, y + h), fade(hv && !full ? c.P.lineHi : c.P.line), c.s(M(4)));
  fxTypeGlyphPublic(c, type, x + M(8), y + h * 0.5f - M(8), M(22), M(16), fade(fxCol(c, type)));
  const float tx = x + M(8 + 22 + 8), tw = x + w - M(6) - tx;
  std::string l1 = nm, l2;  // two lines when it is long, never a cut word
  if (textW(c, nm, T(11.5f), false) > tw) {
    const std::string s = nm;
    for (size_t sp = s.rfind(' '); sp != std::string::npos && sp > 0; sp = s.rfind(' ', sp - 1)) {
      if (textW(c, s.substr(0, sp).c_str(), T(11.5f), false) <= tw) {
        l1 = s.substr(0, sp), l2 = s.substr(sp + 1);
        break;
      }
    }
  }
  const float lh = M(12.6f);
  if (l2.empty()) labelBeside(c, tx, y, h, l1.c_str(), fade(c.P.text), T(11.5f), false);
  else {
    const float ty = y + h * 0.5f - lh;
    text(c, tx, ty + lh * 0.5f - T(11.5f) * 0.68f, l1.c_str(), fade(c.P.text), T(11.5f), false);
    text(c, tx, ty + lh * 1.5f - T(11.5f) * 0.68f, l2.c_str(), fade(c.P.text), T(11.5f), false);
  }
  ImGui::PopID();
}

std::string lowerStr(std::string s) {
  for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  return s;
}

// The library: every table, every shape (LFO, Root, Texture), the sounds; by group with its
// size, filtered by the search field (a search shows every group's matches).
float libraryList(Ctx& c, float x, float y, float w) {
  Ui& u = c.u;
  const std::string f = lowerStr(u.sbFilter);
  const float rh = M(30), gap = M(2);
  static std::map<std::string, bool> folded;  // groups folded by the user (Library groups start folded)
  auto groupHead = [&](const std::string& name, int count, bool startFolded) {
    const auto it = folded.find(name);
    const bool fold = f.empty() && (it == folded.end() ? startFolded : it->second);
    if (!ImGui::IsRectVisible(c.p(x, y), c.p(x + w, y + M(22)))) {  // scrolled out of the list
      y += M(22) + gap;
      return !fold;
    }
    ImGui::PushID(name.c_str());
    ImGui::SetCursorScreenPos(c.p(x, y));
    if (ImGui::InvisibleButton("##grp", c.F.sz(w, M(22))) && f.empty()) folded[name] = !fold;
    const bool hv = ImGui::IsItemHovered();
    if (hv) c.a.setHint(fold ? "Show this group" : "Fold this group");
    ImGui::PopID();
    const ImVec2 m = c.p(x + M(5), y + M(11));
    const float d = c.s(M(2.6f));
    const ImU32 ac = hv ? c.P.text : c.P.textFaint;
    if (fold) c.dl->AddTriangleFilled(ImVec2(m.x - d * 0.5f, m.y - d), ImVec2(m.x - d * 0.5f, m.y + d), ImVec2(m.x + d * 0.7f, m.y), ac);
    else c.dl->AddTriangleFilled(ImVec2(m.x - d, m.y - d * 0.5f), ImVec2(m.x + d, m.y - d * 0.5f), ImVec2(m.x, m.y + d * 0.7f), ac);
    char cnt[16];
    std::snprintf(cnt, sizeof cnt, "%d", count);
    MonoText mono;
    const float cw = textW(c, cnt, T(10), false);
    std::string shown = name;
    while (shown.size() > 3 && textW(c, shown.c_str(), T(10), false) > w - M(16) - cw - M(8)) shown.pop_back();
    text(c, x + M(14), y + M(11) - T(10) * 0.68f, shown.c_str(), hv ? c.P.text : c.P.label, T(10), false);
    text(c, x + w - cw - M(4), y + M(11) - T(10) * 0.68f, cnt, c.P.textFaint, T(10), false);
    y += M(22) + gap;
    return !fold;
  };
  int idx = 0;
  auto row = [&](const std::string& name, int part, bool on, ImU32 picCol, const std::function<void(float, float, float, float, ImU32)>& pic,
                 const std::function<void()>& onClick, const char* hint) {
    ImGui::PushID(idx++);
    if (!ImGui::IsRectVisible(c.p(x, y), c.p(x + w, y + rh))) {  // scrolled out of the list: no item under other controls
      ImGui::PopID();
      y += rh + gap;
      return;
    }
    ImGui::SetCursorScreenPos(c.p(x, y));
    const bool click = ImGui::InvisibleButton("##lib", c.F.sz(w, rh));
    const bool hv = ImGui::IsItemHovered();
    if (part >= 0 && ImGui::BeginDragDropSource()) {
      ImGui::SetDragDropPayload("PLASMA_PART", &part, sizeof(int));
      ImGui::TextUnformatted(name.c_str());
      ImGui::EndDragDropSource();
    }
    if (hv) c.a.setHint(name + ": " + hint);
    if (click) onClick();
    if (on || hv) c.dl->AddRectFilled(c.p(x, y), c.p(x + w, y + rh), on ? c.P.panel : c.P.panelHi, c.s(M(4)));
    float tx = x + M(8);
    if (pic) {
      pic(x + M(8), y + rh * 0.5f - M(8), M(30), M(16), on ? picCol : c.P.textFaint);
      tx = x + M(8 + 30 + 10);
    }
    std::string shown = name;
    while (shown.size() > 3 && textW(c, shown.c_str(), T(11.5f), false) > x + w - M(6) - tx) shown.pop_back();
    if (shown.size() < name.size()) {  // never a cut word: the full name is in the hint
      while (!shown.empty() && shown.back() != ' ') shown.pop_back();
      if (shown.empty()) shown = name.substr(0, 1);
      while (!shown.empty() && shown.back() == ' ') shown.pop_back();
    }
    labelBeside(c, tx, y, rh, shown.c_str(), on || hv ? c.P.text : c.P.label, T(11.5f), false);
    ImGui::PopID();
    y += rh + gap;
  };
  auto match = [&](const std::string& a, const std::string& b) { return f.empty() || lowerStr(a + " " + b).find(f) != std::string::npos; };
  auto squiggle = [&](float px, float py, float pw, float ph, ImU32 col) {
    ImVec2 pts[25];
    for (int j = 0; j <= 24; ++j) {
      const float t = j / 24.f;
      pts[j] = c.p(px + pw * t, py + ph * 0.5f - ph * 0.42f * (std::sin(t * 6.2832f) * 0.75f + std::sin(t * 18.85f) * 0.25f));
    }
    c.dl->AddPolyline(pts, 25, col, 0, std::max(1.f, c.s(M(1.3f))));
  };
  if (u.sbLib == 0) {
    const TableCatalog& cat = tableCatalogUi(c);
    const auto& mips = factoryMips();
    int o = u.layer;
    if (o < 0 || o > 2) o = 0;
    const std::string curRef = c.m.osc[static_cast<size_t>(o)].table;
    const int curT = vali(c, oscP(o, O_Table));
    size_t at = 0;
    for (const auto& g : cat.groups) {
      int n = 0;
      for (size_t i = at; i < at + static_cast<size_t>(g.count); ++i) n += match(cat.entries[i].name, g.name);
      if (n > 0 && groupHead(g.name, n, g.name.rfind("Library", 0) == 0))
        for (size_t i = at; i < at + static_cast<size_t>(g.count); ++i) {
          const TableEntry& e = cat.entries[i];
          if (!match(e.name, g.name)) continue;
          const bool on = present(c, o) && (e.factory > 0 ? curRef.empty() && curT == e.factory : curRef == e.ref);
          const int part = e.factory > 0 ? kPartTable + e.factory : -1;
          const int fi = e.factory - 1;
          row(e.name, part, on, partCol(c, o),
              [&, fi](float px, float py, float pw, float ph, ImU32 col) {
                if (fi < 0 || fi >= static_cast<int>(mips.size()) || !mips[static_cast<size_t>(fi)]) return squiggle(px, py, pw, ph, col);
                const MipTable* m = mips[static_cast<size_t>(fi)].get();
                const int lv = 4, len = m->len[static_cast<size_t>(lv)];
                const float* d = m->frame(lv, m->frames / 2);
                ImVec2 pts[25];
                for (int j = 0; j <= 24; ++j) pts[j] = c.p(px + pw * j / 24.f, py + ph * 0.5f - std::clamp(d[std::min(len - 1, len * j / 24)], -1.f, 1.f) * ph * 0.45f);
                c.dl->AddPolyline(pts, 25, col, 0, std::max(1.f, c.s(M(1.3f))));
              },
              [&c, &u, i] {
                const TableCatalog& ct = tableCatalogUi(c);
                if (i >= ct.entries.size()) return;
                int lo = u.layer;
                if (!present(c, lo)) lo = addPart(c, PLayer);
                if (lo >= 0 && lo < 3) chooseTableEntry(c, lo, ct.entries[i]);
              },
              e.factory > 0 ? "drag onto a layer (or empty space: a new layer), click: load into the selected layer" : "click: load into the selected layer");
        }
      at += static_cast<size_t>(g.count);
    }
  } else if (u.sbLib == 1) {
    {  // LFO shapes
      const auto& sn = lfoShapeNames();
      int n = 0;
      for (int i = 1; i < static_cast<int>(sn.size()); ++i) n += match(sn[static_cast<size_t>(i)], "LFO shapes");
      if (n > 0 && groupHead("LFO shapes", n, false))
        for (int i = 1; i < static_cast<int>(sn.size()); ++i)
          if (match(sn[static_cast<size_t>(i)], "LFO shapes"))
            row(sn[static_cast<size_t>(i)], kPartShape + i, false, c.P.fam[FamLfo],
                [&, i](float px, float py, float pw, float ph, ImU32 col) { drawCurve(c, lfoFactoryShape(i), px, py, pw, ph, col, false); },
                [&, i] { loadShape(c, std::clamp(u.lfoTab, 0, kLfos - 1), i); }, "drag onto an LFO, click: give it to the LFO last opened");
    }
    {  // Root shapes
      static const char* const rn[SubShapeCount] = {"Sine", "Round Rect", "Triangle", "Saw", "Square", "Pulse"};
      const int curS = present(c, 3) ? vali(c, subP(S_Shape)) : -1;
      int n = 0;
      for (const char* r : rn) n += match(r, "Root shapes");
      if (n > 0 && groupHead("Root shapes", n, false))
        for (int i = 0; i < SubShapeCount; ++i)
          if (match(rn[i], "Root shapes"))
            row(rn[i], -1, curS == i, partCol(c, 3),
                [&, i](float px, float py, float pw, float ph, ImU32 col) {
                  ImVec2 pts[33];
                  for (int j = 0; j <= 32; ++j) pts[j] = c.p(px + pw * j / 32.f, py + ph * 0.5f - rootWaveAt(i, j / 32.f) * ph * 0.42f);
                  c.dl->AddPolyline(pts, 33, col, 0, std::max(1.f, c.s(M(1.3f))));
                },
                [&, i] {
                  if (!present(c, 3)) addPart(c, PRoot);
                  setv(c, subP(S_Shape), i);
                },
                "click: give the Root this shape");
    }
    {  // Texture sources
      const auto& nn = noiseNames();
      const int curN = present(c, 4) ? vali(c, noiseP(N_Type)) : -1;
      int n = 0;
      for (int i = 1; i < static_cast<int>(nn.size()); ++i) n += match(nn[static_cast<size_t>(i)], "Texture sources");
      if (n > 0 && groupHead("Texture sources", n, false))
        for (int i = 1; i < static_cast<int>(nn.size()); ++i)
          if (match(nn[static_cast<size_t>(i)], "Texture sources"))
            row(nn[static_cast<size_t>(i)], -1, curN == i, partCol(c, 4),
                [&, i](float px, float py, float pw, float ph, ImU32 col) {
                  for (int j = 0; j < 14; ++j) {
                    const float t = j / 13.f, a = 0.5f + 0.42f * std::sin(j * 2.7f + i * 1.3f) * std::cos(j * 1.9f + i);
                    c.dl->AddLine(c.p(px + pw * t, py + ph * 0.5f), c.p(px + pw * t, py + ph * (1 - a)), col, std::max(1.f, c.s(M(1.1f))));
                  }
                },
                [&, i] {
                  if (!present(c, 4)) addPart(c, PTexture);
                  setv(c, noiseP(N_Type), i);
                },
                "click: give the Texture this source");
    }
  } else {
    int n = 0;
    std::vector<std::pair<std::string, std::function<void()>>> sounds;
    if (const PluginInfo* in = findPlugin(kType))
      for (const auto& p : in->presets) {
        if (p.name == "Init") continue;
        const auto v = p.values;
        const std::string nm = p.name;
        sounds.push_back({nm, [&c, v, nm] { applyPreset(c, v, "", nm); }});
      }
    for (const auto& p : factoryDataPresets()) {
      const auto v = p.values;
      const std::string nm = p.name, data = p.data;
      sounds.push_back({nm, [&c, v, data, nm] { applyPreset(c, v, data, nm); }});
    }
    for (const auto& s : sounds) n += match(s.first, "Factory sounds");
    if (n > 0 && groupHead("Factory sounds", n, false))
      for (const auto& s : sounds)
        if (match(s.first, "Factory sounds")) row(s.first, -1, s.first == c.m.presetName, c.P.accent, {}, s.second, "click to load this sound");
  }
  return y;
}

}  // namespace

// ------------------------------------------------------------------ the Library (a full panel)
namespace {

// A table seen small: up to 16 frames of 96 points each, peak-normalised (kept for every table
// shown, so a long library costs little memory).
struct Thumb {
  int frames = 0, rows = 0;
  std::vector<float> pts;  // rows * 96
};
constexpr int kThumbPts = 96;
bool gRescanTables = false;  // a table was imported: list the folders again

Thumb makeThumb(const Table& t) {
  Thumb th;
  th.frames = t.frames;
  th.rows = std::clamp(t.frames, 1, 16);
  th.pts.resize(static_cast<size_t>(th.rows * kThumbPts));
  float peak = 1e-6f;
  for (int r = 0; r < th.rows; ++r) {
    const int f = th.rows == 1 ? 0 : static_cast<int>(std::lround(r * (t.frames - 1) / static_cast<float>(th.rows - 1)));
    const float* d = t.frame(std::clamp(f, 0, t.frames - 1));
    for (int j = 0; j < kThumbPts; ++j) {
      const float v = d[j * kFrameSize / kThumbPts];
      th.pts[static_cast<size_t>(r * kThumbPts + j)] = v;
      peak = std::max(peak, std::fabs(v));
    }
  }
  for (float& v : th.pts) v /= peak;
  return th;
}

// The thumbnail of catalog entry i: factory tables at once, files a few per frame (nullptr until read).
const Thumb* thumbOf(const TableEntry& e, int& budget) {
  static std::map<std::string, Thumb> cache;
  const std::string key = e.factory > 0 ? "#" + std::to_string(e.factory) : e.ref;
  if (auto it = cache.find(key); it != cache.end()) return it->second.rows ? &it->second : nullptr;
  std::shared_ptr<const Table> t;
  if (e.factory > 0) {
    t = factoryTable(e.factory - 1);
  } else {
    if (budget <= 0) return nullptr;
    --budget;
    if (e.ref.rfind("file:", 0) == 0) t = loadTableFile(e.ref.substr(5));
  }
  Thumb& th = cache[key];
  if (t && t->frames > 0) th = makeThumb(*t);
  return th.rows ? &th : nullptr;
}

// Frames stacked back and up in perspective; the middle one lit.
void drawStack(const Ctx& c, const Thumb& th, float x, float y, float w, float h, ImU32 col, bool glow) {
  const int R = th.rows;
  auto proj = [&](float d, float& x0, float& pw, float& y0, float& a) {
    x0 = x + w * 0.05f + d * w * 0.24f, pw = w * 0.68f * (1.f - 0.14f * d), y0 = y + h * 0.72f - d * h * 0.46f, a = h * 0.2f * (1.f - 0.2f * d);
  };
  std::vector<ImVec2> pts(kThumbPts);
  auto line = [&](int r) {
    const float d = R == 1 ? 0.f : r / static_cast<float>(R - 1);
    float x0, pw, y0, a;
    proj(R == 1 ? 0.5f : d, x0, pw, y0, a);
    for (int j = 0; j < kThumbPts; ++j)
      pts[static_cast<size_t>(j)] = c.p(x0 + pw * j / (kThumbPts - 1.f), y0 - th.pts[static_cast<size_t>(r * kThumbPts + j)] * a);
  };
  const int lit = R / 2;
  for (int r = R - 1; r >= 0; --r) {
    if (r == lit) continue;
    line(r);
    const float d = R == 1 ? 0.f : r / static_cast<float>(R - 1);
    c.dl->AddPolyline(pts.data(), kThumbPts, theme::withAlpha(c.P.text, (0.08f + 0.2f * (1.f - d)) * (c.P.dark ? 1.f : 1.4f)), 0, 1.f);
  }
  line(lit);
  if (glow) {
    float x0, pw, y0, a;
    proj(R == 1 ? 0.5f : lit / static_cast<float>(R - 1), x0, pw, y0, a);
    fillUnder(c, pts, c.p(0, y0).y, col, 0.26f, 0.02f, c.p(0, y0 - a).y);
    glowLine(c, pts, col, M(2), 0.8f);
  } else {
    c.dl->AddPolyline(pts.data(), kThumbPts, col, 0, std::max(1.f, c.s(M(1.6f))));
  }
}

// One shape (the Shapes tab): kind 1 LFO, 2 Root, 3 Texture; n its number there.
struct ShapeItem {
  int kind = 0, n = 0;
  std::string name;
};
std::vector<ShapeItem> shapeItems() {
  std::vector<ShapeItem> v;
  const auto& sn = lfoShapeNames();
  for (int i = 1; i < static_cast<int>(sn.size()); ++i) v.push_back({1, i, sn[static_cast<size_t>(i)]});
  static const char* const rn[SubShapeCount] = {"Sine", "Round Rect", "Triangle", "Saw", "Square", "Pulse"};
  for (int i = 0; i < SubShapeCount; ++i) v.push_back({2, i, rn[i]});
  const auto& nn = noiseNames();
  for (int i = 1; i < static_cast<int>(nn.size()); ++i) v.push_back({3, i, nn[static_cast<size_t>(i)]});
  return v;
}

void drawShape(Ctx& c, const ShapeItem& s, float x, float y, float w, float h, ImU32 col, bool glow) {
  std::vector<ImVec2> pts;
  if (s.kind == 1) {
    if (glow) {
      drawCurve(c, lfoFactoryShape(s.n), x, y, w, h, col, true);
      return;
    }
    drawCurve(c, lfoFactoryShape(s.n), x, y, w, h, col, false);
    return;
  }
  if (s.kind == 2) {
    for (int j = 0; j <= 64; ++j) pts.push_back(c.p(x + w * j / 64.f, y + h * 0.5f - rootWaveAt(s.n, j / 64.f) * h * 0.42f));
  } else {
    for (int j = 0; j < 28; ++j) {
      const float t = j / 27.f, a = 0.5f + 0.42f * std::sin(j * 2.7f + s.n * 1.3f) * std::cos(j * 1.9f + s.n);
      c.dl->AddLine(c.p(x + w * t, y + h * 0.5f + (a - 0.5f) * h * 0.4f), c.p(x + w * t, y + h * (1 - a)), col, std::max(1.f, c.s(M(1.4f))));
    }
    return;
  }
  if (glow) glowLine(c, pts, col, M(2), 0.8f);
  else c.dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), col, 0, std::max(1.f, c.s(M(1.5f))));
}

// A text field with a magnifier (the Library's search).
void searchField(Ctx& c, const char* id, float x, float y, float w, float h, std::string& s, const char* hint) {
  layoutBox(c, x, y, w, h, "search");
  c.dl->AddRectFilled(c.p(x, y), c.p(x + w, y + h), c.P.field, c.s(M(4)));
  c.dl->AddRect(c.p(x, y), c.p(x + w, y + h), ImGui::IsMouseHoveringRect(c.p(x, y), c.p(x + w, y + h)) ? c.P.lineHi : c.P.line, c.s(M(4)));
  const ImVec2 m = c.p(x + M(16), y + h * 0.5f - M(1));
  c.dl->AddCircle(m, c.s(M(4.2f)), c.P.textFaint, 14, std::max(1.f, c.s(M(1.3f))));
  c.dl->AddLine(ImVec2(m.x + c.s(M(3.2f)), m.y + c.s(M(3.2f))), ImVec2(m.x + c.s(M(6.5f)), m.y + c.s(M(6.5f))), c.P.textFaint, std::max(1.f, c.s(M(1.3f))));
  ImGui::SetCursorScreenPos(c.p(x + M(30), y + M(2)));
  ImGui::PushItemWidth(c.s(w - M(36)));
  char buf[96];
  std::snprintf(buf, sizeof buf, "%s", s.c_str());
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(c.s(M(2)), (c.s(h - M(4)) - ImGui::GetFontSize()) * 0.5f));
  ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));
  ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(0, 0, 0, 0));
  ImGui::PushStyleColor(ImGuiCol_FrameBgActive, IM_COL32(0, 0, 0, 0));
  ImGui::PushStyleColor(ImGuiCol_TextDisabled, c.P.textFaint);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.f);
  if (ImGui::InputTextWithHint(id, hint, buf, sizeof buf)) s = buf;
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor(4);
  ImGui::PopItemWidth();
}

// A folder row in the Library's left column; true on a click.
bool folderRow(Ctx& c, const char* id, float y, const char* label, int count, bool on, int depth) {
  const float x = kLibX0 + M(10), w = kLibSideW - M(20), h = M(28);
  ImGui::PushID(id);
  ImGui::SetCursorScreenPos(c.p(x, y));
  const bool hit = ImGui::InvisibleButton("##fr", c.F.sz(w, h));
  const bool hv = ImGui::IsItemHovered();
  ImGui::PopID();
  if (on || hv) c.dl->AddRectFilled(c.p(x, y), c.p(x + w, y + h - 1), on ? c.P.panelHi : theme::withAlpha(c.P.panelHi, 0.5f), c.s(M(4)));
  if (on) c.dl->AddRectFilled(c.p(x, y + M(3)), c.p(x + M(2.5f), y + h - M(4)), c.P.accent, c.s(M(1)));
  const float fx = x + M(10) + depth * M(20);
  {  // a folder mark
    const ImU32 fc = on ? c.P.textDim : c.P.textFaint;
    const float fy = y + h * 0.5f;
    c.dl->AddRect(c.p(fx, fy - M(3.5f)), c.p(fx + M(11), fy + M(4.5f)), fc, c.s(M(1)), 0, std::max(1.f, c.s(M(1))));
    c.dl->AddLine(c.p(fx, fy - M(3.5f)), c.p(fx + M(4), fy - M(3.5f)), fc, std::max(1.f, c.s(M(2))));
  }
  const float tx = fx + M(21);
  char cnt[16] = "";
  float cw = 0;
  if (count >= 0) {
    std::snprintf(cnt, sizeof cnt, "%d", count);
    MonoText mono;
    cw = textW(c, cnt, T(10), false);
    textR(c, x + w - M(8), y + h * 0.5f - T(10) * 0.68f, cnt, c.P.textFaint, T(10), false);
  }
  c.dl->PushClipRect(c.p(tx, y), c.p(x + w - M(14) - cw, y + h), true);
  text(c, tx, y + h * 0.5f - T(12) * 0.68f, label, on ? c.P.text : c.P.textDim, depth ? T(11.5f) : T(12), false);
  c.dl->PopClipRect();
  return hit;
}

void libHead(const Ctx& c, float y, const char* s) { capsText(c, kLibX0 + M(18), y + M(8) - T(9.5f) * 0.62f, s, c.P.textFaint, T(9.5f), M(1.2f)); }

// A rounded filter chip; true on a click.
bool chip(Ctx& c, const char* id, float x, float y, float& w, const char* label, bool on) {
  w = textW(c, label, T(11.5f), false) + M(24);
  const float h = M(26);
  ImGui::PushID(id);
  ImGui::SetCursorScreenPos(c.p(x, y));
  const bool hit = ImGui::InvisibleButton("##chip", c.F.sz(w, h));
  const bool hv = ImGui::IsItemHovered();
  ImGui::PopID();
  layoutBox(c, x, y, w, h, label);
  c.dl->AddRectFilled(c.p(x, y), c.p(x + w, y + h), on ? c.P.accentSoft : (hv ? c.P.headOn : c.P.panelHi), c.s(h * 0.5f));
  c.dl->AddRect(c.p(x, y), c.p(x + w, y + h), on ? c.P.accent : c.P.line, c.s(h * 0.5f));
  textMid(c, x, x + w, y + h * 0.5f, label, on ? c.P.accentText : c.P.textDim, T(11.5f), false);
  return hit;
}

// The middle column's card grid: n cards, 4 across, scrolled; draw(i, x, y, w, h) for the visible ones.
void cardGrid(Ctx& c, int n, const std::function<void(Ctx&, int, float, float, float, float)>& draw) {
  const float x = kLibX0 + kLibSideW + M(16), x1 = kLibInfoX - M(16), y0 = kLibBodyY + M(52), y1 = kLibY1 - M(4);
  scrollBody(c, x - 12, y0, x1 - x + 24, y1, c.u.libScroll, c.u.libContent, [&](Ctx& cc, float bx, float by, float bw) {
    const float gap = M(10), cw = (bw - gap * 3) / 4, ch = M(124), rowP = ch + M(11);
    const float clipY0 = cc.dl->GetClipRectMin().y, clipY1 = cc.dl->GetClipRectMax().y;
    for (int i = 0; i < n; ++i) {
      const float cx = bx + (i % 4) * (cw + gap), cy = by + (i / 4) * rowP;
      if (cc.p(0, cy + ch).y < clipY0 || cc.p(0, cy).y > clipY1) continue;
      draw(cc, i, cx, cy, cw, ch);
    }
    return by + ((n + 3) / 4) * rowP;
  });
}

// One card: a picture well, the name, a mono note at the right; true on a click (dbl: double-click).
bool card(Ctx& c, int i, float x, float y, float w, float h, bool on, const char* name, const char* note, bool& dbl,
          const std::function<void(float, float, float, float)>& pic) {
  ImGui::PushID(i);
  ImGui::SetCursorScreenPos(c.p(x, y));
  const bool hit = ImGui::InvisibleButton("##card", c.F.sz(w, h));
  const bool hv = ImGui::IsItemHovered();
  dbl = hv && ImGui::IsMouseDoubleClicked(0);
  ImGui::PopID();
  c.dl->AddRectFilled(c.p(x, y), c.p(x + w, y + h), on ? theme::mix(c.P.panelHi, c.P.accent, 0.06f) : (hv ? c.P.headOn : c.P.panelHi), c.s(M(5)));
  c.dl->AddRect(c.p(x, y), c.p(x + w, y + h), on ? c.P.accent : (hv ? c.P.lineHi : c.P.line), c.s(M(5)), 0, on ? std::max(1.f, c.s(M(1.2f))) : 1.f);
  wellLook(c, x + M(8), y + M(8), w - M(16), M(84));
  c.dl->PushClipRect(c.p(x + M(8), y + M(8)), c.p(x + w - M(8), y + M(92)), true);
  pic(x + M(8), y + M(8), w - M(16), M(84));
  c.dl->PopClipRect();
  float nw = 0;
  if (note && *note) {
    MonoText mono;
    nw = textW(c, note, T(10), false) + M(8);
    textR(c, x + w - M(8), y + M(107) - T(10) * 0.68f, note, c.P.textFaint, T(10), false);
  }
  std::string nm = name;
  while (nm.size() > 3 && textW(c, nm.c_str(), T(12), false) > w - M(16) - nw) nm.pop_back();
  if (nm.size() < std::strlen(name)) nm += "\xE2\x80\xA6";
  text(c, x + M(8), y + M(107) - T(12) * 0.68f, nm.c_str(), on ? c.P.text : c.P.textDim, T(12), false);
  if (hv) c.a.setHint(std::string(name) + ": click to choose it, double-click to load it");
  return hit;
}

// A tag with a round outline; returns its width.
float tag(const Ctx& c, float x, float y, const char* s) {
  const float w = textW(c, s, T(10.5f), false) + M(18);
  c.dl->AddRect(c.p(x, y), c.p(x + w, y + M(20)), c.P.line, c.s(M(10)));
  text(c, x + M(9), y + M(10) - T(10.5f) * 0.68f, s, c.P.textDim, T(10.5f), false);
  return w;
}

std::string lower(std::string s) {
  for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  return s;
}

void openFolderOf(const std::string& path) {
  std::string url = "file://";
  std::error_code ec;
  for (unsigned char ch : fs::absolute(fs::u8path(path), ec).parent_path().generic_u8string()) {
    if (std::isalnum(ch) || ch == '/' || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
      url += static_cast<char>(ch);
    } else {
      char b[4];
      std::snprintf(b, sizeof b, "%%%02X", ch);
      url += b;
    }
  }
  collabui::openUrl(url);
}

void tablesTab(Ctx& c) {
  Ui& u = c.u;
  const TableCatalog& cat = tableCatalogUi(c, gRescanTables);
  gRescanTables = false;
  std::string& grp = u.libGroup[0];
  if (grp.empty()) grp = "Factory";
  // the folder column: Factory, your folders, the installed library (all, then each folder)
  auto groupCount = [&](const std::string& prefix, bool exact) {
    int n = 0;
    for (const auto& g : cat.groups) n += (exact ? g.name == prefix : g.name.rfind(prefix, 0) == 0) ? g.count : 0;
    return n;
  };
  float y = kLibBodyY + M(16);
  libHead(c, y, "Factory");
  y += M(24);
  if (folderRow(c, "fac", y, "All factory", groupCount("Factory", true), grp == "Factory", 0)) grp = "Factory", u.libScroll = 0;
  y += M(28) + M(14);
  libHead(c, y, "Your folders");
  y += M(24);
  if (folderRow(c, "mine", y, "My tables", groupCount("My tables", false), grp == "*My tables", 0)) grp = "*My tables", u.libScroll = 0;
  y += M(28);
  for (const auto& g : cat.groups)
    if (g.name.rfind("My tables: ", 0) == 0 && y < kLibY1 - M(60)) {
      if (folderRow(c, g.name.c_str(), y, g.name.c_str() + 11, g.count, grp == g.name, 1)) grp = g.name, u.libScroll = 0;
      y += M(28);
    }
  y += M(14);
  libHead(c, y, "Installed library");
  y += M(24);
  const int libN = groupCount("Library", false);
  if (folderRow(c, "lib", y, "Found on this computer", libN, grp == "*Library", 0)) grp = "*Library", u.libScroll = 0;
  y += M(28);
  for (const auto& g : cat.groups)
    if (g.name.rfind("Library: ", 0) == 0 && y < kLibY1 - M(110)) {
      if (folderRow(c, g.name.c_str(), y, g.name.c_str() + 9, g.count, grp == g.name, 1)) grp = g.name, u.libScroll = 0;
      y += M(28);
    }
  {  // a note: read in place, and Read again
    const float ny = kLibY1 - M(96), nx = kLibX0 + M(16), nw = kLibSideW - M(32);
    c.dl->AddRect(c.p(nx, ny), c.p(nx + nw, ny + M(80)), theme::withAlpha(c.P.line, 0.8f), c.s(M(4)));
    text(c, nx + M(10), ny + M(12), libN ? "Read in place, nothing is copied." : "No installed tables found.", c.P.textFaint, T(10.5f), false);
    if (btnLook(c, "rescan", nx + M(10), ny + M(40), M(100), M(26), "Read again", false, false, T(11))) tableCatalogUi(c, true);
    if (ImGui::IsItemHovered()) c.a.setHint("Lists the tables again: your table folder and the installed wavetable library");
  }
  // what is shown: the folder (or every table while searching), the chip, the sort
  const std::string f = lower(u.sbFilter);
  static bool az = false;
  std::vector<int> shown;
  for (size_t i = 0; i < cat.entries.size(); ++i) {
    const TableEntry& e = cat.entries[i];
    if (!f.empty()) {
      if (lower(e.name + " " + e.group).find(f) == std::string::npos) continue;
    } else if (grp[0] == '*' ? e.group.rfind(grp.substr(1), 0) != 0 : e.group != grp) {
      continue;
    }
    shown.push_back(static_cast<int>(i));
  }
  if (az) std::stable_sort(shown.begin(), shown.end(), [&](int a, int b) { return lower(cat.entries[static_cast<size_t>(a)].name) < lower(cat.entries[static_cast<size_t>(b)].name); });
  {  // chips: all, single cycles, short and long tables (by their frames, known once read)
    static const char* const names[4] = {"All", "Single cycle", "Up to 16 frames", "Long tables"};
    float cx = kLibX0 + kLibSideW + M(16);
    for (int k = 0; k < 4; ++k) {
      float w = 0;
      if (chip(c, names[k], cx, kLibBodyY + M(14), w, names[k], u.libChip == k)) u.libChip = k, u.libScroll = 0;
      cx += w + M(6);
    }
    if (btnLook(c, "sort", kLibInfoX - M(16) - M(84), kLibBodyY + M(14), M(84), M(26), az ? "Sort: Name" : "Sort: Folder", false, false, T(11)))
      az = !az;
  }
  int budget = 4;
  if (u.libChip > 0) {
    std::vector<int> keep;
    for (int i : shown) {
      const Thumb* th = thumbOf(cat.entries[static_cast<size_t>(i)], budget);
      const int fr = th ? th->frames : -1;
      if (fr < 0 || (u.libChip == 1 && fr == 1) || (u.libChip == 2 && fr > 1 && fr <= 16) || (u.libChip == 3 && fr > 16)) keep.push_back(i);
    }
    shown.swap(keep);
  }
  int& sel = u.libSel[0];
  if (sel < 0 || sel >= static_cast<int>(cat.entries.size())) {
    // the open layer's table, else the first shown
    const int o = std::clamp(u.layer, 0, 2);
    const std::string& ref = c.m.osc[static_cast<size_t>(o)].table;
    const int ft = vali(c, oscP(o, O_Table));
    for (int i : shown) {
      const TableEntry& e = cat.entries[static_cast<size_t>(i)];
      if (e.factory > 0 ? ref.empty() && ft == e.factory : ref == e.ref) sel = i;
    }
    if (sel < 0 && !shown.empty()) sel = shown[0];
  }
  auto loadSel = [&] {
    if (sel < 0 || sel >= static_cast<int>(cat.entries.size())) return;
    int lo = std::clamp(u.libInto, 0, 2);
    if (!present(c, lo)) lo = addPart(c, PLayer);
    if (lo >= 0 && lo < 3) chooseTableEntry(c, lo, cat.entries[static_cast<size_t>(sel)]);
  };
  cardGrid(c, static_cast<int>(shown.size()), [&](Ctx& cc, int k, float x, float y0, float w, float h) {
    const int i = shown[static_cast<size_t>(k)];
    const TableEntry& e = cat.entries[static_cast<size_t>(i)];
    const Thumb* th = thumbOf(e, budget);
    char note[24] = "";
    if (th) std::snprintf(note, sizeof note, "%d fr", th->frames);
    bool dbl = false;
    const bool on = sel == i;
    if (card(cc, i, x, y0, w, h, on, e.name.c_str(), note, dbl, [&](float px, float py, float pw, float ph) {
          if (th) drawStack(cc, *th, px + M(4), py + M(4), pw - M(8), ph - M(8), on ? cc.P.accent : cc.P.text, on);
        }))
      sel = i;
    if (dbl) sel = i, loadSel();
  });
  if (shown.empty())
    textC(c, (kLibX0 + kLibSideW + kLibInfoX) * 0.5f, kLibBodyY + M(120), f.empty() ? "No tables in this folder" : "No table has that in its name", c.P.textFaint, T(12), false);
  // the chosen table
  const float x = kLibInfoX + M(16), w = kLibX1 - M(16) - x, iy = kLibBodyY + M(16);
  if (sel < 0 || sel >= static_cast<int>(cat.entries.size())) return;
  const TableEntry& e = cat.entries[static_cast<size_t>(sel)];
  c.dl->PushClipRect(c.p(x, iy - M(4)), c.p(x + w, iy + M(24)), true);
  text(c, x, iy + M(10) - T(15) * 0.68f, e.name.c_str(), c.P.text, T(15), true);
  text(c, x + textW(c, e.name.c_str(), T(15), true) + M(8), iy + M(11) - T(11) * 0.68f, e.group.c_str(), c.P.textFaint, T(11), false);
  c.dl->PopClipRect();
  const float wy = iy + M(24), wh = M(200);
  wellLook(c, x, wy, w, wh);
  gridLook(c, x, wy, w, wh, 8, 4, false);
  capsText(c, x + M(9), wy + M(8), "Preview", c.P.textFaint, T(9), M(1));
  int b2 = 4;
  const Thumb* th = thumbOf(e, b2);
  if (th) drawStack(c, *th, x + M(8), wy + M(16), w - M(16), wh - M(20), partCol(c, std::clamp(u.libInto, 0, 2)), true);
  float ty = wy + wh + M(12), tx = x;
  if (th) {
    char b[32];
    std::snprintf(b, sizeof b, th->frames == 1 ? "%d frame" : "%d frames", th->frames);
    tx += tag(c, tx, ty, b) + M(6);
    std::snprintf(b, sizeof b, "%d points", kFrameSize);
    tx += tag(c, tx, ty, b) + M(6);
  }
  ty += M(36);
  text(c, x, ty, "Load into", c.P.textDim, T(12), false);
  ty += M(20);
  const int hit = segPick(c, "into", x, ty, w, M(28), {"Layer 1", "Layer 2", "Layer 3"}, std::clamp(u.libInto, 0, 2), false, T(11.5f));
  if (hit >= 0) u.libInto = hit;
  const float by = kLibY1 - M(16) - M(30), bw = (w - M(8)) * 0.5f;
  if (e.factory == 0 && e.ref.rfind("file:", 0) == 0) {
    if (btnLook(c, "where", x, by, bw, M(30), "Show in folder", false, false, T(12))) openFolderOf(e.ref.substr(5));
  } else {
    text(c, x, by + M(15) - T(11) * 0.68f, "Built in", c.P.textFaint, T(11), false);
  }
  if (btnLook(c, "load", x + bw + M(8), by, bw, M(30), "Load", false, true, T(12))) loadSel();
  if (ImGui::IsItemHovered()) c.a.setHint("Load the table into the chosen layer (or double-click its card)");
}

void shapesTab(Ctx& c) {
  Ui& u = c.u;
  static const std::vector<ShapeItem> items = shapeItems();
  std::string& grp = u.libGroup[1];
  if (grp.empty()) grp = "*";
  int counts[4] = {static_cast<int>(items.size()), 0, 0, 0};
  for (const auto& s : items) ++counts[s.kind];
  static const char* const kinds[4] = {"All shapes", "LFO shapes", "Root shapes", "Texture sources"};
  static const char* const keys[4] = {"*", "lfo", "root", "tex"};
  float y = kLibBodyY + M(16);
  libHead(c, y, "Factory");
  y += M(24);
  for (int k = 0; k < 4; ++k) {
    if (folderRow(c, keys[k], y, kinds[k], counts[k], grp == keys[k], k ? 1 : 0)) grp = keys[k], u.libScroll = 0;
    y += M(28);
  }
  int kind = 0;
  for (int k = 1; k < 4; ++k)
    if (grp == keys[k]) kind = k;
  {  // chips: the same kinds
    static const char* const chipNames[4] = {"All", "LFO", "Root", "Texture"};
    float cx = kLibX0 + kLibSideW + M(16);
    for (int k = 0; k < 4; ++k) {
      float w = 0;
      if (chip(c, chipNames[k], cx, kLibBodyY + M(14), w, chipNames[k], kind == k)) grp = keys[k], u.libScroll = 0, kind = k;
      cx += w + M(6);
    }
  }
  const std::string f = lower(u.sbFilter);
  std::vector<int> shown;
  for (size_t i = 0; i < items.size(); ++i)
    if ((!f.empty() || kind == 0 || items[i].kind == kind) && (f.empty() || lower(items[i].name).find(f) != std::string::npos)) shown.push_back(static_cast<int>(i));
  int& sel = u.libSel[1];
  if ((sel < 0 || sel >= static_cast<int>(items.size())) && !shown.empty()) sel = shown[0];
  static const char* const kindNote[4] = {"", "LFO", "Root", "Texture"};
  auto colOf = [&](const ShapeItem& s) { return s.kind == 1 ? c.P.fam[FamLfo] : partCol(c, s.kind == 2 ? 3 : 4); };
  auto loadSel = [&] {
    if (sel < 0 || sel >= static_cast<int>(items.size())) return;
    const ShapeItem& s = items[static_cast<size_t>(sel)];
    if (s.kind == 1) {
      loadShape(c, std::clamp(u.libInto, 0, 3), s.n);
    } else if (s.kind == 2) {
      if (!present(c, 3)) addPart(c, PRoot);
      setv(c, subP(S_Shape), s.n);
    } else {
      if (!present(c, 4)) addPart(c, PTexture);
      setv(c, noiseP(N_Type), s.n);
    }
  };
  cardGrid(c, static_cast<int>(shown.size()), [&](Ctx& cc, int k, float x, float y0, float w, float h) {
    const int i = shown[static_cast<size_t>(k)];
    const ShapeItem& s = items[static_cast<size_t>(i)];
    bool dbl = false;
    const bool on = sel == i;
    if (card(cc, i, x, y0, w, h, on, s.name.c_str(), kindNote[s.kind], dbl,
             [&](float px, float py, float pw, float ph) { drawShape(cc, s, px + M(6), py + M(8), pw - M(12), ph - M(16), on ? colOf(s) : cc.P.textDim, false); }))
      sel = i;
    if (dbl) sel = i, loadSel();
  });
  if (shown.empty()) textC(c, (kLibX0 + kLibSideW + kLibInfoX) * 0.5f, kLibBodyY + M(120), "No shape has that in its name", c.P.textFaint, T(12), false);
  if (sel < 0 || sel >= static_cast<int>(items.size())) return;
  const ShapeItem& s = items[static_cast<size_t>(sel)];
  const float x = kLibInfoX + M(16), w = kLibX1 - M(16) - x, iy = kLibBodyY + M(16);
  text(c, x, iy + M(10) - T(15) * 0.68f, s.name.c_str(), c.P.text, T(15), true);
  text(c, x + textW(c, s.name.c_str(), T(15), true) + M(8), iy + M(11) - T(11) * 0.68f, kinds[s.kind], c.P.textFaint, T(11), false);
  const float wy = iy + M(24), wh = M(200);
  wellLook(c, x, wy, w, wh);
  gridLook(c, x, wy, w, wh, 8, 4, true);
  capsText(c, x + M(9), wy + M(8), "Preview", c.P.textFaint, T(9), M(1));
  drawShape(c, s, x + M(8), wy + M(22), w - M(16), wh - M(30), colOf(s), true);
  float ty = wy + wh + M(12);
  tag(c, x, ty, kinds[s.kind]);
  ty += M(36);
  text(c, x, ty, "Load into", c.P.textDim, T(12), false);
  ty += M(20);
  if (s.kind == 1) {
    const int hit = segPick(c, "into", x, ty, w, M(28), {"LFO 1", "LFO 2", "LFO 3", "LFO 4"}, std::clamp(u.libInto, 0, 3), false, T(11.5f));
    if (hit >= 0) u.libInto = hit;
  } else {
    segPick(c, "into1", x, ty, w, M(28), {s.kind == 2 ? "Root" : "Texture"}, 0, false, T(11.5f));
  }
  const float by = kLibY1 - M(16) - M(30), bw = (w - M(8)) * 0.5f;
  if (btnLook(c, "load", x + bw + M(8), by, bw, M(30), "Load", false, true, T(12))) loadSel();
  if (ImGui::IsItemHovered()) c.a.setHint("Give the chosen part this shape (or double-click its card)");
}

}  // namespace

void libraryPage(Ctx& c) {
  Ui& u = c.u;
  u.sbLib = std::clamp(u.sbLib, 0, 2);
  // the frame: header, then folders | items | the chosen one
  const float X0 = kLibX0, X1 = kLibX1, Y0 = kLibY0, Y1 = kLibY1;
  c.dl->AddRectFilled(c.p(X0, Y0), c.p(X1, Y1), c.P.panel, c.s(M(6)));
  c.dl->AddRectFilled(c.p(X0 + kLibSideW, kLibBodyY), c.p(kLibInfoX, Y1), theme::mix(c.P.panel, c.P.panelHi, 0.3f));
  c.dl->AddRect(c.p(X0, Y0), c.p(X1, Y1), c.P.line, c.s(M(6)));
  hline(c, X0, X1, kLibBodyY, c.P.line);
  vline(c, X0 + kLibSideW, kLibBodyY, Y1, c.P.line);
  vline(c, kLibInfoX, kLibBodyY, Y1, c.P.line);
  // the header: Back, the title, the search, the kind, Import
  if (btnLook(c, "back", X0 + M(16), Y0 + M(14), M(62), M(28), "Back", false, false, T(12))) closeOverlay(u);
  if (ImGui::IsItemHovered()) c.a.setHint("Back to where you were (Esc)");
  text(c, X0 + M(90), Y0 + M(28) - T(16) * 0.68f, "Library", c.P.text, T(16), true);
  {
    char hint[96];
    if (u.sbLib == 0) std::snprintf(hint, sizeof hint, i18n::tr("Search %d tables by name or folder"), static_cast<int>(tableCatalogUi(c).entries.size()));
    else if (u.sbLib == 1) std::snprintf(hint, sizeof hint, i18n::tr("Search %d shapes"), static_cast<int>(shapeItems().size()));
    else std::snprintf(hint, sizeof hint, "%s", i18n::tr("Search sounds by name or folder"));
    searchField(c, "##libfind", X0 + M(158), Y0 + M(13), M(420), M(30), u.sbLib == 2 ? u.browserFilter : u.sbFilter, hint);
  }
  {
    const float iw = M(62), sw = M(194), ix = X1 - M(16) - iw, sx = ix - M(12) - sw;
    const int hit = segPick(c, "libkind", sx, Y0 + M(14), sw, M(28), {"Tables", "Shapes", "Sounds"}, u.sbLib, false, T(11.5f));
    if (hit >= 0 && hit != u.sbLib) u.sbLib = hit, u.libScroll = 0, u.libInto = 0;
    if (btnLook(c, "import", ix, Y0 + M(14), iw, M(28), "Import", false, false, T(12))) {
      if (u.sbLib == 2) {
        openPresetDialog(c);
      } else {
        openFile(c, i18n::tr("Import a wavetable"), {".wav"}, [](App& a, const PluginTarget&, const std::string& p) {
          std::error_code ec;
          const fs::path dir = fs::u8path(userTableDir(a.configDir()));
          fs::create_directories(dir, ec);
          fs::copy_file(fs::u8path(p), dir / fs::u8path(p).filename(), fs::copy_options::skip_existing, ec);
          if (ec) {
            a.notify(i18n::tr("Could not copy the wavetable"), true);
          } else {
            a.notify(i18n::tr("Added to My tables"), false);
            gRescanTables = true;
          }
        });
        u.libGroup[0] = "*My tables";
      }
    }
    if (ImGui::IsItemHovered()) c.a.setHint(u.sbLib == 2 ? "Open a sound file" : "Copy a wavetable WAV into My tables");
  }
  if (u.sbLib == 0) tablesTab(c);
  else if (u.sbLib == 1) shapesTab(c);
  else presetBrowser(c);
}

void macroRenamePopup(Ctx& c, int i) {
  static char buf[48];
  if (macroRenameRequest() == i) {
    macroRenameRequest() = -1;
    std::snprintf(buf, sizeof buf, "%s", c.m.macroName[static_cast<size_t>(i)].c_str());
    ImGui::OpenPopup("##mren");
  }
  if (ImGui::BeginPopup("##mren")) {
    ImGui::TextUnformatted(i18n::tr("Macro name"));
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    if (ImGui::InputText("##mn", buf, sizeof buf, ImGuiInputTextFlags_EnterReturnsTrue)) {
      c.m.macroName[static_cast<size_t>(i)] = buf;
      commit(c, "Rename macro");
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
}

void synthTab(Ctx& c) {
  Ui& u = c.u;
  static int devFrames = 0;
  if (u.devLayers > 0 && ++devFrames > 3) {  // dev: n layers on
    for (int o = 0; o < kOscs; ++o)
      if ((val(c, oscP(o, O_Enable)) >= 0.5) != (o < u.devLayers)) setv(c, oscP(o, O_Enable), o < u.devLayers ? 1 : 0);
    u.devLayers = 0;
  }
  if (u.devPlay && ++devFrames > 4) {  // dev (+play): a held note with LFO 1 moving Layer 1's position, for the live views
    setv(c, modP(0, M_Source), SrcLfo1), setv(c, modP(0, M_Amount), 0.8), setv(c, modP(0, M_Dest), oscP(0, O_WtPos) + 1);
    previewKey(c, 60, true);
    u.devPlay = false;
  }
  const float x1 = kW - sidebarW(u);
  panelRect(c, 0, kTopH, kW, kPlayY - kTopH, c.P.frame);
  // the design: the content 20 from the left, 16 under the top bar, 16 left of the sidebar
  const float y0 = kTopH + M(16) - 4, y1 = kPlayY - drawerH(u) - M(6);
  if (u.devScrollMod && u.modY > 0 && ++devFrames > 6) u.synthScroll = u.modY - 8;
  scrollBody(c, M(20) - 12, y0, x1 - M(20) + 24, y1, u.synthScroll, u.synthContent, [](Ctx& cc, float x, float y, float w) { return synthGrid(cc, x, y, w); });
  // a part dropped on empty space: add it (a table: a new layer with it; a shape: a new LFO)
  if (ImGui::BeginDragDropTargetCustom(ImRect(c.p(0, y0), c.p(x1, y1)), ImGui::GetID("##synthdrop"))) {
    if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("PLASMA_PART", ImGuiDragDropFlags_AcceptNoDrawDefaultRect)) {
      int v = 0;
      std::memcpy(&v, p->Data, sizeof(int));
      {
        const ImVec2 m = ImGui::GetIO().MousePos;
        u.dropAt = ImVec2((m.x - c.p(0, 0).x) / c.s(1) - 12, (m.y - c.p(0, y0 + 4 - u.synthScroll).y) / c.s(1));
      }
      if (v >= kPartShape) {
        const int id = addPart(c, PLfo);
        if (id >= 100) loadShape(c, id - 100, v - kPartShape);
      } else if (v >= kPartTable) {
        const int id = addPart(c, PLayer);
        if (id >= 0 && id < 3) loadTable(c, id, v - kPartTable);
      } else if (v < kPartFx) {
        addPart(c, v);
      }
    }
    ImGui::EndDragDropTarget();
  }
  if (u.devDrag >= 0 && (u.devDrag < kPartFx || u.devDrag >= kPartTable)) devGhost(c, u.devDrag, x1 - 220, 300);
}

void effectsTab(Ctx& c) {
  Ui& u = c.u;
  const Pal& P = c.P;
  panelRect(c, 0, kTopH, kW, kPlayY - kTopH, P.frame);
  const auto nodes = resolvedFx(c.m, c.st.params);
  static const char* const names[3] = {"Main", "Send A", "Send B"};
  static const char* const subs[3] = {"every sound goes through it", "fed by each part's send", "fed by each part's send"};
  const float x0 = M(20), x1 = kW - sidebarW(u), gap = M(12), lw = (x1 - x0 - gap * 2) / 3;
  const float y0 = kTopH + M(16), y1 = kPlayY - M(16) - drawerH(u);
  for (int r = 0; r < 3; ++r) {
    const float x = x0 + r * (lw + gap);
    ImGui::PushID(r + 7800);
    c.dl->AddRectFilled(c.p(x, y0), c.p(x + lw, y1), barBg(c), c.s(M(6)));
    c.dl->AddRect(c.p(x, y0), c.p(x + lw, y1), u.fxRack == r ? P.lineHi : P.line, c.s(M(6)));
    int n = 0;
    for (const auto& q : nodes) n += q.rack == r && vali(c, fxP(q.slot, X_Type)) != FxNone;
    const float ix = x + M(12), iw = lw - M(24), hy = y0 + M(12), hh = M(28), hc = hy + hh * 0.5f;
    text(c, ix, hc - T(14) * 0.68f, names[r], P.text, T(14), true);
    float rx = ix + iw;
    {  // its level setting as a small bar (Main: the output, a send: its return)
      const float mw = M(40), mh = M(10);
      rx -= mw;
      const int lk = r == 0 ? G_MasterVol : (r == 1 ? G_Bus1Vol : G_Bus2Vol);
      const float by = hc - mh * 0.5f, fw = mw * std::clamp(valf(c, lk), 0.f, 1.f);
      c.dl->AddRectFilled(c.p(rx, by), c.p(rx + mw, by + mh), P.field, c.s(M(1)));
      if (fw > 0.5f) c.dl->AddRectFilled(c.p(rx, by), c.p(rx + fw, by + mh), n ? P.good : P.textFaint, c.s(M(1)));
      rx -= M(10);
    }
    {
      char b[24];
      std::snprintf(b, sizeof b, n == 1 ? "%d effect" : "%d effects", n);
      MonoText mono;
      const float bw = textW(c, b, T(10.5f), false);
      rx -= bw;
      text(c, rx, hc - T(10.5f) * 0.68f, b, P.textFaint, T(10.5f), false);
      rx -= M(10);
    }
    {
      const float tx = ix + textW(c, names[r], T(14), true) + M(10);
      if (textW(c, subs[r], T(11.5f), false) < rx - tx) text(c, tx, hc - T(11.5f) * 0.68f, subs[r], P.textFaint, T(11.5f), false);
    }
    ImGui::SetCursorScreenPos(c.p(x, y0));
    if (ImGui::InvisibleButton("##lanehead", c.F.sz(lw, M(48)))) u.fxRack = r;
    if (ImGui::IsItemHovered()) c.a.setHint(std::string(names[r]) + ": click to make it the lane the sidebar adds to");
    // the foot, then the effects between
    const float fy = y1 - M(18) - M(28);
    hline(c, ix, ix + iw, fy - M(15), P.line);
    fxLaneFoot(c, r, ix, fy, iw);
    scrollBody(c, ix - 12, y0 + M(48) - 4, iw + 24, fy - M(22), u.fxScroll[r], u.fxContent[r],
               [r](Ctx& cc, float bx, float by, float bw) { return fxLane(cc, r, bx, by, bw); });
    ImGui::PopID();
  }
  if (u.devDrag >= kPartFx && u.devDrag < kPartTable) devGhost(c, u.devDrag, x1 - 220, 300);
}

void sidebar(Ctx& c) {
  Ui& u = c.u;
  const bool fxTab = u.view == VEffects;
  const Pal& P = c.P;
  const float sx = kW - M(20) - M(248), sw = M(248), sy = kTopH + M(16), sb = kPlayY - M(16) - drawerH(u);
  if (sb - sy < M(60)) return;
  c.dl->AddRectFilled(c.p(sx, sy), c.p(sx + sw, sb), barBg(c), c.s(M(6)));
  c.dl->AddRect(c.p(sx, sy), c.p(sx + sw, sb), P.line, c.s(M(6)));
  const float ix = sx + M(14), iw = sw - M(28);
  float y = sy + M(14);
  auto head = [&](const char* title, const char* note) {  // a mono capitals head with a note at the right
    {
      MonoText mono;
      capsText(c, ix, y + M(7.5f) - T(10.5f) * 0.68f, title, P.label, T(10.5f), M(1.05f));
    }
    if (note && *note) textR(c, ix + iw, y + M(7.5f) - T(10.5f) * 0.68f, note, P.textFaint, T(10.5f), false);
    y += M(15) + M(12);
  };
  auto group = [&](const char* title) {
    MonoText mono;
    y += M(2);
    capsText(c, ix, y + M(6.75f) - T(10) * 0.68f, title, P.textFaint, T(10), M(0.8f));
    y += M(13.5f) - M(4) + M(12) - M(2);
  };
  if (fxTab) {  // the effect types by what they do
    int used = 0;
    for (int s = 0; s < kFxSlots; ++s) used += vali(c, fxP(s, X_Type)) != FxNone;
    const bool full = used >= kFxSlots;
    char b[48];
    std::snprintf(b, sizeof b, "%d of %d free", kFxSlots - used, kFxSlots);
    head("Effects", b);
    scrollBody(c, ix - 12, y - 4, iw + 24, sb - M(8), u.sbModScroll, u.sbModContent, [full](Ctx& cc, float bx, float by, float bw) {
      struct G {
        const char* name;
        std::vector<int> types;
      };
      static const std::vector<G> groups = {{"Tone", {FxDistortion, FxFilter, FxEq}},
                                            {"Movement", {FxChorus, FxFlanger, FxPhaser, FxHyper, FxBode}},
                                            {"Room and echo", {FxDelay, FxReverb, FxConvolve}},
                                            {"Level", {FxComp, FxUtility}},
                                            {"Split into bands", {FxSplitLH, FxSplitLMH, FxSplitMS}}};
      const float tw = (bw - M(6)) / 2, th = M(40);
      for (const auto& g : groups) {
        {
          MonoText mono;
          capsText(cc, bx, by + M(2), g.name, cc.P.textFaint, T(10), M(0.8f));
        }
        by += M(13.5f) + M(8);
        int i = 0;
        for (int t : g.types) fxTile(cc, t, bx + (i % 2) * (tw + M(6)), by + (i / 2) * (th + M(6)), tw, th, full), ++i;
        by += ((i + 1) / 2) * (th + M(6)) + M(6);
      }
      return by;
    });
    return;
  }
  head("Parts", "drag into a section");
  {
    const float tw = (iw - M(6)) / 2, th = M(40);
    auto tiles = [&](std::initializer_list<int> parts) {
      int i = 0;
      for (int p : parts) partTile(c, p, ix + (i % 2) * (tw + M(6)), y + (i / 2) * (th + M(6)), tw, th), ++i;
      y += ((i + 1) / 2) * (th + M(6)) - M(6) + M(12);
    };
    group("Sources");
    tiles({PLayer, PRoot, PTexture});
    group("Shape");
    tiles({PFilter, kAmpTile});
    group("Movement");
    tiles({PLfo, PEnv, PMacro, PKeys, PChance});
  }
  hline(c, ix, ix + iw, y + M(2), P.line);
  y += M(1 + 4) + M(12);
  head("Library", "");
  {  // search
    const float h = M(28);
    static const char* const ph[3] = {"Search tables", "Search shapes", "Search sounds"};
    layoutBox(c, ix, y, iw, h, "search");
    c.dl->AddRectFilled(c.p(ix, y), c.p(ix + iw, y + h), P.field, c.s(M(4)));
    c.dl->AddRect(c.p(ix, y), c.p(ix + iw, y + h), ImGui::IsMouseHoveringRect(c.p(ix, y), c.p(ix + iw, y + h)) ? P.lineHi : P.line, c.s(M(4)));
    {  // a magnifier
      const ImVec2 m = c.p(ix + M(9 + 5), y + h * 0.5f - M(1));
      c.dl->AddCircle(m, c.s(M(3.8f)), P.textFaint, 14, std::max(1.f, c.s(M(1.3f))));
      c.dl->AddLine(ImVec2(m.x + c.s(M(2.9f)), m.y + c.s(M(2.9f))), ImVec2(m.x + c.s(M(6)), m.y + c.s(M(6))), P.textFaint, std::max(1.f, c.s(M(1.3f))));
    }
    ImGui::SetCursorScreenPos(c.p(ix + M(9 + 12 + 6), y + M(2)));
    ImGui::PushItemWidth(c.s(iw - M(9 + 12 + 6 + 6)));
    char buf[64];
    std::snprintf(buf, sizeof buf, "%s", u.sbFilter.c_str());
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(c.s(M(2)), (c.s(h - M(4)) - ImGui::GetFontSize()) * 0.5f));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, P.textFaint);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.f);
    if (ImGui::InputTextWithHint("##sbfind", i18n::tr(ph[std::clamp(u.sbLib, 0, 2)]), buf, sizeof buf)) u.sbFilter = buf;
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(4);
    ImGui::PopItemWidth();
    y += h + M(12);
  }
  {
    const int hit = segPick(c, "libtabs", ix, y, iw, M(28), {"Tables", "Shapes", "Sounds"}, u.sbLib, false, T(11.5f));
    if (hit >= 0 && hit != u.sbLib) u.sbLib = hit, u.sbScroll = 0;
    y += M(28) + M(12);
  }
  scrollBody(c, ix - 12, y - 4, iw + 24, sb - M(8), u.sbScroll, u.sbContent, [](Ctx& cc, float bx, float by, float bw) { return libraryList(cc, bx, by, bw); });
}

}  // namespace gl::packs::plasma::ui
