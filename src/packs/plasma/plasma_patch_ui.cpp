// Gridline — "plasma" pack: the Patch view. The sound as boxes on a calm canvas: the parts in use
// left to right in signal order (sources, shape, amp, effects, out; the sends under the effects),
// quiet lines for where the sound goes, and one tile per movement source in use under the sound.
// Nothing else is drawn at rest: a box shows how many links move it; pointing at a tile (or a box)
// draws one path to what it moves (or what moves it) with the amounts. Boxes and tiles move freely
// (snapped to the grid and to their neighbours); their places are saved with the sound, Clean up
// sorts them back into the flow. The model and the layout live in plasma_patch_dsp.cpp. A sound
// line is a route setting, a link is one link slot: every edit is the same parameter edit the panels
// make (one undo step a gesture).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include "../../app/EditorZoomUi.h"
#include "../../core/I18n.h"
#include "../../third_party/imgui/imgui_internal.h"
#include "plasma_patch.h"
#include "plasma_ui.h"

namespace gl::packs::plasma::ui {
namespace {

using patch::NSrc;
using patch::NFilt;
using patch::NAmp;
using patch::NOut;
using patch::NSendA;
using patch::NSendB;
using patch::NVoices;
using patch::NFx;
using patch::NMove;
using patch::NAddSrc;
using patch::NAddFilt;
using patch::NAddFx;
using patch::NAddMove;
using patch::DF1;
using patch::DF2;
using patch::DAmp;
using patch::DOut;
using patch::DSendA;
using patch::DSendB;
using patch::DChain;
enum : int { NArp = 31, NPhrase = 32 };  // the arpeggiator and phrases (opened from the play bar)

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


float TG(float px) { return px / 1.25f; }  // a text size in graph units
constexpr float kAnimSecs = 0.28f;          // Clean up: the boxes glide into place

int moveNodeOf(int src) { return patch::moveTileOf(src); }
bool isTile(int id) { return id >= NMove && id < NMove + SourceCount; }
bool isAdd(int id) { return id >= NAddSrc && id <= NAddMove; }
ImU32 nodeColour(const Ctx& c, int id) {
  if (isTile(id)) return c.P.fam[familyOf(id - NMove)];
  if (isAdd(id)) return c.P.textFaint;
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

// ---- the graph of the current sound: the model's boxes at their places (the saved ones, else the
// default flow), the one being moved under the pointer, Clean up's glide
struct Graph {
  patch::Patch P;
  std::vector<PNode> nodes;
  std::vector<PWire> wires;
  std::map<int, size_t> at;  // id -> index
  std::map<int, int> fxBox;  // effect slot -> the box that shows it
  std::vector<int> chain[3];
  int mainHead = NOut;
  float minX = 0, minY = 0, maxX = 0, maxY = 0;
  std::vector<patch::Line> lines;  // the sound lines, routed for where the boxes are now
  PNode* find(int id) {
    auto it = at.find(id);
    return it == at.end() ? nullptr : &nodes[it->second];
  }
  const PNode* find(int id) const {
    auto it = at.find(id);
    return it == at.end() ? nullptr : &nodes[it->second];
  }
};
Ui* gPatchUi = nullptr;  // the editor whose Patch view is drawing
std::set<int>& shownMoves() {  // movement sources added by hand (shown though unused)
  static std::set<int> none;
  return gPatchUi ? gPatchUi->pShownMoves : none;
}
int ownerBox(const Graph& g, int k) { return patch::ownerBox(g.P, k); }

// The boxes to show although nothing needs them: the selected one, a link's ends, sources added by hand.
std::set<int> keepShown(const Ctx& c) {
  std::set<int> keep;
  for (int s : shownMoves()) keep.insert(NMove + s);
  if (c.u.pSel >= 0) keep.insert(c.u.pSel);
  if (c.u.pLink >= 0 && vali(c, modP(c.u.pLink, M_Source)) > SrcNone) keep.insert(NMove + vali(c, modP(c.u.pLink, M_Source)));
  return keep;
}

Graph buildGraph(const Ctx& c) {
  Graph g;
  g.P = patch::buildPatch(c.st.params, c.m, keepShown(c));
  patch::place(g.P, c.m.patchPos);
  // the bounds (Fit) of the places the boxes go to, before the move and the glide
  g.minX = g.minY = 1e9f, g.maxX = g.maxY = -1e9f;
  for (const auto& b : g.P.boxes)
    g.minX = std::min(g.minX, b.x), g.minY = std::min(g.minY, b.y), g.maxX = std::max(g.maxX, b.x + b.w), g.maxY = std::max(g.maxY, b.y + b.h);
  if (g.P.boxes.empty()) g.minX = g.minY = g.maxX = g.maxY = 0;
  const Ui& u = c.u;
  if (u.pMove >= 0)
    if (patch::Box* b = g.P.find(u.pMove)) b->x = u.pMoveX, b->y = u.pMoveY;
  if (u.pAnimT >= 0) {  // Clean up: from the old places into the new ones
    const float t = std::clamp((static_cast<float>(ImGui::GetTime()) - u.pAnimT) / kAnimSecs, 0.f, 1.f), e = 1 - (1 - t) * (1 - t) * (1 - t);
    for (auto& b : g.P.boxes)
      if (auto it = u.pAnimFrom.find(b.id); it != u.pAnimFrom.end() && t < 1)
        b.x = it->second[0] + (b.x - it->second[0]) * e, b.y = it->second[1] + (b.y - it->second[1]) * e;
  }
  g.fxBox = g.P.fxBox, g.mainHead = g.P.mainHead;
  for (int r = 0; r < 3; ++r) g.chain[r] = g.P.chain[r];
  for (const auto& b : g.P.boxes) {
    PNode n;
    n.id = b.id, n.col = b.col, n.x = b.x, n.y = b.y, n.w = b.w, n.h = b.h, n.on = b.on;
    g.at[n.id] = g.nodes.size();
    g.nodes.push_back(n);
  }
  for (const auto& e : g.P.edges) g.wires.push_back({e.from, e.to, e.kind, e.weight, e.r, e.dest});
  g.lines = patch::soundLines(g.P);
  return g;
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
    y = wrapped(c, x, y, w, "Drag from a box's right edge to send its sound somewhere else. Drag a movement tile's dot onto a box to link it.", c.P.textDim, 11.5f) + 8;
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
  }
  return {};
}
std::string boxName(const Ctx& c, int id) {
  char buf[48];
  if (id >= NFx && id < NFx + kFxSlots) return i18n::tr(fxDisplayName(vali(c, fxP(id - NFx, X_Type))));
  if (id == NVoices) return "Play";
  if (id >= NMove + SrcMacro1 && id <= NMove + SrcMacro8 && !c.m.macroName[static_cast<size_t>(id - NMove - SrcMacro1)].empty())
    return c.m.macroName[static_cast<size_t>(id - NMove - SrcMacro1)];
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
    if (src >= SrcMacro1 && src <= SrcMacro8) return {{macroP(src - SrcMacro1), "Amount", 0}};
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
      Ctx cc{c.a, c.t, c.F, c.dl, c.st, c.m, c.u, pal2, c.live, c.hasLive, c.graph};
      fxPicture(cc, s, px, py, pw, ph, big);
    } else {
      textMid(c, x, x + w, y + h * 0.5f, textW(c, "No effects yet", TG(10.5f), false) < w - 8 ? "No effects yet" : "Empty", c.P.textFaint, TG(10.5f), false);
    }
  } else if (id >= NMove) {
    const int src = id - NMove;
    if (src >= SrcLfo1 && src <= SrcLfo10) lfoMini(c, src - SrcLfo1, px, py, pw, ph);
    else if (src >= SrcEnv1 && src <= SrcEnv4) envMini(c, src - SrcEnv1, px, py, pw, ph);
    else if (src >= SrcMacro1 && src <= SrcMacro8) {  // how far the macro is turned
      const ImU32 fc = c.P.fam[FamMacro];
      const float v = static_cast<float>(valf(c, macroP(src - SrcMacro1)));
      c.dl->AddRectFilled(c.p(px + pw * 0.3f, py + 1), c.p(px + pw * 0.7f, py + ph - 1), theme::withAlpha(c.P.line, 0.6f), c.s(1));
      c.dl->AddRectFilled(c.p(px + pw * 0.3f, py + ph - 1 - v * (ph - 2)), c.p(px + pw * 0.7f, py + ph - 1), theme::withAlpha(fc, 0.85f), c.s(1));
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

// The open link list (a movement source dropped on a box or on the canvas): the box's main controls
// and the ones already moved first; `all`: every control, with a search.
struct LinkAsk {
  int src = -1, box = -1;
  ImVec2 at;
  bool all = false;
};
LinkAsk& linkAsk() {
  static LinkAsk a;
  return a;
}
// Popups asked for inside a box (its own ID scope) open at the canvas's level, where they are drawn.
bool gOpenLink = false, gOpenAdd = false;

// How a box looks this frame.
struct BoxLook {
  int links = 0;              // links that move it
  std::vector<ImU32> dots;    // their families (one dot each)
  std::string label;          // pointing: the moved control and how far (in its header)
  ImU32 labelCol = 0;
  int labelSlot = -1;         // the link the label stands for (drag it to change how far)
  bool sel = false, dim = false, lit = false, drop = false;
};

// ---- moving a box: snapped to the grid, or to a neighbour's edge or middle when close to one
float snapAxis(float v, float size, const std::vector<std::pair<float, float>>& others, float& guide) {
  float best = 1e9f, out = std::round(v / patch::kGrid) * patch::kGrid;
  guide = NAN;
  for (const auto& [o, os] : others) {
    const float cand[3][2] = {{o, o}, {o + os - size, o + os}, {o + os * 0.5f - size * 0.5f, o + os * 0.5f}};
    for (const auto& cg : cand)
      if (std::fabs(cg[0] - v) < 6 && std::fabs(cg[0] - v) < best) best = std::fabs(cg[0] - v), out = cg[0], guide = cg[1];
  }
  return out;
}
void moveTo(Ui& u, const Graph& gr, const PNode& n, float rx, float ry) {
  std::vector<std::pair<float, float>> xs, ys;
  for (const auto& o : gr.nodes)
    if (o.id != n.id && !isAdd(o.id)) xs.push_back({o.x, o.w}), ys.push_back({o.y, o.h});
  u.pMoveX = snapAxis(rx, n.w, xs, u.pGuideX);
  u.pMoveY = snapAxis(ry, n.h, ys, u.pGuideY);
}
// The move ends: the box stays where it was put, clear of the others (every place is saved with the
// sound: one undo step); a moved effect puts the main chain in its boxes' left-to-right order.
void finishMove(Ctx& c, const Graph& gr, const PNode& n) {
  Ui& u = c.u;
  const int from = n.id;
  (void)u;
  // let go on another box: it slides to the nearest free place below (or beside) it
  patch::Patch p = gr.P;
  if (patch::Box* b = p.find(from)) {
    auto hits = [&] {
      for (const auto& o : p.boxes)
        if (o.id != from && b->x < o.x + o.w + 8 && o.x < b->x + b->w + 8 && b->y < o.y + o.h + 8 && o.y < b->y + b->h + 8) return true;
      return false;
    };
    const float x0 = b->x, y0 = b->y;
    for (int t = 0; t < 400 && hits(); ++t) {
      const int ring = t / 2 + 1;
      if (t % 2 == 0) b->x = x0, b->y = y0 + patch::kGrid * 2 * static_cast<float>(ring);
      else b->x = x0 + patch::kGrid * 2 * static_cast<float>(ring), b->y = y0;
    }
  }
  c.m.patchPos = patch::positions(p);
  if (from >= NFx && from < NFx + kFxSlots) {  // the main chain plays in its boxes' left-to-right order
    std::vector<int> order = p.chain[0];
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return p.find(NFx + a)->x < p.find(NFx + b)->x; });
    if (order != p.chain[0]) {
      fxOrderMain(c, order);  // the model only: the commit below makes the move and the order one step
      commit(c, "Move effect");
      return;
    }
  }
  commit(c, "Move box");
}
// Click to select, drag to move (snapped), right-click for its menu, a movement source dropped on it
// to link one of its controls.
void boxGestures(Ctx& g, const Graph& gr, const PNode& n, const std::string& name) {
  Ui& u = g.u;
  const int id = n.id;
  const ImVec2 a = g.p(n.x, n.y), b = g.p(n.x + n.w, n.y + n.h);
  ImGui::SetCursorScreenPos(a);
  ImGui::SetNextItemAllowOverlap();
  ImGui::InvisibleButton("##box", ImVec2(b.x - a.x, b.y - a.y), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
  if (ImGui::IsItemActivated()) u.pMoveStartX = n.x, u.pMoveStartY = n.y, u.pMoveDX = 0, u.pMoveDY = 0;
  if (ImGui::IsItemClicked(0)) u.pSel = id, u.pLink = -1;
  if (ImGui::IsItemActive() && ImGui::IsMouseDragging(0, 3) && !ImGui::GetDragDropPayload()) {
    u.pMove = id;
    u.pMoveDX += ImGui::GetIO().MouseDelta.x / g.F.s, u.pMoveDY += ImGui::GetIO().MouseDelta.y / g.F.s;
    moveTo(u, gr, n, u.pMoveStartX + u.pMoveDX, u.pMoveStartY + u.pMoveDY);
  }
  if (ImGui::IsItemDeactivated() && u.pMove == id) {
    finishMove(g, gr, n);
    u.pMove = -1, u.pGuideX = u.pGuideY = NAN;
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenOverlappedByItem)) {
    u.pHover = id;
    if (isTile(id)) g.a.setHint(name + ": point to see what it moves; drag its dot onto a box to link it; drag the tile to move it");
    else g.a.setHint(name + ": click to edit it, drag to move it, double-click to open it in the panels");
    if (ImGui::IsMouseDoubleClicked(0)) openInPanels(g, id);
  }
  if (ImGui::BeginDragDropTarget()) {  // a movement source dropped here: pick which control it moves
    if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("PLASMA_SRC")) {
      int src = 0;
      std::memcpy(&src, pl->Data, sizeof(int));
      linkAsk() = {src, id, ImGui::GetIO().MousePos, false};
      gOpenLink = true;
    }
    ImGui::EndDragDropTarget();
  }
  if (ImGui::BeginPopupContextItem("##nodemenu")) {
    const int en = enableOf(id);
    ImGui::TextDisabled("%s", name.c_str());
    ImGui::Separator();
    if (ImGui::MenuItem(i18n::tr("Open in the panels"))) openInPanels(g, id);
    if (en >= 0 && ImGui::MenuItem(val(g, en) >= 0.5 ? i18n::tr("Turn off") : i18n::tr("Turn on"))) setv(g, en, val(g, en) >= 0.5 ? 0 : 1);
    if (id >= NFx && id < NFx + kFxSlots && ImGui::MenuItem(i18n::tr("Remove"))) fxRemove(g, id - NFx);
    if (isTile(id) && shownMoves().count(id - NMove) && ImGui::MenuItem(i18n::tr("Hide"))) shownMoves().erase(id - NMove);
    ImGui::EndPopup();
  }
}
// The count of links that move a box: one dot per link (its family), then the number.
float countLook(Ctx& g, float rx, float cy, const BoxLook& L) {
  if (L.links <= 0) return rx;
  char b[16];
  std::snprintf(b, sizeof b, "%d", L.links);
  float w;
  {
    MonoText mono;
    w = textW(g, b, TG(10.5f), true);
    text(g, rx - w, cy - TG(10.5f) * 0.68f, b, g.P.textDim, TG(10.5f), true);
  }
  float x = rx - w - 5;
  const int nd = std::min(4, static_cast<int>(L.dots.size()));
  for (int i = nd - 1; i >= 0; --i) {
    g.dl->AddCircleFilled(g.p(x - 3, cy), g.s(3), L.dots[static_cast<size_t>(i)], 10);
    x -= 8;
  }
  return x - 2;
}
// A label in a box's header: the moved control and how far (pointing). Drag it to change how far.
float labelLook(Ctx& g, float rx, float cy, const BoxLook& L, const char* id) {
  const float w = textW(g, L.label.c_str(), TG(11), false);
  if (L.labelSlot >= 0 && g.u.pSel >= 0) {  // the selected source's links: the amount follows the pointer
    ImGui::SetCursorScreenPos(g.p(rx - w - 4, cy - 9));
    ImGui::InvisibleButton(id, ImVec2(g.s(w + 8), g.s(18)));
    if (ImGui::IsItemHovered()) g.a.setHint("Drag up or down to change how far it moves; Alt+click takes the link away");
    if (ImGui::IsItemActive() && ImGui::GetIO().MouseDelta.y != 0) {
      const int k = modP(L.labelSlot, M_Amount);
      setv(g, k, std::clamp(val(g, k) - ImGui::GetIO().MouseDelta.y * 0.005, -1.0, 1.0), true);
    }
    if (ImGui::IsItemClicked(0) && ImGui::GetIO().KeyAlt) removeLink(g, L.labelSlot);
  }
  text(g, rx - w, cy - TG(11) * 0.68f, L.label.c_str(), L.labelCol, TG(11), false);
  return rx - w - 8;
}

