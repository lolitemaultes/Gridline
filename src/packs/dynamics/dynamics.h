// Gridline — "dynamics" pack: mastering dynamics, loudness, enhancement,
// distortion and multi-effects. Parameter indices shared by the catalogue
// (dynamics_info.cpp), the runtimes (*_dsp.cpp) and the editors (*_ui.cpp).
#pragma once
#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include "../../core/PluginInfo.h"
#include "../../engine/Effects.h"

namespace gl::packs::dynamics {

// ------------------------------------------------------------ gl.master
// Three crossover bands (low / mid / high) and a master band, each with the
// same block of parameters (offset kMsBand0 + band * kMsBandStride).
enum MasterGlobal {
  MsLowFreq = 0,  // low / mid crossover
  MsHighFreq,     // mid / high crossover
  MsLowSlope,     // 6 / 12 / 24 dB per octave
  MsHighSlope,
  MsLookahead,    // ms
  MsBandMix,      // processed bands vs. dry (before the master band)
  MsLowCut,       // input high-pass (Hz, minimum = off)
  MsSolo,         // None / Low / Mid / High
  MsOversample,   // saturation oversampling
  MsBand0,
};
enum MasterBand {
  MbMode = 0,  // Bypass / Active / Mute / No compression (gain stages + saturation only) / Merged (low / high: into mid)
  MbPre,       // dB
  MbPost,      // dB
  MbAttack,    // ms
  MbRelease,   // ms
  MbThreshHi,  // dB: above it the "above" ratio applies
  MbRatioHi,   // x:1 (1 = no compression, 30 = limit)
  MbKnee,      // dB
  MbThreshLo,  // dB: below it the "below" ratio applies
  MbRatioLo,   // <1 lifts quiet parts (upward compression), >1 expands downward
  MbGate,      // dB: hard gate floor (minimum = off)
  MbSatThresh, // dB: soft saturation starts here (relative to 0 dBFS)
  MbSatCeil,   // dB: saturation ceiling
  MbStereo,    // -1 mono … 0 unchanged … +1 wide
  MbWindow,    // ms: detector RMS window (0 = peak)
  MbStride,
};
inline constexpr int kMsBands = 4;  // low, mid, high, master
inline constexpr int kMsBaseCount = MsBand0 + kMsBands * MbStride;
inline int msParam(int band, int k) { return MsBand0 + band * MbStride + k; }

// κ2: per-band extension block (appended after the four band blocks so older
// states keep their indices): the reference maximizer's drawn compression
// curve, second release, envelope curves and modes.
inline constexpr int kMsCurvePts = 8;
enum MasterBandExt {
  MxRelease2 = 0,  // ms, 0 = off
  MxAtkCurve,      // 0..7 (curve 1-8): attack / release 2 smoothing stages
  MxRelCurve,      // 0..7 (curve 1-8): release stages
  MxEnvMode,       // detector: 0 peak window, 1 RMS window
  MxSatMode,       // 0 off, 1 mode A, 2 mode B
  MxCurveMode,     // 0 parametric (thresholds / ratios), 1 drawn curve
  MxPoints,        // number of drawn points (2..8)
  MxPt0,           // x, y, tension per point (x, y in 0..1: x = log9(2·in + 1), out = (9^y − 1) / 2)
  MxStride = MxPt0 + kMsCurvePts * 3,
};
inline constexpr int MsExt0 = kMsBaseCount;
inline int msExt(int band, int k) { return MsExt0 + band * MxStride + k; }
inline constexpr int kMsCount = MsExt0 + kMsBands * MxStride;

// Drawn curve of a band (x ascending in 0..1). Value at x with per-segment tension.
struct DrawnCurve {
  int n = 3;
  double x[kMsCurvePts] = {0, 0.5, 1}, y[kMsCurvePts] = {0, 0.5, 0.5}, t[kMsCurvePts] = {0, 0, 0};
  double at(double xv) const;
};
DrawnCurve masterDrawnCurve(const PluginState& s, int band);
// Gain (linear) the drawn curve applies at a linear input level a (0..4).
double drawnCurveGain(const DrawnCurve& c, double a);

// Static transfer curve of one band (dB in → dB out, before post gain).
// Shared by the runtime and the editor so the drawn curve is the real one.
double masterCurve(const PluginState& s, int band, double inDb);
double masterCurveParams(double thHi, double ratioHi, double knee, double thLo, double ratioLo, double gate, double inDb);

// ------------------------------------------------------------ gl.emphasis
enum Emph {
  EmInput = 0,   // dB drive into the limiter
  EmCeiling,     // dB output ceiling
  EmRelease,     // ms
  EmCharacter,   // Balanced / Loud / Punchy / Smooth
  EmLowEmph,     // dB pre-emphasis below ~150 Hz (de-emphasised afterwards)
  EmHighEmph,    // dB pre-emphasis above ~4 kHz
  EmClip,        // 0..1 share of the work done by the clipper
  EmClipHard,    // soft / hard clipper
  EmLink,        // stereo link 0..1
  EmStereoMode,  // L/R or M/S detection
  EmDcBlock,
  EmOversample,
  EmLevelMatch,
  EmDelta,
  EmCount
};

// ------------------------------------------------------------ gl.enhance
// κ2: EnCustom = 1 runs the embedded program (a full gl.master parameter block
// at EnProg0, e.g. imported from the reference plugin) instead of a built-in voicing;
// the amount then sets the program's band mix, as the reference does.
// J-26: EnCustom = 2 runs the peak-emphasis limiter (emphasizer_ref.h) with the
// appended controls: input dB, emphasis, soft clip, level match, output gain
// (linear), bypass.
enum Enh { EnAmount = 0, EnMode, EnCustom, EnProg0 };
inline constexpr int EnPeakIn = EnProg0 + kMsCount;
inline constexpr int EnPeakEmph = EnPeakIn + 1, EnPeakSoft = EnPeakIn + 2, EnPeakMatch = EnPeakIn + 3, EnPeakOut = EnPeakIn + 4,
                     EnPeakBypass = EnPeakIn + 5;
inline constexpr int EnCount = EnPeakIn + 6;

// ------------------------------------------------------------ gl.amprack
inline constexpr int kRackSlots = 6;
inline constexpr int kRackKnobs = 6;
enum RackGlobal { RkInput = 0, RkOutput, RkSoftClip, RkOversample, RkSlot0 };
enum RackSlotParam { RsType = 0, RsOn, RsK0, RsK1, RsK2, RsK3, RsK4, RsK5, RsMix, RsStride };
inline constexpr int kRkCount = RkSlot0 + kRackSlots * RsStride;
inline int rkParam(int slot, int k) { return RkSlot0 + slot * RsStride + k; }
enum RackModule {
  RmEmpty = 0, RmOverdrive, RmFuzz, RmAmp, RmCompressor, RmEq, RmWah, RmChorus, RmPhaser, RmDelay, RmReverb,
  RmGate, RmTremolo, RmCount
};
const char* rackModuleName(int type);
// Label of knob k for a module type (nullptr = knob unused).
const char* rackKnobLabel(int type, int k);
// Display text of knob k (value 0..1) for a module type.
void rackKnobText(int type, int k, double v, char* buf, int size);
// Default knob values for a freshly chosen module.
double rackKnobDefault(int type, int k);

// ------------------------------------------------------------ gl.overdrive
enum Od {
  OdPreBand = 0, OdColor, OdPreAmp, OdBoost, OdPostFilter, OdPostGain, OdOversample,
  // J-26 reference engine (OdModel = 1; pre-model states keep the legacy engine): knob positions
  // 0..1 as the original — pre gain 1 + 25 p³ (x100: 2500), tone filter coefficient colour³,
  // post gain (OdPostGainK)², two one-pole low-passes with pole 0.2 + 0.7 · post filter.
  OdModel, OdPostGainK,
  OdCount
};
double overdriveCurve(double x, double color);

// ------------------------------------------------------------ gl.fastdist
enum Fd {
  FdPreAmp = 0, FdThreshold, FdType, FdMix, FdPost,
  // κ3 reference engine: knob positions (pre 0.5..1.5, mix / post 0..1, gain law 0.1 (11^x - 1)),
  // drive 1..10 (1 clean), types A (dB-domain compressor) / B (dB power law).
  FdPreK, FdDrive, FdMixK, FdPostK, FdClassic,
  FdCount
};
// Reference transfer table value for |x| in 0..1 (type 0 A, 1 B; drive 1..10).
double fastDistRef(double a, int type, int drive);
double fastDistCurve(double x, double threshold, int type);

// ------------------------------------------------------------ gl.squeeze
enum Sq {
  SqGain = 0, SqMix,
  SqSquare, SqSteps,                                // squarize
  SqPunchAmount, SqPreserve, SqImpact, SqRelation,  // puncher (cycle-length tricks)
  SqRingFreq, SqRingAmount,                         // ring modulator
  SqRateDiv,                                        // sample-rate division
  SqFilterOn, SqFilterType, SqCutoff, SqResonance, SqFilterPos,
  // J-26 reference engine (SqModel = 1; pre-model states keep the legacy engine): linear gain
  // after the mix, quantiser steps S (>= 32768 = off), cutoff knob position (50 Hz .. 0.75·sr),
  // periodic puncher (impact ms at the amount gain, then preserve ms at unity). Filter types in
  // the reference: LP, HP, BP, notch, LP − HP; SqResonance = r, SqFilterPos 0 pre / 1 post.
  SqModel, SqGainRef, SqStepsRef, SqCutoffPos, SqImpactMs, SqPreserveMs, SqAmountRef,
  SqCount
};
// Reference laws (raw control values of the original → engine units).
inline double sqStepsOfRaw(double raw) { return std::pow(32768.0, 1.0 - std::clamp(raw, 0.0, 32768.0) / 32768.0); }
inline double sqMsOfRaw(double raw) { return (std::pow(44101.0, std::clamp(raw, 0.0, 44100.0) / 44100.0) - 1.0) / 44.1; }
inline double sqCutoffHz(double pos, double sr) { return 50.0 * std::pow(0.75 * sr / 50.0, std::clamp(pos, 0.0, 1.0)); }

// ------------------------------------------------------------ gl.effector
enum Ef {
  EfKind = 0, EfX, EfY, EfModX, EfModY, EfModShape, EfModRate, EfInput, EfOutput, EfMix, EfBypass,
  // J-26 reference engine (EfModel = 1; pre-model states keep the legacy engine): the twelve
  // stored values of the original, 0..1 each, in its order (efref:: laws in effector_ref.h).
  EfModel, EfRawKind, EfRawBypass, EfRawMix, EfRawX, EfRawY, EfRawRate, EfRawTempoRed, EfRawModX, EfRawModY,
  EfRawShape, EfRawIn, EfRawOut,
  EfCount
};
inline constexpr int kEfKinds = 12;
const char* effectorKindName(int kind);
const char* effectorAxisLabel(int kind, int axis);  // axis 0 = X, 1 = Y
// LFO length in beats for the rate choice.
double effectorRateBeats(int choice);
// Modulation shape value (-1..1) at phase 0..1.
double effectorShape(int shape, double phase, double randA, double randB);

// ------------------------------------------------------------ gl.transplit
// The input splits into a transient part and a sustain part (detector:
// fast vs slow envelope, sensitivity, hold, release, detection speed); each
// part has its own level, tilt and effect slot, then they are summed. The
// band split / balance, drive, mix and gain act on the whole result.
// TsAttack / TsRelease are the transient / sustain levels (-1 silent, 0 unity,
// +1 = +12 dB); TsAttackShape is the detection speed, TsReleaseShape the
// detector release. Indices 0..8 are the original layout; 9.. were appended.
enum Ts {
  TsSplitFreq = 0, TsBalance, TsAttack, TsAttackShape, TsRelease, TsReleaseShape, TsDrive, TsMix, TsGain,
  TsSensitivity, TsHold, TsTransTilt, TsSusTilt,
  TsTransFx, TsTransFxMix, TsTransFxA, TsTransFxB,
  TsSusFx, TsSusFxMix, TsSusFxA, TsSusFxB,
  TsLookahead, TsStereoLink, TsTransOn, TsSusOn, TsTransSync, TsSusSync,
  TsCount
};
// Effect slot kinds (TsTransFx / TsSusFx). A / B per kind: saturate drive /
// warmth, bounce time curve / amp curve, diffuse and blur depth / size, echo
// feedback / time.
enum TsFxKind { TsFxSaturate = 0, TsFxBounce, TsFxDiffuse, TsFxBlur, TsFxEcho, TsFxOff, TsFxCount };
inline constexpr int kTsSlotStride = TsSusFx - TsTransFx;  // transient slot params + stride = sustain slot params
// Transient / sustain level (-1..1) → linear gain.
inline double tsLevelGain(double v) { return v >= 0 ? 1.0 + 3.0 * v : 1.0 + v; }
// Echo time: free 1..1000 ms (log) or tempo-synced 1..768 ticks of 1/96 beat.
inline double tsEchoSeconds(double b, bool sync, double tempo) {
  b = b < 0 ? 0 : (b > 1 ? 1 : b);
  if (sync) return (1.0 + std::round(b * 767.0)) / 96.0 * 60.0 / (tempo < 10 ? 10 : tempo);
  return 0.001 * std::pow(1000.0, b);
}

// Runtimes, one factory per source file.
std::unique_ptr<Effect> createMasterEffect(const std::string& type);  // master_dsp.cpp
std::unique_ptr<Effect> createRackEffect(const std::string& type);    // rack_dsp.cpp
std::unique_ptr<Effect> createDistEffect(const std::string& type);    // dist_dsp.cpp

}  // namespace gl::packs::dynamics
