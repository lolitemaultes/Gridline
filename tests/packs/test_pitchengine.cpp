// Gridline: J-140 shared pitch / time engine (engine/pitch, engine/TimeStretch).
// Objective checks on synthetic material, each against the reader or engine it
// replaced where one existed:
//   kernel     passband flatness, stopband (aliasing), sweep aliasing, pitch
//              accuracy, THD+N, exact DC, exact whole-frame delay reads;
//   stretch    J-153's one stretch: tones clean and in tune, low partials in
//              tune near unity speed, loudness, peaks;
//   extremes   +-48 semitones, x0.25 / x4, silence, DC, impulses, tiny
//              inputs, subnormal input: exact lengths, finite, bounded;
//   realtime   block-size invariance, no allocation while rendering;
//   call sites playlist clip realtime stretch, audio editor resampling and modes,
//              live effects (interval, halt, pitch shifter) band-limited.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
#include <string>
#include <vector>
#include "../../src/core/PluginInfo.h"
#include "../../src/core/Automation.h"
#include "../../src/engine/Effects.h"
#include "../../src/engine/Program.h"
#include "../../src/engine/Sample.h"
#include "../../src/engine/TimeStretch.h"
#include "../../src/engine/pitch/Kernel.h"
#include "../../src/packs/edison/edison.h"
#include "../../src/packs/mini/mini.h"
#include "../../src/packs/spectral/spectral.h"
#include "../Check.h"

