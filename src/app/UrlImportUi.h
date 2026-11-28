// Gridline - J-192: File > Import > Import from URL... (Ctrl+Shift+U). A popup takes a link
// (typed, Paste, or already on the clipboard), reads it (title, uploader, site, length; the items
// of a playlist), and imports the best audio as a lossless WAV into the project's "Imported"
// folder, as an audio clip on a new track at the playhead (or into the browser only).
// The first use on a computer without the tools sets them up inside the same popup.
// Logic: core/UrlImport.h, core/MediaTools.h. Behaviour: docs/import-url.md.
#pragma once
#include <functional>
#include <string>
#include <vector>
#include "PluginUI.h"

namespace gl {
class App;
struct PluginState;
}

namespace gl {
// Abacus: a file into a pad's layer (-1: the pad and layer selected in its editor), one undo step.
// The editor's Load button, a drop on the layer and Import from URL all use it.
bool padsLoadSample(App& a, const PluginTarget& t, const std::string& path, int pad = -1, int layer = -1);
}  // namespace gl

namespace gl::urlimportui {

// K-408: optional target for the reusable URL import popup. When `load` is set, the popup
// defaults to loading the imported lossless WAV straight into that plugin through its existing
// load path (preserving auto-chop, deck choice, wavetable/IR import, undo and collab), while
// keeping every File > Import from URL option (trim range, playlist picker, source metadata).
struct Target {
  std::string label;  // human-readable plugin / deck name shown in the popup
  std::function<void(App&, const std::string& wavPath, const std::string& title)> load;
  bool multiFile = false;  // true for multi-sample targets (Tracker Sampler, Multisampler)
};

// At startup: `automated` runs (screenshots, scripts, --hidden) never touch the network.
void start(bool automated);
void open(App& a);                       // File > Import > Import from URL (playlist / browser)
void openFor(App& a, Target target);     // K-408: open targeting a specific plugin / slot
// K-408: complete audit list of every audio-loading native plugin type.
bool pluginLoadsAudio(const std::string& type);
const std::vector<std::string>& audioLoadingPluginTypes();
// K-408: opens the URL import popup targeting plugin `t` (using its exact native load path).
// `subTarget`: plugin-specific selector (e.g. Platter deck 0=A/1=B, Plasma 0=sample/1=wavetable).
bool openForPlugin(App& a, const PluginTarget& t, int subTarget = 0);
// Kerf, Granular and Tercet keep their file as the plugin's data: the load their Load buttons use.
void loadPluginSample(App& a, const PluginTarget& t, const std::string& path);
// K-408: extracts the primary audio file path stored by an audio-loading plugin (for source info).
std::string pluginAudioPath(const PluginState& st);
// The Add menu's "Audio from URL as sampler...": each imported file a new sampler channel.
void openForNewSampler(App& a);
// K-408: records the focused audio-loading plugin window so Ctrl+Shift+U targets it.
void noteFocusedPlugin(const PluginTarget& t);
bool openForFocusedPlugin(App& a);

void draw(App& a);                // per frame (drawDialogs)
void settingsSection(App& a);     // Settings > Files: IMPORT FROM URL
void clipSourceInfo(App& a, const std::string& audioPath);  // clip / plugin properties: where the audio came from
std::string importedDir(const App& a);  // "<project>/Imported" (unsaved: the config folder's)
void shutdown();                  // at exit: running tools are stopped
// Dev scripts (--dev-input): "urlimport open | forplugin <ch> [sub] | newsampler | has <ch> <text> | clipboard <text> | paste | url <text> | fetch |
// import | cancel | close | placement clip|browser|plugin | playlist first|all | trim <from> <to> | state".
void devCommand(App& a, const std::vector<std::string>& args);

}  // namespace gl::urlimportui
