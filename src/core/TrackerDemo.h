// Gridline - the Tracker's demo module (original music, built on the demo song's instruments).
#pragma once
#include "Song.h"

namespace gl {

// K-106 (checkpoint 15): "Glasswork", a short song written the way a tracker module is: 64-row
// patterns that use many tracks at once (drums, bass, pads, keys, arpeggio, lead, bells, effects).
// Its instruments are the demo song's own sounds plus three Tracker Samplers (lead, arpeggio, bell)
// playing their built-in sounds; the demo song itself is not changed. The order list plays
// intro, groove, theme, groove, break, theme, theme.
Song makeTrackerDemo();

}  // namespace gl