// ---- allocation counter (realtime-safety checks) ----
namespace {
thread_local bool gCount = false;
std::atomic<long> gAllocs{0};
}  // namespace
void* operator new(std::size_t n) {
  if (gCount) gAllocs.fetch_add(1, std::memory_order_relaxed);
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) {
  if (gCount) gAllocs.fetch_add(1, std::memory_order_relaxed);
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"  // the replaced operator new is malloc
#endif
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

using namespace gl;
namespace ed = gl::packs::edison;

namespace {

constexpr double kSr = 48000.0;
constexpr double kPi = 3.14159265358979323846;
using cd = std::complex<double>;

struct CountScope {
  CountScope() { gAllocs = 0, gCount = true; }
  ~CountScope() { gCount = false; }
  long n() const { return gAllocs.load(); }
};

// ------------------------------------------------------------ material

std::vector<float> sine(double f, size_t n, double a = 0.5) {
  std::vector<float> y(n);
  for (size_t i = 0; i < n; ++i) y[i] = static_cast<float>(a * std::sin(2 * kPi * f * static_cast<double>(i) / kSr));
  return y;
}
// Log sweep f0 -> f1 over n frames; instantaneous frequency at frame i.
std::vector<float> sweep(double f0, double f1, size_t n, double a = 0.5) {
  std::vector<float> y(n);
  const double T = static_cast<double>(n) / kSr, k = std::log(f1 / f0);
  for (size_t i = 0; i < n; ++i) {
    const double t = static_cast<double>(i) / kSr;
    y[i] = static_cast<float>(a * std::min({1.0, t / 0.01, (T - t) / 0.01}) * std::sin(2 * kPi * f0 * T / k * (std::exp(t / T * k) - 1)));
  }
  return y;
}
double sweepHz(double t, double T, double f0, double f1) { return f0 * std::pow(f1 / f0, t / T); }
// Buzzy vowel: 150 Hz glottal-like harmonics through three formant peaks.
std::vector<float> vowel(double f0, size_t n, double f1 = 700, double b1 = 110) {
  std::vector<float> y(n);
  const double F[3] = {f1, 1220, 2600}, B[3] = {b1, 140, 220}, G[3] = {1.0, 0.6, 0.3};
  for (size_t i = 0; i < n; ++i) {
    double v = 0;
    for (int k = 1; k * f0 < 6000; ++k) {
      double amp = 0;
      for (int j = 0; j < 3; ++j) amp += G[j] / (1 + std::pow((k * f0 - F[j]) / B[j], 2));
      v += amp / std::sqrt(static_cast<double>(k)) * std::cos(2 * kPi * k * f0 * static_cast<double>(i) / kSr);
    }
    y[i] = static_cast<float>(0.25 * v);
  }
  return y;
}
std::vector<float> noise(size_t n, uint32_t s = 0x2468aceu, double a = 0.3) {
  std::vector<float> y(n);
  for (auto& v : y) {
    s ^= s << 13, s ^= s >> 17, s ^= s << 5;
    v = static_cast<float>(a * (static_cast<double>(s) / 4294967296.0 * 2 - 1));
  }
  return y;
}

// ------------------------------------------------------------ analysis

void fft(std::vector<cd>& a) {
  const size_t n = a.size();
  for (size_t i = 1, j = 0; i < n; ++i) {
    size_t bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) std::swap(a[i], a[j]);
  }
  for (size_t len = 2; len <= n; len <<= 1) {
    const double ang = -2 * kPi / static_cast<double>(len);
    const cd wl(std::cos(ang), std::sin(ang));
    for (size_t i = 0; i < n; i += len) {
      cd w(1);
      for (size_t j = 0; j < len / 2; ++j) {
        const cd u = a[i + j], v = a[i + j + len / 2] * w;
        a[i + j] = u + v, a[i + j + len / 2] = u - v;
        w *= wl;
      }
    }
  }
}
// 7-term Blackman-Harris window (sidelobes under -160 dB).
double bh7(size_t i, size_t n) {
  static const double c[7] = {0.27105140069342, 0.43329793923448, 0.21812299954311, 0.06592544638803,
                              0.01081174209837, 0.00077658482522, 0.00001388721735};
  const double x = 2 * kPi * static_cast<double>(i) / static_cast<double>(n);
  double w = 0;
  for (int k = 0; k < 7; ++k) w += (k % 2 ? -1 : 1) * c[k] * std::cos(k * x);
  return w;
}
std::vector<double> power(const std::vector<float>& x, long a, size_t N) {
  std::vector<cd> b(N);
  for (size_t i = 0; i < N; ++i) {
    const long k = a + static_cast<long>(i);
    b[i] = (k >= 0 && k < static_cast<long>(x.size()) ? x[static_cast<size_t>(k)] : 0.0) * bh7(i, N);
  }
  fft(b);
  std::vector<double> p(N / 2 + 1);
  for (size_t k = 0; k <= N / 2; ++k) p[k] = std::norm(b[k]);
  return p;
}
double db(double p) { return 10 * std::log10(std::max(p, 1e-30)); }
double rms(const std::vector<float>& x, size_t a, size_t b) {
  double e = 0;
  b = std::min(b, x.size());
  for (size_t i = a; i < b; ++i) e += static_cast<double>(x[i]) * x[i];
  return std::sqrt(e / static_cast<double>(std::max<size_t>(1, b - a)));
}
// THD+N (dB) of the sine near f in y[a, a + 16384): all but +-14 bins round the peak, 20 Hz .. 20 kHz.
double thdn(const std::vector<float>& y, size_t a, double f) {
  const size_t N = 16384;
  if (a + N > y.size()) return 0;
  const auto p = power(y, static_cast<long>(a), N);
  const double bin = kSr / N;
  size_t pk = static_cast<size_t>(f / 1.05 / bin);
  for (size_t k = pk; k < std::min(p.size(), static_cast<size_t>(f * 1.05 / bin)); ++k)
    if (p[k] > p[pk]) pk = k;
  double tot = 0, sig = 0;
  for (size_t k = static_cast<size_t>(20 / bin); k < std::min(p.size(), static_cast<size_t>(20000 / bin)); ++k) {
    tot += p[k];
    if (k + 14 >= pk && k <= pk + 14) sig += p[k];
  }
  return db((tot - sig) / std::max(sig, 1e-30));
}
// Strongest partial within +-5 % of `near` in y[a, a + 32768), interpolated (Hz).
double peakHz(const std::vector<float>& y, size_t a, double near) {
  const size_t N = 32768;
  if (a + N > y.size()) return 0;
  const auto p = power(y, static_cast<long>(a), N);
  const size_t k0 = static_cast<size_t>(near / 1.05 * N / kSr), k1 = std::min(p.size() - 2, static_cast<size_t>(near * 1.05 * N / kSr));
  size_t best = k0;
  for (size_t k = k0; k <= k1; ++k)
    if (p[k] > p[best]) best = k;
  const double ym = std::log(p[best - 1] + 1e-30), y0 = std::log(p[best] + 1e-30), yp = std::log(p[best + 1] + 1e-30);
  return (static_cast<double>(best) + 0.5 * (ym - yp) / (ym - 2 * y0 + yp)) * kSr / static_cast<double>(N);
}
double cents(double f, double ref) { return f > 0 ? 1200 * std::log2(f / ref) : 1e9; }
// Sweep aliasing: energy (25 Hz .. 20 kHz) outside the expected tone +-8 bins vs inside,
// over frames whose expected output tone is below 0.9 of the output Nyquist frequency
// (mean, dB); and the level of frames whose tone is above it (should be gone).
struct Alias {
  double meanDb = -300, foldDb = -300;
};
template <class F>
Alias aliasing(const std::vector<float>& y, F expected) {
  const size_t N = 2048, H = 1024;
  double un = 0, in = 0, ref = 0;
  std::vector<double> folds;
  for (long a = 0; a + static_cast<long>(N) < static_cast<long>(y.size()); a += H) {
    const double fa = expected(static_cast<double>(a) / kSr), fb = expected((static_cast<double>(a) + N) / kSr);
    if (fa <= 0 || fb <= 0) continue;
    const auto p = power(y, a, N);
    const double bin = kSr / N;
    double tot = 0;
    for (size_t k = static_cast<size_t>(25 / bin); k < static_cast<size_t>(20000 / bin); ++k) tot += p[k];
    const double lo = std::min(fa, fb), hi = std::max(fa, fb);
    if (lo > 0.95 * kSr / 2) {
      folds.push_back(tot);
      continue;
    }
    if (hi > 0.9 * kSr / 2) continue;
    double s = 0;
    for (size_t k = static_cast<size_t>(std::max(1.0, lo / bin - 8)); k <= std::min(p.size() - 1, static_cast<size_t>(hi / bin + 8)); ++k) s += p[k];
    un += std::max(0.0, tot - s), in += s, ref = std::max(ref, s);
  }
  Alias r;
  r.meanDb = db(un / std::max(in, 1e-30));
  for (double f : folds) r.foldDb = std::max(r.foldDb, db(f / std::max(ref, 1e-30)));
  return r;
}
bool finite(const std::vector<float>& v) {
  for (float x : v)
    if (!std::isfinite(x)) return false;
  return true;
}
bool noSubnormals(const std::vector<float>& v) {
  for (float x : v)
    if (std::fpclassify(x) == FP_SUBNORMAL) return false;
  return true;
}
double peakOf(const std::vector<float>& v) {
  double p = 0;
  for (float x : v) p = std::max(p, static_cast<double>(std::fabs(x)));
  return p;
}

// ------------------------------------------------------------ legacy references
// The readers the engine replaced, for side-by-side comparisons.
float legacyLinear(const std::vector<float>& x, double pos) {
  const auto i = static_cast<int64_t>(std::floor(pos));
  const double f = pos - static_cast<double>(i);
  auto at = [&](int64_t k) { return k >= 0 && k < static_cast<int64_t>(x.size()) ? x[static_cast<size_t>(k)] : 0.f; };
  return static_cast<float>(at(i) + (at(i + 1) - at(i)) * f);
}
float legacyHermite(const std::vector<float>& x, double pos) {
  const auto i = static_cast<int64_t>(std::floor(pos));
  const auto t = static_cast<float>(pos - static_cast<double>(i));
  auto at = [&](int64_t k) { return k >= 0 && k < static_cast<int64_t>(x.size()) ? x[static_cast<size_t>(k)] : 0.f; };
  const float xm1 = at(i - 1), x0 = at(i), x1 = at(i + 1), x2 = at(i + 2);
  const float c1 = 0.5f * (x1 - xm1), c2 = xm1 - 2.5f * x0 + 2.f * x1 - 0.5f * x2, c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
  return ((c3 * t + c2) * t + c1) * t + x0;
}
template <class R>
std::vector<float> readAll(const std::vector<float>& x, double rate, R reader) {
  std::vector<float> y(static_cast<size_t>(static_cast<double>(x.size()) / rate));
  for (size_t j = 0; j < y.size(); ++j) y[j] = reader(static_cast<double>(j) * rate);
  return y;
}

// ------------------------------------------------------------ engine drivers

std::vector<float> offline(const std::vector<float>& x, stretch::Mode m, double pitch, double timeFactor) {
  stretch::Settings s;
  s.mode = m, s.pitch = pitch;
  const auto n = static_cast<int64_t>(x.size());
  const auto outLen = static_cast<int64_t>(std::llround(static_cast<double>(n) * (m == stretch::Mode::Resample ? 1.0 / pitch : timeFactor)));
  std::vector<float> y(static_cast<size_t>(std::max<int64_t>(0, outLen))), y2(y.size());
  const float* in[2] = {x.data(), x.data()};
  float* out[2] = {y.data(), y2.data()};
  stretch::process(in, 2, n, out, outLen, kSr, s);
  return y;
}

struct Src {
  const std::vector<float>* x = nullptr;
  size_t pos = 0;
  static void read(void* ctx, float* const* d, int n) {
    auto* s = static_cast<Src*>(ctx);
    for (int i = 0; i < n; ++i) {
      const float v = s->pos < s->x->size() ? (*s->x)[s->pos] : 0.f;
      d[0][i] = d[1][i] = v;
      ++s->pos;
    }
  }
};
// Realtime voice: `frames` output frames at speed / pitch in blocks of `block`.
std::vector<float> realtime(const std::vector<float>& x, double speed, double pitch, size_t frames, int block, long* allocs = nullptr) {
  stretch::Voice v;
  v.prepare(2, kSr, 4.0, 4.0);
  Src src{&x, 0};
  std::vector<float> L(frames), R(frames);
  CountScope cs;
  v.start(&Src::read, &src, speed, pitch);
  for (size_t at = 0; at < frames;) {
    const int n = static_cast<int>(std::min<size_t>(static_cast<size_t>(block), frames - at));
    float* o[2] = {L.data() + at, R.data() + at};
    v.render(o, n, speed, pitch, &Src::read, &src);
    at += static_cast<size_t>(n);
  }
  if (allocs) *allocs = cs.n();
  return L;
}

std::vector<float> effect(const PluginState& s, const std::vector<float>& x, int block = 256, long* allocs = nullptr) {
  auto fx = createEffect(s.type);
  fx->prepareHost(s, kSr);
  ProcessContext ctx;
  ctx.sampleRate = kSr;
  std::vector<float> L = x, R = x;
  long a = 0;
  for (size_t at = 0; at < L.size(); at += static_cast<size_t>(block)) {
    const int n = static_cast<int>(std::min<size_t>(static_cast<size_t>(block), L.size() - at));
    CountScope cs;
    fx->process(s, ctx, L.data() + at, R.data() + at, n);
    a += cs.n();
  }
  if (allocs) *allocs = a;
  return L;
}

const char* modeLabel(stretch::Mode m) { return stretch::modeName(m); }
constexpr stretch::Mode kModes[2] = {stretch::Mode::Resample, stretch::kStretch};

}  // namespace

