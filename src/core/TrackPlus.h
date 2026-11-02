// Gridline - K-12: the playlist track "+" (quick path from an instrument to a
// pattern on the arrangement). The edits behind its menu: give a track an
// instrument (a rack channel or a new one) and start a pattern for that
// instrument on the track. Each Document function is one undo step; they are
// plain song edits, so a collaborative session syncs them like any other
// track edit.
#pragma once
#include <string>
#include "Song.h"

namespace gl {
class Document;
}

namespace gl::trackplus {

// A pattern name for `base` (the instrument's name): "Base", else "Base 2",
// "Base 3" ... (the first one no pattern uses). Empty base: "Pattern N".
std::string patternName(const Song& s, const std::string& base);
// Where a new clip `len` ticks long lands on `track` at or after `at`: `at`
// itself when that span is free, else the end of the clip in the way (rounded
// up to a bar), repeated until it fits.
Tick freeSpot(const Song& s, int track, Tick at, Tick len);
// The tick the "+" places a new pattern at: the song position when it is in
// view [viewStart, viewEnd) (snapped down to its bar), else the view's left
// edge (snapped up to the next bar).
Tick placeTick(const Song& s, Tick songPos, Tick viewStart, Tick viewEnd);

struct Started {
  ObjectId pattern = kNoId, clip = kNoId, channel = kNoId;
};

// A new pattern for the instrument track `track` (named and coloured after its
// instrument), a clip of it on the track at freeSpot(at). One undo step.
// Fails (with a reason) when the track has no instrument.
Started newPatternOnTrack(Document& d, int track, Tick at, std::string* why = nullptr);
// The track plays the rack channel `channel` (an instrument track); with
// `startPattern` (and no clips on the track yet) a first pattern is started
// on it as newPatternOnTrack does. One undo step.
Started assignChannel(Document& d, int track, ObjectId channel, bool startPattern, Tick at, std::string* why = nullptr);
// The same with a new channel of `pluginType` (added after the last channel).
Started assignNewInstrument(Document& d, int track, const std::string& pluginType, bool startPattern, Tick at, std::string* why = nullptr);

}  // namespace gl::trackplus
