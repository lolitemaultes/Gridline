// Gridline - K-247: the LFO board (core/Lfo.h, Song::lfos), the card rack.
//
// - drawBoard: the "LFO board" window: a card per LFO (four across, one column when narrow), or a
//   compact list in a narrow place (Studio's mixer column), each with its wave, shape, rate, restart,
//   knobs and target chips. Studio docks it as a tab beside the Mixer (a pop-out gives the full board),
//   Clips in the mixer band, Smooth in the lower pane, the Tracker as its LFO page.
// - drawDetail: the "LFO detail" window: the selected LFO's inspector (the same as the board's) with
//   the drawn shape editor (Clips' device band, a floating window elsewhere).
// - sourceMenu and friends: the LFO, Chaos and Random submenus of every knob's Modulate menu.
// - knobOverlay / controlOverlay: on a knob (or a plugin editor's control) an LFO moves: the swing as a
//   dotted arc and the live value as a dot; Alt-drag sets the amount; the drop target of an LFO's
//   drag handle (a ring on every control that can take it, "Drop: LFO 1 moves Resonance").
#pragma once
#include <string>
#include <vector>
#include "../../third_party/imgui/imgui.h"
#include "../core/Params.h"
#include "../core/Lfo.h"
#include "../core/Song.h"

namespace gl {
class App;
}

namespace gl::lfoui {

void drawBoard(App& a);
void drawDetail(App& a);
// The knob menu's Modulate submenu (packs/modulation drawLinkMenu) draws these:
// True when a board source (LFO, Chaos, Random) can move `pid`.
bool canModulate(App& a, ParamId pid);
// The submenu's name for a kind ("LFO", "Chaos", "Random"; untranslated) and whether a source of
// that kind moves `pid` now.
const char* sourceMenuName(LfoKind kind);
bool movedBy(App& a, ParamId pid, LfoKind kind);
// The body of one source submenu: the board's sources of `kind` (checked: moving `pid`; a click links
// or unlinks), then "New ..." (and, for LFOs, the presets).
void sourceMenu(App& a, ParamId pid, LfoKind kind);
// The submenu's rows: a new source of `kind` moving `pid` (one undo step; its id, kNoId: refused), and
// a click on a source (links it, or unlinks it when it moves `pid` already).
ObjectId knobNew(App& a, ParamId pid, LfoKind kind);
bool knobToggle(App& a, ParamId pid, ObjectId source);
// "<source> settings (amount)" submenus, one per board source moving `pid`; false when none.
bool linkedSettingsItems(App& a, ParamId pid);
// Removes every board source's link to `pid` (call inside an undo group); how many.
int removeLinks(App& a, ParamId pid);
// "Open the LFO board" (its shortcut), in every layout.
void openBoardItem(App& a);
// On a Knob of parameter `pid` centred at `c` with `radius`, right after its item: the marks, the
// drop target and the Alt-drag. `active`: the knob's item is held.
void knobOverlay(App& a, ParamId pid, ImVec2 c, float radius, bool active);
// The same on any control rectangle (a plugin editor's knob or slider), right after its item.
void controlOverlay(App& a, ParamId pid, ImVec2 mn, ImVec2 mx, bool active);
// True when an Alt gesture on the knob of `pid` belongs to the LFO amount (an LFO moves it).
bool altOwned(App& a, ParamId pid);
// The board colour k (0..7) as drawn on the current theme.
ImU32 colour(int k);
// Shows the board (Alt+F9, View menu): opens it, or brings it forward; again: closes it.
void toggle(App& a);
// Shows the board (never closes it): after a Browser preset made or changed a source.
void toggleOpen(App& a);
// Selects LFO `id` on the board (its card lit, the detail window's LFO).
void select(ObjectId id);
ObjectId selected();
// 0.7.9 presets of a whole source (shape, drawn points, rate, restart, knobs; never targets).
// The user's presets (read again every two seconds; refreshPresets reads them on the next call).
const std::vector<lfo::Preset>& userPresets(App& a);
void refreshPresets();
// Loads preset `p` into source `into` (one undo step), or, with kNoId, makes a new source from it
// (selected); false when refused (the board is full).
bool usePreset(App& a, const lfo::Preset& p, ObjectId into);
// Saves source `id` as a user preset `name` ("Folder/Name" makes a folder); false (and a notice) on error.
bool savePreset(App& a, ObjectId id, const std::string& name);
// Opens the "Save as a preset" popover for source `id`.
void askSavePreset(ObjectId id);
// Dev command "lfo": lfo report | new [chaos|random] | link <n> <owner> <index> [amount] | select <n> | detail |
// compact 0|1 | popout | filter <0..2> | addtarget <n> | settings <n> <owner> <index> | preset <n|new> <k> |
// presetsave <n> <name> | presets | asksave <n> | menu card|inspector|presets|new|board [n] | editshape <n> |
// tool <0|1> [grid] | selpoint <n> <k> | points <n> | editor.
void devCommand(App& a, const std::vector<std::string>& args, int frame);

}  // namespace gl::lfoui
