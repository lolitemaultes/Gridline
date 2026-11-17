// Gridline - the Clips layout's In/Out section (K-112c checkpoint 55, the tester's gap 2): where a
// track's sound comes from, whether its input is heard, and where its sound goes, on the clip grid's
// strips and in the Arrangement's track headers, as the reference's I/O section.
//
//   From:    an audio track's device input (a menu of the inputs); a MIDI or instrument track's notes
//            come from every MIDI input ("All Ins", as the reference's default), on all channels or one
//            (checkpoint 56: the person's MIDI routing, Settings > MIDI, which plays that channel into
//            the track's instrument whether it is selected or not).
//   Monitor: an audio track's input monitoring: In (always heard), Auto (heard while armed), Off.
//            A MIDI track's (checkpoint 56): Auto (the selected track plays the keyboard), Off (never),
//            In (played when the selected track is not listening; the engine plays one track's
//            instrument from the keyboard at a time). Per person and Set (clips.midiMonitor).
//   To:      the mixer track the track plays into: Main, a group track or a return (a menu).
//   Returns: their Audio To; Main: Cue Out and Master Out (shown; one output pair).
//
// Every control edits the same song fields as Studio's mixer (the input, the monitoring, the first
// send), one undo step each, so every layout and collaborator sees the change. Shown or hidden with
// View > In/Out (Cmd+Alt+I), the person's setting `clips.showIO` (off by default).
#pragma once
#include <string>
#include "../core/Types.h"
#include "../../third_party/imgui/imgui.h"

namespace gl {
class App;
}

namespace gl::liveio {

bool shown(const App& a);
void toggle(App& a);
constexpr float kHeaderW = 120.0f;  // the I/O column right of an Arrangement header's name (checkpoint 56)
constexpr float kStripH = 106.0f;   // the I/O rows at the top of a grid strip (labelled, checkpoint 56)
constexpr float kLabelledH = 96.0f; // from this height the rows carry the reference's labels
constexpr float kMidH = 80.0f;      // checkpoint 57 (N56-07): from this height the From and To labels show (no source row)

// The labels as drawn ("In 1 - In 2", "No input", "All Ins"; "Main", "Group", "A Reverb"; "" none).
std::string fromLabel(const App& a, int track);
std::string toLabel(const App& a, int track);
// Draws the section for `track` into [p0, p1] (three rows, or one row From | monitor | To when the
// rectangle is low) and acts on a left click there (`hovered`: the pointer is over its window).
// Returns true while the pointer is over one of its controls.
bool drawBlock(App& a, ImDrawList* dl, int track, ImVec2 p0, ImVec2 p1, bool hovered);
// Checkpoint 56 (N55-10): a return's section (Audio To) and Main's (Cue Out, Master Out), at the foot of
// the rectangle, lined up with the tracks' Audio To.
bool drawMixerBlock(App& a, ImDrawList* dl, ObjectId mixer, ImVec2 p0, ImVec2 p1, bool hovered);
// A MIDI track's monitoring (InputMonitor), by its instrument; and the instrument the MIDI keyboard
// plays (App::processMidi): the selected one unless its monitor is Off, else one whose monitor is In.
int midiMonitor(const App& a, ObjectId instrument);
void setMidiMonitor(App& a, ObjectId instrument, int monitor);
ObjectId midiTarget(const App& a, ObjectId selected);
// Once a frame in Clips: the From and To menus (opened by drawBlock).
void drawMenus(App& a);

}  // namespace gl::liveio
