// Gridline — "spectral" pack tests: vocoder, pitch shifter, pitch corrector,
// scratcher, band splitter, spreader.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "../../src/core/PluginInfo.h"
#include "../../src/engine/Effects.h"
#include "../../src/packs/spectral/spectral.h"
#include "../Check.h"

using namespace gl;
namespace sp = gl::packs::spectral;

namespace {

constexpr double kSr = 44100.0;
constexpr double kPi = 3.14159265358979323846;
const char* kTypes[] = {"gl.vocoder", "gl.pitch", "gl.pitcher", "gl.scratcher", "gl.splitter", "gl.spread"};

struct Buf {
  std::vector<float> L, R;
  explicit Buf(size_t n = 0) : L(n, 0.f), R(n, 0.f) {}
  size_t size() const { return L.size(); }
};

// Runs an effect over a buffer in blocks of `block` frames.
void run(Effect& fx, const PluginState& st, Buf& b, int block = 256, double sr = kSr) {
  ProcessContext ctx;
  ctx.sampleRate = sr;
  for (size_t i = 0; i < b.size(); i += static_cast<size_t>(block)) {
    int n = static_cast<int>(std::min(static_cast<size_t>(block), b.size() - i));
    fx.process(st, ctx, b.L.data() + i, b.R.data() + i, n);
  }
}

Buf sine(double hz, double seconds, double amp = 0.5) {
  Buf b(static_cast<size_t>(seconds * kSr));
  for (size_t i = 0; i < b.size(); ++i) b.L[i] = b.R[i] = static_cast<float>(amp * std::sin(2 * kPi * hz * static_cast<double>(i) / kSr));
  return b;
}

Buf noise(double seconds, double amp = 0.3, uint32_t seed = 1) {
  Buf b(static_cast<size_t>(seconds * kSr));
  uint32_t s = seed;
  for (size_t i = 0; i < b.size(); ++i) {
    s = s * 1664525u + 1013904223u;
    b.L[i] = static_cast<float>(amp * ((s >> 8) / 8388608.0 - 1.0));
    s = s * 1664525u + 1013904223u;
    b.R[i] = static_cast<float>(amp * ((s >> 8) / 8388608.0 - 1.0));
  }
  return b;
}

double rms(const std::vector<float>& x, size_t a, size_t b) {
  double s = 0;
  for (size_t i = a; i < b; ++i) s += static_cast<double>(x[i]) * x[i];
  return std::sqrt(s / static_cast<double>(std::max<size_t>(1, b - a)));
}

double peak(const std::vector<float>& x, size_t a, size_t b) {
  double p = 0;
  for (size_t i = a; i < b; ++i) p = std::max(p, static_cast<double>(std::fabs(x[i])));
  return p;
}

// Frequency from interpolated positive-going zero crossings in [a, b).
double zcFreq(const std::vector<float>& x, size_t a, size_t b) {
  double first = -1, last = -1;
  int count = 0;
  for (size_t i = a + 1; i < b; ++i)
    if (x[i - 1] < 0 && x[i] >= 0) {
      double t = static_cast<double>(i - 1) + x[i - 1] / (x[i - 1] - x[i]);
      if (first < 0) first = t;
      last = t;
      ++count;
    }
  return count > 1 ? (count - 1) * kSr / (last - first) : 0;
}

// Goertzel magnitude of frequency hz over [a, b).
double tone(const std::vector<float>& x, size_t a, size_t b, double hz) {
  double w = 2 * kPi * hz / kSr, c = 2 * std::cos(w), s1 = 0, s2 = 0;
  for (size_t i = a; i < b; ++i) {
    double s0 = x[i] + c * s1 - s2;
    s2 = s1;
    s1 = s0;
  }
  return std::sqrt(s1 * s1 + s2 * s2 - c * s1 * s2) / static_cast<double>(b - a);
}

// Strongest frequency (1 Hz grid) between lo and hi over [a, b).
double peakFreq(const std::vector<float>& x, size_t a, size_t b, double lo, double hi) {
  double best = 0, bf = 0;
  for (double f = lo; f <= hi; f += 1.0) {
    double m = tone(x, a, b, f);
    if (m > best) {
      best = m;
      bf = f;
    }
  }
  return bf;
}

bool allFinite(const Buf& b, double limit = 1e3) {
  for (size_t i = 0; i < b.size(); ++i)
    if (!std::isfinite(b.L[i]) || !std::isfinite(b.R[i]) || std::fabs(b.L[i]) > limit || std::fabs(b.R[i]) > limit) return false;
  return true;
}

PluginState stateOf(const char* type) { return defaultPluginState(type); }
void set(PluginState& s, int k, float v) { s.params[static_cast<size_t>(k)] = v; }

}  // namespace