void drawBox(Ctx& g, const Graph& gr, const PNode& n, const BoxLook& L) {
  const Pal& P = g.P;
  const int id = n.id;
  const std::string name = boxName(g, id);
  ImGui::PushID(id + 40000);
  const ImU32 fc = nodeColour(g, id);
  blockFrame(g, n.x, n.y, n.w, n.h, fc, L.sel, false);
  if (L.drop || L.lit) {  // a link lands here, or pointing lights it: its border in the colour, with a glow
    const ImU32 lc = L.drop ? P.fam[FamLfo] : fc;
    ImVec2 fa, fb;
    blockBox(g, n.x, n.y, n.w, n.h, fa, fb);
    const float o = g.s(1.5f);
    g.dl->AddRect(ImVec2(fa.x - o, fa.y - o), ImVec2(fb.x + o, fb.y + o), theme::withAlpha(lc, 0.3f), blockRound(g) + o, 0, o);
    blockRing(g, fa, fb, theme::mix(lc, P.line, 0.3f), blockBorderW(g, L.sel));
  }
  boxGestures(g, gr, n, name);
  // the head: light, name, what it is; the link count (or, pointing, what moves and how far)
  const float hy = n.y + 16;
  float tx = n.x + 10;
  const int en = enableOf(id);
  if (en >= 0) {
    led(g, en, tx, hy - 3.5f, 7);
    tx += 14;
  }
  float rx = n.x + n.w - 9;
  rx = L.label.empty() ? countLook(g, rx, hy, L) : labelLook(g, rx, hy, L, "##amt");
  {
    const float nw = textW(g, name.c_str(), TG(12.5f), true);
    g.dl->PushClipRect(g.p(tx, n.y + 1), g.p(rx, n.y + n.h - 1), true);
    textHeavy(g, tx, hy - TG(12.5f) * 0.68f, name.c_str(), n.on ? P.text : P.textDim, TG(12.5f));
    const std::string sub = id == NOut ? std::string() : boxSub(g, gr, id);
    if (!sub.empty() && L.label.empty() && tx + nw + 7 < rx) {  // what it is, shortened to the room left
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
    const float lv[2] = {g.hasLive ? std::sqrt(std::clamp(g.live.outL, 0.f, 1.f)) : 0.f, g.hasLive ? std::sqrt(std::clamp(g.live.outR, 0.f, 1.f)) : 0.f};
    for (int i = 0; i < 2; ++i) {
      const float my = n.y + 36 + i * 13.f, mx0 = n.x + 10, mx1 = n.x + n.w - 10;
      g.dl->AddRectFilled(g.p(mx0, my), g.p(mx1, my + 7), P.well, g.s(2));
      if (lv[i] > 0.001f) g.dl->AddRectFilled(g.p(mx0, my), g.p(mx0 + (mx1 - mx0) * lv[i], my + 7), lv[i] > 0.95f ? P.danger : P.good, g.s(2));
    }
  } else {
    boxPicture(g, gr, id, n.x + 9, n.y + 30, n.w - 18, n.h - 39, false);
  }
  if (!n.on || L.dim) {  // over the whole box, its border too, so the border stays even
    ImVec2 fa, fb;
    blockBox(g, n.x, n.y, n.w, n.h, fa, fb);
    if (!n.on) g.dl->AddRectFilled(fa, fb, theme::withAlpha(P.frame, 0.4f), blockRound(g));
    if (L.dim) g.dl->AddRectFilled(fa, fb, theme::withAlpha(P.frame, 0.62f), blockRound(g));
  }
  ImGui::PopID();
}

// A movement tile: its live picture, its name, how many links; the dot at its right end is what
// you drag onto a box to link it.
void drawTile(Ctx& g, const Graph& gr, const PNode& n, const BoxLook& L, int nLinks) {
  const Pal& P = g.P;
  const int id = n.id, src = id - NMove;
  const std::string name = boxName(g, id);
  ImGui::PushID(id + 40000);
  const ImU32 fc = nodeColour(g, id);
  blockFrame(g, n.x, n.y, n.w, n.h, fc, L.sel, false);
  if (L.lit || L.drop) {
    ImVec2 fa, fb;
    blockBox(g, n.x, n.y, n.w, n.h, fa, fb);
    g.dl->AddRectFilled(fa, fb, theme::withAlpha(fc, 0.12f), blockRound(g));
    blockRing(g, fa, fb, fc, blockBorderW(g, true));
  }
  boxGestures(g, gr, n, name);
  // its link port on the top edge's middle: where its modulation lines leave, the dot you drag to link
  const float dx = n.x + n.w * 0.5f, dy = n.y;
  ImGui::SetCursorScreenPos(g.p(dx - 8, dy - 8));
  ImGui::InvisibleButton("##dot", ImVec2(g.s(16), g.s(16)));
  const bool hv = ImGui::IsItemHovered();
  if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoPreviewTooltip)) {
    ImGui::SetDragDropPayload("PLASMA_SRC", &src, sizeof(int));
    g.u.pDragPin = id;
    ImGui::EndDragDropSource();
  }
  if (hv) g.u.pHover = id, g.a.setHint(name + ": drag this dot onto a box to link it to one of its controls");
  g.dl->AddCircleFilled(g.p(dx, dy), g.s(hv ? 5.5f : 4.5f), theme::mix(P.frame, P.well, 0.5f), 16);
  g.dl->AddCircle(g.p(dx, dy), g.s(hv ? 5.5f : 4.5f), fc, 16, g.s(1.5f));
  g.dl->AddCircleFilled(g.p(dx, dy), g.s(2), fc, 10);
  // picture, name, what it is (or, pointing, what it moves there), the link count
  boxPicture(g, gr, id, n.x + 7, n.y + 7, 32, n.h - 14, false);
  const float tx = n.x + 47, cy = n.y + n.h * 0.5f;
  float rx = n.x + n.w - 10;
  char cnt[24];
  std::snprintf(cnt, sizeof cnt, "%d", nLinks);
  {
    MonoText mono;
    const float w = textW(g, cnt, TG(10.5f), true);
    text(g, rx - w, cy - TG(10.5f) * 0.68f, cnt, P.textFaint, TG(10.5f), true);
    rx -= w + 8;
  }
  const float lw = L.label.empty() ? 0.f : textW(g, L.label.c_str(), TG(11), false) + 8;
  g.dl->PushClipRect(g.p(tx, n.y + 2), g.p(std::max(tx, rx - lw), n.y + n.h - 2), true);
  const float nw = textW(g, name.c_str(), TG(12), true);
  textHeavy(g, tx, cy - TG(12) * 0.68f, name.c_str(), P.text, TG(12));
  g.dl->PopClipRect();
  if (!L.label.empty()) {
    BoxLook l2 = L;
    labelLook(g, rx, cy, l2, "##amt");
  } else {
    const std::string sub = boxSub(g, gr, id);
    if (!sub.empty() && tx + nw + 7 + textW(g, sub.c_str(), TG(10.5f), false) < rx) text(g, tx + nw + 7, cy - TG(10.5f) * 0.68f, sub.c_str(), P.textFaint, TG(10.5f), false);
  }
  if (L.dim) {
    ImVec2 fa, fb;
    blockBox(g, n.x, n.y, n.w, n.h, fa, fb);
    g.dl->AddRectFilled(fa, fb, theme::withAlpha(P.frame, 0.62f), blockRound(g));
  }
  ImGui::PopID();
  (void)src;
}

