// Gridline — playlist editing gestures as pure operations (J-28). The playlist
// view turns pointer drags into calls here (one undo step per gesture through
// Document::editClips / editSong); tests drive the same calls on synthetic
// songs. Every clip kind (pattern, audio, automation) takes every gesture.
//
// Conventions: `base` is the playlist as it was when the gesture started; the
// returned vector is the whole playlist after the gesture (same clips, same
// order, same ids), so a drag can be recomputed from the start each frame.
#pragma once
#include <set>
#include <vector>
#include "../core/Automation.h"
#include "../core/Song.h"

namespace gl::pltools {

// What clicks with the draw / paint tools place ("current clip source").
struct Source {
  ClipKind kind = ClipKind::Pattern;
  ObjectId id = kNoId;  // Pattern::id, Channel::id (audio) or AutomationClip::id
  bool operator==(const Source& o) const { return kind == o.kind && id == o.id; }
};
bool sourceValid(const Song& s, const Source& src);
// A new clip of `src` at (track, start) with its natural length (id unset).
Clip makeClip(const Song& s, const Source& src, int track, Tick start);
// Natural length of a new clip of `src` (pattern / automation content, audio
// at its current settings; at least one beat).
Tick sourceLength(const Song& s, const Source& src);
// The source a clip plays.
Source sourceOf(const Clip& c);

// Move: selected clips by dt ticks and dtr tracks. The shift is limited so
// that no clip starts before 0 or leaves [0, trackCount); clips may not enter
// or leave a locked track (`locked[track]` != 0) — then nothing moves.
std::vector<Clip> moveClips(const std::vector<Clip>& base, const std::set<ObjectId>& sel, Tick dt, int dtr, int trackCount,
                            const std::vector<char>& locked = {});

// Right edge: every selected clip's end moves by d (at least 1 tick long).
// Audio clips stop at the end of their audio unless `stretch` is on; with
// `stretch` an audio clip keeps its content and plays it slower / faster.
std::vector<Clip> resizeEnds(const Song& s, const std::vector<Clip>& base, const std::set<ObjectId>& sel, Tick d, bool stretch);

// Left edge: every selected clip's start moves by d while its content stays
// in place (pattern / automation: offset; audio: source start in seconds,
// sample accurate). Limited so content never starts before its beginning and
// every clip keeps at least 1 tick. With `stretch`, audio clips keep their
// whole content and squeeze / stretch it instead. With `zeroCross`, an audio
// clip's new source start moves to the nearest zero crossing (within 5 ms).
std::vector<Clip> trimStarts(const Song& s, const TempoMap& tm, const std::vector<Clip>& base, const std::set<ObjectId>& sel, Tick d,
                             bool stretch, bool zeroCross = false);

// Slip: the content moves by d ticks inside the clip, the clip stays. Pattern
// content wraps around (patterns repeat); automation and audio stop at their
// beginning (and audio before its end).
Clip slipClip(const Song& s, const TempoMap& tm, const Clip& base, Tick d);

// Slice: cuts every listed clip that spans `at` (all kinds; audio keeps its
// sample positions). Returns the ids of the right-hand parts.
std::vector<ObjectId> sliceClips(Song& s, const std::vector<ObjectId>& ids, Tick at);
// Clips crossed by a vertical slice line at `at` over tracks [t0, t1].
std::vector<ObjectId> clipsAcross(const Song& s, Tick at, int t0, int t1);

// Paint: positions of a trail of back-to-back clips of length `len` laid from
// `anchor` towards the pointer at `to` (both directions).
std::vector<Tick> paintSlots(Tick anchor, Tick to, Tick len);
// True when [start, start + len) on `track` overlaps a clip.
bool occupied(const Song& s, int track, Tick start, Tick len);

// Clone (Shift + drag): copies of the selected clips at the same place (new
// ids, appended). Returns the new ids in playlist order.
std::vector<ObjectId> cloneClips(Song& s, const std::set<ObjectId>& sel);

// Tracks: move track `from` to `to` (clips follow, other tracks shift).
bool moveTrack(Song& s, int from, int to);

}  // namespace gl::pltools
