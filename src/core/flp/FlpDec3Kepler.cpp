// Gridline — FLP import, third wave: Kepler Exo → Analog ("gl.analog").
//
// State (all shipped presets, FL 21.2 – 24): u32 1, u32 1, u32 size, u32 0,
// u32 0, u32 size, u32 0, u32 name length, name ("Kepler-2", NUL included),
// u32 version (1, 2), u32 count (151), `count` floats 0..1 in parameter-tag
// order (unaligned), then u32 1, u32 rows (8) and per row i32 source,
// i32 destination (-1 = empty row; the destination is a parameter tag), then
// a 12-byte-header chunk of opaque runtime data. Tags 151..154 (VCA smooth,
// saturator quality, master tune, high-pass switch) are not stored.
//
// Tag map (normalized values; switches as index / (n - 1)):
//   0 DCO1 LFO1 amount, 1 its range switch, 2 DCO1 LFO2 amount, 3 range,
//   4 env 2 → DCO1 (0.5 = none), 5 DCO1 pulse width, 6 PW source (LFO / manual /
//   env), 7 pulse on, 8 saw on, 9 sub on, 10 sub level, 11 sub phase,
//   12 DCO2 on, 13 DCO2 semitones (-48..48), 14 DCO2 detune, 15 saw on,
//   16 sub on, 17 sub level, 18 sub phase, 19 DCO2 pulse width, 20/21 DCO2
//   LFO1 amount / range, 22/23 LFO2 amount / range, 24 env 2 → DCO2, 25 PW
//   source, 26 pulse on, 27 hard sync, 28/29 cross mod 1→2 / 2→1, 30/31 their
//   shapes, 32/33 DCO levels, 34 noise, 35 DCO1/2 AM, 36 high-pass, 37 cutoff,
//   38 resonance, 39 key follow, 40 slope (6/12/18/24 dB), 41 filter FM,
//   42 its shape, 43 env polarity, 44 env 1 → cutoff, 45/46 LFO1 / LFO2 →
//   cutoff, 47 VCA source (env 1 / gate / env 2), 48-53 LFO1 rate, sync, delay,
//   trigger mode, retrigger phase on, phase, 54-60 LFO2 rate, sync, delay,
//   trigger mode, shape, retrigger phase on, phase, 61-64 env 1 ADSR,
//   65-68 env 2 ADSR, 69 chorus mode (off / I / II / I+II), 70 chorus noise,
//   71 chorus gate, 72 volume, 73 velocity → volume, 74 pan, 75 mono,
//   76 always slide, 77 retrigger, 78 release mode, 79 portamento mode,
//   80 portamento, 81 LFO trigger, 82 hold, 83 octave (-1 / 0 / +1),
//   84 bend range, 85-91 arpeggiator (on, rate, sync, range, mode 0..8, gate,
//   variant), 92-99 matrix amounts (0.5 = 0), 100-109 EQ (on, Q ×3, gain ×3,
//   freq ×3), 110-120 delay (on, sync, input, time, right tap, feedback,
//   feedback cutoff, feedback type, stereo, blur, tone), 121-133 reverb (on,
//   low cut, high cut, predelay, predelay feedback, size, diffusion, decay,
//   damping, width, dry, early, wet), 134-136 saturation (on, amount, pre /
//   post), 137-148 ensemble chorus (on, delay, feedback, rate, depth, movement,
//   movement rate, lines, low pass, high pass, mix, phase), 149/150 X/Y offset.
// Matrix sources: 0 LFO1 bipolar triangle, 1 LFO2 bipolar, 2 env 1, 3 env 2,
// 4 velocity, 5 release velocity, 6-9 mod X / Y (bi / uni), 10 gate,
// 11 inverted gate, 12 LFO1 sine, 13 LFO1 unipolar, 14 LFO2 unipolar, 15 key
// (bipolar), 16-20 extra-range LFO variants, 21 alternate, 22 key (unipolar),
// 23 random.
//
// Mapping onto the three-oscillator Analog: osc 1 = DCO1 (saw, else pulse),
// osc 2 = DCO2 (saw, else pulse; hard sync / cross-mod / AM from osc 1),
// osc 3 = the second waveform of DCO1 (or of DCO2) when both are on; one sub;
// env 1 → filter envelope (and the amp envelope when the VCA follows it),
// env 2 → mod envelope; LFO routes and the matrix share Analog's two LFO
// targets and four matrix rows; chorus / ensemble, delay, reverb and
// saturation → the FX block. The two engines differ, so every mapping is
// approximate (exact = false).
#include <algorithm>
#include <cmath>
#include "../../packs/synths/synths.h"
#include "FlpDec3.h"

