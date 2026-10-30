// Gridline - J-192: File > Import > Import from URL... (Ctrl+Shift+U). A popup takes a link
// (typed, Paste, or already on the clipboard), reads it (title, uploader, site, length; the items
// of a playlist), and imports the best audio as a lossless WAV into the project's "Imported"
// folder, as an audio clip on a new track at the playhead (or into the browser only).
// The first use on a computer without the tools sets them up inside the same popup.
// Logic: core/UrlImport.h, core/MediaTools.h. Behaviour: docs/import-url.md.
#pragma once
#include <string>
#include <vector>

namespace gl {
class App;
}

namespace gl::urlimportui {

// At startup: `automated` runs (screenshots, scripts, --hidden) never touch the network.
void start(bool automated);
void open(App& a);                // the menu item / shortcut
void draw(App& a);                // per frame (drawDialogs)
void settingsSection(App& a);     // Settings > Files: IMPORT FROM URL
void clipSourceInfo(App& a, const std::string& audioPath);  // clip properties: where the audio came from
std::string importedDir(const App& a);  // "<project>/Imported" (unsaved: the config folder's)
void shutdown();                  // at exit: running tools are stopped
// Dev scripts (--dev-input): "urlimport open | clipboard <text> | paste | url <text> | fetch |
// import | cancel | close | placement clip|browser | playlist first|all | trim <from> <to> | state".
void devCommand(App& a, const std::vector<std::string>& args);

}  // namespace gl::urlimportui
