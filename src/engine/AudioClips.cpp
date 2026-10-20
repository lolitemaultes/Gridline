// Gridline — audio clip playback (arrangement). Audio thread: no allocation.
//
// Every output sample is computed from the song position alone, so playback
// can start, seek or loop anywhere. Plain clips read the source at a fixed
// rate (resampling). Pitch-independent playback (pitch shift without a speed
// change, time stretch, tempo-synced clips) uses two overlapping grains with
// sin² windows: grain k starts at playback time k·H, reads the source from
// where the clip's time map places that moment, at the pitch ratio. The
// windows sum to one, so at unit speed and pitch the result equals the
// plain read exactly.
#include <algorithm>
#include <cmath>
#include <limits>
#include "Program.h"

namespace gl {

namespace {
constexpr double kGrainHop = 0.030;    // seconds between grain starts (grain length = 2 hops)
constexpr double kDeclick = 0.002;     // minimum fade at clip edges, seconds
constexpr double kPi = 3.14159265358979323846;

inline void readFrame(const Sample& s, double sec, bool reverse, float& l, float& r) {
  double f = sec * s.sampleRate;
  const double last = static_cast<double>(s.frames() - 1);
  if (reverse) f = last - f;
  if (!(f >= -1e-6) || f > last + 1e-6) {  // tolerate rounding at the edges
    l = r = 0.f;
    return;
  }
  f = std::clamp(f, 0.0, last);
  size_t i = static_cast<size_t>(f);
  if (i + 1 >= s.frames()) {
    l = s.left[i];
    r = s.right[i];
    return;
  }
  float fr = static_cast<float>(f - static_cast<double>(i));
  l = s.left[i] + (s.left[i + 1] - s.left[i]) * fr;
  r = s.right[i] + (s.right[i + 1] - s.right[i]) * fr;
}
}  // namespace

void renderAudioClips(const std::vector<ProgAudioClip>& clips, const TempoMap& map, double sr, double anchorTick,
                      int64_t clock, int from, int len, float* L, float* R, double pitchSemis) {
  if (clips.empty() || len <= 0) return;
  const double chanRatio = pitchSemis != 0 ? std::pow(2.0, pitchSemis / 12.0) : 1.0;
  const double t0 = map.tickAfterSamples(anchorTick, static_cast<double>(clock), sr);
  const double t1 = map.tickAfterSamples(anchorTick, static_cast<double>(clock + len), sr);
  const double sec0 = map.secondsAt(t0);
  auto sampleOf = [&](double tick) {  // same rounding as note dispatch
    return static_cast<int64_t>(std::ceil(map.samplesBetween(anchorTick, tick, sr) - 1e-9)) - clock;
  };
  for (const ProgAudioClip& c : clips) {
    if (static_cast<double>(c.start) > t1 + 1.0) break;  // sorted by start
    if (static_cast<double>(c.end) < t0 - 1.0 || !c.sample) continue;
    int64_t a = std::clamp<int64_t>(sampleOf(static_cast<double>(c.start)), 0, len);
    int64_t b = std::clamp<int64_t>(sampleOf(static_cast<double>(c.end)), 0, len);
    if (b <= a) continue;
    const Sample& s = *c.sample;
    // Source position at playback time u (seconds since the clip start).
    auto srcAt = [&](double u, double tickAtU) {
      return c.secPerTick > 0 ? c.srcStart + (tickAtU - static_cast<double>(c.start)) * c.secPerTick : c.srcStart + u * c.speed;
    };
    // Declick only where the clip cuts into the audio (an explicit fade replaces it).
    // J-43: FL's per-edge declicker mode: 2 bypasses it, 1 / 3 always declick.
    const bool dIn = c.declickIn == 2 ? false : c.declickIn == 1 || c.declickIn == 3 || c.srcStart > 0;
    const bool dOut = c.declickOut == 2 ? false : c.declickOut == 1 || c.declickOut == 3 || c.cutEnd;
    const double fin = c.fadeIn > 0 ? c.fadeIn : (dIn ? kDeclick : 0.0);
    const double fout = c.fadeOut > 0 ? c.fadeOut : (dOut ? kDeclick : 0.0);
    // J-43: FL's fade law, sampled like FL (n = round(length · rate) steps).
    const FadeLaw lawIn = FadeLaw::make(c.shapeIn, c.tensionIn), lawOut = FadeLaw::make(c.shapeOut, c.tensionOut);
    const double nIn = fadeSamples(c.fadeIn, sr), nOut = fadeSamples(c.fadeOut, sr);
    int64_t cachedGrain = std::numeric_limits<int64_t>::min();
    double grainSrc[2] = {0, 0};
    for (int64_t i = a; i < b; ++i) {
      const double u = sec0 + static_cast<double>(i) / sr - c.startSec;
      double g = c.gain;
      // Fades (J-38: curve shape and tension; the 2 ms declick stays linear).
      if (fin > 0 && u < fin) g *= c.fadeIn > 0 ? lawIn.gain(fadeProgressIn(u * sr, nIn)) : std::max(0.0, u / fin);
      const double rest = c.durSec - u;
      if (fout > 0 && rest < fout) g *= c.fadeOut > 0 ? lawOut.gain(fadeProgressOut(rest * sr, nOut)) : std::max(0.0, rest / fout);
      if (g <= 0.0) continue;
      float l = 0.f, r = 0.f;
      if (!c.granular && chanRatio == 1.0) {
        double src = srcAt(u, t0 + (t1 - t0) * static_cast<double>(i) / static_cast<double>(len));
        if (src >= c.srcEnd) continue;
        readFrame(s, src, c.reverse, l, r);
      } else {
        // Grains k (starting at k·H) and k − 1 overlap at u.
        const int64_t k = static_cast<int64_t>(std::floor(u / kGrainHop));
        if (k != cachedGrain) {
          for (int q = 0; q < 2; ++q) {
            double uk = static_cast<double>(k - q) * kGrainHop;
            double tk = c.secPerTick > 0 ? map.tickAtSeconds(c.startSec + uk) : 0.0;
            // The grain overlapping the clip start is placed so that it reads the
            // clip start exactly at u = 0 (no lost or smeared first transient).
            grainSrc[q] = uk < 0 ? c.srcStart + uk * (c.granular ? c.pitch : c.speed) * chanRatio : srcAt(uk, tk);
          }
          cachedGrain = k;
        }
        for (int q = 0; q < 2; ++q) {
          double du = u - static_cast<double>(k - q) * kGrainHop;  // time inside the grain (0 … 2H)
          double src = grainSrc[q] + du * (c.granular ? c.pitch : c.speed) * chanRatio;
          if (src >= c.srcEnd || src < -1e-7) continue;
          double w = std::sin(kPi * du / (2.0 * kGrainHop));
          float gl = 0.f, gr = 0.f;
          readFrame(s, src, c.reverse, gl, gr);
          l += static_cast<float>(w * w) * gl;
          r += static_cast<float>(w * w) * gr;
        }
      }
      L[from + i] += l * static_cast<float>(g);
      R[from + i] += r * static_cast<float>(g);
    }
  }
}

}  // namespace gl
