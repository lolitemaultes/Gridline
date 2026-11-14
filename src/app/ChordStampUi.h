// Gridline: the piano roll's chord stamp button and its chord picker.
//
// The button sits right after the piano roll's menu button, at the left of its toolbar. A click
// opens a compact picker: the recent chords first, then every chord type in groups, the scale
// chords (when a scale is set), the inversion and "Just the next chord". Picking a chord turns the
// stamp on with it (and the Draw tool when a tool that draws no notes was chosen): Draw and Paint
// then place that chord, its root on the clicked key. While stamping, the button is lit and shows
// the chord's symbol where there is room; a click on the stamp, or Esc over the piano roll, turns
// the stamp off. The burger menu's Stamp submenu keeps the same choices.
#pragma once
#include <string>
#include <vector>

namespace gl {
class App;
struct PianoRollState;

namespace stampui {

// Draws the button (and its picker popup) at the cursor. `roomForLabel`: the chord's symbol may
// show beside the stamp while stamping.
void button(App& a, bool roomForLabel);
// The piano roll has the keyboard: Esc turns a stamp off (unless something else takes the key).
void escapeTurnsOff(App& a);

// The stamp is on (a chord or a scale chord is chosen).
bool on(const PianoRollState& st);
// What the button shows while stamping: the chord's symbol ("m7"), "Scale" for a scale chord.
std::string shortLabel(const PianoRollState& st);
// Picks a chord from the picker: the stamp turns on with it; it goes to the top of the recent list.
void pick(App& a, int chord);
void turnOff(PianoRollState& st);
// The recent chords, most recent first (tools::chords() indices, kept in the settings).
std::vector<int> recent(const App& a);

}  // namespace stampui
}  // namespace gl
