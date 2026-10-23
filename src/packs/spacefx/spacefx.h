// Gridline — "spacefx" pack: delays, reverbs, convolution and modulation
// effects. Parameter indices shared by the catalogue, DSP and editors.
//
//   gl.convolver   Convolver       partitioned FFT convolution, built-in or file IRs
//   gl.multidelay  Multi Delay     eight routed delay taps with filters, gates, diffusion
//   gl.banddelay   Band Delay      spectral delay: per-band time / feedback / pan / level curves
//   gl.hall        Hall            16-line FDN reverb with shimmer, ducking and freeze
//   gl.ensemble    Ensemble        multi-line modulated chorus with slow movement
//   gl.bbdchorus   Bucket Chorus   bucket-brigade style chorus with fixed modes
//   gl.swirl       Swirl           multi-voice chorus / flanger
//   gl.vphaser     Retro Phaser    allpass-stage phaser with LFO shapes and echo
#pragma once
#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>
#include "../../core/PluginInfo.h"
#include "../../engine/Sample.h"

namespace gl::packs::spacefx {

// --------------------------------------------------------------- Convolver
enum ConvParam {
  CvSource,     // IR source: file or a built-in space
  CvSize,       // built-in: size / decay
  CvTone,       // built-in: brightness
  CvStart,      // ms trimmed from the IR start
  CvLength,     // s, maximum IR length
  CvSync,       // tempo-synced length (applied at run time)
  CvSyncLen,    // synced length choice
  CvFadeIn,     // ms
  CvFadeOut,    // fraction of the length faded at the end
  CvTighten,    // extra exponential decay
  CvReverse,
  CvStretch,    // resampling ratio (0.5..2)
  CvPreDelay,   // ms
  CvLowCut,     // Hz, applied to the IR
  CvHighCut,    // Hz, applied to the IR
  CvSelfConv,   // self-convolution blend
  CvNormalize,
  CvStereo,     // Stereo / True stereo / Mono
  CvInWidth,    // input stereo width
  CvWidth,      // wet stereo width
  CvDry,
  CvWet,
  // J-1: EQ curve mix, IR envelopes on/off, stretch mode, reported latency.
  CvEqMix,      // blend of the EQ-curve-filtered IR (EQ curve in PluginState::data)
  CvEnvOn,      // apply the IR amplitude envelope (points in PluginState::data)
  CvStretchMode,// Resample (pitch follows) / Keep pitch (spectral)
  CvLatency,    // samples, informational (reported, not applied)
  // J-42: IR build model — 0 Gridline editing chain, 1 FL reference chain (normalise to a
  // 300 ms RMS peak, pan / volume / separation envelopes, self-convolution, stretch + rate
  // conversion, zero-phase EQ curve FIR, tail trim; spec j35_convolver.md). Old states → 0.
  CvModel,
  CvCount
};
// Point envelope (IR amplitude: t = seconds, v 0..1 with 0.5 = unity, ×2 at 1;
// EQ: t = 0..1 on a log 20 Hz .. 20 kHz axis, v 0.5 = 0 dB, ±18 dB at 1 / 0).
struct EnvPoint {
  double t = 0, v = 0.5, k = 0;  // k = tension −1..1
  int c = 0;                     // curve kind (FL point shape, reference build; 0 = single curve)
};
// PluginState::data of gl.convolver: line 1 = IR path; then (version 1)
//   "#v 1", "#env t,v,k;t,v,k;…", "#eq x,v,k;…" (older states hold only the path).
// J-42: "#pan" / "#sep" (reference build: the source IR's pan and stereo
// separation envelopes, 0.5 = neutral), optional 4th point field = curve kind.
// J-106: "#mode eq=smooth;env=straight" holds the curve modes (CurveMode); a curve
// without an entry is CurveAuto.
enum CurveMode { CurveAuto = -1, CurveStraight = 0, CurveSmooth = 1 };
struct ConvData {
  std::string path;
  std::vector<EnvPoint> env, eq, pan, sep;
  int envMode = CurveAuto, eqMode = CurveAuto;
};
ConvData parseConvData(const std::string& data);
std::string formatConvData(const ConvData& d);
// J-106 curve modes. Straight: segments bent by the end point's tension (the
// IR build's segment law). Smooth: one monotone cubic through every point.
// No stored mode (CurveAuto: older projects, presets, FL imports, programmatic
// states) is Straight, so they sound as before; the editor gives a new (empty)
// curve Smooth when its first point is added (curveModeShown).
inline bool curveSmooth(int mode) { return mode == CurveSmooth; }
inline int curveModeShown(int mode, const std::vector<EnvPoint>& pts) {
  return mode != CurveAuto ? mode : pts.empty() ? CurveSmooth : CurveStraight;
}
// Smooth mode: monotone piecewise-cubic Hermite interpolation (Fritsch–Carlson
// PCHIP with Fritsch–Butland harmonic-mean tangents) along the curve's own x
// axis (log frequency for the EQ, seconds for the envelope). It passes exactly
// through every point, never leaves the range of the two points around a
// segment (no overshoot), has zero slope at the first and last point (flat ends,
// held beyond them) and no corners. Tension (k) and curve kind (c) are ignored
// (kept for Straight). The editor's drawing and the IR build both evaluate it
// through this struct (UI thread only; the audio thread never sees curves).
struct SmoothCurve {
  explicit SmoothCurve(const std::vector<EnvPoint>& pts);
  double at(double t) const;
  std::vector<double> x, y, m;  // knots and tangents
};
// Envelope value at t (linear segments bent by tension).
double envAt(const std::vector<EnvPoint>& pts, double t);
// Legacy-build curve evaluator: envAt (Straight) or SmoothCurve (Smooth).
struct ConvCurve {
  ConvCurve(const std::vector<EnvPoint>& pts, bool smooth);
  double at(double t) const { return sm_ ? sc_.at(t) : envAt(*p_, t); }

