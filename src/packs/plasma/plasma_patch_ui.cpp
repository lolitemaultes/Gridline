// Gridline — "plasma" pack: the Patch view. The same sound as boxes and wires in strict columns, left
// to right in signal order: what plays the notes, the five sources, the two filters, the amp, the
// main effects and the two sends, the output; the movement sources in a row below. Wires run
// square in the gaps between the columns; movement wires show on hover, select or Show Movement. A sound wire is a route
// setting, a motion wire is one link slot: the graph is a picture of the fixed engine, so it can
// only draw (and make) what the engine can do. Every edit is the same parameter edit the panels
// make (one undo step a gesture). Node places, zoom and the selection are editor state only.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include "../../core/I18n.h"
#include "../../third_party/imgui/imgui_internal.h"
#include "plasma_ui.h"

namespace gl::packs::plasma::ui {
namespace {

// Node ids.
enum : int { NSrc = 0, NFilt = 10, NAmp = 20, NOut = 21, NSendA = 22, NSendB = 23, NVoices = 30, NArp = 31, NPhrase = 32, NFx = 100, NMove = 200 };
// Where a sound wire ends.
enum : int { DF1 = 0, DF2, DAmp, DOut, DSendA, DSendB, DChain };


struct PNode {
  int id = 0, col = 0;
  float x = 0, y = 0, w = 0, h = 0;  // graph units
  bool on = true;
};
struct PWire {
  int from, to;
  int kind;      // 0 sound, 1 send, 2 notes, 3 a step down the column (an effect into the next)
  float weight;  // sound: share of the source (filter balance)
  int r;         // route of the source end (-1 none)
  int dest;      // DF1..DChain
};

int routeOfNode(int id) {
  if (id >= NSrc && id < NSrc + 3) return id - NSrc;  // RtA..RtC
  if (id == NSrc + 3) return RtSub;
  if (id == NSrc + 4) return RtNoise;
  if (id == NFilt) return RtF1;
  if (id == NFilt + 1) return RtF2;
  return -1;
}
int nodeOfRoute(int r) {
  if (r <= RtC) return NSrc + r;
  if (r == RtSub) return NSrc + 3;
  if (r == RtNoise) return NSrc + 4;
  return r == RtF1 ? NFilt : NFilt + 1;
}
const char* nodeTitle(int id, char* buf, size_t n) {
  static const char* const src[5] = {"Layer 1", "Layer 2", "Layer 3", "Root", "Texture"};
  if (id >= NSrc && id < NSrc + 5) return src[id - NSrc];
  if (id == NFilt || id == NFilt + 1) return id == NFilt ? "Filter 1" : "Filter 2";
  switch (id) {
    case NAmp: return "Amp";
    case NOut: return "Out";
    case NSendA: return "Send A";
    case NSendB: return "Send B";
    case NVoices: return "Voices";
    case NArp: return "Arpeggiator";
    case NPhrase: return "Phrases";
    default: break;
  }
  if (id >= NMove) {
    std::snprintf(buf, n, "%s", sourceLabel(id - NMove).c_str());
    return buf;
  }
  return "Effect";
}
int enableOf(int id) {
  if (id >= NSrc && id < NSrc + 3) return oscP(id - NSrc, O_Enable);
  if (id == NSrc + 3) return subP(S_Enable);
  if (id == NSrc + 4) return noiseP(N_Enable);
  if (id == NFilt || id == NFilt + 1) return filtP(id - NFilt, F_Enable);
  if (id >= NFx && id < NFx + kFxSlots) return fxP(id - NFx, X_On);
  if (id == NArp) return G_ArpOn;
  if (id == NPhrase) return G_ClipOn;
  return -1;
}
// The node that owns parameter k (a link wire ends there).
int ownerNode(int k) {
  if (k >= kOscBase && k < kSubBase) return NSrc + (k - kOscBase) / O_Count;
  if (k >= kSubBase && k < kNoiseBase) return NSrc + 3;
  if (k >= kNoiseBase && k < kFiltBase) return NSrc + 4;
  if (k >= kFiltBase && k < kRouteBase) return NFilt + (k - kFiltBase) / F_Count;
  if (k >= kRouteBase && k < kEnvBase) return nodeOfRoute((k - kRouteBase) / R_Count);
  if (k >= kEnvBase && k < kLfoBase) {
    const int e = (k - kEnvBase) / E_Count;
    return e == 0 ? NAmp : NMove + SrcEnv1 + e;
  }
  if (k >= kLfoBase && k < kMacroBase) return NMove + SrcLfo1 + (k - kLfoBase) / L_Count;
  if (k >= kMacroBase && k < kMacroBase + kMacros) return NMove + SrcMacro1 + (k - kMacroBase);
  if (k >= kFxBase && k < kTailBase) return NFx + (k - kFxBase) / X_Count;
  if (k == G_Amp) return NAmp;
  if (k == G_MasterVol || k == G_DirectVol || k == G_Bus1Vol || k == G_Bus2Vol || k == G_Bus1Dest || k == G_Bus2Dest) return NOut;
  if (k == tailP(T_NoiseTone)) return NSrc + 4;
  if (k == tailP(T_Pad1)) return NFilt;
  if (k == tailP(T_Pad2)) return NFilt + 1;
  return NVoices;
}
// Every parameter of a node (the open node's list).
std::vector<int> nodeParams(const Ctx& c, int id) {
  std::vector<int> v;
  auto range = [&](int a, int n) {
    for (int q = 0; q < n; ++q) v.push_back(a + q);
  };
  auto route = [&](int r) {
    range(routeP(r, 0), R_Count);
    v.push_back(tailP(T_RawA + r));
  };
  if (id >= NSrc && id < NSrc + 3) range(oscP(id - NSrc, 0), O_Count), route(id - NSrc);
  else if (id == NSrc + 3) range(subP(0), S_Count), route(RtSub);
  else if (id == NSrc + 4) {
    range(noiseP(0), N_Count), route(RtNoise);
    for (int t : {T_NoiseTone, T_NKeyMin, T_NKeyMax, T_NKeyFold, T_NoiseLaw}) v.push_back(tailP(t));
  } else if (id == NFilt || id == NFilt + 1) {
    const int f = id - NFilt;
    range(filtP(f, 0), F_Count), route(f == 0 ? RtF1 : RtF2);
    v.push_back(tailP(f == 0 ? T_Pad1 : T_Pad2));
  } else if (id == NAmp) {
    range(envP(0, 0), E_Count), v.push_back(G_Amp);
  } else if (id == NOut) {
    for (int k : {G_MasterVol, G_DirectVol, G_Bus1Vol, G_Bus2Vol, G_Bus1Dest, G_Bus2Dest}) v.push_back(k);
  } else if (id == NSendA || id == NSendB) {
    v.push_back(id == NSendA ? G_Bus1Vol : G_Bus2Vol);
    v.push_back(id == NSendA ? G_Bus1Dest : G_Bus2Dest);
    if (id == NSendA) v.push_back(tailP(T_BusChain));
  } else if (id == NVoices) {
    for (int k : {G_Poly, G_Mono, G_Legato, G_PortaTime, G_PortaCurve, G_PortaAlways, G_PortaScaled, G_Transpose, G_MasterTune, G_BendUp,
                  G_BendDown, G_Priority, G_SameNote, G_EnvScale, G_LfoScale, G_Key, G_Scale})
      v.push_back(k);
  } else if (id == NArp || id == NPhrase) {
    v.push_back(id == NArp ? G_ArpOn : G_ClipOn);
  } else if (id >= NFx && id < NFx + kFxSlots) {
    const int s = id - NFx, type = vali(c, fxP(s, X_Type));
    v.push_back(fxP(s, X_On));
    const int n = static_cast<int>(fxParams(type).size());
    range(fxP(s, X_P0), std::min(n, kFxParams));
  } else if (id >= NMove) {
    const int src = id - NMove;
    if (src >= SrcEnv1 && src <= SrcEnv4) range(envP(src - SrcEnv1, 0), E_Count);
    else if (src >= SrcLfo1 && src <= SrcLfo10) range(lfoP(src - SrcLfo1, 0), L_Count);
    else if (src >= SrcMacro1 && src <= SrcMacro8) v.push_back(macroP(src - SrcMacro1));
  }
  return v;
}

// A pin row's name in plain words: the control's own name, sentence case, the usual short forms
// spelled out ("WT POS" -> "Position").
std::string plainName(int k) {
  std::string n = paramShortName(k);
  static const std::pair<const char*, const char*> words[] = {
      {"WT POS", "Position"}, {"FREQ", "Cutoff"}, {"RES", "Resonance"}, {"VOL", "Level"}, {"LVL", "Level"}, {"PAN", "Pan"},
      {"OCT", "Octave"}, {"SEMI", "Semitone"}, {"FINE", "Fine"}, {"ATK", "Attack"}, {"DEC", "Decay"}, {"SUS", "Sustain"},
      {"REL", "Release"}, {"UNI", "Unison"}, {"DET", "Detune"}, {"BPM", "Tempo sync"}};
  for (const auto& w : words)
    if (n == w.first) return w.second;
  for (size_t i = 1; i < n.size(); ++i) n[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(n[i])));
  return n;
}

// ---- the layout: strict columns (graph units are design pixels at 100%), rows 100 apart, compact
// boxes; the selected box grows a dial row and pushes its column down
struct ColDef {
  float x, w;
};
constexpr ColDef kCols[6] = {{24, 96}, {168, 168}, {384, 168}, {600, 150}, {798, 160}, {1006, 52}};
enum : int { CPlay = 0, CSrc, CShape, CAmp, CFx, COut };
constexpr float kRow0 = 44, kRowStep = 100, kBoxH = 86, kKnobRowH = 66, kPinMid = 43, kPinLow = 65, kMoveRow0 = 616;
float TG(float px) { return px / 1.25f; }  // a text size in graph units
float rowY(int i) { return kRow0 + kRowStep * static_cast<float>(i); }

// Movement boxes: the macros share one box, an LFO's second output is the LFO's box.
int moveNodeOf(int src) {
  if (src >= SrcMacro1 && src <= SrcMacro8) return NMove + SrcMacro1;
  if (src >= SrcLfo1Y && src <= SrcLfo10Y) return NMove + SrcLfo1 + (src - SrcLfo1Y);
  return NMove + src;
}
ImU32 nodeColour(const Ctx& c, int id) {
  if (id >= NMove) return c.P.fam[familyOf(id - NMove)];
  if (id == NVoices) return c.P.textDim;
  if (id == NOut) return c.P.accent;
  if (id >= NFx && id < NFx + kFxSlots) return fxCol(c, vali(c, fxP(id - NFx, X_Type)));
  if (id == NSendA || id == NSendB) {  // the colour of its first effect
    for (const auto& n : resolvedFx(c.m, c.st.params))
      if (n.parent < 0 && n.rack == (id == NSendA ? 1 : 2) && vali(c, fxP(n.slot, X_Type)) != FxNone) return fxCol(c, vali(c, fxP(n.slot, X_Type)));
    return c.P.textDim;
  }
  return partCol(c, id);
}

// ---- the graph of the current sound
struct Graph {
  std::vector<PNode> nodes;
  std::vector<PWire> wires;
  std::map<int, size_t> at;  // id -> index
  std::map<int, int> fxBox;  // effect slot -> the box that shows it
  std::vector<int> chain[3];
  int mainHead = NOut;  // where the main chain starts: its first effect, or Out
  float mainB = 0, sendY = 0, busY = 0, moveY = 0, maxX = 0, maxY = 0;
  PNode* find(int id) {
    auto it = at.find(id);
    return it == at.end() ? nullptr : &nodes[it->second];
  }
};
Ui* gPatchUi = nullptr;  // the editor whose Patch view is drawing (each editor keeps its own layout)
std::map<int, std::pair<float, float>>& offsets() {  // the user's moves of boxes (Tidy clears them)
  static std::map<int, std::pair<float, float>> none;
  return gPatchUi ? gPatchUi->pOffsets : none;
}
std::set<int>& shownMoves() {  // movement sources added by hand (shown though unused)
  static std::set<int> none;
  return gPatchUi ? gPatchUi->pShownMoves : none;
}
// The box a parameter shows on (a link to it counts there).
int ownerBox(const Graph& g, int k) {
  if (k == G_Bus1Vol || k == G_Bus1Dest || k == tailP(T_BusChain)) return NSendA;
  if (k == G_Bus2Vol || k == G_Bus2Dest) return NSendB;
  const int n = ownerNode(k);
  if (n >= NFx && n < NFx + kFxSlots) {
    auto it = g.fxBox.find(n - NFx);
    return it == g.fxBox.end() ? n : it->second;
  }
  if (n >= NMove) return moveNodeOf(n - NMove);
  return n;
}

Graph buildGraph(const Ctx& c) {
  Graph g;
  const auto fx = resolvedFx(c.m, c.st.params);
  for (const auto& n : fx)
    if (n.parent < 0 && vali(c, fxP(n.slot, X_Type)) != FxNone) g.chain[n.rack].push_back(n.slot);
  for (const auto& n : fx) {  // every slot, a split's bands too, shows on its top box
    int top = n.slot;
    for (int guard = 0; guard < kFxSlots; ++guard) {
      int up = -1;
      for (const auto& q : fx)
        if (q.slot == top) up = q.parent;
      if (up < 0) break;
      top = up;
    }
    g.fxBox[n.slot] = n.rack == 0 ? NFx + top : n.rack == 1 ? NSendA : NSendB;
  }
  float colB[6] = {};
  auto add = [&](int id, int col, float y, float x = -1) {
    PNode n;
    n.id = id, n.col = col;
    n.x = x >= 0 ? x : kCols[col].x, n.w = kCols[col].w;
    n.y = x >= 0 ? y : std::max(y, colB[col]);
    n.h = kBoxH + (c.u.pSel == id && id != NOut ? kKnobRowH : 0.f);
    const int en = enableOf(id);
    n.on = en < 0 || val(c, en) >= 0.5;
    if (auto o = offsets().find(id); o != offsets().end()) n.x += o->second.first, n.y += o->second.second;
    if (x < 0) colB[col] = std::max(colB[col], n.y + n.h + 14);
    g.at[id] = g.nodes.size();
    g.nodes.push_back(n);
  };
  add(NVoices, CPlay, rowY(2));
  for (int i = 0; i < 5; ++i) add(NSrc + i, CSrc, rowY(i));
  add(NFilt, CShape, rowY(0));
  add(NFilt + 1, CShape, rowY(2));
  add(NAmp, CAmp, rowY(2));
  const int nMain = static_cast<int>(g.chain[0].size());
  for (int i = 0; i < nMain; ++i) add(NFx + g.chain[0][static_cast<size_t>(i)], CFx, rowY(i));
  add(NSendA, CFx, rowY(std::max(3, nMain)));
  add(NSendB, CFx, rowY(std::max(4, nMain + 1)));
  add(NOut, COut, rowY(2));
  for (float b : colB) g.mainB = std::max(g.mainB, b - 14);
  g.sendY = g.mainB + 40;
  g.moveY = std::max(kMoveRow0, g.mainB + 86);
  g.busY = g.moveY - 20;
  g.mainHead = nMain > 0 ? NFx + g.chain[0][0] : NOut;
  // movement: the sources in use, the ones shown by hand and three to start from
  std::set<int> mv = {NMove + SrcLfo1, NMove + SrcEnv2, NMove + SrcMacro1};
  for (int s = 0; s < kModSlots; ++s) {
    const int src = vali(c, modP(s, M_Source)), dest = vali(c, modP(s, M_Dest));
    if (src <= SrcNone || dest <= 0) continue;
    mv.insert(moveNodeOf(src));
    const int o = ownerBox(g, dest - 1);
    if (o >= NMove) mv.insert(o);
  }
  for (int s : shownMoves()) mv.insert(moveNodeOf(s));
  if (c.u.pSel >= NMove + SrcEnv2 && c.u.pSel < NMove + SourceCount) mv.insert(c.u.pSel);  // the one opened from the panels
  if (c.u.pLink >= 0 && vali(c, modP(c.u.pLink, M_Source)) > SrcNone) mv.insert(moveNodeOf(vali(c, modP(c.u.pLink, M_Source))));
  std::vector<int> order;
  for (int s = SrcLfo1; s <= SrcLfo10; ++s) order.push_back(NMove + s);
  for (int s = SrcEnv2; s <= SrcEnv4; ++s) order.push_back(NMove + s);
  order.push_back(NMove + SrcMacro1);
  for (int s = SrcNone + 1; s < SourceCount; ++s)
    if (!(s >= SrcLfo1 && s <= SrcLfo10Y) && !(s >= SrcEnv1 && s <= SrcEnv4) && !(s >= SrcMacro1 && s <= SrcMacro8)) order.push_back(NMove + s);
  int i = 0;
  for (int id : order) {
    if (!mv.count(id)) continue;
    const int col = CSrc + i % 4;
    add(id, col, g.moveY + kRowStep * static_cast<float>(i / 4), kCols[col].x);
    g.nodes.back().col = col;
    ++i;
  }
  for (const auto& n : g.nodes) g.maxX = std::max(g.maxX, n.x + n.w), g.maxY = std::max(g.maxY, n.y + n.h);
  // sound: the routes as the engine plays them
  auto wire = [&](int from, int to, int kind, float wt, int r, int dest) { g.wires.push_back({from, to, kind, wt, r, dest}); };
  for (int r = 0; r < kRoutes; ++r) {
    const int from = nodeOfRoute(r);
    const int d = vali(c, routeP(r, R_Dest));
    const float b2 = (static_cast<float>(valf(c, routeP(r, R_Balance))) + 1.f) * 0.5f;
    const bool filterNode = r == RtF1 || r == RtF2;
    if (d == RouteFilter) {
      if (filterNode) wire(from, r == RtF1 ? NFilt + 1 : NFilt, 0, 1.f, r, r == RtF1 ? DF2 : DF1);
      else {
        if (b2 < 0.995f) wire(from, NFilt, 0, 1.f - b2, r, DF1);
        if (b2 > 0.005f) wire(from, NFilt + 1, 0, b2, r, DF2);
      }
    } else if (d == RouteMain) {
      wire(from, NAmp, 0, 1.f, r, DAmp);
    } else if (d == RouteDirect) {
      wire(from, NOut, 0, 1.f, r, DOut);
    }
    if (valf(c, routeP(r, R_Bus1)) > 0.001f) wire(from, NSendA, 1, static_cast<float>(valf(c, routeP(r, R_Bus1))), r, DSendA);
    if (valf(c, routeP(r, R_Bus2)) > 0.001f) wire(from, NSendB, 1, static_cast<float>(valf(c, routeP(r, R_Bus2))), r, DSendB);
  }
  // the main chain: the amp into its first effect, each effect into the next, the last one out
  wire(NAmp, g.mainHead, 0, 1.f, -1, -1);
  for (int k = 0; k + 1 < nMain; ++k) wire(NFx + g.chain[0][static_cast<size_t>(k)], NFx + g.chain[0][static_cast<size_t>(k + 1)], 3, 1.f, -1, -1);
  if (nMain > 0) wire(NFx + g.chain[0].back(), NOut, 0, 1.f, -1, -1);
  // the sends' returns, as the engine mixes them: into the main chain or straight out; a chained
  // send feeds the other one instead (both ways: Send B feeds Send A and neither returns)
  // (a send nothing is sent to has nothing to return)
  const int chainMode = vali(c, tailP(T_BusChain));
  bool fedA = false, fedB = false;
  for (int r = 0; r < kRoutes; ++r) fedA = fedA || valf(c, routeP(r, R_Bus1)) > 0.001f, fedB = fedB || valf(c, routeP(r, R_Bus2)) > 0.001f;
  const bool getsA = fedA || (chainMode >= 2 && fedB), getsB = fedB || (chainMode == 1 && fedA);
  if (chainMode == 1) wire(NSendA, NSendB, 3, 1.f, -1, DChain);
  if (chainMode >= 2) wire(NSendB, NSendA, 3, 1.f, -1, DChain);
  if ((chainMode == 0 || chainMode == 2) && getsA) wire(NSendA, vali(c, G_Bus1Dest) == 0 ? g.mainHead : NOut, 0, 1.f, -1, -1);
  if (chainMode <= 1 && getsB) wire(NSendB, vali(c, G_Bus2Dest) == 0 ? g.mainHead : NOut, 0, 1.f, -1, -1);
  // notes: what plays the notes into each source that is on
  for (int k = 0; k < 5; ++k)
    if (g.find(NSrc + k)->on) wire(NVoices, NSrc + k, 2, 1.f, -1, -1);
  return g;
}

// ---- orthogonal routes. A wire leaves its box sideways, runs up or down a lane in the gap between
// two columns and enters its target sideways; wires into one target share its lane (a bus). A wire
// that has to cross a column crosses it where no box is.
struct RP {
  float x, y;
  int gap = -1, key = 0;  // gap >= 0: x is that lane
};
struct Lanes {
  std::map<int, std::vector<int>> keys;
  void use(int gap, int key) {
    auto& v = keys[gap];
    if (std::find(v.begin(), v.end(), key) == v.end()) v.push_back(key);
  }
  float x(int gap, int key) const {
    if (gap < 0) return kCols[0].x - 12;
    const float l = kCols[gap].x + kCols[gap].w, r = kCols[std::min(gap + 1, 5)].x, mid = (l + r) * 0.5f;
    auto it = keys.find(gap);
    if (it == keys.end()) return mid;
    const auto& v = it->second;
    const int n = static_cast<int>(v.size());
    const int i = static_cast<int>(std::find(v.begin(), v.end(), key) - v.begin());
    const float sp = n > 1 ? std::min(8.f, (r - l - 16) / static_cast<float>(n - 1)) : 0.f;
    return mid + (static_cast<float>(i) - static_cast<float>(n - 1) * 0.5f) * sp;
  }
};
bool freeAt(const Graph& g, float y, float x1, float x2) {
  for (const auto& n : g.nodes)
    if (n.x < x2 && n.x + n.w > x1 && y > n.y - 6 && y < n.y + n.h + 6) return false;
  return true;
}
ImVec2 outAt(const PNode& n) { return ImVec2(n.x + n.w, n.y + kPinMid); }
ImVec2 inAt(const PNode& n) { return ImVec2(n.x, n.y + kPinMid); }
std::vector<RP> planSound(const Graph& g, const PNode& A, const PNode& B, ImVec2 a, ImVec2 b, int exitKey, int inKey) {
  if (B.col == A.col + 1) return {{a.x, a.y}, {0, a.y, A.col, inKey}, {0, b.y, A.col, inKey}, {b.x, b.y}};
  if (B.col > A.col + 1) {
    const float x1 = kCols[A.col].x + kCols[A.col].w, x2 = kCols[B.col].x;
    std::vector<float> cand;
    for (const auto& n : g.nodes)
      if (n.col > A.col && n.col < B.col && n.id < NMove) cand.push_back(n.y - 7), cand.push_back(n.y + n.h + 7);
    std::sort(cand.begin(), cand.end(), [&](float p, float q) { return std::fabs(p - b.y) + 0.5f * std::fabs(p - a.y) < std::fabs(q - b.y) + 0.5f * std::fabs(q - a.y); });
    cand.insert(cand.begin(), {b.y, a.y});
    float cy = b.y;
    for (float y : cand)
      if (y >= 30 && freeAt(g, y, x1 + 1, x2 - 1)) {
        cy = y;
        break;
      }
    const int gA = A.col, gB = B.col - 1;
    if (cy == a.y && cy == b.y) return {{a.x, a.y}, {b.x, b.y}};
    if (cy == a.y) return {{a.x, a.y}, {0, a.y, gB, inKey}, {0, b.y, gB, inKey}, {b.x, b.y}};
    if (cy == b.y) return {{a.x, a.y}, {0, a.y, gA, exitKey}, {0, b.y, gA, exitKey}, {b.x, b.y}};
    return {{a.x, a.y}, {0, a.y, gA, exitKey}, {0, cy, gA, exitKey}, {0, cy, gB, inKey}, {0, b.y, gB, inKey}, {b.x, b.y}};
  }
  // the same column (a send returning into the main chain): over the top of the column
  const float top = 30;
  return {{a.x, a.y}, {0, a.y, A.col, exitKey}, {0, top, A.col, exitKey}, {0, top, B.col - 1, inKey}, {0, b.y, B.col - 1, inKey}, {b.x, b.y}};
}

// ---- wire paths (screen points). A wire to the next column runs along a lane in the gutter
// beside its source: wires from one source share that lane and split only at their targets, so
// the five sources fan out in their own order. Longer wires are smooth curves.
void cornerTo(std::vector<ImVec2>& v, ImVec2 corner, ImVec2 next, float r) {
  const ImVec2 prev = v.back();
  const float d1 = std::hypot(corner.x - prev.x, corner.y - prev.y), d2 = std::hypot(next.x - corner.x, next.y - corner.y);
  const float rr = std::min(r, std::min(d1, d2) * 0.5f);
  if (rr < 0.5f) {
    v.push_back(corner);
    return;
  }
  const ImVec2 a(corner.x + (prev.x - corner.x) * rr / d1, corner.y + (prev.y - corner.y) * rr / d1);
  const ImVec2 b(corner.x + (next.x - corner.x) * rr / d2, corner.y + (next.y - corner.y) * rr / d2);
  v.push_back(a);
  for (int i = 1; i < 4; ++i) {  // a quadratic round-off
    const float t = i / 4.f, u = 1 - t;
    v.push_back(ImVec2(u * u * a.x + 2 * u * t * corner.x + t * t * b.x, u * u * a.y + 2 * u * t * corner.y + t * t * b.y));
  }
  v.push_back(b);
}
std::vector<ImVec2> lanePath(const std::vector<ImVec2>& pts, float r) {  // a polyline through pts, corners rounded
  std::vector<ImVec2> v = {pts.front()};
  for (size_t i = 1; i + 1 < pts.size(); ++i) cornerTo(v, pts[i], pts[i + 1], r);
  v.push_back(pts.back());
  return v;
}
float pathLen(const std::vector<ImVec2>& v) {
  float L = 0;
  for (size_t i = 1; i < v.size(); ++i) L += std::hypot(v[i].x - v[i - 1].x, v[i].y - v[i - 1].y);
  return L;
}
ImVec2 pathAt(const std::vector<ImVec2>& v, float t) {
  float want = t * pathLen(v);
  for (size_t i = 1; i < v.size(); ++i) {
    const float d = std::hypot(v[i].x - v[i - 1].x, v[i].y - v[i - 1].y);
    if (want <= d && d > 0) return ImVec2(v[i - 1].x + (v[i].x - v[i - 1].x) * want / d, v[i - 1].y + (v[i].y - v[i - 1].y) * want / d);
    want -= d;
  }
  return v.back();
}
float pathDist(const std::vector<ImVec2>& v, ImVec2 m) {
  float best = 1e9f;
  for (size_t i = 1; i < v.size(); ++i) {
    const ImVec2 a = v[i - 1], b = v[i];
    const float dx = b.x - a.x, dy = b.y - a.y, l2 = dx * dx + dy * dy;
    const float t = l2 > 0 ? std::clamp(((m.x - a.x) * dx + (m.y - a.y) * dy) / l2, 0.f, 1.f) : 0.f;
    best = std::min(best, std::hypot(a.x + dx * t - m.x, a.y + dy * t - m.y));
  }
  return best;
}
// Draws a path solid, dashed (send) or dotted (motion).
void drawPath(ImDrawList* dl, const std::vector<ImVec2>& v, ImU32 col, float th, int style, float unit) {
  if (style == 0) {
    dl->AddPolyline(v.data(), static_cast<int>(v.size()), col, 0, th);
    return;
  }
  const float on = style == 1 ? 7 * unit : 2.2f * unit, off = style == 1 ? 5 * unit : 3.2f * unit;
  float phase = 0;
  bool draw = true;
  for (size_t i = 1; i < v.size(); ++i) {
    ImVec2 a = v[i - 1];
    const ImVec2 b = v[i];
    float d = std::hypot(b.x - a.x, b.y - a.y);
    while (d > 0) {
      const float left = (draw ? on : off) - phase, step = std::min(left, d);
      const ImVec2 e(a.x + (b.x - a.x) * step / d, a.y + (b.y - a.y) * step / d);
      if (draw) dl->AddLine(a, e, col, th);
      phase += step, d -= step, a = e;
      if (phase >= (draw ? on : off) - 1e-3f) phase = 0, draw = !draw;
    }
  }
}

// Connects a sound wire dragged from node `from` onto node `to` (false + the reason when the engine cannot).
bool connect(Ctx& c, int from, int to, std::string& why) {
  const int r = routeOfNode(from);
  if (r >= 0) {
    const bool isFilter = from == NFilt || from == NFilt + 1;
    if (to == NFilt || to == NFilt + 1) {
      if (isFilter) {
        if (to == from) return why = "A filter cannot feed itself", false;
        setv(c, routeP(r, R_Dest), RouteFilter);
        return true;
      }
      if (!routeFeeds(c, r, to - NFilt)) routeToggleFeed(c, r, to - NFilt);
      return true;
    }
    if (to == NAmp) return setv(c, routeP(r, R_Dest), RouteMain), true;
    if (to == NOut) return setv(c, routeP(r, R_Dest), RouteDirect), true;
    if (to == NSendA || to == NSendB) {
      const int k = routeP(r, to == NSendA ? R_Bus1 : R_Bus2);
      if (val(c, k) < 0.001) setv(c, k, 0.5);
      return true;
    }
    why = isFilter ? "A filter can go to the other filter, the amp, straight out, or the sends"
                   : "A source can go to the filters, the amp, straight out, or the sends";
    return false;
  }
  if (from == NSendA && to == NSendB) return setv(c, tailP(T_BusChain), 1), true;
  why = from == NSendA ? "Send A can feed Send B" : "This connection is fixed: drag from a source or a filter";
  return false;
}
// Takes a sound wire away (Alt+click).
void disconnect(Ctx& c, const PWire& w) {
  if (w.r < 0) {
    if (w.dest == DChain) setv(c, tailP(T_BusChain), 0);
    return;
  }
  const bool isFilter = w.r == RtF1 || w.r == RtF2;
  switch (w.dest) {
    case DF1:
    case DF2:
      if (isFilter) setv(c, routeP(w.r, R_Dest), RouteMain);
      else routeToggleFeed(c, w.r, w.dest == DF1 ? 0 : 1);
      break;
    case DAmp:
    case DOut: setv(c, routeP(w.r, R_Dest), RouteNone); break;
    case DSendA: setv(c, routeP(w.r, R_Bus1), 0); break;
    case DSendB: setv(c, routeP(w.r, R_Bus2), 0); break;
    default: break;
  }
}
void removeLink(Ctx& c, int s) {
  std::vector<std::pair<int, double>> ps;
  for (int q = 0; q < M_Count; ++q)
    if (const ParamDesc* d = pdesc(modP(s, q))) ps.push_back({modP(s, q), d->def});
  commitParams(c, "Remove link", ps);  // one undo step
  if (c.u.pLink == s) c.u.pLink = -1;
}


// Text wrapped to w (form units) at word breaks; returns the y under it.
float wrapped(Ctx& c, float x, float y, float w, const std::string& s, ImU32 col, float px) {
  std::string line, word;
  auto flush = [&] {
    if (!line.empty()) text(c, x, y, line.c_str(), col, px, false), y += px + 6;
    line.clear();
  };
  for (size_t i = 0; i <= s.size(); ++i) {
    if (i == s.size() || s[i] == ' ') {
      const std::string next = line.empty() ? word : line + " " + word;
      if (!line.empty() && textW(c, next.c_str(), px, false) > w) flush(), line = word;
      else line = next;
      word.clear();
    } else {
      word += s[i];
    }
  }
  flush();
  return y;
}

// Every setting of the selected box (the panels' columns), or the selected link's.
float allSettings(Ctx& c, float x, float y, float w) {
  Ui& u = c.u;
  if (u.pLink >= 0) {
    const int s = u.pLink, src = vali(c, modP(s, M_Source)), dest = vali(c, modP(s, M_Dest));
    if (src > SrcNone && dest > 0) {  // what moves what, in words
      char buf[48];
      const std::string t = sourceLabel(src) + " moves " + nodeTitle(ownerNode(dest - 1), buf, sizeof buf) + " " + plainName(dest - 1);
      c.dl->AddCircleFilled(c.p(x + 5, y + 10), c.s(4.5f), c.P.fam[familyOf(src)], 12);
      c.dl->PushClipRect(c.p(x + 14, y), c.p(x + w, y + 24), true);
      text(c, x + 14, y + 3, t.c_str(), c.P.text, 12.0f, true);
      c.dl->PopClipRect();
      y += 26;
      y = wrapped(c, x, y, w, paramSentence(dest - 1), c.P.textDim, 10.5f) + 6;
    } else {
      y = wrapped(c, x, y, w, "An empty link: choose a source and a control.", c.P.textDim, 10.5f) + 6;
    }
    return linkEditor(c, s, x - 14, y, w + 28);
  }
  const int id = u.pSel;
  if (id < 0) {
    y = wrapped(c, x, y, w, "Click a box to edit it here.", c.P.textDim, 11.5f) + 8;
    y = wrapped(c, x, y, w, "Drag from a box's right edge to send its sound somewhere else. Drag a motion box's dot onto any control or pin to link it.", c.P.textDim, 11.5f) + 8;
    return wrapped(c, x, y, w, "Tab switches back to the panels.", c.P.textDim, 11.5f);
  }
  DetailsOn every;  // the inspector shows each control of the box
  if (id >= NSrc && id < NSrc + 5) {
    u.src = id - NSrc;
    if (u.src < 3) u.layer = u.src;
    return u.src < 3 ? layerColumn(c, u.src, x, y, w) : u.src == 3 ? rootColumn(c, x, y, w) : textureColumn(c, x, y, w);
  }
  if (id == NFilt || id == NFilt + 1) return filterColumn(c, id - NFilt, x, y, w);
  if (id == NAmp) return ampColumn(c, x, y, w);
  if (id == NVoices)
    return fieldGrid(c, x, y, w, 1, 32,
                     {{G_Poly, "Notes at once"}, {G_Priority, "Steals"}, {G_SameNote, "One per key"}, {G_Mono, "One voice"}, {G_Legato, "Glide notes"},
                      {G_PortaTime, "Glide time"}, {G_PortaCurve, "Glide curve"}, {G_PortaAlways, "Glide every note"}, {G_PortaScaled, "Constant glide rate"},
                      {G_BendUp, "Bend up"}, {G_BendDown, "Bend down"}, {G_Transpose, "Transpose"}, {G_MasterTune, "Fine tune"}, {G_Amp, "Depth"},
                      {G_EnvScale, "All envelope times"}},
                     26);
  if (id >= NFx && id < NFx + kFxSlots) {
    for (const auto& n : resolvedFx(c.m, c.st.params))
      if (n.slot == id - NFx) u.fxRack = n.rack;
    u.fxSel = id - NFx;
    return effectsColumn(c, x, y, w);
  }
  if (id == NSendA || id == NSendB) {
    u.fxRack = id == NSendA ? 1 : 2;
    return effectsColumn(c, x, y, w);
  }
  if (id == NOut)
    return fieldGrid(c, x, y, w, 1, 32,
                     {{G_MasterVol, "Output level"}, {G_DirectVol, "Direct level"}, {G_Bus1Vol, "Send A level"}, {G_Bus2Vol, "Send B level"},
                      {G_Bus1Dest, "Send A goes to"}, {G_Bus2Dest, "Send B goes to"}},
                     26);
  if (id == NArp || id == NPhrase) {
    y = fieldGrid(c, x, y, w, 1, 32, {{id == NArp ? G_ArpOn : G_ClipOn, id == NArp ? "Arpeggiator on" : "Phrases on"}}, 26);
    if (button(c, "openfull", x, y, w, 26, id == NArp ? "Open the arpeggiator" : "Open the phrases", false, 11.5f)) {
      u.patch = false;
      openView(u, id == NArp ? VArp : VPhrases);
    }
    return y + 32;
  }
  if (id >= NMove) {
    const int src = id - NMove;
    if (src >= SrcEnv1 && src <= SrcEnv4) {
      envEditor(c, src - SrcEnv1, x, y, w, 0, true);
      return y + 420;
    }
    if (src >= SrcLfo1 && src <= SrcLfo10) {
      const int l = src - SrcLfo1;
      c.dl->AddRectFilled(c.p(x, y), c.p(x + w, y + 80), c.P.well, c.s(4));
      lfoMini(c, l, x + 6, y + 6, w - 12, 68);
      y += 88;
      y = fieldGrid(c, x, y, w, 1, 32,
                    {{lfoP(l, L_Rate), "Rate"}, {lfoP(l, L_Bpm), "Synced"}, {lfoP(l, L_Mode), "Start"}, {lfoP(l, L_Type), "Kind"},
                     {lfoP(l, L_Rise), "Fade in"}, {lfoP(l, L_Delay), "Wait"}, {lfoP(l, L_Smooth), "Soften"}, {lfoP(l, L_Shape), "Shape"},
                     {lfoP(l, L_Phase), "Start phase"}, {lfoP(l, L_PhaseSnap), "Phase snap"}, {lfoP(l, L_Anchor), "Song lock"}, {lfoP(l, L_Mono), "One for all notes"},
                     {lfoP(l, L_Dir), "Direction"}, {lfoP(l, L_Swing), "Swing"}, {lfoP(l, L_Dot), "Dotted"}, {lfoP(l, L_Trip), "Triplet"},
                     {lfoP(l, L_Rate10x), "Rate x 10"}, {lfoP(l, L_GridX), "Grid across"}, {lfoP(l, L_GridY), "Grid down"}},
                    26);
      {  // the LFO buses (shared by every LFO)
        std::vector<FieldItem> items;
        static const char* const bn[16] = {"Bus 1", "Bus 2", "Bus 3", "Bus 4", "Bus 5", "Bus 6", "Bus 7", "Bus 8",
                                           "Bus 9", "Bus 10", "Bus 11", "Bus 12", "Bus 13", "Bus 14", "Bus 15", "Bus 16"};
        for (int b = 0; b < kLfoBusses; ++b) items.push_back({busP(b), bn[b]});
        y = fieldGrid(c, x, y, w, 2, 32, items, 26);
      }
      if (button(c, "openlfo", x, y, w, 26, "Open the shape editor", false, 11.5f)) u.patch = false, u.lfoTab = l, openView(u, VLfo);
      return y + 32;
    }
    if (src >= SrcMacro1 && src <= SrcMacro8) {
      std::vector<FieldItem> items;
      static const char* const mn[kMacros] = {"Macro 1", "Macro 2", "Macro 3", "Macro 4", "Macro 5", "Macro 6", "Macro 7", "Macro 8"};
      for (int m = 0; m < kMacros; ++m) items.push_back({macroP(m), mn[m]});
      y = fieldGrid(c, x, y, w, 2, 32, items, 26);
      text(c, x, y, "Right-click the macro in the panels to name it.", c.P.textDim, 10.5f, false);
      return y + 24;
    }
    text(c, x, y, "This source follows the playing:", c.P.textDim, 11.0f, false);
    text(c, x, y + 18, sourceLabel(src).c_str(), c.P.text, 12.0f, true);
    return y + 44;
  }
  return y;
}

// Where the add menu opens: beside the box the wire comes from (right of it, else left), at the
// height of the drop, inside the canvas, so it never covers that box.
constexpr float kAddMenuW = 360;
ImVec2 gAddMenuPos;
void addMenuAt(const Ctx& g, const PNode& n, ImVec2 drop, ImVec2 c0, ImVec2 c1) {
  const ImVec2 a = g.p(n.x, n.y), b = g.p(n.x + n.w, n.y + n.h);
  const float mw = g.s(kAddMenuW), mh = g.s(520), gap = g.s(14);
  float x = std::max(drop.x + gap, b.x + gap);
  if (x + mw > c1.x) x = a.x - gap - mw;  // no room at the right: open at the left of the box
  if (x < c0.x) x = std::max(c0.x, b.x + gap);
  const float y = std::clamp(drop.y - g.s(24), c0.y, std::max(c0.y, c1.y - mh));
  gAddMenuPos = ImVec2(x, y);
}

// The search menu after a sound wire is dropped on empty canvas: where it can go, or an effect.
// One plain sentence per effect, for the add menu.
const char* fxSentence(int t) {
  switch (t) {
    case FxDistortion: return i18n::N_("Bends the wave for grit, warmth or fuzz.");
    case FxFlanger: return i18n::N_("A short moving echo that sweeps a metallic whoosh.");
    case FxPhaser: return i18n::N_("Sweeping notches that swirl through the sound.");
    case FxChorus: return i18n::N_("Detuned copies that thicken and widen.");
    case FxDelay: return i18n::N_("Repeats the sound in time, left and right.");
    case FxComp: return i18n::N_("Evens out the level and adds punch.");
    case FxReverb: return i18n::N_("Puts the sound in a room or a hall.");
    case FxEq: return i18n::N_("Lifts or cuts low, middle and high frequencies.");
    case FxFilter: return i18n::N_("One more filter after the voices.");
    case FxHyper: return i18n::N_("Many detuned copies for a huge, wide sound.");
    case FxBode: return i18n::N_("Shifts every frequency by the same amount, for metallic tones.");
    case FxConvolve: return i18n::N_("A real space from a recorded impulse.");
    case FxUtility: return i18n::N_("Level, pan, width and phase.");
    case FxSplitLH: return i18n::N_("Splits into lows and highs, each with its own effects.");
    case FxSplitLMH: return i18n::N_("Splits into lows, mids and highs, each with its own effects.");
    case FxSplitMS: return i18n::N_("Splits the middle from the sides, each with its own effects.");
    default: return "";
  }
}
constexpr float kPi = 3.14159265f;
// A small picture of what an effect type does (screen pixels, in a menu).
void fxTypeGlyph(ImDrawList* dl, int t, ImVec2 a, ImVec2 b, ImU32 col, ImU32 faint) {
  dl->AddRectFilled(a, b, faint, 3.f);
  const float w = b.x - a.x, h = b.y - a.y, th = 1.5f;
  auto P = [&](float fx, float fy) { return ImVec2(a.x + 3 + fx * (w - 6), b.y - 3 - fy * (h - 6)); };
  std::vector<ImVec2> v;
  auto fn = [&](auto f) {
    v.clear();
    for (int i = 0; i <= 24; ++i) v.push_back(P(i / 24.f, f(i / 24.f)));
    dl->AddPolyline(v.data(), static_cast<int>(v.size()), col, 0, th);
  };
  switch (t) {
    case FxDistortion: fn([](float x) { return 0.5f + 0.45f * std::tanh(6.f * (x - 0.5f)) / std::tanh(3.f); }); break;
    case FxFilter: fn([](float x) { return x < 0.55f ? 0.75f + (x > 0.4f ? (x - 0.4f) * 1.2f : 0.f) : std::max(0.05f, 0.93f - (x - 0.55f) * 2.4f); }); break;
    case FxEq: fn([](float x) { return 0.5f + 0.3f * std::exp(-(x - 0.3f) * (x - 0.3f) * 60.f) - 0.25f * std::exp(-(x - 0.75f) * (x - 0.75f) * 80.f); }); break;
    case FxDelay:
      for (int k = 0; k < 5; ++k) dl->AddRectFilled(P(0.05f + k * 0.2f, 0), P(0.1f + k * 0.2f, 0.9f * std::pow(0.6f, static_cast<float>(k))), col, 1.f);
      break;
    case FxReverb: case FxConvolve:
      for (int k = 0; k < 18; ++k) dl->AddLine(P(0.05f + k * 0.052f, 0), P(0.05f + k * 0.052f, 0.9f * std::exp(-k / 6.f) * (0.6f + 0.4f * ((k * 7) % 5) / 4.f)), col, 1.f);
      break;
    case FxComp: fn([](float x) { return x < 0.5f ? x : 0.5f + (x - 0.5f) * 0.3f; }); break;
    case FxChorus: case FxFlanger: case FxPhaser: case FxHyper:
      fn([](float x) { return 0.5f + 0.35f * std::sin(x * 2 * kPi * 1.5f); });
      fn([t](float x) { return 0.5f + 0.35f * std::sin(x * 2 * kPi * 1.5f + (t == FxHyper ? 1.2f : 0.6f)); });
      break;
    case FxBode: fn([](float x) { return 0.5f + 0.35f * std::sin(x * 2 * kPi * (1.f + x)); }); break;
    case FxUtility: fn([](float) { return 0.5f; }); break;
    default:  // a split: one line in, two or three out
      dl->AddLine(P(0, 0.5f), P(0.4f, 0.5f), col, th);
      for (int k = 0; k < (t == FxSplitLMH ? 3 : 2); ++k) {
        const float y = t == FxSplitLMH ? 0.15f + k * 0.35f : 0.25f + k * 0.5f;
        dl->AddLine(P(0.4f, 0.5f), P(0.6f, y), col, th), dl->AddLine(P(0.6f, y), P(1, y), col, th);
      }
      break;
  }
}
// A menu row: a picture (optional), the name and one plain sentence under it. True when chosen.
bool menuRow(Ctx& c, const char* id, const char* name, const char* sentence, int fxType) {
  const float lh = ImGui::GetTextLineHeight(), pic = fxType > 0 ? lh * 3.0f : 0.f;
  const float w = ImGui::GetContentRegionAvail().x;  // the menu's own width (the sentence wraps in it)
  const ImVec2 at = ImGui::GetCursorScreenPos();
  const float tx = at.x + pic + (pic > 0 ? 8 : 2), wrap = std::max(40.f, at.x + w - tx - 4);
  const float sentH = ImGui::CalcTextSize(sentence, nullptr, false, wrap).y;
  const float rowH = std::max(lh * 2.2f, lh + 3 + sentH + 4);
  const bool hit = ImGui::Selectable(id, false, 0, ImVec2(w, rowH));
  ImDrawList* dl = ImGui::GetWindowDrawList();
  if (fxType > 0) fxTypeGlyph(dl, fxType, ImVec2(at.x, at.y + 3), ImVec2(at.x + pic, at.y + std::min(rowH, lh * 2.2f) - 3), c.P.accent, c.P.well);
  dl->AddText(ImVec2(tx, at.y + 1), ImGui::GetColorU32(ImGuiCol_Text), name);
  dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(tx, at.y + lh + 3), ImGui::GetColorU32(ImGuiCol_TextDisabled), sentence, nullptr, wrap);
  layoutWindowText(ImVec2(tx, at.y + 1), name, 0);
  layoutWindowText(ImVec2(tx, at.y + lh + 3), sentence, wrap);
  return hit;
}

