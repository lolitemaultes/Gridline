// Gridline — files the operating system asks Gridline to open: a double click in the file
// manager, Open with, a file dropped on the Dock icon.
//
// Linux and Windows start a new Gridline with the file as its argument: main.cpp opens it, or
// (K-121) hands it to the Gridline that is already running (core/SingleInstance.h), whose main
// loop queues it here like an open request. macOS sends an open-documents request instead, to the running Gridline or to the one it
// starts. SDL reports that request as a drop with no window: SDL_DROPBEGIN, SDL_DROPFILE per
// file, SDL_DROPCOMPLETE, each with windowID 0 (drops on a window always carry its id). Those
// events are taken here, before the drop handling would place the files at the pointer, and
// the files open like a double click in the browser: a project replaces the open one after the
// unsaved-changes prompt.
//
// AppKit also sends the command line's files as such a request; main.cpp opens those itself,
// so setCommandLine() names them and they are passed over while Gridline starts.
#pragma once
#include <SDL.h>
#include <string>
#include <vector>

namespace gl {
class App;
}

namespace gl::opendocs {

// The program's arguments (before any event is read). Arguments that are not options are
// files main.cpp opens itself; the value after --dev-os-open is the test's own request.
void setCommandLine(int argc, char** argv);
// Start-up is over: from now on every request opens, the command line's files too (a later
// double click on the same file).
void startupDone();
// True when `ev` is part of an open request (windowID 0). Its file is kept and the event's
// text freed; the caller drops the event.
bool take(const SDL_Event& ev);
// A complete request waits.
bool pending();
// The files of the complete requests, in the order they came, and clears them.
std::vector<std::string> takeReady();
// K-121: files a later launch handed over (Linux, Windows): they wait as a complete request.
void queue(const std::vector<std::string>& files);
// The running Gridline opens requested files as a double click in the browser would: the first
// project among them replaces the open one (after the unsaved-changes prompt); other files
// are added to the project.
void open(App& a, const std::vector<std::string>& files);
// Tests: posts the SDL events macOS's open request makes, for these files.
void post(const std::vector<std::string>& paths);
// The file path an open request names: a path, or a file:// link turned into one; "" otherwise.
std::string requestPath(const std::string& raw);

}  // namespace gl::opendocs
