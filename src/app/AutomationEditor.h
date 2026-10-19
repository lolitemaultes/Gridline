// Gridline — automation editing UI (J-5): the automation clip editor window
// (points, tension, curve modes, selection, clipboard, tools, LFO tool, shapes
// library, target picker), creating automation from any control, automation
// recording, and the pattern-automation lane of the piano roll.
#pragma once
#include <functional>
#include <string>
#include "../../third_party/imgui/imgui.h"
#include "../core/Params.h"

namespace gl {

class App;

// ---- automation clip editor window --------------------------------------------
// Per frame from the main loop (draws nothing while closed).
void drawAutomationEditor(App& a);
// Opens the editor on an automation clip. `placement` (a playlist clip of it)
// puts the view in song time with the target's other clips as ghosts.
void openAutomationEditor(App& a, ObjectId automation, ObjectId placement = kNoId);
// Development (screenshots): 1 = the editor on a demo clip, 2 = with the LFO
// tool, 3 = a pattern-automation lane in the piano roll (--show autoedit /
// autolfo / autolane).
void devAutomationEditor(App& a, int mode);

// ---- creating automation --------------------------------------------------------
// New automation clip for a parameter: starts at the current value, is placed
// at the playback position (or over the playlist time selection) on a free
// track, and opens in the editor. Returns the AutomationClip id.
ObjectId createAutomationClip(App& a, ParamId pid);
// Shows the parameter's pattern-automation lane of the current pattern in the
// piano roll (created at the current value when missing).
void editPatternEvents(App& a, ParamId pid);
// Knob / control context-menu section: create clip, edit clips, edit events,
// start the song from this value.
void automationMenuItems(App& a, ParamId pid);

// ---- recording -------------------------------------------------------------------
// Record toggle: while playing, moving a control writes automation (song mode:
// into the clip under the playhead, or a new clip; pattern mode: the current
// pattern's lane). Each gesture becomes one thinned undo step.
bool& automationRecording();

// ---- piano roll: pattern automation lane ------------------------------------------
// Target shown in the piano roll's lower lane (owner kNoId: note properties).
ParamId& pianoAutomationLane();
// Lane-selector popup section (lanes of the pattern, add a lane, delete).
void patternLaneMenu(App& a, ObjectId pattern, ObjectId channel);
// Short label for the lane selector.
std::string patternLaneLabel(App& a);
// Draws and edits the lane inside [min, max]. `xOf`/`tickAt` map pattern ticks
// to screen x; `grid` is the snap grid. `canvasHovered` / `canvasActive`
// describe the piano roll canvas item the lane belongs to.
void drawPatternAutomationLane(App& a, ImDrawList* dl, ImVec2 min, ImVec2 max, ObjectId pattern,
                               const std::function<float(double)>& xOf, const std::function<double(float)>& tickAt,
                               Tick grid, bool canvasHovered, bool canvasActive);

}  // namespace gl
