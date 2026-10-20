// Gridline — audio clip fades and automatic crossfades (J-38, FL's law: J-43).
//
// Every audio clip edge can fade: a length (seconds of playback time), a curve
// shape and a tension that bends the curve. Where two audio clips on the same
// playlist track overlap (the later one starting inside the earlier one), the
// overlap becomes a crossfade automatically: the earlier clip fades out and
// the later one fades in over exactly the shared span (FL's behaviour with
// the project's "Automatic crossfades" on). The automatic fade follows the
// clips whenever they move or resize; it is derived from the arrangement and
// never stored. Editing a fade by hand stores it on the clip (Clip::fadeIn /
// fadeInManual …), and the stored value wins from then on (until the fade is
// reset to automatic).
//
// Curves (J-43, FL's fade law): an edge fades along one of two base curves of
// its progress p (0 at the silent end, 1 at full level): "smooth" (raised
// cosine, FL's default) or "single" (a straight line). The tension t then
// bends the resulting level g:
//   t > 0: 1 - ((1 + t)^(1 - g) - 1) / t   (bulges up: loud early / late)
//   t < 0: ((1 + |t|)^g - 1) / |t|         (bows down)
// Tension is kept in FL's own unit (any finite value, 0 = plain; FL rounds it
// to 0.01 when it plays). The law is closed under mirroring,
// 1 - law_t(1 - g) = law_-t(g), so a crossfade whose two sides carry opposite
// tensions (FL's default: the partner mirrors the edited edge) keeps a
// constant amplitude for both shapes. FL samples an n-sample fade at
// p = k / (n + 1), k = 1 … n (fadeProgressIn / fadeProgressOut).
#pragma once
#include <cstdint>
#include <vector>
#include "Automation.h"
#include "Song.h"

namespace gl {

enum class FadeShape : uint8_t {
  Smooth = 0,  // raised cosine (FL "smooth curve", the default)
  Single = 1,  // straight line, bent by the tension (FL "single curve")
};
inline constexpr int kFadeShapeCount = 2;
const char* fadeShapeName(FadeShape s);

// FL's tension law on a level g in [0, 1] (exact form above).
double fadeTension(double g, float tension);
// Gain at progress p (0 = silent edge, 1 = full level).
double fadeGain(double p, FadeShape shape, float tension);

// The same law with its constants prepared once (audio thread: per sample).
struct FadeLaw {
  FadeShape shape = FadeShape::Smooth;
  double t = 0, inv = 0, ln1 = 0;  // tension (rounded to 0.01 like FL), 1 / |t|, ln(1 + |t|)
  static FadeLaw make(FadeShape shape, float tension);
  double gain(double p) const;
};

// Progress of FL's sampled fades: sample k (0-based, from the fade start) of
// an n-sample fade-in sits at (k + 1) / (n + 1); a fade-out `rest` samples
// before the clip end at rest / (n + 1). n = round(seconds · rate), at least 1.
inline double fadeSamples(double seconds, double rate) {
  const double n = static_cast<double>(static_cast<int64_t>(seconds * rate + 0.5));
  return n < 1 ? 1 : n;
}
inline double fadeProgressIn(double k, double n) { return k < 0 ? 0 : k >= n ? 1 : (k + 1) / (n + 1); }
inline double fadeProgressOut(double rest, double n) { return rest <= 0 ? 0 : rest >= n + 1 ? 1 : rest / (n + 1); }

// Gridline's handle position (-1 … 1, 0 = plain, up = louder) <-> FL tension.
// The handle moves logarithmically on FL's readout scale (|10 t| + 1 up to
// 6351 at the ends, t = ±635).
float fadeTensionFromUi(float u);
float fadeTensionToUi(float t);

// The fade on one edge of a clip, as it plays.
struct EdgeFade {
  double len = 0;                        // seconds of playback time (0 = none)
  FadeShape shape = FadeShape::Smooth;
  float tension = 0;                     // FL unit
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
// The same for another clip list of the song (an arrangement that is not
// the current one).
std::vector<ClipFades> resolveAllFades(const Song& s, const TempoMap& tm, const std::vector<Clip>& clips);

// True when the edge's fade is stored on the clip (set by hand or loaded).
inline bool fadeInIsManual(const Clip& c) { return c.fadeInManual || c.fadeIn > 0; }
inline bool fadeOutIsManual(const Clip& c) { return c.fadeOutManual || c.fadeOut > 0; }

}  // namespace gl
