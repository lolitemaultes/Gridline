// Gridline - the control bars' model in the app (core/Toolbar.h): which parts each layout's bar shows, in
// which order, and the pinned commands; the right-click bar list; pinning from the command palette.
//
// The model a bar draws is its layout's, or the active workspace's own bar and pins when that workspace
// keeps them (core/Workspaces.h). Edits go where the model came from. Everything is per person
// (settings.json), never in a project and never sent to collaborators.
#pragma once
#include <string>
#include <vector>
#include "../core/LayoutPreset.h"
#include "../core/Toolbar.h"
#include "Widgets.h"

namespace gl {
class App;
}

namespace gl::toolbarui {

// The model of the active layout's bar now (cached for the frame).
const toolbar::Model& model(App& a);
// Any layout's model as the bar uses it (its active workspace's own bar and pins when it has them).
toolbar::Model modelFor(App& a, layout::Preset p);
// The layout's own model (never a workspace's).
toolbar::Model layoutModel(App& a, layout::Preset p);
// Something changed (a workspace switch, a settings edit): read the model again.
void changed();

bool shown(App& a, const char* part);   // the active layout's bar shows this part
bool always(App& a, const char* part);  // ... and keeps it when the window is narrow

// Edits of a layout's bar or pins: the active workspace's own when it keeps its own, else the layout's.
// Each is one settings write.
void setBar(App& a, layout::Preset p, const toolbar::Bar& b);
void setPins(App& a, layout::Preset p, const std::vector<toolbar::Pin>& pins);
bool barIsOwn(App& a, layout::Preset p);
bool pinsAreOwn(App& a, layout::Preset p);
void resetBar(App& a, layout::Preset p);

// ---- pins ----
// Pins (or unpins) a command palette row in the active layout; false when it cannot be pinned.
bool togglePin(App& a, const std::string& command, const std::string& label);
bool isPinned(App& a, const std::string& command);
// The commands the palette lists first, in order.
std::vector<std::string> paletteFirst(App& a);
// The pins the active layout's bar draws (where says the bar), in order.
std::vector<toolbar::Pin> barPins(App& a);
// A pin's name as shown, and its glyph.
std::string pinLabel(const toolbar::Pin& p);
ui::Icon pinIcon(const toolbar::Pin& p);
// Runs a pinned command (as the palette's Enter does; the next frame). False when it is not there now.
bool runPin(App& a, const toolbar::Pin& p);
// The right-click menu of a pinned button: show as label or icon, where it shows, move, unpin.
void pinMenu(App& a, const toolbar::Pin& p);

// ---- the bar list (right-click an empty bit of any layout's bar) ----
void openBarList(App& a);
// Drawn by each bar, inside its window (opens where it was asked).
void drawBarList(App& a);
bool barListOpen();

// Dragging parts into a new order (the bar list and the Layouts settings page). A list keeps one of
// these across frames. Each frame: row() after the item that covers a row's grip and name (a drag on
// it starts moving that part; clicks and right-clicks on it work as before), then finish() after the
// last row: it draws the insertion line where the part will land (between two rows of its group, or
// after the last one) and, when the mouse is let go, moves the part there. Only movable parts drag:
// inside their own group, or into another free side of the bar (Studio's left and right, Clips' three
// groups; toolbar::canMoveInto). heading() before a group's rows marks where that group starts, so a
// part dragged over it (or over a group that has no parts left) lands in that group.
class PartDrag {
 public:
  // `label`: the name shown on the row (drawn on the ghost that follows the pointer while it drags).
  void row(const std::string& section, const std::string& id, const char* label, bool movable, ImVec2 r0, ImVec2 r1);
  // A group's heading (and, for an empty group, the space under it a part can be dropped on).
  void heading(const std::string& section, const char* label, ImVec2 r0, ImVec2 r1);
  // `x0`, `x1`: how wide the line is drawn. True when the bar changed.
  bool finish(layout::Preset p, toolbar::Bar& bar, ImDrawList* dl, float x0, float x1);
  // The part being dragged ("" when none), and the group it would land in now.
  const std::string& dragging() const { return id_; }
  const std::string& target() const { return target_; }
  void cancel() { id_.clear(); }
  // Dev: the rows of the last frame ("<section> <id> <x0> <y0> <x1> <y1>") and the drag's state.
  std::string devRows() const;

 private:
  struct Row {
    std::string section, id, label;
    bool movable = false;
    ImVec2 r0, r1;
    bool heading = false;
  };
  std::vector<Row> rows_, last_;
  std::string id_, section_, target_;
  std::string devLine_;
  int frame_ = -1;  // the last frame finish() ran (a list not drawn for a frame lets go of its drag)
};

// Dev: each bar notes where it drew its parts this frame (the side it laid them in, from x0 to x1) and
// the edges its left and right groups are laid from ("toolbar parts" prints them, for alignment checks).
void notePart(const std::string& id, const std::string& side, float x0, float x1);
void noteBarEdges(float left, float right);

// Dev: "toolbar dump|parts|hide <part>|show <part>|move <part> <dir>|pin <command> [label]|unpin <command>|run <n>|list|reset|always <part>|rows".
void devCommand(App& a, const std::vector<std::string>& args);

}  // namespace gl::toolbarui