// The search menu after a sound wire is dropped on empty canvas: where its sound can go, or an
// effect to add (to the chosen chain), each with a picture and a plain sentence.
void addMenuAfterDrop(Ctx& c, int from) {
  static char q[64] = "";
  static int chain = 0;
  if (ImGui::IsWindowAppearing()) q[0] = 0, ImGui::SetKeyboardFocusHere();
  ImGui::SetNextItemWidth(c.s(300));
  ImGui::InputTextWithHint("##q", i18n::tr("Search"), q, sizeof q);
  std::string ql = q;
  for (auto& ch : ql) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  auto match = [&](const char* a, const char* b) {
    std::string l = std::string(a) + " " + b;
    for (auto& ch : l) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return ql.empty() || l.find(ql) != std::string::npos;
  };
  char buf[48];
  if (routeOfNode(from) >= 0) {
    const std::string head = std::string(i18n::tr("Send the sound of")) + " " + i18n::tr(nodeTitle(from, buf, sizeof buf)) + " " + i18n::tr("to");
    ImGui::SeparatorText(head.c_str());
    static const int tos[6] = {NFilt, NFilt + 1, NAmp, NOut, NSendA, NSendB};
    static const char* const names[6] = {i18n::N_("Filter 1"), i18n::N_("Filter 2"), i18n::N_("The amp"), i18n::N_("Straight out"),
                                         i18n::N_("Send A"), i18n::N_("Send B")};
    static const char* const says[6] = {i18n::N_("Shapes its tone with the first filter."), i18n::N_("Shapes its tone with the second filter."),
                                        i18n::N_("Skips the filters, then the amp envelope and the main effects."),
                                        i18n::N_("Goes out as it is: no filters, envelope or main effects."),
                                        i18n::N_("Also feeds the first send chain (its level is set on the wire)."),
                                        i18n::N_("Also feeds the second send chain (its level is set on the wire).")};
    for (int i = 0; i < 6; ++i) {
      if (from == tos[i] || !match(i18n::tr(names[i]), i18n::tr(says[i]))) continue;
      ImGui::PushID(i);
      if (menuRow(c, "##to", i18n::tr(names[i]), i18n::tr(says[i]), 0)) {
        std::string why;
        if (!connect(c, from, tos[i], why)) c.u.pWhy = why, c.u.pWhyT = static_cast<float>(ImGui::GetTime());
      }
      ImGui::PopID();
    }
  }
  ImGui::SeparatorText(i18n::tr("Add an effect"));
  {
    static const char* const rk[3] = {i18n::N_("Main chain"), i18n::N_("Send A"), i18n::N_("Send B")};
    ImGui::TextDisabled("%s", i18n::tr("to"));
    for (int r = 0; r < 3; ++r) {
      ImGui::SameLine();
      if (ImGui::RadioButton(i18n::tr(rk[r]), chain == r)) chain = r;
    }
  }
  for (int t = FxDistortion; t < FxTypeCount; ++t) {
    const char* n = i18n::tr(fxDisplayName(t));
    const char* sent = i18n::tr(fxSentence(t));
    if (!match(n, sent)) continue;
    ImGui::PushID(t + 100);
    if (menuRow(c, "##fx", n, sent, t) && !fxAdd(c, t, chain))
      c.u.pWhy = "All 24 effect places are in use", c.u.pWhyT = static_cast<float>(ImGui::GetTime());
    ImGui::PopID();
  }
}

