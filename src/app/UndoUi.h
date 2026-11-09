#pragma once
// Undo and redo in the interface: the Undo / Redo shortcuts (never while a menu or a dialog is
// open; a press made while an edit is still open waits for it), editors with a history of their
// own (they take the press while focused and their history has a step), the selection kept with
// each step, and the history panel (with a collaborative project's marks and, when asked for,
// collaborators' edits).

namespace gl {
class App;
}

namespace gl::undoui {

// Wires the document's history callbacks (selection kept with each step). Once, at start.
void install(App& a);
// True when this frame's Undo (dir -1) or Redo (dir +1) shortcut was pressed for an editor with
// a history of its own; marks the press as taken so the project's history leaves it.
bool editorPressed(int dir);
// The project's shortcuts, once per frame (from handleGlobalShortcuts).
void frame(App& a);
// Undo / redo of the project's history from a menu or a button: false when nothing moved.
bool undo(App& a);
bool redo(App& a);
// When the last undo / redo happened (App::now); a plugin's state reported within kFollowSeconds
// of it follows what the undo restored instead of being an edit of its own.
double lastMoveAt();
constexpr double kFollowSeconds = 1.5;
// The history panel (View > Undo history).
void historyWindow(App& a);

}  // namespace gl::undoui
