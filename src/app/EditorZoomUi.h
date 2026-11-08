// Gridline - K-19: plugin editor windows have their own size (75 % to 200 %, K-136: one
// global size from Settings > Interface unless a plugin has its own) on top of the
// interface scale (core/EditorZoom.h for the rules).
//
// An editor is laid out at its design size in logical pixels, as always, inside a child
// region of its window; at any other zoom the window reserves the zoomed size and, once
// the frame is drawn, the region's draw lists (its child windows and the popups it opened
// too) are magnified or reduced about the region's top-left corner. Under 100 % the region
// is laid out past the window's edges (the clip is widened for it, the window's content
// size is put back), and while the pointer is off the picture its windows take no mouse
// input. Text is rasterised at the
// magnified density and anti-aliasing fringes are kept one device pixel wide, so the
// result is as sharp as a natively larger editor. The pointer over the magnified picture
// is mapped back to design coordinates before ImGui sees it (a drag that started inside
// keeps that mapping until the buttons are released), so every control works unchanged.
// At 100 % nothing of this runs: the editor draws exactly as before.
//
// Only Gridline's built-in editors are zoomed; an installed plugin's own interface keeps
// its native size (its window is not drawn through here).
#pragma once
#include <string>
#include "../../third_party/imgui/imgui.h"

namespace gl {
class App;
}

namespace gl::edzoomui {

// Before Begin() of the editor window `key` ("fxeditor", "channelsettings") showing
// plugin `type`, whose content is `contentW` logical px wide at 100 %: the zoom it draws
// at this frame (the remembered or automatic one, capped to the work area; 1 when
// `zoomable` is false, e.g. an installed plugin's generic page).
float prepare(App& a, const char* key, const std::string& type, float contentW, bool zoomable = true);
// Right after Begin() (and only when it returned true): the editor's content follows,
// drawn at its design size. Pair with end().
void begin(App& a, const char* key);
void end(App& a, const char* key);

// Menu items "Editor size": Automatic, 100 % ... 200 % for `type` (in the plugin window menu).
void sizeMenu(App& a, const std::string& type, const char* key);

// Main loop, after every mouse event of the frame is queued and before ImGui::NewFrame():
// pointer positions over a magnified editor are mapped to its design coordinates.
void mapPointer();
// Main loop, right after ImGui::Render(): the magnified regions' draw lists are scaled.
void afterRender();

// Dev checks (--dev-input "edzoom"): the zoom drawn last frame for `key` (0 = not drawn),
// the design size and the window's size and position, to stdout.
void devDump(const char* key);
// Development / tests: a point in design coordinates of editor `key` to where it shows.
ImVec2 toScreen(const char* key, ImVec2 p);

}  // namespace gl::edzoomui
