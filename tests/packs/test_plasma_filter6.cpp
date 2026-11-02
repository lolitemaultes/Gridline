// Gridline — "plasma" pack tests: the imported voicing's drive stage ahead of the filter's dry / wet split.
// Expected values are plain numbers read off the reference synth's output (held saw at key 48 through a
// voice filter at mix 0, and through the filter effect at mix 100); Plasma's own designs, nothing from the
// source is used or shipped.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include "../../src/packs/plasma/plasma.h"
#include "../../src/packs/plasma/plasma_dsp.h"
#include "../Check.h"

namespace pl = gl::packs::plasma;

namespace {
constexpr double kSr = 48000;

struct Lv {
  double in, dry, wet;  // rms dB of the input, the dry path and the filter output
};

// A held saw (130.8 Hz, peak amp) through a fresh filter; levels over the second half second.
Lv run(int type, double drive, double amp, bool boost = true, bool pad = false, double cutoffNorm = 0.5, double res = 0.0) {
  pl::Filter flt;
  const int settle = 24000, meas = 24000;
  double si = 0, sd = 0, sw = 0, ph = 0;
  const double hz = pl::cutoffHz(cutoffNorm);
  for (int i = 0; i < settle + meas; ++i) {
    if (i % 32 == 0) flt.set(type, kSr, hz, res, drive, 0.0, 0.0, boost, pad);
    ph += 130.81 / kSr;
    ph -= std::floor(ph);
    const float x = static_cast<float>(amp * (2 * ph - 1));
    float l = x, r = x;
    flt.process(l, r);
    if (i >= settle) si += double(x) * x, sd += double(flt.dry(0)) * flt.dry(0), sw += double(l) * l;
  }
  auto db = [&](double s) { return 10 * std::log10(s / meas + 1e-30); };
  return {db(si), db(sd), db(sw)};
}
}  // namespace

