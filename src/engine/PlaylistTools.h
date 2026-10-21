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

// J-79: zero crossings ("Check for zero-crossing when editing audio clips"):
// the source position (seconds) of the zero crossing (mono sum changes sign
// or is 0) nearest to `sec` within kZeroCrossReach; with `notAfter` only
// crossings at or before `sec` count (clip ends: the cut lands on silence
// before the clip stops). `sec` when the audio has none there.
constexpr double kZeroCrossReach = 0.005;
double nearestZeroCrossing(const Song& s, const Clip& c, double sec, bool notAfter = false);

// Right edge: every selected clip's end moves by d (at least 1 tick long).
// Audio clips stop at the end of their audio unless `stretch` is on; with
// `stretch` an audio clip keeps its content and plays it slower / faster.
// With `zeroCross` (and a tempo map), a trimmed audio clip's audio ends on
// the zero crossing at or just before its new end (Clip::audioEnd); growing
// a clip past an end set that way clears it again.
std::vector<Clip> resizeEnds(const Song& s, const std::vector<Clip>& base, const std::set<ObjectId>& sel, Tick d, bool stretch,
                             const TempoMap* tm = nullptr, bool zeroCross = false);

// J-79: stretch mode keeps the pitch. The sampler channels of the selected
// audio clips that resample (pitch follows speed), and the switch of those
// channels to their pitch-keeping time stretch.
std::set<ObjectId> resamplingChannels(const Song& s, const std::vector<Clip>& clips, const std::set<ObjectId>& sel);
void keepPitch(Song& s, const std::set<ObjectId>& channels);

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
// J-79: with `zeroCross`, an audio clip's new source start is moved to the
// nearest zero crossing.
Clip slipClip(const Song& s, const TempoMap& tm, const Clip& base, Tick d, bool zeroCross = false);

// Slice: cuts every listed clip that spans `at` (all kinds; audio keeps its
// sample positions). Returns the ids of the right-hand parts. J-79: with
// `zeroCross`, an audio cut lands on the zero crossing nearest to it: the left
// part's audio ends and the right part's audio starts there.
std::vector<ObjectId> sliceClips(Song& s, const std::vector<ObjectId>& ids, Tick at, bool zeroCross = false);
// Clips crossed by a vertical slice line at `at` over tracks [t0, t1].
std::vector<ObjectId> clipsAcross(const Song& s, Tick at, int t0, int t1);

// Paint: positions of a trail of back-to-back clips of length `len` laid from
// `anchor` towards the pointer at `to` (both directions).
std::vector<Tick> paintSlots(Tick anchor, Tick to, Tick len);
// True when [start, start + len) on `track` overlaps a clip.
bool occupied(const Song& s, int track, Tick start, Tick len);

// Clone (Shift + drag): copies of the selected clips at the same place (new
// ids, appended). Returns the new ids in playlist order. Copies keep every
// clip setting; grouped clips form new groups (one per original group, Q-3)
// and recording takes are not shared.
std::vector<ObjectId> cloneClips(Song& s, const std::set<ObjectId>& sel);
// Q-3: gives copies of grouped clips new groups (one new group per original
// group among `copies`), so copies never join the originals' groups.
void regroupCopies(Song& s, std::vector<Clip>& copies);

// J-79: stacking (J-38): on a track, a clip that starts later lies on the
// clips it overlaps; equal starts stack in playlist order. The topmost clip
// covering (track, t), kNoId when none.
ObjectId topClipAt(const Song& s, int track, Tick t);

// J-79: a delete sweep (right-drag / delete tool) removes the topmost clip
// under the pointer. A clip uncovered by a deletion is not deleted while the
// pointer stays where the deleted clip was, so a click on overlapping clips
// removes only the one on top; dragging on onto the uncovered clip's own
// area deletes it too (FL: one clip per click, every clip the drag enters).
struct EraseSweep {
  struct Span {
    int track;
    Tick start, end;
  };
  std::vector<Span> spans;  // clips deleted by this sweep
  bool covers(int track, Tick t) const;
  void add(const Song& s, const Clip& c);
  // The clip to delete at (track, t): `hit` (the clip the view hit-tested,
  // kNoId: the topmost clip there), kNoId where this sweep already deleted one.
  ObjectId pick(const Song& s, int track, Tick t, ObjectId hit = kNoId) const;
};

// Tracks: move track `from` to `to` (clips follow, other tracks shift).
bool moveTrack(Song& s, int from, int to);

}  // namespace gl::pltools