namespace gl::flp::dec3 {

namespace {

namespace sy = gl::packs::synths;
using gen2::begin;
using gen2::set;

constexpr int kTags = 151;

double tlog(double x, double lo, double hi) { return lo * std::pow(hi / lo, std::clamp(x, 0.0, 1.0)); }
int idx(double x, int n) { return std::clamp(static_cast<int>(std::lround(x * (n - 1))), 0, n - 1); }
double bip(double x) { return std::clamp(x * 2.0 - 1.0, -1.0, 1.0); }

// Value curves (reasoned from the control ranges and the presets' sound
// design; not measured).
double cutoffHz(double x) { return tlog(x, 20.0, 20000.0); }
double atkSec(double x) { return tlog(x, 0.001, 3.0); }
double decSec(double x) { return tlog(x, 0.002, 12.0); }
double lfoHz(double x) { return std::clamp(tlog(x, 0.02, 40.0), 0.02, 40.0); }
double glideSec(double x) { return x <= 0 ? 0.001 : tlog(x, 0.005, 2.0); }
// Synced LFO rate (high = fast) → Analog division index.
int lfoDiv(double x) { return std::clamp(static_cast<int>(std::lround((1.0 - std::clamp(x, 0.0, 1.0)) * (sy::DivCount - 1))), 0, sy::DivCount - 1); }
// Oscillator LFO depth → pitch modulation (±1 = ±24 semitones in Analog).
double oscLfoAmt(double x, bool range) { return std::clamp(x * x * (range ? 24.0 : 2.0) / 24.0, 0.0, 1.0); }

struct State {
  float v[kTags] = {};
  int src[8] = {-1, -1, -1, -1, -1, -1, -1, -1}, dst[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
  bool ok = false;
  double operator()(int tag) const { return tag >= 0 && tag < kTags ? static_cast<double>(v[tag]) : 0.0; }
  bool on(int tag) const { return (*this)(tag) >= 0.5; }
};

State parse(const Blob& b) {
  State s;
  uint32_t nameLen = b.u32(28);
  if (b.u32(0) != 1 || nameLen == 0 || nameLen > 64) return s;
  size_t at = 32 + nameLen;
  if (!b.has(at, 8)) return s;
  size_t count = b.u32(at + 4);
  if (count == 0 || count > 1024 || !b.has(at + 8, 4 * count)) return s;
  for (size_t t = 0; t < static_cast<size_t>(kTags) && t < count; ++t) {
    float f = b.f32(at + 8 + 4 * t);
    s.v[t] = std::isfinite(f) ? std::clamp(f, 0.0f, 1.0f) : 0.0f;
  }
  size_t m = at + 8 + 4 * count;
  if (b.has(m, 8) && b.u32(m) == 1) {
    size_t rows = std::min<size_t>(b.u32(m + 4), 8);
    for (size_t r = 0; r < rows && b.has(m + 8 + 8 * r, 8); ++r) {
      s.src[r] = b.i32(m + 8 + 8 * r);
      s.dst[r] = b.i32(m + 12 + 8 * r);
    }
  }
  s.ok = true;
  return s;
}

int mxSource(int k) {
  switch (k) {
    case 0: case 12: case 13: case 16: case 18: case 19: return sy::AsLfo1;
    case 1: case 14: case 17: case 20: return sy::AsLfo2;
    case 2: return sy::AsFilterEnv;
    case 3: return sy::AsModEnv;
    case 4: case 5: return sy::AsVelocity;
    case 10: return sy::AsAmpEnv;
    case 15: case 22: return sy::AsKey;
    case 23: return sy::AsRandom;
    default: return sy::AsNone;  // mod wheel X / Y, inverted gate, alternate
  }
}

int mxDest(int tag) {
  switch (tag) {
    case 37: case 36: return sy::AdCutoff;
    case 38: return sy::AdResonance;
    case 72: return sy::AdVolume;
    case 74: return sy::AdPan;
    case 34: return sy::AdNoise;
    case 5: case 19: return sy::AdPulseWidth;
    case 13: case 14: return sy::AdOsc2Pitch;
    case 28: case 29: case 35: return sy::AdOscMod;
    case 48: return sy::AdLfo1Rate;
    case 54: return sy::AdLfo2Rate;
    default: return sy::AdNone;
  }
}

// Collects modulation routes, then gives each LFO its strongest route and
// puts the rest into the matrix.
struct Routes {
  struct R {
    int src, dst;
    double amt;
  };
  std::vector<R> list;
  void add(int src, int dst, double amt) {
    if (src == sy::AsNone || dst == sy::AdNone || std::fabs(amt) < 1e-4) return;
    for (auto& r : list)
      if (r.src == src && r.dst == dst) {
        r.amt = std::clamp(r.amt + amt, -1.0, 1.0);
        return;
      }
    list.push_back({src, dst, std::clamp(amt, -1.0, 1.0)});
  }
};

bool decodeKeplerExo(Context&, const std::vector<Event>& evs, PluginMapping& m) {
  using namespace sy;
  State t = parse(gen2::stateBlob(evs));
  begin(m, "gl.analog");
  m.exact = false;
  if (!t.ok) return true;  // empty / unknown state: defaults

  // ---- oscillators
  const bool p1 = t.on(7), s1 = t.on(8), sub1 = t.on(9);
  const bool dco2 = t.on(12), s2 = t.on(15), sub2 = t.on(16), p2 = t.on(26);
  const double lvl1 = t(32), lvl2 = t(33);
  const int semis2 = static_cast<int>(std::lround(t(13) * 96.0 - 48.0));
  const double fine2 = t(14) * 50.0;  // detune 0..1 → 0..+50 cents
  auto pw = [](double x) { return 0.5 - 0.45 * std::clamp(x, 0.0, 1.0); };
  auto osc = [&](int k, bool on, int wave, int coarse, double fine, double level, double width) {
    set(m, anOsc(k, OscOn), on ? 1 : 0);
    set(m, anOsc(k, OscWave), wave);
    set(m, anOsc(k, OscCoarse), std::clamp(coarse, -36, 36));
    set(m, anOsc(k, OscFine), std::clamp(fine, -100.0, 100.0));
    set(m, anOsc(k, OscLevel), std::clamp(level, 0.0, 1.0));
    set(m, anOsc(k, OscPw), width);
    set(m, anOsc(k, OscMod), ModNone);
  };
  const bool on1 = p1 || s1, on2 = dco2 && (p2 || s2);
  osc(0, on1, s1 ? WaveSaw : WavePulse, 0, 0.0, 0.8 * lvl1, pw(t(5)));
  osc(1, on2, s2 ? WaveSaw : WavePulse, semis2, fine2, 0.8 * lvl2, pw(t(19)));
  if (p1 && s1) osc(2, true, WavePulse, 0, 0.0, 0.8 * lvl1, pw(t(5)));
  else if (dco2 && p2 && s2) osc(2, true, WavePulse, semis2, fine2, 0.8 * lvl2, pw(t(19)));
  else osc(2, false, WavePulse, 0, 0.0, 0.0, 0.5);
  // Osc 2 is modulated by osc 1 in Analog: hard sync, cross-mod 1→2 or AM.
  if (on2 && t.on(27)) {
    set(m, anOsc(1, OscMod), ModSync);
  } else if (on2 && t(28) > 0.01) {
    set(m, anOsc(1, OscMod), ModFm);
    set(m, anOsc(1, OscModAmt), t(28));
  } else if (on2 && t(35) > 0.01) {
    set(m, anOsc(1, OscMod), ModRing);
    set(m, anOsc(1, OscModAmt), t(35));
  }
  // Sub: DCO1's, else DCO2's when it plays at the base pitch.
  double sub = sub1 ? t(10) * lvl1 : (dco2 && sub2 && semis2 == 0 ? t(17) * lvl2 : 0.0);
  set(m, AnSubLevel, std::clamp(0.8 * sub, 0.0, 1.0));
  set(m, AnSubOct, 0);
  set(m, AnSubWave, 1);
  set(m, AnNoise, t(34));
  set(m, AnUniVoices, 1);

  // ---- filter and envelopes
  set(m, AnFltMode, t(40) < 0.5 ? FltLp12 : FltLp24);
  set(m, AnCutoff, cutoffHz(t(37)));
  set(m, AnReso, t(38));
  set(m, AnKeyTrack, t(39));
  set(m, AnFltEnv, (t.on(43) ? -1.0 : 1.0) * t(44));
  set(m, AnVelFilter, 0.0);
  const double A1 = atkSec(t(61)), D1 = decSec(t(62)), S1 = t(63), R1 = decSec(t(64));
  const double A2 = atkSec(t(65)), D2 = decSec(t(66)), S2 = t(67), R2 = decSec(t(68));
  set(m, AnFenA, A1), set(m, AnFenD, D1), set(m, AnFenS, S1), set(m, AnFenR, R1);
  set(m, AnModA, A2), set(m, AnModD, D2), set(m, AnModS, S2), set(m, AnModR, R2);
  switch (idx(t(47), 3)) {
    case 0: set(m, AnAmpA, A1), set(m, AnAmpD, D1), set(m, AnAmpS, S1), set(m, AnAmpR, R1); break;
    case 2: set(m, AnAmpA, A2), set(m, AnAmpD, D2), set(m, AnAmpS, S2), set(m, AnAmpR, R2); break;
    default: set(m, AnAmpA, 0.002), set(m, AnAmpD, 0.3), set(m, AnAmpS, 1.0), set(m, AnAmpR, 0.008); break;  // gate
  }

  // ---- LFOs
  set(m, anLfo(0, LfoShapeP), LfoTriangle);
  static const int kLfo2Shapes[6] = {LfoSine, LfoTriangle, LfoSquare, LfoSawUp, LfoSawDown, LfoSampleHold};
  set(m, anLfo(1, LfoShapeP), kLfo2Shapes[idx(t(58), 6)]);
  for (int l = 0; l < 2; ++l) {
    int base = l == 0 ? 48 : 54;
    bool sync = t.on(base + 1);
    set(m, anLfo(l, LfoSync), sync ? 1 : 0);
    set(m, anLfo(l, LfoRate), lfoHz(t(base)));
    set(m, anLfo(l, LfoDiv), lfoDiv(t(base)));
    set(m, anLfo(l, LfoDelay), t(base + 2) > 0 ? tlog(t(base + 2), 0.01, 5.0) : 0.001);
    set(m, anLfo(l, LfoRetrig), t.on(base + 3) ? 1 : 0);
    set(m, anLfo(l, LfoDest), AdNone);
    set(m, anLfo(l, LfoAmt), 0.0);
  }

  // ---- modulation routes
  Routes rt;
  // Fixed routes of the panel.
  double l1o1 = oscLfoAmt(t(0), t.on(1)), l1o2 = dco2 ? oscLfoAmt(t(20), t.on(21)) : 0.0;
  double l2o1 = oscLfoAmt(t(2), t.on(3)), l2o2 = dco2 ? oscLfoAmt(t(22), t.on(23)) : 0.0;
  auto pitchRoute = [&](int src, double a1, double a2) {
    if (a1 > 0 && std::fabs(a1 - a2) < 1e-3) {
      rt.add(src, AdPitch, a1);
    } else {
      rt.add(src, AdOsc1Pitch, a1);
      rt.add(src, AdOsc2Pitch, a2);
    }
  };
  pitchRoute(AsLfo1, l1o1, l1o2);
  pitchRoute(AsLfo2, l2o1, l2o2);
  rt.add(AsLfo1, AdCutoff, t(45));
  rt.add(AsLfo2, AdCutoff, t(46));
  // Env 2 → DCO pitch (0.5 = none, ±1 = ±12 semitones).
  double e2o1 = bip(t(4)) * 0.5, e2o2 = dco2 ? bip(t(24)) * 0.5 : 0.0;
  if (std::fabs(e2o1) > 1e-3 && std::fabs(e2o1 - e2o2) < 1e-3) {
    rt.add(AsModEnv, AdPitch, e2o1);
  } else {
    rt.add(AsModEnv, AdOsc1Pitch, e2o1);
    rt.add(AsModEnv, AdOsc2Pitch, e2o2);
  }
  // Pulse-width modulation (source LFO1 / manual / env 1) of the pulse oscillators.
  for (int k = 0; k < 2; ++k) {
    bool pulse = k == 0 ? p1 : (dco2 && p2);
    int ps = idx(t(k == 0 ? 6 : 25), 3);
    if (pulse && ps != 1) rt.add(ps == 0 ? AsLfo1 : AsFilterEnv, AdPulseWidth, 0.9 * t(k == 0 ? 5 : 19) / (k == 1 && p1 ? 2.0 : 1.0));
  }
  // The panel's matrix (amount 0.5 = none).
  for (int r = 0; r < 8; ++r) {
    if (t.src[r] < 0 || t.dst[r] < 0) continue;
    rt.add(mxSource(t.src[r]), mxDest(t.dst[r]), bip(t(92 + r)));
  }
  // Mod env: its strongest route; LFOs: theirs; the rest → matrix rows.
  auto takeBest = [&](int src) -> Routes::R {
    Routes::R best{AsNone, AdNone, 0.0};
    size_t at = rt.list.size();
    for (size_t i = 0; i < rt.list.size(); ++i)
      if (rt.list[i].src == src && std::fabs(rt.list[i].amt) > std::fabs(best.amt)) best = rt.list[i], at = i;
    if (at < rt.list.size()) rt.list.erase(rt.list.begin() + static_cast<long>(at));
    return best;
  };
  Routes::R me = takeBest(AsModEnv);
  set(m, AnModDest, me.dst);
  set(m, AnModAmt, me.amt);
  for (int l = 0; l < 2; ++l) {
    Routes::R r = takeBest(l == 0 ? AsLfo1 : AsLfo2);
    set(m, anLfo(l, LfoDest), r.dst);
    set(m, anLfo(l, LfoAmt), r.amt);
  }
  std::stable_sort(rt.list.begin(), rt.list.end(), [](const Routes::R& a, const Routes::R& b) { return std::fabs(a.amt) > std::fabs(b.amt); });
  for (int r = 0; r < kAnMatrixRows; ++r) {
    bool use = static_cast<size_t>(r) < rt.list.size();
    set(m, anMx(r, 0), use ? rt.list[static_cast<size_t>(r)].src : AsNone);
    set(m, anMx(r, 1), use ? rt.list[static_cast<size_t>(r)].dst : AdNone);
    set(m, anMx(r, 2), use ? rt.list[static_cast<size_t>(r)].amt : 0.0);
  }

  // ---- voice
  set(m, AnVolume, std::min(1.0, 1.1 * t(72)));
  set(m, AnVelAmp, t(73));
  set(m, AnPan, bip(t(74)));
  set(m, AnPlayMode, t.on(75) ? PlayMono : PlayPoly);
  set(m, AnGlide, glideSec(t(80)));
  set(m, AnGlideMode, t.on(76) ? 0 : 1);
  set(m, AnTranspose, 12.0 * (idx(t(83), 3) - 1));

  // ---- arpeggiator: rate 1, 1/2, 1/4, 1/8, 1/16 beat; modes up, down,
  // up & down, three random kinds, three as-played kinds.
  set(m, AnArpOn, t.on(85) ? 1 : 0);
  static const int kArpDiv[5] = {Div1_4, Div1_8, Div1_16, Div1_32, Div1_32};
  set(m, AnArpDiv, kArpDiv[idx(t(86), 5)]);
  set(m, AnArpOct, 1 + idx(t(88), 3));
  static const int kArpMode[9] = {ArpUp, ArpDown, ArpUpDown, ArpRandom, ArpRandom, ArpRandom, ArpPlayed, ArpPlayed, ArpPlayed};
  set(m, AnArpMode, kArpMode[idx(t(89), 9)]);
  set(m, AnArpGate, std::clamp(t(90), 0.05, 1.0));

  // ---- effects
  const int fx = AnFxDrive;
  set(m, fx + FxDrive, t.on(134) ? 0.7 * t(135) : 0.0);
  int cho = idx(t(69), 4);  // off, I, II, I+II
  if (t.on(137)) {
    set(m, fx + FxChoOn, 1);
    set(m, fx + FxChoRate, tlog(t(140), 0.05, 5.0));
    set(m, fx + FxChoDepth, t(141));
    set(m, fx + FxChoMix, t(147));
  } else {
    set(m, fx + FxChoOn, cho > 0 ? 1 : 0);
    set(m, fx + FxChoRate, cho == 1 ? 0.5 : cho == 2 ? 0.85 : 1.0);
    set(m, fx + FxChoDepth, cho == 3 ? 0.7 : 0.5);
    set(m, fx + FxChoMix, 0.5);
  }
  set(m, fx + FxDlyOn, t.on(110) ? 1 : 0);
  // Delay time 0..1 → divisions from 1/32 to 1 bar (synced or not).
  set(m, fx + FxDlyDiv, std::clamp(static_cast<int>(std::lround(t(113) * Div1_1)), 0, static_cast<int>(Div1_1)));
  set(m, fx + FxDlyFb, 0.95 * t(115));
  set(m, fx + FxDlyMix, 0.6 * t(112));
  set(m, fx + FxRevOn, t.on(121) ? 1 : 0);
  set(m, fx + FxRevSize, t(126));
  set(m, fx + FxRevDamp, t(129));
  set(m, fx + FxRevMix, t(133));

  // ---- automation: FL parameter index = tag; recorded values use the
  // control ranges of the editor.
  auto cutFn = [](double n) { return cutoffHz(n); };
  std::vector<Link> links = {
      {10, AnSubLevel, [](double n) { return 0.8 * n; }},
      {34, AnNoise, {}},
      {37, AnCutoff, cutFn},
      {38, AnReso, [](double n) { return n; }},
      {39, AnKeyTrack, [](double n) { return n; }},
      {44, AnFltEnv, [neg = t.on(43)](double n) { return neg ? -n : n; }},
      {48, anLfo(0, LfoRate), lfoHz},
      {54, anLfo(1, LfoRate), lfoHz},
      {61, AnFenA, atkSec},
      {62, AnFenD, decSec},
      {63, AnFenS, [](double n) { return n; }},
      {64, AnFenR, decSec},
      {65, AnModA, atkSec},
      {66, AnModD, decSec},
      {67, AnModS, [](double n) { return n; }},
      {68, AnModR, decSec},
      {72, AnVolume, [](double n) { return std::min(1.0, 1.1 * n); }},
      {73, AnVelAmp, [](double n) { return n; }},
      {74, AnPan, bip},
      {80, AnGlide, glideSec},
      {126, fx + FxRevSize, [](double n) { return n; }},
      {133, fx + FxRevMix, [](double n) { return n; }},
      {135, fx + FxDrive, [](double n) { return 0.7 * n; }},
  };
  std::vector<RawRange> ranges = {{10, 0, 6000},   {34, 0, 245760}, {37, 1000, 170000}, {38, 0, 6000}, {39, 0, 6000},
                                  {44, 0, 6000},   {48, 1, 245760}, {54, 1, 245760},    {61, 0, 10000}, {62, 0, 10000},
                                  {63, 0, 6000},   {64, 0, 10000},  {65, 0, 10000},     {66, 0, 10000}, {67, 0, 6000},
                                  {68, 0, 10000},  {72, 0, 65536},  {73, 0, 65536},     {74, 0, 65536}, {80, 0, 65536},
                                  {126, 0, 10000}, {133, 0, 10000}, {135, 0, 6000}};
  linkAll(m, std::move(links), std::move(ranges));
  return true;
}

}  // namespace

void registerKepler(std::map<std::string, PluginDecoder>& d) { add(d, "Kepler Exo", decodeKeplerExo); }

}  // namespace gl::flp::dec3
