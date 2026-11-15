// Gridline - the region shell of a layout (K-112): a small tree of named regions.
//
// Each layout splits the dock area into named regions ("logic.inspector", "live.device",
// "tracker.header"). A region splits off one edge of its parent's remaining area, in the
// order the regions are listed; what is left of the whole area is the layout's centre
// region. Sizes are a share of the parent or fixed pixels, with limits. A region can be
// hidden (its windows close and its space goes back to its parent) and can be exclusive:
// one of the windows homed there shows at a time (the Logic lower pane shows the editors or
// the mixer), each remembering its own size.
//
// The windows each region holds come from core/LayoutHomes.h. The app builds dock nodes
// from the tree (app/LayoutShellUi.cpp); this file is pure data and geometry, tested alone.
//
// What the dock file cannot hold (hidden regions, the member shown in each exclusive region,
// member sizes) is the region State, saved per person in settings.json under
// "layout.<id>.state" (never in a project, never sent to collaborators).
//
// Studio is listed for its homes and geometry, but keeps its own dock builder
// (app/Shell.cpp buildDefaultLayout), so it stays exactly as it was.
#pragma once
#include <map>
#include <set>
#include <string>
#include <vector>
#include "LayoutPreset.h"
#include "Workspace.h"

namespace gl::layout {

enum class Edge : uint8_t { Left, Right, Top, Bottom };
// How the windows of one region are switched.
enum class TabStyle : uint8_t {
  Dock,       // dock tabs (Studio)
  Segmented,  // a segmented control in the region's title (Logic panes)
  None,       // one window, no tabs
  Pages,      // page buttons in a strip (Tracker pages)
};

struct RegionSize {
  float share = 0;     // of the parent's remaining extent along the split axis (used when px == 0)
  float px = 0;        // fixed size in logical pixels (0 = use share)
  float minPx = 0;     // never smaller than this ...
  float maxPx = 0;     // ... nor larger than this (0 = no limit) ...
  float maxShare = 0;  // ... nor this share of the parent (0 = no limit)
};

struct Region {
  const char* id;        // "<layout>.<name>"
  const char* parent;    // "" = the whole dock area
  Edge edge = Edge::Left;
  RegionSize size;
  bool visibleByDefault = true;
  bool exclusive = false;  // one of its windows shows at a time
  TabStyle tabs = TabStyle::Dock;
  bool fixed = false;      // keeps its pixel size when the window is resized (bands)
};

struct Shell {
  Preset preset = Preset::Studio;
  const char* centre = "";  // the region that gets what is left
  TabStyle centreTabs = TabStyle::Dock;  // None: the centre's window draws its own title (no dock tab)
  std::vector<Region> regions;
};
// The height of the title strip a Segmented region draws its windows' names in (logical px).
constexpr float kPaneStripH = 26.0f;

const Shell& shell(Preset p);
const Region* findRegion(Preset p, const std::string& id);
// True for the centre region or a listed region of the layout.
bool hasRegion(Preset p, const std::string& id);

// ---- per-person state of a layout's regions ----
struct State {
  std::set<std::string> hidden;                // regions hidden although visible by default
  std::set<std::string> shown;                 // regions shown although hidden by default
  std::map<std::string, std::string> member;   // exclusive region -> the window shown there
  std::map<std::string, float> memberSize;     // "<region>|<window>" -> its size along the split axis (px)
  bool operator==(const State& o) const {
    return hidden == o.hidden && shown == o.shown && member == o.member && memberSize == o.memberSize;
  }
};
bool visible(const Shell& sh, const State& st, const std::string& region);
void setVisible(const Shell& sh, State& st, const std::string& region, bool on);
// Compact JSON text for settings.json ("" for the default state) and back. Unknown or broken
// text gives the default state; unknown region ids are dropped.
std::string toJson(const State& st);
State fromJson(Preset p, const std::string& text);
std::string stateKey(Preset p);  // "layout.<id>.state"

// ---- geometry ----
struct Placed {
  std::string id;
  workspace::Rect rect;
  bool visible = false;
};
// Every region's rectangle in a w x h dock area (hidden regions: visible false, empty rect),
// the centre region last. Rectangles of visible regions never overlap and cover the area.
std::vector<Placed> solve(Preset p, float w, float h, const State& st);
// The size a region takes from its parent's extent `parent` along its split axis (px).
float regionExtent(const Region& r, float parent, const State& st, const std::string& member = "");

}  // namespace gl::layout
