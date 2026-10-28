// Gridline — J-27 menu parity sweep: the operations behind menu items the
// reference DAW has and Gridline lacked (playlist track macros, audio clip
// batch edits, automation clip merging / conversion, chord and scale helpers,
// time selection and marker helpers, the song time display formats, the score
// logger, channel cloning with links, live-area preparation, layer helpers).
// Song edits go through Document (one undo step each); note tools are pure
// functions on a note list like NoteTools. Tested in tests/test_menu_parity.cpp.
#pragma once
#include <cstdint>
#include <deque>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include "Document.h"
#include "MenuOps.h"
#include "MidiFile.h"

namespace gl::ops {

// ------------------------------------------------------- playlist tracks ----
// Locks (or unlocks) every playlist track of the current arrangement. Returns
// how many tracks changed.
int setAllTracksLocked(Document& d, bool locked);
// Folds (or unfolds) every track group. Returns how many groups changed.
int setAllGroupsFolded(Document& d, bool folded);
// The group a track belongs to: [head, head + size] (head == -1: none).
std::pair<int, int> trackGroupOf(const Song& s, int track);
// Gives the tracks of the group holding `track` a gradient from the group
// head's colour to a darker shade of it. False when the track is not grouped.
bool autoColorTrackGroup(Document& d, int track);
// Moves every clip of `pattern` onto `track` (positions kept). Returns how many moved.
int sendPatternClipsToTrack(Document& d, ObjectId pattern, int track);
// First playlist track with no clip at all (-1: none).
int firstEmptyTrack(const Song& s);

// ------------------------------------------------------------ audio clips ----
// Pitch of the audio clips (others are skipped) moves by `cents`. Returns how many changed.
int pitchAudioClips(Document& d, const std::set<ObjectId>& clips, float cents);
// Flips the reverse switch of the audio clips. Returns how many changed.
int reverseAudioClips(Document& d, const std::set<ObjectId>& clips);
// The audio clip gets its own copy of its sample channel (so its channel
// settings can change without touching the other clips). Returns the new
// channel (kNoId: not an audio clip).
ObjectId makeAudioClipUnique(Document& d, ObjectId clip);
// Sampler time mode of the channels playing audio clips: 0 = resample (pitch
// follows speed), 7 = Rubber Band, any other value the stretch (J-153 / J-157,
// engine/TimeStretch). `clips` empty = every audio clip.
// Returns how many channels changed.
int setAudioClipTimeMode(Document& d, const std::set<ObjectId>& clips, int mode);
// Declick mode (0 off … 4 smooth, the sampler's "Declick" choice) of the
// sampler channels among `channels`. Returns how many changed.
int setSamplerDeclick(Document& d, const std::vector<ObjectId>& channels, int mode);

// ------------------------------------------------------------- automation ----
// Merges the automation clips among `clips` that drive the same parameter as
// the first of them into one automation clip spanning them all (placed on the
// earliest clip's track; values kept in parameter terms). Returns the new
// playlist clip (kNoId: fewer than two such clips).
ObjectId mergeAutomationClips(Document& d, const std::set<ObjectId>& clips);
// Turns a pattern automation lane into an automation clip: one placement per
// playlist clip of the pattern (or one at the song start when it is not
// placed), on the first empty track; the lane is removed. Returns the
// automation clip contents id (kNoId: no such lane).
ObjectId laneToAutomationClip(Document& d, ObjectId pattern, ObjectId owner, uint32_t index);
// Automation clip contents without any playlist placement.
std::vector<ObjectId> unusedAutomationClips(const Song& s);
int deleteUnusedAutomationClips(Document& d);

// ------------------------------------------------------------------ notes ----
// Keeps, in every `period` ticks, only the notes that start in part `part`
// (1-based) of `parts` equal parts; the other affected notes are deleted
// (`remove` = true: the notes in that part are deleted instead).
void keepTimeSlot(std::vector<Note>& notes, NoteSel& sel, int part, int parts, Tick period, bool remove = false);
// Inverts every chord (notes sharing a start): dir > 0 moves its lowest note
// up an octave, dir < 0 its highest note down an octave.
void invertChords(std::vector<Note>& notes, NoteSel& sel, int dir);
// Selects the notes that share their start with another note (stacked / chords).
void selectStacked(const std::vector<Note>& notes, NoteSel& sel);
// Best-fitting scale for the notes (selected, else all): root 0..11 and a
// tools::scales() index (> 0). `fit` = share of the notes (by length) in the scale.
struct ScaleGuess {
  int root = 0;
  int scale = 0;
  double fit = 0;
};
ScaleGuess detectScale(const std::vector<Note>& notes, const NoteSel& sel);
// A chord built from the scale (tools::scales() index > 0, root 0..11) by
// stacking every other scale degree: `count` notes with `pitch` as the lowest
// note (bottom up) or as the highest (top down). The pitch is first moved onto
// the scale. Without a scale the result is just {pitch}.
std::vector<int> scaleChord(int pitch, int root, int scale, int count, bool topDown);
// Chord name of the notes (selected, else all), e.g. "C", "Am7", "F#sus4/C#";
// "" when there are fewer than two distinct pitch classes or no match.
std::string chordName(const std::vector<Note>& notes, const NoteSel& sel);

// ----------------------------------------------------- time selection ----
// Time range covered by the clips (end of the last one); {0, 0} when none.
std::pair<Tick, Tick> clipsTimeRange(const Song& s, const std::set<ObjectId>& clips);
// The time selection moved by its own length (dir < 0: earlier, clamped at 0).
std::pair<Tick, Tick> shiftTimeSelection(Tick from, Tick to, int dir);

// ------------------------------------------------------------- markers ----
// Two markers at the start and the end of [from, to). Returns how many were added.
int addMarkerPair(Document& d, Tick from, Tick to);
// A loop marker at `from` and a plain end marker at `to` (playback repeats the range).
int placeLoopMarkers(Document& d, Tick from, Tick to);
// J-85 (FL "loop" marker): one step to a song loop. A marker "Loop" with the
// SongLoop action at `pos`: song playback that reaches the song end goes on
// from it. A song has one (FL's rule): an earlier song loop marker is removed,
// and placing it where it already is removes it (toggle). One undo step.
// Returns the new marker's index (-1: removed, or `pos` < 0).
int addSongLoopMarker(Document& d, Tick pos);
// FL's "Place loop" (Shift+T): loop markers around the time selection
// [selStart, selEnd) when there is one, else a song loop marker at `pos`.
// Returns the loop marker's index (-1: nothing placed).
int placeLoop(Document& d, Tick selStart, Tick selEnd, Tick pos);
// Index of the song loop marker playback returns to at the song end (the last
// one before it), -1 when none.
int songLoopMarker(const Song& s);
// A skip marker at `pos` and a marker at the next bar line: playback jumps
// from `pos` to the next bar. Returns the skip marker's index.
int addJumpToNextBar(Document& d, Tick pos);

// ---------------------------------------------------------- time display ----
enum class TimeFormat : int {
  BarBeatTick = 0,   // 001:01:000 (middle unit: beat or step)
  MinSecCs,          // 0:00:00 minutes : seconds : centiseconds
  Smpte,             // hh:mm:ss:ff at a frame rate
  Count
};
// Frame rates offered for SMPTE (fps; 29.97 / 59.94 with drop or non-drop frames).
struct SmpteRate {
  const char* name;
  double fps;
  bool drop;
};
const std::vector<SmpteRate>& smpteRates();
// Song time at `tick` (tempo automation followed for clock formats). The
// bar format uses beats, or steps (a quarter of a beat) when `steps`.
std::string formatSongTime(const Song& s, double tick, TimeFormat f, bool steps = false, int smpteRate = 1);

// ------------------------------------------------------------ score log ----
// Every note played on MIDI inputs, kept for a while so a performance played
// while not recording can be put into a pattern afterwards.
struct ScoreLog {
  struct Event {
    double time = 0;  // seconds (any monotonic clock)
    int pitch = 60;
    int velocity = 0;  // 0 = note off
  };
  std::deque<Event> events;
  double keepSeconds = 600;  // older events are dropped
  bool enabled = true;
  void noteOn(double t, int pitch, int velocity);
  void noteOff(double t, int pitch);
  void clear() { events.clear(); }
  void prune(double now);
};
// Notes played in the last `seconds` before `now`, in ticks at `bpm` / `ppq`,
// the first note starting at 0. Notes still held end at `now`.
std::vector<Note> scoreLogNotes(const ScoreLog& log, double now, double seconds, double bpm, int ppq);
// Writes those notes into the pattern's channel (merged with what is there, or
// replacing it); the pattern grows to hold them. Returns how many notes.
int dumpScoreLog(Document& d, ObjectId pattern, ObjectId channel, const ScoreLog& log, double now, double seconds,
                 bool replace);
// A one-track MIDI file of the notes (the first note at tick 0) at the song's
// PPQ and `bpm` — "save the note clipboard to the browser".
MidiFileData notesToMidiFile(const std::vector<Note>& notes, int ppq, double bpm, const std::string& name);

// ------------------------------------------------------------- channels ----
// Clones the channel (after it, with its pattern data) and duplicates the MIDI
// controller links and internal controller links that drive its parameters,
// so the copy follows the same controls. Returns the clone.
ObjectId cloneChannelWithLinks(Document& d, ObjectId channel);
// Repeats the first bar of the channel's step row through the whole pattern.
bool autoFillSteps(Document& d, ObjectId pattern, ObjectId channel);
// Puts a layer's children into a (new) channel display group named `name`.
// Returns the group index (-1: not a layer).
int groupLayerChildren(Document& d, ObjectId layer, const std::string& name);
// Deletes a layer's child channels (the layer stays). Returns how many.
int deleteLayerChildren(Document& d, ObjectId layer);

// ------------------------------------------------------------ live mode ----
// Prepares the arrangement for live launching: every distinct clip source of
// each track is placed once, one after the other, in a new live area at the
// song start (the arrangement moves right to make room). Returns the area
// length in ticks (0: nothing to prepare).
Tick prepareLiveArea(Document& d);

}  // namespace gl::ops
