// Gridline — audio clip fades and automatic crossfades (J-38).
//
// Every audio clip edge can fade: a length (seconds of playback time), a curve
// shape and a tension that bends the curve. Where two audio clips on the same
// playlist track overlap (the later one starting inside the earlier one), the
// overlap becomes a crossfade automatically: the earlier clip fades out and
// the later one fades in over exactly the shared span, with an equal-power
// curve. The automatic fade follows the clips whenever they move or resize;
// it is derived from the arrangement and never stored. Editing a fade by hand
// stores it on the clip (Clip::fadeIn / fadeInManual …), and the stored value
// wins from then on (until the fade is reset to automatic).
//
// Tension: the curve's progress p (0 at the silent end, 1 at full level) is
// warped by w(p) = p / (p + (1 - p)·e^(k·t)). The family is closed under
// mirroring, 1 − w_t(1 − p) = w_−t(p), so a crossfade whose two sides carry
// opposite tensions keeps its sum: constant power for the equal-power shape,
// constant amplitude for linear and S-curve.
#pragma once
#include <cstdint>
#include <vector>
#include "Automation.h"
#include "Song.h"

namespace gl {

enum class FadeShape : uint8_t {
  Default = 0,     // linear for a plain fade, equal power for a crossfade
  Linear = 1,      // straight ramp (constant amplitude across a crossfade)
  EqualPower = 2,  // quarter sine (constant power across a crossfade)
  SCurve = 3,      // raised cosine: gentle at both ends
};
inline constexpr int kFadeShapeCount = 4;
const char* fadeShapeName(FadeShape s);

inline constexpr double kFadeTensionK = 2.302585092994046;  // ln 10: tension ±1 bends by a factor of 10

// Warped progress (see above); tension in [-1, 1], 0 = unchanged. Negative
// tension bulges the gain curve up (the level arrives early / leaves late).
double fadeWarp(double p, float tension);
// Gain at progress p (0 = silent edge, 1 = full level) for a resolved shape
// (Default is treated as Linear).
double fadeGain(double p, FadeShape shape, float tension);

// The fade on one edge of a clip, as it plays.
struct EdgeFade {
  double len = 0;                        // seconds of playback time (0 = none)
  FadeShape shape = FadeShape::Linear;   // resolved (never Default)
  float tension = 0;
  bool automatic = false;                // derived from an overlap (not stored on the clip)
  bool crossfade = false;                // the edge overlaps another audio clip on the track
  ObjectId partner = kNoId;              // that clip (its other edge fades the opposite way)
  double overlap = 0;                    // length of the shared span, seconds
};
struct ClipFades {
  EdgeFade in, out;
};

// Audio clips sharing playlist track: the clip whose start lies inside
// `c` with the longest overlap (out side) / the clip that `c` starts inside
// with the longest overlap (in side). A clip completely inside another one
// does not crossfade with it. kNoId = none.
ObjectId crossfadePartnerIn(const Song& s, const Clip& c);
ObjectId crossfadePartnerOut(const Song& s, const Clip& c);

// Effective fades of one clip (audio clips; other kinds get none).
ClipFades resolveFades(const Song& s, const TempoMap& tm, const Clip& c);
// Effective fades of every clip, indexed like Song::playlist (one pass per
// track; use this when compiling or drawing the whole arrangement).
std::vector<ClipFades> resolveAllFades(const Song& s, const TempoMap& tm);

// True when the edge's fade is stored on the clip (set by hand or loaded).
inline bool fadeInIsManual(const Clip& c) { return c.fadeInManual || c.fadeIn > 0; }
inline bool fadeOutIsManual(const Clip& c) { return c.fadeOutManual || c.fadeOut > 0; }

}  // namespace gl
