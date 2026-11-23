// Gridline — keyboard / MIDI workflow UI entry points (MidiUi.cpp).
#pragma once
#include <string>
#include "../core/LinkCapture.h"
#include "../core/MidiRecord.h"
#include "../core/Params.h"
#include "../core/Song.h"

namespace gl {

class App;
class Settings;

void drawRemoteDialog(App& a);                            // "Remote control settings" window
// The dialog's buttons (dev runs press them without a mouse): Apply saves the link (one undo step),
// Cancel drops it, Remove deletes the link being edited.
void remoteDialogApply(App& a);
void remoteDialogCancel(App& a);
void remoteDialogRemove(App& a);
// k078-songlinks: the Remove button beside an internal link on a song setting (one undo step).
void remoteDialogRemoveInternal(App& a);
// Dev runs: open, listening, the source it would link, what it captured and the meter, the undo depth.
std::string remoteDialogDescribe(App& a);
// k078-linkctl: what the link dialog listens for (MidiWorkflow.cpp). While it listens, every input's
// controls go to the capture and drive nothing else; the latest control moved on purpose is the link's.
namespace linkctl {
LinkCapture& capture();
bool& notes();        // keys and pads count too (a note or button link)
void start(App& a);   // starts listening again (forgets what was captured)
// A control surface (an FLkey's DAW port, Mackie Control, a pad controller) owns this input.
bool surfacePort(const std::string& port);
}  // namespace linkctl
void remoteLinkMenuItems(App& a, ParamId pid);            // knob / fader context menus
void actionLinkMenuItems(App& a, RemoteAction action);    // transport buttons
void recordOptionsMenu(App& a);                           // record button menu
void typingKeyboardMenu(App& a);                          // typing keyboard button menu
std::string typingKeyName(int key);                       // a typing key as this keyboard's layout names it
// K-113, GitHub #79: the layout typingKeyName names keys after: "My keys say" (setting "keys.myKeys",
// Computer, "Same as my computer", by default), shared with the names of shortcuts (core/KeyCaps.h).
// Keys play by where they sit whatever it is.
extern TypingLayout typingLayout;
void loadTypingLayout(const Settings& s);           // from the settings (the app at startup)
void setTypingLayout(App& a, TypingLayout l);             // and stores it in the settings
void myKeysItems(App& a);                                 // GitHub #79: the "My keys say" choices as menu items
// Whether the typing keyboard is on when Gridline starts: "keys.typingAtStart" is "on", "off" or
// "last" (the default: as it was left, kept in "keys.typingOn" when Gridline closes).
bool typingKeyboardAtStart(const Settings& s);
void midiSettingsPage(App& a, float fieldW);              // Settings → MIDI

}  // namespace gl
