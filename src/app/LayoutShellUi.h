// Gridline - the region shell in the interface (K-112, core/LayoutShell.h, core/LayoutHomes.h).
//
// Builds a layout's dock tree from its region tree and docks every window into its home
// region; keeps exclusive regions to one window at a time with a size per window; pins fixed
// bands after a resize; hides and shows regions (state saved per person in layout.<id>.state);
// and the switch snapshot: the last frame before a layout switch is copied to a texture and
// drawn over the new layout while it settles, then cross-faded out, so a switch never shows a
// blank or half laid out frame.
//
// Studio never goes through the shell (it keeps app/Shell.cpp buildDefaultLayout), so it stays
// exactly as it was. Live and Logic keep their K-79 arrangements for people until their phases
// ship; the region shells of Live, Logic and Tracker are development layouts (ui.layoutDev or
// --layout-dev), so nobody sees a rough layout.
#pragma once
#include <string>
#include <vector>
#include "../../third_party/imgui/imgui.h"
#include "../core/LayoutPreset.h"
#include "../core/LayoutShell.h"
#include "../core/SemanticAnchor.h"

namespace gl {
class App;
}

namespace gl::layoutui {

// ---- the window registry: the open flag of every window the shell can place ----
struct WindowFlag {
  const char* window;  // stable name (core/LayoutHomes.h Item::window)
  bool* open;          // its open flag
};
// Every window of the app with an open flag (App's show flags and the new parts').
const std::vector<WindowFlag>& windowFlags(App& a);  // built once (the flags live as long as the App)
bool* windowOpenFlag(App& a, const std::string& stableName);

// ---- development layouts ----
void setDevLayouts(bool on);  // --layout-dev: this run only
bool devLayouts(const App& a);  // ui.layoutDev or --layout-dev
// True when the layout is built from its region shell (Tracker always; Live and Logic as development layouts).
bool shellBuilt(const App& a, layout::Preset p);

// The dock file and its default-version settings key: a region shell keeps its own file
// ("layout-live-shell.ini"), so a development run never touches the K-79 arrangement's file.
std::string iniFile(const App& a, layout::Preset p);
std::string versionKey(const App& a, layout::Preset p);

// ---- building ----
// Builds the dock tree of `p` in `dockspace` (size in logical px) from its regions and the
// person's region state, and docks every homed window into its region.
void buildShell(App& a, layout::Preset p, ImGuiID dockspace, ImVec2 size);
// The dock node of a region in the last build (0 when hidden or not built).
ImGuiID regionNode(const std::string& region);
// The window that draws a Segmented region's title strip (its windows by name).
std::string stripWindowName(const std::string& region);
// The windows a shell layout opens with (the homes' openByDefault), the others in its regions closed.
void showShellWindows(App& a, layout::Preset p);
// K-297 (round 7): the windows open in the layout shown, kept for the next start (with its dock
// file): savePanes() at quit, restorePanes() once at start when that file is read. A region shell
// is built again at start from the person's region state (the splits) with these panes open; the
// Tracker's page in front comes back too. Studio's dock file keeps its own arrangement and only
// its open windows come from here. Returns false when nothing was kept for that layout.
void savePanes(App& a);
bool restorePanes(App& a, layout::Preset p);
// R02-05: Reset layout: the Tracks layout's remembered open windows are dropped (they came back
// over the default set once the confirmation dialog had closed).
void forgetLogicWindows();
// On a layout switch, before the new layout applies: windows an exclusive region hid open again
// (K-297: a region shell's panes and region state are kept for this run first).
void leaveLayout(App& a);
// K-297: on a layout switch, once the new layout is active: its region state (the designed one on its
// first visit in a run, else as it was left; its panes come back in showShellWindows), and another
// layout's new parts that only float here are closed.
void enterLayout(App& a, layout::Preset to);
// Per frame, before the windows draw: exclusive regions show one window at a time, each with its
// own size; fixed bands keep their pixel size; region state changes are saved.
void shellFrame(App& a);
// The per-person region state of a layout (from settings, or this run's under --layout).
layout::State regionState(const App& a, layout::Preset p);
void setRegionState(App& a, layout::Preset p, const layout::State& st);
// Shows or hides a region of the active layout (its windows close with it; the dock tree is rebuilt).
void setRegionVisible(App& a, const std::string& region, bool on, bool alone = true);  // alone: see layout::setVisible
bool takeShellRebuild();  // main.cpp: a region was shown or hidden, rebuild the dock tree keeping the windows
// K-237: the pane tab strips (core/PaneTabs.h). Reopens the tab closed last (in `region` when one is
// named) where it stood; steps the tabs of the pane holding the focused window (Ctrl+PageDown and
// Ctrl+PageUp). False when there was nothing to do.
bool reopenClosedTab(App& a, const std::string& region = "");
bool cyclePaneTab(App& a, int dir);
// K-237: rebuild the dock tree next frame keeping the open windows (a workspace switch set new region state).
void requestShellRebuild();
// K-112c: a fixed band grows by `px` this frame and the next (the Live device band while devices'
// whole editors show in the expanded panel above its chain). Not saved: it ends when the caller stops
// asking, and the band never takes more than 60 % of its parent.
void setBandBoost(const std::string& region, float px);
// K-106: a fixed band is exactly `px` this frame and the next (the Tracker's header: a strip while
// the pattern is extended, taller while its help shows), at most 60 % of its parent. Not saved.
// `maxShare`: the most of its parent the band may take (K-106 R6-05: the Tracker's instrument editor
// takes nearly all the lower part, as the classic one replaces the pattern).
void setBandSize(const std::string& region, float px, float maxShare = 0.6f);

// The window flags a window's Begin uses in the active layout: docked homes drop NoDocking
// and AlwaysAutoResize (R2), and a window that opens (appears) goes to its region. Studio and
// floating homes return `base` unchanged. Pass the name the window's Begin uses.
int dockFlags(const char* windowName, int base);

// ---- the switch snapshot ----
// After the frame is rendered and before it is shown: when a switch is pending, the frame is
// copied (one GL copy) to the snapshot texture. The read framebuffer is the one just drawn to.
void captureSnapshot(int fbW, int fbH);
// Over the main window: the snapshot at full strength for the two frames the new layout settles
// in, then faded out over about 120 ms. False when there is no snapshot to draw. Called every
// frame: the snapshot is dropped (texture freed) when it was not drawn on the frame right after
// it was taken, when the home screen shows, when the window was resized, or after 0.6 s.
bool drawSnapshot(double now, int fbW, int fbH, bool homeShown);
// Dev: a snapshot texture is held (never once the fade is over).
bool snapshotHeld();
// Shutdown / graphics reset: the texture is freed while the context is current.
void releaseSnapshot();
// Dev: the snapshot's alpha on the last frame (-1 when none was drawn), for the switch test.
float snapshotAlpha();

// A window whose home is Float in a region shell: where it opens. Inside the centre region, along
// its lower edge from the right, under the centre's own toolbar: the open floating parts share the
// width side by side (each at most its wanted size), so none covers another, the side regions (an
// inspector, a browser) or runs off the main window. `retile` (optional) is set when the tiling
// changed since this window was last placed (the centre resized, a side region opened, another
// floating part opened or closed): place it again. False in Studio and for every other home (the
// window keeps its own placement).
bool floatHome(App& a, const char* stableName, ImVec2 want, ImVec2* pos, ImVec2* size, bool* retile = nullptr);

// Dev report of the shell (--dev-input "shell dump"): every region's node rectangle, visibility and windows.
std::string shellReport(App& a);

// Dev checks (--dev-input "layout ..."):
//   layout dump              the shell report (regions, nodes, windows, state)
//   layout show|hide <region>
//   layout open|close <window>
//   layout state             one line: layout, selection, transport, open windows, project hash, snapshot alpha
// K-112: shows an object (core/SemanticAnchor.h) in the active layout: selects it and opens the
// window that shows it there (Studio: the rack, the mixer, the piano roll, the playlist; other
// layouts: the part their region holds). Only the selection and the view change. False when the
// object is gone.
bool reveal(App& a, const anchor::Target& t);
// The window reveal() opens for a kind in the active layout (nullptr: none).
const char* revealWindow(const App& a, anchor::Kind k);
// Per frame (dev): runs "layout inventory" (each homed window opened in turn, checked against its home).
void devFrame(App& a, int frame);
void devCommand(App& a, const std::vector<std::string>& args, int frame);
// The "layout state" line (also printed on every switch frame by --switch-check).
std::string stateLine(App& a);

}  // namespace gl::layoutui
