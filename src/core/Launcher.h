// Gridline - J-141: the welcome launcher's logic, free of any UI so it is unit tested:
//   * the start-up plan (what opens, and whether the launcher shows, for each way Gridline
//     is started),
//   * the launcher's state machine (shown / hidden, with or without an open project),
//   * its layout: rail, header and the tile grid for a window size (logical pixels).
// The drawing lives in src/app/LauncherUi.cpp.
#pragma once
#include <string>
#include <vector>

namespace gl::launcher {

// ---------------------------------------------------------------- start-up plan
enum class StartAction { None, Demo, Template, Empty, Last };

struct StartupInputs {
  bool plainLaunch = true;     // no command-line arguments (file paths or options)
  bool forceLauncher = false;  // --show launcher
  bool freshInstall = false;   // no settings file existed
  bool demoShown = false;      // general.demoShown
  bool launcherOn = true;      // general.launcher (Settings > General)
  std::string startupMode = "template";  // general.startup: template | empty | last
  bool lastExists = false;     // the newest recent project is on disk
  bool defaultTemplate = false;  // general.defaultTemplate is set
};

struct StartupPlan {
  StartAction action = StartAction::None;  // None: keep the project App made (starter / default template)
  bool showLauncher = false;
  bool markDemoShown = false;  // set general.demoShown (and save the settings)
};

StartupPlan planStartup(const StartupInputs& in);

// ---------------------------------------------------------------- state machine
// The launcher is the application's home: shown over (instead of) the project view.
// `projectOpen` is false while nothing the user chose is open (the start-up
// placeholder project, or after Close project): then the launcher cannot be left
// except by opening or creating a project.
class State {
 public:
  bool visible() const { return visible_; }
  bool projectOpen() const { return projectOpen_; }
  // Seconds stamp of the last change of visibility (motion).
  double changedAt() const { return changedAt_; }

  void showAtStartup(double now = 0);   // shown, no project open
  void showHome(double now = 0);        // the logo: shown, the open project kept
  bool back(double now = 0);            // hidden again; false (still shown) when no project is open
  void projectOpened(double now = 0);   // new / open / template / demo / file event: hidden, project open
  void projectClosed(double now = 0);   // Close project: shown, no project

 private:
  void set(bool v, double now);
  bool visible_ = false;
  bool projectOpen_ = true;
  double changedAt_ = -1;
};

// ---------------------------------------------------------------- layout
struct Rect {
  float x = 0, y = 0, w = 0, h = 0;
  float r() const { return x + w; }
  float b() const { return y + h; }
  bool overlaps(const Rect& o) const { return x < o.r() && o.x < r() && y < o.b() && o.y < b(); }
  bool inside(const Rect& o, float eps = 0.01f) const {
    return x >= o.x - eps && y >= o.y - eps && r() <= o.r() + eps && b() <= o.b() + eps;
  }
};

// Grid items in order: tiles, and section headers (a full row label, then a new row).
enum class Item { Tile, Section };

// Rail rows (wide layout): brand, actions, the library label, views, settings, version.
enum class RailRow { Brand, Action, Label, View, Settings, Version };

struct LayoutIn {
  float width = 1440, height = 900;  // window, logical px
  std::vector<Item> items;
  bool hasBack = false;        // "Back to project" shown
  float titleW = 160;          // page title text width
  float backW = 180;           // back button width (text + icon + padding)
  float brandW = 120;          // lockup width
  int actions = 3;             // rail action rows
  int views = 2;               // rail view rows
  float scrollbar = 10;        // room the grid's scrollbar takes when the tiles overflow
};

// Tile metrics (design tokens).
inline constexpr float kRailW = 232, kRailPad = 20, kRailRowH = 34, kNarrowBelow = 880;
inline constexpr float kPad = 36, kPadNarrow = 16, kHeaderH = 44, kGap = 20;
inline constexpr float kTileMin = 232, kTileMax = 320, kThumbRatio = 0.6f, kCaptionH = 62;
inline constexpr float kSectionH = 34, kSearchW = 240, kSearchMin = 120, kIconBtn = 32;

struct Layout {
  bool rail = true;            // wide: the left rail; narrow: an icon row in the header
  Rect railRect;
  std::vector<std::pair<RailRow, Rect>> railRows;
  Rect brand;                  // narrow: the lockup in the header
  std::vector<Rect> headerIcons;  // narrow: icon buttons (actions, views, settings)
  Rect title, search, back;    // back.w == 0: none
  Rect grid;                   // the scrolling grid's viewport
  int cols = 1;
  float tileW = 0, tileH = 0, thumbH = 0;
  std::vector<Rect> items;     // per LayoutIn::items, in grid content coordinates (0,0 = grid top-left, unscrolled)
  float contentH = 0;          // grid content height
};

Layout layout(const LayoutIn& in);

}  // namespace gl::launcher
