// Gridline — J-87: time stretching and pitch shifting (engine/TimeStretch on
// Signalsmith Stretch, MIT).
//  * pitch accuracy within ±2 cents: the offline engine (every mode, low and
//    high notes), playlist audio clips, sampler voices, the audio editor, the
//    Pitch Shifter's Hi-fi engine;
//  * exact lengths and aligned ends (offline stretch, the editor, slices);
//  * transients: drum hits stay sharp and are not smeared ahead of
//    themselves — measurably better than the previous realtime grains;
//  * no clicks at clip starts / ends, song loops or sampler loops;
//  * block-size invariance (clip playback, stretch voices, the sampler);
//  * Formant keep keeps the spectral envelope where plain pitch moves it;
//  * FL stretch methods ↔ modes, and a Gridline → FLP → Gridline round trip of
//    every time mode with its pitch and time values;
//  * realtime safety: no allocation in clip playback, stretch voices, the
//    sampler's stretch voices or the Pitch Shifter's Hi-fi engine.
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <new>
#include <string>
#include <vector>
#include "../src/core/PluginInfo.h"
#include "../src/core/PluginParams.h"
#include "../src/core/Project.h"
#include "../src/core/WavWriter.h"
#include "../src/core/flp/Flp.h"
#include "../src/core/flp/FlpExport.h"
#include "../src/engine/Effects.h"
#include "../src/engine/Generators.h"
#include "../src/engine/Program.h"
#include "../src/engine/Render.h"
#include "../src/engine/Sample.h"
#include "../src/engine/SliceProcess.h"
#include "../src/engine/TimeStretch.h"
#include "../src/packs/edison/edison.h"
#include "../src/packs/spectral/spectral.h"
#include "Check.h"

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
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

using namespace gl;
namespace fs = std::filesystem;

namespace {

constexpr double kSr = 44100.0;
constexpr double kPi = 3.14159265358979323846;

struct CountScope {
  CountScope() { gAllocs = 0, gCount = true; }
  ~CountScope() { gCount = false; }
  long n() const { return gAllocs.load(); }
};

std::vector<float> tone(double f, double sec, double amp = 0.5) {
  std::vector<float> x(static_cast<size_t>(sec * kSr));
  for (size_t i = 0; i < x.size(); ++i) x[i] = static_cast<float>(amp * std::sin(2 * kPi * f * static_cast<double>(i) / kSr));
  return x;
}

// Drum-like hits every 0.25 s from 0.05 s (kick / noise hits, fast decays).
std::vector<double> hitTimes(double sec) {
  std::vector<double> t;
  for (double x = 0.05; x < sec - 0.2; x += 0.25) t.push_back(x);
  return t;
}
std::vector<float> drums(double sec) {
  std::vector<float> x(static_cast<size_t>(sec * kSr), 0.f);
  uint32_t rng = 12345;
  int k = 0;
  for (double t0 : hitTimes(sec)) {
    const bool kick = k++ % 2 == 0;
    for (size_t i = static_cast<size_t>(t0 * kSr); i < x.size() && i < static_cast<size_t>((t0 + 0.2) * kSr); ++i) {
      const double t = static_cast<double>(i) / kSr - t0;
      rng = rng * 1664525u + 1013904223u;
      const double n = (static_cast<double>(rng >> 8) / 8388608.0) - 1.0;
      x[i] += static_cast<float>(kick ? 0.9 * std::exp(-t / 0.035) * std::sin(2 * kPi * (50 * t + 3.3 * (1 - std::exp(-t / 0.03))))
                                      : 0.5 * std::exp(-t / 0.02) * n);
    }
  }
  return x;
}

std::string writeTmp(const std::string& name, const std::vector<float>& l, const std::vector<float>* r = nullptr) {
  AudioBuffer b(2, l.size());
  for (size_t i = 0; i < l.size(); ++i) b.at(0, i) = l[i], b.at(1, i) = r ? (*r)[i] : l[i];
  const std::string path = (fs::temp_directory_path() / name).string();
  writeWav(path, b, static_cast<int>(kSr), 24);
  sampleCache().clear();
  return path;
}

using cd = std::complex<double>;
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
double bh(size_t i, size_t n) {
  const double x = 2 * kPi * static_cast<double>(i) / static_cast<double>(n);
  return 0.35875 - 0.48829 * std::cos(x) + 0.14128 * std::cos(2 * x) - 0.01168 * std::cos(3 * x);
}
// Strongest partial within ±6 % of `near` in x[a, a + 32768), interpolated.
double peakHz(const float* x, size_t a, double near) {
  const size_t N = 32768;
  std::vector<cd> b(N);
  for (size_t i = 0; i < N; ++i) b[i] = x[a + i] * bh(i, N);
  fft(b);
  const size_t k0 = static_cast<size_t>(near / 1.06 * N / kSr), k1 = static_cast<size_t>(near * 1.06 * N / kSr);
  size_t best = k0;
  for (size_t k = k0; k <= k1; ++k)
    if (std::abs(b[k]) > std::abs(b[best])) best = k;
  const double ym = std::log(std::abs(b[best - 1])), y0 = std::log(std::abs(b[best])), yp = std::log(std::abs(b[best + 1]));
  return (static_cast<double>(best) + 0.5 * (ym - yp) / (ym - 2 * y0 + yp)) * kSr / static_cast<double>(N);
}
double cents(double f, double ref) { return 1200 * std::log2(f / ref); }

double rms(const float* x, size_t n) {
  double e = 0;
  for (size_t i = 0; i < n; ++i) e += static_cast<double>(x[i]) * x[i];
  return std::sqrt(e / static_cast<double>(std::max<size_t>(1, n)));
}

// Transient quality of hits expected at `times` (seconds) in x: the mean
// pre-echo (dB: the loudest 0.5 ms in the 30 … 3 ms before each hit,
// relative to the hit's peak) and the mean 10 → 90 % rise time (ms).
struct Hits {
  double preDb = 0, riseMs = 0;
};
Hits hits(const std::vector<float>& x, const std::vector<double>& times) {
  const size_t w = static_cast<size_t>(0.0005 * kSr);
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
    const long c = std::lround(t * kSr);
    if (c < static_cast<long>(0.04 * kSr) || c + static_cast<long>(0.03 * kSr) >= static_cast<long>(x.size())) continue;
    double pk = 0;
    long at = c;
    for (long i = c - static_cast<long>(0.004 * kSr); i < c + static_cast<long>(0.03 * kSr); ++i)
      if (const double e = env(i); e > pk) pk = e, at = i;
    long i90 = at;
    while (i90 > c - static_cast<long>(0.03 * kSr) && env(i90) > 0.9 * pk) --i90;
    long i10 = i90;
    while (i10 > c - static_cast<long>(0.06 * kSr) && env(i10) > 0.1 * pk) --i10;
    double pre = 0;
    for (long i = c - static_cast<long>(0.03 * kSr); i < c - static_cast<long>(0.003 * kSr); ++i) pre = std::max(pre, env(i));
    h.preDb += 20 * std::log10(pre / pk + 1e-6);
    h.riseMs += static_cast<double>(i90 - i10) / kSr * 1000;
    ++n;
  }
  if (n) h.preDb /= n, h.riseMs /= n;
  return h;
}

