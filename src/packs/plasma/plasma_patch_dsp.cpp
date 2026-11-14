// Gridline — "plasma" pack: the Patch view's model and layout (see plasma_patch.h). No drawing here:
// the editor draws what this builds, and the tests check it.
#include "plasma_patch.h"
#include <algorithm>
#include <cmath>
#include <functional>
#include <queue>
#include <utility>

namespace gl::packs::plasma::patch {
namespace {

// Box sizes and the default flow (graph units: design pixels at 100 %).
// Gaps between boxes leave room for a line (24 between rows, 40 between effects for a return to come up).
constexpr float kSrcW = 168, kH = 72, kStep = 96, kShapeX = 280, kColStep = 232, kAmpX = 528, kAmpW = 128, kFxGap = 88, kFxW = 104, kFxStep = 144;
constexpr float kOutW = 104, kSendW = 160, kTileW = 192, kTileH = 40, kTileStepX = 216, kTileStepY = 64, kAddH = 40;
constexpr int kTilesPerRow = 6;

int vali(const std::vector<float>& p, int k) { return k >= 0 && k < static_cast<int>(p.size()) ? static_cast<int>(std::lround(p[static_cast<size_t>(k)])) : 0; }
float valf(const std::vector<float>& p, int k) { return k >= 0 && k < static_cast<int>(p.size()) ? p[static_cast<size_t>(k)] : 0.f; }
float snap(float v) { return std::round(v / kGrid) * kGrid; }
int enableParam(int id) {
  if (id >= NSrc && id < NSrc + 3) return oscP(id - NSrc, O_Enable);
  if (id == NSrc + 3) return subP(S_Enable);
  if (id == NSrc + 4) return noiseP(N_Enable);
  if (id == NFilt || id == NFilt + 1) return filtP(id - NFilt, F_Enable);
  return -1;
}
bool overlap(const Box& a, const Box& b, float gap) {
  return a.x < b.x + b.w + gap && b.x < a.x + a.w + gap && a.y < b.y + b.h + gap && b.y < a.y + a.h + gap;
}
// Places every box with the given order per column.
void layoutWith(Patch& p, const std::map<int, std::vector<int>>& order) {
  auto list = [&](int col) {
    auto it = order.find(col);
    return it == order.end() ? std::vector<int>{} : it->second;
  };
  auto at = [&](int id, float x, float y) {
    if (Box* b = p.find(id)) b->x = snap(x), b->y = snap(y);
  };
  auto feeds = [&](int a, int b) {
    for (const auto& e : p.edges)
      if (e.from == a && e.to == b) return true;
    return false;
  };
  auto src = list(CSrc), shape = list(CShape), fx = list(CFx), sends = list(CSend), moves = list(CMove);
  // a filter fed by the other filter goes one column to the right (its line runs forward)
  int serial = -1;
  for (int f = 0; f < 2; ++f)
    if (feeds(NFilt + 1 - f, NFilt + f) && std::find(shape.begin(), shape.end(), NFilt + f) != shape.end() &&
        std::find(shape.begin(), shape.end(), NFilt + 1 - f) != shape.end())
      serial = NFilt + f;
  if (serial >= 0) shape.erase(std::find(shape.begin(), shape.end(), serial));
  const float shift = serial >= 0 ? kColStep : 0.f, ampX = kAmpX + shift, fxX = ampX + kAmpW + kFxGap;
  const int n = static_cast<int>(src.size());
  for (int i = 0; i < n; ++i) at(src[static_cast<size_t>(i)], 0, kStep * static_cast<float>(i));
  const float span = std::max(1, n) * kStep - (kStep - kH);
  float bottom = n > 0 ? span : 0;
  if (p.find(NAddSrc)) at(NAddSrc, 0, n * kStep), bottom = std::max(bottom, n * kStep + kAddH);
  const int k = static_cast<int>(shape.size());
  float prev = -1e9f;
  for (int i = 0; i < k; ++i) {
    float y = snap(span * (static_cast<float>(i) + 0.5f) / static_cast<float>(k) - kH * 0.5f);
    y = std::max({y, prev + kStep, 0.f});
    at(shape[static_cast<size_t>(i)], kShapeX, y);
    prev = y;
    bottom = std::max(bottom, y + kH);
  }
  if (serial >= 0)
    if (const Box* f = p.find(2 * NFilt + 1 - serial)) at(serial, kShapeX + kColStep, f->y);
  if (p.find(NAddFilt)) {
    const float y = k > 0 ? prev + kStep : snap(span * 0.5f - kAddH * 0.5f);
    at(NAddFilt, kShapeX, y), bottom = std::max(bottom, y + kAddH);
  }
  const float ampY = std::max(0.f, snap(span * 0.5f - kH * 0.5f));
  at(NAmp, ampX, ampY);
  for (size_t i = 0; i < fx.size(); ++i) at(fx[i], fxX + kFxStep * static_cast<float>(i), ampY);
  const float outX = fxX + kFxStep * static_cast<float>(fx.size());
  at(NOut, outX, ampY);
  bottom = std::max(bottom, ampY + kH);
  // the sends under the Amp (their taps come from the left, their returns go right); a send fed by
  // the other send sits to its right
  const float below = ampY + kH + 48;
  float sy = below;
  for (size_t i = 0; i < sends.size(); ++i) {
    const int id = sends[i], other = id == NSendA ? NSendB : NSendA;
    if (feeds(other, id) && std::find(sends.begin(), sends.end(), other) != sends.end()) continue;  // placed beside its feeder
    at(id, ampX, sy);
    if (feeds(id, other)) at(other, ampX + kSendW + 56, sy);
    bottom = std::max(bottom, sy + kH);
    sy += kStep;
  }
  if (p.find(NAddFx)) at(NAddFx, outX, below), bottom = std::max(bottom, below + kAddH);
  const float ty = snap(bottom + 64);
  size_t i = 0;
  for (; i < moves.size(); ++i)
    at(moves[i], kTileStepX * static_cast<float>(i % kTilesPerRow), ty + kTileStepY * static_cast<float>(i / kTilesPerRow));
  if (p.find(NAddMove)) at(NAddMove, kTileStepX * static_cast<float>(i % kTilesPerRow), ty + kTileStepY * static_cast<float>(i / kTilesPerRow));
}

// The canonical order of each column.
std::map<int, std::vector<int>> defaultOrder(const Patch& p) {
  std::map<int, std::vector<int>> o;
  for (const auto& b : p.boxes)
    if (b.col != CAdd && b.col != CFx) o[b.col].push_back(b.id);
  for (auto& [col, v] : o) {
    if (col == CMove)
      std::stable_sort(v.begin(), v.end(), [](int a, int b) {
        return std::make_pair(familyRank(a - NMove), a) < std::make_pair(familyRank(b - NMove), b);
      });
    else std::sort(v.begin(), v.end());
  }
  for (int s : p.chain[0]) o[CFx].push_back(NFx + s);
  return o;
}

}  // namespace

Box* Patch::find(int id) {
  for (auto& b : boxes)
    if (b.id == id) return &b;
  return nullptr;
}
const Box* Patch::find(int id) const {
  for (const auto& b : boxes)
    if (b.id == id) return &b;
  return nullptr;
}

int routeOfBox(int id) {
  if (id >= NSrc && id < NSrc + 3) return id - NSrc;
  if (id == NSrc + 3) return RtSub;
  if (id == NSrc + 4) return RtNoise;
  if (id == NFilt) return RtF1;
  if (id == NFilt + 1) return RtF2;
  return -1;
}
int boxOfRoute(int r) {
  if (r <= RtC) return NSrc + r;
  if (r == RtSub) return NSrc + 3;
  if (r == RtNoise) return NSrc + 4;
  return r == RtF1 ? NFilt : NFilt + 1;
}
int moveTileOf(int src) {
  if (src >= SrcLfo1Y && src <= SrcLfo10Y) return NMove + SrcLfo1 + (src - SrcLfo1Y);
  return NMove + src;
}
int familyRank(int src) {
  if (src >= SrcLfo1 && src <= SrcLfo10Y) return 0;
  if (src >= SrcEnv1 && src <= SrcEnv4) return 1;
  if (src >= SrcMacro1 && src <= SrcMacro8) return 2;
  switch (src) {
    case SrcVelo: case SrcNote: case SrcReleaseVelo: case SrcAftertouch: case SrcPolyAT: case SrcModWheel: case SrcPitchBend:
    case SrcExprX: case SrcExprY: case SrcExprZ: return 3;
    case SrcNoise: case SrcOscA: case SrcOscB: case SrcOscC: case SrcSub: case SrcFilter1: case SrcFilter2: return 5;
    default: return 4;
  }
}
bool movable(int id) { return id < NAddSrc; }
bool isLink(const std::vector<float>& params, int s) { return vali(params, modP(s, M_Source)) > SrcNone && vali(params, modP(s, M_Dest)) > 0; }

int ownerBox(const Patch& p, int k) {
  if (k >= kOscBase && k < kSubBase) return NSrc + (k - kOscBase) / O_Count;
  if (k >= kSubBase && k < kNoiseBase) return NSrc + 3;
  if (k >= kNoiseBase && k < kFiltBase) return NSrc + 4;
  if (k >= kFiltBase && k < kRouteBase) return NFilt + (k - kFiltBase) / F_Count;
  if (k >= kRouteBase && k < kEnvBase) return boxOfRoute((k - kRouteBase) / R_Count);
  if (k >= kEnvBase && k < kLfoBase) {
    const int e = (k - kEnvBase) / E_Count;
    return e == 0 ? NAmp : NMove + SrcEnv1 + e;
  }
  if (k >= kLfoBase && k < kMacroBase) return NMove + SrcLfo1 + (k - kLfoBase) / L_Count;
  if (k >= kMacroBase && k < kMacroBase + kMacros) return NMove + SrcMacro1 + (k - kMacroBase);
  if (k >= kFxBase && k < kTailBase) {
    const int s = (k - kFxBase) / X_Count;
    auto it = p.fxBox.find(s);
    return it == p.fxBox.end() ? NFx + s : it->second;
  }
  if (k == G_Amp) return NAmp;
  if (k == G_MasterVol || k == G_DirectVol) return NOut;
  if (k == G_Bus1Vol || k == G_Bus1Dest || k == tailP(T_BusChain)) return NSendA;
  if (k == G_Bus2Vol || k == G_Bus2Dest) return NSendB;
  if (k == tailP(T_NoiseTone)) return NSrc + 4;
  if (k == tailP(T_Pad1)) return NFilt;
  if (k == tailP(T_Pad2)) return NFilt + 1;
  return NVoices;
}

Patch buildPatch(const std::vector<float>& params, const Model& m, const std::set<int>& keep) {
  Patch p;
  const auto fx = resolvedFx(m, params);
  for (const auto& n : fx)
    if (n.parent < 0 && vali(params, fxP(n.slot, X_Type)) != FxNone) p.chain[n.rack].push_back(n.slot);
  for (const auto& n : fx) {  // every slot, a split's bands too, shows on its top box
    int top = n.slot;
    for (int guard = 0; guard < kFxSlots; ++guard) {
      int up = -1;
      for (const auto& q : fx)
        if (q.slot == top) up = q.parent;
      if (up < 0) break;
      top = up;
    }
    p.fxBox[n.slot] = n.rack == 0 ? NFx + top : n.rack == 1 ? NSendA : NSendB;
  }
  // what the links move, and from where
  std::set<int> linked, tiles;
  for (int s = 0; s < kModSlots; ++s) {
    if (!isLink(params, s)) continue;
    const int src = vali(params, modP(s, M_Source)), dest = vali(params, modP(s, M_Dest)) - 1;
    tiles.insert(moveTileOf(src));
    const int o = ownerBox(p, dest);
    linked.insert(o);
    if (o >= NMove) tiles.insert(o);
  }
  for (int k : keep)
    if (k >= NMove && k < NMove + SourceCount) tiles.insert(moveTileOf(k - NMove));
  auto add = [&](int id, int col, float w, float h, bool on = true) {
    Box b;
    b.id = id, b.col = col, b.w = w, b.h = h, b.on = on;
    p.boxes.push_back(b);
  };
  bool srcHidden = false, filtHidden = false;
  for (int i = 0; i < 5; ++i) {
    const int id = NSrc + i;
    const bool on = valf(params, enableParam(id)) >= 0.5f;
    if (on || linked.count(id) || keep.count(id)) add(id, CSrc, kSrcW, kH, on);
    else srcHidden = true;
  }
  if (linked.count(NVoices)) add(NVoices, CSrc, kSrcW, kH);
  for (int f = 0; f < 2; ++f) {
    const int id = NFilt + f;
    const bool on = valf(params, enableParam(id)) >= 0.5f;
    if (on || linked.count(id) || keep.count(id)) add(id, CShape, kSrcW, kH, on);
    else filtHidden = true;
  }
  add(NAmp, CAmp, kAmpW, kH);
  for (int s : p.chain[0]) add(NFx + s, CFx, kFxW, kH, valf(params, fxP(s, X_On)) >= 0.5f);
  add(NOut, COut, kOutW, kH);
  // the sends: shown while something goes into them or they hold effects
  const int chainMode = vali(params, tailP(T_BusChain));
  bool fedA = false, fedB = false;
  for (int r = 0; r < kRoutes; ++r) fedA = fedA || valf(params, routeP(r, R_Bus1)) > 0.001f, fedB = fedB || valf(params, routeP(r, R_Bus2)) > 0.001f;
  const bool showA = fedA || !p.chain[1].empty() || linked.count(NSendA) || keep.count(NSendA) || (chainMode >= 2 && fedB);
  const bool showB = fedB || !p.chain[2].empty() || linked.count(NSendB) || keep.count(NSendB) || (chainMode == 1 && fedA);
  if (showA) add(NSendA, CSend, kSendW, kH);
  if (showB) add(NSendB, CSend, kSendW, kH);
  for (int t : tiles) add(t, CMove, kTileW, kTileH);
  if (srcHidden) add(NAddSrc, CAdd, kSrcW, kAddH);
  if (filtHidden) add(NAddFilt, CAdd, kSrcW, kAddH);
  add(NAddFx, CAdd, kFxW, kAddH);
  add(NAddMove, CAdd, kTileW, kTileH);
  // the sound lines, as the engine plays them
  auto wire = [&](int from, int to, int kind, float wt, int r, int dest) {
    if (p.find(from) && p.find(to)) p.edges.push_back({from, to, kind, wt, r, dest});
  };
  for (int r = 0; r < kRoutes; ++r) {
    const int from = boxOfRoute(r);
    const int d = vali(params, routeP(r, R_Dest));
    const float b2 = (valf(params, routeP(r, R_Balance)) + 1.f) * 0.5f;
    const bool filterBox = r == RtF1 || r == RtF2;
    if (d == RouteFilter) {
      if (filterBox) wire(from, r == RtF1 ? NFilt + 1 : NFilt, 0, 1.f, r, r == RtF1 ? DF2 : DF1);
      else {
        if (b2 < 0.995f) wire(from, NFilt, 0, 1.f - b2, r, DF1);
        if (b2 > 0.005f) wire(from, NFilt + 1, 0, b2, r, DF2);
      }
    } else if (d == RouteMain) {
      wire(from, NAmp, 0, 1.f, r, DAmp);
    } else if (d == RouteDirect) {
      wire(from, NOut, 0, 1.f, r, DOut);
    }
    if (valf(params, routeP(r, R_Bus1)) > 0.001f) wire(from, NSendA, 1, valf(params, routeP(r, R_Bus1)), r, DSendA);
    if (valf(params, routeP(r, R_Bus2)) > 0.001f) wire(from, NSendB, 1, valf(params, routeP(r, R_Bus2)), r, DSendB);
  }
  p.mainHead = p.chain[0].empty() ? NOut : NFx + p.chain[0][0];
  wire(NAmp, p.mainHead, 0, 1.f, -1, -1);
  for (size_t k = 0; k + 1 < p.chain[0].size(); ++k) wire(NFx + p.chain[0][k], NFx + p.chain[0][k + 1], 3, 1.f, -1, -1);
  if (!p.chain[0].empty()) wire(NFx + p.chain[0].back(), NOut, 0, 1.f, -1, -1);
  // the sends' returns, as the engine mixes them: into the main chain or straight out; a chained
  // send feeds the other one instead (both ways: Send B feeds Send A and neither returns)
  const bool getsA = fedA || (chainMode >= 2 && fedB), getsB = fedB || (chainMode == 1 && fedA);
  if (chainMode == 1) wire(NSendA, NSendB, 3, 1.f, -1, DChain);
  if (chainMode >= 2) wire(NSendB, NSendA, 3, 1.f, -1, DChain);
  if ((chainMode == 0 || chainMode == 2) && getsA) wire(NSendA, vali(params, G_Bus1Dest) == 0 ? p.mainHead : NOut, 0, 1.f, -1, -1);
  if (chainMode <= 1 && getsB) wire(NSendB, vali(params, G_Bus2Dest) == 0 ? p.mainHead : NOut, 0, 1.f, -1, -1);
  return p;
}

void defaultLayout(Patch& p) { layoutWith(p, defaultOrder(p)); }

void place(Patch& p, const std::map<int, std::array<float, 2>>& saved) {
  defaultLayout(p);
  std::vector<Box*> done, rest;
  for (auto& b : p.boxes) {
    auto it = movable(b.id) ? saved.find(b.id) : saved.end();
    if (it != saved.end()) b.x = it->second[0], b.y = it->second[1], done.push_back(&b);
    else rest.push_back(&b);
  }
  if (done.empty()) return;
  // a box without a saved place keeps its default one when that is free, else the nearest free
  // place below it (then to its right): next to what it connects to, never on a box
  for (Box* b : rest) {
    const float x0 = b->x, y0 = b->y;
    auto clear = [&] {
      for (const Box* d : done)
        if (overlap(*b, *d, kGrid)) return false;
      return true;
    };
    for (int t = 0; t < 400 && !clear(); ++t) {
      const int ring = t / 2 + 1;
      if (t % 2 == 0) b->x = x0, b->y = y0 + kGrid * 2 * ring;
      else b->x = x0 + kGrid * 2 * ring, b->y = y0;
    }
    done.push_back(b);
  }
}

bool cleanUp(Patch& p) {
  // the user's order: each column top to bottom (the movement tiles by family, then in reading
  // order); the main chain keeps the engine's order (it is the sound)
  auto o = defaultOrder(p);
  auto cy = [&](int id) {
    const Box* b = p.find(id);
    return b ? b->y + b->h * 0.5f : 0.f;
  };
  auto cx = [&](int id) {
    const Box* b = p.find(id);
    return b ? b->x + b->w * 0.5f : 0.f;
  };
  for (auto& [col, v] : o) {
    if (col == CFx) continue;
    if (col == CMove)
      std::stable_sort(v.begin(), v.end(), [&](int a, int b) {
        const int fa = familyRank(a - NMove), fb = familyRank(b - NMove);
        if (fa != fb) return fa < fb;
        const float ya = std::round(cy(a) / kTileStepY), yb = std::round(cy(b) / kTileStepY);
        return ya != yb ? ya < yb : cx(a) < cx(b);
      });
    else
      std::stable_sort(v.begin(), v.end(), [&](int a, int b) { return cy(a) != cy(b) ? cy(a) < cy(b) : cx(a) < cx(b); });
  }
  Patch user = p, def = p;
  layoutWith(user, o);
  defaultLayout(def);
  const Score su = score(user), sd = score(def);
  const bool keepUser = su.overlaps == 0 && su.through == 0 && su.crossings <= sd.crossings && su.total() <= sd.total();
  p = keepUser ? user : def;
  return keepUser;
}

std::map<int, std::array<float, 2>> positions(const Patch& p) {
  std::map<int, std::array<float, 2>> v;
  for (const auto& b : p.boxes)
    if (movable(b.id)) v[b.id] = {b.x, b.y};
  return v;
}

// ---- the lines: ports, then the shortest clear path between them

namespace {

Pt operator+(Pt a, Pt b) { return {a.x + b.x, a.y + b.y}; }
Pt operator-(Pt a, Pt b) { return {a.x - b.x, a.y - b.y}; }
Pt operator*(Pt a, float k) { return {a.x * k, a.y * k}; }
float dist(Pt a, Pt b) { return std::hypot(a.x - b.x, a.y - b.y); }
float pathLength(const std::vector<Pt>& v) {
  float L = 0;
  for (size_t i = 1; i < v.size(); ++i) L += dist(v[i - 1], v[i]);
  return L;
}
// Points every `step` units along a path (its corners kept).
std::vector<Pt> dense(const std::vector<Pt>& v, float step) {
  std::vector<Pt> o;
  if (v.empty()) return o;
  o.push_back(v[0]);
  for (size_t i = 1; i < v.size(); ++i) {
    const float d = dist(v[i - 1], v[i]);
    const int n = std::max(1, static_cast<int>(std::ceil(d / step)));
    for (int k = 1; k <= n; ++k) o.push_back(v[i - 1] + (v[i] - v[i - 1]) * (static_cast<float>(k) / static_cast<float>(n)));
  }
  return o;
}

bool segCross(Pt a, Pt b, Pt c, Pt d) {
  const float d1 = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x), d2 = (b.x - a.x) * (d.y - a.y) - (b.y - a.y) * (d.x - a.x);
  const float d3 = (d.x - c.x) * (a.y - c.y) - (d.y - c.y) * (a.x - c.x), d4 = (d.x - c.x) * (b.y - c.y) - (d.y - c.y) * (b.x - c.x);
  return ((d1 > 0) != (d2 > 0)) && ((d3 > 0) != (d4 > 0)) && d1 != 0 && d2 != 0 && d3 != 0 && d4 != 0;
}
int crossings(const std::vector<std::vector<Pt>>& paths) {
  int n = 0;
  for (size_t i = 0; i < paths.size(); ++i)
    for (size_t j = i + 1; j < paths.size(); ++j)
      for (size_t a = 1; a < paths[i].size(); ++a)
        for (size_t b = 1; b < paths[j].size(); ++b) n += segCross(paths[i][a - 1], paths[i][a], paths[j][b - 1], paths[j][b]);
  return n;
}

// Finds paths round the boxes of one patch.
class Router {
 public:
  explicit Router(const Patch& p) : p_(p) {
    lineBounds(p, x0_, y0_, x1_, y1_);
  }
  // From port a (leaving along na) to port b (arriving against nb, its outward normal).
  std::vector<Pt> path(Pt a, Pt na, Pt b, Pt nb) {
    const float d = dist(a, b);
    const float k = std::clamp(d * 0.45f, 24.f, 160.f);
    std::vector<Pt> curve = bezier(a, a + na * k, b + nb * k, b);
    if (clear(curve, 3)) return curve;
    const float k2 = std::clamp(d * 0.25f, 16.f, 64.f);  // a tighter curve
    std::vector<Pt> c2 = bezier(a, a + na * k2, b + nb * k2, b);
    if (clear(c2, 3)) return c2;
    return square(a, na, b, nb, curve);
  }
  // A path through the gaps with rounded corners (the look of the audio lanes), off the lanes other
  // lines already use; `fallback` when there is no way at all.
  std::vector<Pt> square(Pt a, Pt na, Pt b, Pt nb, const std::vector<Pt>& fallback) {
    cur_ = portKey(a);
    std::vector<Pt> v = search(a, na, b, nb);
    if (v.empty()) return fallback;
    std::vector<Pt> out = rounded(v, 0);
    for (float r : {28.f, 14.f, 6.f})
      if (std::vector<Pt> s = rounded(v, r); clear(s, 0.5f)) {
        out = s;
        break;
      }
    take(out);
    return out;
  }
  // A routed line's cells: the next line avoids running along it (each line its own lane), except
  // near the ports, where lines from one port fork.
  void take(const std::vector<Pt>& v) {
    build();
    const auto pts = dense(v, 4);
    const float L = pathLength(pts);
    float at = 0;
    for (size_t i = 0; i < pts.size(); ++i) {
      if (i > 0) at += dist(pts[i - 1], pts[i]);
      if (at < 24 || at > L - 24) continue;
      int c, r;
      cellOf(pts[i], c, r);
      used_[idx(c, r)] = portKey(v.front());
    }
  }
  // No point inside a box (grown by pad) except near the two ends; all inside the bounds.
  bool clear(const std::vector<Pt>& v, float pad) const {
    const auto pts = dense(v, 3);
    const float L = pathLength(pts);
    float at = 0;
    for (size_t i = 0; i < pts.size(); ++i) {
      if (i > 0) at += dist(pts[i - 1], pts[i]);
      const Pt q = pts[i];
      if (q.x < x0_ || q.x > x1_ || q.y < y0_ || q.y > y1_) return false;
      if (at < pad + 4 || at > L - pad - 4) continue;
      for (const auto& b : p_.boxes)
        if (q.x > b.x - pad && q.x < b.x + b.w + pad && q.y > b.y - pad && q.y < b.y + b.h + pad) return false;
    }
    return true;
  }