// ---- the boxes
std::string boxSub(const Ctx& c, const Graph& g, int id) {
  if (id >= NSrc && id < NSrc + 3) return layerSourceName(c, id - NSrc);
  if (id == NSrc + 3) return ptext(c, subP(S_Shape));
  if (id == NSrc + 4) return ptext(c, noiseP(N_Type));
  if (id == NFilt || id == NFilt + 1) {
    std::string t = ptext(c, filtP(id - NFilt, F_Type));
    if (const size_t br = t.find(" ["); br != std::string::npos) t.resize(br);
    return t;
  }
  if (id == NAmp) return "Env 1";
  if (id >= NFx && id < NFx + kFxSlots) return "Main";
  if (id == NSendA || id == NSendB) {
    std::string s;
    for (int slot : g.chain[id == NSendA ? 1 : 2]) s += (s.empty() ? "" : ", ") + std::string(i18n::tr(fxDisplayName(vali(c, fxP(slot, X_Type)))));
    return s.empty() ? std::string("empty") : s;
  }
  if (id >= NMove) {
    const int src = id - NMove;
    if (src >= SrcLfo1 && src <= SrcLfo10) return ptext(c, lfoP(src - SrcLfo1, L_Rate));
    if (src == SrcMacro1) {
      int top = 4;
      for (int s = 0; s < kModSlots; ++s)
        if (vali(c, modP(s, M_Source)) > SrcMacro1 + 3 && vali(c, modP(s, M_Source)) <= SrcMacro8) top = 8;
      return top == 4 ? "1 to 4" : "1 to 8";
    }
  }
  return {};
}
std::string boxName(const Ctx& c, int id) {
  char buf[48];
  if (id >= NFx && id < NFx + kFxSlots) return i18n::tr(fxDisplayName(vali(c, fxP(id - NFx, X_Type))));
  if (id == NVoices) return "Play";
  if (id == NMove + SrcMacro1) return "Macros";
  if (id >= NMove + SrcEnv1 && id <= NMove + SrcEnv4) return "Envelope " + std::to_string(id - NMove - SrcEnv1 + 1);
  return nodeTitle(id, buf, sizeof buf);
}
// The dials of a box (the first two show on the selected box, all four in the inspector).
struct Dial {
  int k;
  const char* label;
  int flags;
};
std::vector<Dial> boxDials(const Ctx& c, int id) {
  if (id >= NSrc && id < NSrc + 3) {
    const int o = id - NSrc;
    return {{oscP(o, O_WtPos), "Position", 0}, {oscP(o, O_Level), "Level", 0}, {oscP(o, O_Pan), "Pan", KnobBipolar}, {oscP(o, O_UniDetune), "Detune", 0}};
  }
  if (id == NSrc + 3) return {{subP(S_Level), "Level", 0}, {subP(S_Pan), "Pan", KnobBipolar}, {subP(S_Octave), "Octave", KnobBipolar}, {subP(S_Coarse), "Pitch", KnobBipolar}};
  if (id == NSrc + 4) return {{noiseP(N_Level), "Level", 0}, {noiseP(N_Pan), "Pan", KnobBipolar}, {tailP(T_NoiseTone), "Tone", KnobBipolar}, {noiseP(N_Pitch), "Pitch", 0}};
  if (id == NFilt || id == NFilt + 1) {
    const int f = id - NFilt;
    return {{filtP(f, F_Freq), "Cutoff", 0}, {filtP(f, F_Res), "Resonance", 0}, {filtP(f, F_Drive), "Drive", 0}, {filtP(f, F_KeyTrack), "Key track", 0}};
  }
  if (id == NAmp) return {{envP(0, E_Attack), "Attack", 0}, {envP(0, E_Decay), "Decay", 0}, {envP(0, E_Sustain), "Sustain", 0}, {envP(0, E_Release), "Release", 0}};
  if (id >= NFx && id < NFx + kFxSlots) {
    const int s = id - NFx;
    const auto& defs = fxParams(vali(c, fxP(s, X_Type)));
    std::vector<Dial> v;
    int mixQ = -1;
    for (int q = 0; q < static_cast<int>(defs.size()); ++q)
      if (defs[static_cast<size_t>(q)].kind == 0 && std::strcmp(defs[static_cast<size_t>(q)].key, "kParamWet") == 0) mixQ = q;
    for (int q = 0; q < static_cast<int>(defs.size()) && static_cast<int>(v.size()) < (mixQ >= 0 ? 3 : 4); ++q) {
      const auto& d = defs[static_cast<size_t>(q)];
      if (q != mixQ && d.kind == 0) v.push_back({fxP(s, X_P0 + q), d.label, (d.min < 0 && d.max > 0) ? KnobBipolar : 0});
    }
    if (mixQ >= 0) v.push_back({fxP(s, X_P0 + mixQ), "Mix", 0});
    return v;
  }
  if (id == NSendA || id == NSendB) return {{id == NSendA ? G_Bus1Vol : G_Bus2Vol, "Return", 0}};
  if (id == NOut) return {{G_MasterVol, "Level", 0}, {G_DirectVol, "Direct", 0}, {G_Bus1Vol, "Send A", 0}, {G_Bus2Vol, "Send B", 0}};
  if (id == NVoices) return {{G_PortaTime, "Glide", 0}, {G_Transpose, "Transpose", KnobBipolar}, {G_BendUp, "Bend up", 0}, {G_MasterTune, "Fine tune", KnobBipolar}};
  if (id >= NMove) {
    const int src = id - NMove;
    if (src >= SrcLfo1 && src <= SrcLfo10) {
      const int l = src - SrcLfo1;
      return {{lfoP(l, L_Rate), "Rate", 0}, {lfoP(l, L_Smooth), "Smooth", 0}, {lfoP(l, L_Rise), "Fade in", 0}, {lfoP(l, L_Delay), "Wait", 0}};
    }
    if (src >= SrcEnv1 && src <= SrcEnv4) {
      const int e = src - SrcEnv1;
      return {{envP(e, E_Attack), "Attack", 0}, {envP(e, E_Decay), "Decay", 0}, {envP(e, E_Sustain), "Sustain", 0}, {envP(e, E_Release), "Release", 0}};
    }
    if (src == SrcMacro1) return {{macroP(0), "Macro 1", 0}, {macroP(1), "Macro 2", 0}, {macroP(2), "Macro 3", 0}, {macroP(3), "Macro 4", 0}};
  }
  return {};
}
// A small keyboard (Play): the keys held now lit.
void keysPicture(Ctx& c, float x, float y, float w, float h) {
  const int n = 10;
  const float kw = w / n;
  const ImU32 white = c.P.dark ? IM_COL32(201, 208, 212, 255) : IM_COL32(221, 227, 231, 255);
  const ImU32 black = c.P.dark ? IM_COL32(30, 34, 37, 255) : IM_COL32(42, 49, 53, 255);
  const bool playing = c.hasLive && c.live.voices > 0;
  for (int i = 0; i < n; ++i) c.dl->AddRectFilled(c.p(x + i * kw + 0.5f, y + 4), c.p(x + (i + 1) * kw - 0.5f, y + h - 4), playing && i == 3 ? c.P.accent : white, c.s(1));
  for (int i : {0, 1, 3, 4, 5, 7, 8}) c.dl->AddRectFilled(c.p(x + (i + 1) * kw - kw * 0.3f, y + 4), c.p(x + (i + 1) * kw + kw * 0.3f, y + 4 + (h - 8) * 0.58f), black, c.s(1));
}
// A box's picture (also the inspector's, bigger).
void boxPicture(Ctx& c, const Graph& g, int id, float x, float y, float w, float h, bool big) {
  if (id == NVoices) {
    keysPicture(c, x, y, w, h);
    return;
  }
  if (id >= NSrc && id < NSrc + 3 && big) {
    layerPicture(c, id - NSrc, x, y, w, h);
    return;
  }
  wellLook(c, x, y, w, h);
  if (big) gridLook(c, x, y, w, h, 8, 4, false);
  c.dl->PushClipRect(c.p(x, y), c.p(x + w, y + h), true);
  const float px = x + 3, py = y + 3, pw = w - 6, ph = h - 6;
  if (id >= NSrc && id < NSrc + 5) sourceMini(c, id - NSrc, px, py, pw, ph);
  else if (id == NFilt || id == NFilt + 1) filterCurve(c, id - NFilt, x, y, w, h, false);
  else if (id == NAmp) envMini(c, 0, px, py, pw, ph);
  else if ((id >= NFx && id < NFx + kFxSlots) || id == NSendA || id == NSendB) {
    int s = id - NFx;
    if (id == NSendA || id == NSendB) s = g.chain[id == NSendA ? 1 : 2].empty() ? -1 : g.chain[id == NSendA ? 1 : 2][0];
    if (s >= 0) {
      Pal pal2 = c.P;
      pal2.accent = fxCol(c, vali(c, fxP(s, X_Type)));
      Ctx cc{c.a, c.t, c.F, c.dl, c.st, c.m, c.u, pal2, c.live, c.hasLive};
      fxPicture(cc, s, px, py, pw, ph, big);
    } else {
      textMid(c, x, x + w, y + h * 0.5f, textW(c, "No effects yet", TG(10.5f), false) < w - 8 ? "No effects yet" : "Empty", c.P.textFaint, TG(10.5f), false);
    }
  } else if (id >= NMove) {
    const int src = id - NMove;
    if (src >= SrcLfo1 && src <= SrcLfo10) lfoMini(c, src - SrcLfo1, px, py, pw, ph);
    else if (src >= SrcEnv1 && src <= SrcEnv4) envMini(c, src - SrcEnv1, px, py, pw, ph);
    else if (src == SrcMacro1) {
      const ImU32 fc = c.P.fam[FamMacro];
      for (int i = 0; i < 4; ++i) {
        const float bw = pw / 4, bx = px + i * bw + bw * 0.25f, v = static_cast<float>(valf(c, macroP(i)));
        c.dl->AddRectFilled(c.p(bx, py + 2), c.p(bx + bw * 0.5f, py + ph - 2), theme::withAlpha(c.P.line, 0.6f));
        c.dl->AddRectFilled(c.p(bx, py + ph - 2 - v * (ph - 4)), c.p(bx + bw * 0.5f, py + ph - 2), theme::withAlpha(fc, 0.8f));
      }
    } else {
      float raw = c.hasLive ? c.live.source[src] : 0.f;
      bool bip = false;
      const float v = sourceMeterValue(src, raw, bip);
      const float mid = bip ? px + pw * 0.5f : px;
      c.dl->AddRectFilled(c.p(px, py + ph * 0.5f - 2), c.p(px + pw, py + ph * 0.5f + 2), theme::withAlpha(c.P.line, 0.6f), c.s(2));
      c.dl->AddRectFilled(c.p(std::min(mid, mid + v * pw * (bip ? 0.5f : 1.f)), py + ph * 0.5f - 2), c.p(std::max(mid, mid + v * pw * (bip ? 0.5f : 1.f)), py + ph * 0.5f + 2),
                          c.P.fam[familyOf(src)], c.s(2));
    }
  }
  c.dl->PopClipRect();
}
// A count badge: how many links move this box.
void badge(Ctx& c, float rx, float cy, int n) {
  char b[16];
  std::snprintf(b, sizeof b, "%d", n);
  MonoText mono;
  const float tw = textW(c, b, TG(9.5f), true), w = tw + 17, h = 16;
  const ImU32 lc = c.P.fam[FamLfo];
  c.dl->AddRectFilled(c.p(rx - w, cy - h * 0.5f), c.p(rx, cy + h * 0.5f), theme::withAlpha(lc, 0.16f), c.s(8));
  c.dl->AddCircleFilled(c.p(rx - w + 8, cy), c.s(3), lc, 10);
  text(c, rx - w + 13, cy - TG(9.5f) * 0.68f, b, c.P.textDim, TG(9.5f), true);
}
// The open link search (a movement source dropped on a box or on the canvas).
struct LinkAsk {
  int src = -1, box = -1;
  ImVec2 at;
};
LinkAsk& linkAsk() {
  static LinkAsk a;
  return a;
}

