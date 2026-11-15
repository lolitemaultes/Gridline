// Gridline - the layout parts (K-112 R4): panels the other layouts are built from, available in
// every layout. Each is a window of its own (its name is its home in core/LayoutHomes): docked in
// its region where the layout has one, floating otherwise (in Studio they float, closed until
// opened from View > Panels). Everything they change goes through the document, so it is undoable,
// saved with the project and seen live by a collaborator in any layout.
//
//   Inspector     the selected channel, mixer track and clips as cards (name, colour, route,
//                 volume and pan), with the buttons that open their editors
//   Library       instruments and effects by category; a click adds one (a channel, or the
//                 selected mixer track's first free slot)
//   Lists         markers (position, name, meter or key) and the tempo and meter: a click moves
//                 the song position there
//   Device chain  the selected chain: the instrument, the track's effect slots (and the master
//                 chain on the master); the chosen device's editor is drawn here (hosted bodies)
//   Clip panel    the selected clips' properties (source, track, start, length, loop, mute)
//   Track scopes  every mixer track's level, live
//   Info box      what the pointer is over, and the key that does it
//   Files         the project's audio files (channel samples, audio clips), missing ones marked
//   Notes         the project notes (the project info's comments), shared with collaborators
#pragma once
#include <string>
#include <vector>

namespace gl {
class App;
}

namespace gl::layoutparts {

// K-112b: the Logic layout's mixer body (a legend column and lined-up strips), drawn in the Mixer window.
void logicMixer(App& a);
// Draws the open parts (call once per frame with the other windows).
void draw(App& a);
// View > Panels: one item per part (checked when open).
void panelsMenu(App& a);
// The Info box is open in a layout that homes it in the status line (it has no window there).
bool infoInBar(App& a);
// Dev ("parts ..."): notes set <file> | dump <file> | focus | blur; chain instrument | fx <slot> | master.
void devCommand(App& a, const std::vector<std::string>& args);

}  // namespace gl::layoutparts