// The largest second difference |x[i] − 2x[i−1] + x[i−2]| over [a, b) (clicks).
double maxCurve(const float* x, size_t a, size_t b) {
  double m = 0;
  for (size_t i = std::max<size_t>(a, 2); i < b; ++i) m = std::max(m, static_cast<double>(std::fabs(x[i] - 2 * x[i - 1] + x[i - 2])));
  return m;
}

// One sampler channel (time mode `mode`) holding `path`, audio clips added by the caller.
Song clipSong(const std::string& path, int mode, double tempo = 120) {
  Song s = makeEmptySong();
  s.tempo = tempo;
  Channel ch;
  ch.id = s.allocateId();
  ch.name = "Audio";
  ch.plugin = defaultPluginState("gl.sampler");
  ch.plugin.data = path;
  ch.plugin.params[pp::SmpStretchMode] = static_cast<float>(mode);
  ch.mixerTrack = s.masterId();
  ch.audioClip = true;
  s.channels.push_back(ch);
  return s;
}
Clip audioClip(Song& s, Tick start, Tick len, float cents = 0, double stretch = 1) {
  Clip c;
  c.id = s.allocateId();
  c.kind = ClipKind::Audio;
  c.source = s.channels.back().id;
  c.start = start;
  c.length = len;
  c.pitch = cents;
  c.stretch = stretch;
  return c;
}
std::vector<float> left(const AudioBuffer& b) { return std::vector<float>(b.plane(0), b.plane(0) + b.frames); }

}  // namespace

