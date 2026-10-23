// Gridline — J-92 user report: "the bitcrush effect, when I tweaked the bits
// parameter it had a cap and wouldn't make bits high". Every bit depth 1..24
// of gl.bitcrush is reachable (descriptor, knob law, ParamId set / get,
// project save / load) and audibly cleaner than the one below it, up to a
// near-transparent 24 bits; the other bit crushers (Tracer's bit depth, Hex
// Synth lo-fi bits) reach their top too.
//
// J-98 user report: "the bits parameter really only makes a difference at
// around 5.6 bits, anything over sounds very similar". The quantiser is right
// (6 dB per bit, continuous in fractional bits); the knob law is logarithmic
// so most of the travel is the audible range, and schema v7 moves old
// automation to the new law.
//
// J-104 user report: "model it off where kHz is the main parameter". Rate (Hz,
// log knob, "Off" at the top) replaced the 1..50 Downsample divider: a phase
// accumulated sample-and-hold at any rate, anti-alias / smoothing converter
// filters and dither. Tested here: the readout matches the measured hold rate,
// automated sweeps have no clicks or zipper, the filters remove aliasing and
// images, Rate and Bits change the sound across their whole travel (printed
// as a table), and old states / projects / presets render identically.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>
#include "../src/core/Params.h"
#include "../src/core/PluginInfo.h"
#include "../src/core/PluginParams.h"
#include "../src/core/Presets.h"
#include "../src/core/Project.h"
#include "../src/core/Song.h"
#include "../src/engine/Bitcrush.h"
#include "../src/engine/Effects.h"
#include "Check.h"
#include "Hermetic.h"

using namespace gl;

