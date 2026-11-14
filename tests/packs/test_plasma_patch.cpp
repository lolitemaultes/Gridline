// Gridline — "plasma" pack tests: the Patch view's model and layout (plasma_patch.h). Every part and
// link of a sound has a box; the default flow is clean; Clean up turns messy layouts (a pile in one
// corner, overlaps, the flow reversed, boxes scattered far off, the big sound shuffled) into clean
// ones, keeps the user's order where it can, is stable, and saved places come back exactly.
#include <algorithm>
#include <cmath>
#include <functional>
#include <initializer_list>
#include <memory>
#include <cstdio>
#include <random>
#include <set>
#include <string>
#include "../../src/core/PluginInfo.h"
#include "../../src/packs/plasma/plasma.h"
#include "../../src/packs/plasma/plasma_dsp.h"
#include "../../src/packs/plasma/plasma_patch.h"
#include "../Check.h"

using namespace gl;
namespace pl = gl::packs::plasma;
namespace pp = gl::packs::plasma::patch;

namespace {

struct Sound {
  std::vector<float> params;
  pl::Model m;
};
Sound initSound() {
  Sound s;
  s.params = defaultPluginState(pl::kType).params;
  s.m = pl::defaultModel();
  return s;
}
Sound bigSound() {
  Sound s = initSound();
  pp::bigSound(s.params, s.m);
  return s;
}
void print(const char* what, const pp::Score& s) {
  std::printf("# %s: overlaps %d, through %d, crossings %d, backward %d, off grid %d (total %ld)\n", what, s.overlaps, s.through, s.crossings,
              s.backward, s.offGrid, s.total());
}
bool clean(const pp::Score& s) { return s.overlaps == 0 && s.through == 0 && s.offGrid == 0 && s.backward == 0; }
bool samePlaces(const pp::Patch& a, const pp::Patch& b) {
  if (a.boxes.size() != b.boxes.size()) return false;
  for (const auto& x : a.boxes) {
    const pp::Box* y = b.find(x.id);
    if (!y || x.x != y->x || x.y != y->y) return false;
  }
  return true;
}
// A messy layout: places given to every movable box, through the saved places (as the editor does).
pp::Patch messy(const Sound& s, const std::function<void(pp::Box&, size_t)>& f) {
  pp::Patch p = pp::buildPatch(s.params, s.m);
  pp::defaultLayout(p);
  std::map<int, std::array<float, 2>> saved;
  for (size_t i = 0; i < p.boxes.size(); ++i) {
    if (!pp::movable(p.boxes[i].id)) continue;
    f(p.boxes[i], i);
    saved[p.boxes[i].id] = {p.boxes[i].x, p.boxes[i].y};
  }
  pp::Patch q = pp::buildPatch(s.params, s.m);
  pp::place(q, saved);
  return q;
}

// ---- the lines' geometry (LINES.md): every line from a port on one box edge to a port on another,
// never through a box, never off the canvas, continuous; modulation from a tile's top centre to the
// target's bottom edge, one line per tile and box. Returns the number of faults (and prints them).
// `messy`: boxes may overlap (a layout the editor never leaves: it slides a dropped box clear). Then
// a port can be under or boxed in by other boxes, so only the ends and the canvas are checked.
int lineFaults(const char* what, const pp::Patch& p, const std::vector<float>& params, bool forward, bool messy = false) {
  int bad = 0;
  auto fault = [&](const char* why, const pp::Line& l) {
    if (bad < 6) std::printf("# %s: line %d -> %d: %s\n", what, l.from, l.to, why);
    ++bad;
  };
  float bx0 = 1e9f, by0 = 1e9f, bx1 = -1e9f, by1 = -1e9f;
  for (const auto& b : p.boxes) bx0 = std::min(bx0, b.x), by0 = std::min(by0, b.y), bx1 = std::max(bx1, b.x + b.w), by1 = std::max(by1, b.y + b.h);
  auto on = [](float v, float at) { return std::fabs(v - at) < 0.01f; };
  auto within = [](float v, float lo, float hi) { return v >= lo - 0.01f && v <= hi + 0.01f; };
  bool overlapping = false;
  for (size_t i = 0; i < p.boxes.size(); ++i)
    for (size_t j = i + 1; j < p.boxes.size(); ++j) {
      const auto &a = p.boxes[i], &b = p.boxes[j];
      overlapping = overlapping || (a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h);
    }
  const bool boxesChecked = !(messy && overlapping);
  auto path = [&](const pp::Line& l) {
    if (l.pts.size() < 2) return fault("no path", l);
    if (!on(l.pts.front().x, l.a.x) || !on(l.pts.front().y, l.a.y) || !on(l.pts.back().x, l.b.x) || !on(l.pts.back().y, l.b.y))
      return fault("the path does not start and end at its ports", l);
    for (size_t i = 0; i < l.pts.size(); ++i) {
      const auto q = l.pts[i];
      if (!std::isfinite(q.x) || !std::isfinite(q.y)) return fault("a point is not a number", l);
      if (q.x < bx0 - pp::kLineMargin - 0.01f || q.x > bx1 + pp::kLineMargin + 0.01f || q.y < by0 - pp::kLineMargin - 0.01f || q.y > by1 + pp::kLineMargin + 0.01f)
        return fault("leaves the canvas", l);
      if (i > 0) {  // continuous: short steps along a curve, or a straight run along the grid
        const auto o = l.pts[i - 1];
        const float d = std::hypot(q.x - o.x, q.y - o.y);
        if (d > 40 && std::fabs(q.x - o.x) > 0.01f && std::fabs(q.y - o.y) > 0.01f) return fault("jumps", l);
        for (int k = 0; k <= 16 && boxesChecked; ++k) {  // no point inside a box
          const float t = static_cast<float>(k) / 16.f, x = o.x + (q.x - o.x) * t, y = o.y + (q.y - o.y) * t;
          for (const auto& b : p.boxes)
            if (x > b.x + 0.5f && x < b.x + b.w - 0.5f && y > b.y + 0.5f && y < b.y + b.h - 0.5f) return fault("runs through a box", l);
        }
      }
    }
  };
  const auto sound = pp::soundLines(p);
  int edges = 0;
  for (const auto& e : p.edges) edges += p.find(e.from) && p.find(e.to);
  if (static_cast<int>(sound.size()) != edges) std::printf("# %s: %zu sound lines for %d connections\n", what, sound.size(), edges), ++bad;
  for (const auto& l : sound) {
    const pp::Box *A = p.find(l.from), *B = p.find(l.to);
    if (!A || !B) {
      fault("a missing box", l);
      continue;
    }
    if (!on(l.a.x, A->x + A->w) || !within(l.a.y, A->y, A->y + A->h)) fault("does not start on its box's right edge", l);
    if (!on(l.b.x, B->x) || !within(l.b.y, B->y, B->y + B->h)) fault("does not end on its box's left edge", l);
    if (forward && B->x + B->w * 0.5f < A->x + A->w * 0.5f) fault("runs backward", l);
    path(l);
  }
  // modulation: every tile and every box pointed (and all at once)
  std::vector<int> all = {-1};
  for (const auto& b : p.boxes) all.push_back(b.id);
  for (int only : all) {
    const auto mod = pp::modLines(p, params, only);
    std::set<std::pair<int, int>> seen;
    for (const auto& l : mod) {
      const pp::Box *T = p.find(l.from), *B = p.find(l.to);
      if (!T || !B || T->col != pp::CMove) {
        fault("modulation not from a tile to a box", l);
        continue;
      }
      if (only >= 0 && l.from != only && l.to != only) fault("shown for a box not pointed at", l);
      if (!seen.insert({l.from, l.to}).second) fault("two modulation lines for one tile and box", l);
      if (!on(l.a.x, T->x + T->w * 0.5f) || !on(l.a.y, T->y)) fault("does not start at its tile's top centre", l);
      if (!on(l.b.y, B->y + B->h) || !within(l.b.x, B->x, B->x + B->w)) fault("does not end on its box's bottom edge", l);
      if (l.slots.empty()) fault("stands for no link", l);
      path(l);
    }
  }
  return bad;
}

}  // namespace

