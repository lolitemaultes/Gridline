// Gridline - keyboard commands in the UI: every shortcut check goes through
// pressed("command.id") against the central keymap (core/Keymap.h), so the
// shortcut editor (Settings > Shortcuts) rebinds them everywhere.
#pragma once
#include <string>
#include "../../third_party/imgui/imgui.h"
#include "../core/Keymap.h"

namespace gl::keys {

ImGuiKey toImGui(Key k);
Key fromImGui(ImGuiKey k);

// Which editor has keyboard focus this frame (set by the global shortcut
// handler from the focused window). Global commands whose chord an editor
// command of that context also uses do not fire there (the editor wins).
void setFocusContext(Context c);
Context focusContext();

// K-175: the typing keyboard is on (set once a frame): the chords it plays (playsOnTypingKeyboard)
// do not run their commands, except the one that switches it off.
void setTypingKeyboard(bool on);
// K-106: the tracker pattern editor has the keyboard (set once a frame): the chords it uses itself
// (note, value and navigation keys, its block keys) run no command, so one key press does one thing.
// Space, the function keys, undo and the other Ctrl chords keep their commands.
void setEditorKeys(bool on);
bool editorTakes(const Chord& c);

// True when one of the command's chords was pressed this frame with exactly
// its modifiers (Ctrl also matches Cmd). Never true while text is typed or
// while the shortcut editor is capturing a chord.
bool pressed(const char* id);
// Like pressed(), whatever modifiers are held (Q-3: Esc cancels a drag that
// started with Shift / Ctrl held). Only for commands bound to plain keys.
bool pressedHeld(const char* id);
// True while one of the command's keys is held down (any modifiers; Q-3: FL
// makes Shift+drag copies unique when V is held as the drag ends).
bool keyDown(const char* id);

// True when exactly the modifiers of the mouse gesture modifier `id` are held
// (core/Keymap.h mouseModifiers; Ctrl also matches Cmd). False for unknown ids.
bool modifierHeld(const char* id);
// Same, but a gesture whose modifier is none (0) matches when no modifier is
// held (plain wheel / drag gestures).
bool modifierExact(const char* id);
// J-83: the gesture modifier's keys are held, other modifiers allowed too
// (Ctrl+Shift+wheel: the wheel-adjust modifier plus the fine key). False for
// gestures without a modifier.
bool modifierIncludes(const char* id);

// K-124: like pressed(), but an editor command on the same chord that only acts in a passing
// state (cancel a drag, close a tool) does not take the key from it: that state shows itself
// through the Esc bookkeeping below instead.
bool pressedPastEditor(const char* id);

// K-124: who used Esc this frame. escapeFrameBegin() runs right after ImGui::NewFrame and notes
// what was going on as the frame started (an open popup or menu, an active item or drag, a text
// field, a pressed mouse button, a shortcut capture). escapePressed() is ImGui::IsKeyPressed(Esc)
// for dialogs and tools: when it reports the key it also marks it as used. escapeBusy() is true
// when the key went to one of them (or the frame started busy), so a later command leaves it.
void escapeFrameBegin();
bool escapePressed(bool repeat = true);
void claimEscape();
bool escapeBusy();

// Menu shortcut text: the first chord ("Ctrl+S"), nullptr when unbound.
const char* label(const char* id);
// K-112B: the first chord with a modifier ("Cmd+2" where the command also has a plain "X"), else
// label(): a Window menu names the window keys, as the reference's does.
const char* modLabel(const char* id);
// Every chord, "Ctrl+Y / Ctrl+Shift+Z" ("" when unbound).
std::string labels(const char* id);
// K-297: a chord as shown in menus and Settings. On macOS the Clips set (Cmd and Ctrl folded into
// one modifier) shows it as Cmd, the key a Mac user presses; the other sets show it as before.
std::string shown(const Chord& c);
// The same for a fixed menu hint written as "Ctrl+X" (a pointer that stays valid).
const char* shownText(const char* text);
// K-93: the command whose label() returned exactly this pointer (a menu item's shortcut
// text), nullptr when it is not one (a literal or unbound).
const char* commandForLabel(const char* text);
// K-93: pressed(id), pressedPastEditor(id) and pressedHeld(id) are true during ImGui frame
// `frame` (the command palette runs a command that has no menu item this way).
void inject(const char* id, int frame);

// Shortcut editor: while capturing, pressed() is always false and
// captureChord() reports the first non-modifier key pressed with the held
// modifiers. The editor calls setCapturing(true) every frame it shows the
// capture; it lapses by itself one frame after the editor stops drawing.
void setCapturing(bool on);
bool capturing();
bool captureChord(Chord* out);

}  // namespace gl::keys
