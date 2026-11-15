// Gridline - the tracker pattern editor (K-106, the Pattern editor part) and the Tracker layout's
// header band (the Order list part): rows of notes per rack channel, typed on the computer keyboard
// in the classic tracker way.
//
// The editor shows App::curPattern, the pattern the piano roll and the channel rack show, through
// core/TrackerGrid.h: one row per step, one track per rack channel (its overlapping notes in note
// columns), cells with note, instrument, volume, pan and an effect letter. Every edit is a Document
// command (setNotes, the steps through editPattern; several channels inside one group), so undo,
// saving and collaboration treat it as a piano roll edit and every layout sees the same notes.
// Cursor, edit mode, edit step and selection are this person's view state, never saved or sent.
//
// Keys: while the editor has the keyboard (its window focused, no text field), main.cpp hands it the
// SDL key events first (sdlKey). Note keys go by where the key sits (the typing keyboard's two rows,
// any keyboard layout), values by the character typed. The chords it uses run no other command
// (keys::setEditorKeys); Space, the function keys (F1 to F8 pick the octave in the Tracker key set)
// and the other shortcuts keep working.
#pragma once
#include <string>
#include <vector>

namespace gl {
class App;
}

namespace gl::trackerview {

// The Pattern editor part's body.
void patternEditor(App& a);
// The Order list part's body: in the Tracker layout the classic header band (order list, song box,
// scopes, buttons, instrument and sample lists, page strip); elsewhere the order list alone.
void orderList(App& a);

// main.cpp, for every SDL key event: true when the pattern editor takes it (the typing keyboard and
// the other handlers then leave it). Key-ups end the notes a key auditioned and are never taken.
bool sdlKey(App& a, int scancode, int keycode, int mods, bool down, bool repeat);
// The editor had the keyboard on the last frame.
bool hasKeyboard();
// A header button was clicked: the keyboard goes back to the pattern editor when its page is the
// one shown (the classic tracker's buttons never take the keys from the pattern).
void keysBackToPattern();
// The editor was drawn on the last frame (it is the pattern page: PAT does not bring the rack forward over it).
bool shown();

// View state shared by the editor and the header band.
int editStep();
void setEditStep(int n);
bool editMode();
void setEditMode(bool on);
// Block tools on the selection (or the cursor's row on its track): transpose by semitones, and
// expand / shrink (rows spread twice as far apart, or every other row kept closer).
void transposeBlock(App& a, int semis);
void scaleBlock(App& a, bool expand);
// Transpose by scope: the cursor's track, every track of the pattern, or the block.
enum class TransposeScope { Track, Pattern, Block };
void transposeScope(App& a, TransposeScope scope, int semis);
// The Edit and Pattern menus' pattern commands (the same as the editor's keys; a menu item runs
// whether edit mode is on or not). Each edit is one undo step through the Document.
enum class BlockOp { Cut, Copy, Paste, Clear, SelectAll, Deselect };
void blockOp(App& a, BlockOp op);
bool hasBlock();           // a block is marked
bool hasClip();            // the tracker clipboard holds rows
// A row inserted / deleted at the cursor: the cursor's track, or every track.
void insertRow(App& a, bool everyTrack);
void deleteRow(App& a, bool everyTrack);
// Volume, pan or effect values on a straight line through the block (the field under the cursor;
// on the note field, the volume). The cursor's track from row to the pattern end with no block.
void interpolate(App& a);
// Note columns of the cursor's track: one more / one fewer (empty ones only; view only).
void addColumn(App& a, int delta);
bool follow();
void setFollow(bool on);
// The editor's cursor field: true on an effect or value field (the menu words Interpolate by it).
int cursorField();

// The header band hidden (the pattern gets the room) and back.
void setExtended(App& a, bool on);
bool extended();
// The centre page shown in the Tracker layout (a window homed in tracker.pages, or the instrument
// page, which opens the selected channel's instrument).
void showPage(App& a, const char* window);
// Brings a centre page's hidden tab to the front (over the next frames, until it shows). The
// centre has no tab bar, so focus alone does not pick the tab; the header band pumps this.
void bringPage(const char* window);
void pumpPage(App& a);
// What the header shows: 0 its boxes, 1 Help, 2 Files (the header-left swap).
void setHeaderSwap(int which);
int headerSwap();

// Dev (--dev-input "tracker ..."):
//   tracker dump                 the shown pattern: one line per non-empty cell, the cursor and the play row
//   tracker focus                gives the editor the keyboard
//   tracker cursor <track> <row> [column] [field]
//   tracker edit on|off · tracker step <n> · tracker follow on|off · tracker select <t0> <r0> <t1> <r1>
//   tracker geometry             the editor's rectangle, the row height and the cursor cell's rectangle
//   tracker page <window>        a centre page to the front ("Pattern editor", "Playlist", "Channel rack", "Mixer", "Live pads", "Piano roll")
//   tracker swap none|help|files the header's swap
void devCommand(App& a, const std::vector<std::string>& args, int frame);

}  // namespace gl::trackerview