int main() {
  constexpr int kL24 = 7, kH12 = 9, kCombP = 39, kRM = 76, kAcid = 100, kDirty = 102, kFormant = 86;
  // small signal with PAD (no clip): the drive's gain is (1 + drive)^4 (source: +7.7 / +14.1 / +24.1 dB at
  // drive 25 / 50 / 100 %)
  const double a = 0.01;
  const Lv p25 = run(kL24, 0.25, a, true, true), p50 = run(kL24, 0.5, a, true, true), p100 = run(kL24, 1.0, a, true, true);
  std::printf("# PAD dry gain: %.2f / %.2f / %.2f dB\n", p25.dry - p25.in, p50.dry - p50.in, p100.dry - p100.in);
  expect(std::fabs(p25.dry - p25.in - 7.7) < 0.2, "imported dry path: drive 25 % gain");
  expect(std::fabs(p50.dry - p50.in - 14.1) < 0.2, "imported dry path: drive 50 % gain");
  expect(std::fabs(p100.dry - p100.in - 24.1) < 0.2, "imported dry path: drive 100 % gain");
  // without PAD the stage soft-clips at the source's full scale: on a saw at the level the measured notes reach
  // the filter with, the clip takes 0.6 / 2.1 / 3.9 / 5.4 dB off the gain at drive 50 / 75 / 90 / 100 %
  const double amp = 0.56;
  const double want[4] = {0.6, 2.1, 3.9, 5.4}, drv[4] = {0.5, 0.75, 0.9, 1.0};
  for (int k = 0; k < 4; ++k) {
    const Lv v = run(kH12, drv[k], amp);
    const double comp = 20 * std::log10(std::pow(1 + drv[k], 4.0)) - (v.dry - v.in);
    std::printf("# drive %.0f %%: clip takes %.2f dB (source %.1f)\n", 100 * drv[k], comp, want[k]);
    expect(std::fabs(comp - want[k]) < 0.4, ("imported dry path: soft clip at drive " + std::to_string(static_cast<int>(100 * drv[k]))).c_str());
  }
  // combs, flangers and phasers: the dry path falls with the resonance (source: -0.8 / -2.2 / -4.1 / -6.0 dB at
  // resonance 0 / 30 / 60 / 100 %)
  const double cr[4] = {0.0, 0.3, 0.6, 1.0}, cw[4] = {-0.8, -2.2, -4.1, -6.0};
  for (int k = 0; k < 4; ++k) {
    const Lv c0 = run(kCombP, 0.0, a, true, false, 0.5, cr[k]);
    expect(std::fabs(c0.dry - c0.in - cw[k]) < 0.15, ("imported comb dry path at resonance " + std::to_string(static_cast<int>(100 * cr[k]))).c_str());
  }
  // the ladders and formants keep the raw input on the dry path; so does every type in the Classic voicing
  for (int t : {kAcid, kDirty, kFormant}) {
    const Lv v = run(t, 0.8, amp);
    expect(std::fabs(v.dry - v.in) < 1e-3, ("raw dry path for type " + std::to_string(t)).c_str());
  }
  const Lv cl = run(kL24, 0.8, amp, false);
  expect(std::fabs(cl.dry - cl.in) < 1e-3, "Classic voicing: raw dry path");
  // the filter runs on the same driven signal: a wide-open comb's output follows the dry path's drive
  // (source, comb at resonance 50 %: -28.8 / -15.5 / -11.1 dB at drive 0 / 50 / 100 %, i.e. +13.3 / +17.7 dB)
  const Lv w0 = run(kCombP, 0.0, amp, true, false, 0.5, 0.5), w50 = run(kCombP, 0.5, amp, true, false, 0.5, 0.5),
           w100 = run(kCombP, 1.0, amp, true, false, 0.5, 0.5);
  std::printf("# comb wet over drive 0: %.2f / %.2f dB\n", w50.wet - w0.wet, w100.wet - w0.wet);
  expect(std::fabs(w50.wet - w0.wet - 13.3) < 0.5, "imported comb under drive 50 %");
  expect(std::fabs(w100.wet - w0.wet - 17.7) < 0.5, "imported comb under drive 100 %");
  // ring mod under drive: the stage clips at the same full scale (source, drive 60 %: +6.6 dB over a unit clip)
  const Lv r0 = run(kRM, 0.0, a), r60 = run(kRM, 0.6, a);
  expect(std::fabs(r60.wet - r0.wet - 20 * std::log10(std::pow(1.6, 4.0))) < 0.3, "imported ring mod keeps the drive gain");
  // the acid ladder and Wsp come out inverted (source, held saw at mix 50 %: -29.4 / -32.8 dB, against -20.8 /
  // -19.1 dB fully wet); the other ladders stay in phase (mix 50 %: within 0.2 dB of an in-phase mix)
  // DistComb 2 runs its clipped loop at the source's full scale (source, held saw, cutoff 0.7, resonance 30 %:
  // +3.0 dB over the input fully wet; Plasma now +1.5 dB, -8.5 dB before)
  const Lv d2 = run(92, 0.0, amp, true, false, 0.7, 0.3);
  std::printf("# DistComb 2 wet over input: %.2f dB\n", d2.wet - d2.in);
  expect(d2.wet - d2.in > -0.5 && d2.wet - d2.in < 4.0, "imported DistComb 2 level");
  for (int t : {kAcid, 96, 90, 101}) {
    pl::Filter flt;
    double sxy = 0;
    for (int i = 0; i < 48000; ++i) {
      if (i % 32 == 0) flt.set(t, kSr, pl::cutoffHz(t == 96 || t == 90 ? 0.7 : 0.8), t == 90 ? 0.3 : 0.0, 0.0, 0.0, 0.0, true);
      const float x = static_cast<float>(0.3 * std::sin(6.283185307179586 * 130.81 * i / kSr));
      float l = x, r = x;
      flt.process(l, r);
      if (i >= 24000) sxy += double(x) * l;
    }
    expect(t == 101 ? sxy > 0 : sxy < 0, ("imported output polarity of type " + std::to_string(t)).c_str());
  }
  // extremes stay finite
  bool ok = true;
  for (int t : {kL24, kH12, kCombP, kRM, 78, 79, 89, 98})
    for (double d : {0.0, 1.0}) {
      const Lv v = run(t, d, 4.0, true, d > 0.5);
      ok = ok && std::isfinite(v.dry) && std::isfinite(v.wet);
    }
  expect(ok, "imported drive stage stays finite at extremes");
  return finish();
}