void drawBox(Ctx& g, const Graph& gr, const PNode& n, int links, bool sel, bool dim, bool hov) {
  const Pal& P = g.P;
  const int id = n.id;
  const std::string name = boxName(g, id);
  ImGui::PushID(id + 40000);
  const ImVec2 a = g.p(n.x, n.y), b = g.p(n.x + n.w, n.y + n.h);
  const ImU32 fc = nodeColour(g, id);
  blockFrame(g, n.x, n.y, n.w, n.h, fc, sel, false);
  if (hov) {
    const ImU32 lc = P.fam[FamLfo];
    g.dl->AddRect(ImVec2(a.x - g.s(1), a.y - g.s(1)), ImVec2(b.x + g.s(1), b.y + g.s(1)), theme::withAlpha(lc, 0.3f), g.s(6), 0, g.s(1.5f));
    g.dl->AddRect(a, b, theme::mix(lc, P.line, 0.3f), g.s(6));
  }
  // the whole box: click to select, drag to move (its own controls sit over it)
  ImGui::SetCursorScreenPos(a);
  ImGui::SetNextItemAllowOverlap();
  ImGui::InvisibleButton("##box", ImVec2(b.x - a.x, b.y - a.y), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
  if (ImGui::IsItemClicked(0)) g.u.pSel = id, g.u.pLink = -1;
  if (ImGui::IsItemActive() && ImGui::IsMouseDragging(0, 3) && !ImGui::GetDragDropPayload()) {
    auto& o = offsets()[id];
    o.first += ImGui::GetIO().MouseDelta.x / g.F.s, o.second += ImGui::GetIO().MouseDelta.y / g.F.s;
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenOverlappedByItem)) {
    g.u.pHover = id;
    g.a.setHint(name + (id >= NMove ? ": hover to see what it moves, drag its pin onto a box to link it" : ": click to edit it on the right, drag to move it"));
  }
  if (id >= NFx && id < NFx + kFxSlots) {  // effects: drag onto another effect to reorder
    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
      const int s = id - NFx;
      ImGui::SetDragDropPayload("PLASMA_FX", &s, sizeof(int));
      ImGui::TextUnformatted(name.c_str());
      ImGui::EndDragDropSource();
    }
    fxDropOn(g, id - NFx);
  }
  if (ImGui::BeginDragDropTarget()) {  // a movement source dropped here: pick which control it moves
    if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("PLASMA_SRC")) {
      int src = 0;
      std::memcpy(&src, pl->Data, sizeof(int));
      linkAsk() = {src, id, ImGui::GetIO().MousePos};
      ImGui::OpenPopup("##patchlink");
    }
    ImGui::EndDragDropTarget();
  }
  if (ImGui::BeginPopupContextItem("##nodemenu")) {
    const int en = enableOf(id);
    ImGui::TextDisabled("%s", name.c_str());
    ImGui::Separator();
    if (en >= 0 && ImGui::MenuItem(val(g, en) >= 0.5 ? i18n::tr("Turn off") : i18n::tr("Turn on"))) setv(g, en, val(g, en) >= 0.5 ? 0 : 1);
    if (id >= NFx && id < NFx + kFxSlots && ImGui::MenuItem(i18n::tr("Remove"))) fxRemove(g, id - NFx);
    if (id >= NMove && ImGui::MenuItem(i18n::tr("Hide"))) shownMoves().erase(id - NMove);
    ImGui::EndPopup();
  }
  // the head: light (or the link dot of a movement box), name, what it is, the link count
  const float hy = n.y + 18;
  float tx = n.x + 9;
  const int en = enableOf(id);
  if (id >= NMove) {
    g.dl->AddCircle(g.p(tx + 6, hy), g.s(5), fc, 16, g.s(1.4f));
    g.dl->AddCircleFilled(g.p(tx + 6, hy), g.s(2.2f), fc, 10);
    tx += 18;
  } else if (en >= 0) {
    led(g, en, tx, hy - 3.5f, 7);
    tx += 14;
  }
  float rx = n.x + n.w - 8;
  if (links > 0) {
    badge(g, rx, hy, links);
    char b[16];
    std::snprintf(b, sizeof b, "%d", links);
    MonoText mono;
    rx -= textW(g, b, TG(9.5f), true) + 17 + 6;
  }
  {
    const float nw = textW(g, name.c_str(), TG(12.5f), true);
    g.dl->PushClipRect(g.p(tx, n.y + 4), g.p(rx, n.y + 32), true);
    textHeavy(g, tx, hy - TG(12.5f) * 0.68f, name.c_str(), n.on ? P.text : P.textDim, TG(12.5f));
    const std::string sub = id == NOut ? std::string() : boxSub(g, gr, id);
    if (!sub.empty() && n.w > 120 && tx + nw + 7 < rx) {  // what it is, shortened to the room left (never cut by the box)
      std::string s2 = sub;
      const float room = rx - 3 - (tx + nw + 7);
      if (textW(g, s2.c_str(), TG(11), false) > room) {
        while (!s2.empty() && textW(g, (s2 + "\xE2\x80\xA6").c_str(), TG(11), false) > room) s2.pop_back();
        while (!s2.empty() && s2.back() == ' ') s2.pop_back();
        if (s2.size() >= 2) s2 += "\xE2\x80\xA6";
        else s2.clear();
      }
      if (!s2.empty()) text(g, tx + nw + 7, hy - TG(11) * 0.68f, s2.c_str(), P.textFaint, TG(11), false);
    }
    g.dl->PopClipRect();
  }
  if (id == NOut) {  // two level meters
    const float mh = n.h - 42, my = n.y + 34, mx = n.x + n.w * 0.5f;
    const float lv[2] = {g.hasLive ? std::sqrt(std::clamp(g.live.outL, 0.f, 1.f)) : 0.f, g.hasLive ? std::sqrt(std::clamp(g.live.outR, 0.f, 1.f)) : 0.f};
    for (int i = 0; i < 2; ++i) {
      const float x0 = mx + (i ? 2.f : -9.f);
      g.dl->AddRectFilled(g.p(x0, my), g.p(x0 + 7, my + mh), P.well, g.s(2));
      if (lv[i] > 0.001f) g.dl->AddRectFilled(g.p(x0, my + mh * (1 - lv[i])), g.p(x0 + 7, my + mh), lv[i] > 0.95f ? P.danger : P.good, g.s(2));
    }
  } else {
    boxPicture(g, gr, id, n.x + 9, n.y + 34, n.w - 18, 44, false);
  }
  // the selected box: its two main dials
  if (sel && n.h > kBoxH) {
    const auto d = boxDials(g, id);
    KnobColour kc(fc);
    const int nd = std::min(2, static_cast<int>(d.size()));
    for (int i = 0; i < nd; ++i) {
      ImGui::PushID(i + 300);
      const float cx = nd == 1 ? n.x + n.w * 0.5f : n.x + n.w * (i ? 0.73f : 0.27f);
      knob(g, d[static_cast<size_t>(i)].k, cx, n.y + kBoxH + 14, 13, d[static_cast<size_t>(i)].label, KnobValue | d[static_cast<size_t>(i)].flags);
      ImGui::PopID();
    }
  }
  if (!n.on) g.dl->AddRectFilled(a, b, theme::withAlpha(P.frame, 0.4f), g.s(6));
  if (dim) g.dl->AddRectFilled(a, b, theme::withAlpha(P.frame, 0.62f), g.s(6));
  ImGui::PopID();
}

