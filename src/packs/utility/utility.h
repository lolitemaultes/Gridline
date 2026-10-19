// Gridline — "utility" pack: parameter indices and design helpers shared by
// the catalogue, the DSP, the editors and the tests.
//
//   gl.drawneq      Curve EQ        drawable 23-point EQ curve, 4 morphable snapshots,
//                                   minimum-phase (filter bank) or linear-phase (FFT)
//   gl.bankeq       Morph EQ        31 parallel third-octave bands, level / pan / send curves,
//                                   8 morphable banks + master, send output to another insert
//   gl.freefilter   Free Filter     one RBJ biquad with 7 types, DC blocker
//   gl.fastlp       Quick Lowpass   cheap resonant 2-pole lowpass
//   gl.bassboost    Bass Boost      resonant low band added to the signal
//   gl.scope        Scope           oscilloscope / spectrum / spectrogram / vectorscope / loudness
//   gl.spectro      Spectrogram     spectrum bars or lines + scrolling sonogram
//   gl.meter        Level Meter     peak / RMS / true-peak meter with hold and clip LEDs
//   gl.clock        Clock           big transport readout
//   gl.notes        Notes           100-page project notebook (text in PluginState::data)
//   gl.mute         Mute            click-free mute per side
//   gl.send         Send Level      send volume / pan / dry level to a target insert
//   gl.stereomatrix Stereo Matrix   L/R mixing matrix, inter-channel delay and dephasing
#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include "../../core/PluginInfo.h"

