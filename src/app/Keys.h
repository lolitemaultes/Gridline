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

// Menu shortcut text: the first chord ("Ctrl+S"), nullptr when unbound.
const char* label(const char* id);
// Every chord, "Ctrl+Y / Ctrl+Shift+Z" ("" when unbound).
std::string labels(const char* id);

// Shortcut editor: while capturing, pressed() is always false and
// captureChord() reports the first non-modifier key pressed with the held
// modifiers. The editor calls setCapturing(true) every frame it shows the
// capture; it lapses by itself one frame after the editor stops drawing.
void setCapturing(bool on);
bool capturing();
bool captureChord(Chord* out);

}  // namespace gl::keys
