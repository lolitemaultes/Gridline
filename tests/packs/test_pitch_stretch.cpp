// Gridline: J-153 one smooth stretch for all audio (engine/TimeStretch,
// engine/AudioClips, the sampler's stretch voices). Objective checks on
// synthetic material that every transform stays smooth:
//   hits       a bass under kick drums keeps its level through every hit
//              (offline renders, sampler voices), at 44.1 / 48 / 96 kHz,
//              mono and stereo; drums keep every hit (none lost or doubled)
//              at 0.25x .. 8x; a plucked note decays without a swell;
//   smooth     steady tones and a bass at 0.5x .. 4x and -12 .. +12 st have
//              no hop-rate clicks, offline and in realtime;
//   time maps  tempo-mapped clips keep their hits where the map puts them;
//   realtime   pitch changes ramp within a render call (no zipper), the
//              voice allocates nothing and is the same for any block size,
//              a note started inside a sample starts at full level, the
//              stretched tail of a note fades out;
//   clips      playback switching between the offline render and the
//              realtime stretch crossfades;
//   general    offline renders are deterministic, stereo correlation kept,
//              a hit on the first frame stays there;
//   rubber     J-157: the Rubber Band choice, offline and realtime: exact
//              length, aligned with the source (envelope cross-correlation
//              within 1 ms), level kept, no clicks, the same realtime output
//              for any block size, no allocation, seeks primed with pre-roll,
//              +-12 semitones in tune, 44.1 / 48 / 96 kHz, tempo maps, clip
//              playback and sampler notes; CPU cost printed.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <new>
#include <string>
#include <vector>
#include "../../src/core/PluginInfo.h"
#include "../../src/core/PluginParams.h"
#include "../../src/core/WavWriter.h"
#include "../../src/engine/Generators.h"
#include "../../src/engine/Program.h"
#include "../../src/engine/Sample.h"
#include "../../src/engine/Slicing.h"
#include "../../src/engine/TimeStretch.h"
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
namespace fs = std::filesystem;

