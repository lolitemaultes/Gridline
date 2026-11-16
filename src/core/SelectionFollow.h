// Gridline - K-234: the selection follows across the channel rack, the mixer and the piano roll.
// Selecting a channel selects the mixer track it plays through; selecting a mixer track selects
// the channels routed to it (the first of them becomes the channel the piano roll and the
// keyboard follow). A track no channel routes to, and the master, leave the channels as they are.
// On by default; Channel rack menu > "Selection follows the mixer". Selection is each person's
// own view (not part of the project), so nothing here edits the document. Pure (test_k234.cpp).
#pragma once
#include <set>
#include "Song.h"

namespace gl::selfollow {

struct State {
  ObjectId channel = kNoId;  // what the last step saw
  ObjectId mixer = kNoId;
};

// One step (once a frame): compares the selection with what the last step saw and makes the
// other side follow the side that changed. `selChannels`, `selChannel`, `selMixer` are the
// app's selection (updated in place). Returns true when it changed something.
inline bool step(const Song& s, State& st, std::set<ObjectId>& selChannels, ObjectId& selChannel, ObjectId& selMixer) {
  bool changed = false;
  if (selChannel != st.channel) {
    if (const Channel* c = s.channel(selChannel)) {
      const ObjectId to = c->mixerTrack != kNoId && s.mixerTrack(c->mixerTrack) ? c->mixerTrack : s.masterId();
      if (to != kNoId && to != selMixer) selMixer = to, changed = true;
    }
  } else if (selMixer != st.mixer && selMixer != s.masterId() && s.mixerTrack(selMixer)) {
    std::set<ObjectId> routed;
    ObjectId first = kNoId;
    for (const Channel& c : s.channels)
      if (c.mixerTrack == selMixer) {
        routed.insert(c.id);
        if (first == kNoId) first = c.id;
      }
    if (!routed.empty() && routed != selChannels) {
      selChannels = routed;
      if (!routed.count(selChannel)) selChannel = first;
      changed = true;
    }
  }
  st.channel = selChannel, st.mixer = selMixer;
  return changed;
}

}  // namespace gl::selfollow
