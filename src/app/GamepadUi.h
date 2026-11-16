// Gridline - K-199: game controllers (Steam Deck Game mode and any pad) and the command pad.
//
// SDL's game controller API opens every controller as it is connected; its buttons run the
// keymap's commands (core/Gamepad.h), so a button does what the shortcut does. Menu (or Y) opens
// the command pad: an overlay of large tiles, drawn only while open, so no layout changes.
#pragma once
#include <cstddef>
#include <string>
#include <vector>
#include "../core/Gamepad.h"

union SDL_Event;

namespace gl {
class App;
}

namespace gl::gamepadui {

// After SDL_Init: starts the controller subsystem and notes whether this is a Game mode session.
// (init and handleEvent live in GamepadSdl.cpp, the program only: plugin builds have no SDL.)
void init(App& a);
// Every SDL event: controllers added and removed, buttons and the left stick. True when used.
bool handleEvent(App& a, const SDL_Event& e);
// From the SDL side: a button pressed or let go, the left stick (axis 0 = x, 1 = y, -32768..32767),
// how many controllers are connected, and the URL opener for Steam's on-screen keyboard.
void pressButton(App& a, gamepad::Button b, bool down);
void stickAxis(App& a, int axis, int value);
void setControllers(size_t n);
void setOpenUrl(bool (*f)(const char*));
// The rules' part of init (no SDL): Game mode detection.
void initRules(App& a);
// Per frame, with the dialogs: the command run by the last button, the pad, the one-time note.
void draw(App& a);
bool padOpen();
// A Game mode session (Steam Deck Game mode, gamescope).
bool gameMode();
// The controller is listened to (input.gamepad, Game mode, a button pressed).
bool listening(App& a);
// Settings > Shortcuts: the controller's buttons, alone, with L1 held and in the note layer.
void bindingsTable(App& a, float height);
// input.dockPlugins: built-in plugin windows open docked with the main view ("auto": in a Game
// mode session, which shows one window; "on"; "off").
bool dockPlugins(App& a);
// Before Begin() of a plugin window that docks (dockPlugins): docks it with the main view as it appears.
void nextPluginWindowDocked(App& a);
// Dev input: gamepad button|down|up <sdl name> [l1] | gamepad stick up|down | gamepad chan (the selected
// channel's plugin window) | gamepad settings general|keys | gamepad state (one line to stdout).
void devCommand(App& a, const std::vector<std::string>& args, int frame);

}  // namespace gl::gamepadui