namespace {

constexpr double kPi = 3.14159265358979323846;
using Sig = std::vector<float>;

struct CountScope {
  long start;
  CountScope() : start(gAllocs.load()) { gCount = true; }
  ~CountScope() { gCount = false; }
  long n() const { return gAllocs.load() - start; }
};

// ------------------------------------------------------------ signals

// A sawtooth bass (55 Hz, 40 harmonics) with a kick drum every 0.5 s from 0.25 s.
Sig bassKick(double sr, double sec, std::vector<double>* hits = nullptr) {
  Sig x(static_cast<size_t>(sec * sr), 0.f);
  for (size_t i = 0; i < x.size(); ++i) {
    double v = 0;
    const double t = static_cast<double>(i) / sr;
    for (int k = 1; k <= 40; ++k) v += std::sin(2 * kPi * k * 55 * t) / k;
    x[i] = static_cast<float>(0.25 * v * 2 / kPi);
  }
  uint32_t rng = 777;
  for (double h = 0.25; h < sec - 0.3; h += 0.5) {
    if (hits) hits->push_back(h);
    const auto a = static_cast<size_t>(h * sr);
    for (size_t i = a; i < x.size() && i < a + static_cast<size_t>(0.4 * sr); ++i) {
      const double t = static_cast<double>(i - a) / sr;
      const double f = 50 + 120 * std::exp(-t / 0.02);
      // Phase of the falling chirp (integral of f).
      const double ph = 2 * kPi * (50 * t + 120 * 0.02 * (1 - std::exp(-t / 0.02)));
      double v = std::sin(ph) * std::exp(-t / 0.12);
      (void)f;
      if (i - a < 200) {
        rng = rng * 1664525u + 1013904223u;
        v += ((static_cast<double>(rng >> 8) / 8388608.0) - 1.0) * std::exp(-static_cast<double>(i - a) / 40.0) * 0.5;
      }
      x[i] += static_cast<float>(0.5 * v);
    }
  }
  return x;
}

// Kick / noise hits every 0.25 s from 0.05 s, 0.2 s each, silence between (as in test_j87).
std::vector<double> drumTimes(double sec) {
  std::vector<double> t;
  for (double x = 0.05; x < sec - 0.2; x += 0.25) t.push_back(x);
  return t;
}
Sig drums(double sr, double sec) {
  Sig x(static_cast<size_t>(sec * sr), 0.f);
  uint32_t rng = 12345;
  int k = 0;
  for (double t0 : drumTimes(sec)) {
    const bool kick = k++ % 2 == 0;
    for (size_t i = static_cast<size_t>(t0 * sr); i < x.size() && i < static_cast<size_t>((t0 + 0.2) * sr); ++i) {
      const double t = static_cast<double>(i) / sr - t0;
      rng = rng * 1664525u + 1013904223u;
      const double n = (static_cast<double>(rng >> 8) / 8388608.0) - 1.0;
      x[i] += static_cast<float>(kick ? 0.9 * std::exp(-t / 0.035) * std::sin(2 * kPi * (50 * t + 3.3 * (1 - std::exp(-t / 0.03))))
                                      : 0.5 * std::exp(-t / 0.02) * n);
    }
  }
  return x;
}

// Plucked notes (sharp onset, harmonic decay) every second from 0.1 s, 0.9 s each.
Sig plucks(double sr, double sec, std::vector<double>* at = nullptr) {
  Sig x(static_cast<size_t>(sec * sr), 0.f);
  const double notes[] = {220, 277.18, 329.63, 440, 196, 246.94, 293.66, 392};
  int k = 0;
  for (double h = 0.1; h < sec - 0.9; h += 1.0, ++k) {
    if (at) at->push_back(h);
    const double f0 = notes[k % 8];
    const auto a = static_cast<size_t>(h * sr);
    for (size_t i = a; i < x.size() && i < a + static_cast<size_t>(0.9 * sr); ++i) {
      const double t = static_cast<double>(i - a) / sr;
      double v = 0;
      for (int m = 1; m < 12; ++m) v += std::sin(2 * kPi * m * f0 * t) * std::exp(-t * (2 + 1.5 * m)) / m;
      x[i] += static_cast<float>(0.25 * v);
    }
  }
  return x;
}

Sig sine(double f, size_t n, double sr, double a = 0.5) {
  Sig y(n);
  for (size_t i = 0; i < n; ++i) y[i] = static_cast<float>(a * std::sin(2 * kPi * f * static_cast<double>(i) / sr));
  return y;
}

// ------------------------------------------------------------ engines

std::vector<Sig> offline(const std::vector<Sig>& x, double pitch, double tf, double sr) {
  stretch::Settings s;
  s.mode = stretch::kStretch, s.pitch = pitch;
  const auto n = static_cast<int64_t>(x[0].size());
  const auto outLen = static_cast<int64_t>(std::llround(static_cast<double>(n) * tf));
  std::vector<Sig> y(x.size(), Sig(static_cast<size_t>(outLen)));
  std::vector<const float*> in;
  std::vector<float*> out;
  for (size_t c = 0; c < x.size(); ++c) in.push_back(x[c].data()), out.push_back(y[c].data());
  stretch::process(in.data(), static_cast<int>(x.size()), n, out.data(), outLen, sr, s);
  return y;
}
Sig offline1(const Sig& x, double pitch, double tf, double sr) { return offline({x, x}, pitch, tf, sr)[0]; }

// J-157: the same through Rubber Band.
std::vector<Sig> offlineRb(const std::vector<Sig>& x, double pitch, double tf, double sr) {
  stretch::Settings s;
  s.mode = stretch::Mode::RubberBand, s.pitch = pitch;
  const auto n = static_cast<int64_t>(x[0].size());
  const auto outLen = static_cast<int64_t>(std::llround(static_cast<double>(n) * tf));
  std::vector<Sig> y(x.size(), Sig(static_cast<size_t>(outLen)));
  std::vector<const float*> in;
  std::vector<float*> out;
  for (size_t c = 0; c < x.size(); ++c) in.push_back(x[c].data()), out.push_back(y[c].data());
  stretch::process(in.data(), static_cast<int>(x.size()), n, out.data(), outLen, sr, s);
  return y;
}
Sig offlineRb1(const Sig& x, double pitch, double tf, double sr) { return offlineRb({x, x}, pitch, tf, sr)[0]; }

std::string writeTmp(const std::string& name, const Sig& l, double sr) {
  AudioBuffer b(2, l.size());
  for (size_t i = 0; i < l.size(); ++i) b.at(0, i) = l[i], b.at(1, i) = l[i];
  const std::string path = (fs::temp_directory_path() / name).string();
  writeWav(path, b, static_cast<int>(sr), 24);
  sampleCache().clear();
  return path;
}

stretch::Mode gSamplerMode = stretch::kStretch;  // the time mode sampler() uses
// One sampler note (the stretch, time stretch `tf`) over `frames`, in blocks of `block`.
struct SamplerRun {
  PluginState st;
  ChannelAux aux;
};
Sig sampler(const std::string& path, double tf, size_t frames, double sr, int block = 256, float start = 0.f, float pitchSemis = 0.f,
            int note = 60) {
  PluginState st = defaultPluginState("gl.sampler");
  st.data = path;
  st.params[pp::SmpStretchMode] = static_cast<float>(gSamplerMode);
  st.params[pp::SmpTimeStretch] = static_cast<float>(tf);
  st.params[pp::SmpStart] = start;
  st.params[pp::SmpPitchShift] = pitchSemis * 100.f;
  ChannelAux aux;
  prepareChannelAux(st, aux);
  auto gen = createGenerator("gl.sampler");
  gen->prepareHost(st, sr);
  ProcessContext ctx;
  ctx.sampleRate = sr;
  Sig L(frames, 0.f), R(frames, 0.f);
  gen->noteOn(1, note, 1.0f, 0);
  for (size_t i = 0; i < frames; i += static_cast<size_t>(block)) {
    const int n = static_cast<int>(std::min<size_t>(static_cast<size_t>(block), frames - i));
    gen->process(st, aux, ctx, L.data() + i, R.data() + i, n);
  }
  return L;
}

// ------------------------------------------------------------ analysis

double rms(const Sig& x, size_t a, size_t b) {
  double e = 0;
  b = std::min(b, x.size());
  for (size_t i = a; i < b; ++i) e += static_cast<double>(x[i]) * x[i];
  return b > a ? std::sqrt(e / static_cast<double>(b - a)) : 0.0;
}
bool finite(const Sig& x) {
  for (float v : x)
    if (!std::isfinite(v)) return false;
  return true;
}
// The bass's upper harmonics (300 .. 2000 Hz, above the kick's body): two
// RBJ high-pass and two low-pass biquads.
Sig bassBand(const Sig& x, double sr) {
  struct Bq {
    double b0, b1, b2, a1, a2, z1 = 0, z2 = 0;
    Bq(bool hp, double f, double sr) {
      const double w = 2 * kPi * f / sr, c = std::cos(w), al = std::sin(w) / (2 * 0.7071), a0 = 1 + al;
      b0 = (hp ? (1 + c) / 2 : (1 - c) / 2) / a0, b1 = (hp ? -(1 + c) : 1 - c) / a0, b2 = b0, a1 = -2 * c / a0, a2 = (1 - al) / a0;
    }
    double operator()(double v) {
      const double y = b0 * v + z1;
      z1 = b1 * v - a1 * y + z2, z2 = b2 * v - a2 * y;
      return y;
    }
  };
  Bq f[4] = {Bq(true, 300, sr), Bq(true, 300, sr), Bq(false, 2000, sr), Bq(false, 2000, sr)};
  Sig y(x.size());
  for (size_t i = 0; i < x.size(); ++i) y[i] = static_cast<float>(f[3](f[2](f[1](f[0](x[i])))));
  return y;
}
// The bass level around each hit (at hit * tf): the lowest 25 ms level within
// +-80 ms against the median level within +-0.25 * tf s. Mean and worst (dB).
struct Dip {
  double mean = 0, worst = 0;
};
Dip bassDips(const Sig& y, const std::vector<double>& hits, double tf, double sr) {
  const auto w = static_cast<size_t>(0.025 * sr), hop = static_cast<size_t>(0.005 * sr);
  const Sig yb = bassBand(y, sr);
  std::vector<double> e;
  for (size_t a = 0; a + w <= yb.size(); a += hop) e.push_back(20 * std::log10(rms(yb, a, a + w) + 1e-10));
  auto at = [&](double t) { return static_cast<long>(std::lround((t * sr - 0.5 * static_cast<double>(w)) / static_cast<double>(hop))); };
  Dip d;
  int n = 0;
  for (double h : hits) {
    const double c = h * tf;
    const long a = at(c - 0.08), b = at(c + 0.08), ra = at(c - 0.25 * tf), rb = at(c + 0.25 * tf);
    if (ra < 0 || rb >= static_cast<long>(e.size())) continue;
    std::vector<double> ref(e.begin() + ra, e.begin() + rb);
    std::nth_element(ref.begin(), ref.begin() + static_cast<long>(ref.size() / 2), ref.end());
    double lo = 1e9;
    for (long i = a; i <= b; ++i) lo = std::min(lo, e[static_cast<size_t>(i)]);
    const double dip = lo - ref[ref.size() / 2];
    d.mean += dip, d.worst = std::min(d.worst, dip), ++n;
  }
  if (n) d.mean /= n;
  return d;
}
// Drum hits: mean pre-echo (dB, loudest 0.5 ms in the 30 .. 3 ms before each
// hit against its peak) and mean 10 -> 90 % rise time (ms), as in test_j87.
struct Hits {
  double preDb = 0, riseMs = 0;
};
Hits hits(const Sig& x, const std::vector<double>& times, double sr) {
  const auto w = static_cast<size_t>(0.0005 * sr);
  auto env = [&](long i) {
    double e = 0;
    for (size_t j = 0; j < w; ++j) {
      const size_t k = static_cast<size_t>(i) + j;
      if (k < x.size()) e += static_cast<double>(x[k]) * x[k];
    }
    return std::sqrt(e / static_cast<double>(w));
  };
  Hits h;
  int n = 0;
  for (double t : times) {
    const long c = std::lround(t * sr);
    if (c < static_cast<long>(0.04 * sr) || c + static_cast<long>(0.03 * sr) >= static_cast<long>(x.size())) continue;
    double pk = 0;
    long at = c;
    for (long i = c - static_cast<long>(0.004 * sr); i < c + static_cast<long>(0.03 * sr); ++i)
      if (const double e = env(i); e > pk) pk = e, at = i;
    long i90 = at;
    while (i90 > c - static_cast<long>(0.03 * sr) && env(i90) > 0.9 * pk) --i90;
    long i10 = i90;
    while (i10 > c - static_cast<long>(0.06 * sr) && env(i10) > 0.1 * pk) --i10;
    double pre = 0;
    for (long i = c - static_cast<long>(0.03 * sr); i < c - static_cast<long>(0.003 * sr); ++i) pre = std::max(pre, env(i));
    h.preDb += 20 * std::log10(pre / pk + 1e-6);
    h.riseMs += static_cast<double>(i90 - i10) / sr * 1000;
    ++n;
  }
  if (n) h.preDb /= n, h.riseMs /= n;
  return h;
}
// A plucked note's decay after its attack: the largest rise (dB) of the 10 ms
// level over the lowest level since the peak, within 120 ms (a dip then a
// swell where the attack hands over to the stretched decay).
double handoverSwell(const Sig& y, const std::vector<double>& at, double tf, double sr) {
  const auto w = static_cast<size_t>(0.010 * sr);
  double worst = 0;
  for (double t : at) {
    const auto c = static_cast<size_t>(t * tf * sr);
    size_t pkAt = c;
    double pk = 0;
    for (size_t i = c > w ? c - w : 0; i < c + static_cast<size_t>(0.03 * sr); i += w / 4)
      if (const double r = rms(y, i, i + w); r > pk) pk = r, pkAt = i;
    double lo = pk;
    for (size_t i = pkAt; i < pkAt + static_cast<size_t>(0.12 * sr); i += w / 4) {
      const double r = rms(y, i, i + w);
      lo = std::min(lo, r);
      if (lo > 1e-6) worst = std::max(worst, 20 * std::log10(r / lo));
    }
  }
  return worst;
}
// Spectrum power (Hann) of x[a, a + n), n a power of two.
std::vector<double> power(const Sig& x, size_t a, size_t n) {
  std::vector<std::complex<double>> b(n);
  for (size_t i = 0; i < n; ++i) b[i] = x[a + i] * (0.5 - 0.5 * std::cos(2 * kPi * static_cast<double>(i) / static_cast<double>(n)));
  for (size_t i = 1, j = 0; i < n; ++i) {
    size_t bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) std::swap(b[i], b[j]);
  }
  for (size_t len = 2; len <= n; len <<= 1) {
    const double ang = -2 * kPi / static_cast<double>(len);
    const std::complex<double> wl(std::cos(ang), std::sin(ang));
    for (size_t i = 0; i < n; i += len) {
      std::complex<double> w(1);
      for (size_t j = 0; j < len / 2; ++j) {
        const auto u = b[i + j], v = b[i + j + len / 2] * w;
        b[i + j] = u + v, b[i + j + len / 2] = u - v;
        w *= wl;
      }
    }
  }
  std::vector<double> p(n / 2);
  for (size_t k = 0; k < n / 2; ++k) p[k] = std::norm(b[k]);
  return p;
}
double maxCurve(const Sig& x, size_t a, size_t b) {
  double m = 0;
  for (size_t i = std::max<size_t>(a, 2); i < std::min(b, x.size()); ++i) m = std::max(m, static_cast<double>(std::fabs(x[i] - 2 * x[i - 1] + x[i - 2])));
  return m;
}
double corr(const Sig& a, const Sig& b, size_t from, size_t to) {
  double ab = 0, aa = 0, bb = 0;
  for (size_t i = from; i < to && i < a.size(); ++i) ab += static_cast<double>(a[i]) * b[i], aa += static_cast<double>(a[i]) * a[i], bb += static_cast<double>(b[i]) * b[i];
  return ab / std::sqrt(aa * bb + 1e-30);
}

