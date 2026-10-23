// Gridline — J-75: drags from other applications inside the interface.
//
// The platform layer's events (core/ExternalDrop.h) drive an ImGui drag with
// an external source (ImGuiDragDropFlags_SourceExtern): while the files hover,
// the source is submitted every frame with the pointer the window system
// reports, so every drop target in Gridline sees it exactly like a file
// dragged from the browser — the playlist draws ghost clips, the rack, mixer,
// piano roll, plugin windows and the browser highlight and describe what the
// drop would do (tooltip + status bar). On drop the source is left out for
// one frame, which delivers the payload to the target under the pointer;
// when no target takes it, the files open as from a double click.
//
// Escape or leaving the window cancels. Files that arrive after the drop
// (promises, downloads) are dropped where they were let go once they are all
// here ("Dropped" folder of the project).
#pragma once
#include <string>
#include <vector>
#include "../../third_party/imgui/imgui.h"
#include "../core/ExternalDrop.h"
#include "BrowserUi.h"

namespace gl {
class App;
}

namespace gl::xdragui {

// Main loop, after the SDL events and before ImGui::NewFrame: takes the
// platform events (pointer → ImGui, in window coordinates).
void pump(App& a);
// After the platform backend's NewFrame: the drag's pointer again (the
// backend reports the system pointer while the window has focus).
void feedPointer();
// Right after ImGui::NewFrame: submits the hovering drag (or leaves it out on
// the drop frame), Escape cancels, status hint when no window takes it.
void beginFrame(App& a);
// Before ImGui::Render: finishes a drop no target took (open the files).
void endFrame(App& a);

// An external drag hovers / is being dropped.
bool active();
// Its items (in drop order).
const std::vector<xdrop::Item>& items();
// The ImGui payload is the external drag's.
bool ownsPayload(const ImGuiPayload* pl);
// A target performed the drop this frame.
void markConsumed();
// The drop of `items` waits for pending files (performItems / openFiles call it).
void defer(App& a, const std::vector<xdrop::Item>& items, browse::DropTarget target, const browserui::DropPlace& place, const xdrop::Mods& mods);
// Drops still waiting for files.
size_t waiting();

// SDL's drop events (X11 / Wayland / Windows; SDL2 has no hover position):
// collected between begin and complete, then dropped at the pointer.
void sdlDropBegin();
void sdlDropFile(const std::string& path);
void sdlDropText(const std::string& text);  // file:// and http(s) links
void sdlDropComplete(float x, float y);

// --dev-input simulated drags: "dragenter x y <file>...", "dragmove x y",
// "drop [x y]", "dragleave". A file "promise:<path>" is a promised file that
// arrives (copied into the drop folder) a few frames after the drop;
// "plugin:<type>" drags a plugin as the browser does (J-97).
void devEnter(float x, float y, const std::vector<std::string>& files);
void devMove(float x, float y);
void devDrop(float x, float y, bool here);
void devLeave();

}  // namespace gl::xdragui
