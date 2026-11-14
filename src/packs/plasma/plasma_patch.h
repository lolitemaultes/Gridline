// Gridline — "plasma" pack: the Patch view's model and layout, without drawing (so tests can run it).
// A sound is boxes (the parts in use, the effects, the sends, Out, one tile per movement source in
// use) and the sound lines between them. Each box has a place: the one saved with the sound
// (Model::patchPos, set by moving it), else the default flow (sources, shape, amp, effects, out, left
// to right; sends under the Amp; movement tiles under the sound, by family). Clean up sorts every
// box back into that flow, keeping the order the user gave each column where that reads as well.
#pragma once
#include <array>
#include <map>
#include <set>
#include <vector>
#include "plasma.h"

namespace gl::packs::plasma::patch {

// Box ids (stable: saved positions are keyed by them).
enum : int {
  NSrc = 0,          // + 0..2 the layers, + 3 the root, + 4 the texture
  NFilt = 10,        // + 0..1
  NAmp = 20, NOut = 21, NSendA = 22, NSendB = 23,
  NVoices = 30,      // what plays the notes (shown only when a link moves one of its settings)
  NFx = 100,         // + the effect slot (main chain)
  NMove = 200,       // + the modulation source (a tile)
  NAddSrc = 900, NAddFilt, NAddFx, NAddMove,  // the "add" tiles (placed, never saved)
};
// Where a sound line ends (the route setting it stands for).
enum : int { DF1 = 0, DF2, DAmp, DOut, DSendA, DSendB, DChain };
// Columns of the flow.
enum Col : int { CSrc = 0, CShape, CAmp, CFx, COut, CSend, CMove, CAdd };

inline constexpr float kGrid = 8;

struct Box {
  int id = 0, col = 0;
  float x = 0, y = 0, w = 0, h = 0;
  bool on = true;     // its part plays (a part that is off shows only while something links to it)
};
struct Edge {
  int from = 0, to = 0;
  int kind = 0;       // 0 sound, 1 send, 3 a step along the chain (an effect into the next, a send into the other)
  float weight = 1;   // sound: the share of the source (filter balance)
  int r = -1;         // the route of the source end (-1 none)
  int dest = -1;      // DF1..DChain
};
struct Pt {
  float x, y;
};

struct Patch {
  std::vector<Box> boxes;
  std::vector<Edge> edges;
  std::map<int, int> fxBox;  // effect slot -> the box that shows it
  std::vector<int> chain[3]; // racks: main, send A, send B (top-level slots in order)
  int mainHead = NOut;       // where the main chain starts: its first effect, or Out
  Box* find(int id);
  const Box* find(int id) const;
};

int routeOfBox(int id);              // RtA.. for a source or filter box, -1 otherwise
int boxOfRoute(int r);
int moveTileOf(int src);             // the tile of a modulation source (an LFO's second output is its LFO's tile)
int familyRank(int src);             // 0 LFOs, 1 envelopes, 2 macros, 3 key and touch, 4 chance, 5 audio
int ownerBox(const Patch& p, int k); // the box a parameter shows on (a link to it counts there)
bool movable(int id);                // a box the user can move (saved with the sound)
bool isLink(const std::vector<float>& params, int slot);

// The boxes and sound lines of a sound (sizes set, not placed). `keep`: boxes to show although
// unused (a selected part that is off, a movement source added by hand).
Patch buildPatch(const std::vector<float>& params, const Model& m, const std::set<int>& keep = {});
// The default flow (every box placed).
void defaultLayout(Patch& p);
// Saved places first; every other box at its default place, moved clear of the saved ones.
void place(Patch& p, const std::map<int, std::array<float, 2>>& saved);
// Clean up: every box sorted into the flow, evenly on the grid, the user's order within each column
// kept unless the default order crosses fewer lines. Returns true when it kept the user's order.
bool cleanUp(Patch& p);
// The places to save (movable boxes only).
std::map<int, std::array<float, 2>> positions(const Patch& p);
// ---- the lines (see analysis K-227 LINES.md). Every line runs from one port to another, both on a
// box edge: audio leaves a box's right edge and enters a left edge (each line into a box has its own
// in-port, spread down the edge in the order of the sources); modulation leaves a tile's top centre
// and enters the target's bottom edge (one line per tile and box). A line is one smooth curve, or,
// when that would touch a box, a path through the gaps with rounded corners; it never passes through
// a box and stays within the boxes' bounds plus kLineMargin.
inline constexpr float kLineMargin = 48;
struct Line {
  int from = 0, to = 0;     // boxes
  int edge = -1;            // a sound line: its index in Patch::edges (-1: modulation)
  std::vector<int> slots;   // modulation: the links it stands for
  Pt a{0, 0}, b{0, 0};      // its start and end ports
  std::vector<Pt> pts;      // the drawn path: pts.front() == a, pts.back() == b
};
void lineBounds(const Patch& p, float& x0, float& y0, float& x1, float& y1);
Pt outPort(const Box& b);       // audio out: the right edge's middle
Pt modOutPort(const Box& tile); // modulation out: a tile's top centre
std::vector<Line> soundLines(const Patch& p);
// The modulation lines (one per tile and box); `only` >= 0: just the lines of that tile or box.
std::vector<Line> modLines(const Patch& p, const std::vector<float>& params, int only = -1);
bool lineThroughBox(const Patch& p, const Line& l);

// How tidy a layout is (lower is better).
struct Score {
  int overlaps = 0;   // pairs of boxes that overlap
  int offGrid = 0;    // boxes off the grid
  int through = 0;    // sound lines that run through a box
  int crossings = 0;  // sound lines crossing each other
  int backward = 0;   // sound lines that run right to left
  long total() const { return 1000L * overlaps + 400L * through + 60L * backward + 10L * crossings + offGrid; }
};
Score score(const Patch& p);

// A big sound for the tests and screenshots: three layers, a root and a texture, both filters,
// four effects, both sends, six LFOs (one using its second output), two envelopes, three named
// macros, velocity, the mod wheel and chance: 25 links.
void bigSound(std::vector<float>& params, Model& m);

}  // namespace gl::packs::plasma::patch