// A dashed "add" tile: a part, an effect or a movement source to add.
void drawAdd(Ctx& g, const PNode& n) {
  const char* label = n.id == NAddSrc ? "Layer, root or texture" : n.id == NAddFilt ? "Filter" : n.id == NAddFx ? "Effect" : "Add movement";
  ImGui::PushID(n.id + 40000);
  ImGui::SetCursorScreenPos(g.p(n.x, n.y));
  const bool hit = ImGui::InvisibleButton("##add", ImVec2(g.s(n.w), g.s(n.h)));
  const bool hv = ImGui::IsItemHovered();
  if (hv) g.a.setHint(n.id == NAddMove ? "Add a movement source (an LFO, an envelope, a macro, ...)" : "Add one to the sound");
  const ImU32 col = hv ? g.P.textDim : theme::withAlpha(g.P.textFaint, 0.8f);
  const ImVec2 a = g.p(n.x, n.y), b = g.p(n.x + n.w, n.y + n.h);
  drawPath(g.dl, {a, ImVec2(b.x, a.y), b, ImVec2(a.x, b.y), a}, col, std::max(1.f, g.s(1)), 1, g.F.s * 0.6f);
  const std::string s = std::string("+  ") + i18n::tr(label);
  if (textW(g, s.c_str(), TG(11.5f), false) < n.w - 12) textMid(g, n.x, n.x + n.w, n.y + n.h * 0.5f, s.c_str(), col, TG(11.5f), false);
  else textMid(g, n.x, n.x + n.w, n.y + n.h * 0.5f, "+", col, TG(13), false);
  if (hit) g.u.pAddTile = n.id, gOpenAdd = true;
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
// card under it (channel 0). Its own splitter: the frame may already have split the draw list (the
// keyboard drawer sits on a channel of its own), and splitting it again would wipe that channel.
template <class F>
float cardAround(Ctx& c, float x, float y, float w, ImU32 strip, F&& body) {
  ImDrawListSplitter split;
  split.Split(c.dl, 2);
  split.SetCurrentChannel(c.dl, 1);
  const float end = body(x + M(16), y + M(14), w - M(32));
  split.SetCurrentChannel(c.dl, 0);
  if (strip) blockFrame(c, x, y, w, end + M(14) - y, strip, false, false);
  else {
    c.dl->AddRectFilled(c.p(x, y), c.p(x + w, end + M(14)), c.P.panel, c.s(M(6)));
    c.dl->AddRect(c.p(x, y), c.p(x + w, end + M(14)), c.P.line, c.s(M(6)));
  }
  split.Merge(c.dl);
  return end + M(14);
}
float cardTitle(Ctx& c, float x, float y, float w, const char* t, const std::string& note) {
  text(c, x, y + M(9) - T(13) * 0.68f, t, c.P.text, T(13), true);
  if (!note.empty()) textR(c, x + w, y + M(9) - T(11) * 0.68f, note.c_str(), c.P.textFaint, T(11), false);
  return y + M(18) + M(12);
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
      iy = wrapped(c, ix, iy, iw, "Drag from a box's right edge to send its sound somewhere else. Drag a movement tile's dot onto a box to link it to one of its controls.",
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
                   isTile(id) ? "Drag its dot onto a box to link it to one of that box's controls."
                               : slots.empty() ? "Nothing moves it yet. Drag a movement box's pin onto it to link one."
                                               : "Point at a movement tile to see its links.",
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


// ---- the link list: which control of a box (or of any box) a movement source moves. Dropped on a
// box it is short: that box's main controls and the ones already moved; "All controls" opens the
// full list with a search.
struct Cand {
  int k, box;
  std::string name, where, sentence;
  int slot;  // already linked from this source (-1 no)
};
int gDevPick = -1;  // dev: the row the open link list takes
void linkSearch(Ctx& c, const Graph& gr) {
  LinkAsk& ask = linkAsk();
  static char q[64] = "";
  static int hot = 0;
  static bool focus = false;
  const bool shortList = ask.box >= 0 && !ask.all;
  if (ImGui::IsWindowAppearing()) q[0] = 0, hot = 0, focus = !shortList;
  {
    const std::string head = shortList ? std::string(i18n::tr("Link")) + " " + sourceLabel(ask.src) + " " + i18n::tr("to") + " " + boxName(c, ask.box)
                                        : std::string(i18n::tr("Link")) + " " + sourceLabel(ask.src) + " " + i18n::tr("to a control");
    ImGui::TextDisabled("%s", head.c_str());
    if (!shortList) {
      const char* enter = i18n::tr("Enter to link");
      ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - ImGui::CalcTextSize(enter).x);
      ImGui::TextDisabled("%s", enter);
    }
  }
  if (!shortList) {
    if (focus) ImGui::SetKeyboardFocusHere(), focus = false;
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputTextWithHint("##q", i18n::tr("Search the controls"), q, sizeof q)) hot = 0;
  }
  std::string ql = shortList ? std::string() : std::string(q);
  for (auto& ch : ql) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  // the short list: the box's dials, then its controls something already moves
  std::set<int> few;
  int allOfBox = 0;
  if (ask.box >= 0) {
    for (const auto& d : boxDials(c, ask.box)) few.insert(d.k);
    for (int s = 0; s < kModSlots; ++s)
      if (vali(c, modP(s, M_Source)) > SrcNone && vali(c, modP(s, M_Dest)) > 0 && ownerBox(gr, vali(c, modP(s, M_Dest)) - 1) == ask.box)
        few.insert(vali(c, modP(s, M_Dest)) - 1);
    for (int k : nodeParams(c, ask.box))
      if (const ParamDesc* d = pdesc(k); d && d->kind == ParamKind::Continuous) ++allOfBox;
  }
  std::vector<Cand> sound, fxs, moves;
  std::set<int> seen;
  auto addBox = [&](int box) {
    for (int k : nodeParams(c, box)) {
      if (seen.count(k) || (shortList && !few.count(k))) continue;
      const ParamDesc* d = pdesc(k);
      if (!d || d->kind != ParamKind::Continuous) continue;
      seen.insert(k);
      Cand cd{k, box, shortList ? plainName(k) : boxName(c, box) + " " + plainName(k), "", paramSentence(k), -1};
      std::string l = cd.name + " " + cd.sentence;
      for (auto& ch : l) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
      if (!ql.empty() && l.find(ql) == std::string::npos) continue;
      for (int s : modSlotsFor(c, k))
        if (vali(c, modP(s, M_Source)) == ask.src) cd.slot = s;
      const PNode* n = gr.find(box);
      static const char* const cols[8] = {"Sources", "Shape", "Amp", "Effects", "Out", "Sends", "Movement", ""};
      cd.where = box >= NFx && box < NFx + kFxSlots ? "Main" : box == NSendA ? "Send A" : box == NSendB ? "Send B" : n ? cols[std::clamp(n->col, 0, 7)] : "Movement";
      if (cd.slot >= 0) {
        char b[32];
        std::snprintf(b, sizeof b, "linked, %+.0f%%", valf(c, modP(cd.slot, M_Amount)) * 100.0);
        cd.where = b, cd.sentence = "Already linked. Pick it to change how far.";
      } else if (shortList) {
        cd.where.clear();
      }
      ((box >= NFx && box < NFx + kFxSlots) || box == NSendA || box == NSendB ? fxs : isTile(box) ? moves : sound).push_back(cd);
    }
  };
  if (ask.box >= 0) addBox(ask.box);
  if (!shortList && (ask.box < 0 || !ql.empty())) {
    for (const auto& n : gr.nodes)
      if (n.id != moveNodeOf(ask.src) && n.id != NVoices && !isAdd(n.id)) addBox(n.id);
    addBox(NVoices);  // what plays the notes last
  }
  std::vector<const Cand*> all;
  for (auto* v : {&sound, &fxs, &moves})
    for (const auto& cd : *v) all.push_back(&cd);
  if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) hot = std::min(hot + 1, static_cast<int>(all.size()) - 1);
  if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) hot = std::max(hot - 1, 0);
  const Cand* chosen = nullptr;
  if (ImGui::IsKeyPressed(ImGuiKey_Enter) && hot < static_cast<int>(all.size())) chosen = all[static_cast<size_t>(hot)];
  if (gDevPick >= 0) {
    if (gDevPick < static_cast<int>(all.size())) chosen = all[static_cast<size_t>(gDevPick)];
    gDevPick = -1;
  }
  const float lh = ImGui::GetTextLineHeight();
  const float rowGuess = shortList ? lh * 1.7f : ImGui::GetTextLineHeightWithSpacing() * 2.6f;
  ImGui::BeginChild("##list", ImVec2(0, std::min(c.s(M(380)), rowGuess * static_cast<float>(all.size() + (shortList ? 0 : 3)) + (shortList ? lh * 0.4f : 0.f))));
  int idx = 0;
  const char* heads[3] = {"Sound", "Effects", "Movement"};
  int hi = 0;
  for (auto* v : {&sound, &fxs, &moves}) {
    if (!v->empty() && !shortList) ImGui::SeparatorText(i18n::tr(heads[hi]));
    ++hi;
    for (const auto& cd : *v) {
      ImGui::PushID(cd.k);
      const float pic = shortList ? lh * 1.6f : lh * 2.4f, w = ImGui::GetContentRegionAvail().x;
      const ImVec2 at = ImGui::GetCursorScreenPos();
      const float tx = at.x + pic + 10, wrap = std::max(40.f, at.x + w - tx - 6);
      const float sentH = shortList ? 0.f : ImGui::CalcTextSize(cd.sentence.c_str(), nullptr, false, wrap).y;
      const float rowH = shortList ? lh * 1.6f : std::max(lh * 2.2f, lh + 3 + sentH + 6);
      const bool isHot = idx == hot;
      if (ImGui::Selectable("##c", isHot, 0, ImVec2(w, rowH))) chosen = &cd;
      if (ImGui::IsItemHovered()) hot = idx;
      ImDrawList* dl = ImGui::GetWindowDrawList();
      const ImU32 bc = nodeColour(c, cd.box);
      const ImVec2 p0(at.x + 2, at.y + 3), p1(at.x + pic, at.y + (shortList ? rowH - 3 : lh * 1.8f));
      if (cd.box >= NFx && cd.box < NFx + kFxSlots && !shortList) fxTypeGlyph(dl, vali(c, fxP(cd.box - NFx, X_Type)), p0, p1, bc, c.P.well);
      else {
        dl->AddRectFilled(p0, p1, c.P.well, 3.f);
        const float v = static_cast<float>(std::clamp((val(c, cd.k) - pdesc(cd.k)->min) / std::max(1e-9, pdesc(cd.k)->max - pdesc(cd.k)->min), 0.0, 1.0));
        dl->AddRectFilled(ImVec2(p0.x + 3, p1.y - 6), ImVec2(p0.x + 3 + (p1.x - p0.x - 6) * v, p1.y - 3), bc, 1.5f);
        dl->AddRect(p0, p1, c.P.line, 3.f);
      }
      const ImU32 tc = cd.slot >= 0 ? ImGui::GetColorU32(ImGuiCol_TextDisabled) : ImGui::GetColorU32(ImGuiCol_Text);
      const float ty = shortList ? at.y + (rowH - lh) * 0.5f : at.y + 2;
      dl->AddText(ImVec2(tx, ty), tc, cd.name.c_str());
      layoutWindowText(ImVec2(tx, ty), cd.name.c_str(), 0);
      if (!cd.where.empty()) {
        const ImVec2 ws = ImGui::CalcTextSize(cd.where.c_str());
        dl->AddText(ImVec2(at.x + w - ws.x - 6, ty), ImGui::GetColorU32(ImGuiCol_TextDisabled), cd.where.c_str());
      }
      if (!shortList) {
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(tx, at.y + lh + 4), ImGui::GetColorU32(ImGuiCol_TextDisabled), cd.sentence.c_str(), nullptr, wrap);
        layoutWindowText(ImVec2(tx, at.y + lh + 4), cd.sentence.c_str(), wrap);
      }
      ImGui::PopID();
      ++idx;
    }
  }
  if (all.empty()) ImGui::TextDisabled("%s", i18n::tr("No control matches"));
  ImGui::EndChild();
  if (shortList) {  // the rest of the box's controls, with a search
    ImGui::Separator();
    char b[64];
    std::snprintf(b, sizeof b, "%s (%d)", i18n::tr("All controls"), allOfBox);
    if (ImGui::Selectable(b)) ask.all = true, hot = 0, focus = true;
  }
  if (chosen) {  // Enter or a click: one undo step either way
    int s = chosen->slot;
    if (s >= 0) c.u.pLink = s, c.u.pSel = -1;  // already linked: its row says so, so show that link
    else s = addModulation(c, ask.src, chosen->k, 0.25f);  // a new link starts at 25 %, as in the Panels
    if (s < 0) c.u.pWhy = "All 64 links are in use", c.u.pWhyT = static_cast<float>(ImGui::GetTime());
    ImGui::CloseCurrentPopup();
  }
}

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

