// Gridline — K-128: core helpers for Smooth's Event List and Score Editor.
// Pure, UI-independent formatting, parsing, key signatures, pitch spelling,
// duration classification, staff assignment and beaming.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include "Song.h"

namespace gl::smoothscore {

// ---- Event List formatting & parsing ----

// Formats a tick position as 1-based "Bar.Beat.Div.Tick" (e.g. "1.1.1.1"), where Div is a sixteenth note.
std::string formatBbtPos(const Song& s, Tick t, const Pattern* pat = nullptr);
// Formats a tick duration as 0-based "Bars.Beats.Divs.Ticks" (e.g. "0.1.0.0" for one beat).
std::string formatBbtLen(const Song& s, Tick len, const Pattern* pat = nullptr);

// Parses "Bar.Beat.Div.Tick" (dots or spaces; trailing fields optional) into a tick >= 0.
bool parseBbtPos(const Song& s, const std::string& text, Tick& out, const Pattern* pat = nullptr);
// Parses a 0-based "Bars.Beats.Divs.Ticks" duration into a tick >= 1.
bool parseBbtLen(const Song& s, const std::string& text, Tick& out, const Pattern* pat = nullptr);

// MIDI pitch (0..127) <-> note name ("C4" = 60, "C#4" = 61, ...).
std::string formatPitch(int pitch);
bool parsePitch(const std::string& text, int& out);

// ---- Event List row model ----

enum class EventKind : uint8_t { Note = 0, Velocity = 1, ModCc = 2 };

struct EventFilter {
  bool notes = true;
  bool velocity = true;
  bool modCc = false;
};

struct EventRow {
  EventKind kind = EventKind::Note;
  Tick pos = 0;
  Tick length = 0;
  int channelNum = 1;       // 1-based display channel index
  int pitch = 60;           // Note / Velocity: MIDI pitch; ModCc: param index / CC
  int value = 100;          // Note: velocity (0..127); Velocity: pressure/accent (0..127); ModCc: 0..127
  size_t noteIndex = 0;     // index in Pattern::notes[channel] (valid for Note & Velocity)
  int autoIndex = -1;       // index in Pattern::automation (valid for ModCc >= 0)
  int autoPointIndex = -1;  // index in PatternAutomation::points (valid for ModCc)
  std::string status;       // "Note", "Vel", "Mod", "CC"
  std::string detail;       // e.g. "C4" or parameter name
};

// Builds a chronologically sorted list of events for `(pat, chId)` filtered by `filter`.
std::vector<EventRow> buildEventRows(const Song& s, const Pattern& pat, ObjectId chId, const EventFilter& filter);

// ---- Score Editor notation model ----

enum class ClefMode : int { Treble = 0, Bass = 1, Grand = 2 };
enum class QuantGrid : int { Quarter = 0, Eighth = 1, Sixteenth = 2 };

// Chooses Treble, Bass, or Grand from the pitch range of `notes`.
ClefMode autoClefForNotes(const std::vector<Note>& notes);

// Grid step in ticks for a display quantize choice (minimum 1 tick).
Tick gridStepTicks(int ppq, QuantGrid grid);
Tick quantizeTick(Tick t, int ppq, QuantGrid grid);

// Circle-of-fifths key signature (-7..+7; negative = flats, positive = sharps) from (root 0..11, tools::scales() index).
int keySignatureFifths(int root, int scale);
std::string keySignatureLabel(int root, int scale);

// Diatonic staff steps for key signature accidentals (in standard engraving order).
// Step convention: C0 = 0, D0 = 1, ..., C4 (middle C) = 28, E4 = 30, G4 = 32, B4 = 34, D5 = 36, F5 = 38.
// Bass staff lines: G2 = 18, B2 = 20, D3 = 22, F3 = 24, A3 = 26.
std::vector<int> keySigSteps(int fifths, bool treble);

struct SpelledPitch {
  int diatonicStep = 28;  // C4 = 28
  int accidental = 0;     // -1 = flat, 0 = natural, +1 = sharp
};

SpelledPitch spellPitch(int midiPitch, int keyFifths);
// Inverse of spellPitch: maps a staff diatonic step to MIDI pitch (0..127) applying the key signature.
int diatonicToMidi(int diatonicStep, int keyFifths);

enum class NoteValue : uint8_t {
  Whole = 0,
  DottedHalf,
  Half,
  DottedQuarter,
  Quarter,
  Eighth,
  Sixteenth
};

NoteValue classifyDuration(Tick ticks, int ppq, QuantGrid grid);
inline bool isHollowHead(NoteValue v) { return v == NoteValue::Whole || v == NoteValue::DottedHalf || v == NoteValue::Half; }
inline bool isDotted(NoteValue v) { return v == NoteValue::DottedHalf || v == NoteValue::DottedQuarter; }
inline bool hasStem(NoteValue v) { return v != NoteValue::Whole; }
inline int flagCount(NoteValue v) { return v == NoteValue::Eighth ? 1 : v == NoteValue::Sixteenth ? 2 : 0; }

struct ScoreNote {
  size_t noteIndex = 0;
  Tick qStart = 0;
  Tick qLength = 0;
  int pitch = 60;
  int velocity = 100;
  int diatonicStep = 28;
  int drawnAccidental = -2;  // -2 = none, -1 = flat, 0 = natural, +1 = sharp
  bool bassStaff = false;
  bool stemUp = true;
  NoteValue value = NoteValue::Quarter;
  int beamGroup = -1;        // >= 0 when beamed with adjacent 8th/16th notes in the same beat
};

// Computes quantized positions, staff assignment, per-bar accidentals, stem directions and beam groups.
std::vector<ScoreNote> layoutScoreNotes(const Song& s, const Pattern& pat, const std::vector<Note>& notes,
                                        ClefMode clef, QuantGrid grid, int keyFifths);

}  // namespace gl::smoothscore
