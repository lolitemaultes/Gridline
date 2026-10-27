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
//
// J-87: pitched, stretched and tempo-synced clips play their offline-quality
// render (ProgAudioClip::hq, engine/TimeStretch) once it is ready, indexed by
// the sample count since the clip's first sample; the grains above are the
// fallback while it renders and while the channel pitch is automated. Plain
// clips read the source through a band-limited windowed-sinc kernel.
//
// J-140: the grains read through the shared band-limited kernel too (no
// aliasing when pitched up), and each grain is shifted by up to 10 ms to where
// its start best continues the grain before it (WSOLA, Verhelst & Roelands):
// tones keep their phase across grains instead of beating. The alignment is
// the one piece of state (ProgAudioClip::rtGrain); after a jump the first
// grain plays where the time map puts it.
#include <algorithm>
#include <cmath>
#include <limits>
#include "Program.h"

namespace gl {

namespace {
constexpr double kGrainHop = 0.030;    // seconds between grain starts (grain length = 2 hops)
constexpr double kDeclick = 0.002;     // minimum fade at clip edges, seconds
constexpr double kPi = 3.14159265358979323846;
constexpr double kAlignTol = 0.010;    // seconds a grain may move to line up with the previous one

// Source frame of source time `sec` (reverse clips read from the end).
inline double frameOf(const Sample& s, double sec, bool reverse) {
  const double f = sec * s.sampleRate;
  return reverse ? static_cast<double>(s.frames() - 1) - f : f;
}

// WSOLA: the offset (seconds of source time) that best lines the start of a
// grain at source time `nominal` up with the source at `cont` (where the
// previous grain is when the new one starts), over `span` seconds.
double alignGrain(const Sample& s, bool reverse, double nominal, double cont, double span) {
  const auto n = static_cast<int64_t>(s.frames());
  const auto len = static_cast<int64_t>(span * s.sampleRate);
  const auto tol = static_cast<int64_t>(kAlignTol * s.sampleRate);
  if (len < 16 || tol < 1 || n < 2) return 0.0;
  const int64_t stride = std::max<int64_t>(1, len / 256);
  const auto a0 = static_cast<int64_t>(std::llround(frameOf(s, cont, reverse)));
  const auto b0 = static_cast<int64_t>(std::llround(frameOf(s, nominal, reverse)));
  const int64_t dir = reverse ? -1 : 1;
  auto mono = [&](int64_t i) { return i >= 0 && i < n ? 0.5 * (s.left[static_cast<size_t>(i)] + s.right[static_cast<size_t>(i)]) : 0.0; };
  auto score = [&](int64_t lag) {
    double ab = 0, bb = 0;
    for (int64_t j = 0; j < len; j += stride) {
      const double x = mono(a0 + dir * j), y = mono(b0 + lag + dir * j);
      ab += x * y, bb += y * y;
    }
    return bb > 1e-12 ? ab / std::sqrt(bb) : -1e30;
  };
  int64_t best = 0;
  double bs = score(0);
  if (bs <= -1e29) return 0.0;  // silence: nothing to line up
  for (int64_t lag = -tol; lag <= tol; lag += 4)
    if (const double v = score(lag); v > bs) bs = v, best = lag;
  const int64_t c = best;
  for (int64_t lag = c - 3; lag <= c + 3; ++lag)
    if (lag != c && std::llabs(lag) <= tol)
      if (const double v = score(lag); v > bs) bs = v, best = lag;
  // Sub-sample lag (parabola through the peak): the grains read through the
  // band-limited kernel, so a fraction of a frame is honoured.
  double frac = 0;
  const double ym = score(best - 1), yp = score(best + 1), den = ym - 2 * bs + yp;
  if (ym > -1e29 && yp > -1e29 && den < -1e-12) frac = std::clamp(0.5 * (ym - yp) / den, -0.5, 0.5);
  return (static_cast<double>(best) + frac) * static_cast<double>(dir) / s.sampleRate;
}

// Band-limited read (J-87): plain clips, sample-rate conversion.
inline void readFrameSinc(const Sample& s, double sec, bool reverse, float& l, float& r, const stretch::SincTable& t, double rate) {
  double f = sec * s.sampleRate;
  const double last = static_cast<double>(s.frames() - 1);
  if (reverse) f = last - f;
  if (!(f >= -1e-6) || f > last + 1e-6) {
    l = r = 0.f;
    return;
  }
  const double fr = f - std::floor(f);
  if (rate <= 1.000001 && (fr < 1e-7 || fr > 1 - 1e-7)) {  // on a sample, not reading faster: exact
    const auto i = static_cast<size_t>(std::clamp(std::llround(f), 0LL, static_cast<long long>(s.frames() - 1)));
    l = s.left[i];
    r = s.right[i];
    return;
  }
  stretch::readSinc2(s.left.data(), s.right.data(), static_cast<int64_t>(s.frames()), f, rate, t, l, r);
}

}  // namespace

void renderAudioClips(const std::vector<ProgAudioClip>& clips, const TempoMap& map, double sr, double anchorTick,
                      int64_t clock, int from, int len, float* L, float* R, double pitchSemis) {
  if (clips.empty() || len <= 0) return;
  const double chanRatio = pitchSemis != 0 ? std::pow(2.0, pitchSemis / 12.0) : 1.0;
  const stretch::SincTable& rt = stretch::sincTable(stretch::Quality::Realtime);  // built by the program builder
  const stretch::SincTable& compact = stretch::sincTable(stretch::Quality::Compact);
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
    // J-87: the offline render, while it applies.
    // (A render in progress publishes its finished start: frames below hqReady.)
    const stretch::RenderedClip* hq = c.hq && std::fabs(pitchSemis - c.hqSemis) < 1e-6 ? c.hq.get() : nullptr;
    const int64_t hqReady = hq ? hq->readyFrames.load(std::memory_order_acquire) : 0;
    const int64_t hqFrames = hq ? hq->frames() : 0;
    const int64_t first = sampleOf(static_cast<double>(c.start));  // block index of the clip's first sample
    const double readRate = c.speed * s.sampleRate / sr;              // source frames per output frame (plain reads)
    for (int64_t i = a; i < b; ++i) {
      const double u = sec0 + static_cast<double>(i) / sr - c.startSec;
      double g = c.gain;
      // Fades (J-38: curve shape and tension; the 2 ms declick stays linear).
      if (fin > 0 && u < fin) g *= c.fadeIn > 0 ? lawIn.gain(fadeProgressIn(u * sr, nIn)) : std::max(0.0, u / fin);
      const double rest = c.durSec - u;
      if (fout > 0 && rest < fout) g *= c.fadeOut > 0 ? lawOut.gain(fadeProgressOut(rest * sr, nOut)) : std::max(0.0, rest / fout);
      if (g <= 0.0) continue;
      float l = 0.f, r = 0.f;
      const int64_t m = i - first;
      if (hq && m < hqReady) {
        if (m < 0) continue;
        l = hq->L[static_cast<size_t>(m)];
        r = hq->R[static_cast<size_t>(m)];
      } else if (hq && hqReady >= hqFrames) {
        continue;  // past the render's end: silence
      } else if (!c.granular && chanRatio == 1.0) {
        double src = srcAt(u, t0 + (t1 - t0) * static_cast<double>(i) / static_cast<double>(len));
        if (src >= c.srcEnd) continue;
        readFrameSinc(s, src, c.reverse, l, r, rt, c.secPerTick > 0 ? 1.0 : readRate);
      } else {
        // Grains k (starting at k·H) and k − 1 overlap at u.
        const int64_t k = static_cast<int64_t>(std::floor(u / kGrainHop));
        const double inRate = (c.granular ? c.pitch : c.speed) * chanRatio;  // source seconds per playback second, in a grain
        if (k != cachedGrain) {
          for (int q = 0; q < 2; ++q) {
            double uk = static_cast<double>(k - q) * kGrainHop;
            double tk = c.secPerTick > 0 ? map.tickAtSeconds(c.startSec + uk) : 0.0;
            // The grain overlapping the clip start is placed so that it reads the
            // clip start exactly at u = 0 (no lost or smeared first transient).
            grainSrc[q] = uk < 0 ? c.srcStart + uk * inRate : srcAt(uk, tk);
          }
          // J-140: grain k - 1 as it was aligned (when it was the last one
          // placed); grain k lined up with it.
          if (c.rtGrain == k) {
            grainSrc[0] = c.rtGrainSrc[0], grainSrc[1] = c.rtGrainSrc[1];
          } else {
            // Only a grain that starts now is moved (playback went on from the
            // grain before); after a jump or a new program it plays as mapped.
            if (c.rtGrain == k - 1) {
              grainSrc[1] = c.rtGrainSrc[0];
              if (k > 0) grainSrc[0] += alignGrain(s, c.reverse, grainSrc[0], grainSrc[1] + kGrainHop * inRate, kGrainHop * inRate);
            }
            c.rtGrain = k, c.rtGrainSrc[0] = grainSrc[0], c.rtGrainSrc[1] = grainSrc[1];
          }
          cachedGrain = k;
        }
        const double grainRate = inRate * s.sampleRate / sr;  // source frames per output frame
        for (int q = 0; q < 2; ++q) {
          double du = u - static_cast<double>(k - q) * kGrainHop;  // time inside the grain (0 … 2H)
          double src = grainSrc[q] + du * inRate;
          if (src >= c.srcEnd || src < -1e-7) continue;
          double w = std::sin(kPi * du / (2.0 * kGrainHop));
          float gl = 0.f, gr = 0.f;
          readFrameSinc(s, src, c.reverse, gl, gr, compact, grainRate);
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
