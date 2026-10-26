// Gridline - theme choice in the settings (J-55, Settings > Theme): dark,
// light or follow the system, plus the user's accent / role colours,
// contrast and brightness (theme::Tweaks). Keys:
//   theme.mode        "dark" | "light" | "system"  ("" = from ui.darkTheme, older settings)
//   theme.accent / theme.hot / theme.led / theme.playhead   "#RRGGBB" ("" = theme default)
//   theme.contrast / theme.brightness                       -1 .. 1
//   theme.controls    "slate" | "graphite" | "hairline" | "lumen" | "classic" (J-129; unknown = slate)
#pragma once
#include <cstdint>
#include <string>
#include "ControlStyle.h"
#include "Theme.h"

namespace gl {
class Settings;
}

namespace gl::theme {

enum class Mode { Dark, Light, System };

Mode modeFromSettings(const Settings& s);
void modeToSettings(Mode m, Settings& s);  // also keeps ui.darkTheme in step
Tweaks tweaksFromSettings(const Settings& s);
void tweaksToSettings(const Tweaks& t, Settings& s);
// Control style (J-129): how knobs, faders, buttons, meters and displays are drawn.
ui::ControlStyle controlStyleFromSettings(const Settings& s);
void controlStyleToSettings(ui::ControlStyle c, Settings& s);
// Removes every theme setting (mode stays dark / light as it was).
void resetThemeSettings(Settings& s);

// "#RRGGBB" <-> 0xRRGGBB (Tweaks::kKeep for "" or an invalid text).
std::string colourText(uint32_t rgb);
uint32_t parseColour(const std::string& text);

// What the desktop asks for: macOS appearance, GTK_THEME / GNOME colour scheme.
// Unknown = dark (Gridline's default look). Runs a short query: call on start,
// on changing the setting and when the window regains focus, not per frame.
bool systemPrefersDark();
// Resolves the mode and applies palette + tweaks and the control style. The desktop is asked once
// and then only when requerySystem is set (mode chosen, window focused again).
void applyFromSettings(const Settings& s, bool requerySystem = false);

}  // namespace gl::theme
