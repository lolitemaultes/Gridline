// Gridline: the "Audio tools" submenu of an audio clip's menu, the same in every layout (Studio's
// playlist, the Clips arrangement and clip slots, Smooth's regions, the Tracker's clip pages), with
// Loupe's tools: Clean up, Declick, Declip, Deverb, Noise gate, Equalize, Reverb, Blur, Normalize,
// Amplify, Fade in, Fade out, Reverse and Drum loop stretch. Reverse runs at once (nothing to set,
// its progress shown only when it takes a moment); every other pick opens the tool's window with
// Loupe's own panel (the same settings and presets), its Preview (A processed, B original,
// C removed) and an Accept that runs with progress and Stop. The clicked clip, or every selected
// audio clip with it, gets a new file of its own (engine/ClipTools.h); one undo step.
#pragma once
#include <string>
#include <vector>
#include "../core/Types.h"

namespace gl {

class App;

namespace cliptoolsui {

// The "Audio tools" submenu for `clip` (and the audio clips selected with it).
void clipMenu(App& a, ObjectId clip);
// Opens the window of tool `kind` (packs::edison::ToolKind) on `clip` and its selection (Reverse:
// runNow).
void open(App& a, ObjectId clip, int kind);
// Runs tool `kind` with its own settings on `clip` and the audio clips selected with it, no window.
void runNow(App& a, ObjectId clip, int kind);
// Every frame (from drawDialogs).
void draw(App& a);
// App exit: a running job is stopped and waited for.
void shutdown();
// Dev: "cliptools state | open <clip> <tool> | scope whole|selection | accept | wait | stop | close |
// hover | click <tool> | rows | select <clip>...".
bool devCommand(App& a, const std::vector<std::string>& args, int frame);

}  // namespace cliptoolsui
}  // namespace gl
