// Gridline - the dock plan of a Studio workspace (core/DockPlan.h): JSON round trip, ratios clamped,
// unknown windows dropped, empty sides folded, one central leaf, floating rectangles kept sane.
#include <cmath>
#include <limits>
#include <set>
#include <string>
#include "Check.h"
#include "core/DockPlan.h"

using namespace gl;
using dockplan::Node;
using dockplan::Plan;

namespace {

Node leaf(std::vector<std::string> w, bool central = false, std::string front = "") {
  Node n;
  n.windows = std::move(w);
  n.central = central;
  n.front = std::move(front);
  return n;
}

Node split(int axis, float ratio, Node a, Node b) {
  Node n;
  n.split = true;
  n.axis = axis;
  n.ratio = ratio;
  n.kids = {std::move(a), std::move(b)};
  return n;
}

// The Studio default as a plan: browser left, the mixer right, the playlist and piano roll in the middle,
// the channel rack floating.
Plan studio() {
  Plan p;
  p.root = split(0, 0.18f, leaf({"Browser", "Undo history"}, false, "Browser"),
                 split(0, 0.8f, leaf({"Playlist", "Piano roll"}, true, "Playlist"), leaf({"Mixer"})));
  p.floating.push_back({"Channel rack", 260, 80, 620, 360});
  return p;
}

const std::set<std::string> kKnown = {"Browser", "Undo history", "Playlist", "Piano roll", "Mixer", "Channel rack", "Master chain"};
bool known(const std::string& w) { return kKnown.count(w) > 0; }

}  // namespace