namespace gl::packs::utility {

// ------------------------------------------------------------- parameters

// gl.drawneq — 4 snapshots x 23 log-spaced points (dB), then the controls.
inline constexpr int kCeBands = 23;  // ~half-octave points: 4 x 23 + 7 controls fit the 99 slot parameters
inline constexpr int kCeSnaps = 4;
inline constexpr int cePoint(int snap, int band) { return snap * kCeBands + band; }
enum CurveEq {
  CeMorphX = kCeSnaps * kCeBands,
  CeMorphY,
  CeDepth,   // -1..1 curve scale (negative inverts)
  CeShift,   // octaves
  CeSmooth,  // seconds (morph / depth / shift glide)
  CeOutput,  // dB
  CePhase,   // 0 minimum phase, 1 linear phase
  CeCount
};
// Mixer-slot parameters are addressed with a stride of 100 (index 99 = slot mix).
static_assert(CeCount <= 99, "Curve EQ must fit the mixer slot parameter range");
inline constexpr double kCeRange = 18.0;          // point range (dB)
inline constexpr int kCeLinearKernel = 2048;      // FIR length (linear phase)
inline constexpr int kCeLinearHop = 1024;         // block size of the FFT convolution
inline constexpr int kCeLinearLatency = kCeLinearHop + kCeLinearKernel / 2;
inline double ceBandHz(int band) { return 20.0 * std::pow(1000.0, static_cast<double>(band) / (kCeBands - 1)); }

// gl.freefilter
enum FreeFilter { FfType, FfFreq, FfQ, FfGain, FfDc, FfCount };
enum FreeFilterType { FtLowPass, FtBandPass, FtHighPass, FtNotch, FtLowShelf, FtPeak, FtHighShelf, FtCount };
inline bool ffHasGain(int type) { return type == FtLowShelf || type == FtPeak || type == FtHighShelf; }

// gl.fastlp
enum QuickLp { QlCutoff, QlReso, QlCount };

// gl.bassboost
enum BassBoost { BbFreq, BbAmount, BbCount };

// gl.scope
enum Scope {
  ScMode,       // Oscilloscope / Spectrum / Spectrogram / Vectorscope / Meters / Grid
  ScGain,       // visual gain (dB)
  ScWindow,     // oscilloscope window (s)
  ScRange,      // spectrum dB range
  ScTilt,       // spectrum slope (dB/oct)
  ScRes,        // FFT size choice
  ScMeterMode,  // Peak / RMS / VU / Loudness
  ScHold,       // peak hold (s)
  ScDecay,      // meter fall (dB/s)
  ScVecMode,    // Lissajous / Polar dots
  ScFreeze,     // hold the display
  // J-1 reference display engine (used when ScEngine = 1).
  ScAutoView,      // pick the view from the shape of the display
  ScWaveRefresh,   // oscilloscope refresh (ms)
  ScSplit,         // left / right in separate lanes
  ScSplitGap,      // gap between the lanes (% of the height)
  ScTaper,         // taper the waveform towards both window edges
  ScSmoothZoom,    // 2x interpolation of zoomed-in waveforms
  ScSpecRefresh,   // spectrum refresh (ms)
  ScSpecBands,     // 32 .. 8192 bands (FFT = 2 x bands)
  ScFreqCurve,     // frequency axis: 0 linear .. 1 logarithmic
  ScPink,          // +3 dB/oct (pink noise reads flat)
  ScSharpen,       // time-frequency reassignment of the peaks
  ScMeterType,     // Peak / PPM / RMS / VU / A-weighted Leq / BS.1770 loudness
  ScMeterCurve,    // scale curve 0..1
  ScHoldInf,       // infinite peak hold
  ScFallTime,      // fall time of the full scale (ms)
  ScVecStyle,      // Thin / Ribbon / Tube / Glow
  ScVecWidth,      // line width (px)
  ScVecAdd,        // additive drawing
  ScVecRefresh,    // vectorscope refresh (ms)
  ScFade,          // fade the display after silence
  ScFadeDelay,     // ms of silence before fading
  ScEngine,        // 0 legacy display (pre-J-1 projects), 1 reference
  ScCount
};
enum ScopeView { SvOsc, SvSpectrum, SvSonogram, SvVector, SvMeters, SvGrid, SvCount };
inline constexpr int kScLegacyCount = ScFreeze + 1;  // parameter count before J-1
static_assert(kScLegacyCount == 11 && ScEngine == 32, "core/PluginInfo.cpp normalizePluginState migrates gl.scope by these indices");
enum ScopeMeterType { SmtPeak, SmtPpm, SmtRms, SmtVu, SmtLeqA, SmtLoudness, SmtCount };
enum ScopeVecStyle { SvsThin, SvsRibbon, SvsTube, SvsGlow };
// Spectrum bands for choice index i (32 .. 8192) and the FFT size (2 x bands, at least 1024).
inline int scopeBands(int idx) { return 32 << std::clamp(idx, 0, 8); }
inline int scopeFftSize(int idx) { return std::max(1024, 2 * scopeBands(idx)); }

// ---------------------------------------------------------- gl.bankeq (Morph EQ)
// 31 parallel constant-peak band-passes at 20·2^(i/3) Hz; three curves per bank
// (level, pan, send) in a 0..2 "v" domain; 8 morphable banks plus a master bank.
// Every band value and band mode is a parameter (automatable), in the reference
// order: value (bank k, curve t, band b) at kBqValueBase + k·93 + t·31 + b.
inline constexpr int kBqBands = 31, kBqBanks = 8, kBqTargets = 3;
inline constexpr int kBqMaster = kBqBanks;  // bank index of the master curve
enum BankEqTarget { BtLevel, BtPan, BtSend };
enum BankEqMode { BmOn, BmOff, BmLocked };
enum BankEq {
  BqShift,       // octaves (-10..10), curve moved along the bands
  BqMix,         // -1..1: 1 as drawn, 0 flat, -1 inverted
  BqMorph,       // 0..7 bank position
  BqSmooth,      // 0..1 motion smoothing (law: bqSmoothSec)
  BqOutput,      // dB (-60 = silence)
  BqBandwidth,   // octaves 0.267..1.667
  BqOversample,  // 2x around the filter bank
  BqAddMode,     // out = in + bands·(gain - 1)
  BqSendOnly,    // main output untouched, bands feed the send only
  BqSendTo,      // mixer index of the send curve's destination (-1 none)
  BqEditTarget,  // editor: curve being drawn (level / pan / send)
  BqEditBank,    // editor: bank being drawn (0..7)
  BqEditMaster,  // editor: draw the master curve
  BqDrawMode,    // editor: free / line / curve
  BqControlCount
};
inline constexpr int kBqValueBase = BqControlCount;
inline constexpr int kBqValues = (kBqBanks + 1) * kBqTargets * kBqBands;  // 837
inline constexpr int kBqModeBase = kBqValueBase + kBqValues;
inline constexpr int kBqCount = kBqModeBase + kBqTargets * kBqBands;  // 945
static_assert(kBqCount <= 999, "Morph EQ must fit the mixer slot parameter range");
inline constexpr int bqValue(int bank, int target, int band) { return kBqValueBase + bank * kBqTargets * kBqBands + target * kBqBands + band; }
inline constexpr int bqMode(int target, int band) { return kBqModeBase + target * kBqBands + band; }
inline double bqBandHz(int b) { return 20.0 * std::pow(2.0, b / 3.0); }
// Level / send law: v 0..2 → gain (v = 1 unity, 2 = x4, 0 = silence) and its inverse.
inline double bqGain(double v) { return (std::pow(3.0, v) - 1.0) * 0.5; }
inline double bqValueOfGain(double g) { return std::log(2.0 * std::max(0.0, g) + 1.0) / std::log(3.0); }
// Output normalisation of the band sum for a bandwidth in octaves.
inline double bqNorm(double bwOct) { return std::pow(10.0, -3.0 / 20.0) / (3.0 * std::max(bwOct, 1e-3)); }
// Smoothing knob (0..1) → time to reach 99.9 % (s).
inline double bqSmoothSec(double x) {
  const double tick = 44100.0 / 224.0;
  return (std::pow(11.0, std::clamp(x, 0.0, 1.0)) - 1.0) * 40000.0 / tick / tick;
}
// Output knob law of the reference (0..256 → gain) in dB; -60 = silence.
inline double bqOutputDbOfRaw(double v) {
  double g = (std::pow(11.0, v / 128.0) - 1.0) / 10.0;
  return g > 0.001 ? 20.0 * std::log10(g) : -60.0;
}
// Pan law: linear balance with a 0 dB centre (p -1..1) → left / right factor.
inline double bqPanL(double p) { return p >= 0 ? 1.0 - p : 1.0; }
inline double bqPanR(double p) { return p >= 0 ? 1.0 : 1.0 + p; }
// Bank value of curve t at a fractional band position (clamped, linear between bands).
inline double bqAt(const float* p, int bank, int target, double x) {
  x = std::clamp(x, 0.0, static_cast<double>(kBqBands - 1));
  int i = std::min(static_cast<int>(x), kBqBands - 2);
  double f = x - i;
  double a = p[bqValue(bank, target, i)], b = p[bqValue(bank, target, i + 1)];
  return a + (b - a) * f;
}
// Effective per-band values of a parameter vector (size kBqCount) for the
// given shift / mix / morph: v[t][b] in the v domain after shift → mix → morph,
// panOut[b] = pan after the master curve (-1..1), gain[t][b] = level / send gain
// after the master curve. Locked bands keep the edited bank's value (no motion,
// no master); off bands get gain 0 / pan 0 and report BmOff in modeOut.
struct BankFrame {
  float v[kBqTargets][kBqBands];
  float gain[kBqTargets][kBqBands];  // level / send gains (pan entry unused)
  float pan[kBqBands];
  uint8_t mode[kBqTargets][kBqBands];
};
void bankEffective(const float* p, double shiftOct, double mix, double morph, BankFrame& out);
// Steady-state response magnitude of the band sum at hz for per-band channel
// gains g[b] (norm included); add = true adds the dry path.
double bankResponse(double hz, double sr, double bwOct, const double* g, bool add);

// gl.spectro
enum Spectro { SgMode, SgAmp, SgScale, SgPeaks, SgWindow, SgStereo, SgSpeed, SgCount };
enum SpectroView { SpBars, SpLines, SpSonogram };

// gl.meter
enum LevelMeter { LmMode, LmRange, LmHold, LmDecay, LmTruePeak, LmCount };

// gl.clock
enum Clock { CkDisplay, CkSource, CkCount };
enum ClockDisplay { CdBoth, CdBars, CdTime, CdSamples };

// gl.notes
enum Notes { NtPage, NtCount };
inline constexpr int kNotePages = 100;

// gl.mute
enum Mute { MuMute, MuSide, MuFade, MuCount };

// gl.send

// gl.stereomatrix
enum StereoMatrix {
  SmLL, SmRL, SmLR, SmRR, SmDelay, SmDephase,
  SmFxPos,  // κ3: 0 before the matrix, 1 after
  // κ3 reference engine: slider positions use the gain law sign(x) (3^|x| - 1) / 2 (1 = unity, 2 = +12 dB),
  // delay / dephase knob positions -1..1 (delay (1001^|k| - 1) / 2 ms, all-pass corner (5001^(1-|k|) - 1) 4.398 + 10 Hz).
  SmDelayK, SmDephaseK, SmClassic,
  SmCount
};

// ------------------------------------------------------------------- FFT

// Complex radix-2 FFT with precomputed tables (allocate once; transform is realtime-safe).
class Fft {
 public:
  Fft() = default;
  explicit Fft(int n) { init(n); }
  void init(int n) {
    n_ = n;
    rev_.assign(static_cast<size_t>(n), 0);
    cs_.assign(static_cast<size_t>(n / 2), 0.0f);
    sn_.assign(static_cast<size_t>(n / 2), 0.0f);
    int bits = 0;
    while ((1 << bits) < n) ++bits;
    for (int i = 0; i < n; ++i) {
      int r = 0;
      for (int b = 0; b < bits; ++b)
        if (i & (1 << b)) r |= 1 << (bits - 1 - b);
      rev_[static_cast<size_t>(i)] = r;
    }
    for (int i = 0; i < n / 2; ++i) {
      double a = -2.0 * 3.14159265358979323846 * i / n;
      cs_[static_cast<size_t>(i)] = static_cast<float>(std::cos(a));
      sn_[static_cast<size_t>(i)] = static_cast<float>(std::sin(a));
    }
  }
  int size() const { return n_; }
  // In-place transform of re/im (inverse: conjugate twiddles, scaled by 1/n).
  void transform(float* re, float* im, bool inverse) const {
    const int n = n_;
    for (int i = 0; i < n; ++i) {
      int j = rev_[static_cast<size_t>(i)];
      if (i < j) {
        std::swap(re[i], re[j]);
        std::swap(im[i], im[j]);
      }
    }
    for (int len = 2; len <= n; len <<= 1) {
      int half = len / 2, step = n / len;
      for (int i = 0; i < n; i += len)
        for (int j = 0; j < half; ++j) {
          float wr = cs_[static_cast<size_t>(j * step)], wi = sn_[static_cast<size_t>(j * step)];
          if (inverse) wi = -wi;
          int a = i + j, b = a + half;
          float xr = re[b] * wr - im[b] * wi, xi = re[b] * wi + im[b] * wr;
          re[b] = re[a] - xr;
          im[b] = im[a] - xi;
          re[a] += xr;
          im[a] += xi;
        }
    }
    if (inverse) {
      float s = 1.0f / static_cast<float>(n);
      for (int i = 0; i < n; ++i) {
        re[i] *= s;
        im[i] *= s;
      }
    }
  }

