// Gridline - K-123 (4.5): instrument channels armed for note recording, in every layout.
//
// Arming a channel means "the notes I play go to this instrument, and record there". With nothing
// armed, notes go to the selected channel as before. Several armed channels all play (and record)
// what is played; the selected one, when armed, plays from the input itself. The set is
// App::playlist.midiArm (the channels Smooth's R buttons arm, by sound), a performance switch of this
// computer: never saved, never shared in a collaboration (each person arms their own).
//
// Where it is set: the channel rack's row dot, the Studio playlist's instrument track headers, the
// Tracker's track headers and Smooth's R buttons (logicplug). Clips arms its MIDI tracks itself
// (livelook), so there this module stays out of the way.
#pragma once
#include <vector>
#include "../../third_party/imgui/imgui.h"
#include "../core/Types.h"

namespace gl {

class App;

namespace midiarm {

// Channel arming applies (not in Clips).
bool applies(const App& a);
bool armed(const App& a, ObjectId channel);
void setArmed(App& a, ObjectId channel, bool on);
// Click on a dot: toggles; `exclusive` (Alt+click) arms only this one.
void toggle(App& a, ObjectId channel, bool exclusive = false);
// The armed channels that exist, in rack order.
std::vector<ObjectId> armedChannels(const App& a);

// What live notes play: the selected channel when it is armed or nothing is armed, else the first
// armed channel (Smooth: its armed or monitored track's sound).
ObjectId liveChannel(const App& a);
// Every channel a live note plays, liveChannel first.
std::vector<ObjectId> liveChannels(const App& a);
// Whether notes played on `channel` record (with channels armed: only theirs).
bool records(const App& a, ObjectId channel);
// Live notes are layered over several channels (armed ones).
bool layering(const App& a);
// A layered note (not liveChannel's) starts or ends: it records with the take. False: nothing took it.
bool layerNote(App& a, ObjectId channel, int pitch, int velocity, bool on, double tick);

// The record dot as every header draws it: a ring (hover, faded by `alpha`) or, armed, a red disc.
void drawDot(ImDrawList* dl, ImVec2 c, float r, bool isArmed, bool hot, float alpha = 1.0f);
const char* dotHint(bool isArmed);

}  // namespace midiarm
}  // namespace gl
