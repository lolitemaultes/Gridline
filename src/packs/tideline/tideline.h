// Gridline — "tideline" pack: Tideline, a one-knob rhythmic volume shaper
// (sidechain-style pumping without a sidechain). Parameter indices and the
// gain-curve model shared by the catalogue, the DSP, the editor and the tests.
//
//   gl.tideline   Tideline   tempo-synced / audio- / MIDI-triggered ducking curves,
//                          16 built-in shapes + a drawable 8-node curve, duration
//                          stretch, phase shift, low/high band split, stereo/mid/side,
//                          anti-click smoothing, zero latency.
//
// Every built-in shape is a small parametric definition (attack / hold /
// release times as fractions of the cycle, release law and exponent, floor,
// repeats, reverse) evaluated by curveGain(); nothing is tabulated.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace gl::packs::tideline {

inline constexpr int kNodes = 8;  // custom curve nodes

enum Param {
  BwAmount = 0,  // 0..1 depth of the duck (0 = untouched, 1 = full curve)
  BwShape,       // choice: 16 shapes + Custom
  BwRate,        // choice: cycle length (see kRateBeats)
  BwTrigger,     // choice: Tempo / Audio / MIDI
  BwLoop,        // toggle: triggered modes repeat the cycle until the next trigger
  BwEnd,         // 0..1 one-shot end: the curve holds its value from here until the next trigger
  BwDuration,    // -1..1 stretches (+) or shortens (-) the duck
  BwShift,       // -0.5..0.5 cycle: moves the curve against the beat / delays a trigger
  BwSmooth,      // ms: anti-click smoothing of the gain
  BwBand,        // choice: Full / Lows / Highs
  BwBandFreq,    // Hz crossover
  BwBandListen,  // toggle: hear only the processed band
  BwStereo,      // choice: Stereo / Mid / Side
  BwScIn,        // J-45 input route of the audio trigger (-1 sidechain input, 0 own input, n source)
  BwThreshold,   // dB audio trigger threshold
  BwRetrigger,   // ms minimum time between audio triggers
  BwDetLow,      // Hz detector high-pass
  BwDetHigh,     // Hz detector low-pass
  BwMidiPort,    // -1 any port, else 0..255
  BwMidiNote,    // -1 any note, else 0..127
  BwTension,     // -1..1 bend of the custom curve's segments
  BwNodeX0,      // kNodes x positions (0..1 of the cycle)
  BwNodeY0 = BwNodeX0 + kNodes,  // kNodes levels (0 = silent, 1 = full)
  BwCount = BwNodeY0 + kNodes
};

inline constexpr int kShapeCount = 16;
inline constexpr int kShapeCustom = kShapeCount;  // last choice
inline const char* const kShapeNames[kShapeCount + 1] = {
    "Pump",   "Snap",     "Tight",    "Classic", "Round",  "Long",   "Breath", "Glide", "Sub Soft",
    "Sub Deep", "Trim",   "Trim Fade", "Inhale", "Inhale Long", "Chop", "Stutter", "Custom"};

inline constexpr int kRateCount = 10;
inline const char* const kRateNames[kRateCount] = {"1/16", "1/8", "1/4", "1/2", "1 bar", "2 bars", "1/8 T", "1/4 T", "1/8 D", "1/4 D"};
inline constexpr double kRateBeats[kRateCount] = {0.25, 0.5, 1.0, 2.0, 4.0, 8.0, 1.0 / 3.0, 2.0 / 3.0, 0.75, 1.5};
inline constexpr int kRateDefault = 2;  // 1/4

enum Trigger { TrigTempo = 0, TrigAudio, TrigMidi };
enum Band { BandFull = 0, BandLows, BandHighs };
enum Stereo { StStereo = 0, StMid, StSide };

// Release laws of the built-in shapes.
enum Law {
  LawEase = 0,  // 1-(1-u)^k: fast recovery that settles gently (the classic pump)
  LawRise,      // u^k: stays down, then opens late (sub-friendly, "trim")
  LawS,         // symmetric S-curve u^k / (u^k + (1-u)^k)
  LawLinear     // straight line
};

struct ShapeDef {
  float open;     // cycle fraction left at unity before the drop (trim shapes let the kick through)
  float attack;   // cycle fraction from unity down to the floor
  float hold;     // cycle fraction held at the floor
  float release;  // cycle fraction back up to unity
  float floor;    // lowest gain of the curve (0 = silent)
  int law;        // Law
  float k;        // law exponent
  int repeats;    // ducks per cycle (rhythm shapes)
  bool reverse;   // time-reversed (the duck swells into the beat)
};

