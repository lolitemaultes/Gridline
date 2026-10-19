// Gridline — mixer strip 3-band EQ (low shelf, peak, high shelf) run after a
// track's effect slots and before its fader. Realtime-safe: coefficients are
// redesigned only when a band setting or the sample rate changes.
#pragma once
#include <array>
#include "Dsp.h"

namespace gl {

struct StripEqSettings {
  std::array<float, 3> gain{}, freq{}, q{};
  bool operator==(const StripEqSettings& o) const { return gain == o.gain && freq == o.freq && q == o.q; }
  bool operator!=(const StripEqSettings& o) const { return !(*this == o); }
  bool active() const { return gain[0] != 0.0f || gain[1] != 0.0f || gain[2] != 0.0f; }
};

inline dsp::Biquad::Type stripEqType(int band) {
  return band == 0 ? dsp::Biquad::LowShelf : band == 1 ? dsp::Biquad::Peak : dsp::Biquad::HighShelf;
}

// Designs one band's biquad.
inline void designStripEqBand(dsp::Biquad& bq, const StripEqSettings& s, int band, double sr) {
  const size_t b = static_cast<size_t>(band);
  bq.set(stripEqType(band), sr, s.freq[b], s.q[b], s.gain[b]);
}

// Magnitude of the whole EQ at `hz` (UI curve, tests).
inline double stripEqMagnitude(const StripEqSettings& s, double hz, double sr) {
  double m = 1.0;
  for (int b = 0; b < 3; ++b) {
    if (s.gain[static_cast<size_t>(b)] == 0.0f) continue;
    dsp::Biquad bq;
    designStripEqBand(bq, s, b, sr);
    m *= bq.magnitude(hz, sr);
  }
  return m;
}

struct StripEqState {
  dsp::Biquad bq[3][2];  // band × channel
  StripEqSettings key;
  double sr = 0;
  bool wasActive = false;

  void process(const StripEqSettings& s, double sampleRate, float* L, float* R, int n) {
    if (!s.active()) {
      wasActive = false;
      key.gain = {0.0f, 0.0f, 0.0f};
      return;
    }
    if (s != key || sampleRate != sr) {
      for (int b = 0; b < 3; ++b) {
        // keep the filter memory: only the coefficients change
        dsp::Biquad d;
        designStripEqBand(d, s, b, sampleRate);
        for (auto& ch : bq[b]) {
          if (key.gain[static_cast<size_t>(b)] == 0.0f) ch.clear();  // band was bypassed: stale memory
          ch.b0 = d.b0, ch.b1 = d.b1, ch.b2 = d.b2, ch.a1 = d.a1, ch.a2 = d.a2;
        }
      }
      key = s;
      sr = sampleRate;
    }
    wasActive = true;
    for (int b = 0; b < 3; ++b) {
      if (s.gain[static_cast<size_t>(b)] == 0.0f) continue;
      dsp::Biquad& l = bq[b][0];
      dsp::Biquad& r = bq[b][1];
      for (int i = 0; i < n; ++i) {
        L[i] = static_cast<float>(l.process(L[i]));
        R[i] = static_cast<float>(r.process(R[i]));
      }
    }
  }
};

}  // namespace gl
