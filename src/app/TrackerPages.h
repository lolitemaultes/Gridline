// Gridline - the Tracker layout's Song, Steps and Mix pages (K-106). See TrackerPages.cpp.
#pragma once
#include <string>

namespace gl {
class App;
}

namespace gl::trackerpages {

// The Tracker layout is on screen, built from its region shell: the Playlist, Channel rack and
// Mixer windows draw these pages instead of their own views.
bool active(const App& a);
// The frame the Tracker layout comes in (a switch, or the start): the shell docks its windows
// over this frame, so a window not docked yet skips drawing (the switch snapshot covers the
// frame); the hidden pages then ask for no icon sizes they will not show. Kept by the header band,
// which draws on every Tracker frame.
bool enteringFrame();
void song(App& a);    // the Playlist window's body
void steps(App& a);   // the Channel rack window's body
void mixer(App& a);   // the Mixer window's body
void pads(App& a);    // the Live pads window's body
// A page title bar in the Tracker look (the title left, `info` right in the monospaced face) at
// the window cursor, full width; the cursor moves below it. For the pages their own views draw (Roll).
void pageTitle(App& a, const char* title, const std::string& info);

}  // namespace gl::trackerpages