int main() {
  // ---------------------------------------------------------------- catalogue
  for (const char* t : kTypes) {
    const PluginInfo* info = findPlugin(t);
    expect(info && info->kind == PluginKind::Effect, (std::string("catalogued effect: ") + t).c_str());
    expect(info && info->presets.size() >= 2 && info->presets.size() <= 6, (std::string("2-6 factory presets: ") + t).c_str());
    expect(info && info->editorWidth > 0, (std::string("editor width set: ") + t).c_str());
    auto fx = createEffect(t);
    expect(fx != nullptr, (std::string("creates: ") + t).c_str());
    expect(dynamic_cast<sp::SpectralFx*>(fx.get()) != nullptr, (std::string("exposes telemetry: ") + t).c_str());
    PluginState old;
    old.type = t;
    old.params = {0.0f};
    normalizePluginState(old);
    expect(info && old.params.size() == info->params.size(), (std::string("normalize pads old state: ") + t).c_str());
    // Short parameter vectors still process.
    Buf b = noise(0.2);
    if (fx) run(*fx, old, b);
    expect(allFinite(b), (std::string("short state processes finitely: ") + t).c_str());
  }

  // ------------------------------------------------------- silence -> silence
  for (const char* t : kTypes) {
    const PluginInfo* info = findPlugin(t);
    for (size_t pr = 0; info && pr < info->presets.size(); ++pr) {
      auto fx = createEffect(t);
      PluginState st = presetState(*info, pr);
      Buf b(static_cast<size_t>(kSr * 1.5));
      run(*fx, st, b);
      expect(peak(b.L, 0, b.size()) < 1e-6 && peak(b.R, 0, b.size()) < 1e-6,
             (std::string("silence in -> silence out: ") + t + " / " + info->presets[pr].name).c_str());
    }
  }

  // ------------------------------------------------- extremes produce finite
  for (const char* t : kTypes) {
    const PluginInfo* info = findPlugin(t);
    for (int side = 0; side < 2; ++side) {
      auto fx = createEffect(t);
      PluginState st = stateOf(t);
      for (size_t k = 0; k < info->params.size(); ++k) {
        const ParamDesc& d = info->params[k];
        st.params[k] = static_cast<float>(d.kind == ParamKind::Choice ? (side ? d.choices.size() - 1 : 0) : (side ? d.max : d.min));
      }
      Buf b = noise(1.0, 0.9);
      run(*fx, st, b, 333);
      // Also through a sample-rate change.
      Buf c = noise(0.5, 0.9, 7);
      run(*fx, st, c, 128, 96000.0);
      expect(allFinite(b) && allFinite(c), (std::string("finite at parameter ") + (side ? "maxima: " : "minima: ") + t).c_str());
    }
  }

  // ------------------------------------------------------ block invariance
  for (const char* t : kTypes) {
    PluginState st = stateOf(t);
    auto a = createEffect(t), b = createEffect(t);
    Buf x = noise(1.0, 0.4, 3), y = x;
    run(*a, st, x, 64);
    run(*b, st, y, 509);
    double diff = 0;
    for (size_t i = 0; i < x.size(); ++i) diff = std::max(diff, static_cast<double>(std::fabs(x.L[i] - y.L[i]) + std::fabs(x.R[i] - y.R[i])));
    expect(diff < 1e-5, (std::string("block-size invariant: ") + t).c_str());
  }

  // ===================================================================== vocoder (legacy model; reference model: tests/j1)
  {
    // Modulator (left) = gated noise, carrier (right) = steady saw.
    PluginState st = stateOf("gl.vocoder");
    set(st, sp::VxModel, 0);  // 0.5 engine
    set(st, sp::VcRelease, 10);
    Buf b(static_cast<size_t>(kSr * 2));
    Buf nz = noise(2.0, 0.4, 11);
    double ph = 0;
    for (size_t i = 0; i < b.size(); ++i) {
      bool gateOn = (i / static_cast<size_t>(kSr / 4)) % 2 == 0;
      b.L[i] = gateOn ? nz.L[i] : 0.f;
      ph += 110.0 / kSr;
      ph -= std::floor(ph);
      b.R[i] = static_cast<float>(0.5 * (2 * ph - 1));
    }
    auto fx = createEffect("gl.vocoder");
    run(*fx, st, b);
    const size_t q = static_cast<size_t>(kSr / 4);
    double on = rms(b.L, q * 4 + q / 4, q * 5 - 100), off = rms(b.L, q * 5 + q / 4, q * 6 - 100);
    std::printf("# vocoder gated: on %.4f off %.6f\n", on, off);
    expect(on > 0.02 && off < on * 0.03, "vocoder: modulator envelope gates the carrier");
    expect(on < 2.0, "vocoder: output level sane");

    // The modulator's spectrum shapes the carrier: low vs high modulator tone, noise carrier.
    auto spectral = [&](double modHz) {
      PluginState s2 = stateOf("gl.vocoder");
      set(s2, sp::VxModel, 0);  // 0.5 engine
      set(s2, sp::VcBands, 7);  // 64 bands
      Buf c(static_cast<size_t>(kSr * 1.0));
      Buf cn = noise(1.0, 0.4, 5);
      for (size_t i = 0; i < c.size(); ++i) {
        c.L[i] = static_cast<float>(0.4 * std::sin(2 * kPi * modHz * static_cast<double>(i) / kSr));
        c.R[i] = cn.R[i];
      }
      auto v = createEffect("gl.vocoder");
      run(*v, s2, c);
      size_t a = c.size() / 2;
      return std::make_pair(tone(c.L, a, c.size(), 400), tone(c.L, a, c.size(), 3000));
    };
    auto lowMod = spectral(400), highMod = spectral(3000);
    std::printf("# vocoder spectra: low mod %.5f/%.5f, high mod %.5f/%.5f\n", lowMod.first, lowMod.second, highMod.first, highMod.second);
    expect(lowMod.first > lowMod.second * 4 && highMod.second > highMod.first * 4, "vocoder: modulator spectrum transfers onto the carrier");

    // Built-in carrier: a sung (sine) modulator produces output with no right input.
    PluginState s3 = stateOf("gl.vocoder");
    set(s3, sp::VxModel, 0);  // 0.5 engine
    set(s3, sp::VcCarrier, 1);
    Buf d = sine(300, 1.0, 0.4);
    for (auto& r : d.R) r = 0;
    auto v3 = createEffect("gl.vocoder");
    run(*v3, s3, d);
    expect(rms(d.L, d.size() / 2, d.size()) > 0.01, "vocoder: built-in carrier sounds");
    // Swap: modulator on the right.
    PluginState s4 = stateOf("gl.vocoder");
    set(s4, sp::VxModel, 0);  // 0.5 engine
    set(s4, sp::VcSwap, 1);
    Buf e(static_cast<size_t>(kSr));
    for (size_t i = 0; i < e.size(); ++i) e.L[i] = static_cast<float>(0.4 * std::sin(2 * kPi * 220 * static_cast<double>(i) / kSr));
    auto v4 = createEffect("gl.vocoder");
    run(*v4, s4, e);
    expect(rms(e.L, e.size() / 2, e.size()) < 1e-4, "vocoder: swapped routing takes the modulator from the right");
    auto* tel = dynamic_cast<sp::SpectralFx*>(v3.get());
    expect(tel && tel->tel.count.load() == 32, "vocoder: band telemetry published");
  }

  // ============================================================== pitch shifter
  for (int mode = 0; mode < 2; ++mode) {
    for (double semis : {7.0, -12.0, 3.0}) {
      PluginState st = stateOf("gl.pitch");
      set(st, sp::PsEngine, 0);  // classic engines (the J-1 reference engines: tests/j1/j1_spectral2.inc)
      set(st, sp::PsMode, static_cast<float>(mode));
      set(st, sp::PsPitch, static_cast<float>(semis));
      auto fx = createEffect("gl.pitch");
      Buf b = sine(440, 2.0, 0.5);
      run(*fx, st, b);
      double want = 440 * std::pow(2.0, semis / 12.0);
      double got = mode == 1 ? zcFreq(b.L, b.size() / 2, b.size()) : 0;
      if (mode == 0) {
        // The delay shifter's grain crossfades disturb zero crossings: compare tone energy instead.
        double pf = peakFreq(b.L, b.size() / 2, b.size(), 100, 1500);
        double lvl = rms(b.L, b.size() / 2, b.size());
        std::printf("# pitch fast %+g st: spectral peak %.1f Hz (want %.1f), rms %.3f\n", semis, pf, want, lvl);
        expect(std::fabs(pf / want - 1) < 0.01 && lvl > 0.2, ("pitch shifter (fast) moves a sine by " + std::to_string(static_cast<int>(semis)) + " st").c_str());
      } else {
        std::printf("# pitch spectral %+g st: %.2f Hz (want %.2f)\n", semis, got, want);
        expect(std::fabs(got / want - 1) < 0.005, ("pitch shifter (spectral) moves a sine by " + std::to_string(static_cast<int>(semis)) + " st").c_str());
        double lvl = rms(b.L, b.size() / 2, b.size());
        std::printf("# level %.3f\n", lvl);
        expect(lvl > 0.25 && lvl < 0.5, "pitch shifter (spectral) keeps the level");
      }
    }
  }
  {
    // Fine tune + formant preservation path stays clean on a harmonic signal.
    PluginState st = stateOf("gl.pitch");
    set(st, sp::PsEngine, 0);
    set(st, sp::PsPitch, -5);
    set(st, sp::PsFine, 30);
    set(st, sp::PsPreserve, 1);
    auto fx = createEffect("gl.pitch");
    Buf b = sine(330, 1.5, 0.4);
    run(*fx, st, b);
    double want = 330 * std::pow(2.0, (-5 + 0.3) / 12.0);
    double got = zcFreq(b.L, b.size() / 2, b.size());
    std::printf("# pitch formant-keep: %.2f Hz (want %.2f)\n", got, want);
    expect(std::fabs(got / want - 1) < 0.005, "pitch shifter: fine tune with formant preservation");
    auto* t = dynamic_cast<sp::SpectralFx*>(fx.get());
    expect(t && t->tel.v[sp::TvLatency].load() > 0, "pitch shifter: latency reported");
  }

  // ============================================================ pitch corrector
  auto corrected = [&](double hz, int scale, int key, float speed) {
    PluginState st = stateOf("gl.pitcher");
    set(st, sp::PcModel, 0);  // legacy engine (reference model: tests/j1/j1_spectral2.inc)
    set(st, sp::PcScale, static_cast<float>(scale));
    set(st, sp::PcKey, static_cast<float>(key));
    set(st, sp::PcSpeed, speed);
    auto fx = createEffect("gl.pitcher");
    Buf b = sine(hz, 2.0, 0.5);
    run(*fx, st, b);
    auto* t = dynamic_cast<sp::SpectralFx*>(fx.get());
    std::printf("# pitcher %.1f Hz -> detected %.2f target %.0f\n", hz, t->tel.v[sp::TvDetected].load(), t->tel.v[sp::TvTarget].load());
    return std::make_pair(zcFreq(b.L, b.size() / 2, b.size()), rms(b.L, b.size() / 2, b.size()));
  };
  {
    auto r1 = corrected(452.0, 1, 0, 1.0f);  // A4 + 47 ct, C major -> A4
    std::printf("# pitcher 452 -> %.2f Hz (rms %.3f)\n", r1.first, r1.second);
    expect(std::fabs(r1.first / 440.0 - 1) < 0.004, "pitch corrector snaps a sharp A to 440 Hz");
    expect(r1.second > 0.2 && r1.second < 0.6, "pitch corrector keeps the level");
    auto r2 = corrected(425.0, 1, 0, 1.0f);  // between G# and A: C major has no G# -> A4
    std::printf("# pitcher 425 major -> %.2f Hz\n", r2.first);
    expect(std::fabs(r2.first / 440.0 - 1) < 0.004, "pitch corrector respects the scale (no G# in C major)");
    auto r3 = corrected(425.0, 0, 0, 1.0f);  // chromatic -> G#4
    std::printf("# pitcher 425 chromatic -> %.2f Hz\n", r3.first);
    expect(std::fabs(r3.first / 415.3047 - 1) < 0.004, "pitch corrector: chromatic snaps to G#");
    auto r4 = corrected(196.0 * std::pow(2.0, 0.3 / 12), 2, 9, 1.0f);  // A minor, G3 + 30 ct -> G3
    std::printf("# pitcher G3+30 -> %.2f Hz\n", r4.first);
    expect(std::fabs(r4.first / 196.0 - 1) < 0.004, "pitch corrector: low note in A minor");
  }
  {
    // Custom scale from the note toggles.
    PluginState st = stateOf("gl.pitcher");
    set(st, sp::PcModel, 0);
    set(st, sp::PcScale, sp::kScaleCount - 1);
    for (int i = 0; i < 12; ++i) set(st, sp::PcNote0 + i, i == 7 ? 1.f : 0.f);  // only G
    float pv[sp::PcCount];
    for (int k = 0; k < sp::PcCount; ++k) pv[k] = st.params[static_cast<size_t>(k)];
    expect(sp::pitcherMask(pv) == (1 << 7), "pitch corrector: custom mask from note toggles");
    expect(sp::nearestInMask(69.4, 1 << 7) == 67, "pitch corrector: nearest enabled note");
    // Harmony voice a fifth up adds a tone at 1.5x.
    set(st, sp::PcScale, 0);
    set(st, sp::PcVoice1Level, 1.0f);
    set(st, sp::PcVoice1, 7);
    set(st, sp::PcDiatonic, 0);
    auto fx = createEffect("gl.pitcher");
    Buf b = sine(440, 1.5, 0.4);
    run(*fx, st, b);
    double fifth = tone(b.L, b.size() / 2, b.size(), 440 * std::pow(2.0, 7 / 12.0)) + tone(b.R, b.size() / 2, b.size(), 440 * std::pow(2.0, 7 / 12.0));
    double root = tone(b.L, b.size() / 2, b.size(), 440) + tone(b.R, b.size() / 2, b.size(), 440);
    std::printf("# harmony: root %.4f fifth %.4f\n", root, fifth);
    expect(fifth > 0.05 && root > 0.05, "pitch corrector: harmony voice adds a fifth");
  }

  // ============================================================== band splitter
  {
    const double freqs[] = {40, 150, 200, 600, 2000, 5000, 8000, 15000};
    double worst = 0;
    for (int slope = 0; slope < 3; ++slope)
      for (int bands = 0; bands < 3; ++bands)
        for (double f : freqs) {
          PluginState st = stateOf("gl.splitter");
          set(st, sp::SpModel, 0);  // 0.5 engine
          set(st, sp::SpSlope, static_cast<float>(slope));
          set(st, sp::SpBands, static_cast<float>(bands));
          auto fx = createEffect("gl.splitter");
          Buf b = sine(f, 1.0, 0.5);
          run(*fx, st, b);
          double a = peak(b.L, b.size() / 2, b.size());
          worst = std::max(worst, std::fabs(20 * std::log10(a / 0.5)));
        }
    std::printf("# splitter worst sum deviation %.4f dB\n", worst);
    expect(worst < 0.1, "band splitter sums flat (+-0.1 dB) for every slope and band count");
    // Solo isolates the low band.
    PluginState st = stateOf("gl.splitter");
    set(st, sp::SpModel, 0);  // 0.5 engine
    set(st, sp::SpSlope, 2);
    set(st, sp::splitParam(0, sp::SbSolo), 1);
    auto fx = createEffect("gl.splitter");
    Buf hi = sine(5000, 0.6, 0.5);
    run(*fx, st, hi);
    Buf lo = sine(60, 0.6, 0.5);
    auto fx2 = createEffect("gl.splitter");
    run(*fx2, st, lo);
    double hiDb = 20 * std::log10(peak(hi.L, hi.size() / 2, hi.size()) / 0.5 + 1e-12);
    double loDb = 20 * std::log10(peak(lo.L, lo.size() / 2, lo.size()) / 0.5 + 1e-12);
    std::printf("# splitter solo low: 5 kHz %.1f dB, 60 Hz %.2f dB\n", hiDb, loDb);
    expect(hiDb < -60 && std::fabs(loDb) < 0.5, "band splitter: solo isolates a band");
    // Band gain applies to its band only.
    PluginState g = stateOf("gl.splitter");
    set(g, sp::SpModel, 0);  // 0.5 engine
    set(g, sp::splitParam(2, sp::SbGain), -12);
    auto fx3 = createEffect("gl.splitter");
    Buf h2 = sine(9000, 0.6, 0.5);
    run(*fx3, g, h2);
    double gDb = 20 * std::log10(peak(h2.L, h2.size() / 2, h2.size()) / 0.5);
    expect(std::fabs(gDb + 12) < 0.3, "band splitter: high band gain");
    // Width 0 on a band collapses it to mono.
    PluginState w = stateOf("gl.splitter");
    set(w, sp::SpModel, 0);  // 0.5 engine
    for (int bnd = 0; bnd < 3; ++bnd) set(w, sp::splitParam(bnd, sp::SbWidth), 0);
    auto fx4 = createEffect("gl.splitter");
    Buf n4 = noise(0.5, 0.3, 9);
    run(*fx4, w, n4);
    double sd = 0;
    for (size_t i = n4.size() / 2; i < n4.size(); ++i) sd = std::max(sd, static_cast<double>(std::fabs(n4.L[i] - n4.R[i])));
    expect(sd < 1e-4, "band splitter: zero width is mono");
  }

  // ================================================================== scratcher
  {
    PluginState st = stateOf("gl.scratcher");
    auto fx = createEffect("gl.scratcher");
    Buf b = sine(500, 1.0, 0.5);
    run(*fx, st, b);
    double lvl = peak(b.L, b.size() / 2, b.size());
    expect(lvl > 0.45 && lvl < 0.55, "scratcher: running deck passes the input");
    // Motor off: the platter spins down to silence.
    set(st, sp::ScMotor, 0);
    set(st, sp::ScStop, 0.5f);
    Buf c = sine(500, 1.5, 0.5);
    run(*fx, st, c);
    double tail = rms(c.L, c.size() - 4410, c.size());
    std::printf("# scratcher stopped tail %.6f\n", tail);
    expect(tail < 1e-3, "scratcher: motor stop brings the deck to silence");
    auto* t = dynamic_cast<sp::SpectralFx*>(fx.get());
    expect(t && std::fabs(t->tel.v[sp::TvVelocity].load()) < 1e-6, "scratcher: platter velocity reaches zero");
    // Motor back on: up to speed and back at the live position.
    set(st, sp::ScMotor, 1);
    Buf d = sine(500, 1.5, 0.5);
    run(*fx, st, d);
    double back = peak(d.L, d.size() - 4410, d.size());
    expect(back > 0.45 && back < 0.55 && t->tel.v[sp::TvLag].load() < 0.01, "scratcher: motor restart returns to live");
    // Auto pattern scratches (pitch moves): output differs from a steady tone.
    PluginState pat = stateOf("gl.scratcher");
    set(pat, sp::ScPattern, 1);
    auto fx2 = createEffect("gl.scratcher");
    Buf e = sine(500, 2.0, 0.5);
    run(*fx2, pat, e);
    double f1 = zcFreq(e.L, static_cast<size_t>(kSr * 1.0), static_cast<size_t>(kSr * 1.1));
    double f2 = zcFreq(e.L, static_cast<size_t>(kSr * 1.25), static_cast<size_t>(kSr * 1.35));
    std::printf("# scratch pattern freqs %.1f %.1f\n", f1, f2);
    expect(allFinite(e) && std::fabs(f1 - f2) > 20, "scratcher: baby pattern varies the playback speed");
    // Crossfader at 0 = live input untouched.
    PluginState live = stateOf("gl.scratcher");
    set(live, sp::ScFader, 0);
    set(live, sp::ScPattern, 2);
    auto fx3 = createEffect("gl.scratcher");
    Buf f = sine(300, 0.5, 0.5), ref = f;
    run(*fx3, live, f);
    double dd = 0;
    for (size_t i = 0; i < f.size(); ++i) dd = std::max(dd, static_cast<double>(std::fabs(f.L[i] - ref.L[i])));
    expect(dd < 1e-5, "scratcher: crossfader closed passes the live input");
  }

  // =================================================================== spreader
  {
    // Complement mode is mono-compatible: L+R unchanged.
    PluginState st = stateOf("gl.spread");
    set(st, sp::SrModel, 0);  // legacy engine (reference model: tests/j1/j1_spectral2.inc)
    set(st, sp::SrSpread, 1.0f);
    set(st, sp::SrBassMono, 0);
    auto fx = createEffect("gl.spread");
    Buf b = noise(1.0, 0.3, 21), ref = b;
    run(*fx, st, b);
    double md = 0, side = 0, sideIn = 0;
    for (size_t i = 0; i < b.size(); ++i) {
      md = std::max(md, static_cast<double>(std::fabs((b.L[i] + b.R[i]) - (ref.L[i] + ref.R[i]))));
      side += std::pow(b.L[i] - b.R[i], 2.0);
      sideIn += std::pow(ref.L[i] - ref.R[i], 2.0);
    }
    expect(md < 1e-4, "spreader: complementary widening keeps the mono sum");
    expect(side > sideIn * 1.1, "spreader: widening adds side energy");
    // Mono input becomes stereo.
    Buf m = noise(1.0, 0.3, 4);
    m.R = m.L;
    auto fx2 = createEffect("gl.spread");
    run(*fx2, st, m);
    double s2 = 0;
    for (size_t i = 0; i < m.size(); ++i) s2 += std::fabs(m.L[i] - m.R[i]);
    expect(s2 > 10, "spreader: mono input gains width");
    // Separation -100% collapses to mono; mono bass removes low side content.
    PluginState mono = stateOf("gl.spread");
    set(mono, sp::SrModel, 0);
    set(mono, sp::SrSpread, 0);
    set(mono, sp::SrSeparation, -1);
    auto fx3 = createEffect("gl.spread");
    Buf n3 = noise(0.5, 0.3, 8);
    run(*fx3, mono, n3);
    double d3 = 0;
    for (size_t i = 0; i < n3.size(); ++i) d3 = std::max(d3, static_cast<double>(std::fabs(n3.L[i] - n3.R[i])));
    expect(d3 < 1e-6, "spreader: separation -100% is mono");
    PluginState bass = stateOf("gl.spread");
    set(bass, sp::SrModel, 0);
    set(bass, sp::SrSpread, 0);
    auto fx4 = createEffect("gl.spread");
    Buf lb(static_cast<size_t>(kSr));
    for (size_t i = 0; i < lb.size(); ++i) {
      lb.L[i] = static_cast<float>(0.5 * std::sin(2 * kPi * 50 * static_cast<double>(i) / kSr));
      lb.R[i] = -lb.L[i];  // pure side at 50 Hz
    }
    run(*fx4, bass, lb);
    expect(peak(lb.L, lb.size() / 2, lb.size()) < 0.15, "spreader: mono bass removes low side content");
    // Polarity invert left.
    PluginState inv = stateOf("gl.spread");
    set(inv, sp::SrModel, 0);
    set(inv, sp::SrSpread, 0);
    set(inv, sp::SrBassMono, 0);
    set(inv, sp::SrInvert, 1);
    auto fx5 = createEffect("gl.spread");
    Buf s5 = sine(1000, 0.2, 0.5), r5 = s5;
    run(*fx5, inv, s5);
    double e5 = 0;
    for (size_t i = 0; i < s5.size(); ++i) e5 = std::max(e5, static_cast<double>(std::fabs(s5.L[i] + r5.L[i])) + std::fabs(s5.R[i] - r5.R[i]));
    expect(e5 < 1e-5, "spreader: invert left polarity");
    // DC removal.
    PluginState dc = stateOf("gl.spread");
    set(dc, sp::SrModel, 0);
    set(dc, sp::SrSpread, 0);
    set(dc, sp::SrDcBlock, 1);
    auto fx6 = createEffect("gl.spread");
    Buf s6(static_cast<size_t>(kSr * 2));
    for (size_t i = 0; i < s6.size(); ++i) s6.L[i] = s6.R[i] = 0.3f;
    run(*fx6, dc, s6);
    expect(peak(s6.L, s6.size() - 1000, s6.size()) < 0.01, "spreader: removes DC offset");
  }

  // ------------------------------------------------- cost (informational)
  for (const char* t : kTypes) {
    PluginState st = stateOf(t);
    if (std::string(t) == "gl.vocoder") {
      set(st, sp::VcBands, 9);
      set(st, sp::VcOrder, 2);
    }
    auto fx = createEffect(t);
    Buf b = noise(5.0, 0.3, 5);
    auto t0 = std::chrono::steady_clock::now();
    run(*fx, st, b, 512);
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("# cost %-13s %.1fx realtime\n", t, 5.0 / std::max(secs, 1e-9));
  }

  // Pitch shifter dry/wet mix: 0 = the (latency-aligned) dry signal, 0.5 = both.
  {
    PluginState st = stateOf("gl.pitch");
    set(st, sp::PsEngine, 0);
    set(st, sp::PsPitch, 12.f);
    set(st, sp::PsMix, 0.f);
    auto fx = createEffect("gl.pitch");
    Buf b = sine(440, 1.0, 0.5);
    run(*fx, st, b);
    double pf = peakFreq(b.L, b.size() / 2, b.size(), 100, 1500);
    expect(std::fabs(pf / 440 - 1) < 0.01 && rms(b.L, b.size() / 2, b.size()) > 0.3, "pitch shifter mix 0 passes the dry signal");
    set(st, sp::PsMix, 0.5f);
    auto fx2 = createEffect("gl.pitch");
    Buf c = sine(440, 1.0, 0.5);
    run(*fx2, st, c);
    bool finite = true;
    for (float v : c.L) finite = finite && std::isfinite(v);
    expect(finite && rms(c.L, c.size() / 2, c.size()) > 0.1, "pitch shifter half mix is finite and audible");
  }

  return finish();
}