// ---- the inspector: the selected box (picture, dials), what feeds it and where it goes, what
// moves it, and all its settings folded
const Graph* gGraph = nullptr;  // this frame's graph (the inspector reads it)

// A pill: a colour dot and a few words. Returns its width.
float tagLook(Ctx& c, float x, float y, const std::string& s, ImU32 dot) {
  const float w = textW(c, s.c_str(), T(11), false) + M(9 + 7 + 6 + 9), h = M(24);
  c.dl->AddRectFilled(c.p(x, y), c.p(x + w, y + h), c.P.field, c.s(h * 0.5f));
  c.dl->AddRect(c.p(x, y), c.p(x + w, y + h), c.P.line, c.s(h * 0.5f));
  c.dl->AddCircleFilled(c.p(x + M(9 + 3.5f), y + h * 0.5f), c.s(M(3.5f)), dot, 10);
  text(c, x + M(9 + 7 + 6), y + h * 0.5f - T(11) * 0.68f, s.c_str(), c.P.textDim, T(11), false);
  return w;
}
// A card whose height is known only after its content: the content draws first (channel 1), the
// card under it (channel 0).
template <class F>
float cardAround(Ctx& c, float x, float y, float w, ImU32 strip, F&& body) {
  c.dl->ChannelsSplit(2);
  c.dl->ChannelsSetCurrent(1);
  const float end = body(x + M(16), y + M(14), w - M(32));
  c.dl->ChannelsSetCurrent(0);
  if (strip) blockFrame(c, x, y, w, end + M(14) - y, strip, false, false);
  else {
    c.dl->AddRectFilled(c.p(x, y), c.p(x + w, end + M(14)), c.P.panel, c.s(M(6)));
    c.dl->AddRect(c.p(x, y), c.p(x + w, end + M(14)), c.P.line, c.s(M(6)));
  }
  c.dl->ChannelsMerge();
  return end + M(14);
}
float cardTitle(Ctx& c, float x, float y, float w, const char* t, const std::string& note) {
  text(c, x, y + M(9) - T(13) * 0.68f, t, c.P.text, T(13), true);
  if (!note.empty()) textR(c, x + w, y + M(9) - T(11) * 0.68f, note.c_str(), c.P.textFaint, T(11), false);
  return y + M(18) + M(12);
}
void openInPanels(Ctx& c, int id) {
  Ui& u = c.u;
  u.patch = false;
  if (id >= NSrc && id < NSrc + 5) u.src = id - NSrc, u.view = id - NSrc < 3 ? VLayer : VRootTex, u.layer = std::min(id - NSrc, 2);
  else if (id == NFilt || id == NFilt + 1) u.view = VFilters;
  else if (id == NAmp || id == NVoices) u.view = VAmp;
  else if (id >= NMove + SrcLfo1 && id <= NMove + SrcLfo10) u.lfoTab = id - NMove - SrcLfo1, openView(u, VLfo);
  else if (id >= NMove + SrcEnv1 && id <= NMove + SrcEnv4) u.envTab = id - NMove - SrcEnv1, openView(u, VEnv);
  else if (id >= NMove) u.view = VLinks;
  else u.view = VEffects;
}
std::string wireWords(const Ctx& c, int id, float weight) {
  std::string s = boxName(c, id);
  if (weight < 0.999f) {
    char b[16];
    std::snprintf(b, sizeof b, ", %d%%", static_cast<int>(std::lround(weight * 100)));
    s += b;
  }
  return s;
}
float inspector(Ctx& c, float x, float y, float w) {
  Ui& u = c.u;
  const Graph& gr = *gGraph;
  if (u.pLink >= 0) {
    return cardAround(c, x, y, w, 0, [&](float ix, float iy, float iw) {
      iy = cardTitle(c, ix, iy, iw, "Link", "");
      return allSettings(c, ix, iy, iw) - M(6);
    });
  }
  const int id = u.pSel;
  if (id < 0 || !gr.at.count(id)) {
    return cardAround(c, x, y, w, 0, [&](float ix, float iy, float iw) {
      iy = cardTitle(c, ix, iy, iw, "Nothing selected", "");
      iy = wrapped(c, ix, iy, iw, "Click a box to edit it here.", c.P.textDim, T(11.5f)) + M(4);
      iy = wrapped(c, ix, iy, iw, "Drag from a box's right edge to send its sound somewhere else. Drag a movement box's pin onto a box to link it to one of its controls.",
                   c.P.textDim, T(11.5f)) + M(4);
      return wrapped(c, ix, iy, iw, "Tab switches back to the panels.", c.P.textFaint, T(11.5f)) - M(6);
    });
  }
  const ImU32 fc = nodeColour(c, id);
  const std::string name = boxName(c, id);
  // the box: name, picture, four dials
  y = cardAround(c, x, y, w, fc, [&](float ix, float iy, float iw) {
    const float hy = iy + M(12);
    float tx = ix;
    const int en = enableOf(id);
    if (en >= 0) led(c, en, tx, hy - M(4), M(8)), tx += M(16);
    text(c, tx, hy - T(14) * 0.68f, name.c_str(), c.P.text, T(14), true);
    const std::string sub = boxSub(c, gr, id);
    const float nw = textW(c, name.c_str(), T(14), true);
    const float bw = textW(c, "Open", T(11.5f), false) + M(18);
    if (!sub.empty()) {
      c.dl->PushClipRect(c.p(tx + nw + M(8), iy), c.p(ix + iw - bw - M(6), iy + M(24)), true);
      text(c, tx + nw + M(8), hy - T(11) * 0.68f, sub.c_str(), c.P.textFaint, T(11), false);
      c.dl->PopClipRect();
    }
    if (btnLook(c, "open", ix + iw - bw, iy, bw, M(24), "Open", true, false, T(11.5f))) openInPanels(c, id);
    if (ImGui::IsItemHovered()) c.a.setHint("Open " + name + " in the panels");
    iy += M(24) + M(12);
    boxPicture(c, gr, id, ix, iy, iw, M(130), true);
    iy += M(130);
    const auto d = boxDials(c, id);
    if (!d.empty()) {
      iy += M(12);
      KnobColour kc(fc);
      const int nd = std::min(4, static_cast<int>(d.size()));
      const float cw = M(56), step = nd > 1 ? (iw - cw) / 3.f : 0.f;
      for (int i = 0; i < nd; ++i) {
        ImGui::PushID(i + 500);
        const float cx = nd == 1 ? ix + cw * 0.5f : ix + cw * 0.5f + step * static_cast<float>(i);
        knob(c, d[static_cast<size_t>(i)].k, cx, iy + M(19), M(19), d[static_cast<size_t>(i)].label, KnobValue | d[static_cast<size_t>(i)].flags);
        ImGui::PopID();
      }
      iy += M(38 + 32);
    }
    return iy;
  });
  y += M(12);
  // where its sound comes from and where it goes (as the engine plays it)
  std::vector<std::pair<std::string, ImU32>> in, out;
  for (const auto& wr : gr.wires) {
    if (wr.kind == 2 && id != NVoices) continue;
    if (wr.to == id) in.push_back({wireWords(c, wr.from, wr.kind == 0 ? wr.weight : 1.f) + (wr.kind == 1 ? " (send)" : ""), nodeColour(c, wr.from)});
    if (wr.from == id) out.push_back({wireWords(c, wr.to, wr.kind == 0 ? wr.weight : 1.f) + (wr.kind == 1 ? " (send)" : ""), nodeColour(c, wr.to)});
  }
  if (id < NMove) {
    y = cardAround(c, x, y, w, 0, [&](float ix, float iy, float iw) {
      const bool isSrc = id >= NSrc && id < NSrc + 5;
      const char* note = id == NFilt || id == NFilt + 1 ? "split by the filter balance" : id == NVoices ? "notes" : "";
      if (!isSrc && id != NVoices) {
        iy = cardTitle(c, ix, iy, iw, "Fed by", note);
        float tx = ix;
        if (in.empty()) text(c, ix, iy + M(12) - T(11.5f) * 0.68f, "Nothing yet", c.P.textFaint, T(11.5f), false), iy += M(24);
        for (const auto& t : in) {
          const float tw = textW(c, t.first.c_str(), T(11), false) + M(31);
          if (tx > ix && tx + tw > ix + iw) tx = ix, iy += M(30);
          tx += tagLook(c, tx, iy, t.first, t.second) + M(6);
        }
        if (!in.empty()) iy += M(24);
        iy += M(12);
      }
      // then goes to: label at the left, the targets at the right
      const char* lab = isSrc ? "Goes to" : id == NVoices ? "Plays" : "Then goes to";
      if (isSrc || id == NVoices) iy = cardTitle(c, ix, iy, iw, lab, ""), lab = nullptr;
      float rx = ix + iw, ry = iy;
      if (lab) text(c, ix, iy + M(12) - T(12) * 0.68f, lab, c.P.label, T(12), false);
      if (out.empty()) textR(c, rx, ry + M(12) - T(11.5f) * 0.68f, id == NOut ? "Plasma's output" : "Nowhere", c.P.textFaint, T(11.5f), false);
      const float minX = lab ? ix + textW(c, lab, T(12), false) + M(10) : ix;
      for (const auto& t : out) {
        const float tw = textW(c, t.first.c_str(), T(11), false) + M(31);
        if (rx - tw < minX && rx < ix + iw) rx = ix + iw, ry += M(30);
        tagLook(c, rx - tw, ry, t.first, t.second);
        rx -= tw + M(6);
      }
      return ry + M(24);
    });
    y += M(12);
  }
  // what moves it (a movement box: what it moves)
  y = cardAround(c, x, y, w, 0, [&](float ix, float iy, float iw) {
    std::vector<int> slots;
    for (int s = 0; s < kModSlots; ++s) {
      const int src = vali(c, modP(s, M_Source)), dest = vali(c, modP(s, M_Dest));
      if (src <= SrcNone || dest <= 0) continue;
      if (id >= NMove ? moveNodeOf(src) == id : ownerBox(gr, dest - 1) == id) slots.push_back(s);
    }
    char note[24];
    std::snprintf(note, sizeof note, "%d link%s", static_cast<int>(slots.size()), slots.size() == 1 ? "" : "s");
    iy = cardTitle(c, ix, iy, iw, id >= NMove ? "Moves" : "Moved by", slots.empty() ? std::string("no links") : std::string(note));
    for (int s : slots) {
      const int src = vali(c, modP(s, M_Source)), k = vali(c, modP(s, M_Dest)) - 1;
      ImGui::PushID(s + 7700);
      ImGui::SetCursorScreenPos(c.p(ix - M(4), iy - M(2)));
      if (ImGui::InvisibleButton("##row", c.F.sz(iw + M(8), M(28)))) u.pLink = s;
      const bool hv = ImGui::IsItemHovered();
      if (hv) c.dl->AddRectFilled(c.p(ix - M(4), iy - M(2)), c.p(ix + iw + M(4), iy + M(26)), c.P.panelHi, c.s(M(4)));
      if (hv) c.a.setHint("Click: this link's settings");
      const std::string tag = id >= NMove ? boxName(c, ownerBox(gr, k)) : sourceLabel(src);
      const float tw = tagLook(c, ix, iy, tag, id >= NMove ? nodeColour(c, ownerBox(gr, k)) : c.P.fam[familyOf(src)]);
      char amt[16];
      std::snprintf(amt, sizeof amt, "%+.0f%%", valf(c, modP(s, M_Amount)) * 100.0);
      float vw;
      {
        MonoText mono;
        vw = std::max(textW(c, amt, T(11.5f), false), M(40));
        textR(c, ix + iw, iy + M(12) - T(11.5f) * 0.68f, amt, c.P.text, T(11.5f), false);
      }
      const std::string pn = plainName(k);
      c.dl->PushClipRect(c.p(ix + tw + M(8), iy), c.p(ix + iw - vw - M(8), iy + M(24)), true);
      textR(c, ix + iw - vw - M(10), iy + M(12) - T(12) * 0.68f, pn.c_str(), c.P.label, T(12), false);
      c.dl->PopClipRect();
      ImGui::PopID();
      iy += M(30);
    }
    if (!slots.empty()) iy += M(4);
    return wrapped(c, ix, iy, iw,
                   id >= NMove ? "Drag its pin onto a box to link it to one of that box's controls."
                               : slots.empty() ? "Nothing moves it yet. Drag a movement box's pin onto it to link one."
                                               : "Hover a movement box to see its wires, or turn on Movement above.",
                   c.P.textFaint, T(11.5f)) -
           M(6);
  });
  y += M(12);
  // everything else, folded
  {
    const int n = static_cast<int>(nodeParams(c, id).size());
    const float h = M(36);
    ImGui::SetCursorScreenPos(c.p(x, y));
    if (ImGui::InvisibleButton("##all", c.F.sz(w, h))) u.pAll = !u.pAll;
    const bool hv = ImGui::IsItemHovered();
    c.dl->AddRectFilled(c.p(x, y), c.p(x + w, y + h), hv ? c.P.panelHi : theme::mix(c.P.panel, c.P.frame, 0.4f), c.s(M(5)));
    c.dl->AddRect(c.p(x, y), c.p(x + w, y + h), c.P.line, c.s(M(5)));
    iconArrow(c.dl, c.p(x + M(16), y + h * 0.5f), c.s(M(3.5f)), u.pAll ? 3 : 1, c.P.label);
    const std::string t = "All " + name + " settings";
    text(c, x + M(28), y + h * 0.5f - T(12) * 0.68f, t.c_str(), c.P.textDim, T(12), false);
    char b[12];
    std::snprintf(b, sizeof b, "%d", n);
    textR(c, x + w - M(14), y + h * 0.5f - T(11) * 0.68f, b, c.P.textFaint, T(11), false);
    y += h;
    if (u.pAll) y = allSettings(c, x, y + M(12), w);
  }
  return y;
}

