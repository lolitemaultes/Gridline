// Gridline — audio clip fades and automatic crossfades (J-38). See Fades.h.
#include "Fades.h"

#include <algorithm>
#include <cmath>

namespace gl {

namespace {
constexpr double kPi = 3.14159265358979323846;

bool xfadeCandidate(const Song& s, const Clip& c) {
  return c.kind == ClipKind::Audio && !c.muted && s.clipLength(c) > 0;
}

EdgeFade makeEdge(bool manual, double stored, uint8_t shape, float tension, ObjectId partner, double overlap, bool autoOn, double dur) {
  EdgeFade e;
  e.crossfade = partner != kNoId;
  e.partner = partner;
  e.overlap = e.crossfade ? overlap : 0.0;
  if (manual) {
    e.len = stored;
  } else if (autoOn && e.crossfade) {
    e.len = overlap;
    e.automatic = true;
  }
  e.len = std::clamp(e.len, 0.0, std::max(0.0, dur));
  const FadeShape fs = shape < kFadeShapeCount ? static_cast<FadeShape>(shape) : FadeShape::Default;
  e.shape = fs != FadeShape::Default ? fs : e.crossfade ? FadeShape::EqualPower : FadeShape::Linear;
  e.tension = std::isfinite(tension) ? std::clamp(tension, -1.0f, 1.0f) : 0.0f;
  return e;
}

ClipFades makeFades(const Song& s, const TempoMap& tm, const Clip& c, ObjectId pin, double ovIn, ObjectId pout, double ovOut) {
  ClipFades f;
  if (c.kind != ClipKind::Audio) return f;
  const double sec0 = tm.secondsAt(static_cast<double>(c.start));
  const double dur = tm.secondsAt(static_cast<double>(c.start + s.clipLength(c))) - sec0;
  const bool autoOn = s.autoCrossfades;
  f.in = makeEdge(fadeInIsManual(c), c.fadeIn, c.fadeInShape, c.fadeInTension, pin, ovIn, autoOn, dur);
  f.out = makeEdge(fadeOutIsManual(c), c.fadeOut, c.fadeOutShape, c.fadeOutTension, pout, ovOut, autoOn, dur);
  return f;
}

// Overlap of `later` starting inside `earlier` (seconds), or -1 when they do
// not form a crossfade (no overlap, same start, or `later` ends inside).
double partialOverlap(const Song& s, const TempoMap& tm, const Clip& earlier, const Clip& later) {
  const Tick e0 = earlier.start, e1 = earlier.start + s.clipLength(earlier);
  const Tick l0 = later.start, l1 = later.start + s.clipLength(later);
  if (!(l0 > e0 && l0 < e1 && l1 > e1)) return -1.0;
  return tm.secondsAt(static_cast<double>(e1)) - tm.secondsAt(static_cast<double>(l0));
}
}  // namespace

const char* fadeShapeName(FadeShape s) {
  switch (s) {
    case FadeShape::Default: return "Automatic";
    case FadeShape::Linear: return "Linear";
    case FadeShape::EqualPower: return "Equal power";
    case FadeShape::SCurve: return "S-curve";
  }
  return "Linear";
}

double fadeWarp(double p, float tension) {
  p = std::clamp(p, 0.0, 1.0);
  if (tension == 0.0f) return p;
  const double a = std::exp(kFadeTensionK * static_cast<double>(std::clamp(tension, -1.0f, 1.0f)));
  const double d = p + (1.0 - p) * a;
  return d > 0 ? p / d : 0.0;
}

double fadeGain(double p, FadeShape shape, float tension) {
  const double w = fadeWarp(p, tension);
  switch (shape) {
    case FadeShape::EqualPower: return std::sin(w * kPi * 0.5);
    case FadeShape::SCurve: return 0.5 - 0.5 * std::cos(w * kPi);
    case FadeShape::Default:
    case FadeShape::Linear: break;
  }
  return w;
}

ObjectId crossfadePartnerIn(const Song& s, const Clip& c) {
  if (!xfadeCandidate(s, c)) return kNoId;
  ObjectId best = kNoId;
  Tick bestOv = 0;
  const Tick c0 = c.start, c1 = c.start + s.clipLength(c);
  for (const Clip& o : s.playlist) {
    if (o.id == c.id || o.track != c.track || !xfadeCandidate(s, o)) continue;
    const Tick o0 = o.start, o1 = o.start + s.clipLength(o);
    if (c0 > o0 && c0 < o1 && c1 > o1 && o1 - c0 > bestOv) bestOv = o1 - c0, best = o.id;
  }
  return best;
}

ObjectId crossfadePartnerOut(const Song& s, const Clip& c) {
  if (!xfadeCandidate(s, c)) return kNoId;
  ObjectId best = kNoId;
  Tick bestOv = 0;
  const Tick c0 = c.start, c1 = c.start + s.clipLength(c);
  for (const Clip& o : s.playlist) {
    if (o.id == c.id || o.track != c.track || !xfadeCandidate(s, o)) continue;
    const Tick o0 = o.start, o1 = o.start + s.clipLength(o);
    if (o0 > c0 && o0 < c1 && o1 > c1 && c1 - o0 > bestOv) bestOv = c1 - o0, best = o.id;
  }
  return best;
}

ClipFades resolveFades(const Song& s, const TempoMap& tm, const Clip& c) {
  if (c.kind != ClipKind::Audio) return {};
  const ObjectId pin = crossfadePartnerIn(s, c), pout = crossfadePartnerOut(s, c);
  const Clip* ci = pin != kNoId ? s.clip(pin) : nullptr;
  const Clip* co = pout != kNoId ? s.clip(pout) : nullptr;
  return makeFades(s, tm, c, pin, ci ? partialOverlap(s, tm, *ci, c) : 0.0, pout, co ? partialOverlap(s, tm, c, *co) : 0.0);
}

std::vector<ClipFades> resolveAllFades(const Song& s, const TempoMap& tm) {
  const size_t n = s.playlist.size();
  std::vector<ClipFades> out(n);
  // Audio clips that can crossfade, by track then start.
  std::vector<size_t> idx;
  std::vector<Tick> end(n, 0);
  for (size_t i = 0; i < n; ++i) {
    const Clip& c = s.playlist[i];
    if (c.kind != ClipKind::Audio) continue;
    end[i] = c.start + s.clipLength(c);
    if (xfadeCandidate(s, c)) idx.push_back(i);
  }
  std::sort(idx.begin(), idx.end(), [&](size_t a, size_t b) {
    const Clip &p = s.playlist[a], &q = s.playlist[b];
    return p.track != q.track ? p.track < q.track : p.start != q.start ? p.start < q.start : a < b;
  });
  std::vector<ObjectId> pin(n, kNoId), pout(n, kNoId);
  std::vector<Tick> ovIn(n, 0), ovOut(n, 0);
  for (size_t a = 0; a < idx.size(); ++a) {
    const size_t i = idx[a];
    const Clip& e = s.playlist[i];
    for (size_t b = a + 1; b < idx.size(); ++b) {
      const size_t j = idx[b];
      const Clip& l = s.playlist[j];
      if (l.track != e.track || l.start >= end[i]) break;
      if (!(l.start > e.start && end[j] > end[i])) continue;  // same start or contained: no crossfade
      const Tick ov = end[i] - l.start;
      if (ov > ovOut[i]) ovOut[i] = ov, pout[i] = l.id;
      if (ov > ovIn[j]) ovIn[j] = ov, pin[j] = e.id;
    }
  }
  for (size_t i = 0; i < n; ++i) {
    const Clip& c = s.playlist[i];
    if (c.kind != ClipKind::Audio) continue;
    auto secs = [&](Tick from, Tick to) { return tm.secondsAt(static_cast<double>(to)) - tm.secondsAt(static_cast<double>(from)); };
    out[i] = makeFades(s, tm, c, pin[i], pin[i] != kNoId ? secs(c.start, c.start + ovIn[i]) : 0.0, pout[i],
                       pout[i] != kNoId ? secs(end[i] - ovOut[i], end[i]) : 0.0);
  }
  return out;
}

}  // namespace gl
