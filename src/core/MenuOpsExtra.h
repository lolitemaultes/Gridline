// Gridline — more menu operations (J-18 coverage sweep): time markers, insert /
// delete space, pattern sorting, channel cut-itself / swing mix / layers,
// discard note lengths, tap tempo (mixer submix / unrouting live in MixerOps). Every edit goes
// through Document (one undo step each); tested in tests/test_coverage.cpp.
#pragma once
#include <string>
#include <vector>
#include "MenuOps.h"

namespace gl::ops {

// ---------------------------------------------------------- time markers ----
// Markers of the current arrangement, kept sorted by position. A time
// signature marker has num/den > 0. Returns the new marker's index (-1 if the
// position is negative).
int addMarker(Document& d, Tick pos, const std::string& name, int num = 0, int den = 0);
bool renameMarker(Document& d, int index, const std::string& name);
bool removeMarker(Document& d, int index);
// Removes the markers in [from, to); returns how many.
int removeMarkersIn(Document& d, Tick from, Tick to);
// Index of the first marker after / last marker before `pos` (-1: none).
int nextMarker(const Song& s, Tick pos);
int previousMarker(const Song& s, Tick pos);
// A free default name ("Marker 1", "Marker 2", …).
std::string nextMarkerName(const Song& s);

// ------------------------------------------------------- playlist space ----
// Inserts `length` ticks of silence at `at`: clips and markers starting at or
// after `at` move right (clips that start earlier stay where they are).
bool insertSpace(Document& d, Tick at, Tick length);
// Removes the time range [from, to): clips inside it are deleted, clips that
// run into it end at `from` (clips spanning the whole range get shorter by its
// length), pattern / automation clips that start inside it and run past `to`
// lose their head (audio clips starting inside are deleted), later clips,
// markers and the live area end move left by the range length.
bool deleteSpace(Document& d, Tick from, Tick to);

// ------------------------------------------------------------- patterns ----
enum class PatternSort { Name, Color };
bool sortPatterns(Document& d, PatternSort by);

// ------------------------------------------------------------- channels ----
// Each channel cuts itself: its own new cut group, cut by the same group
// (a new note stops the channel's previous note).
bool cutItself(Document& d, const std::vector<ObjectId>& channels);
// Share of the song swing each channel follows (0..1).
bool setSwingMix(Document& d, const std::vector<ObjectId>& channels, float mix);
// A new layer channel (after the last of `channels`) that plays them all.
// Returns its id (kNoId when fewer than one channel is given).
ObjectId createLayer(Document& d, const std::vector<ObjectId>& channels);

// ---------------------------------------------------------------- notes ----
// Sets the selected notes (all when none) to `len` ticks — step-length notes.
void discardLengths(std::vector<Note>& notes, NoteSel& sel, Tick len);

// ---------------------------------------------------------- auto scroll ----
// Page-style follow: the horizontal scroll (px) that keeps the play position
// (`playPx`, px from the timeline start) in a `viewW`-wide view — unchanged
// while it is visible, otherwise the next page starts just before it.
float followScroll(float scrollX, float playPx, float viewW);

// ------------------------------------------------------------ tap tempo ----
// Tempo from taps: the average interval of the last few taps; a pause longer
// than `resetAfter` seconds starts over.
struct TapTempo {
  double resetAfter = 2.0;
  int window = 8;  // taps averaged
  std::vector<double> taps;  // seconds
  // Registers a tap at `t` seconds; returns the tempo (BPM) or 0 while fewer than two taps.
  double tap(double t);
  double bpm() const;
  void reset() { taps.clear(); }
};

}  // namespace gl::ops