namespace {

constexpr double kSr = 44100;
constexpr double kPi = 3.141592653589793;

using Signal = std::vector<float>;
using Automate = std::function<void(PluginState&, int pos)>;

// Renders a mono signal (both channels) through the effect in 64-frame blocks.
Signal render(PluginState st, const Signal& in, double sr = kSr, const Automate& autom = nullptr, int block = 64) {
  auto fx = createEffect(st.type);
  if (!fx) return {};
  fx->prepareHost(st, sr);
  Signal L = in, R = in;
  ProcessContext c;
  c.sampleRate = sr, c.offline = true;
  const int n = static_cast<int>(in.size());
  for (int pos = 0; pos < n; pos += block) {
    if (autom) autom(st, pos);
    fx->process(st, c, L.data() + pos, R.data() + pos, std::min(block, n - pos));
  }
  return L;
}

Signal sine(double hz, int frames, double amp = 0.5, double sr = kSr) {
  Signal s(static_cast<size_t>(frames));
  for (int i = 0; i < frames; ++i) s[static_cast<size_t>(i)] = static_cast<float>(amp * std::sin(2 * kPi * hz * i / sr));
  return s;
}
Signal noise(int frames, double amp = 0.3) {
  Signal s(static_cast<size_t>(frames));
  uint32_t r = 12345;
  for (auto& v : s) {
    r = r * 1664525u + 1013904223u;
    v = static_cast<float>(((r >> 8) * (1.0 / 16777216.0) * 2 - 1) * amp);
  }
  return s;
}
// A "music-like" multi-sine: octaves from 110 Hz to 14 kHz, falling 3 dB per octave.
Signal multiSine(int frames, double sr = kSr) {
  Signal s(static_cast<size_t>(frames), 0.f);
  double amp = 0.22;
  for (double f = 110; f < 15000; f *= 2, amp *= 0.7071)
    for (int i = 0; i < frames; ++i) s[static_cast<size_t>(i)] += static_cast<float>(amp * std::sin(2 * kPi * f * i / sr + f * 0.001));
  return s;
}

// Error of the crushed signal against the dry one (dB re the dry RMS).
double errorDb(const Signal& dry, const Signal& wet, int from = 1000) {
  double e = 0, s = 0;
  for (size_t i = static_cast<size_t>(from); i < dry.size(); ++i) {
    const double d = static_cast<double>(wet[i]) - dry[i];
    e += d * d, s += static_cast<double>(dry[i]) * dry[i];
  }
  return e > 0 ? 10 * std::log10(e / s) : -300;
}
double errorDb(PluginState st, int frames = 22050) {
  const Signal dry = sine(997, frames);
  return errorDb(dry, render(st, dry));
}
// Level of one frequency (dB re full scale sine), Hann-windowed DFT bin.
double toneDb(const Signal& x, double hz, double sr = kSr, int from = 4000) {
  const size_t n = x.size() - static_cast<size_t>(from);
  double re = 0, im = 0, wsum = 0;
  for (size_t i = 0; i < n; ++i) {
    const double w = 0.5 - 0.5 * std::cos(2 * kPi * i / (n - 1));
    const double v = x[i + static_cast<size_t>(from)] * w;
    re += v * std::cos(2 * kPi * hz * i / sr), im -= v * std::sin(2 * kPi * hz * i / sr), wsum += w;
  }
  return 20 * std::log10(2 * std::sqrt(re * re + im * im) / wsum + 1e-12);
}

PluginState crusher(double rate, double bits) {
  PluginState st = defaultPluginState("gl.bitcrush");
  st.params[pp::BcRate] = static_cast<float>(rate);
  st.params[pp::BcBits] = static_cast<float>(bits);
  st.params[pp::BcMix] = 1;
  return st;
}

void testBits() {
  const PluginInfo* info = findPlugin("gl.bitcrush");
  expect(info != nullptr, "gl.bitcrush exists");
  if (!info) return;
  const ParamDesc& d = info->params[pp::BcBits];
  expect(d.fromNormalized(1.0) == 24.0 && d.fromNormalized(0.0) == 1.0, "Bits: knob travel 0..1 reaches 1..24");
  expect(d.clamp(24) == 24 && d.format(24) != d.format(23), "Bits: 24 is a distinct, unclamped value");
  {  // J-98: knob law. Most of the travel is the audible range (1..12 bits).
    const double n8 = d.toNormalized(8), n12 = d.toNormalized(12);
    char b[96];
    std::snprintf(b, sizeof b, "Bits knob law: 1..8 bits %.0f %% of the travel, 1..12 bits %.0f %%", n8 * 100, n12 * 100);
    expect(n8 > 0.6 && n12 > 0.75 && 1 - n12 < 0.25, b);
    expect(std::fabs(d.fromNormalized(d.toNormalized(5.6)) - 5.6) < 1e-9, "Bits knob law round trips");
    expect(d.format(5.6) == "5.6 bits" && d.format(24) == "24.0 bits", "Bits value text keeps fractional bits (5.6 is not shown as 6)");
  }
  PluginState st = crusher(pp::kBcRateOff, 8);  // Rate off isolates the bit depth
  double prev = 999;
  int ok = 0;
  std::string line;
  for (int b = 1; b <= 24; ++b) {
    st.params[pp::BcBits] = static_cast<float>(b);
    const double e = errorDb(st);
    char buf[32];
    std::snprintf(buf, sizeof buf, " %d:%.0f", b, e);
    line += buf;
    if (e < prev - 3 || e < -130) ++ok;  // each extra bit about 6 dB cleaner, until the float floor
    prev = e;
  }
  std::printf("# bits (rate off) -> error dB:%s\n", line.c_str());
  expect(ok == 24, "Bits 1..24: every extra bit makes the output cleaner (monotonic distortion)");
  st.params[pp::BcBits] = 24;
  expect(errorDb(st) < -125, "Bits 24, Rate off: near transparent");
  st.params[pp::BcBits] = 1;
  expect(errorDb(st) > -15, "Bits 1: heavily crushed");
  st.params[pp::BcBits] = 12.0f;
  const double e12 = errorDb(st);
  st.params[pp::BcBits] = 12.5f;
  const double e125 = errorDb(st);
  st.params[pp::BcBits] = 13.0f;
  const double e13 = errorDb(st);
  expect(e125 < e12 && e125 > e13, "Bits 12 / 12.5 / 13: fractional depths in between");
  // Dither: at 4 bits it trades the harmonic distortion for noise.
  PluginState d4 = crusher(pp::kBcRateOff, 4);
  const Signal x = sine(1000, 44100);
  const double h0 = toneDb(render(d4, x), 3000);
  d4.params[pp::BcDither] = 1;
  const double h1 = toneDb(render(d4, x), 3000);
  char b[96];
  std::snprintf(b, sizeof b, "Dither 100%% at 4 bits: 3rd harmonic %.1f -> %.1f dB", h0, h1);
  expect(h1 < h0 - 15, b);
}

// Descriptor: log knob 50 Hz .. Off, readout "11.0 kHz" / "820 Hz" / "Off".
void testRateDescriptor() {
  const PluginInfo* info = findPlugin("gl.bitcrush");
  if (!info) return;
  const ParamDesc& d = info->params[pp::BcRate];
  expect(d.name == "Rate" && d.scale == ParamScale::Log && d.min == pp::kBcRateMin && d.max == pp::kBcRateOff, "Rate: log knob 50 Hz .. top");
  expect(d.format(11025) == "11.0 kHz" && d.format(820) == "820 Hz" && d.format(pp::kBcRateOff) == "Off" && d.format(4410) == "4.41 kHz",
         "Rate readout: 11.0 kHz / 820 Hz / 4.41 kHz / Off");
  expect(d.format(d.fromNormalized(1.0)) == "Off" && d.fromNormalized(0.0) == 50.0, "Rate: the knob's travel reaches 50 Hz and Off");
  expect(std::fabs(d.toNormalized(11025) - 0.75) < 0.06, "Rate: 11 kHz sits near three quarters of the travel");
  const PluginState def = defaultPluginState("gl.bitcrush");
  expect(def.params[pp::BcRate] == 11025.f && def.params.size() == static_cast<size_t>(pp::BcCount), "default instance: Rate 11.0 kHz");
}

// The readout matches the measured hold rate (at 44.1 and 48 kHz, fractional periods too).
void testRateReadout() {
  const PluginInfo* info = findPlugin("gl.bitcrush");
  if (!info) return;
  double worst = 0;
  std::string line;
  for (double sr : {44100.0, 48000.0})
    for (double rate : {60.0, 100.0, 820.0, 2205.0, 3000.0, 7777.0, 11025.0, 15000.0, 30000.0}) {
      // Held values = changes in the output for noise (24 bits: equal neighbours never happen).
      PluginState st = crusher(rate, 24);
      const int n = static_cast<int>(sr);
      const Signal y = render(st, noise(n), sr);
      int changes = 0;
      for (int i = 1; i < n; ++i) changes += y[static_cast<size_t>(i)] != y[static_cast<size_t>(i - 1)];
      const double measured = changes * sr / (n - 1);
      const std::string txt = info->params[pp::BcRate].format(rate);
      double shown = std::atof(txt.c_str()) * (txt.find("kHz") != std::string::npos ? 1000.0 : 1.0);
      const double err = std::fabs(measured - shown) / shown;
      worst = std::max(worst, err);
      if (sr == 44100.0) {
        char b[64];
        std::snprintf(b, sizeof b, " %s=%.0f", txt.c_str(), measured);
        line += b;
      }
    }
  std::printf("# rate readout -> measured holds/s at 44.1 kHz:%s\n", line.c_str());
  char b[96];
  std::snprintf(b, sizeof b, "Rate readout matches the measured hold rate (worst %.2f %%)", worst * 100);
  expect(worst < 0.006, b);
  PluginState off = crusher(pp::kBcRateOff, 24);
  const Signal x = noise(8000);
  expect(errorDb(x, render(off, x)) < -125, "Rate Off: every sample passes (transparent at 24 bits)");
}

// Automated Rate sweeps: smooth, no clicks or zipper; the hold count follows the rate.
void testSweep() {
  const int n = static_cast<int>(kSr * 2);
  auto sweep = [&](PluginState& st, int pos) {  // 50 Hz -> 20 kHz -> 50 Hz, exponential, per 64-frame block
    const double t = static_cast<double>(pos) / n, u = t < 0.5 ? t * 2 : 2 - t * 2;
    st.params[pp::BcRate] = static_cast<float>(50.0 * std::pow(400.0, u));
  };
  // With both converter filters on, the output is smooth: no step can be larger than the
  // input's own slope allows (a click or zipper step would be).
  PluginState st = crusher(50, 24);
  st.params[pp::BcAntiAlias] = 1, st.params[pp::BcSmooth] = 1;
  const Signal x = sine(330, n, 0.5);
  const Signal y = render(st, x, kSr, sweep);
  double maxStep = 0;
  for (int i = 2000; i < n; ++i) maxStep = std::max(maxStep, std::fabs(static_cast<double>(y[static_cast<size_t>(i)]) - y[static_cast<size_t>(i - 1)]));
  const double slope = 0.5 * 2 * kPi * 330 / kSr;
  char b[128];
  std::snprintf(b, sizeof b, "Rate sweep 50 Hz..20 kHz with filters: largest step %.4f (input slope %.4f), no clicks", maxStep, slope);
  expect(maxStep < slope * 1.6, b);
  // Zipper: the same sweep stepped once per 64 frames vs once per 2048 frames; the glide makes
  // the coarse automation as smooth (energy above 16 kHz stays low).
  auto hfDb = [&](const Signal& s) {
    double e = 0;
    for (int i = 2000; i + 2 < n; ++i) {
      const double d2 = static_cast<double>(s[static_cast<size_t>(i)]) - 2.0 * s[static_cast<size_t>(i + 1)] + s[static_cast<size_t>(i + 2)];
      e += d2 * d2;
    }
    return 10 * std::log10(e / (n - 2002) + 1e-30);
  };
  const Signal coarse = render(st, x, kSr, sweep, 2048);
  const double hfFine = hfDb(y), hfCoarse = hfDb(coarse), hfStatic = hfDb(sine(330, n, 0.5));
  std::snprintf(b, sizeof b, "Rate sweep HF (2nd difference) energy: per-64 %.1f dB, per-2048 %.1f dB (dry %.1f dB)", hfFine, hfCoarse, hfStatic);
  expect(hfCoarse < hfFine + 3 && hfFine < hfStatic + 6, b);
  // The phase accumulator: holds taken during a sweep equal the integral of rate / sr.
  crush::Crusher core;
  core.reset();
  double expected = 0;
  for (int i = 0; i < n; ++i) {
    const double t = static_cast<double>(i) / n, u = t < 0.5 ? t * 2 : 2 - t * 2, rate = 50.0 * std::pow(400.0, u);
    const double P = crush::periodFor(rate, kSr, pp::kBcRateOff);
    double l = 0, r = 0;
    core.frame(l, r, P, 1 << 23, 0, 0);
    expected += 1.0 / P;
  }
  std::snprintf(b, sizeof b, "sweep: %llu holds, rate integral %.1f", static_cast<unsigned long long>(core.holds()), expected);
  expect(std::fabs(static_cast<double>(core.holds()) - expected) < 2.0, b);
}

// Anti-alias removes the folded input, Smoothing removes the images above the hold rate.
void testFilters() {
  const double rate = kSr / 16;  // 2756.25 Hz
  const Signal x2k = sine(2000, 44100), x1k = sine(1000, 44100);
  PluginState st = crusher(rate, 24);
  const double alias0 = toneDb(render(st, x2k), rate - 2000);
  const double image0 = toneDb(render(st, x1k), rate - 1000);
  st.params[pp::BcAntiAlias] = 1;
  const double alias1 = toneDb(render(st, x2k), rate - 2000);
  st.params[pp::BcAntiAlias] = 0, st.params[pp::BcSmooth] = 1;
  const double image1 = toneDb(render(st, x1k), rate - 1000);
  const double fund1 = toneDb(render(st, x1k), 1000);
  st.params[pp::BcAntiAlias] = 0.5, st.params[pp::BcSmooth] = 0;
  const double aliasHalf = toneDb(render(st, x2k), rate - 2000);
  std::printf("# filters at %.0f Hz: alias of 2 kHz at %.0f Hz: AA 0 %.1f dB, AA 50%% %.1f dB, AA 100%% %.1f dB; image of 1 kHz at %.0f Hz: "
              "smoothing 0 %.1f dB, 100%% %.1f dB (1 kHz itself %.1f dB)\n",
              rate, rate - 2000, alias0, aliasHalf, alias1, rate - 1000, image0, image1, fund1);
  expect(alias0 > -12 && alias1 < alias0 - 35, "Anti-alias 100%: the folded 2 kHz (at 756 Hz) drops by more than 35 dB");
  expect(aliasHalf < alias0 + 1 && aliasHalf > alias1, "Anti-alias 50%: in between");
  expect(image0 > -20 && image1 < image0 - 30, "Smoothing 100%: the image above the hold rate drops by more than 30 dB");
  expect(fund1 > -10, "Smoothing keeps the wanted signal below half the hold rate");
}

// Rate and Bits change the sound across the whole travel: noise (error re dry) and
// the spectrum (energy above 5 kHz, images / aliasing) per knob position.
void testTravelTable() {
  const PluginInfo* info = findPlugin("gl.bitcrush");
  if (!info) return;
  const Signal x = multiSine(32768);
  std::printf("# Rate travel (Bits 24, multi-sine 110 Hz..14 kHz)\n#  knob  rate        error dB  7k/14k dB  440 Hz dB\n");
  double prevErr = 1e9;
  int moved = 0, steps = 0;
  for (int k = 0; k <= 20; ++k) {
    const double n = k / 20.0, rate = info->params[pp::BcRate].fromNormalized(n);
    const Signal y = render(crusher(rate, 24), x);
    const double err = errorDb(x, y);
    const double hf = std::max(toneDb(y, 7040), toneDb(y, 14080));  // the top octaves of the test signal
    std::printf("#  %.2f  %-10s  %7.1f  %7.1f  %7.1f\n", n, info->params[pp::BcRate].format(rate).c_str(), err, hf, toneDb(y, 440));
    if (k > 0) {
      ++steps;
      if (std::fabs(err - prevErr) > 0.3) ++moved;
    }
    prevErr = err;
  }
  char b[160];
  std::snprintf(b, sizeof b, "Rate: %d of %d knob steps (5 %%) change the error by more than 0.3 dB", moved, steps);
  expect(moved >= steps - 1, b);
  // Below about 1.6 bits the quantiser's first step (half of 1 / 2^(bits-1)) is above the
  // multi-sine's peaks (0.6), so it is gated to silence; a full-scale sine shows that range.
  std::printf("# Bits travel (Rate Off)\n#  knob  bits       multi-sine dB  full-scale sine dB\n");
  const Signal loud = sine(997, 22050, 0.98);
  double prevLoud = 1e9;
  prevErr = 1e9, moved = 0, steps = 0;
  for (int k = 0; k <= 20; ++k) {
    const double n = k / 20.0, bits = info->params[pp::BcBits].fromNormalized(n);
    const double err = errorDb(x, render(crusher(pp::kBcRateOff, bits), x)), errLoud = errorDb(loud, render(crusher(pp::kBcRateOff, bits), loud));
    std::printf("#  %.2f  %-9s  %7.1f        %7.1f\n", n, info->params[pp::BcBits].format(bits).c_str(), err, errLoud);
    if (k > 0) {
      ++steps;
      if (err < prevErr - 1.0 || (bits < 2 && std::fabs(errLoud - prevLoud) > 0.3)) ++moved;
    }
    prevErr = err, prevLoud = errLoud;
  }
  std::snprintf(b, sizeof b, "Bits: %d of %d knob steps (5 %%) change the noise by more than 1 dB (0.3 dB at full scale below 2 bits)", moved, steps);
  expect(moved == steps, b);
}

// The engine before J-104 (Downsample N, integer counter, hold = current sample).
Signal legacyRender(const Signal& in, double bits, double ds) {
  Signal out(in.size());
  double cnt = 0, hold = 0;
  const double steps = std::pow(2.0, bits - 1);
  ds = std::max(1.0, ds);
  for (size_t i = 0; i < in.size(); ++i) {
    cnt += 1.0;
    if (cnt >= ds) {
      cnt -= ds;
      hold = std::round(in[i] * steps) / steps;
    }
    out[i] = static_cast<float>(hold);
  }
  return out;
}

PluginState oldState(double bits, double ds) {  // 13 values as saved before J-104
  PluginState st = defaultPluginState("gl.bitcrush");
  st.params.resize(static_cast<size_t>(pp::kBcV1Count));
  st.params[pp::BcBits] = static_cast<float>(bits);
  st.params[1] = static_cast<float>(ds);
  return st;
}

// Old states / presets / projects: Downsample N -> 44.1 kHz / N, identical renders.
void testLegacy() {
  const Signal x = multiSine(22050);
  double worst = 0;
  bool rates = true;
  for (double ds : {1.0, 2.0, 3.0, 4.0, 7.0, 11.0, 16.0, 33.0, 50.0}) {
    PluginState st = oldState(6.5, ds);
    normalizePluginState(st);
    rates = rates && st.params.size() == static_cast<size_t>(pp::BcCount) &&
            (ds == 1.0 ? st.params[pp::BcRate] == static_cast<float>(pp::kBcRateOff) : std::fabs(st.params[pp::BcRate] - 44100.0 / ds) < 0.01);
    const Signal a = render(st, x), b = legacyRender(x, 6.5, ds);
    for (size_t i = 0; i < a.size(); ++i) worst = std::max(worst, std::fabs(static_cast<double>(a[i]) - b[i]));
  }
  expect(rates, "old states: Downsample N loads as Rate 44.1 kHz / N (1 = Off)");
  char b[96];
  std::snprintf(b, sizeof b, "old states render identically to the pre-J-104 engine (max difference %.2e)", worst);
  expect(worst < 1e-6, b);
  // Normalizing twice does not convert again; new states are left alone.
  PluginState st = oldState(8, 4);
  normalizePluginState(st);
  normalizePluginState(st);
  expect(std::fabs(st.params[pp::BcRate] - 11025.f) < 0.01f, "normalizing again keeps Rate (no second conversion)");
  // The reference follows the app's audio rate (48 kHz device: /4 -> 12 kHz).
  setLegacyRateReference(48000);
  PluginState s48 = oldState(8, 4);
  normalizePluginState(s48);
  setLegacyRateReference(44100);
  const Signal y48 = render(s48, sine(997, 9600, 0.5, 48000), 48000), l48 = legacyRender(sine(997, 9600, 0.5, 48000), 8, 4);
  double w48 = 0;
  for (size_t i = 0; i < y48.size(); ++i) w48 = std::max(w48, std::fabs(static_cast<double>(y48[i]) - l48[i]));
  expect(s48.params[pp::BcRate] == 12000.f && w48 < 1e-6, "48 kHz app: Downsample 4 loads as 12.0 kHz and renders the same at 48 kHz");
  // An old user preset file (13 values).
  const std::string root = (std::filesystem::path(test::hermeticTestEnv()) / "presets").string();
  expect(savePreset(root, oldState(5, 8), "old crush"), "write an old-format preset");
  PluginState pr;
  expect(loadPreset((std::filesystem::path(root) / "gl.bitcrush" / "old crush.json").string(), "gl.bitcrush", pr) &&
             std::fabs(pr.params[pp::BcRate] - 44100.0f / 8) < 0.01f && pr.params[pp::BcBits] == 5.f && pr.params.size() == static_cast<size_t>(pp::BcCount),
         "old preset: Downsample 8 loads as 5.51 kHz");
}

// v7 project: plugin state, automation clip, pattern lane and link ranges on the old
// Downsample index load as the equivalent rates; the render matches the old engine.
void testProjectMigration() {
  const PluginInfo* info = findPlugin("gl.bitcrush");
  if (!info) return;
  const ParamDesc& d = info->params[pp::BcRate];
  auto oldNorm = [](double ds) { return (ds - 1.0) / 49.0; };  // linear 1..50
  Song s = makeEmptySong();
  s.mixer[1].slots[0].plugin = oldState(6, 4);
  const ObjectId t = s.mixer[1].id;
  const ParamId rate = slotParam(t, 0, pp::BcRate), bits = slotParam(t, 0, pp::BcBits);
  AutomationClip c;
  c.id = s.nextId++;
  c.owner = rate.owner, c.index = rate.index;
  c.min = static_cast<float>(oldNorm(2)), c.max = static_cast<float>(oldNorm(40));
  for (double ds : {2.0, 3.0, 8.0, 20.0, 40.0})
    c.points.push_back({static_cast<Tick>(ds * 96), static_cast<float>((oldNorm(ds) - c.min) / (c.max - c.min)), 0, AutoShape::Curve});
  AutomationClip other = c;  // Bits: untouched by this migration
  other.id = s.nextId++;
  other.owner = bits.owner, other.index = bits.index;
  s.automations = {c, other};
  ControllerLink l;
  l.owner = rate.owner, l.index = rate.index, l.min = static_cast<float>(oldNorm(1)), l.max = static_cast<float>(oldNorm(16));
  s.links.push_back(l);
  PatternAutomation lane;
  lane.owner = rate.owner, lane.index = rate.index;
  for (double ds : {2.0, 5.0, 12.0}) lane.points.push_back({static_cast<Tick>(ds * 96), static_cast<float>(oldNorm(ds)), 0, AutoShape::Hold});
  s.patterns[0].automation.push_back(lane);
  Json j = songToJson(s);
  j.object["version"] = Json::makeNumber(7);  // as saved before J-104
  // songToJson wrote the 13-value state as is
  Song back;
  const bool ok = songFromJson(j, back) && back.automations.size() == 2 && back.links.size() == 1 && !back.patterns[0].automation.empty();
  expect(ok, "schema v7 project with Downsample automation loads");
  if (!ok) return;
  const PluginState& st = back.mixer[1].slots[0].plugin;
  expect(st.params.size() == static_cast<size_t>(pp::BcCount) && std::fabs(st.params[pp::BcRate] - 11025.f) < 0.01f, "v7 project: Downsample 4 -> 11.0 kHz");
  double worst = 0;  // relative rate error
  const AutomationClip& bc = back.automations[0];
  for (size_t i = 0; i < bc.points.size(); ++i) {
    const double ds = 1 + 49 * (c.min + c.points[i].value * (c.max - c.min));
    const double got = d.fromNormalized(bc.min + bc.points[i].value * (bc.max - bc.min));
    worst = std::max(worst, std::fabs(got - 44100 / ds) / (44100 / ds));
  }
  for (size_t i = 0; i < lane.points.size(); ++i) {
    const double ds = 1 + 49 * lane.points[i].value, got = d.fromNormalized(back.patterns[0].automation[0].points[i].value);
    worst = std::max(worst, std::fabs(got - 44100 / ds) / (44100 / ds));
  }
  const double l0 = d.fromNormalized(back.links[0].min), l1 = d.fromNormalized(back.links[0].max);
  worst = std::max({worst, std::fabs(l1 - 44100 / 16.0) / (44100 / 16.0)});
  char b[128];
  std::snprintf(b, sizeof b, "v7 Downsample automation / pattern lane / link range keep their rates (worst %.4f %%; link %s .. %s)", worst * 100,
                d.format(l0).c_str(), d.format(l1).c_str());
  expect(worst < 1e-4 && d.format(l0) == "Off", b);
  bool same = back.automations[1].min == other.min && back.automations[1].max == other.max;
  expect(same, "automation of other parameters is untouched");
  const Signal x = multiSine(22050);
  const Signal a = render(st, x), ref = legacyRender(x, 6, 4);
  double w = 0;
  for (size_t i = 0; i < a.size(); ++i) w = std::max(w, std::fabs(static_cast<double>(a[i]) - ref[i]));
  std::snprintf(b, sizeof b, "v7 project renders identically to the pre-J-104 engine (max difference %.2e)", w);
  expect(w < 1e-6, b);
  Json j8 = songToJson(back);
  Song again;
  expect(songFromJson(j8, again) && again.automations[0] == back.automations[0] && again.links[0].min == back.links[0].min &&
             again.mixer[1].slots[0].plugin.params == st.params,
         "v8 projects load without remapping");
}

// Song-level: ParamId set / get and project save / load keep the extremes.
void testRoundTrip() {
  Song s = makeEmptySong();
  s.mixer[1].slots[0].plugin = defaultPluginState("gl.bitcrush");
  const ObjectId t = s.mixer[1].id;
  expect(setParam(s, slotParam(t, 0, pp::BcBits), 24.0) && setParam(s, slotParam(t, 0, pp::BcRate), 50.0) &&
             setParam(s, slotParam(t, 0, pp::BcDither), 1.0),
         "set Bits 24 / Rate 50 Hz / Dither 100%");
  Json j;
  Song back;
  expect(jsonParse(jsonStringify(songToJson(s)), j) && songFromJson(j, back), "project round trip");
  expect(getParam(back, slotParam(t, 0, pp::BcBits)).value_or(0) == 24.0 && getParam(back, slotParam(t, 0, pp::BcRate)).value_or(0) == 50.0 &&
             getParam(back, slotParam(t, 0, pp::BcDither)).value_or(0) == 1.0,
         "Bits 24 / Rate 50 Hz / Dither survive save / load");
}

// J-98: projects saved before the logarithmic Bits law (schema <= 6) play the
// same bit depths: automation clips, pattern automation and controller link
// ranges that target Bits are moved to the new law on load; other targets stay.
void testLawMigration() {
  const PluginInfo* info = findPlugin("gl.bitcrush");
  const ParamDesc& d = info->params[pp::BcBits];
  auto oldNorm = [](double bits) { return (bits - 1.0) / 23.0; };  // linear law before J-98
  Song s = makeEmptySong();
  s.mixer[1].slots[0].plugin = defaultPluginState("gl.bitcrush");
  s.mixer[1].slots[1].plugin = defaultPluginState("gl.bitcrush");
  const ObjectId t = s.mixer[1].id;
  const ParamId bits = slotParam(t, 0, pp::BcBits), mix = slotParam(t, 1, pp::BcMix);
  AutomationClip c;
  c.id = s.nextId++;
  c.owner = bits.owner, c.index = bits.index;
  c.min = static_cast<float>(oldNorm(2)), c.max = static_cast<float>(oldNorm(20));
  for (double b : {2.0, 4.0, 8.0, 12.0, 20.0}) c.points.push_back({static_cast<Tick>(b * 96), static_cast<float>((oldNorm(b) - c.min) / (c.max - c.min)), 0, AutoShape::Curve});
  AutomationClip other = c;
  other.id = s.nextId++;
  other.owner = mix.owner, other.index = mix.index;
  s.automations = {c, other};
  ControllerLink l;
  l.owner = bits.owner, l.index = bits.index, l.min = static_cast<float>(oldNorm(4)), l.max = static_cast<float>(oldNorm(16));
  s.links.push_back(l);
  PatternAutomation lane;
  lane.owner = bits.owner, lane.index = bits.index;
  for (double b : {3.0, 6.0, 10.0}) lane.points.push_back({static_cast<Tick>(b * 96), static_cast<float>(oldNorm(b)), 0, AutoShape::Hold});
  s.patterns[0].automation.push_back(lane);
  Json j = songToJson(s);
  j.object["version"] = Json::makeNumber(6);
  Song back;
  const bool ok = songFromJson(j, back) && back.automations.size() == 2 && back.links.size() == 1 && !back.patterns[0].automation.empty();
  expect(ok, "schema v6 project with Bits automation loads");
  if (!ok) return;
  double worst = 0;
  const AutomationClip& bc = back.automations[0];
  for (size_t i = 0; i < bc.points.size(); ++i) {
    const double want = (c.min + c.points[i].value * (c.max - c.min)) * 23 + 1;
    worst = std::max(worst, std::fabs(d.fromNormalized(bc.min + bc.points[i].value * (bc.max - bc.min)) - want));
  }
  for (size_t i = 0; i < lane.points.size(); ++i)
    worst = std::max(worst, std::fabs(d.fromNormalized(back.patterns[0].automation[0].points[i].value) - (lane.points[i].value * 23 + 1)));
  worst = std::max({worst, std::fabs(d.fromNormalized(back.links[0].min) - 4), std::fabs(d.fromNormalized(back.links[0].max) - 16)});
  char b[96];
  std::snprintf(b, sizeof b, "v6 Bits automation / pattern lane / link range keep their bit depths (worst %.4f bits)", worst);
  expect(worst < 1e-3, b);
  bool same = back.automations[1].min == other.min && back.automations[1].max == other.max && back.automations[1].points.size() == other.points.size();
  for (size_t i = 0; same && i < other.points.size(); ++i)
    same = std::fabs(back.automations[1].points[i].value - std::clamp(other.points[i].value, 0.0f, 1.0f)) < 1e-6f;
  expect(same, "automation of other parameters is untouched");
  Json j7 = songToJson(back);
  Song again;
  expect(songFromJson(j7, again) && again.automations[0] == back.automations[0] && again.links[0].min == back.links[0].min,
         "current projects load without remapping");
}

// Factory presets show the rate control off and all render finite, distinct sounds.
void testPresets() {
  const PluginInfo* info = findPlugin("gl.bitcrush");
  if (!info) return;
  const Signal x = multiSine(22050);
  std::vector<double> errs;
  bool finite = true;
  for (size_t p = 0; p < info->presets.size(); ++p) {
    const Signal y = render(presetState(*info, p), x);
    for (float v : y) finite = finite && std::isfinite(v);
    errs.push_back(errorDb(x, y));
    std::printf("# preset %-16s rate %-9s error %.1f dB\n", info->presets[p].name.c_str(),
                info->params[pp::BcRate].format(presetState(*info, p).params[pp::BcRate]).c_str(), errs.back());
  }
  expect(info->presets.size() >= 6 && finite, "factory presets render finite audio");
  std::sort(errs.begin(), errs.end());
  bool distinct = true;
  for (size_t i = 1; i < errs.size(); ++i) distinct = distinct && errs[i] - errs[i - 1] > 0.05;
  expect(distinct, "factory presets sound different from each other");
}

// Other bit crushers reach their top.
void testOthers() {
  const PluginInfo* dly = findPlugin("gl.delay");
  expect(dly && dly->params[pp::DlyBits].fromNormalized(1.0) == 16.0, "Tracer bit depth: knob reaches 16");
  const PluginInfo* hex = findPlugin("gl.hexsynth");
  bool found = false;
  if (hex)
    for (const auto& p : hex->params)
      if (p.name == "FX 1 lo-fi bits") found = p.fromNormalized(1.0) == p.max;
  expect(found, "Hex Synth lo-fi bits: knob reaches its top");
}

}  // namespace

int main() {
  gl::test::hermeticTestEnv("bitcrush_range");
  testBits();
  testRateDescriptor();
  testRateReadout();
  testSweep();
  testFilters();
  testTravelTable();
  testLegacy();
  testProjectMigration();
  testRoundTrip();
  testLawMigration();
  testPresets();
  testOthers();
  return finish();
}
