// Gridline: K-47: a lost OpenGL context (a driver update, a driver timeout). The project lives in
// memory and the audio does not need the graphics, so what a reset takes can be built again: the
// context, what ImGui keeps in it, the interface's own textures.
//
// How a loss is seen: a context asked for with robustness reports resets through
// glGetGraphicsResetStatus (Windows and Linux, when the driver offers it). Without that, GL_CONTEXT_LOST
// from glGetError and a context that no longer answers glGetString are what is left. macOS does not
// report context loss to an application at all and needs none of this in practice: there it can only
// be simulated. What cannot be built again goes through the crash pop-up with a recovery copy
// (crash::endAfterGraphicsLoss), never a black window.
#pragma once
#include <SDL.h>

namespace gl::glrecovery {

// The main window's context at the start: robust where the system offers it, plain otherwise; on
// Linux and Windows OpenGL 3.0 core, else 3.0 compatibility, else 2.1 (K-75). Keeps the attributes
// for the contexts that share with it.
SDL_GLContext createContext(SDL_Window* window);
// The context just became current: learns how it reports a reset.
void attach();
// The shading language for ImGui's renderer in the attached context ("#version 130", or 120 on an
// OpenGL 2.x context; 150 on macOS). Contexts that share with the main one use the same.
const char* glsl();
// Whether this context reports resets by itself (for the log and the tests).
bool canReport();
// Once a frame, on the main context: why it is lost (a short text for the report), or nullptr.
const char* poll();
// Dev scripts: the next poll reports a loss, and all that follows is the real path.
void simulateLoss();
// After a report: a driver still resetting says so; a new context must wait for it to end.
void waitForResetToEnd(int maxMs);
// A new context for the main window, asked for again for a few seconds (the driver may be coming
// back). nullptr when none could be had. Dev: GRIDLINE_DEV_GL_NO_NEW_CONTEXT refuses at once.
SDL_GLContext recreateContext(SDL_Window* window, int maxMs);
// In the current ImGui context, after its renderer started again: every texture it keeps comes back
// from the copy ImGui holds in memory.
void restoreTextures();

}  // namespace gl::glrecovery
