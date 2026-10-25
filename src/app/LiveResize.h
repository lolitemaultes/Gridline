// Gridline — J-116: windows redraw while they are being resized (the main window and detached
// ones, every platform). See core/LiveResize.h for when a frame is drawn from SDL's event watch,
// docs/detachable-windows.md for each platform's behaviour.
#pragma once
#include <SDL.h>
#include <functional>
#include <string>

namespace gl::liveresize {

// Once the main window, the ImGui backends and viewports are set up: `frame` draws one complete
// frame (tick, interface, render, swap of every window), the same as the main loop's.
void install(SDL_Window* mainWindow, std::function<void()> frame);
void uninstall();

// Around the main loop's SDL_PollEvent loop (the only place a frame may be drawn from the watch).
void pumpBegin();
void pumpEnd();
// Around every frame (the main loop's and the watch's): the re-entrancy guard.
void frameBegin(bool live);
void frameEnd();
// The current frame is drawn from inside a blocked event pump (no frame-clock wait then).
bool inLiveFrame();

// Dev: `f` runs in the next event pump, outside any frame (simulated / real resize drags).
void queueDev(std::function<void()> f);
// Dev: the window system's modal resize loop as the gate sees it, forced on (simulation).
void setSimulatedModal(bool on);
// Dev: frames drawn from the watch so far.
int liveFrames();
// Dev: "liveresize frame <n>: live-frames ... avg-ms ... max-ms ... interval-ms ..." and the
// size each recent live frame was drawn at.
void dump(int frame);

}  // namespace gl::liveresize
