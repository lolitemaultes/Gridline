// Gridline: the silence tools' rows (K-253), side by side wherever audio is edited:
//   Trim silence at the ends   Remove silence   Silence settings >
// in the playlist's audio clip menus (every layout; the selected audio clips together, one undo
// step), in Loupe's Edit menu, and in the menus of channels that play a sample (the Sampler,
// Kerf, Granular and the other sample instruments: the channel's sample becomes a new file, its
// positions move with the audio). One engine (engine/Silence.h) and one set of settings
// (Settings "silence.*", per person) for every place.
#pragma once
#include <set>
#include "../core/Types.h"
#include "../engine/Silence.h"

namespace gl {

class App;

namespace silenceui {

silence::Options options(const App& a);
// The settings submenu's rows (threshold, shortest gap, margin, fades, breaths and, with
// `ripple`, Close the gaps), drawn inside a menu.
void settingsRows(App& a, bool ripple);
// The three rows for an audio clip (and the audio clips selected with it). True when one edited.
bool clipItems(App& a, ObjectId clip);
// Trim (remove false) or Remove on the clips; one undo step. Returns the clips changed.
int applyToClips(App& a, const std::set<ObjectId>& clips, bool remove);
// The three rows for a channel that plays a sample. True when one edited.
bool channelItems(App& a, ObjectId channel);
// Trim or Remove on a channel's sample (one undo step). False with a notice when nothing changed.
bool applyToChannel(App& a, ObjectId channel, bool remove);

}  // namespace silenceui
}  // namespace gl
