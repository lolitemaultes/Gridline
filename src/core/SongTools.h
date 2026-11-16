// Gridline — song and piano roll helpers (J-31): key markers, quantized song
// position jumps, incoming MIDI snapped to the scale, editable ghost notes
// and the piano roll's background waveform.
#pragma once
#include <set>
#include <string>
#include <utility>
#include <vector>
#include "Document.h"

namespace gl::ops {

// ---------------------------------------------------------------- key markers ----
struct KeyAt {
  int root = 0;   // 0..11
  int scale = 0;  // tools::scales() index
  int marker = -1;  // index of the key marker in effect, -1 = none
};
// The key in effect at song tick `t`: the last key marker at or before it.
KeyAt keyAt(const Song& s, Tick t);
std::string keyName(int root, int scale);  // "A Natural minor"
// Adds a key marker (current arrangement); returns its index.
int addKeyMarker(Document& d, Tick pos, int root, int scale);
// Where the piano roll's pattern sits in the song: its first clip's start (0 when not placed).
Tick patternSongPosition(const Song& s, ObjectId pattern);
// The key the piano roll highlights for a pattern (key markers at its first placement).
KeyAt patternKey(const Song& s, ObjectId pattern);

// --------------------------------------------------------- quantized jumps ----
// Grid for a quantized song position jump: 0 off, 1 beat, 2 bar, 3 two bars,
// 4 four bars, 5 eight bars. Returns ticks (0 = jump at once).
inline constexpr int kJumpGridCount = 6;
Tick jumpGrid(const Song& s, int mode);
const char* jumpGridName(int mode);

// ------------------------------------------------------------- MIDI snapping ----
// A played note moved onto the scale when snapping is on (scale 0 = no scale).
int snapIncoming(int pitch, int root, int scale, bool on);

// ------------------------------------------------------------- ghost notes ----
struct GhostHit {
  ObjectId channel = kNoId;
  int index = -1;  // note index in that channel's list
};
// The note of another channel (not `exclude`; only `only` when not empty)
// under song tick `t` and `pitch`; the shortest one wins.
GhostHit ghostNoteAt(const Pattern& p, ObjectId exclude, const std::set<ObjectId>& only, Tick t, int pitch);

// K-198: several channels edited in one piano roll. `edit` holds the channels edited beside the
// target (the channel new notes go to). Making `to` the target keeps the old target editable
// (`from`, when it is a channel) and takes `to` out of the set.
void retarget(std::set<ObjectId>& edit, ObjectId from, ObjectId to);
// The channels with notes in `p`, but `exclude`, in the song's channel order.
std::vector<ObjectId> channelsWithNotes(const Song& s, const Pattern& p, ObjectId exclude);
// Snaps every note start of `notes` to the nearest multiple of `grid` (> 1).
void quantizeStarts(std::vector<Note>& notes, Tick grid);

// ---------------------------------------------------------- background waveform ----
// Peak columns (min, max per column, -1..1) of a sample drawn behind the
// notes: column k covers ticks [t0 + k * ticksPerCol, …). `secPerTick` maps
// pattern ticks to seconds of the sample (tempo); `offsetSec` = sample time at tick 0.
std::vector<std::pair<float, float>> waveformColumns(const float* L, const float* R, size_t frames, double sampleRate,
                                                     double secPerTick, double offsetSec, double t0, double ticksPerCol,
                                                     int cols);

}  // namespace gl::ops