// Hop-rate clicks: the largest third difference over [a, b) against the
// 99.9th percentile of it (a clean steady tone stays near 1; a click at a
// block boundary of the engine stands out).
double clickRatio(const Sig& x, size_t a, size_t b) {
  std::vector<double> d;
  for (size_t i = std::max<size_t>(a, 3); i < std::min(b, x.size()); ++i)
    d.push_back(std::fabs(static_cast<double>(x[i]) - 3.0 * x[i - 1] + 3.0 * x[i - 2] - x[i - 3]));
  if (d.size() < 1000) return 0;
  const double mx = *std::max_element(d.begin(), d.end());
  std::nth_element(d.begin(), d.begin() + static_cast<long>(d.size() * 999 / 1000), d.end());
  return mx / (d[d.size() * 999 / 1000] + 1e-12);
}
struct Src {
  const Sig* x;
  size_t pos = 0;
  static void read(void* ctx, float* const* d, int n) {
    auto* s = static_cast<Src*>(ctx);
    for (int i = 0; i < n; ++i, ++s->pos) d[0][i] = d[1][i] = s->pos < s->x->size() ? (*s->x)[s->pos] : 0.f;
  }
};
// A realtime voice over x: `frames` frames at speed / pitch, in blocks of `block`.
Sig voice(const Sig& x, double speed, double pitch, size_t frames, double sr, int block, long* allocs = nullptr,
          stretch::Engine engine = stretch::Engine::Stretch) {
  stretch::Voice v;
  v.prepare(2, sr, 8.0, 8.0, false, engine);
  Src src{&x};
  Sig L(frames), R(frames);
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
// J-157: the lag (ms) of y's level envelope against x's read on y's time line
// at 1 / tf (10 ms RMS windows every 0.5 ms, mean removed, the best of +-40 ms
// by cross-correlation, refined between hops), and the correlation there.
double envLag(const Sig& x, const Sig& y, double tf, double sr, double* corrOut = nullptr) {
  const double hop = 0.0005 * sr, half = 0.005 * sr;
  const size_t frames = std::min(y.size(), static_cast<size_t>(static_cast<double>(x.size()) * tf));
  const auto count = static_cast<size_t>(static_cast<double>(frames) / hop);
  auto env = [&](const Sig& s, double scale) {
    std::vector<double> e(count, 0.0);
    double mean = 0;
    for (size_t k = 0; k < count; ++k) {
      const double c = static_cast<double>(k) * hop / scale, h = half / scale;
      const auto a = static_cast<size_t>(std::max(0.0, c - h));
      const auto b = std::min(s.size(), std::max(a + 1, static_cast<size_t>(c + h)));
      e[k] = rms(s, a, b), mean += e[k];
    }
    mean /= static_cast<double>(count);
    for (double& v : e) v -= mean;
    return e;
  };
  const auto ex = env(x, tf), ey = env(y, 1.0);
  const int range = static_cast<int>(0.04 * sr / hop);
  std::vector<double> cs(static_cast<size_t>(2 * range + 1));
  for (int l = -range; l <= range; ++l) {
    double s = 0, aa = 0, bb = 0;
    for (size_t k = static_cast<size_t>(range) + 40; k + static_cast<size_t>(range) + 40 < count; ++k) {
      const double u = ex[k], v = ey[static_cast<size_t>(static_cast<long>(k) + l)];
      s += u * v, aa += u * u, bb += v * v;
    }
    cs[static_cast<size_t>(l + range)] = s / std::sqrt(aa * bb + 1e-30);
  }
  size_t k = 0;
  for (size_t i = 1; i < cs.size(); ++i)
    if (cs[i] > cs[k]) k = i;
  double d = 0;
  if (k > 0 && k + 1 < cs.size()) {
    const double den = cs[k - 1] - 2 * cs[k] + cs[k + 1];
    if (den < 0) d = 0.5 * (cs[k - 1] - cs[k + 1]) / den;
  }
  if (corrOut) *corrOut = cs[k];
  return (static_cast<double>(k) - range + d) * hop / sr * 1000;
}
// The frequency (Hz) of the strongest spectral peak of x[a, a + 65536) (parabolic interpolation).
double peakHz(const Sig& x, size_t a, double sr) {
  const size_t n = 65536;
  const auto p = power(x, a, n);
  size_t k = 1;
  for (size_t i = 2; i + 1 < p.size(); ++i)
    if (p[i] > p[k]) k = i;
  const double l = std::log(p[k - 1] + 1e-30), c = std::log(p[k] + 1e-30), r = std::log(p[k + 1] + 1e-30);
  const double d = 0.5 * (l - r) / (l - 2 * c + r);
  return (static_cast<double>(k) + d) * sr / static_cast<double>(n);
}
// A 440 Hz tone under a slowly moving level (1.7, 3.1 and 5.3 Hz swells): one
// partial, so no beating moves with the pitch, and no onsets, so the timing of
// a stretch shows in the level, not in how it places a hit (R3 moves hits by a
// millisecond or two on purpose; the CLI does the same).
Sig swells(double sr, double sec) {
  Sig x(static_cast<size_t>(sec * sr), 0.f);
  for (size_t i = 0; i < x.size(); ++i) {
    const double t = static_cast<double>(i) / sr;
    const double g = 0.55 + 0.2 * std::sin(2 * kPi * 1.7 * t) + 0.15 * std::sin(2 * kPi * 3.1 * t + 1) + 0.1 * std::sin(2 * kPi * 5.3 * t + 2);
    x[i] = static_cast<float>(g * 0.4 * std::sin(2 * kPi * 440 * t));
  }
  return x;
}

SamplePtr sampleOf(const Sig& x, double sr) {
  auto s = std::make_shared<Sample>();
  s->sampleRate = sr;
  s->left = x;
  s->setMono();
  return s;
}

}  // namespace

