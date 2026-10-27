// Gridline: J-144 Voice pitch / time mode on voices (engine/pitch/Psola,
// engine/pitch/VoiceTrack) and the Formant mode's formant analysis.
// One check per defect the J-144 assessment found, on synthetic material:
//   old pitch left in the output (a comb at the source's harmonics), pitch
//   down and up, Voice and Formant;
//   breaths / noise stretched into a comb filter;
//   voicing that flickers, octave errors, pure-tone detection that flips;
//   the top octave lost when pitching down;
//   realtime grains cut at block edges (clicks); unison not transparent;
// plus the engine contract: block-size invariance, no allocation, offline
// determinism, 44.1 / 48 / 96 kHz, mono and stereo.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <vector>
#include "../../src/engine/TimeStretch.h"
#include "../../src/engine/pitch/VoiceTrack.h"
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

namespace {

constexpr double kPi = 3.14159265358979323846;
char msg[256];

struct CountScope {
  CountScope() { gAllocs = 0, gCount = true; }
  ~CountScope() { gCount = false; }
  long n() const { return gAllocs.load(); }
};

// ------------------------------------------------------------ material

// A sung / spoken vowel: harmonics of f0 (falling 6 dB per octave from a
// glottal-like source) through three formant resonances; optional vibrato.
// f0Of(t) gives the pitch; harmonic phases follow it.
struct VowelOpts {
  double f0 = 150, vibrato = 0, rate = 5.5;  // vibrato depth (fraction of f0), rate Hz
  double F1 = 700, F2 = 1220, F3 = 2600;
  bool noFundamental = false;  // the first harmonic removed, the second boosted (an octave trap)
  double top = 6000;           // highest harmonic frequency
};
std::vector<float> vowel(double sr, size_t n, const VowelOpts& o) {
  std::vector<float> y(n);
  const double F[3] = {o.F1, o.F2, o.F3}, B[3] = {110, 140, 220}, G[3] = {1.0, 0.6, 0.3};
  const int K = static_cast<int>(o.top / (o.f0 * (1 + o.vibrato)));
  std::vector<double> amp(static_cast<size_t>(K + 1));
  double ph = 0;
  for (size_t i = 0; i < n; ++i) {
    const double t = static_cast<double>(i) / sr;
    const double f = o.f0 * (1 + o.vibrato * std::sin(2 * kPi * o.rate * t));
    double v = 0;
    for (int k = 1; k <= K; ++k) {
      const double fk = k * f;
      if (fk > 0.45 * sr) break;
      double a = 0;
      for (int j = 0; j < 3; ++j) a += G[j] / (1 + std::pow((fk - F[j]) / B[j], 2));
      a /= static_cast<double>(k);
      if (o.noFundamental) a = k == 1 ? 0.0 : (k == 2 ? 3 * a : a);
      v += a * std::cos(k * ph);
    }
    ph += 2 * kPi * f / sr;
    if (ph > 2 * kPi * 1e6) ph -= 2 * kPi * 1e6;
    y[i] = static_cast<float>(0.3 * v);
  }
  return y;
}
std::vector<float> sine(double sr, double f, size_t n, double a = 0.5) {
  std::vector<float> y(n);
  for (size_t i = 0; i < n; ++i) y[i] = static_cast<float>(a * std::sin(2 * kPi * f * static_cast<double>(i) / sr));
  return y;
}
std::vector<float> noise(size_t n, uint32_t s = 0x13579bdu, double a = 0.3) {
  std::vector<float> y(n);
  for (auto& v : y) {
    s ^= s << 13, s ^= s >> 17, s ^= s << 5;
    v = static_cast<float>(a * (static_cast<double>(s) / 4294967296.0 * 2 - 1));
  }
  return y;
}
void fade(std::vector<float>& y, double sr, double ms = 10) {
  const auto k = std::min(y.size() / 2, static_cast<size_t>(ms * 0.001 * sr));
  for (size_t i = 0; i < k; ++i) {
    const auto g = static_cast<float>(static_cast<double>(i) / static_cast<double>(k));
    y[i] *= g, y[y.size() - 1 - i] *= g;
  }
}

// ------------------------------------------------------------ measures

double rms(const std::vector<float>& x, size_t a, size_t b) {
  b = std::min(b, x.size());
  double e = 0;
  for (size_t i = a; i < b; ++i) e += static_cast<double>(x[i]) * x[i];
  return b > a ? std::sqrt(e / static_cast<double>(b - a)) : 0.0;
}
// Level (dB) of frequency f in y[a, a + n) (Hann window).
double levelAt(const std::vector<float>& y, size_t a, size_t n, double f, double sr) {
  double re = 0, im = 0;
  const double w = 2 * kPi * f / sr;
  for (size_t i = 0; i < n; ++i) {
    const double h = 0.5 - 0.5 * std::cos(2 * kPi * (static_cast<double>(i) + 0.5) / static_cast<double>(n));
    const double v = h * y[a + i];
    re += v * std::cos(w * static_cast<double>(i)), im -= v * std::sin(w * static_cast<double>(i));
  }
  return 10 * std::log10(re * re + im * im + 1e-30);
}
double corr(const std::vector<double>& a, const std::vector<double>& b) {
  double ma = 0, mb = 0;
  for (size_t i = 0; i < a.size(); ++i) ma += a[i], mb += b[i];
  ma /= static_cast<double>(a.size()), mb /= static_cast<double>(b.size());
  double ab = 0, aa = 0, bb = 0;
  for (size_t i = 0; i < a.size(); ++i) ab += (a[i] - ma) * (b[i] - mb), aa += (a[i] - ma) * (a[i] - ma), bb += (b[i] - mb) * (b[i] - mb);
  return aa > 0 && bb > 0 ? ab / std::sqrt(aa * bb) : 0.0;
}
// Old-pitch imprint of a steady vowel shifted from f0 to f0 * p: the output's
// harmonic levels (detrended by their own 5-harmonic mean, so the formants
// do not count) against the grid of the source's harmonics, cos(2 pi k f0' / f0):
// 0 = no trace of the old pitch, toward 1 = the output's harmonics that fall on
// old harmonics stick out (heard as the old pitch ringing through).
double imprint(const std::vector<float>& y, double sr, double f0, double p, size_t a0, size_t a1) {
  const double fo = f0 * p;
  const int K = static_cast<int>(4000 / fo);
  const auto n = static_cast<size_t>(0.08 * sr);
  double sum = 0;
  int frames = 0;
  for (size_t a = a0; a + n <= a1; a += n / 2) {
    std::vector<double> h(static_cast<size_t>(K)), d(static_cast<size_t>(K)), c(static_cast<size_t>(K));
    for (int k = 1; k <= K; ++k) h[static_cast<size_t>(k - 1)] = levelAt(y, a, n, k * fo, sr);
    for (int k = 0; k < K; ++k) {
      double m = 0;
      for (int j = -2; j <= 2; ++j) m += h[static_cast<size_t>(std::clamp(k + j, 0, K - 1))];
      d[static_cast<size_t>(k)] = h[static_cast<size_t>(k)] - m / 5;
      c[static_cast<size_t>(k)] = std::cos(2 * kPi * (k + 1) * fo / f0);
    }
    sum += corr(d, c);
    ++frames;
  }
  return frames ? sum / frames : 0.0;
}
// Fine structure of the long-term spectrum (dB standard deviation around a
// 1/6-octave smoothing, 300 Hz .. 0.35 sr): a comb filter shows as ripple.
void fft(std::vector<std::complex<double>>& a) {
  const size_t n = a.size();
  for (size_t i = 1, j = 0; i < n; ++i) {
    size_t bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) std::swap(a[i], a[j]);
  }
  for (size_t len = 2; len <= n; len <<= 1) {
    const std::complex<double> wl = std::polar(1.0, -2 * kPi / static_cast<double>(len));
    for (size_t i = 0; i < n; i += len) {
      std::complex<double> w = 1;
      for (size_t j = 0; j < len / 2; ++j, w *= wl) {
        const auto u = a[i + j], v = a[i + j + len / 2] * w;
        a[i + j] = u + v, a[i + j + len / 2] = u - v;
      }
    }
  }
}
double ripple(const std::vector<float>& y, double sr, size_t a0, size_t a1) {
  const size_t n = 2048;
  const size_t bins = n / 2;
  std::vector<double> P(bins, 0.0);
  std::vector<std::complex<double>> F(n);
  for (size_t a = a0; a + n <= a1; a += n / 2) {
    for (size_t i = 0; i < n; ++i)
      F[i] = (0.5 - 0.5 * std::cos(2 * kPi * (static_cast<double>(i) + 0.5) / static_cast<double>(n))) * y[a + i];
    fft(F);
    for (size_t b = 1; b < bins; ++b) P[b] += std::norm(F[b]);
  }
  std::vector<double> L(bins);
  for (size_t b = 1; b < bins; ++b) L[b] = 10 * std::log10(P[b] + 1e-30);
  const auto b0 = static_cast<size_t>(300.0 / sr * n), b1 = static_cast<size_t>(0.35 * n);
  double s = 0;
  int k = 0;
  for (size_t b = b0; b < b1; ++b) {
    const auto w = std::max<size_t>(2, b / 12);
    double m = 0;
    int c = 0;
    for (size_t j = b - std::min(b - 1, w); j <= std::min(bins - 1, b + w); ++j) m += L[j], ++c;
    const double d = L[b] - m / c;
    s += d * d, ++k;
  }
  return std::sqrt(s / k);
}
// Energy (dB) of y[a, b) above fLo Hz: a one-pole-free measure, through a
// windowed-sinc high-pass built here.
double bandAbove(const std::vector<float>& y, double sr, double fLo, size_t a, size_t b) {
  const int h = 128;
  std::vector<double> k(2 * h + 1);
  const double fc = fLo / sr;
  for (int i = -h; i <= h; ++i) {
    const double w = 0.42 + 0.5 * std::cos(kPi * i / (h + 1)) + 0.08 * std::cos(2 * kPi * i / (h + 1));
    const double lp = i == 0 ? 2 * fc : std::sin(2 * kPi * fc * i) / (kPi * i);
    k[static_cast<size_t>(i + h)] = ((i == 0 ? 1.0 : 0.0) - lp) * w;
  }
  double e = 0;
  for (size_t i = std::max<size_t>(a, h); i < std::min(b, y.size() - h); ++i) {
    double v = 0;
    for (int j = -h; j <= h; ++j) v += k[static_cast<size_t>(j + h)] * y[static_cast<size_t>(static_cast<int64_t>(i) + j)];
    e += v * v;
  }
  return 10 * std::log10(e + 1e-30);
}
// Pitch (Hz) of y[a, a + n): autocorrelation peak near the expected period,
// refined by a parabola.
double pitchOf(const std::vector<float>& y, double sr, size_t a, size_t n, double expect) {
  const double T = sr / expect;
  const auto lo = static_cast<int>(T * 0.8), hi = static_cast<int>(T * 1.25) + 1;
  auto ac = [&](int l) {
    double s = 0, e1 = 0, e2 = 0;
    for (size_t i = a; i < a + n; ++i) {
      const double u = y[i], v = y[i + static_cast<size_t>(l)];
      s += u * v, e1 += u * u, e2 += v * v;
    }
    return s / std::sqrt(e1 * e2 + 1e-30);
  };
  int best = lo;
  double bv = -2;
  for (int l = lo; l <= hi; ++l)
    if (const double v = ac(l); v > bv) bv = v, best = l;
  const double ym = ac(best - 1), y0 = ac(best), yp = ac(best + 1);
  const double den = ym - 2 * y0 + yp;
  const double off = den < 0 ? 0.5 * (ym - yp) / den : 0.0;
  return sr / (best + off);
}
// Worst click: the largest second difference in any 1 ms block over the
// median block RMS of the second difference (a smooth vowel stays low).
double clickScore(const std::vector<float>& y, double sr, size_t a, size_t b) {
  const auto blk = static_cast<size_t>(0.001 * sr);
  std::vector<double> r, pk;
  for (size_t i = a; i + blk + 2 <= b; i += blk) {
    double e = 0, p = 0;
    for (size_t j = i; j < i + blk; ++j) {
      const double d = static_cast<double>(y[j + 2]) - 2.0 * y[j + 1] + y[j];
      e += d * d, p = std::max(p, std::fabs(d));
    }
    r.push_back(std::sqrt(e / static_cast<double>(blk)));
    pk.push_back(p);
  }
  std::vector<double> s = r;
  std::nth_element(s.begin(), s.begin() + static_cast<std::ptrdiff_t>(s.size() / 2), s.end());
  const double med = s[s.size() / 2];
  double worst = 0;
  for (double p : pk) worst = std::max(worst, p / (med + 1e-30));
  return worst;
}

