// Gridline: the clip menus' render rows (K-250, K-251) and Send to Loupe (K-252), the same in
// every layout's clip menu (Studio and Tracker, Clips, Smooth):
//   Render clip to audio         the clip replaced in place by an audio clip of its sound
//   Render clip to file...       the clip's sound as a WAV file
// With the clip in a selection of several, each selected clip gets its own render:
//   Render each clip to audio    every selected clip replaced by its own audio clip (one undo step)
//   Render each clip to a file...  one WAV file per clip, in a folder
// The renders are engine/ClipRender's (sample for sample what the song makes over the clip's span
// with only that clip playing). Consolidate (several clips into one) stays its own row.
#pragma once
#include <set>
#include "../core/Types.h"

namespace gl {

class App;

namespace cliprenderui {

// The clips a menu opened on `clip` acts on: the playlist selection when it holds the clip, else the clip.
std::set<ObjectId> targets(const App& a, ObjectId clip);
// The render rows for `clip` (and its selection). Returns true when a row was chosen (the edit
// runs at the start of the next frame, so the caller's pointers stay valid while it draws).
bool menuItems(App& a, ObjectId clip);
// Replaces each clip of `clips` by its own render (one undo step), at the start of the next frame.
void renderInPlace(App& a, const std::set<ObjectId>& clips);
// K-252: opens an audio clip's sound in Loupe (every edit comes back to the clip's channel). The
// Smooth layout shows Loupe in its lower pane. False when the clip has no audio.
bool sendToLoupe(App& a, ObjectId clip);
// K-252: the "Send to Loupe" row of an audio clip's menu. Returns true when chosen.
bool sendToLoupeItem(App& a, ObjectId clip);

}  // namespace cliprenderui
}  // namespace gl
