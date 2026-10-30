// Gridline — "plasma" pack tests: matched filter voicing (J-203b). With the drive mode on Boost
// (what every Serum import sets) Plasma's filter types follow laws measured on the hosted source
// synth. Each expected row below is the source's transfer, in dB, of white noise routed through the
// filter, averaged over 1/6-octave bands (centre 25 Hz * 2^(b/6), band +-1/12 octave), 4 s render at
// key 60, drive 0, filter-on power over filter-off power. Values are plain numbers read off those
// measurements; nothing from the source is used or shipped. Here the same band power is taken from
// steady-state sines spread across each band (a linear filter's white-noise band power).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include "../../src/packs/plasma/plasma.h"
#include "../../src/packs/plasma/plasma_dsp.h"
#include "../Check.h"

namespace pl = gl::packs::plasma;

namespace {
constexpr double kSr = 96000;  // Plasma runs its voices (and their filters) 2x oversampled
constexpr double kPi = 3.14159265358979323846;
const int kBands[8] = {14, 22, 24, 26, 30, 36, 42, 48};

double center(int b) { return 25.0 * std::pow(2.0, b / 6.0); }

// Power gain (linear) of one sine through a fresh filter.
double sineGain(int type, double hz, double res, double var, double f, bool boost) {
  pl::Filter flt;
  const int settle = 19200, meas = 28800;
  double pin = 0, pout = 0;
  for (int i = 0; i < settle + meas; ++i) {
    if (i % 32 == 0) flt.set(type, kSr, hz, res, 0.0, var, 0.0, boost);
    const float x = static_cast<float>(0.05 * std::sin(2 * kPi * f * i / kSr));
    float l = x, r = x;
    flt.process(l, r);
    if (i >= settle) pin += double(x) * x, pout += double(l) * l;
  }
  return pout / pin;
}

double bandDb(int type, double hz, double res, double var, int b, bool boost) {
  const double lo = center(b) * std::pow(2.0, -1.0 / 12), hi = center(b) * std::pow(2.0, 1.0 / 12);
  double s = 0;
  const int n = type >= 39 && type <= 70 ? 32 : 8;  // combs: several teeth per band
  for (int i = 0; i < n; ++i) s += sineGain(type, hz, res, var, lo + (hi - lo) * (i + 0.5) / n, boost);
  return 10 * std::log10(s / n + 1e-30);
}

struct Row {
  const char* name;
  int type;  // Plasma filter index
  double f, res, var;
  double db[8];
};
// Source measurements (dB) at bands 14, 22, 24, 26, 30, 36, 42, 48 (126 Hz .. 6.4 kHz); cutoff value
// f is Plasma's normalized cutoff (0.5 = 426 Hz, 0.7 = 2.08 kHz).
const Row kRows[] = {
    {"L12 r60", 5, 0.5, 0.6, 0.0, {-10.5, -3.8, 1.3, -3.9, -19.0, -33.0, -45.7, -57.9}},
    {"L12 r60 var100", 5, 0.5, 0.6, 1.0, {0.5, 6.5, 11.1, 6.4, -7.9, -22.4, -34.9, -47.1}},
    {"L18 r30", 6, 0.5, 0.3, 0.0, {-7.6, -4.1, -3.2, -7.7, -21.9, -41.0, -59.4, -77.5}},
    {"H24 r60", 11, 0.5, 0.6, 0.0, {-51.5, -13.1, -0.8, -0.6, -6.4, -9.7, -10.4, -10.7}},
    {"B12 r30", 12, 0.5, 0.3, 0.0, {-17.3, -4.8, -0.7, -3.4, -9.6, -17.8, -24.3, -30.5}},
    {"P12 r60", 14, 0.5, 0.6, 0.0, {-10.5, -5.0, 0.7, -2.7, -8.9, -10.1, -10.5, -10.8}},
    {"N12 r60", 16, 0.5, 0.6, 0.0, {-0.9, -0.7, -6.4, -2.1, -0.1, -0.1, 0.1, -0.1}},
    {"LH12 r60 var50", 19, 0.5, 0.6, 0.5, {-14.4, 17.7, 20.7, 11.1, -5.9, -21.7, -34.8, -47.2}},
    {"LN12 r60 var50", 22, 0.5, 0.6, 0.5, {-2.4, -19.9, -2.4, -1.9, -11.1, -23.0, -35.0, -47.1}},
    {"LBH12 r60 var50", 31, 0.5, 0.6, 0.5, {-9.8, 4.0, 11.3, 7.5, -2.1, -10.9, -17.6, -23.6}},
    {"LNH24 r60", 36, 0.5, 0.6, 0.0, {1.1, 7.4, 11.8, 4.0, -18.2, -44.3, -69.8, -94.2}},
    {"MgL12 r60 var100", 1, 0.7, 0.6, 1.0, {-1.1, -0.2, 0.3, 1.4, 2.8, 12.9, 1.0, -10.2}},
    {"MgL24 r60 var50", 3, 0.5, 0.6, 0.5, {-3.9, 4.1, 5.3, -3.8, -18.9, -40.3, -63.4, -87.5}},
    // ladders (VAR has no effect on them in the source)
    {"LadderMg r60", 99, 0.5, 0.6, 0.0, {-1.0, 6.3, 7.2, -2.4, -17.0, -38.5, -61.4, -85.2}},
    {"LadderAcid r30", 100, 0.5, 0.3, 0.0, {1.2, 0.3, -5.0, -10.1, -20.6, -39.5, -61.5, -84.7}},
    {"LadderAcid r0 f0.7", 100, 0.7, 0.0, 0.0, {-2.5, -5.4, -6.9, -9.2, -12.0, -21.3, -33.1, -49.7}},
    {"LadderEMS r60", 101, 0.5, 0.6, 0.0, {1.6, 10.9, 7.2, -1.2, -13.5, -32.2, -53.5, -76.4}},
    {"ZDF_A r60", 84, 0.5, 0.6, 0.0, {-10.4, -3.6, -0.2, -9.8, -24.7, -46.7, -69.8, -93.9}},
    {"DirtyMg r60", 102, 0.5, 0.6, 0.0, {-7.7, -5.5, -11.5, -18.9, -31.6, -53.8, -77.3, -101.6}},
    // other state-variable designs, pass / ring types, combs, flangers and phasers
    {"Exp r60 var50", 105, 0.5, 0.6, 0.5, {-7.1, -9.3, -13.8, -29.6, -9.5, -7.2, -7.0, -6.9}},
    {"ExpBPF r30", 106, 0.5, 0.3, 0.0, {-12.7, -6.1, -5.0, -4.6, -6.6, -11.4, -17.2, -23.1}},
    {"Wsp r60", 96, 0.5, 0.6, 0.0, {0.2, 5.4, 9.5, 6.2, -8.3, -22.7, -35.3, -47.6}},
    {"Wsp r30 var100", 96, 0.5, 0.3, 1.0, {-23.8, -3.5, 1.6, 3.4, -0.5, -2.6, -3.1, -3.1}},
    {"BandReject r0 var50", 89, 0.5, 0.0, 0.5, {-15.2, -26.5, -26.8, -27.4, -21.4, -12.1, -4.7, -1.0}},
    {"BandReject r60 var25", 89, 0.5, 0.6, 0.25, {7.2, -6.7, -9.3, -8.5, 1.3, 5.1, 1.1, 0.2}},
    {"PZ_SVF r60 var50", 103, 0.5, 0.6, 0.5, {-0.1, -0.2, -0.3, -0.3, 0.5, -0.2, -0.2, -0.0}},
    {"RM r60 var50", 76, 0.5, 0.6, 0.5, {-3.0, -2.4, -2.3, -3.1, -3.0, -2.9, -2.8, -2.6}},
    {"CombP r60", 39, 0.5, 0.6, 0.0, {-13.8, -11.2, -1.7, -7.3, -3.3, -6.0, -8.6, -7.2}},
    {"FlangeN r60", 48, 0.5, 0.6, 0.0, {-7.0, -7.9, -21.6, -12.1, -14.4, -5.0, -2.4, -2.5}},
    {"Phase24P r60", 57, 0.5, 0.6, 0.0, {-12.4, -8.0, -15.4, -31.3, -12.6, -3.7, 1.8, 3.6}},
    {"Phase12N r30", 56, 0.5, 0.3, 0.0, {-3.1, -0.8, -3.4, -5.5, -9.5, -15.5, -20.2, -22.3}},
};
}  // namespace