namespace {

// ---- dev / tests: pointer moves fed to the app one a frame (a drag as the hand makes it)
struct DevEv {
  float x, y;
  int btn;  // -1 move only, 1 press, 0 release
};
std::vector<DevEv>& devMouse() {
  static std::vector<DevEv> q;
  return q;
}
skin::Form gLastG;              // the canvas transform (last frame)
bool gHaveG = false;
ImVec2 gFreeSpot(-1, -1);       // an empty place on the canvas (last frame)
int gDevPoint = -1;             // dev: the box shown as pointed at (screenshots)
std::map<int, ImVec2> gEnds;    // each drawn link's end handle (last frame), by slot
void devDrag(ImVec2 a, ImVec2 b, bool release) {  // a, b: the editor's own coordinates
  auto& q = devMouse();
  q.push_back({a.x, a.y, -1});
  q.push_back({a.x, a.y, 1});
  for (int i = 1; i <= 10; ++i) q.push_back({a.x + (b.x - a.x) * static_cast<float>(i) / 10.f, a.y + (b.y - a.y) * static_cast<float>(i) / 10.f, -1});
  q.push_back({b.x, b.y, -1});
  if (release) q.push_back({b.x, b.y, 0}), q.push_back({-1e9f, -1e9f, -1});
}
// One queued pointer event a frame. The editor is drawn at its design size and zoomed as one
// picture, and the window maps the pointer back: post where the point shows, then correct by
// what arrived (a press or a release waits until the pointer is where it should be).
void devMouseFrame() {
  auto& q = devMouse();
  static ImVec2 corr(0, 0), last(-1, -1);
  static int tries = 0;
  if (q.empty()) {
    last = ImVec2(-1, -1);
    return;
  }
  ImGuiIO& io = ImGui::GetIO();
  const float z = std::max(0.1f, edzoomui::currentZoom());
  if (last.x > -1e8f && last.x >= 0 && io.MousePos.x > -1e8f) corr.x += (last.x - io.MousePos.x) * z, corr.y += (last.y - io.MousePos.y) * z;
  const DevEv e = q.front();
  const bool there = std::fabs(io.MousePos.x - e.x) < 1.5f && std::fabs(io.MousePos.y - e.y) < 1.5f;
  if (e.btn >= 0 && !there && tries++ < 6) {  // not there yet: move again
    const ImVec2 at = edzoomui::toScreenHere(ImVec2(e.x, e.y));
    io.AddMousePosEvent(at.x + corr.x, at.y + corr.y);
    last = ImVec2(e.x, e.y);
    return;
  }
  tries = 0;
  q.erase(q.begin());
  if (e.x < -1e8f) {
    io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    last = ImVec2(-1, -1);
  } else if (e.btn < 0) {
    const ImVec2 at = edzoomui::toScreenHere(ImVec2(e.x, e.y));
    io.AddMousePosEvent(at.x + corr.x, at.y + corr.y);
    last = ImVec2(e.x, e.y);
  } else {
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, e.btn == 1);
    last = ImVec2(-1, -1);
  }
}