 private:
  int n_ = 0;
  std::vector<int> rev_;
  std::vector<float> cs_, sn_;
};

// ------------------------------------------------------------ Curve EQ design

// Catmull-Rom interpolation of the dB points over log frequency.
inline double ceCurveAt(const float* pts, double hz) {
  double pos = std::log(std::max(hz, 1e-3) / 20.0) / std::log(1000.0) * (kCeBands - 1);
  if (pos <= 0) return pts[0];
  if (pos >= kCeBands - 1) return pts[kCeBands - 1];
  int i = static_cast<int>(pos);
  double t = pos - i;
  auto p = [&](int k) { return static_cast<double>(pts[std::clamp(k, 0, kCeBands - 1)]); };
  double p0 = p(i - 1), p1 = p(i), p2 = p(i + 1), p3 = p(i + 2);
  double v = 0.5 * ((2 * p1) + (-p0 + p2) * t + (2 * p0 - 5 * p1 + 4 * p2 - p3) * t * t + (-p0 + 3 * p1 - 3 * p2 + p3) * t * t * t);
  double lo = std::min(p1, p2), hi = std::max(p1, p2);  // no overshoot between two points
  double span = std::max(1.0, hi - lo) * 0.5;
  return std::clamp(v, lo - span, hi + span);
}

// Bilinear morph of the four snapshots: A top-left, B top-right, C bottom-left, D bottom-right.
inline void ceMorph(const PluginState& s, double x, double y, float* out) {
  x = std::clamp(x, 0.0, 1.0);
  y = std::clamp(y, 0.0, 1.0);
  double w[4] = {(1 - x) * (1 - y), x * (1 - y), (1 - x) * y, x * y};
  for (int b = 0; b < kCeBands; ++b) {
    double v = 0;
    for (int k = 0; k < kCeSnaps; ++k) {
      size_t i = static_cast<size_t>(cePoint(k, b));
      v += w[k] * (i < s.params.size() ? s.params[i] : 0.f);
    }
    out[b] = static_cast<float>(v);
  }
}

// Target response (dB, before output gain) for morphed points, depth and shift.
inline double ceTargetDb(const float* morphed, double depth, double shiftOct, double hz) {
  return depth * ceCurveAt(morphed, hz / std::pow(2.0, shiftOct));
}

// Minimum-phase mode: one peaking biquad per point. The interaction between
// neighbouring bands is removed by a few relaxation steps over a precomputed
// band-to-band matrix (dB response of each band at every centre per dB of gain).
inline constexpr double kCeBankQ = 1.6;
struct CeBank {
  double sr = 0;
  float m[kCeBands][kCeBands] = {};
  void prepare(double sampleRate);
  // Corrected per-band gains (dB) for per-band targets.
  void solve(const double* target, double* gains) const {
    for (int i = 0; i < kCeBands; ++i) gains[i] = target[i];
    for (int it = 0; it < 12; ++it) {
      for (int i = 0; i < kCeBands; ++i) {
        double r = 0;
        for (int j = 0; j < kCeBands; ++j) r += m[i][j] * gains[j];
        gains[i] = std::clamp(gains[i] + 0.8 * (target[i] - r), -36.0, 36.0);
      }
    }
  }
};

// Peaking biquad coefficients (RBJ) as plain numbers.
struct Coefs {
  double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
  double magnitude(double hz, double sr) const {
    double w = 2 * 3.14159265358979323846 * hz / sr;
    double c1 = std::cos(w), s1 = std::sin(w), c2 = std::cos(2 * w), s2 = std::sin(2 * w);
    double nr = b0 + b1 * c1 + b2 * c2, ni = -(b1 * s1 + b2 * s2);
    double dr = 1 + a1 * c1 + a2 * c2, di = -(a1 * s1 + a2 * s2);
    return std::sqrt((nr * nr + ni * ni) / std::max(1e-30, dr * dr + di * di));
  }
};
inline Coefs peakCoefs(double sr, double hz, double q, double gainDb) {
  hz = std::clamp(hz, 10.0, sr * 0.49);
  double A = std::pow(10.0, gainDb / 40.0), w = 2 * 3.14159265358979323846 * hz / sr, cw = std::cos(w);
  double alpha = std::sin(w) / (2 * q);
  double a0 = 1 + alpha / A;
  Coefs c;
  c.b0 = (1 + alpha * A) / a0;
  c.b1 = -2 * cw / a0;
  c.b2 = (1 - alpha * A) / a0;
  c.a1 = -2 * cw / a0;
  c.a2 = (1 - alpha / A) / a0;
  return c;
}

// RBJ band-pass with constant 0 dB peak gain; bandwidth in octaves (digital, bilinear-warped).
inline Coefs bqBandCoefs(double sr, double hz, double bwOct) {
  double w = 2 * 3.14159265358979323846 * hz / sr, sw = std::sin(w), cw = std::cos(w);
  double alpha = sw * std::sinh(std::log(2.0) / 2.0 * bwOct * w / std::max(sw, 1e-9));
  double a0 = 1 + alpha;
  Coefs c;
  c.b0 = alpha / a0;
  c.b1 = 0;
  c.b2 = -alpha / a0;
  c.a1 = -2 * cw / a0;
  c.a2 = (1 - alpha) / a0;
  return c;
}

// ------------------------------------------------------------ Free Filter design

// Maps the free filter state to RBJ coefficients (shared by DSP and editor).
Coefs freeFilterCoefs(int type, double sr, double hz, double q, double gainDb);

// Curve EQ minimum-phase band b: shelves at both ends (they keep their gain up to
// DC / Nyquist), peaking filters in between.
inline Coefs ceBandCoefs(double sr, int band, double gainDb) {
  if (band == 0) return freeFilterCoefs(FtLowShelf, sr, ceBandHz(0) * 1.25, 0.707, gainDb);
  if (band == kCeBands - 1) return freeFilterCoefs(FtHighShelf, sr, std::min(ceBandHz(band) * 0.8, sr * 0.4), 0.707, gainDb);
  return peakCoefs(sr, ceBandHz(band), kCeBankQ, gainDb);
}
inline void CeBank::prepare(double sampleRate) {
  sr = sampleRate;
  const double proto = 6.0;
  for (int j = 0; j < kCeBands; ++j) {
    Coefs c = ceBandCoefs(sr, j, proto);
    for (int i = 0; i < kCeBands; ++i)
      m[i][j] = static_cast<float>(20.0 * std::log10(std::max(1e-9, c.magnitude(std::min(ceBandHz(i), sr * 0.45), sr))) / proto);
  }
}

// ------------------------------------------------------------- Bass Boost design

// The boost is a low shelf with a slightly resonant knee (Q 0.8): full lift below
// `hz`, untouched highs, no cancellation dip above the corner.
inline double bassBoostDb(double amount) { return 18.0 * std::clamp(amount, 0.0, 1.0); }
inline Coefs bassBoostCoefs(double sr, double hz, double amount) { return freeFilterCoefs(FtLowShelf, sr, hz, 0.8, bassBoostDb(amount)); }

// ---------------------------------------------------------------- notes data

// Pages are stored in PluginState::data as "GLNOTES1\n" followed by, for each
// non-empty page, "#<page>:<length>\n<text>\n" (length in bytes; text verbatim).
std::vector<std::string> decodeNotes(const std::string& data);
std::string encodeNotes(const std::vector<std::string>& pages);

}  // namespace gl::packs::utility
