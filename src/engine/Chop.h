// Gridline — chop, stutter and time-region editing in the playlist (PL-13).
//
// Song-level operations (callers wrap each in one Document::editSong, so every
// action is one undo step). The results are ordinary clips: audio clips with
// source windows, pitch, stretch, gain, fades and reverse; pattern clips with
// content offsets or a new pattern of repeated notes — so they save, export
// to FL and play back like any hand-made clip.
//
// Slicing tiles each clip exactly: the pieces start where the previous one
// ends (no gaps, no overlaps) and audio pieces start at the source position
// the original clip played at that tick (sample-accurate; see ClipOps).
#pragma once
#include <set>
#include <string>
#include <vector>
#include "../core/Song.h"

namespace gl::chop {

// ------------------------------------------------------------------ slicing ----
// Cuts every listed clip (all kinds) at the song grid lines (multiples of `grid`).
// Returns the ids of all pieces (originals included), in playlist order.
std::vector<ObjectId> sliceAtGrid(Song& s, const std::set<ObjectId>& clips, Tick grid);
// Cuts every listed clip into `parts` pieces of equal length (boundaries at
// start + round(length * k / parts); the last piece takes the remainder).
std::vector<ObjectId> sliceEqual(Song& s, const std::set<ObjectId>& clips, int parts);
// Audio clips: cuts at the transients of their audio (sensitivity 0..1);
// pattern clips: at the note onsets of their content (steps included).
std::vector<ObjectId> sliceAtTransients(Song& s, const std::set<ObjectId>& clips, double sensitivity);
// Song ticks where a pattern clip's notes start (inside the clip, exclusive of its start).
std::vector<Tick> patternOnsets(const Song& s, const Clip& c);

// ------------------------------------------------------------------ stutter ----
// A stutter repeats (or plays on through) slices of a region with a rate,
// decay, pitch and speed ramps, reverse and gate.
struct StutterParams {
  int rate = 16;          // note value 1/rate of a whole note: 4, 8, 16, 32, 64
  bool triplet = false;   // slices 2/3 as long
  int endRate = 0;        // 0 = constant; else the rate reached at the region end (rolls / builds)
  double decay = 0;       // -1..1: > 0 each repeat quieter by this share, < 0 builds up to full
  double pitchStart = 0, pitchEnd = 0;  // semitones, ramped over the repeats
  int reverse = 0;        // 0 off, 1 every slice, 2 every other slice
  double gate = 1.0;      // 0.05..1: share of each slice that sounds
  double speedStart = 1.0, speedEnd = 1.0;  // playback speed (0.1..2), ramped over the region
  bool follow = false;    // false: every slice repeats the region's first slice; true: slices play on
  double fadeMs = 2.0;    // audio slices: fade in / out (0 = the engine's edge declick only)
  bool operator==(const StutterParams& o) const;
};

// One planned slice (ticks relative to the region start).
struct Step {
  Tick at = 0;        // slot start
  Tick slot = 0;      // slot length (the slots tile the region exactly)
  Tick play = 0;      // sounding length (gate), 1..slot
  double src = 0;     // content position played at the slot start
  double speed = 1;   // content ticks per song tick
  double semis = 0;   // pitch offset
  double gain = 1;    // linear
  bool reverse = false;
};
// Slot length (ticks) of a rate at a PPQ.
Tick rateTicks(int rate, bool triplet, int ppq);
// The slices of a region of `length` ticks.
std::vector<Step> plan(const StutterParams& p, Tick length, int ppq);

// Applies the stutter to every listed audio / pattern clip over [a, b)
// (limited to each clip). Audio: the region becomes one audio clip per slice
// (source window, pitch, stretch, gain, reverse, fades). Pattern: the region
// becomes one clip of a new pattern holding the repeated notes. Returns the
// ids of the new clips.
std::vector<ObjectId> stutter(Song& s, const std::set<ObjectId>& clips, Tick a, Tick b, const StutterParams& p);

// --------------------------------------------------------------- chop presets ----
enum class Preset { BeatRepeat = 0, TapeStop, HalfTime, TripletStutter, ReverseBuild, Count };
inline constexpr int kPresetCount = static_cast<int>(Preset::Count);
const char* presetName(Preset p);  // untranslated (i18n::N_ marked)
const char* presetHint(Preset p);
StutterParams presetParams(Preset p);

// ----------------------------------------------------------- time selection ----
// Clips on `tracks` (empty = every track) that overlap [a, b).
std::set<ObjectId> clipsInRange(const Song& s, Tick a, Tick b, const std::set<int>& tracks = {});
// Cuts the clips on `tracks` at a and b; returns the ids of the pieces inside [a, b).
std::set<ObjectId> isolateRange(Song& s, Tick a, Tick b, const std::set<int>& tracks = {});
// Copies [a, b) on `tracks` to [b, 2b - a): clips there are cut away first.
// Returns the ids of the copies.
std::vector<ObjectId> duplicateRange(Song& s, Tick a, Tick b, const std::set<int>& tracks = {});
// Inserts `length` ticks at `at` on `tracks` only (empty = all tracks; then
// markers move too): clips across `at` are cut, later clips move right.
bool insertSpace(Song& s, Tick at, Tick length, const std::set<int>& tracks = {});
// Removes [a, b) on `tracks` (empty = all, markers too): clips are cut at the
// edges (audio sample-accurately), the inside is deleted and later clips move left.
bool deleteSpace(Song& s, Tick a, Tick b, const std::set<int>& tracks = {});

// Renders the clips on `tracks` over [a, b) (cut at the edges; master effects
// bypassed) into a WAV in `dir` and places it as an audio clip at `a` on
// `destTrack` (-1: the first track below the sources that is free over [a, b)).
// Returns the new clip id (kNoId: nothing to render).
ObjectId consolidateRange(Song& s, Tick a, Tick b, const std::set<int>& tracks, const std::string& dir, double sampleRate,
                          int destTrack = -1);

// "Chop it": consolidates [a, b) and loads the audio into a new slicer channel
// cut at every `grid` ticks (0: at transients with `sensitivity`), with a new
// pattern that plays the slices in order (slice k on base note + k), placed at
// `a` on `destTrack` (-1: first free track below the sources).
struct ChopResult {
  ObjectId channel = kNoId, pattern = kNoId, clip = kNoId;
  int slices = 0;
};
ChopResult chopRangeToSlicer(Song& s, Tick a, Tick b, const std::set<int>& tracks, Tick grid, double sensitivity,
                             const std::string& dir, double sampleRate, int destTrack = -1);

}  // namespace gl::chop
