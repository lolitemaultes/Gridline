// Gridline — hosted (VST3) plugins in the app: per-frame sync with the song,
// native editor windows, and the generic parameter editor.
#pragma once
#include <string>
#include "PluginUI.h"

union SDL_Event;

namespace gl {

class App;
namespace host {
class HostedInstance;
}

// Running hosted plugin behind a channel instrument / mixer slot (nullptr when
// not hosted, not instantiated or missing).
host::HostedInstance* hostedInstanceFor(App& a, const PluginTarget& t);

// Once per frame: edits made in plugin editors become song edits, plugin
// state changes are captured into the song (throttled), editor windows that
// lost their plugin close, the Linux plugin run loop runs.
void pumpHostedPlugins(App& a);
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
bool hostedEditorOpen(const PluginTarget& t);
// J-111g: the plugin window the mouse pointer is over (collaborators' cursors):
// its plugin and the pointer's fraction of the window. False when none.
bool hostedEditorUnderMouse(PluginTarget& t, float& fx, float& fy);
void closeHostedEditor(App& a, const PluginTarget& t);
void closeAllHostedEditors(App& a);
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
// Page a plugin window opens on (0 parameters, 1 processing, 2 settings; dev checks).
void setHostedWrapperPage(int page);

}  // namespace gl
