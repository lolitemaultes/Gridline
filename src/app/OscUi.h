// Gridline: OSC in the app (docs/osc.md). Runs the OSC server as Settings > MIDI > OSC says,
// applies received messages on the UI thread (core/OscControl.h: transport, mixer, channels,
// plugin parameters, patterns, markers, OSC controller links and learning), sends feedback to a
// touch surface, and draws the settings section with its activity monitor and test button.
#pragma once

namespace gl {
class App;
namespace oscui {

// Per frame: starts, restarts or stops the server, applies what arrived, sends feedback.
void tick(App& a);
// Stops the server and releases notes it holds (app shutdown).
void shutdown();
// Settings > MIDI: the OSC section.
void settingsSection(App& a, float fieldW);
// True while the server runs (the remote control dialog mentions OSC learning then).
bool listening();

}  // namespace oscui
}  // namespace gl
