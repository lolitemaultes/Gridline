// Gridline - the control bar of each layout as a model: its parts in order and the pinned commands.
//
// Each layout's bar is made of built-in parts (the transport, the tempo, the window buttons ...). A part
// lives in one section of its bar (Studio: the left and the right side; Clips: left, middle and right;
// Smooth: the transport and its switches; the Tracker has no free bar). A person can switch parts off,
// reorder the movable ones inside their section and mark a part "always shown" (it stays when the window
// gets narrow). Where a layout's bar has free sides (Studio's left and right, Clips' left, middle and right),
// a movable part may also move to another of them; its saved part then names the side it stands in.
// Pinned commands are buttons for any command or menu item of the command palette, drawn
// where the bar's "pins" part stands (the Tracker draws them in its header's button grid) and, if wanted,
// listed at the top of the palette.
//
// The defaults reproduce each layout's bar as it was before it could be customised, so nothing changes
// until a person changes it. Saved per person in settings.json as "layout.<id>.toolbar", never in a
// project (a workspace may keep its own copy, core/Workspaces.h).
//
// Pure data and rules, no ImGui; tested alone (tests/test_k256_toolbar.cpp).
#pragma once
#include <functional>
#include <string>
#include <vector>
#include "LayoutPreset.h"

namespace gl::toolbar {

// A built-in part of a layout's bar.
struct PartInfo {
  const char* id;        // stable: "transport", "display.tempo", ...
  const char* label;     // English, translatable
  const char* section;   // the section id it lives in ("left", "right", "middle", "switches", ...)
  int fitLevel;          // narrow windows: parts leave in rising levels (all of one level together); 0 = never leaves
  bool movable;          // may be reordered inside its section
  bool shown;            // on by default
};

// The sections of a layout's bar in drawing order (English, translatable labels).
struct SectionInfo {
  const char* id;
  const char* label;
  bool open = false;  // a free side: movable parts of the other free sides may move into it
};

const std::vector<PartInfo>& parts(layout::Preset p);
const std::vector<SectionInfo>& sections(layout::Preset p);
const PartInfo* part(layout::Preset p, const std::string& id);

// The pins part: where the pinned commands stand on the bar.
constexpr const char* kPins = "pins";

struct Part {
  std::string id;
  bool shown = true;
  bool always = false;  // stays when the window is narrow
  std::string section;  // the side it was moved to ("" = its own, the default one)
  bool operator==(const Part& o) const { return id == o.id && shown == o.shown && always == o.always && section == o.section; }
};

enum class Where : int { Both = 0, Bar = 1, Palette = 2 };
const char* whereName(Where w);  // English, translatable: "Bar and palette", "Bar only", "Palette only"

struct Pin {
  std::string command;  // a command palette row id ("cmd:edit.undo", "menu:...")
  std::string label;    // as it was shown when pinned (English when it had one): used while the row is not there
  bool icon = false;    // drawn as its icon only (else its name)
  Where where = Where::Both;
  bool operator==(const Pin& o) const { return command == o.command && label == o.label && icon == o.icon && where == o.where; }
  bool onBar() const { return where != Where::Palette; }
  bool inPalette() const { return where != Where::Bar; }
};

struct Bar {
  std::vector<Part> parts;  // every built-in part of the layout, in order
  bool operator==(const Bar& o) const { return parts == o.parts; }
};

constexpr int kMaxPins = 24;
constexpr size_t kMaxCommand = 300;

// ---- the bar ----
Bar defaults(layout::Preset p);
// Puts a bar in order: unknown and repeated parts dropped, missing built-ins added where their default
// neighbours are (a part a later version adds shows up as it would by default), parts that cannot move
// put back in their default places, and every part kept in its own section (sections in their order).
void normalise(layout::Preset p, Bar& b);
// The parts of one section, in order (a part moved to another side is listed there).
std::vector<const Part*> sectionParts(layout::Preset p, const Bar& b, const std::string& section);
// The section a part stands in now: the side it was moved to, else its default one ("" for unknown ids).
std::string sectionOf(layout::Preset p, const Bar& b, const std::string& id);
// A part may stand in `section`: its own, or another free side when its own is a free side too.
bool canMoveInto(layout::Preset p, const std::string& id, const std::string& section);
const Part* find(const Bar& b, const std::string& id);
bool shown(const Bar& b, const std::string& id);  // a part is on (unknown ids: false)
bool setShown(Bar& b, const std::string& id, bool on);
bool setAlways(Bar& b, const std::string& id, bool on);
// Moves a part one place earlier or later (dir -1 / +1) inside its section, or to `to` (an index among
// its section's parts). Refused for parts that cannot move.
bool step(layout::Preset p, Bar& b, const std::string& id, int dir);
bool moveTo(layout::Preset p, Bar& b, const std::string& id, int to);
// Drag and drop: moves a part right before (`after` false) or right after (`after` true) another movable
// part of its section, where the insertion line showed. An empty or unknown `anchor` puts it at the end
// of its section. Refused for parts that cannot move; false when nothing changes.
bool moveBeside(layout::Preset p, Bar& b, const std::string& id, const std::string& anchor, bool after);
// The same into any section the part may stand in (canMoveInto): right before or after `anchor` there, or
// at the end of that section when `anchor` is empty or not in it. In its own section: moveBeside.
bool moveInto(layout::Preset p, Bar& b, const std::string& id, const std::string& section, const std::string& anchor, bool after);

// The shown parts that may leave a narrow window, in the groups they leave in (rising fit level; a part
// marked always shown and fit level 0 never leave).
std::vector<std::vector<std::string>> leaveOrder(layout::Preset p, const Bar& b);
// Narrow windows: which shown parts stay, given each part's width (`width(id)`, 0 for a part that draws
// nothing) and `room`. Parts leave by rising fit level (a level leaves all together); a part marked
// always shown, and fit level 0, never leave. `fixed` is width the bar needs whatever leaves.
// Returns the ids that leave.
std::vector<std::string> fit(layout::Preset p, const Bar& b, const std::function<float(const std::string&)>& width, float room, float fixed = 0);

// ---- pins ----
// A command can be pinned when it has an id that names a command or a menu item (not a setting, a
// window row, a plugin, or nothing).
bool pinnable(const std::string& command);
// Pins a command (at the end, or at `at`); refused when not pinnable, already pinned or the list is full.
bool pin(std::vector<Pin>& pins, const Pin& p, int at = -1);
bool unpin(std::vector<Pin>& pins, const std::string& command);
bool pinned(const std::vector<Pin>& pins, const std::string& command);
bool movePin(std::vector<Pin>& pins, int from, int to);
// Drops pins that are not pinnable or repeated, and those `exists` says are gone (when given).
void cleanPins(std::vector<Pin>& pins, const std::function<bool(const std::string&)>& exists = nullptr);
// The command ids the palette lists first (pins shown there), in order.
std::vector<std::string> paletteFirst(const std::vector<Pin>& pins);

// ---- JSON ----
std::string barJson(const Bar& b);
Bar barFromJson(layout::Preset p, const std::string& text);  // broken text: the defaults
std::string pinsJson(const std::vector<Pin>& pins);
std::vector<Pin> pinsFromJson(const std::string& text);
// The whole settings value: {"parts": [...], "pins": [...]} ("" = the defaults and no pins).
struct Model {
  Bar bar;
  std::vector<Pin> pins;
  bool operator==(const Model& o) const { return bar == o.bar && pins == o.pins; }
};
std::string toJson(layout::Preset p, const Model& m);  // "" for the defaults with no pins
Model fromJson(layout::Preset p, const std::string& text);
std::string settingsKey(layout::Preset p);  // "layout.<id>.toolbar"

// Smooth's older show / hide switches (settings "ui.cbar.*" and their defaults), read once into its
// bar. `value(key)` gives a switch the person had set (nullptr-like: false in `has` when never set).
Bar migrateSmooth(const std::function<bool(const std::string& key, bool& value)>& value);
const std::vector<std::string>& smoothLegacyKeys();

}  // namespace gl::toolbar
