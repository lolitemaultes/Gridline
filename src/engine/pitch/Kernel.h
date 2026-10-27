// Gridline: band-limited interpolation, the one reader every pitch / speed
// change of recorded audio goes through (J-140).
//
// A read at fractional position `pos` for a read rate `rate` (input frames per
// output frame) is a convolution with a Kaiser-windowed sinc whose cutoff
// follows the rate: reading faster than 1:1 lowers the cutoff to the output
// Nyquist frequency (no aliasing when pitching up), reading slower keeps it at
// the input Nyquist frequency (no images when pitching down). Each read is
// normalised by the sum of its weights, so DC passes exactly.
//
// Three kernels:
//   Compact   8 zero crossings, for engines with many voices (unison
//             oscillators, grain clouds) and for very high rates;
//   Realtime  24 zero crossings, the default for live playback;
//   Offline   48 zero crossings, renders and offline tools.
// Above rate 6 a Realtime read switches to the Compact kernel (its cost grows
// with the rate); every kernel follows the rate up to 64 (+72 semitones).
//
// Each kernel has two guards (cutoff as a fraction of the lower Nyquist
// frequency). Reading slower (pitch down) its transition band must end at the
// input Nyquist frequency, or the images of the top octave come back into
// the audible band; reading faster (pitch up) it may run a little past the
// output Nyquist frequency, as what folds back lands above 20 kHz.
//
// Tables are built once (thread-safe); call table(q) from a non-realtime
// thread first so the audio thread only reads.
#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace gl::pitch {

struct SincTable {
  int zeroCrossings = 0;
  int phases = 0;            // table steps per zero crossing
  double guardDown = 0.9;    // cutoff at rates up to 1 (fraction of the input Nyquist frequency)
  double guardUp = 0.94;     // cutoff above rate 1 (fraction of the output Nyquist frequency)
  const SincTable* fast = nullptr;  // the kernel very fast reads switch to (rate over 6)
  std::vector<float> table;  // zeroCrossings · phases + 2 entries of sinc · window over [0, zc]
  // Kernel value at x zero crossings from the centre (|x| ≤ zc).
  float at(double x) const {
    x = x < 0 ? -x : x;
    const double p = x * phases;
    const auto i = static_cast<size_t>(p);
    if (i + 1 >= table.size()) return 0.f;
    const float f = static_cast<float>(p - static_cast<double>(i));
    return table[i] + (table[i + 1] - table[i]) * f;
  }
};

enum class Quality { Realtime = 0, Offline = 1, Compact = 2 };
const SincTable& table(Quality q);

// Highest read rate whose cutoff is still followed (higher rates read with the cutoff of this one).
inline constexpr double kMaxRate = 64.0;

// The table a read at `rate` uses: Realtime hands very fast reads to Compact.
inline const SincTable& tableFor(const SincTable& t, double rate) { return rate > 6.0 && t.fast ? *t.fast : t; }

// Cutoff in cycles per input sample × 2 (1 = input Nyquist).
inline double cutoffOf(double rate, const SincTable& t) {
  const double r = rate < 1e-6 ? 1e-6 : (rate > kMaxRate ? kMaxRate : rate);
  const double up = t.guardUp / r;
  return r <= 1.0 || up > t.guardDown ? t.guardDown : up;  // continuous through rate 1
}

// Generic read: get(i, c) returns channel c of frame i (any index; the
// accessor handles bounds, rings, loops). C channels into out[0 .. C).
template <int C, class Get>
inline void readWith(const Get& get, double pos, double rate, const SincTable& t0, float* out) {
  const SincTable& t = tableFor(t0, rate);
  const double fc = cutoffOf(rate, t);
  const double hw = t.zeroCrossings / fc;
  const auto i0 = static_cast<int64_t>(std::ceil(pos - hw)), i1 = static_cast<int64_t>(std::floor(pos + hw));
  const float* tab = t.table.data();
  const auto last = static_cast<double>(t.table.size() - 2);
  const double step = fc * t.phases;
  double acc[C > 0 ? C : 1] = {}, wsum = 0;
  const auto ic = static_cast<int64_t>(std::floor(pos));
  double tp = (pos - static_cast<double>(i0)) * step;
  for (int64_t i = i0; i <= (ic < i1 ? ic : i1); ++i, tp -= step) {
    const double q = tp < 0 ? 0 : (tp > last ? last : tp);
    const auto k = static_cast<size_t>(q);
    const double w = tab[k] + (tab[k + 1] - tab[k]) * (q - static_cast<double>(k));
    wsum += w;
    for (int c = 0; c < C; ++c) acc[c] += w * get(i, c);
  }
  tp = (static_cast<double>(ic + 1) - pos) * step;
  for (int64_t i = ic + 1; i <= i1; ++i, tp += step) {
    const double q = tp > last ? last : tp;
    const auto k = static_cast<size_t>(q);
    const double w = tab[k] + (tab[k + 1] - tab[k]) * (q - static_cast<double>(k));
    wsum += w;
    for (int c = 0; c < C; ++c) acc[c] += w * get(i, c);
  }
  for (int c = 0; c < C; ++c) out[c] = wsum > 1e-9 ? static_cast<float>(acc[c] / wsum) : 0.0f;
}

