// Gridline — "mini" pack tests (Gridline Suite Mini): catalogue, creation,
// silence, extremes, block-size invariance, state normalisation and one
// behaviour check per plugin against the targets measured for the suite.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include "../../src/core/PluginInfo.h"
#include "../../src/engine/Effects.h"
#include "../../src/packs/mini/mini.h"
#include "../Check.h"

using namespace gl;
using namespace gl::packs::mini;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kSr = 48000.0;

void run(Effect& fx, const PluginState& s, std::vector<float>& L, std::vector<float>& R, int block, double sr = kSr) {
  ProcessContext c;
  c.sampleRate = sr;
  for (size_t off = 0; off < L.size(); off += static_cast<size_t>(block)) {
    const int n = static_cast<int>(std::min<size_t>(static_cast<size_t>(block), L.size() - off));
    fx.process(s, c, L.data() + off, R.data() + off, n);
  }
}

std::vector<float> sine(double hz, size_t n, double amp = 1.0) {
  std::vector<float> v(n);
  for (size_t i = 0; i < n; ++i) v[i] = static_cast<float>(amp * std::sin(2 * kPi * hz * static_cast<double>(i) / kSr));
  return v;
}

std::vector<float> noise(size_t n, uint32_t seed, float amp = 0.5f) {
  std::vector<float> v(n);
  for (auto& x : v) {
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    x = static_cast<float>(static_cast<int32_t>(seed)) / 2147483648.0f * amp;
  }
  return v;
}

double rms(const std::vector<float>& v, size_t a, size_t b) {
  double s = 0;
  for (size_t i = a; i < b; ++i) s += static_cast<double>(v[i]) * v[i];
  return std::sqrt(s / static_cast<double>(std::max<size_t>(1, b - a)));
}
double peak(const std::vector<float>& v, size_t a, size_t b) {
  double m = 0;
  for (size_t i = a; i < b; ++i) m = std::max(m, static_cast<double>(std::fabs(v[i])));
  return m;
}
double db(double g) { return 20 * std::log10(std::max(1e-12, g)); }

// Magnitude of one frequency (Goertzel, normalised to a sine's amplitude).
double tone(const std::vector<float>& v, size_t a, size_t b, double hz) {
  const double w = 2 * kPi * hz / kSr, c = 2 * std::cos(w);
  double s1 = 0, s2 = 0;
  for (size_t i = a; i < b; ++i) {
    const double s0 = v[i] + c * s1 - s2;
    s2 = s1;
    s1 = s0;
  }
  const double re = s1 - s2 * std::cos(w), im = s2 * std::sin(w);
  return 2 * std::sqrt(re * re + im * im) / static_cast<double>(b - a);
}

PluginState st(const char* type, std::vector<std::pair<int, float>> v = {}) {
  PluginState s = defaultPluginState(type);
  for (auto [k, x] : v) s.params[static_cast<size_t>(k)] = x;
  return s;
}

// Runs a mono signal (both channels) and returns the left output.
std::vector<float> through(const PluginState& s, std::vector<float> in, int block = 256) {
  auto fx = createEffect(s.type);
  auto R = in;
  run(*fx, s, in, R, block);
  return in;
}

// Steady gain (dB) of a sine.
double gainDb(const PluginState& s, double hz, double amp = 0.25) {
  const size_t n = static_cast<size_t>(kSr);
  auto in = sine(hz, n, amp);
  auto out = through(s, in);
  return db(rms(out, n / 2, n) / rms(in, n / 2, n));
}

bool finiteAll(const std::vector<float>& v) {
  for (float x : v)
    if (!std::isfinite(x)) return false;
  return true;
}

}  // namespace

