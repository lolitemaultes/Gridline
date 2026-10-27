// Gridline: J-145 glitch-free time stretching (engine/TimeStretch,
// engine/pitch/Hpss, engine/pitch/TdStretch, engine/AudioClips, the sampler's
// stretch voices). Objective checks on synthetic material for each glitch the
// J-145 assessment found:
//   hits       a bass under kick drums keeps its level through every hit
//              (offline renders, tempo-mapped clips, sampler voices), at
//              44.1 / 48 / 96 kHz, mono and stereo; drums in silence stay
//              sharp with nothing ahead of the hits; a plucked note's attack
//              hands over to its decay without a dip; no hit lost or doubled
//              at 0.25x .. 8x;
//   time maps  tempo-mapped clips keep their hits where the map puts them;
//   realtime   pitch changes ramp within a render call (no zipper), the
//              hybrid voice allocates nothing and is the same for any block
//              size, a note started inside a sample starts at full level,
//              the stretched tail of a note fades out;
//   clips      playback switching between the offline render and the
//              realtime grains crossfades;
//   general    offline renders are deterministic, stereo correlation kept.
#include <algorithm>
#include <atomic>
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
#include "../../src/engine/pitch/Level.h"
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

std::vector<Sig> offline(const std::vector<Sig>& x, stretch::Mode m, double pitch, double tf, double sr) {
  stretch::Settings s;
  s.mode = m, s.pitch = pitch;
  const auto n = static_cast<int64_t>(x[0].size());
  const auto outLen = static_cast<int64_t>(std::llround(static_cast<double>(n) * tf));
  std::vector<Sig> y(x.size(), Sig(static_cast<size_t>(outLen)));
  std::vector<const float*> in;
  std::vector<float*> out;
  for (size_t c = 0; c < x.size(); ++c) in.push_back(x[c].data()), out.push_back(y[c].data());
  stretch::process(in.data(), static_cast<int>(x.size()), n, out.data(), outLen, sr, s);
  return y;
}
Sig offline1(const Sig& x, stretch::Mode m, double pitch, double tf, double sr) { return offline({x, x}, m, pitch, tf, sr)[0]; }

std::string writeTmp(const std::string& name, const Sig& l, double sr) {
  AudioBuffer b(2, l.size());
  for (size_t i = 0; i < l.size(); ++i) b.at(0, i) = l[i], b.at(1, i) = l[i];
  const std::string path = (fs::temp_directory_path() / name).string();
  writeWav(path, b, static_cast<int>(sr), 24);
  sampleCache().clear();
  return path;
}

// One sampler note (time mode `mode`, time stretch `tf`) over `frames`, in blocks of `block`.
struct SamplerRun {
  PluginState st;
  ChannelAux aux;
};
Sig sampler(const std::string& path, stretch::Mode mode, double tf, size_t frames, double sr, int block = 256, float start = 0.f,
            float pitchSemis = 0.f, int note = 60) {
  PluginState st = defaultPluginState("gl.sampler");
  st.data = path;
  st.params[pp::SmpStretchMode] = static_cast<float>(mode);
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
// Onset count of a signal (the engine's own detector).
size_t onsetCount(const Sig& x, double sr) { return stretch::detectOnsets(x.data(), static_cast<int64_t>(x.size()), sr, 0.5).size(); }
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

// A voice reader over a buffer that supplies the harmonic part and the onsets.
struct HybridSrc {
  const Sig* x = nullptr;
  const Sample* h = nullptr;
  const std::vector<int64_t>* on = nullptr;
  size_t pos = 0, next = 0;
  static int read(void* ctx, float* const* d, int n, bool) {
    auto* s = static_cast<HybridSrc*>(ctx);
    int hit = -1;
    for (int i = 0; i < n; ++i, ++s->pos) {
      if (s->on && s->next < s->on->size() && static_cast<int64_t>(s->pos) >= (*s->on)[s->next]) {
        if (hit < 0) hit = i;
        while (s->next < s->on->size() && (*s->on)[s->next] <= static_cast<int64_t>(s->pos)) ++s->next;
      }
      const bool in = s->pos < s->x->size();
      d[0][i] = d[1][i] = in ? (*s->x)[s->pos] : 0.f;
      if (s->h) d[2][i] = d[3][i] = in ? s->h->left[s->pos] : 0.f;
    }
    return hit;
  }
};
SamplePtr sampleOf(const Sig& x, double sr) {
  auto s = std::make_shared<Sample>();
  s->sampleRate = sr;
  s->left = x;
  s->setMono();
  return s;
}

const char* name(stretch::Mode m) { return stretch::modeName(m); }

}  // namespace