// Gridline's own parametric shapes (fractions of one cycle). Families:
// 0-7 pump, 8-9 sub bass, 10-11 trim (on the kick itself), 12-13 reverse, 14-15 rhythm.
inline const ShapeDef kShapes[kShapeCount] = {
    {0.f, 0.004f, 0.000f, 0.62f, 0.00f, LawEase, 2.6f, 1, false},    // Pump: fast drop, smooth fast recovery
    {0.f, 0.003f, 0.000f, 0.32f, 0.00f, LawEase, 2.2f, 1, false},    // Snap: short tight duck
    {0.f, 0.003f, 0.020f, 0.45f, 0.00f, LawS, 1.8f, 1, false},       // Tight: brief hold then an S-shaped return
    {0.f, 0.004f, 0.000f, 0.80f, 0.00f, LawEase, 3.4f, 1, false},    // Classic: long tail over most of the beat
    {0.f, 0.006f, 0.000f, 0.70f, 0.00f, LawS, 1.4f, 1, false},       // Round: soft S across the beat
    {0.f, 0.004f, 0.050f, 0.90f, 0.00f, LawRise, 0.7f, 1, false},    // Long: stays low, opens before the next beat
    {0.f, 0.020f, 0.000f, 0.85f, 0.00f, LawS, 1.15f, 1, false},      // Breath: gentle, near-linear S
    {0.f, 0.004f, 0.000f, 0.95f, 0.00f, LawLinear, 1.0f, 1, false},  // Glide: straight ramp over the whole beat
    {0.f, 0.008f, 0.060f, 0.70f, 0.00f, LawRise, 1.6f, 1, false},    // Sub Soft: sub bass, slower fade-up
    {0.f, 0.008f, 0.120f, 0.80f, 0.00f, LawRise, 2.4f, 1, false},    // Sub Deep: sub bass, long low hold
    {0.10f, 0.020f, 0.780f, 0.08f, 0.00f, LawS, 1.5f, 1, false},     // Trim: keeps the hit, cuts its tail
    {0.03f, 0.300f, 0.580f, 0.08f, 0.00f, LawS, 1.5f, 1, false},     // Trim Fade: hit fades out, then silence
    {0.f, 0.004f, 0.000f, 0.60f, 0.00f, LawEase, 2.4f, 1, true},     // Inhale: reverse swell into the beat
    {0.f, 0.004f, 0.000f, 0.95f, 0.00f, LawS, 1.6f, 1, true},        // Inhale Long: long reverse suction
    {0.f, 0.010f, 0.200f, 0.06f, 0.00f, LawS, 1.0f, 2, false},       // Chop: two gated cuts per cycle
    {0.f, 0.006f, 0.000f, 0.55f, 0.00f, LawEase, 2.0f, 4, false},    // Stutter: four quick pumps per cycle
};

// Release law u (0..1) -> 0..1.
inline double lawValue(int law, double u, double k) {
  u = std::clamp(u, 0.0, 1.0);
  switch (law) {
    case LawEase: return 1.0 - std::pow(1.0 - u, k);
    case LawRise: return std::pow(u, k);
    case LawS: {
      double a = std::pow(u, k), b = std::pow(1.0 - u, k);
      return a + b > 0 ? a / (a + b) : u;
    }
    default: return u;
  }
}

// Duration factor of the Duration parameter (-1..1 -> 0.35x..2.8x).
inline double durationFactor(double duration) { return std::pow(2.0, std::clamp(duration, -1.0, 1.0) * 1.5); }

// Built-in shape gain at cycle position p (0..1), duration factor f.
inline double shapeGain(const ShapeDef& s, double p, double f) {
  p -= std::floor(p);
  if (s.repeats > 1) {
    p *= s.repeats;
    p -= std::floor(p);
  }
  if (s.reverse) p = 1.0 - p;
  const double o = s.open, a = s.attack;
  double h = s.hold * f, r = s.release * f;
  const double room = 1.0 - o - a;  // stretched segments never pass the cycle end
  if (h + r > room) {
    const double q = room / (h + r);
    h *= q;
    r *= q;
  }
  const double fl = s.floor;
  if (p < o) return 1.0;
  p -= o;
  if (p < a) {  // cosine drop: no corner at either end
    const double u = p / a;
    return fl + (1.0 - fl) * 0.5 * (1.0 + std::cos(u * 3.14159265358979));
  }
  p -= a;
  if (p < h) return fl;
  p -= h;
  if (p < r) return fl + (1.0 - fl) * lawValue(s.law, p / r, s.k);
  return 1.0;
}

// Custom curve: nodes (x, y) sorted by x, closed over the cycle (the last
// node joins the first one across the cycle end). Tension bends every
// segment (> 0 rises early, < 0 rises late).
struct CustomCurve {
  double x[kNodes], y[kNodes];
  int n = 0;
  double tension = 0;
};