int main() {
  const pitch::SincTable& rt = pitch::table(pitch::Quality::Realtime);
  const pitch::SincTable& off = pitch::table(pitch::Quality::Offline);
  const pitch::SincTable& cmp = pitch::table(pitch::Quality::Compact);
  char msg[256];

  // ============================================================ kernel
  {
    // Passband: a 1 kHz tone read at 0.5x .. 2x keeps its level and pitch.
    const auto x = sine(1000, static_cast<size_t>(1.5 * kSr));
    double worstLevel = 0, worstCents = 0, worstThd = -300;
    for (double rate : {0.5, 0.8, 1.25, 2.0})
      for (const pitch::SincTable* t : {&rt, &off}) {
        const auto y = readAll(x, rate, [&](double p) { return pitch::read(x.data(), static_cast<int64_t>(x.size()), p, rate, *t); });
        const double f = 1000 * rate;
        worstLevel = std::max(worstLevel, std::fabs(20 * std::log10(rms(y, 2000, y.size() - 2000) / (0.5 / std::sqrt(2.0)))));
        worstCents = std::max(worstCents, std::fabs(cents(peakHz(y, 2000, f), f)));
        worstThd = std::max(worstThd, thdn(y, 2000, f));
      }
    std::printf("# kernel passband: level %.4f dB, pitch %.4f cents, THD+N %.1f dB\n", worstLevel, worstCents, worstThd);
    expect(worstLevel < 0.02 && worstCents < 0.05, "kernel: a tone read at 0.5x .. 2x keeps its level (0.02 dB) and pitch (0.05 cents)");
    expect(worstThd < -100, "kernel: THD+N under -100 dB at 0.5x .. 2x");

    // Stopband: an 18 kHz tone read at 2x (36 kHz, above Nyquist) must vanish.
    const auto hi = sine(18000, static_cast<size_t>(kSr));
    auto level = [&](const std::vector<float>& y) { return 20 * std::log10(rms(y, 1000, y.size() - 1000) / (0.5 / std::sqrt(2.0)) + 1e-12); };
    const double sRt = level(readAll(hi, 2.0, [&](double p) { return pitch::read(hi.data(), static_cast<int64_t>(hi.size()), p, 2.0, rt); }));
    const double sOff = level(readAll(hi, 2.0, [&](double p) { return pitch::read(hi.data(), static_cast<int64_t>(hi.size()), p, 2.0, off); }));
    const double sCmp = level(readAll(hi, 2.0, [&](double p) { return pitch::read(hi.data(), static_cast<int64_t>(hi.size()), p, 2.0, cmp); }));
    const double sLin = level(readAll(hi, 2.0, [&](double p) { return legacyLinear(hi, p); }));
    const double sHer = level(readAll(hi, 2.0, [&](double p) { return legacyHermite(hi, p); }));
    std::printf("# kernel stopband (18 kHz at 2x): realtime %.1f, offline %.1f, compact %.1f dB; legacy linear %.1f, Hermite %.1f dB\n", sRt, sOff,
                sCmp, sLin, sHer);
    expect(sRt < -85 && sOff < -100 && sCmp < -50, "kernel: a tone pushed above Nyquist is removed (realtime -85, offline -100, compact -50 dB)");
    expect(sLin > -20 && sHer > -20, "legacy readers alias the same tone back into the band (reference)");

    // Sweep aliasing at +7 semitones and +12, realtime vs the legacy linear reader.
    const auto sw = sweep(20, 20000, static_cast<size_t>(4 * kSr));
    const double T = 4.0;
    for (double st : {7.0, 12.0}) {
      const double r = std::pow(2.0, st / 12.0);
      auto expected = [&](double tOut) {
        const double tin = tOut * r;
        if (tin < 0.02 || tin > T - 0.02) return -1.0;
        const double fs = sweepHz(tin, T, 20, 20000);
        return fs > 18000 ? -1.0 : fs * r;
      };
      const Alias a = aliasing(readAll(sw, r, [&](double p) { return pitch::read(sw.data(), static_cast<int64_t>(sw.size()), p, r, rt); }), expected);
      const Alias l = aliasing(readAll(sw, r, [&](double p) { return legacyLinear(sw, p); }), expected);
      std::printf("# sweep +%g st: realtime alias %.1f dB fold %.1f dB; legacy linear alias %.1f dB fold %.1f dB\n", st, a.meanDb, a.foldDb, l.meanDb,
                  l.foldDb);
      std::snprintf(msg, sizeof msg, "kernel: sweep at +%g st, aliasing under -85 dB and 40 dB under the legacy linear reader", st);
      expect(a.meanDb < -85 && a.foldDb < -80 && std::max(a.meanDb, a.foldDb) < std::max(l.meanDb, l.foldDb) - 40, msg);
    }

    // DC passes exactly at any position and rate (weights normalised).
    std::vector<float> dc(20000, 0.5f);
    double dcErr = 0;
    for (double rate : {0.03, 0.7, 1.0, 3.3, 9.0, 40.0})
      for (double p = 9000.25; p < 11000; p += 97.37)
        for (const pitch::SincTable* t : {&cmp, &rt, &off})
          dcErr = std::max(dcErr, std::fabs(pitch::read(dc.data(), 20000, p, rate, *t) - 0.5));
    expect(dcErr < 1e-6, "kernel: DC passes exactly at every rate (0.03x .. 40x) and position");

    // Delay-line reads: whole frames at 1:1 are exact; unwritten frames read as the newest.
    std::vector<float> ring(1024, 0.f);
    for (size_t i = 0; i < 700; ++i) ring[i] = static_cast<float>(std::sin(0.1 * static_cast<double>(i)));
    bool exact = true;
    for (int d = 1; d < 600; ++d) exact = exact && pitch::readBack(ring.data(), 1023, 700, d, 1.0, rt) == ring[static_cast<size_t>(700 - d)];
    const float near = pitch::readBack(ring.data(), 1023, 700, 1.25, 1.0, rt);
    expect(exact && std::isfinite(near) && std::fabs(near) < 1.2f, "delay-line reads: whole-frame delays exact, reads next to the write head stay bounded");

    // Extremes: +-72 semitones, far outside the data, one-frame inputs.
    bool ok = true;
    const auto nz = noise(5000);
    for (double rate : {1.0 / 64, 1.0 / 16, 16.0, 64.0})
      for (double p : {-1e6, -30.0, 0.0, 2500.5, 4999.9, 5030.0, 1e9})
        for (const pitch::SincTable* t : {&cmp, &rt, &off}) {
          const float v = pitch::read(nz.data(), 5000, p, rate, *t);
          ok = ok && std::isfinite(v) && std::fabs(v) < 1.0f;
        }
    const float one = 0.7f;
    ok = ok && std::fabs(pitch::read(&one, 1, 0.0, 1.0, rt)) <= 0.7001f && pitch::read(&one, 1, 50.0, 1.0, rt) == 0.f;
    expect(ok, "kernel: finite and bounded at 1/64x .. 64x, far outside the data and on one-frame inputs");

    // Offline conversion: 44.1 kHz -> 48 kHz keeps a 1 kHz tone clean.
    std::vector<float> s44(44100);
    for (size_t i = 0; i < s44.size(); ++i) s44[i] = static_cast<float>(0.5 * std::sin(2 * kPi * 1000 * static_cast<double>(i) / 44100));
    const auto c48 = pitch::convert(s44.data(), s44.size(), 44100, 48000);
    const double cThd = thdn(c48, 4000, 1000);
    std::printf("# 44.1 -> 48 kHz conversion: THD+N %.1f dB\n", cThd);
    expect(c48.size() == static_cast<size_t>(std::llround(s44.size() * 48000.0 / 44100.0)) && cThd < -110,
           "sample-rate conversion: exact length, THD+N under -110 dB");
  }

  // ============================================================ the stretch
  {
    // A 220 Hz tone shifted offline: clean and in tune.
    const auto t = sine(220, static_cast<size_t>(2.0 * kSr));
    double worstTh = -300, worstC = 0;
    for (double st : {-12.0, -5.0, 7.0, 12.0}) {
      const double r = std::pow(2.0, st / 12.0);
      const auto y = offline(t, stretch::kStretch, r, 1.0);
      const double th = thdn(y, 24000, 220 * r), c = cents(peakHz(y, 24000, 220 * r), 220 * r);
      std::printf("# stretch sine %+g st: THD+N %.1f dB, %.3f cents\n", st, th, c);
      worstTh = std::max(worstTh, th), worstC = std::max(worstC, std::fabs(c));
    }
    expect(worstTh < -60 && worstC < 0.1, "stretch: a 220 Hz tone at -12 .. +12 st is clean (THD+N under -60 dB) and in tune (0.1 cent)");

    // A 55 Hz bass (8 harmonics) near unity speed and a semitone off: every
    // partial in tune. (A hop under 30 ms let the engine mistune low partials
    // here by up to 12 Hz: a rough, noisy bass.)
    std::vector<float> bass(static_cast<size_t>(4 * kSr));
    for (size_t i = 0; i < bass.size(); ++i) {
      double v = 0;
      for (int k = 1; k <= 8; ++k) v += std::sin(2 * kPi * 55 * k * static_cast<double>(i) / kSr + k) / k;
      bass[i] = static_cast<float>(0.3 * v);
    }
    double worstHz = 0;
    for (const auto& [pitch, tf] : {std::pair<double, double>{1.0, 0.94}, {1.0, 1.06}, {1.0, 1.5}, {std::pow(2.0, 1 / 12.0), 1.0}, {std::pow(2.0, -1 / 12.0), 1.0}}) {
      const auto y = offline(bass, stretch::kStretch, pitch, tf);
      for (int k = 1; k <= 8; ++k) {
        const double want = 55 * k * pitch;
        worstHz = std::max(worstHz, std::fabs(peakHz(y, static_cast<size_t>(0.5 * kSr), want) - want));
      }
    }
    std::printf("# 55 Hz bass x0.94 .. x1.5 and +-1 st: partials within %.3f Hz\n", worstHz);
    expect(worstHz < 0.5, "stretch: a low bass near unity speed keeps every partial in tune (within 0.5 Hz)");

    // Loudness: x0.5 .. x2 stretches keep the level of tones and noise. (Noise
    // loses up to 2 dB: frames of unrelated phases overlap with less gain
    // than a steady tone's, a known trait of phase vocoders.)
    double worst = 0, worstNoise = 0;
    const auto nz = noise(static_cast<size_t>(2 * kSr));
    for (double tf : {0.5, 0.8, 1.25, 2.0})
      for (const std::vector<float>* x : {&nz, &t}) {
        const auto y = offline(*x, stretch::kStretch, 1.0, tf);
        const double g = 20 * std::log10(rms(y, y.size() / 5, y.size() * 4 / 5) / rms(*x, x->size() / 5, x->size() * 4 / 5));
        if (std::fabs(g) > 0.5) std::printf("#   %s x%g: %+.2f dB\n", x == &nz ? "noise" : "tone", tf, g);
        (x == &nz ? worstNoise : worst) = std::max(x == &nz ? worstNoise : worst, std::fabs(g));
      }
    std::printf("# stretch loudness: tones %.2f dB, noise %.2f dB\n", worst, worstNoise);
    expect(worst < 0.5 && worstNoise < 2.0, "stretch: x0.5 .. x2 keeps the level of tones (0.5 dB) and noise (2 dB)");

    // Peaks: a full-scale square (crest factor 1, the lowest there is).
    // Resample copies the waveform: only band-limiting overshoot. The stretch
    // re-phases the harmonics, which raises the crest factor (not the
    // loudness): its level must stay put.
    std::vector<float> sq(static_cast<size_t>(kSr));
    for (size_t i = 0; i < sq.size(); ++i) sq[i] = (i / 50) % 2 ? 1.f : -1.f;
    double pkTd = 0, lvSp = 0;
    for (stretch::Mode mode : kModes)
      for (double st : {-12.0, 12.0}) {
        const auto y = offline(sq, mode, std::pow(2.0, st / 12.0), 1.0);
        if (mode == stretch::Mode::Resample) pkTd = std::max(pkTd, peakOf(y));
        else lvSp = std::max(lvSp, std::fabs(20 * std::log10(rms(y, y.size() / 5, y.size() * 4 / 5))));
      }
    std::printf("# full-scale square +-12 st: resample peak %.3f; stretch level %.2f dB\n", pkTd, lvSp);
    expect(pkTd < 1.4, "Resample: a full-scale square peaks only by band-limiting overshoot (under 1.4)");
    expect(lvSp < 1.5, "stretch: a full-scale square keeps its level within 1.5 dB (no boost past its peak)");
  }

  // ============================================================ extremes
  {
    bool lengths = true, fin = true, silent = true, dcOk = true, sub = true;
    const auto imp = [] {
      std::vector<float> x(24000, 0.f);
      x[12000] = 1.f;
      return x;
    }();
    const std::vector<float> zero(24000, 0.f), dc(48000, 0.5f), tiny(10, 0.3f);
    // A tone decaying into silence: no engine may leave subnormal values behind
    // (they stall the FPU in later processing).
    std::vector<float> subn(36000, 0.f);
    for (size_t i = 0; i < 12000; ++i)
      subn[i] = static_cast<float>(0.5 * std::exp(-static_cast<double>(i) / 1500.0) * std::sin(2 * kPi * 440 * static_cast<double>(i) / kSr));
    for (const stretch::Mode m : kModes) {
      for (double st : {-48.0, 0.0, 48.0})
        for (double tf : {0.25, 1.0, 4.0}) {
          const double r = std::pow(2.0, st / 12.0);
          for (const std::vector<float>* x : {&imp, &zero, &tiny, static_cast<const std::vector<float>*>(&subn)}) {
            const auto y = offline(*x, m, r, tf);
            const auto want = static_cast<size_t>(std::llround(static_cast<double>(x->size()) * (m == stretch::Mode::Resample ? 1.0 / r : tf)));
            lengths = lengths && y.size() == want;
            fin = fin && finite(y) && peakOf(y) < 4.0;
            if (x == &zero) silent = silent && peakOf(y) == 0.0;
            if (x == &subn && !noSubnormals(y)) {
              sub = false;
              std::printf("# subnormals from %s %+g st x%g\n", modeLabel(m), st, tf);
            }
          }
        }
      if (m == stretch::Mode::Resample)
        for (double tf : {0.5, 2.0}) {
          const auto y = offline(dc, m, 1.0, tf);
          const double mean = rms(y, y.size() / 4, y.size() * 3 / 4);
          if (std::fabs(mean - 0.5) > 0.02) {
            dcOk = false;
            std::printf("# DC through %s x%g: %.4f\n", modeLabel(m), tf, mean);
          }
        }
    }
    expect(lengths, "stretch and resample at +-48 st and x0.25 .. x4: exactly the asked length (impulse, silence, 10-frame and decaying inputs)");
    expect(fin, "stretch and resample at the extremes: finite output, bounded peaks");
    expect(silent, "stretch and resample: silence in, exact silence out");
    expect(sub, "stretch and resample: a sound decaying into silence leaves no subnormal values");
    expect(dcOk, "Resample: DC passes at x0.5 and x2 (within 2 %)");
  }

  // ============================================================ realtime voices
  {
    const auto v = vowel(180, static_cast<size_t>(1.5 * kSr));
    for (const auto& [speed, pitch] : {std::pair<double, double>{0.8, 1.3}, {1.0, 0.5}, {2.0, 1.0}}) {
      long a1 = 0;
      const auto y1 = realtime(v, speed, pitch, 50000, 512, &a1);
      const auto y2 = realtime(v, speed, pitch, 50000, 61);
      float d = 0;
      for (size_t i = 0; i < y1.size(); ++i) d = std::max(d, std::fabs(y1[i] - y2[i]));
      std::snprintf(msg, sizeof msg, "realtime voice x%g %+.1f st: the same output for any block size (diff %.2g)", speed, 12 * std::log2(pitch),
                    static_cast<double>(d));
      expect(d < 1e-5f && rms(y1, 5000, 45000) > 0.01, msg);
      std::snprintf(msg, sizeof msg, "realtime voice x%g: start and render without allocation", speed);
      expect(a1 == 0, msg);
    }
    // Realtime voice in tune and clean.
    const auto t = sine(220, static_cast<size_t>(2 * kSr));
    const auto yt = realtime(t, 1.0, std::pow(2.0, 7 / 12.0), 80000, 256);
    const double c = cents(peakHz(yt, 20000, 220 * std::pow(2.0, 7 / 12.0)), 220 * std::pow(2.0, 7 / 12.0));
    const double th = thdn(yt, 20000, 220 * std::pow(2.0, 7 / 12.0));
    std::printf("# realtime voice +7 st: %.3f cents, THD+N %.1f dB\n", c, th);
    expect(std::fabs(c) < 0.1 && th < -60, "realtime voice: a tone in tune within 0.1 cent, THD+N under -60 dB");
  }

  // ============================================================ call sites
  {
    // Playlist clip realtime stretch (while the render is on its way): a bass tone pitched +7 st stays in tune.
    auto smp = std::make_shared<Sample>();
    smp->sampleRate = kSr;
    {
      std::vector<float> b(static_cast<size_t>(3 * kSr));
      for (size_t i = 0; i < b.size(); ++i) {
        double s = 0;
        for (int h = 1; h <= 30; ++h) s += std::sin(2 * kPi * h * 55 * static_cast<double>(i) / kSr) / h;
        b[i] = static_cast<float>(0.3 * s);
      }
      smp->left = b;
      smp->setMono();
    }
    TempoMap map;
    map.setConstant(120, 96);
    ProgAudioClip c;
    c.sample = smp, c.srcStart = 0, c.srcEnd = smp->seconds(), c.speed = 1.0, c.pitch = std::pow(2.0, 7 / 12.0), c.granular = true;
    c.startSec = 0, c.durSec = smp->seconds(), c.start = 0, c.end = static_cast<Tick>(std::ceil(c.durSec * 192));
    c.rt = makeClipVoice(kSr);
    const std::vector<ProgAudioClip> clips{c};
    std::vector<float> L(static_cast<size_t>(2.5 * kSr)), R(L.size());
    long allocs = 0;
    for (size_t at = 0; at < L.size(); at += 512) {
      const int n = static_cast<int>(std::min<size_t>(512, L.size() - at));
      CountScope cs;
      renderAudioClips(clips, map, kSr, 0, static_cast<int64_t>(at), 0, n, L.data() + at, R.data() + at);
      allocs += cs.n();
    }
    const double want = 55 * 4 * std::pow(2.0, 7 / 12.0);  // 4th harmonic: finer resolution
    const double cc = cents(peakHz(L, 24000, want), want);
    std::printf("# clip realtime stretch, 55 Hz bass +7 st: %.2f cents (4th harmonic)\n", cc);
    expect(std::fabs(cc) < 3.0 && allocs == 0, "playlist clip realtime stretch: a pitched bass stays in tune (3 cents), no allocation");

    // Audio editor: resampling through the shared kernel, every method gives the exact length.
    ed::Audio a = ed::makeAudio(44100, 1, 44100);
    for (int64_t i = 0; i < a.frames(); ++i) a.L[static_cast<size_t>(i)] = a.R[static_cast<size_t>(i)] = static_cast<float>(0.5 * std::sin(2 * kPi * 1000 * static_cast<double>(i) / 44100));
    const ed::Audio b = ed::resampleAudio(a, 48000, 2);
    std::vector<float> bl(b.L.begin(), b.L.end());
    const double eThd = thdn(bl, 4000, 1000);
    std::printf("# audio editor 44.1 -> 48 kHz: THD+N %.1f dB\n", eThd);
    expect(b.frames() == 48000 && eThd < -110, "audio editor resample: exact length, THD+N under -110 dB");
    bool lens = true;
    for (int m = 0; m < static_cast<int>(ed::Stretch::Count); ++m) {
      const ed::Audio o = ed::stretchAudio(a, 1.5, 3, static_cast<ed::Stretch>(m), 1);
      const int64_t want2 = static_cast<ed::Stretch>(m) == ed::Stretch::Tape ? std::llround(44100 / std::pow(2.0, 3 / 12.0)) : std::llround(44100 * 1.5);
      lens = lens && std::llabs(o.frames() - want2) <= 1 && finite(std::vector<float>(o.L.begin(), o.L.end()));
    }
    expect(lens, "audio editor stretch: both methods give the exact length");

    // Live effects: an octave up on a 15 kHz tone (30 kHz, above Nyquist) must not fold back.
    const auto hi = sine(15000, static_cast<size_t>(kSr), 0.5);
    auto bandDb = [&](const std::vector<float>& y) { return 20 * std::log10(rms(y, 12000, y.size()) / (0.5 / std::sqrt(2.0)) + 1e-12); };
    PluginState iv = defaultPluginState("gl.miniinterval");
    iv.params[static_cast<size_t>(packs::mini::interval::Pitch)] = 12.f;
    iv.params[static_cast<size_t>(packs::mini::interval::Mix)] = 1.f;
    long ia = 0;
    const double ivDb = bandDb(effect(iv, hi, 256, &ia));
    std::vector<float> leg(hi.size(), 0.f);  // the legacy two-tap shifter (Hermite) for reference
    {
      const double G = 0.05 * kSr;
      double p = 0;
      for (size_t i = 0; i < hi.size(); ++i) {
        p += (1.0 - 2.0) / G;
        p -= std::floor(p);
        double q = p + 0.5;
        q -= std::floor(q);
        const double w1 = std::sin(kPi * p) * std::sin(kPi * p);
        const double pos1 = static_cast<double>(i) - (2.0 + p * G) + 1, pos2 = static_cast<double>(i) - (2.0 + q * G) + 1;
        leg[i] = static_cast<float>(w1 * legacyHermite(hi, pos1) + (1 - w1) * legacyHermite(hi, pos2));
      }
    }
    const double legDb = bandDb(leg);
    std::printf("# interval +12 st on 15 kHz: %.1f dB (legacy Hermite taps %.1f dB)\n", ivDb, legDb);
    expect(ivDb < -45 && ivDb < legDb - 25 && ia == 0, "interval shifter: an octave up does not fold back (45 dB down, 25 dB under the legacy taps), no allocation");

    PluginState ps = defaultPluginState("gl.pitch");
    ps.params[static_cast<size_t>(packs::spectral::PsEngine)] = 0.f;  // Classic
    ps.params[static_cast<size_t>(packs::spectral::PsMode)] = 0.f;    // fast delay-line shifter
    ps.params[static_cast<size_t>(packs::spectral::PsPitch)] = 12.f;
    ps.params[static_cast<size_t>(packs::spectral::PsMix)] = 1.f;
    long pa = 0;
    const double psDb = bandDb(effect(ps, hi, 256, &pa));
    std::printf("# pitch shifter (classic fast) +12 st on 15 kHz: %.1f dB\n", psDb);
    expect(psDb < -45 && pa == 0, "pitch shifter, classic fast engine: an octave up does not fold back, no allocation");

    PluginState hs = defaultPluginState("gl.minihalt");
    hs.params[static_cast<size_t>(packs::mini::halt::Run)] = 0.f;
    hs.params[static_cast<size_t>(packs::mini::halt::StopTime)] = 800.f;
    long ha = 0;
    const auto ho = effect(hs, sine(9000, static_cast<size_t>(kSr), 0.5), 128, &ha);
    // Slowing down, the images of a 9 kHz tone (mirrored round the input rate) must stay out.
    double img = 0, tot = 0;
    for (long at2 = 4800; at2 + 2048 < static_cast<long>(ho.size()); at2 += 2048) {
      const auto p = power(ho, at2, 2048);
      for (size_t k = 1; k < p.size(); ++k) {
        tot += p[k];
        if (k * kSr / 2048 > 9500) img += p[k];
      }
    }
    std::printf("# halt stop on 9 kHz: energy above 9.5 kHz %.1f dB\n", db(img / std::max(tot, 1e-30)));
    expect(finite(ho) && db(img / std::max(tot, 1e-30)) < -60 && ha == 0, "tape stop: slowing down leaves no images above the tone, no allocation");
  }

  return finish();
}