int main() {
  // Matched voicing (Boost): each band within 1.5 dB of the source (2 dB for the ladders,
  // whose saturating input bends the source's noise measurement a little); deep stopband bands
  // (source below -30 dB) only need to stay below -25 dB, and a band holding a narrow notch
  // (source 8 dB or more below both neighbours) within 4 dB: its depth in a 1/6-octave band swings
  // with tiny shifts of the notch frequency (the source's own renders vary by a few dB there).
  for (const Row& r : kRows) {
    const double hz = pl::cutoffHz(r.f);
    const double tol = r.type <= 3 || r.type >= 84 || (r.type >= 39 && r.type <= 70) ? 2.0 : 1.5;
    double worst = 0, gotAll[8];
    bool ok = true;
    for (int i = 0; i < 8; ++i) {
      const double got = gotAll[i] = bandDb(r.type, hz, r.res, r.var, kBands[i], true);
      if (!std::isfinite(got)) ok = false;
      if (r.db[i] < -30) {
        if (got > -25) ok = false;
        continue;
      }
      const bool notch = i > 0 && i < 7 && r.db[i] < r.db[i - 1] - 8 && r.db[i] < r.db[i + 1] - 8;
      if (notch) {
        if (std::fabs(got - r.db[i]) > 4) ok = false;
        continue;
      }
      worst = std::max(worst, std::fabs(got - r.db[i]));
    }
    std::printf("# %s worst %.2f dB:", r.name, worst);
    for (double g : gotAll) std::printf(" %.1f", g);
    std::printf("\n");
    expect(ok && worst <= tol, (std::string("matched ") + r.name + " follows the source within tolerance").c_str());
  }

  // Classic voicing (Leveled, the default) is unchanged by the matched laws: L12 keeps its passband
  // level at high resonance (the matched law loses about 11 dB there).
  {
    const double hz = pl::cutoffHz(0.5);
    const double classic = bandDb(5, hz, 0.6, 0.0, 14, false);
    const double matched = bandDb(5, hz, 0.6, 0.0, 14, true);
    expect(classic > matched + 6, "classic L12 keeps its passband at resonance 60 %");
  }

  // Stereo spread (matched): the source's L - R level at 6.4 kHz of L12 r30, cutoff 0.5, is +65.9 dB at
  // stereo 0 % and +8.1 dB at 25 % (cubic in the offset); the voice passes (stereo - 0.5) * 48 semitones.
  {
    auto lrDb = [](double stereo) {
      pl::Filter flt;
      double pl2 = 0, pr2 = 0;
      for (int i = 0; i < 48000; ++i) {
        if (i % 32 == 0) flt.set(5, kSr, pl::cutoffHz(0.5), 0.3, 0.0, 0.0, (stereo - 0.5) * 48.0, true);
        const float x = static_cast<float>(0.05 * std::sin(2 * kPi * 6400.0 * i / kSr));
        float l = x, r = x;
        flt.process(l, r);
        if (i >= 19200) pl2 += double(l) * l, pr2 += double(r) * r;
      }
      return 10 * std::log10((pl2 + 1e-30) / (pr2 + 1e-30));
    };
    const double s0 = lrDb(0.0), s25 = lrDb(0.25);
    std::printf("# stereo L-R at 6.4 kHz: s0 %.1f dB, s25 %.1f dB\n", s0, s25);
    expect(std::fabs(s0 - 65.9) < 3.0 && std::fabs(s25 - 8.1) < 1.5, "matched stereo spread follows the source");
  }

  // Every matched type stays finite at the extremes (full resonance, VAR ends, cutoff ends).
  {
    bool ok = true;
    for (int t : {0, 1, 2, 3, 4, 5, 11, 15, 17, 19, 22, 30, 31, 36, 38, 84, 99, 100, 101, 102})
      for (double f : {0.0, 1.0})
        for (double v : {0.0, 1.0}) {
          pl::Filter flt;
          for (int i = 0; i < 4800; ++i) {
            if (i % 32 == 0) flt.set(t, kSr, pl::cutoffHz(f), 1.0, 1.0, v, 12.0, true);
            float l = (i * 7919 % 101) / 50.f - 1.f, r = -l;
            flt.process(l, r);
            if (!std::isfinite(l) || !std::isfinite(r) || std::fabs(l) > 1e4f) ok = false;
          }
        }
    expect(ok, "matched types stay finite at parameter extremes");
  }
  return finish();
}
