// Gridline - Studio's zone keys in the app (core/ZoneKeys.h): hold Tab and press a digit to jump to a
// zone's window (opened when closed, never closed by the key); Tab alone goes back to the zone before.
//
// Off by default (Settings > Layouts > Studio). Only in the Studio layout with a key set that leaves
// plain Tab free (not the Clips or Tracker sets), and never while a text field, a popup or menu, the
// command palette, the welcome screen, a plugin's own window or the Stage view has the keys. While it is
// on, Dear ImGui's tabbing runs only in text fields (as in Clips), so Tab no longer walks into the
// browser's search. Per person; nothing is kept in the project.
#pragma once
#include <string>
#include <vector>
#include "../core/ZoneKeys.h"

union SDL_Event;

namespace gl {
class App;
}

namespace gl::zonekeysui {

// The setting is on (whatever the layout).
bool enabled(const App& a);
// The key set chosen binds plain Tab (the Clips and Tracker sets): the setting is greyed.
bool keySetTakesTab();
// Zone keys work this frame (enabled, Studio, a free key set, and nothing else has the keys).
bool live(App& a);
// An SDL key event (before Dear ImGui sees it): true when the layer took it (Tab while it works, a
// digit while Tab is held), so nothing else acts on it (the typing keyboard plays no note).
// mainWindow: the SDL id of Gridline's main window (a key for any other window is not the layer's).
bool sdlEvent(App& a, const SDL_Event& e, unsigned mainWindow);
// Per frame (after the window keys): the jump asked for, the zone the person clicked into, tabbing.
void frame(App& a);
// The zones as set (Settings > Layouts > Studio) and back.
zones::Zones load(const App& a);
void store(App& a, const zones::Zones& z);
// The windows a zone may hold (stable names, English), in the order the setting lists them.
const std::vector<const char*>& choices();
// Dev checks (--dev-input "zones dump|on|off|jump <n>|back").
void devCommand(App& a, const std::vector<std::string>& args);

}  // namespace gl::zonekeysui
