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

// K-447: only what the user chose is split: the clip's own part of its file (a slice, a trim, a
// piece the split or cut tool left), and of that only song ticks [t0, t1) when t1 > t0 (a time
// selection, the clip editor's selection). The stems land lined up with exactly that part.
void openForClip(App& a, ObjectId clip, Tick t0 = 0, Tick t1 = 0);
// A file (the browser: all of it), or seconds [start, end) of it when end > start (Loupe's selection).
void openForFile(App& a, const std::string& path, const std::string& name = "", double start = 0, double end = 0);
// The "Split stems..." item of an audio clip's menus: the clip's part inside the playlist's time
// selection (or the Marquee tool's area on its track) when one covers it, else the whole clip.
void clipMenuItem(App& a, ObjectId clip);
// The song ticks of `clip` that clipMenuItem splits (t1 <= t0: the whole clip).
void clipMenuRange(const App& a, ObjectId clip, Tick& t0, Tick& t1);
// Every frame (from drawDialogs).
void draw(App& a);
// App exit: a running split or download is cancelled and joined.
void shutdown();

}  // namespace stemsui

// "<project>/Stems" (unsaved project: the config folder's Stems).
std::string stemsDir(const App& a);

}  // namespace gl
