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
void draw(App& a);

}  // namespace gl::newtracks