int main() {
  char msg[256];

  // ============================================================ a bass through the hits
  {
    // One continuous stream: nothing restarts at a hit, so the bass under the
    // kicks keeps its level (J-87 restarted the sound at every hit; the
    // J-140 .. J-145 onset paths swapped engines there).
    for (double sr : {44100.0, 48000.0, 96000.0}) {
      std::vector<double> hs;
      const Sig x = bassKick(sr, sr > 50000 ? 3.0 : 6.0, &hs);
      const Dip src = bassDips(x, hs, 1.0, sr);
      double worstMean = 0, worstDip = 0;
      for (double tf : {0.8, 2.0}) {
        const Dip d = bassDips(offline1(x, 1.0, tf, sr), hs, tf, sr);
        worstMean = std::min(worstMean, d.mean), worstDip = std::min(worstDip, d.worst);
      }
      std::printf("# bass under kicks %.0f Hz: source mean dip %.2f dB worst %.2f; offline mean %.2f dB, worst %.2f dB\n", sr, src.mean, src.worst,
                  worstMean, worstDip);
      std::snprintf(msg, sizeof msg, "offline stretch at %.0f Hz: a bass keeps its level through the hits (x0.8, x2)", sr);
      expect(worstMean > -2.0 && worstDip > -4.0, msg);
    }
    // One channel, pitched.
    std::vector<double> hs;
    const Sig x = bassKick(44100, 6.0, &hs);
    const auto y = offline({x}, std::pow(2.0, 3 / 12.0), 2.0, 44100)[0];
    const Dip d = bassDips(y, hs, 2.0, 44100);  // (the band holds the moved harmonics too)
    std::printf("# bass under kicks, mono +3 st x2: mean dip %.2f dB, worst %.2f dB\n", d.mean, d.worst);
    expect(finite(y) && y.size() == x.size() * 2 && d.mean > -2.0 && d.worst > -3.0, "offline stretch, one channel, pitched: the bass keeps its level through the hits");

    // Sampler voices (realtime).
    const std::string path = writeTmp("gl_j153_basskick.wav", x, 44100);
    const Sig sv = sampler(path, 2.0, static_cast<size_t>(11.5 * 44100), 44100);
    const Dip dd = bassDips(sv, hs, 2.0, 44100);
    std::printf("# bass under kicks, sampler voice x2: mean dip %.2f dB, worst %.2f dB\n", dd.mean, dd.worst);
    expect(dd.mean > -2.0 && dd.worst > -3.5, "sampler voice: a bass keeps its level through the hits");
  }

  // ============================================================ drums: every hit, on time
  {
    // A spectral stretch spreads a hit over its block; the guard removes what
    // would sound ahead of it in the silence before (pre-echo under -40 dB).
    // Every hit is there with its energy, centred where the time map puts it
    // (energy-weighted, the hit's first 60 ms in the source, its spread
    // +-120 ms in the render). (At x4 a hit in silence loses about 4 dB of its
    // energy to the spread: a known trait.)
    auto hitAt = [](const Sig& x, long a, long b, double& centre) {
      double e = 0, m = 0;
      for (long i = std::max(0L, a); i < std::min(b, static_cast<long>(x.size())); ++i) {
        const double v = static_cast<double>(x[static_cast<size_t>(i)]) * x[static_cast<size_t>(i)];
        e += v, m += v * static_cast<double>(i);
      }
      centre = e > 0 ? m / e : 0;
      return e;
    };
    for (double sr : {44100.0, 96000.0}) {
      const Sig d = drums(sr, 3.0);
      double pre = -300, rise = 0, worstLevel = 0, worstMs = 0;
      bool ok = true;
      for (double tf : {0.75, 1.25, 2.0}) {
        const Sig y = offline1(d, 1.0, tf, sr);
        ok = ok && finite(y) && y.size() == static_cast<size_t>(std::llround(static_cast<double>(d.size()) * tf));
        std::vector<double> t;
        for (double h : drumTimes(3.0)) t.push_back(h * tf);
        const Hits h = hits(y, t, sr);
        pre = std::max(pre, h.preDb), rise = std::max(rise, h.riseMs);
        for (double h0 : drumTimes(3.0)) {
          const auto c = static_cast<long>(h0 * sr), cy = static_cast<long>(h0 * tf * sr);
          double ms = 0, my = 0;
          const double es = hitAt(d, c, c + static_cast<long>(0.06 * sr), ms);
          const double ey = hitAt(y, cy - static_cast<long>(0.12 * sr), cy + static_cast<long>(0.06 * tf * sr + 0.12 * sr), my);
          // the source hit's centre and energy, stretched with it
          const double want = static_cast<double>(cy) + (ms - static_cast<double>(c)) * tf;
          const double gain = 10 * std::log10(ey / (es * tf));
          if (std::fabs(gain) > 2 || std::fabs(my - want) / sr * 1000 > 6)
            std::printf("#   x%g hit %.2f s: %+.2f dB, %+.2f ms\n", tf, h0, gain, (my - want) / sr * 1000);
          worstLevel = std::max(worstLevel, std::fabs(gain));
          worstMs = std::max(worstMs, std::fabs(my - want) / sr * 1000);
        }
      }
      std::printf("# drums offline %.0f Hz x0.75 .. x2: hit energy within %.2f dB, centred within %.2f ms; pre-echo %.1f dB, rise %.2f ms\n", sr,
                  worstLevel, worstMs, pre, rise);
      std::snprintf(msg, sizeof msg, "offline stretch at %.0f Hz: every drum hit keeps its energy (2.5 dB) and its place (10 ms) at x0.75 .. x2, no pre-echo", sr);
      expect(ok && worstLevel < 2.5 && worstMs < 10.0 && pre < -40.0, msg);
    }
  }

  // ============================================================ plucks: no swell after the attack
  {
    std::vector<double> at;
    const Sig p = plucks(44100, 5.0, &at);
    double worst = 0;
    for (double tf : {1.5, 2.0}) {
      const double v = handoverSwell(offline1(p, 1.0, tf, 44100), at, tf, 44100);
      std::printf("#   x%g: %.2f dB\n", tf, v);
      worst = std::max(worst, v);
    }
    const double src = handoverSwell(p, at, 1.0, 44100);
    std::printf("# plucks: largest swell after the attack %.2f dB (source %.2f dB)\n", worst, src);
    expect(worst < src + 1.5, "stretch: a plucked note decays from its attack without a dip and swell");
  }

  // ============================================================ smooth: no hop-rate clicks
  {
    // Steady tones through the engine's blocks: a click where blocks meet
    // stands far above the tone's own third difference. (A 20 ms hop with 4
    // blocks overlapping gave a 55 Hz bass 30 .. 50 clicks a second at x2.)
    const double sr = 44100;
    Sig bass(static_cast<size_t>(4 * sr));
    for (size_t i = 0; i < bass.size(); ++i) {
      double v = 0;
      for (int k = 1; k <= 8; ++k) v += std::sin(2 * kPi * 55 * k * static_cast<double>(i) / sr + k) / k;
      bass[i] = static_cast<float>(0.3 * v);
    }
    const Sig tone = sine(440, static_cast<size_t>(4 * sr), sr, 0.5);
    double worstOff = 0, worstRt = 0;
    for (const Sig* x : {static_cast<const Sig*>(&bass), &tone})
      for (const auto& [tf, st] : {std::pair<double, double>{0.5, 0}, {0.75, 0}, {1.5, 0}, {2, 0}, {4, 0}, {1, -12}, {1, -5}, {1, -1}, {1, 1}, {1, 5}, {1, 12}}) {
        const double p = std::pow(2.0, st / 12.0);
        const Sig y = offline1(*x, p, tf, sr);
        const double off = clickRatio(y, static_cast<size_t>(0.5 * sr * tf), static_cast<size_t>(3.5 * sr * tf));
        const auto frames = static_cast<size_t>(3.5 * sr * std::min(tf, 1.0));
        const Sig r = voice(*x, 1.0 / tf, p, frames, sr, 256);
        const double rt = clickRatio(r, static_cast<size_t>(0.5 * sr), frames);
        if (off > 3 || rt > 3) std::printf("#   %s x%g %+g st: click ratio offline %.2f, realtime %.2f\n", x == &bass ? "bass" : "tone", tf, st, off, rt);
        worstOff = std::max(worstOff, off), worstRt = std::max(worstRt, rt);
      }
    std::printf("# steady bass and tone, x0.5 .. x4, -12 .. +12 st: worst click ratio offline %.2f, realtime %.2f\n", worstOff, worstRt);
    expect(worstOff < 3 && worstRt < 3, "stretch: steady tones stay smooth at x0.5 .. x4 and -12 .. +12 st (no hop-rate clicks), offline and realtime");
  }

  // ============================================================ tempo-mapped clips
  {
    // 2 ms bursts every 0.5 s over a 110 Hz bed; the rate alternates 1.0 / 1.5 each second.
    const double sr = 44100;
    Sig x(static_cast<size_t>(30 * sr));
    uint32_t rng = 99;
    for (size_t i = 0; i < x.size(); ++i) x[i] = static_cast<float>(0.05 * std::sin(2 * kPi * 110 * static_cast<double>(i) / sr));
    std::vector<double> bursts;
    for (double c = 0.25; c < 29.5; c += 0.5) {
      bursts.push_back(c);
      const auto a = static_cast<size_t>(c * sr);
      for (size_t i = 0; i < 88; ++i) {
        rng = rng * 1664525u + 1013904223u;
        x[a + i] += static_cast<float>(0.8 * ((static_cast<double>(rng >> 8) / 8388608.0) - 1.0) * std::sin(kPi * static_cast<double>(i) / 88));
      }
    }
    auto srcAt = [sr](int64_t j) {
      const double t = static_cast<double>(j);
      const double k = std::floor(t / sr);
      const auto ki = static_cast<int64_t>(k);
      return std::floor(k / 2) * 2.5 * sr + (ki % 2 ? sr : 0.0) + (t - k * sr) * (ki % 2 ? 1.5 : 1.0);
    };
    const auto outLen = static_cast<int64_t>(static_cast<double>(x.size()) / 1.25);
    double worst = 0;
    Sig y(static_cast<size_t>(outLen)), y2(y.size());
    const float* in[2] = {x.data(), x.data()};
    float* out[2] = {y.data(), y2.data()};
    stretch::Settings st;
    stretch::processMapped(in, 2, static_cast<int64_t>(x.size()), out, outLen, sr, st, srcAt);
    for (double c : bursts) {
      int64_t lo = 0, hi = outLen;  // expected output frame of the burst
      while (lo < hi) {
        const int64_t mid = (lo + hi) / 2;
        if (srcAt(mid) < c * sr) lo = mid + 1;
        else hi = mid;
      }
      if (lo < 2000 || lo + 2000 >= outLen) continue;
      // The burst's energy centre within +-40 ms (high-passed: the bed is low).
      double best = 0;
      long bestAt = 0;
      for (long k = -1764; k < 1764; ++k) {
        double e = 0;
        for (long j = 0; j < 44; ++j) {
          const auto i = static_cast<size_t>(lo + k + j);
          const double v = y[i] - y[i - 1];
          e += v * v;
        }
        if (e > best) best = e, bestAt = k + 22;
      }
      const double err = std::fabs(static_cast<double>(bestAt) - 44) / sr * 1000;  // the burst's centre is 44 frames in
      if (err > 1) std::printf("# burst at %.2f s: %.2f ms\n", c, err);
      worst = std::max(worst, err);
    }
    std::printf("# tempo steps 1.0 / 1.5 over 30 s: worst burst offset %.2f ms\n", worst);
    expect(worst < 1.0, "tempo-mapped clips: every hit within 1 ms of where the map puts it");
  }

  // ============================================================ realtime voices
  {
    const double sr = 44100;
    // Pitch automation: a 5 Hz vibrato of +-1 semitone set once per 256-frame call.
    const Sig t = sine(1000, static_cast<size_t>(3 * sr), sr, 0.4);
    stretch::Voice v;
    v.prepare(2, sr, 4.0, 4.0);
    Src src{&t};
    v.start(&Src::read, &src, 1.0, 1.0);
    Sig L(static_cast<size_t>(2 * sr)), R(L.size());
    for (size_t at = 0; at < L.size(); at += 256) {
      const double p = std::pow(2.0, std::sin(2 * kPi * 5 * static_cast<double>(at) / sr) / 12.0);
      float* o[2] = {L.data() + at, R.data() + at};
      v.render(o, static_cast<int>(std::min<size_t>(256, L.size() - at)), 1.0, p, &Src::read, &src);
    }
    // Stepped pitch leaves sidebands at the call rate (172 Hz) around the tone.
    const auto pw = power(L, 22050, 32768);
    double side = 0, all = 0;
    for (size_t k = 1; k < pw.size(); ++k) {
      const double f = static_cast<double>(k) * sr / 32768;
      all += pw[k];
      if (std::fabs(std::fabs(f - 1000) - 172) < 25) side += pw[k];
    }
    const double sideDb = 10 * std::log10(side / all + 1e-30);
    std::printf("# vibrato set per 256-frame call: sidebands at the call rate %.1f dB\n", sideDb);
    expect(sideDb < -70, "realtime voice: pitch changes ramp within each call (no zipper sidebands at the call rate)");

    // No allocation, the same output for any block size (a seek primed with pre-roll too).
    std::vector<double> hs;
    const Sig bk = bassKick(sr, 3.0, &hs);
    Sig outs[2];
    long allocs = 0;
    for (int pass = 0; pass < 2; ++pass) {
      stretch::Voice hv;
      hv.prepare(2, sr, 4.0, 4.0);
      Src bs{&bk};
      const int pre = hv.prerollFor(0.6);
      bs.pos = static_cast<size_t>(0.5 * sr) - static_cast<size_t>(pre);
      Sig o(static_cast<size_t>(3 * sr)), o2(o.size());
      CountScope cs;
      hv.start(&Src::read, &bs, 0.6, 1.1, pre);
      for (size_t at = 0; at < o.size();) {
        const int n = static_cast<int>(std::min<size_t>(pass ? 61 : 512, o.size() - at));
        float* op[2] = {o.data() + at, o2.data() + at};
        hv.render(op, n, 0.6, 1.1, &Src::read, &bs);
        at += static_cast<size_t>(n);
      }
      allocs += cs.n();
      outs[pass] = o;
    }
    float vd = 0;
    for (size_t i = 0; i < outs[0].size(); ++i) vd = std::max(vd, std::fabs(outs[0][i] - outs[1][i]));
    expect(allocs == 0 && vd < 1e-5f && rms(outs[0], 0, outs[0].size()) > 0.05,
           "realtime voice: start (with pre-roll) and render allocate nothing; the same output for any block size");

    // A note started inside the sample (a seek) is right from its first block: full level at once.
    const Sig tone = sine(220, static_cast<size_t>(2 * sr), sr, 0.5);
    const std::string tpath = writeTmp("gl_j153_tone.wav", tone, sr);
    {
      const Sig s = sampler(tpath, 1.25, static_cast<size_t>(0.2 * sr), sr, 64, 0.3f);
      const auto per = static_cast<size_t>(2 * sr / 220);  // two periods
      const double r0 = rms(s, 0, per), r1 = rms(s, static_cast<size_t>(0.05 * sr), static_cast<size_t>(0.05 * sr) + 10 * per);
      std::printf("# note started at 30 %%: first two periods %.3f, later %.3f\n", r0, r1);
      expect(r0 > 0.7 * r1 && r1 > 0.25, "sampler voice: a note started inside the sample plays at full level from its first frames");
    }

    // The end of a stretched note fades out (no hard cut while the engine still sounds).
    const Sig shortTone = sine(220, static_cast<size_t>(0.5 * sr), sr, 0.5);
    const std::string spath = writeTmp("gl_j153_short.wav", shortTone, sr);
    {
      const Sig s = sampler(spath, 1.5, static_cast<size_t>(1.5 * sr), sr);
      size_t last = 0;
      for (size_t i = 0; i < s.size(); ++i)
        if (s[i] != 0.f) last = i;
      const double end = rms(s, last > 32 ? last - 32 : 0, last + 1), body = rms(s, static_cast<size_t>(0.2 * sr), static_cast<size_t>(0.4 * sr));
      std::printf("# stretched note end: last frames %.4f against %.4f\n", end, body);
      expect(end < 0.05 * body, "sampler voice: a stretched note ends with a fade, not a cut");
    }
  }

  // ============================================================ clip playback: render <-> realtime
  {
    const double sr = 44100;
    const Sig tone = sine(220, static_cast<size_t>(4 * sr), sr, 0.5);
    const std::string path = writeTmp("gl_j153_cliptone.wav", tone, sr);
    SamplePtr smp = sampleCache().get(path);
    TempoMap map;
    map.setConstant(120, 96);
    ProgAudioClip c;
    c.start = 0, c.end = 192 * 6;
    c.startSec = 0, c.durSec = map.secondsAt(static_cast<double>(c.end));
    c.sample = smp, c.srcStart = 0, c.srcEnd = smp->seconds(), c.speed = 0.8, c.granular = true;
    stretch::ClipRequest rq;
    rq.sample = smp, rq.srcEnd = c.srcEnd, rq.mode = stretch::kStretch, rq.rate = sr, rq.speed = 0.8;
    rq.frames = static_cast<int64_t>(std::ceil(c.durSec * sr)) + 2;
    c.hq = stretch::renderClip(rq, false);
    c.rt = makeClipVoice(sr);
    // The channel pitch moves off the render's pitch (0) for 0.5 s and back: realtime, then the render again.
    const std::vector<ProgAudioClip> clips{c};  // the engine keeps its clips (and their playback state)
    const int64_t total = static_cast<int64_t>(2.5 * sr), off0 = static_cast<int64_t>(1.0 * sr), off1 = static_cast<int64_t>(1.5 * sr);
    Sig L(static_cast<size_t>(total), 0.f), R(L.size(), 0.f);
    long allocs = 0;
    for (int64_t at = 0; at < total; at += 512) {
      const int len = static_cast<int>(std::min<int64_t>(512, total - at));
      const double semis = at >= off0 && at < off1 ? 0.02 : 0.0;  // 2 cents: the same sound, the other path
      CountScope cs;
      renderAudioClips(clips, map, sr, 0, at, 0, len, L.data() + at, R.data() + at, semis);
      allocs += cs.n();
    }
    const double base = maxCurve(L, static_cast<size_t>(0.3 * sr), static_cast<size_t>(0.9 * sr));
    const double sw = std::max(maxCurve(L, static_cast<size_t>(off0 - 600), static_cast<size_t>(off0 + 2000)),
                               maxCurve(L, static_cast<size_t>(off1 - 600), static_cast<size_t>(off1 + 2000)));
    const double mid = rms(L, static_cast<size_t>(off0 + 0.1 * sr), static_cast<size_t>(off1 - 0.05 * sr));
    std::printf("# clip render <-> realtime switch: curvature %.4f against %.4f in steady play; realtime level %.3f\n", sw, base, mid);
    expect(c.hq && sw < 2.0 * base && mid > 0.3 && allocs == 0,
           "clip playback: switching between the render and the realtime stretch crossfades (no click), no allocation");
  }

  // ============================================================ determinism, stereo
  {
    const double sr = 48000;
    std::vector<double> hs;
    const Sig bk = bassKick(sr, 2.0, &hs);
    const Sig a = offline1(bk, std::pow(2.0, 2 / 12.0), 1.3, sr), b = offline1(bk, std::pow(2.0, 2 / 12.0), 1.3, sr);
    expect(a == b, "offline stretch: renders are deterministic (bit-identical)");
    // Stereo: a centred bass, hats left and right from different noise: the correlation stays.
    Sig l = bk, r = bk;
    uint32_t ra = 5, rb = 17;
    for (double h = 0.1; h < 1.9; h += 0.125) {
      const auto at = static_cast<size_t>(h * sr);
      for (size_t i = 0; i < static_cast<size_t>(0.05 * sr) && at + i < l.size(); ++i) {
        ra = ra * 1664525u + 1013904223u, rb = rb * 22695477u + 1u;
        const double e = 0.3 * std::exp(-static_cast<double>(i) / (0.008 * sr));
        l[at + i] += static_cast<float>(e * ((static_cast<double>(ra >> 8) / 8388608.0) - 1.0));
        r[at + i] += static_cast<float>(e * ((static_cast<double>(rb >> 8) / 8388608.0) - 1.0));
      }
    }
    const double cin = corr(l, r, 0, l.size());
    const auto y = offline({l, r}, 1.0, 1.6, sr);
    const double change = std::fabs(corr(y[0], y[1], 0, y[0].size()) - cin);
    std::printf("# stereo correlation %.3f in, change %.3f\n", cin, change);
    expect(change < 0.05, "offline stretch: the stereo correlation stays (within 0.05)");
  }

  // ============================================================ J-157: Rubber Band
  {
    // Length, alignment (envelope lag), level and pitch at each rate, offline and realtime.
    double worstLagOff = 0, worstLagRt = 0, worstLevel = 0, worstCents = 0, minCorr = 1;
    bool lengths = true, fin = true;
    for (double sr : {44100.0, 48000.0, 96000.0}) {
      const Sig sw = swells(sr, sr > 50000 ? 3.0 : 5.0);
      for (double tf : {0.75, 1.0, 1.5, 2.0}) {
        for (double st : {0.0, 12.0, -12.0}) {
          if (tf != 1.0 && st != 0.0) continue;
          const double p = std::pow(2.0, st / 12.0);
          const Sig y = offlineRb1(sw, p, tf, sr);
          lengths = lengths && y.size() == static_cast<size_t>(std::llround(static_cast<double>(sw.size()) * tf));
          // (the last 0.3 s holds the stretched sound, not silence: the engine reached the end)
          lengths = lengths && rms(y, y.size() - static_cast<size_t>(0.3 * sr), y.size()) > 0.02;
          fin = fin && finite(y);
          double c = 0;
          const double lag = envLag(sw, y, tf, sr, &c);
          minCorr = std::min(minCorr, c);
          const auto frames = static_cast<size_t>(static_cast<double>(sw.size()) * std::min(tf, 1.0) * 0.9);
          const Sig r = voice(sw, 1.0 / tf, p, frames, sr, 256, nullptr, stretch::Engine::RubberBand);
          fin = fin && finite(r);
          double cr = 0;
          const double lagRt = envLag(sw, r, tf, sr, &cr);
          minCorr = std::min(minCorr, cr);
          const double lv = 20 * std::log10(rms(y, 0, y.size()) / rms(sw, 0, sw.size()));
          if (std::fabs(lag) > 1 || std::fabs(lagRt) > 1 || std::fabs(lv) > 1.5)
            std::printf("#   rubber %.0f Hz x%g %+g st: lag offline %.2f ms (corr %.2f), realtime %.2f ms (corr %.2f), level %+.2f dB\n", sr, tf, st, lag, c,
                        lagRt, cr, lv);
          worstLagOff = std::max(worstLagOff, std::fabs(lag)), worstLagRt = std::max(worstLagRt, std::fabs(lagRt));
          worstLevel = std::max(worstLevel, std::fabs(lv));
        }
      }
      // In tune: a 440 Hz tone an octave up and down, offline and realtime.
      const Sig tone = sine(440, static_cast<size_t>(2.5 * sr), sr, 0.4);
      for (double st : {-12.0, 12.0}) {
        const double p = std::pow(2.0, st / 12.0);
        const Sig y = offlineRb1(tone, p, 1.0, sr);
        const Sig r = voice(tone, 1.0, p, static_cast<size_t>(2.2 * sr), sr, 256, nullptr, stretch::Engine::RubberBand);
        for (const Sig* s : {&y, &r}) {
          const double hz = peakHz(*s, static_cast<size_t>(0.3 * sr), sr);
          worstCents = std::max(worstCents, std::fabs(1200 * std::log2(hz / (440 * p))));
        }
      }
    }
    std::printf("# rubber band x0.75 .. x2, +-12 st, 44.1 / 48 / 96 kHz: envelope lag offline %.2f ms, realtime %.2f ms (lowest correlation %.2f); "
                "level within %.2f dB; pitch within %.2f cents\n",
                worstLagOff, worstLagRt, minCorr, worstLevel, worstCents);
    expect(lengths && fin, "rubber band: offline renders have the exact length, filled to the end, at 44.1 / 48 / 96 kHz");
    expect(worstLagOff <= 1.0 && worstLagRt <= 1.0 && minCorr > 0.8,
           "rubber band: offline and realtime output line up with the source (envelope cross-correlation lag within 1 ms)");
    expect(worstLevel < 1.5, "rubber band: the level stays (within 1.5 dB) at x0.75 .. x2 and +-12 st");
    expect(worstCents < 2.0, "rubber band: an octave up and down in tune (2 cents), offline and realtime");

    // No clicks on steady tones (offline and realtime).
    const double sr = 44100;
    Sig bass(static_cast<size_t>(4 * sr));
    for (size_t i = 0; i < bass.size(); ++i) {
      double v = 0;
      for (int k = 1; k <= 8; ++k) v += std::sin(2 * kPi * 55 * k * static_cast<double>(i) / sr + k) / k;
      bass[i] = static_cast<float>(0.3 * v);
    }
    const Sig tone = sine(440, static_cast<size_t>(4 * sr), sr, 0.5);
    double worstOff = 0, worstRt = 0;
    for (const Sig* x : {static_cast<const Sig*>(&bass), &tone})
      for (const auto& [tf, st] : {std::pair<double, double>{0.5, 0}, {0.75, 0}, {1.5, 0}, {2, 0}, {4, 0}, {1, -12}, {1, -5}, {1, 5}, {1, 12}}) {
        const double p = std::pow(2.0, st / 12.0);
        const Sig y = offlineRb1(*x, p, tf, sr);
        const double off = clickRatio(y, static_cast<size_t>(0.5 * sr * tf), static_cast<size_t>(3.5 * sr * tf));
        const auto frames = static_cast<size_t>(3.5 * sr * std::min(tf, 1.0));
        const Sig r = voice(*x, 1.0 / tf, p, frames, sr, 256, nullptr, stretch::Engine::RubberBand);
        const double rt = clickRatio(r, static_cast<size_t>(0.5 * sr), frames);
        if (off > 3 || rt > 3) std::printf("#   rubber %s x%g %+g st: click ratio offline %.2f, realtime %.2f\n", x == &bass ? "bass" : "tone", tf, st, off, rt);
        worstOff = std::max(worstOff, off), worstRt = std::max(worstRt, rt);
      }
    std::printf("# rubber band steady bass and tone, x0.5 .. x4, -12 .. +12 st: worst click ratio offline %.2f, realtime %.2f\n", worstOff, worstRt);
    expect(worstOff < 3 && worstRt < 3, "rubber band: steady tones stay smooth (no clicks), offline and realtime");

    // Realtime: no allocation, the same output for any block size, a seek primed with pre-roll.
    std::vector<double> hs;
    const Sig bk = bassKick(sr, 3.0, &hs);
    Sig outs[3];
    long allocs = 0;
    for (int pass = 0; pass < 3; ++pass) {
      stretch::Voice hv;
      hv.prepare(2, sr, 4.0, 4.0, false, stretch::Engine::RubberBand);
      Src bs{&bk};
      const int pre = hv.prerollFor(0.6);
      bs.pos = static_cast<size_t>(0.5 * sr) - static_cast<size_t>(pre);
      Sig o(static_cast<size_t>(2.5 * sr)), o2(o.size());
      CountScope cs;
      hv.start(&Src::read, &bs, 0.6, 1.1, pre);
      for (size_t at = 0; at < o.size();) {
        const int n = static_cast<int>(std::min<size_t>(pass == 0 ? 512 : pass == 1 ? 61 : 16, o.size() - at));
        float* op[2] = {o.data() + at, o2.data() + at};
        hv.render(op, n, 0.6, 1.1, &Src::read, &bs);
        at += static_cast<size_t>(n);
      }
      allocs += cs.n();
      outs[pass] = o;
    }
    float vd = 0;
    for (int pass = 1; pass < 3; ++pass)
      for (size_t i = 0; i < outs[0].size(); ++i) vd = std::max(vd, std::fabs(outs[0][i] - outs[pass][i]));
    std::printf("# rubber band realtime: %ld allocations in start + render; largest difference between block sizes 512 / 61 / 16: %g\n", allocs, vd);
    expect(allocs == 0 && vd < 1e-6f && rms(outs[0], 0, outs[0].size()) > 0.05,
           "rubber band realtime: start (with pre-roll) and render allocate nothing; the same output for any block size");

    // The seek lines up: a stream started 1 s in (primed) against one started at 0, from 1 s on.
    {
      const Sig tn = swells(sr, 4.0);
      const double speed = 0.8, pitch = std::pow(2.0, 3 / 12.0);
      const Sig whole = voice(tn, speed, pitch, static_cast<size_t>(3 * sr), sr, 256, nullptr, stretch::Engine::RubberBand);
      stretch::Voice sv;
      sv.prepare(2, sr, 8.0, 8.0, false, stretch::Engine::RubberBand);
      Src s2{&tn};
      const int pre = sv.prerollFor(speed);
      const auto startOut = static_cast<size_t>(1.0 * sr);  // output frame 1 s plays source 0.8 s
      s2.pos = static_cast<size_t>(std::llround(static_cast<double>(startOut) * speed)) - static_cast<size_t>(pre);
      sv.start(&Src::read, &s2, speed, pitch, pre);
      Sig seek(static_cast<size_t>(1.5 * sr)), seekR(seek.size());
      for (size_t at = 0; at < seek.size(); at += 256) {
        float* op[2] = {seek.data() + at, seekR.data() + at};
        sv.render(op, static_cast<int>(std::min<size_t>(256, seek.size() - at)), speed, pitch, &Src::read, &s2);
      }
      // The seek's level envelope against the unbroken stream's from 1 s, and the source's.
      const Sig tail(whole.begin() + static_cast<long>(startOut), whole.begin() + static_cast<long>(startOut + seek.size()));
      const Sig srcPart(tn.begin() + static_cast<long>(std::llround(static_cast<double>(startOut) * speed)), tn.end());
      double c1 = 0, c2 = 0;
      const double lag = envLag(tail, seek, 1.0, sr, &c1), lagSrc = envLag(srcPart, seek, 1.0 / speed, sr, &c2);
      const double r0 = rms(seek, 0, static_cast<size_t>(0.02 * sr)), r1 = rms(tail, 0, static_cast<size_t>(0.02 * sr));
      std::printf("# rubber band seek at 1 s (pre-roll %d frames): envelope lag %.2f ms against the unbroken stream (corr %.2f), %.2f ms against "
                  "the source (corr %.2f); first 20 ms level %.3f against %.3f\n",
                  pre, lag, c1, lagSrc, c2, r0, r1);
      expect(std::fabs(lag) <= 1.0 && std::fabs(lagSrc) <= 1.0 && c1 > 0.9 && r0 > 0.8 * r1,
             "rubber band realtime: a seek primed with pre-roll lines up with the unbroken stream (1 ms) at full level from its first frames");
    }

    // A pitch bend sweeping -7 .. +7 semitones (and speed 0.7 .. 1.3) while it plays: no allocation, smooth.
    {
      const Sig tn = sine(330, static_cast<size_t>(6 * sr), sr, 0.4);
      double crs = 0;
      for (const auto eng : {stretch::Engine::Stretch, stretch::Engine::RubberBand}) {
      stretch::Voice bv;
      bv.prepare(2, sr, 4.0, 4.0, false, eng);
      Src s3{&tn};
      Sig o(static_cast<size_t>(3 * sr)), o2(o.size());
      long al = 0;
      {
        CountScope cs;
        bv.start(&Src::read, &s3, 1.0, 1.0, 0);
        for (size_t at = 0; at < o.size(); at += 128) {
          const double u = static_cast<double>(at) / static_cast<double>(o.size());
          const double semis = 7 * std::sin(2 * kPi * 1.5 * u), speed = 1.0 + 0.3 * std::sin(2 * kPi * u);
          float* op[2] = {o.data() + at, o2.data() + at};
          bv.render(op, static_cast<int>(std::min<size_t>(128, o.size() - at)), speed, std::pow(2.0, semis / 12), &Src::read, &s3);
        }
        al = cs.n();
      }
      const double cr = clickRatio(o, static_cast<size_t>(0.3 * sr), o.size());
      if (eng == stretch::Engine::Stretch) {
        crs = cr;
        if (std::getenv("GL_J157_DUMP")) writeTmp("gl_j157_bend_ss.wav", o, sr);
        continue;
      }
      if (std::getenv("GL_J157_DUMP")) writeTmp("gl_j157_bend_rb.wav", o, sr);
      std::printf("# rubber band realtime pitch bend +-7 st with speed 0.7 .. 1.3: %ld allocations, click ratio %.2f (the stretch: %.2f)\n", al, cr, crs);
      expect(al == 0 && finite(o) && cr < 3, "rubber band realtime: bending the pitch and speed while playing allocates nothing and stays smooth");
      }
    }

    // A tempo map (rates 1.0 / 1.5 every second): the swells stay where the map puts them.
    {
      const Sig x = swells(sr, 12.0);
      auto srcAt = [sr](int64_t j) {
        const double t = static_cast<double>(j);
        const double k = std::floor(t / sr);
        const auto ki = static_cast<int64_t>(k);
        return std::floor(k / 2) * 2.5 * sr + (ki % 2 ? sr : 0.0) + (t - k * sr) * (ki % 2 ? 1.5 : 1.0);
      };
      const auto outLen = static_cast<int64_t>(static_cast<double>(x.size()) / 1.25) - static_cast<int64_t>(sr);
      Sig y(static_cast<size_t>(outLen)), y2(y.size());
      const float* in[2] = {x.data(), x.data()};
      float* out[2] = {y.data(), y2.data()};
      stretch::Settings st;
      st.mode = stretch::Mode::RubberBand;
      stretch::processMapped(in, 2, static_cast<int64_t>(x.size()), out, outLen, sr, st, srcAt);
      // The map's reading of the source, plainly: the expected envelope.
      Sig ref(y.size());
      for (int64_t j = 0; j < outLen; ++j) ref[static_cast<size_t>(j)] = x[static_cast<size_t>(std::min<double>(srcAt(j), static_cast<double>(x.size() - 1)))];
      double c = 0;
      const double lag = envLag(ref, y, 1.0, sr, &c);
      // Each 2 s stretch on its own (a drift would show as a growing lag).
      double worstWin = 0, minWinCorr = 1;
      for (int w = 0; w + 2 <= 8; w += 2) {
        const auto a = static_cast<long>(w * sr), b = static_cast<long>((w + 2) * sr);
        const Sig rr(ref.begin() + a, ref.begin() + b), yy(y.begin() + a, y.begin() + b);
        double cc = 0;
        worstWin = std::max(worstWin, std::fabs(envLag(rr, yy, 1.0, sr, &cc)));
        minWinCorr = std::min(minWinCorr, cc);
      }
      std::printf("# rubber band tempo steps 1.0 / 1.5: envelope lag %.2f ms against the map (corr %.2f); worst 2 s window %.2f ms (corr %.2f)\n", lag,
                  c, worstWin, minWinCorr);
      expect(std::fabs(lag) <= 3.0 && worstWin <= 3.0 && minWinCorr > 0.95,
             "rubber band: tempo-mapped clips follow the map (envelope lag within 3 ms: the engine eases into each rate change), no drift");
    }

    // Clip playback: the Rubber Band render and its realtime voice crossfade without a click.
    {
      const Sig ct = sine(220, static_cast<size_t>(4 * sr), sr, 0.5);
      const std::string path = writeTmp("gl_j157_cliptone.wav", ct, sr);
      SamplePtr smp = sampleCache().get(path);
      TempoMap map;
      map.setConstant(120, 96);
      ProgAudioClip c;
      c.start = 0, c.end = 192 * 6;
      c.startSec = 0, c.durSec = map.secondsAt(static_cast<double>(c.end));
      c.sample = smp, c.srcStart = 0, c.srcEnd = smp->seconds(), c.speed = 0.8, c.granular = true;
      c.engine = stretch::Engine::RubberBand;
      stretch::ClipRequest rq;
      rq.sample = smp, rq.srcEnd = c.srcEnd, rq.mode = stretch::Mode::RubberBand, rq.rate = sr, rq.speed = 0.8;
      rq.frames = static_cast<int64_t>(std::ceil(c.durSec * sr)) + 2;
      c.hq = stretch::renderClip(rq, false);
      c.rt = makeClipVoice(sr, stretch::Engine::RubberBand);
      const std::vector<ProgAudioClip> clips{c};
      const int64_t total = static_cast<int64_t>(2.5 * sr), off0 = static_cast<int64_t>(1.0 * sr), off1 = static_cast<int64_t>(1.5 * sr);
      Sig L(static_cast<size_t>(total), 0.f), R(L.size(), 0.f);
      long al = 0;
      for (int64_t at = 0; at < total; at += 512) {
        const int len = static_cast<int>(std::min<int64_t>(512, total - at));
        const double semis = at >= off0 && at < off1 ? 0.02 : 0.0;
        CountScope cs;
        renderAudioClips(clips, map, sr, 0, at, 0, len, L.data() + at, R.data() + at, semis);
        al += cs.n();
      }
      const double base = maxCurve(L, static_cast<size_t>(0.3 * sr), static_cast<size_t>(0.9 * sr));
      const double swc = std::max(maxCurve(L, static_cast<size_t>(off0 - 600), static_cast<size_t>(off0 + 2000)),
                                  maxCurve(L, static_cast<size_t>(off1 - 600), static_cast<size_t>(off1 + 2000)));
      const double mid = rms(L, static_cast<size_t>(off0 + 0.1 * sr), static_cast<size_t>(off1 - 0.05 * sr));
      std::printf("# rubber band clip render <-> realtime switch: curvature %.4f against %.4f in steady play; realtime level %.3f\n", swc, base, mid);
      expect(c.hq && swc < 2.0 * base && mid > 0.3 && al == 0,
             "rubber band clip playback: switching between its render and its realtime voice crossfades (no click), no allocation");
    }

    // Sampler notes on Rubber Band: the bass under the kicks keeps its level.
    {
      std::vector<double> h2;
      const Sig x = bassKick(sr, 6.0, &h2);
      const std::string path = writeTmp("gl_j157_basskick.wav", x, sr);
      gSamplerMode = stretch::Mode::RubberBand;
      const Sig sv = sampler(path, 2.0, static_cast<size_t>(11.5 * sr), sr);
      gSamplerMode = stretch::kStretch;
      const Dip dd = bassDips(sv, h2, 2.0, sr);
      std::printf("# rubber band sampler voice x2: bass mean dip %.2f dB, worst %.2f dB, level %.3f\n", dd.mean, dd.worst, rms(sv, 0, sv.size()));
      expect(finite(sv) && rms(sv, 0, sv.size()) > 0.05 && dd.mean > -3.0, "rubber band sampler voices: notes play, the bass keeps its level through the hits");
    }

    // CPU: offline and realtime, stereo, 44.1 kHz (a steady mix of bass and tone).
    {
      Sig mix(static_cast<size_t>(10 * sr));
      for (size_t i = 0; i < mix.size(); ++i) mix[i] = 0.5f * bass[i % bass.size()] + 0.3f * tone[i % tone.size()];
      for (const auto& [tf, st] : {std::pair<double, double>{1.5, 0}, {1.0, 5}}) {
        const double p = std::pow(2.0, st / 12.0);
        auto t0 = std::chrono::steady_clock::now();
        const auto y = offlineRb({mix, mix}, p, tf, sr);
        const double dOff = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        t0 = std::chrono::steady_clock::now();
        const auto z = offline({mix, mix}, p, tf, sr);
        const double dSs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        t0 = std::chrono::steady_clock::now();
        const auto frames = static_cast<size_t>(8 * sr);
        const Sig r = voice(mix, 1.0 / tf, p, frames, sr, 256, nullptr, stretch::Engine::RubberBand);
        const double dRt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        t0 = std::chrono::steady_clock::now();
        const Sig r2 = voice(mix, 1.0 / tf, p, frames, sr, 256);
        const double dRt2 = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        const double outSec = static_cast<double>(y[0].size()) / sr;
        std::printf("# CPU x%g %+g st, stereo 44.1 kHz (times realtime): Rubber Band offline %.1f, realtime %.1f; Stretch offline %.1f, realtime %.1f\n",
                    tf, st, outSec / dOff, 8.0 / dRt, outSec / dSs, 8.0 / dRt2);
        (void)z, (void)r, (void)r2;
      }
      // A note-on: start() and the first 256 frames (the engine works through its start delay first).
      stretch::Voice nv;
      nv.prepare(2, sr, 4.0, 4.0, false, stretch::Engine::RubberBand);
      double worstMs = 0, sumMs = 0;
      for (int k = 0; k < 20; ++k) {
        Src s4{&mix};
        const auto t0 = std::chrono::steady_clock::now();
        nv.start(&Src::read, &s4, 1.0, k % 2 ? 1.5 : 0.75, 0);
        Sig f0(256), f1(256);
        float* fo[2] = {f0.data(), f1.data()};
        nv.render(fo, 256, 1.0, k % 2 ? 1.5 : 0.75, &Src::read, &s4);
        const double ms = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() * 1000;
        worstMs = std::max(worstMs, ms), sumMs += ms;
      }
      std::printf("# rubber band note-on (start + first 256 frames, stereo 44.1 kHz): %.2f ms on average, worst %.2f ms; lookahead at x1 %d frames\n", sumMs / 20,
                  worstMs, nv.lookahead(1.0, 1.0));
    }
  }

  // ============================================================ the first frame stays put
  {
    const double sr = 44100;
    Sig x(static_cast<size_t>(0.5 * sr), 0.f);
    x[0] = 1.0f, x[4851] = 1.0f;
    const Sig y = offline1(x, 1.0, 2.0, sr);
    size_t at = 0, pk = 0;
    for (size_t i = 0; i < 2000; ++i)
      if (std::fabs(y[i]) > std::fabs(y[pk])) pk = i;
    while (at < y.size() && std::fabs(y[at]) < 0.5f * std::fabs(y[pk])) ++at;
    std::printf("# a hit on the first frame: its peak at frame %zu (first half-peak frame %zu)\n", pk, at);
    expect(pk <= 2, "offline stretch: a hit on the first frame stays on the first frame");
  }

  return finish();
}
