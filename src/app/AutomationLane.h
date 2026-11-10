// Gridline - the piano roll's pattern automation lane (J-5, K-171): drawing
// values with a chosen curve, and selecting, moving, erasing, copying and
// simplifying its points as the automation clip editor does (the lane and the
// editor edit the same pattern automation clips, with the same operations in
// core/AutomationEdit.h). Every edit is one undo step and syncs like any other
// pattern edit.
#pragma once
#include <functional>
#include <string>
#include <vector>
#include "../../third_party/imgui/imgui.h"
#include "../core/Params.h"
#include "../core/Song.h"

namespace gl {

class App;
enum class Tool;

// Target shown in the piano roll's lower lane (owner kNoId: note properties).
ParamId& pianoAutomationLane();
// Lane-selector popup section (lanes of the pattern, add a lane, the lane's
// point commands, open in the editor, delete).
void patternLaneMenu(App& a, ObjectId pattern, ObjectId channel);
// Short label for the lane selector.
std::string patternLaneLabel(App& a);
// Draws and edits the lane inside [min, max]. `xOf`/`tickAt` map pattern ticks
// to screen x; `grid` is the snap grid (1: no snap). `canvasHovered` /
// `canvasActive` describe the piano roll canvas item the lane belongs to;
// `tool` is the piano roll's tool (Delete erases points, Select selects, the
// drawing tools draw; Zoom and Playback leave the lane alone).
void drawPatternAutomationLane(App& a, ImDrawList* dl, ImVec2 min, ImVec2 max, ObjectId pattern,
                               const std::function<float(double)>& xOf, const std::function<double(float)>& tickAt,
                               Tick grid, bool canvasHovered, bool canvasActive, Tool tool);
// The draw-curve chip in the key column under the lane name ([min, max]); opens
// its menu on a click. Returns true when it took the click.
bool drawLaneCurveChip(App& a, ImDrawList* dl, ImVec2 min, ImVec2 max, ObjectId pattern, bool canvasHovered, bool clicked);
// While the lane has the keyboard (after a click in it, while it has a
// selection), the piano roll's editing keys act on its points: delete, select
// all, deselect, copy, cut, paste, quantize, Esc. Returns true while the lane
// has the keyboard (the piano roll then leaves those keys to it).
bool automationLaneKeys(App& a, ObjectId pattern, Tick grid);
// A click elsewhere in the piano roll: the lane gives the keyboard back.
void automationLaneBlur();
// The curve the drawing tools write (Hold = stepped, Curve = linear, Smooth).
AutoShape laneDrawShape(App& a);
void setLaneDrawShape(App& a, AutoShape s);
// The automation point clipboard shared by the lane and the clip editor
// (positions relative to the first point).
std::vector<AutoPoint>& automationClipboard();
// Simplifies the lane's selected span (the whole lane with fewer than two
// selected points) to `result` curves within `tolerance` (value units): one undo
// step. The Simplify popover's Apply.
void simplifyAutomationLane(App& a, ObjectId pattern, double tolerance, AutoShape result);
// Dev check (--dev-input "alane"): the lane's state as one line.
std::string automationLaneDump(App& a, ObjectId pattern);

}  // namespace gl
