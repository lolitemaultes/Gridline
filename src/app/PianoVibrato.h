// Gridline: the piano roll's Vibrato tool (K-262). Each note's vibrato drawn on it (a violet wave
// whose height is the depth and whose cycles are the rate, the envelope shaded with handles for
// the fade in, the depth and the fade out), the gestures (drag up: depth; sideways or the wheel:
// rate; the round handles: fade times and, up or down, their curves; the envelope's start: the
// delay), the right-click menu (shape, sync, centre, rate sweep, presets, copy and paste, natural
// variation, remove) and the toolbar strip that holds the defaults for new vibrato.
// PianoRoll.cpp hosts it: the notes, their boxes, the commit (one undo step per drag).
#pragma once
#include <string>
#include <vector>
#include "../../third_party/imgui/imgui.h"
#include "../core/Types.h"

namespace gl {
class App;
}

namespace gl::pianovib {

inline constexpr ImU32 kViolet = IM_COL32(0xD6, 0x73, 0xEA, 0xFF);

// What the pointer is over on a note.
struct Hit {
  enum Kind { None, Body, Delay, FadeIn, FadeOut, Depth } kind = None;
  int note = -1;
};

// A note's box in the grid: its left and right edges, the centre line of its row, the row height.
struct Box {
  float x0 = 0, x1 = 0, cy = 0, rowH = 0;
};
// The song's time: seconds per tick and per beat (synced rates), at its tempo.
struct Time {
  double secPerTick = 0.5 / 96, beatSec = 0.5;
};

// Draws a note's vibrato. `full`: with the tool (the envelope and its handles); else only the wave,
// dimmed. `hot`: the handle under the pointer (Hit::None: none).
void drawNote(ImDrawList* dl, const Note& n, const Box& b, const Time& tm, bool full, Hit::Kind hot, bool selected);
// The part of the note's vibrato at `m` (Hit::None when off the note).
Hit::Kind hitTest(const Note& n, const Box& b, ImVec2 m);

// A drag in progress.
struct Drag {
  Hit hit;
  ImVec2 start;
  std::vector<Note> base;       // the notes at the press
  std::vector<size_t> targets;  // the pressed note first, then the other selected notes
  int axis = 0;                 // body drags: 0 undecided, 1 depth (vertical), 2 rate (sideways)
};
// The notes as the drag leaves them; `readout` the line shown by the pointer. False when nothing
// changed yet (below the drag threshold).
bool dragTo(Drag& d, ImVec2 m, const Box& b, bool fine, bool snap, std::vector<Note>& out, std::string& readout);
// The wheel over a note: its rate one step faster (up) or slower; returns the notes changed.
void wheelRate(std::vector<Note>& ns, const std::vector<size_t>& targets, float wheel);

// The readout by the pointer ("Depth 95 ct · rate 5.5 Hz · 3 notes").
void readoutTip(const std::string& text);

// The right-click menu's body over `targets` (the clicked note first). Changes `ns`; `label` names
// the undo step. True when something changed.
bool menuBody(App& a, std::vector<Note>& ns, const std::vector<size_t>& targets, const char*& label);

// The toolbar strip (VIBRATO, preset, cents, rate, Sync): the defaults for new vibrato. Edits also
// apply to the selected notes that have vibrato: `ns` changed, true returned, `label` set.
bool toolbarStrip(App& a, std::vector<Note>* ns, const std::vector<size_t>& selected, const char*& label);
// The defaults new vibrato starts from (the strip's).
const NoteVibrato& defaults();

// The hint for the tool, with how this channel's instrument plays it (each note's own pitch, or
// the channel pitch bend that overlapping notes share).
std::string toolHint(App& a, ObjectId channel);

// A note's vibrato as words for its tooltip ("Vibrato 35 ct, 5.6 Hz, fade in 30%").
std::string describe(const NoteVibrato& v);

}  // namespace gl::pianovib
