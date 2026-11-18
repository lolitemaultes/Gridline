// Gridline — J-31 feature gaps: the chord progression editor window and the
// menu entries for groups & loops, piano roll extras, recording extras, key
// markers, quantized jumps and export by track group. Operations live in
// core/ChordTools, core/Grouping, core/SongTools, engine/AudioLogger and
// engine/AudioRecord (undoable where they edit the song, tested in
// tests/test_j31.cpp). Each *Items function draws a block inside an open menu.
#pragma once
#include <string>
#include <vector>
#include "../core/Types.h"

namespace gl {

class App;

namespace j31 {

// ---- windows and per-frame work (from drawDialogs) ----
void drawWindows(App& a);
void openChordEditor(App& a);
void closeChordEditor(App& a);  // K-112 (dev): keeps the changes, as its close button

// ---- main menu ----
void patternsItems(App& a);  // Patterns: pattern groups
void toolsItems(App& a);     // Tools: audio logger, cancel recording
void optionsItems(App& a);   // Options: quantized jumps
void recordingNamingItems(App& a);   // K-314: the "Name takes after" submenu (inside an open menu)
void recordingOptionItems(App& a);   // K-123: the record options menu's groups (inside an open menu): audio takes
void recordingRollItems(App& a);     // pre-roll and post-roll
void recordingMonitorItems(App& a);  // monitoring of the armed tracks
void recordingControlsMenu(App& a);  // Record controls >: when they open and how they show

// ---- context menus ----
void channelStepItems(App& a, ObjectId channel);                  // channel menu > Steps: step loop, truncated swing
void channelKeyItems(App& a, ObjectId channel);                   // channel menu > Instrument and routing: key range, layer split
void rackItems(App& a, const std::vector<ObjectId>& selection);  // channel rack menu: step loops, truncated swing
void pianoToolItems(App& a);                                     // piano roll › Tools: chord editor, note groups
void pianoViewItems(App& a);                                     // piano roll › View: MIDI snap, ghosts, waveform, key markers
void playlistItems(App& a);                                      // playlist menu: clip groups
void markerItems(App& a, Tick at);                               // playlist › Time markers: add key marker
void mixerSoloItems(App& a, ObjectId track);                     // mixer track menu: alt solo
void mixerRecordItems(App& a, ObjectId track);                   // mixer track menu: record point

// ---- piano roll hooks ----
// Scale highlighting the piano roll shows (key markers when following them).
void pianoKey(App& a, int& root, int& scale);
// Background waveform behind the notes (canvas rect, view start tick, ticks per pixel).
void drawPianoBackground(App& a, float x0, float y0, float x1, float y1, double tick0, double ticksPerPx);
// A click on a ghost note with editable ghosts on: switches to its channel and
// selects the note. Returns true when it handled the click.
bool editGhostAt(App& a, Tick t, int pitch);
// K-198: a note of a channel edited beside the target (PianoRollState::editChannels) under the
// pointer becomes the selection and its channel the target (the old target stays edited).
bool takeEditedNoteAt(App& a, Tick t, int pitch);
// Selection after a click: grouped notes come along.
void expandNoteGroups(App& a);

// ---- transport / MIDI hooks ----
// A song position jump requested by the user (quantized when the option is on).
void songJump(App& a, Tick tick);
// A played note's pitch with "snap incoming MIDI" applied.
int incomingPitch(App& a, int pitch);
// "Cancel recording" was chosen: the session's takes are discarded (AudioRecordUi).
bool takeCancelRequested();
// Stops and discards the takes being recorded (Cancel recording; K-123: the card's Discard this take).
void cancelRecording(App& a);

}  // namespace j31
}  // namespace gl