int main() {
  char msg[256];
  const stretch::Mode onsetModes[] = {stretch::Mode::Auto, stretch::Mode::Transient, stretch::Mode::Beats};

  // ============================================================ a bass through the hits
  {
    // J-87 restarted the whole sound at every hit: the bass fell silent ahead
    // of each kick (Beats: a gap of up to 13 dB) and came back with a click.
    // What is left: around each hit the harmonic engine plays at the original
    // speed (the elastic map) and its rate change costs the bass a little
    // (Rubber Band R3: -0.9 dB here; the source itself -0.4 dB).
    for (double sr : {44100.0, 48000.0, 96000.0}) {
      std::vector<double> hs;
      const Sig x = bassKick(sr, sr > 50000 ? 3.0 : 6.0, &hs);
      const Dip src = bassDips(x, hs, 1.0, sr);
      std::printf("# bass under kicks, source %.0f Hz: mean dip %.2f dB, worst %.2f dB\n", sr, src.mean, src.worst);
      double worstMean = 0, worstDip = 0;
      const char* worstAt = "";
      for (stretch::Mode m : onsetModes)
        for (double tf : {0.8, 2.0}) {
          const Dip d = bassDips(offline1(x, m, 1.0, tf, sr), hs, tf, sr);
          if (d.mean < worstMean) worstMean = d.mean, worstAt = name(m);
          worstDip = std::min(worstDip, d.worst);
        }
      std::printf("# bass under kicks, offline %.0f Hz: mean dip %.2f dB (%s), worst %.2f dB\n", sr, worstMean, worstAt, worstDip);
      std::snprintf(msg, sizeof msg, "offline onset modes at %.0f Hz: a bass keeps its level through the hits (x0.8, x2)", sr);
      expect(worstMean > -2.0 && worstDip > -4.0, msg);
    }
    // One channel, pitched.
    std::vector<double> hs;
    const Sig x = bassKick(44100, 6.0, &hs);
    const auto y = offline({x}, stretch::Mode::Auto, std::pow(2.0, 3 / 12.0), 2.0, 44100)[0];
    const Dip d = bassDips(y, hs, 2.0, 44100);  // (the band holds the moved harmonics too)
    std::printf("# bass under kicks, mono +3 st x2: mean dip %.2f dB, worst %.2f dB\n", d.mean, d.worst);
    expect(finite(y) && y.size() == x.size() * 2 && d.mean > -2.0 && d.worst > -3.0, "offline Auto, one channel, pitched: the bass keeps its level through the hits");

    // Sampler voices (realtime): the hybrid with the sample's harmonic part.
    const std::string path = writeTmp("gl_j145_basskick.wav", x, 44100);
    double worstMean = 0, worstDip = 0;
    for (stretch::Mode m : onsetModes) {
      const Sig s = sampler(path, m, 2.0, static_cast<size_t>(11.5 * 44100), 44100);
      const Dip dd = bassDips(s, hs, 2.0, 44100);
      std::printf("#   sampler %s: mean dip %.2f dB, worst %.2f dB\n", name(m), dd.mean, dd.worst);
      worstMean = std::min(worstMean, dd.mean), worstDip = std::min(worstDip, dd.worst);
    }
    std::printf("# bass under kicks, sampler voices x2: mean dip %.2f dB, worst %.2f dB\n", worstMean, worstDip);
    // (Transient takes every kick for a hit, Auto only the strongest: more spans, more rate changes.)
    expect(worstMean > -2.5 && worstDip > -3.5, "sampler voices (Auto, Transient, Beats): a bass keeps its level through the hits");
  }

  // ============================================================ drums: sharp, nothing ahead
  {
    for (double sr : {44100.0, 96000.0}) {
      const Sig d = drums(sr, 3.0);
      double pre = -300, rise = 0;
      for (stretch::Mode m : onsetModes)
        for (double tf : {1.25, 2.0}) {
          std::vector<double> t;
          for (double h : drumTimes(3.0)) t.push_back(h * tf);
          const Hits h = hits(offline1(d, m, 1.0, tf, sr), t, sr);
          pre = std::max(pre, h.preDb), rise = std::max(rise, h.riseMs);
        }
      std::printf("# drums offline %.0f Hz: worst pre-echo %.1f dB, rise %.2f ms\n", sr, pre, rise);
      std::snprintf(msg, sizeof msg, "offline onset modes at %.0f Hz: drum hits stay sharp, nothing ahead of them", sr);
      expect(pre < -50 && rise < 3.0, msg);
    }
    const Sig d = drums(44100, 3.0);
    const std::string path = writeTmp("gl_j145_drums.wav", d, 44100);
    double pre = -300, rise = 0;
    for (stretch::Mode m : onsetModes) {
      std::vector<double> t;
      for (double h : drumTimes(3.0)) t.push_back(h * 2.0);
      const Hits h = hits(sampler(path, m, 2.0, static_cast<size_t>(6 * 44100), 44100), t, 44100);
      pre = std::max(pre, h.preDb), rise = std::max(rise, h.riseMs);
    }
    std::printf("# drums, sampler voices x2: worst pre-echo %.1f dB, rise %.2f ms\n", pre, rise);
    expect(pre < -50 && rise < 3.0, "sampler voices (Auto, Transient, Beats): drum hits stay sharp, nothing ahead of them");
    // Extremes: every hit once (none lost, none doubled), exact length, finite.
    // (Silence ahead, so the first hit is not at the very start at x0.25.)
    Sig dp(static_cast<size_t>(0.3 * 44100), 0.f);
    dp.insert(dp.end(), d.begin(), d.end());
    const size_t want = onsetCount(dp, 44100);
    bool ok = true;
    for (stretch::Mode m : onsetModes)
      for (double tf : {0.25, 4.0, 8.0}) {
        const Sig y = offline1(dp, m, 1.0, tf, 44100);
        const size_t got = onsetCount(y, 44100);
        // At x8 the spectral engine's long decays of a kick's body can swell
        // back once by about 14 dB, 30 dB under the hit (a known limit of
        // phase vocoders at large factors): one such regrowth is tolerated.
        const size_t extra = tf >= 8 && m != stretch::Mode::Beats ? 1 : 0;
        if (got < want || got > want + extra || !finite(y) || y.size() != static_cast<size_t>(std::llround(static_cast<double>(dp.size()) * tf))) {
          ok = false;
          std::printf("# %s x%g: %zu hits of %zu:", name(m), tf, got, want);
          for (int64_t o : stretch::detectOnsets(y.data(), static_cast<int64_t>(y.size()), 44100, 0.5)) std::printf(" %.3f", static_cast<double>(o) / 44100 / tf);
          std::printf("\n");
        }
      }
    std::snprintf(msg, sizeof msg, "onset modes at x0.25, x4, x8: every hit (%zu), none doubled, exact length, finite", want);
    expect(ok, msg);
  }

  // ============================================================ plucks: attack hands over
  {
    std::vector<double> at;
    const Sig p = plucks(44100, 5.0, &at);
    double worst = 0, worstBeats = 0;
    for (stretch::Mode m : onsetModes)
      for (double tf : {1.5, 2.0}) {
        const double v = handoverSwell(offline1(p, m, 1.0, tf, 44100), at, tf, 44100);
        std::printf("#   %s x%g: %.2f dB\n", name(m), tf, v);
        (m == stretch::Mode::Beats ? worstBeats : worst) = std::max(m == stretch::Mode::Beats ? worstBeats : worst, v);
      }
    const double src = handoverSwell(p, at, 1.0, 44100);
    std::printf("# plucks: largest swell after the attack %.2f dB, Beats %.2f dB (source %.2f dB)\n", worst, worstBeats, src);
    expect(worst < src + 1.5, "Auto, Transient: a plucked note decays from its attack without a dip and swell (no misaligned attack copy)");
    // Beats: both parts run on time-domain engines, which line their grains up
    // each its own way once the speed leaves the original: a note whose body
    // the hit carries past its span swells by up to 3 dB (known limit).
    expect(worstBeats < src + 3.5, "Beats: a plucked note's swell after the attack stays within 3.5 dB of the source's");
  }

  // ============================================================ tempo-mapped clips
  {
    // 2 ms bursts every 0.5 s over a 110 Hz bed; the rate alternates 1.0 / 1.5 each second.
    // (The bursts stand about 20 dB over the bed: Auto takes a jump of 15 dB
    // for a hit, and one it does not take goes through the harmonic engine,
    // which places it only within its block.)
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
    const char* worstMode = "";
    for (stretch::Mode m : {stretch::Mode::Mono, stretch::Mode::Auto, stretch::Mode::Transient, stretch::Mode::Beats}) {
      Sig y(static_cast<size_t>(outLen)), y2(y.size());
      const float* in[2] = {x.data(), x.data()};
      float* out[2] = {y.data(), y2.data()};
      stretch::Settings s;
      s.mode = m;
      stretch::processMapped(in, 2, static_cast<int64_t>(x.size()), out, outLen, sr, s, srcAt);
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
        if (err > 4) std::printf("# %s burst at %.2f s: %.2f ms\n", name(m), c, err);
        if (err > worst) worst = err, worstMode = name(m);
      }
    }
    std::printf("# tempo steps 1.0 / 1.5 over 30 s: worst burst offset %.2f ms (%s)\n", worst, worstMode);
    expect(worst < 4.0, "tempo-mapped clips: every hit within 4 ms of where the map puts it (Mono, Auto, Transient, Beats)");
  }

  // ============================================================ realtime voices
  {
    const double sr = 44100;
    // Pitch automation: a 5 Hz vibrato of +-1 semitone set once per 256-frame call.
    const Sig t = sine(1000, static_cast<size_t>(3 * sr), sr, 0.4);
    struct Src {
      const Sig* x;
      size_t pos = 0;
      static int read(void* ctx, float* const* d, int n, bool) {
        auto* s = static_cast<Src*>(ctx);
        for (int i = 0; i < n; ++i, ++s->pos) d[0][i] = d[1][i] = s->pos < s->x->size() ? (*s->x)[s->pos] : 0.f;
        return -1;
      }
    };
    stretch::Voice v;
    v.prepare(2, sr, stretch::Mode::Mono, 4.0, 4.0);
    Src src{&t};
    v.start(&Src::read, &src, 1.0, 1.0);
    Sig L(static_cast<size_t>(2 * sr)), R(L.size());
    for (size_t at = 0; at < L.size(); at += 256) {
      const double p = std::pow(2.0, std::sin(2 * kPi * 5 * static_cast<double>(at) / sr) / 12.0);
      float* o[2] = {L.data() + at, R.data() + at};
      v.render(o, static_cast<int>(std::min<size_t>(256, L.size() - at)), 1.0, p, 0, &Src::read, &src);
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

    // The hybrid voice: no allocation, the same output for any block size.
    std::vector<double> hs;
    const Sig bk = bassKick(sr, 3.0, &hs);
    SamplePtr smp = sampleOf(bk, sr);
    SamplePtr hp = stretch::harmonicOf(smp, stretch::onsetSensitivity(stretch::Mode::Auto));
    auto on = stretch::onsetsOf(smp, stretch::onsetSensitivity(stretch::Mode::Auto));
    expect(hp && hp->frames() == smp->frames() && !on->empty(), "harmonicOf: the harmonic part of a sample, at its onsets");
    Sig outs[2];
    long allocs = 0;
    for (int pass = 0; pass < 2; ++pass) {
      stretch::Voice hv;
      hv.prepare(2, sr, stretch::Mode::Auto, 4.0, 4.0);
      HybridSrc hsrc{&bk, hp.get(), on.get()};
      Sig o(static_cast<size_t>(4 * sr)), o2(o.size());
      CountScope cs;
      hv.start(&HybridSrc::read, &hsrc, 0.6, 1.1, true, true);
      for (size_t at = 0; at < o.size();) {
        const int n = static_cast<int>(std::min<size_t>(pass ? 61 : 512, o.size() - at));
        float* op[2] = {o.data() + at, o2.data() + at};
        hv.render(op, n, 0.6, 1.1, 0, &HybridSrc::read, &hsrc);
        at += static_cast<size_t>(n);
      }
      allocs += cs.n();
      outs[pass] = o;
    }
    float vd = 0;
    for (size_t i = 0; i < outs[0].size(); ++i) vd = std::max(vd, std::fabs(outs[0][i] - outs[1][i]));
    expect(allocs == 0 && vd < 1e-5f && rms(outs[0], 0, outs[0].size()) > 0.05,
           "hybrid voice: start and render allocate nothing; the same output for any block size");

    // A note started inside the sample (a seek) is right from its first block: full level at once.
    const Sig tone = sine(220, static_cast<size_t>(2 * sr), sr, 0.5);
    const std::string tpath = writeTmp("gl_j145_tone.wav", tone, sr);
    bool firstOk = true;
    for (stretch::Mode m : {stretch::Mode::Auto, stretch::Mode::Mono, stretch::Mode::Beats}) {
      const Sig s = sampler(tpath, m, 1.25, static_cast<size_t>(0.2 * sr), sr, 64, 0.3f);
      const auto per = static_cast<size_t>(2 * sr / 220);  // two periods
      const double r0 = rms(s, 0, per), r1 = rms(s, static_cast<size_t>(0.05 * sr), static_cast<size_t>(0.05 * sr) + 10 * per);
      if (!(r0 > 0.7 * r1 && r1 > 0.25)) {
        firstOk = false;
        std::printf("# %s started at 30 %%: first 5 ms %.3f, later %.3f; ms:", name(m), r0, r1);
        for (int k = 0; k < 12; ++k) std::printf(" %.3f", rms(s, k * per / 2, (k + 1) * per / 2));
        std::printf("\n");
      }
    }
    expect(firstOk, "sampler voices: a note started inside the sample plays at full level from its first frames");

    // The end of a stretched note fades out (no hard cut while the engine still sounds).
    const Sig shortTone = sine(220, static_cast<size_t>(0.5 * sr), sr, 0.5);
    const std::string spath = writeTmp("gl_j145_short.wav", shortTone, sr);
    bool tailOk = true;
    for (stretch::Mode m : {stretch::Mode::Auto, stretch::Mode::Mono, stretch::Mode::Beats}) {
      const Sig s = sampler(spath, m, 1.5, static_cast<size_t>(1.5 * sr), sr);
      size_t last = 0;
      for (size_t i = 0; i < s.size(); ++i)
        if (s[i] != 0.f) last = i;
      const double end = rms(s, last > 32 ? last - 32 : 0, last + 1), body = rms(s, static_cast<size_t>(0.2 * sr), static_cast<size_t>(0.4 * sr));
      if (!(end < 0.05 * body)) {
        tailOk = false;
        std::printf("# %s: the last frames %.4f against %.4f\n", name(m), end, body);
      }
    }
    expect(tailOk, "sampler voices: a stretched note ends with a fade, not a cut");
  }

  // ============================================================ clip playback: render <-> grains
  {
    const double sr = 44100;
    const Sig tone = sine(220, static_cast<size_t>(4 * sr), sr, 0.5);
    const std::string path = writeTmp("gl_j145_cliptone.wav", tone, sr);
    SamplePtr smp = sampleCache().get(path);
    TempoMap map;
    map.setConstant(120, 96);
    ProgAudioClip c;
    c.start = 0, c.end = 192 * 6;
    c.startSec = 0, c.durSec = map.secondsAt(static_cast<double>(c.end));
    c.sample = smp, c.srcStart = 0, c.srcEnd = smp->seconds(), c.speed = 0.8, c.granular = true;
    stretch::ClipRequest rq;
    rq.sample = smp, rq.srcEnd = c.srcEnd, rq.mode = stretch::Mode::Auto, rq.rate = sr, rq.speed = 0.8;
    rq.frames = static_cast<int64_t>(std::ceil(c.durSec * sr)) + 2;
    c.hq = stretch::renderClip(rq, false);
    // The channel pitch moves off the render's pitch (0) for 0.5 s and back: grains, then the render again.
    const std::vector<ProgAudioClip> clips{c};  // the engine keeps its clips (and their playback state)
    const int64_t total = static_cast<int64_t>(2.5 * sr), off0 = static_cast<int64_t>(1.0 * sr), off1 = static_cast<int64_t>(1.5 * sr);
    Sig L(static_cast<size_t>(total), 0.f), R(L.size(), 0.f);
    for (int64_t at = 0; at < total; at += 512) {
      const int len = static_cast<int>(std::min<int64_t>(512, total - at));
      const double semis = at >= off0 && at < off1 ? 0.02 : 0.0;  // 2 cents: the same sound, another engine
      renderAudioClips(clips, map, sr, 0, at, 0, len, L.data() + at, R.data() + at, semis);
    }
    const double base = maxCurve(L, static_cast<size_t>(0.3 * sr), static_cast<size_t>(0.9 * sr));
    const double sw = std::max(maxCurve(L, static_cast<size_t>(off0 - 600), static_cast<size_t>(off0 + 2000)),
                               maxCurve(L, static_cast<size_t>(off1 - 600), static_cast<size_t>(off1 + 2000)));
    std::printf("# clip render <-> grains switch: curvature %.4f against %.4f in steady play\n", sw, base);
    {
      double mx = 0;
      size_t at = 0;
      for (size_t i = static_cast<size_t>(0.9 * sr); i < static_cast<size_t>(2.0 * sr); ++i) {
        const double v = std::fabs(L[i] - 2 * L[i - 1] + L[i - 2]);
        if (v > mx) mx = v, at = i;
      }
      std::printf("#   worst at %.4f s (switches at %.4f, %.4f):", static_cast<double>(at) / sr, static_cast<double>(off0) / sr, static_cast<double>(off1) / sr);
      for (size_t i = at - 4; i < at + 4; ++i) std::printf(" %.4f", L[i]);
      std::printf("\n");
    }
    expect(c.hq && sw < 2.0 * base, "clip playback: switching between the render and the realtime grains crossfades (no click)");
  }

  // ============================================================ level follower: the peak limit ramps
  {
    // The follower boosts a quiet block (+12 dB), then a block arrives whose
    // source peak allows no boost. The gain used to drop to the limit at that
    // block's first frame (a step: a click); now it ramps, ahead of the frames
    // that need it, and a lone spike inside a block is held to the room.
    const double sr = 44100;
    for (int pass = 0; pass < 2; ++pass) {
      pitch::LevelFollower f;
      f.setRate(sr);
      f.reset();
      const int m = 256;
      std::vector<float> g;  // the gain per frame (the input is a constant 0.1)
      double spikeOut = 0, spikeRoom = 0;
      for (int b = 0; b < 60; ++b) {
        float buf[2][m];
        for (int c = 0; c < 2; ++c) std::fill(buf[c], buf[c] + m, 0.1f);
        const bool spike = pass == 1 && b == 40;
        if (spike) buf[0][180] = buf[1][180] = 0.5f;
        float* y[2] = {buf[0], buf[1]};
        // pass 0: from block 30 the source peak equals the block's (no room for a boost);
        // pass 1: room for the boost on the bed, but not on the spike of block 40.
        const double lim = pass == 0 ? (b < 30 ? 10.0 : 1.0 / 1.4125) : (spike ? 1.0 : 10.0);
        f.apply(y, 2, m, 0.16, 0.01, lim);
        if (spike) spikeOut = buf[0][180], spikeRoom = 1.4125 * lim * 0.5;
        for (int j = 0; j < m; ++j)
          if (j != 180 || !spike) g.push_back(buf[0][j] / 0.1f);
      }
      double step = 0;
      for (size_t i = 1; i < g.size(); ++i) step = std::max(step, std::fabs(static_cast<double>(g[i]) - g[i - 1]));
      const double before = g[29 * m + m - 1], after = g.back();
      std::printf("# level follower pass %d: gain %.2f -> %.2f, largest change per frame %.4f%s\n", pass, before, after, step,
                  pass ? "" : " (limited from block 30)");
      if (pass == 0) {
        expect(before > 3.0 && after < 1.01 && step < 1.05 / 44.1, "level follower: a falling peak limit ramps the gain down (no step), then holds it");
      } else {
        std::printf("# level follower spike: %.3f against a room of %.3f\n", spikeOut, spikeRoom);
        expect(spikeOut <= spikeRoom * 1.001 && step < 1.05 / 44.1, "level follower: a spike inside a block is held to the room, the gain ramping ahead of it");
      }
    }
  }

  // ============================================================ determinism, stereo
  {
    const double sr = 48000;
    std::vector<double> hs;
    const Sig bk = bassKick(sr, 2.0, &hs);
    bool same = true;
    for (stretch::Mode m : onsetModes) {
      const Sig a = offline1(bk, m, std::pow(2.0, 2 / 12.0), 1.3, sr), b = offline1(bk, m, std::pow(2.0, 2 / 12.0), 1.3, sr);
      same = same && a == b;
    }
    expect(same, "offline onset modes: renders are deterministic (bit-identical)");
    // Stereo: a centred bass, hats left and right from different noise: the correlation stays.
    Sig l = bk, r = bk;
    uint32_t a = 5, b = 17;
    for (double h = 0.1; h < 1.9; h += 0.125) {
      const auto at = static_cast<size_t>(h * sr);
      for (size_t i = 0; i < static_cast<size_t>(0.05 * sr) && at + i < l.size(); ++i) {
        a = a * 1664525u + 1013904223u, b = b * 22695477u + 1u;
        const double e = 0.3 * std::exp(-static_cast<double>(i) / (0.008 * sr));
        l[at + i] += static_cast<float>(e * ((static_cast<double>(a >> 8) / 8388608.0) - 1.0));
        r[at + i] += static_cast<float>(e * ((static_cast<double>(b >> 8) / 8388608.0) - 1.0));
      }
    }
    const double cin = corr(l, r, 0, l.size());
    double worst = 0;
    for (stretch::Mode m : onsetModes) {
      const auto y = offline({l, r}, m, 1.0, 1.6, sr);
      worst = std::max(worst, std::fabs(corr(y[0], y[1], 0, y[0].size()) - cin));
    }
    std::printf("# stereo correlation %.3f in, worst change %.3f\n", cin, worst);
    expect(worst < 0.05, "offline onset modes: the stereo correlation stays (within 0.05)");
  }

  // ============================================================ the map's ends stay put
  // A sound that starts on a hit starts on time: the smoothing of the hit
  // spans' time map must not move its first frame (a smoothed map once read a
  // clip 20 frames late when a second hit followed 0.11 s later).
  {
    const double sr = 44100;
    Sig x(static_cast<size_t>(0.5 * sr), 0.f);
    x[0] = 1.0f, x[4851] = 1.0f;
    int worst = 0;
    for (stretch::Mode m : {stretch::Mode::Auto, stretch::Mode::Transient, stretch::Mode::Beats}) {
      const Sig y = offline1(x, m, 1.0, 2.0, sr);
      size_t at = 0;
      while (at < y.size() && std::fabs(y[at]) < 0.5f) ++at;
      worst = std::max(worst, static_cast<int>(at));
    }
    std::printf("# a hit on the first frame starts at frame %d (worst mode)\n", worst);
    expect(worst <= 2, "offline onset modes: a hit on the first frame stays on the first frame");
  }

  return finish();
}
