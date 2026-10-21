// Gridline — out-of-process plugin sandbox ("Run in sandbox").
//
// A sandboxed plugin runs in a child process (`<exe> --plugin-sandbox
// <shm name>`) that hosts the real Backend. The parent talks to it through
//   * a shared-memory block for audio: input / sidechain / output buffers,
//     the block's parameter changes, note events and transport, plus two
//     sequence counters (request, done) forming a lock-free handshake: the
//     audio thread fills the block, publishes `request`, wakes the child with
//     one byte on a pipe and spins (bounded) until `done` matches;
//   * a socket for everything else (create, prepare, state, parameter values
//     and text, programs, editor, host events), request/response, UI thread.
// When the child dies (crash, kill) or stops answering, the instance is
// marked crashed: its output is silent, the song keeps its state, and the UI
// offers to reload it. The audio thread never blocks on the child for more
// than a few block lengths.
#pragma once
#include <memory>
#include <string>
#include "../Backend.h"

namespace gl::host {

// `name` titles the editor window the sandbox process opens.
std::unique_ptr<Backend> createSandboxBackend(const std::string& exe, const std::string& format, const std::string& entry,
                                              const std::string& uid, std::string* err, const std::string& name = {});
// Child process entry point (`--plugin-sandbox <shm name>`); returns the exit code.
int sandboxChildMain(const std::string& shmName);

// Child-side native window for a plugin editor (macOS: SandboxWindow.mm).
// Returns the content view to attach the editor to, nullptr when unsupported.
// (x, y): top-left based global position of the window frame, or
// Editor::kNoPlace to centre it.
void* sandboxOpenWindow(const std::string& title, int w, int h, bool resizable, int x = Editor::kNoPlace, int y = Editor::kNoPlace);
// Top-left based global frame of the editor window (title bar included).
bool sandboxWindowFrame(int& x, int& y, int& w, int& h);
void sandboxResizeWindow(int w, int h);
bool sandboxWindowOpen();  // false after the user closed it
void sandboxCloseWindow();
void sandboxPumpEvents();  // non-blocking
// Waits up to `seconds` for `fd` to become readable, handling window events,
// timers and redraws meanwhile (macOS: inside the run loop); true when readable.
bool sandboxWaitEvents(int fd, double seconds);
// Keys the interface in the editor window did not use (for Gridline, J-66).
bool sandboxPopKey(KeyEvent& e);
void sandboxDevKeys();  // dev check: Space typed into the interface's own view
// Dev measurement (GRIDLINE_SANDBOX_STATS=1, GRIDLINE_DEV_DRAG="x,y,s"): the
// interface's drawing per second on stderr, a knob turned after 240 idles.
void sandboxDevStats(int idles);
// Plugin checks (macOS): a parent view in a window that is never shown;
// nullptr when unsupported. pumpOffscreenEvents runs the event loop briefly.
void* offscreenEditorParent(int w, int h);
void closeOffscreenEditorParent();
void pumpOffscreenEvents(double seconds);

}  // namespace gl::host