int main() {
  const Sound big = bigSound();
  // ---- every part and every link has a place
  {
    pp::Patch p = pp::buildPatch(big.params, big.m);
    int links = 0, reached = 0;
    for (int s = 0; s < pl::kModSlots; ++s) {
      if (!pp::isLink(big.params, s)) continue;
      ++links;
      const int src = static_cast<int>(std::lround(big.params[static_cast<size_t>(pl::modP(s, pl::M_Source))]));
      const int dest = static_cast<int>(std::lround(big.params[static_cast<size_t>(pl::modP(s, pl::M_Dest))])) - 1;
      reached += p.find(pp::moveTileOf(src)) && p.find(pp::ownerBox(p, dest));
    }
    expect(links == 25, "the big sound has 25 links");
    expect(reached == links, "every link has its source tile and its target box");
    for (int id : std::initializer_list<int>{pp::NSrc, pp::NSrc + 1, pp::NSrc + 2, pp::NSrc + 3, pp::NSrc + 4, pp::NFilt, pp::NFilt + 1, pp::NAmp, pp::NOut, pp::NSendA, pp::NSendB})
      if (!p.find(id)) std::printf("# missing box %d\n", id);
    int parts = 0;
    for (int id : std::initializer_list<int>{pp::NSrc, pp::NSrc + 1, pp::NSrc + 2, pp::NSrc + 3, pp::NSrc + 4, pp::NFilt, pp::NFilt + 1, pp::NAmp, pp::NOut, pp::NSendA, pp::NSendB})
      parts += p.find(id) != nullptr;
    expect(parts == 11, "every part of the big sound has a box (5 sources, 2 filters, amp, out, 2 sends)");
    int fx = 0, tiles = 0;
    for (const auto& b : p.boxes) fx += b.col == pp::CFx, tiles += b.col == pp::CMove;
    expect(fx == 4, "the four main effects have a box each");
    expect(tiles == 14, "one tile per movement source in use (six LFOs, two envelopes, three macros, velocity, mod wheel, chance)");
    expect(p.find(pp::NMove + pl::SrcLfo1Y + 3) == nullptr, "an LFO's second output is on its LFO's tile");
    int sound = 0;
    for (const auto& e : p.edges) sound += e.kind == 0;
    expect(sound >= 12, "the routes are sound lines (sources, filters, the chain, the returns)");
  }
  {  // the starting sound: one layer, one filter, the amp, out; nothing moves it, so no tiles but Add
    const Sound s = initSound();
    pp::Patch p = pp::buildPatch(s.params, s.m);
    int tiles = 0;
    for (const auto& b : p.boxes) tiles += b.col == pp::CMove;
    expect(p.find(pp::NSrc) && p.find(pp::NAmp) && p.find(pp::NOut) && p.find(pp::NAddSrc) && p.find(pp::NAddMove), "the starting sound: Layer 1, Amp, Out and the add tiles");
    expect(tiles == 0, "unused movement sources stay tucked away (only the Add tile)");
    pp::defaultLayout(p);
    const pp::Score sc = pp::score(p);
    print("starting sound", sc);
    expect(clean(sc), "the starting sound's default flow is clean");
    expect(lineFaults("starting sound", p, s.params, true) == 0, "the starting sound: every line runs port to port");
  }
  // ---- the default flow of the big sound is clean and runs left to right
  pp::Patch def = pp::buildPatch(big.params, big.m);
  pp::defaultLayout(def);
  {
    const pp::Score sc = pp::score(def);
    print("big default", sc);
    expect(clean(sc), "big sound: the default flow has no overlaps, no lines through boxes, all on the grid, all left to right");
    const pp::Box *a = def.find(pp::NSrc), *f = def.find(pp::NFilt), *amp = def.find(pp::NAmp), *o = def.find(pp::NOut), *t = def.find(pp::NMove + pl::SrcLfo1);
    expect(a->x < f->x && f->x < amp->x && amp->x < o->x, "sources, shape, amp, then out, left to right");
    float soundBottom = 0;
    for (const auto& b : def.boxes)
      if (b.col != pp::CMove && b.id != pp::NAddMove) soundBottom = std::max(soundBottom, b.y + b.h);
    expect(t->y > soundBottom, "the movement tiles sit under the sound");
    const pp::Box *sa = def.find(pp::NSendA), *fx0 = def.find(pp::NFx);
    expect(sa->y > amp->y + amp->h && sa->x < fx0->x, "the sends sit under the Amp, before the effects");
    expect(lineFaults("big default", def, big.params, true) == 0, "big sound: every line runs port to port, clear of the boxes, on the canvas");
  }
  // ---- saved places come back exactly (through the sound's data, as a preset or project keeps them)
  {
    Sound s = big;
    s.m.patchPos[pp::NSrc] = {123.5f, -40.25f};
    s.m.patchPos[pp::NMove + pl::SrcLfo1] = {900.f, 812.f};
    pl::Model back;
    expect(pl::modelFromData(pl::modelToData(s.m), back) && back.patchPos == s.m.patchPos, "box places are saved with the sound and read back exactly");
    pp::Patch p = pp::buildPatch(s.params, back);
    pp::place(p, back.patchPos);
    expect(p.find(pp::NSrc)->x == 123.5f && p.find(pp::NSrc)->y == -40.25f, "a moved box opens where it was left");
    bool clear = true;
    for (size_t i = 0; i < p.boxes.size(); ++i)
      for (size_t j = i + 1; j < p.boxes.size(); ++j) {
        const auto &a = p.boxes[i], &b = p.boxes[j];
        if (a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h) clear = false;
      }
    expect(clear, "boxes without a saved place are placed clear of the moved ones");
    pl::Model none;
    expect(pl::modelFromData(pl::modelToData(big.m), none) && none.patchPos.empty() && pl::modelToData(big.m).find("patchPos") == std::string::npos,
           "a sound nobody arranged saves no places");
  }
  // ---- Clean up on deliberately messy layouts
  std::mt19937 rng(227);
  struct Case {
    const char* name;
    Sound s;
    std::function<void(pp::Box&, size_t)> f;
  };
  const Sound init = initSound();
  std::vector<Case> cases;
  cases.push_back({"everything piled in one corner", big, [](pp::Box& b, size_t i) { b.x = 3.f + static_cast<float>(i % 3), b.y = 5.f; }});
  cases.push_back({"boxes overlapping", big, [](pp::Box& b, size_t i) { b.x = b.x * 0.55f + 13.f, b.y = b.y * 0.5f + static_cast<float>(i) * 3.f; }});
  cases.push_back({"the flow reversed, right to left", big, [](pp::Box& b, size_t) { b.x = 1300.f - b.x - b.w; }});
  {
    auto r = std::make_shared<std::mt19937>(11);
    cases.push_back({"scattered far apart, off screen", big, [r](pp::Box& b, size_t) {
                       std::uniform_real_distribution<float> d(-5000.f, 5000.f);
                       b.x = d(*r), b.y = d(*r);
                     }});
  }
  {  // the big sound shuffled: the boxes' places dealt out to other boxes, a little off the grid
    pp::Patch p = pp::buildPatch(big.params, big.m);
    pp::defaultLayout(p);
    std::vector<std::array<float, 2>> places;
    for (const auto& b : p.boxes)
      if (pp::movable(b.id)) places.push_back({b.x, b.y});
    std::shuffle(places.begin(), places.end(), rng);
    auto shared = std::make_shared<std::vector<std::array<float, 2>>>(places);
    auto n = std::make_shared<size_t>(0);
    cases.push_back({"the 25-link sound shuffled at random", big, [shared, n](pp::Box& b, size_t) {
                       const auto& q = (*shared)[(*n)++ % shared->size()];
                       b.x = q[0] + 3.f, b.y = q[1] - 5.f;
                     }});
  }
  cases.push_back({"the starting sound piled up", init, [](pp::Box& b, size_t) { b.x = 40.f, b.y = 40.f; }});
  for (auto& cs : cases) {
    pp::Patch p = messy(cs.s, cs.f);
    const auto before = pp::positions(p);
    const pp::Score s0 = pp::score(p);
    print((std::string(cs.name) + ", before").c_str(), s0);
    pp::Patch q = p;
    pp::cleanUp(q);
    const pp::Score s1 = pp::score(q);
    print((std::string(cs.name) + ", after").c_str(), s1);
    expect(s1.overlaps == 0 && s1.through == 0, (std::string("Clean up: ") + cs.name + ": no overlaps and no lines through boxes").c_str());
    expect(s1.offGrid == 0 && s1.backward == 0, (std::string("Clean up: ") + cs.name + ": every box on the grid, the flow left to right").c_str());
    expect(s1.total() < s0.total(), (std::string("Clean up: ") + cs.name + ": the layout scores better than before").c_str());
    expect(lineFaults((std::string(cs.name) + ", messy").c_str(), p, cs.s.params, false, true) == 0,
           (std::string("lines on a messy layout: ") + cs.name + ": port to port, clear of the boxes, on the canvas").c_str());
    expect(lineFaults((std::string(cs.name) + ", cleaned").c_str(), q, cs.s.params, true) == 0,
           (std::string("lines after Clean up: ") + cs.name + ": port to port, forward, clear of the boxes").c_str());
    pp::Patch r = q;
    pp::cleanUp(r);
    expect(samePlaces(q, r), (std::string("Clean up: ") + cs.name + ": a second Clean up changes nothing").c_str());
    // the editor saves the places Clean up gives; Undo puts the old saved places back
    pp::Patch u = pp::buildPatch(cs.s.params, cs.s.m);
    pp::place(u, before);
    expect(pp::positions(u) == before, (std::string("Clean up: ") + cs.name + ": the messy places come back exactly").c_str());
  }
  // ---- boxes moved by hand (as a drag leaves them: on the grid, slid clear of the others)
  {
    std::mt19937 mr(4227);
    int bad = 0;
    for (int round = 0; round < 12; ++round) {
      pp::Patch p = def;
      std::vector<int> ids;
      for (const auto& b : p.boxes)
        if (pp::movable(b.id)) ids.push_back(b.id);
      for (int k = 0; k < 5; ++k) {
        pp::Box* b = p.find(ids[mr() % ids.size()]);
        std::uniform_real_distribution<float> d(-300.f, 300.f);
        const float ox = b->x, oy = b->y;
        b->x = std::round((b->x + d(mr)) / pp::kGrid) * pp::kGrid, b->y = std::round((b->y + d(mr)) / pp::kGrid) * pp::kGrid;
        for (const auto& o : p.boxes)  // a box let go on another slides clear in the editor: here it stays put
          if (o.id != b->id && b->x < o.x + o.w + 8 && o.x < b->x + b->w + 8 && b->y < o.y + o.h + 8 && o.y < b->y + b->h + 8) b->x = ox, b->y = oy;
      }
      pp::Patch q = pp::buildPatch(big.params, big.m);  // through the saved places: the moved boxes slide clear
      pp::place(q, pp::positions(p));
      bad += lineFaults(("moved, round " + std::to_string(round)).c_str(), q, big.params, false);
    }
    expect(bad == 0, "moved boxes: every line still runs port to port, clear of the boxes, on the canvas");
  }
  // ---- Clean up keeps the user's order where it reads as well
  {
    pp::Patch p = def;
    // the user put LFO 6 first among the LFO tiles and Envelope 3 before Envelope 2
    p.find(pp::NMove + pl::SrcLfo1 + 5)->x = -300;
    p.find(pp::NMove + pl::SrcLfo1 + 5)->y = def.find(pp::NMove + pl::SrcLfo1)->y;
    std::swap(p.find(pp::NMove + pl::SrcEnv2)->x, p.find(pp::NMove + pl::SrcEnv3)->x);
    const bool kept = pp::cleanUp(p);
    expect(kept, "Clean up keeps the user's order when it crosses no more lines");
    expect(p.find(pp::NMove + pl::SrcLfo1 + 5)->x < p.find(pp::NMove + pl::SrcLfo1)->x, "the user's order of the LFO tiles is kept");
    expect(p.find(pp::NMove + pl::SrcEnv3)->x < p.find(pp::NMove + pl::SrcEnv2)->x, "the user's order of the envelope tiles is kept");
    expect(p.find(pp::NMove + pl::SrcEnv2)->x > p.find(pp::NMove + pl::SrcLfo1 + 4)->x || p.find(pp::NMove + pl::SrcEnv2)->y > p.find(pp::NMove + pl::SrcLfo1)->y,
           "the tiles stay grouped by family (envelopes after the LFOs)");
    expect(clean(pp::score(p)), "the result is clean");
  }
  {  // Root above Layer 1 in the user's column: kept when it crosses no more lines than the default
    pp::Patch p = def;
    p.find(pp::NSrc + 3)->y = -200;
    pp::Patch d2 = def;
    const bool kept = pp::cleanUp(p);
    const pp::Score sc = pp::score(p);
    expect(clean(sc), "a moved source: Clean up is clean either way");
    expect(kept ? p.find(pp::NSrc + 3)->y < p.find(pp::NSrc)->y : samePlaces(p, d2), "a moved source: kept above Layer 1, or the default flow when that crosses fewer lines");
  }
  // ---- adding a part keeps the others where they are (a stable layout)
  {
    Sound s = big;
    pp::Patch a = pp::buildPatch(s.params, s.m);
    pp::defaultLayout(a);
    s.m.patchPos = pp::positions(a);
    s.params[static_cast<size_t>(pl::modP(30, pl::M_Source))] = pl::SrcEnv1 + 3;  // Envelope 4 onto Layer 2's level
    s.params[static_cast<size_t>(pl::modP(30, pl::M_Dest))] = static_cast<float>(pl::oscP(1, pl::O_Level) + 1);
    pp::Patch b = pp::buildPatch(s.params, s.m);
    pp::place(b, s.m.patchPos);
    bool still = true;
    for (const auto& x : a.boxes)
      if (pp::movable(x.id) && b.find(x.id) && (b.find(x.id)->x != x.x || b.find(x.id)->y != x.y)) still = false;
    expect(still && b.find(pp::NMove + pl::SrcEnv1 + 3), "a new movement source gets a tile and nothing else moves");
    const pp::Score sc = pp::score(b);
    expect(sc.overlaps == 0, "the new tile lands in a free spot");
  }
  // ---- moving a box never prepares the sound again (the prepared data's key leaves the places out)
  {
    PluginState st = defaultPluginState(pl::kType);
    pl::Model m = pl::defaultModel();
    m.patchPos[pp::NSrc] = {16, 24};
    st.data = pl::modelToData(m);
    expect(st.data.find("patchPos") != std::string::npos, "the saved sound carries the places");
    const auto first = pl::buildAux(st);
    m.patchPos[pp::NSrc] = {400, -96};
    m.patchPos[pp::NAmp] = {800, 8};
    st.data = pl::modelToData(m);
    expect(pl::buildAux(st) == first, "moving a box keeps the prepared data (no rebuild)");
    m.patchPos.clear();
    st.data = pl::modelToData(m);
    expect(pl::buildAux(st) == first, "a sound without places shares the same prepared data");
    m.osc[0].table = "custom";
    m.osc[0].frames.assign(1, std::vector<float>(2048, 0.25f));
    st.data = pl::modelToData(m);
    expect(pl::buildAux(st) != first, "a change the sound needs still prepares it again");
  }
  // ---- junk places in a saved sound are skipped, huge ones are held in range
  {
    pl::Model m = pl::defaultModel();
    std::string d = pl::modelToData(m);
    const size_t at = d.find("\"macroNames\"");
    expect(at != std::string::npos, "the saved sound has a place to add the field");
    d.insert(at, "\"patchPos\": [[0, 1e30, -1e30], [10, \"x\", 2], [21], [-4, 1, 1], [5000, 1, 1], \"junk\", [20, 32, 48, 99]],\n");
    pl::Model back = pl::defaultModel();
    expect(pl::modelFromData(d, back), "a sound with junk places still loads");
    expect(back.patchPos.size() == 2 && back.patchPos.count(0) && back.patchPos.count(20), "only the well-formed places are kept");
    expect(back.patchPos[0][0] <= 20000.f && back.patchPos[0][1] >= -20000.f, "huge places are held in range");
    expect(back.patchPos[20][0] == 32.f && back.patchPos[20][1] == 48.f, "a place from a newer version (extra fields) is read");
  }
  return finish();
}
