// Gridline - the tracker view of a pattern (K-106): rows, tracks, note columns and cells over the
// pattern's own notes and rack steps, and the edits the pattern editor makes, as pure functions.
//
// Nothing here is stored: a row is one step of the pattern (Song::ticksPerStep), a track is a rack
// channel, its note columns are the notes that overlap in time (first free column, in start order),
// a note's OFF is where its length ends. The editor (app/TrackerView.cpp) builds the tracks from the
// song every frame and writes its edits back through the Document (setNotes, setSteps), the same
// commands the piano roll and the channel rack send, so undo, saving and collaboration treat a
// tracker edit as any other, and every layout shows the same notes.
//
// The rule from the plan's risk review holds throughout: the tracker never drops, rounds or rewrites
// data to show it. An edit changes only what was typed; a note it cannot show exactly is marked
// (off the row grid, past the pattern end) and left alone.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "Song.h"

namespace gl {
class Document;
}

namespace gl::trk {

// The rows of a pattern.
struct Grid {
  Tick rowTicks = 1;    // one row = one step
  int rows = 16;        // steps per bar x bars
  int rowsPerBeat = 4;  // highlight every beat ...
  int rowsPerBar = 16;  // ... and every bar
  Tick length() const { return rowTicks * rows; }
  Tick tickOf(int row) const { return rowTicks * row; }
};
Grid gridOf(const Song& s, const Pattern& p);

// The sub-columns of a note column, left to right.
enum class Field : uint8_t { Note, Instrument, Volume, Pan, Effect, Count };
// Cursor positions inside a field (the note is one; values are hex digits; the effect a letter and two digits).
int fieldDigits(Field f);
// Characters a field takes on screen.
int fieldChars(Field f);

// One cell of a note column.
struct Cell {
  enum Kind : uint8_t { Empty, On, Off, Step };
  Kind kind = Empty;
  int note = -1;          // On: the note starting here; Off: the note ending here (index in the channel's notes)
  int step = -1;          // Step: the step's index
  bool offGrid = false;   // On: the start is not on the row; Off: the end is not on the row (marked "~")
};

// A rack channel in the tracker: its note columns and cells.
struct Track {
  ObjectId channel = kNoId;
  int columns = 1;               // note columns shown
  int voices = 0;                // columns the notes need (columns may add empty ones)
  std::vector<int> col;          // per note: its column
  std::vector<Cell> cells;       // rows x columns, row-major
  int pastEnd = 0;               // notes starting at or after the pattern's end (kept, marked)
  bool hasSteps = false;         // the channel has rack steps in this pattern
  int rows = 0;
  const Cell& at(int row, int column) const;
};
// Columns for a channel's notes (first free column, in start order): col[i] for notes[i]. `count`: columns used.
std::vector<int> assignColumns(const std::vector<Note>& notes, int* count = nullptr);
// The tracker track of a channel in a pattern, at least `minColumns` note columns (at most kMaxColumns).
constexpr int kMaxColumns = 16;
Track buildTrack(const Song& s, const Pattern& p, ObjectId channel, const Grid& g, int minColumns = 1);

// ---- text ----
std::string noteText(int pitch);    // "C-4", "C#4" (Gridline's names: 60 = C-4)
std::string hex2(int v);            // "7F"
std::string rowText(int row, bool hex);  // "1F" / "31" (3 digits past FF / 99)

// ---- effect letters on native note properties ----
// Letters, in the order an effect column shows them: G slide, P portamento, T fine tune (signed
// cents, two's complement), X mod X, Y mod Y, R release velocity.
constexpr const char* kEffectLetters = "GPTXYR";
struct Effect {
  char letter = 0;  // 0 = none
  int value = 0;    // 0..255 as shown
  bool exact = true;  // false: the value does not fit two digits (fine tune beyond +-127 cents: "T!!")
};
// The note's properties that differ from Gridline's defaults, as effect letters (in kEffectLetters order).
std::vector<Effect> effectsOf(const Note& n);
// Sets a letter's property from a two-digit value (G and P switch on, whatever the digits). False: unknown letter.
bool setEffect(Note& n, char letter, int value);
// Puts a letter's property back to Gridline's default.
void resetEffect(Note& n, char letter);
// What a letter means, in the status line.
std::string effectHelp(char letter, int value);

// ---- edits on one channel's notes ----
// Every edit works on a copy of a channel's notes (and steps), as setNotes / setSteps take them, and
// reports what it did. `refused` says why nothing changed when the edit is not allowed.
struct Result {
  bool changed = false;
  std::string refused;   // "" = allowed
  int refusedCount = 0;  // transpose: notes left alone because they would leave 0..127
};
// The notes in the order Document::setNotes keeps them (start, then pitch), with its limits.
void normalize(std::vector<Note>& notes);

// A note typed at (row, column): an existing note there takes the new pitch; otherwise a new note
// starts on the row and lasts to the next note in that column (or the pattern's end), and the note
// sounding in that column is cut where the new one starts.
Result putNote(std::vector<Note>& notes, const Track& t, const Grid& g, int row, int column, int pitch, int velocity);
// OFF typed at (row, column): the note sounding in that column ends on the row.
Result putOff(std::vector<Note>& notes, const Track& t, const Grid& g, int row, int column);
// Delete on a field: the note column deletes the note (an OFF: the note sounds on to the next one or
// the end); volume, pan and effect go back to Gridline's defaults. Steps: clears the step (note
// field), velocity and pan back to defaults.
Result clearField(std::vector<Note>& notes, std::vector<uint8_t>& steps, std::vector<StepProps>& props, const Track& t, const Grid& g,
                  int row, int column, Field f);
// A value typed into volume (velocity 0..127, 0 stores 1), pan (0..128, 64 = centre) or the effect.
Result setVolume(std::vector<Note>& notes, std::vector<uint8_t>& steps, const Track& t, int row, int column, int value);
Result setPan(std::vector<Note>& notes, std::vector<StepProps>& props, const Track& t, int row, int column, int value);
// `shown`: the letter the cell showed (it is reset when another letter is typed over it).
Result setEffectAt(std::vector<Note>& notes, const Track& t, int row, int column, char shown, char letter, int value);

// Insert a row at `row` (the notes on and after it move down one row; a note across it grows) and
// delete the row (its notes go; the later ones move up; a note across it shrinks). Whole track.
// Notes pushed past the end are kept. Steps move too; a step that would fall off the end refuses.
Result insertRow(std::vector<Note>& notes, std::vector<uint8_t>& steps, std::vector<StepProps>& props, const Grid& g, int row);
Result deleteRow(std::vector<Note>& notes, std::vector<uint8_t>& steps, std::vector<StepProps>& props, const Grid& g, int row);
// Moves the notes starting in rows [r0, r1] by `semis`; those that would leave 0..127 stay (counted).
Result transpose(std::vector<Note>& notes, const Grid& g, int r0, int r1, int semis);
// Expand (x2) or shrink (/2) the notes starting in rows [r0, r1] about row r0: starts and lengths
// scale; notes pushed past the end are kept. Shrinking a note shorter than one tick keeps one tick.
Result scaleRows(std::vector<Note>& notes, const Grid& g, int r0, int r1, bool expand);

// Interpolate: the notes starting in rows [r0, r1] get values on a straight line, by start time,
// from the first of them to the last (the two ends keep theirs). `f`: Volume (velocity), Pan, or
// Effect (the first note's letter, when the last note shows the same one; G and P have no value).
// Refused with fewer than three notes, or effect ends without a shared value letter.
Result interpolate(std::vector<Note>& notes, const Grid& g, int r0, int r1, Field f);

// ---- blocks ----
// A copied block: rows, and per track the notes starting in it (start relative to the block's first
// row). Rack steps are copied as one-row notes.
struct Block {
  int rows = 0;
  std::vector<std::vector<Note>> tracks;
  bool empty() const { return tracks.empty() || rows <= 0; }
};
std::vector<Note> copyRows(const std::vector<Note>& notes, const std::vector<uint8_t>& steps, const std::vector<StepProps>& props,
                           const Grid& g, int r0, int r1);
// Removes the notes starting in rows [r0, r1] (and the steps there).
Result cutRows(std::vector<Note>& notes, std::vector<uint8_t>& steps, const Grid& g, int r0, int r1);
// Pastes one track of a block at `row`: the notes starting in the rows it covers go first.
Result pasteRows(std::vector<Note>& notes, std::vector<uint8_t>& steps, const Grid& g, int row, int rows, const std::vector<Note>& block);

// ---- the song ----
// One order list entry: a slice of the song between two places where a pattern clip starts or ends.
// T-13: where no pattern clip plays but audio clips do, the slices between the audio clips' starts
// and ends are entries too (pattern kNoId, an audio part); an audio clip under a pattern entry is
// counted on it and does not cut it.
struct OrderEntry {
  Tick start = 0, end = 0;
  ObjectId pattern = kNoId;     // the top clip's pattern (kNoId: an audio part)
  int patterns = 1;             // distinct patterns playing in the slice (more than one: a combined entry; 0: an audio part)
  std::vector<ObjectId> clips;  // the pattern clips covering it, top track first
  std::vector<ObjectId> audio;  // the audio clips playing in it, top track first
};
// Derived from the playlist (the live area left out); nothing is stored.
std::vector<OrderEntry> orderList(const Song& s);
// Whether the song has a pattern entry (an audio part alone is not one).
bool hasPatternEntries(const Song& s);
// The pattern tick `pattern` plays at song position `pos`, -1 when no clip of it plays there.
Tick patternTickAt(const Song& s, ObjectId pattern, Tick pos);
// The pattern of the top pattern clip under `pos` (kNoId: none).
ObjectId patternAt(const Song& s, Tick pos);
// Gridline's own pattern number (1-based, as Studio names it).
int patternNumber(const Song& s, ObjectId pattern);

// Order list edits. The order list is the playlist seen as a list, so these edit the playlist,
// each as one undo step (and so collaboration sends them like any clip edit):
//   insert: a clip of `pattern` at entry `at` (at == the list's size: after the last entry),
//           on the entry's top track (track 0 after the end); the song from there moves right;
//   remove: the pattern clips of entry `at` go and the song after it moves left.
// Nothing is dropped: an entry whose clips run into other entries, or with audio or automation
// clips starting inside it, is refused (it is edited on the Song page).
enum class OrderEdit { Done, Nothing, Spans, OtherClips };
OrderEdit orderInsert(Document& d, int at, ObjectId pattern);
OrderEdit orderRemove(Document& d, int at);
//   pattern: entry `at` plays `pattern` instead (its one clip changes pattern; when the clip takes
//            its pattern's length, the song after it moves by the difference).
OrderEdit orderSetPattern(Document& d, int at, ObjectId pattern);

}  // namespace gl::trk
