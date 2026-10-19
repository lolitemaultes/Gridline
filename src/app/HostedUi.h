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

// Native plugin editor windows (the plugin's view in a separate window).
bool openHostedEditor(App& a, const PluginTarget& t);  // false (with a toast) when the plugin has no editor
bool hostedEditorOpen(const PluginTarget& t);
void closeHostedEditor(App& a, const PluginTarget& t);
void closeAllHostedEditors(App& a);
// Window events for editor windows (close button, resize). Returns true when
// the event belonged to an editor window.
bool hostedEditorEvent(App& a, const SDL_Event& ev);
// Screen capture of an open editor window (dev checks); false when unavailable.
bool screenshotHostedEditor(const PluginTarget& t, const std::string& pngPath);

// Plugin window body for hosted types: editor button, plugin info and the
// generic, searchable parameter list.
void drawHostedEditor(App& a, const PluginTarget& t, float width);
// Page a plugin window opens on (0 parameters, 1 processing, 2 settings; dev checks).
void setHostedWrapperPage(int page);

}  // namespace gl