// ---- the link search: which control of a box (or of any box) a movement source moves
struct Cand {
  int k, box;
  std::string name, where, sentence;
  int slot;  // already linked from this source (-1 no)
};
void linkSearch(Ctx& c, const Graph& gr) {
  LinkAsk& ask = linkAsk();
  static char q[64] = "";
  static int hot = 0;
  if (ImGui::IsWindowAppearing()) q[0] = 0, hot = 0, ImGui::SetKeyboardFocusHere(1);
  {
    const std::string head = std::string(i18n::tr("Link")) + " " + sourceLabel(ask.src) + " " + i18n::tr("to a control");
    ImGui::TextDisabled("%s", head.c_str());
    const char* enter = i18n::tr("Enter to link");
    ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - ImGui::CalcTextSize(enter).x);
    ImGui::TextDisabled("%s", enter);
  }
  ImGui::SetNextItemWidth(-1);
  if (ImGui::InputTextWithHint("##q", i18n::tr("Search the controls"), q, sizeof q)) hot = 0;
  std::string ql = q;
  for (auto& ch : ql) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  // the candidates: the box it was dropped on first, then every box (unless a box was asked)
  std::vector<Cand> sound, fxs, moves;
  std::set<int> seen;
  auto addBox = [&](int box) {
    for (int k : nodeParams(c, box)) {
      if (seen.count(k)) continue;
      const ParamDesc* d = pdesc(k);
      if (!d || d->kind != ParamKind::Continuous) continue;
      seen.insert(k);
      Cand cd{k, box, boxName(c, box) + " " + plainName(k), "", paramSentence(k), -1};
      std::string l = cd.name + " " + cd.sentence;
      for (auto& ch : l) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
      if (!ql.empty() && l.find(ql) == std::string::npos) continue;
      for (int s : modSlotsFor(c, k))
        if (vali(c, modP(s, M_Source)) == ask.src) cd.slot = s;
      const PNode* n = gr.at.count(box) ? &gr.nodes[gr.at.at(box)] : nullptr;
      static const char* const cols[6] = {"Play", "Sources", "Shape", "Amp", "Effects", "Out"};
      cd.where = box >= NFx && box < NFx + kFxSlots ? "Main" : box == NSendA ? "Send A" : box == NSendB ? "Send B" : n && box < NMove ? cols[n->col] : "Movement";
      if (cd.slot >= 0) {
        char b[32];
        std::snprintf(b, sizeof b, "linked, %+.0f%%", valf(c, modP(cd.slot, M_Amount)) * 100.0);
        cd.where = b, cd.sentence = "Already linked. Pick it to change how far.";
      }
      ((box >= NFx && box < NFx + kFxSlots) || box == NSendA || box == NSendB ? fxs : box >= NMove ? moves : sound).push_back(cd);
    }
  };
  if (ask.box >= 0) addBox(ask.box);
  if (ask.box < 0 || !ql.empty()) {
    for (const auto& n : gr.nodes)
      if (n.id != moveNodeOf(ask.src) && n.id != NVoices) addBox(n.id);
    addBox(NVoices);  // what plays the notes last
  }
  std::vector<const Cand*> all;
  for (auto* v : {&sound, &fxs, &moves})
    for (const auto& cd : *v) all.push_back(&cd);
  if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) hot = std::min(hot + 1, static_cast<int>(all.size()) - 1);
  if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) hot = std::max(hot - 1, 0);
  const Cand* chosen = nullptr;
  if (ImGui::IsKeyPressed(ImGuiKey_Enter) && hot < static_cast<int>(all.size())) chosen = all[static_cast<size_t>(hot)];
  ImGui::BeginChild("##list", ImVec2(0, std::min(c.s(M(380)), ImGui::GetTextLineHeightWithSpacing() * 2.6f * static_cast<float>(all.size() + 3))));
  int idx = 0;
  const char* heads[3] = {"Sound", "Effects", "Movement"};
  int hi = 0;
  for (auto* v : {&sound, &fxs, &moves}) {
    if (!v->empty()) ImGui::SeparatorText(i18n::tr(heads[hi]));
    ++hi;
    for (const auto& cd : *v) {
      ImGui::PushID(cd.k);
      const float lh = ImGui::GetTextLineHeight(), pic = lh * 2.4f, w = ImGui::GetContentRegionAvail().x;
      const ImVec2 at = ImGui::GetCursorScreenPos();
      const float tx = at.x + pic + 10, wrap = std::max(40.f, at.x + w - tx - 6);
      const float sentH = ImGui::CalcTextSize(cd.sentence.c_str(), nullptr, false, wrap).y;
      const float rowH = std::max(lh * 2.2f, lh + 3 + sentH + 6);
      const bool isHot = idx == hot;
      if (ImGui::Selectable("##c", isHot, 0, ImVec2(w, rowH))) chosen = &cd;
      if (ImGui::IsItemHovered()) hot = idx;
      ImDrawList* dl = ImGui::GetWindowDrawList();
      const ImU32 bc = nodeColour(c, cd.box);
      const ImVec2 p0(at.x + 2, at.y + 3), p1(at.x + pic, at.y + lh * 1.8f);
      if (cd.box >= NFx && cd.box < NFx + kFxSlots) fxTypeGlyph(dl, vali(c, fxP(cd.box - NFx, X_Type)), p0, p1, bc, c.P.well);
      else {
        dl->AddRectFilled(p0, p1, c.P.well, 3.f);
        const float v = static_cast<float>(std::clamp((val(c, cd.k) - pdesc(cd.k)->min) / std::max(1e-9, pdesc(cd.k)->max - pdesc(cd.k)->min), 0.0, 1.0));
        dl->AddRectFilled(ImVec2(p0.x + 3, p1.y - 6), ImVec2(p0.x + 3 + (p1.x - p0.x - 6) * v, p1.y - 3), bc, 1.5f);
        dl->AddRect(p0, p1, c.P.line, 3.f);
      }
      const ImU32 tc = cd.slot >= 0 ? ImGui::GetColorU32(ImGuiCol_TextDisabled) : ImGui::GetColorU32(ImGuiCol_Text);
      dl->AddText(ImVec2(tx, at.y + 2), tc, cd.name.c_str());
      const ImVec2 ws = ImGui::CalcTextSize(cd.where.c_str());
      dl->AddText(ImVec2(at.x + w - ws.x - 6, at.y + 2), ImGui::GetColorU32(ImGuiCol_TextDisabled), cd.where.c_str());
      dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(tx, at.y + lh + 4), ImGui::GetColorU32(ImGuiCol_TextDisabled), cd.sentence.c_str(), nullptr, wrap);
      layoutWindowText(ImVec2(tx, at.y + 2), cd.name.c_str(), 0);
      layoutWindowText(ImVec2(tx, at.y + lh + 4), cd.sentence.c_str(), wrap);
      ImGui::PopID();
      ++idx;
    }
  }
  if (all.empty()) ImGui::TextDisabled("%s", i18n::tr("No control matches"));
  ImGui::EndChild();
  if (chosen) {  // Enter or a click: one undo step either way
    int s = chosen->slot;
    if (s < 0) s = addModulation(c, ask.src, chosen->k, 0.5f);
    if (s < 0) c.u.pWhy = "All 64 links are in use", c.u.pWhyT = static_cast<float>(ImGui::GetTime());
    else c.u.pLink = s, c.u.pSel = -1;
    ImGui::CloseCurrentPopup();
  }
}

// A toolbar chip: a colour dash and a word, lit when on.
bool showChip(Ctx& c, const char* id, float x, float y, float w, const char* label, bool on, ImU32 col) {
  const float h = M(28);
  ImGui::SetCursorScreenPos(c.p(x, y));
  const bool hit = ImGui::InvisibleButton(id, c.F.sz(w, h));
  const bool hv = ImGui::IsItemHovered();
  layoutBox(c, x, y, w, h, label);
  if (on) c.dl->AddRectFilled(c.p(x, y), c.p(x + w, y + h), c.P.panelHi, c.s(M(4)));
  c.dl->AddRect(c.p(x, y), c.p(x + w, y + h), on || hv ? c.P.lineHi : c.P.line, c.s(M(4)));
  c.dl->AddRectFilled(c.p(x + M(10), y + h * 0.5f - M(1.5f)), c.p(x + M(22), y + h * 0.5f + M(1.5f)), on ? col : theme::withAlpha(col, 0.5f), c.s(M(1.5f)));
  text(c, x + M(29), y + h * 0.5f - T(11.5f) * 0.68f, label, on ? c.P.text : c.P.textFaint, T(11.5f), false);
  return hit;
}
float chipW(Ctx& c, const char* label) { return textW(c, label, T(11.5f), false) + M(39); }

}  // namespace

void patchSelectFor(Ui& u) {
  u.patch = true, u.pLink = -1;
  switch (u.view) {
    case VLayer: u.pSel = NSrc + u.layer; break;
    case VRootTex: u.pSel = NSrc + std::clamp(u.src, 3, 4); break;
    case VFilters: u.pSel = NFilt; break;
    case VAmp: u.pSel = NAmp; break;
    case VEffects: u.pSel = NFx + std::max(0, u.fxSel); break;
    case VLinks: u.pSel = -1, u.pLink = u.linkSel; break;
    case VEnv: u.pSel = u.envTab == 0 ? NAmp : NMove + SrcEnv1 + u.envTab; break;
    case VLfo: u.pSel = NMove + SrcLfo1 + u.lfoTab; break;
    default: break;  // the full-width views open over the Patch view as over the panels
  }
  if (u.devPSel >= 0) u.pSel = u.devPSel;
}

// Dev / tests (GL_PLASMA_PATCH_DEV="<action> <args>;..."): the Patch view's own actions, in order,
// one every 8 frames once the view has settled, through the same calls as the mouse and keys:
// "connect <from> <to>", "cut <route> <dest>" (Alt+click on a wire), "select <node>" then
// "delete" (the Delete key), "unlink <slot>", "addfx <type> <rack>", "tidy", "undo", "redo".
// Before each action (and once after the last) it prints PLASMA_PATCH_STATE <n> undo <position>
// then the plain values of Layer 1's route (destination, balance, Send A), Layer 1's light and
// effect slot 0's type; after it, PLASMA_PATCH <n> <action> ok|refused[: why].
void patchDevActions(Ctx& c) {
  static const char* script = std::getenv("GL_PLASMA_PATCH_DEV");
  if (!script) return;
  static int frames = 0, next = 0;
  if (++frames < 40 || frames % 8) return;
  std::vector<std::string> steps;
  {
    std::string all = script;
    for (size_t at = 0; at <= all.size();) {
      const size_t e = std::min(all.find(';', at), all.size());
      if (e > at) steps.push_back(all.substr(at, e - at));
      at = e + 1;
    }
  }
  if (next > static_cast<int>(steps.size())) return;
  int nLinks = 0;
  for (int s = 0; s < kModSlots; ++s) nLinks += vali(c, modP(s, M_Source)) > SrcNone && vali(c, modP(s, M_Dest)) > 0;
  std::fprintf(stderr, "PLASMA_PATCH_STATE %d undo %zu dest %g bal %g sendA %g on %g fx %g links %d\n", next, c.a.doc.history().position(),
               val(c, routeP(RtA, R_Dest)), val(c, routeP(RtA, R_Balance)), val(c, routeP(RtA, R_Bus1)), val(c, oscP(0, O_Enable)),
               val(c, fxP(0, X_Type)), nLinks);
  if (next == static_cast<int>(steps.size())) {
    ++next;
    return;
  }
  int a1 = -1, a2 = -1;
  char act[16] = {};
  std::sscanf(steps[static_cast<size_t>(next)].c_str(), "%15s %d %d", act, &a1, &a2);
  const std::string A = act;
  bool ok = true;
  std::string why;
  c.a.doc.endGesture();  // its own undo step, as a click is
  if (A == "connect") ok = connect(c, a1, a2, why);
  else if (A == "cut") disconnect(c, PWire{0, 0, 0, 1.f, a1 < 0 ? -1 : a1, a2});
  else if (A == "select") c.u.pSel = a1, c.u.pLink = -1;
  else if (A == "delete") {
    if (c.u.pLink >= 0) removeLink(c, c.u.pLink);
    else if (c.u.pSel >= NFx && c.u.pSel < NFx + kFxSlots) fxRemove(c, c.u.pSel - NFx), c.u.pSel = -1;
    else if (c.u.pSel >= 0 && enableOf(c.u.pSel) >= 0) setv(c, enableOf(c.u.pSel), 0);
    else ok = false;
  } else if (A == "link") {  // LFO 1 onto Layer 1's level, as the link picker does
    const int last = addModulation(c, SrcLfo1, oscP(0, O_Level), 0.5f);
    ok = last >= 0;
    if (ok) c.u.pLink = last, c.u.pSel = -1;
  } else if (A == "unlink") removeLink(c, a1);
  else if (A == "addfx") ok = fxAdd(c, a1, a2);
  else if (A == "tidy") offsets().clear(), c.u.pzoom = -1;
  else if (A == "undo") ok = c.a.doc.undo();
  else if (A == "redo") ok = c.a.doc.redo();
  else ok = false;
  c.a.doc.endGesture();
  std::fprintf(stderr, "PLASMA_PATCH %d %s %s%s%s\n", next, act, ok ? "ok" : "refused", why.empty() ? "" : ": ", why.c_str());
  ++next;
}

