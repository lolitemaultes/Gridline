// Gridline — "plasma" pack tests: the matched dirty ladder under drive. Expected values are plain numbers
// read off the hosted source synth's output for a held saw at key 48 through its filter effect; Plasma's
// own designs, nothing from the source is used or shipped.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include "../../src/packs/plasma/plasma.h"
#include "../../src/packs/plasma/plasma_dsp.h"
#include "../Check.h"

namespace pl = gl::packs::plasma;

namespace {
constexpr double kSr = 96000;
constexpr int kDirty = 102;

struct Out {
  double rmsDb, mean, rms;
};

// A held saw (130.8 Hz, peak amp) through a fresh matched dirty ladder at resonance 0.
Out run(double cutoffNorm, double drive, double amp) {
  pl::Filter flt;
  const int settle = 38400, meas = 38400;
  double s = 0, s2 = 0;
  double ph = 0;
  const double hz = pl::cutoffHz(cutoffNorm);
  for (int i = 0; i < settle + meas; ++i) {
    if (i % 32 == 0) flt.set(kDirty, kSr, hz, 0.0, drive, 0.0, 0.0, true);
    ph += 130.81 / kSr;
    ph -= std::floor(ph);
    const float x = static_cast<float>(amp * (2 * ph - 1));
    float l = x, r = x;
    flt.process(l, r);
    if (i >= settle) s += l, s2 += double(l) * l;
  }
  const double rms = std::sqrt(s2 / meas);
  return {20 * std::log10(rms + 1e-30), s / meas, rms};
}
}  // namespace

int main() {
  // a saw at about the level the measured notes reach the filter with
  const double amp = 0.95;
  const Out o5 = run(0.5, 1.0, amp), o8 = run(0.8, 1.0, amp), o10 = run(1.0, 1.0, amp);
  std::printf("# drive 100: cutoff 0.5 %.1f dB, 0.8 %.1f dB, 1.0 %.1f dB\n", o5.rmsDb, o8.rmsDb, o10.rmsDb);
  // Source, drive 100 %: cutoff 0.8 is 8.1 dB and cutoff 1 11.2 dB louder than cutoff 0.5 (the clip ceiling
  // rises with the cutoff)
  expect(std::fabs(o8.rmsDb - o5.rmsDb - 8.1) < 1.5, "driven dirty ladder: cutoff 0.8 over 0.5 follows the source");
  expect(std::fabs(o10.rmsDb - o5.rmsDb - 11.2) < 1.5, "driven dirty ladder: cutoff 1 over 0.5 follows the source");
  // Source, cutoff 0.2: driven, the output is mostly a negative offset (mean -0.008 of rms 0.009); at drive 0
  // there is none
  const Out d1 = run(0.2, 1.0, amp), d0 = run(0.2, 0.0, amp);
  std::printf("# cutoff 0.2: drive 100 mean %.5f rms %.5f, drive 0 mean %.6f\n", d1.mean, d1.rms, d0.mean);
  expect(d1.mean < 0 && -d1.mean > 0.7 * d1.rms, "driven dirty ladder at a low cutoff leaves a negative offset");
  expect(std::fabs(d0.mean) < 0.05 * std::fabs(d1.mean), "no offset at drive 0");
  // extremes stay finite
  bool ok = true;
  for (double f : {0.0, 0.5, 1.0})
    for (double d : {0.0, 1.0}) {
      const Out o = run(f, d, 4.0);
      ok = ok && std::isfinite(o.rms) && std::isfinite(o.mean);
    }
  expect(ok, "dirty ladder stays finite at the extremes");
  return finish();
}
