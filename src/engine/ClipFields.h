// Gridline: the audio clip editor's settings, one field at a time. Every
// control of the editor's Clip and Time tabs goes through set(): it changes
// exactly that field of the clip (and of every other clip in the selection,
// by the same amount), as one undo step (a drag with a merge key stays one
// step), and collaborators get it as any other clip edit. UI-free, so the
// tests drive the same code the editor does.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "../core/Document.h"

namespace gl::clipfields {

enum class Field : int {
  GainDb,         // dB (below -96: silent)
  Pan,            // K-414: -100 .. +100 % (-1..+1)
  Mute,           // 0 / 1
  Reverse,        // 0 / 1 (the clip keeps the same audio, played backwards)
  FadeIn,         // seconds (set by hand)
  FadeOut,
  FadeInShape,    // 0 smooth, 1 straight
  FadeOutShape,
  FadeInCurve,    // -1 .. 1 on the fade handle's scale (core/Fades.h fadeTensionToUi)
  FadeOutCurve,
  Semitones,      // whole semitones of the clip's pitch (the cents stay)
  Cents,          // -50 .. 50 on top of the semitones
  Stretch,        // time factor (2: twice as long); the pitch is kept
  Speed,          // percent; the pitch follows (a stretch and the matching transpose)
  FollowTempo,    // 0 / 1
  Loop,           // 0 / 1
  LoopStart,      // ticks into the clip's audio where the first pass starts (Clip::offset)
  Count
};

const char* undoLabel(Field f);
// The clip's value of `f` in the field's units.
double get(const Song& s, const Clip& c, Field f);
// Sets `f` to `value` on `anchor` and moves every other clip of `ids` by the same amount (each
// clamped to its own range; switches take the value). One undo step (`mergeKey` folds a drag).
// False when nothing changed.
bool set(Document& d, const std::vector<ObjectId>& ids, ObjectId anchor, Field f, double value, uint64_t mergeKey = 0);
// The field's range.
void range(Field f, double& lo, double& hi);

}  // namespace gl::clipfields
