// Gridline — κ3 editors (reference-geometry editors for the stereo / creative effects).
#pragma once

namespace gl {
class App;
struct PluginTarget;
// Draws the editor when the type is one of the κ3 types; false otherwise.
bool drawK3Editor(App& a, const PluginTarget& t, float width);
}  // namespace gl
