// Gridline: J-142, relative edits of one clip parameter across a selection.
//
// When the clip under an edit (a handle drag, a knob or a drag field, a typed
// value, a reset) belongs to a playlist selection of more than one clip, the
// edit reaches every selected clip of a kind that has the parameter (today:
// audio clips; pattern and automation clips have no comparable per-clip
// level, fade or pitch). Each clip moves by the same amount from its own value
// at the start of the gesture, so clips that differed keep differing:
//
//   GainDb        dB added (a +2 dB drag adds 2 dB to each clip), limits
//                 -96 dB (below that: silent) .. +36 dB.
//   Pitch         cents added, limits -4800 .. +4800.
//   FadeIn/Out    seconds of playback time added to each clip's effective
//                 fade length (automatic crossfade lengths included; the edge
//                 is then stored as set by hand), limits 0 .. the clip's own
//                 length minus its other fade.
//   FadeIn/OutCurve
//                 the curve's bend on the handle's scale (-1 .. +1, the
//                 tension handle's travel) added; a tied crossfade partner
//                 mirrors each clip's new bend as before.
//
// Every clip is clamped at its own limits without holding the others back;
// the offset is always applied to the values captured when the gesture began
// (a clip that hit a limit comes back when the drag reverses). A reset sets
// every target to the parameter's default. Pure: no UI, unit tested
// (tests/test_clip_multi_edit.cpp).
#pragma once
#include <cstdint>
#include <set>
#include <string>
#include <vector>
#include "Document.h"
#include "Song.h"

namespace gl::clipmulti {

enum class Param { GainDb, Pitch, FadeIn, FadeOut, FadeInCurve, FadeOutCurve };

// Whether the clip kind carries the parameter.
bool hasParam(const Clip& c, Param p);

// The clips an edit of `clicked` reaches: every selected clip carrying the
// parameter when `clicked` is selected together with others, else `clicked`
// alone (empty when it does not carry the parameter).
std::vector<ObjectId> targets(const Song& s, const std::set<ObjectId>& selected, ObjectId clicked, Param p);
// True when an edit of `clicked` reaches more than that one clip.
bool isMulti(const Song& s, const std::set<ObjectId>& selected, ObjectId clicked, Param p);

struct Entry {
  ObjectId id = kNoId;
  double base = 0;            // value when the gesture began (parameter units above)
  double lo = 0, hi = 0;      // this clip's limits
  ObjectId mirror = kNoId;    // curves: the tied crossfade partner whose opposite edge mirrors it
};
struct Snapshot {
  Param param = Param::GainDb;
  ObjectId anchor = kNoId;    // the clip under the pointer
  std::vector<Entry> clips;
  const Entry* find(ObjectId id) const;
  // The offsets past which every clip sits at a limit (no further change).
  double minDelta() const;
  double maxDelta() const;
};

// Values captured now (the start of a gesture) for `ids` (unknown ids and
// clips without the parameter are skipped).
Snapshot capture(const Song& s, const std::vector<ObjectId>& ids, Param p, ObjectId anchor);

// The value of one clip in parameter units (effective fade length for fades).
double valueOf(const Song& s, const Clip& c, Param p);
// Writes a value (already clamped) into a clip.
void store(Clip& c, Param p, double v);
// The parameter's default (reset).
double defaultOf(Param p);

// Each captured clip set to clamp(base + delta) in `clips` (a copy of the
// playlist). Curves of the anchor keep the handle's detent: a result within
// `curveDetent` of the plain curve snaps to it (the other clips do not snap).
void applyOffset(std::vector<Clip>& clips, const Snapshot& snap, double delta, double curveDetent = 0.0);
// Every captured clip set to `value` (clamped per clip): resets, pasted values.
void applyValue(std::vector<Clip>& clips, const Snapshot& snap, double value);

// The same through the document: one step labelled `label` (mergeable with
// `mergeKey`, a drag: one undo step for the whole gesture).
bool offset(Document& doc, const std::string& label, const Snapshot& snap, double delta, uint64_t mergeKey = 0,
            double curveDetent = 0.0);
bool setAll(Document& doc, const std::string& label, const Snapshot& snap, double value, uint64_t mergeKey = 0);

}  // namespace gl::clipmulti