// ------------------------------------------------------------ drivers

std::vector<std::vector<float>> offlineN(const std::vector<std::vector<float>>& x, double sr, stretch::Mode m, double pitch, double timeFactor,
                                         double formant = 0) {
  stretch::Settings s;
  s.mode = m, s.pitch = pitch, s.formant = formant;
  const auto n = static_cast<int64_t>(x[0].size());
  const auto outLen = static_cast<int64_t>(std::llround(static_cast<double>(n) * timeFactor));
  std::vector<std::vector<float>> y(x.size(), std::vector<float>(static_cast<size_t>(outLen)));
  std::vector<const float*> in;
  std::vector<float*> out;
  for (size_t c = 0; c < x.size(); ++c) in.push_back(x[c].data()), out.push_back(y[c].data());
  stretch::process(in.data(), static_cast<int>(x.size()), n, out.data(), outLen, sr, s);
  return y;
}
std::vector<float> offline(const std::vector<float>& x, double sr, stretch::Mode m, double pitch, double timeFactor, double formant = 0) {
  return offlineN({x}, sr, m, pitch, timeFactor, formant)[0];
}

struct Src {
  const std::vector<float>* x = nullptr;
  size_t pos = 0;
  static int read(void* ctx, float* const* d, int n, bool) {
    auto* s = static_cast<Src*>(ctx);
    for (int i = 0; i < n; ++i) {
      const float v = s->pos < s->x->size() ? (*s->x)[s->pos] : 0.f;
      d[0][i] = d[1][i] = v;
      ++s->pos;
    }
    return -1;
  }
};
// Realtime Voice voice: `frames` output frames in blocks of `block`; pitchOf(block index) may vary the pitch.
template <class PitchAt>
std::vector<float> realtimeVar(const std::vector<float>& x, double sr, double speed, PitchAt pitchAt, size_t frames, int block,
                               long* allocs = nullptr) {
  stretch::Voice v;
  v.prepare(1, sr, stretch::Mode::Voice, 4.0, 4.0);
  Src src{&x, 0};
  std::vector<float> L(frames), R(frames);
  CountScope cs;
  v.start(&Src::read, &src, speed, pitchAt(0));
  int k = 0;
  for (size_t at = 0; at < frames; ++k) {
    const int n = static_cast<int>(std::min<size_t>(static_cast<size_t>(block), frames - at));
    float* o[2] = {L.data() + at, R.data() + at};
    v.render(o, n, speed, pitchAt(k), 0, &Src::read, &src);
    at += static_cast<size_t>(n);
  }
  if (allocs) *allocs = cs.n();
  return L;
}
std::vector<float> realtime(const std::vector<float>& x, double sr, double speed, double pitch, size_t frames, int block, long* allocs = nullptr) {
  return realtimeVar(x, sr, speed, [pitch](int) { return pitch; }, frames, block, allocs);
}

