// Gridline — offline renders of songs with a live area (performance mode, J-32).
//
// Live-area clips only play when launched by hand, so an offline render would
// skip them. Like the reference, a render plays the song from the live area's
// end (the song start marker) with the arrangement after it as arranged, and
// the clips that were playing when the project was saved (Clip::launched)
// start there, following their track's follow mode. A pure performance set
// (nothing arranged after the live area) with launched clips renders for the
// live area's length; with nothing launched either, the live area plays as an
// ordinary arrangement so the render is not silent.
#pragma once
#include "../core/Song.h"

namespace gl {

// Rewrites `s` so the engine plays it as a plain song (no live area left:
// launched clips become arrangement clips after it and the live-area clips go,
// or the live area is dissolved into the arrangement) and returns the tick the
// render starts at (0 when the song has no live area).
Tick prepareOfflineLive(Song& s);

}  // namespace gl
