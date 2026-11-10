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
#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#include <emmintrin.h>
#define GL_PITCH_SSE2 1
#elif defined(__ARM_NEON) || defined(_M_ARM64)
#include <arm_neon.h>
#define GL_PITCH_NEON 1
#endif

namespace gl::pitch {

// K-159: four float lanes for the readers' dot products (SSE2 on x86, NEON on
// ARM, plain arrays elsewhere). Multiply and add stay two roundings (never
// fused) and the lanes are summed in one fixed order, so every platform gives
// the same result.
namespace lanes {
#if defined(GL_PITCH_SSE2)
using V = __m128;
inline V zero() { return _mm_setzero_ps(); }
inline V load(const float* p) { return _mm_loadu_ps(p); }
inline V add(V a, V b) { return _mm_add_ps(a, b); }
inline V madd(V acc, V a, V b) { return _mm_add_ps(acc, _mm_mul_ps(a, b)); }
inline float sum(V v) {
  float t[4];
  _mm_storeu_ps(t, v);
  return (t[0] + t[1]) + (t[2] + t[3]);
}
#elif defined(GL_PITCH_NEON)
using V = float32x4_t;
inline V zero() { return vdupq_n_f32(0.0f); }
inline V load(const float* p) { return vld1q_f32(p); }
inline V add(V a, V b) { return vaddq_f32(a, b); }
inline V madd(V acc, V a, V b) { return vaddq_f32(acc, vmulq_f32(a, b)); }
inline float sum(V v) {
  float t[4];
  vst1q_f32(t, v);
  return (t[0] + t[1]) + (t[2] + t[3]);
}
#else
struct V {
  float v[4];
};
inline V zero() { return V{{0, 0, 0, 0}}; }
inline V load(const float* p) { return V{{p[0], p[1], p[2], p[3]}}; }
inline V add(V a, V b) { return V{{a.v[0] + b.v[0], a.v[1] + b.v[1], a.v[2] + b.v[2], a.v[3] + b.v[3]}}; }
inline V madd(V acc, V a, V b) {
  float m[4];
  for (int u = 0; u < 4; ++u) m[u] = a.v[u] * b.v[u];
  return V{{acc.v[0] + m[0], acc.v[1] + m[1], acc.v[2] + m[2], acc.v[3] + m[3]}};
}
inline float sum(V v) { return (v.v[0] + v.v[1]) + (v.v[2] + v.v[3]); }
#endif
}  // namespace lanes

struct SincTable {
  int zeroCrossings = 0;
  int phases = 0;            // table steps per zero crossing
  double guardDown = 0.9;    // cutoff at rates up to 1 (fraction of the input Nyquist frequency)
  double guardUp = 0.94;     // cutoff above rate 1 (fraction of the output Nyquist frequency)
  const SincTable* fast = nullptr;  // the kernel very fast reads switch to (rate over 6)
  std::vector<float> table;  // zeroCrossings · phases + 2 entries of sinc · window over [0, zc]
  // The same kernel laid out for reading (K-159), built with the table:
  //  - pairs: table[i] and table[i + 1] - table[i] side by side (one load per tap);
  //  - poly: the kernel at the fixed cutoff guardDown (every read up to rate
  //    guardUp / guardDown) as polyRows + 1 rows of polyTaps contiguous
  //    weights, row k for a read polyRows-th fractions k past a frame, taps
  //    starting polyLeft frames before it; polySum the sum of each row.
  struct Pair {
    float v, d;
  };
  std::vector<Pair> pairs;
  int polyLeft = 0, polyTaps = 0, polyRows = 0;
  std::vector<float> poly;
  std::vector<double> polySum;
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

// K-159: the reads of contiguous audio (the hot path of every sample player).
// The same kernel as readWith, computed for speed: the result differs from
// readWith by rounding only (under -120 dB). Like readWith, a read depends only
// on the frames within the kernel's reach: taps outside [0, n) read zeros
// through a small local copy, with the same arithmetic as inside, so a read
// gives the same bits whatever the arrays hold beyond its reach (a stream read
// as it fills, then again filled further, replays bit for bit).
//
// At the fixed cutoff the weights come from the polyphase rows (two rows,
// blended by the fraction between them): a dot product over contiguous taps.
inline constexpr int kPolyMaxTaps = 128;  // the widest row (Offline: 104 taps)
template <int C>
inline void readPoly(const float* const* x, int64_t n, double pos, const SincTable& t, float* out) {
  const double fl = std::floor(pos);
  const double fp = (pos - fl) * t.polyRows;
  int k = static_cast<int>(fp);
  if (k >= t.polyRows) k = t.polyRows - 1;
  const float g = static_cast<float>(fp - k);
  const int T = t.polyTaps;
  const float* w0 = t.poly.data() + static_cast<size_t>(k) * static_cast<size_t>(T);
  const float* w1 = w0 + T;
  const double ws = t.polySum[static_cast<size_t>(k)] + g * (t.polySum[static_cast<size_t>(k) + 1] - t.polySum[static_cast<size_t>(k)]);
  const int64_t first = static_cast<int64_t>(fl) - t.polyLeft;
  const bool inside = first >= 0 && first + T <= n;
  float copy[C > 0 ? C : 1][kPolyMaxTaps];
  const float* xs[C > 0 ? C : 1];
  lanes::V a[C > 0 ? C : 1], b[C > 0 ? C : 1];
  for (int c = 0; c < C; ++c) {
    a[c] = b[c] = lanes::zero();
    if (inside) {
      xs[c] = x[c] + first;
    } else {
      for (int j = 0; j < T; ++j) copy[c][j] = first + j >= 0 && first + j < n ? x[c][first + j] : 0.0f;
      xs[c] = copy[c];
    }
  }
  for (int j = 0; j < T; j += 4) {
    const lanes::V v0 = lanes::load(w0 + j), v1 = lanes::load(w1 + j);
    for (int c = 0; c < C; ++c) {
      const lanes::V xv = lanes::load(xs[c] + j);
      a[c] = lanes::madd(a[c], v0, xv);
      b[c] = lanes::madd(b[c], v1, xv);
    }
  }
  for (int c = 0; c < C; ++c) {
    const float A = lanes::sum(a[c]), B = lanes::sum(b[c]);
    out[c] = ws > 1e-9 ? static_cast<float>((A + g * (B - A)) / ws) : 0.0f;
  }
}

// Above the fixed cutoff (reading faster): the weights of a block of taps
// first (table positions stepped in 32.32 fixed point, one load per tap), then
// the dot products.
template <int C>
inline void readStepped(const float* const* x, int64_t n, double pos, double rate, const SincTable& t, float* out) {
  const double fc = cutoffOf(rate, t);
  const double hw = t.zeroCrossings / fc;
  const auto i0 = static_cast<int64_t>(std::ceil(pos - hw)), i1 = static_cast<int64_t>(std::floor(pos + hw));
  const auto ic = static_cast<int64_t>(std::floor(pos));
  const SincTable::Pair* tab = t.pairs.data();
  constexpr double kOne = 4294967296.0;  // 2^32
  const double step = fc * t.phases;
  const auto st = static_cast<int64_t>(step * kOne + 0.5);
  const auto lastQ = static_cast<int64_t>(static_cast<double>(t.table.size() - 2) * kOne);
  constexpr int kBlock = 256;
  float w[kBlock];
  float copy[C > 0 ? C : 1][kBlock];
  lanes::V acc[C > 0 ? C : 1], s = lanes::zero();
  float accTail[C > 0 ? C : 1] = {}, sTail = 0;  // taps past the last whole group of four
  for (int c = 0; c < C; ++c) acc[c] = lanes::zero();
  const auto weight = [tab](int64_t q) {
    const SincTable::Pair& e = tab[static_cast<size_t>(q >> 32)];
    return e.v + e.d * (static_cast<float>(static_cast<int32_t>((q >> 8) & 0xFFFFFF)) * (1.0f / 16777216.0f));
  };
  // Left of pos the table position falls by `step` per tap, right of it it rises.
  int64_t qL = static_cast<int64_t>((pos - static_cast<double>(i0)) * step * kOne + 0.5);
  int64_t qR = static_cast<int64_t>((static_cast<double>(ic + 1) - pos) * step * kOne + 0.5);
  for (int64_t b0 = i0; b0 <= i1; b0 += kBlock) {
    const int m = static_cast<int>(i1 - b0 + 1 < kBlock ? i1 - b0 + 1 : kBlock);
    const int left = static_cast<int>(ic - b0 + 1 < 0 ? 0 : (ic - b0 + 1 > m ? m : ic - b0 + 1));  // taps of this block left of pos
    for (int j = 0; j < left; ++j, qL -= st) w[j] = weight(qL < 0 ? 0 : (qL > lastQ ? lastQ : qL));
    for (int j = left; j < m; ++j, qR += st) w[j] = weight(qR > lastQ ? lastQ : qR);
    const float* xs[C > 0 ? C : 1];
    const bool inside = b0 >= 0 && b0 + m <= n;
    for (int c = 0; c < C; ++c) {
      if (inside) {
        xs[c] = x[c] + b0;
      } else {
        for (int j = 0; j < m; ++j) copy[c][j] = b0 + j >= 0 && b0 + j < n ? x[c][b0 + j] : 0.0f;
        xs[c] = copy[c];
      }
    }
    const int whole = m / 4 * 4;
    for (int j = 0; j < whole; j += 4) {
      const lanes::V wv = lanes::load(w + j);
      s = lanes::add(s, wv);
      for (int c = 0; c < C; ++c) acc[c] = lanes::madd(acc[c], wv, lanes::load(xs[c] + j));
    }
    for (int j = whole; j < m; ++j) {
      sTail += w[j];
      for (int c = 0; c < C; ++c) accTail[c] += w[j] * xs[c][j];
    }
  }
  const double ws = static_cast<double>(lanes::sum(s) + sTail);
  for (int c = 0; c < C; ++c) out[c] = ws > 1e-9 ? static_cast<float>(static_cast<double>(lanes::sum(acc[c]) + accTail[c]) / ws) : 0.0f;
}

// Contiguous channels x[c][0, n), zero outside.
template <int C>
inline void readArrays(const float* const* x, int64_t n, double pos, double rate, const SincTable& t0, float* out) {
  const SincTable& t = tableFor(t0, rate);
  if (t.pairs.empty()) {  // (every table has its reading layouts; this is the reference read)
    readWith<C>([x, n](int64_t i, int c) { return i >= 0 && i < n ? x[c][i] : 0.0f; }, pos, rate, t, out);
  } else if (cutoffOf(rate, t) == t.guardDown && !t.poly.empty()) {
    readPoly<C>(x, n, pos, t, out);
  } else {
    readStepped<C>(x, n, pos, rate, t, out);
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