int main() {
  // ---- catalogue -----------------------------------------------------------------
  {
    bool listed = true, shape = true, presets = true, names = true;
    std::set<std::string> seen;
    for (const Entry& e : kEntries) {
      const PluginInfo* p = findPlugin(e.type);
      if (!p) {
        listed = false;
        std::printf("# missing %s\n", e.type);
        continue;
      }
      if (p->category != kCategory || p->kind != PluginKind::Effect || p->editorWidth != 440.f || p->name != e.name) shape = false;
      if (p->presets.size() < 2 || p->presets.size() > 6) presets = false;
      for (const auto& pr : p->presets)
        for (const auto& [k, v] : pr.values)
          if (k < 0 || static_cast<size_t>(k) >= p->params.size() || p->params[static_cast<size_t>(k)].clamp(v) != v) {
            presets = false;
            std::printf("# %s preset %s value %d out of range\n", e.type, pr.name.c_str(), k);
          }
      if (!seen.insert(e.name).second) names = false;
    }
    int inCategory = 0;
    for (const auto& p : pluginCatalog())
      if (p.category == kCategory) ++inCategory;
    expect(kEntryCount == 35 && listed, "all 35 Mini plugins are in the catalogue");
    expect(shape && inCategory == 35, "category Gridline Suite Mini, effects, one editor width, catalogue names");
    expect(presets, "2 to 6 factory presets each, every value in range");
    expect(names, "Mini names are unique");
  }

  // ---- creation, silence, extremes, block invariance -------------------------------
  {
    bool created = true, silent = true, finite = true, invariant = true, presetsFinite = true;
    for (const Entry& e : kEntries) {
      auto fx = createEffect(e.type);
      if (!fx) {
        created = false;
        continue;
      }
      const PluginInfo* info = findPlugin(e.type);
      // Silence in, silence out.
      {
        std::vector<float> L(24000, 0.f), R(24000, 0.f);
        run(*fx, defaultPluginState(e.type), L, R, 256);
        if (peak(L, 0, L.size()) > 1e-7 || peak(R, 0, R.size()) > 1e-7) {
          silent = false;
          std::printf("# %s: output from silence (%g)\n", e.type, peak(L, 0, L.size()));
        }
      }
      // Every parameter at its minimum, then at its maximum, with a loud input.
      for (int side = 0; side < 2; ++side) {
        PluginState s = defaultPluginState(e.type);
        for (size_t k = 0; k < info->params.size(); ++k) s.params[k] = static_cast<float>(side ? info->params[k].max : info->params[k].min);
        normalizePluginState(s);
        auto f2 = createEffect(e.type);
        auto L = noise(48000, 7, 1.0f), R = noise(48000, 11, 1.0f);
        run(*f2, s, L, R, 333);
        if (!finiteAll(L) || !finiteAll(R) || peak(L, 0, L.size()) > 200.0) {
          finite = false;
          std::printf("# %s: extremes (%s) peak %g\n", e.type, side ? "max" : "min", peak(L, 0, L.size()));
        }
      }
      for (size_t pr = 0; pr < info->presets.size(); ++pr) {
        auto f2 = createEffect(e.type);
        auto L = noise(48000, 3), R = noise(48000, 5);
        run(*f2, presetState(*info, pr), L, R, 512);
        if (!finiteAll(L) || !finiteAll(R) || peak(L, 0, L.size()) > 100.0) {
          presetsFinite = false;
          std::printf("# %s preset %s: peak %g\n", e.type, info->presets[pr].name.c_str(), peak(L, 0, L.size()));
        }
      }
      // Block-size invariance (defaults and every preset).
      for (size_t pr = 0; pr <= info->presets.size(); ++pr) {
        const PluginState s = pr == 0 ? defaultPluginState(e.type) : presetState(*info, pr - 1);
        auto a = createEffect(e.type), b = createEffect(e.type);
        auto L1 = noise(30000, 21), R1 = noise(30000, 22), L2 = L1, R2 = R1;
        run(*a, s, L1, R1, 1024);
        run(*b, s, L2, R2, 37);
        double d = 0;
        for (size_t i = 0; i < L1.size(); ++i) d = std::max({d, static_cast<double>(std::fabs(L1[i] - L2[i])), static_cast<double>(std::fabs(R1[i] - R2[i]))});
        if (d > 1e-5) {
          invariant = false;
          std::printf("# %s (%zu): block size changes the output by %g\n", e.type, pr, d);
        }
      }
    }
    expect(created, "every Mini type creates an effect");
    expect(silent, "silence in, silence out");
    expect(finite, "all parameters at their extremes stay finite and bounded");
    expect(presetsFinite, "every factory preset stays finite and bounded");
    expect(invariant, "output does not depend on the block size");
  }

  // ---- state normalisation ---------------------------------------------------------
  {
    bool ok = true;
    for (const Entry& e : kEntries) {
      const PluginInfo* info = findPlugin(e.type);
      PluginState s;
      s.type = e.type;
      normalizePluginState(s);
      if (s.params.size() != info->params.size()) ok = false;
      for (size_t k = 0; k < info->params.size(); ++k)
        if (s.params[k] != static_cast<float>(info->params[k].def)) ok = false;
      PluginState w = defaultPluginState(e.type);
      for (auto& v : w.params) v = 1e9f;
      w.params.push_back(5.f);
      normalizePluginState(w);
      if (w.params.size() != info->params.size()) ok = false;
      for (size_t k = 0; k < info->params.size(); ++k)
        if (w.params[k] > static_cast<float>(info->params[k].max) + 1e-6f) ok = false;
    }
    expect(ok, "short states take the defaults, wild values clamp, extra values drop");
  }

  // ---- behaviour: utility ------------------------------------------------------------
  {
    auto in = sine(440, 48000, 0.3);
    auto out = through(st("gl.minitrim", {{trim::Gain, -6.f}}), in);
    expect(std::fabs(db(rms(out, 24000, 48000) / rms(in, 24000, 48000)) + 6.0) < 0.01, "Trim: -6 dB is -6 dB");
    out = through(st("gl.minitrim", {{trim::FlipL, 1.f}}), in);
    expect(std::fabs(out[30000] + in[30000]) < 1e-6, "Trim: flip inverts the left channel");
  }
  {
    auto fx = createEffect("gl.miniweave");
    std::vector<float> L(4800, 0.3f), R(4800, -0.1f);
    run(*fx, st("gl.miniweave", {{weave::LL, 0}, {weave::RL, 1}, {weave::LR, 1}, {weave::RR, 0}}), L, R, 128);
    expect(std::fabs(L[4000] + 0.1f) < 1e-4 && std::fabs(R[4000] - 0.3f) < 1e-4, "Weave: swap exchanges left and right");
  }
  {
    auto fx = createEffect("gl.miniskew");
    std::vector<float> L(4800, 0.f), R(4800, 0.f);
    L[100] = R[100] = 1.f;
    run(*fx, st("gl.miniskew", {{skew::Delay, 8.f}, {skew::Side, 1.f}}), L, R, 64);
    size_t at = 0;
    for (size_t i = 0; i < R.size(); ++i)
      if (std::fabs(R[i]) > std::fabs(R[at])) at = i;
    expect(L[100] > 0.99f && at == 100 + 384, "Skew: the right side arrives 8 ms (384 samples) late");
  }
  {
    auto fx = createEffect("gl.minibreadth");
    auto L = sine(300, 24000, 0.3), R = sine(300, 24000, -0.3);  // pure side
    run(*fx, st("gl.minibreadth", {{breadth::Width, 0.f}}), L, R, 128);
    expect(peak(L, 12000, 24000) < 1e-3, "Breadth: width 0 removes the side signal");
  }
  // ---- tone --------------------------------------------------------------------------
  {
    const PluginState flat = st("gl.minibands");
    const double a = gainDb(flat, 100), b = gainDb(flat, 1000), c = gainDb(flat, 6000);
    expect(std::fabs(a) < 0.1 && std::fabs(b) < 0.1 && std::fabs(c) < 0.1, "Bands: flat at 0 dB (crossovers sum to unity)");
    expect(std::fabs(gainDb(st("gl.minibands", {{bands::Low, 6.f}}), 50) - 6.0) < 0.5, "Bands: +6 dB low band lifts 50 Hz by 6 dB");
  }
  {
    const PluginState lp12 = st("gl.minisieve", {{sieve::Cutoff, 500.f}});
    const double s12 = gainDb(lp12, 4000) - gainDb(lp12, 8000);
    const PluginState lp24 = st("gl.minisieve", {{sieve::Cutoff, 500.f}, {sieve::Slope, 1.f}});
    const double s24 = gainDb(lp24, 4000) - gainDb(lp24, 8000);
    std::printf("# Sieve slope: %.1f dB/oct (12), %.1f dB/oct (24)\n", s12, s24);
    expect(std::fabs(gainDb(lp12, 50)) < 0.3 && s12 > 11 && s12 < 15, "Sieve: low pass passes the lows and falls 12 dB per octave");
    expect(s24 > 22 && s24 < 30, "Sieve: 24 dB slope falls 24 dB per octave");
  }
  {
    const PluginState s = st("gl.minirungs", {{rungs::Cutoff, 250.f}, {rungs::Resonance, 0.f}});
    const double slope = gainDb(s, 2000) - gainDb(s, 4000);
    std::printf("# Rungs slope: %.1f dB/oct\n", slope);
    expect(slope > 21 && slope < 30 && gainDb(s, 40) > -2, "Rungs: ladder low pass falls 24 dB per octave");
    const PluginState r = st("gl.minirungs", {{rungs::Cutoff, 1000.f}, {rungs::Resonance, 0.8f}});
    expect(gainDb(r, 1000, 0.05) > gainDb(r, 300, 0.05) + 3, "Rungs: resonance peaks at the cutoff");
  }
  {
    const PluginState s = st("gl.miniscorch", {{scorch::Cutoff, 500.f}, {scorch::Drive, 0.f}});
    expect(gainDb(s, 100, 0.05) > -1.5 && gainDb(s, 5000, 0.05) < -30, "Scorch: low pass response at zero drive");
    auto in = sine(200, 48000, 0.8);
    auto out = through(st("gl.miniscorch", {{scorch::Cutoff, 8000.f}, {scorch::Drive, 1.f}}), in);
    expect(tone(out, 24000, 48000, 600) > 0.01 * tone(out, 24000, 48000, 200), "Scorch: drive adds harmonics");
  }
  {
    const PluginState s = st("gl.minivowel", {{vowel::X, 0.f}, {vowel::Y, 0.f}, {vowel::Sharpness, 1.f}});
    expect(gainDb(s, 280) > gainDb(s, 600) + 6, "Vowel: closed vowel puts the first formant at 280 Hz");
  }
  {
    const PluginState s = st("gl.minitines", {{tines::Freq, 200.f}, {tines::Feedback, 0.9f}, {tines::Damp, 0.f}, {tines::Mix, 1.f}});
    expect(gainDb(s, 400, 0.05) > gainDb(s, 500, 0.05) + 10, "Tines: comb peaks on the harmonics of its frequency");
  }
  {
    auto fx = createEffect("gl.minichime");
    std::vector<float> L(48000, 0.f), R(48000, 0.f);
    L[0] = R[0] = 1.f;
    run(*fx, st("gl.minichime", {{chime::Note, 69.f}, {chime::Mix, 1.f}, {chime::Decay, 1000.f}}), L, R, 256);
    expect(tone(L, 4800, 14400, 440) > 3 * tone(L, 4800, 14400, 600), "Chime: an impulse rings at the note (A4 = 440 Hz)");
  }
  // ---- dynamics ----------------------------------------------------------------------
  {
    auto in = sine(1000, 48000, 0.5);  // -6 dB peak
    auto out = through(st("gl.minipress", {{press::Threshold, -18.f}, {press::Ratio, 4.f}, {press::Knee, 0.f}}), in);
    const double outDb = db(peak(out, 36000, 48000));
    std::printf("# Press: -6 dB in -> %.2f dB out (target -15)\n", outDb);
    expect(std::fabs(outDb + 15.0) < 1.0, "Press: 4:1 above -18 dB turns -6 dB into -15 dB");
  }
  {
    auto in = noise(48000, 99, 1.0f);
    auto out = through(st("gl.miniceiling", {{ceiling::Input, 12.f}, {ceiling::Ceiling, -1.f}}), in);
    expect(peak(out, 0, out.size()) <= std::pow(10.0, -1.0 / 20.0) + 1e-6, "Ceiling: never above the ceiling");
    expect(effectLatency(st("gl.miniceiling"), 48000) == 95, "Ceiling: 2 ms look-ahead reported as latency");
  }
  {
    auto in = sine(100, 48000, 1.0);
    auto out = through(st("gl.minishear", {{shear::Ceiling, -6.f}, {shear::Soft, 0.f}}), in);
    expect(db(peak(out, 24000, 48000)) < -5.0 && db(peak(out, 24000, 48000)) > -7.0, "Shear: clips at the ceiling");
  }
  {
    auto in = sine(500, 96000, std::pow(10.0, -30.0 / 20.0));
    auto out = through(st("gl.minipack", {{pack::Threshold, -12.f}, {pack::Amount, 1.f}}), in);
    const double outDb = db(peak(out, 72000, 96000));
    std::printf("# Pack: -30 dB in -> %.2f dB out (target -12)\n", outDb);
    expect(std::fabs(outDb + 12.0) < 2.0, "Pack: pulls a quiet signal up to the target level");
  }
  {
    auto in = sine(500, 96000, std::pow(10.0, -60.0 / 20.0));
    auto out = through(st("gl.minispan", {{span::LowThresh, -40.f}, {span::LowRatio, 0.5f}}), in);
    const double outDb = db(peak(out, 72000, 96000));
    expect(outDb > -55.0 && outDb < -45.0, "Span: upward processing lifts a -60 dB signal by about 10 dB");
  }
  {
    auto quiet = through(st("gl.minilatch"), sine(500, 24000, 0.001));
    auto loud = through(st("gl.minilatch"), sine(500, 24000, 0.1));
    expect(peak(quiet, 12000, 24000) < 1e-5 && peak(loud, 12000, 24000) > 0.09, "Latch: closes under the threshold, opens above");
  }
  {
    std::vector<float> in(48000, 0.f);
    for (size_t i = 0; i < 24000; ++i) in[i] = static_cast<float>(0.3 * std::exp(-static_cast<double>(i) / 2000.0) * std::sin(2 * kPi * 200 * i / kSr));
    auto up = through(st("gl.minipunch", {{punch::Attack, 1.f}}), in);
    auto down = through(st("gl.minipunch", {{punch::Attack, -1.f}}), in);
    expect(peak(up, 0, 2400) > peak(in, 0, 2400) * 1.3 && peak(down, 0, 2400) < peak(in, 0, 2400) * 0.8, "Punch: attack raises / lowers the hit");
  }
  // ---- drive -------------------------------------------------------------------------
  {
    auto in = sine(200, 48000, 1.0);
    auto out = through(st("gl.miniburn", {{burn::Drive, 24.f}}), in);
    expect(peak(out, 24000, 48000) < 1.1 && tone(out, 24000, 48000, 600) > 0.1, "Burn: saturates (bounded, odd harmonics)");
    expect(effectLatency(st("gl.miniburn"), 48000) == 22, "Burn: oversampling latency reported");
  }
  {
    auto in = sine(200, 48000, 0.9);
    auto out = through(st("gl.minibend", {{bend::Shape, 5.f}, {bend::Amount, 1.f}}), in);
    expect(tone(out, 24000, 48000, 400) > 0.2, "Bend: bounce shape folds a sine up an octave");
  }
  {
    auto in = sine(300, 48000, 0.8);
    auto out = through(st("gl.miniwarp", {{warp::Drive, 1.f}, {warp::Tone, 2000.f}}), in);
    expect(tone(out, 24000, 48000, 600) + tone(out, 24000, 48000, 900) > 0.02, "Warp: phase distortion adds harmonics");
  }
  {
    auto in = sine(100, 48000, 0.8);
    auto out = through(st("gl.minicrumble", {{crumble::Bits, 1.f}}), in);
    bool levels = true;
    for (size_t i = 1000; i < out.size(); ++i)
      if (std::fabs(out[i]) > 1e-6f && std::fabs(std::fabs(out[i]) - 1.f) > 1e-6f) levels = false;
    expect(levels, "Crumble: 1 bit leaves three levels");
  }
  {
    auto in = sine(1000, 48000, 0.5);
    auto out = through(st("gl.miniclang", {{clang::Freq, 220.f}}), in);
    const double lo = tone(out, 24000, 48000, 780), hi = tone(out, 24000, 48000, 1220), mid = tone(out, 24000, 48000, 1000);
    std::printf("# Clang: 780 Hz %.3f, 1220 Hz %.3f, 1000 Hz %.4f\n", lo, hi, mid);
    expect(lo > 0.2 && hi > 0.2 && mid < 0.01, "Clang: 1 kHz x 220 Hz gives 780 and 1220 Hz, no carrier");
  }
  // ---- pitch -------------------------------------------------------------------------
  {
    auto in = sine(500, 48000, 0.5);
    auto out = through(st("gl.minislide", {{slide::Shift, 1000.f}}), in);
    const double up = tone(out, 24000, 48000, 1500), orig = tone(out, 24000, 48000, 500);
    std::printf("# Slide: 1500 Hz %.3f, 500 Hz %.4f\n", up, orig);
    expect(up > 0.4 && orig < 0.05, "Slide: +1 kHz moves 500 Hz to 1500 Hz");
  }
  {
    auto in = sine(1000, 48000, 0.5);
    auto out = through(st("gl.miniinterval", {{interval::Pitch, 12.f}}), in);
    const double oct = tone(out, 24000, 48000, 2000), orig = tone(out, 24000, 48000, 1000);
    std::printf("# Interval: 2000 Hz %.3f, 1000 Hz %.4f\n", oct, orig);
    expect(oct > 4 * orig && oct > 0.2, "Interval: +12 semitones moves 1 kHz to 2 kHz");
  }
  // ---- motion ------------------------------------------------------------------------
  for (const char* ty : {"gl.minidouble", "gl.minicrowd", "gl.minijet", "gl.miniorbit"}) {
    auto in = noise(48000, 5);
    auto out = through(st(ty), in);
    double d = 0;
    for (size_t i = 24000; i < out.size(); ++i) d = std::max(d, static_cast<double>(std::fabs(out[i] - in[i])));
    expect(d > 0.01 && rms(out, 24000, 48000) > 0.3 * rms(in, 24000, 48000), (std::string(ty) + ": modulated wet signal mixed in").c_str());
  }
  // ---- time --------------------------------------------------------------------------
  {
    auto fx = createEffect("gl.miniecho");
    std::vector<float> L(24000, 0.f), R(24000, 0.f);
    L[0] = R[0] = 1.f;
    run(*fx, st("gl.miniecho", {{echo::Sync, 0.f}, {echo::Time, 100.f}, {echo::Feedback, 0.5f}, {echo::Mix, 1.f}}), L, R, 128);
    expect(std::fabs(L[4800] - 1.f) < 1e-3 && std::fabs(L[9600] - 0.5f) < 1e-3 && std::fabs(L[14400] - 0.25f) < 1e-3,
           "Echo: repeats every 100 ms, halving with 50% feedback");
  }
  {
    auto fx = createEffect("gl.minivolley");
    std::vector<float> L(48000, 0.f), R(48000, 0.f);
    L[0] = R[0] = 1.f;
    run(*fx, st("gl.minivolley", {{volley::Time, 100.f}, {volley::Ratio, 1.5f}, {volley::Mix, 1.f}}), L, R, 128);
    expect(std::fabs(L[4800] - 1.f) < 1e-3 && std::fabs(R[7200] - 1.f) < 1e-3, "Volley: left at the time, right at time x ratio");
  }
  {
    auto fx = createEffect("gl.minivault");
    std::vector<float> L(96000, 0.f), R(96000, 0.f);
    L[0] = R[0] = 1.f;
    run(*fx, st("gl.minivault", {{vault::Decay, 2.5f}, {vault::Mix, 1.f}, {vault::Damp, 0.f}}), L, R, 256);
    const double early = rms(L, 9600, 19200), late = rms(L, 48000, 57600);
    const double drop = db(early / late), expected = 0.8 * 60.0 / 2.5;
    std::printf("# Vault: %.1f dB over 0.8 s (RT60 2.5 s -> %.1f dB)\n", drop, expected);
    expect(late > 0 && drop > expected * 0.6 && drop < expected * 1.5, "Vault: tail decays at the set RT60");
  }
  {
    auto fx = createEffect("gl.minirewind");
    std::vector<float> L(19200), R;
    for (size_t i = 0; i < L.size(); ++i) L[i] = static_cast<float>(i) / 20000.f;
    R = L;
    const std::vector<float> in = L;
    run(*fx, st("gl.minirewind", {{rewind::Sync, 0.f}, {rewind::Length, 100.f}, {rewind::Fade, 0.f}}), L, R, 100);
    bool rev = true;
    for (size_t k = 0; k < 4800; k += 97)
      if (std::fabs(L[4800 + k] - in[4799 - k]) > 1e-4f) rev = false;
    expect(rev, "Rewind: each 100 ms segment plays the previous one backwards");
  }
  {
    auto in = sine(300, 48000, 0.5);
    auto pass = through(st("gl.minihalt"), in);
    bool same = true;
    for (size_t i = 0; i < in.size(); ++i)
      if (pass[i] != in[i]) same = false;
    auto stop = through(st("gl.minihalt", {{halt::Run, 0.f}, {halt::StopTime, 200.f}}), in);
    expect(same && peak(stop, 24000, 48000) < 1e-6 && peak(stop, 0, 2400) > 0.3, "Halt: passes through while running, winds down to silence when stopped");
  }
  {
    std::vector<float> in(48000, 0.5f);
    auto out = through(st("gl.minipulse", {{pulse::Attack, 0.5f}, {pulse::Release, 1.f}}), in);
    const double stepN = 0.25 * 60.0 / 140.0 * kSr;  // 1/16 at the default 140 BPM
    const float on = out[static_cast<size_t>(stepN * 4.5)], off = out[static_cast<size_t>(stepN * 5.5)];
    expect(std::fabs(on - 0.5f) < 0.01f && std::fabs(off) < 0.01f, "Pulse: alternating steps open and close the gate");
  }
  return finish();
}
