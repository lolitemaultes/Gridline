// Gridline — keyboard / MIDI workflow UI entry points (MidiUi.cpp).
#pragma once
#include "../core/Params.h"
#include "../core/Song.h"

namespace gl {

class App;

void drawRemoteDialog(App& a);                            // "Remote control settings" window
void remoteLinkMenuItems(App& a, ParamId pid);            // knob / fader context menus
void actionLinkMenuItems(App& a, RemoteAction action);    // transport buttons
void recordOptionsMenu(App& a);                           // record button menu
void typingKeyboardMenu(App& a);                          // typing keyboard button menu
void midiSettingsPage(App& a, float fieldW);              // Settings → MIDI

}  // namespace gl
