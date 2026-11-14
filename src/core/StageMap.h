// Gridline - the Stage view's maths (app/StageView.cpp, docs/stage-view.md). Pure functions, no ImGui
// (tests/test_k188_stage.cpp):
//   - the perspective stage on screen: where a track at (pan, distance) stands, how big its token is,
//     its width footprint, and back;
//   - groups: tracks that share one spot (the same distance and pan within a small tolerance, with
//     hysteresis so automation never makes them flicker), the group's tray and its cells;
//   - the no-overlap rule of a drop: join the token or tray under it, or ease to the nearest free spot;
//   - display separation of tokens that would overlap without sharing a spot;
//   - the label rules (one line, one cut keeping the start and the distinguishing end, neighbours share
//     space, a dropped line or a hidden label when still too tight, labels inside the stage edges);
//   - Spread out, and the words the panel shows.
#pragma once
#include <cstddef>
#include <functional>
#include <map>
#include <string>
#include <vector>
#include "Types.h"

namespace gl::stage {

struct Pt {
  float x = 0, y = 0;
};
struct Rect {
  float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  float w() const { return x1 - x0; }
  float h() const { return y1 - y0; }
  float area() const { return w() > 0 && h() > 0 ? w() * h() : 0.0f; }
  bool contains(float x, float y) const { return x >= x0 && x <= x1 && y >= y0 && y <= y1; }
  Rect grown(float d) const { return {x0 - d, y0 - d, x1 + d, y1 + d}; }
};
float overlapArea(const Rect& a, const Rect& b);

// ---------------------------------------------------------------- the stage on screen
constexpr double kPerspective = 0.12;  // rows a little closer at the back
constexpr float kTokenFront = 52.0f;   // token side at the front (px at scale 1) ...
constexpr float kTokenBack = 36.0f;    // ... and at the back
constexpr float kTokenTray = 40.0f;    // tokens in a group's tray
constexpr float kCellW = 96.0f, kCellH = 66.0f;  // a tray cell
constexpr int kTrayRows = 3;
constexpr float kBulge = 12.0f;        // the front edge's curve toward the listener
constexpr int kCrowded = 25;           // from this many tracks, tokens shrink to 86 %

// The stage trapezoid and the token rows. Build it with layout().
struct Geo {
  float cx = 0, backY = 0, frontY = 0;   // the back edge, the front edge's straight chord
  float backHalf = 0, frontHalf = 0;     // half widths of the back and front edges
  float rowFront = 0, rowBack = 0;       // token centres at distance 0 and 1
  float scale = 1.0f, tokenScale = 1.0f;
  float listenerY = 0;                   // the listener's centre
  // The stage's half width at height y (interpolated along the side edges).
  float halfAt(float y) const;
  // The token centre's y at distance d (0 .. 1) and back.
  float yOf(double d) const;
  double dOfY(float y) const;
  // How far pan +-1 reaches from the centre at distance d.
  float reach(double d) const;
  float xOf(double pan, double d) const;
  double panOfX(float x, double d) const;
  Pt at(double pan, double d) const { return {xOf(pan, d), yOf(d)}; }
  // The token's side and rectangle at distance d.
  float side(double d) const;
  Rect token(double pan, double d) const;
  // The width footprint (an ellipse at the token's base): half axes for a stereo separation.
  float footRx(double d, double sep) const;
  // The stereo separation whose footprint has half axis rx (the width handles).
  double sepOfFootRx(double d, float rx) const;
  float footRy(double d) const;
  // Inside the stage (the trapezoid, the curved front not counted).
  bool inside(float x, float y) const;
};
// The stage inside the body rectangle (between the toolbar and the control panel) at UI scale
// `scale`, for `tracks` tracks.
Geo layout(float x0, float y0, float x1, float y1, float scale, int tracks);

// Snapping: the five depths (front, near, middle, far, back) and the centre magnet.
double snapDepth(double d);
double snapPan(double pan, double zone);

// ---------------------------------------------------------------- groups
struct Pos {
  ObjectId id = kNoId;
  double pan = 0, depth = 0;
};
constexpr double kJoin = 0.03;   // same spot: pan and distance within this ...
constexpr double kLeave = 0.06;  // ... and a group holds together until a member is this far away
// Groups of two or more tracks at one spot, members in input (mixer) order. `prev` maps a track to
// its group last time (any stable key, for example the group's first member): members of one earlier
// group stay together up to kLeave, so automation moving them never makes the group flicker.
std::vector<std::vector<size_t>> groups(const std::vector<Pos>& pts, const std::map<ObjectId, ObjectId>* prev = nullptr);
// The group's spot: the members' mean position.
Pos spotOf(const std::vector<Pos>& pts, const std::vector<size_t>& members);

// A group's tray: just above its spot (below it when there is no room above), opaque, cells in
// columns of 4 / 6 / 8 / 10 by size, at most three rows; a larger group shows "+N" in its last cell.
struct Tray {
  Rect box;
  Pt spot;
  bool above = true;      // the tray is above its spot (the stem points down)
  int cols = 0, rows = 0;
  int shown = 0, more = 0;  // members drawn in cells, and the rest behind "+N"
  std::vector<Rect> cells;  // shown members' cells, then the "+N" cell when more > 0
  float token = kTokenTray;
};
Tray trayLayout(const Geo& g, Pt spot, int count, float minX, float maxX, float minY, float maxY);
// The member cell under a point, the "+N" cell (== shown), or -1.
int trayCellAt(const Tray& t, float x, float y);

// ---------------------------------------------------------------- dropping without overlaps
// Something a dropped token must not rest on: a token or a whole tray, and the spot it stands for.
struct Obstacle {
  Rect r;
  double pan = 0, depth = 0;
  int key = -1;  // the caller's index (a track or a group)
};
struct Drop {
  enum Kind { Free, Join, Nudged } kind = Free;
  double pan = 0, depth = 0;
  int key = -1;  // Join: the obstacle's key
};
constexpr float kJoinShare = 0.30f;  // a drop covering this share of a token joins it
constexpr float kGap = 4.0f;         // free tokens keep this gap (px at scale 1)
// The join target under a drag: the obstacle the pointer is over, else the one the dragged token's
// rectangle covers most when that is at least kJoinShare of the token (`exact`: any overlap at all).
int joinTarget(const Geo& g, double pan, double depth, Pt pointer, const std::vector<Obstacle>& obs, bool exact);
// Where a drop at (pan, depth) ends: joining a target (taking its exact spot), where it was dropped
// when nothing is in the way, or eased to the nearest free spot at the same distance (then a row
// nearer or further) when it would overlap without joining. Overlapping is never a resting state.
Drop resolveDrop(const Geo& g, double pan, double depth, Pt pointer, const std::vector<Obstacle>& obs, bool exact);

// Display only: horizontal offsets that keep tokens of one row from overlapping (tracks whose saved
// positions are close but not one spot). The saved positions do not change.
std::vector<float> separate(const std::vector<Rect>& tokens, float gap);

// ---------------------------------------------------------------- labels
using Measure = std::function<float(const std::string&)>;
// One line within `budget` px: the whole name, or one cut keeping the start and the ending that tells
// it apart: a twin's differing word, a numbered / copy / side / bracket ending, the digits ending a
// name without spaces, else a plain end cut. Cuts fall on whole characters (UTF-8).
std::string cutLabel(const std::string& name, float budget, const Measure& m, const std::vector<std::string>& others = {});
struct LabelIn {
  std::string name;
  float cx = 0, y = 0;   // centre x, top
  float token = kTokenFront;
  float minX = 0, maxX = 0;  // the stage edges at this row (labels stay 8 px inside)
  bool chip = false;     // its full name shows in a chip (hovered, selected or dragged): no label
};
struct LabelOut {
  std::string text;
  float x = 0, y = 0, w = 0;  // left, top, width
  bool visible = true;
  bool dropped = false;       // moved one line down (rule 5)
};
// The label rules for every label at once (rows are labels within 6 px of the same top).
std::vector<LabelOut> layoutLabels(const std::vector<LabelIn>& in, const Measure& m, float scale);
// Two capitals or initials for a tile without an icon ("Super Saw" -> "SS", "kick" -> "K").
std::string initials(const std::string& name);

// ---------------------------------------------------------------- arranging
// Spread out: per distance row, in input (mixer) order, even spacing from the centre outward, left and
// right in turn; every track keeps its distance. A row too crowded for its tokens alternates half its
// tracks one step further back (a step nearer at the back). `minSpacing`: the pan step two tokens need.
std::vector<Pos> spreadOut(const std::vector<Pos>& tracks, double minSpacing);

// ---------------------------------------------------------------- words
// Plain words for the panel and the drag tooltip (English, marked for translation by the caller).
std::string panWords(double pan);            // "Centre", "60% right"
const char* distanceWord(double d);          // Front, Near, Middle, Far, Back
const char* distanceHint(double d);          // what that distance does
const char* widthWord(double sep);           // Mono .. Very wide
const char* focusWord(double focus);         // Soft, Natural, Present
const char* focusHint(double focus);
const char* strengthWord(double strength);   // Gentle, Natural, Strong
const char* roomSizeWord(double size);       // Small, Medium, Large, Hall
const char* stageWidthWord(double sep);      // the master's width
double widthPercent(double sep);             // 0 mono, 100 as recorded, 200 doubled
double levelDb(double volume);               // the fader in dB (-inf at 0)
// What a move changes, for the drag tooltip: line 1 the distance, line 2 the side ("" none).
const char* moveDepthLine(double fromD, double toD);
const char* moveSideLine(double fromPan, double toPan);

}  // namespace gl::stage
