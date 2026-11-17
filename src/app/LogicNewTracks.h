// Gridline - K-112b: the New tracks dialog of the Tracks layout (the Logic preset). One card per
// kind of track (instrument, empty playlist track, mixer track), the chosen kind's details below,
// and how many to create. Opened from Track > New tracks... while that layout is active.
#pragma once

namespace gl {
class App;
}

namespace gl::newtracks {

void open();
bool isOpen();
// R06-07: the dialog is modal: while open (and the frame it closes) no shortcut acts behind it.
bool holdsKeys();
void draw(App& a);
// K-112D (D3): Create with the defaults (one Tercet instrument track, its editor not opened); tests.
void createDefault(App& a);

}  // namespace gl::newtracks
