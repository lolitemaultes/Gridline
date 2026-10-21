// Gridline - choosing pattern mode shows the channel rack (J-77, user
// request; FL's PAT / SONG button also shows the channel rack for pattern
// mode). Watches the transport mode once per frame: a switch to PAT from
// SONG / LIVE reopens a closed rack (where it was) or brings an open one to
// the front - never moved, never given the keyboard focus. Switching to SONG
// or LIVE leaves the rack alone. Switches the program makes on its own
// (opening a project, the piano roll's ruler scrub) are marked quiet.
#pragma once
#include "../engine/AudioEngine.h"

namespace gl {

struct RackReveal {
  enum class Action { None, Show, Front };
  PlayMode last = PlayMode::Pattern;

  // A mode change the program makes itself (no rack): call right after setMode.
  void quietSwitch(PlayMode now) { last = now; }
  // Per frame with the current mode and whether the rack window is shown.
  Action step(PlayMode mode, bool rackShown) {
    Action a = Action::None;
    if (mode != last && mode == PlayMode::Pattern) a = rackShown ? Action::Front : Action::Show;
    last = mode;
    return a;
  }
};

}  // namespace gl
