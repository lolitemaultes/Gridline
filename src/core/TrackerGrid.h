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
#include "TrackerChannel.h"

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
std::string noteText(int pitch, bool flats = false);  // ("Db4" with flats)    // "C-4", "C#4" (Gridline's names: 60 = C-4)
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
  bool carried[4] = {true, true, true, true};  // the parts it holds (Part order: note, volume, pan, effect; the copy mask)
  bool empty() const { return tracks.empty() || rows <= 0; }
};
std::vector<Note> copyRows(const std::vector<Note>& notes, const std::vector<uint8_t>& steps, const std::vector<StepProps>& props,
                           const Grid& g, int r0, int r1);
// Removes the notes starting in rows [r0, r1] (and the steps there).
Result cutRows(std::vector<Note>& notes, std::vector<uint8_t>& steps, const Grid& g, int r0, int r1);
// Pastes one track of a block at `row`: the notes starting in the rows it covers go first.
Result pasteRows(std::vector<Note>& notes, std::vector<uint8_t>& steps, const Grid& g, int row, int rows, const std::vector<Note>& block);

// ---- clipboard masks (the Advanced edit box) ----
// The parts of a cell the masks act on: the note itself (pitch, place and length), its volume
// (velocity), its pan and its effect letters.
enum class Part : uint8_t { Note, Volume, Pan, Effect, Count };
constexpr int kParts = static_cast<int>(Part::Count);
static_assert(kParts == 4, "Block::carried holds one flag per part");
struct Masks {
  bool copy[kParts] = {true, true, true, true};         // parts a copy or cut takes
  bool paste[kParts] = {true, true, true, true};        // parts a paste writes
  bool transparent[kParts] = {false, false, false, false};  // a pasted part left empty keeps what is there
};
// A part at Gridline's default (no volume, pan or effect set: velocity 100, centre pan, no letter).
bool partEmpty(const Note& n, Part p);
// Copies part `p` (not the note part) from `from` into `to`.
void copyPart(const Note& from, Note& to, Part p);
// The copy mask on copied rows: the parts it leaves out go back to their defaults.
void maskCopied(std::vector<Note>& block, const Masks& m);
// A paste through the masks. `carried`: the parts the block holds (the copy mask when it was
// taken). Row by row: with the note part pasted, the block's notes replace the row's (each keeps
// the parts not pasted from the note it replaces, the first with the first); transparent, a row
// the block leaves empty keeps its notes. Without the note part, the row's notes keep their
// pitch and place and take the pasted parts from the block's notes on that row, in order. A
// transparent part does not overwrite with an empty one.
Result pasteRowsMasked(std::vector<Note>& notes, std::vector<uint8_t>& steps, const Grid& g, int row, int rows, const std::vector<Note>& block,
                       const bool carried[kParts], const Masks& m);

// ---- an imported module's cells (K-106; Note::tracker, core/TrackerStream.h) ----
// A note imported from a module carries the module's cells of the rows it sounds through: its
// own row (note, instrument, volume column, effect) and the rows after it on its tracker channel
// (volume column and effects that act on the sounding voice). The Tracker shows them in the
// module's own notation; nothing is converted, so showing them changes nothing.
//
// The cells of `n` from its first row: out[k] is the module cell of row (start row + k). At most
// `maxRows`. Empty for a note that carries none.
std::vector<tracker::Cell> moduleRows(const Note& n, int maxRows);
// The imported note sounding in `column` on `row` whose cells cover the row: its index in
// `notes`, and `k` the row's place in its cells. -1 when none.
int moduleNoteAt(const std::vector<Note>& notes, const Track& t, const Grid& g, int row, int column, int* k);
// The volume column as the classic editor writes it: "40" (set volume, 00 to 40), "-3", "+3",
// "D3", "U3" (fine down, up), "S3" (vibrato speed), "V3" (vibrato), "P8" (pan), "L3", "R3" (pan
// slides), "M3" (glide). "" when empty.
std::string moduleVolText(uint8_t vol);
// The effect: its digit or letter (0 to 9, A to Z) and two hex digits, "" when empty.
std::string moduleEffectText(uint8_t efx, uint8_t param);
// The cell of row `k` of a stream replaced, in place (Gridline stores only these bytes, so the
// edit is exact and nothing else in the stream moves). False: no such row, or the stream would
// grow past what a voice keeps.
bool setStreamCell(std::vector<uint8_t>& stream, int k, const tracker::Cell& c);
// Edits of a module cell that change only that cell's own bytes and the voice it is on, and
// that Gridline therefore stores exactly: the volume column's set volume (0 to 0x40, -1 clears
// it) and the effects 8 (set pan) and C (set volume) (`efx` -1 clears it). Any other change of a
// module cell is refused with the reason (the rest of the module was played out at import).
Result setModuleVolume(std::vector<Note>& notes, const Track& t, const Grid& g, int row, int column, int volume);
Result setModuleEffect(std::vector<Note>& notes, const Track& t, const Grid& g, int row, int column, int efx, int param);

// R8-03: the rows of an imported pattern that the song plays. A module's pattern of 33 rows comes
// in as whole bars (48 rows) and its song clips play the first 33: the rows past the furthest any
// clip of it reaches never play. g.rows for every other pattern (no module notes, a clip playing
// it whole or looping it, or no clip).
int playedRows(const Song& s, const Pattern& p, const Grid& g);

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
// GitHub #70: where the editors of `pattern` (piano roll, tracker) show the play position, in
// pattern ticks with the fraction kept so the line glides (-1: it is not playing there). Pattern
// mode: the engine's position while `pattern` is the pattern playing. Song mode: the pattern tick
// under the song position `pos`, through the top clip of the pattern there (patternTickAt).
double patternPlayPos(const Song& s, ObjectId pattern, bool songMode, ObjectId playingPattern, double pos);
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
// R6-02: a pattern's new length, the song keeping its order: each song clip that plays the pattern's
// own length grows or shrinks with it and the clips after it move by the difference (the song gets
// longer or shorter; two entries never overlap). One undo step with the length change when the
// caller groups them; a group of its own otherwise.
void setPatternLengthInSong(Document& d, ObjectId pattern, int bars, int stepsPerBar);

}  // namespace gl::trk
