// Gridline - J-191: "Split stems...": separates an audio clip, a browser sample or the audio
// editor's sound into vocals, drums, bass and other (guitar and piano with the six-stem model)
// on a background thread. The window picks the model (downloaded on first use, with its
// licence note), the stems to keep and where they go: new tracks under the clip, aligned with
// it (one undo step), or files in the project's Stems folder. Progress with time left; Cancel.
#pragma once
#include <string>
#include "../core/Types.h"

namespace gl {

class App;

namespace stemsui {

void openForClip(App& a, ObjectId clip);
void openForFile(App& a, const std::string& path, const std::string& name = "");
// The "Split stems..." item of an audio clip's menus.
void clipMenuItem(App& a, ObjectId clip);
// Every frame (from drawDialogs).
void draw(App& a);
// App exit: a running split or download is cancelled and joined.
void shutdown();

}  // namespace stemsui

// "<project>/Stems" (unsaved project: the config folder's Stems).
std::string stemsDir(const App& a);

}  // namespace gl
