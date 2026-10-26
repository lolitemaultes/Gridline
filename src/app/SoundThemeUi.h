// Gridline - J-133: the Sounds group of Settings > Theme. Interface sounds on /
// off, the sound theme (graphite, prism, tine, signal, classic) with a preview
// per theme, the volume, and the per-event switches with a preview each.
// Self-contained so the Theme page can place it in its own group.
#pragma once

namespace gl {

class App;

namespace uisound {
// Draws the group's rows (no section frame; the caller opens and closes it).
void drawThemeSettings(App& a, float fieldW);
}  // namespace uisound

}  // namespace gl
