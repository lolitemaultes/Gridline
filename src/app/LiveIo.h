// Gridline - the Clips layout's In/Out section (K-112c checkpoint 55, the tester's gap 2): where a
// track's sound comes from, whether its input is heard, and where its sound goes, on the clip grid's
// strips and in the Arrangement's track headers, as the reference's I/O section.
//
//   From:    an audio track's device input (a menu of the inputs); a MIDI or instrument track's notes
//            come from every MIDI input ("All Ins", as the reference's default).
//   Monitor: an audio track's input monitoring: In (always heard), Auto (heard while armed), Off.
//   To:      the mixer track the track plays into: Main, a group track or a return (a menu).
//
// Every control edits the same song fields as Studio's mixer (the input, the monitoring, the first
// send), one undo step each, so every layout and collaborator sees the change. Shown or hidden with
// View > In/Out (Cmd+Alt+I), the person's setting `clips.showIO` (off by default).
#pragma once
#include <string>
#include "../../third_party/imgui/imgui.h"

namespace gl {
class App;
}

namespace gl::liveio {

bool shown(const App& a);
void toggle(App& a);
constexpr float kHeaderW = 120.0f;  // the I/O column at the left of an Arrangement header
constexpr float kStripH = 62.0f;    // the I/O rows at the top of a grid strip

// The labels as drawn ("In 1 - In 2", "No input", "All Ins"; "Main", "Group", "A Reverb"; "" none).
std::string fromLabel(const App& a, int track);
std::string toLabel(const App& a, int track);
// Draws the section for `track` into [p0, p1] (three rows, or one row From | monitor | To when the
// rectangle is low) and acts on a left click there (`hovered`: the pointer is over its window).
// Returns true while the pointer is over one of its controls.
bool drawBlock(App& a, ImDrawList* dl, int track, ImVec2 p0, ImVec2 p1, bool hovered);
// Once a frame in Clips: the From and To menus (opened by drawBlock).
void drawMenus(App& a);

}  // namespace gl::liveio