void patchView(Ctx& c) {
  Ui& u = c.u;
  const Pal& P = c.P;
  const float cx0 = M(20), cx1 = kW - M(334), cy0 = kTopH + M(56), ix = kW - M(320), iw = M(300);
  gPatchUi = &u;
  patchDevActions(c);
  Graph gr = buildGraph(c);
  gGraph = &gr;
  int links = 0;
  std::map<int, int> boxLinks;
  for (int s = 0; s < kModSlots; ++s) {
    const int src = vali(c, modP(s, M_Source)), dest = vali(c, modP(s, M_Dest));
    if (src <= SrcNone || dest <= 0) continue;
    ++links, ++boxLinks[ownerBox(gr, dest - 1)];
  }
  // ---- toolbar: what to show, Tidy; zoom and the link count at the right
  {
    const float ty = kTopH + M(16), h = M(28);
    {
      MonoText mono;
      capsText(c, cx0, ty + h * 0.5f - T(9.5f) * 0.62f, "Show", P.textFaint, T(9.5f), M(1));
    }
    float x = cx0 + capsW(c, "Show", T(9.5f), M(1)) + M(10);
    struct Ch {
      const char* id;
      const char* label;
      bool* on;
      ImU32 col;
      const char* hint;
    };
    const Ch chs[4] = {{"##shs", "Sound", &u.pShowSound, P.accent, "Show the sound wires"},
                       {"##shm", "Movement", &u.pShowMove, P.fam[FamLfo], "Show every movement wire (else only on hover or select)"},
                       {"##shf", "Sends", &u.pShowFx, P.textDim, "Show the send wires"},
                       {"##shn", "Notes", &u.pShowNotes, P.textFaint, "Show where the notes go"}};
    for (const auto& ch : chs) {
      const float w = chipW(c, ch.label);
      if (showChip(c, ch.id, x, ty, w, ch.label, *ch.on, ch.col)) *ch.on = !*ch.on;
      if (ImGui::IsItemHovered()) c.a.setHint(ch.hint);
      x += w + M(10);
    }
    vline(c, x, ty + M(4), ty + h - M(4), P.line);
    x += M(10);
    if (btnLook(c, "tidy", x, ty, M(50), h, "Tidy", false, false, T(12))) offsets().clear(), u.pzoom = -1;
    if (ImGui::IsItemHovered()) c.a.setHint("Put every box back in its column");
    // right: the link count, Fit, + zoom -
    char b[32];
    std::snprintf(b, sizeof b, "%d", links);
    float rx = cx1;
    const float lw = textW(c, " of 64", T(11.5f), false);
    float nw;
    {
      MonoText mono;
      nw = textW(c, b, T(11.5f), true);
      text(c, rx - lw - nw, ty + h * 0.5f - T(11.5f) * 0.68f, b, links >= kModSlots ? P.hot : P.text, T(11.5f), true);
    }
    text(c, rx - lw, ty + h * 0.5f - T(11.5f) * 0.68f, " of 64", P.textFaint, T(11.5f), false);
    rx -= lw + nw + textW(c, "Links ", T(11.5f), false);
    text(c, rx, ty + h * 0.5f - T(11.5f) * 0.68f, "Links ", P.textFaint, T(11.5f), false);
    rx -= M(14);
    const float bh = M(26), by = ty + (h - bh) * 0.5f;
    const float fw = textW(c, "Fit", T(12), false) + M(20);
    rx -= fw;
    if (btnLook(c, "fit", rx, by, fw, bh, "Fit", false, false, T(12))) u.pzoom = -1;
    if (ImGui::IsItemHovered()) c.a.setHint("Fit the whole patch in view (F)");
    auto zoomBy = [&](float f) {  // around the canvas centre
      const float z = std::max(0.4f, u.pzoom), nz = std::clamp(z * f, 0.4f, 2.0f);
      const float mx = (cx1 - cx0) * 0.5f, my = (kPlayY - M(16) - cy0) * 0.5f;
      u.ppanX = mx - (mx - u.ppanX) * nz / z, u.ppanY = my - (my - u.ppanY) * nz / z, u.pzoom = nz;
    };
    rx -= M(4) + bh;
    if (btnLook(c, "zin", rx, by, bh, bh, "+", false, false, T(12))) zoomBy(1.12f);
    rx -= M(4) + M(52);
    {
      char zb[16];
      std::snprintf(zb, sizeof zb, "%d%%", static_cast<int>(std::lround(std::max(0.4f, u.pzoom) * 100)));
      c.dl->AddRectFilled(c.p(rx, by), c.p(rx + M(52), by + bh), P.field, c.s(M(4)));
      c.dl->AddRect(c.p(rx, by), c.p(rx + M(52), by + bh), P.line, c.s(M(4)));
      MonoText mono;
      textMid(c, rx, rx + M(52), by + bh * 0.5f, zb, P.text, T(11.5f), true);
    }
    rx -= M(4) + bh;
    if (btnLook(c, "zout", rx, by, bh, bh, "-", false, false, T(12))) zoomBy(1 / 1.12f);
  }
  // ---- the canvas: graph units zoomed and panned
  const float cB = kPlayY - M(16) - drawerH(u);
  const ImVec2 c0 = c.p(cx0, cy0), c1 = c.p(cx1, cB);
  ++u.pFrames;  // a sound loaded with the editor arrives a frame later: fit and the dev hooks wait for it
  if (u.pzoom < 0 && u.pFrames >= 3) {  // fit (never larger than 100%)
    const float cw = (cx1 - cx0) / kMk, ch = (cB - cy0) / kMk;
    u.pzoom = std::clamp(std::min(cw / std::max(1.f, gr.maxX + 24), ch / std::max(1.f, gr.maxY + 20)), 0.4f, 1.0f);
    u.ppanX = 0, u.ppanY = 0;
  }
  const float zoom = std::max(0.4f, u.pzoom);
  skin::Form G = c.F;
  G.s = c.F.s * kMk * zoom;
  G.o = ImVec2(c0.x + u.ppanX * c.F.s, c0.y + u.ppanY * c.F.s);
  Ctx g{c.a, c.t, G, c.dl, c.st, c.m, c.u, c.P, c.live, c.hasLive};
  ImGuiWindow* win = ImGui::GetCurrentWindow();
  const ImVec2 keepMax = win->DC.CursorMaxPos;  // boxes panned out of view must not grow the window
  c.dl->AddRectFilled(c0, c1, theme::mix(P.frame, P.well, 0.5f), c.s(M(6)));
  c.dl->AddRect(c0, c1, P.line, c.s(M(6)));
  ImGui::PushClipRect(c0, c1, true);
  layoutScroll(c);  // the canvas pans: a box half out of view is not a cut
  {  // the dot grid
    const float step = 24 * G.s;
    if (step > 6) {
      const float sx = std::fmod(G.o.x - c0.x, step), sy = std::fmod(G.o.y - c0.y, step);
      for (float yy = c0.y + sy; yy < c1.y; yy += step)
        for (float xx = c0.x + sx; xx < c1.x; xx += step) c.dl->AddCircleFilled(ImVec2(xx, yy), std::max(0.8f, G.s * 0.9f), theme::withAlpha(P.line, 0.7f), 6);
    }
  }
  // the background: pan by dragging, zoom by the wheel (submitted first, everything overlaps it)
  ImGui::SetCursorScreenPos(c0);
  ImGui::SetNextItemAllowOverlap();
  ImGui::InvisibleButton("##canvas", ImVec2(c1.x - c0.x, c1.y - c0.y), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
  const bool canvasHot = ImGui::IsItemHovered();
  if (ImGui::IsItemActive() && (ImGui::IsMouseDragging(0, 2) || ImGui::IsMouseDragging(2, 2)))
    u.ppanX += ImGui::GetIO().MouseDelta.x / c.F.s, u.ppanY += ImGui::GetIO().MouseDelta.y / c.F.s;
  if (ImGui::IsItemClicked()) u.pSel = -1, u.pLink = -1;
  if (ImGui::BeginDragDropTarget()) {  // a movement source dropped on the canvas: any control
    if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("PLASMA_SRC")) {
      int src = 0;
      std::memcpy(&src, pl->Data, sizeof(int));
      linkAsk() = {src, -1, ImGui::GetIO().MousePos};
      ImGui::OpenPopup("##patchlink");
    }
    ImGui::EndDragDropTarget();
  }
  if (canvasHot && ImGui::GetIO().MouseWheel != 0) {
    const ImVec2 m = ImGui::GetIO().MousePos;
    const float mfx = (m.x - c0.x) / c.F.s, mfy = (m.y - c0.y) / c.F.s;
    const float nz = std::clamp(zoom * std::pow(1.12f, ImGui::GetIO().MouseWheel), 0.4f, 2.0f);
    u.ppanX = mfx - (mfx - u.ppanX) * nz / zoom, u.ppanY = mfy - (mfy - u.ppanY) * nz / zoom, u.pzoom = nz;
  }
  if (canvasHot && ImGui::IsKeyPressed(ImGuiKey_F, false) && !ImGui::GetIO().WantTextInput) u.pzoom = -1;
  // column heads
  {
    MonoText mono;
    static const char* const heads[6] = {"Play", "Sources", "Shape", "Amp", "Effects", "Out"};
    for (int i = 0; i < 6; ++i) capsText(g, kCols[i].x, 12, heads[i], theme::withAlpha(P.textFaint, 0.75f), TG(9.5f), 1);
    capsText(g, kCols[0].x, gr.moveY + 4, "Movement", theme::withAlpha(P.textFaint, 0.75f), TG(9.5f), 1);
  }
  // hover: a movement box lights what it moves and dims the rest
  const int hover = u.devPatch == 3 ? NMove + SrcLfo1 : u.pHover;
  u.pHover = -1;
  std::set<int> related;
  const bool moveHover = hover >= NMove;
  if (moveHover) {
    related.insert(hover);
    for (int s = 0; s < kModSlots; ++s) {
      const int src = vali(c, modP(s, M_Source)), dest = vali(c, modP(s, M_Dest));
      if (src > SrcNone && dest > 0 && moveNodeOf(src) == hover) related.insert(ownerBox(gr, dest - 1));
    }
  }
  // ---- the wires: plan every route (lanes first), then draw
  struct Drawn {
    std::vector<RP> plan;
    int kind;  // 0 sound, 1 send, 2 notes, 3 a step down the column, 4 movement
    size_t wire;
    int slot;
    bool lit;
  };
  std::vector<Drawn> ds;
  Lanes lanes;
  auto visible = [&](int kind) { return kind == 1 ? u.pShowFx : kind == 2 ? u.pShowNotes : kind == 4 ? true : u.pShowSound; };
  for (size_t i = 0; i < gr.wires.size(); ++i) {
    const auto& w = gr.wires[i];
    const PNode *A = gr.find(w.from), *B = gr.find(w.to);
    if (!A || !B || !visible(w.kind)) continue;
    Drawn d{{}, w.kind, i, -1, !moveHover};
    if (w.kind == 3) {
      const bool down = B->y > A->y;
      const PNode& U = down ? *A : *B;
      const PNode& D = down ? *B : *A;
      d.plan = {{U.x + U.w * 0.5f, U.y + U.h}, {D.x + D.w * 0.5f, D.y}};
    } else if (w.kind == 1) {
      const ImVec2 a(A->x + A->w, A->y + kPinLow), b = inAt(*B);
      const float ly = gr.sendY + (w.to == NSendB ? 6.f : 0.f);
      d.plan = {{a.x, a.y}, {0, a.y, A->col, 200000 + w.from}, {0, ly, A->col, 200000 + w.from}, {0, ly, CFx - 1, 300000 + w.to}, {0, b.y, CFx - 1, 300000 + w.to}, {b.x, b.y}};
    } else if (w.kind == 2) {
      d.plan = planSound(gr, *A, *B, outAt(*A), inAt(*B), 0, 700000);
    } else {
      d.plan = planSound(gr, *A, *B, outAt(*A), inAt(*B), 100000 + w.from, w.to);
    }
    ds.push_back(d);
  }
  for (int s = 0; s < kModSlots; ++s) {  // movement: only on hover, select or Show Movement
    const int src = vali(c, modP(s, M_Source)), dest = vali(c, modP(s, M_Dest));
    if (src <= SrcNone || dest <= 0) continue;
    const int mid = moveNodeOf(src), tid = ownerBox(gr, dest - 1);
    const bool show = u.pShowMove || hover == mid || u.pSel == mid || u.pLink == s;
    const PNode *A = gr.find(mid), *B = gr.find(tid);
    if (!show || !A || !B) continue;
    const ImVec2 b(B->x, B->y + kPinLow);
    const int gB = B->col - 1;
    // out of the side that faces the target; into the lane before the target's column
    const bool left = gB < A->col;
    const ImVec2 a = left ? inAt(*A) : outAt(*A);
    const int gA = left ? A->col - 1 : A->col;
    Drawn d{{}, 4, 0, s, true};
    if (gA == gB) d.plan = {{a.x, a.y}, {0, a.y, gB, 500000 + tid}, {0, b.y, gB, 500000 + tid}, {b.x, b.y}};
    else d.plan = {{a.x, a.y}, {0, a.y, gA, 400000 + mid}, {0, gr.busY, gA, 400000 + mid}, {0, gr.busY, gB, 500000 + tid}, {0, b.y, gB, 500000 + tid}, {b.x, b.y}};
    ds.push_back(d);
  }
  for (const auto& d : ds)
    for (const auto& p : d.plan)
      if (p.gap >= 0) lanes.use(p.gap, p.key);
  const float t = static_cast<float>(ImGui::GetTime());
  const bool playing = c.hasLive && c.live.voices > 0;
  std::vector<std::pair<ImVec2, int>> pins;  // screen point, 0 sound 1 movement
  const ImVec2 mouse = ImGui::GetIO().MousePos;
  int hotWire = -1;
  for (int pass = 0; pass < 2; ++pass)  // sends and notes under the sound
    for (size_t i = 0; i < ds.size(); ++i) {
      const auto& d = ds[i];
      if ((pass == 0) != (d.kind == 1 || d.kind == 2)) continue;
      if (d.kind == 4) continue;
      std::vector<ImVec2> pts;
      for (const auto& p : d.plan) pts.push_back(g.p(p.gap >= 0 ? lanes.x(p.gap, p.key) : p.x, p.y));
      const auto path = lanePath(pts, G.u(8));
      const auto& w = gr.wires[d.wire];
      const PNode *A = gr.find(w.from), *B = gr.find(w.to);
      float alpha = d.lit ? 1.f : 0.3f;
      if (!A->on || !B->on) alpha *= 0.35f;
      if (d.kind != 2 && canvasHot && pathDist(path, mouse) < G.u(4)) hotWire = static_cast<int>(d.wire);
      const bool hotOne = hotWire == static_cast<int>(d.wire);
      if (d.kind == 1) drawPath(c.dl, path, theme::withAlpha(hotOne ? P.hot : P.textDim, 0.45f * alpha + (hotOne ? 0.4f : 0.f)), std::max(1.f, G.u(1.3f)), 1, G.s * 0.75f);
      else if (d.kind == 2) drawPath(c.dl, path, theme::withAlpha(P.textFaint, 0.6f * alpha), std::max(1.f, G.u(1.2f)), 2, G.s);
      else {
        drawPath(c.dl, path, theme::withAlpha(hotOne ? P.hot : P.accent, 0.7f * alpha + (hotOne ? 0.3f : 0.f)), std::max(1.2f, G.u(w.kind == 0 ? 1.2f + 0.8f * w.weight : 2.f)), 0, G.s);
        if (playing && A->on && B->on) {  // dots that move with the sound
          const float L = pathLen(path), gap = G.u(46);
          for (float dd = std::fmod((t + static_cast<float>(i) * 0.13f) * G.u(50), gap); dd < L; dd += gap)
            c.dl->AddCircleFilled(pathAt(path, dd / std::max(1.f, L)), std::max(1.f, G.u(1.4f)), theme::withAlpha(P.text, 0.8f * alpha), 6);
        }
      }
      pins.push_back({path.front(), 0}), pins.push_back({path.back(), 0});
    }
  if (hotWire >= 0) {
    c.a.setHint("Alt+click: take this connection away");
    if (ImGui::GetIO().KeyAlt && ImGui::IsMouseClicked(0)) disconnect(c, gr.wires[static_cast<size_t>(hotWire)]);
  }
  // ---- the boxes
  for (const auto& n : gr.nodes) {
    auto it = boxLinks.find(n.id);
    drawBox(g, gr, n, it == boxLinks.end() ? 0 : it->second, u.pSel == n.id, moveHover && !related.count(n.id), hover == n.id && moveHover);
  }
  // ---- movement wires over the boxes' edges, with their pins
  for (const auto& d : ds) {
    if (d.kind != 4) continue;
    std::vector<ImVec2> pts;
    for (const auto& p : d.plan) pts.push_back(g.p(p.gap >= 0 ? lanes.x(p.gap, p.key) : p.x, p.y));
    const auto path = lanePath(pts, G.u(8));
    const int src = vali(c, modP(d.slot, M_Source));
    const bool byp = val(c, modP(d.slot, M_Bypass)) >= 0.5;
    const ImU32 col = byp ? P.textFaint : P.fam[familyOf(src)];
    const bool sel = u.pLink == d.slot;
    drawPath(c.dl, path, col, std::max(1.2f, G.u(sel ? 2.2f : 1.6f)), sel ? 0 : 2, G.s);
    if (!byp) {
      const float L = pathLen(path), gap = G.u(64);
      for (float dd = std::fmod((t + static_cast<float>(d.slot) * 0.3f) * G.u(40), gap); dd < L; dd += gap) glowDot(g, pathAt(path, dd / std::max(1.f, L)), 2.2f, col);
    }
    pins.push_back({path.front(), 1}), pins.push_back({path.back(), 1});
  }
  for (const auto& [at, kind] : pins) {
    const float r = G.u(4);
    if (kind == 0) {
      c.dl->AddCircleFilled(at, r, theme::mix(P.frame, P.well, 0.5f), 12);
      c.dl->AddCircle(at, r, theme::withAlpha(P.accent, 0.8f), 12, std::max(1.f, G.u(1.5f)));
    } else {
      const ImU32 lc = P.fam[FamLfo];
      c.dl->AddQuadFilled(ImVec2(at.x, at.y - r), ImVec2(at.x + r, at.y), ImVec2(at.x, at.y + r), ImVec2(at.x - r, at.y), theme::mix(P.frame, P.well, 0.5f));
      c.dl->AddQuad(ImVec2(at.x, at.y - r), ImVec2(at.x + r, at.y), ImVec2(at.x, at.y + r), ImVec2(at.x - r, at.y), lc, std::max(1.f, G.u(1.5f)));
    }
  }
  // ---- the pins you drag: a source or filter's sound out, a movement box's link
  for (const auto& n : gr.nodes) {
    const bool soundOut = (n.id >= NSrc && n.id < NSrc + 5) || n.id == NFilt || n.id == NFilt + 1 || n.id == NSendA;
    if (!soundOut && n.id < NMove) continue;
    const ImVec2 o = g.p(n.x + n.w, n.y + kPinMid);
    ImGui::PushID(n.id + 46000);
    ImGui::SetCursorScreenPos(ImVec2(o.x - G.u(7), o.y - G.u(7)));
    ImGui::InvisibleButton("##pin", ImVec2(G.u(14), G.u(14)));
    const bool hv = ImGui::IsItemHovered();
    if (n.id >= NMove) {
      const int src = n.id == NMove + SrcMacro1 ? SrcMacro1 : n.id - NMove;
      if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoPreviewTooltip)) {
        ImGui::SetDragDropPayload("PLASMA_SRC", &src, sizeof(int));
        u.pDragPin = n.id;
        ImGui::EndDragDropSource();
      }
      if (hv) c.a.setHint(boxName(c, n.id) + ": drag onto a box to link it to one of its controls");
      const float r = G.u(hv ? 5 : 4);
      const ImU32 lc = P.fam[familyOf(src)];
      c.dl->AddQuadFilled(ImVec2(o.x, o.y - r), ImVec2(o.x + r, o.y), ImVec2(o.x, o.y + r), ImVec2(o.x - r, o.y), hv ? lc : theme::mix(P.frame, P.well, 0.5f));
      c.dl->AddQuad(ImVec2(o.x, o.y - r), ImVec2(o.x + r, o.y), ImVec2(o.x, o.y + r), ImVec2(o.x - r, o.y), theme::withAlpha(lc, hv || moveHover ? 1.f : 0.55f),
                    std::max(1.f, G.u(1.5f)));
    } else {
      if (ImGui::IsItemActivated()) u.pDragNode = n.id;
      if (hv) {
        c.a.setHint(boxName(c, n.id) + ": drag from here to where its sound should go");
        c.dl->AddCircleFilled(o, G.u(5), P.hot, 12);
      }
    }
    ImGui::PopID();
  }
  // a movement link being dragged: a dotted wire from its pin to the pointer
  if (const ImGuiPayload* pl = ImGui::GetDragDropPayload(); pl && pl->IsDataType("PLASMA_SRC") && u.pDragPin >= 0) {
    if (const PNode* A = gr.find(u.pDragPin)) {
      const ImVec2 a = g.p(A->x + A->w, A->y + kPinMid);
      drawPath(c.dl, lanePath({a, ImVec2(a.x + G.u(12), a.y), ImVec2(a.x + G.u(12), mouse.y), mouse}, G.u(8)), P.fam[FamLfo], std::max(1.2f, G.u(1.8f)), 2, G.s);
    }
  } else {
    u.pDragPin = -1;
  }
  // a sound wire being dragged out of a box
  if (u.pDragNode >= 0) {
    PNode* A = gr.find(u.pDragNode);
    if (A && ImGui::IsMouseDown(0)) {
      const ImVec2 a = g.p(A->x + A->w, A->y + kPinMid);
      drawPath(c.dl, lanePath({a, ImVec2(a.x + G.u(14), a.y), ImVec2(a.x + G.u(14), mouse.y), mouse}, G.u(8)), P.hot, std::max(1.5f, G.u(2)), 0, G.s);
      for (const auto& n : gr.nodes) {  // light the boxes it can go to
        const bool ok = n.id != u.pDragNode && ((routeOfNode(u.pDragNode) >= 0 && (n.id == NFilt || n.id == NFilt + 1 || n.id == NAmp || n.id == NOut ||
                                                                                  n.id == NSendA || n.id == NSendB)) ||
                                                (u.pDragNode == NSendA && n.id == NSendB));
        if (ok) c.dl->AddRect(g.p(n.x - 3, n.y - 3), g.p(n.x + n.w + 3, n.y + n.h + 3), P.good, G.u(8), 0, std::max(1.f, G.u(2)));
      }
    } else {
      int target = -1;
      for (const auto& n : gr.nodes)
        if (ImGui::IsMouseHoveringRect(g.p(n.x, n.y), g.p(n.x + n.w, n.y + n.h), false)) target = n.id;
      if (target >= 0 && target != u.pDragNode) {
        std::string why;
        if (!connect(c, u.pDragNode, target, why)) u.pWhy = why, u.pWhyT = static_cast<float>(ImGui::GetTime());
      } else if (target < 0 && ImGui::IsMouseHoveringRect(c0, c1)) {
        u.pAddFrom = u.pDragNode;
        if (A) addMenuAt(g, *A, mouse, c0, c1);
        ImGui::OpenPopup("##patchadd");
      }
      u.pDragNode = -1;
    }
  }
  // ---- the map of the whole patch (bottom right): click or drag to look somewhere else
  {
    const float mw = M(112), mh = M(66), mx = cx1 - M(8) - mw, my = cB - M(8) - mh;
    const ImVec2 ma = c.p(mx, my), mb = c.p(mx + mw, my + mh);
    c.dl->AddRectFilled(ma, mb, theme::withAlpha(P.frame, 0.6f), c.s(M(4)));
    c.dl->AddRect(ma, mb, theme::withAlpha(P.line, 0.6f), c.s(M(4)));
    const float pad = M(6), sx = (mw - pad * 2) / std::max(1.f, gr.maxX + 24), sy = (mh - pad * 2) / std::max(1.f, gr.maxY + 20);
    const float sc = std::min(sx, sy);
    for (const auto& n : gr.nodes)
      c.dl->AddRectFilled(c.p(mx + pad + n.x * sc, my + pad + n.y * sc), c.p(mx + pad + (n.x + n.w) * sc, my + pad + (n.y + n.h) * sc), theme::withAlpha(nodeColour(c, n.id), 0.28f));
    // the part in view
    const float vx0 = -u.ppanX / (kMk * zoom), vy0 = -u.ppanY / (kMk * zoom), vw = (cx1 - cx0) / (kMk * zoom), vh = (cB - cy0) / (kMk * zoom);
    const ImVec2 va = c.p(mx + pad + std::max(0.f, vx0) * sc, my + pad + std::max(0.f, vy0) * sc);
    const ImVec2 vb = c.p(std::min(mx + mw - 2, mx + pad + (vx0 + vw) * sc), std::min(my + mh - 2, my + pad + (vy0 + vh) * sc));
    c.dl->AddRect(va, vb, theme::withAlpha(P.accent, 0.45f), c.s(M(2)));
    ImGui::SetCursorScreenPos(ma);
    ImGui::InvisibleButton("##map", ImVec2(mb.x - ma.x, mb.y - ma.y));
    if (ImGui::IsItemActive()) {  // centre the view on the point
      const ImVec2 m = ImGui::GetIO().MousePos;
      const float gx = ((m.x - ma.x) / c.F.s - pad) / sc, gy = ((m.y - ma.y) / c.F.s - pad) / sc;
      u.ppanX = (cx1 - cx0) * 0.5f - gx * kMk * zoom, u.ppanY = (cB - cy0) * 0.5f - gy * kMk * zoom;
    }
    if (ImGui::IsItemHovered()) c.a.setHint("The whole patch: click or drag to look there");
  }
  if (u.devPatch == 6 && u.pFrames >= 4 && u.pFrames <= 6) {  // dev: both sends in use (a room on A, a chorus on B), Layer 1 and Root sending
    if (u.pFrames == 4) setv(c, routeP(RtA, R_Bus1), 0.4), setv(c, routeP(RtSub, R_Bus2), 0.3), fxAdd(c, FxReverb, 1);
    if (u.pFrames == 5) fxAdd(c, FxChorus, 2);
    if (u.pFrames == 6) fxAdd(c, FxEq, 2), u.devPatch = 5;
  }
  if (u.devPatch >= 3 && u.pFrames >= 6 && u.pSel < 0) u.pSel = NFilt;  // dev: Filter 1 selected (as the design)
  if (u.devPatch == 1 && u.pFrames >= 6) {  // dev: the first link selected
    for (int s = 0; s < kModSlots && u.pLink < 0; ++s)
      if (vali(c, modP(s, M_Source)) > SrcNone && vali(c, modP(s, M_Dest)) > 0) u.pLink = s;
    u.devPatch = 0;
  }
  if (u.devPatch == 2 && u.pFrames >= 6) {  // dev: the search menu, as after a wire from Layer 1 is dropped
    u.pAddFrom = NSrc, u.devPatch = 0;
    if (PNode* A = gr.find(NSrc)) addMenuAt(g, *A, g.p(A->x + A->w, A->y + kPinMid), c0, c1);
    ImGui::OpenPopup("##patchadd");
  }
  if (u.devPatch == 4 && u.pFrames >= 6) {  // dev: the link search, as after LFO 1 is dropped on the canvas
    u.devPatch = 0;
    if (const PNode* A = gr.find(NMove + SrcLfo1)) linkAsk() = {SrcLfo1, -1, g.p(kCols[CShape].x + 16, gr.mainB - 160)}, (void)A;
    ImGui::OpenPopup("##patchlink");
  }
  ImGui::SetNextWindowPos(gAddMenuPos, ImGuiCond_Appearing);
  ImGui::SetNextWindowSize(ImVec2(c.s(kAddMenuW), 0), ImGuiCond_Appearing);
  ImGui::SetNextWindowSizeConstraints(ImVec2(c.s(kAddMenuW), 0), ImVec2(c.s(kAddMenuW), c.s(620)));
  if (ImGui::BeginPopup("##patchadd")) {
    addMenuAfterDrop(c, u.pAddFrom);
    ImGui::EndPopup();
  }
  {
    const LinkAsk& ask = linkAsk();
    const float w = c.s(M(330));
    ImVec2 at(ask.at.x + c.s(M(14)), ask.at.y - c.s(M(24)));
    if (at.x + w > c1.x) at.x = ask.at.x - c.s(M(14)) - w;
    at.x = std::max(c0.x, at.x);
    at.y = std::clamp(at.y, c0.y, std::max(c0.y, c1.y - c.s(M(470))));
    ImGui::SetNextWindowPos(at, ImGuiCond_Appearing);
    ImGui::SetNextWindowSize(ImVec2(w, 0), ImGuiCond_Appearing);
    ImGui::SetNextWindowSizeConstraints(ImVec2(w, 0), ImVec2(w, c.s(M(470))));
    if (ImGui::BeginPopup("##patchlink")) {
      linkSearch(c, gr);
      ImGui::EndPopup();
    }
  }
  // right-click the background: show movement sources, add effects
  if (canvasHot && ImGui::IsMouseClicked(1) && !ImGui::IsAnyItemHovered()) ImGui::OpenPopup("##canvasmenu");
  if (ImGui::BeginPopup("##canvasmenu")) {
    if (ImGui::BeginMenu(i18n::tr("Show a movement source"))) {
      int src = -1;
      if (sourceMenuItems(c, src) && src > SrcNone) shownMoves().insert(src);
      ImGui::EndMenu();
    }
    static const char* const rk[3] = {"Add an effect to the main chain", "Add an effect to Send A", "Add an effect to Send B"};
    for (int r = 0; r < 3; ++r)
      if (ImGui::BeginMenu(i18n::tr(rk[r]))) {
        fxAddMenu(c, r);
        ImGui::EndMenu();
      }
    if (ImGui::MenuItem(i18n::tr("Tidy"))) offsets().clear(), u.pzoom = -1;
    if (ImGui::MenuItem(i18n::tr("Fit"))) u.pzoom = -1;
    ImGui::EndPopup();
  }
  // Delete: an effect goes, a box with a light turns off; a selected link goes
  if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::GetIO().WantTextInput && !ImGui::IsAnyItemActive() &&
      !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) && (u.pLink >= 0 || u.pSel >= 0) &&
      (ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false))) {  // Backspace: the Mac delete key
    c.a.doc.beginGroup("Delete");  // a key press is not a pointer gesture: keep it one undo step
    if (u.pLink >= 0) removeLink(c, u.pLink);
    else if (u.pSel >= NFx && u.pSel < NFx + kFxSlots) fxRemove(c, u.pSel - NFx), u.pSel = -1;
    else if (u.pSel >= 0 && enableOf(u.pSel) >= 0) setv(c, enableOf(u.pSel), 0);
    c.a.doc.endGroup();
  }
  ImGui::PopClipRect();
  win->DC.CursorMaxPos = keepMax;
  // the reason a connection was refused, for a moment
  if (!u.pWhy.empty() && ImGui::GetTime() - u.pWhyT < 3.0) {
    const float w = textW(c, u.pWhy.c_str(), T(11.5f), false) + M(24);
    c.dl->AddRectFilled(c.p(cx0 + M(12), cB - M(44)), c.p(cx0 + M(12) + w, cB - M(14)), P.head, c.s(M(5)));
    c.dl->AddRect(c.p(cx0 + M(12), cB - M(44)), c.p(cx0 + M(12) + w, cB - M(14)), P.hot, c.s(M(5)));
    text(c, cx0 + M(24), cB - M(29) - T(11.5f) * 0.68f, u.pWhy.c_str(), P.text, T(11.5f), false);
  }
  // ---- the inspector
  scrollBody(c, ix - 12, cy0 - M(40), iw + 24, kPlayY - M(16) - drawerH(u), u.pScroll, u.pContent, inspector);
  gGraph = nullptr;
  gPatchUi = nullptr;
}

void fxTypeGlyphPublic(Ctx& c, int type, float x, float y, float w, float h, ImU32 col) {
  fxTypeGlyph(c.dl, type, c.p(x, y), c.p(x + w, y + h), col, c.P.well);
}

}  // namespace gl::packs::plasma::ui