 private:
  const std::vector<EnvPoint>* p_;
  bool sm_;
  SmoothCurve sc_;
};
// EQ curve gain (dB) at hz, and the frequency of an x position.
double convEqDb(const std::vector<EnvPoint>& eq, double hz, bool smooth = false);
// FL export of a Smooth curve (FL's segments are straight / tension only): plain
// straight points (k 0, c 0) through the original points plus enough in between
// that errDb(straight value, smooth value) stays within `tolDb` everywhere.
std::vector<EnvPoint> smoothToSegments(const std::vector<EnvPoint>& pts, double (*errDb)(double, double), double tolDb);
// Those errors in dB: EQ values (the worse of the two IR builds' gain laws) and IR
// amplitude envelope values (×2 at 1); levels floored at -80 dB.
double convEqErrDb(double a, double b);
double convEnvErrDb(double a, double b);
inline double convEqHz(double x) { return 20.0 * std::pow(1000.0, std::clamp(x, 0.0, 1.0)); }
// FL knob laws (x = 0..1 wheel position).
inline double convDryGain(double x) { return (std::pow(11.0, x) - 1.0) / 10.0; }
inline double convWetGain(double x) { return (std::pow(1001.0, x) - 1.0) / 1000.0; }
inline double convPreDelayMs(double x) { return 500.0 * (std::pow(7.0, x) - 1.0) / 6.0; }
inline double convStretch(double x) { return std::pow(2.0, x); }
enum ConvSource { SrcFile, SrcRoom, SrcHall, SrcPlate, SrcSpring, SrcChamber, SrcCabinet, SrcGated, SrcReverse, SrcCount };
inline constexpr double kConvSyncBeats[] = {0.25, 0.5, 1, 2, 4, 8, 16};

// ------------------------------------------------------------- Multi Delay
inline constexpr int kTaps = 8;
enum MultiGlobal { MdSync, MdDry, MdWet, MdInput, MdFbScale, MdGlobalCount };
enum MultiField {
  MdOn, MdSteps, MdTimeMs, MdOffset, MdInLevel, MdInPan, MdInFType, MdInCut, MdInRes,
  MdFeedback, MdFbMode, MdFbPan, MdFbFType, MdFbCut, MdFbRes, MdSep,
  MdModRate, MdModDepth, MdDiffuse, MdGateDiv, MdGateShape, MdOutLevel, MdOutPan, MdToNext,
  MdFieldCount
};
inline int mdParam(int tap, int field) { return MdGlobalCount + tap * MdFieldCount + field; }
inline constexpr int MdCount = MdGlobalCount + kTaps * MdFieldCount;
inline constexpr int kMdMaxDelay = 1 << 18;  // samples per tap line (5.9 s @ 44.1 kHz)
// J-1 reference model (appended after the tap block). In the reference model
// MdInFType / MdFbFType use all eight filter types (Off, LP, HP, BP, Notch,
// low shelf, peaking, high shelf), MdFbMode adds "Off" (no delay line),
// MdGateDiv / MdGateShape are the grain division / window bend, the levels
// are signed gains and MdInRes / MdFbRes are the 0..1 resonance control.
enum MultiExtGlobal { MdModel = MdCount, MdOversample, MdExtGlobalEnd };
enum MultiExtField { MdxTempo, MdxInStages, MdxInGain, MdxInPost, MdxFbStages, MdxFbGain, MdxLeaveFirst, MdxCount };
inline int mdExt(int tap, int e) { return MdExtGlobalEnd + tap * MdxCount + e; }
inline constexpr int MdTotal = MdExtGlobalEnd + kTaps * MdxCount;
enum MdFilterType { MfOff, MfLowPass, MfHighPass, MfBandPass, MfNotch, MfLowShelf, MfPeak, MfHighShelf, MfCount };
enum MdFbModeRef { MfbNormal, MfbPingPong, MfbInvert, MfbOff };

// -------------------------------------------------------------- Band Delay
inline constexpr int kBdPoints = 32;  // curve points, log-spaced 20 Hz – 20 kHz
enum BandTarget { BtDelay, BtFeedback, BtPan, BtLevel, BtCount };
enum BandParam { BdMaxTime, BdSync, BdSyncTime, BdFeedback, BdSmooth, BdKeepPitch, BdMorph, BdLowCut, BdSat, BdDry, BdWet, BdCount };
// J-42 reference model (BdModel = 1, sfx_mbd.h): 16 fixed bands, per-band delay lines,
// 8 banks (data line "mbd8" + 384 values) — engine values 0..1 as the reference stores them.
enum BandRefParam {
  BdModel = BdCount,
  BdRScale,     // 0.5 = 0; |2x − 1| = max time (1 s), x < 0.5 flips the band curve
  BdRTenth,     // time × 0.1
  BdRFeedback,  // feedback = 0.95 x (or decay time with weighting)
  BdRSmooth,    // smoothing time x seconds
  BdRMode,      // filter bank: < 1/3 gentle, < 2/3 steep, else linear phase
  BdRKeep,      // keep pitch (tap cross-fades)
  BdRMorph,     // bank 1 + 7 x
  BdRMix,       // dry / wet cross-fade
  BdRHarmonic,  // delay = one period around C5
  BdRWeight,    // feedback as a decay time
  BdRHp,        // wet high pass 10 · 100^x Hz
  BdRSat,       // < 1/3 limiter, < 2/3 tanh, else clip
  BdTotal
};
inline constexpr double kBdSyncBeats[] = {0.25, 0.5, 0.75, 1, 1.5, 2, 3, 4};
// Curves: [bank][target][point]; values 0..1 (pan: 0..1 maps to -1..1).
struct BandCurves {
  float v[2][BtCount][kBdPoints];
};
BandCurves defaultBandCurves();
BandCurves parseBandCurves(const std::string& data);
std::string formatBandCurves(const BandCurves& c);
double bandPointHz(int point);  // centre frequency of a curve point
inline constexpr int kBdFft = 1024, kBdHop = 256;

// -------------------------------------------------------------------- Hall
enum HallParam {
  HvInGain, HvInLowCut, HvInHighCut,
  HvOn, HvPreDelay, HvPreSync, HvPreSyncTime, HvSize, HvDecay, HvCharacter, HvDiffusion, HvModRate, HvModDepth, HvHQ,
  HvFreeze, HvLowMult, HvLowXover, HvHighMult, HvHighXover,
  HvFbOn, HvFbMode, HvFbPitch, HvFbGain, HvFbDelay, HvFbLowCut, HvFbHighCut, HvFbMix,
  HvEnvMode, HvEnvThresh, HvEnvAmount, HvEnvAttack, HvEnvRelease,
  HvDry, HvWet, HvEqFreq, HvEqGain, HvEqQ, HvWidth, HvLimiter,
  // J-1 reference model (HvModel = 1; states saved before it normalise to 0 = legacy).
  // Reference meaning of shared params: HvSize → S = 1 + 99·size (prime line table),
  // HvCharacter → mixing-matrix rotation, HvFreeze 0 normal / 1 freeze / 2,3 sustain,
  // HvFbMode → pitch type (feedback shimmer / two-voice feed-forward), HvPreSync → HvPreTicks.
  HvModel, HvBright, HvModFreq, HvPreTicks, HvFbSync, HvFbTicks,
  HvEnvRoute, HvEnvScale, HvEnvOffset, HvEnvSmooth, HvEnvLowCut, HvEnvSidechain,
  HvFfDirect, HvFfPitch1, HvFfPitch2, HvFfAmp1, HvFfAmp2, HvFfDelay1, HvFfDelay2, HvFfSync1, HvFfSync2, HvFfTicks1, HvFfTicks2,
  // J-45: the envelope's sidechain source (PluginInfo::sideInParam): -1 the
  // track's sidechain input, 0 none, n the track's n-th source (FL input route).
  HvScIn,
  HvCount
};
enum HallFreeze { HfNormal, HfFreeze, HfInfinite, HfSustain };
enum HallEnvRoute { HeOff, HeWet, HeDecay };
// Synced time of a 1..768 tick wheel: 16·(v − 1)/767 sixteenth steps (seconds at a tempo).
inline double hallTicksSeconds(double ticks, double bpm) {
  double steps = 16.0 * (std::clamp(ticks, 1.0, 768.0) - 1.0) / 767.0;
  return steps * 15.0 / std::max(1.0, bpm);
}
// Reference FDN: delay-line lengths (samples at 44.1 kHz) for a size S = 1..100.
void hallLineLengths(double size01, int out[16]);
inline constexpr double kSyncBeats[] = {0.125, 0.25, 0.375, 0.5, 0.75, 1, 1.5, 2, 4};
// RT60 (s) of the tail at a frequency, as the DSP computes it (also drawn by the editor).
double hallRt60(const PluginState& s, double hz);

// ---------------------------------------------------------------- Ensemble
// J-26: EnModel = 1 runs the reference engine (hyperchorus_ref.h) from the eleven knob
// positions EnK0..EnK10 (tag order: delay, feedback, mod rate, mod amount, movement amount,
// movement rate, 8 lines, low pass, high pass, mix, LFO phase difference).
enum EnsParam { EnDelay, EnFeedback, EnModAmount, EnModRate, EnMoveAmount, EnMoveRate, EnLines, EnHighPass, EnLowPass, EnPhase, EnSpread, EnMix,
                EnModel, EnK0, EnK1, EnK2, EnK3, EnK4, EnK5, EnK6, EnK7, EnK8, EnK9, EnK10, EnCount };

// ----------------------------------------------------------- Bucket Chorus
enum BbdParam { BcMode, BcTime1, BcTime2, BcRate, BcSync, BcSyncRate, BcWave, BcPhaseLR, BcStartPhase, BcFeedback, BcHighPass,
                BcHiss, BcGain, BcMonoIn, BcInvertWet, BcNoiseGate, BcMix,
                BcModel,      // J-1: 0 legacy, 1 reference (old states load as legacy)
                BcSyncTicks,  // reference sync period, ticks of 1/192 beat
                BcCount };
inline constexpr double kLfoSyncBeats[] = {0.0625, 0.125, 0.25, 0.5, 1, 2, 4, 8, 16};
// Effective modulation settings of a mode (shared with the editor's display).
struct BbdModeSettings {
  double minMs, maxMs, rateHz;
  int wave;  // 0 sine, 1 triangle
};
BbdModeSettings bbdSettings(const PluginState& s, double tempo);

// ------------------------------------------------------------------- Swirl
enum SwirlParam {
  SwOrder, SwDepth, SwSpeed, SwDelay, SwSpread, SwCross, SwDry, SwWet,
  // κ3 reference engine: knob positions (depth / speed / delay / spread 0..1, cross / dry / wet -1..1),
  // 2 x order voices, cross = opposite-channel gain, levels 0.1 (11^|x| - 1).
  SwDepthK, SwSpeedK, SwDelayK, SwSpreadK, SwCrossK, SwDryK, SwWetK, SwModel,
  SwCount
};

// ------------------------------------------------------------ Retro Phaser
enum PhaserParam {
  VpStages, VpFreqMin, VpFreqMax, VpManual, VpManualFreq, VpFeedback, VpInvertFb,
  VpWave, VpRate, VpSync, VpSyncRate, VpTension, VpPhaseLR, VpStartPhase,
  VpColorGain, VpColorFreq, VpInputHp, VpFbHp, VpDrive, VpNoise, VpNoiseGate, VpMonoIn, VpMix,
  VpDelayOn, VpDelayTime, VpDelaySync, VpDelaySyncTime, VpDelayFb, VpDelayMix, VpDelayStereo, VpDelayLp, VpDelayHp,
  // J-1 reference model (old states load as legacy).
  VpModel, VpHq, VpEngine, VpRateLo, VpRateHi, VpDepthScale, VpDepthPull, VpSyncTicks, VpDelayKeep, VpDelayTicks,
  VpCount
};
inline constexpr int kPhaserStages[] = {4, 6, 8, 10, 12};
// LFO shape value (0..1) at phase 0..1 for a wave choice and tension (shared with the editor).
double phaserLfo(int wave, double phase, double tension);

// ------------------------------------------------------------ shared helpers
// Processed impulse response (time domain, native rate) for a convolver
// state; cached, safe to call from the UI thread. nullptr if unavailable.
// left/right hold the L-out paths (true stereo: [0]=LL [1]=RR, see IrInfo).
struct IrView {
  std::shared_ptr<const Sample> ir;  // display channels (L, R)
  int paths = 2;                     // 2 = stereo, 4 = true stereo
  double rawSeconds = 0;             // length of the source before trimming
  std::string error;
};
IrView convolverIr(const PluginState& s);
// The engine rate the convolver prepares kernels for (updated by the audio thread).
void setConvolverRateHint(double sr);
double convolverRateHint();

}  // namespace gl::packs::spacefx