int main() {
  // ---- round trip ----
  {
    Plan p = studio();
    dockplan::sanitise(p, known);
    const std::string text = dockplan::toJson(p);
    Plan q = dockplan::fromJson(text);
    dockplan::sanitise(q, known);
    expect(q == p, "a plan round-trips through JSON");
    expect(dockplan::toJson(q) == text, "the JSON text is stable (the same text again)");
    expect(q.root.kids[1].kids[0].central && q.root.kids[1].kids[0].front == "Playlist", "the central leaf and its front window are kept");
    expect(q.floating.size() == 1 && q.floating[0].window == "Channel rack" && q.floating[0].w == 620, "a floating window keeps its rectangle");
    const auto all = dockplan::windows(q);
    expect(all.size() == 6 && all.back() == "Channel rack", "windows(): docked first, then floating");
  }
  // ---- similar: a rebuilt plan captures the same, give or take a split's rounding ----
  {
    Plan a = studio(), b = studio();
    b.root.ratio += 0.004f;
    b.floating[0].x += 1;
    expect(dockplan::similar(a, b), "a capture a pixel off is the same arrangement");
    b.root.ratio += 0.05f;
    expect(!dockplan::similar(a, b), "a pane moved by hand is not");
    Plan c = studio();
    c.root.kids[1].kids[1].windows = {"Mixer", "Master chain"};
    expect(!dockplan::similar(a, c), "another window in a pane is not");
  }
  // ---- broken text ----
  expect(dockplan::fromJson("").empty(), "empty text is an empty plan");
  expect(dockplan::fromJson("{not json").empty(), "broken text is an empty plan");
  expect(dockplan::fromJson("[1,2]").empty(), "text that is not an object is an empty plan");
  // ---- ratios ----
  {
    Plan p;
    p.root = split(1, 1.7f, leaf({"Playlist"}, true), leaf({"Mixer"}));
    dockplan::sanitise(p, known);
    expect(std::fabs(p.root.ratio - dockplan::kMaxRatio) < 1e-6f, "a ratio over 1 is clamped");
    p.root.ratio = -3;
    dockplan::sanitise(p, known);
    expect(std::fabs(p.root.ratio - dockplan::kMinRatio) < 1e-6f, "a negative ratio is clamped");
    p.root.ratio = std::numeric_limits<float>::quiet_NaN();
    dockplan::sanitise(p, known);
    expect(std::fabs(p.root.ratio - 0.5f) < 1e-6f, "a NaN ratio becomes an even split");
    expect(p.root.axis == 1, "the axis is kept");
  }
  // ---- unknown and repeated windows ----
  {
    Plan p;
    p.root = split(0, 0.3f, leaf({"Browser", "Some plugin", "Browser"}), leaf({"Playlist", "Old window"}, true));
    p.floating.push_back({"Mixer", 0, 0, 400, 300});
    p.floating.push_back({"Playlist", 0, 0, 400, 300});
    p.floating.push_back({"Gone", 0, 0, 400, 300});
    dockplan::sanitise(p, known);
    expect(p.root.kids[0].windows == std::vector<std::string>({"Browser"}), "unknown and repeated windows leave a leaf");
    expect(p.root.kids[1].windows == std::vector<std::string>({"Playlist"}), "unknown windows leave the central leaf");
    expect(p.floating.size() == 1 && p.floating[0].window == "Mixer", "a floating window docked elsewhere or unknown is dropped");
  }
  // ---- empty sides fold ----
  {
    Plan p;
    p.root = split(0, 0.2f, leaf({"Gone"}), split(1, 0.7f, leaf({"Playlist"}, true), leaf({"Also gone"})));
    dockplan::sanitise(p, known);
    expect(!p.root.split && p.root.central && p.root.windows == std::vector<std::string>({"Playlist"}), "splits with an empty side fold into the other side");
  }
  {
    Plan p;
    p.root = split(0, 0.25f, leaf({"Browser"}), leaf({"Gone"}, true));
    dockplan::sanitise(p, known);
    expect(p.root.split && p.root.kids[1].central && p.root.kids[1].windows.empty(), "an emptied central leaf stays (the main area is always there)");
  }
  // ---- one central leaf ----
  {
    Plan p;
    p.root = split(0, 0.5f, leaf({"Browser"}, true), leaf({"Playlist"}, true));
    dockplan::sanitise(p, known);
    expect(p.root.kids[0].central && !p.root.kids[1].central, "only the first central leaf stays central");
    Plan q;
    q.root = split(0, 0.5f, leaf({"Browser"}), leaf({"Playlist", "Piano roll"}));
    dockplan::sanitise(q, known);
    expect(!q.root.kids[0].central && q.root.kids[1].central, "without one, the fullest leaf becomes the main area");
  }
  // ---- a front that is not in its leaf ----
  {
    Plan p;
    p.root = leaf({"Playlist"}, true, "Mixer");
    dockplan::sanitise(p, known);
    expect(p.root.front.empty(), "a front window not in its leaf is cleared");
  }
  // ---- broken splits and depth ----
  {
    Plan p = dockplan::fromJson(R"({"tree":{"split":"x","ratio":0.3,"kids":[{"windows":["Browser"]}]}})");
    dockplan::sanitise(p, known);
    expect(!p.root.split && p.root.central, "a split with one child reads as a leaf");
    Node deep = leaf({"Playlist"}, true);
    for (int i = 0; i < 30; ++i) deep = split(i % 2, 0.5f, leaf({}), deep);
    Plan d;
    d.root = deep;
    d.root.kids[0] = leaf({"Browser"});
    dockplan::sanitise(d, known);
    const auto all = dockplan::windows(d);
    expect(all.size() == 2, "a very deep tree keeps its windows");
  }
  // ---- floating rectangles ----
  {
    Plan p;
    p.root = leaf({"Playlist"}, true);
    p.floating.push_back({"Mixer", std::numeric_limits<float>::infinity(), -5, 3, 1e9f});
    dockplan::sanitise(p, known);
    expect(p.floating[0].x == 0 && p.floating[0].w == 60 && p.floating[0].h == 20000, "a floating rectangle is made finite and sane");
    const dockplan::Float f = dockplan::fitInto({"Mixer", 1300, 700, 400, 300}, 1440, 800);
    expect(f.x == 1040 && f.y == 500 && f.w == 400, "fitInto moves a window back inside the dock space");
    const dockplan::Float g = dockplan::fitInto({"Mixer", -50, 10, 2000, 300}, 1280, 700);
    expect(g.x == 0 && g.w == 1280, "fitInto shrinks a window wider than the dock space");
  }
  return finish();
}