int main() {
  // ------------------------------------------------------------ modes, FL mapping
  {
    expect(stretch::modeOf(0) == stretch::Mode::Resample && stretch::modeOf(1) == stretch::Mode::Auto &&
               stretch::modeOf(4) == stretch::Mode::Formant && stretch::modeOf(99) == stretch::Mode::Formant &&
               stretch::modeOf(-3) == stretch::Mode::Resample && stretch::modeOf(NAN) == stretch::Mode::Resample,
           "time modes: parameter values clamp to the five modes");
    bool all = true;
    for (int m = 0; m < static_cast<int>(stretch::Mode::Count); ++m)
      all = all && stretch::modeFromFl(stretch::flCodeOf(static_cast<stretch::Mode>(m))) == static_cast<stretch::Mode>(m);
    expect(all, "every mode exports as an FL method that imports back as the same mode");
    expect(stretch::modeFromFl(0) == stretch::Mode::Resample && stretch::modeFromFl(-1) == stretch::Mode::Auto &&
               stretch::modeFromFl(5) == stretch::Mode::Auto && stretch::modeFromFl(2) == stretch::Mode::Mono &&
               stretch::modeFromFl(9) == stretch::Mode::Mono && stretch::modeFromFl(3) == stretch::Mode::Transient &&
               stretch::modeFromFl(7) == stretch::Mode::Transient && stretch::modeFromFl(-2) == stretch::Mode::Formant,
           "FL methods map to the closest mode (stretch / auto → Auto, mono / speech → Mono, slice / transient → Transient, pro → Formant)");
  }

  // ------------------------------------------------------------ offline engine: pitch accuracy
  {
    double worst = 0;
    for (stretch::Mode m : {stretch::Mode::Auto, stretch::Mode::Mono, stretch::Mode::Transient, stretch::Mode::Formant})
      for (double f : {110.0, 440.0, 1500.0})
        for (double st : {-12.0, -5.0, -1.0, 1.0, 5.0, 12.0}) {
          std::vector<float> x = tone(f, 2.0), y(x.size());
          const float* in[1] = {x.data()};
          float* out[1] = {y.data()};
          stretch::Settings s;
          s.mode = m;
          s.pitch = std::pow(2.0, st / 12.0);
          stretch::process(in, 1, static_cast<int64_t>(x.size()), out, static_cast<int64_t>(y.size()), kSr, s);
          worst = std::max(worst, std::fabs(cents(peakHz(y.data(), y.size() / 2 - 16384, f * s.pitch), f * s.pitch)));
        }
    char name[160];
    std::snprintf(name, sizeof name, "offline engine: pitch within 2 cents in every mode, 110 Hz … 1.5 kHz, ±1 … ±12 st (worst %.3f ct)", worst);
    expect(worst < 2.0, name);
  }

  // ------------------------------------------------------------ exact lengths, aligned ends
  {
    bool exact = true, aligned = true;
    for (int64_t n : {int64_t(300), int64_t(5000), int64_t(44100), int64_t(100000)})
      for (double f : {0.5, 0.8, 1.25, 2.0}) {
        std::vector<float> x(static_cast<size_t>(n), 0.f);
        x[static_cast<size_t>(n / 2)] = 1.0f;  // a click in the middle
        const int64_t outLen = std::llround(static_cast<double>(n) * f);
        std::vector<float> y(static_cast<size_t>(outLen) + 8, 7.0f);
        const float* in[1] = {x.data()};
        float* out[1] = {y.data()};
        stretch::Settings s;
        s.mode = stretch::Mode::Transient;
        stretch::process(in, 1, n, out, outLen, kSr, s);
        exact = exact && y[static_cast<size_t>(outLen)] == 7.0f;  // nothing written past the length
        if (n >= 44100) {
          size_t at = 0;
          for (size_t i = 0; i < static_cast<size_t>(outLen); ++i)
            if (std::fabs(y[i]) > std::fabs(y[at])) at = i;
          aligned = aligned && std::fabs(static_cast<double>(at) - static_cast<double>(n / 2) * f) < 0.002 * kSr;
        }
      }
    expect(exact, "offline engine: output exactly the asked length (short and long inputs, ×0.5 … ×2)");
    expect(aligned, "offline engine: aligned (a click in the middle lands in the middle, within 2 ms)");
    packs::edison::Audio a;
    a.rate = kSr, a.channels = 2, a.L = tone(330, 1.3), a.R = a.L;
    bool ed = true;
    for (int m : {0, 1, 3, 4})
      for (double tf : {0.7, 1.0, 1.6}) {
        packs::edison::Audio b = packs::edison::stretchAudio(a, tf, 3, static_cast<packs::edison::Stretch>(m), 1);
        ed = ed && static_cast<int64_t>(b.L.size()) == std::llround(static_cast<double>(a.L.size()) * tf) && b.R.size() == b.L.size();
      }
    expect(ed, "audio editor: every method gives the exact length");
    const std::vector<float> d = drums(1.0);
    const float* in[1] = {d.data()};
    auto sl = sliceproc::stretch(in, 1, static_cast<int64_t>(d.size()), 52919, 1.0, kSr, stretch::Mode::Transient);
    expect(sl[0].size() == 52919, "slicer: exact slice length");
  }

  // ------------------------------------------------------------ playlist clips: pitch, length, sync
  {
    const std::string path = writeTmp("gl_j87_tone.wav", tone(220, 4.0));
    double worst = 0;
    for (int mode : {1, 2, 3, 4})
      for (double st : {-12.0, -5.0, 5.0, 12.0}) {
        Song s = clipSong(path, mode);
        s.playlist.push_back(audioClip(s, 0, 192 * 3, static_cast<float>(st * 100)));
        RenderOptions o;
        std::vector<float> y = left(renderSong(s, o));
        worst = std::max(worst, std::fabs(cents(peakHz(y.data(), 44100, 220 * std::pow(2.0, st / 12)), 220 * std::pow(2.0, st / 12))));
      }
    char name[160];
    std::snprintf(name, sizeof name, "audio clips: pitch within 2 cents in every stretch mode, ±5 / ±12 st (worst %.3f ct)", worst);
    expect(worst < 2.0, name);
    // Resample mode: pitch through the band-limited resampler, exact too.
    Song r = clipSong(path, 0);
    r.playlist.push_back(audioClip(r, 0, 192 * 2, 700));
    RenderOptions o;
    std::vector<float> y = left(renderSong(r, o));
    expect(std::fabs(cents(peakHz(y.data(), 22050, 220 * std::pow(2.0, 7 / 12.0)), 220 * std::pow(2.0, 7 / 12.0))) < 2.0,
           "audio clips: resample mode in tune");
    // Tempo sync: a 120 BPM clip in a 100 BPM song lasts 1.2× as long, same pitch, audio to the end.
    Song t = clipSong(path, 1, 100);
    Clip c = audioClip(t, 0, 0);
    c.syncBpm = 120;
    c.length = 96 * 4 * 2;  // two bars = 4.8 s at 100 BPM, 4 s of audio at 120
    t.playlist.push_back(c);
    std::vector<float> z = left(renderSong(t, o));
    const size_t endAt = static_cast<size_t>(4.8 * kSr);
    expect(z.size() >= endAt && rms(z.data() + endAt - 8000, 6000) > 0.3 && std::fabs(cents(peakHz(z.data(), 44100, 220), 220)) < 2.0,
           "audio clips: tempo sync stretches to the song tempo, pitch kept, audio to the clip end");
  }

  // ------------------------------------------------------------ transients: better than the realtime grains
  {
    const double sec = 3.0;
    const std::vector<float> d = drums(sec);
    const std::string path = writeTmp("gl_j87_drums.wav", d);
    SamplePtr smp = sampleCache().get(path);
    TempoMap map;
    map.setConstant(120, 96);
    for (double f : {1.25, 2.0}) {
      ProgAudioClip c;
      c.start = 0;
      c.end = static_cast<Tick>(std::llround(sec * f * 192));
      c.startSec = 0;
      c.durSec = map.secondsAt(static_cast<double>(c.end));
      c.sample = smp;
      c.srcStart = 0, c.srcEnd = smp->seconds();
      c.speed = 1.0 / f;
      c.granular = true;
      const int64_t frames = static_cast<int64_t>(std::ceil(c.durSec * kSr));
      auto play = [&](const ProgAudioClip& clip) {
        std::vector<float> L(static_cast<size_t>(frames), 0.f), R(L.size(), 0.f);
        for (int64_t at = 0; at < frames; at += 512) {
          const int len = static_cast<int>(std::min<int64_t>(512, frames - at));
          renderAudioClips({clip}, map, kSr, 0, at, 0, len, L.data() + at, R.data() + at);
        }
        return L;
      };
      const std::vector<float> legacy = play(c);  // no render: the realtime grains
      stretch::ClipRequest rq;
      rq.sample = smp, rq.srcStart = c.srcStart, rq.srcEnd = c.srcEnd, rq.mode = stretch::Mode::Auto;
      rq.rate = kSr, rq.frames = frames + 2, rq.speed = c.speed;
      c.hq = stretch::renderClip(rq, false);
      const std::vector<float> hq = play(c);
      std::vector<double> times;
      for (double t : hitTimes(sec)) times.push_back(t * f);
      const Hits a = hits(legacy, times), b = hits(hq, times);
      char name[200];
      std::snprintf(name, sizeof name, "drums ×%.2f: pre-echo %.1f → %.1f dB, rise %.2f → %.2f ms (sharper, nothing ahead of the hit)", f, a.preDb,
                    b.preDb, a.riseMs, b.riseMs);
      expect(c.hq && b.preDb < -50 && b.preDb < a.preDb - 20 && b.riseMs < 3.0 && b.riseMs < a.riseMs * 0.5, name);
    }
    // The sampler's realtime stretch voices restart at the onsets: no pre-echo either.
    PluginState st = defaultPluginState("gl.sampler");
    st.data = path;
    st.params[pp::SmpStretchMode] = static_cast<float>(stretch::Mode::Transient);
    st.params[pp::SmpTimeStretch] = 1.25f;
    ChannelAux aux;
    prepareChannelAux(st, aux);
    auto gen = createGenerator("gl.sampler");
    gen->prepareHost(st, kSr);
    ProcessContext ctx;
    ctx.sampleRate = kSr;
    const size_t total = static_cast<size_t>(sec * 1.25 * kSr);
    std::vector<float> L(total, 0.f), R(total, 0.f);
    gen->noteOn(1, 60, 1.0f, 0);
    for (size_t i = 0; i < total; i += 256) {
      const int n = static_cast<int>(std::min<size_t>(256, total - i));
      gen->process(st, aux, ctx, L.data() + i, R.data() + i, n);
    }
    std::vector<double> times;
    for (double t : hitTimes(sec)) times.push_back(t * 1.25);
    const Hits h = hits(L, times);
    char name[160];
    std::snprintf(name, sizeof name, "sampler stretch voice ×1.25 (transient): pre-echo %.1f dB, rise %.2f ms", h.preDb, h.riseMs);
    expect(h.preDb < -50 && h.riseMs < 3.0, name);
  }

  // ------------------------------------------------------------ sampler voices: pitch, block size, loops
  {
    const std::string path = writeTmp("gl_j87_tone2.wav", tone(220, 3.0));
    double worst = 0;
    bool invariant = true;
    for (int mode : {1, 2, 4})
      for (double st : {-12.0, -5.0, 7.0, 12.0}) {
        PluginState s = defaultPluginState("gl.sampler");
        s.data = path;
        s.params[pp::SmpStretchMode] = static_cast<float>(mode);
        ChannelAux aux;
        prepareChannelAux(s, aux);
        ProcessContext ctx;
        ctx.sampleRate = kSr;
        std::vector<float> outs[2];
        for (int pass = 0; pass < 2; ++pass) {
          auto gen = createGenerator("gl.sampler");
          gen->prepareHost(s, kSr);
          const size_t total = static_cast<size_t>(2.0 * kSr);
          std::vector<float> L(total, 0.f), R(total, 0.f);
          gen->noteOn(1, 60 + st, 1.0f, 0);
          const int B = pass == 0 ? 64 : 333;
          for (size_t i = 0; i < total; i += static_cast<size_t>(B)) {
            const int n = static_cast<int>(std::min<size_t>(static_cast<size_t>(B), total - i));
            gen->process(s, aux, ctx, L.data() + i, R.data() + i, n);
          }
          outs[pass] = L;
        }
        for (size_t i = 0; i < outs[0].size(); ++i) invariant = invariant && std::fabs(outs[0][i] - outs[1][i]) < 1e-6f;
        const double want = 220 * std::pow(2.0, st / 12.0);
        worst = std::max(worst, std::fabs(cents(peakHz(outs[0].data(), 22050, want), want)));
      }
    char name[160];
    std::snprintf(name, sizeof name, "sampler stretch voices: notes in tune within 2 cents (worst %.3f ct)", worst);
    expect(worst < 2.0, name);
    expect(invariant, "sampler stretch voices: identical output for any block size");
    // Forward loop in a stretch mode: no click at the loop point (the voice
    // reads a continuous loop: 10 whole periods of a 225 Hz tone, 1960 frames).
    const std::string loopPath = writeTmp("gl_j87_tone225.wav", tone(225, 3.0));
    PluginState s = defaultPluginState("gl.sampler");
    s.data = loopPath;
    s.params[pp::SmpStretchMode] = 1;
    s.params[pp::SmpLoop] = 1;
    s.params[pp::SmpOneShot] = 0;
    s.params[pp::SmpLoopStart] = 0.0f;
    s.params[pp::SmpLoopEnd] = static_cast<float>(1960.5 / 132299.0);
    ChannelAux aux;
    prepareChannelAux(s, aux);
    auto gen = createGenerator("gl.sampler");
    gen->prepareHost(s, kSr);
    ProcessContext ctx;
    ctx.sampleRate = kSr;
    std::vector<float> L(44100, 0.f), R(44100, 0.f);
    gen->noteOn(1, 64, 1.0f, 0);
    for (size_t i = 0; i < L.size(); i += 256) gen->process(s, aux, ctx, L.data() + i, R.data() + i, static_cast<int>(std::min<size_t>(256, L.size() - i)));
    const double f = 225 * std::pow(2.0, 4 / 12.0), bound = 0.5 * 0.8 * std::pow(2 * kPi * f / kSr, 2);
    expect(maxCurve(L.data(), 4410, L.size()) < bound * 4, "sampler stretch voice: looping without clicks");
  }

  // ------------------------------------------------------------ clicks at clip edges and song loops
  {
    const std::string path = writeTmp("gl_j87_tone3.wav", tone(330, 4.0, 0.6));
    Song s = clipSong(path, 1);
    // Two clips cut into the audio (start offset, cut end), pitched, back to back.
    Clip a = audioClip(s, 0, 192, 300);
    a.audioStart = 0.5;
    Clip b = audioClip(s, 192, 192, -200, 1.3);
    b.audioStart = 1.1;
    s.playlist.push_back(a);
    s.playlist.push_back(b);
    RenderJob job;
    job.loops = 2;  // the song wraps: the clip starts again
    RenderResult r = renderAudio(s, job);
    const std::vector<float> y = left(r.audio);
    const double fmax = 330 * std::pow(2.0, 3 / 12.0);
    const double bound = 0.6 * 0.8 * std::pow(2 * kPi * fmax / kSr, 2);
    expect(!y.empty() && maxCurve(y.data(), 0, y.size()) < bound * 6, "pitched / stretched clips: no clicks at clip starts, ends and the song loop");
  }

  // ------------------------------------------------------------ block-size invariance of clip playback
  {
    const std::string path = writeTmp("gl_j87_drums2.wav", drums(2.0));
    Song s = clipSong(path, 3);
    s.playlist.push_back(audioClip(s, 10, 300, 500, 1.4));
    RenderOptions o;
    o.blockSize = 64;
    AudioBuffer a = renderSong(s, o);
    o.blockSize = 4096;
    AudioBuffer b = renderSong(s, o);
    bool same = a.frames == b.frames;
    for (size_t i = 0; same && i < a.data.size(); ++i) same = a.data[i] == b.data[i];
    expect(same, "clip renders: identical for any block size");
    // The rendered clip read through the engine in odd splits.
    SamplePtr smp = sampleCache().get(path);
    TempoMap map;
    map.setConstant(120, 96);
    ProgAudioClip c;
    c.start = 7, c.end = 300;
    c.startSec = map.secondsAt(7), c.durSec = map.secondsAt(300) - c.startSec;
    c.sample = smp, c.srcStart = 0, c.srcEnd = smp->seconds(), c.speed = 0.8, c.granular = true;
    stretch::ClipRequest rq;
    rq.sample = smp, rq.srcEnd = c.srcEnd, rq.mode = stretch::Mode::Auto, rq.rate = kSr, rq.speed = 0.8;
    rq.frames = static_cast<int64_t>(std::ceil(c.durSec * kSr)) + 2;
    c.hq = stretch::renderClip(rq, false);
    const int64_t total = static_cast<int64_t>(2 * kSr);
    std::vector<float> outs[2];
    for (int pass = 0; pass < 2; ++pass) {
      std::vector<float> L(static_cast<size_t>(total), 0.f), R(L.size(), 0.f);
      const int B = pass ? 97 : 1024;
      for (int64_t at = 0; at < total; at += B) {
        const int len = static_cast<int>(std::min<int64_t>(B, total - at));
        renderAudioClips({c}, map, kSr, 0, at, 0, len, L.data() + at, R.data() + at);
      }
      outs[pass] = L;
    }
    expect(outs[0] == outs[1] && rms(outs[0].data(), outs[0].size()) > 0.01, "clip playback: rendered clips read identically for any split");
    // Stretch voices: identical for any render split.
    struct Src {
      std::vector<float> x;
      size_t pos = 0;
      static int read(void* ctx, float* const* d, int n, bool) {
        auto* s = static_cast<Src*>(ctx);
        for (int i = 0; i < n; ++i) d[0][i] = d[1][i] = s->pos < s->x.size() ? s->x[s->pos++] : 0.f;
        return -1;
      }
    };
    std::vector<float> vo[2];
    for (int pass = 0; pass < 2; ++pass) {
      stretch::Voice v;
      v.prepare(2, kSr, stretch::Mode::Mono);
      Src src;
      src.x = tone(300, 2.0);
      v.start(&Src::read, &src, 0.9, 1.3);
      std::vector<float> L(60000), R(60000);
      for (size_t at = 0; at < L.size();) {
        const int n = static_cast<int>(std::min<size_t>(pass ? 61 : 512, L.size() - at));
        float* o[2] = {L.data() + at, R.data() + at};
        v.render(o, n, 0.9, 1.3, 0, &Src::read, &src);
        at += static_cast<size_t>(n);
      }
      vo[pass] = L;
    }
    float vd = 0;
    for (size_t i = 0; i < vo[0].size(); ++i) vd = std::max(vd, std::fabs(vo[0][i] - vo[1][i]));
    expect(vd < 1e-5f, "stretch voice: the same output for any render split (positions accumulate in double)");
  }

  // ------------------------------------------------------------ Formant keep
  {
    // A buzzy voice (150 Hz) through one resonance at 800 Hz; 7 semitones up.
    std::vector<float> x(static_cast<size_t>(2 * kSr));
    for (size_t i = 0; i < x.size(); ++i) {
      double v = 0;
      for (int k = 1; k * 150 < 8000; ++k) v += std::sin(2 * kPi * k * 150 * static_cast<double>(i) / kSr) / (1 + std::pow((k * 150 - 800) / 120.0, 2));
      x[i] = static_cast<float>(0.2 * v);
    }
    auto centroid = [](const std::vector<float>& y) {
      const size_t N = 32768;
      std::vector<cd> b(N);
      for (size_t i = 0; i < N; ++i) b[i] = y[y.size() / 2 - N / 2 + i] * bh(i, N);
      fft(b);
      double num = 0, den = 0;
      for (size_t k = 1; k < N / 2; ++k) num += static_cast<double>(k) * std::norm(b[k]), den += std::norm(b[k]);
      return num / den * kSr / static_cast<double>(N);
    };
    const double c0 = centroid(x);
    double c[2];
    for (int k = 0; k < 2; ++k) {
      std::vector<float> y(x.size());
      const float* in[1] = {x.data()};
      float* out[1] = {y.data()};
      stretch::Settings s;
      s.mode = k ? stretch::Mode::Formant : stretch::Mode::Auto;
      s.pitch = std::pow(2.0, 7 / 12.0);
      stretch::process(in, 1, static_cast<int64_t>(x.size()), out, static_cast<int64_t>(y.size()), kSr, s);
      c[k] = centroid(y) / c0;
    }
    char name[160];
    std::snprintf(name, sizeof name, "formant keep: the spectral envelope stays (centroid ×%.2f) where plain pitch moves it (×%.2f)", c[1], c[0]);
    expect(c[0] > 1.3 && c[1] < 1.2, name);
  }

  // ------------------------------------------------------------ Pitch Shifter Hi-fi
  {
    namespace sp = gl::packs::spectral;
    PluginState st = defaultPluginState("gl.pitch");
    expect(st.params[sp::PsEngine] == sp::kPsHifi, "Pitch Shifter: new instances use the Hi-fi engine");
    st.params[sp::PsPitch] = -5;
    auto fx = createEffect("gl.pitch");
    fx->prepareHost(st, kSr);
    const int lat = fx->latencyFor(st, kSr);
    ProcessContext ctx;
    ctx.sampleRate = kSr;
    std::vector<float> L = tone(440, 2.0), R = L;
    long allocs = 0;
    for (size_t i = 0; i < L.size(); i += 256) {
      CountScope cs;
      fx->process(st, ctx, L.data() + i, R.data() + i, static_cast<int>(std::min<size_t>(256, L.size() - i)));
      allocs += cs.n();
    }
    const double want = 440 * std::pow(2.0, -5 / 12.0);
    expect(lat > 0 && lat < 0.25 * kSr && std::fabs(cents(peakHz(L.data(), static_cast<size_t>(lat) + 22050, want), want)) < 2.0,
           "Pitch Shifter Hi-fi: in tune within 2 cents, latency reported for delay compensation");
    expect(allocs == 0, "Pitch Shifter Hi-fi: no allocation while processing");
    // The output is exactly `lat` frames late: a click comes out there.
    auto fx2 = createEffect("gl.pitch");
    PluginState s0 = defaultPluginState("gl.pitch");
    fx2->prepareHost(s0, kSr);
    const int lat0 = fx2->latencyFor(s0, kSr);
    std::vector<float> a(static_cast<size_t>(lat0) + 20000, 0.f), b(a.size(), 0.f);
    a[1000] = b[1000] = 1.0f;
    for (size_t i = 0; i < a.size(); i += 512) fx2->process(s0, ctx, a.data() + i, b.data() + i, static_cast<int>(std::min<size_t>(512, a.size() - i)));
    size_t at = 0;
    for (size_t i = 0; i < a.size(); ++i)
      if (std::fabs(a[i]) > std::fabs(a[at])) at = i;
    expect(std::abs(static_cast<long>(at) - static_cast<long>(1000 + lat0)) <= 2, "Pitch Shifter Hi-fi: the reported latency is the real one");
  }

  // ------------------------------------------------------------ realtime safety
  {
    long probe = 0;
    {
      CountScope cs;
      std::vector<float>* v = new std::vector<float>(100);
      delete v;
      probe = cs.n();
    }
    expect(probe >= 2, "allocation counter sees allocations (self-check)");
    // Clip playback from a render, stretch voices, the sampler's stretch voices.
    const std::string path = writeTmp("gl_j87_tone4.wav", tone(250, 2.0));
    SamplePtr smp = sampleCache().get(path);
    TempoMap map;
    map.setConstant(120, 96);
    ProgAudioClip c;
    c.start = 0, c.end = 192, c.durSec = 1.0, c.sample = smp, c.srcEnd = smp->seconds(), c.speed = 1, c.pitch = 1.5, c.granular = true;
    stretch::ClipRequest rq;
    rq.sample = smp, rq.srcEnd = c.srcEnd, rq.mode = stretch::Mode::Auto, rq.rate = kSr, rq.frames = 44102, rq.pitch = 1.5;
    c.hq = stretch::renderClip(rq, false);
    const std::vector<ProgAudioClip> clips{c};
    std::vector<float> L(512), R(512);
    long n1 = 0;
    {
      CountScope cs;
      for (int64_t at = 0; at < 44100; at += 512) renderAudioClips(clips, map, kSr, 0, at, 0, 512, L.data(), R.data());
      n1 = cs.n();
    }
    expect(n1 == 0, "clip playback (rendered clip): no allocation");
    struct Src {
      double ph = 0;
      static int read(void* ctx, float* const* d, int n, bool) {
        auto* s = static_cast<Src*>(ctx);
        for (int i = 0; i < n; ++i, s->ph += 0.03) d[0][i] = d[1][i] = static_cast<float>(0.4 * std::sin(s->ph));
        return -1;
      }
    };
    stretch::Voice v;
    v.prepare(2, kSr, stretch::Mode::Auto);
    Src src;
    long n2 = 0;
    {
      CountScope cs;
      v.start(&Src::read, &src, 1.0, 0.75);
      for (int k = 0; k < 200; ++k) {
        float* o[2] = {L.data(), R.data()};
        v.render(o, 512, 1.0 + 0.2 * std::sin(k * 0.1), 0.75 + 0.5 * (k % 3), 0, &Src::read, &src);
      }
      n2 = cs.n();
    }
    expect(n2 == 0, "stretch voice: start and render without allocation (changing speed and pitch)");
    PluginState st = defaultPluginState("gl.sampler");
    st.data = path;
    st.params[pp::SmpStretchMode] = static_cast<float>(stretch::Mode::Transient);
    ChannelAux aux;
    prepareChannelAux(st, aux);
    auto gen = createGenerator("gl.sampler");
    gen->prepareHost(st, kSr);
    ProcessContext ctx;
    ctx.sampleRate = kSr;
    std::fill(L.begin(), L.end(), 0.f);
    gen->process(st, aux, ctx, L.data(), R.data(), 512);  // takes the prepared voices over
    long n3 = 0;
    {
      CountScope cs;
      for (int k = 0; k < 120; ++k) {
        if (k % 20 == 0) gen->noteOn(static_cast<uint32_t>(k + 1), 55 + k % 12, 0.9f, 3);
        gen->process(st, aux, ctx, L.data(), R.data(), 512);
      }
      n3 = cs.n();
    }
    expect(n3 == 0, "sampler stretch voices: note-ons and rendering without allocation");
  }

  // ------------------------------------------------------------ FLP round trip of every time mode
  {
    const fs::path dir = fs::temp_directory_path() / "gl_j87_flp";
    std::error_code ec;
    fs::create_directories(dir, ec);
    const std::string path = (dir / "loop.wav").string();
    {
      AudioBuffer b(2, 44100);
      std::vector<float> t = tone(200, 1.0);
      for (size_t i = 0; i < t.size(); ++i) b.at(0, i) = b.at(1, i) = t[i];
      writeWav(path, b, 44100, 16);
      sampleCache().clear();
    }
    Song s = makeEmptySong();
    for (int m = 0; m < 5; ++m) {
      // An audio clip channel per mode (one clip each)…
      Channel ch;
      ch.id = s.allocateId();
      ch.name = "Clip " + std::to_string(m);
      ch.plugin = defaultPluginState("gl.sampler");
      ch.plugin.data = path;
      ch.plugin.params[pp::SmpStretchMode] = static_cast<float>(m);
      ch.plugin.params[pp::SmpPitchShift] = static_cast<float>(100 * m - 200);
      ch.plugin.params[pp::SmpTimeStretch] = m ? 1.5f : 1.0f;
      ch.audioClip = true;
      ch.mixerTrack = s.masterId();
      s.channels.push_back(ch);
      Clip c;
      c.id = s.allocateId();
      c.kind = ClipKind::Audio;
      c.source = ch.id;
      c.start = static_cast<Tick>(m) * 384;
      c.length = 192;
      c.pitch = static_cast<float>(-300 + 150 * m);
      c.track = m;
      s.playlist.push_back(c);
      // …and a sampler channel per mode.
      Channel sm;
      sm.id = s.allocateId();
      sm.name = "Sampler " + std::to_string(m);
      sm.plugin = defaultPluginState("gl.sampler");
      sm.plugin.data = path;
      sm.plugin.params[pp::SmpStretchMode] = static_cast<float>(m);
      sm.plugin.params[pp::SmpPitchShift] = m ? static_cast<float>(50 * m) : 0.f;
      sm.plugin.params[pp::SmpTimeStretch] = m ? 0.75f : 1.0f;
      sm.mixerTrack = s.masterId();
      s.channels.push_back(sm);
    }
    flp::ExportOptions opt;
    opt.useOrigin = false;
    opt.projectDir = dir.string();
    std::vector<uint8_t> bytes;
    flp::ExportReport rep;
    Song back;
    flp::Report ir;
    std::string err;
    const bool ok = flp::exportFlp(s, bytes, rep, opt) && importFlpBytes(bytes, back, ir, &err, "", dir.string(), false);
    int matched = 0, total = 0;
    std::string why;
    if (ok)
      for (const Channel& want : s.channels) {
        ++total;
        const Channel* got = nullptr;
        for (const Channel& c : back.channels)
          if (c.name == want.name) got = &c;
        if (!got || got->plugin.type != "gl.sampler" || got->plugin.params.size() <= static_cast<size_t>(pp::SmpTimeStretch)) {
          why += " missing:" + want.name;
          continue;
        }
        const auto& p = got->plugin.params;
        const auto& q = want.plugin.params;
        const bool mode = p[pp::SmpStretchMode] == q[pp::SmpStretchMode];
        const bool stretchOk = q[pp::SmpStretchMode] == 0 || (std::fabs(p[pp::SmpTimeStretch] - q[pp::SmpTimeStretch]) < 1e-3f &&
                                                              std::fabs(p[pp::SmpPitchShift] - q[pp::SmpPitchShift]) < 0.5f);
        if (mode && stretchOk)
          ++matched;
        else
          why += " " + want.name + "(mode " + std::to_string(p[pp::SmpStretchMode]) + ")";
      }
    bool clips = ok;
    if (ok)
      for (const Clip& c : s.playlist) {
        const Clip* g = nullptr;
        for (const Clip& x : back.playlist)
          if (x.kind == ClipKind::Audio && x.start == c.start) g = &x;
        clips = clips && g && std::fabs(g->pitch - c.pitch) < 0.5f;
      }
    char name[300];
    std::snprintf(name, sizeof name, "FLP round trip: every time mode with its pitch shift and time stretch (%d/%d)%s", matched, total, why.c_str());
    expect(ok && matched == total, name);
    expect(clips, "FLP round trip: audio clip pitch kept");
    fs::remove_all(dir, ec);
  }

  return finish();
}