// Clean up: every box into the flow (the user's order kept where it reads well), gliding there.
// One undo step; false (and nothing changes) when it is already clean.
bool cleanUpNow(Ctx& c) {
  patch::Patch p = patch::buildPatch(c.st.params, c.m, keepShown(c));
  patch::place(p, c.m.patchPos);
  std::map<int, std::array<float, 2>> from;
  for (const auto& b : p.boxes) from[b.id] = {b.x, b.y};
  const auto before = patch::positions(p);
  patch::cleanUp(p);
  const auto to = patch::positions(p);
  if (to == before) return false;
  c.u.pAnimFrom = from, c.u.pAnimT = static_cast<float>(ImGui::GetTime());
  c.m.patchPos = to;
  commit(c, "Clean up");
  return true;
}

// The layout as the saved sound has it (read back from the data, as a reload would): its score and
// a signature of every place.
void printLayout(const Ctx& c, int n) {
  Model m2;
  modelFromData(c.st.data, m2);
  patch::Patch p = patch::buildPatch(c.st.params, m2, {});
  patch::place(p, m2.patchPos);
  const patch::Score s = patch::score(p);
  unsigned long long h = 1469598103934665603ull;
  for (const auto& b : p.boxes)
    for (long v : {static_cast<long>(b.id), std::lround(b.x * 4), std::lround(b.y * 4)}) h = (h ^ static_cast<unsigned long long>(v)) * 1099511628211ull;
  std::fprintf(stderr, "PLASMA_PATCH_LAYOUT %d saved %zu boxes %zu overlaps %d through %d offgrid %d backward %d crossings %d total %d sig %016llx\n", n,
               m2.patchPos.size(), p.boxes.size(), s.overlaps, s.through, s.offGrid, s.backward, s.crossings, static_cast<int>(s.total()), h);
}

// Messy layouts (dev / tests): 0 a pile in one corner, 1 boxes over each other, 2 the flow reversed,
// 3 scattered far off, 4 every box in another's place.
std::map<int, std::array<float, 2>> messy(const patch::Patch& p0, int kind) {
  patch::Patch p = p0;
  patch::defaultLayout(p);
  std::map<int, std::array<float, 2>> v;
  float maxX = 0;
  for (const auto& b : p.boxes) maxX = std::max(maxX, b.x + b.w);
  std::vector<std::array<float, 2>> places;
  for (const auto& b : p.boxes)
    if (patch::movable(b.id)) places.push_back({b.x, b.y});
  unsigned seed = 7u + static_cast<unsigned>(kind);
  auto rnd = [&] {
    seed = seed * 1664525u + 1013904223u;
    return static_cast<float>((seed >> 8) & 0xffff) / 65535.f;
  };
  for (size_t i = places.size(); i > 1; --i) std::swap(places[i - 1], places[static_cast<size_t>(rnd() * static_cast<float>(i - 1) + 0.5f) % i]);
  int i = 0;
  for (const auto& b : p.boxes) {
    if (!patch::movable(b.id)) continue;
    std::array<float, 2> at = {b.x, b.y};
    switch (kind) {
      case 0: at = {static_cast<float>(i % 4) * 8, static_cast<float>(i) * 8}; break;
      case 1: at = {b.x * 0.45f + 13, b.y * 0.4f + 5}; break;
      case 2: at = {maxX - b.x - b.w, b.y}; break;
      case 3: at = {(rnd() - 0.5f) * 6000, (rnd() - 0.5f) * 6000}; break;
      default: at = places[static_cast<size_t>(i)]; break;
    }
    v[b.id] = at;
    ++i;
  }
  return v;
}

// Dev / tests (GL_PLASMA_PATCH_DEV="<action> <args>;..."): the Patch view's own actions, in order,
// one every 8 frames once the view has settled (a drag waits for its pointer moves), through the
// same calls as the mouse and keys: "connect <from> <to>", "cut <route> <dest>" (Alt+click on a
// line), "select <box>" then "delete" (the Delete key), "unlink <slot>", "addfx <type> <rack>",
// "link" (LFO 1 onto Layer 1's level), "undo", "redo", "big" (the 25-link sound), "messy <kind>",
// "cleanup" (the button), "where <box>", "pick <row>" (the open link list), and real pointer
// drags: "drag <box> <dx> <dy>" (moves it), "linkdrag <source> <box>" (a tile's dot onto a box),
// "unlinkdrag <slot>" (a selected tile's link end onto empty canvas), "draghold"/"linkhold" (the
// same, never let go: screenshots). Before each action (and once after the last) it prints
// PLASMA_PATCH_STATE <n> undo <position> then Layer 1's route (destination, balance, Send A),
// Layer 1's light, effect slot 0's type and the link count, and PLASMA_PATCH_LAYOUT <n> with the
// saved layout read back from the data; after it, PLASMA_PATCH <n> <action> ok|refused[: why].
void patchDevActions(Ctx& c, const Graph& gr) {
  static const char* script = std::getenv("GL_PLASMA_PATCH_DEV");
  if (!script) return;
  devMouseFrame();
  static int frames = 0, next = 0, quiet = 0;
  if (!devMouse().empty()) {
    quiet = 0;
    return;
  }
  if (++quiet < 4 || ++frames < 40 || frames % 8) return;
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
  printLayout(c, next);
  if (next == static_cast<int>(steps.size())) {
    ++next;
    return;
  }
  int a1 = -1, a2 = -1, a3 = 0;
  char act[16] = {};
  std::sscanf(steps[static_cast<size_t>(next)].c_str(), "%15s %d %d %d", act, &a1, &a2, &a3);
  const std::string A = act;
  bool ok = true;
  std::string why;
  c.a.doc.endGesture();  // its own undo step, as a click is
  auto centre = [&](const PNode& n) { return gLastG.p(n.x + n.w * 0.5f, n.y + n.h * 0.5f); };
  if (A == "connect") ok = connect(c, a1, a2, why);
  else if (A == "cut") disconnect(c, PWire{0, 0, 0, 1.f, a1 < 0 ? -1 : a1, a2});
  else if (A == "select") c.u.pSel = a1, c.u.pLink = -1;
  else if (A == "delete") {
    if (c.u.pLink >= 0) removeLink(c, c.u.pLink);
    else if (c.u.pSel >= NFx && c.u.pSel < NFx + kFxSlots) fxRemove(c, c.u.pSel - NFx), c.u.pSel = -1;
    else if (c.u.pSel >= 0 && enableOf(c.u.pSel) >= 0) setv(c, enableOf(c.u.pSel), 0);
    else ok = false;
  } else if (A == "link") {  // LFO 1 onto Layer 1's level, as the link list does
    const int last = addModulation(c, SrcLfo1, oscP(0, O_Level), 0.25f);
    ok = last >= 0;
    if (ok) c.u.pLink = last, c.u.pSel = -1;
  } else if (A == "unlink") removeLink(c, a1);
  else if (A == "addfx") ok = fxAdd(c, a1, a2);
  else if (A == "undo") ok = c.a.doc.undo();
  else if (A == "redo") ok = c.a.doc.redo();
  else if (A == "big") {  // the big sound: every part, both sends, six effects, 25 links
    std::vector<float> ps = c.st.params;
    Model m2 = c.m;
    patch::bigSound(ps, m2);
    std::vector<std::pair<int, double>> changed;
    for (size_t k = 0; k < ps.size(); ++k)
      if (k >= c.st.params.size() || ps[k] != c.st.params[k]) changed.push_back({static_cast<int>(k), ps[k]});
    c.m = m2;
    commitParams(c, "Load a sound", changed);
    c.u.pzoom = -1;
  } else if (A == "messy") {
    c.m.patchPos = messy(gr.P, a1);
    commit(c, "Move box");
    c.u.pzoom = -1;
  } else if (A == "cleanup") ok = cleanUpNow(c), c.u.pzoom = -1;
  else if (A == "fit") c.u.pzoom = -1;
  else if (A == "point") gDevPoint = a1;
  else if (A == "where") {
    Model m2;
    modelFromData(c.st.data, m2);
    patch::Patch p = patch::buildPatch(c.st.params, m2, {});
    patch::place(p, m2.patchPos);
    const patch::Box* b = p.find(a1);
    ok = b != nullptr;
    if (b) std::fprintf(stderr, "PLASMA_PATCH_BOX %d %d %g %g saved %d\n", next, a1, b->x, b->y, m2.patchPos.count(a1) ? 1 : 0);
  } else if (A == "pick") gDevPick = a1, ok = ImGui::IsPopupOpen("##patchlink");
  else if (A == "drag" || A == "draghold") {
    const PNode* n = gr.find(a1);
    ok = n && gHaveG;
    if (ok) devDrag(centre(*n), ImVec2(centre(*n).x + gLastG.u(static_cast<float>(a2)), centre(*n).y + gLastG.u(static_cast<float>(a3))), A == "drag");
  } else if (A == "linkdrag" || A == "linkhold") {
    const PNode *t = gr.find(moveNodeOf(a1)), *b = gr.find(a2);
    ok = t && b && gHaveG;
    if (ok) devDrag(gLastG.p(t->x + t->w * 0.5f, t->y + 1), centre(*b), A == "linkdrag");
  } else if (A == "unlinkdrag") {
    auto it = gEnds.find(a1);
    ok = it != gEnds.end() && gFreeSpot.x >= 0;
    if (ok) devDrag(it->second, gFreeSpot, true);
  } else ok = false;
  c.a.doc.endGesture();
  std::fprintf(stderr, "PLASMA_PATCH %d %s %s%s%s\n", next, act, ok ? "ok" : "refused", why.empty() ? "" : ": ", why.c_str());
  ++next;
}

}  // namespace

