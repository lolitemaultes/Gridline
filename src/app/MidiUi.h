// Gridline — keyboard / MIDI workflow UI entry points (MidiUi.cpp).
#pragma once
#include <string>
#include "../core/MidiRecord.h"
#include "../core/Params.h"
#include "../core/Song.h"

namespace gl {

class App;
class Settings;

void drawRemoteDialog(App& a);                            // "Remote control settings" window
void remoteLinkMenuItems(App& a, ParamId pid);            // knob / fader context menus
void actionLinkMenuItems(App& a, RemoteAction action);    // transport buttons
void recordOptionsMenu(App& a);                           // record button menu
void typingKeyboardMenu(App& a);                          // typing keyboard button menu
std::string typingKeyName(int key);                       // a typing key as this keyboard's layout names it
extern int (*typingLayoutKey)(int position);              // what a position types (the app sets it; plugin builds have no SDL)
// K-113: the layout typingKeyName names keys after (setting "keys.typingLayout", Standard by default).
// Keys play by where they sit whatever it is.
extern TypingLayout typingLayout;
void loadTypingLayout(const Settings& s);           // from the settings (the app at startup)
void setTypingLayout(App& a, TypingLayout l);             // and stores it in the settings
// Whether the typing keyboard is on when Gridline starts: "keys.typingAtStart" is "on", "off" or
// "last" (the default: as it was left, kept in "keys.typingOn" when Gridline closes).
bool typingKeyboardAtStart(const Settings& s);
void midiSettingsPage(App& a, float fieldW);              // Settings → MIDI

}  // namespace gl
