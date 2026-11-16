// Gridline - K-199: playing Gridline with a game controller (Steam Deck Game mode and any pad).
//
// Pure rules, no SDL and no ImGui: whether the session is a Game mode session, which command a
// controller button runs (with the L1 layer), the command pad's tiles and how its lit tile moves.
// Commands are the keymap's ids ("transport.play", "edit.undo", ...), so a button runs exactly
// what the shortcut runs. A few are the pad's own ("pad.toggle", "pad.close", "pad.nextView",
// "pad.prevView", "pad.prevChannel", "pad.nextChannel", "pad.notes", "pad.osk"), handled by the
// app (GamepadUi).
#pragma once
#include <array>
#include <functional>
#include <string>
#include <vector>

namespace gl::gamepad {

enum class Button {
  A, B, X, Y,
  Back, Guide, Start,
  LeftStick, RightStick,
  L1, R1,
  Up, Down, Left, Right,
  Count
};

// SDL's own names ("a", "b", "back", "start", "leftshoulder", "dpup", ...); Count when unknown.
Button buttonFromName(const std::string& name);
const char* buttonName(Button b);
// What the button is called on the controller, for tiles and hints ("A", "Menu", "L3").
const char* buttonLabel(Button b);

// Game mode: Steam on the Deck or a gamescope session. `env` returns the variable or nullptr.
bool gameMode(const std::function<const char*(const char*)>& env);

// input.gamepad: "auto" (on in Game mode, and as soon as a controller button is pressed), "on", "off".
enum class Mode { Auto, On, Off };
Mode modeFromSetting(const std::string& s);
bool listening(Mode m, bool gameModeSession, bool controllerUsed);

struct Binding {
  Button button;
  bool withL1;
  const char* command;
};
const std::vector<Binding>& defaultBindings();
// The command a button press runs: the L1 layer while L1 is held (falling back to the plain
// binding when the layer has none). With the pad open, A runs the lit tile, B and Menu close it
// and the D-pad moves (the app handles those); "" when the button does nothing.
std::string commandFor(Button b, bool l1Held);
// The pad's own commands: true for "pad.*" ids this module defines.
bool isPadCommand(const std::string& id);
// English name of a pad command ("Next window"), nullptr for other ids.
const char* padCommandLabel(const std::string& id);

// ---- the note-entry layer (R1 or the pad's Note entry tile turn it on and off)
// While it is on, the D-pad and the face buttons play eight notes of the scale: Left, Down,
// Right, Up, X, A, B, Y = degrees 1 to 8 (8 is the octave). L1 raises a note a semitone; the
// left stick up and down moves the octave. Menu, View, R1, the sticks' clicks and L1 keep their
// jobs. `scale` holds the semitones of the scale's degrees from its root (7 of them; empty =
// major), `root` its pitch class.
bool playsNote(Button b);
int noteFor(Button b, bool l1Held, int octave, int root = 0, const std::vector<int>& scale = {});
constexpr int kDefaultOctave = 5;  // the C at MIDI note 60
constexpr int kMinOctave = 1, kMaxOctave = 8;

// ---- the command pad: 3 rows of 5 tiles
constexpr int kPadCols = 5, kPadRows = 3, kPadTiles = kPadCols * kPadRows;
struct Tile {
  const char* label;    // English, translated when drawn
  const char* command;  // keymap id or a pad command
};
const std::array<Tile, kPadTiles>& padTiles();
// The lit tile after a D-pad or stick move: left and right wrap within the row, up and down stop
// at the first and last rows.
int padMove(int tile, int dx, int dy);

// The rows of the controller list on Settings > Keyboard: a button, what it runs alone, with L1
// held and in the note layer (English; "" = nothing).
struct BindingRow {
  Button button;
  std::string alone, withL1, notes;
};
std::vector<BindingRow> bindingRows(const std::function<const char*(const std::string&)>& commandLabel);

// The windows the D-pad steps through (view commands), and the next one from `current`
// (-1 when unknown: the first).
const std::vector<const char*>& viewCycle();
int nextView(int current, int dir);

}  // namespace gl::gamepad
