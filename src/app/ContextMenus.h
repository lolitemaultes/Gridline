// Gridline — helpers shared by the main menu (Menus.cpp) and the context menus
// (ContextMenus.cpp). Window files only use the entry points in Windows.h.
#pragma once
#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <vector>
#include "../core/MenuOps.h"
#include "../core/Params.h"

namespace gl {

class App;

// R01-16: the Tracks layout's word for a clip menu row ("region"); other layouts keep the text.
const char* regionWord(const char* english);
// R11-10: the copy toast: "1 region copied" / "3 regions copied" (other layouts: clips).
std::string copiedText(size_t n);

namespace menus {

// Clips' and Smooth's track menus (and Smooth's Track menu in the menu bar): Duplicate without notes (the
// selected tracks, or `track`, with their instruments, mixer tracks and settings and no clips, one undo
// step) and Copy to a new pattern (the track's notes and steps from its clip in a new pattern, placed on
// the track where that clip is, one undo step; can: the track has a clip or pattern to copy from).
void trackDuplicateWithoutNotes(App& a, int track);
bool trackCanCopyToNewPattern(const App& a, int track);
void trackCopyToNewPattern(App& a, int track);

// Palette grid inside a menu; returns true and sets *out when a colour is picked.
bool colorGrid(uint32_t* out);
uint32_t randomColor();
// Inline name field inside a (sub)menu; calls ok(name) on Enter and closes the menu.
void renameField(const std::string& current, const std::function<void(const std::string&)>& ok);

// Modal one-line text prompt (F2 rename, template names, counts).
void openPrompt(const std::string& title, const std::string& initial, std::function<void(const std::string&)> ok);
void drawPrompt();

// File dialog shortcut (defaults to the project folder or $HOME).
void fileDialog(App& a, const char* title, bool save, std::vector<std::string> exts, const std::string& name,
                std::function<void(const std::string&)> ok);

// Piano-roll edits on (current pattern, selected channel), keeping the piano roll selection.
bool pianoHasTarget(const App& a);
bool pianoEdit(App& a, const char* label, const std::function<void(std::vector<Note>&, ops::NoteSel&)>& fn);
size_t pianoSelectionCount(const App& a);
// Tools / selection items shared by the Tools menu and the piano roll menu.
void pianoToolItems(App& a);
void pianoSelectItems(App& a);
void progressionItems(App& a, bool riff);

// Edit commands routed to the focused editor (App::editFocus).
const char* editTargetName(const App& a);
void editCut(App& a);
void editCopy(App& a);
void editPaste(App& a);
void editDelete(App& a);
void editSelectAll(App& a);
void editDeselect(App& a);
void editDuplicate(App& a);
bool canPaste(const App& a);

// Instrument / effect pickers grouped by browser category.
void instrumentItems(const std::function<void(const std::string& type)>& pick, const std::string& current = "");
void effectItems(const std::function<void(const std::string& type)>& pick, const std::string& current = "");

// K-125: the instrument picks of the channel rack's menus and plugin menus: a new channel (after
// `after`; kNoId: after the selected channel) or a replaced instrument. Its editor opens at once.
ObjectId addInstrumentPick(App& a, const std::string& type, ObjectId after = kNoId);
bool replaceInstrumentPick(App& a, ObjectId channel, const std::string& type);

// Renames whatever F2 means for the focused editor.
void renameFocused(App& a);

// K-425: deletes one or several channels in 1 undo step (1 confirmation when deleting several),
// cleaning up UI selection and routing/pattern references.
void deleteChannels(App& a, const std::vector<ObjectId>& ids, const char* pluralNoun = nullptr);

}  // namespace menus

// K-112c: the Live knob menu's rows. knobNewLane: an automation clip for `pid` on a new playlist
// track; knobMutedAutomation: `pid`'s muted automation clips on the playlist; knobReenable: unmutes
// them (how many); knobCopyName: the control's name to the clipboard.
ObjectId knobNewLane(App& a, ParamId pid);
std::set<ObjectId> knobMutedAutomation(const App& a, ParamId pid);
int knobReenable(App& a, ParamId pid);
void knobCopyName(App& a, ParamId pid);
}  // namespace gl
