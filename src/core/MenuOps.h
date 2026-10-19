// Gridline — editing operations behind the main menu and the context menus.
// Song-level operations go through Document (one undo step each); note tools
// are pure functions on a note list so the piano roll, menus and tests share them.
#pragma once
#include <cstdint>
#include <set>
#include <string>
#include <vector>
#include "Document.h"

namespace gl::ops {

// ---------------------------------------------------------------- patterns ----
bool patternIsEmpty(const Pattern& p);
// First empty pattern in list order (kNoId if none).
ObjectId firstEmptyPattern(const Song& s);
// Next empty pattern after `after` (wrapping); kNoId if none.
ObjectId nextEmptyPattern(const Song& s, ObjectId after);
// Next empty pattern after `after`, or a new one appended after the last pattern.
ObjectId nextEmptyOrNewPattern(Document& d, ObjectId after);
// Patterns with no clip in the playlist.
std::vector<ObjectId> unusedPatterns(const Song& s);
// Deletes unused patterns, keeping at least one pattern. Returns how many were deleted.
int deleteUnusedPatterns(Document& d);
// Inserts a new empty pattern before `before` (or at the end). Returns its id.
ObjectId insertPattern(Document& d, ObjectId before, const std::string& name);
bool movePattern(Document& d, ObjectId id, int newIndex);
bool setPatternColor(Document& d, ObjectId id, Rgb color);
// Transposes every piano-roll note of the pattern (all channels).
bool transposePattern(Document& d, ObjectId id, int semitones);
// One new pattern per channel that has data in `id`, named "<pattern> - <channel>".
// Playlist clips of the original are replaced by one clip per new pattern on
// consecutive tracks. Returns the new pattern ids (empty if nothing to split).
std::vector<ObjectId> splitPatternByChannel(Document& d, ObjectId id);
// Default name for the next new pattern ("Pattern N").
std::string nextPatternName(const Song& s);

// ---------------------------------------------------------------- channels ----
// Fills the channel's step row in the pattern: every `every` steps from `offset`.
bool fillSteps(Document& d, ObjectId pattern, ObjectId channel, int every, int offset = 0, uint8_t velocity = 100);
// Rotates the channel's steps and notes by one step (dir < 0: left, > 0: right).
bool rotateChannel(Document& d, ObjectId pattern, ObjectId channel, int dir);
// Removes the channel's steps and notes from the pattern.
bool clearChannel(Document& d, ObjectId pattern, ObjectId channel);
bool setChannelsColor(Document& d, const std::vector<ObjectId>& channels, Rgb color);
// Blends colours from the first to the last channel's colour (rack order).
bool gradientColor(Document& d, const std::vector<ObjectId>& channels);
bool setChannelsMuted(Document& d, const std::vector<ObjectId>& channels, bool muted);
// Transposes the channels' notes in one pattern.
bool transposeChannels(Document& d, ObjectId pattern, const std::vector<ObjectId>& channels, int semitones);
enum class ChannelSort { Name, Color, MixerTrack, Instrument };
bool sortChannels(Document& d, ChannelSort by);
// Moves several channels one row up (dir < 0) or down (dir > 0) keeping their order.
bool moveChannels(Document& d, const std::vector<ObjectId>& channels, int dir);
std::vector<ObjectId> cloneChannels(Document& d, const std::vector<ObjectId>& channels);
bool removeChannels(Document& d, const std::vector<ObjectId>& channels);
// Channels without steps or notes in any pattern.
std::vector<ObjectId> unusedChannels(const Song& s);
// Insert tracks nothing is routed to: no channels, no effects, no incoming sends, default name.
bool mixerTrackIsFree(const Song& s, ObjectId track);
// Routes each channel to its own free insert track (adding tracks when needed)
// and names/colours the track after the channel. Returns the tracks used.
std::vector<ObjectId> routeToFreeMixerTracks(Document& d, const std::vector<ObjectId>& channels);
// Routes the channels to `track`, or (consecutive) to track, track+1, …
bool routeChannels(Document& d, const std::vector<ObjectId>& channels, ObjectId track, bool consecutive);

// ----------------------------------------------------------------- playlist ----
bool insertPlaylistTrack(Document& d, int index);   // shifts later clips down
bool deletePlaylistTrack(Document& d, int index);   // deletes its clips, shifts the rest up
bool movePlaylistTrack(Document& d, int index, int dir);  // swaps with a neighbour (and their clips)
// Copies the track (name, colour, clips) into a new track right below it.
bool clonePlaylistTrack(Document& d, int index);
bool removeClips(Document& d, const std::set<ObjectId>& clips);
// Copies the clips right after the end of the selection; returns the new clip ids.
std::vector<ObjectId> duplicateClips(Document& d, const std::set<ObjectId>& clips);
// Gives the clip its own copy of its pattern. Returns the new pattern id.
ObjectId makeClipUnique(Document& d, ObjectId clip);
// Merges the clips into one new pattern (notes; steps become notes at C5), placed
// at the earliest start on the first clip's track. Returns the new clip id.
ObjectId mergeClips(Document& d, const std::set<ObjectId>& clips);
// Splits a clip into pieces of `every` ticks.
std::vector<ObjectId> chopClip(Document& d, ObjectId clip, Tick every);
// Deletes all clips on a track (the track itself stays).
bool clearPlaylistTrack(Document& d, int index);

// -------------------------------------------------------------------- mixer ----
bool moveMixerTrack(Document& d, ObjectId id, int dir);  // never moves the master
// Inserting, resetting and routing mixer tracks: see MixerOps.h (gl::mixops).
// Removes trailing insert tracks that are free. Returns how many were removed.
int trimUnusedMixerTracks(Document& d);
bool setSlotEnabled(Document& d, ObjectId track, int slot, bool enabled);
bool soloMixerTrack(Document& d, ObjectId id);  // exclusive solo toggle

// -------------------------------------------------------------------- notes ----
// All tools act on the selected notes (`sel`, same size as `notes`), or on all
// notes when nothing is selected. They keep `sel` aligned with `notes`.
using NoteSel = std::vector<bool>;
void sortNotes(std::vector<Note>& notes, NoteSel& sel);
// Moves starts (and optionally ends) to the nearest grid line.
void quantize(std::vector<Note>& notes, NoteSel& sel, Tick grid, bool ends);
// Stretches each note to the start of the next note (same channel, any pitch).
void legato(std::vector<Note>& notes, NoteSel& sel);
// Cuts notes into pieces of `len` ticks.
void chop(std::vector<Note>& notes, NoteSel& sel, Tick len);
// Joins touching/overlapping notes of the same pitch.
void glue(std::vector<Note>& notes, NoteSel& sel);
void transpose(std::vector<Note>& notes, NoteSel& sel, int semitones);
void nudge(std::vector<Note>& notes, NoteSel& sel, Tick delta);
// Mirrors pitches around the middle of the selection's range.
void flipVertical(std::vector<Note>& notes, NoteSel& sel);
// Mirrors timing inside the selection's time range.
void flipHorizontal(std::vector<Note>& notes, NoteSel& sel);
// Scales velocities by `factor` (e.g. 0.8) around 0.
void scaleVelocity(std::vector<Note>& notes, NoteSel& sel, double factor);
// Random velocity (±velRange) and timing (±timeRange ticks) offsets; deterministic for a seed.
void humanize(std::vector<Note>& notes, NoteSel& sel, int velRange, Tick timeRange, uint32_t seed);
// Offsets the notes of each chord (same start) by `step` ticks, lowest first.
void strum(std::vector<Note>& notes, NoteSel& sel, Tick step);
// Turns chords (same start) into rising arpeggios of `step`-tick notes over the chord length.
void arpeggiate(std::vector<Note>& notes, NoteSel& sel, Tick step);
// Copies the selection (or all) right after its end; the copies become the selection.
void duplicate(std::vector<Note>& notes, NoteSel& sel, Tick grid);
// Deletes the selected notes (all when none selected).
void deleteNotes(std::vector<Note>& notes, NoteSel& sel);
// Selection helpers.
void selectAll(const std::vector<Note>& notes, NoteSel& sel);
void selectNone(NoteSel& sel);
void invertSelection(NoteSel& sel);
void selectOdd(const std::vector<Note>& notes, NoteSel& sel);  // every other note in time order
void selectSamePitch(const std::vector<Note>& notes, NoteSel& sel);  // pitches of the selection
void selectOverlapping(const std::vector<Note>& notes, NoteSel& sel);
void selectRandom(const std::vector<Note>& notes, NoteSel& sel, uint32_t seed);

// Chord progressions (one chord per bar) for the Tools menu.
struct Progression {
  const char* name;
  std::vector<int> degrees;  // scale degrees 1..7
  bool minor;
};
const std::vector<Progression>& progressions();
// Notes of a progression in the key of `root` (MIDI note), `barTicks` per chord, `bars` long.
std::vector<Note> progressionNotes(const Progression& p, int root, Tick barTicks, int bars, int velocity = 96);
// A simple rising arpeggio riff over the same progression (`stepTicks` per note).
std::vector<Note> progressionRiff(const Progression& p, int root, Tick barTicks, Tick stepTicks, int bars, int velocity = 100);

}  // namespace gl::ops