inline double bendSeg(double t, double tension) {
  t = std::clamp(t, 0.0, 1.0);
  if (std::fabs(tension) < 1e-6) return t;
  const double e = std::pow(2.0, std::fabs(tension) * 2.5);
  return tension > 0 ? 1.0 - std::pow(1.0 - t, e) : std::pow(t, e);
}

// Fills a CustomCurve from a parameter vector (sorted; insertion sort, no allocation).
template <class Vec>
inline void customFrom(const Vec& params, CustomCurve& c) {
  c.n = kNodes;
  c.tension = std::clamp(static_cast<double>(params[BwTension]), -1.0, 1.0);
  for (int i = 0; i < kNodes; ++i) {
    c.x[i] = std::clamp(static_cast<double>(params[static_cast<size_t>(BwNodeX0 + i)]), 0.0, 1.0);
    c.y[i] = std::clamp(static_cast<double>(params[static_cast<size_t>(BwNodeY0 + i)]), 0.0, 1.0);
  }
  for (int i = 1; i < kNodes; ++i)
    for (int j = i; j > 0 && c.x[j - 1] > c.x[j]; --j) {
      std::swap(c.x[j - 1], c.x[j]);
      std::swap(c.y[j - 1], c.y[j]);
    }
}

// Custom curve gain at p, time-scaled by the duration factor f around the first node.
inline double customGain(const CustomCurve& c, double p, double f) {
  p -= std::floor(p);
  if (c.n <= 0) return 1.0;
  if (f != 1.0) p = std::clamp(c.x[0] + (p - c.x[0]) / f, 0.0, 1.0);
  // segment containing p (closed loop)
  int i = c.n - 1;
  for (int k = 0; k < c.n; ++k)
    if (c.x[k] <= p) i = k;
    else break;
  const bool before = p < c.x[0];
  const int j = before ? 0 : (i + 1) % c.n;
  double x0 = c.x[before ? c.n - 1 : i], x1 = c.x[j];
  double pp = p;
  if (before) x0 -= 1.0;           // wrap from the last node
  else if (j == 0) x1 += 1.0;      // wrap to the first node
  const double y0 = c.y[before ? c.n - 1 : i], y1 = c.y[j];
  const double w = x1 - x0;
  const double t = w > 1e-9 ? (pp - x0) / w : 1.0;
  return y0 + (y1 - y0) * bendSeg(t, y1 >= y0 ? c.tension : -c.tension);
}

// Default custom nodes: a pump to start editing from.
inline constexpr float kDefNodeX[kNodes] = {0.00f, 0.03f, 0.10f, 0.22f, 0.36f, 0.52f, 0.72f, 0.94f};
inline constexpr float kDefNodeY[kNodes] = {0.00f, 0.08f, 0.38f, 0.64f, 0.81f, 0.91f, 0.97f, 1.00f};

// Curve gain (0..1, before Amount) of shape `shape` at cycle position p.
inline double curveAt(int shape, const CustomCurve& custom, double p, double duration) {
  const double f = durationFactor(duration);
  if (shape >= 0 && shape < kShapeCount) return shapeGain(kShapes[shape], p, f);
  return customGain(custom, p, f);
}

// Output gain with the Amount control: 1 - amount * (1 - curve).
inline double applyAmount(double curve, double amount) { return 1.0 - std::clamp(amount, 0.0, 1.0) * (1.0 - curve); }

// UI side channel (Effect::meters): [0] cycle position 0..1 (-1 idle),
// [1] current gain 0..1, [2] trigger flash 0..1, [3] output peak 0..1.
inline constexpr int kMeterCount = 4;

// ---- Kickstart stand-in (core/PluginStandIn.h): a missing Kickstart /
// Kickstart 2 opens as Tideline. The plugin's saved state (a JUCE value tree
// in its VST3 / AU / VST2 state) is read for the loop length, the selected
// curve (redrawn as Tideline's custom curve), mix, band and trigger settings.
struct KickstartReport {
  std::string format, error;
  std::vector<std::string> notes;  // what Tideline only approximates
};
// Tideline parameters for a hosted Kickstart state (`type`: hosted type,
// `data`: its saved state, `hosted`: its saved parameter values).
bool kickstartToTideline(const std::string& type, const std::string& data, const std::vector<float>& hosted,
                         std::vector<float>& out, KickstartReport& rep);
// Parameter `index` of the hosted type -> Tideline parameter + normalized
// value curve (17 points over the original's 0..1). False: no counterpart.
// Parameter index of a plugin parameter id (VST3 / AU id; VST2: the index) or -1.
int kickstartParamIndexOfId(const std::string& type, uint32_t id);
bool kickstartParamLink(const std::string& type, int index, int& param, std::vector<float>& curve, std::string& title);

}  // namespace gl::packs::tideline