  static std::vector<Pt> roundedPath(const std::vector<Pt>& v, float r) { return rounded(v, r); }

 private:
  static constexpr float kCell = 8, kInflate = 6, kStub = 16;
  const Patch& p_;
  float x0_, y0_, x1_, y1_;
  int cols_ = 0, rows_ = 0;
  std::vector<unsigned char> blocked_;
  std::vector<int> near_;  // cells to the nearest blocked cell (capped)
  std::vector<int> used_;  // cells a line already runs through: its start port's key (0 none)
  int cur_ = 0;            // the start port of the line being routed
  static int portKey(Pt a) {
    const unsigned h = (static_cast<unsigned>(std::lround(a.x * 4)) * 73856093u) ^ (static_cast<unsigned>(std::lround(a.y * 4)) * 19349663u);
    return static_cast<int>((h & 0x3fffffffu) | 1u);
  }

  static std::vector<Pt> bezier(Pt p0, Pt p1, Pt p2, Pt p3) {
    const float L = dist(p0, p1) + dist(p1, p2) + dist(p2, p3);
    const int n = std::max(12, static_cast<int>(L / 6));
    std::vector<Pt> v;
    for (int i = 0; i <= n; ++i) {
      const float t = static_cast<float>(i) / static_cast<float>(n), u = 1 - t;
      v.push_back(p0 * (u * u * u) + p1 * (3 * u * u * t) + p2 * (3 * u * t * t) + p3 * (t * t * t));
    }
    v.front() = p0, v.back() = p3;
    return v;
  }
  // Rounds every corner of a square path (radius r, never more than half a side).
  static std::vector<Pt> rounded(const std::vector<Pt>& v, float r) {
    if (r <= 0 || v.size() < 3) return v;
    std::vector<Pt> o = {v[0]};
    for (size_t i = 1; i + 1 < v.size(); ++i) {
      const Pt prev = o.back(), c = v[i], next = v[i + 1];
      const float d1 = dist(prev, c), d2 = dist(c, next), rr = std::min({r, d1 * 0.5f, d2 * 0.5f});
      if (rr < 0.5f) {
        o.push_back(c);
        continue;
      }
      const Pt a = c + (prev - c) * (rr / d1), b = c + (next - c) * (rr / d2);
      for (int k = 0; k <= 6; ++k) {
        const float t = static_cast<float>(k) / 6.f, u = 1 - t;
        o.push_back(a * (u * u) + c * (2 * u * t) + b * (t * t));
      }
    }
    o.push_back(v.back());
    return o;
  }
  void build() {
    if (cols_) return;
    cols_ = std::max(1, static_cast<int>(std::ceil((x1_ - x0_) / kCell)));
    rows_ = std::max(1, static_cast<int>(std::ceil((y1_ - y0_) / kCell)));
    blocked_.assign(static_cast<size_t>(cols_ * rows_), 0);
    used_.assign(blocked_.size(), 0);
    for (int r = 0; r < rows_; ++r)
      for (int c = 0; c < cols_; ++c) {
        const Pt q = centre(c, r);
        for (const auto& b : p_.boxes) {
          if (q.x > b.x && q.x < b.x + b.w && q.y > b.y && q.y < b.y + b.h) blocked_[idx(c, r)] = 2;  // inside a box
          else if (q.x > b.x - kInflate && q.x < b.x + b.w + kInflate && q.y > b.y - kInflate && q.y < b.y + b.h + kInflate)
            blocked_[idx(c, r)] = std::max<unsigned char>(blocked_[idx(c, r)], 1);  // along its edge
        }
      }
    near_.assign(blocked_.size(), 4);  // breadth first from the blocked cells, up to 4 cells away
    std::vector<int> q;
    for (int i = 0; i < static_cast<int>(blocked_.size()); ++i)
      if (blocked_[static_cast<size_t>(i)]) near_[static_cast<size_t>(i)] = 0, q.push_back(i);
    for (size_t h = 0; h < q.size(); ++h) {
      const int i = q[h], c = i % cols_, r = i / cols_, d = near_[static_cast<size_t>(i)];
      if (d >= 3) continue;
      const int nb[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
      for (const auto& o : nb) {
        const int cc = c + o[0], rr = r + o[1];
        if (cc < 0 || rr < 0 || cc >= cols_ || rr >= rows_) continue;
        const size_t j = idx(cc, rr);
        if (near_[j] > d + 1) near_[j] = d + 1, q.push_back(static_cast<int>(j));
      }
    }
  }
  size_t idx(int c, int r) const { return static_cast<size_t>(r * cols_ + c); }
  Pt centre(int c, int r) const { return {x0_ + (static_cast<float>(c) + 0.5f) * kCell, y0_ + (static_cast<float>(r) + 0.5f) * kCell}; }
  void cellOf(Pt q, int& c, int& r) const {
    c = std::clamp(static_cast<int>((q.x - x0_) / kCell), 0, cols_ - 1), r = std::clamp(static_cast<int>((q.y - y0_) / kCell), 0, rows_ - 1);
  }
  // The free cell just out of a port: a stub's length away, or closer when a box stands nearer than
  // that (its centre always out in front of the port, never inside a box).
  void portCell(Pt a, Pt n, int& c, int& r) const {
    for (float d : {kStub, 12.f, 8.f, 4.f}) {
      cellOf(a + n * d, c, r);
      const Pt q = centre(c, r);
      if ((q.x - a.x) * n.x + (q.y - a.y) * n.y > 0.5f && blocked_[idx(c, r)] != 2) return;
    }
    cellOf(a + n * kStub, c, r);
  }
  static int dirOf(Pt n) { return std::fabs(n.x) > std::fabs(n.y) ? (n.x > 0 ? 0 : 1) : (n.y > 0 ? 2 : 3); }
  // A square path through the free cells: few turns, away from box edges.
  std::vector<Pt> search(Pt a, Pt na, Pt b, Pt nb) {
    build();
    int sc, sr, gc, gr;
    portCell(a, na, sc, sr), portCell(b, nb, gc, gr);
    const int D[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    const int startDir = dirOf(na), endDir = dirOf(nb * -1.f);
    const size_t N = blocked_.size() * 4;
    std::vector<float> cost(N, 1e30f);
    std::vector<int> from(N, -1);
    using Item = std::pair<float, int>;
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> open;
    const int s0 = static_cast<int>(idx(sc, sr)) * 4 + startDir;
    cost[static_cast<size_t>(s0)] = 0, open.push({0.f, s0});
    const int goalCell = static_cast<int>(idx(gc, gr));
    int best = -1;
    while (!open.empty()) {
      const auto [cst, s] = open.top();
      open.pop();
      if (cst > cost[static_cast<size_t>(s)]) continue;
      const int cell = s / 4, dir = s % 4;
      if (cell == goalCell) {
        best = s;
        break;
      }
      const int c = cell % cols_, r = cell / cols_;
      for (int nd = 0; nd < 4; ++nd) {
        if ((nd ^ 1) == dir && nd / 2 == dir / 2) continue;  // never straight back
        const int cc = c + D[nd][0], rr = r + D[nd][1];
        if (cc < 0 || rr < 0 || cc >= cols_ || rr >= rows_) continue;
        const size_t j = idx(cc, rr);
        // a box is never crossed while there is another way: hugely dear inside, dear along its edge
        // (only a port another box covers, on a messy layout, leaves no other way)
        float add = kCell + (nd != dir ? 40.f : 0.f) + static_cast<float>(std::max(0, 3 - near_[j])) * 6.f;
        if (static_cast<int>(j) != goalCell) add += blocked_[j] == 2 ? 5000.f : blocked_[j] == 1 ? 80.f : 0.f;
        if (used_[j] && used_[j] != cur_) add += 30.f;  // its own lane beside another line (lines from one port may share theirs)
        if (static_cast<int>(j) == goalCell && nd != endDir) add += 40.f;
        const int t = static_cast<int>(j) * 4 + nd;
        if (cst + add < cost[static_cast<size_t>(t)]) cost[static_cast<size_t>(t)] = cst + add, from[static_cast<size_t>(t)] = s, open.push({cst + add, t});
      }
    }
    if (best < 0) return {};
    std::vector<Pt> cells;
    for (int s = best; s >= 0; s = from[static_cast<size_t>(s)]) cells.push_back(centre((s / 4) % cols_, (s / 4) / cols_));
    std::reverse(cells.begin(), cells.end());
    // the first run lines up with the start port, the last with the end port
    const bool hA = startDir < 2, hB = endDir < 2;
    for (size_t i = 0; i < cells.size(); ++i) {
      if (hA ? std::fabs(cells[i].y - cells[0].y) > 0.1f : std::fabs(cells[i].x - cells[0].x) > 0.1f) break;
      (hA ? cells[i].y : cells[i].x) = hA ? a.y : a.x;
    }
    const Pt last = cells.back();
    for (size_t i = cells.size(); i-- > 0;) {
      if (hB ? std::fabs(cells[i].y - last.y) > 0.1f : std::fabs(cells[i].x - last.x) > 0.1f) break;
      (hB ? cells[i].y : cells[i].x) = hB ? b.y : b.x;
    }
    std::vector<Pt> v = {a};
    for (const Pt& q : cells) v.push_back(q);
    v.push_back(b);
    // only the corners
    std::vector<Pt> o = {v[0]};
    for (size_t i = 1; i + 1 < v.size(); ++i) {
      const Pt p0 = o.back(), p1 = v[i], p2 = v[i + 1];
      const float cross = (p1.x - p0.x) * (p2.y - p1.y) - (p1.y - p0.y) * (p2.x - p1.x);
      if (std::fabs(cross) > 0.01f && dist(p0, p1) > 0.01f) o.push_back(p1);
    }
    o.push_back(v.back());
    return o;
  }
};

}  // namespace

void lineBounds(const Patch& p, float& x0, float& y0, float& x1, float& y1) {
  x0 = y0 = 1e9f, x1 = y1 = -1e9f;
  for (const auto& b : p.boxes) x0 = std::min(x0, b.x), y0 = std::min(y0, b.y), x1 = std::max(x1, b.x + b.w), y1 = std::max(y1, b.y + b.h);
  if (p.boxes.empty()) x0 = y0 = x1 = y1 = 0;
  x0 -= kLineMargin, y0 -= kLineMargin, x1 += kLineMargin, y1 += kLineMargin;
}

Pt outPort(const Box& b) { return {b.x + b.w, b.y + b.h * 0.5f}; }
Pt modOutPort(const Box& tile) { return {tile.x + tile.w * 0.5f, tile.y}; }

std::vector<Line> soundLines(const Patch& p) {
  std::vector<Line> v;
  Router R(p);
  // each target box: its in-ports spread down its left edge, in the order of their sources
  std::map<int, std::vector<size_t>> into;
  for (size_t i = 0; i < p.edges.size(); ++i)
    if (p.find(p.edges[i].from) && p.find(p.edges[i].to)) into[p.edges[i].to].push_back(i);
  std::map<size_t, Pt> inAt;
  for (auto& [to, list] : into) {
    const Box* B = p.find(to);
    std::stable_sort(list.begin(), list.end(), [&](size_t a, size_t b) {
      const Pt pa = outPort(*p.find(p.edges[a].from)), pb = outPort(*p.find(p.edges[b].from));
      return pa.y != pb.y ? pa.y < pb.y : pa.x < pb.x;
    });
    const float n = static_cast<float>(list.size()), gap = list.size() > 1 ? std::min(14.f, (B->h - 20) / (n - 1)) : 0.f;
    for (size_t k = 0; k < list.size(); ++k) inAt[list[k]] = {B->x, B->y + B->h * 0.5f + (static_cast<float>(k) - (n - 1) * 0.5f) * gap};
  }
  // Forward lines: out along the port's row, one vertical run in the gap just left of the target, in
  // along the in-port's row, corners rounded. Every line into one column has its own lane in that gap,
  // ordered to cross the fewest others, so each can be followed by eye (no shared runs).
  struct Run {
    size_t e;
    Pt a, b;
    float gapL;
  };
  std::map<int, std::vector<Run>> gaps;  // by the target column's left edge
  for (size_t i = 0; i < p.edges.size(); ++i) {
    const Edge& e = p.edges[i];
    const Box *A = p.find(e.from), *B = p.find(e.to);
    if (!A || !B) continue;
    const Pt a = outPort(*A), b = inAt[i];
    if (b.x - a.x < 40) continue;
    float gapL = a.x;  // the right edge of the last box before the target that stands in the run's way
    const float y0 = std::min(a.y, b.y) - 8, y1 = std::max(a.y, b.y) + 8;
    for (const auto& o : p.boxes)
      if (o.x + o.w <= b.x - 8 && o.x + o.w > gapL && o.y < y1 && o.y + o.h > y0) gapL = o.x + o.w;
    gaps[static_cast<int>(std::lround(b.x))].push_back({i, a, b, gapL});
  }
  std::map<size_t, std::vector<Pt>> laned;
  for (auto& [bx, runs] : gaps) {
    float L = -1e9f;
    for (const auto& r : runs) L = std::max(L, r.gapL);
    const float lo = L + 14, hi = static_cast<float>(bx) - 14;
    if (hi < lo) continue;
    const size_t n = runs.size();
    const float sp = n > 1 ? std::min(12.f, (hi - lo) / static_cast<float>(n - 1)) : 0.f, mid = (lo + hi) * 0.5f;
    auto square = [&](const Run& r, float lane) {
      if (std::fabs(r.a.y - r.b.y) < 0.5f) return std::vector<Pt>{r.a, r.b};
      return std::vector<Pt>{r.a, {lane, r.a.y}, {lane, r.b.y}, r.b};
    };
    auto lay = [&](const std::vector<Run>& order) {
      std::vector<std::vector<Pt>> v;
      for (size_t k = 0; k < order.size(); ++k) v.push_back(square(order[k], mid + (static_cast<float>(k) - static_cast<float>(n - 1) * 0.5f) * sp));
      return v;
    };
    // a few orders; the one with the fewest crossings wins (ties: the first)
    using Key = std::function<float(const Run&)>;
    const std::vector<Key> keys = {
        [](const Run& r) { return r.b.y > r.a.y ? -r.a.y : r.a.y; },  // down lines: lower sources nearer; up lines the other way
        [](const Run& r) { return r.b.y > r.a.y ? r.b.y : -r.b.y; },
        [](const Run& r) { return r.a.y; }, [](const Run& r) { return -r.a.y; }, [](const Run& r) { return r.b.y; }, [](const Run& r) { return -r.b.y; }};
    std::vector<Run> best = runs;
    int bestX = 1 << 30;
    for (const auto& k : keys) {
      std::vector<Run> o = runs;
      std::stable_sort(o.begin(), o.end(), [&](const Run& x, const Run& y) { return k(x) < k(y); });
      const int c = crossings(lay(o));
      if (c < bestX) bestX = c, best = o;
    }
    const auto paths = lay(best);
    for (size_t k = 0; k < best.size(); ++k) {
      const auto& sq = paths[k];
      std::vector<Pt> path = sq.size() == 2 ? sq : Router::roundedPath(sq, std::min(16.f, std::fabs(best[k].a.y - best[k].b.y) * 0.5f));
      if (R.clear(path, 0.5f)) laned[best[k].e] = path;
    }
  }
  for (const auto& [i, path] : laned) R.take(path);
  for (size_t i = 0; i < p.edges.size(); ++i) {
    const Edge& e = p.edges[i];
    const Box *A = p.find(e.from), *B = p.find(e.to);
    if (!A || !B) continue;
    Line L;
    L.from = e.from, L.to = e.to, L.edge = static_cast<int>(i);
    L.a = outPort(*A), L.b = inAt[i];
    if (auto it = laned.find(i); it != laned.end()) L.pts = it->second;
    else if (B->x > L.a.x + 40) L.pts = R.square(L.a, {1, 0}, L.b, {-1, 0}, {L.a, L.b});  // forward but blocked: through the gaps
    else L.pts = R.path(L.a, {1, 0}, L.b, {-1, 0});
    v.push_back(std::move(L));
  }
  return v;
}

std::vector<Line> modLines(const Patch& p, const std::vector<float>& params, int only) {
  // one line per (tile, box): the links it stands for
  std::map<std::pair<int, int>, std::vector<int>> groups;
  for (int s = 0; s < kModSlots; ++s) {
    if (!isLink(params, s)) continue;
    const int tile = moveTileOf(vali(params, modP(s, M_Source))), box = ownerBox(p, vali(params, modP(s, M_Dest)) - 1);
    if (tile == box || !p.find(tile) || !p.find(box)) continue;
    groups[{tile, box}].push_back(s);
  }
  // each target: its ports spread along its bottom edge, in the order of their tiles
  std::map<int, std::vector<int>> tilesOf;
  for (const auto& [k, slots] : groups) tilesOf[k.second].push_back(k.first);
  std::map<std::pair<int, int>, Pt> inAt;
  for (auto& [box, tiles] : tilesOf) {
    const Box* B = p.find(box);
    std::stable_sort(tiles.begin(), tiles.end(), [&](int a, int b) { return modOutPort(*p.find(a)).x < modOutPort(*p.find(b)).x; });
    const float n = static_cast<float>(tiles.size()), gap = tiles.size() > 1 ? std::min(16.f, (B->w - 32) / (n - 1)) : 0.f;
    for (size_t k = 0; k < tiles.size(); ++k) inAt[{tiles[k], box}] = {B->x + B->w * 0.5f + (static_cast<float>(k) - (n - 1) * 0.5f) * gap, B->y + B->h};
  }
  std::vector<Line> v;
  Router R(p);
  for (const auto& [k, slots] : groups) {
    if (only >= 0 && k.first != only && k.second != only) continue;
    Line L;
    L.from = k.first, L.to = k.second, L.slots = slots;
    L.a = modOutPort(*p.find(k.first)), L.b = inAt[k];
    L.pts = R.path(L.a, {0, -1}, L.b, {0, 1});
    v.push_back(std::move(L));
  }
  return v;
}

bool lineThroughBox(const Patch& p, const Line& l) {
  const auto pts = dense(l.pts, 2);
  for (const Pt& q : pts)
    for (const auto& b : p.boxes)
      if (q.x > b.x + 0.5f && q.x < b.x + b.w - 0.5f && q.y > b.y + 0.5f && q.y < b.y + b.h - 0.5f) return true;
  return false;
}

Score score(const Patch& p) {
  Score s;
  for (size_t i = 0; i < p.boxes.size(); ++i) {
    const Box& a = p.boxes[i];
    if (std::fabs(a.x - snap(a.x)) > 0.01f || std::fabs(a.y - snap(a.y)) > 0.01f) ++s.offGrid;
    for (size_t j = i + 1; j < p.boxes.size(); ++j)
      if (overlap(a, p.boxes[j], -0.5f)) ++s.overlaps;
  }
  const auto lines = soundLines(p);
  for (const auto& l : lines) {
    if (lineThroughBox(p, l)) ++s.through;
    const Box *A = p.find(l.from), *B = p.find(l.to);
    if (A && B && B->x + B->w * 0.5f < A->x + A->w * 0.5f) ++s.backward;
  }
  // crossings: two lines that cross away from their ports (lines sharing a port fork there)
  auto segX = [](Pt a, Pt b, Pt c, Pt d) {
    const float d1 = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x), d2 = (b.x - a.x) * (d.y - a.y) - (b.y - a.y) * (d.x - a.x);
    const float d3 = (d.x - c.x) * (a.y - c.y) - (d.y - c.y) * (a.x - c.x), d4 = (d.x - c.x) * (b.y - c.y) - (d.y - c.y) * (b.x - c.x);
    return ((d1 > 0) != (d2 > 0)) && ((d3 > 0) != (d4 > 0)) && d1 != 0 && d2 != 0 && d3 != 0 && d4 != 0;
  };
  auto nearEnd = [](const Line& l, Pt q) { return dist(q, l.a) < 12 || dist(q, l.b) < 12; };
  for (size_t i = 0; i < lines.size(); ++i)
    for (size_t j = i + 1; j < lines.size(); ++j) {
      const auto& P = lines[i].pts;
      const auto& Q = lines[j].pts;
      bool crossed = false;
      for (size_t a = 1; a < P.size() && !crossed; ++a)
        for (size_t b = 1; b < Q.size() && !crossed; ++b)
          if (segX(P[a - 1], P[a], Q[b - 1], Q[b]) && !nearEnd(lines[i], P[a]) && !nearEnd(lines[j], Q[b]) && !nearEnd(lines[i], Q[b]) &&
              !nearEnd(lines[j], P[a]))
            crossed = true;
      s.crossings += crossed;
    }
  return s;
}

void bigSound(std::vector<float>& params, Model& m) {
  if (static_cast<int>(params.size()) < kParamCount) params.resize(static_cast<size_t>(kParamCount), 0.f);
  auto set = [&](int k, float v) { params[static_cast<size_t>(k)] = v; };
  for (int o = 0; o < kOscs; ++o) set(oscP(o, O_Enable), 1);
  set(subP(S_Enable), 1), set(noiseP(N_Enable), 1), set(filtP(0, F_Enable), 1), set(filtP(1, F_Enable), 1);
  auto route = [&](int r, int dest, float bal) { set(routeP(r, R_Dest), static_cast<float>(dest)), set(routeP(r, R_Balance), bal); };
  route(RtA, RouteFilter, -0.4f), route(RtB, RouteFilter, 1.f), route(RtC, RouteFilter, 0.f), route(RtSub, RouteMain, -1.f),
      route(RtNoise, RouteFilter, 1.f), route(RtF1, RouteMain, -1.f), route(RtF2, RouteMain, -1.f);
  set(routeP(RtC, R_Bus1), 0.3f), set(routeP(RtNoise, R_Bus2), 0.25f);
  const int types[6] = {FxDistortion, FxChorus, FxDelay, FxReverb, FxReverb, FxPhaser};
  const int racks[6] = {0, 0, 0, 0, 1, 2};
  m.fx.clear();
  for (int s = 0; s < 6; ++s) {
    set(fxP(s, X_Type), static_cast<float>(types[s])), set(fxP(s, X_On), 1);
    FxNode n;
    n.slot = s, n.rack = racks[s];
    m.fx.push_back(n);
  }
  m.macroName[0] = "Bright", m.macroName[1] = "Motion", m.macroName[2] = "Width";
  struct L {
    int src, k;
    float amt;
    bool both;
  };
  const L links[25] = {
      {SrcLfo1, oscP(0, O_WtPos), .45f, false}, {SrcLfo1, oscP(1, O_WtPos), .30f, false}, {SrcLfo1, filtP(0, F_Freq), .25f, true},
      {SrcLfo1 + 1, oscP(0, O_Pan), .40f, true}, {SrcLfo1 + 1, oscP(1, O_Pan), -.40f, false},
      {SrcLfo1 + 2, filtP(1, F_Freq), .35f, false}, {SrcLfo1 + 2, fxP(1, X_P0), .20f, false},
      {SrcLfo1 + 3, oscP(2, O_Warp1), .30f, true}, {SrcLfo1Y + 3, filtP(1, F_Res), .25f, false},
      {SrcLfo1 + 4, oscP(2, O_WtPos), .60f, false}, {SrcLfo1 + 5, fxP(0, X_P0), .50f, false},
      {SrcEnv2, filtP(0, F_Freq), .55f, false}, {SrcEnv2, oscP(0, O_Warp1), .40f, false}, {SrcEnv2, oscP(2, O_Level), .30f, false},
      {SrcEnv3, filtP(1, F_Freq), .40f, false}, {SrcEnv3, G_Bus1Vol, .25f, false},
      {SrcMacro1, filtP(0, F_Freq), .35f, false}, {SrcMacro1, filtP(1, F_Res), .20f, false},
      {SrcMacro1 + 1, lfoP(0, L_Rate), .50f, false}, {SrcMacro1 + 1, lfoP(1, L_Rate), .40f, false},
      {SrcMacro1 + 2, fxP(3, X_P0), .60f, false}, {SrcMacro1 + 2, fxP(2, X_P0), .40f, false},
      {SrcVelo, oscP(0, O_Level), .25f, false}, {SrcModWheel, filtP(0, F_Res), .20f, false}, {SrcRand1, oscP(1, O_Fine), .10f, true}};
  for (int s = 0; s < 25; ++s) {
    set(modP(s, M_Source), static_cast<float>(links[s].src)), set(modP(s, M_Dest), static_cast<float>(links[s].k + 1));
    set(modP(s, M_Amount), links[s].amt), set(modP(s, M_Bipolar), links[s].both ? 1.f : 0.f);
  }
}

}  // namespace gl::packs::plasma::patch
