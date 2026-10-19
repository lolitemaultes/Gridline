// Gridline — helpers shared by the main menu (Menus.cpp) and the context menus
// (ContextMenus.cpp). Window files only use the entry points in Windows.h.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include "../core/MenuOps.h"

namespace gl {

class App;

namespace menus {

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

// Renames whatever F2 means for the focused editor.
void renameFocused(App& a);

}  // namespace menus
}  // namespace gl
