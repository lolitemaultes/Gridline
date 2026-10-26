// Gridline — J-113 / J-114 detachable windows: panels, Gridline's plugin editors
// and dialogs can live in OS windows of their own (Dear ImGui multi-viewports
// over the SDL2 + OpenGL3 backends). See docs/detachable-windows.md.
//
// J-116 model: only the title-bar pop-out button detaches a panel (into an
// OS-decorated window of its own). Dragging a window by its title bar or tab is
// exactly as without detaching (docking, dock targets, Shift to float, clipped to
// the main window), wherever it is released. A detached window is moved and
// resized by the OS only and never docks by being dragged; it comes back with
// the dock-back button, its OS close button, "Bring all windows back", Reset
// layout or the setting turned off.
//
// ImGui's coordinate space is OS coordinates relative to the main window's
// top-left, divided by the interface scale: the main viewport stays at (0, 0)
// with the size it always had, so everything inside the main window (layout,
// dev-input coordinates, screenshots) is unchanged. This module converts the
// backend's platform callbacks, monitors and mouse positions to that space,
// keeps detached windows in place when the main window moves, saves and
// re-anchors them in layout.ini, and decides when the whole application lost
// the keyboard focus (J-109).
#pragma once
#include <SDL.h>
#include <string>
#include <vector>
#include "../../third_party/imgui/imgui.h"

namespace gl {
class App;
}

namespace gl::viewports {

struct Options {
  bool hidden = false;  // --hidden: platform windows are created but never shown or focused
};

// After the ImGui SDL2 / OpenGL3 backends are initialised (main context current).
void install(SDL_Window* mainWindow, SDL_GLContext gl, const Options& o);
// Off-screen runs (--screenshot / --profile-frames with --size): two virtual monitors,
// the main window and one of the same size to its right, so runs are the same everywhere.
void setVirtualMonitors(bool on);

// The platform can create windows outside the main one (not Wayland, backend support).
bool available();
// Why not (empty when available), for Settings.
std::string unavailableReason();
// Detaching is on this frame (available and allowed).
bool enabled();
// Per frame, before ImGui_ImplSDL2_NewFrame(): the "Allow windows outside the main window"
// setting. Turning it off brings every window back first (exactly as before J-113 after that).
void setAllowed(bool allowed);

// Per frame, right after ImGui_ImplSDL2_NewFrame(), before any other mouse input is queued:
// interface scale and the main window's logical size for this frame.
void beginFrame(float uiScale, ImVec2 mainLogicalSize);
// Right after ImGui::NewFrame(), before any window is begun.
void afterNewFrame();
// Around ImGui::DockSpace() of the main window (its id): its tab bars get the pop-out button.
void beginDockSpace(ImGuiID dockspace);
void endDockSpace();
// After every window of the frame, before ImGui::Render(): the pop-out / dock-back buttons
// in the title bars of floating windows (docked ones have theirs in the tab bar).
void drawTitleButtons();
// After the main window is rendered, before it swaps: the detached windows are
// updated, rendered and swapped (the main GL context is current again afterwards).
void renderWindows();

// Every SDL event (focus tracking, the main window minimised / restored).
void onEvent(const SDL_Event& e);
// True once when the whole application lost the keyboard focus (no Gridline window
// gained it): J-109 releases the live keys then, never when the focus only moves
// between the main window and a detached one.
bool takeAppFocusLost();
// The SDL window id of the OS window showing the window called `name` (the main window's for
// "main" or a window inside it), 0 when there is no such window.
Uint32 sdlWindowOf(const std::string& name);
// J-116: `sdlId` is the OS window of a detached Gridline window (not the main one).
bool ownsWindow(Uint32 sdlId);
// J-116: the SDL ids of every detached window's OS window.
std::vector<Uint32> windowIds();

// Plugin windows (a channel's window, an insert's editor): call right before their
// ImGui::Begin(name). A plugin window is outside the main window only while it stays
// open after its pop-out button: one that is about to open (it was not drawn last
// frame) opens inside the main window, whatever layout.ini saved (a restart, another
// project) or where it was when it was closed. Open, it is left where it is.
void openAttached(const char* name);

// View > Bring all windows back into Gridline.
void gatherAll();
// Detached windows (own OS windows) right now.
int detachedCount();

// J-113 / J-114 / J-116 dev-input commands (see main.cpp); true when `cmd` was one of them.
bool devCommand(App& a, const std::string& cmd, const std::vector<std::string>& args, int frame);

}  // namespace gl::viewports