// The tracker run over a whole signal (with a tail of silence so everything is decided).
struct Track {
  gl::pitch::VoiceTrack t;
  Track(const std::vector<float>& x, double sr) {
    t.configure(sr, 1 << 21);
    for (float v : x) t.push(v);
    for (int i = 0; i < static_cast<int>(sr); ++i) t.push(0.0f);
  }
};

}  // namespace

int main() {
  // ============================================================ old pitch imprint
  {
    const double sr = 48000;
    VowelOpts o;
    o.f0 = 180;
    const auto x = vowel(sr, static_cast<size_t>(2.0 * sr), o);
    const auto a0 = static_cast<size_t>(0.4 * sr), a1 = static_cast<size_t>(1.6 * sr);
    double worst = 0;
    for (double semis : {-12.0, -7.0, -5.0, 7.0}) {
      const double p = std::pow(2.0, semis / 12);
      const auto y = offline(x, sr, stretch::Mode::Voice, p, 1.0);
      const double im = imprint(y, sr, o.f0, p, a0, a1);
      std::printf("# Voice imprint %+g st: %+.3f\n", semis, im);
      worst = std::max(worst, im);
    }
    expect(worst < 0.2, "Voice: no comb at the old pitch's harmonics after a shift (-12, -7, -5, +7 st: imprint under 0.2)");
    double fw = 0;
    for (double semis : {-7.0, 7.0}) {
      const double p = std::pow(2.0, semis / 12);
      const auto y = offline(x, sr, stretch::Mode::Formant, p, 1.0);
      const double im = imprint(y, sr, o.f0, p, a0, a1);
      std::printf("# Formant imprint %+g st: %+.3f\n", semis, im);
      fw = std::max(fw, im);
    }
    // (With the engine's own pitch guess this steady vowel measured about 0.98.)
    expect(fw < 0.5, "Formant: the formant analysis follows the voice's pitch (imprint under 0.5 at -7 and +7 st)");
  }

  // ============================================================ noise comb
  {
    const double sr = 48000;
    const auto x = noise(static_cast<size_t>(1.5 * sr));
    const double r0 = ripple(x, sr, 0, x.size());
    double worst = 0;
    for (double tf : {1.25, 2.0, 4.0}) {
      const auto y = offline(x, sr, stretch::Mode::Voice, 1.0, tf);
      const double r = ripple(y, sr, static_cast<size_t>(0.1 * sr), y.size() - static_cast<size_t>(0.1 * sr));
      std::printf("# noise x%g: spectral ripple %.2f dB (source %.2f dB)\n", tf, r, r0);
      worst = std::max(worst, r - r0);
    }
    const auto yd = offline(x, sr, stretch::Mode::Voice, std::pow(2.0, -5.0 / 12), 1.25);
    const double rd = ripple(yd, sr, static_cast<size_t>(0.1 * sr), yd.size() - static_cast<size_t>(0.1 * sr));
    worst = std::max(worst, rd - r0);
    expect(worst < 0.6, "Voice: noise and breaths stretched x1.25 .. x4 (and -5 st x1.25) without a comb filter (ripple within 0.6 dB of the source)");
    // Loudness kept (power-complementary joins for noise).
    const auto y2 = offline(x, sr, stretch::Mode::Voice, 1.0, 2.0);
    const double dl = 20 * std::log10(rms(y2, 4800, y2.size() - 4800) / rms(x, 0, x.size()));
    std::snprintf(msg, sizeof msg, "Voice: noise stretched x2 keeps its level (%.2f dB)", dl);
    expect(std::fabs(dl) < 0.5, msg);
  }

  // ============================================================ tracker
  {
    const double sr = 44100;
    // A sung vowel with vibrato and breath noise at -26 dB: voiced throughout, one pitch.
    VowelOpts o;
    o.f0 = 140, o.vibrato = 0.03;
    auto x = vowel(sr, static_cast<size_t>(2.0 * sr), o);
    const auto nz = noise(x.size(), 0x5eedu, 1.0);
    const double g = rms(x, 0, x.size()) * std::pow(10.0, -26.0 / 20) / rms(nz, 0, nz.size());
    for (size_t i = 0; i < x.size(); ++i) x[i] += static_cast<float>(g * nz[i]);
    Track tr(x, sr);
    int flips = 0, unv = 0, cnt = 0;
    bool last = true;
    for (double t = 0.1; t < 1.9; t += 0.002) {
      const bool v = tr.t.voicedAt(t * sr);
      flips += v != last, last = v, unv += !v, ++cnt;
    }
    std::snprintf(msg, sizeof msg, "tracker: a sung vowel with vibrato and breath noise stays voiced (%d flips, %.1f %% unvoiced)", flips, 100.0 * unv / cnt);
    expect(flips == 0 && unv == 0, msg);
    int bad = 0, n = 0, tonal = 0;
    for (int64_t i = tr.t.firstMark(); i < tr.t.markCount(); ++i) {
      const auto& m = tr.t.mark(i);
      if (m.voiced <= 0 || m.pos < 0.1 * sr || m.pos > 1.9 * sr) continue;
      const double t = m.pos / sr;
      const double f = o.f0 * (1 + o.vibrato * std::sin(2 * kPi * o.rate * t));
      ++n;
      bad += std::fabs(std::log2(sr / m.period / f)) > 0.05;
      tonal += m.tonal > 0.05f;
    }
    std::snprintf(msg, sizeof msg, "tracker: one mark per period, within 3.5 %% of the true pitch through the vibrato (%d of %d off)", bad, n);
    expect(n > 200 && bad == 0, msg);
    std::snprintf(msg, sizeof msg, "tracker: a voice is never taken for a pure tone (%d of %d marks)", tonal, n);
    expect(tonal == 0, msg);
    // Octave trap: no fundamental, a strong second harmonic.
    VowelOpts q;
    q.f0 = 110, q.noFundamental = true;
    const auto xq = vowel(sr, static_cast<size_t>(1.5 * sr), q);
    Track tq(xq, sr);
    int oct = 0, nq = 0;
    for (int64_t i = tq.t.firstMark(); i < tq.t.markCount(); ++i) {
      const auto& m = tq.t.mark(i);
      if (m.voiced <= 0 || m.pos < 0.1 * sr || m.pos > 1.4 * sr) continue;
      ++nq;
      oct += std::fabs(std::log2(sr / m.period / q.f0)) > 0.3;
    }
    std::snprintf(msg, sizeof msg, "tracker: a voice without its fundamental keeps its pitch, no octave jumps (%d of %d marks off)", oct, nq);
    expect(nq > 100 && oct == 0, msg);
    // Pure tones are transposed (tonal), in range and above it.
    for (double f : {220.0, 1500.0}) {
      Track ts(sine(sr, f, static_cast<size_t>(1.0 * sr)), sr);
      int lowTonal = 0, ns = 0;
      for (int64_t i = ts.t.firstMark(); i < ts.t.markCount(); ++i) {
        const auto& m = ts.t.mark(i);
        if (m.voiced <= 0 || m.pos < 0.1 * sr || m.pos > 0.9 * sr) continue;
        ++ns, lowTonal += m.tonal < 0.9f;
      }
      std::snprintf(msg, sizeof msg, "tracker: a %g Hz sine is a pure tone on every mark (%d of %d not)", f, lowTonal, ns);
      expect(ns > 10 && lowTonal == 0, msg);
    }
  }

  // ============================================================ the top octave at -12
  {
    const double sr = 48000;
    // Vowels with fricatives ("s") between them: energy up to 20 kHz.
    VowelOpts o;
    o.f0 = 200;
    const auto v = vowel(sr, static_cast<size_t>(1.6 * sr), o);
    auto hiss = noise(v.size(), 0xabcdu, 0.2);
    for (size_t i = hiss.size() - 1; i >= 2; --i) hiss[i] = 0.25f * (hiss[i] - 2 * hiss[i - 1] + hiss[i - 2]);  // tilted up
    std::vector<float> x(v.size());
    const auto seg = static_cast<size_t>(0.2 * sr);
    for (size_t i = 0; i < x.size(); ++i) {
      const bool fric = (i / seg) % 2 == 1;
      x[i] = fric ? hiss[i] : v[i];
    }
    const auto y = offline(x, sr, stretch::Mode::Voice, 0.5, 1.0);
    const double ei = bandAbove(x, sr, 12000, 0, x.size()), eo = bandAbove(y, sr, 12000, 0, y.size());
    std::snprintf(msg, sizeof msg, "Voice -12 st: the 12 .. 24 kHz band of fricatives survives (%.1f dB against the input)", eo - ei);
    expect(eo - ei > -6, msg);
  }

  // ============================================================ realtime
  {
    const double sr = 48000;
    VowelOpts o;
    o.f0 = 100, o.F1 = 500;
    auto x = vowel(sr, static_cast<size_t>(1.5 * sr), o);
    fade(x, sr);
    const auto frames = static_cast<size_t>(1.3 * sr);
    const auto rt = realtime(x, sr, 1.0, 2.0, frames, 256);
    const auto off = offline(x, sr, stretch::Mode::Voice, 2.0, 1.0);
    const double cr = clickScore(rt, sr, static_cast<size_t>(0.1 * sr), frames - static_cast<size_t>(0.1 * sr));
    const double co = clickScore(off, sr, static_cast<size_t>(0.1 * sr), frames - static_cast<size_t>(0.1 * sr));
    std::snprintf(msg, sizeof msg, "realtime Voice +12 st: whole grains, no clicks at block edges (worst %.1f, offline %.1f)", cr, co);
    expect(cr < 1.5 * co + 1.0, msg);
    // Unison: the voice plays unchanged (grains land on the input's own periods).
    VowelOpts u;
    u.f0 = 160, u.vibrato = 0.02;
    auto xu = vowel(sr, static_cast<size_t>(1.2 * sr), u);
    fade(xu, sr);
    std::vector<float> xs(static_cast<size_t>(0.3 * sr), 0.0f);
    xs.insert(xs.end(), xu.begin(), xu.end());
    xs.resize(xs.size() + static_cast<size_t>(0.2 * sr), 0.0f);
    const auto yu = realtime(xs, sr, 1.0, 1.0, xs.size(), 128);
    double e = 0, s = 0;
    for (size_t i = 0; i < xs.size(); ++i) e += (yu[i] - xs[i]) * (yu[i] - xs[i]), s += xs[i] * xs[i];
    std::snprintf(msg, sizeof msg, "realtime Voice at unison: the voice comes out unchanged, from the first block (error %.1f dB)", 10 * std::log10(e / s + 1e-30));
    expect(10 * std::log10(e / s + 1e-30) < -50, msg);
    // Block-size invariance and no allocation with the pitch moving.
    long al = 0;
    auto glide = [](int k) { return 0.7 + 0.002 * (k % 300); };
    const auto y1 = realtimeVar(x, sr, 1.6, glide, frames / 2, 37, &al);
    const auto y2 = realtimeVar(x, sr, 1.6, glide, frames / 2, 37);
    float d = 0;
    for (size_t i = 0; i < y1.size(); ++i) d = std::max(d, std::fabs(y1[i] - y2[i]));
    expect(d == 0.0f, "realtime Voice: the same input renders the same output");
    const auto b1 = realtime(x, sr, 1.6, 0.7, frames / 2, 37);
    const auto b2 = realtime(x, sr, 1.6, 0.7, frames / 2, 1024);
    float db = 0;
    for (size_t i = 0; i < b1.size(); ++i) db = std::max(db, std::fabs(b1[i] - b2[i]));
    std::snprintf(msg, sizeof msg, "realtime Voice -6 st x1.6: the same output for blocks of 37 and 1024 frames (diff %.2g)", static_cast<double>(db));
    expect(db < 1e-6f, msg);
    std::snprintf(msg, sizeof msg, "realtime Voice: a gliding pitch renders without allocation (%ld)", al);
    expect(al == 0, msg);
  }

  // ============================================================ rates, channels, determinism
  {
    bool tune = true;
    for (double sr : {44100.0, 48000.0, 96000.0}) {
      VowelOpts o;
      o.f0 = 150;
      const auto x = vowel(sr, static_cast<size_t>(1.2 * sr), o);
      for (double semis : {-7.0, 5.0}) {
        const double p = std::pow(2.0, semis / 12);
        const auto y = offline(x, sr, stretch::Mode::Voice, p, 1.0);
        const double f = pitchOf(y, sr, static_cast<size_t>(0.3 * sr), static_cast<size_t>(0.5 * sr), o.f0 * p);
        const double cents = 1200 * std::log2(f / (o.f0 * p));
        std::printf("# %g Hz, %+g st: %.2f cents\n", sr, semis, cents);
        tune = tune && std::fabs(cents) < 2.0;
      }
    }
    expect(tune, "Voice at 44.1, 48 and 96 kHz: shifted vowels in tune within 2 cents");
    const double sr = 48000;
    VowelOpts o;
    o.f0 = 170;
    const auto x = vowel(sr, static_cast<size_t>(1.0 * sr), o);
    std::vector<float> half(x.size());
    for (size_t i = 0; i < x.size(); ++i) half[i] = 0.5f * x[i];
    const auto m = offlineN({x}, sr, stretch::Mode::Voice, 0.8, 1.3);
    const auto st = offlineN({x, half}, sr, stretch::Mode::Voice, 0.8, 1.3);
    const auto again = offlineN({x, half}, sr, stretch::Mode::Voice, 0.8, 1.3);
    float dl = 0, dr = 0, dd = 0;
    for (size_t i = 0; i < m[0].size(); ++i) {
      dl = std::max(dl, std::fabs(st[0][i] - m[0][i]));
      dr = std::max(dr, std::fabs(st[1][i] - 0.5f * st[0][i]));
      dd = std::max({dd, std::fabs(st[0][i] - again[0][i]), std::fabs(st[1][i] - again[1][i])});
    }
    std::snprintf(msg, sizeof msg, "Voice stereo: both channels cut at the same marks (right = left / 2 within %.2g)", static_cast<double>(dr));
    expect(dr < 1e-5f, msg);
    expect(dd == 0.0f, "Voice offline: renders are deterministic");
    std::printf("# stereo left against mono render: %.3g\n", static_cast<double>(dl));
  }
  return finish();
}
