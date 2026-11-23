// Gridline - the dock plan of a workspace: the Studio layout's panes as a tree, in Gridline's own terms.
//
// Studio arranges its windows with a free dock tree (no region shell), so a workspace of Studio keeps
// that tree here: splits (axis and the share of the first side) and leaves (the windows docked there in
// tab order, the one in front, and whether it is the main area), plus the floating windows with their
// rectangles (relative to the dock space's top left corner). The app captures it from the live dock
// tree and rebuilds the tree from it; a plan is never a raw dock file, so nothing is loaded mid-session.
//
// Pure data and rules, no ImGui; tested alone (tests/test_k256_dockplan.cpp).
#pragma once
#include <functional>
#include <string>
#include <vector>

namespace gl::dockplan {

constexpr float kMinRatio = 0.05f, kMaxRatio = 0.95f;
constexpr int kMaxDepth = 12;
constexpr int kMaxWindows = 64;

struct Node {
  bool split = false;
  int axis = 0;        // a split: 0 side by side (x), 1 stacked (y)
  float ratio = 0.5f;  // a split: the first child's share of the node
  std::vector<Node> kids;            // a split: exactly two
  std::vector<std::string> windows;  // a leaf: its windows in tab order (stable names)
  std::string front;                 // a leaf: the window in front ("" = the first)
  bool central = false;              // a leaf: the main area (the dock space's central node)
  bool operator==(const Node& o) const;
};

struct Float {
  std::string window;
  float x = 0, y = 0, w = 0, h = 0;  // relative to the dock space's top left corner, pixels
  bool operator==(const Float& o) const { return window == o.window && x == o.x && y == o.y && w == o.w && h == o.h; }
};

struct Plan {
  Node root;
  std::vector<Float> floating;
  bool empty() const { return !root.split && root.windows.empty() && floating.empty(); }
  bool operator==(const Plan& o) const { return root == o.root && floating == o.floating; }
};

// Every window of a plan (docked first, in tree order, then floating).
std::vector<std::string> windows(const Plan& p);
// True when the tree has a central leaf.
bool hasCentral(const Node& n);

// Makes a plan safe to build: ratios clamped to [kMinRatio, kMaxRatio] (a non-finite one becomes 0.5),
// windows `known` refuses dropped, a window kept once (the first place it appears), a split with an empty
// side folded into its other side (an empty central leaf stays, the main area is always there), a front
// that is not in its leaf cleared, too deep trees cut, floating rectangles made finite and at least 60 px.
// Without a central leaf the largest leaf on the first sides becomes it.
void sanitise(Plan& p, const std::function<bool(const std::string&)>& known);

// JSON text and back. Broken text gives an empty plan (the app then keeps its default build).
std::string toJson(const Plan& p);
Plan fromJson(const std::string& text);

// The same arrangement within `tol` (ratios) and `px` (floating rectangles): a plan rebuilt from a
// capture captures the same, give or take the pixels a split rounds to.
bool similar(const Plan& a, const Plan& b, float tol = 0.01f, float px = 2.0f);

// A floating rectangle fitted into a dock space of w x h (moved, then shrunk, to lie inside it).
Float fitInto(const Float& f, float w, float h);

}  // namespace gl::dockplan
