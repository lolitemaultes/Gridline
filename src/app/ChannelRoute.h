// Gridline — J-195: "Route to a free mixer track" for one channel (sampler,
// audio clip channel, Gridline instrument or installed plugin): the header
// button of the channel window, the first item of its route list, the channel
// rack's route menu and the hosted plugin window's bar all run this.
#pragma once
#include <vector>
#include "../core/Types.h"

namespace gl {

class App;

namespace chanroute {

// Routes the channel to the lowest free insert (a new insert when none is
// free; nothing past the maximum), names and colours the track after the
// channel, in one undo step. A channel already alone on its own insert keeps
// it (the track takes its name). Selects the track and says what happened in
// a notification. Returns the track (kNoId: not routed).
ObjectId routeToFree(App& a, ObjectId channel);
// Same for several channels, each to its own track (rack selection).
std::vector<ObjectId> routeToFree(App& a, const std::vector<ObjectId>& channels);
// The button's hint for this channel (translated, static text).
const char* hint(const App& a, ObjectId channel);
// The header button: an icon button `size` square with its hint. True when
// clicked: the caller runs routeToFree (once it no longer holds pointers into
// the song).
bool button(const App& a, ObjectId channel, float size);
// The menu item "Route to a free mixer track" (route menus); true when picked
// (the caller runs routeToFree).
bool menuItem(const char* shortcut = nullptr);
// Same as the first entry of a list (the channel window's mixer track list).
bool listItem();

}  // namespace chanroute
}  // namespace gl
