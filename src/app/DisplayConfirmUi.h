// Gridline - the "Keep this change?" dialog (J-77) and the settings that use
// it. See DisplayConfirm.h for the flow.
#pragma once
#include <string>
#include "DisplayConfirm.h"

namespace gl {
class App;
}

namespace gl::ui {

// Starts confirming a change (Keep / Revert, before / after, countdown).
void confirmDisplayChange(App& a, dispconf::Change c);
// Per frame (drawDialogs): the dialog and its countdown.
void drawDisplayConfirm(App& a);

// The wired settings.
void confirmScale(App& a, double to);                                   // interface size (preview first)
void confirmLayoutReset(App& a);                                        // default window layout (preview first)
// Theme: `before` taken with dispconf::take(settings, themeKeys()) before the edit;
// the edit is already in the settings (and on screen).
void confirmTheme(App& a, const dispconf::Snapshot& before, const char* what);
void confirmLanguage(App& a, const std::string& code);                  // applied at once, bilingual buttons
// Audio output: sample rate / buffer (`before` = take(settings, audioKeys()), edit already written)
void confirmAudio(App& a, const dispconf::Snapshot& before);
void confirmAudioDevice(App& a, const std::string& device);             // "" = system default

}  // namespace gl::ui