// Contiguous channels x[c][0, n), zero outside.
template <int C>
inline void readArrays(const float* const* x, int64_t n, double pos, double rate, const SincTable& t0, float* out) {
  const SincTable& t = tableFor(t0, rate);
  const double hw = t.zeroCrossings / cutoffOf(rate, t);
  if (pos - hw >= 0 && pos + hw < static_cast<double>(n)) {
    readWith<C>([x](int64_t i, int c) { return x[c][i]; }, pos, rate, t, out);
  } else {
    readWith<C>([x, n](int64_t i, int c) { return i >= 0 && i < n ? x[c][i] : 0.0f; }, pos, rate, t, out);
  }
}

inline float read(const float* x, int64_t n, double pos, double rate, const SincTable& t) {
  const float* xs[1] = {x};
  float o;
  readArrays<1>(xs, n, pos, rate, t, &o);
  return o;
}
inline void read2(const float* l, const float* r, int64_t n, double pos, double rate, const SincTable& t, float& outL, float& outR) {
  const float* xs[2] = {l, r};
  float o[2];
  readArrays<2>(xs, n, pos, rate, t, o);
  outL = o[0], outR = o[1];
}

// Exact on whole frames when not reading faster (no filtering of a plain 1:1 read).
inline bool exactFrame(double pos, double rate) {
  return std::fabs(rate) <= 1.000001 && std::fabs(pos - std::floor(pos + 0.5)) < 1e-9;
}

// Circular buffer of `size` frames (any size), indices taken modulo size.
// pos is an absolute (unwrapped) position; the caller keeps it within the
// history the ring holds.
inline float readRing(const float* buf, int64_t size, double pos, double rate, const SincTable& t) {
  float o;
  readWith<1>(
      [buf, size](int64_t i, int) {
        int64_t k = i % size;
        if (k < 0) k += size;
        return buf[k];
      },
      pos, rate, t, &o);
  return o;
}
// Delay-line read: the sample written d frames ago (d = 1 is the newest) from a
// power-of-two ring of mask + 1 frames whose next write goes to w. The head
// moves `rate` frames per output frame; frames newer than the newest (not yet
// written) read as the newest.
inline float readBack(const float* buf, size_t mask, size_t w, double d, double rate, const SincTable& t) {
  if (exactFrame(d, rate)) {
    const auto k = static_cast<size_t>(d < 1.5 ? 1 : std::floor(d + 0.5));
    return buf[(w - k) & mask];
  }
  float o;
  readWith<1>(
      [buf, mask, w](int64_t i, int) {
        if (i > -1) i = -1;
        return buf[(w + static_cast<size_t>(i)) & mask];
      },
      -d, std::fabs(rate), t, &o);
  return o;
}
// Frames the reader reaches on each side of pos at this rate (history / lookahead a ring must keep).
inline int reach(double rate, const SincTable& t0) {
  const SincTable& t = tableFor(t0, rate);
  return static_cast<int>(std::ceil(t.zeroCrossings / cutoffOf(rate, t))) + 1;
}

// The most frames any read at a rate up to `rateMax` reaches (buffer sizing).
inline int maxReach(double rateMax, const SincTable& t) {
  const int a = reach(rateMax < 6.0 ? rateMax : 6.0, t), b = reach(rateMax, t);
  return a > b ? a : b;
}

// Offline: out[j] = x(start + j · rate), j < outLen.
void resample(const float* x, int64_t n, double start, double rate, float* out, int64_t outLen, Quality q = Quality::Offline);
void resample2(const float* l, const float* r, int64_t n, double start, double rate, float* outL, float* outR, int64_t outLen,
               Quality q = Quality::Offline);
// Offline sample-rate conversion of a whole channel: round(n · to / from) frames.
std::vector<float> convert(const float* x, size_t n, double fromRate, double toRate, Quality q = Quality::Offline);

}  // namespace gl::pitch
