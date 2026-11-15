// Gridline — hosted (VST3) plugins in the app: per-frame sync with the song,
// native editor windows, and the generic parameter editor.
#pragma once
#include <string>
#include <vector>
#include "PluginUI.h"

union SDL_Event;
struct SDL_Window;

namespace gl {

class App;
namespace host {
class HostedInstance;
}

// K-47: a graphics driver reset: the plugin windows' bars let go of the lost context, then come back
// on the new one (defined with the windows).
void pluginWindowsGraphicsLost();
void pluginWindowsGraphicsBack(SDL_Window* mainWindow, void* mainGl);
// K-133: then every open plugin interface is closed and opened again, so a plugin that draws with
// OpenGL of its own builds it anew. Returns how many opened again.
int reopenHostedEditorsAfterGraphicsReset(App& a);

// Running hosted plugin behind a channel instrument / mixer slot (nullptr when
// not hosted, not instantiated or missing).
host::HostedInstance* hostedInstanceFor(App& a, const PluginTarget& t);

// Once per frame: edits made in plugin editors become song edits, plugin
// state changes are captured into the song (throttled), editor windows that
// lost their plugin close, the Linux plugin run loop runs.
void pumpHostedPlugins(App& a);
// Called around the Linux plugin run loop (true before, false after), whose timers let interfaces draw:
// the application releases its own OpenGL context there when it draws through another API than the
// interfaces (Wayland, app/ForeignGl.h). Unset: nothing is done.
void setRunLoopGlBracket(void (*bracket)(bool enter));
// Writes every hosted plugin's current state into the song (before saving).
void captureHostedStates(App& a);

// The plugin's own programs (factory presets): selecting one reads the
// parameters back into the song and captures the new state.
bool selectHostedProgram(App& a, const PluginTarget& t, int index);
// Replaces the instance by a fresh one (after a crash; keeps the song's state).
void reloadHostedPlugin(App& a, const PluginTarget& t);
// "Run in sandbox" for every instance of a hosted type (plugin manager,
// plugin window); running instances are replaced.
void setHostedSandbox(App& a, const std::string& type, bool on);

// Plugin windows of installed plugins (PluginWindow.cpp). An installed
// plugin opens as itself: its own interface at the top of a Gridline window,
// a compact control bar under it (presets, bypass, sandbox, generic, CPU /
// latency, settings) and the settings pages unfolding under the bar. Only
// plugins without an interface (or with the generic editor chosen) use
// Gridline's parameter panel.
bool openHostedEditor(App& a, const PluginTarget& t);  // false (with a toast) when it cannot open
// K-112 (any layout; Studio never calls them): a plugin window set aside without closing it. It
// leaves the screen, its interface and instance keep running, and it comes back where it was when
// its last reason is cleared. Reasons are bits, so the person's "hide all" and a layout's own rule
// (only the selected track's windows) do not undo each other. Sandboxed interfaces (shown in the
// sandbox's own window) are never set aside. False: no such window, or a sandboxed one.
enum HostedEditorHide : unsigned { kHideAll = 1u, kHideTrack = 2u, kHideAny = ~0u };
bool setHostedEditorHidden(const PluginTarget& t, bool hidden, unsigned reasons = kHideAny);
bool hostedEditorHidden(const PluginTarget& t);
std::vector<PluginTarget> openHostedEditorTargets();  // every open plugin window, shown or set aside
// "Hide or show plugin windows": sets every shown window aside, or (none shown) brings back the
// ones set aside by it. Returns true when it hid them.
bool toggleHostedEditorsHidden();
// K-112: how a layout runs installed plugins' windows. A layout that wants it calls
// setPluginWindowPolicy every frame (managed = true); when no layout has called it since the last
// frame, nothing below applies and every window it set aside comes back (Studio: as before).
struct PluginWindowPolicy {
  bool managed = false;
  bool openOnLoad = true;    // a plugin put on a track here (Document::onPluginPlaced: not undo, redo or a collaborator) opens its window
  bool several = true;       // off: opening a plugin window closes the others
  bool selectedOnly = true;  // when the selected track changes, only its plugins' windows show (others set aside)
  ObjectId channel = kNoId;  // the selected track: its channel ...
  ObjectId mixer = kNoId;    // ... and the mixer track its chain runs through
};
void setPluginWindowPolicy(const PluginWindowPolicy& p);
bool pluginWindowPolicyManaged();  // a layout called setPluginWindowPolicy last frame
bool hostedEditorOpen(const PluginTarget& t);
// J-111g: the plugin window the mouse pointer is over (collaborators' cursors):
// its plugin and the pointer's fraction of the window. False when none.
bool hostedEditorUnderMouse(PluginTarget& t, float& fx, float& fy);
void closeHostedEditor(App& a, const PluginTarget& t, const char* why = nullptr);  // why: for the debug log
// K-9 dev: every open plugin window with its native window ids and the plugin's rectangle, to stdout.
void devPrintPluginWindows(int frame);
// Dev (GitHub issue 7): a drag inside the first open plugin window's interface (container point x, y,
// up and down for `seconds`), as the mouse does it (macOS). False when there is none or no support.
bool devDragHostedEditor(int x, int y, double seconds);
void closeAllHostedEditors(App& a);
// K-37: at quit, before the system windows go: interfaces still closing are given a last, bounded
// wait and their windows are destroyed now (later, the plugin instance going away would do that
// with SDL and the display already shut down).
void settleClosingHostedEditors();
// K-37: a bridged interface never came back: the helper thread is still inside the bridge, and the
// bridge's own threads end the process when its statics go. The process leaves without static teardown.
bool hostedEditorAbandoned();
// Window events for plugin windows (input to their control bar, close,
// resize). Returns true when the event belonged to one.
bool hostedEditorEvent(App& a, const SDL_Event& ev);
// After the main window is presented: draws the control bars of open plugin
// windows (each in its own GL / ImGui context; the main one is restored).
void renderHostedEditors(App& a);
// Each frame before windows are drawn: a request to show an installed
// plugin (App::settingsChannel / App::fxEditor) opens its own interface
// instead of Gridline's panel, unless the plugin has none, the generic
// editor is chosen, or the panel was asked for explicitly.
void routePluginWindows(App& a);
// The plugin window of a target is showing (native window or Gridline panel).
bool pluginWindowShown(App& a, const PluginTarget& t);
// Channel button / mixer slot: shows or hides the target's plugin window.
void togglePluginWindow(App& a, const PluginTarget& t);
// Gridline's own panel for an installed plugin (generic parameters,
// processing, settings), asked for explicitly.
void showHostedPanel(App& a, const PluginTarget& t);
// Screen capture of an open plugin window, our own window only (dev checks).
bool screenshotHostedEditor(const PluginTarget& t, const std::string& pngPath);
// Dev checks: logs requested / resulting sizes of plugin windows to stderr;
// the settings panel of new plugin windows starts unfolded on `page` (-1: folded).
void setPluginWindowLog(bool on);
void setPluginWindowUiScale(float scale);  // K-35: the main window's interface scale (each frame)
void setPluginWindowPanel(int page);
// Dev checks: plugin windows are resized to w x h (as by the user) shortly after opening.
void setPluginWindowDevResize(int w, int h);
// Dev checks: a control bar button ("generic", "sandbox", "settings") pressed shortly after opening.
void setPluginWindowDevAction(const std::string& action);
// Dev measurement (--frame-stats): plugin interfaces' drawing is counted.
void setPluginWindowDrawStats(bool on);
// Dev check (J-66): Space typed into the first plugin window's interface.
void setPluginWindowDevKeys(bool on);

// Plugin window body for hosted types: editor button, plugin info and the
// generic, searchable parameter list.
void drawHostedEditor(App& a, const PluginTarget& t, float width);
// K-112c: one generic parameter row of an installed plugin (name, value bar with the plugin's own
// value text; the Live layout's device band), and the parameter last changed in the plugin's own
// interface or such a row (-1 when none yet this session).
void hostedParamRow(App& a, const PluginTarget& t, int k, float width);
int hostedLastTouched(const PluginTarget& t);
// Page a plugin window opens on (0 parameters, 1 processing, 2 settings; dev checks).
void setHostedWrapperPage(int page);

}  // namespace gl