void patchView(Ctx& c) {
  Ui& u = c.u;
  const Pal& P = c.P;
  const bool inspect = u.pSel >= 0 || u.pLink >= 0;
  const float cx0 = M(20), cx1 = inspect ? kW - M(334) : kW - M(20), cy0 = kTopH + M(56), ix = kW - M(320), iw = M(300);
  gPatchUi = &u;
  if (u.pAnimT >= 0 && ImGui::GetTime() - u.pAnimT > kAnimSecs) u.pAnimT = -1, u.pAnimFrom.clear();
  Graph gr = buildGraph(c);
  patchDevActions(c, gr);
  gGraph = &gr;
  // the links of the sound
  struct Lk {
    int slot, src, k, tile, box;
  };
  std::vector<Lk> lks;
  for (int s = 0; s < kModSlots; ++s) {
    const int src = vali(c, modP(s, M_Source)), dest = vali(c, modP(s, M_Dest));
    if (src <= SrcNone || dest <= 0) continue;
    lks.push_back({s, src, dest - 1, moveNodeOf(src), ownerBox(gr, dest - 1)});
  }
  // ---- the header: Clean up and Fit at the left, the link count at the right
  {
    const float ty = kTopH + M(16), h = M(28);
    float x = cx0;
    const float cw = textW(c, "Clean up", T(12), false) + M(28);
    if (btnLook(c, "cleanup", x, ty, cw, h, "Clean up", false, false, T(12))) cleanUpNow(c);
    if (ImGui::IsItemHovered()) c.a.setHint("Sort the boxes into the flow: sources, shape, amp, effects, out, with the movement below (one undo step)");
    x += cw + M(8);
    const float fw = textW(c, "Fit", T(12), false) + M(24);
    if (btnLook(c, "fit", x, ty, fw, h, "Fit", true, false, T(12))) u.pzoom = -1;
    if (ImGui::IsItemHovered()) c.a.setHint("Fit the whole patch in view (F)");
    char b[32];
    std::snprintf(b, sizeof b, "%d", static_cast<int>(lks.size()));
    float rx = kW - M(20);
    const float lw = textW(c, " of 64", T(11.5f), false);
    float nw;
    {
      MonoText mono;
      nw = textW(c, b, T(11.5f), true);
      text(c, rx - lw - nw, ty + h * 0.5f - T(11.5f) * 0.68f, b, static_cast<int>(lks.size()) >= kModSlots ? P.hot : P.text, T(11.5f), true);
    }
    text(c, rx - lw, ty + h * 0.5f - T(11.5f) * 0.68f, " of 64", P.textFaint, T(11.5f), false);
    rx -= lw + nw + textW(c, "Links ", T(11.5f), false);
    text(c, rx, ty + h * 0.5f - T(11.5f) * 0.68f, "Links ", P.textFaint, T(11.5f), false);
  }
  // ---- the canvas: graph units zoomed and panned
  const float cB = kPlayY - M(16) - drawerH(u);
  const ImVec2 c0 = c.p(cx0, cy0), c1 = c.p(cx1, cB);
  ++u.pFrames;  // a sound loaded with the editor arrives a frame later: fit and the dev hooks wait for it
  if (u.pzoom < 0 && u.pFrames >= 3) {  // fit the whole patch in the full width (never larger than 100%)
    const std::vector<patch::Line> modAll = patch::modLines(gr.P, c.st.params);
    const float cw = (kW - M(20) - cx0) / kMk, ch = (cB - cy0) / kMk;
    float fx0 = gr.minX, fy0 = gr.minY, fx1 = gr.maxX, fy1 = gr.maxY;  // the boxes and every line, sound and links
    for (const std::vector<patch::Line>* set : {static_cast<const std::vector<patch::Line>*>(&gr.lines), &modAll})
      for (const auto& l : *set)
        for (const auto& q : l.pts) fx0 = std::min(fx0, q.x), fy0 = std::min(fy0, q.y), fx1 = std::max(fx1, q.x), fy1 = std::max(fy1, q.y);
    const float gw = fx1 - fx0 + 48, gh = fy1 - fy0 + 40;
    const float z = std::clamp(std::min(cw / std::max(1.f, gw), ch / std::max(1.f, gh)), 0.4f, 1.0f);
    u.pzoom = z;
    u.ppanX = (gw * z < cw ? (cw - gw * z) * 0.5f : 0.f) * kMk - (fx0 - 24) * kMk * z;
    u.ppanY = -(fy0 - 20) * kMk * z;
  }
  const float zoom = std::max(0.4f, u.pzoom);
  skin::Form G = c.F;
  G.s = c.F.s * kMk * zoom;
  G.o = ImVec2(c0.x + u.ppanX * c.F.s, c0.y + u.ppanY * c.F.s);
  gLastG = G, gHaveG = true;
  Ctx g{c.a, c.t, G, c.dl, c.st, c.m, c.u, c.P, c.live, c.hasLive, c.graph};
  g.graph = true;  // the boxes zoom: their text keeps its proportion to them
  ImGuiWindow* win = ImGui::GetCurrentWindow();
  const ImVec2 keepMax = win->DC.CursorMaxPos;  // boxes panned out of view must not grow the window
  c.dl->AddRectFilled(c0, c1, theme::mix(P.frame, P.well, 0.5f), c.s(M(6)));
  c.dl->AddRect(c0, c1, P.line, c.s(M(6)));
  ImGui::PushClipRect(c0, c1, true);
  layoutScroll(c);  // the canvas pans: a box half out of view is not a cut
  {  // the dot grid (every 24 units; boxes snap to 8)
    const float step = 24 * G.s;
    if (step > 6) {
      const float sx = std::fmod(G.o.x - c0.x, step), sy = std::fmod(G.o.y - c0.y, step);
      for (float yy = c0.y + sy; yy < c1.y; yy += step)
        for (float xx = c0.x + sx; xx < c1.x; xx += step) c.dl->AddCircleFilled(ImVec2(xx, yy), std::max(0.8f, G.s * 0.9f), theme::withAlpha(P.line, 0.55f), 6);
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
  if (canvasHot) c.a.setHint("Drag the background to look around, the wheel zooms; right-click to add");
  if (ImGui::BeginDragDropTarget()) {  // a movement source dropped on the canvas: any control
    if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("PLASMA_SRC")) {
      int src = 0;
      std::memcpy(&src, pl->Data, sizeof(int));
      linkAsk() = {src, -1, ImGui::GetIO().MousePos, true};
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
  const ImVec2 mouse = ImGui::GetIO().MousePos;
  // ---- pointing: a tile (what it moves) or a box (what moves it); nothing while a box moves
  const ImGuiPayload* dragPl = ImGui::GetDragDropPayload();
  const bool linkDrag = dragPl && dragPl->IsDataType("PLASMA_SRC");
  int point = u.devPatch == 3 ? NMove + SrcLfo1 : gDevPoint >= 0 ? gDevPoint : u.pHover;
  if (point < 0 && u.pSel >= 0) point = u.pSel;
  if (u.pMove >= 0 || linkDrag || u.pEndDrag >= 0) point = u.pEndDrag >= 0 ? u.pSel : -1;
  u.pHover = -1;
  std::map<int, BoxLook> looks;
  std::map<int, int> tileLinks;
  for (const auto& l : lks) {
    auto& L = looks[l.box];
    ++L.links;
    L.dots.push_back(val(c, modP(l.slot, M_Bypass)) >= 0.5 ? P.textFaint : P.fam[familyOf(l.src)]);
    ++tileLinks[l.tile];
  }
  auto amountText = [&](const Lk& l) {
    char b[24];
    std::snprintf(b, sizeof b, " %+.0f%%", valf(c, modP(l.slot, M_Amount)) * 100.0);
    return plainName(l.k) + b;
  };
  std::vector<const Lk*> shown;  // the links drawn now
  if (point >= 0) {
    std::map<int, std::vector<const Lk*>> at;  // the box the label goes on -> its links
    for (const auto& l : lks) {
      if (l.tile == point) at[l.box].push_back(&l), shown.push_back(&l);
      else if (l.box == point) at[l.tile].push_back(&l), shown.push_back(&l);
    }
    if (!shown.empty()) {
      for (const auto& n : gr.nodes) looks[n.id].dim = !isAdd(n.id);
      looks[point].dim = false, looks[point].lit = true;
      for (auto& [id, v] : at) {
        auto& L = looks[id];
        L.dim = false, L.lit = true;
        L.label = amountText(*v[0]);
        if (v.size() > 1) L.label += " +" + std::to_string(v.size() - 1);
        L.labelCol = P.fam[familyOf(v[0]->src)];
        L.labelSlot = v.size() == 1 && u.pSel == point ? v[0]->slot : -1;
      }
    }
  } else if (u.full) {  // Full: each box names what moves it
    std::map<int, std::string> names;
    for (const auto& l : lks)
      if (!isTile(l.box)) names[l.box] += (names[l.box].empty() ? "" : ", ") + plainName(l.k);
    for (auto& [id, s] : names) looks[id].label = s, looks[id].labelCol = P.textDim;
  }
  for (auto& [id, L] : looks) L.sel = u.pSel == id;
  // ---- the sound lines: one per connection, out of a right edge into its own in-port on a left edge;
  // calm at rest, faded while pointing (the ports are drawn over the boxes' edges, after them)
  const float t = static_cast<float>(ImGui::GetTime());
  const bool playing = c.hasLive && c.live.voices > 0;
  int hotWire = -1;
  struct PortMark {
    ImVec2 at;
    bool out;
    float alpha;
  };
  std::vector<PortMark> ports;
  for (const auto& l : gr.lines) {
    const auto& w = gr.wires[static_cast<size_t>(l.edge)];
    const PNode *A = gr.find(w.from), *B = gr.find(w.to);
    if (!A || !B || l.pts.size() < 2) continue;
    std::vector<ImVec2> path;
    for (const auto& q : l.pts) path.push_back(g.p(q.x, q.y));
    float alpha = shown.empty() ? 1.f : 0.25f;
    if (!A->on || !B->on) alpha *= 0.4f;
    if (canvasHot && pathDist(path, mouse) < G.u(4)) hotWire = l.edge;
    const bool hotOne = hotWire == l.edge;
    const float th = w.kind == 1 ? 0.9f + 0.9f * std::clamp(w.weight, 0.f, 1.f) : 1.1f + 0.8f * w.weight;
    drawPath(c.dl, path, theme::withAlpha(hotOne ? P.hot : P.accent, 0.55f * alpha + (hotOne ? 0.45f : 0.f)), std::max(1.f, G.u(th)), 0, G.s);
    if (playing && A->on && B->on && shown.empty()) {  // dots that move with the sound
      const float L = pathLen(path), gap = G.u(46);
      for (float dd = std::fmod((t + static_cast<float>(l.edge) * 0.13f) * G.u(50), gap); dd < L; dd += gap)
        c.dl->AddCircleFilled(pathAt(path, dd / std::max(1.f, L)), std::max(1.f, G.u(1.4f)), theme::withAlpha(P.text, 0.7f * alpha), 6);
    }
    ports.push_back({path.front(), true, alpha}), ports.push_back({path.back(), false, alpha});
  }
  if (hotWire >= 0) {
    c.a.setHint("Alt+click: take this connection away");
    if (ImGui::GetIO().KeyAlt && ImGui::IsMouseClicked(0)) disconnect(c, gr.wires[static_cast<size_t>(hotWire)]);
  }
  // ---- snapping guides while a box moves
  if (u.pMove >= 0) {
    const ImU32 gc = theme::withAlpha(P.accent, 0.35f);
    if (!std::isnan(u.pGuideX)) c.dl->AddLine(ImVec2(G.p(u.pGuideX, 0).x, c0.y), ImVec2(G.p(u.pGuideX, 0).x, c1.y), gc, 1);
    if (!std::isnan(u.pGuideY)) c.dl->AddLine(ImVec2(c0.x, G.p(0, u.pGuideY).y), ImVec2(c1.x, G.p(0, u.pGuideY).y), gc, 1);
  }
  // ---- the boxes, the tiles, the add tiles
  for (const auto& n : gr.nodes) {
    BoxLook L = looks.count(n.id) ? looks[n.id] : BoxLook{};
    L.sel = u.pSel == n.id;
    if (linkDrag && !isTile(n.id) && !isAdd(n.id) && ImGui::IsMouseHoveringRect(g.p(n.x, n.y), g.p(n.x + n.w, n.y + n.h), false)) L.drop = true;
    if (isAdd(n.id)) drawAdd(g, n);
    else if (isTile(n.id)) drawTile(g, gr, n, L, tileLinks.count(n.id) ? tileLinks[n.id] : 0);
    else drawBox(g, gr, n, L);
  }
  // the sound ports, over the boxes' edges: a dot where a line leaves, a ring where it arrives
  for (const auto& pm : ports) {
    const ImU32 col = theme::withAlpha(P.accent, 0.85f * pm.alpha);
    if (pm.out) c.dl->AddCircleFilled(pm.at, std::max(1.5f, G.u(3)), col, 12);
    else {
      c.dl->AddCircleFilled(pm.at, std::max(1.5f, G.u(3)), theme::mix(P.frame, P.well, 0.5f), 12);
      c.dl->AddCircle(pm.at, std::max(1.5f, G.u(3)), col, 12, std::max(1.f, G.u(1.2f)));
    }
  }
  // ---- the links being pointed at: one curve per tile and box, from the tile's top port to the
  // box's bottom edge, in the source's colour, over the faded rest
  gEnds.clear();
  const std::vector<patch::Line> mlines = shown.empty() ? std::vector<patch::Line>{} : patch::modLines(gr.P, c.st.params, point);
  for (const auto& ml : mlines) {
    if (ml.slots.empty() || ml.pts.size() < 2) continue;
    const int slot0 = ml.slots[0], src = vali(c, modP(slot0, M_Source));
    bool byp = true;
    for (int s2 : ml.slots) byp = byp && val(c, modP(s2, M_Bypass)) >= 0.5;
    const ImU32 col = byp ? P.textFaint : P.fam[familyOf(src)];
    bool endDrag = false;
    for (int s2 : ml.slots) endDrag = endDrag || u.pEndDrag == s2;
    std::vector<ImVec2> path;
    if (endDrag) {  // its end follows the pointer: let go on the canvas to take the link away
      const ImVec2 a = g.p(ml.a.x, ml.a.y), m = mouse;
      const float k = std::max(G.u(24), std::fabs(m.y - a.y) * 0.5f);
      for (int i = 0; i <= 24; ++i) {
        const float tt = static_cast<float>(i) / 24.f, uu = 1 - tt;
        const ImVec2 p1(a.x, a.y - k), p2(m.x, m.y + k);
        path.push_back(ImVec2(uu * uu * uu * a.x + 3 * uu * uu * tt * p1.x + 3 * uu * tt * tt * p2.x + tt * tt * tt * m.x,
                              uu * uu * uu * a.y + 3 * uu * uu * tt * p1.y + 3 * uu * tt * tt * p2.y + tt * tt * tt * m.y));
      }
    } else {
      for (const auto& q : ml.pts) path.push_back(g.p(q.x, q.y));
    }
    drawPath(c.dl, path, theme::withAlpha(col, endDrag ? 0.6f : 0.95f), std::max(1.2f, G.u(1.7f)), endDrag ? 2 : 0, G.s);
    if (!byp && !endDrag) {  // a dot that runs from the source to what it moves
      const float L = pathLen(path), gap = G.u(64);
      for (float dd = std::fmod((t + static_cast<float>(slot0) * 0.3f) * G.u(40), gap); dd < L; dd += gap) glowDot(g, pathAt(path, dd / std::max(1.f, L)), 2.f, col);
    }
    const ImVec2 sPort = path.front(), e = g.p(ml.b.x, ml.b.y);
    c.dl->AddCircleFilled(sPort, G.u(2.5f), col, 12);
    for (int s2 : ml.slots) gEnds[s2] = e;
    if (endDrag) continue;
    if (u.pSel == ml.from && ml.slots.size() == 1) {  // the selected tile's link ends: drag one off its box to unlink
      ImGui::PushID(slot0 + 47000);
      ImGui::SetCursorScreenPos(ImVec2(e.x - G.u(7), e.y - G.u(7)));
      ImGui::InvisibleButton("##end", ImVec2(G.u(14), G.u(14)));
      const bool hv = ImGui::IsItemHovered();
      if (hv) c.a.setHint("Drag this end off the box to take the link away");
      if (ImGui::IsItemActive() && ImGui::IsMouseDragging(0, 3)) u.pEndDrag = slot0;
      c.dl->AddCircleFilled(e, G.u(hv ? 4.5f : 3.5f), col, 12);
      ImGui::PopID();
    } else {
      c.dl->AddCircleFilled(e, G.u(3), col, 12);
    }
  }
  if (u.pEndDrag >= 0 && !ImGui::IsMouseDown(0)) {  // let go: off every box takes the link away
    bool onBox = false;
    for (const auto& n : gr.nodes)
      if (ImGui::IsMouseHoveringRect(g.p(n.x, n.y), g.p(n.x + n.w, n.y + n.h), false)) onBox = true;
    if (!onBox && ImGui::IsMouseHoveringRect(c0, c1, false)) removeLink(c, u.pEndDrag);
    u.pEndDrag = -1;
  }
  // ---- the sound pins you drag: a source's or a filter's sound out (shown on the box under the pointer)
  for (const auto& n : gr.nodes) {
    const bool soundOut = (n.id >= NSrc && n.id < NSrc + 5) || n.id == NFilt || n.id == NFilt + 1 || n.id == NSendA;
    if (!soundOut) continue;
    const ImVec2 o = g.p(n.x + n.w, n.y + n.h * 0.5f);
    ImGui::PushID(n.id + 46000);
    ImGui::SetCursorScreenPos(ImVec2(o.x - G.u(7), o.y - G.u(7)));
    ImGui::InvisibleButton("##pin", ImVec2(G.u(14), G.u(14)));
    const bool hv = ImGui::IsItemHovered();
    if (ImGui::IsItemActivated()) u.pDragNode = n.id;
    const bool near = ImGui::IsMouseHoveringRect(g.p(n.x, n.y), g.p(n.x + n.w + 8, n.y + n.h), false) && u.pMove < 0 && !linkDrag;
    if (hv) c.a.setHint(boxName(c, n.id) + ": drag from here to where its sound should go");
    if (hv || near || u.pDragNode == n.id) {
      c.dl->AddCircleFilled(o, G.u(4.5f), hv ? P.hot : theme::mix(P.frame, P.well, 0.5f), 12);
      c.dl->AddCircle(o, G.u(4.5f), hv ? P.hot : theme::withAlpha(P.accent, 0.9f), 12, std::max(1.f, G.u(1.5f)));
    }
    ImGui::PopID();
  }
  // a movement link being dragged: a dotted line from its tile's dot to the pointer
  if (linkDrag && u.pDragPin >= 0) {
    if (const PNode* A = gr.find(u.pDragPin)) {
      const ImVec2 a = g.p(A->x + A->w * 0.5f, A->y);
      std::vector<ImVec2> cable;
      const float k = std::max(G.u(24), std::fabs(mouse.y - a.y) * 0.5f);
      for (int i = 0; i <= 24; ++i) {
        const float tt = static_cast<float>(i) / 24.f, uu = 1 - tt;
        cable.push_back(ImVec2(uu * uu * uu * a.x + 3 * uu * uu * tt * a.x + 3 * uu * tt * tt * mouse.x + tt * tt * tt * mouse.x,
                               uu * uu * uu * a.y + 3 * uu * uu * tt * (a.y - k) + 3 * uu * tt * tt * (mouse.y + k) + tt * tt * tt * mouse.y));
      }
      drawPath(c.dl, cable, nodeColour(c, u.pDragPin), std::max(1.2f, G.u(1.8f)), 2, G.s);
    }
  } else if (!linkDrag) {
    u.pDragPin = -1;
  }
  // a sound line being dragged out of a box
  if (u.pDragNode >= 0) {
    PNode* A = gr.find(u.pDragNode);
    if (A && ImGui::IsMouseDown(0)) {
      const ImVec2 a = g.p(A->x + A->w, A->y + A->h * 0.5f);
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
        if (!isAdd(n.id) && ImGui::IsMouseHoveringRect(g.p(n.x, n.y), g.p(n.x + n.w, n.y + n.h), false)) target = n.id;
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
  {  // an empty place on the canvas (dev: where a link end is let go)
    gFreeSpot = ImVec2(-1, -1);
    for (float yy = c1.y - c.s(M(30)); yy > c0.y + c.s(M(30)) && gFreeSpot.x < 0; yy -= c.s(M(24)))
      for (float xx = c0.x + c.s(M(30)); xx < c1.x - c.s(M(30)) && gFreeSpot.x < 0; xx += c.s(M(24))) {
        bool free = true;
        for (const auto& n : gr.nodes)
          if (xx > g.p(n.x, 0).x - 24 && xx < g.p(n.x + n.w, 0).x + 24 && yy > g.p(0, n.y).y - 24 && yy < g.p(0, n.y + n.h).y + 24) free = false;
        if (free) gFreeSpot = ImVec2(xx, yy);
      }
  }
  if (u.devPatch == 6 && u.pFrames >= 4 && u.pFrames <= 6) {  // dev: both sends in use (a room on A, a chorus on B), Layer 1 and Root sending
    if (u.pFrames == 4) setv(c, routeP(RtA, R_Bus1), 0.4), setv(c, routeP(RtSub, R_Bus2), 0.3), fxAdd(c, FxReverb, 1);
    if (u.pFrames == 5) fxAdd(c, FxChorus, 2);
    if (u.pFrames == 6) fxAdd(c, FxEq, 2), u.devPatch = 5, u.pzoom = -1;
  }
  if (u.devPatch == 1 && u.pFrames >= 6) {  // dev: the first link selected
    for (int s = 0; s < kModSlots && u.pLink < 0; ++s)
      if (vali(c, modP(s, M_Source)) > SrcNone && vali(c, modP(s, M_Dest)) > 0) u.pLink = s;
    u.devPatch = 0;
  }
  if (u.devPatch == 2 && u.pFrames >= 6) {  // dev: the search menu, as after a line from Layer 1 is let go on the canvas
    u.pAddFrom = NSrc, u.devPatch = 0;
    if (PNode* A = gr.find(NSrc)) addMenuAt(g, *A, g.p(A->x + A->w, A->y + A->h * 0.5f), c0, c1);
    ImGui::OpenPopup("##patchadd");
  }
  if (u.devPatch == 4 && u.pFrames >= 6) {  // dev: the full link list, as after LFO 1 is let go on the canvas
    u.devPatch = 0;
    if (const PNode* A = gr.find(NAmp)) linkAsk() = {SrcLfo1, -1, g.p(A->x, A->y), true};
    ImGui::OpenPopup("##patchlink");
  }
  if (gOpenLink) ImGui::OpenPopup("##patchlink"), gOpenLink = false;
  if (gOpenAdd) ImGui::OpenPopup("##addtile"), gOpenAdd = false;
  ImGui::SetNextWindowPos(gAddMenuPos, ImGuiCond_Appearing);
  ImGui::SetNextWindowSize(ImVec2(c.s(kAddMenuW), 0), ImGuiCond_Appearing);
  ImGui::SetNextWindowSizeConstraints(ImVec2(c.s(kAddMenuW), 0), ImVec2(c.s(kAddMenuW), c.s(620)));
  if (ImGui::BeginPopup("##patchadd")) {
    addMenuAfterDrop(c, u.pAddFrom);
    ImGui::EndPopup();
  }
  {
    const LinkAsk& ask = linkAsk();
    const float w = c.s(M(ask.box >= 0 && !ask.all ? 280 : 330));
    ImVec2 at(ask.at.x + c.s(M(14)), ask.at.y - c.s(M(24)));
    if (at.x + w > c1.x) at.x = ask.at.x - c.s(M(14)) - w;
    at.x = std::max(c0.x, at.x);
    at.y = std::clamp(at.y, c0.y, std::max(c0.y, c1.y - c.s(M(470))));
    ImGui::SetNextWindowPos(at, ImGuiCond_Appearing);
    ImGui::SetNextWindowSize(ImVec2(w, 0), ImGuiCond_Always);
    ImGui::SetNextWindowSizeConstraints(ImVec2(w, 0), ImVec2(w, c.s(M(470))));
    if (ImGui::BeginPopup("##patchlink")) {
      linkSearch(c, gr);
      ImGui::EndPopup();
    }
  }
  // an add tile: what can be added there
  if (ImGui::BeginPopup("##addtile")) {
    char buf[48];
    if (u.pAddTile == NAddSrc || u.pAddTile == NAddFilt) {
      const int from = u.pAddTile == NAddSrc ? NSrc : NFilt, n = u.pAddTile == NAddSrc ? 5 : 2;
      for (int i = 0; i < n; ++i)
        if (!gr.find(from + i) && enableOf(from + i) >= 0 && ImGui::MenuItem(i18n::tr(nodeTitle(from + i, buf, sizeof buf)))) setv(c, enableOf(from + i), 1);
    } else if (u.pAddTile == NAddFx) {
      static const char* const rk[3] = {"To the main chain", "To Send A", "To Send B"};
      for (int r = 0; r < 3; ++r)
        if (ImGui::BeginMenu(i18n::tr(rk[r]))) {
          fxAddMenu(c, r);
          ImGui::EndMenu();
        }
    } else {
      int src = -1;
      if (sourceMenuItems(c, src) && src > SrcNone) shownMoves().insert(src);
    }
    ImGui::EndPopup();
  }
  // right-click the background: add, Clean up, Fit
  if (canvasHot && ImGui::IsMouseClicked(1) && !ImGui::IsAnyItemHovered()) ImGui::OpenPopup("##canvasmenu");
  if (ImGui::BeginPopup("##canvasmenu")) {
    if (ImGui::BeginMenu(i18n::tr("Add a movement source"))) {
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
    ImGui::Separator();
    if (ImGui::MenuItem(i18n::tr("Clean up"))) cleanUpNow(c);
    if (ImGui::MenuItem(i18n::tr("Fit"))) u.pzoom = -1;
    ImGui::EndPopup();
  }
  // Delete: an effect goes, a box with a light turns off; a selected link goes. Escape: nothing selected.
  if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::GetIO().WantTextInput && !ImGui::IsAnyItemActive() &&
      !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) {
    if ((u.pLink >= 0 || u.pSel >= 0) && (ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false))) {  // Backspace: the Mac delete key
      c.a.doc.beginGroup("Delete");  // a key press is not a pointer gesture: keep it one undo step
      if (u.pLink >= 0) removeLink(c, u.pLink);
      else if (u.pSel >= NFx && u.pSel < NFx + kFxSlots) fxRemove(c, u.pSel - NFx), u.pSel = -1;
      else if (u.pSel >= 0 && enableOf(u.pSel) >= 0) setv(c, enableOf(u.pSel), 0);
      c.a.doc.endGroup();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) u.pSel = -1, u.pLink = -1;
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
  // ---- the inspector: only while something is selected
  if (inspect) scrollBody(c, ix - 12, cy0 - M(40), iw + 24, kPlayY - M(16) - drawerH(u), u.pScroll, u.pContent, inspector);
  gGraph = nullptr;
  gPatchUi = nullptr;
}

void fxTypeGlyphPublic(Ctx& c, int type, float x, float y, float w, float h, ImU32 col) {
  fxTypeGlyph(c.dl, type, c.p(x, y), c.p(x + w, y + h), col, c.P.well);
}

}  // namespace gl::packs::plasma::ui
