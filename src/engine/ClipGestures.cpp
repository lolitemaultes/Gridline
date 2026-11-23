// Gridline: the pointer gestures the playlist and the audio clip editor share. See ClipGestures.h.
#include "ClipGestures.h"
#include <algorithm>
#include <cmath>
#include "../core/Fades.h"
#include "PlaylistTools.h"

namespace gl::clipgest {

namespace {
Clip one(const std::vector<Clip>& v, const Clip& c) {
  for (const Clip& k : v)
    if (k.id == c.id) return k;
  return c;
}
}  // namespace

Clip trimStart(const Song& s, const TempoMap& tm, const Clip& c, Tick d, bool stretch, bool zeroCross) {
  return one(pltools::trimStarts(s, tm, {c}, {c.id}, d, stretch, zeroCross), c);
}

Clip trimEnd(const Song& s, const TempoMap& tm, const Clip& c, Tick d, bool stretch, bool zeroCross) {
  return one(pltools::resizeEnds(s, {c}, {c.id}, d, stretch, &tm, zeroCross), c);
}

Clip slip(const Song& s, const TempoMap& tm, const Clip& c, Tick d, bool zeroCross) { return pltools::slipClip(s, tm, c, d, zeroCross); }

double fadeLength(int kind, double atSec, double durSec, double otherSec) {
  const double at = std::clamp(atSec, 0.0, std::max(0.0, durSec));
  return std::max(0.0, kind == 1 ? std::min(at, durSec - otherSec) : std::min(durSec - at, durSec - otherSec));
}

float fadeCurveUi(float startUi, float downPx) {
  // J-43: drag up = louder (FL's positive tension), a detent at the plain curve.
  const float t = std::clamp(startUi - downPx / 70.0f, -1.0f, 1.0f);
  return std::fabs(t) < 0.03f ? 0.0f : t;
}

float fadeCurve(float startUi, float downPx) { return fadeTensionFromUi(fadeCurveUi(startUi, downPx)); }

}  // namespace gl::clipgest
